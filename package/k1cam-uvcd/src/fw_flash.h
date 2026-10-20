/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * fw_flash.h -- MTD access for the firmware updater and dumps.
 *
 * Everything goes through a small ops table so that host tests can run the
 * real updater against an in-memory NOR model (with injected failures)
 * instead of flash. fw_flash_mtd() is the real one: /proc/mtd for the
 * partition list, /dev/mtdN with MEMGETINFO/MEMERASE for I/O.
 */
#ifndef FW_FLASH_H
#define FW_FLASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "fw_layout.h"

#define FW_MTD_NAME_MAX 32
#define FW_MTD_MAX_PARTS 16
/* MTD type for NOR flash, as MEMGETINFO reports it (mtd-abi.h MTD_NORFLASH). */
#define FW_MTD_NORFLASH 3

struct fw_mtd_part {
	int index; /* N in /dev/mtdN */
	char name[FW_MTD_NAME_MAX];
	uint32_t size;
	uint32_t erasesize;
};

struct fw_mtd_geometry {
	uint8_t type;
	uint32_t size;
	uint32_t erasesize;
	uint32_t writesize;
};

struct fw_flash;
struct fw_flash_ops {
	/* Partitions in kernel order, the whole-flash alias included.
	 * Returns how many, or -errno. */
	int (*list)(struct fw_flash *f, struct fw_mtd_part *parts, int max);
	/* A handle >= 0, or -errno. */
	int (*open)(struct fw_flash *f, int index, bool writable);
	int (*geometry)(struct fw_flash *f, int h, struct fw_mtd_geometry *g);
	/* As pread/pwrite: may transfer less than asked, or fail with -1 and
	 * errno (EINTR included). */
	ssize_t (*pread)(struct fw_flash *f, int h, void *buf, size_t len, uint32_t off);
	ssize_t (*pwrite)(struct fw_flash *f, int h, const void *buf, size_t len, uint32_t off);
	/* 0 or -errno. off and len are erase-block aligned. */
	int (*erase)(struct fw_flash *f, int h, uint32_t off, uint32_t len);
	void (*close)(struct fw_flash *f, int h);
};

struct fw_flash {
	const struct fw_flash_ops *ops;
	void *ctx;
};

/* The real MTD devices. */
void fw_flash_mtd(struct fw_flash *f);

/* Checked loops over the ops: retry EINTR, continue short transfers, and
 * fail (errno EIO) on a transfer that makes no progress. 0 or -1 with
 * errno set. */
int fw_flash_read_all(struct fw_flash *f, int h, void *buf, size_t len, uint32_t off);
int fw_flash_write_all(struct fw_flash *f, int h, const void *buf, size_t len, uint32_t off);
/* Erase [off, off + len), both multiples of erasesize. */
int fw_flash_erase_range(struct fw_flash *f, int h, uint32_t erasesize, uint32_t off,
			 uint32_t len);

enum fw_layout_kind {
	FW_LAYOUT_UNKNOWN = 0,
	FW_LAYOUT_SINGLE,
	FW_LAYOUT_AB,
};

/* The kernel's partitions matched against fw_layout.h. */
struct fw_layout_map {
	enum fw_layout_kind kind;
	int count;                                 /* entries in parts */
	struct fw_mtd_part parts[FW_MTD_MAX_PARTS]; /* in kernel order */
	int all;                                   /* index into parts of FW_PART_ALL, or -1 */
	uint32_t erasesize;                        /* of the whole flash */
};

/* List the partitions and recognize the layout. A list that matches
 * neither layout exactly, name for name, size for size and in order,
 * followed by an 8 MiB "all", is FW_LAYOUT_UNKNOWN. Returns 0, or -errno
 * if the list could not be read. */
int fw_layout_resolve(struct fw_flash *f, struct fw_layout_map *map);
const char *fw_layout_id(enum fw_layout_kind kind);

/* The partition called name in map, or NULL. */
const struct fw_mtd_part *fw_layout_find(const struct fw_layout_map *map, const char *name);

/* Open a partition of map by name and check its geometry against the
 * compiled table: NOR, the expected size, and an erase size that divides
 * it. Returns a handle, or -errno (EINVAL for a geometry mismatch). */
int fw_layout_open(struct fw_flash *f, const struct fw_layout_map *map, const char *name,
		   bool writable, struct fw_mtd_geometry *geometry);

#endif /* FW_FLASH_H */
