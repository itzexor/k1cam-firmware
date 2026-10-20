/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * fw_crc.h -- the two checksums the firmware formats use.
 *
 * fw_crc32 is zlib's CRC-32, the one U-Boot uses for its environment and
 * for uImage headers and data: fw_crc32(0, buf, len) == U-Boot's
 * crc32(0, buf, len). fw_crc16 is CRC-16/CCITT-FALSE (poly 0x1021, init
 * 0xffff, no reflection), which guards each 60-byte XU DATA frame.
 */
#ifndef FW_CRC_H
#define FW_CRC_H

#include <stddef.h>
#include <stdint.h>

uint32_t fw_crc32(uint32_t crc, const void *data, size_t len);
uint16_t fw_crc16(uint16_t crc, const void *data, size_t len);
#define FW_CRC16_INIT 0xffff

#endif /* FW_CRC_H */
