/**
 * Copyright 2020-present Facebook. All Rights Reserved.
 *
 * This program file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program in a file named COPYING; if not, write to the
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA
 */

package partition

import (
	"bytes"
	"crypto/md5"
	"encoding/hex"
	"encoding/json"

	"github.com/pkg/errors"
)

// supported FBOBMC_IMAGE_META_VER
var fbmetaSupportedVersions = []int{1}

const fbmetaImageMetaPartitionSize = 64 * 1024
const fbmetaImageMetaPartitionOffset = 0x000F0000

// FBMetaInfo contains relevant info in the image-meta partition.
type FBMetaInfo struct {
	FBOBMC_IMAGE_META_VER int              `json:"FBOBMC_IMAGE_META_VER"`
	PartInfos             []FBMetaPartInfo `json:"part_infos"`
}

// FBMetaPartInfo is analogous to PartitionConfigInfo, but a shim is required
// to convert it properly to use the correct "type".
type FBMetaPartInfo struct {
	Name   string `json:"name"`
	Size   uint32 `json:"size"`
	Offset uint32 `json:"offset"`
	Type   string `json:"type"`
	// For now, the only checksum used is MD5
	Checksum string `json:"md5"`
	// applicable only for FIT partitions
	// this is the minimum number of children nodes of the 'images' node
	FitImageNodes uint32 `json:"num-nodes"`
}

// FBMetaChecksum is a separate JSON that contains the md5 checksum
// of the image-meta partition.
type FBMetaChecksum struct {
	Checksum string `json:"meta_md5"`
}

/**
 * FBMetaImagePartition is a full image that contains the image-meta partition.
 * The image-meta partition contains the information
 * of the partitions and indicates the validation scheme
 * required
 */
type FBMetaImagePartition struct {
	Name     string
	Data     []byte
	Offset   uint32
	metaInfo FBMetaInfo
}

// parseAndValidateFBImageMetaFBJSON parses and validates FBMetaInfo given
// the bytes containing the meta-partition region
var parseAndValidateFBImageMetaJSON = func(data []byte) (FBMetaInfo, error) {
	var metaInfo FBMetaInfo
	var metaChecksum FBMetaChecksum
	// the image-meta partition contains two lines
	// ending with '\n' (0x0A).
	// the first line contains the image-meta JSON (FBMetaInfo)
	// the second line contains the checksum for image-meta (FBMetaChecksum)
	splitData := bytes.Split(data, []byte{'\n'})
	// must have at least 3 elements
	if len(splitData) < 3 {
		return metaInfo, errors.Errorf("Meta partition incomplete: cannot find two lines of " +
			"JSON")
	}

	imageMetaJSONData := splitData[0]
	imageMetaChecksumJSONData := splitData[1]

	// get the checksums first
	err := json.Unmarshal(imageMetaChecksumJSONData, &metaChecksum)
	if err != nil {
		return metaInfo, errors.Errorf("Unable to unmarshal image-meta checksum JSON: %v",
			err)
	}
	checksum := metaChecksum.Checksum

	// calculate md5sum of imageMetaJSONData
	hash := md5.Sum(imageMetaJSONData)
	calcChecksum := hex.EncodeToString(hash[:])

	if calcChecksum != checksum {
		return metaInfo, errors.Errorf("'image-meta' checksum (%v) does not match checksums supplied (%v)",
			calcChecksum, checksum)
	}

	// get metaInfo
	err = json.Unmarshal(imageMetaJSONData, &metaInfo)
	if err != nil {
		return metaInfo, errors.Errorf("Unable to unmarshal image-meta JSON: %v",
			err)
	}

	return metaInfo, nil
}
