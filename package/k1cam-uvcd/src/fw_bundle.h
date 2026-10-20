/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * fw_bundle.h -- k1cam.upd, the routine kernel + rootfs update bundle.
 *
 * Version 1, all integers little-endian, every reserved byte zero:
 *
 *   0x000 magic "K1CAMUPD"
 *   0x008 u16 format version (1)
 *   0x00a u16 header size (256)
 *   0x00c u32 flags (0)
 *   0x010 board id[32], layout id[32], build id[64]: printable ASCII,
 *         NUL-terminated and NUL-padded
 *   0x090 u32 lowest boot compatibility level the release needs
 *   0x094 u16 component count (2), 0x096 u16 record size (64)
 *   0x098 u32 bundle length (the whole file)
 *   0x0c0 manifest SHA-256: of bytes 0x000-0x17f with this field zeroed
 *   0x100 component records, kernel then rootfs, 64 bytes each:
 *         u16 type (1 kernel, 2 rootfs), u16 flags (0), u32 payload
 *         offset, u32 payload length, u32 installed length (= payload
 *         length), SHA-256 of the payload
 *   0x180 payloads at 64-byte aligned offsets, in record order, zero gaps,
 *         the last one ending the file
 *
 * The digests catch corruption; they don't authenticate the publisher.
 * Signing would be a new version with a compiled-in key.
 */
#ifndef FW_BUNDLE_H
#define FW_BUNDLE_H

#include <stddef.h>
#include <stdint.h>

#include "fw_sha256.h"

#define FW_BUNDLE_MAGIC "K1CAMUPD"
#define FW_BUNDLE_VERSION 1
#define FW_BUNDLE_HEADER_SIZE 256
#define FW_BUNDLE_RECORD_SIZE 64
#define FW_BUNDLE_COMPONENTS 2
#define FW_BUNDLE_MANIFEST_SIZE (FW_BUNDLE_HEADER_SIZE + FW_BUNDLE_COMPONENTS * FW_BUNDLE_RECORD_SIZE)
#define FW_BUNDLE_ALIGN 64

#define FW_BUNDLE_BOARD_LEN 32
#define FW_BUNDLE_LAYOUT_LEN 32
#define FW_BUNDLE_BUILD_LEN 64

/* Header offsets */
#define FW_BUNDLE_OFF_MAGIC 0x000
#define FW_BUNDLE_OFF_VERSION 0x008
#define FW_BUNDLE_OFF_HEADER_SIZE 0x00a
#define FW_BUNDLE_OFF_FLAGS 0x00c
#define FW_BUNDLE_OFF_BOARD 0x010
#define FW_BUNDLE_OFF_LAYOUT 0x030
#define FW_BUNDLE_OFF_BUILD 0x050
#define FW_BUNDLE_OFF_MIN_BOOT 0x090
#define FW_BUNDLE_OFF_COUNT 0x094
#define FW_BUNDLE_OFF_RECORD_SIZE 0x096
#define FW_BUNDLE_OFF_LENGTH 0x098
#define FW_BUNDLE_OFF_MANIFEST_SHA 0x0c0
#define FW_BUNDLE_OFF_RECORDS 0x100

/* Record offsets */
#define FW_BUNDLE_REC_TYPE 0x00
#define FW_BUNDLE_REC_FLAGS 0x02
#define FW_BUNDLE_REC_OFFSET 0x04
#define FW_BUNDLE_REC_LENGTH 0x08
#define FW_BUNDLE_REC_INSTALLED 0x0c
#define FW_BUNDLE_REC_SHA 0x10

enum fw_component {
	FW_COMPONENT_KERNEL = 1,
	FW_COMPONENT_ROOTFS = 2,
};

/* Why a bundle was refused; maps onto the protocol's error codes. */
enum fw_bundle_status {
	FW_BUNDLE_OK = 0,
	FW_BUNDLE_FORMAT,     /* malformed, unknown version or nonzero reserved bytes */
	FW_BUNDLE_HASH,       /* manifest or payload digest mismatch */
	FW_BUNDLE_IMAGE,      /* the uImage or SquashFS inside is not intact */
	FW_BUNDLE_TOO_LARGE,  /* a component is bigger than its partition */
	FW_BUNDLE_INCOMPATIBLE, /* another board or layout, or newer boot compat */
};

struct fw_bundle_component {
	enum fw_component type;
	uint32_t offset;
	uint32_t length;
	uint8_t sha256[FW_SHA256_SIZE];
};

struct fw_bundle {
	char board[FW_BUNDLE_BOARD_LEN];
	char layout[FW_BUNDLE_LAYOUT_LEN];
	char build[FW_BUNDLE_BUILD_LEN];
	uint32_t min_boot_compat;
	uint32_t length;
	struct fw_bundle_component component[FW_BUNDLE_COMPONENTS]; /* kernel, rootfs */
};

/* Check everything that can be checked from the bundle alone: structure,
 * digests, the uImage header and data CRCs, the SquashFS superblock, and
 * that each component fits its partition. why (may be NULL) gets a short
 * reason. */
enum fw_bundle_status fw_bundle_parse(const uint8_t *buf, size_t len, struct fw_bundle *b,
				      char *why, size_t why_size);

/* Check that a parsed bundle is for this board and layout, and that the
 * installed boot environment (boot_compat) can boot it. */
enum fw_bundle_status fw_bundle_compatible(const struct fw_bundle *b, uint32_t boot_compat,
					   char *why, size_t why_size);

/* Assemble a bundle. Returns the total length written to out (NULL to
 * just measure), or 0 if a string is too long. Deterministic: the same
 * inputs give the same bytes. */
size_t fw_bundle_build(uint8_t *out, const char *board, const char *layout, const char *build,
		       uint32_t min_boot_compat, const uint8_t *kernel, uint32_t kernel_len,
		       const uint8_t *rootfs, uint32_t rootfs_len);

/* Individual image checks, also used on what was read back from flash. */
int fw_check_uimage(const uint8_t *buf, uint32_t len, char *why, size_t why_size);
int fw_check_squashfs(const uint8_t *buf, uint32_t len, char *why, size_t why_size);

const char *fw_bundle_status_name(enum fw_bundle_status status);

#endif /* FW_BUNDLE_H */
