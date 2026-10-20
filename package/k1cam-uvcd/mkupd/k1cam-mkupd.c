/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * k1cam-mkupd -- build and check k1cam.upd, the kernel + rootfs update
 * bundle (format in src/fw_bundle.h). Runs on the build host.
 *
 *   k1cam-mkupd -o OUT -k uImage -r rootfs.squashfs -B BUILD_ID
 *               [-b BOARD] [-l LAYOUT] [-c MIN_BOOT_COMPAT] [-i OS_RELEASE]
 *   k1cam-mkupd --check FILE    validate a bundle and print its manifest
 *                               (and its release information, info: lines)
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
	const char *text;
	size_t blen, tlen;

	if (!buf)
		return 1;
	enum fw_bundle_status st = fw_bundle_split(buf, len, &blen, &text, &tlen, why, sizeof(why));
	if (st == FW_BUNDLE_OK)
		st = fw_bundle_parse(buf, blen, &b, why, sizeof(why));
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
	/* The text ends the file: no NUL after it. */
	for (size_t at = 0; at < tlen;) {
		const char *nl = memchr(text + at, '\n', tlen - at);
		size_t n = nl ? (size_t)(nl - (text + at)) : tlen - at;
		printf("info:%.*s\n", (int)n, text + at);
		at += n + 1;
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
			" [-l LAYOUT] [-c MIN_BOOT_COMPAT] [-i OS_RELEASE]\n"
			"       k1cam-mkupd --check FILE\n"
			"       k1cam-mkupd --layout\n");
	return 2;
}

int main(int argc, char **argv)
{
	const char *out = NULL, *kernel = NULL, *rootfs = NULL, *build = NULL, *info = NULL;
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
		case 'i': info = val; break;
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

	uint32_t klen, rlen, ilen = 0;
	uint8_t *k = slurp(kernel, &klen), *r = slurp(rootfs, &rlen);
	uint8_t *text = info ? slurp(info, &ilen) : NULL;
	if (!k || !r || (info && !text))
		return 1;
	size_t len = fw_bundle_build(NULL, board, layout_id, build, (uint32_t)compat, k, klen, r,
				     rlen);
	size_t extra = text ? fw_bundle_info_build(NULL, NULL, (const char *)text, ilen) : 0;
	if (!len) {
		fprintf(stderr, "k1cam-mkupd: identifier too long\n");
		return 1;
	}
	if (text && !extra) {
		fprintf(stderr, "k1cam-mkupd: %s: not 1 to %d bytes of printable ASCII lines\n", info,
			FW_INFO_TEXT_MAX);
		return 1;
	}
	size_t total = len + extra;
	uint8_t *bundle = malloc(total);
	if (!bundle) {
		fprintf(stderr, "k1cam-mkupd: out of memory\n");
		return 1;
	}
	fw_bundle_build(bundle, board, layout_id, build, (uint32_t)compat, k, klen, r, rlen);
	if (text)
		fw_bundle_info_build(bundle + len, bundle, (const char *)text, ilen);

	/* What the camera will check, checked here first, and the release
	 * information read back the way hosts read it. */
	struct fw_bundle b;
	char why[160];
	const char *got;
	size_t blen, glen;
	enum fw_bundle_status st = fw_bundle_split(bundle, total, &blen, &got, &glen, why,
						   sizeof(why));
	if (st == FW_BUNDLE_OK && (blen != len || glen != ilen || (text && memcmp(got, text, ilen)))) {
		snprintf(why, sizeof(why), "the release information does not read back");
		st = FW_BUNDLE_FORMAT;
	}
	if (st == FW_BUNDLE_OK)
		st = fw_bundle_parse(bundle, blen, &b, why, sizeof(why));
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
	free(text);
	return 0;
}
