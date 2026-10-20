/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * fw_sha256.h -- SHA-256 (FIPS 180-4) for firmware images and dumps.
 *
 * In-process, so that the updater's integrity checks never depend on
 * running and parsing a shell tool. Shared by uvcd, uvcdctl and the
 * bundle builder.
 */
#ifndef FW_SHA256_H
#define FW_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define FW_SHA256_SIZE 32

struct fw_sha256 {
	uint32_t h[8];
	uint64_t length; /* bytes hashed so far */
	uint8_t block[64];
	size_t used; /* bytes waiting in block */
};

void fw_sha256_init(struct fw_sha256 *s);
void fw_sha256_update(struct fw_sha256 *s, const void *data, size_t len);
void fw_sha256_final(struct fw_sha256 *s, uint8_t digest[FW_SHA256_SIZE]);
void fw_sha256(const void *data, size_t len, uint8_t digest[FW_SHA256_SIZE]);

/* Lower-case hex, NUL-terminated: out needs 2 * FW_SHA256_SIZE + 1 bytes. */
void fw_sha256_hex(const uint8_t digest[FW_SHA256_SIZE], char *out);

#endif /* FW_SHA256_H */
