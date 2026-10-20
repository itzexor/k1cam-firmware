/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * firmware.c -- uvcdctl firmware: info, status, dump, upgrade, reboot, cancel.
 *
 * The protocol is in ../src/uvcd_firmware.h. Every COMMAND and DATA frame is
 * followed by STATUS reads until the camera reports that frame's sequence:
 * a SET_CUR that USB accepted only means the frame arrived. A frame whose
 * transfer failed is sent again unchanged; the camera runs a repeated frame
 * only once.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/usb/video.h>
#include <linux/uvcvideo.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "uvcdctl.h"
#include "../src/fw_bundle.h"
#include "../src/fw_crc.h"
#include "../src/fw_layout.h"
#include "../src/fw_sha256.h"

#define FW_EXIT_OK 0
#define FW_EXIT_FAILED 1
#define FW_EXIT_USAGE 2
#define FW_EXIT_ROLLED_BACK 3
#define FW_EXIT_UNCONFIRMED 4

#define ACK_TIMEOUT_MS 10000
#define SEND_TRIES 4
#define RECORD_MAX 4096

struct fwc {
	int fd;
	char path[64];
	uint32_t seq;
	uint8_t status[UVCD_FW_FRAME_SIZE];
};

static volatile sig_atomic_t interrupted;

static void on_interrupt(int sig)
{
	(void)sig;
	interrupted = 1;
}

static int64_t mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(int ms)
{
	struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000};
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR && !interrupted)
		;
}

/* --------------------------------------------------------------------------
 * Names
 */
static const char *phase_name(uint8_t phase)
{
	static const char *const names[] = {
		"idle", "complete", "error", "accepted", "receiving", "validating", "erasing",
		"programming", "verifying", "staged", "rebooting", "aborted", "reading", "ready",
	};
	return phase < sizeof(names) / sizeof(names[0]) ? names[phase] : "unknown";
}

static const char *error_name(uint8_t error)
{
	static const char *const names[] = {
		"none", "bad frame", "bad version", "unsupported", "stale sequence",
		"no such session", "busy", "not possible now", "out of range", "gap in data",
		"conflicting data", "checksum mismatch", "out of memory", "too large",
		"unsupported flash layout", "boot state does not allow it", "malformed bundle",
		"incompatible bundle", "digest mismatch", "damaged image", "flash access failed",
		"flash erase failed", "flash write failed", "flash verify failed",
		"boot environment failed", "preparing settings failed", "not cancellable",
		"timed out", "disconnected", "reboot failed",
	};
	return error < sizeof(names) / sizeof(names[0]) ? names[error] : "unknown";
}

static const char *boot_name(uint8_t boot)
{
	static const char *const names[] = {"unknown", "single", "confirmed", "trial",
					    "fallback"};
	return boot < sizeof(names) / sizeof(names[0]) ? names[boot] : "unknown";
}

static const char *slot_name(uint8_t slot)
{
	return slot == FW_SLOT_A ? "a" : slot == FW_SLOT_B ? "b" : "none";
}

static void print_error(const char *what, uint8_t code, uint32_t detail)
{
	fprintf(stderr, "uvcdctl: %s: %s (%u)", what, error_name(code), code);
	if (detail && (code == UVCD_FW_ERROR_MTD || code == UVCD_FW_ERROR_ERASE ||
		       code == UVCD_FW_ERROR_WRITE || code == UVCD_FW_ERROR_ENV ||
		       code == UVCD_FW_ERROR_OVERLAY || code == UVCD_FW_ERROR_REBOOT))
		fprintf(stderr, ": %s", strerror((int)detail));
	fputc('\n', stderr);
}

/* --------------------------------------------------------------------------
 * Transport
 */
static bool protocol_missing(int err)
{
	return err == ENOENT || err == EINVAL || err == EBADRQC;
}

static int fwc_probe(struct fwc *c)
{
	static const struct {
		uint8_t selector, caps;
	} want[] = {
		{UVCD_FW_COMMAND_SELECTOR, UVC_CONTROL_CAP_SET},
		{UVCD_FW_DATA_SELECTOR, UVC_CONTROL_CAP_GET | UVC_CONTROL_CAP_SET},
		{UVCD_FW_STATUS_SELECTOR, UVC_CONTROL_CAP_GET},
	};
	for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		uint8_t len[2] = {0}, info = 0;
		if (xu_query_bytes(c->fd, want[i].selector, UVC_GET_LEN, len, sizeof(len)) < 0 ||
		    xu_query_bytes(c->fd, want[i].selector, UVC_GET_INFO, &info, 1) < 0)
			return -1;
		if ((len[0] | len[1] << 8) != UVCD_FW_FRAME_SIZE ||
		    (info & want[i].caps) != want[i].caps) {
			errno = EPROTO;
			return -1;
		}
	}
	return 0;
}

static int fwc_status(struct fwc *c)
{
	if (xu_query_bytes(c->fd, UVCD_FW_STATUS_SELECTOR, UVC_GET_CUR, c->status,
			   UVCD_FW_FRAME_SIZE) < 0)
		return -1;
	if (!uvcd_fw_has_magic(c->status) ||
	    c->status[UVCD_FW_STATUS_VERSION] != UVCD_FW_PROTOCOL_VERSION) {
		errno = EPROTO;
		return -1;
	}
	return 0;
}

static int fwc_open(struct fwc *c, const char *device)
{
	memset(c, 0, sizeof(*c));
	c->fd = open_device(device, c->path, sizeof(c->path));
	if (c->fd < 0)
		return -1;
	if (fwc_probe(c) != 0 || fwc_status(c) != 0) {
		if (protocol_missing(errno))
			fprintf(stderr,
				"uvcdctl: %s: this camera's firmware has no firmware protocol "
				"(Extension Unit selectors 26-28); install a newer image by "
				"flashing it\n",
				c->path);
		else
			fprintf(stderr, "uvcdctl: %s: firmware protocol: %s\n", c->path,
				errno == EPROTO ? "unexpected reply" : strerror(errno));
		close(c->fd);
		c->fd = -1;
		return -1;
	}
	struct timeval tv;
	gettimeofday(&tv, NULL);
	c->seq = (uint32_t)getpid() * 2654435761u ^ (uint32_t)tv.tv_sec ^ (uint32_t)tv.tv_usec;
	return 0;
}

static void fwc_close(struct fwc *c)
{
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
}

static uint32_t next_seq(struct fwc *c)
{
	if (++c->seq == 0)
		c->seq = 1;
	return c->seq;
}

/* Send one frame and wait for its STATUS. Returns its result code (0
 * accepted), or -1 if the camera could not be reached (errno set). */
static int fwc_send(struct fwc *c, uint8_t selector, uint8_t *frame, uint32_t seq)
{
	int err = ETIMEDOUT;
	for (int attempt = 0; attempt < SEND_TRIES; attempt++) {
		if (xu_query_bytes(c->fd, selector, UVC_SET_CUR, frame, UVCD_FW_FRAME_SIZE) < 0) {
			err = errno;
			if (err == ENODEV || protocol_missing(err))
				break;
			/* The frame may still have arrived: look before resending. */
		}
		int64_t deadline = mono_ms() + ACK_TIMEOUT_MS;
		while (mono_ms() < deadline) {
			if (fwc_status(c) == 0) {
				if (uvcd_fw_get_le32(c->status + UVCD_FW_STATUS_SEQUENCE) == seq)
					return c->status[UVCD_FW_STATUS_RESULT];
				/* Older status: the frame never arrived; send it again. */
				break;
			}
			err = errno;
			if (err == ENODEV)
				goto out;
			sleep_ms(50);
		}
	}
out:
	errno = err;
	return -1;
}

static int fwc_command(struct fwc *c, uint8_t op, uint8_t target, uint8_t flags,
		       uint32_t session, uint32_t offset, uint32_t length, const uint8_t *digest)
{
	uint8_t f[UVCD_FW_FRAME_SIZE] = {0};
	uint32_t seq = next_seq(c);

	uvcd_fw_put_magic(f + UVCD_FW_COMMAND_MAGIC);
	f[UVCD_FW_COMMAND_VERSION] = UVCD_FW_PROTOCOL_VERSION;
	f[UVCD_FW_COMMAND_OPCODE] = op;
	f[UVCD_FW_COMMAND_TARGET] = target;
	f[UVCD_FW_COMMAND_FLAGS] = flags;
	uvcd_fw_put_le32(f + UVCD_FW_COMMAND_SEQUENCE, seq);
	uvcd_fw_put_le32(f + UVCD_FW_COMMAND_SESSION, session);
	uvcd_fw_put_le32(f + UVCD_FW_COMMAND_OFFSET, offset);
	uvcd_fw_put_le32(f + UVCD_FW_COMMAND_LENGTH, length);
	if (digest)
		memcpy(f + UVCD_FW_COMMAND_DIGEST, digest, FW_SHA256_SIZE);
	return fwc_send(c, UVCD_FW_COMMAND_SELECTOR, f, seq);
}

/* A command that has to be accepted. Prints why not. */
static bool command_ok(struct fwc *c, const char *what, int result)
{
	if (result == 0)
		return true;
	if (result < 0)
		fprintf(stderr, "uvcdctl: %s: %s\n", what, strerror(errno));
	else
		print_error(what, (uint8_t)result,
			    uvcd_fw_get_le32(c->status + UVCD_FW_STATUS_DETAIL));
	return false;
}

/* READ a window of target and fetch it. Returns the payload length (0 at
 * the end), or -1. Two transfers, not three: the window carries the READ's
 * sequence, so STATUS is only asked when the window is not the one asked
 * for (the READ was rejected, or never arrived). */
static int fwc_window(struct fwc *c, uint8_t target, uint32_t session, uint32_t offset,
		      uint8_t *out)
{
	for (int attempt = 0; attempt < SEND_TRIES; attempt++) {
		uint8_t f[UVCD_FW_FRAME_SIZE] = {0}, w[UVCD_FW_FRAME_SIZE];
		uint32_t seq = next_seq(c);
		uvcd_fw_put_magic(f + UVCD_FW_COMMAND_MAGIC);
		f[UVCD_FW_COMMAND_VERSION] = UVCD_FW_PROTOCOL_VERSION;
		f[UVCD_FW_COMMAND_OPCODE] = UVCD_FW_OP_READ;
		f[UVCD_FW_COMMAND_TARGET] = target;
		uvcd_fw_put_le32(f + UVCD_FW_COMMAND_SEQUENCE, seq);
		uvcd_fw_put_le32(f + UVCD_FW_COMMAND_SESSION, session);
		uvcd_fw_put_le32(f + UVCD_FW_COMMAND_OFFSET, offset);
		if (xu_query_bytes(c->fd, UVCD_FW_COMMAND_SELECTOR, UVC_SET_CUR, f, sizeof(f)) < 0 &&
		    (errno == ENODEV || protocol_missing(errno)))
			break;
		if (xu_query_bytes(c->fd, UVCD_FW_DATA_SELECTOR, UVC_GET_CUR, w, sizeof(w)) == 0) {
			uint8_t n = w[UVCD_FW_DATA_LENGTH];
			uint16_t crc = fw_crc16(FW_CRC16_INIT, w, UVCD_FW_DATA_CRC);
			if (n <= UVCD_FW_DATA_MAX)
				crc = fw_crc16(crc, w + UVCD_FW_DATA_PAYLOAD, n);
			if (n <= UVCD_FW_DATA_MAX && crc == uvcd_fw_get_le16(w + UVCD_FW_DATA_CRC) &&
			    uvcd_fw_get_le32(w + UVCD_FW_DATA_SEQUENCE) == seq &&
			    uvcd_fw_get_le32(w + UVCD_FW_DATA_SESSION) == session &&
			    uvcd_fw_get_le32(w + UVCD_FW_DATA_OFFSET) == offset &&
			    w[UVCD_FW_DATA_KIND] == target) {
				memcpy(out, w + UVCD_FW_DATA_PAYLOAD, n);
				return n;
			}
		} else if (errno == ENODEV) {
			break;
		}
		/* Not the window asked for: was the READ refused? */
		if (fwc_status(c) == 0 && uvcd_fw_get_le32(c->status + UVCD_FW_STATUS_SEQUENCE) == seq &&
		    c->status[UVCD_FW_STATUS_RESULT]) {
			print_error("read", c->status[UVCD_FW_STATUS_RESULT],
				    uvcd_fw_get_le32(c->status + UVCD_FW_STATUS_DETAIL));
			return -1;
		}
	}
	fprintf(stderr, "uvcdctl: read at %u: no valid reply\n", offset);
	return -1;
}

/* A whole text object (info record, manifest), NUL-terminated. */
static int fwc_object(struct fwc *c, uint8_t target, uint32_t session, char *out, size_t size)
{
	size_t len = 0;
	for (;;) {
		uint8_t buf[UVCD_FW_DATA_MAX];
		int n = fwc_window(c, target, session, (uint32_t)len, buf);
		if (n < 0)
			return -1;
		if (len + (size_t)n >= size) {
			fprintf(stderr, "uvcdctl: reply too long\n");
			return -1;
		}
		memcpy(out + len, buf, (size_t)n);
		len += (size_t)n;
		if (n < UVCD_FW_DATA_MAX)
			break;
	}
	out[len] = '\0';
	return 0;
}

static int fwc_info(struct fwc *c, char *record, size_t size)
{
	if (!command_ok(c, "info", fwc_command(c, UVCD_FW_OP_INFO, 0, 0, 0, 0, 0, NULL)))
		return -1;
	return fwc_object(c, UVCD_FW_TARGET_INFO, 0, record, size);
}

static bool record_value(const char *record, const char *key, char *out, size_t size)
{
	size_t key_len = strlen(key);
	for (const char *p = record; *p;) {
		size_t len = strcspn(p, "\n");
		if (len > key_len && !strncmp(p, key, key_len) && p[key_len] == '=') {
			size_t n = len - key_len - 1 < size - 1 ? len - key_len - 1 : size - 1;
			memcpy(out, p + key_len + 1, n);
			out[n] = '\0';
			return true;
		}
		p += len + (p[len] == '\n');
	}
	if (size)
		out[0] = '\0';
	return false;
}

/* --------------------------------------------------------------------------
 * Progress, on stderr only
 */
static void progress(const char *what, uint64_t done, uint64_t total)
{
	static int64_t last;
	static int last_pct = -1;
	int pct = total ? (int)(done * 100 / total) : 0;
	int64_t now = mono_ms();
	bool tty = isatty(STDERR_FILENO);

	if (tty ? now - last < 200 && done != total : pct / 10 == last_pct / 10)
		return;
	last = now;
	last_pct = pct;
	if (tty)
		fprintf(stderr, "\r%-12s %3d%%  %llu/%llu KiB  ", what, pct,
			(unsigned long long)(done / 1024), (unsigned long long)(total / 1024));
	else
		fprintf(stderr, "%s %d%%\n", what, pct);
}

static void progress_end(void)
{
	if (isatty(STDERR_FILENO))
		fputc('\n', stderr);
}

/* Poll STATUS while the operation is in one of the phases in busy[]. */
static int wait_while(struct fwc *c, const uint8_t *busy, size_t n, int64_t timeout_ms)
{
	int64_t deadline = mono_ms() + timeout_ms;
	uint8_t shown = 0xff;
	for (;;) {
		if (fwc_status(c) != 0) {
			if (errno == ENODEV || mono_ms() > deadline)
				return -1;
			sleep_ms(200);
			continue;
		}
		uint8_t phase = c->status[UVCD_FW_STATUS_PHASE];
		bool waiting = false;
		for (size_t i = 0; i < n; i++)
			waiting |= phase == busy[i];
		if (!waiting) {
			if (shown != 0xff)
				progress_end();
			return 0;
		}
		if (phase != shown && shown != 0xff)
			progress_end();
		shown = phase;
		progress(phase_name(phase), uvcd_fw_get_le32(c->status + UVCD_FW_STATUS_DONE),
			 uvcd_fw_get_le32(c->status + UVCD_FW_STATUS_TOTAL));
		if (interrupted || mono_ms() > deadline) {
			errno = interrupted ? EINTR : ETIMEDOUT;
			return -1;
		}
		uint16_t poll = uvcd_fw_get_le16(c->status + UVCD_FW_STATUS_POLL_MS);
		sleep_ms(poll < 20 ? 20 : poll > 1000 ? 1000 : poll);
	}
}

/* --------------------------------------------------------------------------
 * info, status
 */
static int fw_info(const char *device)
{
	struct fwc c;
	char record[RECORD_MAX];
	if (fwc_open(&c, device) != 0)
		return FW_EXIT_FAILED;
	int rc = fwc_info(&c, record, sizeof(record)) == 0 ? FW_EXIT_OK : FW_EXIT_FAILED;
	if (rc == FW_EXIT_OK)
		fputs(record, stdout);
	fwc_close(&c);
	return rc;
}

static void print_status(const uint8_t *s)
{
	uint32_t f = uvcd_fw_get_le32(s + UVCD_FW_STATUS_FEATURES);
	uint8_t phase = s[UVCD_FW_STATUS_PHASE], result = s[UVCD_FW_STATUS_RESULT];
	uint8_t error = s[UVCD_FW_STATUS_ERROR], op = s[UVCD_FW_STATUS_OPERATION];

	printf("protocol: %u\n", s[UVCD_FW_STATUS_VERSION]);
	printf("phase: %s (%u)\n", phase_name(phase), phase);
	printf("operation: %s (%u)\n",
	       op == UVCD_FW_OP_BEGIN_DUMP ? "dump" : op == UVCD_FW_OP_BEGIN_UPGRADE ? "upgrade" :
	       op == UVCD_FW_OP_ABORT ? "cancel" : op == UVCD_FW_OP_REBOOT ? "reboot" : "none",
	       op);
	if (phase == UVCD_FW_PHASE_ERROR)
		printf("error: %s (%u), detail %u\n", error_name(error), error,
		       uvcd_fw_get_le32(s + UVCD_FW_STATUS_DETAIL));
	printf("progress: %u/%u\n", uvcd_fw_get_le32(s + UVCD_FW_STATUS_DONE),
	       uvcd_fw_get_le32(s + UVCD_FW_STATUS_TOTAL));
	printf("session: %u\n", uvcd_fw_get_le32(s + UVCD_FW_STATUS_SESSION));
	printf("last-frame: sequence %u, opcode %u, %s (%u)\n",
	       uvcd_fw_get_le32(s + UVCD_FW_STATUS_SEQUENCE), s[UVCD_FW_STATUS_OPCODE],
	       result ? error_name(result) : "accepted", result);
	printf("boot: %s (%u)\n", boot_name(s[UVCD_FW_STATUS_BOOT]), s[UVCD_FW_STATUS_BOOT]);
	printf("slots: running %s, confirmed %s, trial %s\n",
	       slot_name(s[UVCD_FW_STATUS_RUNNING_SLOT]), slot_name(s[UVCD_FW_STATUS_ACTIVE_SLOT]),
	       slot_name(s[UVCD_FW_STATUS_TRIAL_SLOT]));
	printf("features:%s%s%s%s\n", f & UVCD_FW_FEATURE_INFO ? " info" : "",
	       f & UVCD_FW_FEATURE_DUMP ? " dump" : "", f & UVCD_FW_FEATURE_AB_SLOTS ? " ab" : "",
	       f & UVCD_FW_FEATURE_UPGRADE ? " upgrade" : "");
}

static int fw_status(const char *device)
{
	struct fwc c;
	if (fwc_open(&c, device) != 0)
		return FW_EXIT_FAILED;
	print_status(c.status);
	fwc_close(&c);
	return FW_EXIT_OK;
}

/* --------------------------------------------------------------------------
 * dump
 */
static int fw_dump(const char *device, bool live, const char *file)
{
	char part[PATH_MAX], manifest[512], digest_hex[2 * FW_SHA256_SIZE + 1] = "";
	char consistency[32] = "", size_text[16] = "";
	struct stat st;
	struct fwc c;
	int rc = FW_EXIT_FAILED, fd = -1;
	uint32_t session = 0, size = 0, got = 0;
	bool started = false, created = false;
	int64_t t0;

	if (snprintf(part, sizeof(part), "%s.part", file) >= (int)sizeof(part)) {
		fprintf(stderr, "uvcdctl: %s: name too long\n", file);
		return FW_EXIT_USAGE;
	}
	if (lstat(file, &st) == 0) {
		fprintf(stderr, "uvcdctl: %s exists; not overwriting it\n", file);
		return FW_EXIT_FAILED;
	}
	if (fwc_open(&c, device) != 0)
		return FW_EXIT_FAILED;
	fd = open(part, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0) {
		fprintf(stderr, "uvcdctl: %s: %s%s\n", part, strerror(errno),
			errno == EEXIST ? " (left by an earlier dump; remove it first)" : "");
		goto out;
	}
	created = true;

	int r = fwc_command(&c, UVCD_FW_OP_BEGIN_DUMP, UVCD_FW_TARGET_IMAGE,
			    live ? UVCD_FW_FLAG_ALLOW_LIVE : 0, 0, 0, 0, NULL);
	if (r == UVCD_FW_ERROR_NO_MEMORY) {
		fprintf(stderr, "uvcdctl: the camera has too little free RAM to copy its flash for "
				"a consistent dump; --live reads it in place instead (a diagnostic "
				"dump that may be inconsistent)\n");
		goto out;
	}
	if (!command_ok(&c, "dump", r))
		goto out;
	session = uvcd_fw_get_le32(c.status + UVCD_FW_STATUS_SESSION);
	started = true;
	static const uint8_t copying[] = {UVCD_FW_PHASE_ACCEPTED, UVCD_FW_PHASE_READING};
	if (wait_while(&c, copying, sizeof(copying), 120000) != 0 ||
	    c.status[UVCD_FW_STATUS_PHASE] != UVCD_FW_PHASE_READY) {
		if (c.status[UVCD_FW_STATUS_PHASE] == UVCD_FW_PHASE_ERROR)
			print_error("dump", c.status[UVCD_FW_STATUS_ERROR],
				    uvcd_fw_get_le32(c.status + UVCD_FW_STATUS_DETAIL));
		else
			fprintf(stderr, "uvcdctl: dump: snapshot not ready: %s\n",
				interrupted ? "interrupted" : strerror(errno));
		goto out;
	}
	if (fwc_object(&c, UVCD_FW_TARGET_MANIFEST, session, manifest, sizeof(manifest)) != 0)
		goto out;
	record_value(manifest, "size", size_text, sizeof(size_text));
	record_value(manifest, "sha256", digest_hex, sizeof(digest_hex));
	record_value(manifest, "consistency", consistency, sizeof(consistency));
	size = (uint32_t)strtoul(size_text, NULL, 10);
	if (size != FW_FLASH_SIZE) {
		fprintf(stderr, "uvcdctl: dump: unexpected flash size '%s'\n", size_text);
		goto out;
	}

	struct fw_sha256 sha;
	fw_sha256_init(&sha);
	t0 = mono_ms();
	while (got < size) {
		uint8_t buf[UVCD_FW_DATA_MAX];
		if (interrupted) {
			fprintf(stderr, "\nuvcdctl: dump interrupted\n");
			goto out;
		}
		int n = fwc_window(&c, UVCD_FW_TARGET_IMAGE, session, got, buf);
		if (n <= 0) {
			if (n == 0)
				fprintf(stderr, "uvcdctl: dump ended early at %u\n", got);
			goto out;
		}
		for (ssize_t w = 0; w < n;) {
			ssize_t m = pwrite(fd, buf + w, (size_t)(n - w), (off_t)got + w);
			if (m < 0 && errno == EINTR)
				continue;
			if (m <= 0) {
				fprintf(stderr, "uvcdctl: %s: %s\n", part, strerror(errno));
				goto out;
			}
			w += m;
		}
		fw_sha256_update(&sha, buf, (size_t)n);
		got += (uint32_t)n;
		progress("dumping", got, size);
	}
	progress_end();
	command_ok(&c, "end dump",
		   fwc_command(&c, UVCD_FW_OP_END_DUMP, 0, 0, session, 0, 0, NULL));
	started = false;

	uint8_t digest[FW_SHA256_SIZE];
	char hex[2 * FW_SHA256_SIZE + 1];
	fw_sha256_final(&sha, digest);
	fw_sha256_hex(digest, hex);
	if (fsync(fd) != 0 || close(fd) != 0) {
		fd = -1;
		fprintf(stderr, "uvcdctl: %s: %s\n", part, strerror(errno));
		goto out;
	}
	fd = -1;
	if (digest_hex[0] && strcmp(hex, digest_hex) != 0) {
		fprintf(stderr, "uvcdctl: dump: received data does not match the camera's digest\n");
		goto out;
	}
	/* link + unlink: never replaces a file that appeared meanwhile. */
	if (link(part, file) != 0) {
		fprintf(stderr, "uvcdctl: %s: %s\n", file, strerror(errno));
		goto out;
	}
	unlink(part);
	double secs = (double)(mono_ms() - t0) / 1000.0;
	printf("%s: %u bytes, sha256 %s, %s%s\n", file, size, hex, consistency,
	       strcmp(consistency, "snapshot") ? " (not a consistent snapshot)" : "");
	fprintf(stderr, "transfer: %.1f s, %.1f KiB/s\n", secs, secs > 0 ? size / 1024.0 / secs : 0);
	rc = FW_EXIT_OK;
out:
	if (started)
		fwc_command(&c, UVCD_FW_OP_ABORT, 0, 0, session, 0, 0, NULL);
	if (fd >= 0)
		close(fd);
	if (rc != FW_EXIT_OK && created && got)
		fprintf(stderr, "uvcdctl: partial dump left in %s (%u of %u bytes; not a backup)\n",
			part, got, size);
	else if (rc != FW_EXIT_OK && created)
		unlink(part);
	fwc_close(&c);
	return rc;
}

/* --------------------------------------------------------------------------
 * upgrade
 */
static uint8_t *read_file(const char *path, uint32_t *len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	struct stat st;
	if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > 64 * 1024 * 1024) {
		fprintf(stderr, "uvcdctl: %s: %s\n", path, fd < 0 ? strerror(errno) : "bad size");
		if (fd >= 0)
			close(fd);
		return NULL;
	}
	uint8_t *buf = malloc((size_t)st.st_size);
	size_t used = 0;
	while (buf && used < (size_t)st.st_size) {
		ssize_t n = read(fd, buf + used, (size_t)st.st_size - used);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			free(buf);
			buf = NULL;
		} else {
			used += (size_t)n;
		}
	}
	close(fd);
	if (!buf)
		fprintf(stderr, "uvcdctl: %s: read failed\n", path);
	*len = (uint32_t)used;
	return buf;
}

/* Find the camera with this serial number again (after its reboot). */
static int fwc_find(struct fwc *c, const char *serial, char *record, size_t size)
{
	int matches = 0, found = -1;
	char path[32], value[64];

	for (int i = 0; i < 64; i++) {
		snprintf(path, sizeof(path), "/dev/video%d", i);
		int fd = open(path, O_RDWR);
		if (fd < 0)
			continue;
		struct fwc probe = {.fd = fd, .seq = c->seq};
		snprintf(probe.path, sizeof(probe.path), "%s", path);
		if (!is_our_camera(fd) || fwc_probe(&probe) != 0 || fwc_status(&probe) != 0 ||
		    fwc_info(&probe, record, size) != 0) {
			close(fd);
			continue;
		}
		record_value(record, "serial", value, sizeof(value));
		if (serial[0] ? strcmp(value, serial) != 0 : false) {
			close(fd);
			continue;
		}
		matches++;
		if (found >= 0)
			close(found);
		found = fd;
		c->seq = probe.seq;
		snprintf(c->path, sizeof(c->path), "%s", path);
		memcpy(c->status, probe.status, sizeof(c->status));
	}
	if (matches != 1) {
		if (found >= 0)
			close(found);
		if (matches > 1)
			fprintf(stderr, "uvcdctl: more than one camera matches; use cameras with "
					"serial numbers\n");
		return -1;
	}
	c->fd = found;
	return fwc_info(c, record, size);
}

/* After the reboot: wait for the camera to come back on the new build and
 * confirm it, or to come back on the old one (rolled back). */
static int wait_for_outcome(struct fwc *c, const char *serial, const char *build,
			    int timeout_s)
{
	char record[RECORD_MAX], value[80], boot[16], trial[8];
	int64_t deadline = mono_ms() + (int64_t)timeout_s * 1000;
	const char *last = "";

	fprintf(stderr, "waiting for the camera to boot the trial and confirm it "
			"(up to %d s)...\n", timeout_s);
	while (mono_ms() < deadline && !interrupted) {
		if (c->fd < 0 && fwc_find(c, serial, record, sizeof(record)) != 0) {
			fwc_close(c);
			sleep_ms(1000);
			continue;
		}
		if (c->fd >= 0 && fwc_info(c, record, sizeof(record)) != 0) {
			fwc_close(c);
			sleep_ms(1000);
			continue;
		}
		record_value(record, "build", value, sizeof(value));
		record_value(record, "boot", boot, sizeof(boot));
		record_value(record, "trial_slot", trial, sizeof(trial));
		bool new_build = !strcmp(value, build);
		if (new_build && !strcmp(boot, "confirmed")) {
			char slot[8];
			record_value(record, "running_slot", slot, sizeof(slot));
			printf("confirmed: %s runs %s in slot %s\n", c->path, build, slot);
			return FW_EXIT_OK;
		}
		if (!new_build && !strcmp(boot, "confirmed") && !trial[0]) {
			printf("rolled back: the trial of %s failed; the camera runs %s again\n", build,
			       value);
			return FW_EXIT_ROLLED_BACK;
		}
		const char *now = new_build ? "trial running, not confirmed yet" :
				  trial[0] ? "trial scheduled, camera not rebooted yet" : boot;
		if (strcmp(now, last))
			fprintf(stderr, "%s\n", now);
		last = now;
		sleep_ms(1000);
	}
	printf("not confirmed: %s\n", interrupted ? "interrupted" :
	       c->fd < 0 ? "the camera did not come back" : "still waiting for the trial");
	return FW_EXIT_UNCONFIRMED;
}

static int fw_reboot_and_wait(struct fwc *c, uint32_t session, const char *serial,
			      const char *build, int timeout_s)
{
	if (!command_ok(c, "reboot",
			fwc_command(c, UVCD_FW_OP_REBOOT, 0, 0, session, 0, 0, NULL)))
		return FW_EXIT_UNCONFIRMED;
	/* It disconnects when it actually reboots. */
	int64_t gone = mono_ms() + 30000;
	while (mono_ms() < gone && !interrupted) {
		if (fwc_status(c) != 0 && errno == ENODEV)
			break;
		if (c->status[UVCD_FW_STATUS_PHASE] == UVCD_FW_PHASE_ERROR) {
			print_error("reboot", c->status[UVCD_FW_STATUS_ERROR],
				    uvcd_fw_get_le32(c->status + UVCD_FW_STATUS_DETAIL));
			return FW_EXIT_UNCONFIRMED;
		}
		sleep_ms(250);
	}
	fwc_close(c);
	if (!build)
		return FW_EXIT_OK;
	return wait_for_outcome(c, serial, build, timeout_s);
}

static int fw_upgrade(const char *device, const char *file, bool reboot, int timeout_s)
{
	char record[RECORD_MAX], why[160], value[80], serial[64], target[8];
	struct fw_bundle b;
	struct fwc c = {.fd = -1};
	uint32_t len, session = 0;
	uint8_t digest[FW_SHA256_SIZE];
	int rc = FW_EXIT_FAILED;
	bool receiving = false;

	uint8_t *bundle = read_file(file, &len);
	if (!bundle)
		return FW_EXIT_FAILED;
	enum fw_bundle_status st = fw_bundle_parse(bundle, len, &b, why, sizeof(why));
	if (st != FW_BUNDLE_OK) {
		fprintf(stderr, "uvcdctl: %s: %s: %s\n", file, fw_bundle_status_name(st), why);
		goto out;
	}
	if (fwc_open(&c, device) != 0 || fwc_info(&c, record, sizeof(record)) != 0)
		goto out;

	/* Everything the camera would refuse, refused here first. */
	record_value(record, "layout", value, sizeof(value));
	if (strcmp(value, FW_LAYOUT_AB_ID) != 0) {
		fprintf(stderr, "uvcdctl: the camera's flash layout is %s, not %s: install the A/B "
				"image (k1cam.bin) once by flashing it, then update with bundles\n",
			value, FW_LAYOUT_AB_ID);
		goto out;
	}
	record_value(record, "boot_compat", value, sizeof(value));
	st = fw_bundle_compatible(&b, (uint32_t)strtoul(value, NULL, 10), why, sizeof(why));
	if (st != FW_BUNDLE_OK) {
		fprintf(stderr, "uvcdctl: %s: %s\n", file, why);
		goto out;
	}
	record_value(record, "features", value, sizeof(value));
	if (!strstr(value, "upgrade")) {
		char boot[16], trial[8];
		record_value(record, "boot", boot, sizeof(boot));
		record_value(record, "trial_slot", trial, sizeof(trial));
		if (trial[0])
			fprintf(stderr, "uvcdctl: a trial of slot %s is pending: let it confirm "
					"(uvcdctl firmware reboot) or withdraw it (uvcdctl firmware "
					"cancel) first\n", trial);
		else
			fprintf(stderr, "uvcdctl: the camera cannot update now (boot: %s); it must "
					"run its confirmed slot\n", boot);
		goto out;
	}
	record_value(record, "build", value, sizeof(value));
	if (!strcmp(value, b.build)) {
		fprintf(stderr, "uvcdctl: the camera already runs %s\n", b.build);
		goto out;
	}
	record_value(record, "serial", serial, sizeof(serial));
	record_value(record, "active_slot", target, sizeof(target));
	snprintf(target, sizeof(target), "%s", !strcmp(target, "a") ? "b" : "a");
	fprintf(stderr, "installing %s into slot %s (camera runs %s)\n", b.build, target, value);

	fw_sha256(bundle, len, digest);
	if (!command_ok(&c, "begin upgrade",
			fwc_command(&c, UVCD_FW_OP_BEGIN_UPGRADE, 0, 0, 0, 0, len, digest)))
		goto out;
	session = uvcd_fw_get_le32(c.status + UVCD_FW_STATUS_SESSION);
	receiving = true;

	int64_t t0 = mono_ms();
	for (uint32_t off = 0; off < len;) {
		uint8_t f[UVCD_FW_FRAME_SIZE] = {0};
		uint8_t n = len - off < UVCD_FW_DATA_MAX ? (uint8_t)(len - off) : UVCD_FW_DATA_MAX;
		uint32_t seq = next_seq(&c);
		if (interrupted) {
			fprintf(stderr, "\nuvcdctl: upload interrupted\n");
			goto out;
		}
		uvcd_fw_put_le32(f + UVCD_FW_DATA_SESSION, session);
		uvcd_fw_put_le32(f + UVCD_FW_DATA_OFFSET, off);
		uvcd_fw_put_le32(f + UVCD_FW_DATA_SEQUENCE, seq);
		f[UVCD_FW_DATA_LENGTH] = n;
		memcpy(f + UVCD_FW_DATA_PAYLOAD, bundle + off, n);
		uint16_t crc = fw_crc16(FW_CRC16_INIT, f, UVCD_FW_DATA_CRC);
		uvcd_fw_put_le16(f + UVCD_FW_DATA_CRC, fw_crc16(crc, f + UVCD_FW_DATA_PAYLOAD, n));
		int r = fwc_send(&c, UVCD_FW_DATA_SELECTOR, f, seq);
		if (!command_ok(&c, "upload", r))
			goto out;
		off += n;
		progress("uploading", off, len);
	}
	progress_end();
	double secs = (double)(mono_ms() - t0) / 1000.0;
	fprintf(stderr, "upload: %.1f s, %.1f KiB/s\n", secs, secs > 0 ? len / 1024.0 / secs : 0);

	if (!command_ok(&c, "finish upgrade",
			fwc_command(&c, UVCD_FW_OP_FINISH_UPGRADE, 0, 0, session, 0, len, NULL)))
		goto out;
	static const uint8_t installing[] = {UVCD_FW_PHASE_ACCEPTED, UVCD_FW_PHASE_VALIDATING,
					     UVCD_FW_PHASE_ERASING, UVCD_FW_PHASE_PROGRAMMING,
					     UVCD_FW_PHASE_VERIFYING};
	if (wait_while(&c, installing, sizeof(installing), 600000) != 0) {
		fprintf(stderr, "uvcdctl: install: %s\n",
			interrupted ? "interrupted" : strerror(errno));
		goto out;
	}
	uint8_t phase = c.status[UVCD_FW_STATUS_PHASE];
	receiving = false;
	if (phase != UVCD_FW_PHASE_STAGED) {
		if (phase == UVCD_FW_PHASE_ERROR)
			print_error("install", c.status[UVCD_FW_STATUS_ERROR],
				    uvcd_fw_get_le32(c.status + UVCD_FW_STATUS_DETAIL));
		else
			fprintf(stderr, "uvcdctl: install ended %s\n", phase_name(phase));
		goto out;
	}
	printf("staged: %s is in slot %s and scheduled as a trial; not confirmed yet\n",
	       b.build, target);
	/* Before the wait's progress on stderr, also when stdout is a pipe. */
	fflush(stdout);
	if (!reboot) {
		printf("reboot the camera, or run 'uvcdctl firmware reboot', to start the trial\n");
		rc = FW_EXIT_OK;
		goto out;
	}
	rc = fw_reboot_and_wait(&c, session, serial, b.build, timeout_s);
out:
	if (receiving && session)
		fwc_command(&c, UVCD_FW_OP_ABORT, 0, 0, session, 0, 0, NULL);
	fwc_close(&c);
	free(bundle);
	return rc;
}

/* --------------------------------------------------------------------------
 * reboot, cancel
 */
static int fw_reboot(const char *device)
{
	struct fwc c;
	if (fwc_open(&c, device) != 0)
		return FW_EXIT_FAILED;
	if (c.status[UVCD_FW_STATUS_TRIAL_SLOT] == FW_SLOT_NONE) {
		fprintf(stderr, "uvcdctl: no trial is scheduled\n");
		fwc_close(&c);
		return FW_EXIT_FAILED;
	}
	int rc = fw_reboot_and_wait(&c, 0, "", NULL, 0);
	if (rc == FW_EXIT_OK)
		printf("rebooting into the trial; 'uvcdctl firmware info' shows when it is "
		       "confirmed\n");
	return rc;
}

static int fw_cancel(const char *device)
{
	struct fwc c;
	if (fwc_open(&c, device) != 0)
		return FW_EXIT_FAILED;
	uint8_t phase = c.status[UVCD_FW_STATUS_PHASE];
	uint32_t session = uvcd_fw_get_le32(c.status + UVCD_FW_STATUS_SESSION);
	bool active = session && phase != UVCD_FW_PHASE_COMPLETE && phase != UVCD_FW_PHASE_ERROR &&
		      phase != UVCD_FW_PHASE_ABORTED && phase != UVCD_FW_PHASE_IDLE;
	int rc = FW_EXIT_FAILED;

	if (!active && c.status[UVCD_FW_STATUS_TRIAL_SLOT] == FW_SLOT_NONE) {
		printf("nothing to cancel\n");
		rc = FW_EXIT_OK;
		goto out;
	}
	if (!command_ok(&c, "cancel",
			fwc_command(&c, UVCD_FW_OP_ABORT, 0, UVCD_FW_FLAG_UNSCHEDULE,
				    active ? session : 0, 0, 0, NULL)))
		goto out;
	static const uint8_t busy[] = {UVCD_FW_PHASE_ACCEPTED, UVCD_FW_PHASE_READING,
				       UVCD_FW_PHASE_VALIDATING, UVCD_FW_PHASE_ERASING,
				       UVCD_FW_PHASE_PROGRAMMING, UVCD_FW_PHASE_VERIFYING};
	if (wait_while(&c, busy, sizeof(busy), 120000) != 0)
		goto out;
	phase = c.status[UVCD_FW_STATUS_PHASE];
	if (phase == UVCD_FW_PHASE_ERROR) {
		print_error("cancel", c.status[UVCD_FW_STATUS_ERROR],
			    uvcd_fw_get_le32(c.status + UVCD_FW_STATUS_DETAIL));
		goto out;
	}
	printf("cancelled%s\n", c.status[UVCD_FW_STATUS_TRIAL_SLOT] == FW_SLOT_NONE ?
	       "; no trial is scheduled" : "");
	rc = FW_EXIT_OK;
out:
	fwc_close(&c);
	return rc;
}

/* --------------------------------------------------------------------------
 * Entry
 */
static int usage_error(const char *msg)
{
	fprintf(stderr, "uvcdctl: firmware: %s (see 'uvcdctl help firmware')\n", msg);
	return FW_EXIT_USAGE;
}

int cmd_firmware(int argc, char **argv, const char *device)
{
	const char *sub = argv[0], *file = NULL;
	bool live = false, reboot = true;
	int timeout_s = 600;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--live") && !strcmp(sub, "dump")) {
			live = true;
		} else if (!strcmp(argv[i], "--no-reboot") && !strcmp(sub, "upgrade")) {
			reboot = false;
		} else if (!strcmp(argv[i], "--wait-timeout") && !strcmp(sub, "upgrade") &&
			   i + 1 < argc) {
			char *end;
			long t = strtol(argv[++i], &end, 10);
			if (*end || t < 1 || t > 86400)
				return usage_error("--wait-timeout takes 1-86400 seconds");
			timeout_s = (int)t;
		} else if (argv[i][0] == '-' || file) {
			return usage_error("unexpected argument");
		} else {
			file = argv[i];
		}
	}
	bool needs_file = !strcmp(sub, "dump") || !strcmp(sub, "upgrade");
	if (needs_file != (file != NULL))
		return usage_error(needs_file ? "FILE missing" : "unexpected argument");

	struct sigaction sa = {.sa_handler = on_interrupt};
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	interrupted = 0;

	if (!strcmp(sub, "info"))
		return fw_info(device);
	if (!strcmp(sub, "status"))
		return fw_status(device);
	if (!strcmp(sub, "dump"))
		return fw_dump(device, live, file);
	if (!strcmp(sub, "upgrade"))
		return fw_upgrade(device, file, reboot, timeout_s);
	if (!strcmp(sub, "reboot"))
		return fw_reboot(device);
	if (!strcmp(sub, "cancel"))
		return fw_cancel(device);
	return usage_error("unknown subcommand");
}
