/* SPDX-License-Identifier: GPL-3.0-only */
#include "fw_bundle.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "fw_crc.h"
#include "fw_layout.h"

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

static void say(char *why, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void say(char *why, size_t size, const char *fmt, ...)
{
	if (!why || !size)
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(why, size, fmt, ap);
	va_end(ap);
}

static bool all_zero(const uint8_t *p, size_t len)
{
	while (len--) {
		if (*p++)
			return false;
	}
	return true;
}

/* A NUL-terminated, NUL-padded field of printable ASCII, not empty. */
static bool string_field(const uint8_t *p, size_t size, char *out)
{
	size_t len = strnlen((const char *)p, size);
	if (len == 0 || len == size || !all_zero(p + len, size - len))
		return false;
	for (size_t i = 0; i < len; i++) {
		if (p[i] < 0x20 || p[i] > 0x7e)
			return false;
	}
	memcpy(out, p, len + 1);
	return true;
}

static void manifest_digest(const uint8_t *buf, uint8_t digest[FW_SHA256_SIZE])
{
	static const uint8_t zero[FW_SHA256_SIZE];
	struct fw_sha256 s;
	fw_sha256_init(&s);
	fw_sha256_update(&s, buf, FW_BUNDLE_OFF_MANIFEST_SHA);
	fw_sha256_update(&s, zero, sizeof(zero));
	fw_sha256_update(&s, buf + FW_BUNDLE_OFF_MANIFEST_SHA + FW_SHA256_SIZE,
			 FW_BUNDLE_MANIFEST_SIZE - FW_BUNDLE_OFF_MANIFEST_SHA - FW_SHA256_SIZE);
	fw_sha256_final(&s, digest);
}

int fw_check_uimage(const uint8_t *buf, uint32_t len, char *why, size_t why_size)
{
	uint8_t header[64];

	if (len < sizeof(header) || be32(buf) != 0x27051956) {
		say(why, why_size, "kernel is not a uImage");
		return -1;
	}
	memcpy(header, buf, sizeof(header));
	memset(header + 4, 0, 4);
	if (fw_crc32(0, header, sizeof(header)) != be32(buf + 4)) {
		say(why, why_size, "uImage header CRC mismatch");
		return -1;
	}
	if (be32(buf + 12) != len - sizeof(header)) {
		say(why, why_size, "uImage says %u data bytes, component has %u", be32(buf + 12),
		    len - (uint32_t)sizeof(header));
		return -1;
	}
	if (fw_crc32(0, buf + sizeof(header), len - sizeof(header)) != be32(buf + 24)) {
		say(why, why_size, "uImage data CRC mismatch");
		return -1;
	}
	/* IH_OS_LINUX, IH_ARCH_MIPS, IH_TYPE_KERNEL */
	if (buf[28] != 5 || buf[29] != 5 || buf[30] != 2) {
		say(why, why_size, "uImage is not a Linux MIPS kernel (os %u arch %u type %u)",
		    buf[28], buf[29], buf[30]);
		return -1;
	}
	return 0;
}

int fw_check_squashfs(const uint8_t *buf, uint32_t len, char *why, size_t why_size)
{
	if (len < 96 || le32(buf) != 0x73717368) {
		say(why, why_size, "rootfs is not SquashFS");
		return -1;
	}
	uint32_t block_size = le32(buf + 12);
	uint16_t block_log = le16(buf + 22);
	if (le16(buf + 28) != 4 || le16(buf + 30) != 0 || block_log < 12 || block_log > 20 ||
	    block_size != 1u << block_log) {
		say(why, why_size, "unsupported SquashFS superblock (version %u.%u, block %u)",
		    le16(buf + 28), le16(buf + 30), block_size);
		return -1;
	}
	uint64_t used = (uint64_t)le32(buf + 40) | (uint64_t)le32(buf + 44) << 32;
	/* mksquashfs pads the image with zeros to a 4 KiB multiple. */
	if (used < 96 || used > len || len - used >= 4096 || !all_zero(buf + used, len - used)) {
		say(why, why_size, "SquashFS uses %llu bytes of a %u byte component",
		    (unsigned long long)used, len);
		return -1;
	}
	return 0;
}

enum fw_bundle_status fw_bundle_parse(const uint8_t *buf, size_t len, struct fw_bundle *b,
				      char *why, size_t why_size)
{
	uint8_t digest[FW_SHA256_SIZE];

	memset(b, 0, sizeof(*b));
	if (len < FW_BUNDLE_MANIFEST_SIZE || memcmp(buf, FW_BUNDLE_MAGIC, 8) != 0) {
		say(why, why_size, "not a k1cam update bundle");
		return FW_BUNDLE_FORMAT;
	}
	if (le16(buf + FW_BUNDLE_OFF_VERSION) != FW_BUNDLE_VERSION) {
		say(why, why_size, "bundle format version %u is not supported",
		    le16(buf + FW_BUNDLE_OFF_VERSION));
		return FW_BUNDLE_FORMAT;
	}
	if (le16(buf + FW_BUNDLE_OFF_HEADER_SIZE) != FW_BUNDLE_HEADER_SIZE ||
	    le32(buf + FW_BUNDLE_OFF_FLAGS) != 0 ||
	    le16(buf + FW_BUNDLE_OFF_COUNT) != FW_BUNDLE_COMPONENTS ||
	    le16(buf + FW_BUNDLE_OFF_RECORD_SIZE) != FW_BUNDLE_RECORD_SIZE ||
	    !all_zero(buf + 0x09c, FW_BUNDLE_OFF_MANIFEST_SHA - 0x09c) ||
	    !all_zero(buf + FW_BUNDLE_OFF_MANIFEST_SHA + FW_SHA256_SIZE,
		      FW_BUNDLE_OFF_RECORDS - FW_BUNDLE_OFF_MANIFEST_SHA - FW_SHA256_SIZE)) {
		say(why, why_size, "malformed bundle header");
		return FW_BUNDLE_FORMAT;
	}
	if (le32(buf + FW_BUNDLE_OFF_LENGTH) != len) {
		say(why, why_size, "bundle says %u bytes, file has %zu",
		    le32(buf + FW_BUNDLE_OFF_LENGTH), len);
		return FW_BUNDLE_FORMAT;
	}
	manifest_digest(buf, digest);
	if (memcmp(digest, buf + FW_BUNDLE_OFF_MANIFEST_SHA, FW_SHA256_SIZE) != 0) {
		say(why, why_size, "bundle manifest digest mismatch");
		return FW_BUNDLE_HASH;
	}
	if (!string_field(buf + FW_BUNDLE_OFF_BOARD, FW_BUNDLE_BOARD_LEN, b->board) ||
	    !string_field(buf + FW_BUNDLE_OFF_LAYOUT, FW_BUNDLE_LAYOUT_LEN, b->layout) ||
	    !string_field(buf + FW_BUNDLE_OFF_BUILD, FW_BUNDLE_BUILD_LEN, b->build)) {
		say(why, why_size, "malformed bundle identifiers");
		return FW_BUNDLE_FORMAT;
	}
	b->min_boot_compat = le32(buf + FW_BUNDLE_OFF_MIN_BOOT);
	b->length = (uint32_t)len;

	static const enum fw_component order[FW_BUNDLE_COMPONENTS] = {FW_COMPONENT_KERNEL,
								     FW_COMPONENT_ROOTFS};
	static const uint32_t capacity[FW_BUNDLE_COMPONENTS] = {FW_KERNEL_CAPACITY,
							       FW_ROOTFS_CAPACITY};
	uint64_t end = FW_BUNDLE_MANIFEST_SIZE;
	for (int i = 0; i < FW_BUNDLE_COMPONENTS; i++) {
		const uint8_t *r = buf + FW_BUNDLE_OFF_RECORDS + i * FW_BUNDLE_RECORD_SIZE;
		struct fw_bundle_component *c = &b->component[i];
		c->type = (enum fw_component)le16(r + FW_BUNDLE_REC_TYPE);
		c->offset = le32(r + FW_BUNDLE_REC_OFFSET);
		c->length = le32(r + FW_BUNDLE_REC_LENGTH);
		memcpy(c->sha256, r + FW_BUNDLE_REC_SHA, FW_SHA256_SIZE);
		if (c->type != order[i] || le16(r + FW_BUNDLE_REC_FLAGS) != 0 ||
		    le32(r + FW_BUNDLE_REC_INSTALLED) != c->length ||
		    !all_zero(r + FW_BUNDLE_REC_SHA + FW_SHA256_SIZE,
			      FW_BUNDLE_RECORD_SIZE - FW_BUNDLE_REC_SHA - FW_SHA256_SIZE)) {
			say(why, why_size, "malformed component record %d", i);
			return FW_BUNDLE_FORMAT;
		}
		/* In order, aligned, not overlapping, zeros in between, and
		 * all inside the file (64-bit sums: no wraparound). */
		if (c->length == 0 || c->offset % FW_BUNDLE_ALIGN || c->offset < end ||
		    (uint64_t)c->offset + c->length > len || !all_zero(buf + end, c->offset - end)) {
			say(why, why_size, "component %d has a bad offset or length", i);
			return FW_BUNDLE_FORMAT;
		}
		end = (uint64_t)c->offset + c->length;
		if (c->length > capacity[i]) {
			say(why, why_size, "%s is %u bytes, its partition holds %u",
			    i ? "rootfs" : "kernel", c->length, capacity[i]);
			return FW_BUNDLE_TOO_LARGE;
		}
	}
	if (end != len) {
		say(why, why_size, "bundle has %llu bytes after its last component",
		    (unsigned long long)(len - end));
		return FW_BUNDLE_FORMAT;
	}
	for (int i = 0; i < FW_BUNDLE_COMPONENTS; i++) {
		const struct fw_bundle_component *c = &b->component[i];
		fw_sha256(buf + c->offset, c->length, digest);
		if (memcmp(digest, c->sha256, FW_SHA256_SIZE) != 0) {
			say(why, why_size, "%s digest mismatch", i ? "rootfs" : "kernel");
			return FW_BUNDLE_HASH;
		}
	}
	if (fw_check_uimage(buf + b->component[0].offset, b->component[0].length, why,
			    why_size) != 0 ||
	    fw_check_squashfs(buf + b->component[1].offset, b->component[1].length, why,
			      why_size) != 0)
		return FW_BUNDLE_IMAGE;
	return FW_BUNDLE_OK;
}

enum fw_bundle_status fw_bundle_compatible(const struct fw_bundle *b, uint32_t boot_compat,
					   char *why, size_t why_size)
{
	if (strcmp(b->board, FW_BOARD_ID) != 0) {
		say(why, why_size, "bundle is for board %s, not %s", b->board, FW_BOARD_ID);
		return FW_BUNDLE_INCOMPATIBLE;
	}
	if (strcmp(b->layout, FW_LAYOUT_AB_ID) != 0) {
		say(why, why_size, "bundle is for flash layout %s, not %s", b->layout,
		    FW_LAYOUT_AB_ID);
		return FW_BUNDLE_INCOMPATIBLE;
	}
	if (b->min_boot_compat > boot_compat) {
		say(why, why_size, "bundle needs boot compatibility %u, the camera has %u",
		    b->min_boot_compat, boot_compat);
		return FW_BUNDLE_INCOMPATIBLE;
	}
	return FW_BUNDLE_OK;
}

static uint32_t align_up(uint32_t v)
{
	return (v + FW_BUNDLE_ALIGN - 1) & ~(uint32_t)(FW_BUNDLE_ALIGN - 1);
}

size_t fw_bundle_build(uint8_t *out, const char *board, const char *layout, const char *build,
		       uint32_t min_boot_compat, const uint8_t *kernel, uint32_t kernel_len,
		       const uint8_t *rootfs, uint32_t rootfs_len)
{
	if (strlen(board) >= FW_BUNDLE_BOARD_LEN || strlen(layout) >= FW_BUNDLE_LAYOUT_LEN ||
	    strlen(build) >= FW_BUNDLE_BUILD_LEN)
		return 0;
	uint32_t kernel_off = FW_BUNDLE_MANIFEST_SIZE;
	uint32_t rootfs_off = align_up(kernel_off + kernel_len);
	size_t total = (size_t)rootfs_off + rootfs_len;
	if (!out)
		return total;

	memset(out, 0, total);
	memcpy(out + FW_BUNDLE_OFF_MAGIC, FW_BUNDLE_MAGIC, 8);
	put_le16(out + FW_BUNDLE_OFF_VERSION, FW_BUNDLE_VERSION);
	put_le16(out + FW_BUNDLE_OFF_HEADER_SIZE, FW_BUNDLE_HEADER_SIZE);
	memcpy(out + FW_BUNDLE_OFF_BOARD, board, strlen(board));
	memcpy(out + FW_BUNDLE_OFF_LAYOUT, layout, strlen(layout));
	memcpy(out + FW_BUNDLE_OFF_BUILD, build, strlen(build));
	put_le32(out + FW_BUNDLE_OFF_MIN_BOOT, min_boot_compat);
	put_le16(out + FW_BUNDLE_OFF_COUNT, FW_BUNDLE_COMPONENTS);
	put_le16(out + FW_BUNDLE_OFF_RECORD_SIZE, FW_BUNDLE_RECORD_SIZE);
	put_le32(out + FW_BUNDLE_OFF_LENGTH, (uint32_t)total);

	const uint8_t *data[FW_BUNDLE_COMPONENTS] = {kernel, rootfs};
	const uint32_t offset[FW_BUNDLE_COMPONENTS] = {kernel_off, rootfs_off};
	const uint32_t length[FW_BUNDLE_COMPONENTS] = {kernel_len, rootfs_len};
	for (int i = 0; i < FW_BUNDLE_COMPONENTS; i++) {
		uint8_t *r = out + FW_BUNDLE_OFF_RECORDS + i * FW_BUNDLE_RECORD_SIZE;
		put_le16(r + FW_BUNDLE_REC_TYPE, i ? FW_COMPONENT_ROOTFS : FW_COMPONENT_KERNEL);
		put_le32(r + FW_BUNDLE_REC_OFFSET, offset[i]);
		put_le32(r + FW_BUNDLE_REC_LENGTH, length[i]);
		put_le32(r + FW_BUNDLE_REC_INSTALLED, length[i]);
		fw_sha256(data[i], length[i], r + FW_BUNDLE_REC_SHA);
		memcpy(out + offset[i], data[i], length[i]);
	}
	manifest_digest(out, out + FW_BUNDLE_OFF_MANIFEST_SHA);
	return total;
}

const char *fw_bundle_status_name(enum fw_bundle_status status)
{
	switch (status) {
	case FW_BUNDLE_OK:
		return "ok";
	case FW_BUNDLE_FORMAT:
		return "malformed bundle";
	case FW_BUNDLE_HASH:
		return "digest mismatch";
	case FW_BUNDLE_IMAGE:
		return "damaged image";
	case FW_BUNDLE_TOO_LARGE:
		return "component too large";
	case FW_BUNDLE_INCOMPATIBLE:
		return "incompatible bundle";
	}
	return "unknown";
}
