#include "modbus.h"
#include "rackmond.h"
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <stdarg.h>
#include <syslog.h>
#include <signal.h>
#include <linux/serial.h>
#include <sched.h>
#include <assert.h>

#define MAX_ACTIVE_ADDRS 24
#define MAX_RACKS        3
#define MAX_SHELVES      2
#define MAX_PSUS         3

#define REGISTER_PSU_STATUS 0x68

#define READ_ERROR_RESPONSE -2

/*
 * check for new PSUs every "PSU_SCAN_INTERVAL" seconds
 */
#define PSU_SCAN_INTERVAL 120

/*
 * Fetch/refresh data from all PSUs every "PSU_REFRESH_DATA_INTERVAL"
 * seconds.
 */
#define PSU_REFRESH_DATA_INTERVAL 1

/*
 * REG_INT_DATA_SIZE defines the memory size required to store a specific
 * "Register Interval": uint32_t is reserved for timestamp, while the
 * remaining is to store register values (per_register_size=sizeof(u16),
 * register_count=iv->len).
 */
#define REG_INT_DATA_SIZE(_iv) (sizeof(uint32_t) + \
                                (sizeof(uint16_t) * (_iv)->len))

#define TIME_UPDATE(_t)  do {                        \
  struct timespec _ts;                               \
  if (clock_gettime(CLOCK_REALTIME, &_ts) != 0) {    \
    OBMC_ERROR(errno, "failed to get current time"); \
    (_t) = 0;                                        \
  } else {                                           \
    (_t) = _ts.tv_sec;                               \
  }                                                  \
} while (0)

static int scanning = 0;

typedef struct {
  // hold this for the duration of a command
  pthread_mutex_t lock;
#define dev_lock(_d)   mutex_lock_helper(&((_d)->lock), "dev_lock")
#define dev_unlock(_d) mutex_unlock_helper(&((_d)->lock), "dev_lock")
  int tty_fd;
} rs485_dev_t;

typedef struct {
  uint16_t begin; /* starting register address */
  int num;        /* number of registers */
} reg_req_t;

typedef struct {
  monitor_interval* i;
  void* mem_begin;
  size_t mem_pos;
} reg_range_data_t;

typedef struct {
  uint8_t addr;
  uint32_t crc_errors;
  uint32_t timeout_errors;
  reg_range_data_t range_data[1];
} psu_datastore_t;

typedef struct {
  char* buffer;
  size_t len;
  size_t pos;
  int fd;
} write_buf_t;

/*
 * Global rackmond config structure, protected by its mutex lock.
 */
static struct {
  // global rackmond lock
  pthread_mutex_t lock;
#define global_lock()   mutex_lock_helper(&rackmond_config.lock, "global_lock")
#define global_unlock() mutex_unlock_helper(&rackmond_config.lock, "global_lock")

  // number of register read commands to send to each PSU
  int num_reqs;
  // register read commands (begin+length)
  reg_req_t *reqs;
  monitoring_config *config;

  uint8_t num_active_addrs;
  uint8_t active_addrs[MAX_ACTIVE_ADDRS];
  psu_datastore_t* stored_data[MAX_ACTIVE_ADDRS];
  FILE *status_log;

  // timeout in nanosecs
  int modbus_timeout;

  // inter-command delay
  int min_delay;

  int paused;

  rs485_dev_t rs485;
} rackmond_config = {
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .modbus_timeout = 300000,
};

static int mutex_lock_helper(pthread_mutex_t *lock, const char *name)
{
  int error = pthread_mutex_lock(lock);
  if (error != 0) {
    OBMC_ERROR(error, "failed to acquire %s", name);
    return -1;
  }

  return 0;
}

static int mutex_unlock_helper(pthread_mutex_t *lock, const char *name)
{
  int error = pthread_mutex_unlock(lock);
  if (error != 0) {
    OBMC_ERROR(error, "failed to release %s", name);
    return -1;
  }

  return 0;
}

static int buf_open(write_buf_t* buf, int fd, size_t len) {
  int error = 0;
  char* bufmem;

  bufmem = malloc(len);
  if(!bufmem) {
    BAIL("Couldn't allocate write buffer of len %zd for fd %d\n", len, fd);
  }
  buf->buffer = bufmem;
  buf->pos = 0;
  buf->len = len;
  buf->fd = fd;

cleanup:
  return error;
}

static ssize_t buf_flush(write_buf_t* buf) {
  int ret;

  ret = write(buf->fd, buf->buffer, buf->pos);
  if(ret > 0) {
    memmove(buf->buffer, buf->buffer + ret, buf->pos - ret);
    buf->pos -= ret;
  }

  return ret;
}

static ssize_t buf_write(write_buf_t* buf, void* from, size_t len) {
  int ret;

  // write will not fill buffer, only memcpy
  if((buf->pos + len) < buf->len) {
    memcpy(buf->buffer + buf->pos, from, len);
    buf->pos += len;
    return len;
  }

  // write would exceed buffer, flush first
  ret = buf_flush(buf);
  if (ret < 0) {
    return ret;
  } else if (buf->pos != 0) {
    // write() was interrupted but partially succeeded -- the buffer partially
    // flushed but no bytes of the requested buf_write went through
    return 0;
  }

  if (len > buf->len) {
    // write is larger than buffer, skip buffer
    return write(buf->fd, from, len);
  } else {
    return buf_write(buf, from, len);
  }
}

static int buf_printf(write_buf_t* buf, const char* format, ...) {
  char tmpbuf[512];
  int error = 0;
  int ret;
  va_list args;

  va_start(args, format);
  ret = vsnprintf(tmpbuf, sizeof(tmpbuf), format, args);
  ERR_EXIT(ret);

  if(ret > sizeof(tmpbuf)) {
    BAIL("truncated buf_printf (%d bytes truncated to %d)",
         ret, sizeof(tmpbuf));
  }
  ERR_EXIT(buf_write(buf, tmpbuf, ret));

cleanup:
  va_end(args);
  return error;
}

static int buf_close(write_buf_t* buf) {
  int error = 0;
  int fret = buf_flush(buf);
  int cret = close(buf->fd);

  free(buf->buffer);
  buf->buffer = NULL;
  ERR_EXIT(fret);
  ERR_LOG_EXIT(cret, "failed to close buf->fd");

cleanup:
  return error;
}

static char psu_address(int rack, int shelf, int psu) {
  int rack_a = ((rack & 3) << 3);
  int shelf_a = ((shelf & 1) << 2);
  int psu_a = (psu & 3);

  return 0xA0 | rack_a | shelf_a | psu_a;
}

static int modbus_command(rs485_dev_t* dev, int timeout, char* cmd_buf,
                          size_t cmd_size, char* resp_buf, size_t resp_size,
                          size_t exp_resp_len) {
  modbus_req req;
  int error;
  useconds_t delay = rackmond_config.min_delay;

  req.tty_fd = dev->tty_fd;
  req.modbus_cmd = cmd_buf;
  req.cmd_len = cmd_size;
  req.dest_buf = resp_buf;
  req.dest_limit = resp_size;
  req.timeout = timeout;
  req.expected_len = (exp_resp_len != 0 ? exp_resp_len : resp_size);
  req.scan = scanning;

  if (dev_lock(dev) != 0) {
    return -1;
  }

  error = modbuscmd(&req);
  if (delay != 0) {
    usleep(delay);
  }

  dev_unlock(dev);

  if (error < 0) {
    return error;
  }
  return req.dest_len;
}

/*
 * Read Holding Register (Function 3) Request Header Size:
 *   Slave_Addr (1-byte) + Function (1-byte) + Starting_Reg_Addr (2-bytes) +
 *   Reg_Count (2-bytes).
 */
#define MODBUS_FUN3_REQ_HDR_SIZE        6

/*
 * Read Holding Register (Function 3) Response packet size:
 *   Slave_Addr (1-byte) + Function (1-byte) + Data_Byte_Count (1-byte) +
 *   Reg_Value_Array (2 * Reg_Count) + CRC (2-byte)
 */
#define MODBUS_FUN3_RESP_PKT_SIZE(nreg) (2 * (nreg) + 5)

static int read_holding_reg(rs485_dev_t *dev, int timeout, uint8_t slave_addr,
                            uint16_t reg_start_addr, uint16_t reg_count,
                            uint16_t *out) {
  int error = 0;
  int dest_len;
  char command[MODBUS_FUN3_REQ_HDR_SIZE];
  size_t resp_len = MODBUS_FUN3_RESP_PKT_SIZE(reg_count);
  char *resp_buf;

  resp_buf = malloc(resp_len);
  if (resp_buf == NULL) {
    OBMC_WARN("failed to allocate response buffer");
    return -1;
  }

  command[0] = slave_addr;
  command[1] = MODBUS_READ_HOLDING_REGISTERS;
  command[2] = reg_start_addr >> 8;
  command[3] = reg_start_addr & 0xFF;
  command[4] = reg_count >> 8;
  command[5] = reg_count & 0xFF;

  dest_len = modbus_command(dev, timeout, command, MODBUS_FUN3_REQ_HDR_SIZE,
                            resp_buf, resp_len, 0);
  ERR_EXIT(dest_len);

  if (dest_len >= 5) {
    memcpy(out, resp_buf + 3, reg_count * 2);
  } else {
    OBMC_WARN("Unexpected short but CRC correct response!\n");
    error = -1;
    goto cleanup;
  }
  if (resp_buf[0] != slave_addr) {
    OBMC_WARN("Got response from addr %02x when expected %02x\n",
              resp_buf[0], slave_addr);
    error = -1;
    goto cleanup;
  }
  if (resp_buf[1] != MODBUS_READ_HOLDING_REGISTERS) {
    // got an error response instead of a read regsiters response
    // likely because the requested registers aren't available on
    // this model (e.g. stingray)
    error = READ_ERROR_RESPONSE;
    goto cleanup;
  }
  if (resp_buf[2] != (reg_count * 2)) {
    OBMC_WARN("Got %d register data bytes when expecting %d\n",
              resp_buf[2], (reg_count * 2));
    error = -1;
    goto cleanup;
  }

cleanup:
  free(resp_buf);
  return error;
}

static int sub_uint8s(const void* a, const void* b) {
  return (*(uint8_t*)a) - (*(uint8_t*)b);
}

/*
 * Scan connected PSUs. Executed in monitoring thread.
 */
static int check_active_psus(void) {
  int error = 0;
  int rack, shelf, psu, offset;

  if (global_lock() != 0) {
    return -1;
  }

  if (rackmond_config.paused == 1) {
    global_unlock();
    usleep(1000);
    return 0;
  }
  if (rackmond_config.config == NULL) {
    global_unlock();
    usleep(5000);
    return 0;
  }

  offset = 0;
  scanning = 1;
  for (rack = 0; rack < MAX_RACKS; rack++) {
    for (shelf = 0; shelf < MAX_SHELVES; shelf++) {
      for (psu = 0; psu < MAX_PSUS; psu++) {
        int err;
        uint16_t output = 0;
        char addr = psu_address(rack, shelf, psu);

        err = read_holding_reg(&rackmond_config.rs485,
                               rackmond_config.modbus_timeout, addr,
                               REGISTER_PSU_STATUS, 1, &output);
        if (err != 0) {
          continue;
        }
        if (offset >= ARRAY_SIZE(rackmond_config.active_addrs)) {
          OBMC_WARN("Too many PSUs detected: addr %#02x ignored.", addr);
          continue;
        }

        dbg("detected PSU at addr %#02x", addr);
        rackmond_config.active_addrs[offset++] = addr;
      }
    }
  }
  rackmond_config.num_active_addrs = offset;

  //its the only stdlib sort
  qsort(rackmond_config.active_addrs, rackmond_config.num_active_addrs,
        sizeof(uint8_t), sub_uint8s);

  scanning = 0;
  global_unlock();

  return error;
}

/*
 * "psu_datastore_t" points to the memory area which stores all the
 * monitored registers of a specific PSU (identified by address), and
 * logically the memory area can be divided into 3 sub-areas:
 *
 * M = "config->num_intervals", and N = "iv->keep" in below picture:
 *
 * |--------------------------|
 * |                          |
 * | struct psu_datastore_t   |
 * |                          |
 * |--------------------------|
 * | reg_range_data_t[0]      |
 * | reg_range_data_t[1]      |
 * | ......                   |
 * | reg_range_data_t[M-1]    |
 * |--------------------------|
 * | timestamp(4-byte)        |
 * | reg_interval_0,keep#0    |
 * | timestamp(4-byte)        |
 * | reg_interval_0,keep#1    |
 * |......                    |
 * | timestamp(4-byte)        |
 * | reg_interval_0,keep#N-1] |
 * | timestamp(4-byte)        |
 * | reg_interval_1,keep#0    |
 * | timestamp(4-byte)        |
 * | reg_interval_1,keep#1    |
 * |......                    |
 * | timestamp(4-byte)        |
 * | reg_interval_M-1,keep#0  |
 * |......                    |
 * | timestamp(4-byte)        |
 * | reg_interval_M-1,keep#N-1|
 * |--------------------------|
 */
static psu_datastore_t* alloc_monitoring_data(uint8_t addr) {
  void *mem;
  size_t size;
  psu_datastore_t *d;
  monitor_interval *iv;
  int i, pitch, data_size;

  size = sizeof(psu_datastore_t) +
         sizeof(reg_range_data_t) * rackmond_config.config->num_intervals;

  for (i = 0; i < rackmond_config.config->num_intervals; i++) {
    iv = &rackmond_config.config->intervals[i];
    pitch = REG_INT_DATA_SIZE(iv);
    data_size = pitch * iv->keep;
    size += data_size;
  }

  d = calloc(1, size);
  if (d == NULL) {
    OBMC_WARN("Failed to allocate memory for sensor data.\n");
    return NULL;
  }

  d->addr = addr;
  d->crc_errors = 0;
  d->timeout_errors = 0;
  mem = d;
  mem += (sizeof(psu_datastore_t) +
          sizeof(reg_range_data_t) * rackmond_config.config->num_intervals);

  for (i = 0; i < rackmond_config.config->num_intervals; i++) {
    iv = &rackmond_config.config->intervals[i];
    pitch = REG_INT_DATA_SIZE(iv);
    data_size = pitch * iv->keep;
    d->range_data[i].i = iv;
    d->range_data[i].mem_begin = mem;
    d->range_data[i].mem_pos = 0;
    mem += data_size;
  }

  return d;
}

static int sub_storeptrs(const void* va, const void *vb) {
  //more *s than i like :/
  psu_datastore_t* a = *(psu_datastore_t**)va;
  psu_datastore_t* b = *(psu_datastore_t**)vb;

  //nulls to the end
  if (b == NULL && a == NULL) {
    return 0;
  }
  if (b == NULL) {
    return -1;
  }
  if (a == NULL) {
    return 1;
  }
  return (int)(a->addr - b->addr);
}

static int lookup_data_slot(uint8_t addr)
{
  int i;

  for (i = 0; i < ARRAY_SIZE(rackmond_config.stored_data); i++) {
    if (rackmond_config.stored_data[i] == NULL ||
        rackmond_config.stored_data[i]->addr == addr) {
      return i; /* Found the slot */
    }
  }

  return -1; /* No slot available. */
}

static int alloc_monitoring_datas(void) {
  int i;
  int error = 0;

  if (global_lock() != 0) {
    return -1;
  }

  if (rackmond_config.config == NULL) {
    global_unlock();
    return 0;
  }

  qsort(rackmond_config.stored_data, MAX_ACTIVE_ADDRS,
        sizeof(psu_datastore_t*), sub_storeptrs);

  for (i = 0; i < rackmond_config.num_active_addrs; i++) {
    int slot;
    uint8_t addr = rackmond_config.active_addrs[i];

    slot = lookup_data_slot(addr);
    if (slot < 0) {
      OBMC_WARN("no data space left for psu addr %#02x", addr);
      error = -1;
      break;
    }

    if (rackmond_config.stored_data[slot] == NULL) {
      // this will only be logged once per address
      OBMC_INFO("Detected PSU at address 0x%02x", addr);

      rackmond_config.stored_data[slot] = alloc_monitoring_data(addr);
      if (rackmond_config.stored_data[slot] == NULL) {
        OBMC_WARN("failed to allocate datastore for psu addr %#02x", addr);
        error = -1;
        break;
      }
    }
  } /* for */

  global_unlock();
  return error;
}

static void record_data(reg_range_data_t* rd, uint32_t time,
                        uint16_t* regs) {
  int n_regs = (rd->i->len);
  int pitch = REG_INT_DATA_SIZE(rd->i);
  int mem_size = pitch * rd->i->keep;

  memcpy(rd->mem_begin + rd->mem_pos, &time, sizeof(time));
  rd->mem_pos += sizeof(time);
  memcpy(rd->mem_begin + rd->mem_pos, regs, n_regs * sizeof(uint16_t));
  rd->mem_pos += n_regs * sizeof(uint16_t);
  rd->mem_pos = rd->mem_pos % mem_size;
}

static int fetch_monitored_data(void) {
  int pos;
  int error = 0;

  if (global_lock() != 0) {
    return -1;
  }

  if (rackmond_config.paused == 1) {
    global_unlock();
    usleep(1000);
    goto cleanup;
  }
  if (rackmond_config.config == NULL) {
    global_unlock();
    goto cleanup;
  }
  global_unlock();

  for (pos = 0; pos < ARRAY_SIZE(rackmond_config.stored_data); pos++) {
    int r;
    uint8_t addr;
    psu_datastore_t *mdata = rackmond_config.stored_data[pos];

    if (mdata == NULL) {
      continue;
    }

    addr = mdata->addr;
    for (r = 0; r < rackmond_config.config->num_intervals; r++) {
      int err;
      uint32_t timestamp;
      reg_range_data_t* rd = &mdata->range_data[r];
      monitor_interval* iv = rd->i;
      uint16_t regs[iv->len];

      err = read_holding_reg(&rackmond_config.rs485,
                             rackmond_config.modbus_timeout, addr,
                             iv->begin, iv->len, regs);

      sched_yield();
      if (err != 0) {
        if (err != READ_ERROR_RESPONSE) {
          log("Error %d reading %02x registers at %02x from %02x\n",
              err, iv->len, iv->begin, addr);
          if(err == MODBUS_BAD_CRC) {
            mdata->crc_errors++;
          }
          if(err == MODBUS_RESPONSE_TIMEOUT) {
            mdata->timeout_errors++;
          }
        }
        continue;
      }

      TIME_UPDATE(timestamp);
      if (iv->flags & MONITOR_FLAG_ONLY_CHANGES) {
        int pitch = REG_INT_DATA_SIZE(iv);
        int lastpos = rd->mem_pos - pitch;
        if (lastpos < 0) {
          lastpos = (pitch * iv->keep) - pitch;
        }
        if (!memcmp(rd->mem_begin + lastpos + sizeof(timestamp),
              regs, sizeof(uint16_t) * iv->len) &&
           memcmp(rd->mem_begin, "\x00\x00\x00\x00", 4)) {
          continue;
        }

        if (rackmond_config.status_log) {
          time_t rawt;
          struct tm* ti;
          char timestr[80];

          time(&rawt);
          ti = localtime(&rawt);
          strftime(timestr, sizeof(timestr), "%b %e %T", ti);
          fprintf(rackmond_config.status_log,
                  "%s: Change to status register %02x on address %02x. "
                  "New value: %02x\n",
                  timestr, iv->begin, addr, regs[0]);
          fflush(rackmond_config.status_log);
        }
      }

      global_lock();
      record_data(rd, timestamp, regs);
      global_unlock();
    } /* for each monitor_interval */
  } /* for each psu */

cleanup:
  return error;
}

static time_t search_at = 0;

void* monitoring_loop(void* arg) {

  rackmond_config.status_log = fopen(RACKMON_STAT_STORE, "a+");
  if (rackmond_config.status_log == NULL) {
    OBMC_ERROR(errno, "failed to open %s", RACKMON_STAT_STORE);
    /* XXX shall we exit the thread??? */
  }

  while(1) {
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    if (search_at < ts.tv_sec) {
      check_active_psus();
      alloc_monitoring_datas();
      clock_gettime(CLOCK_REALTIME, &ts);
      search_at = ts.tv_sec + PSU_SCAN_INTERVAL;
    }

    /*
     * Please be aware it takes ~9 seconds for rackmond to go through 6
     * PSUs and collect all the data, which means the minimum data refresh
     * interval is 9 seconds (for a specific PSU register, when fetching
     * data in a dead loop).
     * Adding a small delay has little impact when 6 PSUs are connected,
     * means CPU usage and data refresh interval is similar with or without
     * delay. But such delay saves a lot of CPU resources when no PSU is
     * connected: CPU usage goes from ~10% (dead loop) to ~0% when delay
     * is introduced.
     */
    fetch_monitored_data();
    sleep(PSU_REFRESH_DATA_INTERVAL);
  }
  return NULL;
}

static void close_rs485_dev(rs485_dev_t *dev)
{
  if (dev->tty_fd >= 0) {
    pthread_mutex_destroy(&dev->lock);
    close(dev->tty_fd);
  }
}

static int open_rs485_dev(const char* tty_filename, rs485_dev_t *dev) {
  int error = 0;
  struct serial_rs485 rs485conf;

  dbg("Opening %s\n", tty_filename);
  dev->tty_fd = open(tty_filename, O_RDWR | O_NOCTTY);
  ERR_EXIT(dev->tty_fd);

  /*
   * NOTE: "SER_RS485_RTS_AFTER_SEND" and "SER_RS485_RX_DURING_TX" flags
   * are not handled in kernel 4.1, but they are required in the latest
   * kernel.
   */
  memset(&rs485conf, 0, sizeof(rs485conf));
  rs485conf.flags = SER_RS485_ENABLED;
  rs485conf.flags |= (SER_RS485_RTS_AFTER_SEND | SER_RS485_RX_DURING_TX);

  dbg("Putting %s in RS485 mode\n", tty_filename);
  error = ioctl(dev->tty_fd, TIOCSRS485, &rs485conf);
  ERR_LOG_EXIT(error, "failed to turn on RS485 mode");

  error = pthread_mutex_init(&dev->lock, NULL);
  if (error != 0) {
    OBMC_ERROR(error, "failed to initialize rs485 dev mutex");
    error = -1;
  }

cleanup:
  return error;
}

static int run_cmd_raw_modbus(rackmond_command* cmd, write_buf_t *wb)
{
  char *resp_buf;
  int timeout, resp_len;
  uint16_t exp_resp_len, resp_len_wire;

  if (cmd->raw_modbus.custom_timeout) {
    timeout = cmd->raw_modbus.custom_timeout * 1000; // ms to us
  } else {
    timeout = rackmond_config.modbus_timeout;
  }

  exp_resp_len = cmd->raw_modbus.expected_response_length;
  if (exp_resp_len == 0) {
    exp_resp_len = 1024;
  }

  resp_buf = malloc(exp_resp_len);
  if (resp_buf == NULL) {
    OBMC_WARN("failed to allocate resp_buf for raw_modbus command");
    return -1;
  }

  resp_len = modbus_command(&rackmond_config.rs485, timeout,
                            cmd->raw_modbus.data, cmd->raw_modbus.length,
                            resp_buf, exp_resp_len, exp_resp_len);
  if(resp_len < 0) {
    uint16_t error = -resp_len;

    resp_len_wire = 0;
    buf_write(wb, &resp_len_wire, sizeof(uint16_t));
    buf_write(wb, &error, sizeof(uint16_t));
  } else {
    resp_len_wire = resp_len;
    buf_write(wb, &resp_len_wire, sizeof(uint16_t));
    buf_write(wb, resp_buf, resp_len);
  }

  free(resp_buf);
  return 0;
}

static int run_cmd_set_config(rackmond_command* cmd, write_buf_t *wb)
{
  int error = 0;
  size_t config_size;

  if (global_lock() != 0) {
    return -1;
  }

  if (rackmond_config.config != NULL) {
    BAIL("rackmond already configured\n");
  }

  config_size = sizeof(monitoring_config) +
          (sizeof(monitor_interval) * cmd->set_config.config.num_intervals);
  rackmond_config.config = calloc(1, config_size);
  if (rackmond_config.config == NULL) {
    BAIL("failed to allocate config structure");
  }

  memcpy(rackmond_config.config, &cmd->set_config.config, config_size);
  OBMC_INFO("got configuration");
  TIME_UPDATE(search_at);

cleanup:
  global_unlock();
  return error;
}

static int run_cmd_dump_status(rackmond_command* cmd, write_buf_t *wb)
{
  if (global_lock() != 0) {
    return -1;
  }

  if (rackmond_config.config == NULL) {
    buf_printf(wb, "Unconfigured\n");
  } else {
    int i;
    time_t now;

    TIME_UPDATE(now);
    buf_printf(wb, "Monitored PSUs:\n");
    for (i = 0; i < ARRAY_SIZE(rackmond_config.stored_data); i++) {
      if (rackmond_config.stored_data[i] == NULL) {
        continue;
      }

      buf_printf(wb, "PSU addr %02x - crc errors: %d, timeouts: %d\n",
                 rackmond_config.stored_data[i]->addr,
                 rackmond_config.stored_data[i]->crc_errors,
                 rackmond_config.stored_data[i]->timeout_errors);
    }

    buf_printf(wb, "Active on last scan: ");
    for (i = 0; i < rackmond_config.num_active_addrs; i++) {
      buf_printf(wb, "%02x ", rackmond_config.active_addrs[i]);
    }
    buf_printf(wb, "\n");
    buf_printf(wb, "Next scan in %d seconds.\n", search_at - now);
  }

 global_unlock();
 return 0;
}

static int run_cmd_force_scan(rackmond_command* cmd, write_buf_t *wb)
{
  if (global_lock() != 0) {
    return -1;
  }

  if (rackmond_config.config == NULL) {
    buf_printf(wb, "Unconfigured\n");
  } else {
    TIME_UPDATE(search_at);
    buf_printf(wb, "Triggering PSU scan...\n");
  }

  global_unlock();
  return 0;
}

static int run_cmd_dump_json(rackmond_command* cmd, write_buf_t *wb)
{
  if (global_lock() != 0) {
    return -1;
  }

  if (rackmond_config.config == NULL) {
    buf_write(wb, "[]", 2);
  } else {
    int i, j, c;
    uint32_t now;
    int data_pos = 0;

    TIME_UPDATE(now);
    buf_write(wb, "[", 1);
    while (rackmond_config.stored_data[data_pos] != NULL &&
           data_pos < MAX_ACTIVE_ADDRS) {
      psu_datastore_t *pdata = rackmond_config.stored_data[data_pos];

      buf_printf(wb, "{\"addr\":%d,\"crc_fails\":%d,\"timeouts\":%d,"
                 "\"now\":%d,\"ranges\":[",
                 pdata->addr, pdata->crc_errors, pdata->timeout_errors, now);

      for (i = 0; i < rackmond_config.config->num_intervals; i++) {
        uint32_t time;
        reg_range_data_t *rd = &pdata->range_data[i];
        char* mem_pos = rd->mem_begin;

        buf_printf(wb,"{\"begin\":%d,\"readings\":[", rd->i->begin);
        // want to cut the list off early just before
        // the first entry with time == 0
        memcpy(&time, mem_pos, sizeof(time));
        for(j = 0; j < rd->i->keep && time != 0; j++) {
          mem_pos += sizeof(time);
          buf_printf(wb, "{\"time\":%d,\"data\":\"", time);
          for(c = 0; c < rd->i->len * 2; c++) {
            buf_printf(wb, "%02x", *mem_pos);
            mem_pos++;
          }
          buf_write(wb, "\"}", 2);
          memcpy(&time, mem_pos, sizeof(time));
          if (time == 0) {
            break;
          }
          if ((j+1) < rd->i->keep) {
            buf_write(wb, ",", 1);
          }
        }
        buf_write(wb, "]}", 2);
        if ((i+1) < rackmond_config.config->num_intervals) {
          buf_write(wb, ",", 1);
        }
      }

      data_pos++;
      if (data_pos < MAX_ACTIVE_ADDRS &&
          rackmond_config.stored_data[data_pos] != NULL) {
        buf_write(wb, "]},", 3);
      } else {
        buf_write(wb, "]}", 2);
      }
    }
    buf_write(wb, "]", 1);
  }

  global_unlock();
  return 0;
}

static int run_cmd_pause_monitoring(rackmond_command* cmd, write_buf_t *wb)
{
  uint8_t was_paused;

  if (global_lock() != 0) {
    return -1;
  }

  was_paused = rackmond_config.paused;
  rackmond_config.paused = 1;
  buf_write(wb, &was_paused, sizeof(was_paused));

  global_unlock();
  return 0;
}

static int run_cmd_start_monitoring(rackmond_command* cmd, write_buf_t *wb)
{
  uint8_t was_started;

  if (global_lock() != 0) {
    return -1;
  }

  was_started = !rackmond_config.paused;
  rackmond_config.paused = 0;
  buf_write(wb, &was_started, sizeof(was_started));

  global_unlock();
  return 0;
}

static struct {
  const char *name;
  int (*handler)(rackmond_command* cmd, write_buf_t *wb);
} rackmond_cmds[COMMAND_TYPE_MAX] = {
  [COMMAND_TYPE_RAW_MODBUS] = {
    .name = "raw_modbus",
    .handler = run_cmd_raw_modbus,
  },
  [COMMAND_TYPE_SET_CONFIG] = {
    .name = "set_config",
    .handler = run_cmd_set_config,
  },
  [COMMAND_TYPE_DUMP_DATA_JSON] = {
    .name = "dump_data_json",
    .handler = run_cmd_dump_json,
  },
  [COMMAND_TYPE_PAUSE_MONITORING] = {
    .name = "pause_monitoring",
    .handler = run_cmd_pause_monitoring,
  },
  [COMMAND_TYPE_START_MONITORING] = {
    .name = "start_monitoring",
    .handler = run_cmd_start_monitoring,
  },
  [COMMAND_TYPE_DUMP_STATUS] = {
    .name = "dump_status",
    .handler = run_cmd_dump_status,
  },
  [COMMAND_TYPE_FORCE_SCAN] = {
    .name = "force_scan",
    .handler = run_cmd_force_scan,
  },
};

static int do_command(int sock, rackmond_command* cmd) {
  int error = 0;
  write_buf_t wb;
  uint16_t type = cmd->type;

  //128k write buffer
  if (buf_open(&wb, sock, 128*1000) != 0) {
    return -1;
  }

  if (type <= COMMAND_TYPE_NONE || type >= COMMAND_TYPE_MAX ||
      rackmond_cmds[type].name == NULL) {
    BAIL("Unknown command type %u\n", type);
  }
  dbg("processing command %s\n", rackmond_cmds[type].name);

  assert(rackmond_cmds[type].handler != NULL);
  error = rackmond_cmds[type].handler(cmd, &wb);
  if (error != 0) {
    OBMC_WARN("error while processing command %s", rackmond_cmds[type].name);
  } else {
    dbg("command %s was handled properly", rackmond_cmds[type].name);
  }

cleanup:
  buf_close(&wb);
  return error;
}

// receive the command as a length prefixed block
// (uint16_t, followed by data)
// this is all over a local socket, won't be doing
// endian flipping, clients should only be local procs
// compiled for the same arch

static int recv_sock_message(int sock, void *buf, size_t size)
{
  int ret = 0;
  struct pollfd pfd = {
    .fd = sock,
    .events = POLLIN | POLLERR | POLLHUP,
  };

  do {
    ret = poll(&pfd, 1, 1000);
    if (ret < 0) {
      return -1;
    } else if (ret == 0) {
      dbg("poll socket (%d) timed out", sock);
      continue;
    } else if (pfd.revents & (POLLERR | POLLHUP)) {
      errno = EIO;
      return -1;
    }

    ret = recv(sock, buf, size, MSG_DONTWAIT);
  } while (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));

  return ret;
}

static int handle_connection(int sock) {
  int ret;
  int error = 0;
  char body_buf[1024];
  uint16_t body_len = 0;

  dbg("new connection established, socket=%d\n", sock);

  ret = recv_sock_message(sock, &body_len, sizeof(body_len));
  if (ret < 0) {
    OBMC_ERROR(errno, "failed to recv message header from socket %d", sock);
    return -1;
  } else if (ret < sizeof(body_len)) {
    OBMC_WARN("message header short read, expect %u, actual %u",
              sizeof(body_len), ret);
    return -1;
  } else if (body_len > sizeof(body_buf)) {
    OBMC_WARN("message body exceeds max size (%u > %u)",
              body_len, sizeof(body_buf));
    return -1;
  }

  ret = recv_sock_message(sock, body_buf, body_len);
  if (ret < 0) {
    OBMC_ERROR(errno, "failed to recv message body from socket %d", sock);
    return -1;
  }

  error = do_command(sock, (rackmond_command*)body_buf);

  return error;
}

int main(int argc, char** argv) {
  int error = 0;
  int sock, sock_len;
  pthread_t monitoring_tid;
  struct sockaddr_un local, client;

  signal(SIGPIPE, SIG_IGN);

  obmc_log_init("rackmond", LOG_INFO, 0);
  obmc_log_set_syslog(LOG_CONS, LOG_DAEMON);
  if (getenv("RACKMOND_FOREGROUND") == NULL) {
    obmc_log_unset_std_stream();
    daemon(0, 0);
  }

  if (getenv("RACKMOND_TIMEOUT") != NULL) {
    rackmond_config.modbus_timeout = atoll(getenv("RACKMOND_TIMEOUT"));
    OBMC_INFO("set timeout to RACKMOND_TIMEOUT (%dms)",
              rackmond_config.modbus_timeout / 1000);
  }
  if (getenv("RACKMOND_MIN_DELAY") != NULL) {
    rackmond_config.min_delay = atoll(getenv("RACKMOND_MIN_DELAY"));
    OBMC_INFO("set mindelay to RACKMOND_MIN_DELAY(%dus)",
              rackmond_config.min_delay);
  }
  verbose = getenv("RACKMOND_VERBOSE") != NULL ? 1 : 0;

  OBMC_INFO("rackmon/modbus service starting");
  ERR_EXIT(open_rs485_dev(DEFAULT_TTY, &rackmond_config.rs485));

  error = pthread_create(&monitoring_tid, NULL, monitoring_loop, NULL);
  if (error != 0) {
    OBMC_ERROR(error, "failed to create monitor loop thread");
    error = -1;
    goto cleanup;
  }

  sock = socket(AF_UNIX, SOCK_STREAM, 0);
  ERR_LOG_EXIT(sock, "failed to create socket");

  unlink(RACKMON_IPC_SOCKET); /* ignore failures */
  local.sun_family = AF_UNIX;
  strcpy(local.sun_path, RACKMON_IPC_SOCKET);
  sock_len = sizeof(local.sun_family) + strlen(local.sun_path);
  ERR_LOG_EXIT(bind(sock, (struct sockaddr *)&local, sock_len),
               "failed to bind socket");

  ERR_LOG_EXIT(listen(sock, 20), "failed to set passive socket");

  OBMC_INFO("rackmon/modbus service listening");
  while(1) {
    socklen_t clisocklen = sizeof(struct sockaddr_un);
    int clisock = accept(sock, (struct sockaddr*) &client, &clisocklen);
    ERR_LOG_EXIT(clisock, "failed to accept connect request");

    handle_connection(clisock);
    close(clisock);
  }

cleanup:
  close_rs485_dev(&rackmond_config.rs485);
  if (error != 0) {
    error = 1;
  }
  return error;
}
