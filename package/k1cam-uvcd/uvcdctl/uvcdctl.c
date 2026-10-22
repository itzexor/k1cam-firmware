/*
 * uvcdctl -- host-side control CLI for k1cam-uvcd
 *
 * Everything goes over the UVC control paths on a single V4L2 fd. There is
 * no side channel and no libusb:
 *
 *   standard controls   -> native V4L2 controls (UVC Processing Unit
 *                          and Camera Terminal)
 *   custom ISP controls -> UVC Extension Unit 4, via UVCIOC_CTRL_QUERY
 *   encoder settings    -> UVC Extension Unit 4, via UVCIOC_CTRL_QUERY
 *   factory reset       -> UVC Extension Unit 4, selector 10
 *   H.264 keyframe      -> UVC Extension Unit 4, selector 20
 *
 * The two control lists are mutually exclusive: a knob is either a standard
 * V4L2 control or an XU control, never both, so there is exactly one way to
 * reach any given setting and one runtime state behind it.
 *
 * UVCIOC_CTRL_QUERY talks to the XU directly through the kernel's uvcvideo
 * driver, so no UVCIOC_CTRL_MAP is needed and the custom controls stay out
 * of the V4L2 control list (where they would be duplicates).
 *
 * Settings persist on their own -- the daemon writes them back whenever it
 * accepts a change. There is deliberately no "save" command.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/usb/video.h>
#include <linux/uvcvideo.h>
#include <linux/videodev2.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "uvcdctl.h"

#define UVCD_XU_RESET_SELECTOR 10
#define UVCD_XU_KEYFRAME_SELECTOR 20
#define UVCD_SOCKET_PATH "/var/run/uvcd.sock"
/* Probe selector for device discovery: one of the original nine, so an
 * older camera is still recognised (and then reports what it lacks). */
#define UVCD_XU_PROBE_SELECTOR 9
#define UVCD_XU_VALUE_SIZE 2

enum ctrl_kind {
	CTRL_STANDARD, /* native V4L2 control (UVC Processing Unit) */
	CTRL_CAMERA,   /* native V4L2 control (UVC Camera Terminal) */
	CTRL_CUSTOM,   /* UVC Extension Unit 4 control */
};

struct ctrl_def {
	unsigned id; /* stable user-facing number; usable anywhere a name is */
	const char *name;
	enum ctrl_kind kind;
	uint32_t cid;      /* CTRL_STANDARD: V4L2 CID */
	uint8_t selector;  /* CTRL_CUSTOM: XU selector */
	const char *help;  /* what the values mean; printed by describe */
};

/* IDs are assigned by hand and never reused or renumbered -- they are what
 * people type and put in scripts. Grouped with gaps so each group can grow:
 *
 *    1-19  standard controls (UVC Processing Unit and Camera Terminal /
 *          native V4L2)
 *   20-39  image and ISP controls (Extension Unit)
 *   40-49  H.264 encoder (Extension Unit)
 *   50-59  MJPEG encoder (Extension Unit)
 *
 * The standard controls keep their V4L2 names; the Extension Unit ones are
 * named for what they do. */
static const struct ctrl_def ctrl_defs[] = {
	{1, "brightness", CTRL_STANDARD, V4L2_CID_BRIGHTNESS, 0,
	 "0-255, 128: default"},
	{2, "contrast", CTRL_STANDARD, V4L2_CID_CONTRAST, 0,
	 "0-255, 128: default"},
	{3, "saturation", CTRL_STANDARD, V4L2_CID_SATURATION, 0,
	 "0-255, 128: default"},
	{4, "sharpness", CTRL_STANDARD, V4L2_CID_SHARPNESS, 0,
	 "0-180, 128: default; more made MJPEG frames too big for USB"},
	{5, "hue", CTRL_STANDARD, V4L2_CID_HUE, 0,
	 "-128 to 127, 0: default"},
	{6, "gamma", CTRL_STANDARD, V4L2_CID_GAMMA, 0,
	 "40-500: gamma x100, 100: factory curve"},
	/* 7 was backlight: at any strength auto-exposure hunted. Retired. */
	{8, "power-line-frequency", CTRL_STANDARD, V4L2_CID_POWER_LINE_FREQUENCY, 0,
	 "0: off, 1: 50 Hz, 2: 60 Hz"},
	{9, "white-balance-auto", CTRL_STANDARD, V4L2_CID_AUTO_WHITE_BALANCE, 0,
	 "0: manual (white-balance-temperature), 1: auto"},
	{10, "white-balance-temperature", CTRL_STANDARD, V4L2_CID_WHITE_BALANCE_TEMPERATURE, 0,
	 "2800-7500: Kelvin; manual mode only"},
	/* V4L2's auto-exposure menu: uvcvideo maps it onto UVC's mode bits. */
	{11, "auto-exposure", CTRL_CAMERA, V4L2_CID_EXPOSURE_AUTO, 0,
	 "1: manual (exposure-time), 3: auto"},
	{12, "exposure-priority", CTRL_CAMERA, V4L2_CID_EXPOSURE_AUTO_PRIORITY, 0,
	 "0: constant frame rate, 1: frame rate may drop to expose longer"},
	{13, "exposure-time", CTRL_CAMERA, V4L2_CID_EXPOSURE_ABSOLUTE, 0,
	 "1-655: 100 us units; manual mode only, auto reads back its own"},
	{14, "zoom", CTRL_CAMERA, V4L2_CID_ZOOM_ABSOLUTE, 0,
	 "100-400: digital zoom x100, 100: full view; at most 290 at 1080p"},
	{15, "pan", CTRL_CAMERA, V4L2_CID_PAN_ABSOLUTE, 0,
	 "-36000 to 36000: moves a zoomed view, positive is right"},
	{16, "tilt", CTRL_CAMERA, V4L2_CID_TILT_ABSOLUTE, 0,
	 "-36000 to 36000: moves a zoomed view, positive is up"},

	{20, "exposure-compensation", CTRL_CUSTOM, 0, 3,
	 "0-255, 128: none, higher is brighter"},
	{21, "max-analog-gain", CTRL_CUSTOM, 0, 1,
	 "0-223: analog gain ceiling, 32 per doubling; 160: 32x (default), 216-223: ~107x, the most"},
	{22, "max-digital-gain", CTRL_CUSTOM, 0, 2,
	 "0-160: digital gain ceiling, 32 per doubling; 80: ~5.66x (default)"},
	{23, "spatial-denoise", CTRL_CUSTOM, 0, 4,
	 "0-255: strength, 128: as tuned, 0: default"},
	{24, "temporal-denoise", CTRL_CUSTOM, 0, 5,
	 "0-255: strength, 128: as tuned"},
	{25, "defective-pixel-correction", CTRL_CUSTOM, 0, 6,
	 "0-255: strength, 128: as tuned"},
	{26, "dynamic-range-compression", CTRL_CUSTOM, 0, 7,
	 "0-255: strength, 128: as tuned; no effect with the shipped sensor tuning"},
	{28, "highlight-suppression", CTRL_CUSTOM, 0, 9,
	 "0: off, 1-10: strength"},
	/* 27 was defog-strength, which had no effect; 29 and 30 were
	 * horizontal-flip and vertical-flip. All retired, not reused. UVC
	 * does define rotation (Roll), but V4L2 has no control for it, so it
	 * rides in the Extension Unit where Linux can reach it. */
	{31, "rotation", CTRL_CUSTOM, 0, 22,
	 "0 or 180: degrees"},
	{32, "metering", CTRL_CUSTOM, 0, 21,
	 "0: tuning default, 1: average, 2: center-weighted, 3: spot (of the view), "
	 "4: subject (what stands out bright, kept from clipping)"},

	{40, "h264-bitrate-kbps", CTRL_CUSTOM, 0, 13,
	 "0: automatic, 1-16000: target kbps"},
	{41, "h264-rate-control", CTRL_CUSTOM, 0, 14,
	 "0: CBR, 1: VBR, 2: capped VBR"},
	/* 42 was h264-gop-frames: the camera now sends a keyframe every second,
	 * the only GOP its encoder makes reliably. Retired, not reused. */
	{43, "h264-min-qp", CTRL_CUSTOM, 0, 16,
	 "-1: automatic (20), 0-51: lowest QP (best quality)"},
	{44, "h264-max-qp", CTRL_CUSTOM, 0, 17,
	 "-1: SDK default, 0-51: highest QP (worst quality)"},
	{45, "h264-profile", CTRL_CUSTOM, 0, 18,
	 "0: baseline, 1: main, 2: high"},

	{50, "mjpeg-quality", CTRL_CUSTOM, 0, 19,
	 "1-100: higher is better"},
};
#define CTRL_COUNT (sizeof(ctrl_defs) / sizeof(ctrl_defs[0]))

struct command_def {
	const char *name;
	const char *args;
	unsigned min_args, max_args;
	const char *summary;
	const char *description;
};

static const struct command_def commands[] = {
	{"get", "<key>", 1, 1, "read one control",
	 "Read the current value of one control.\n"
	 "<key> is a control name or numeric ID from list.\n"
	 "USB standard controls may reflect the host driver's cache; local reads\n"
	 "use the daemon's control state.\n\n"
	 "Examples:\n  uvcdctl get brightness\n  uvcdctl get 1\n"},
	{"set", "<key> <value>", 2, 2, "change one control (persists automatically)",
	 "Change one control to a numeric value. Accepted changes persist\n"
	 "automatically; there is no save step. USB writes are checked by readback.\n"
	 "<key> is a control name or numeric ID from list. Use describe <key>\n"
	 "to see the range, default and meaning of its values.\n\n"
	 "Examples:\n  uvcdctl set brightness 128\n  uvcdctl set 1 128\n"},
	{"list", "[key]", 0, 1, "show values, ranges and defaults (all if no key)",
	 "Show each control's ID, name, unit, current value, range and default.\n"
	 "Omit <key> for all controls, or pass a control name or numeric ID.\n"
	 "Changed values are highlighted on a terminal unless NO_COLOR is set.\n"
	 "Use describe for the same table with value descriptions.\n\n"
	 "Examples:\n  uvcdctl list\n  uvcdctl list brightness\n"},
	{"describe", "[key]", 0, 1, "list with value descriptions",
	 "Show the list table with an extra column explaining control values.\n"
	 "Omit <key> for all controls, or pass a control name or numeric ID.\n"
	 "Like list, this reads current values, ranges and defaults from the camera.\n\n"
	 "Examples:\n  uvcdctl describe\n  uvcdctl describe h264-rate-control\n"},
	{"reset", "", 0, 0, "restore factory defaults",
	 "Restore every control to its factory default and persist the reset\n"
	 "immediately. This affects all controls, including encoder settings.\n\n"
	 "Example:\n  uvcdctl reset\n"},
	{"keyframe", "", 0, 0, "make the H.264 stream emit a keyframe now",
	 "Request an IDR keyframe from the running H.264 encoder.\n"
	 "This has no effect unless an H.264 stream is active.\n\n"
	 "Example:\n  uvcdctl keyframe\n"},
	{"firmware", "<subcommand> [args]", 1, 5, "firmware info, flash dump and A/B update",
	 "  info                 what the camera runs: build, flash layout, slots\n"
	 "  status               the firmware protocol's current state (read only)\n"
	 "  dump [--live] FILE   save the whole 8 MiB flash to FILE\n"
	 "  upgrade [--no-reboot] [--wait-timeout SECONDS] FILE\n"
	 "                       install a k1cam.upd bundle into the inactive slot,\n"
	 "                       boot it as a trial and wait until it is confirmed\n"
	 "  reboot               start a trial that was installed with --no-reboot\n"
	 "  cancel               stop the current dump or update, or withdraw a\n"
	 "                       trial that is scheduled but not confirmed\n\n"
	 "dump copies the flash into the camera's RAM first and refuses if there is\n"
	 "not enough; --live reads it in place instead, a diagnostic dump that may\n"
	 "be inconsistent. The file appears only once complete and verified; until\n"
	 "then data goes to FILE.part, which is left behind if the dump fails.\n\n"
	 "upgrade never writes the running or confirmed slot, the bootloader or\n"
	 "the settings partition. The new slot is confirmed by the camera itself\n"
	 "after it booted and encoded video; a trial that fails three boots falls\n"
	 "back to the previous slot. Exit status: 0 confirmed (or staged with\n"
	 "--no-reboot), 1 failed before a trial was scheduled (the camera still\n"
	 "boots what it ran), 3 rolled back, 4 not confirmed (still pending, or\n"
	 "the camera did not come back).\n\n"
	 "Examples:\n  uvcdctl firmware info\n  uvcdctl firmware dump k1-backup.bin\n"
	 "  uvcdctl firmware upgrade output/k1cam/images/k1cam.upd\n"},
	{"help", "[command]", 0, 1, "show general or command-specific help",
	 "Omit <command> for the command list, or name a command to see its\n"
	 "syntax, description and examples. Help works without a camera.\n\n"
	 "Examples:\n  uvcdctl help\n  uvcdctl help set\n"},
};
#define COMMAND_COUNT (sizeof(commands) / sizeof(commands[0]))

static const struct command_def *find_command(const char *name)
{
	for (size_t i = 0; i < COMMAND_COUNT; i++) {
		if (!strcmp(name, commands[i].name))
			return &commands[i];
	}
	return NULL;
}

static void usage(FILE *out, const char *prog)
{
	fprintf(out, "usage: %s [--device PATH] <command> [args]\ncommands:\n", prog);
	for (size_t i = 0; i < COMMAND_COUNT; i++) {
		char syntax[64];
		snprintf(syntax, sizeof(syntax), "%s%s%s", commands[i].name,
			 *commands[i].args ? " " : "", commands[i].args);
		fprintf(out, "  %-18s %s\n", syntax, commands[i].summary);
	}
	fprintf(out,
		"\n"
		"<key> is a control name or its numeric id (see list).\n"
		"The camera is found automatically; override with --device /dev/videoN.\n"
		"Use %s help <command> for details and examples.\n",
		prog);
}

static void command_usage(FILE *out, const char *prog, const struct command_def *cmd)
{
	fprintf(out, "usage: %s [--device PATH] %s%s%s\n\n%s", prog, cmd->name,
		*cmd->args ? " " : "", cmd->args, cmd->description);
}

int xioctl(int fd, unsigned long request, void *arg)
{
	int r;
	do {
		r = ioctl(fd, request, arg);
	} while (r < 0 && errno == EINTR);
	return r;
}

static const struct ctrl_def *find_ctrl(const char *key)
{
	char *end;
	unsigned long id = strtoul(key, &end, 10);
	bool numeric = (*key != '\0' && *end == '\0');

	for (size_t i = 0; i < CTRL_COUNT; i++) {
		const struct ctrl_def *d = &ctrl_defs[i];
		if (numeric ? d->id == id : strcasecmp(key, d->name) == 0)
			return d;
	}
	return NULL;
}

/* --------------------------------------------------------------------------
 * UVC Extension Unit access
 */
int xu_query_bytes(int fd, uint8_t selector, uint8_t query, void *data, uint16_t size)
{
	struct uvc_xu_control_query q;

	memset(&q, 0, sizeof(q));
	q.unit = UVCD_XU_UNIT_ID;
	q.selector = selector;
	q.query = query;
	q.size = size;
	q.data = data;

	return xioctl(fd, UVCIOC_CTRL_QUERY, &q);
}

static int xu_query(int fd, uint8_t selector, uint8_t query, int16_t *value)
{
	uint8_t data[UVCD_XU_VALUE_SIZE] = {0};

	if (query == UVC_SET_CUR) {
		data[0] = (uint8_t)(*value & 0xff);
		data[1] = (uint8_t)((*value >> 8) & 0xff);
	}
	if (xu_query_bytes(fd, selector, query, data, sizeof(data)) < 0)
		return -1;

	if (query != UVC_SET_CUR)
		*value = (int16_t)(data[0] | ((uint16_t)data[1] << 8));
	return 0;
}

/* --------------------------------------------------------------------------
 * Device discovery
 *
 * Match on behaviour rather than on VID:PID or a product string: the camera
 * is the uvcvideo node that answers an XU query on unit 4. That is exactly
 * the property uvcdctl depends on, so anything it finds, it can drive.
 */
bool is_our_camera(int fd)
{
	struct v4l2_capability cap;
	int16_t probe;

	if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0)
		return false;
	if (strcmp((const char *)cap.driver, "uvcvideo") != 0)
		return false;
	return xu_query(fd, UVCD_XU_PROBE_SELECTOR, UVC_GET_LEN, &probe) == 0;
}

int open_device(const char *explicit_path, char *chosen, size_t chosen_len)
{
	if (explicit_path) {
		int fd = open(explicit_path, O_RDWR);
		if (fd < 0) {
			fprintf(stderr, "uvcdctl: %s: %s\n", explicit_path, strerror(errno));
			return -1;
		}
		snprintf(chosen, chosen_len, "%s", explicit_path);
		return fd;
	}

	for (int i = 0; i < 64; i++) {
		char path[32];
		snprintf(path, sizeof(path), "/dev/video%d", i);

		int fd = open(path, O_RDWR);
		if (fd < 0)
			continue;
		if (is_our_camera(fd)) {
			snprintf(chosen, chosen_len, "%s", path);
			return fd;
		}
		close(fd);
	}

	fprintf(stderr, "uvcdctl: no k1cam camera found (use --device /dev/videoN)\n");
	return -1;
}

/* --------------------------------------------------------------------------
 * Uniform get/set across both control kinds
 */
/* A control the camera simply doesn't implement reports ENOENT (Extension
 * Unit, selector outside bmControls) or EINVAL (Processing Unit, bit not set
 * in the descriptor). Worth naming, because that is exactly what an older
 * firmware looks like from here. */
static bool is_unsupported(int err)
{
	return err == ENOENT || err == EINVAL;
}

static void report_ctrl_error(const struct ctrl_def *def, const char *op, int err)
{
	if (is_unsupported(err))
		fprintf(stderr, "uvcdctl: %s: not supported by this camera\n", def->name);
	else
		fprintf(stderr, "uvcdctl: %s: %s: %s\n", def->name, op, strerror(err));
}

static int ctrl_get(int fd, const struct ctrl_def *def, int *out)
{
	if (def->kind == CTRL_CUSTOM) {
		int16_t v;
		if (xu_query(fd, def->selector, UVC_GET_CUR, &v) < 0) {
			report_ctrl_error(def, "XU GET_CUR", errno);
			return -1;
		}
		*out = v;
		return 0;
	}

	struct v4l2_control c = {.id = def->cid, .value = 0};
	if (xioctl(fd, VIDIOC_G_CTRL, &c) < 0) {
		report_ctrl_error(def, "VIDIOC_G_CTRL", errno);
		return -1;
	}
	*out = c.value;
	return 0;
}

/* Range/default for either kind. Queried from the device rather than
 * hardcoded here, so the daemon's table stays the single source of truth. */
static int ctrl_range(int fd, const struct ctrl_def *def, int *min, int *max, int *def_val)
{
	if (def->kind == CTRL_CUSTOM) {
		int16_t lo, hi, dv;
		if (xu_query(fd, def->selector, UVC_GET_MIN, &lo) < 0 ||
		    xu_query(fd, def->selector, UVC_GET_MAX, &hi) < 0 ||
		    xu_query(fd, def->selector, UVC_GET_DEF, &dv) < 0)
			return -1;
		*min = lo;
		*max = hi;
		*def_val = dv;
		return 0;
	}

	struct v4l2_queryctrl q = {.id = def->cid};
	if (xioctl(fd, VIDIOC_QUERYCTRL, &q) < 0)
		return -1;
	*min = q.minimum;
	*max = q.maximum;
	*def_val = q.default_value;
	return 0;
}

static int ctrl_set(int fd, const struct ctrl_def *def, int value, int *expected)
{
	int min, max, dv;
	*expected = def->kind == CTRL_CUSTOM && def->selector == 22
		? (value >= 90 ? 180 : 0) : value;
	if (ctrl_range(fd, def, &min, &max, &dv) != 0) {
		report_ctrl_error(def, "range query", errno);
		return -1;
	}
	if (value < min || value > max) {
		fprintf(stderr, "uvcdctl: %s must be between %d and %d\n", def->name, min, max);
		return -1;
	}

	if (def->kind == CTRL_CUSTOM) {
		int16_t v = (int16_t)value;
		if (xu_query(fd, def->selector, UVC_SET_CUR, &v) < 0) {
			fprintf(stderr, "uvcdctl: %s: XU SET_CUR: %s\n", def->name,
				strerror(errno));
			return -1;
		}
		return 0;
	}

	struct v4l2_control c = {.id = def->cid, .value = value};
	if (xioctl(fd, VIDIOC_S_CTRL, &c) < 0) {
		/* uvcvideo refuses these two while their auto control is on. */
		const char *hint =
			errno != EACCES ? "" :
			def->cid == V4L2_CID_WHITE_BALANCE_TEMPERATURE ?
					  " (set white-balance-auto 0 first)" :
			def->cid == V4L2_CID_EXPOSURE_ABSOLUTE ? " (set auto-exposure 1 first)" : "";
		fprintf(stderr, "uvcdctl: %s: VIDIOC_S_CTRL: %s%s\n", def->name, strerror(errno),
			hint);
		return -1;
	}
	*expected = c.value; /* V4L2 may align the request to the control's step. */
	return 0;
}

static void print_value(const struct ctrl_def *def, int value)
{
	printf("%s=%d\n", def->name, value);
}

static int socket_request(const char *command, char *buf, size_t size);

/* A negative fd selects the camera's local control socket. */
static int list_read(int fd, const struct ctrl_def *def, int *value,
		     int *min, int *max, int *dv)
{
	if (fd >= 0)
		return ctrl_range(fd, def, min, max, dv) == 0 &&
		       ctrl_get(fd, def, value) == 0 ? 0 : -1;

	char request[96], reply[160];
	snprintf(request, sizeof(request), "info %s", def->name);
	if (socket_request(request, reply, sizeof(reply)) != 0 ||
	    sscanf(reply, "%d %d %d %d", value, min, max, dv) != 4) {
		errno = EIO;
		return -1;
	}
	/* The daemon stores UVC AE mode bits; the CLI uses V4L2 menu values. */
	if (def->cid == V4L2_CID_EXPOSURE_AUTO) {
		*value = *value == 1 ? V4L2_EXPOSURE_MANUAL : V4L2_EXPOSURE_APERTURE_PRIORITY;
		*min = V4L2_EXPOSURE_MANUAL;
		*max = *dv = V4L2_EXPOSURE_APERTURE_PRIORITY;
	}
	return 0;
}

static int cmd_list(int fd, const struct ctrl_def *selected, bool descriptions)
{
	int rc = 0;
	/* Values changed from their default stand out -- on a terminal only,
	 * and not when NO_COLOR (no-color.org) asks for plain output. */
	bool color = isatty(STDOUT_FILENO) && !getenv("NO_COLOR");
	const char *hl_on = color ? "\033[1;33m" : "";
	const char *hl_off = color ? "\033[0m" : "";
	printf("%3s  %-28s %-4s %6s %6s %6s %7s%s\n", "ID", "NAME", "UNIT", "VALUE", "MIN", "MAX",
	       "DEFAULT", descriptions ? "  DESCRIPTION" : "");
	for (size_t i = 0; i < CTRL_COUNT; i++) {
		const struct ctrl_def *def = &ctrl_defs[i];
		if (selected && def != selected)
			continue;
		const char *kind = def->kind == CTRL_CUSTOM ? "xu" :
				   def->kind == CTRL_CAMERA ? "ct" : "pu";
		int min, max, dv, value;

		/* Show unsupported controls rather than dropping them, so it
		 * is obvious which ones a given firmware is missing. */
		if (list_read(fd, def, &value, &min, &max, &dv) != 0) {
			printf("%3u  %-28s %-4s %6s %6s %6s %7s  %s%s%s\n",
			       def->id, def->name, kind, "-", "-", "-", "-",
			       is_unsupported(errno) ? "unsupported" : "error",
			       descriptions ? ": " : "", descriptions ? def->help : "");
			rc = 1;
			continue;
		}
		bool changed = value != dv;
		printf("%3u  %-28s %-4s %s%6d%s %6d %6d %7d%s%s\n", def->id, def->name, kind,
		       changed ? hl_on : "", value, changed ? hl_off : "", min, max, dv,
		       descriptions ? "  " : "", descriptions ? def->help : "");
	}
	return rc;
}

static int cmd_keyframe(int fd)
{
	int16_t trigger = 1;

	if (xu_query(fd, UVCD_XU_KEYFRAME_SELECTOR, UVC_SET_CUR, &trigger) < 0) {
		fprintf(stderr, "uvcdctl: keyframe: XU SET_CUR: %s\n", strerror(errno));
		if (errno == ENOENT)
			fprintf(stderr,
				"uvcdctl: the camera's gadget descriptor does not expose the "
				"keyframe control -- its kernel predates it.\n");
		return 1;
	}
	/* A no-op unless the camera is currently streaming H.264. */
	printf("keyframe requested\n");
	return 0;
}

static int cmd_reset(int fd)
{
	int16_t trigger = 1;

	if (xu_query(fd, UVCD_XU_RESET_SELECTOR, UVC_SET_CUR, &trigger) < 0) {
		fprintf(stderr, "uvcdctl: factory reset: XU SET_CUR: %s\n", strerror(errno));
		if (errno == ENOENT)
			fprintf(stderr,
				"uvcdctl: the camera's gadget descriptor does not expose the "
				"reset control -- its kernel predates it.\n");
		return 1;
	}

	/* The device has now reset every control, including the standard
	 * ones -- but it did so out of band, and uvcvideo caches a standard
	 * control's value until the host itself writes it. Without this
	 * resync, `v4l2-ctl` (and our own `get`) would keep reporting the
	 * pre-reset values. Writing each queried default back is a no-op on
	 * the device and refreshes the host's cache.
	 *
	 * uvcvideo refuses writes to white-balance-temperature and
	 * exposure-time while their auto control is on, so walk the table
	 * backwards: each comes before the auto control that governs it. One
	 * still refused is inactive: the camera has reset it, and only the
	 * host's copy can lag until it is next set. */
	int rc = 0;
	for (size_t i = CTRL_COUNT; i-- > 0;) {
		const struct ctrl_def *def = &ctrl_defs[i];

		if (def->kind == CTRL_CUSTOM)
			continue;
		struct v4l2_queryctrl q = {.id = def->cid};
		if (xioctl(fd, VIDIOC_QUERYCTRL, &q) < 0) {
			rc = 1;
			continue;
		}
		if (q.flags & V4L2_CTRL_FLAG_INACTIVE)
			continue;
		struct v4l2_control c = {.id = def->cid, .value = q.default_value};
		if (xioctl(fd, VIDIOC_S_CTRL, &c) < 0 && errno != EACCES) {
			fprintf(stderr, "uvcdctl: resync %s: %s\n", def->name, strerror(errno));
			rc = 1;
		}
	}

	printf("factory reset\n");
	return rc;
}

static int parse_int(const char *text, int *value)
{
	char *end;
	long parsed;

	errno = 0;
	parsed = strtol(text, &end, 0);
	if (*text == '\0' || *end != '\0' || errno != 0 || parsed < INT_MIN || parsed > INT_MAX)
		return -1;
	*value = (int)parsed;
	return 0;
}

static int socket_request(const char *command, char *buf, size_t size)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	strncpy(addr.sun_path, UVCD_SOCKET_PATH, sizeof(addr.sun_path) - 1);
	if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	dprintf(fd, "%s\n", command);
	size_t used = 0;
	ssize_t n = 0;
	while (used < size - 1) {
		n = read(fd, buf + used, size - 1 - used);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		used += (size_t)n;
	}
	buf[used] = '\0';
	close(fd);
	/* The final protocol line decides success, including fragmented reads.
	 * A full buffer without EOF is an oversized or incomplete reply. */
	return n == 0 && used >= 3 && !memcmp(buf + used - 3, "ok\n", 3) &&
	       (used == 3 || buf[used - 4] == '\n') ? 0 : 1;
}

static int socket_command(const char *command)
{
	char buf[4097] = {0};
	int rc = socket_request(command, buf, sizeof(buf));
	fputs(buf, stdout);
	return rc;
}

int main(int argc, char **argv)
{
	const char *device = NULL;
	int argi = 1;

	for (; argi < argc; argi++) {
		if (strcmp(argv[argi], "--device") == 0 && argi + 1 < argc) {
			device = argv[++argi];
		} else if (strcmp(argv[argi], "--help") == 0 || strcmp(argv[argi], "-h") == 0) {
			usage(stdout, argv[0]);
			return 0;
		} else {
			break;
		}
	}

	if (argi >= argc) {
		usage(stderr, argv[0]);
		return 2;
	}
	const char *cmd = argv[argi++];
	const struct command_def *command = find_command(cmd);
	if (!command) {
		fprintf(stderr, "uvcdctl: unknown command '%s'\n", cmd);
		usage(stderr, argv[0]);
		return 2;
	}
	if (argc - argi == 1 &&
	    (!strcmp(argv[argi], "--help") || !strcmp(argv[argi], "-h"))) {
		command_usage(stdout, argv[0], command);
		return 0;
	}
	/* Validate arity before opening anything, so a usage error never
	 * depends on whether a camera happens to be plugged in. */
	unsigned nargs = (unsigned)(argc - argi);
	if (nargs < command->min_args || nargs > command->max_args) {
		command_usage(stderr, argv[0], command);
		return 2;
	}
	if (!strcmp(cmd, "help")) {
		if (!nargs) {
			usage(stdout, argv[0]);
			return 0;
		}
		const struct command_def *topic = find_command(argv[argi]);
		if (!topic) {
			fprintf(stderr, "uvcdctl: unknown command '%s'\n", argv[argi]);
			command_usage(stderr, argv[0], command);
			return 2;
		}
		command_usage(stdout, argv[0], topic);
		return 0;
	}
	bool descriptions = strcmp(cmd, "describe") == 0;
	bool listing = strcmp(cmd, "list") == 0 || descriptions;

	const char *key = NULL;
	int value = 0;

	if (strcmp(cmd, "get") == 0) {
		key = argv[argi];
	} else if (strcmp(cmd, "set") == 0) {
		key = argv[argi];
		if (parse_int(argv[argi + 1], &value) != 0) {
			fprintf(stderr, "uvcdctl: '%s' is not a number\n", argv[argi + 1]);
			return 2;
		}
	} else if (listing) {
		key = argi < argc ? argv[argi] : NULL;
	} else if (!strcmp(cmd, "firmware")) {
		/* Its own device handling: an update reconnects after a reboot. */
		return cmd_firmware(argc - argi, argv + argi, device);
	}

	const struct ctrl_def *def = NULL;
	if (key) {
		def = find_ctrl(key);
		if (!def) {
			fprintf(stderr, "uvcdctl: unknown control '%s'\n", key);
			return 2;
		}
	}

	if (!device && access(UVCD_SOCKET_PATH, F_OK) == 0) {
		if (listing)
			return cmd_list(-1, def, descriptions);
		char request[160];
		if (!strcmp(cmd, "get"))
			snprintf(request, sizeof(request), "get %s", def->name);
		else if (!strcmp(cmd, "set"))
			snprintf(request, sizeof(request), "set %s %d", def->name, value);
		else
			snprintf(request, sizeof(request), "%s", cmd);
		return socket_command(request);
	}

	char chosen[64];
	int fd = open_device(device, chosen, sizeof(chosen));
	if (fd < 0)
		return 1;

	int rc;
	if (strcmp(cmd, "get") == 0) {
		int v;
		rc = ctrl_get(fd, def, &v) != 0 ? 1 : 0;
		if (rc == 0)
			print_value(def, v);
	} else if (strcmp(cmd, "set") == 0) {
		int expected;
		rc = ctrl_set(fd, def, value, &expected) != 0 ? 1 : 0;
		if (rc == 0) {
			int v;
			if (ctrl_get(fd, def, &v) != 0) {
				rc = 1;
			} else {
				print_value(def, v);
				if (v != expected) {
					fprintf(stderr, "uvcdctl: %s requested %d, read back %d\n",
						def->name, expected, v);
					rc = 1;
				}
			}
		}
	} else if (listing) {
		rc = cmd_list(fd, def, descriptions);
	} else if (strcmp(cmd, "keyframe") == 0) {
		rc = cmd_keyframe(fd);
	} else {
		rc = cmd_reset(fd);
	}

	close(fd);
	return rc;
}
