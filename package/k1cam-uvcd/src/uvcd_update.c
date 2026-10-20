/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * uvcd_update.c -- device information, flash dumps and A/B updates over the
 * firmware XU protocol (uvcd_firmware.h), and confirming a trial boot.
 *
 * Safety rules this file keeps:
 * - Every flash address comes from fw_layout.h, matched against the
 *   kernel's partitions by name; the host never names an address or device.
 * - An update only writes the slot that is not running and not confirmed,
 *   and only from the confirmed slot with no trial pending.
 * - The new slot is scheduled (one environment write) only after both of
 *   its partitions were written and read back intact; any failure or abort
 *   before that leaves the boot state as it was.
 */
#include "uvcd_update.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/usb/video.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "fw_bundle.h"
#include "fw_crc.h"
#include "fw_env.h"
#include "fw_sha256.h"
#include "uvcd.h"
#include "uvcd_firmware.h"

#define POLL_BUSY_MS 200
#define WINDOW UVCD_FW_DATA_MAX
#define IO_CHUNK 0x8000u
#define LIVE_BLOCK 4096u
#define SNAPSHOT_TRIES 3
#define INFO_MAX 1536
#define MANIFEST_MAX 256
/* Settings a new slot starts with, copied from the running slot's upper
 * layer: only uvcd's controls, whose format every release reads. */
static const char *const carry_over[] = {"etc/uvcd.conf", "etc/uvcd.conf.good"};

enum job {
	JOB_NONE,
	JOB_SNAPSHOT,
	JOB_INSTALL,
	JOB_PROMOTE,
	JOB_UNSCHEDULE,
	JOB_REBOOT,
};

enum trial_state {
	TRIAL_UNCHECKED,
	TRIAL_PROMOTING,
	TRIAL_CONFIRMED,
	TRIAL_FAILED,
};

/* What the flash, the environment and the kernel command line say. */
struct boot_state {
	struct fw_layout_map map;
	bool env_ok;
	int running;   /* from k1cam.slot */
	char mode[16]; /* k1cam.boot: confirmed, trial, fallback */
	int active, trial;
	int upgrade_available, bootcount, bootlimit;
	uint32_t boot_compat;
	uint8_t boot; /* UVCD_FW_BOOT_*, what all of that adds up to */
};

struct release {
	char board[FW_BUNDLE_BOARD_LEN];
	char layout[FW_BUNDLE_LAYOUT_LEN];
	char build[FW_BUNDLE_BUILD_LEN];
	char kernel[48];
	uint32_t min_boot_compat;
};

struct uvcd_update {
	struct uvcd_update_config cfg;
	struct release release;

	pthread_mutex_t lock;
	pthread_cond_t cond;
	pthread_t worker;
	bool worker_started;

	/* Under lock: the operation, as STATUS shows it, and the worker. */
	bool stopping;
	enum job job; /* queued or running; NONE when the worker is free */
	bool cancel;
	uint8_t cancel_error; /* reported for a cancelled job; 0: ABORTED */
	bool committing;      /* past the point an install can be cancelled */
	uint8_t operation, phase, error;
	uint32_t detail, session, done, total;
	uint16_t poll_ms;
	int64_t activity_ms;
	struct boot_state boot;
	bool persist_paused;
	enum trial_state trial;

	/* Main thread only. */
	bool have_last;
	uint32_t last_seq;
	uint8_t last_selector, last_opcode, last_result;
	uint32_t last_detail;
	uint8_t last_frame[UVCD_FW_FRAME_SIZE];
	uint8_t window[UVCD_FW_FRAME_SIZE];
	bool window_valid;
	char info[INFO_MAX];
	size_t info_len;
	bool info_valid;
	uint32_t last_session;

	/* The upload: the main thread fills it while RECEIVING, the worker
	 * owns it from FINISH_UPGRADE on. */
	uint8_t *bundle;
	uint32_t received;
	uint8_t bundle_digest[FW_SHA256_SIZE];

	/* The dump: the worker fills snapshot while READING; the main thread
	 * reads it once READY. Without a snapshot, live_h reads the flash. */
	uint8_t *snapshot;
	int live_h;
	uint8_t live_cache[LIVE_BLOCK];
	uint32_t live_cache_off;
	bool live_cache_valid;
	struct fw_layout_map dump_map;
	char manifest[MANIFEST_MAX];
	size_t manifest_len;
};

static int64_t now_ms(void)
{
	return uvcd_monotonic_ms();
}

/* --------------------------------------------------------------------------
 * Small file helpers
 */
static int read_text(const char *path, char *buf, size_t size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	size_t used = 0;
	while (used < size - 1) {
		ssize_t n = read(fd, buf + used, size - 1 - used);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		used += (size_t)n;
	}
	close(fd);
	buf[used] = '\0';
	return 0;
}

/* The value of key=value in a whitespace-separated list (the kernel
 * command line) or a list of lines. */
static bool find_value(const char *text, const char *key, char *out, size_t size)
{
	size_t key_len = strlen(key);
	for (const char *p = text; *p;) {
		while (*p == ' ' || *p == '\n' || *p == '\t')
			p++;
		size_t len = strcspn(p, " \n\t");
		if (len > key_len && !strncmp(p, key, key_len) && p[key_len] == '=') {
			size_t n = len - key_len - 1;
			if (n >= size)
				n = size - 1;
			memcpy(out, p + key_len + 1, n);
			out[n] = '\0';
			return true;
		}
		p += len;
	}
	return false;
}

static int slot_from(const char *s)
{
	if (!s)
		return FW_SLOT_NONE;
	if (!strcmp(s, "a"))
		return FW_SLOT_A;
	if (!strcmp(s, "b"))
		return FW_SLOT_B;
	return FW_SLOT_NONE;
}

static const char *slot_name(int slot)
{
	return slot == FW_SLOT_A ? "a" : slot == FW_SLOT_B ? "b" : "";
}

static int env_int(const struct fw_env *env, const char *name, int fallback)
{
	const char *v = fw_env_get(env, name);
	if (!v || !*v)
		return fallback;
	char *end;
	long n = strtol(v, &end, 10);
	return *end || n < 0 || n > INT_MAX ? -1 : (int)n;
}

/* snprintf into a PATH_MAX buffer, failing (ENAMETOOLONG) rather than
 * truncating. */
static int pathf(char *out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int pathf(char *out, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(out, PATH_MAX, fmt, ap);
	va_end(ap);
	if (n < 0 || n >= PATH_MAX) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

static int make_dirs(const char *path)
{
	char tmp[PATH_MAX];
	if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	for (char *p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	return mkdir(tmp, 0755) != 0 && errno != EEXIST ? -1 : 0;
}

static void write_run_file(struct uvcd_update *u, const char *name, const char *text)
{
	char path[PATH_MAX];
	if (make_dirs(u->cfg.run_dir) != 0) {
		LOGW("firmware: %s: %s", u->cfg.run_dir, strerror(errno));
		return;
	}
	if (pathf(path, "%s/%s", u->cfg.run_dir, name) != 0)
		return;
	FILE *f = fopen(path, "w");
	if (!f || fprintf(f, "%s\n", text) < 0 || fclose(f) != 0)
		LOGW("firmware: %s: %s", path, strerror(errno));
}

static bool run_file_exists(struct uvcd_update *u, const char *name)
{
	char path[PATH_MAX];
	return pathf(path, "%s/%s", u->cfg.run_dir, name) == 0 && access(path, F_OK) == 0;
}

/* --------------------------------------------------------------------------
 * Boot state
 */
static void load_release(struct uvcd_update *u)
{
	char text[1024];
	struct release *r = &u->release;
	char num[16];

	memset(r, 0, sizeof(*r));
	if (read_text(u->cfg.release_path, text, sizeof(text)) != 0) {
		LOGW("firmware: %s: %s", u->cfg.release_path, strerror(errno));
		text[0] = '\0';
	}
	if (!find_value(text, "board", r->board, sizeof(r->board)))
		snprintf(r->board, sizeof(r->board), "%s", FW_BOARD_ID);
	find_value(text, "layout", r->layout, sizeof(r->layout));
	find_value(text, "build", r->build, sizeof(r->build));
	find_value(text, "kernel", r->kernel, sizeof(r->kernel));
	r->min_boot_compat = find_value(text, "min_boot_compat", num, sizeof(num))
				     ? (uint32_t)strtoul(num, NULL, 10) : 0;
}

/* Reads flash (the environment): call only while no job writes it. */
static void boot_read(struct uvcd_update *u, struct boot_state *bs)
{
	char cmdline[4096], value[16];

	memset(bs, 0, sizeof(*bs));
	bs->running = bs->active = bs->trial = FW_SLOT_NONE;
	bs->bootcount = bs->bootlimit = -1;
	if (fw_layout_resolve(&u->cfg.flash, &bs->map) != 0)
		bs->map.kind = FW_LAYOUT_UNKNOWN;
	if (read_text(u->cfg.cmdline_path, cmdline, sizeof(cmdline)) != 0)
		cmdline[0] = '\0';
	if (find_value(cmdline, "k1cam.slot", value, sizeof(value)))
		bs->running = slot_from(value);
	find_value(cmdline, "k1cam.boot", bs->mode, sizeof(bs->mode));

	if (bs->map.kind == FW_LAYOUT_SINGLE) {
		bs->boot = UVCD_FW_BOOT_SINGLE;
		return;
	}
	if (bs->map.kind != FW_LAYOUT_AB)
		return;

	struct fw_env *env = malloc(sizeof(*env));
	if (env && fw_env_load(&u->cfg.flash, &bs->map, env) == 0) {
		bs->env_ok = true;
		bs->active = slot_from(fw_env_get(env, "active_slot"));
		bs->trial = slot_from(fw_env_get(env, "trial_slot"));
		bs->upgrade_available = env_int(env, "upgrade_available", 0);
		bs->bootcount = env_int(env, "bootcount", 0);
		bs->bootlimit = env_int(env, "bootlimit", 0);
		int compat = env_int(env, "k1_boot_compat", 0);
		bs->boot_compat = compat > 0 ? (uint32_t)compat : 0;
	}
	free(env);
	if (!bs->env_ok || bs->running == FW_SLOT_NONE || bs->active == FW_SLOT_NONE)
		return;
	if (bs->upgrade_available == 1 && bs->trial == bs->running && bs->trial != bs->active)
		bs->boot = UVCD_FW_BOOT_TRIAL;
	else if (bs->running == bs->active)
		bs->boot = UVCD_FW_BOOT_CONFIRMED;
	else
		bs->boot = UVCD_FW_BOOT_FALLBACK;
}

static bool trial_pending(const struct boot_state *bs)
{
	return bs->env_ok && (bs->upgrade_available != 0 || bs->trial != FW_SLOT_NONE);
}

static bool upgrade_allowed(const struct boot_state *bs)
{
	return bs->map.kind == FW_LAYOUT_AB && bs->boot == UVCD_FW_BOOT_CONFIRMED &&
	       !trial_pending(bs);
}

static uint32_t features_of(const struct boot_state *bs)
{
	uint32_t f = UVCD_FW_FEATURE_INFO;
	if (bs->map.all >= 0)
		f |= UVCD_FW_FEATURE_DUMP;
	if (bs->map.kind == FW_LAYOUT_AB)
		f |= UVCD_FW_FEATURE_AB_SLOTS;
	if (upgrade_allowed(bs))
		f |= UVCD_FW_FEATURE_UPGRADE;
	return f;
}

static void boot_refresh(struct uvcd_update *u)
{
	struct boot_state *bs = malloc(sizeof(*bs));
	if (!bs)
		return;
	boot_read(u, bs);
	pthread_mutex_lock(&u->lock);
	u->boot = *bs;
	pthread_mutex_unlock(&u->lock);
	free(bs);
}

static const char *boot_name(uint8_t boot)
{
	switch (boot) {
	case UVCD_FW_BOOT_SINGLE:
		return "single";
	case UVCD_FW_BOOT_CONFIRMED:
		return "confirmed";
	case UVCD_FW_BOOT_TRIAL:
		return "trial";
	case UVCD_FW_BOOT_FALLBACK:
		return "fallback";
	default:
		return "unknown";
	}
}

/* Available memory in KiB: MemAvailable, or on kernels without it (this
 * one is 3.10) free + buffers + page cache - shared memory. */
static long mem_available_kb(struct uvcd_update *u)
{
	char text[2048];
	if (read_text(u->cfg.meminfo_path, text, sizeof(text)) != 0)
		return -1;
	long avail = -1, free_kb = 0, buffers = 0, cached = 0, shmem = 0;
	for (char *line = text; line && *line;) {
		char *next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		long kb;
		if (sscanf(line, "MemAvailable: %ld", &kb) == 1)
			avail = kb;
		else if (sscanf(line, "MemFree: %ld", &kb) == 1)
			free_kb = kb;
		else if (sscanf(line, "Buffers: %ld", &kb) == 1)
			buffers = kb;
		else if (sscanf(line, "Cached: %ld", &kb) == 1)
			cached = kb;
		else if (sscanf(line, "Shmem: %ld", &kb) == 1)
			shmem = kb;
		line = next;
	}
	return avail >= 0 ? avail : free_kb + buffers + cached - shmem;
}

static bool memory_for(struct uvcd_update *u, uint32_t bytes)
{
	long avail = mem_available_kb(u);
	return avail >= 0 && (uint64_t)avail >= bytes / 1024u + u->cfg.memory_reserve_kb;
}

/* --------------------------------------------------------------------------
 * Operation state (worker and main thread)
 */
static void set_phase(struct uvcd_update *u, uint8_t phase, uint32_t done, uint32_t total)
{
	pthread_mutex_lock(&u->lock);
	u->phase = phase;
	u->done = done;
	u->total = total;
	pthread_mutex_unlock(&u->lock);
}

static void set_done(struct uvcd_update *u, uint32_t done)
{
	pthread_mutex_lock(&u->lock);
	u->done = done;
	pthread_mutex_unlock(&u->lock);
}

static void fail(struct uvcd_update *u, uint8_t error, uint32_t detail)
{
	pthread_mutex_lock(&u->lock);
	u->phase = UVCD_FW_PHASE_ERROR;
	u->error = error;
	u->detail = detail;
	u->poll_ms = 0;
	pthread_mutex_unlock(&u->lock);
}

/* The worker checks this between blocks. On true the job stops and reports
 * the cancellation. */
static bool cancelled(struct uvcd_update *u)
{
	pthread_mutex_lock(&u->lock);
	bool c = u->cancel || u->stopping;
	uint8_t why = u->cancel_error;
	if (c) {
		u->phase = why ? UVCD_FW_PHASE_ERROR : UVCD_FW_PHASE_ABORTED;
		u->error = why;
		u->detail = 0;
		u->poll_ms = 0;
	}
	pthread_mutex_unlock(&u->lock);
	return c;
}

static uint32_t new_session(struct uvcd_update *u)
{
	uint32_t id = 0;
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		if (read(fd, &id, sizeof(id)) != sizeof(id))
			id = 0;
		close(fd);
	}
	id ^= (uint32_t)now_ms() ^ (uint32_t)getpid() << 16;
	if (!id || id == u->last_session)
		id = u->last_session + 1 ? u->last_session + 1 : 1;
	u->last_session = id;
	return id;
}

/* --------------------------------------------------------------------------
 * Dump snapshot (worker)
 */
static void default_quiesce(bool pause)
{
	/* jffs2 writes in the writer's own context, and uvcd pauses its own;
	 * what is left is the background garbage collector, which lets
	 * SIGSTOP/SIGCONT through (fs/jffs2/background.c). */
	DIR *d = opendir("/proc");
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		char path[PATH_MAX], comm[32];
		if (e->d_name[0] < '1' || e->d_name[0] > '9' ||
		    pathf(path, "/proc/%s/comm", e->d_name) != 0)
			continue;
		if (read_text(path, comm, sizeof(comm)) == 0 && !strncmp(comm, "jffs2_gcd_mtd", 13))
			kill((pid_t)atoi(e->d_name), pause ? SIGSTOP : SIGCONT);
	}
	closedir(d);
	if (pause)
		sync();
}

static void job_snapshot(struct uvcd_update *u)
{
	const uint32_t size = FW_FLASH_SIZE;
	uint8_t *check = malloc(IO_CHUNK);
	bool verified = false;
	int h = -1, err = 0;

	pthread_mutex_lock(&u->lock);
	u->persist_paused = true;
	pthread_mutex_unlock(&u->lock);
	set_phase(u, UVCD_FW_PHASE_READING, 0, 2 * size);
	u->cfg.quiesce(true);

	h = fw_layout_open(&u->cfg.flash, &u->dump_map, FW_PART_ALL, false, NULL);
	if (h < 0 || !check) {
		err = h < 0 ? -h : ENOMEM;
		goto out;
	}
	/* Read it all, then again: identical reads mean nothing was written
	 * while it was copied. */
	for (int attempt = 0; attempt < SNAPSHOT_TRIES && !verified; attempt++) {
		bool same = true;
		for (uint32_t off = 0; off < size; off += IO_CHUNK) {
			if (cancelled(u))
				goto out;
			if (fw_flash_read_all(&u->cfg.flash, h, u->snapshot + off, IO_CHUNK, off) != 0) {
				err = errno;
				goto out;
			}
			set_done(u, off + IO_CHUNK);
		}
		for (uint32_t off = 0; off < size && same; off += IO_CHUNK) {
			if (cancelled(u))
				goto out;
			if (fw_flash_read_all(&u->cfg.flash, h, check, IO_CHUNK, off) != 0) {
				err = errno;
				goto out;
			}
			same = !memcmp(check, u->snapshot + off, IO_CHUNK);
			set_done(u, size + off + IO_CHUNK);
		}
		verified = same;
		if (!same)
			LOGW("firmware: flash changed while it was copied (try %d)", attempt + 1);
	}

out:
	if (h >= 0)
		u->cfg.flash.ops->close(&u->cfg.flash, h);
	u->cfg.quiesce(false);
	free(check);
	pthread_mutex_lock(&u->lock);
	u->persist_paused = false;
	bool stopped = u->phase == UVCD_FW_PHASE_ABORTED || u->phase == UVCD_FW_PHASE_ERROR;
	pthread_mutex_unlock(&u->lock);
	if (err || stopped) {
		if (err) {
			LOGE("firmware: dump snapshot failed: %s", strerror(err));
			fail(u, UVCD_FW_ERROR_MTD, (uint32_t)err);
		}
		free(u->snapshot);
		u->snapshot = NULL;
		return;
	}

	uint8_t digest[FW_SHA256_SIZE];
	char hex[2 * FW_SHA256_SIZE + 1];
	fw_sha256(u->snapshot, size, digest);
	fw_sha256_hex(digest, hex);
	u->manifest_len = (size_t)snprintf(u->manifest, sizeof(u->manifest),
					   "size=%u\nsha256=%s\nconsistency=%s\nlayout=%s\n", size,
					   hex, verified ? "snapshot" : "unverified",
					   fw_layout_id(u->dump_map.kind));
	LOGI("firmware: dump snapshot ready (%s)", verified ? "verified" : "changed while copied");
	pthread_mutex_lock(&u->lock);
	u->phase = UVCD_FW_PHASE_READY;
	u->done = u->total = size;
	u->poll_ms = 0;
	u->activity_ms = now_ms();
	pthread_mutex_unlock(&u->lock);
}

/* --------------------------------------------------------------------------
 * Install (worker)
 */
static int remove_tree(const char *path)
{
	struct stat st;
	if (lstat(path, &st) != 0)
		return errno == ENOENT ? 0 : -1;
	if (!S_ISDIR(st.st_mode))
		return unlink(path);
	DIR *d = opendir(path);
	if (!d)
		return -1;
	struct dirent *e;
	int ret = 0;
	while (ret == 0 && (e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char child[PATH_MAX];
		ret = pathf(child, "%s/%s", path, e->d_name) == 0 ? remove_tree(child) : -1;
	}
	closedir(d);
	return ret == 0 ? rmdir(path) : ret;
}

static int copy_file(const char *from, const char *to)
{
	char tmp[PATH_MAX];
	uint8_t buf[4096];
	int in = open(from, O_RDONLY | O_CLOEXEC), out = -1, ret = -1;

	tmp[0] = '\0';
	if (in < 0 || pathf(tmp, "%s.tmp", to) != 0)
		goto out;
	out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (out < 0)
		goto out;
	for (;;) {
		ssize_t n = read(in, buf, sizeof(buf));
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			goto out;
		if (n == 0)
			break;
		for (ssize_t w = 0; w < n;) {
			ssize_t m = write(out, buf + w, (size_t)(n - w));
			if (m < 0 && errno == EINTR)
				continue;
			if (m <= 0)
				goto out;
			w += m;
		}
	}
	if (fsync(out) != 0 || close(out) != 0) {
		out = -1;
		goto out;
	}
	out = -1;
	ret = rename(tmp, to);
out:
	if (in >= 0)
		close(in);
	if (out >= 0)
		close(out);
	if (ret != 0 && tmp[0])
		unlink(tmp);
	return ret;
}

/* Give slot target a fresh upper layer, with the carried-over settings of
 * slot source. Never touches the running slot's directory. */
static int prepare_overlay(struct uvcd_update *u, int target, int source)
{
	char dir[PATH_MAX], path[PATH_MAX], from[PATH_MAX];
	struct statvfs vfs;

	if (statvfs(u->cfg.data_root, &vfs) != 0)
		return -1;
	if (vfs.f_flag & ST_RDONLY) {
		errno = EROFS;
		return -1;
	}
	if (pathf(dir, "%s/slots/%s", u->cfg.data_root, slot_name(target)) != 0 ||
	    remove_tree(dir) != 0 || pathf(path, "%s/work", dir) != 0 || make_dirs(path) != 0 ||
	    pathf(path, "%s/root", dir) != 0 || make_dirs(path) != 0)
		return -1;
	for (size_t i = 0; i < sizeof(carry_over) / sizeof(carry_over[0]); i++) {
		struct stat st;
		if (pathf(from, "%s/slots/%s/root/%s", u->cfg.data_root, slot_name(source),
			  carry_over[i]) != 0)
			return -1;
		/* Only what the running slot really has: no whiteouts or links. */
		if (lstat(from, &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		if (pathf(path, "%s/root/%s", dir, carry_over[i]) != 0)
			return -1;
		char *slash = strrchr(path, '/');
		*slash = '\0';
		int ok = make_dirs(path);
		*slash = '/';
		if (ok != 0 || copy_file(from, path) != 0)
			return -1;
	}
	sync();
	return 0;
}

static uint8_t bundle_error(enum fw_bundle_status s)
{
	switch (s) {
	case FW_BUNDLE_HASH:
		return UVCD_FW_ERROR_HASH;
	case FW_BUNDLE_IMAGE:
		return UVCD_FW_ERROR_IMAGE;
	case FW_BUNDLE_TOO_LARGE:
		return UVCD_FW_ERROR_TOO_LARGE;
	case FW_BUNDLE_INCOMPATIBLE:
		return UVCD_FW_ERROR_INCOMPATIBLE;
	default:
		return UVCD_FW_ERROR_BUNDLE;
	}
}

/* Erase the whole partition, write data, read the whole partition back:
 * the image must match its digest and the rest must be erased. */
static int write_partition(struct uvcd_update *u, const struct fw_layout_map *map,
			   const char *name, const uint8_t *data, uint32_t len,
			   const uint8_t digest[FW_SHA256_SIZE], uint8_t *error, uint32_t *detail)
{
	struct fw_mtd_geometry g;
	struct fw_flash *f = &u->cfg.flash;
	uint8_t *buf = malloc(IO_CHUNK);
	int h = fw_layout_open(f, map, name, true, &g), ret = -1;

	*detail = 0;
	if (h < 0 || !buf) {
		*error = h < 0 && h == -EINVAL ? UVCD_FW_ERROR_LAYOUT : UVCD_FW_ERROR_MTD;
		*detail = (uint32_t)(h < 0 ? -h : ENOMEM);
		goto out;
	}
	LOGI("firmware: writing %u bytes to %s", len, name);
	set_phase(u, UVCD_FW_PHASE_ERASING, 0, g.size);
	for (uint32_t off = 0; off < g.size; off += g.erasesize) {
		if (cancelled(u)) {
			*error = 0;
			goto out;
		}
		if (fw_flash_erase_range(f, h, g.erasesize, off, g.erasesize) != 0) {
			*error = UVCD_FW_ERROR_ERASE;
			*detail = (uint32_t)errno;
			goto out;
		}
		set_done(u, off + g.erasesize);
	}
	set_phase(u, UVCD_FW_PHASE_PROGRAMMING, 0, len);
	for (uint32_t off = 0; off < len; off += IO_CHUNK) {
		uint32_t n = len - off < IO_CHUNK ? len - off : IO_CHUNK;
		if (cancelled(u)) {
			*error = 0;
			goto out;
		}
		if (fw_flash_write_all(f, h, data + off, n, off) != 0) {
			*error = UVCD_FW_ERROR_WRITE;
			*detail = (uint32_t)errno;
			goto out;
		}
		set_done(u, off + n);
	}
	set_phase(u, UVCD_FW_PHASE_VERIFYING, 0, g.size);
	struct fw_sha256 s;
	fw_sha256_init(&s);
	for (uint32_t off = 0; off < g.size; off += IO_CHUNK) {
		uint32_t n = g.size - off < IO_CHUNK ? g.size - off : IO_CHUNK;
		if (fw_flash_read_all(f, h, buf, n, off) != 0) {
			*error = UVCD_FW_ERROR_MTD;
			*detail = (uint32_t)errno;
			goto out;
		}
		uint32_t image = off < len ? (len - off < n ? len - off : n) : 0;
		fw_sha256_update(&s, buf, image);
		for (uint32_t i = image; i < n; i++) {
			if (buf[i] != 0xff) {
				LOGE("firmware: %s not erased at 0x%x", name, off + i);
				*error = UVCD_FW_ERROR_VERIFY;
				goto out;
			}
		}
		set_done(u, off + n);
	}
	uint8_t got[FW_SHA256_SIZE];
	fw_sha256_final(&s, got);
	if (memcmp(got, digest, FW_SHA256_SIZE) != 0) {
		LOGE("firmware: %s reads back differently from what was written", name);
		*error = UVCD_FW_ERROR_VERIFY;
		goto out;
	}
	ret = 0;
out:
	if (h >= 0)
		f->ops->close(f, h);
	free(buf);
	return ret;
}

/* One environment transaction, then read back: the values must be there. */
static int env_commit(struct uvcd_update *u, const struct fw_layout_map *map,
		      const char *const *names, const char *const *values, int n)
{
	struct fw_env *env = malloc(sizeof(*env)), *check = malloc(sizeof(*check));
	int ret = -1;

	if (!env || !check || fw_env_load(&u->cfg.flash, map, env) != 0)
		goto out;
	for (int i = 0; i < n; i++) {
		if (fw_env_set(env, names[i], values[i]) != 0)
			goto out;
	}
	if (fw_env_save(&u->cfg.flash, map, env) != 0 ||
	    fw_env_load(&u->cfg.flash, map, check) != 0)
		goto out;
	for (int i = 0; i < n; i++) {
		const char *v = fw_env_get(check, names[i]);
		if (values[i] ? !v || strcmp(v, values[i]) != 0 : v != NULL) {
			errno = EIO;
			goto out;
		}
	}
	ret = 0;
out:
	free(env);
	free(check);
	return ret;
}

static void job_install(struct uvcd_update *u)
{
	struct boot_state *bs = malloc(sizeof(*bs));
	struct fw_bundle b;
	char why[160] = "";
	uint8_t error = 0, digest[FW_SHA256_SIZE];
	uint32_t detail = 0, len;

	pthread_mutex_lock(&u->lock);
	len = u->total;
	pthread_mutex_unlock(&u->lock);

	set_phase(u, UVCD_FW_PHASE_VALIDATING, 0, len);
	if (!bs) {
		error = UVCD_FW_ERROR_NO_MEMORY;
		goto out;
	}
	fw_sha256(u->bundle, len, digest);
	if (memcmp(digest, u->bundle_digest, sizeof(digest)) != 0) {
		snprintf(why, sizeof(why), "bundle does not match the digest it was announced with");
		error = UVCD_FW_ERROR_HASH;
		goto out;
	}
	enum fw_bundle_status st = fw_bundle_parse(u->bundle, len, &b, why, sizeof(why));
	if (st == FW_BUNDLE_OK) {
		boot_read(u, bs);
		st = fw_bundle_compatible(&b, bs->boot_compat, why, sizeof(why));
	}
	if (st != FW_BUNDLE_OK) {
		error = bundle_error(st);
		goto out;
	}
	/* The boot state could have changed since BEGIN_UPGRADE. */
	if (!upgrade_allowed(bs)) {
		snprintf(why, sizeof(why), "boot state no longer allows an update");
		error = UVCD_FW_ERROR_SLOT_STATE;
		goto out;
	}
	set_done(u, len);
	int target = !bs->active;
	char kernel[16], rootfs[16];
	snprintf(kernel, sizeof(kernel), "kernel_%s", slot_name(target));
	snprintf(rootfs, sizeof(rootfs), "rootfs_%s", slot_name(target));
	LOGI("firmware: installing %s into slot %s", b.build, slot_name(target));

	if (write_partition(u, &bs->map, kernel, u->bundle + b.component[0].offset,
			    b.component[0].length, b.component[0].sha256, &error, &detail) != 0 ||
	    write_partition(u, &bs->map, rootfs, u->bundle + b.component[1].offset,
			    b.component[1].length, b.component[1].sha256, &error, &detail) != 0) {
		if (!error) /* cancelled: cancelled() set the phase */
			goto done;
		snprintf(why, sizeof(why), "writing slot %s failed", slot_name(target));
		goto out;
	}

	/* No turning back from here: abort would leave the slot half set up. */
	pthread_mutex_lock(&u->lock);
	if (u->cancel || u->stopping) {
		pthread_mutex_unlock(&u->lock);
		cancelled(u);
		goto done;
	}
	u->committing = true;
	pthread_mutex_unlock(&u->lock);
	if (prepare_overlay(u, target, bs->active) != 0) {
		snprintf(why, sizeof(why), "preparing slot %s's settings: %s", slot_name(target),
			 strerror(errno));
		error = UVCD_FW_ERROR_OVERLAY;
		detail = (uint32_t)errno;
		goto out;
	}
	static const char *const names[] = {"trial_slot", "upgrade_available", "bootcount"};
	const char *values[] = {slot_name(target), "1", "0"};
	if (env_commit(u, &bs->map, names, values, 3) != 0) {
		snprintf(why, sizeof(why), "scheduling the trial: %s", strerror(errno));
		error = UVCD_FW_ERROR_ENV;
		detail = (uint32_t)errno;
		goto out;
	}
	LOGI("firmware: slot %s holds %s; trial scheduled for the next boot", slot_name(target),
	     b.build);
	boot_refresh(u);
	pthread_mutex_lock(&u->lock);
	u->phase = UVCD_FW_PHASE_STAGED;
	u->poll_ms = 0;
	pthread_mutex_unlock(&u->lock);
	goto done;
out:
	LOGE("firmware: update failed: %s", why[0] ? why : "error");
	fail(u, error ? error : UVCD_FW_ERROR_BUNDLE, detail);
done:
	pthread_mutex_lock(&u->lock);
	u->committing = false;
	pthread_mutex_unlock(&u->lock);
	free(u->bundle);
	u->bundle = NULL;
	free(bs);
}

/* --------------------------------------------------------------------------
 * Trial confirmation, unscheduling, reboot (worker)
 */
static bool root_on_slot(struct uvcd_update *u, int slot)
{
	char mounts[8192], want[PATH_MAX];
	if (read_text(u->cfg.mounts_path, mounts, sizeof(mounts)) != 0)
		return false;
	if (pathf(want, "upperdir=%s/slots/%s/root", u->cfg.data_root, slot_name(slot)) != 0)
		return false;
	for (char *line = mounts; line && *line;) {
		char *next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		char dev[64], dir[64], type[32];
		if (sscanf(line, "%63s %63s %31s", dev, dir, type) == 3 && !strcmp(dir, "/") &&
		    !strncmp(type, "overlay", 7)) {
			const char *at = strstr(line, want);
			if (at && (at[strlen(want)] == ',' || at[strlen(want)] == ' '))
				return true;
		}
		line = next;
	}
	return false;
}

static void job_promote(struct uvcd_update *u)
{
	struct boot_state *bs = malloc(sizeof(*bs));
	struct statvfs vfs;
	char why[128] = "";

	if (!bs) {
		snprintf(why, sizeof(why), "out of memory");
		goto failed;
	}
	boot_read(u, bs);
	if (bs->boot != UVCD_FW_BOOT_TRIAL || strcmp(bs->mode, "trial") != 0) {
		snprintf(why, sizeof(why), "not a pending trial of the running slot");
		goto failed;
	}
	if (statvfs(u->cfg.data_root, &vfs) != 0 || (vfs.f_flag & ST_RDONLY)) {
		snprintf(why, sizeof(why), "settings storage is not writable");
		goto failed;
	}
	if (!root_on_slot(u, bs->running)) {
		snprintf(why, sizeof(why), "root is not on slot %s's settings layer",
			 slot_name(bs->running));
		goto failed;
	}
	static const char *const names[] = {"active_slot", "trial_slot", "upgrade_available",
					    "bootcount"};
	const char *values[] = {slot_name(bs->running), NULL, "0", "0"};
	if (env_commit(u, &bs->map, names, values, 4) != 0) {
		snprintf(why, sizeof(why), "saving the boot environment: %s", strerror(errno));
		goto failed;
	}
	LOGI("firmware: slot %s confirmed", slot_name(bs->running));
	write_run_file(u, "confirmed", slot_name(bs->running));
	boot_refresh(u);
	pthread_mutex_lock(&u->lock);
	u->trial = TRIAL_CONFIRMED;
	pthread_mutex_unlock(&u->lock);
	free(bs);
	return;
failed:
	LOGE("firmware: trial not confirmed: %s", why);
	write_run_file(u, "trial-failed", why);
	pthread_mutex_lock(&u->lock);
	u->trial = TRIAL_FAILED;
	pthread_mutex_unlock(&u->lock);
	free(bs);
}

static void job_unschedule(struct uvcd_update *u)
{
	struct fw_layout_map *map = malloc(sizeof(*map));
	static const char *const names[] = {"trial_slot", "upgrade_available", "bootcount"};
	const char *values[] = {NULL, "0", "0"};

	if (!map || fw_layout_resolve(&u->cfg.flash, map) != 0 || map->kind != FW_LAYOUT_AB ||
	    env_commit(u, map, names, values, 3) != 0) {
		LOGE("firmware: withdrawing the trial failed: %s", strerror(errno));
		fail(u, UVCD_FW_ERROR_ENV, (uint32_t)errno);
		free(map);
		boot_refresh(u);
		return;
	}
	free(map);
	LOGI("firmware: scheduled trial withdrawn");
	boot_refresh(u);
	pthread_mutex_lock(&u->lock);
	bool running_trial = u->boot.boot == UVCD_FW_BOOT_FALLBACK && !strcmp(u->boot.mode, "trial");
	u->phase = UVCD_FW_PHASE_ABORTED;
	u->error = 0;
	u->poll_ms = 0;
	pthread_mutex_unlock(&u->lock);
	/* Withdrawn from inside its own trial: the gate takes it back. */
	if (running_trial)
		write_run_file(u, "trial-failed", "trial withdrawn");
}

static int default_reboot(void)
{
	pid_t pid = fork();
	if (pid == 0) {
		execl("/sbin/reboot", "reboot", (char *)NULL);
		_exit(127);
	}
	int status;
	if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0)
		return -1;
	return 0;
}

static void job_reboot(struct uvcd_update *u)
{
	/* Long enough for the host to read REBOOTING. */
	struct timespec pause = {.tv_sec = 1};
	nanosleep(&pause, NULL);
	sync();
	LOGI("firmware: rebooting into the trial");
	if (u->cfg.reboot() != 0) {
		LOGE("firmware: reboot failed");
		fail(u, UVCD_FW_ERROR_REBOOT, (uint32_t)errno);
	}
}

static void *worker_main(void *arg)
{
	struct uvcd_update *u = arg;

	pthread_mutex_lock(&u->lock);
	for (;;) {
		while (!u->stopping && u->job == JOB_NONE)
			pthread_cond_wait(&u->cond, &u->lock);
		if (u->stopping)
			break;
		enum job job = u->job;
		pthread_mutex_unlock(&u->lock);
		switch (job) {
		case JOB_SNAPSHOT:
			job_snapshot(u);
			break;
		case JOB_INSTALL:
			job_install(u);
			break;
		case JOB_PROMOTE:
			job_promote(u);
			break;
		case JOB_UNSCHEDULE:
			job_unschedule(u);
			break;
		case JOB_REBOOT:
			job_reboot(u);
			break;
		case JOB_NONE:
			break;
		}
		pthread_mutex_lock(&u->lock);
		u->job = JOB_NONE;
		u->cancel = false;
		u->cancel_error = 0;
		if (u->phase != UVCD_FW_PHASE_RECEIVING && u->phase != UVCD_FW_PHASE_READY &&
		    u->phase != UVCD_FW_PHASE_STAGED)
			u->poll_ms = 0;
		pthread_cond_broadcast(&u->cond);
	}
	pthread_mutex_unlock(&u->lock);
	return NULL;
}

/* Under lock. */
static void queue_job(struct uvcd_update *u, enum job job)
{
	u->job = job;
	u->cancel = false;
	u->cancel_error = 0;
	u->poll_ms = POLL_BUSY_MS;
	pthread_cond_broadcast(&u->cond);
}

/* --------------------------------------------------------------------------
 * Main thread: the protocol
 */
static void release_session_buffers(struct uvcd_update *u)
{
	free(u->bundle);
	u->bundle = NULL;
	free(u->snapshot);
	u->snapshot = NULL;
	if (u->live_h >= 0) {
		u->cfg.flash.ops->close(&u->cfg.flash, u->live_h);
		u->live_h = -1;
	}
	u->live_cache_valid = false;
	u->manifest_len = 0;
}

/* End a session the main thread owns (RECEIVING or READY). Under lock. */
static void end_session(struct uvcd_update *u, uint8_t phase, uint8_t error)
{
	u->phase = phase;
	u->error = error;
	u->detail = 0;
	u->poll_ms = 0;
	if (phase != UVCD_FW_PHASE_ERROR)
		u->session = 0;
	release_session_buffers(u);
}

static bool bytes_zero(const uint8_t *p, size_t n)
{
	while (n--) {
		if (*p++)
			return false;
	}
	return true;
}

static void build_status(struct uvcd_update *u, uint8_t *out)
{
	memset(out, 0, UVCD_FW_FRAME_SIZE);
	uvcd_fw_put_magic(out);
	out[UVCD_FW_STATUS_VERSION] = UVCD_FW_PROTOCOL_VERSION;
	out[UVCD_FW_STATUS_OPCODE] = u->last_opcode;
	out[UVCD_FW_STATUS_RESULT] = u->last_result;
	uvcd_fw_put_le32(out + UVCD_FW_STATUS_SEQUENCE, u->have_last ? u->last_seq : 0);
	pthread_mutex_lock(&u->lock);
	out[UVCD_FW_STATUS_PHASE] = u->phase;
	uvcd_fw_put_le32(out + UVCD_FW_STATUS_SESSION, u->session);
	uvcd_fw_put_le32(out + UVCD_FW_STATUS_DONE, u->done);
	uvcd_fw_put_le32(out + UVCD_FW_STATUS_TOTAL, u->total);
	uvcd_fw_put_le32(out + UVCD_FW_STATUS_FEATURES, features_of(&u->boot));
	uvcd_fw_put_le16(out + UVCD_FW_STATUS_POLL_MS, u->poll_ms);
	out[UVCD_FW_STATUS_ACTIVE_SLOT] = (uint8_t)u->boot.active;
	out[UVCD_FW_STATUS_TRIAL_SLOT] = (uint8_t)u->boot.trial;
	out[UVCD_FW_STATUS_RUNNING_SLOT] = (uint8_t)u->boot.running;
	out[UVCD_FW_STATUS_BOOT] = u->boot.boot;
	out[UVCD_FW_STATUS_OPERATION] = u->operation;
	out[UVCD_FW_STATUS_ERROR] = u->phase == UVCD_FW_PHASE_ERROR ? u->error : 0;
	uvcd_fw_put_le32(out + UVCD_FW_STATUS_DETAIL,
			 u->last_result ? u->last_detail :
			 u->phase == UVCD_FW_PHASE_ERROR ? u->detail : 0);
	pthread_mutex_unlock(&u->lock);
}

static void set_window(struct uvcd_update *u, uint32_t session, uint32_t seq, uint32_t offset,
		       uint8_t kind, const void *payload, uint8_t n)
{
	uint8_t *w = u->window;
	memset(w, 0, UVCD_FW_FRAME_SIZE);
	uvcd_fw_put_le32(w + UVCD_FW_DATA_SESSION, session);
	uvcd_fw_put_le32(w + UVCD_FW_DATA_OFFSET, offset);
	uvcd_fw_put_le32(w + UVCD_FW_DATA_SEQUENCE, seq);
	w[UVCD_FW_DATA_LENGTH] = n;
	w[UVCD_FW_DATA_KIND] = kind;
	if (n)
		memcpy(w + UVCD_FW_DATA_PAYLOAD, payload, n);
	uint16_t crc = fw_crc16(FW_CRC16_INIT, w, UVCD_FW_DATA_CRC);
	crc = fw_crc16(crc, w + UVCD_FW_DATA_PAYLOAD, n);
	uvcd_fw_put_le16(w + UVCD_FW_DATA_CRC, crc);
	u->window_valid = true;
}

static void build_info(struct uvcd_update *u)
{
	struct boot_state bs;
	char serial[64] = "";
	bool idle;

	pthread_mutex_lock(&u->lock);
	idle = u->job == JOB_NONE;
	pthread_mutex_unlock(&u->lock);
	/* The environment is only read while nothing is writing it. */
	if (idle)
		boot_refresh(u);
	pthread_mutex_lock(&u->lock);
	bs = u->boot;
	pthread_mutex_unlock(&u->lock);
	if (read_text(u->cfg.serial_path, serial, sizeof(serial)) == 0)
		serial[strcspn(serial, "\n")] = '\0';

	uint32_t f = features_of(&bs);
	int n = snprintf(u->info, sizeof(u->info),
			 "protocol=%d\nboard=%s\nlayout=%s\nbuild=%s\nkernel=%s\n"
			 "min_boot_compat=%u\nboot_compat=%u\nserial=%s\nflash_size=%u\n"
			 "erase_size=%u\nkernel_capacity=%u\nrootfs_capacity=%u\nboot=%s\n"
			 "boot_mode=%s\nrunning_slot=%s\n"
			 "active_slot=%s\ntrial_slot=%s\nupgrade_available=%d\nbootcount=%d\n"
			 "bootlimit=%d\nfeatures=%s%s%s%s\n",
			 UVCD_FW_PROTOCOL_VERSION, u->release.board, fw_layout_id(bs.map.kind),
			 u->release.build, u->release.kernel, u->release.min_boot_compat,
			 bs.boot_compat, serial, FW_FLASH_SIZE, bs.map.erasesize,
			 bs.map.kind == FW_LAYOUT_AB ? FW_KERNEL_CAPACITY : 0,
			 bs.map.kind == FW_LAYOUT_AB ? FW_ROOTFS_CAPACITY : 0, boot_name(bs.boot),
			 bs.mode, slot_name(bs.running), slot_name(bs.active),
			 slot_name(bs.trial), bs.upgrade_available, bs.bootcount, bs.bootlimit,
			 "info", f & UVCD_FW_FEATURE_DUMP ? ",dump" : "",
			 f & UVCD_FW_FEATURE_AB_SLOTS ? ",ab" : "",
			 f & UVCD_FW_FEATURE_UPGRADE ? ",upgrade" : "");
	u->info_len = n > 0 && (size_t)n < sizeof(u->info) ? (size_t)n : sizeof(u->info) - 1;
	u->info_valid = true;
}

static int read_live(struct uvcd_update *u, uint32_t offset, uint8_t *out, uint32_t n)
{
	while (n) {
		uint32_t block = offset & ~(LIVE_BLOCK - 1);
		if (!u->live_cache_valid || u->live_cache_off != block) {
			if (fw_flash_read_all(&u->cfg.flash, u->live_h, u->live_cache, LIVE_BLOCK,
					      block) != 0)
				return -1;
			u->live_cache_off = block;
			u->live_cache_valid = true;
		}
		uint32_t at = offset - block, take = LIVE_BLOCK - at < n ? LIVE_BLOCK - at : n;
		memcpy(out, u->live_cache + at, take);
		out += take;
		offset += take;
		n -= take;
	}
	return 0;
}

struct result {
	uint8_t code;
	uint32_t detail;
};

static struct result ok(void)
{
	return (struct result){0, 0};
}

static struct result err(uint8_t code, uint32_t detail)
{
	return (struct result){code, detail};
}

/* Under lock: a session is running or holds resources. */
static bool session_busy(struct uvcd_update *u)
{
	switch (u->phase) {
	case UVCD_FW_PHASE_ACCEPTED:
	case UVCD_FW_PHASE_RECEIVING:
	case UVCD_FW_PHASE_VALIDATING:
	case UVCD_FW_PHASE_ERASING:
	case UVCD_FW_PHASE_PROGRAMMING:
	case UVCD_FW_PHASE_VERIFYING:
	case UVCD_FW_PHASE_READING:
	case UVCD_FW_PHASE_READY:
	case UVCD_FW_PHASE_REBOOTING:
		return true;
	default:
		return u->job != JOB_NONE;
	}
}

static struct result op_info(struct uvcd_update *u, const uint8_t *f)
{
	if (f[UVCD_FW_COMMAND_TARGET] || f[UVCD_FW_COMMAND_FLAGS] ||
	    uvcd_fw_get_le32(f + UVCD_FW_COMMAND_OFFSET) ||
	    uvcd_fw_get_le32(f + UVCD_FW_COMMAND_LENGTH))
		return err(UVCD_FW_ERROR_BAD_FRAME, 0);
	build_info(u);
	return ok();
}

static struct result op_read(struct uvcd_update *u, const uint8_t *f, uint32_t seq)
{
	uint8_t target = f[UVCD_FW_COMMAND_TARGET];
	uint32_t session = uvcd_fw_get_le32(f + UVCD_FW_COMMAND_SESSION);
	uint32_t offset = uvcd_fw_get_le32(f + UVCD_FW_COMMAND_OFFSET);
	uint8_t buf[WINDOW];

	if (f[UVCD_FW_COMMAND_FLAGS] || uvcd_fw_get_le32(f + UVCD_FW_COMMAND_LENGTH))
		return err(UVCD_FW_ERROR_BAD_FRAME, 0);
	if (target == UVCD_FW_TARGET_INFO) {
		if (!u->info_valid)
			return err(UVCD_FW_ERROR_STATE, 0);
		if (offset > u->info_len)
			return err(UVCD_FW_ERROR_RANGE, 0);
		size_t n = u->info_len - offset < WINDOW ? u->info_len - offset : WINDOW;
		set_window(u, session, seq, offset, target, u->info + offset, (uint8_t)n);
		return ok();
	}
	if (target != UVCD_FW_TARGET_IMAGE && target != UVCD_FW_TARGET_MANIFEST)
		return err(UVCD_FW_ERROR_UNSUPPORTED, 0);

	pthread_mutex_lock(&u->lock);
	bool mine = session && session == u->session && u->operation == UVCD_FW_OP_BEGIN_DUMP;
	bool ready = u->phase == UVCD_FW_PHASE_READY;
	if (mine && ready)
		u->activity_ms = now_ms();
	pthread_mutex_unlock(&u->lock);
	if (!mine)
		return err(UVCD_FW_ERROR_SESSION, 0);
	if (!ready)
		return err(UVCD_FW_ERROR_STATE, 0);

	uint32_t size = target == UVCD_FW_TARGET_MANIFEST ? (uint32_t)u->manifest_len : FW_FLASH_SIZE;
	if (offset > size)
		return err(UVCD_FW_ERROR_RANGE, 0);
	uint32_t n = size - offset < WINDOW ? size - offset : WINDOW;
	if (target == UVCD_FW_TARGET_MANIFEST)
		memcpy(buf, u->manifest + offset, n);
	else if (u->snapshot)
		memcpy(buf, u->snapshot + offset, n);
	else if (n && read_live(u, offset, buf, n) != 0)
		return err(UVCD_FW_ERROR_MTD, (uint32_t)errno);
	set_window(u, session, seq, offset, target, buf, (uint8_t)n);
	return ok();
}

static struct result op_begin_dump(struct uvcd_update *u, const uint8_t *f)
{
	uint8_t flags = f[UVCD_FW_COMMAND_FLAGS];
	if (f[UVCD_FW_COMMAND_TARGET] != UVCD_FW_TARGET_IMAGE ||
	    (flags & ~UVCD_FW_FLAG_ALLOW_LIVE) || uvcd_fw_get_le32(f + UVCD_FW_COMMAND_SESSION) ||
	    uvcd_fw_get_le32(f + UVCD_FW_COMMAND_OFFSET) ||
	    uvcd_fw_get_le32(f + UVCD_FW_COMMAND_LENGTH))
		return err(UVCD_FW_ERROR_BAD_FRAME, 0);

	pthread_mutex_lock(&u->lock);
	bool busy = session_busy(u);
	pthread_mutex_unlock(&u->lock);
	if (busy)
		return err(UVCD_FW_ERROR_BUSY, 0);

	if (fw_layout_resolve(&u->cfg.flash, &u->dump_map) != 0 || u->dump_map.all < 0)
		return err(UVCD_FW_ERROR_LAYOUT, 0);
	int h = fw_layout_open(&u->cfg.flash, &u->dump_map, FW_PART_ALL, false, NULL);
	if (h < 0)
		return err(h == -EINVAL ? UVCD_FW_ERROR_LAYOUT : UVCD_FW_ERROR_MTD, (uint32_t)-h);

	release_session_buffers(u);
	bool snapshot = memory_for(u, FW_FLASH_SIZE) && (u->snapshot = malloc(FW_FLASH_SIZE));
	if (!snapshot && !(flags & UVCD_FW_FLAG_ALLOW_LIVE)) {
		u->cfg.flash.ops->close(&u->cfg.flash, h);
		return err(UVCD_FW_ERROR_NO_MEMORY, 0);
	}
	pthread_mutex_lock(&u->lock);
	u->operation = UVCD_FW_OP_BEGIN_DUMP;
	u->session = new_session(u);
	u->error = 0;
	u->detail = 0;
	u->activity_ms = now_ms();
	if (snapshot) {
		u->cfg.flash.ops->close(&u->cfg.flash, h);
		u->phase = UVCD_FW_PHASE_ACCEPTED;
		u->done = 0;
		u->total = 2 * FW_FLASH_SIZE;
		queue_job(u, JOB_SNAPSHOT);
	} else {
		/* Not enough memory for a copy: read the flash as asked. */
		u->live_h = h;
		u->manifest_len = (size_t)snprintf(u->manifest, sizeof(u->manifest),
						   "size=%u\nsha256=\nconsistency=live\nlayout=%s\n",
						   FW_FLASH_SIZE, fw_layout_id(u->dump_map.kind));
		u->phase = UVCD_FW_PHASE_READY;
		u->done = u->total = FW_FLASH_SIZE;
		u->poll_ms = 0;
	}
	pthread_mutex_unlock(&u->lock);
	LOGI("firmware: dump started (%s)", snapshot ? "snapshot" : "live");
	return ok();
}

static struct result op_end_dump(struct uvcd_update *u, const uint8_t *f)
{
	uint32_t session = uvcd_fw_get_le32(f + UVCD_FW_COMMAND_SESSION);
	if (f[UVCD_FW_COMMAND_TARGET] || f[UVCD_FW_COMMAND_FLAGS] ||
	    uvcd_fw_get_le32(f + UVCD_FW_COMMAND_OFFSET) ||
	    uvcd_fw_get_le32(f + UVCD_FW_COMMAND_LENGTH))
		return err(UVCD_FW_ERROR_BAD_FRAME, 0);
	pthread_mutex_lock(&u->lock);
	struct result r = ok();
	if (!session || session != u->session || u->operation != UVCD_FW_OP_BEGIN_DUMP)
		r = err(UVCD_FW_ERROR_SESSION, 0);
	else if (u->phase != UVCD_FW_PHASE_READY)
		r = err(UVCD_FW_ERROR_STATE, 0);
	else
		end_session(u, UVCD_FW_PHASE_COMPLETE, 0);
	pthread_mutex_unlock(&u->lock);
	return r;
}

static struct result op_begin_upgrade(struct uvcd_update *u, const uint8_t *f)
{
	uint32_t len = uvcd_fw_get_le32(f + UVCD_FW_COMMAND_LENGTH);
	const uint32_t max = FW_BUNDLE_MANIFEST_SIZE + FW_KERNEL_CAPACITY + FW_BUNDLE_ALIGN +
			     FW_ROOTFS_CAPACITY;

	if (f[UVCD_FW_COMMAND_TARGET] || f[UVCD_FW_COMMAND_FLAGS] ||
	    uvcd_fw_get_le32(f + UVCD_FW_COMMAND_SESSION) ||
	    uvcd_fw_get_le32(f + UVCD_FW_COMMAND_OFFSET) ||
	    bytes_zero(f + UVCD_FW_COMMAND_DIGEST, FW_SHA256_SIZE))
		return err(UVCD_FW_ERROR_BAD_FRAME, 0);
	pthread_mutex_lock(&u->lock);
	bool busy = session_busy(u);
	pthread_mutex_unlock(&u->lock);
	if (busy)
		return err(UVCD_FW_ERROR_BUSY, 0);
	if (len < FW_BUNDLE_MANIFEST_SIZE)
		return err(UVCD_FW_ERROR_RANGE, 0);
	if (len > max)
		return err(UVCD_FW_ERROR_TOO_LARGE, 0);

	boot_refresh(u);
	pthread_mutex_lock(&u->lock);
	struct boot_state bs = u->boot;
	pthread_mutex_unlock(&u->lock);
	if (bs.map.kind != FW_LAYOUT_AB)
		return err(UVCD_FW_ERROR_LAYOUT, 0);
	if (!upgrade_allowed(&bs))
		return err(UVCD_FW_ERROR_SLOT_STATE, bs.boot);

	release_session_buffers(u);
	if (!memory_for(u, len) || !(u->bundle = malloc(len)))
		return err(UVCD_FW_ERROR_NO_MEMORY, 0);
	memcpy(u->bundle_digest, f + UVCD_FW_COMMAND_DIGEST, FW_SHA256_SIZE);
	u->received = 0;
	pthread_mutex_lock(&u->lock);
	u->operation = UVCD_FW_OP_BEGIN_UPGRADE;
	u->session = new_session(u);
	u->phase = UVCD_FW_PHASE_RECEIVING;
	u->error = 0;
	u->detail = 0;
	u->done = 0;
	u->total = len;
	u->poll_ms = 0;
	u->activity_ms = now_ms();
	pthread_mutex_unlock(&u->lock);
	LOGI("firmware: receiving a %u byte update", len);
	return ok();
}

static struct result op_finish_upgrade(struct uvcd_update *u, const uint8_t *f)
{
	uint32_t session = uvcd_fw_get_le32(f + UVCD_FW_COMMAND_SESSION);
	if (f[UVCD_FW_COMMAND_TARGET] || f[UVCD_FW_COMMAND_FLAGS] ||
	    uvcd_fw_get_le32(f + UVCD_FW_COMMAND_OFFSET))
		return err(UVCD_FW_ERROR_BAD_FRAME, 0);
	pthread_mutex_lock(&u->lock);
	struct result r = ok();
	if (!session || session != u->session || u->operation != UVCD_FW_OP_BEGIN_UPGRADE)
		r = err(UVCD_FW_ERROR_SESSION, 0);
	else if (u->phase != UVCD_FW_PHASE_RECEIVING)
		r = err(UVCD_FW_ERROR_STATE, 0);
	else if (uvcd_fw_get_le32(f + UVCD_FW_COMMAND_LENGTH) != u->total)
		r = err(UVCD_FW_ERROR_RANGE, 0);
	else if (u->received != u->total)
		r = err(UVCD_FW_ERROR_STATE, u->received);
	else {
		u->phase = UVCD_FW_PHASE_ACCEPTED;
		queue_job(u, JOB_INSTALL);
	}
	pthread_mutex_unlock(&u->lock);
	return r;
}

static struct result op_abort(struct uvcd_update *u, const uint8_t *f)
{
	uint32_t session = uvcd_fw_get_le32(f + UVCD_FW_COMMAND_SESSION);
	uint8_t flags = f[UVCD_FW_COMMAND_FLAGS];
	if (f[UVCD_FW_COMMAND_TARGET] || (flags & ~UVCD_FW_FLAG_UNSCHEDULE) ||
	    uvcd_fw_get_le32(f + UVCD_FW_COMMAND_OFFSET) ||
	    uvcd_fw_get_le32(f + UVCD_FW_COMMAND_LENGTH))
		return err(UVCD_FW_ERROR_BAD_FRAME, 0);

	struct result r = ok();
	pthread_mutex_lock(&u->lock);
	if (!session) {
		/* Withdraw a trial scheduled before this session (or daemon). */
		if (!(flags & UVCD_FW_FLAG_UNSCHEDULE))
			r = err(UVCD_FW_ERROR_SESSION, 0);
		else if (session_busy(u))
			r = err(UVCD_FW_ERROR_BUSY, 0);
		else if (!trial_pending(&u->boot))
			r = err(UVCD_FW_ERROR_STATE, 0);
		else {
			u->operation = UVCD_FW_OP_ABORT;
			u->phase = UVCD_FW_PHASE_ACCEPTED;
			u->error = 0;
			queue_job(u, JOB_UNSCHEDULE);
		}
	} else if (session != u->session) {
		r = err(UVCD_FW_ERROR_SESSION, 0);
	} else {
		switch (u->phase) {
		case UVCD_FW_PHASE_RECEIVING:
		case UVCD_FW_PHASE_READY:
			end_session(u, UVCD_FW_PHASE_ABORTED, 0);
			break;
		case UVCD_FW_PHASE_ACCEPTED:
		case UVCD_FW_PHASE_READING:
		case UVCD_FW_PHASE_VALIDATING:
		case UVCD_FW_PHASE_ERASING:
		case UVCD_FW_PHASE_PROGRAMMING:
		case UVCD_FW_PHASE_VERIFYING:
			if (u->committing || u->job == JOB_UNSCHEDULE) {
				r = err(UVCD_FW_ERROR_NOT_CANCELLABLE, 0);
			} else {
				u->cancel = true;
				u->cancel_error = 0;
			}
			break;
		case UVCD_FW_PHASE_STAGED:
			if (!(flags & UVCD_FW_FLAG_UNSCHEDULE)) {
				r = err(UVCD_FW_ERROR_NOT_CANCELLABLE, 0);
			} else {
				u->phase = UVCD_FW_PHASE_ACCEPTED;
				queue_job(u, JOB_UNSCHEDULE);
			}
			break;
		default:
			r = err(UVCD_FW_ERROR_STATE, 0);
			break;
		}
	}
	pthread_mutex_unlock(&u->lock);
	return r;
}

static struct result op_reboot(struct uvcd_update *u, const uint8_t *f)
{
	uint32_t session = uvcd_fw_get_le32(f + UVCD_FW_COMMAND_SESSION);
	if (f[UVCD_FW_COMMAND_TARGET] || f[UVCD_FW_COMMAND_FLAGS] ||
	    uvcd_fw_get_le32(f + UVCD_FW_COMMAND_OFFSET) ||
	    uvcd_fw_get_le32(f + UVCD_FW_COMMAND_LENGTH))
		return err(UVCD_FW_ERROR_BAD_FRAME, 0);
	struct result r = ok();
	pthread_mutex_lock(&u->lock);
	bool staged = session && session == u->session && u->phase == UVCD_FW_PHASE_STAGED;
	bool scheduled = !session && !session_busy(u) && u->boot.upgrade_available == 1 &&
			 u->boot.trial != FW_SLOT_NONE;
	if (session && session != u->session)
		r = err(UVCD_FW_ERROR_SESSION, 0);
	else if (!staged && !scheduled)
		r = err(UVCD_FW_ERROR_STATE, 0);
	else {
		u->operation = staged ? u->operation : UVCD_FW_OP_REBOOT;
		u->phase = UVCD_FW_PHASE_REBOOTING;
		u->error = 0;
		queue_job(u, JOB_REBOOT);
	}
	pthread_mutex_unlock(&u->lock);
	return r;
}

/* Record a frame's outcome for STATUS. */
static void record(struct uvcd_update *u, uint8_t selector, const uint8_t *frame, int32_t len,
		   uint32_t seq, uint8_t opcode, struct result r)
{
	u->have_last = true;
	u->last_seq = seq;
	u->last_selector = selector;
	u->last_opcode = opcode;
	u->last_result = r.code;
	u->last_detail = r.detail;
	memset(u->last_frame, 0, sizeof(u->last_frame));
	if (len > 0)
		memcpy(u->last_frame, frame, len < UVCD_FW_FRAME_SIZE ? (size_t)len : sizeof(u->last_frame));
	if (r.code)
		LOGD("firmware: frame %u (op %u) rejected: %u", seq, opcode, r.code);
}

enum frame_kind {
	FRAME_NEW,
	FRAME_REPEAT, /* the last frame again: keeps its result, not run again */
	FRAME_STALE,  /* the last frame's sequence on other bytes */
};

static enum frame_kind classify(struct uvcd_update *u, uint8_t selector, const uint8_t *frame,
				uint32_t seq)
{
	if (!u->have_last || seq != u->last_seq)
		return FRAME_NEW;
	if (selector == u->last_selector && !memcmp(frame, u->last_frame, UVCD_FW_FRAME_SIZE))
		return FRAME_REPEAT;
	return FRAME_STALE;
}

static void handle_command(struct uvcd_update *u, const uint8_t *f, int32_t len)
{
	uint32_t seq = len >= UVCD_FW_COMMAND_SEQUENCE + 4 ? uvcd_fw_get_le32(f + UVCD_FW_COMMAND_SEQUENCE) : 0;
	uint8_t op = len > UVCD_FW_COMMAND_OPCODE ? f[UVCD_FW_COMMAND_OPCODE] : UVCD_FW_OP_NONE;
	struct result r;

	if (len != UVCD_FW_FRAME_SIZE || !uvcd_fw_has_magic(f) || !seq) {
		r = err(UVCD_FW_ERROR_BAD_FRAME, 0);
		goto out;
	}
	if (f[UVCD_FW_COMMAND_VERSION] != UVCD_FW_PROTOCOL_VERSION) {
		r = err(UVCD_FW_ERROR_BAD_VERSION, 0);
		goto out;
	}
	switch (classify(u, UVCD_FW_COMMAND_SELECTOR, f, seq)) {
	case FRAME_REPEAT:
		return;
	case FRAME_STALE:
		r = err(UVCD_FW_ERROR_STALE_SEQUENCE, 0);
		goto out;
	case FRAME_NEW:
		break;
	}
	if (!bytes_zero(f + UVCD_FW_COMMAND_RESERVED, UVCD_FW_FRAME_SIZE - UVCD_FW_COMMAND_RESERVED) ||
	    (op != UVCD_FW_OP_BEGIN_UPGRADE && !bytes_zero(f + UVCD_FW_COMMAND_DIGEST, FW_SHA256_SIZE))) {
		r = err(UVCD_FW_ERROR_BAD_FRAME, 0);
		goto out;
	}
	switch (op) {
	case UVCD_FW_OP_INFO:
		r = op_info(u, f);
		break;
	case UVCD_FW_OP_READ:
		r = op_read(u, f, seq);
		break;
	case UVCD_FW_OP_BEGIN_UPGRADE:
		r = op_begin_upgrade(u, f);
		break;
	case UVCD_FW_OP_ABORT:
		r = op_abort(u, f);
		break;
	case UVCD_FW_OP_BEGIN_DUMP:
		r = op_begin_dump(u, f);
		break;
	case UVCD_FW_OP_FINISH_UPGRADE:
		r = op_finish_upgrade(u, f);
		break;
	case UVCD_FW_OP_REBOOT:
		r = op_reboot(u, f);
		break;
	case UVCD_FW_OP_END_DUMP:
		r = op_end_dump(u, f);
		break;
	default:
		r = err(UVCD_FW_ERROR_UNSUPPORTED, 0);
		break;
	}
out:
	record(u, UVCD_FW_COMMAND_SELECTOR, f, len, seq, op, r);
}

/* A DATA frame that breaks the upload ends it. Under lock. */
static void upload_failed(struct uvcd_update *u, uint8_t error)
{
	end_session(u, UVCD_FW_PHASE_ERROR, error);
}

static void handle_data(struct uvcd_update *u, const uint8_t *f, int32_t len)
{
	uint32_t seq = len >= UVCD_FW_DATA_SEQUENCE + 4 ? uvcd_fw_get_le32(f + UVCD_FW_DATA_SEQUENCE) : 0;
	struct result r = ok();

	if (len != UVCD_FW_FRAME_SIZE || !seq) {
		r = err(UVCD_FW_ERROR_BAD_FRAME, 0);
		goto out;
	}
	uint8_t n = f[UVCD_FW_DATA_LENGTH];
	if (!n || n > WINDOW || f[UVCD_FW_DATA_KIND] != 0 ||
	    !bytes_zero(f + UVCD_FW_DATA_PAYLOAD + n, WINDOW - n)) {
		r = err(UVCD_FW_ERROR_BAD_FRAME, 0);
		goto out;
	}
	uint16_t crc = fw_crc16(FW_CRC16_INIT, f, UVCD_FW_DATA_CRC);
	crc = fw_crc16(crc, f + UVCD_FW_DATA_PAYLOAD, n);
	if (crc != uvcd_fw_get_le16(f + UVCD_FW_DATA_CRC)) {
		r = err(UVCD_FW_ERROR_CHECKSUM, 0);
		goto out;
	}
	switch (classify(u, UVCD_FW_DATA_SELECTOR, f, seq)) {
	case FRAME_REPEAT:
		return;
	case FRAME_STALE:
		r = err(UVCD_FW_ERROR_STALE_SEQUENCE, 0);
		goto out;
	case FRAME_NEW:
		break;
	}

	uint32_t session = uvcd_fw_get_le32(f + UVCD_FW_DATA_SESSION);
	uint32_t offset = uvcd_fw_get_le32(f + UVCD_FW_DATA_OFFSET);
	const uint8_t *payload = f + UVCD_FW_DATA_PAYLOAD;
	pthread_mutex_lock(&u->lock);
	if (!session || session != u->session || u->operation != UVCD_FW_OP_BEGIN_UPGRADE) {
		r = err(UVCD_FW_ERROR_SESSION, 0);
	} else if (u->phase != UVCD_FW_PHASE_RECEIVING) {
		r = err(UVCD_FW_ERROR_STATE, 0);
	} else if ((uint64_t)offset + n > u->total) {
		r = err(UVCD_FW_ERROR_RANGE, offset);
		upload_failed(u, r.code);
	} else if (offset < u->received) {
		/* Already have it: fine if identical (a resend), fatal if not. */
		if (offset + n > u->received || memcmp(u->bundle + offset, payload, n) != 0) {
			r = err(UVCD_FW_ERROR_CONFLICT, offset);
			upload_failed(u, r.code);
		}
		u->activity_ms = now_ms();
	} else if (offset > u->received) {
		r = err(UVCD_FW_ERROR_GAP, u->received);
		upload_failed(u, r.code);
	} else {
		memcpy(u->bundle + offset, payload, n);
		u->received += n;
		u->done = u->received;
		u->activity_ms = now_ms();
	}
	pthread_mutex_unlock(&u->lock);
out:
	record(u, UVCD_FW_DATA_SELECTOR, f, len, seq, UVCD_FW_OP_DATA, r);
}

/* --------------------------------------------------------------------------
 * Public
 */
void uvcd_update_default_config(struct uvcd_update_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	fw_flash_mtd(&cfg->flash);
	cfg->cmdline_path = "/proc/cmdline";
	cfg->meminfo_path = "/proc/meminfo";
	cfg->mounts_path = "/proc/mounts";
	cfg->release_path = "/usr/lib/k1cam/firmware-release";
	cfg->serial_path = "/sys/module/g_webcam/parameters/iSerialNumber";
	cfg->data_root = "/overlay";
	cfg->run_dir = "/run/k1cam";
	cfg->receive_timeout_ms = 60000;
	cfg->dump_timeout_ms = 120000;
	cfg->memory_reserve_kb = 6144;
}

struct uvcd_update *uvcd_update_create(const struct uvcd_update_config *cfg)
{
	struct uvcd_update *u = calloc(1, sizeof(*u));
	if (!u)
		return NULL;
	if (cfg)
		u->cfg = *cfg;
	else
		uvcd_update_default_config(&u->cfg);
	if (!u->cfg.quiesce)
		u->cfg.quiesce = default_quiesce;
	if (!u->cfg.reboot)
		u->cfg.reboot = default_reboot;
	u->live_h = -1;
	u->boot.running = u->boot.active = u->boot.trial = FW_SLOT_NONE;
	pthread_mutex_init(&u->lock, NULL);
	pthread_cond_init(&u->cond, NULL);
	load_release(u);
	boot_refresh(u);
	if (run_file_exists(u, "trial-failed"))
		u->trial = TRIAL_FAILED; /* an earlier uvcd of this boot judged it */
	else if (run_file_exists(u, "confirmed"))
		u->trial = TRIAL_CONFIRMED;
	if (pthread_create(&u->worker, NULL, worker_main, u) == 0)
		u->worker_started = true;
	else
		LOGE("firmware: worker thread: %s", strerror(errno));
	LOGI("firmware: layout %s, boot %s, slot %s (confirmed %s, trial %s)",
	     fw_layout_id(u->boot.map.kind), boot_name(u->boot.boot), slot_name(u->boot.running),
	     slot_name(u->boot.active), slot_name(u->boot.trial));
	return u;
}

void uvcd_update_destroy(struct uvcd_update *u)
{
	if (!u)
		return;
	pthread_mutex_lock(&u->lock);
	u->stopping = true;
	pthread_cond_broadcast(&u->cond);
	pthread_mutex_unlock(&u->lock);
	if (u->worker_started)
		pthread_join(u->worker, NULL);
	release_session_buffers(u);
	pthread_cond_destroy(&u->cond);
	pthread_mutex_destroy(&u->lock);
	free(u);
}

int uvcd_update_setup(struct uvcd_update *u, uint8_t request, uint8_t selector, bool in,
		      uint16_t length, uint8_t *out)
{
	uint16_t n;

	if (!in) {
		if (request != UVC_SET_CUR || length != UVCD_FW_FRAME_SIZE ||
		    (selector != UVCD_FW_COMMAND_SELECTOR && selector != UVCD_FW_DATA_SELECTOR))
			return -1;
		return UVCD_FW_FRAME_SIZE;
	}
	switch (request) {
	case UVC_GET_INFO:
		out[0] = selector == UVCD_FW_COMMAND_SELECTOR ? UVC_CONTROL_CAP_SET :
			 selector == UVCD_FW_DATA_SELECTOR ? UVC_CONTROL_CAP_GET | UVC_CONTROL_CAP_SET :
							     UVC_CONTROL_CAP_GET;
		return length < 1 ? length : 1;
	case UVC_GET_LEN:
		uvcd_fw_put_le16(out, UVCD_FW_FRAME_SIZE);
		return length < 2 ? length : 2;
	case UVC_GET_CUR:
		n = length < UVCD_FW_FRAME_SIZE ? length : UVCD_FW_FRAME_SIZE;
		if (selector == UVCD_FW_STATUS_SELECTOR) {
			build_status(u, out);
			return n;
		}
		if (selector == UVCD_FW_DATA_SELECTOR) {
			if (!u->window_valid)
				set_window(u, 0, 0, 0, UVCD_FW_TARGET_NONE, NULL, 0);
			memcpy(out, u->window, UVCD_FW_FRAME_SIZE);
			return n;
		}
		return -1;
	default:
		return -1;
	}
}

void uvcd_update_data(struct uvcd_update *u, uint8_t selector, const uint8_t *data,
		      int32_t length)
{
	if (selector == UVCD_FW_COMMAND_SELECTOR)
		handle_command(u, data, length);
	else if (selector == UVCD_FW_DATA_SELECTOR)
		handle_data(u, data, length);
}

void uvcd_update_tick(struct uvcd_update *u)
{
	pthread_mutex_lock(&u->lock);
	int64_t idle = now_ms() - u->activity_ms;
	if ((u->phase == UVCD_FW_PHASE_RECEIVING && idle > u->cfg.receive_timeout_ms) ||
	    (u->phase == UVCD_FW_PHASE_READY && idle > u->cfg.dump_timeout_ms)) {
		LOGW("firmware: session %u abandoned by the host", u->session);
		end_session(u, UVCD_FW_PHASE_ERROR, UVCD_FW_ERROR_TIMEOUT);
	}
	pthread_mutex_unlock(&u->lock);
}

void uvcd_update_disconnect(struct uvcd_update *u)
{
	pthread_mutex_lock(&u->lock);
	if (u->phase == UVCD_FW_PHASE_RECEIVING || u->phase == UVCD_FW_PHASE_READY) {
		end_session(u, UVCD_FW_PHASE_ERROR, UVCD_FW_ERROR_DISCONNECTED);
	} else if (u->phase == UVCD_FW_PHASE_READING ||
		   (u->phase == UVCD_FW_PHASE_ACCEPTED && u->job == JOB_SNAPSHOT)) {
		/* A snapshot nobody will read. An install carries on: once it
		 * was handed over it no longer needs the host. */
		u->cancel = true;
		u->cancel_error = UVCD_FW_ERROR_DISCONNECTED;
	}
	pthread_mutex_unlock(&u->lock);
}

bool uvcd_update_persist_paused(struct uvcd_update *u)
{
	pthread_mutex_lock(&u->lock);
	bool paused = u->persist_paused;
	pthread_mutex_unlock(&u->lock);
	return paused;
}

bool uvcd_update_trial_wants_check(struct uvcd_update *u)
{
	pthread_mutex_lock(&u->lock);
	bool wants = u->trial == TRIAL_UNCHECKED && u->boot.boot == UVCD_FW_BOOT_TRIAL &&
		     !strcmp(u->boot.mode, "trial") && u->job == JOB_NONE;
	pthread_mutex_unlock(&u->lock);
	return wants;
}

void uvcd_update_trial_health(struct uvcd_update *u, bool ok, const char *what)
{
	pthread_mutex_lock(&u->lock);
	if (u->trial != TRIAL_UNCHECKED || u->job != JOB_NONE) {
		pthread_mutex_unlock(&u->lock);
		return;
	}
	if (ok) {
		u->trial = TRIAL_PROMOTING;
		u->job = JOB_PROMOTE;
		pthread_cond_broadcast(&u->cond);
		pthread_mutex_unlock(&u->lock);
		LOGI("firmware: trial healthy (%s); confirming the slot", what);
		return;
	}
	u->trial = TRIAL_FAILED;
	pthread_mutex_unlock(&u->lock);
	LOGE("firmware: trial failed its health check: %s", what);
	write_run_file(u, "trial-failed", what);
}

void uvcd_update_wait_idle(struct uvcd_update *u)
{
	pthread_mutex_lock(&u->lock);
	while (u->job != JOB_NONE)
		pthread_cond_wait(&u->cond, &u->lock);
	pthread_mutex_unlock(&u->lock);
}
