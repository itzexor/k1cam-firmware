/*
 * fw_test.c -- the firmware building blocks: SHA-256, CRCs, the bundle
 * format, the flash helpers and the redundant U-Boot environment.
 *
 * FW_TEST_FIXTURES (set by run.sh) names a directory with environment
 * images made by U-Boot's own mkenvimage, and the build's real uImage and
 * rootfs.squashfs when there is a build.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/fw_bundle.c"
#include "../src/fw_crc.c"
#include "../src/fw_env.c"
#include "../src/fw_flash.c"
#include "../src/fw_sha256.c"
#include "fake_flash.h"

static void hex_is(const uint8_t *digest, const char *want)
{
	char hex[2 * FW_SHA256_SIZE + 1];
	fw_sha256_hex(digest, hex);
	if (strcmp(hex, want)) {
		fprintf(stderr, "got %s\nwant %s\n", hex, want);
		assert(0);
	}
}

static void test_sha256(void)
{
	uint8_t d[FW_SHA256_SIZE];
	/* FIPS 180-2 appendix B vectors */
	fw_sha256("", 0, d);
	hex_is(d, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	fw_sha256("abc", 3, d);
	hex_is(d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	fw_sha256(two, strlen(two), d);
	hex_is(d, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	uint8_t *a = malloc(1000000);
	memset(a, 'a', 1000000);
	fw_sha256(a, 1000000, d);
	hex_is(d, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
	/* Any split into updates gives the same digest. */
	for (size_t step = 1; step < 200; step += 13) {
		struct fw_sha256 s;
		uint8_t e[FW_SHA256_SIZE];
		fw_sha256_init(&s);
		for (size_t at = 0; at < 1000000; at += step)
			fw_sha256_update(&s, a + at, 1000000 - at < step ? 1000000 - at : step);
		fw_sha256_final(&s, e);
		assert(!memcmp(d, e, sizeof(d)));
	}
	free(a);
}

static void test_crc(void)
{
	/* Check values of the catalogued CRCs. */
	assert(fw_crc32(0, "123456789", 9) == 0xcbf43926);
	assert(fw_crc16(FW_CRC16_INIT, "123456789", 9) == 0x29b1);
	assert(fw_crc32(fw_crc32(0, "1234", 4), "56789", 5) == 0xcbf43926);
}

/* --------------------------------------------------------------------------
 * Bundle
 */
static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* A minimal valid uImage: Linux, MIPS, kernel, with correct CRCs. */
static uint8_t *make_uimage(uint32_t data_len, uint32_t *len)
{
	uint8_t *u = calloc(1, 64 + data_len);
	for (uint32_t i = 0; i < data_len; i++)
		u[64 + i] = (uint8_t)(i * 7 + 3);
	put_be32(u, 0x27051956);
	put_be32(u + 12, data_len);
	put_be32(u + 16, 0x80010000);
	put_be32(u + 20, 0x80010000);
	put_be32(u + 24, fw_crc32(0, u + 64, data_len));
	u[28] = 5; /* Linux */
	u[29] = 5; /* MIPS */
	u[30] = 2; /* kernel */
	u[31] = 3; /* LZMA */
	memcpy(u + 32, "Linux-3.10.14", 13);
	put_be32(u + 4, fw_crc32(0, u, 64));
	*len = 64 + data_len;
	return u;
}

/* A SquashFS 4.0 superblock, bytes_used of it, zero padded to 4 KiB. */
static uint8_t *make_squashfs(uint32_t used, uint32_t *len)
{
	uint32_t padded = (used + 4095) & ~4095u;
	uint8_t *s = calloc(1, padded);
	for (uint32_t i = 96; i < used; i++)
		s[i] = (uint8_t)(i * 13 + 1);
	memcpy(s, "hsqs", 4);
	s[12 + 2] = 0x04; /* block size 0x40000 */
	s[22] = 18;       /* block_log */
	s[28] = 4;        /* major */
	s[40] = (uint8_t)used;
	s[41] = (uint8_t)(used >> 8);
	s[42] = (uint8_t)(used >> 16);
	s[43] = (uint8_t)(used >> 24);
	*len = padded;
	return s;
}

static uint8_t *make_bundle(size_t *len, const char *build)
{
	uint32_t klen, rlen;
	uint8_t *k = make_uimage(5000, &klen), *r = make_squashfs(9000, &rlen);
	*len = fw_bundle_build(NULL, FW_BOARD_ID, FW_LAYOUT_AB_ID, build, 1, k, klen, r, rlen);
	uint8_t *b = malloc(*len);
	assert(fw_bundle_build(b, FW_BOARD_ID, FW_LAYOUT_AB_ID, build, 1, k, klen, r, rlen) == *len);
	free(k);
	free(r);
	return b;
}

static enum fw_bundle_status parse(const uint8_t *buf, size_t len)
{
	struct fw_bundle b;
	char why[160];
	enum fw_bundle_status st = fw_bundle_parse(buf, len, &b, why, sizeof(why));
	return st;
}

/* Recompute the manifest digest after editing the header, so a test can
 * reach the checks behind it. */
static void reseal(uint8_t *buf)
{
	manifest_digest(buf, buf + FW_BUNDLE_OFF_MANIFEST_SHA);
}

static void test_bundle(void)
{
	size_t len;
	uint8_t *b = make_bundle(&len, "k1cam-test-1");
	struct fw_bundle parsed;
	char why[160];

	assert(fw_bundle_parse(b, len, &parsed, why, sizeof(why)) == FW_BUNDLE_OK);
	assert(!strcmp(parsed.board, FW_BOARD_ID) && !strcmp(parsed.layout, FW_LAYOUT_AB_ID));
	assert(!strcmp(parsed.build, "k1cam-test-1") && parsed.min_boot_compat == 1);
	assert(parsed.component[0].type == FW_COMPONENT_KERNEL &&
	       parsed.component[0].offset == FW_BUNDLE_MANIFEST_SIZE &&
	       parsed.component[0].length == 5064);
	assert(parsed.component[1].offset % FW_BUNDLE_ALIGN == 0 &&
	       parsed.component[1].length == 12288);
	assert(fw_bundle_compatible(&parsed, 1, why, sizeof(why)) == FW_BUNDLE_OK);
	assert(fw_bundle_compatible(&parsed, 0, why, sizeof(why)) == FW_BUNDLE_INCOMPATIBLE);

	/* Golden vector: format v1 is frozen byte for byte. The same inputs
	 * through tests/bundle_check.py give the same digest. */
	uint8_t d[FW_SHA256_SIZE];
	fw_sha256(b, len, d);
	hex_is(d, "e4dfb5ff4554ce1e031222839b778618ca1ff9c7cdfbc9b507bdbaaac705c442");

	/* Deterministic. */
	size_t len2;
	uint8_t *b2 = make_bundle(&len2, "k1cam-test-1");
	assert(len2 == len && !memcmp(b, b2, len));
	free(b2);

	uint8_t *m = malloc(len);
#define MUTATE(edit, sealed, want)                                                            \
	do {                                                                                  \
		memcpy(m, b, len);                                                            \
		edit;                                                                         \
		if (sealed)                                                                   \
			reseal(m);                                                            \
		enum fw_bundle_status got = parse(m, len);                                    \
		if (got != (want)) {                                                          \
			fprintf(stderr, "%s: got %d want %d\n", #edit, got, want);            \
			assert(0);                                                            \
		}                                                                             \
	} while (0)
	MUTATE(m[0] ^= 1, false, FW_BUNDLE_FORMAT);
	MUTATE(m[FW_BUNDLE_OFF_VERSION] = 2, true, FW_BUNDLE_FORMAT);
	MUTATE(m[FW_BUNDLE_OFF_HEADER_SIZE] = 1, true, FW_BUNDLE_FORMAT); /* 257 */
	MUTATE(m[FW_BUNDLE_OFF_FLAGS] = 1, true, FW_BUNDLE_FORMAT);
	MUTATE(m[0x0a0] = 1, true, FW_BUNDLE_FORMAT);  /* reserved */
	MUTATE(m[0x0f0] = 1, true, FW_BUNDLE_FORMAT);  /* reserved after the digest */
	MUTATE(m[FW_BUNDLE_OFF_COUNT] = 3, true, FW_BUNDLE_FORMAT);
	MUTATE(m[FW_BUNDLE_OFF_RECORD_SIZE] = 32, true, FW_BUNDLE_FORMAT);
	MUTATE(m[FW_BUNDLE_OFF_LENGTH] ^= 1, true, FW_BUNDLE_FORMAT);
	MUTATE(m[FW_BUNDLE_OFF_BUILD] ^= 1, false, FW_BUNDLE_HASH); /* unsealed header edit */
	MUTATE(m[FW_BUNDLE_OFF_BOARD] = 0, true, FW_BUNDLE_FORMAT);  /* empty string */
	MUTATE(memset(m + FW_BUNDLE_OFF_LAYOUT, 'x', FW_BUNDLE_LAYOUT_LEN), true,
	       FW_BUNDLE_FORMAT); /* unterminated */
	MUTATE(m[FW_BUNDLE_OFF_BUILD + 20] = 'x', true, FW_BUNDLE_FORMAT); /* junk after NUL */
	MUTATE(m[FW_BUNDLE_OFF_BUILD + 1] = 0x07, true, FW_BUNDLE_FORMAT); /* control char */
	uint8_t *r0 = m + FW_BUNDLE_OFF_RECORDS, *r1 = r0 + FW_BUNDLE_RECORD_SIZE;
	MUTATE(r0[FW_BUNDLE_REC_TYPE] = 2, true, FW_BUNDLE_FORMAT);  /* order */
	MUTATE(r0[FW_BUNDLE_REC_FLAGS] = 1, true, FW_BUNDLE_FORMAT);
	MUTATE(r0[FW_BUNDLE_REC_INSTALLED] ^= 1, true, FW_BUNDLE_FORMAT);
	MUTATE(r0[0x3f] = 1, true, FW_BUNDLE_FORMAT); /* record reserved */
	MUTATE(r0[FW_BUNDLE_REC_OFFSET] ^= 0x40, true, FW_BUNDLE_FORMAT); /* into the manifest */
	MUTATE(r1[FW_BUNDLE_REC_OFFSET] ^= 0x01, true, FW_BUNDLE_FORMAT); /* misaligned */
	MUTATE(put_le32(r1 + FW_BUNDLE_REC_OFFSET, FW_BUNDLE_MANIFEST_SIZE + 64), true,
	       FW_BUNDLE_FORMAT); /* overlaps the kernel */
	MUTATE(put_le32(r0 + FW_BUNDLE_REC_LENGTH, 0), true, FW_BUNDLE_FORMAT);
	MUTATE(put_le32(r1 + FW_BUNDLE_REC_LENGTH, 0xffffffffu), true,
	       FW_BUNDLE_FORMAT); /* past the end, no wraparound */
	MUTATE(m[FW_BUNDLE_MANIFEST_SIZE + 5064 + 1] = 1, true, FW_BUNDLE_FORMAT); /* gap byte */
	MUTATE(m[FW_BUNDLE_MANIFEST_SIZE + 100] ^= 1, true, FW_BUNDLE_HASH);       /* kernel byte */
	MUTATE(m[len - 1] ^= 1, true, FW_BUNDLE_HASH);                            /* rootfs byte */
	MUTATE(r1[FW_BUNDLE_REC_SHA] ^= 1, true, FW_BUNDLE_HASH);
#undef MUTATE

	/* Truncated anywhere: never accepted, never read out of bounds. */
	for (size_t cut = 0; cut < len; cut += cut < 512 ? 1 : 97)
		assert(parse(b, cut) != FW_BUNDLE_OK);
	/* One trailing byte too many. */
	uint8_t *longer = calloc(1, len + 1);
	memcpy(longer, b, len);
	assert(parse(longer, len + 1) == FW_BUNDLE_FORMAT);
	free(longer);

	/* Random corruption is caught, whatever it hits. */
	srand(1);
	for (int i = 0; i < 3000; i++) {
		memcpy(m, b, len);
		int flips = 1 + rand() % 4;
		for (int j = 0; j < flips; j++)
			m[(size_t)rand() % len] ^= (uint8_t)(1 + rand() % 255);
		if (memcmp(m, b, len))
			assert(parse(m, len) != FW_BUNDLE_OK);
	}
	free(m);

	/* Images that don't check out, sealed into otherwise valid bundles. */
	uint32_t klen, rlen;
	uint8_t *k = make_uimage(2000, &klen), *r = make_squashfs(3000, &rlen);
	uint8_t *out = malloc(fw_bundle_build(NULL, "b", "l", "x", 1, k, klen, r, rlen) + 0x400000);
	k[64] ^= 1; /* data CRC */
	size_t n = fw_bundle_build(out, FW_BOARD_ID, FW_LAYOUT_AB_ID, "x", 1, k, klen, r, rlen);
	assert(parse(out, n) == FW_BUNDLE_IMAGE);
	k[64] ^= 1;
	k[29] = 2; /* ARM */
	put_be32(k + 4, 0);
	put_be32(k + 4, fw_crc32(0, k, 64));
	n = fw_bundle_build(out, FW_BOARD_ID, FW_LAYOUT_AB_ID, "x", 1, k, klen, r, rlen);
	assert(parse(out, n) == FW_BUNDLE_IMAGE);
	free(k);
	k = make_uimage(2000, &klen);
	r[0] = 'x'; /* not SquashFS */
	n = fw_bundle_build(out, FW_BOARD_ID, FW_LAYOUT_AB_ID, "x", 1, k, klen, r, rlen);
	assert(parse(out, n) == FW_BUNDLE_IMAGE);
	r[0] = 'h';
	r[rlen - 1] = 1; /* nonzero padding */
	n = fw_bundle_build(out, FW_BOARD_ID, FW_LAYOUT_AB_ID, "x", 1, k, klen, r, rlen);
	assert(parse(out, n) == FW_BUNDLE_IMAGE);
	free(out);
	free(k);
	free(r);

	/* Components that do not fit their partitions. */
	k = make_uimage(FW_KERNEL_CAPACITY - 64 + 1, &klen);
	r = make_squashfs(3000, &rlen);
	out = malloc(fw_bundle_build(NULL, "b", "l", "x", 1, k, klen, r, rlen));
	n = fw_bundle_build(out, FW_BOARD_ID, FW_LAYOUT_AB_ID, "x", 1, k, klen, r, rlen);
	assert(parse(out, n) == FW_BUNDLE_TOO_LARGE);
	free(out);
	free(k);
	k = make_uimage(FW_KERNEL_CAPACITY - 64, &klen); /* exactly full: fine */
	out = malloc(fw_bundle_build(NULL, "b", "l", "x", 1, k, klen, r, rlen));
	n = fw_bundle_build(out, FW_BOARD_ID, FW_LAYOUT_AB_ID, "x", 1, k, klen, r, rlen);
	assert(parse(out, n) == FW_BUNDLE_OK);
	free(out);
	free(k);
	free(r);

	/* Another board or layout: valid, but not for this camera. */
	k = make_uimage(100, &klen);
	r = make_squashfs(200, &rlen);
	out = malloc(fw_bundle_build(NULL, "b", "l", "x", 1, k, klen, r, rlen) + 64);
	n = fw_bundle_build(out, "other-board", FW_LAYOUT_AB_ID, "x", 1, k, klen, r, rlen);
	assert(fw_bundle_parse(out, n, &parsed, why, sizeof(why)) == FW_BUNDLE_OK);
	assert(fw_bundle_compatible(&parsed, 1, why, sizeof(why)) == FW_BUNDLE_INCOMPATIBLE);
	n = fw_bundle_build(out, FW_BOARD_ID, FW_LAYOUT_SINGLE_ID, "x", 1, k, klen, r, rlen);
	assert(fw_bundle_parse(out, n, &parsed, why, sizeof(why)) == FW_BUNDLE_OK);
	assert(fw_bundle_compatible(&parsed, 1, why, sizeof(why)) == FW_BUNDLE_INCOMPATIBLE);
	char longname[80];
	memset(longname, 'x', sizeof(longname) - 1);
	longname[sizeof(longname) - 1] = '\0';
	assert(fw_bundle_build(NULL, FW_BOARD_ID, FW_LAYOUT_AB_ID, longname, 1, k, klen, r, rlen) == 0);
	free(out);
	free(k);
	free(r);
	free(b);
}

static uint8_t *read_fixture(const char *dir, const char *name, size_t *len)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*len = (size_t)ftell(f);
	rewind(f);
	uint8_t *buf = malloc(*len);
	assert(fread(buf, 1, *len, f) == *len);
	fclose(f);
	return buf;
}

/* The build's own kernel and rootfs pass the checks the camera applies. */
static void test_real_images(const char *dir)
{
	size_t klen, rlen;
	uint8_t *k = read_fixture(dir, "uImage", &klen), *r = read_fixture(dir, "rootfs.squashfs", &rlen);
	if (!k || !r) {
		puts("fw_test: no build images; skipping the real-image checks");
		free(k);
		free(r);
		return;
	}
	char why[160];
	assert(fw_check_uimage(k, (uint32_t)klen, why, sizeof(why)) == 0);
	assert(fw_check_squashfs(r, (uint32_t)rlen, why, sizeof(why)) == 0);
	k[100] ^= 1;
	assert(fw_check_uimage(k, (uint32_t)klen, why, sizeof(why)) != 0);
	free(k);
	free(r);
}

/* --------------------------------------------------------------------------
 * Flash helpers and layout
 */
static void test_flash(void)
{
	struct fake_flash ff = {0};
	struct fw_layout_map map;
	uint8_t buf[100000], back[100000];

	fake_flash_init(&ff, true);
	assert(fw_layout_resolve(&ff.flash, &map) == 0 && map.kind == FW_LAYOUT_AB);
	assert(map.all == (int)FW_LAYOUT_AB_COUNT && map.erasesize == FAKE_ERASESIZE);
	assert(!strcmp(fw_layout_id(map.kind), FW_LAYOUT_AB_ID));
	assert(fw_layout_find(&map, "rootfs_b")->index == 6);
	fake_flash_init(&ff, false);
	assert(fw_layout_resolve(&ff.flash, &map) == 0 && map.kind == FW_LAYOUT_SINGLE);
	ff.unknown_layout = true;
	assert(fw_layout_resolve(&ff.flash, &map) == 0 && map.kind == FW_LAYOUT_UNKNOWN);
	assert(map.all >= 0); /* a dump still works */
	assert(fw_layout_open(&ff.flash, &map, "kernel", true, NULL) == -ENOENT);
	int h = fw_layout_open(&ff.flash, &map, FW_PART_ALL, false, NULL);
	assert(h >= 0);
	ff.flash.ops->close(&ff.flash, h);

	fake_flash_init(&ff, true);
	fw_layout_resolve(&ff.flash, &map);
	struct fw_mtd_geometry g;
	h = fw_layout_open(&ff.flash, &map, "kernel_b", true, &g);
	assert(h >= 0 && g.size == FW_KERNEL_CAPACITY && g.erasesize == FAKE_ERASESIZE);
	for (size_t i = 0; i < sizeof(buf); i++)
		buf[i] = (uint8_t)(i * 31);
	/* Short transfers and EINTR are carried through. */
	ff.short_every = 2;
	ff.eintr_every = 3;
	assert(fw_flash_erase_range(&ff.flash, h, g.erasesize, 0, 0x20000) == 0);
	assert(fw_flash_write_all(&ff.flash, h, buf, sizeof(buf), 0) == 0);
	assert(fw_flash_read_all(&ff.flash, h, back, sizeof(back), 0) == 0);
	assert(!memcmp(buf, back, sizeof(buf)));
	assert(!memcmp(ff.mem + 0x370000, buf, sizeof(buf)));
	/* NOR: writing over unerased data only clears bits. */
	uint8_t ones = 0xff;
	assert(fw_flash_write_all(&ff.flash, h, &ones, 1, 1) == 0);
	assert(ff.mem[0x370001] == buf[1]);
	/* Unaligned erase, reads at the end. */
	assert(fw_flash_erase_range(&ff.flash, h, g.erasesize, 0x100, 0x8000) == -1 &&
	       errno == EINVAL);
	assert(fw_flash_read_all(&ff.flash, h, back, 10, FW_KERNEL_CAPACITY - 5) == -1 &&
	       errno == EIO);
	ff.flash.ops->close(&ff.flash, h);
	/* Geometry that disagrees with the table. */
	ff.wrong_geometry = true;
	assert(fw_layout_open(&ff.flash, &map, "kernel_b", true, NULL) == -EINVAL);
	ff.wrong_geometry = false;
	assert(fake_open_handles(&ff) == 0);
	free(ff.mem);
}

/* --------------------------------------------------------------------------
 * Environment
 */
static void env_from(struct fw_env *env, const char *const *pairs)
{
	memset(env->data, 0, sizeof(env->data));
	env->current = 0;
	for (; *pairs; pairs += 2)
		assert(fw_env_set(env, pairs[0], pairs[1]) == 0);
}

static void write_copy(struct fake_flash *ff, int copy, const struct fw_env *env, uint8_t flags)
{
	fw_env_image(env, flags, ff->mem + (copy ? 0x60000 : 0x50000));
}

static void test_env_choose(void)
{
	static uint8_t a[FW_ENV_SIZE], b[FW_ENV_SIZE];
	struct fw_env env, x, y;
	static const char *const one[] = {"active_slot", "a", NULL};
	static const char *const two[] = {"active_slot", "b", NULL};
	env_from(&x, one);
	env_from(&y, two);
	struct {
		bool a_ok, b_ok;
		uint8_t fa, fb;
		int want;
	} cases[] = {
		{true, false, 1, 1, 0},   {false, true, 1, 1, 1},   {true, true, 1, 0, 0},
		{true, true, 0, 1, 1},    {true, true, 1, 1, 0},    {true, true, 255, 0, 1},
		{true, true, 0, 255, 0},  {true, true, 2, 3, 1},    {true, true, 7, 3, 0},
		{false, false, 1, 0, -1},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		fw_env_image(&x, cases[i].fa, a);
		fw_env_image(&y, cases[i].fb, b);
		if (!cases[i].a_ok)
			a[10] ^= 1;
		if (!cases[i].b_ok)
			b[10] ^= 1;
		int ret = fw_env_choose(&env, a, b);
		assert(env.current == cases[i].want && (ret == 0) == (cases[i].want >= 0));
		if (cases[i].want >= 0)
			assert(!strcmp(fw_env_get(&env, "active_slot"), cases[i].want ? "b" : "a"));
	}
	/* A copy that could not be read counts as invalid. */
	fw_env_image(&x, 0, a);
	assert(fw_env_choose(&env, a, NULL) == 0 && env.current == 0);
}

static void test_env_edit(void)
{
	struct fw_env env;
	static const char *const pairs[] = {"bootcmd", "run k1_boot", "active_slot", "a", NULL};
	env_from(&env, pairs);
	assert(!strcmp(fw_env_get(&env, "active_slot"), "a"));
	assert(!fw_env_get(&env, "active"));    /* a prefix is not a name */
	assert(!fw_env_get(&env, "active_slo"));
	assert(fw_env_set(&env, "active_slot", "b") == 0);
	assert(!strcmp(fw_env_get(&env, "active_slot"), "b"));
	assert(!strcmp(fw_env_get(&env, "bootcmd"), "run k1_boot"));
	assert(fw_env_set(&env, "trial_slot", "a") == 0);
	assert(fw_env_set(&env, "trial_slot", NULL) == 0);
	assert(!fw_env_get(&env, "trial_slot"));
	assert(fw_env_set(&env, "trial_slot", NULL) == 0); /* absent: no-op */
	assert(fw_env_set(&env, "", "x") == -1 && errno == EINVAL);
	assert(fw_env_set(&env, "a=b", "x") == -1 && errno == EINVAL);
	assert(fw_env_set(&env, "x", "line\nbreak") == -1 && errno == EINVAL);
	/* Full: refused, unchanged. */
	char *big = malloc(FW_ENV_DATA_SIZE);
	memset(big, 'v', FW_ENV_DATA_SIZE - 1);
	big[FW_ENV_DATA_SIZE - 1] = '\0';
	assert(fw_env_set(&env, "big", big) == -1 && errno == ENOSPC);
	assert(!strcmp(fw_env_get(&env, "active_slot"), "b"));
	big[FW_ENV_DATA_SIZE - 40] = '\0';
	assert(fw_env_set(&env, "big", big) == 0);
	free(big);
	/* Unterminated data (no empty entry before the end) is refused. */
	memset(env.data, 'x', sizeof(env.data));
	assert(fw_env_set(&env, "a", "b") == -1);
	assert(!fw_env_get(&env, "a"));
}

static bool env_has(struct fw_env *env, const char *name, const char *value)
{
	const char *v = fw_env_get(env, name);
	return value ? v && !strcmp(v, value) : v == NULL;
}

static void test_env_save(void)
{
	struct fake_flash ff = {0};
	struct fw_layout_map map;
	struct fw_env env, check;
	static const char *const pairs[] = {"bootcmd", "run k1_boot", "active_slot", "a",
					    "upgrade_available", "0", NULL};

	fake_flash_init(&ff, true);
	fw_layout_resolve(&ff.flash, &map);
	assert(fw_env_load(&ff.flash, &map, &env) == -1 && errno == ENOMSG);
	assert(fw_env_save(&ff.flash, &map, &env) == -1 && errno == ENOMSG);

	/* As installed: env_a active, env_b blank. */
	env_from(&env, pairs);
	write_copy(&ff, 0, &env, 1);
	assert(fw_env_load(&ff.flash, &map, &env) == 0 && env.current == 0);
	assert(fw_env_set(&env, "trial_slot", "b") == 0);
	assert(fw_env_save(&ff.flash, &map, &env) == 0 && env.current == 1);
	assert(ff.mem[0x50004] == 0 && ff.mem[0x60004] == 1);
	assert(fw_env_load(&ff.flash, &map, &check) == 0 && check.current == 1);
	assert(env_has(&check, "trial_slot", "b") && env_has(&check, "active_slot", "a"));
	/* Alternates. */
	assert(fw_env_set(&env, "trial_slot", NULL) == 0);
	assert(fw_env_save(&ff.flash, &map, &env) == 0 && env.current == 0);
	assert(ff.mem[0x50004] == 1 && ff.mem[0x60004] == 0);
	assert(fw_env_load(&ff.flash, &map, &check) == 0 && env_has(&check, "trial_slot", NULL));
	assert(fake_open_handles(&ff) == 0);

	/* One copy unreadable: the other one is used, and saving repairs. */
	ff.mem[0x50010] ^= 1;
	assert(fw_env_load(&ff.flash, &map, &env) == 0 && env.current == 1);
	assert(env_has(&env, "trial_slot", "b")); /* the older, still valid copy */
	assert(fw_env_save(&ff.flash, &map, &env) == 0 && env.current == 0);
	assert(fw_env_load(&ff.flash, &map, &check) == 0 && check.current == 0);

	/* A write that reads back wrong is not made current. */
	assert(fw_env_set(&env, "bootcount", "2") == 0);
	ff.flip_at = 0x60100;
	assert(fw_env_save(&ff.flash, &map, &env) == -1 && errno == EIO);
	ff.flip_at = -1;
	assert(fw_env_load(&ff.flash, &map, &check) == 0 && check.current == 0);
	assert(env_has(&check, "bootcount", NULL));
	/* An erase that fails leaves the old copy too. */
	ff.fail_erase_at = 0;
	assert(fw_env_save(&ff.flash, &map, &env) == -1);
	fake_flash_faults_off(&ff);
	assert(fw_env_load(&ff.flash, &map, &check) == 0 && env_has(&check, "bootcount", NULL));
	free(ff.mem);
}

/* A power cut at every point of a save leaves exactly the old or the new
 * environment, chosen the way U-Boot chooses. */
static void test_env_power_cut(void)
{
	struct fake_flash ff = {0};
	struct fw_layout_map map;
	struct fw_env env, check;
	static const char *const pairs[] = {"active_slot", "a", "upgrade_available", "0", NULL};
	int new_seen = 0, old_seen = 0;

	fake_flash_init(&ff, true);
	fw_layout_resolve(&ff.flash, &map);
	for (int start = 0; start < 2; start++) {
		for (int cut = 0; cut < 12; cut++) {
			fake_flash_faults_off(&ff);
			memset(ff.mem + 0x50000, 0xff, 0x20000);
			env_from(&env, pairs);
			/* The current copy alternates with history. */
			write_copy(&ff, start, &env, 1);
			if (start)
				write_copy(&ff, 0, &env, 0);
			assert(fw_env_load(&ff.flash, &map, &env) == 0 && env.current == start);
			assert(fw_env_set(&env, "trial_slot", "b") == 0);
			assert(fw_env_set(&env, "upgrade_available", "1") == 0);
			ff.cut_after = cut;
			int ret = fw_env_save(&ff.flash, &map, &env);
			fake_flash_faults_off(&ff); /* power back */
			assert(fw_env_load(&ff.flash, &map, &check) == 0);
			bool is_new = env_has(&check, "trial_slot", "b") &&
				      env_has(&check, "upgrade_available", "1");
			bool is_old = env_has(&check, "trial_slot", NULL) &&
				      env_has(&check, "upgrade_available", "0");
			assert(is_new != is_old);
			assert(env_has(&check, "active_slot", "a"));
			if (ret == 0)
				assert(is_new); /* reported done means done */
			new_seen += is_new;
			old_seen += is_old;
		}
	}
	assert(new_seen && old_seen);
	free(ff.mem);
}

/* Interop with U-Boot's own tools: images from mkenvimage read back, and
 * the image fw_env writes is what mkenvimage makes from the same text. */
static void test_env_mkenvimage(const char *dir)
{
	size_t len;
	uint8_t *redund = read_fixture(dir, "env-redund.bin", &len);
	if (!redund) {
		puts("fw_test: no mkenvimage fixtures; skipping");
		return;
	}
	assert(len == FW_ENV_SIZE);
	struct fw_env env;
	assert(fw_env_choose(&env, redund, NULL) == 0 && env.flags[0] == 1);
	assert(env_has(&env, "active_slot", "a") && env_has(&env, "bootlimit", "3"));
	assert(env_has(&env, "k1_boot", "if test x = x; then echo yes; fi"));
	/* mkenvimage pads with 0xff after the double NUL, fw_env with 0:
	 * different bytes, both valid and equal in content. */
	uint8_t img[FW_ENV_SIZE];
	fw_env_image(&env, 1, img);
	assert(fw_env_choose(&env, img, NULL) == 0);
	size_t used = used_length(env.data);
	assert(used && !memcmp(img + FW_ENV_HEADER, redund + FW_ENV_HEADER, used));
	free(redund);
}

int main(void)
{
	const char *dir = getenv("FW_TEST_FIXTURES");
	test_sha256();
	test_crc();
	test_bundle();
	test_flash();
	test_env_choose();
	test_env_edit();
	test_env_save();
	test_env_power_cut();
	if (dir) {
		test_real_images(dir);
		test_env_mkenvimage(dir);
	} else {
		puts("fw_test: FW_TEST_FIXTURES not set; skipping fixture tests");
	}
	puts("firmware building block tests passed");
	return 0;
}
