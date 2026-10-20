/*
 * update_test.c -- the daemon side of the firmware protocol, driven the way
 * the gadget drives it (SETUP, then DATA for a SET_CUR), against the NOR
 * model in fake_flash.h and a scratch directory standing in for /proc,
 * /overlay and /run.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/fw_bundle.c"
#include "../src/fw_crc.c"
#include "../src/fw_env.c"
#include "../src/fw_flash.c"
#include "../src/fw_sha256.c"
#include "../src/uvcd_update.c"
#include "fake_flash.h"

void uvcd_log(int level, const char *fmt, ...)
{
	(void)level;
	if (getenv("UPDATE_TEST_LOG")) {
		va_list ap;
		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		va_end(ap);
		fputc('\n', stderr);
	}
}

struct dev {
	struct fake_flash ff;
	struct uvcd_update_config cfg;
	struct uvcd_update *u;
	char root[256];
	char path[8][320];
	uint32_t seq;
	int reboots, pauses, resumes;
	bool paused_flag_seen;
};

static struct dev *current;

static void test_quiesce(bool pause)
{
	if (pause) {
		current->pauses++;
		current->paused_flag_seen = uvcd_update_persist_paused(current->u);
	} else {
		current->resumes++;
	}
}

static int test_reboot(void)
{
	current->reboots++;
	return 0;
}

static void write_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");
	assert(f);
	fputs(text, f);
	fclose(f);
}

static void mkdirs(const char *path)
{
	char tmp[400];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
}

enum { P_CMDLINE, P_MEMINFO, P_MOUNTS, P_RELEASE, P_SERIAL, P_DATA, P_RUN };

static void set_cmdline(struct dev *d, const char *slot, const char *mode)
{
	char text[256];
	snprintf(text, sizeof(text), "console=ttyS1 root=/dev/mtdblock4 k1cam.slot=%s "
		 "k1cam.boot=%s k1cam.layout=k1cam-ab-8m-v1\n", slot, mode);
	write_file(d->path[P_CMDLINE], text);
	char mounts[512];
	snprintf(mounts, sizeof(mounts),
		 "/dev/root /rom squashfs ro,relatime 0 0\n"
		 "overlay / overlayfs rw,noatime,lowerdir=/,upperdir=%s/slots/%s/root 0 0\n"
		 "/dev/mtdblock7 /overlay jffs2 rw,noatime 0 0\n",
		 d->path[P_DATA], slot);
	write_file(d->path[P_MOUNTS], mounts);
}

static void set_memory(struct dev *d, long avail_kb)
{
	char text[256];
	/* Linux 3.10: no MemAvailable. */
	snprintf(text, sizeof(text), "MemTotal:  41984 kB\nMemFree: %ld kB\nBuffers: 0 kB\n"
		 "Cached: 1000 kB\nShmem: 1000 kB\n", avail_kb);
	write_file(d->path[P_MEMINFO], text);
}

/* The environment as the A/B image installs it, plus overrides. */
static void set_env(struct dev *d, const char *active, const char *trial, const char *upgrade,
		    const char *bootcount)
{
	struct fw_env env;
	memset(&env, 0, sizeof(env));
	assert(fw_env_set(&env, "bootcmd", "run k1_boot") == 0);
	assert(fw_env_set(&env, "active_slot", active) == 0);
	if (trial)
		assert(fw_env_set(&env, "trial_slot", trial) == 0);
	assert(fw_env_set(&env, "upgrade_available", upgrade) == 0);
	assert(fw_env_set(&env, "bootcount", bootcount) == 0);
	assert(fw_env_set(&env, "bootlimit", "3") == 0);
	assert(fw_env_set(&env, "k1_boot_compat", "1") == 0);
	memset(d->ff.mem + 0x50000, 0xff, 0x20000);
	fw_env_image(&env, 1, d->ff.mem + 0x50000);
}

static struct fw_env *read_env(struct dev *d)
{
	static struct fw_env env;
	struct fw_layout_map map;
	assert(fw_layout_resolve(&d->ff.flash, &map) == 0);
	assert(fw_env_load(&d->ff.flash, &map, &env) == 0);
	return &env;
}

static bool env_is(struct dev *d, const char *name, const char *value)
{
	const char *v = fw_env_get(read_env(d), name);
	return value ? v && !strcmp(v, value) : !v;
}

static void dev_start(struct dev *d)
{
	current = d;
	d->u = uvcd_update_create(&d->cfg);
	assert(d->u);
}

static void dev_stop(struct dev *d)
{
	uvcd_update_destroy(d->u);
	d->u = NULL;
}

/* A camera on the A/B layout, confirmed slot a, running a. */
static void dev_init(struct dev *d, bool ab)
{
	static int n;
	fake_flash_init(&d->ff, ab);
	snprintf(d->root, sizeof(d->root), "%s/update-test-%d-%d",
		 getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid(), n++);
	static const char *const names[] = {"cmdline", "meminfo", "mounts", "release", "serial",
					    "overlay", "run"};
	for (int i = 0; i < 7; i++)
		snprintf(d->path[i], sizeof(d->path[i]), "%s/%s", d->root, names[i]);
	mkdirs(d->path[P_DATA]);
	char slot[400];
	snprintf(slot, sizeof(slot), "%s/slots/a/root/etc", d->path[P_DATA]);
	mkdirs(slot);
	snprintf(slot, sizeof(slot), "%s/slots/a/root/etc/uvcd.conf", d->path[P_DATA]);
	write_file(slot, "brightness=140\n");
	snprintf(slot, sizeof(slot), "%s/slots/a/root/etc/hostname", d->path[P_DATA]);
	write_file(slot, "not carried over\n");
	write_file(d->path[P_RELEASE], "board=k1cam-t31l-gc2083\nlayout=k1cam-ab-8m-v1\n"
		   "build=k1cam-old\nkernel=0123456789ab\nmin_boot_compat=1\nprotocol=1\n");
	write_file(d->path[P_SERIAL], "4242\n");
	set_cmdline(d, "a", "confirmed");
	set_memory(d, 40000);
	if (ab)
		set_env(d, "a", NULL, "0", "0");

	uvcd_update_default_config(&d->cfg);
	d->cfg.flash = d->ff.flash;
	d->cfg.cmdline_path = d->path[P_CMDLINE];
	d->cfg.meminfo_path = d->path[P_MEMINFO];
	d->cfg.mounts_path = d->path[P_MOUNTS];
	d->cfg.release_path = d->path[P_RELEASE];
	d->cfg.serial_path = d->path[P_SERIAL];
	d->cfg.data_root = d->path[P_DATA];
	d->cfg.run_dir = d->path[P_RUN];
	d->cfg.quiesce = test_quiesce;
	d->cfg.reboot = test_reboot;
	d->cfg.memory_reserve_kb = 4096;
	d->seq = 100;
	d->reboots = d->pauses = d->resumes = 0;
	dev_start(d);
}

static void dev_free(struct dev *d)
{
	dev_stop(d);
	char cmd[400];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", d->root);
	assert(system(cmd) == 0);
	free(d->ff.mem);
	d->ff.mem = NULL;
}

/* --------------------------------------------------------------------------
 * The host's side, as the gadget presents it
 */
static void status(struct dev *d, uint8_t *s)
{
	assert(uvcd_update_setup(d->u, UVC_GET_CUR, UVCD_FW_STATUS_SELECTOR, true, 60, s) == 60);
	assert(uvcd_fw_has_magic(s) && s[UVCD_FW_STATUS_VERSION] == 1);
}

static uint8_t phase(struct dev *d)
{
	uint8_t s[60];
	status(d, s);
	return s[UVCD_FW_STATUS_PHASE];
}

static uint32_t status32(struct dev *d, int at)
{
	uint8_t s[60];
	status(d, s);
	return uvcd_fw_get_le32(s + at);
}

/* SET_CUR one frame; returns its result from STATUS. */
static int set_frame(struct dev *d, uint8_t selector, const uint8_t *f, uint32_t seq)
{
	uint8_t s[60];
	assert(uvcd_update_setup(d->u, UVC_SET_CUR, selector, false, 60, NULL) == 60);
	uvcd_update_data(d->u, selector, f, 60);
	status(d, s);
	assert(uvcd_fw_get_le32(s + UVCD_FW_STATUS_SEQUENCE) == seq);
	return s[UVCD_FW_STATUS_RESULT];
}

static void command_frame(uint8_t *f, uint32_t seq, uint8_t op, uint8_t target, uint8_t flags,
			  uint32_t session, uint32_t offset, uint32_t length, const uint8_t *digest)
{
	memset(f, 0, 60);
	uvcd_fw_put_magic(f);
	f[UVCD_FW_COMMAND_VERSION] = 1;
	f[UVCD_FW_COMMAND_OPCODE] = op;
	f[UVCD_FW_COMMAND_TARGET] = target;
	f[UVCD_FW_COMMAND_FLAGS] = flags;
	uvcd_fw_put_le32(f + UVCD_FW_COMMAND_SEQUENCE, seq);
	uvcd_fw_put_le32(f + UVCD_FW_COMMAND_SESSION, session);
	uvcd_fw_put_le32(f + UVCD_FW_COMMAND_OFFSET, offset);
	uvcd_fw_put_le32(f + UVCD_FW_COMMAND_LENGTH, length);
	if (digest)
		memcpy(f + UVCD_FW_COMMAND_DIGEST, digest, 32);
}

static int cmd(struct dev *d, uint8_t op, uint8_t target, uint8_t flags, uint32_t session,
	       uint32_t offset, uint32_t length, const uint8_t *digest)
{
	uint8_t f[60];
	uint32_t seq = ++d->seq;
	command_frame(f, seq, op, target, flags, session, offset, length, digest);
	return set_frame(d, UVCD_FW_COMMAND_SELECTOR, f, seq);
}

static void data_frame(uint8_t *f, uint32_t seq, uint32_t session, uint32_t offset,
		       const uint8_t *payload, uint8_t n)
{
	memset(f, 0, 60);
	uvcd_fw_put_le32(f + UVCD_FW_DATA_SESSION, session);
	uvcd_fw_put_le32(f + UVCD_FW_DATA_OFFSET, offset);
	uvcd_fw_put_le32(f + UVCD_FW_DATA_SEQUENCE, seq);
	f[UVCD_FW_DATA_LENGTH] = n;
	memcpy(f + UVCD_FW_DATA_PAYLOAD, payload, n);
	uint16_t crc = fw_crc16(FW_CRC16_INIT, f, UVCD_FW_DATA_CRC);
	uvcd_fw_put_le16(f + UVCD_FW_DATA_CRC, fw_crc16(crc, f + UVCD_FW_DATA_PAYLOAD, n));
}

static int send_data(struct dev *d, uint32_t session, uint32_t offset, const uint8_t *p,
		     uint8_t n)
{
	uint8_t f[60];
	uint32_t seq = ++d->seq;
	data_frame(f, seq, session, offset, p, n);
	return set_frame(d, UVCD_FW_DATA_SELECTOR, f, seq);
}

/* READ a window and GET it; returns the payload length. */
static int read_window(struct dev *d, uint8_t target, uint32_t session, uint32_t offset,
		       uint8_t *out)
{
	uint8_t w[60];
	int r = cmd(d, UVCD_FW_OP_READ, target, 0, session, offset, 0, NULL);
	if (r)
		return -r;
	assert(uvcd_update_setup(d->u, UVC_GET_CUR, UVCD_FW_DATA_SELECTOR, true, 60, w) == 60);
	uint8_t n = w[UVCD_FW_DATA_LENGTH];
	uint16_t crc = fw_crc16(FW_CRC16_INIT, w, UVCD_FW_DATA_CRC);
	assert(n <= 44 && uvcd_fw_get_le16(w + UVCD_FW_DATA_CRC) ==
				  fw_crc16(crc, w + UVCD_FW_DATA_PAYLOAD, n));
	assert(uvcd_fw_get_le32(w + UVCD_FW_DATA_SEQUENCE) == d->seq);
	assert(uvcd_fw_get_le32(w + UVCD_FW_DATA_OFFSET) == offset);
	assert(uvcd_fw_get_le32(w + UVCD_FW_DATA_SESSION) == session);
	assert(w[UVCD_FW_DATA_KIND] == target);
	memcpy(out, w + UVCD_FW_DATA_PAYLOAD, n);
	return n;
}

static size_t read_object(struct dev *d, uint8_t target, uint32_t session, char *out,
			  size_t size)
{
	size_t len = 0;
	for (;;) {
		uint8_t buf[44];
		int n = read_window(d, target, session, (uint32_t)len, buf);
		assert(n >= 0 && len + (size_t)n < size);
		memcpy(out + len, buf, (size_t)n);
		len += (size_t)n;
		if (n < 44)
			break;
	}
	out[len] = '\0';
	return len;
}

static bool record_has(const char *record, const char *line)
{
	char want[128];
	snprintf(want, sizeof(want), "%s\n", line);
	return strstr(record, want) != NULL;
}

/* --------------------------------------------------------------------------
 * Bundles (as fw_test.c builds them)
 */
static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint8_t *make_bundle(size_t *len, const char *board, const char *build, uint8_t salt)
{
	uint32_t klen = 64 + 70000, rlen = 0x20000, used = rlen - 1000;
	uint8_t *k = calloc(1, klen), *r = calloc(1, rlen);
	for (uint32_t i = 64; i < klen; i++)
		k[i] = (uint8_t)(i * 7 + salt);
	put_be32(k, 0x27051956);
	put_be32(k + 12, klen - 64);
	put_be32(k + 24, fw_crc32(0, k + 64, klen - 64));
	k[28] = 5;
	k[29] = 5;
	k[30] = 2;
	put_be32(k + 4, fw_crc32(0, k, 64));
	for (uint32_t i = 96; i < used; i++)
		r[i] = (uint8_t)(i * 13 + salt);
	memcpy(r, "hsqs", 4);
	r[14] = 4;
	r[22] = 18;
	r[28] = 4;
	put_le32(r + 40, used);
	*len = fw_bundle_build(NULL, board, FW_LAYOUT_AB_ID, build, 1, k, klen, r, rlen);
	uint8_t *b = malloc(*len);
	fw_bundle_build(b, board, FW_LAYOUT_AB_ID, build, 1, k, klen, r, rlen);
	free(k);
	free(r);
	return b;
}

static int upload(struct dev *d, const uint8_t *b, size_t len, uint32_t *session)
{
	uint8_t digest[32];
	fw_sha256(b, len, digest);
	int r = cmd(d, UVCD_FW_OP_BEGIN_UPGRADE, 0, 0, 0, 0, (uint32_t)len, digest);
	if (r)
		return r;
	*session = status32(d, UVCD_FW_STATUS_SESSION);
	assert(*session && phase(d) == UVCD_FW_PHASE_RECEIVING);
	for (size_t off = 0; off < len; off += 44) {
		uint8_t n = len - off < 44 ? (uint8_t)(len - off) : 44;
		r = send_data(d, *session, (uint32_t)off, b + off, n);
		if (r)
			return r;
	}
	assert(status32(d, UVCD_FW_STATUS_DONE) == len);
	return cmd(d, UVCD_FW_OP_FINISH_UPGRADE, 0, 0, *session, 0, (uint32_t)len, NULL);
}

/* What slot b must hold after installing bundle b. */
static void check_slot_b(struct dev *d, const uint8_t *b, size_t len)
{
	struct fw_bundle parsed;
	char why[160];
	assert(fw_bundle_parse(b, len, &parsed, why, sizeof(why)) == FW_BUNDLE_OK);
	const struct fw_bundle_component *k = &parsed.component[0], *r = &parsed.component[1];
	assert(!memcmp(d->ff.mem + 0x370000, b + k->offset, k->length));
	assert(!memcmp(d->ff.mem + 0x4b0000, b + r->offset, r->length));
	for (uint32_t i = k->length; i < FW_KERNEL_CAPACITY; i++)
		assert(d->ff.mem[0x370000 + i] == 0xff);
	for (uint32_t i = r->length; i < FW_ROOTFS_CAPACITY; i++)
		assert(d->ff.mem[0x4b0000 + i] == 0xff);
}

static bool path_exists(struct dev *d, const char *rel)
{
	char p[512];
	snprintf(p, sizeof(p), "%s/%s", d->root, rel);
	return access(p, F_OK) == 0;
}

/* --------------------------------------------------------------------------
 * Tests
 */
static void test_controls(void)
{
	struct dev d = {0};
	uint8_t out[60];
	dev_init(&d, true);
	assert(uvcd_update_setup(d.u, UVC_GET_LEN, 26, true, 2, out) == 2 && out[0] == 60 && !out[1]);
	assert(uvcd_update_setup(d.u, UVC_GET_INFO, 26, true, 1, out) == 1 &&
	       out[0] == UVC_CONTROL_CAP_SET);
	assert(uvcd_update_setup(d.u, UVC_GET_INFO, 27, true, 1, out) == 1 &&
	       out[0] == (UVC_CONTROL_CAP_GET | UVC_CONTROL_CAP_SET));
	assert(uvcd_update_setup(d.u, UVC_GET_INFO, 28, true, 1, out) == 1 &&
	       out[0] == UVC_CONTROL_CAP_GET);
	assert(uvcd_update_setup(d.u, UVC_SET_CUR, 28, false, 60, NULL) == -1);
	assert(uvcd_update_setup(d.u, UVC_SET_CUR, 26, false, 59, NULL) == -1);
	assert(uvcd_update_setup(d.u, UVC_GET_CUR, 26, true, 60, out) == -1);
	assert(uvcd_update_setup(d.u, UVC_GET_MIN, 28, true, 60, out) == -1);
	/* DATA before anything was selected. */
	assert(uvcd_update_setup(d.u, UVC_GET_CUR, 27, true, 60, out) == 60);
	assert(out[UVCD_FW_DATA_KIND] == UVCD_FW_TARGET_NONE && out[UVCD_FW_DATA_LENGTH] == 0);
	/* Idle status says what the camera can do. */
	status(&d, out);
	assert(out[UVCD_FW_STATUS_PHASE] == UVCD_FW_PHASE_IDLE);
	assert(uvcd_fw_get_le32(out + UVCD_FW_STATUS_FEATURES) ==
	       (UVCD_FW_FEATURE_INFO | UVCD_FW_FEATURE_DUMP | UVCD_FW_FEATURE_AB_SLOTS |
		UVCD_FW_FEATURE_UPGRADE));
	assert(out[UVCD_FW_STATUS_ACTIVE_SLOT] == 0 && out[UVCD_FW_STATUS_RUNNING_SLOT] == 0 &&
	       out[UVCD_FW_STATUS_TRIAL_SLOT] == 0xff &&
	       out[UVCD_FW_STATUS_BOOT] == UVCD_FW_BOOT_CONFIRMED);
	for (int i = UVCD_FW_STATUS_RESERVED; i < 60; i++)
		assert(out[i] == 0);
	dev_free(&d);
}

static void hex_frame(const char *hex, uint8_t *out)
{
	memset(out, 0, 60);
	for (int i = 0; *hex; hex++) {
		if (*hex == ' ')
			continue;
		unsigned v;
		assert(sscanf(hex, "%2x", &v) == 1);
		out[i++] = (uint8_t)v;
		hex++;
	}
}

/* The frame layout, written out byte by byte from uvcd_firmware.h's
 * description rather than built with its helpers. */
static void test_golden_frames(void)
{
	struct dev d = {0};
	uint8_t f[60], s[60], want[60];
	dev_init(&d, true);
	/* BEGIN_UPGRADE, sequence 0x01020304, 4096 bytes, digest 0x11 x 32. */
	hex_frame("4b314657 01 03 00 00 04030201 00000000 00000000 00100000"
		  "1111111111111111111111111111111111111111111111111111111111111111"
		  "00000000", f);
	assert(set_frame(&d, 26, f, 0x01020304) == 0);
	status(&d, s);
	uint32_t session = uvcd_fw_get_le32(s + 12);
	/* STATUS: magic, version 1, RECEIVING (4), last opcode 3, result 0,
	 * sequence, session, 0 of 4096, features 0x0f, poll 0, slots a/none/a,
	 * boot confirmed (2), operation 3, no error, detail 0. */
	hex_frame("4b314657 01 04 03 00 04030201 00000000 00000000 00100000 0f000000 0000"
		  "00 ff 00 02 03 00 00000000", want);
	uvcd_fw_put_le32(want + 12, session);
	assert(!memcmp(s, want, 60));
	/* DATA: session, offset 0, sequence 0x0a0b0c0d, 2 bytes, kind 0, CRC,
	 * payload "K1". */
	data_frame(f, 0x0a0b0c0d, session, 0, (const uint8_t *)"K1", 2);
	hex_frame("00000000 00000000 0d0c0b0a 02 00 0000 4b31", want);
	uvcd_fw_put_le32(want, session);
	uint16_t crc = fw_crc16(FW_CRC16_INIT, want, 14);
	crc = fw_crc16(crc, want + 16, 2);
	want[14] = (uint8_t)crc;
	want[15] = (uint8_t)(crc >> 8);
	assert(!memcmp(f, want, 60));
	assert(set_frame(&d, 27, f, 0x0a0b0c0d) == 0 && status32(&d, 16) == 2);
	dev_free(&d);
}

static void test_frames(void)
{
	struct dev d = {0};
	uint8_t f[60], s[60];
	dev_init(&d, true);

	/* Malformed frames are rejected with a reason and change nothing. */
	command_frame(f, 7, UVCD_FW_OP_INFO, 0, 0, 0, 0, 0, NULL);
	f[0] = 'X';
	assert(set_frame(&d, 26, f, 7) == UVCD_FW_ERROR_BAD_FRAME);
	command_frame(f, 8, UVCD_FW_OP_INFO, 0, 0, 0, 0, 0, NULL);
	f[UVCD_FW_COMMAND_VERSION] = 2;
	assert(set_frame(&d, 26, f, 8) == UVCD_FW_ERROR_BAD_VERSION);
	command_frame(f, 9, UVCD_FW_OP_INFO, 0, 0, 0, 0, 0, NULL);
	f[59] = 1;
	assert(set_frame(&d, 26, f, 9) == UVCD_FW_ERROR_BAD_FRAME);
	command_frame(f, 10, UVCD_FW_OP_INFO, 0, 0, 0, 0, 0, NULL);
	f[UVCD_FW_COMMAND_DIGEST] = 1; /* digest outside BEGIN_UPGRADE */
	assert(set_frame(&d, 26, f, 10) == UVCD_FW_ERROR_BAD_FRAME);
	command_frame(f, 11, UVCD_FW_OP_INFO, 1, 0, 0, 0, 0, NULL);
	assert(set_frame(&d, 26, f, 11) == UVCD_FW_ERROR_BAD_FRAME);
	command_frame(f, 12, 99, 0, 0, 0, 0, 0, NULL);
	assert(set_frame(&d, 26, f, 12) == UVCD_FW_ERROR_UNSUPPORTED);
	command_frame(f, 0, UVCD_FW_OP_INFO, 0, 0, 0, 0, 0, NULL);
	assert(set_frame(&d, 26, f, 0) == UVCD_FW_ERROR_BAD_FRAME);
	/* A short data stage (the kernel bounds it to 60). */
	command_frame(f, 13, UVCD_FW_OP_INFO, 0, 0, 0, 0, 0, NULL);
	assert(uvcd_update_setup(d.u, UVC_SET_CUR, 26, false, 60, NULL) == 60);
	uvcd_update_data(d.u, 26, f, 20);
	status(&d, s);
	assert(uvcd_fw_get_le32(s + UVCD_FW_STATUS_SEQUENCE) == 13 &&
	       s[UVCD_FW_STATUS_RESULT] == UVCD_FW_ERROR_BAD_FRAME);
	assert(phase(&d) == UVCD_FW_PHASE_IDLE);

	/* The same frame again is not run again; the same sequence with
	 * other bytes is stale. */
	command_frame(f, 20, UVCD_FW_OP_BEGIN_DUMP, 0, 0, 0, 0, 0, NULL);
	assert(set_frame(&d, 26, f, 20) == 0);
	uint32_t session = status32(&d, UVCD_FW_STATUS_SESSION);
	uvcd_update_wait_idle(d.u);
	assert(phase(&d) == UVCD_FW_PHASE_READY);
	assert(set_frame(&d, 26, f, 20) == 0); /* repeat: still accepted, no new session */
	assert(status32(&d, UVCD_FW_STATUS_SESSION) == session && d.pauses == 1);
	command_frame(f, 20, UVCD_FW_OP_INFO, 0, 0, 0, 0, 0, NULL);
	assert(set_frame(&d, 26, f, 20) == UVCD_FW_ERROR_STALE_SEQUENCE);
	assert(cmd(&d, UVCD_FW_OP_END_DUMP, 0, 0, session, 0, 0, NULL) == 0);
	dev_free(&d);
}

static void test_info(void)
{
	struct dev d = {0};
	char record[2048];
	uint8_t buf[44];
	dev_init(&d, true);
	assert(read_window(&d, UVCD_FW_TARGET_INFO, 0, 0, buf) == -UVCD_FW_ERROR_STATE);
	assert(cmd(&d, UVCD_FW_OP_INFO, 0, 0, 0, 0, 0, NULL) == 0);
	read_object(&d, UVCD_FW_TARGET_INFO, 0, record, sizeof(record));
	assert(record_has(record, "protocol=1") && record_has(record, "board=k1cam-t31l-gc2083"));
	assert(record_has(record, "layout=k1cam-ab-8m-v1") && record_has(record, "build=k1cam-old"));
	assert(record_has(record, "boot=confirmed") && record_has(record, "boot_mode=confirmed"));
	assert(record_has(record, "running_slot=a") && record_has(record, "active_slot=a"));
	assert(record_has(record, "trial_slot=") && record_has(record, "upgrade_available=0"));
	assert(record_has(record, "serial=4242") && record_has(record, "boot_compat=1"));
	assert(record_has(record, "erase_size=32768") &&
	       record_has(record, "kernel_capacity=1310720") &&
	       record_has(record, "rootfs_capacity=1835008"));
	assert(record_has(record, "features=info,dump,ab,upgrade"));
	size_t len = strlen(record);
	assert(read_window(&d, UVCD_FW_TARGET_INFO, 0, (uint32_t)len + 1, buf) ==
	       -UVCD_FW_ERROR_RANGE);
	dev_free(&d);

	/* The original single-slot layout: info and dumps, no updates. */
	dev_init(&d, false);
	assert(cmd(&d, UVCD_FW_OP_INFO, 0, 0, 0, 0, 0, NULL) == 0);
	read_object(&d, UVCD_FW_TARGET_INFO, 0, record, sizeof(record));
	assert(record_has(record, "layout=k1cam-single-8m") && record_has(record, "boot=single"));
	assert(record_has(record, "features=info,dump"));
	uint8_t digest[32] = {1};
	assert(cmd(&d, UVCD_FW_OP_BEGIN_UPGRADE, 0, 0, 0, 0, 4096, digest) == UVCD_FW_ERROR_LAYOUT);
	dev_free(&d);
}

static void read_dump(struct dev *d, uint32_t session, uint8_t *out)
{
	for (uint32_t off = 0; off < FW_FLASH_SIZE; off += 44) {
		int n = read_window(d, UVCD_FW_TARGET_IMAGE, session, off, out + off);
		assert(n == (int)(FW_FLASH_SIZE - off < 44 ? FW_FLASH_SIZE - off : 44));
	}
	uint8_t end[44];
	assert(read_window(d, UVCD_FW_TARGET_IMAGE, session, FW_FLASH_SIZE, end) == 0);
	assert(read_window(d, UVCD_FW_TARGET_IMAGE, session, FW_FLASH_SIZE + 1, end) ==
	       -UVCD_FW_ERROR_RANGE);
}

static void test_dump(void)
{
	struct dev d = {0};
	char manifest[512], hex[65];
	uint8_t *copy = malloc(FW_FLASH_SIZE), digest[32];
	dev_init(&d, true);
	for (uint32_t i = 0; i < FW_FLASH_SIZE; i += 4093)
		d.ff.mem[i] = (uint8_t)(i >> 7);
	fw_sha256(d.ff.mem, FW_FLASH_SIZE, digest);
	fw_sha256_hex(digest, hex);

	assert(cmd(&d, UVCD_FW_OP_BEGIN_DUMP, 0, 0, 0, 0, 0, NULL) == 0);
	uint32_t session = status32(&d, UVCD_FW_STATUS_SESSION);
	uvcd_update_wait_idle(d.u);
	assert(phase(&d) == UVCD_FW_PHASE_READY && d.pauses == 1 && d.resumes == 1);
	assert(d.paused_flag_seen && !uvcd_update_persist_paused(d.u));
	/* One dump at a time; no update during it. */
	assert(cmd(&d, UVCD_FW_OP_BEGIN_DUMP, 0, 0, 0, 0, 0, NULL) == UVCD_FW_ERROR_BUSY);
	uint8_t dg[32] = {1};
	assert(cmd(&d, UVCD_FW_OP_BEGIN_UPGRADE, 0, 0, 0, 0, 4096, dg) == UVCD_FW_ERROR_BUSY);
	read_object(&d, UVCD_FW_TARGET_MANIFEST, session, manifest, sizeof(manifest));
	char want[128];
	snprintf(want, sizeof(want), "sha256=%s", hex);
	assert(record_has(manifest, "size=8388608") && record_has(manifest, want));
	assert(record_has(manifest, "consistency=snapshot"));
	/* The snapshot, not the flash: later writes don't show. */
	d.ff.mem[5] ^= 0xff;
	read_dump(&d, session, copy);
	d.ff.mem[5] ^= 0xff;
	assert(!memcmp(copy, d.ff.mem, FW_FLASH_SIZE));
	uint8_t buf[44];
	assert(read_window(&d, UVCD_FW_TARGET_IMAGE, session + 1, 0, buf) == -UVCD_FW_ERROR_SESSION);
	assert(read_window(&d, 7, session, 0, buf) == -UVCD_FW_ERROR_UNSUPPORTED);
	assert(cmd(&d, UVCD_FW_OP_END_DUMP, 0, 0, session, 0, 0, NULL) == 0);
	assert(phase(&d) == UVCD_FW_PHASE_COMPLETE && !d.u->snapshot);
	assert(read_window(&d, UVCD_FW_TARGET_IMAGE, session, 0, buf) == -UVCD_FW_ERROR_SESSION);

	/* Something writing the flash throughout: retried, then labelled. */
	d.ff.churn_at = 0x700000;
	assert(cmd(&d, UVCD_FW_OP_BEGIN_DUMP, 0, 0, 0, 0, 0, NULL) == 0);
	session = status32(&d, UVCD_FW_STATUS_SESSION);
	uvcd_update_wait_idle(d.u);
	read_object(&d, UVCD_FW_TARGET_MANIFEST, session, manifest, sizeof(manifest));
	assert(record_has(manifest, "consistency=unverified"));
	assert(cmd(&d, UVCD_FW_OP_ABORT, 0, 0, session, 0, 0, NULL) == 0);
	assert(phase(&d) == UVCD_FW_PHASE_ABORTED);
	d.ff.churn_at = -1;

	/* A read error during the snapshot. */
	d.ff.fail_read_at = 30;
	assert(cmd(&d, UVCD_FW_OP_BEGIN_DUMP, 0, 0, 0, 0, 0, NULL) == 0);
	uvcd_update_wait_idle(d.u);
	uint8_t s[60];
	status(&d, s);
	assert(s[UVCD_FW_STATUS_PHASE] == UVCD_FW_PHASE_ERROR &&
	       s[UVCD_FW_STATUS_ERROR] == UVCD_FW_ERROR_MTD &&
	       uvcd_fw_get_le32(s + UVCD_FW_STATUS_DETAIL) == EIO);
	assert(!d.u->snapshot && d.resumes == d.pauses && fake_open_handles(&d.ff) == 0);
	fake_flash_faults_off(&d.ff);

	/* Too little RAM: refused, unless a live (diagnostic) dump is fine. */
	set_memory(&d, 9000);
	assert(cmd(&d, UVCD_FW_OP_BEGIN_DUMP, 0, 0, 0, 0, 0, NULL) == UVCD_FW_ERROR_NO_MEMORY);
	assert(cmd(&d, UVCD_FW_OP_BEGIN_DUMP, 0, UVCD_FW_FLAG_ALLOW_LIVE, 0, 0, 0, NULL) == 0);
	session = status32(&d, UVCD_FW_STATUS_SESSION);
	assert(phase(&d) == UVCD_FW_PHASE_READY && !d.u->snapshot);
	read_object(&d, UVCD_FW_TARGET_MANIFEST, session, manifest, sizeof(manifest));
	assert(record_has(manifest, "consistency=live") && record_has(manifest, "sha256="));
	read_dump(&d, session, copy);
	assert(!memcmp(copy, d.ff.mem, FW_FLASH_SIZE));
	/* A host that goes away. */
	uvcd_update_disconnect(d.u);
	status(&d, s);
	assert(s[UVCD_FW_STATUS_PHASE] == UVCD_FW_PHASE_ERROR &&
	       s[UVCD_FW_STATUS_ERROR] == UVCD_FW_ERROR_DISCONNECTED);
	assert(fake_open_handles(&d.ff) == 0);
	/* Or just stops asking. */
	d.u->cfg.dump_timeout_ms = 0;
	assert(cmd(&d, UVCD_FW_OP_BEGIN_DUMP, 0, UVCD_FW_FLAG_ALLOW_LIVE, 0, 0, 0, NULL) == 0);
	usleep(2000);
	uvcd_update_tick(d.u);
	status(&d, s);
	assert(s[UVCD_FW_STATUS_ERROR] == UVCD_FW_ERROR_TIMEOUT && fake_open_handles(&d.ff) == 0);
	free(copy);
	dev_free(&d);
}

static void test_upgrade(void)
{
	struct dev d = {0};
	size_t len;
	uint32_t session;
	uint8_t *b = make_bundle(&len, FW_BOARD_ID, "k1cam-new", 1);
	uint8_t *before = malloc(FW_FLASH_SIZE);
	char path[512];

	dev_init(&d, true);
	/* Leftovers of an older release in slot b's settings are replaced. */
	snprintf(path, sizeof(path), "%s/slots/b/root/etc", d.path[P_DATA]);
	mkdirs(path);
	snprintf(path, sizeof(path), "%s/slots/b/root/etc/stale", d.path[P_DATA]);
	write_file(path, "old\n");
	memset(d.ff.mem + 0x70000, 0x5a, 0x300000); /* slot a: kernel_a + rootfs_a */
	memcpy(before, d.ff.mem, FW_FLASH_SIZE);

	assert(upload(&d, b, len, &session) == 0);
	uvcd_update_wait_idle(d.u);
	uint8_t s[60];
	status(&d, s);
	assert(s[UVCD_FW_STATUS_PHASE] == UVCD_FW_PHASE_STAGED);
	assert(uvcd_fw_get_le32(s + UVCD_FW_STATUS_SESSION) == session);
	assert(s[UVCD_FW_STATUS_TRIAL_SLOT] == 1 && s[UVCD_FW_STATUS_ACTIVE_SLOT] == 0);
	assert(!(uvcd_fw_get_le32(s + UVCD_FW_STATUS_FEATURES) & UVCD_FW_FEATURE_UPGRADE));
	check_slot_b(&d, b, len);
	/* Nothing else changed: boot, the confirmed slot, the data partition. */
	assert(!memcmp(before, d.ff.mem, 0x50000));
	assert(!memcmp(before + 0x70000, d.ff.mem + 0x70000, 0x300000));
	assert(!memcmp(before + 0x670000, d.ff.mem + 0x670000, 0x190000));
	assert(env_is(&d, "trial_slot", "b") && env_is(&d, "upgrade_available", "1") &&
	       env_is(&d, "bootcount", "0") && env_is(&d, "active_slot", "a") &&
	       env_is(&d, "bootcmd", "run k1_boot"));
	/* Slot b starts from slot a's uvcd settings and nothing else. */
	assert(path_exists(&d, "overlay/slots/b/root/etc/uvcd.conf"));
	assert(path_exists(&d, "overlay/slots/b/work"));
	assert(!path_exists(&d, "overlay/slots/b/root/etc/stale"));
	assert(!path_exists(&d, "overlay/slots/b/root/etc/hostname"));
	assert(path_exists(&d, "overlay/slots/a/root/etc/hostname"));
	/* Another update now would overwrite the scheduled trial: refused. */
	assert(cmd(&d, UVCD_FW_OP_BEGIN_UPGRADE, 0, 0, 0, 0, (uint32_t)len, s) ==
	       UVCD_FW_ERROR_SLOT_STATE);
	/* Not cancellable now, except by withdrawing the trial. */
	assert(cmd(&d, UVCD_FW_OP_ABORT, 0, 0, session, 0, 0, NULL) ==
	       UVCD_FW_ERROR_NOT_CANCELLABLE);
	/* Reboot into it. */
	assert(cmd(&d, UVCD_FW_OP_REBOOT, 0, 0, session + 1, 0, 0, NULL) == UVCD_FW_ERROR_SESSION);
	assert(cmd(&d, UVCD_FW_OP_REBOOT, 0, 0, session, 0, 0, NULL) == 0);
	assert(phase(&d) == UVCD_FW_PHASE_REBOOTING);
	uvcd_update_wait_idle(d.u);
	assert(d.reboots == 1);
	dev_stop(&d);

	/* The trial boot of slot b. */
	set_cmdline(&d, "b", "trial");
	dev_start(&d);
	status(&d, s);
	assert(s[UVCD_FW_STATUS_BOOT] == UVCD_FW_BOOT_TRIAL && s[UVCD_FW_STATUS_RUNNING_SLOT] == 1);
	assert(uvcd_update_trial_wants_check(d.u));
	uvcd_update_trial_health(d.u, true, "test");
	uvcd_update_wait_idle(d.u);
	assert(!uvcd_update_trial_wants_check(d.u));
	assert(env_is(&d, "active_slot", "b") && env_is(&d, "trial_slot", NULL) &&
	       env_is(&d, "upgrade_available", "0") && env_is(&d, "bootcount", "0"));
	assert(path_exists(&d, "run/confirmed") && !path_exists(&d, "run/trial-failed"));
	status(&d, s);
	assert(s[UVCD_FW_STATUS_BOOT] == UVCD_FW_BOOT_CONFIRMED &&
	       (uvcd_fw_get_le32(s + UVCD_FW_STATUS_FEATURES) & UVCD_FW_FEATURE_UPGRADE));
	/* The next update goes to slot a. */
	size_t len2;
	uint8_t *b2 = make_bundle(&len2, FW_BOARD_ID, "k1cam-newer", 2);
	assert(upload(&d, b2, len2, &session) == 0);
	uvcd_update_wait_idle(d.u);
	assert(phase(&d) == UVCD_FW_PHASE_STAGED && env_is(&d, "trial_slot", "a"));
	check_slot_b(&d, b, len); /* slot b, confirmed now, untouched */
	free(b2);
	free(b);
	free(before);
	dev_free(&d);
}

static void expect_install_error(struct dev *d, const uint8_t *b, size_t len,
				 const uint8_t *announced, uint8_t error)
{
	uint8_t digest[32], s[60];
	uint32_t session;
	if (announced)
		memcpy(digest, announced, 32);
	else
		fw_sha256(b, len, digest);
	assert(cmd(d, UVCD_FW_OP_BEGIN_UPGRADE, 0, 0, 0, 0, (uint32_t)len, digest) == 0);
	session = status32(d, UVCD_FW_STATUS_SESSION);
	for (size_t off = 0; off < len; off += 44)
		assert(send_data(d, session, (uint32_t)off, b + off,
				 len - off < 44 ? (uint8_t)(len - off) : 44) == 0);
	assert(cmd(d, UVCD_FW_OP_FINISH_UPGRADE, 0, 0, session, 0, (uint32_t)len, NULL) == 0);
	uvcd_update_wait_idle(d->u);
	status(d, s);
	if (s[UVCD_FW_STATUS_PHASE] != UVCD_FW_PHASE_ERROR || s[UVCD_FW_STATUS_ERROR] != error) {
		fprintf(stderr, "phase %u error %u, want error %u\n", s[UVCD_FW_STATUS_PHASE],
			s[UVCD_FW_STATUS_ERROR], error);
		assert(0);
	}
	/* No trial scheduled, nothing left open or allocated. */
	assert(env_is(d, "trial_slot", NULL) && env_is(d, "upgrade_available", "0"));
	assert(fake_open_handles(&d->ff) == 0 && !d->u->bundle);
}

static void test_upgrade_failures(void)
{
	struct dev d = {0};
	size_t len;
	uint8_t *b = make_bundle(&len, FW_BOARD_ID, "k1cam-new", 3), s[60];
	uint8_t *blank = malloc(0x300000);
	memset(blank, 0xff, 0x300000);
	dev_init(&d, true);

	/* Refused before anything is written to flash. */
	uint8_t wrong[32] = {9};
	expect_install_error(&d, b, len, wrong, UVCD_FW_ERROR_HASH);
	assert(!memcmp(d.ff.mem + 0x370000, blank, 0x300000) && d.ff.erase_calls == 0);
	size_t olen;
	uint8_t *other = make_bundle(&olen, "other-board", "k1cam-new", 3);
	expect_install_error(&d, other, olen, NULL, UVCD_FW_ERROR_INCOMPATIBLE);
	free(other);
	uint8_t *bad = malloc(len);
	memcpy(bad, b, len);
	bad[len - 10] ^= 1; /* damaged file, digest taken of the damage */
	expect_install_error(&d, bad, len, NULL, UVCD_FW_ERROR_HASH);
	assert(d.ff.erase_calls == 0);

	/* Flash failures: the trial is never scheduled. */
	d.ff.fail_erase_at = 3;
	expect_install_error(&d, b, len, NULL, UVCD_FW_ERROR_ERASE);
	status(&d, s);
	assert(uvcd_fw_get_le32(s + UVCD_FW_STATUS_DETAIL) == EIO);
	fake_flash_faults_off(&d.ff);
	d.ff.fail_write_at = 4; /* kernel_b takes 3 writes: the second rootfs_b one */
	expect_install_error(&d, b, len, NULL, UVCD_FW_ERROR_WRITE);
	fake_flash_faults_off(&d.ff);
	d.ff.flip_at = 0x4b0000 + 5000; /* rootfs_b reads back wrong */
	expect_install_error(&d, b, len, NULL, UVCD_FW_ERROR_VERIFY);
	fake_flash_faults_off(&d.ff);
	d.ff.flip_at = 0x370000 + FW_KERNEL_CAPACITY - 1; /* not erased past the image */
	expect_install_error(&d, b, len, NULL, UVCD_FW_ERROR_VERIFY);
	fake_flash_faults_off(&d.ff);
	/* Short writes and EINTR are just slow. */
	d.ff.short_every = 3;
	d.ff.eintr_every = 7;
	uint32_t session;
	assert(upload(&d, b, len, &session) == 0);
	uvcd_update_wait_idle(d.u);
	assert(phase(&d) == UVCD_FW_PHASE_STAGED);
	check_slot_b(&d, b, len);
	fake_flash_faults_off(&d.ff);
	assert(cmd(&d, UVCD_FW_OP_ABORT, 0, UVCD_FW_FLAG_UNSCHEDULE, session, 0, 0, NULL) == 0);
	uvcd_update_wait_idle(d.u);
	assert(phase(&d) == UVCD_FW_PHASE_ABORTED && env_is(&d, "trial_slot", NULL) &&
	       env_is(&d, "upgrade_available", "0"));
	/* An environment that won't save: no trial, and it says so. The
	 * slot's erases come first, then the environment copy's. */
	d.ff.fail_erase_at = (int)(FW_KERNEL_CAPACITY + FW_ROOTFS_CAPACITY) / FAKE_ERASESIZE;
	expect_install_error(&d, b, len, NULL, UVCD_FW_ERROR_ENV);
	fake_flash_faults_off(&d.ff);
	free(bad);
	free(blank);
	free(b);
	dev_free(&d);
}

static void test_upload_protocol(void)
{
	struct dev d = {0};
	size_t len;
	uint8_t *b = make_bundle(&len, FW_BOARD_ID, "k1cam-new", 4), digest[32], f[60], s[60];
	dev_init(&d, true);
	fw_sha256(b, len, digest);

	uint8_t zero[32] = {0};
	assert(cmd(&d, UVCD_FW_OP_BEGIN_UPGRADE, 0, 0, 0, 0, (uint32_t)len, zero) ==
	       UVCD_FW_ERROR_BAD_FRAME);
	assert(cmd(&d, UVCD_FW_OP_BEGIN_UPGRADE, 0, 0, 0, 0, 100, digest) == UVCD_FW_ERROR_RANGE);
	assert(cmd(&d, UVCD_FW_OP_BEGIN_UPGRADE, 0, 0, 0, 0, 0x400000, digest) ==
	       UVCD_FW_ERROR_TOO_LARGE);
	set_memory(&d, 4096 + 100); /* the reserve, and not the bundle */
	assert(cmd(&d, UVCD_FW_OP_BEGIN_UPGRADE, 0, 0, 0, 0, (uint32_t)len, digest) ==
	       UVCD_FW_ERROR_NO_MEMORY);
	set_memory(&d, 40000);

	assert(cmd(&d, UVCD_FW_OP_BEGIN_UPGRADE, 0, 0, 0, 0, (uint32_t)len, digest) == 0);
	uint32_t session = status32(&d, UVCD_FW_STATUS_SESSION);
	assert(send_data(&d, session, 0, b, 44) == 0);
	/* Resending what was received is fine; other bytes are fatal later. */
	assert(send_data(&d, session, 0, b, 44) == 0);
	assert(send_data(&d, session, 10, b + 10, 20) == 0);
	/* The same frame (same sequence) again: kept, not counted twice. */
	data_frame(f, ++d.seq, session, 44, b + 44, 44);
	assert(set_frame(&d, 27, f, d.seq) == 0);
	assert(set_frame(&d, 27, f, d.seq) == 0);
	assert(status32(&d, UVCD_FW_STATUS_DONE) == 88);
	/* Frame-level problems leave the upload alive. */
	data_frame(f, ++d.seq, session, 88, b + 88, 44);
	f[UVCD_FW_DATA_PAYLOAD] ^= 1; /* CRC */
	assert(set_frame(&d, 27, f, d.seq) == UVCD_FW_ERROR_CHECKSUM);
	data_frame(f, ++d.seq, session, 88, b + 88, 44);
	f[UVCD_FW_DATA_KIND] = 1;
	assert(set_frame(&d, 27, f, d.seq) == UVCD_FW_ERROR_BAD_FRAME);
	assert(send_data(&d, session + 1, 88, b + 88, 44) == UVCD_FW_ERROR_SESSION);
	assert(cmd(&d, UVCD_FW_OP_FINISH_UPGRADE, 0, 0, session, 0, (uint32_t)len, NULL) ==
	       UVCD_FW_ERROR_STATE); /* not all there yet */
	assert(cmd(&d, UVCD_FW_OP_FINISH_UPGRADE, 0, 0, session, 0, (uint32_t)len - 1, NULL) ==
	       UVCD_FW_ERROR_RANGE);
	assert(phase(&d) == UVCD_FW_PHASE_RECEIVING);
	/* A gap ends it. */
	assert(send_data(&d, session, 200, b + 200, 44) == UVCD_FW_ERROR_GAP);
	status(&d, s);
	assert(s[UVCD_FW_STATUS_PHASE] == UVCD_FW_PHASE_ERROR &&
	       s[UVCD_FW_STATUS_ERROR] == UVCD_FW_ERROR_GAP && !d.u->bundle);
	assert(send_data(&d, session, 88, b + 88, 44) == UVCD_FW_ERROR_STATE);

	/* So does a resend with different bytes. */
	assert(cmd(&d, UVCD_FW_OP_BEGIN_UPGRADE, 0, 0, 0, 0, (uint32_t)len, digest) == 0);
	session = status32(&d, UVCD_FW_STATUS_SESSION);
	assert(send_data(&d, session, 0, b, 44) == 0);
	uint8_t other[44];
	memcpy(other, b, 44);
	other[3] ^= 1;
	assert(send_data(&d, session, 0, other, 44) == UVCD_FW_ERROR_CONFLICT);
	/* And data past the announced length. */
	assert(cmd(&d, UVCD_FW_OP_BEGIN_UPGRADE, 0, 0, 0, 0, (uint32_t)len, digest) == 0);
	session = status32(&d, UVCD_FW_STATUS_SESSION);
	assert(send_data(&d, session, (uint32_t)len - 10, b, 44) == UVCD_FW_ERROR_RANGE);

	/* Abort while receiving, and the next upload works. */
	assert(cmd(&d, UVCD_FW_OP_BEGIN_UPGRADE, 0, 0, 0, 0, (uint32_t)len, digest) == 0);
	session = status32(&d, UVCD_FW_STATUS_SESSION);
	assert(cmd(&d, UVCD_FW_OP_BEGIN_DUMP, 0, 0, 0, 0, 0, NULL) == UVCD_FW_ERROR_BUSY);
	assert(cmd(&d, UVCD_FW_OP_ABORT, 0, 0, session, 0, 0, NULL) == 0);
	assert(phase(&d) == UVCD_FW_PHASE_ABORTED && !d.u->bundle);
	/* Abandoned, and disconnected. */
	d.u->cfg.receive_timeout_ms = 0;
	assert(cmd(&d, UVCD_FW_OP_BEGIN_UPGRADE, 0, 0, 0, 0, (uint32_t)len, digest) == 0);
	usleep(2000);
	uvcd_update_tick(d.u);
	status(&d, s);
	assert(s[UVCD_FW_STATUS_ERROR] == UVCD_FW_ERROR_TIMEOUT && !d.u->bundle);
	d.u->cfg.receive_timeout_ms = 60000;
	assert(cmd(&d, UVCD_FW_OP_BEGIN_UPGRADE, 0, 0, 0, 0, (uint32_t)len, digest) == 0);
	uvcd_update_disconnect(d.u);
	status(&d, s);
	assert(s[UVCD_FW_STATUS_ERROR] == UVCD_FW_ERROR_DISCONNECTED && !d.u->bundle);
	/* Once installing, a disconnect doesn't stop it. */
	uint32_t sess;
	assert(upload(&d, b, len, &sess) == 0);
	uvcd_update_disconnect(d.u);
	uvcd_update_wait_idle(d.u);
	assert(phase(&d) == UVCD_FW_PHASE_STAGED);
	assert(cmd(&d, UVCD_FW_OP_ABORT, 0, UVCD_FW_FLAG_UNSCHEDULE, sess, 0, 0, NULL) == 0);
	uvcd_update_wait_idle(d.u);
	free(b);
	dev_free(&d);
}

static void cancel_on_erase(struct fake_flash *ff, int count)
{
	struct dev *d = ff->test;
	if (count == 5) {
		pthread_mutex_lock(&d->u->lock);
		d->u->cancel = true;
		pthread_mutex_unlock(&d->u->lock);
	}
}

static void test_cancel_and_state(void)
{
	struct dev d = {0};
	size_t len;
	uint8_t *b = make_bundle(&len, FW_BOARD_ID, "k1cam-new", 5), s[60];
	uint32_t session;
	dev_init(&d, true);

	/* Abort while erasing: stops at the next block, nothing scheduled. */
	d.ff.test = &d;
	d.ff.on_erase = cancel_on_erase;
	assert(upload(&d, b, len, &session) == 0);
	uvcd_update_wait_idle(d.u);
	assert(phase(&d) == UVCD_FW_PHASE_ABORTED && d.ff.erase_calls == 6);
	assert(env_is(&d, "trial_slot", NULL) && !d.u->bundle);
	fake_flash_faults_off(&d.ff);

	/* Withdraw a trial scheduled earlier, from a fresh daemon. */
	assert(upload(&d, b, len, &session) == 0);
	uvcd_update_wait_idle(d.u);
	dev_stop(&d);
	dev_start(&d);
	status(&d, s);
	assert(s[UVCD_FW_STATUS_TRIAL_SLOT] == 1 && s[UVCD_FW_STATUS_PHASE] == UVCD_FW_PHASE_IDLE);
	assert(cmd(&d, UVCD_FW_OP_ABORT, 0, 0, 0, 0, 0, NULL) == UVCD_FW_ERROR_SESSION);
	assert(cmd(&d, UVCD_FW_OP_REBOOT, 0, 0, 0, 0, 0, NULL) == 0); /* reboot into it... */
	uvcd_update_wait_idle(d.u);
	assert(d.reboots == 1);
	dev_stop(&d);
	dev_start(&d);
	assert(cmd(&d, UVCD_FW_OP_ABORT, 0, UVCD_FW_FLAG_UNSCHEDULE, 0, 0, 0, NULL) == 0);
	uvcd_update_wait_idle(d.u);
	assert(env_is(&d, "trial_slot", NULL) && env_is(&d, "upgrade_available", "0"));
	assert(cmd(&d, UVCD_FW_OP_ABORT, 0, UVCD_FW_FLAG_UNSCHEDULE, 0, 0, 0, NULL) ==
	       UVCD_FW_ERROR_STATE); /* nothing left to withdraw */
	assert(cmd(&d, UVCD_FW_OP_REBOOT, 0, 0, 0, 0, 0, NULL) == UVCD_FW_ERROR_STATE);

	/* States that refuse an update. */
	uint8_t digest[32] = {1};
	set_env(&d, "a", "b", "1", "1"); /* trial pending, booted confirmed */
	assert(cmd(&d, UVCD_FW_OP_BEGIN_UPGRADE, 0, 0, 0, 0, (uint32_t)len, digest) ==
	       UVCD_FW_ERROR_SLOT_STATE);
	set_env(&d, "b", NULL, "0", "0"); /* running a, confirmed b: a fallback boot */
	set_cmdline(&d, "a", "fallback");
	assert(cmd(&d, UVCD_FW_OP_BEGIN_UPGRADE, 0, 0, 0, 0, (uint32_t)len, digest) ==
	       UVCD_FW_ERROR_SLOT_STATE);
	status(&d, s);
	assert(s[UVCD_FW_STATUS_BOOT] == UVCD_FW_BOOT_FALLBACK);
	memset(d.ff.mem + 0x50000, 0xff, 0x20000); /* no valid environment */
	set_cmdline(&d, "a", "confirmed");
	assert(cmd(&d, UVCD_FW_OP_BEGIN_UPGRADE, 0, 0, 0, 0, (uint32_t)len, digest) ==
	       UVCD_FW_ERROR_SLOT_STATE);
	status(&d, s);
	assert(s[UVCD_FW_STATUS_BOOT] == UVCD_FW_BOOT_UNKNOWN);
	free(b);
	dev_free(&d);
}

static void test_trial(void)
{
	struct dev d = {0};
	dev_init(&d, true);
	dev_stop(&d);

	/* Running slot b as a trial, but / is on slot a's settings layer. */
	set_env(&d, "a", "b", "1", "1");
	set_cmdline(&d, "b", "trial");
	char mounts[512];
	snprintf(mounts, sizeof(mounts),
		 "overlay / overlayfs rw,lowerdir=/,upperdir=%s/slots/a/root 0 0\n",
		 d.path[P_DATA]);
	write_file(d.path[P_MOUNTS], mounts);
	dev_start(&d);
	assert(uvcd_update_trial_wants_check(d.u));
	uvcd_update_trial_health(d.u, true, "test");
	uvcd_update_wait_idle(d.u);
	assert(path_exists(&d, "run/trial-failed") && !path_exists(&d, "run/confirmed"));
	assert(env_is(&d, "active_slot", "a") && env_is(&d, "trial_slot", "b"));
	assert(!uvcd_update_trial_wants_check(d.u));
	/* A restarted daemon doesn't judge it again. */
	dev_stop(&d);
	dev_start(&d);
	assert(!uvcd_update_trial_wants_check(d.u));
	dev_stop(&d);
	char failed[400];
	snprintf(failed, sizeof(failed), "%s/trial-failed", d.path[P_RUN]);
	unlink(failed);

	/* A failed health check: reported, the environment untouched. */
	set_cmdline(&d, "b", "trial");
	dev_start(&d);
	assert(uvcd_update_trial_wants_check(d.u));
	uvcd_update_trial_health(d.u, false, "no encoded frames");
	assert(path_exists(&d, "run/trial-failed"));
	assert(env_is(&d, "trial_slot", "b") && env_is(&d, "active_slot", "a"));
	uvcd_update_trial_health(d.u, true, "late"); /* already judged */
	uvcd_update_wait_idle(d.u);
	assert(!path_exists(&d, "run/confirmed"));
	dev_stop(&d);
	unlink(failed);

	/* Not a trial: booted confirmed, or the trial is of the other slot. */
	set_cmdline(&d, "a", "confirmed");
	dev_start(&d);
	assert(!uvcd_update_trial_wants_check(d.u));
	dev_stop(&d);

	/* Withdrawn from inside its own trial: the gate takes it back. */
	set_cmdline(&d, "b", "trial");
	dev_start(&d);
	assert(cmd(&d, UVCD_FW_OP_ABORT, 0, UVCD_FW_FLAG_UNSCHEDULE, 0, 0, 0, NULL) == 0);
	uvcd_update_wait_idle(d.u);
	assert(path_exists(&d, "run/trial-failed") && env_is(&d, "trial_slot", NULL));
	uint8_t s[60];
	status(&d, s);
	assert(s[UVCD_FW_STATUS_BOOT] == UVCD_FW_BOOT_FALLBACK);
	dev_free(&d);
}

/* Power lost at every point of an install: the confirmed slot is always
 * intact, and a scheduled trial always has a complete, verified slot. */
static void test_power_cut_install(void)
{
	struct dev d = {0};
	size_t len;
	uint8_t *b = make_bundle(&len, FW_BOARD_ID, "k1cam-new", 6);
	uint8_t *slot_a = malloc(0x300000);
	int scheduled = 0, unscheduled = 0;
	uint32_t session;

	dev_init(&d, true);
	memset(d.ff.mem + 0x70000, 0x3c, 0x300000);
	memcpy(slot_a, d.ff.mem + 0x70000, 0x300000);
	/* Count the mutating operations of a whole install. */
	assert(upload(&d, b, len, &session) == 0);
	uvcd_update_wait_idle(d.u);
	assert(phase(&d) == UVCD_FW_PHASE_STAGED);
	dev_stop(&d);

	for (int cut = 0;; cut++) {
		set_env(&d, "a", NULL, "0", "0");
		memset(d.ff.mem + 0x370000, 0, 0x300000); /* slot b: whatever was there */
		fake_flash_faults_off(&d.ff);
		dev_start(&d);
		d.ff.cut_after = cut;
		assert(upload(&d, b, len, &session) == 0);
		uvcd_update_wait_idle(d.u);
		bool completed = !d.ff.dead;
		dev_stop(&d);
		fake_flash_faults_off(&d.ff); /* power back on */
		assert(!memcmp(slot_a, d.ff.mem + 0x70000, 0x300000));
		struct fw_env *env = read_env(&d);
		assert(!strcmp(fw_env_get(env, "active_slot"), "a"));
		const char *trial = fw_env_get(env, "trial_slot");
		if (trial) {
			assert(!strcmp(trial, "b") &&
			       !strcmp(fw_env_get(env, "upgrade_available"), "1"));
			check_slot_b(&d, b, len);
			scheduled++;
		} else {
			assert(!strcmp(fw_env_get(env, "upgrade_available"), "0"));
			unscheduled++;
		}
		if (completed)
			break;
	}
	assert(scheduled >= 1 && unscheduled > 100);
	free(slot_a);
	free(b);
	dev_free(&d);
}

/* Random frames on both selectors: rejected or not, never a crash, and the
 * flash and boot state stay as they were. */
static void test_fuzz(void)
{
	struct dev d = {0};
	uint8_t f[60], s[60];
	dev_init(&d, true);
	uint8_t *before = malloc(FW_FLASH_SIZE);
	memcpy(before, d.ff.mem, FW_FLASH_SIZE);
	srand(7);
	for (int i = 0; i < 50000; i++) {
		for (int j = 0; j < 60; j++)
			f[j] = (uint8_t)rand();
		if (i % 3 == 0) { /* get past the first checks */
			uvcd_fw_put_magic(f);
			f[UVCD_FW_COMMAND_VERSION] = 1;
			f[UVCD_FW_COMMAND_OPCODE] = (uint8_t)(rand() % 10);
			memset(f + UVCD_FW_COMMAND_DIGEST, 0, 36);
			if (i % 2)
				memset(f + UVCD_FW_COMMAND_OFFSET, 0, 8);
		}
		uint8_t selector = i % 2 ? UVCD_FW_COMMAND_SELECTOR : UVCD_FW_DATA_SELECTOR;
		int32_t len = i % 5 ? 60 : rand() % 61;
		uvcd_update_setup(d.u, UVC_SET_CUR, selector, false, 60, NULL);
		uvcd_update_data(d.u, selector, f, len);
		status(&d, s);
		uvcd_update_setup(d.u, UVC_GET_CUR, UVCD_FW_DATA_SELECTOR, true, 60, f);
		uvcd_update_wait_idle(d.u);
	}
	assert(d.reboots == 0); /* REBOOT needs a scheduled trial */
	assert(!memcmp(before, d.ff.mem, FW_FLASH_SIZE));
	free(before);
	dev_free(&d);
}

int main(void)
{
	test_controls();
	test_golden_frames();
	test_frames();
	test_info();
	test_dump();
	test_upgrade();
	test_upgrade_failures();
	test_upload_protocol();
	test_cancel_and_state();
	test_trial();
	test_power_cut_install();
	test_fuzz();
	puts("firmware protocol tests passed");
	return 0;
}
