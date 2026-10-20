/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * k1cam-mkupd -- build and check k1cam.upd, the kernel + rootfs update
 * bundle (format in src/fw_bundle.h). Runs on the build host.
 *
 *   k1cam-mkupd -o OUT -k uImage -r rootfs.squashfs -B BUILD_ID
 *               [-b BOARD] [-l LAYOUT] [-c MIN_BOOT_COMPAT]
 *   k1cam-mkupd --check FILE    validate a bundle and print its manifest
 *   k1cam-mkupd --layout        print the A/B layout the camera enforces
 *
 * The bundle is assembled by the same code the camera and uvcdctl parse it
 * with, and parsed back before it is written.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fw_bundle.h"
#include "fw_layout.h"
#include "fw_sha256.h"

static uint8_t *slurp(const char *path, uint32_t *len)
{
	FILE *f = fopen(path, "rb");
	struct stat st;
	if (!f || fstat(fileno(f), &st) != 0 || st.st_size <= 0 || st.st_size > 0x7fffffff) {
		fprintf(stderr, "k1cam-mkupd: %s: %s\n", path, f ? "bad size" : strerror(errno));
		if (f)
			fclose(f);
		return NULL;
	}
	uint8_t *buf = malloc((size_t)st.st_size);
	if (!buf || fread(buf, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
		fprintf(stderr, "k1cam-mkupd: %s: read failed\n", path);
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*len = (uint32_t)st.st_size;
	return buf;
}

static int check(const char *path)
{
	uint32_t len;
	uint8_t *buf = slurp(path, &len);
	struct fw_bundle b;
	char why[160];
	char hex[2 * FW_SHA256_SIZE + 1];

	if (!buf)
		return 1;
	enum fw_bundle_status st = fw_bundle_parse(buf, len, &b, why, sizeof(why));
	if (st == FW_BUNDLE_OK)
		st = fw_bundle_compatible(&b, FW_BOOT_COMPAT, why, sizeof(why));
	if (st != FW_BUNDLE_OK) {
		fprintf(stderr, "k1cam-mkupd: %s: %s: %s\n", path, fw_bundle_status_name(st), why);
		free(buf);
		return 1;
	}
	printf("format=%d\nboard=%s\nlayout=%s\nbuild=%s\nmin_boot_compat=%u\nlength=%u\n",
	       FW_BUNDLE_VERSION, b.board, b.layout, b.build, b.min_boot_compat, b.length);
	for (int i = 0; i < FW_BUNDLE_COMPONENTS; i++) {
		fw_sha256_hex(b.component[i].sha256, hex);
		printf("%s=%u+%u sha256:%s\n", i ? "rootfs" : "kernel", b.component[i].offset,
		       b.component[i].length, hex);
	}
	free(buf);
	return 0;
}

static void layout(void)
{
	for (size_t i = 0; i < FW_LAYOUT_AB_COUNT; i++)
		printf("%s 0x%06x 0x%06x\n", fw_layout_ab[i].name, fw_layout_ab[i].offset,
		       fw_layout_ab[i].size);
	printf("%s 0x000000 0x%06x\n", FW_PART_ALL, FW_FLASH_SIZE);
	printf("id %s\nboard %s\nboot_compat %d\n", FW_LAYOUT_AB_ID, FW_BOARD_ID, FW_BOOT_COMPAT);
}

static int usage(void)
{
	fprintf(stderr, "usage: k1cam-mkupd -o OUT -k UIMAGE -r ROOTFS -B BUILD_ID [-b BOARD]"
			" [-l LAYOUT] [-c MIN_BOOT_COMPAT]\n"
			"       k1cam-mkupd --check FILE\n"
			"       k1cam-mkupd --layout\n");
	return 2;
}

int main(int argc, char **argv)
{
	const char *out = NULL, *kernel = NULL, *rootfs = NULL, *build = NULL;
	const char *board = FW_BOARD_ID, *layout_id = FW_LAYOUT_AB_ID;
	unsigned long compat = FW_BOOT_COMPAT;

	if (argc == 3 && !strcmp(argv[1], "--check"))
		return check(argv[2]);
	if (argc == 2 && !strcmp(argv[1], "--layout")) {
		layout();
		return 0;
	}
	for (int i = 1; i < argc; i++) {
		const char *opt = argv[i], *val = i + 1 < argc ? argv[i + 1] : NULL;
		if (!val || opt[0] != '-' || opt[2])
			return usage();
		i++;
		switch (opt[1]) {
		case 'o': out = val; break;
		case 'k': kernel = val; break;
		case 'r': rootfs = val; break;
		case 'B': build = val; break;
		case 'b': board = val; break;
		case 'l': layout_id = val; break;
		case 'c': {
			char *end;
			compat = strtoul(val, &end, 10);
			if (!*val || *end || compat > 0xffffffffUL)
				return usage();
			break;
		}
		default:
			return usage();
		}
	}
	if (!out || !kernel || !rootfs || !build)
		return usage();

	uint32_t klen, rlen;
	uint8_t *k = slurp(kernel, &klen), *r = slurp(rootfs, &rlen);
	if (!k || !r)
		return 1;
	size_t total = fw_bundle_build(NULL, board, layout_id, build, (uint32_t)compat, k, klen, r,
				       rlen);
	uint8_t *bundle = total ? malloc(total) : NULL;
	if (!bundle) {
		fprintf(stderr, "k1cam-mkupd: %s\n", total ? "out of memory" : "identifier too long");
		return 1;
	}
	fw_bundle_build(bundle, board, layout_id, build, (uint32_t)compat, k, klen, r, rlen);

	/* What the camera will check, checked here first. */
	struct fw_bundle b;
	char why[160];
	enum fw_bundle_status st = fw_bundle_parse(bundle, total, &b, why, sizeof(why));
	if (st == FW_BUNDLE_OK)
		st = fw_bundle_compatible(&b, FW_BOOT_COMPAT, why, sizeof(why));
	if (st != FW_BUNDLE_OK) {
		fprintf(stderr, "k1cam-mkupd: refusing to write %s: %s: %s\n", out,
			fw_bundle_status_name(st), why);
		return 1;
	}
	FILE *f = fopen(out, "wb");
	if (!f || fwrite(bundle, 1, total, f) != total || fclose(f) != 0) {
		fprintf(stderr, "k1cam-mkupd: %s: %s\n", out, strerror(errno));
		return 1;
	}
	free(bundle);
	free(k);
	free(r);
	return 0;
}
