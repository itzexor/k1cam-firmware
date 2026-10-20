/* Exercise socket framing, USB writes and listings without a camera, and
 * the firmware commands against the daemon's own protocol code. */
#include <assert.h>
#include <stdarg.h>
#include <sys/syscall.h>
#define main uvcdctl_program_main
#define socket test_socket
#define connect test_connect
#define read test_read
#define close test_close
#define dprintf test_dprintf
#define ioctl test_ioctl
#define open test_open
#define access test_access
#include "../uvcdctl/uvcdctl.c"
#include "../uvcdctl/firmware.c"
#undef main
#include "../src/fw_env.h"
#include "../src/uvcd_update.h"
#include "fake_flash.h"

/* fd 7: --device /fake (and the control socket); fd 9: /dev/video0, the
 * camera as found again after a reboot. Other files are real. */
#define FAKE_FD 7
#define VIDEO0_FD 9

static const char **chunks;
static size_t chunk_index;
static char last_request[160];
static bool local_socket;

int test_access(const char *path, int mode)
{
	assert(!strcmp(path, UVCD_SOCKET_PATH) && mode == F_OK);
	return local_socket ? 0 : -1;
}

int test_socket(int domain, int type, int protocol)
{
	assert(domain == AF_UNIX && type == SOCK_STREAM && protocol == 0);
	return 7;
}
int test_connect(int fd, const struct sockaddr *addr, socklen_t len)
{
	(void)addr;
	(void)len;
	assert(fd == 7);
	return 0;
}
int test_close(int fd)
{
	if (fd == FAKE_FD || fd == VIDEO0_FD)
		return 0;
	return (int)syscall(SYS_close, fd);
}
int test_dprintf(int fd, const char *fmt, ...)
{
	assert(fd == 7);
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(last_request, sizeof(last_request), fmt, ap);
	va_end(ap);
	return n;
}
ssize_t test_read(int fd, void *buf, size_t size)
{
	if (fd != FAKE_FD)
		return (ssize_t)syscall(SYS_read, fd, buf, size);
	const char *chunk = chunks[chunk_index];
	if (!chunk)
		return 0;
	size_t n = strlen(chunk);
	assert(n <= size);
	memcpy(buf, chunk, n); /* deliberately no NUL terminator */
	chunk_index++;
	return (ssize_t)n;
}

static int current_qp = 30, get_error, range_error;

/* --------------------------------------------------------------------------
 * The camera's firmware side: uvcd_update on a NOR model, as the gadget
 * drives it. NULL: a camera whose firmware predates the protocol.
 */
static struct fake_flash cam_flash;
static struct uvcd_update *cam;
static struct uvcd_update_config cam_cfg;
static char cam_root[256], cam_files[7][320];
static bool cam_rebooting, cam_absent, cam_rejects_trial;
static char cam_trial_build[64];

void uvcd_log(int level, const char *fmt, ...)
{
	(void)level;
	(void)fmt;
}

static void put_text(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");
	assert(f);
	fputs(text, f);
	fclose(f);
}

static int cam_reboot(void)
{
	cam_rebooting = true;
	return 0;
}

static void cam_set_boot(const char *slot, const char *mode, const char *build)
{
	char text[512];
	snprintf(text, sizeof(text), "root=/dev/mtdblock4 k1cam.slot=%s k1cam.boot=%s\n", slot,
		 mode);
	put_text(cam_files[0], text);
	snprintf(text, sizeof(text),
		 "overlay / overlayfs rw,lowerdir=/,upperdir=%s/slots/%s/root 0 0\n",
		 cam_files[5], slot);
	put_text(cam_files[2], text);
	snprintf(text, sizeof(text), "board=k1cam-t31l-gc2083\nlayout=k1cam-ab-8m-v1\n"
		 "build=%s\nkernel=0123456789ab\nmin_boot_compat=1\n", build);
	put_text(cam_files[3], text);
}

static void cam_env(const char *active, const char *trial, const char *upgrade)
{
	struct fw_env env;
	memset(&env, 0, sizeof(env));
	assert(fw_env_set(&env, "active_slot", active) == 0);
	if (trial)
		assert(fw_env_set(&env, "trial_slot", trial) == 0);
	assert(fw_env_set(&env, "upgrade_available", upgrade) == 0);
	assert(fw_env_set(&env, "bootcount", "0") == 0);
	assert(fw_env_set(&env, "bootlimit", "3") == 0);
	assert(fw_env_set(&env, "k1_boot_compat", "1") == 0);
	memset(cam_flash.mem + 0x50000, 0xff, 0x20000);
	fw_env_image(&env, 1, cam_flash.mem + 0x50000);
}

static void cam_memory(long kb)
{
	char text[128];
	snprintf(text, sizeof(text), "MemFree: %ld kB\nCached: 0 kB\n", kb);
	put_text(cam_files[1], text);
}

static void cam_power_on(void)
{
	cam = uvcd_update_create(&cam_cfg);
	assert(cam);
}

/* A camera on the A/B layout running build "k1cam-old" from slot a. */
static void cam_init(bool ab)
{
	static const char *const names[] = {"cmdline", "meminfo", "mounts", "release", "serial",
					    "overlay", "run"};
	snprintf(cam_root, sizeof(cam_root), "%s/uvcdctl-test-%d",
		 getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
	char cmd[600];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s/overlay/slots/a/root/etc'",
		 cam_root, cam_root);
	assert(system(cmd) == 0);
	for (int i = 0; i < 7; i++)
		snprintf(cam_files[i], sizeof(cam_files[i]), "%s/%s", cam_root, names[i]);
	put_text(cam_files[4], "777\n");
	cam_memory(40000);
	cam_set_boot("a", "confirmed", "k1cam-old");
	fake_flash_init(&cam_flash, ab);
	for (uint32_t i = 0; i < FW_FLASH_SIZE; i += 997)
		cam_flash.mem[i] = (uint8_t)(i >> 9);
	if (ab)
		cam_env("a", NULL, "0");
	uvcd_update_default_config(&cam_cfg);
	cam_cfg.flash = cam_flash.flash;
	cam_cfg.cmdline_path = cam_files[0];
	cam_cfg.meminfo_path = cam_files[1];
	cam_cfg.mounts_path = cam_files[2];
	cam_cfg.release_path = cam_files[3];
	cam_cfg.serial_path = cam_files[4];
	cam_cfg.data_root = cam_files[5];
	cam_cfg.run_dir = cam_files[6];
	cam_cfg.reboot = cam_reboot;
	cam_cfg.memory_reserve_kb = 4096;
	cam_rebooting = cam_absent = cam_rejects_trial = false;
	cam_power_on();
}

static void cam_power_off(void)
{
	if (cam) {
		uvcd_update_wait_idle(cam);
		uvcd_update_destroy(cam);
	}
	cam = NULL;
}

static void cam_free(void)
{
	cam_power_off();
	char cmd[300];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", cam_root);
	assert(system(cmd) == 0);
	free(cam_flash.mem);
	cam_flash.mem = NULL;
}

/* What the camera does after the reboot uvcdctl asked for: U-Boot runs
 * the trial, uvcd proves the camera works and confirms the slot -- or
 * U-Boot gives up on it and boots the confirmed slot again. */
static void cam_boot_after_reboot(void)
{
	cam_power_off();
	cam_rebooting = false;
	struct fw_layout_map map;
	struct fw_env env;
	assert(fw_layout_resolve(&cam_flash.flash, &map) == 0);
	assert(fw_env_load(&cam_flash.flash, &map, &env) == 0);
	const char *trial = fw_env_get(&env, "trial_slot");
	assert(trial);
	if (cam_rejects_trial) {
		cam_env(fw_env_get(&env, "active_slot"), NULL, "0");
		cam_set_boot(fw_env_get(&env, "active_slot"), "confirmed", "k1cam-old");
		cam_power_on();
		return;
	}
	char slot[4];
	snprintf(slot, sizeof(slot), "%s", trial);
	cam_set_boot(slot, "trial", cam_trial_build);
	cam_power_on();
	assert(uvcd_update_trial_wants_check(cam));
	uvcd_update_trial_health(cam, true, "test");
	uvcd_update_wait_idle(cam);
}

static int cam_query(struct uvc_xu_control_query *q)
{
	if (!cam) {
		errno = ENOENT; /* bmControls without 26-28 */
		return -1;
	}
	if (cam_rebooting) {
		errno = ENODEV;
		return -1;
	}
	int n;
	if (q->query == UVC_SET_CUR) {
		n = uvcd_update_setup(cam, UVC_SET_CUR, q->selector, false, q->size, NULL);
		if (n >= 0)
			uvcd_update_data(cam, q->selector, q->data, q->size);
	} else {
		n = uvcd_update_setup(cam, q->query, q->selector, true, q->size, q->data);
	}
	if (n < 0) {
		errno = EPIPE;
		return -1;
	}
	return 0;
}

int test_open(const char *path, int flags, ...)
{
	if (!strcmp(path, "/fake"))
		return FAKE_FD;
	if (!strncmp(path, "/dev/video", 10)) {
		if (strcmp(path, "/dev/video0") || cam_absent) {
			errno = ENOENT;
			return -1;
		}
		if (cam_rebooting)
			cam_boot_after_reboot();
		return VIDEO0_FD;
	}
	va_list ap;
	va_start(ap, flags);
	int mode = va_arg(ap, int);
	va_end(ap);
	return (int)syscall(SYS_openat, AT_FDCWD, path, flags, mode);
}

int test_ioctl(int fd, unsigned long request, ...)
{
	assert(fd == FAKE_FD || fd == VIDEO0_FD);
	va_list ap;
	va_start(ap, request);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	if (request == VIDIOC_QUERYCAP) {
		memset(arg, 0, sizeof(struct v4l2_capability));
		strcpy((char *)((struct v4l2_capability *)arg)->driver, "uvcvideo");
		return 0;
	}
	if (request == VIDIOC_QUERYCTRL) {
		struct v4l2_queryctrl *q = arg;
		q->minimum = -36000;
		q->maximum = 36000;
		q->step = 3600;
		return 0;
	}
	if (request == VIDIOC_S_CTRL || request == VIDIOC_G_CTRL) {
		((struct v4l2_control *)arg)->value = 0;
		return 0;
	}
	assert(request == UVCIOC_CTRL_QUERY);
	struct uvc_xu_control_query *q = arg;
	if (q->selector >= UVCD_FW_COMMAND_SELECTOR && q->selector <= UVCD_FW_STATUS_SELECTOR)
		return cam_query(q);
	if (cam_rebooting) {
		errno = ENODEV;
		return -1;
	}
	int v = 0;
	switch (q->query) {
	case UVC_GET_MIN:
		if (range_error) { errno = range_error; return -1; }
		v = -1;
		break;
	case UVC_GET_MAX: v = 51; break;
	case UVC_GET_DEF: v = -1; break;
	case UVC_GET_LEN: v = 2; break;
	case UVC_SET_CUR: return 0; /* USB ACK, even if the daemon rejects it */
	case UVC_GET_CUR:
		if (get_error) { errno = EIO; return -1; }
		v = current_qp;
		break;
	default: assert(false);
	}
	q->data[0] = (uint8_t)v;
	q->data[1] = (uint8_t)((uint16_t)v >> 8);
	return 0;
}

static void check(const char **reply_chunks, int expected)
{
	chunks = reply_chunks;
	chunk_index = 0;
	assert(socket_command("set brightness 128") == expected);
}

static int capture_command(int argc, char **argv, FILE *stream, char *out, size_t size)
{
	FILE *capture = tmpfile();
	assert(capture);
	fflush(stream);
	int target = fileno(stream);
	int saved = dup(target);
	assert(saved >= 0 && dup2(fileno(capture), target) >= 0);
	int rc = uvcdctl_program_main(argc, argv);
	fflush(stream);
	assert(dup2(saved, target) >= 0);
	FILE *saved_stream = fdopen(saved, "w");
	assert(saved_stream);
	fclose(saved_stream);
	rewind(capture);
	size_t n = fread(out, 1, size - 1, capture);
	assert(feof(capture));
	out[n] = '\0';
	fclose(capture);
	return rc;
}

static int capture_list(const char *command, const char *key, char *out, size_t size)
{
	char *args[] = {"uvcdctl", "--device", "/fake", (char *)command, (char *)key, NULL};
	char *local_args[] = {"uvcdctl", (char *)command, (char *)key, NULL};
	return local_socket ? capture_command(key ? 3 : 2, local_args, stdout, out, size) :
		capture_command(key ? 5 : 4, args, stdout, out, size);
}

static void check_help(void)
{
	char out[4096];
	/* Any attempted device open would fail the /fake assertion in test_open. */
	char *args[] = {"uvcdctl", "--device", "/must-not-open", "get", NULL, NULL, NULL};
	assert(capture_command(4, args, stderr, out, sizeof(out)) == 2);
	assert(strstr(out, "usage: uvcdctl [--device PATH] get <key>\n"));
	assert(!strstr(out, "commands:") && !strstr(out, "no k1cam camera"));
	args[3] = "set";
	assert(capture_command(4, args, stderr, out, sizeof(out)) == 2);
	assert(strstr(out, "usage: uvcdctl [--device PATH] set <key> <value>\n"));
	assert(!strstr(out, "commands:"));
	args[4] = "brightness";
	assert(capture_command(5, args, stderr, out, sizeof(out)) == 2);
	assert(strstr(out, "set <key> <value>"));
	args[3] = "get";
	args[5] = "extra";
	assert(capture_command(6, args, stderr, out, sizeof(out)) == 2);
	assert(strstr(out, "get <key>") && !strstr(out, "commands:"));

	args[3] = "help";
	args[4] = NULL;
	assert(capture_command(4, args, stdout, out, sizeof(out)) == 0);
	assert(strstr(out, "commands:") && strstr(out, "help [command]"));
	const char *topics[] = {"get", "set", "list", "describe", "reset", "keyframe",
				"firmware", "help"};
	for (size_t i = 0; i < sizeof(topics) / sizeof(topics[0]); i++) {
		char expected[96], help[4096];
		args[3] = "help";
		args[4] = (char *)topics[i];
		assert(capture_command(5, args, stdout, help, sizeof(help)) == 0);
		snprintf(expected, sizeof(expected), "usage: uvcdctl [--device PATH] %s", topics[i]);
		assert(strstr(help, expected) && strstr(help, "Example"));
		assert(!strstr(help, "commands:"));
		args[3] = (char *)topics[i];
		args[4] = "--help";
		assert(capture_command(5, args, stdout, out, sizeof(out)) == 0);
		assert(!strcmp(out, help));
		args[4] = "-h";
		assert(capture_command(5, args, stdout, out, sizeof(out)) == 0);
		assert(!strcmp(out, help));
	}
	args[3] = "help";
	args[4] = "unknown";
	assert(capture_command(5, args, stderr, out, sizeof(out)) == 2);
	assert(strstr(out, "unknown command 'unknown'") && strstr(out, "help [command]"));
	args[4] = "get";
	assert(capture_command(6, args, stderr, out, sizeof(out)) == 2);
	assert(strstr(out, "help [command]") && !strstr(out, "commands:"));
}

static void check_listings(void)
{
	char brief[16384], detailed[16384], numeric[16384];
	assert(capture_list("list", "h264-max-qp", brief, sizeof(brief)) == 0);
	assert(capture_list("describe", "h264-max-qp", detailed, sizeof(detailed)) == 0);
	assert(capture_list("describe", "44", numeric, sizeof(numeric)) == 0);
	assert(!strcmp(detailed, numeric));
	assert(strstr(brief, "h264-max-qp") && !strstr(brief, "brightness"));
	assert(strstr(brief, "DEFAULT") && !strstr(brief, "DESCRIPTION"));
	assert(!strstr(brief, find_ctrl("44")->help));
	assert(strstr(detailed, "DESCRIPTION") && strstr(detailed, find_ctrl("44")->help));
	assert(!strchr(detailed, '\033')); /* redirected output has no colour */
	/* The detailed row is the brief row with the help text appended. */
	const char *brief_row = strchr(brief, '\n') + 1;
	const char *detailed_row = strchr(detailed, '\n') + 1;
	assert(!strncmp(brief_row, detailed_row, strcspn(brief_row, "\n")));
	assert(capture_list("list", NULL, brief, sizeof(brief)) == 0);
	assert(capture_list("describe", NULL, detailed, sizeof(detailed)) == 0);
	for (size_t i = 0; i < CTRL_COUNT; i++) {
		assert(strstr(brief, ctrl_defs[i].name));
		assert(strstr(detailed, ctrl_defs[i].help));
	}
	range_error = ENOENT;
	assert(capture_list("describe", "44", detailed, sizeof(detailed)) == 1);
	assert(strstr(detailed, "unsupported") && strstr(detailed, find_ctrl("44")->help));
	range_error = 0;
	get_error = 1;
	assert(capture_list("describe", "44", detailed, sizeof(detailed)) == 1);
	assert(strstr(detailed, "error") && strstr(detailed, find_ctrl("44")->help));
	get_error = 0;
	assert(capture_list("list", "unknown", brief, sizeof(brief)) == 2);
	assert(!*brief);
	char *extra[] = {"uvcdctl", "list", "44", "extra", NULL};
	assert(uvcdctl_program_main(4, extra) == 2);
	extra[1] = "describe";
	assert(uvcdctl_program_main(4, extra) == 2);

	int value, min, max, dv;
	const char *info[] = {"20 -1 ", "51 -1\nok\n", NULL};
	chunks = info;
	chunk_index = 0;
	assert(list_read(-1, find_ctrl("44"), &value, &min, &max, &dv) == 0);
	assert(value == 20 && min == -1 && max == 51 && dv == -1);
	assert(!strcmp(last_request, "info h264-max-qp\n"));
	/* Both transports pass their data through exactly the same formatter. */
	assert(capture_list("describe", "44", detailed, sizeof(detailed)) == 0);
	assert(capture_list("list", "44", brief, sizeof(brief)) == 0);
	local_socket = true;
	chunks = info;
	chunk_index = 0;
	assert(capture_list("describe", "44", numeric, sizeof(numeric)) == 0);
	assert(!strcmp(detailed, numeric));
	chunk_index = 0;
	assert(capture_list("list", "44", numeric, sizeof(numeric)) == 0);
	assert(!strcmp(brief, numeric));
	local_socket = false;
	const char *ae[] = {"8 1 8 8\nok\n", NULL};
	chunks = ae;
	chunk_index = 0;
	assert(list_read(-1, find_ctrl("auto-exposure"), &value, &min, &max, &dv) == 0);
	assert(value == 3 && min == 1 && max == 3 && dv == 3);
	const char *bad_info[] = {"err unknown control\n", NULL};
	chunks = bad_info;
	chunk_index = 0;
	assert(list_read(-1, find_ctrl("44"), &value, &min, &max, &dv) == -1);
	const char *malformed[] = {"20 -1\nok\n", NULL};
	chunks = malformed;
	chunk_index = 0;
	assert(list_read(-1, find_ctrl("44"), &value, &min, &max, &dv) == -1);
}

static void put_be32_test(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (24 - 8 * i));
}

/* A valid bundle file (as fw_test.c builds them) for build id. */
static void write_bundle(const char *path, const char *build, uint8_t salt)
{
	uint32_t klen = 64 + 30000, rlen = 0x10000, used = rlen - 500;
	uint8_t *k = calloc(1, klen), *r = calloc(1, rlen);
	for (uint32_t i = 64; i < klen; i++)
		k[i] = (uint8_t)(i * 5 + salt);
	put_be32_test(k, 0x27051956);
	put_be32_test(k + 12, klen - 64);
	put_be32_test(k + 24, fw_crc32(0, k + 64, klen - 64));
	k[28] = 5;
	k[29] = 5;
	k[30] = 2;
	put_be32_test(k + 4, fw_crc32(0, k, 64));
	for (uint32_t i = 96; i < used; i++)
		r[i] = (uint8_t)(i * 3 + salt);
	memcpy(r, "hsqs", 4);
	r[14] = 4;
	r[22] = 18;
	r[28] = 4;
	r[40] = (uint8_t)used;
	r[41] = (uint8_t)(used >> 8);
	size_t len = fw_bundle_build(NULL, FW_BOARD_ID, FW_LAYOUT_AB_ID, build, 1, k, klen, r, rlen);
	uint8_t *b = malloc(len);
	fw_bundle_build(b, FW_BOARD_ID, FW_LAYOUT_AB_ID, build, 1, k, klen, r, rlen);
	FILE *f = fopen(path, "wb");
	assert(f && fwrite(b, 1, len, f) == len);
	fclose(f);
	free(b);
	free(k);
	free(r);
}

/* Release information after the bundle, as post-image.sh packs k1cam.upd. */
static void append_info(const char *path)
{
	static const char text[] = "NAME=k1cam\nBUILD_ID=\"k1cam+new, 2026-01-01 00:00:00 +0000\"\n";
	uint8_t head[FW_BUNDLE_MANIFEST_SIZE], info[FW_INFO_HEADER_SIZE + sizeof(text)];
	FILE *f = fopen(path, "rb");
	assert(f && fread(head, 1, sizeof(head), f) == sizeof(head));
	fclose(f);
	size_t n = fw_bundle_info_build(info, head, text, strlen(text));
	f = fopen(path, "ab");
	assert(n && f && fwrite(info, 1, n, f) == n);
	fclose(f);
}

static int fw_run(const char *a, const char *b, const char *c, const char *d, FILE *stream,
		  char *out, size_t size)
{
	char *args[] = {"uvcdctl", "--device", "/fake", "firmware", (char *)a, (char *)b,
			(char *)c, (char *)d, NULL};
	int argc = 5 + (b != NULL) + (c != NULL) + (d != NULL);
	return capture_command(argc, args, stream, out, size);
}

static bool env_says(const char *name, const char *value)
{
	struct fw_layout_map map;
	struct fw_env env;
	assert(fw_layout_resolve(&cam_flash.flash, &map) == 0);
	assert(fw_env_load(&cam_flash.flash, &map, &env) == 0);
	const char *v = fw_env_get(&env, name);
	return value ? v && !strcmp(v, value) : !v;
}

static bool file_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

static void check_firmware(void)
{
	char out[16384], file[400], part[420], bundle[400];

	/* Usage errors never reach a camera. */
	char *bad[] = {"uvcdctl", "--device", "/must-not-open", "firmware", "upgrade", NULL};
	assert(uvcdctl_program_main(5, bad) == 2);
	bad[4] = "frobnicate";
	assert(uvcdctl_program_main(5, bad) == 2);
	assert(fw_run("dump", "--no-reboot", "x", NULL, stderr, out, sizeof(out)) == 2);
	assert(fw_run("info", "extra", NULL, NULL, stderr, out, sizeof(out)) == 2);

	/* A camera without the protocol says so. */
	assert(fw_run("info", NULL, NULL, NULL, stderr, out, sizeof(out)) == 1);
	assert(strstr(out, "no firmware protocol"));

	cam_init(true);
	assert(fw_run("info", NULL, NULL, NULL, stdout, out, sizeof(out)) == 0);
	assert(strstr(out, "build=k1cam-old\n") && strstr(out, "boot=confirmed\n") &&
	       strstr(out, "features=info,dump,ab,upgrade\n") && strstr(out, "serial=777\n"));
	assert(fw_run("status", NULL, NULL, NULL, stdout, out, sizeof(out)) == 0);
	assert(strstr(out, "phase: idle (0)") && strstr(out, "slots: running a, confirmed a, trial none"));

	/* Dump: complete and verified, or nothing but a named partial file. */
	snprintf(file, sizeof(file), "%s/backup.bin", cam_root);
	snprintf(part, sizeof(part), "%s.part", file);
	assert(fw_run("dump", file, NULL, NULL, stdout, out, sizeof(out)) == 0);
	assert(strstr(out, "8388608 bytes") && strstr(out, "snapshot"));
	FILE *f = fopen(file, "rb");
	uint8_t *copy = malloc(FW_FLASH_SIZE);
	assert(f && fread(copy, 1, FW_FLASH_SIZE, f) == FW_FLASH_SIZE && fgetc(f) == EOF);
	fclose(f);
	assert(!memcmp(copy, cam_flash.mem, FW_FLASH_SIZE) && !file_exists(part));
	assert(fw_run("dump", file, NULL, NULL, stderr, out, sizeof(out)) == 1);
	assert(strstr(out, "exists"));
	unlink(file);
	cam_memory(9000);
	assert(fw_run("dump", file, NULL, NULL, stderr, out, sizeof(out)) == 1);
	assert(strstr(out, "--live") && !file_exists(file) && !file_exists(part));
	assert(fw_run("dump", "--live", file, NULL, stdout, out, sizeof(out)) == 0);
	assert(strstr(out, "live (not a consistent snapshot)"));
	f = fopen(file, "rb");
	assert(f && fread(copy, 1, FW_FLASH_SIZE, f) == FW_FLASH_SIZE);
	fclose(f);
	assert(!memcmp(copy, cam_flash.mem, FW_FLASH_SIZE));
	unlink(file);
	/* The camera fails mid-dump: the partial file stays, marked. */
	cam_flash.fail_read_at = 1000; /* of 2048 4 KiB block reads */
	assert(fw_run("dump", "--live", file, NULL, stderr, out, sizeof(out)) == 1);
	assert(strstr(out, "partial dump left in") && file_exists(part) &&
	       !file_exists(file));
	fake_flash_faults_off(&cam_flash);
	assert(fw_run("dump", "--live", file, NULL, stderr, out, sizeof(out)) == 1);
	assert(strstr(out, "earlier dump")); /* never overwrites a partial */
	unlink(part);
	cam_memory(40000);
	free(copy);

	/* Upgrade: staged, rebooted, confirmed by the camera. The file carries
	 * release information, which the camera (refusing trailing bytes) must
	 * not get. */
	snprintf(bundle, sizeof(bundle), "%s/uvcdctl-test-%d.upd",
		 getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
	write_bundle(bundle, "k1cam-new", 1);
	append_info(bundle);
	snprintf(cam_trial_build, sizeof(cam_trial_build), "k1cam-new");
	assert(fw_run("upgrade", bundle, NULL, NULL, stdout, out, sizeof(out)) == 0);
	assert(strstr(out, "staged: k1cam-new is in slot b") &&
	       strstr(out, "confirmed: /dev/video0 runs k1cam-new in slot b"));
	assert(env_says("active_slot", "b") && env_says("trial_slot", NULL));
	/* Same build again: refused. */
	assert(fw_run("upgrade", bundle, NULL, NULL, stderr, out, sizeof(out)) == 1);
	assert(strstr(out, "already runs k1cam-new"));

	/* A trial that U-Boot rolls back is reported as such. */
	cam_free();
	cam_init(true);
	cam_rejects_trial = true;
	assert(fw_run("upgrade", bundle, NULL, NULL, stdout, out, sizeof(out)) == 3);
	assert(strstr(out, "rolled back"));
	assert(env_says("active_slot", "a") && env_says("trial_slot", NULL));

	/* Staged only, then withdrawn. */
	cam_rejects_trial = false;
	assert(fw_run("upgrade", "--no-reboot", bundle, NULL, stdout, out, sizeof(out)) == 0);
	assert(strstr(out, "not confirmed yet") && strstr(out, "firmware reboot"));
	assert(env_says("trial_slot", "b") && !cam_rebooting);
	assert(fw_run("upgrade", bundle, NULL, NULL, stderr, out, sizeof(out)) == 1);
	assert(strstr(out, "trial of slot b is pending"));
	assert(fw_run("cancel", NULL, NULL, NULL, stdout, out, sizeof(out)) == 0);
	assert(strstr(out, "cancelled; no trial is scheduled") && env_says("trial_slot", NULL));
	assert(fw_run("cancel", NULL, NULL, NULL, stdout, out, sizeof(out)) == 0);
	assert(strstr(out, "nothing to cancel"));
	assert(fw_run("reboot", NULL, NULL, NULL, stderr, out, sizeof(out)) == 1);

	/* Staged, rebooted, but the camera never comes back. */
	assert(fw_run("upgrade", "--no-reboot", bundle, NULL, stdout, out, sizeof(out)) == 0);
	cam_absent = true;
	assert(fw_run("reboot", NULL, NULL, NULL, stdout, out, sizeof(out)) == 0);
	assert(cam_rebooting);
	cam_absent = false;
	cam_rebooting = false;
	cam_env("a", NULL, "0");
	cam_free();
	cam_init(true);
	cam_absent = true;
	snprintf(cam_trial_build, sizeof(cam_trial_build), "k1cam-new");
	assert(fw_run("upgrade", "--wait-timeout", "2", bundle, stdout, out, sizeof(out)) == 4);
	assert(strstr(out, "did not come back"));
	cam_absent = false;

	/* Refused before the camera is contacted, or by what it reports. */
	cam_free();
	cam_init(true);
	f = fopen(bundle, "r+b");
	fseek(f, 1000, SEEK_SET);
	fputc(0x55, f);
	fclose(f);
	assert(fw_run("upgrade", bundle, NULL, NULL, stderr, out, sizeof(out)) == 1);
	assert(strstr(out, "digest mismatch"));
	write_bundle(bundle, "k1cam-new", 1);
	f = fopen(bundle, "ab");
	fputc('x', f);
	fclose(f);
	assert(fw_run("upgrade", bundle, NULL, NULL, stderr, out, sizeof(out)) == 1);
	assert(strstr(out, "not release information"));
	cam_free();
	cam_init(false);
	write_bundle(bundle, "k1cam-new", 1);
	assert(fw_run("upgrade", bundle, NULL, NULL, stderr, out, sizeof(out)) == 1);
	assert(strstr(out, "install the A/B image"));
	cam_free();
	unlink(bundle);
}

int main(void)
{
	const char *fragmented[] = {"128\n", "o", "k\n", NULL};
	const char *failed[] = {"err ", "apply failed\n", NULL};
	const char *incomplete[] = {"128\no", NULL};
	const char *embedded[] = {"notok\n", NULL};
	const char *trailing_error[] = {"ok\n", "err apply failed\n", NULL};
	check(fragmented, 0);
	check(failed, 1);
	check(incomplete, 1);
	check(embedded, 1);
	check(trailing_error, 1);
	char *args[] = {"uvcdctl", "--device", "/fake", "set", "h264-max-qp", "20", NULL};
	assert(uvcdctl_program_main(6, args) == 1); /* readback differs from requested */
	current_qp = 20;
	assert(uvcdctl_program_main(6, args) == 0);
	get_error = 1;
	assert(uvcdctl_program_main(6, args) == 1); /* no optimistic fallback */
	get_error = 0;
	args[4] = "pan";
	args[5] = "200";
	assert(uvcdctl_program_main(6, args) == 0); /* host step alignment to 0 */
	check_listings();
	check_help();
	check_firmware();
	puts("uvcdctl regression tests passed");
	return 0;
}
