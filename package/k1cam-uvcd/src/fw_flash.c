/* SPDX-License-Identifier: GPL-3.0-only */
#include "fw_flash.h"

#include <errno.h>
#include <fcntl.h>
#include <mtd/mtd-user.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* --------------------------------------------------------------------------
 * Real MTD devices
 */
static int mtd_list(struct fw_flash *f, struct fw_mtd_part *parts, int max)
{
	(void)f;
	FILE *fp = fopen("/proc/mtd", "r");
	if (!fp)
		return -errno;
	char line[128];
	int n = 0;
	while (fgets(line, sizeof(line), fp)) {
		struct fw_mtd_part p = {0};
		unsigned size, erasesize;
		/* mtd3: 00140000 00008000 "kernel_a" */
		if (sscanf(line, "mtd%d: %x %x \"%31[^\"]\"", &p.index, &size, &erasesize,
			   p.name) != 4)
			continue;
		if (n == max) {
			fclose(fp);
			return -E2BIG;
		}
		p.size = size;
		p.erasesize = erasesize;
		parts[n++] = p;
	}
	fclose(fp);
	return n;
}

static int mtd_open(struct fw_flash *f, int index, bool writable)
{
	(void)f;
	char path[32];
	snprintf(path, sizeof(path), "/dev/mtd%d", index);
	int fd = open(path, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
	return fd < 0 ? -errno : fd;
}

static int mtd_geometry(struct fw_flash *f, int h, struct fw_mtd_geometry *g)
{
	(void)f;
	struct mtd_info_user info;
	if (ioctl(h, MEMGETINFO, &info) != 0)
		return -errno;
	g->type = info.type;
	g->size = info.size;
	g->erasesize = info.erasesize;
	g->writesize = info.writesize;
	return 0;
}

static ssize_t mtd_pread(struct fw_flash *f, int h, void *buf, size_t len, uint32_t off)
{
	(void)f;
	return pread(h, buf, len, off);
}

static ssize_t mtd_pwrite(struct fw_flash *f, int h, const void *buf, size_t len, uint32_t off)
{
	(void)f;
	return pwrite(h, buf, len, off);
}

static int mtd_erase(struct fw_flash *f, int h, uint32_t off, uint32_t len)
{
	(void)f;
	struct erase_info_user e = {.start = off, .length = len};
	return ioctl(h, MEMERASE, &e) == 0 ? 0 : -errno;
}

static void mtd_close(struct fw_flash *f, int h)
{
	(void)f;
	close(h);
}

static const struct fw_flash_ops mtd_ops = {
	.list = mtd_list,
	.open = mtd_open,
	.geometry = mtd_geometry,
	.pread = mtd_pread,
	.pwrite = mtd_pwrite,
	.erase = mtd_erase,
	.close = mtd_close,
};

void fw_flash_mtd(struct fw_flash *f)
{
	f->ops = &mtd_ops;
	f->ctx = NULL;
}

/* --------------------------------------------------------------------------
 * Checked I/O
 */
int fw_flash_read_all(struct fw_flash *f, int h, void *buf, size_t len, uint32_t off)
{
	uint8_t *p = buf;
	while (len) {
		ssize_t n = f->ops->pread(f, h, p, len, off);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			return -1;
		if (n == 0) {
			errno = EIO;
			return -1;
		}
		p += n;
		off += (uint32_t)n;
		len -= (size_t)n;
	}
	return 0;
}

int fw_flash_write_all(struct fw_flash *f, int h, const void *buf, size_t len, uint32_t off)
{
	const uint8_t *p = buf;
	while (len) {
		ssize_t n = f->ops->pwrite(f, h, p, len, off);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			return -1;
		if (n == 0) {
			errno = EIO;
			return -1;
		}
		p += n;
		off += (uint32_t)n;
		len -= (size_t)n;
	}
	return 0;
}

int fw_flash_erase_range(struct fw_flash *f, int h, uint32_t erasesize, uint32_t off,
			 uint32_t len)
{
	if (!erasesize || off % erasesize || len % erasesize) {
		errno = EINVAL;
		return -1;
	}
	for (uint32_t at = off; at < off + len; at += erasesize) {
		int ret = f->ops->erase(f, h, at, erasesize);
		if (ret != 0) {
			errno = -ret;
			return -1;
		}
	}
	return 0;
}

/* --------------------------------------------------------------------------
 * Layout
 */
static bool matches(const struct fw_mtd_part *parts, int count, const struct fw_part *table,
		    size_t entries)
{
	if ((size_t)count != entries + 1)
		return false;
	for (size_t i = 0; i < entries; i++) {
		if (strcmp(parts[i].name, table[i].name) != 0 || parts[i].size != table[i].size)
			return false;
	}
	/* The table is contiguous from 0: the kernel's cmdline partitions
	 * are laid end to end in the same order, so the offsets agree too. */
	return !strcmp(parts[entries].name, FW_PART_ALL) && parts[entries].size == FW_FLASH_SIZE;
}

int fw_layout_resolve(struct fw_flash *f, struct fw_layout_map *map)
{
	memset(map, 0, sizeof(*map));
	map->all = -1;
	int n = f->ops->list(f, map->parts, FW_MTD_MAX_PARTS);
	if (n < 0)
		return n;
	map->count = n;
	for (int i = 0; i < n; i++) {
		if (!strcmp(map->parts[i].name, FW_PART_ALL) && map->parts[i].size == FW_FLASH_SIZE) {
			map->all = i;
			map->erasesize = map->parts[i].erasesize;
		}
	}
	if (matches(map->parts, n, fw_layout_ab, FW_LAYOUT_AB_COUNT))
		map->kind = FW_LAYOUT_AB;
	else if (matches(map->parts, n, fw_layout_single, FW_LAYOUT_SINGLE_COUNT))
		map->kind = FW_LAYOUT_SINGLE;
	else
		map->kind = FW_LAYOUT_UNKNOWN;
	return 0;
}

const char *fw_layout_id(enum fw_layout_kind kind)
{
	switch (kind) {
	case FW_LAYOUT_AB:
		return FW_LAYOUT_AB_ID;
	case FW_LAYOUT_SINGLE:
		return FW_LAYOUT_SINGLE_ID;
	default:
		return "unknown";
	}
}

const struct fw_mtd_part *fw_layout_find(const struct fw_layout_map *map, const char *name)
{
	for (int i = 0; i < map->count; i++) {
		if (!strcmp(map->parts[i].name, name))
			return &map->parts[i];
	}
	return NULL;
}

static const struct fw_part *table_entry(enum fw_layout_kind kind, const char *name)
{
	const struct fw_part *table = kind == FW_LAYOUT_AB ? fw_layout_ab : fw_layout_single;
	size_t entries = kind == FW_LAYOUT_AB ? FW_LAYOUT_AB_COUNT : FW_LAYOUT_SINGLE_COUNT;
	static const struct fw_part all = {FW_PART_ALL, 0, FW_FLASH_SIZE};

	if (!strcmp(name, FW_PART_ALL))
		return &all;
	if (kind != FW_LAYOUT_AB && kind != FW_LAYOUT_SINGLE)
		return NULL;
	for (size_t i = 0; i < entries; i++) {
		if (!strcmp(table[i].name, name))
			return &table[i];
	}
	return NULL;
}

int fw_layout_open(struct fw_flash *f, const struct fw_layout_map *map, const char *name,
		   bool writable, struct fw_mtd_geometry *geometry)
{
	const struct fw_part *want = table_entry(map->kind, name);
	const struct fw_mtd_part *part = fw_layout_find(map, name);
	if (!want || !part)
		return -ENOENT;

	int h = f->ops->open(f, part->index, writable);
	if (h < 0)
		return h;
	struct fw_mtd_geometry g;
	int ret = f->ops->geometry(f, h, &g);
	if (ret == 0 && (g.type != FW_MTD_NORFLASH || g.size != want->size || !g.erasesize ||
			 g.size % g.erasesize || want->offset % g.erasesize || !g.writesize ||
			 g.writesize > g.erasesize))
		ret = -EINVAL;
	if (ret != 0) {
		f->ops->close(f, h);
		return ret;
	}
	if (geometry)
		*geometry = g;
	return h;
}
