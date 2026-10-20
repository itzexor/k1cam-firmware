/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * fw_layout.h -- the K1 camera's flash layouts and identifiers.
 *
 * The updater takes every erase and write address from this table and
 * never from the host. At run time the kernel's partitions (from the
 * mtdparts that U-Boot passes) must match it name for name and size for
 * size, in this order; board/k1cam/uenv.txt is the same layout for U-Boot,
 * and the image builder refuses a uenv.txt that disagrees with it.
 */
#ifndef FW_LAYOUT_H
#define FW_LAYOUT_H

#include <stdint.h>

#define FW_BOARD_ID "k1cam-t31l-gc2083"
#define FW_LAYOUT_AB_ID "k1cam-ab-8m-v1"
/* The original single-slot layout: recognized for INFO and dumps, never
 * updated in place. */
#define FW_LAYOUT_SINGLE_ID "k1cam-single-8m"

#define FW_FLASH_SIZE 0x800000u
/* The whole-flash alias partition every layout ends with. */
#define FW_PART_ALL "all"

/* What the U-Boot environment this layout is installed with can boot:
 * k1cam.slot and k1cam.boot on the kernel command line, the trial and
 * fallback states below. A bundle names the lowest level it needs. */
#define FW_BOOT_COMPAT 1

struct fw_part {
	const char *name;
	uint32_t offset;
	uint32_t size;
};

static const struct fw_part fw_layout_ab[] __attribute__((unused)) = {
	{"boot", 0x000000, 0x050000},     /* U-Boot: 320 KiB */
	{"env_a", 0x050000, 0x010000},    /* redundant U-Boot environment */
	{"env_b", 0x060000, 0x010000},
	{"kernel_a", 0x070000, 0x140000}, /* 1280 KiB */
	{"rootfs_a", 0x1b0000, 0x1c0000}, /* 1792 KiB */
	{"kernel_b", 0x370000, 0x140000},
	{"rootfs_b", 0x4b0000, 0x1c0000},
	{"data", 0x670000, 0x190000},     /* 1600 KiB, shared jffs2 */
};
#define FW_LAYOUT_AB_COUNT (sizeof(fw_layout_ab) / sizeof(fw_layout_ab[0]))

static const struct fw_part fw_layout_single[] __attribute__((unused)) = {
	{"boot", 0x000000, 0x050000},
	{"env", 0x050000, 0x010000},
	{"backup", 0x060000, 0x010000},
	{"kernel", 0x070000, 0x190000},
	{"rootfs", 0x200000, 0x280000},
	{"data", 0x480000, 0x380000},
};
#define FW_LAYOUT_SINGLE_COUNT (sizeof(fw_layout_single) / sizeof(fw_layout_single[0]))

#define FW_KERNEL_CAPACITY 0x140000u
#define FW_ROOTFS_CAPACITY 0x1c0000u
#define FW_ENV_SIZE 0x10000u

/* Slots as the protocol and the environment name them. */
#define FW_SLOT_A 0
#define FW_SLOT_B 1
#define FW_SLOT_NONE 0xff

static inline char fw_slot_letter(int slot)
{
	return slot == FW_SLOT_A ? 'a' : slot == FW_SLOT_B ? 'b' : '-';
}

#endif /* FW_LAYOUT_H */
