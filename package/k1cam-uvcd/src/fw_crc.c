/* SPDX-License-Identifier: GPL-3.0-only */
#include "fw_crc.h"

uint32_t fw_crc32(uint32_t crc, const void *data, size_t len)
{
	const uint8_t *p = data;

	crc = ~crc;
	while (len--) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++)
			crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
	}
	return ~crc;
}

uint16_t fw_crc16(uint16_t crc, const void *data, size_t len)
{
	const uint8_t *p = data;

	while (len--) {
		crc ^= (uint16_t)(*p++ << 8);
		for (int i = 0; i < 8; i++)
			crc = (uint16_t)(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
	}
	return crc;
}
