# Copyright 2015-present Facebook. All Rights Reserved.
all: guid-util

CFLAGS += -Wall -Werror

guid-util: guid-util.c
	$(CC) $(CFLAGS) -lpal -std=c99 -o $@ $^ $(LDFLAGS)

.PHONY: clean

clean:
	rm -rf *.o guid-util
