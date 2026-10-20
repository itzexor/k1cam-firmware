/*
 * fake_flash.h -- an in-memory 8 MiB SPI NOR behind struct fw_flash_ops.
 *
 * It follows NOR physics and the K1's measured geometry, nothing invented
 * for convenience: erase sets whole 32 KiB blocks (the jz-sfc driver's MTD
 * erasesize on the camera) to 0xff and rejects unaligned ranges like
 * MEMERASE; programming can only clear bits (new = old & data); reads past a
 * partition's end return 0 (EOF), as mtdchar does.
 *
 * Faults: fail the Nth erase/write/read, short transfers, EINTR, a bit
 * flipped on read at one address, and a power cut after N mutating
 * operations -- the operation that is cut applies half of itself and
 * everything after it is lost.
 */
#ifndef FAKE_FLASH_H
#define FAKE_FLASH_H

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "../src/fw_flash.h"

#define FAKE_ERASESIZE 0x8000u
#define FAKE_HANDLES 32

struct fake_flash {
	struct fw_flash flash;
	uint8_t *mem;
	const struct fw_part *table;
	size_t entries;
	bool unknown_layout; /* list a layout neither table matches */
	struct {
		bool used, writable;
		int part; /* -1: the "all" alias */
	} handle[FAKE_HANDLES];

	/* Faults. -1: never. */
	int fail_erase_at, fail_write_at, fail_read_at;
	int erases, writes, reads;
	int short_every;      /* every Nth transfer moves half (0: never) */
	int eintr_every;      /* every Nth transfer fails with EINTR (0: never) */
	int64_t flip_at;      /* absolute address read back with bit 0 flipped */
	int cut_after;        /* mutating ops before power is lost; -1: never */
	int mutations;
	bool dead;            /* power is gone */
	int transfers;
	bool wrong_geometry;  /* MEMGETINFO reports another erase size */
	/* Something else writing the flash: every read of churn_at changes
	 * the byte there afterwards (-1: nobody). */
	int64_t churn_at;
	/* Called before every erase, with how many came before. */
	void (*on_erase)(struct fake_flash *ff, int count);
	void *test;
	int erase_calls;
};

static uint32_t fake_part_offset(struct fake_flash *ff, int part)
{
	return part < 0 ? 0 : ff->table[part].offset;
}

static uint32_t fake_part_size(struct fake_flash *ff, int part)
{
	return part < 0 ? FW_FLASH_SIZE : ff->table[part].size;
}

static int fake_list(struct fw_flash *f, struct fw_mtd_part *parts, int max)
{
	struct fake_flash *ff = f->ctx;
	int n = 0;
	for (size_t i = 0; i < ff->entries && n < max; i++, n++) {
		parts[n].index = n;
		snprintf(parts[n].name, sizeof(parts[n].name), "%s",
			 ff->unknown_layout && i == 1 ? "config" : ff->table[i].name);
		parts[n].size = ff->table[i].size;
		parts[n].erasesize = FAKE_ERASESIZE;
	}
	if (n < max) {
		parts[n].index = n;
		snprintf(parts[n].name, sizeof(parts[n].name), "%s", FW_PART_ALL);
		parts[n].size = FW_FLASH_SIZE;
		parts[n].erasesize = FAKE_ERASESIZE;
		n++;
	}
	return n;
}

static int fake_open(struct fw_flash *f, int index, bool writable)
{
	struct fake_flash *ff = f->ctx;
	if (index < 0 || (size_t)index > ff->entries)
		return -ENOENT;
	for (int h = 0; h < FAKE_HANDLES; h++) {
		if (!ff->handle[h].used) {
			ff->handle[h].used = true;
			ff->handle[h].writable = writable;
			ff->handle[h].part = (size_t)index == ff->entries ? -1 : index;
			return h;
		}
	}
	return -EMFILE;
}

static int fake_geometry(struct fw_flash *f, int h, struct fw_mtd_geometry *g)
{
	struct fake_flash *ff = f->ctx;
	g->type = FW_MTD_NORFLASH;
	g->size = fake_part_size(ff, ff->handle[h].part);
	g->erasesize = ff->wrong_geometry ? 0x30000 : FAKE_ERASESIZE;
	g->writesize = 1;
	return 0;
}

/* Common transfer faults. Returns how many bytes to move, or -1. */
static ssize_t fake_transfer(struct fake_flash *ff, size_t len)
{
	ff->transfers++;
	if (ff->eintr_every && ff->transfers % ff->eintr_every == 0) {
		errno = EINTR;
		return -1;
	}
	if (ff->short_every && ff->transfers % ff->short_every == 0 && len > 1)
		return (ssize_t)(len / 2);
	return (ssize_t)len;
}

/* A mutating operation: false when power is gone (and errno set). With
 * *partial set, the operation is the one being cut and applies half. */
static bool fake_mutate(struct fake_flash *ff, bool *partial)
{
	*partial = false;
	if (ff->dead) {
		errno = EIO;
		return false;
	}
	if (ff->cut_after >= 0 && ff->mutations++ >= ff->cut_after) {
		ff->dead = true;
		*partial = true;
	}
	return true;
}

static ssize_t fake_pread(struct fw_flash *f, int h, void *buf, size_t len, uint32_t off)
{
	struct fake_flash *ff = f->ctx;
	if (ff->dead) {
		errno = EIO;
		return -1;
	}
	if (ff->fail_read_at >= 0 && ff->reads++ == ff->fail_read_at) {
		errno = EIO;
		return -1;
	}
	uint32_t size = fake_part_size(ff, ff->handle[h].part);
	if (off >= size)
		return 0;
	if (len > size - off)
		len = size - off;
	ssize_t n = fake_transfer(ff, len);
	if (n < 0)
		return n;
	uint32_t base = fake_part_offset(ff, ff->handle[h].part) + off;
	memcpy(buf, ff->mem + base, (size_t)n);
	if (ff->flip_at >= base && ff->flip_at < base + n)
		((uint8_t *)buf)[ff->flip_at - base] ^= 1;
	if (ff->churn_at >= base && ff->churn_at < base + n)
		ff->mem[ff->churn_at]--;
	return n;
}

static ssize_t fake_pwrite(struct fw_flash *f, int h, const void *buf, size_t len, uint32_t off)
{
	struct fake_flash *ff = f->ctx;
	bool partial;
	if (!ff->handle[h].writable) {
		errno = EBADF;
		return -1;
	}
	uint32_t size = fake_part_size(ff, ff->handle[h].part);
	if (off >= size || len > size - off) {
		errno = ENOSPC;
		return -1;
	}
	if (ff->fail_write_at >= 0 && ff->writes++ == ff->fail_write_at) {
		errno = EIO;
		return -1;
	}
	ssize_t n = fake_transfer(ff, len);
	if (n < 0)
		return n;
	if (!fake_mutate(ff, &partial))
		return -1;
	if (partial)
		n /= 2;
	uint8_t *dst = ff->mem + fake_part_offset(ff, ff->handle[h].part) + off;
	for (ssize_t i = 0; i < n; i++)
		dst[i] &= ((const uint8_t *)buf)[i]; /* NOR: bits only go to 0 */
	if (partial) {
		errno = EIO;
		return -1;
	}
	return n;
}

static int fake_erase(struct fw_flash *f, int h, uint32_t off, uint32_t len)
{
	struct fake_flash *ff = f->ctx;
	bool partial;
	if (!ff->handle[h].writable)
		return -EBADF;
	uint32_t size = fake_part_size(ff, ff->handle[h].part);
	if (off % FAKE_ERASESIZE || len % FAKE_ERASESIZE || !len || off >= size ||
	    len > size - off)
		return -EINVAL;
	if (ff->on_erase)
		ff->on_erase(ff, ff->erase_calls);
	ff->erase_calls++;
	if (ff->fail_erase_at >= 0 && ff->erases++ == ff->fail_erase_at)
		return -EIO;
	if (!fake_mutate(ff, &partial))
		return -EIO;
	memset(ff->mem + fake_part_offset(ff, ff->handle[h].part) + off, 0xff,
	       partial ? len / 2 : len);
	return partial ? -EIO : 0;
}

static void fake_close(struct fw_flash *f, int h)
{
	struct fake_flash *ff = f->ctx;
	ff->handle[h].used = false;
}

static const struct fw_flash_ops fake_ops = {
	.list = fake_list,
	.open = fake_open,
	.geometry = fake_geometry,
	.pread = fake_pread,
	.pwrite = fake_pwrite,
	.erase = fake_erase,
	.close = fake_close,
};

/* A blank (all 0xff) flash with the A/B layout, no faults. */
static __attribute__((unused)) void fake_flash_init(struct fake_flash *ff, bool ab)
{
	uint8_t *mem = ff->mem ? ff->mem : malloc(FW_FLASH_SIZE);
	memset(ff, 0, sizeof(*ff));
	ff->mem = mem;
	memset(ff->mem, 0xff, FW_FLASH_SIZE);
	ff->table = ab ? fw_layout_ab : fw_layout_single;
	ff->entries = ab ? FW_LAYOUT_AB_COUNT : FW_LAYOUT_SINGLE_COUNT;
	ff->fail_erase_at = ff->fail_write_at = ff->fail_read_at = -1;
	ff->flip_at = -1;
	ff->churn_at = -1;
	ff->cut_after = -1;
	ff->flash.ops = &fake_ops;
	ff->flash.ctx = ff;
}

static __attribute__((unused)) void fake_flash_faults_off(struct fake_flash *ff)
{
	ff->fail_erase_at = ff->fail_write_at = ff->fail_read_at = -1;
	ff->erases = ff->writes = ff->reads = 0;
	ff->short_every = ff->eintr_every = 0;
	ff->flip_at = -1;
	ff->churn_at = -1;
	ff->cut_after = -1;
	ff->mutations = 0;
	ff->on_erase = NULL;
	ff->erase_calls = 0;
	ff->dead = false;
	ff->wrong_geometry = false;
}

static __attribute__((unused)) int fake_open_handles(struct fake_flash *ff)
{
	int n = 0;
	for (int h = 0; h < FAKE_HANDLES; h++)
		n += ff->handle[h].used;
	return n;
}

#endif /* FAKE_FLASH_H */
