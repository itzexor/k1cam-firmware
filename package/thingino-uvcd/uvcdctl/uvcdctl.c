/*
 * uvcdctl -- host-side control CLI for thingino-uvcd
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
#define _GNU_SOURCE

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
#include <sys/un.h>
#include <unistd.h>

#define UVCD_XU_UNIT_ID 4
#define UVCD_XU_RESET_SELECTOR 10
#define UVCD_XU_KEYFRAME_SELECTOR 20
#define UVCD_XU_SAVE_SELECTOR 23
#define UVCD_XU_BOOT_SELECTOR 24
#define UVCD_XU_SAVED_SELECTOR 25
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
	 "0-160: auto-exposure analog gain ceiling"},
	{22, "max-digital-gain", CTRL_CUSTOM, 0, 2,
	 "0-160: auto-exposure digital gain ceiling"},
	{23, "spatial-denoise", CTRL_CUSTOM, 0, 4,
	 "0-255: strength, 128: as tuned, 192: default"},
	{24, "temporal-denoise", CTRL_CUSTOM, 0, 5,
	 "0-255: strength, 128: as tuned"},
	{25, "defective-pixel-correction", CTRL_CUSTOM, 0, 6,
	 "0-255: strength, 128: as tuned"},
	{26, "dynamic-range-compression", CTRL_CUSTOM, 0, 7,
	 "0-255: strength, 128: as tuned"},
	{28, "highlight-suppression", CTRL_CUSTOM, 0, 9,
	 "0: off, 1-10: strength"},
	/* 27 was defog-strength, which had no effect; 29 and 30 were
	 * horizontal-flip and vertical-flip. All retired, not reused. UVC
	 * does define rotation (Roll), but V4L2 has no control for it, so it
	 * rides in the Extension Unit where Linux can reach it. */
	{31, "rotation", CTRL_CUSTOM, 0, 22,
	 "0 or 180: degrees"},
	{32, "metering", CTRL_CUSTOM, 0, 21,
	 "0: tuning default, 1: average, 2: center-weighted, 3: spot (of the view)"},

	{40, "h264-bitrate-kbps", CTRL_CUSTOM, 0, 13,
	 "0: automatic, 1-16000: target kbps"},
	{41, "h264-rate-control", CTRL_CUSTOM, 0, 14,
	 "0: CBR, 1: VBR, 2: capped VBR"},
	{42, "h264-gop-frames", CTRL_CUSTOM, 0, 15,
	 "0: one second, 1-300: frames between keyframes, rounded up to whole seconds"},
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

static void usage(FILE *out, const char *prog)
{
	fprintf(out,
		"usage: %s [--device PATH] <command> [args]\n"
		"commands:\n"
		"  get <key>          read one control\n"
		"  set [-t] <key> <value>  change and save (or transient with -t)\n"
		"  save <key>         save the current live value\n"
		"  apply              apply all saved values now\n"
		"  boot [on|off]      show or change apply-on-boot mode\n"
		"  list               show every control's id, value, range and default\n"
		"  describe [key]     explain what a control's values mean (all if no key)\n"
		"  reset              restore factory defaults\n"
		"  keyframe           make the H.264 stream emit a keyframe now\n"
		"\n"
		"<key> is a control name or its numeric id (see list).\n"
		"The camera is found automatically; override with --device /dev/videoN.\n",
		prog);
}

static int xu_query(int fd, uint8_t selector, uint8_t query, int16_t *value);

static unsigned standard_selector(uint32_t cid)
{
	switch (cid) {
	case V4L2_CID_BRIGHTNESS: return 2;
	case V4L2_CID_CONTRAST: return 3;
	case V4L2_CID_POWER_LINE_FREQUENCY: return 5;
	case V4L2_CID_HUE: return 6;
	case V4L2_CID_SATURATION: return 7;
	case V4L2_CID_SHARPNESS: return 8;
	case V4L2_CID_GAMMA: return 9;
	case V4L2_CID_WHITE_BALANCE_TEMPERATURE: return 10;
	case V4L2_CID_AUTO_WHITE_BALANCE: return 11;
	case V4L2_CID_EXPOSURE_AUTO: return 2;
	case V4L2_CID_EXPOSURE_AUTO_PRIORITY: return 3;
	case V4L2_CID_EXPOSURE_ABSOLUTE: return 4;
	case V4L2_CID_ZOOM_ABSOLUTE: return 11;
	case V4L2_CID_PAN_ABSOLUTE:
	case V4L2_CID_TILT_ABSOLUTE: return 13;
	default: return 0;
	}
}

static uint16_t ctrl_ref(const struct ctrl_def *def)
{
	unsigned kind = def->kind == CTRL_CUSTOM ? 1 : def->kind == CTRL_CAMERA ? 2 : 0;
	unsigned selector = def->kind == CTRL_CUSTOM ? def->selector : standard_selector(def->cid);
	return (uint16_t)((kind << 8) | selector);
}

static int ctrl_save(int fd, const struct ctrl_def *def)
{
	int16_t ref = (int16_t)ctrl_ref(def);
	return xu_query(fd, UVCD_XU_SAVE_SELECTOR, UVC_SET_CUR, &ref);
}

static int ctrl_saved(int fd, const struct ctrl_def *def, int *value)
{
	int16_t ref = (int16_t)ctrl_ref(def), v;
	if (xu_query(fd, UVCD_XU_SAVED_SELECTOR, UVC_SET_CUR, &ref) ||
	    xu_query(fd, UVCD_XU_SAVED_SELECTOR, UVC_GET_CUR, &v))
		return -1;
	*value = v;
	return 0;
}

static int xioctl(int fd, unsigned long request, void *arg)
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
static int xu_query(int fd, uint8_t selector, uint8_t query, int16_t *value)
{
	struct uvc_xu_control_query q;
	uint8_t data[UVCD_XU_VALUE_SIZE] = {0};

	if (query == UVC_SET_CUR) {
		data[0] = (uint8_t)(*value & 0xff);
		data[1] = (uint8_t)((*value >> 8) & 0xff);
	}

	memset(&q, 0, sizeof(q));
	q.unit = UVCD_XU_UNIT_ID;
	q.selector = selector;
	q.query = query;
	q.size = sizeof(data);
	q.data = data;

	if (xioctl(fd, UVCIOC_CTRL_QUERY, &q) < 0)
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
static bool is_our_camera(int fd)
{
	struct v4l2_capability cap;
	int16_t probe;

	if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0)
		return false;
	if (strcmp((const char *)cap.driver, "uvcvideo") != 0)
		return false;
	return xu_query(fd, UVCD_XU_PROBE_SELECTOR, UVC_GET_LEN, &probe) == 0;
}

static int open_device(const char *explicit_path, char *chosen, size_t chosen_len)
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

	fprintf(stderr, "uvcdctl: no thingino-uvcd camera found (use --device /dev/videoN)\n");
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

static int ctrl_set(int fd, const struct ctrl_def *def, int value)
{
	int min, max, dv;

	if (ctrl_range(fd, def, &min, &max, &dv) == 0 && (value < min || value > max)) {
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
		fprintf(stderr, "uvcdctl: %s: VIDIOC_S_CTRL: %s\n", def->name, strerror(errno));
		return -1;
	}
	return 0;
}

static void print_value(const struct ctrl_def *def, int value)
{
	printf("%s=%d\n", def->name, value);
}

/* Static text only -- needs no camera, so it works as offline reference. */
static void describe(const struct ctrl_def *def)
{
	printf("%3u  %-28s %s\n", def->id, def->name, def->help);
}

static int cmd_list(int fd)
{
	int rc = 0;
	/* Values changed from their default stand out -- on a terminal only,
	 * and not when NO_COLOR (no-color.org) asks for plain output. */
	bool color = isatty(STDOUT_FILENO) && !getenv("NO_COLOR");
	const char *hl_on = color ? "\033[1;33m" : "";
	const char *hl_off = color ? "\033[0m" : "";
	printf("%3s  %-28s %-4s %6s %6s %6s %7s %7s\n", "ID", "NAME", "UNIT", "VALUE", "MIN", "MAX",
	       "DEFAULT", "SAVED");
	for (size_t i = 0; i < CTRL_COUNT; i++) {
		const struct ctrl_def *def = &ctrl_defs[i];
		const char *kind = def->kind == CTRL_CUSTOM ? "xu" :
				   def->kind == CTRL_CAMERA ? "ct" : "pu";
		int min, max, dv, value;

		/* Show unsupported controls rather than dropping them, so it
		 * is obvious which ones a given firmware is missing. */
		if (ctrl_range(fd, def, &min, &max, &dv) != 0) {
			printf("%3u  %-28s %-4s %6s\n", def->id, def->name, kind,
			       is_unsupported(errno) ? "unsupported" : "error");
			rc = 1;
			continue;
		}
		if (ctrl_get(fd, def, &value) != 0) {
			rc = 1;
			continue;
		}
		bool changed = value != dv;
		int saved;
		bool has_saved = ctrl_saved(fd, def, &saved) == 0 && saved != -32768;
		char saved_text[16];
		if (has_saved)
			snprintf(saved_text, sizeof(saved_text), "%d", saved);
		else
			strcpy(saved_text, "-");
		printf("%3u  %-28s %-4s %s%6d%s %6d %6d %7d %7s\n", def->id, def->name, kind,
		       changed ? hl_on : "", value, changed ? hl_off : "", min, max, dv, saved_text);
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
	 * the device and refreshes the host's cache. */
	int rc = 0;
	for (size_t i = 0; i < CTRL_COUNT; i++) {
		const struct ctrl_def *def = &ctrl_defs[i];
		int min, max, dv;

		if (def->kind == CTRL_CUSTOM)
			continue;
		if (ctrl_range(fd, def, &min, &max, &dv) != 0) {
			rc = 1;
			continue;
		}
		struct v4l2_control c = {.id = def->cid, .value = dv};
		if (xioctl(fd, VIDIOC_S_CTRL, &c) < 0) {
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

static int socket_command(const char *command)
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
	char buf[4096];
	ssize_t n;
	int rc = 1;
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		fwrite(buf, 1, (size_t)n, stdout);
		if (n >= 3 && strstr(buf, "ok\n"))
			rc = 0;
	}
	close(fd);
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

	/* Validate arity before opening anything, so a usage error never
	 * depends on whether a camera happens to be plugged in. */
	const char *key = NULL;
	int value = 0;
	bool transient = false;

	if (strcmp(cmd, "get") == 0) {
		if (argi >= argc) {
			usage(stderr, argv[0]);
			return 2;
		}
		key = argv[argi];
	} else if (strcmp(cmd, "set") == 0) {
		if (argi < argc && (!strcmp(argv[argi], "-t") || !strcmp(argv[argi], "--transient"))) {
			transient = true;
			argi++;
		}
		if (argi + 1 >= argc) {
			usage(stderr, argv[0]);
			return 2;
		}
		key = argv[argi];
		if (parse_int(argv[argi + 1], &value) != 0) {
			fprintf(stderr, "uvcdctl: '%s' is not a number\n", argv[argi + 1]);
			return 2;
		}
	} else if (strcmp(cmd, "save") == 0) {
		if (argi >= argc) {
			usage(stderr, argv[0]);
			return 2;
		}
		key = argv[argi];
	} else if (strcmp(cmd, "describe") == 0) {
		key = argi < argc ? argv[argi] : NULL;
	} else if (strcmp(cmd, "boot") == 0) {
		key = argi < argc ? argv[argi] : NULL;
	} else if (strcmp(cmd, "list") != 0 &&
		   strcmp(cmd, "apply") != 0 && strcmp(cmd, "reset") != 0 &&
		   strcmp(cmd, "keyframe") != 0) {
		usage(stderr, argv[0]);
		return 2;
	}

	const struct ctrl_def *def = NULL;
	if (key && strcmp(cmd, "boot")) {
		def = find_ctrl(key);
		if (!def) {
			fprintf(stderr, "uvcdctl: unknown control '%s'\n", key);
			return 2;
		}
	}

	if (strcmp(cmd, "describe") == 0) {
		printf("%3s  %-28s %s\n", "ID", "NAME", "VALUES");
		if (def) {
			describe(def);
		} else {
			for (size_t i = 0; i < CTRL_COUNT; i++)
				describe(&ctrl_defs[i]);
		}
		return 0;
	}

	if (!device && access(UVCD_SOCKET_PATH, F_OK) == 0) {
		char request[160];
		if (!strcmp(cmd, "get") || !strcmp(cmd, "save"))
			snprintf(request, sizeof(request), "%s %s", cmd, def->name);
		else if (!strcmp(cmd, "set"))
			snprintf(request, sizeof(request), "set %s %d", def->name, value);
		else if (!strcmp(cmd, "boot") && key)
			snprintf(request, sizeof(request), "boot %s", key);
		else
			snprintf(request, sizeof(request), "%s", cmd);
		int rc = socket_command(request);
		if (rc == 0 && !strcmp(cmd, "set") && !transient) {
			snprintf(request, sizeof(request), "save %s", def->name);
			rc = socket_command(request);
		}
		return rc;
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
		rc = ctrl_set(fd, def, value) != 0 ? 1 : 0;
		if (rc == 0 && !transient && ctrl_save(fd, def) != 0) {
			fprintf(stderr, "uvcdctl: save %s: %s\n", def->name, strerror(errno));
			rc = 1;
		}
		if (rc == 0) {
			int v;
			/* Echo what actually took effect: the daemon clamps. */
			if (ctrl_get(fd, def, &v) == 0)
				print_value(def, v);
			else
				print_value(def, value);
		}
	} else if (strcmp(cmd, "save") == 0) {
		rc = ctrl_save(fd, def) != 0;
	} else if (strcmp(cmd, "apply") == 0) {
		rc = 0;
		for (size_t i = 0; i < CTRL_COUNT; i++) {
			int saved;
			if (ctrl_saved(fd, &ctrl_defs[i], &saved) != 0 || saved == -32768)
				continue;
			if (ctrl_set(fd, &ctrl_defs[i], saved) != 0)
				rc = 1;
		}
	} else if (strcmp(cmd, "boot") == 0) {
		int16_t boot;
		if (key) {
			if (strcmp(key, "on") && strcmp(key, "off")) {
				fprintf(stderr, "uvcdctl: boot expects on or off\n");
				rc = 2;
			} else {
				boot = !strcmp(key, "on");
				rc = xu_query(fd, UVCD_XU_BOOT_SELECTOR, UVC_SET_CUR, &boot) != 0;
			}
		} else {
			rc = xu_query(fd, UVCD_XU_BOOT_SELECTOR, UVC_GET_CUR, &boot) != 0;
			if (!rc)
				printf("%s\n", boot ? "on" : "off");
		}
	} else if (strcmp(cmd, "list") == 0) {
		rc = cmd_list(fd);
	} else if (strcmp(cmd, "keyframe") == 0) {
		rc = cmd_keyframe(fd);
	} else {
		rc = cmd_reset(fd);
	}

	close(fd);
	return rc;
}
