#include <assert.h>
#include <stdarg.h>
#define ioctl test_ioctl
#define unlink test_unlink
#include "../src/uvcd_gadget.c"
#include "../src/uvcd_ctl.c"
#include "../src/fw_env.h"
#include "fake_flash.h"

static int saves, good_saves, queued, idrs, restarts;
static int restart_errors, start_errors, start_calls, bitrate_errors, bitrate_calls;
static struct uvcd_control_state restarted_controls;
static struct uvcd_control_state last_saved, last_good;
static int unlinks, streak_writes, last_streak, save_errors;

int test_unlink(const char *path)
{
	(void)path;
	unlinks++;
	return 0;
}

int uvcd_config_streak_write(const char *path, int n)
{
	(void)path;
	streak_writes++;
	last_streak = n;
	return 0;
}
static bool host_reading;

void uvcd_log(int level, const char *fmt, ...)
{
	(void)level;
	(void)fmt;
}

void uvcd_config_defaults(struct uvcd_control_state *out)
{
	memset(out, 0, sizeof(*out));
	for (size_t i = 0; i < UVCD_CTRL_DEF_COUNT; i++)
		*uvcd_ctrl_field(out, &uvcd_ctrl_defs[i]) = uvcd_ctrl_defs[i].def;
}

int uvcd_config_save(const char *path, const struct uvcd_control_state *c)
{
	if (save_errors) { save_errors--; errno = ENOSPC; return -1; }
	if (!strcmp(path, UVCD_CONFIG_GOOD_PATH)) {
		good_saves++;
		last_good = *c;
	} else {
		assert(!strcmp(path, UVCD_CONFIG_PATH));
		saves++;
		last_saved = *c;
	}
	return 0;
}

bool uvcd_pipeline_restart_encoder(uvcd_pipeline_t *p, uint8_t format)
{
	(void)format;
	restarts++;
	p->frame.seq = 0;
	if (restart_errors) {
		restart_errors--;
		p->running = false;
		return false;
	}
	return true;
}

/* The format tables live in uvcd_pipeline.c; handle_data_event's COMMIT path
 * links against them. */
const struct uvcd_frame_info uvcd_frames[UVCD_NUM_FRAMES + 1];
uint32_t uvcd_snap_interval(uint32_t interval) { return interval; }

/* The trial self-test starts its own small streams: recorded, not checked
 * against a negotiated stream. */
static bool selftest_streams;
static int test_streams, stops;
static uint8_t test_stream_format, test_stream_frame;

void uvcd_pipeline_stop(uvcd_pipeline_t *p)
{
	stops++;
	p->running = false;
}

int uvcd_pipeline_start(uvcd_pipeline_t *p, uint8_t format, uint8_t frame, uint32_t interval)
{
	if (selftest_streams) {
		test_streams++;
		test_stream_format = format;
		test_stream_frame = frame;
		assert(interval == UVCD_INTERVAL_30FPS);
		p->running = true;
		p->frame.seq = 0;
		return 0;
	}
	assert(format == p->cur_format && frame == p->cur_frame && interval == p->cur_interval);
	start_calls++;
	restarted_controls = p->controls;
	p->frame.seq = 0;
	if (start_errors) {
		start_errors--;
		return -EIO;
	}
	p->running = true;
	return 0;
}

int uvcd_enc_request_keyframe(uvcd_pipeline_t *p)
{
	(void)p;
	idrs++;
	return 0;
}

int uvcd_camera_live_exposure(uvcd_camera_t *cam) { (void)cam; return -1; }
static int awb_ct = -1;
int uvcd_camera_awb_ct(uvcd_camera_t *cam) { (void)cam; return awb_ct; }
int uvcd_enc_apply_bitrate(uvcd_pipeline_t *p)
{
	bitrate_calls++;
	restarted_controls = p->controls;
	if (bitrate_errors) {
		bitrate_errors--;
		return -EIO;
	}
	return 0;
}

/* The camera, as the gadget sees it: it takes or rejects a change, and may
 * fill in a dependent value (leaving auto white balance keeps the measured
 * temperature). */
static int applies, apply_error, camera_guard_arms;
static bool apply_resets_sequence;
static size_t last_field;
int uvcd_pipeline_apply(uvcd_pipeline_t *p, struct uvcd_control_state *next, size_t field)
{
	if (apply_resets_sequence)
		p->frame.seq = 0; /* restored stream after a rejected view change */
	applies++;
	last_field = field;
	if (apply_error)
		return apply_error;
	if (field == offsetof(struct uvcd_control_state, wb_auto) && !next->wb_auto)
		next->wb_temp = 3300;
	return 0;
}

void uvcd_pipeline_camera_guard_arm(uvcd_pipeline_t *p)
{
	(void)p;
	camera_guard_arms++;
}

static struct uvc_request_data last_response;

int test_ioctl(int fd, unsigned long request, ...)
{
	(void)fd;
	va_list ap;
	va_start(ap, request);
	void *arg = va_arg(ap, void *);
	struct v4l2_buffer *buf = arg;
	va_end(ap);
	if (request == UVCIOC_SEND_RESPONSE) {
		last_response = *(struct uvc_request_data *)arg;
		return 0;
	}
	if (request == VIDIOC_DQBUF) {
		if (!host_reading) {
			errno = EAGAIN;
			return -1;
		}
		buf->index = 0;
		return 0;
	}
	assert(request == VIDIOC_QBUF);
	queued++;
	return 0;
}

static void fw_frame(uint8_t *f, uint32_t seq, uint8_t op)
{
	memset(f, 0, 60);
	uvcd_fw_put_magic(f);
	f[UVCD_FW_COMMAND_VERSION] = UVCD_FW_PROTOCOL_VERSION;
	f[UVCD_FW_COMMAND_OPCODE] = op;
	uvcd_fw_put_le32(f + UVCD_FW_COMMAND_SEQUENCE, seq);
}

static void fw_status(gadget_t *g, uint8_t *s)
{
	struct uvc_request_data resp = {0};
	struct usb_ctrlrequest req = {.bRequestType = USB_DIR_IN, .bRequest = UVC_GET_CUR,
				      .wValue = UVCD_FW_STATUS_SELECTOR << 8,
				      .wIndex = UVCD_CUSTOM_UNIT_ID << 8, .wLength = 60};
	handle_firmware_setup(g, &req, &resp);
	assert(resp.length == 60);
	memcpy(s, resp.data, 60);
}

/* Selectors 26-28 go to the firmware service, SET data stages included. */
static void test_firmware_routing(void)
{
	gadget_t g = {0};
	struct uvc_request_data resp = {0};
	struct usb_ctrlrequest req = {.bRequestType = USB_DIR_IN, .bRequest = UVC_GET_LEN,
				      .wValue = UVCD_FW_STATUS_SELECTOR << 8,
				      .wIndex = UVCD_CUSTOM_UNIT_ID << 8, .wLength = 2};
	assert(firmware_selector(26) && firmware_selector(28) && !firmware_selector(25) &&
	       !firmware_selector(22));
	handle_firmware_setup(&g, &req, &resp);
	assert(resp.length == -1); /* no service: stall */

	struct fake_flash ff = {0};
	struct uvcd_update_config cfg;
	fake_flash_init(&ff, true);
	uvcd_update_default_config(&cfg);
	cfg.flash = ff.flash;
	cfg.cmdline_path = cfg.release_path = cfg.serial_path = "/nonexistent";
	g.update = uvcd_update_create(&cfg);
	handle_firmware_setup(&g, &req, &resp);
	assert(resp.length == 2 && resp.data[0] == UVCD_FW_FRAME_SIZE && resp.data[1] == 0);

	req = (struct usb_ctrlrequest){.bRequestType = USB_DIR_OUT, .bRequest = UVC_SET_CUR,
				       .wValue = UVCD_FW_COMMAND_SELECTOR << 8,
				       .wIndex = UVCD_CUSTOM_UNIT_ID << 8, .wLength = 60};
	handle_firmware_setup(&g, &req, &resp);
	assert(resp.length == 60 && g.last_cs == UVCD_FW_COMMAND_SELECTOR &&
	       g.last_unit == UVCD_CUSTOM_UNIT_ID && g.last_request == UVC_SET_CUR &&
	       g.last_intf == UVC_INTF_CONTROL);
	struct uvc_request_data data = {.length = 60};
	fw_frame(data.data, 42, UVCD_FW_OP_INFO);
	handle_data_event(&g, &data);
	uint8_t s[60];
	fw_status(&g, s);
	assert(uvcd_fw_get_le32(s + UVCD_FW_STATUS_SEQUENCE) == 42 &&
	       s[UVCD_FW_STATUS_RESULT] == 0 && s[UVCD_FW_STATUS_OPCODE] == UVCD_FW_OP_INFO);
	/* A SET the service refuses (wrong length) stalls and leaves no
	 * pending data stage behind. */
	g.last_cs = 0;
	req.wLength = 2;
	handle_firmware_setup(&g, &req, &resp);
	assert(resp.length == -1 && g.last_cs == 0);
	/* Not a selector anything is behind: still the XU control path. */
	req.wValue = UVCD_CUSTOM_RESET << 8;
	req.wLength = 2;
	resp.length = 0;
	handle_custom_setup(&g, &req, &resp);
	assert(resp.length == 2 && g.last_cs == UVCD_CUSTOM_RESET);
	uvcd_update_destroy(g.update);
	free(ff.mem);
}

static void put_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");
	assert(f);
	fputs(text, f);
	fclose(f);
}

/* A trial boot of slot b, with what uvcd_update needs to confirm it. */
static struct uvcd_update *trial_service(struct fake_flash *ff, char *root, size_t size)
{
	static struct uvcd_update_config cfg;
	static char paths[6][300];
	snprintf(root, size, "%s/gadget-test-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
		 (int)getpid());
	char cmd[700];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s/overlay/slots/b/root'", root, root);
	assert(system(cmd) == 0);
	static const char *const names[] = {"cmdline", "mounts", "release", "overlay", "run",
					    "meminfo"};
	for (int i = 0; i < 6; i++)
		snprintf(paths[i], sizeof(paths[i]), "%s/%s", root, names[i]);
	put_file(paths[0], "root=/dev/mtdblock6 k1cam.slot=b k1cam.boot=trial\n");
	char mounts[512];
	snprintf(mounts, sizeof(mounts), "overlay / overlayfs rw,upperdir=%s/slots/b/root 0 0\n",
		 paths[3]);
	put_file(paths[1], mounts);
	put_file(paths[2], "build=k1cam-new\n");
	put_file(paths[5], "MemFree: 30000 kB\n");
	fake_flash_init(ff, true);
	struct fw_env env;
	memset(&env, 0, sizeof(env));
	assert(fw_env_set(&env, "active_slot", "a") == 0 &&
	       fw_env_set(&env, "trial_slot", "b") == 0 &&
	       fw_env_set(&env, "upgrade_available", "1") == 0);
	fw_env_image(&env, 1, ff->mem + 0x50000);
	uvcd_update_default_config(&cfg);
	cfg.flash = ff->flash;
	cfg.cmdline_path = paths[0];
	cfg.mounts_path = paths[1];
	cfg.release_path = paths[2];
	cfg.serial_path = "/nonexistent";
	cfg.data_root = paths[3];
	cfg.run_dir = paths[4];
	cfg.meminfo_path = paths[5];
	return uvcd_update_create(&cfg);
}

static void publish(uvcd_pipeline_t *p, const uint8_t *frame, uint32_t len, bool key)
{
	pthread_mutex_lock(&p->frame.lock);
	memcpy(p->frame.data, frame, len);
	p->frame.len = len;
	p->frame.is_key = key;
	p->frame.seq++;
	pthread_mutex_unlock(&p->frame.lock);
}

static bool env_value(struct fake_flash *ff, const char *name, const char *want)
{
	struct fw_layout_map map;
	struct fw_env env;
	assert(fw_layout_resolve(&ff->flash, &map) == 0 && fw_env_load(&ff->flash, &map, &env) == 0);
	const char *v = fw_env_get(&env, name);
	return want ? v && !strcmp(v, want) : !v;
}

static bool run_file(const char *root, const char *name)
{
	char path[400];
	snprintf(path, sizeof(path), "%s/run/%s", root, name);
	return access(path, F_OK) == 0;
}

/* A trial boot proves the camera works -- frames through both encoders --
 * before the slot is confirmed. */
static void test_trial_selftest(void)
{
	static const uint8_t h264[] = {0, 0, 0, 1, 0x65, 0x88, 0x84};
	static const uint8_t mjpeg[] = {0xff, 0xd8, 0xff, 0xe0, 1, 2, 0xff, 0xd9};
	static uint8_t frame_buf[64];
	struct fake_flash ff = {0};
	char root[256];
	uvcd_pipeline_t p = {0};
	gadget_t g = {.pipe = &p};

	pthread_mutex_init(&p.frame.lock, NULL);
	p.frame.data = frame_buf;
	selftest_streams = true;
	test_streams = stops = 0;

	/* No trial: nothing happens. */
	selftest_tick(&g);
	assert(test_streams == 0);

	g.update = trial_service(&ff, root, sizeof(root));
	assert(uvcd_update_trial_wants_check(g.update));
	selftest_tick(&g);
	assert(test_streams == 1 && test_stream_format == UVCD_FMT_H264 &&
	       test_stream_frame == UVCD_FRAME_360P);
	/* Only valid frames count, and H.264 needs a keyframe among them. */
	publish(&p, mjpeg, sizeof(mjpeg), false); /* wrong format: ignored */
	selftest_tick(&g);
	for (int i = 0; i < 5; i++) {
		publish(&p, h264, sizeof(h264), false);
		selftest_tick(&g);
	}
	assert(test_streams == 1); /* no keyframe yet */
	publish(&p, h264, sizeof(h264), true);
	selftest_tick(&g);
	assert(stops == 1 && test_streams == 2 && test_stream_format == UVCD_FMT_MJPEG);
	for (int i = 0; i < 5; i++) {
		publish(&p, mjpeg, sizeof(mjpeg), false);
		selftest_tick(&g);
	}
	assert(stops == 2 && g.selftest.step == SELFTEST_OFF);
	uvcd_update_wait_idle(g.update);
	assert(env_value(&ff, "active_slot", "b") && env_value(&ff, "trial_slot", NULL) &&
	       env_value(&ff, "upgrade_available", "0"));
	assert(run_file(root, "confirmed") && !uvcd_update_trial_wants_check(g.update));
	selftest_tick(&g);
	assert(test_streams == 2); /* done for this boot */
	uvcd_update_destroy(g.update);

	/* A camera that encodes nothing: three tries per step, then the
	 * trial is reported failed and left pending. */
	g.update = trial_service(&ff, root, sizeof(root));
	g.selftest.step = SELFTEST_OFF;
	test_streams = stops = 0;
	selftest_tick(&g);
	for (int i = 0; i < 3; i++) {
		assert(g.selftest.step == SELFTEST_H264);
		g.selftest.since_ms -= SELFTEST_STEP_MS + 1;
		selftest_tick(&g);
	}
	assert(test_streams == 3 && stops == 3 && g.selftest.step == SELFTEST_OFF);
	assert(run_file(root, "trial-failed") && !run_file(root, "confirmed"));
	assert(env_value(&ff, "trial_slot", "b") && env_value(&ff, "active_slot", "a"));
	uvcd_update_destroy(g.update);

	/* A host that streams first: its healthy stream is the proof. */
	g.update = trial_service(&ff, root, sizeof(root));
	g.streaming = true;
	g.healthy_since_ms = uvcd_monotonic_ms() - SELFTEST_HOST_MS - 1;
	selftest_tick(&g);
	uvcd_update_wait_idle(g.update);
	assert(env_value(&ff, "active_slot", "b") && run_file(root, "confirmed"));
	uvcd_update_destroy(g.update);

	char cmd[300];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
	assert(system(cmd) == 0);
	free(ff.mem);
	selftest_streams = false;
	pthread_mutex_destroy(&p.frame.lock);
}

static void test_rejected_view(void)
{
	static const char *names[] = {"zoom", "pan", "tilt"};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		uvcd_pipeline_t p = {0};
		pthread_mutex_init(&p.frame.lock, NULL);
		uvcd_config_defaults(&p.controls);
		struct uvcd_control_state old = p.controls;
		uint8_t data[] = {1, 2, 3, 4}, output[4] = {0};
		p.frame.data = data;
		p.frame.len = sizeof(data);
		p.frame.seq = 1000;
		gadget_t g = {.pipe = &p, .streaming = true, .read_seq = 1000,
			     .watched_seq = 1000, .healthy_since_ms = 1,
			     .cur_format = UVCD_FMT_MJPEG, .buf_count = 1};
		g.buffers[0].start = output;
		g.buffers[0].length = sizeof(output);
		apply_error = -EIO;
		apply_resets_sequence = true;
		int applied = -123;
		assert(uvcd_ctrl_set(&g, uvcd_ctrl_find(names[i]), 200, &applied) == -1);
		assert(applied == -123 && !g.config_dirty);
		assert(memcmp(&old, &p.controls, sizeof(old)) == 0);
		assert(g.read_seq == 0 && g.watched_seq == 0 && g.healthy_since_ms == 0);
		assert(g.last_frame_ms > 0);
		/* The restored stream's very first frame can be delivered. */
		p.frame.seq = 1;
		int before = queued;
		deliver_frame(&g);
		assert(queued == before + 1 && g.read_seq == 1);
		assert(memcmp(data, output, sizeof(data)) == 0);
		pthread_mutex_destroy(&p.frame.lock);
	}
	apply_error = 0;
	apply_resets_sequence = false;
}

static void camera_write(gadget_t *g, uint8_t selector, int a, int b)
{
	struct uvc_request_data d = {.length = camera_control_length(selector)};
	unsigned parts = camera_control_parts(selector);
	put_le(d.data, a, d.length / parts);
	if (parts == 2)
		put_le(d.data + d.length / 2, b, d.length / 2);
	g->last_cs = selector;
	g->last_request = UVC_SET_CUR;
	g->last_intf = UVC_INTF_CONTROL;
	g->last_unit = UVCD_CAMERA_TERMINAL_ID;
	handle_data_event(g, &d);
}

/* While a stream runs, a burst of zoom/pan/tilt writes re-creates the
 * stream at most once per interval, and the last value asked for lands. */
static void test_view_throttle(void)
{
	uvcd_pipeline_t p = {0};
	pthread_mutex_init(&p.frame.lock, NULL);
	uvcd_config_defaults(&p.controls);
	gadget_t g = {.pipe = &p, .streaming = true};
	int before = applies;

	camera_write(&g, UVC_CT_ZOOM_ABSOLUTE_CONTROL, 150, 0); /* at once */
	assert(applies == before + 1 && p.controls.zoom == 150);
	for (int z = 160; z <= 200; z += 10)
		camera_write(&g, UVC_CT_ZOOM_ABSOLUTE_CONTROL, z, 0);
	camera_write(&g, UVC_CT_PANTILT_ABSOLUTE_CONTROL, 3600, -7200);
	view_tick(&g);
	assert(applies == before + 1 && p.controls.zoom == 150 && p.controls.pan == 0);
	/* Other controls are not held back. */
	camera_write(&g, UVC_CT_AE_PRIORITY_CONTROL, 1, 0);
	assert(applies == before + 2 && p.controls.ae_priority == 1);

	g.view_applied_ms -= UVCD_VIEW_INTERVAL_MS;
	view_tick(&g);
	assert(applies == before + 5); /* zoom, pan, tilt */
	assert(p.controls.zoom == 200 && p.controls.pan == 3600 && p.controls.tilt == -7200);
	view_tick(&g);
	assert(applies == before + 5);

	/* Without a stream a view change costs nothing: no throttle. */
	g.streaming = false;
	camera_write(&g, UVC_CT_ZOOM_ABSOLUTE_CONTROL, 120, 0);
	camera_write(&g, UVC_CT_ZOOM_ABSOLUTE_CONTROL, 130, 0);
	assert(applies == before + 7 && p.controls.zoom == 130);

	/* A factory reset drops a held-back view rather than undo it later. */
	g.streaming = true;
	camera_write(&g, UVC_CT_ZOOM_ABSOLUTE_CONTROL, 250, 0);
	assert(g.view_pending_set[0]);
	assert(uvcd_ctrl_reset(&g) == 0);
	g.view_applied_ms -= UVCD_VIEW_INTERVAL_MS;
	view_tick(&g);
	assert(p.controls.zoom == UVCD_ZOOM_DEF);
	pthread_mutex_destroy(&p.frame.lock);
}

/* One control request through the SETUP path; the reply's length, -1 for
 * a stall. */
static int setup(gadget_t *g, uint8_t dir, uint8_t request, uint8_t cs, uint8_t unit,
		 uint8_t intf, uint16_t length)
{
	struct usb_ctrlrequest req = {.bRequestType = dir | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
				      .bRequest = request, .wValue = cs << 8,
				      .wIndex = unit << 8 | intf, .wLength = length};
	last_response.length = -2;
	handle_setup_event(g, &req);
	return last_response.length;
}

static int error_code(gadget_t *g)
{
	assert(setup(g, USB_DIR_IN, UVC_GET_CUR, UVC_VC_REQUEST_ERROR_CODE_CONTROL, 0,
		     UVC_INTF_CONTROL, 1) == 1);
	return last_response.data[0];
}

/* A stall leaves its reason in the request error code control, which
 * uvcvideo reads after each one; any request that succeeds clears it,
 * reading the code included. */
static void test_request_error_code(void)
{
	uvcd_pipeline_t p = {0};
	gadget_t g = {.pipe = &p};
	uvcd_config_defaults(&p.controls);

	assert(error_code(&g) == UVCD_REQUEST_ERROR_NONE);
	assert(setup(&g, USB_DIR_IN, UVC_GET_INFO, UVC_VC_REQUEST_ERROR_CODE_CONTROL, 0,
		     UVC_INTF_CONTROL, 1) == 1 && last_response.data[0] == UVC_CONTROL_CAP_GET);

	static const struct {
		uint8_t dir, request, cs, unit, intf, code;
	} stalls[] = {
		/* a retired unit 4 selector, a removed PU control */
		{USB_DIR_IN, UVC_GET_CUR, 8, UVCD_CUSTOM_UNIT_ID, UVC_INTF_CONTROL,
		 UVCD_REQUEST_ERROR_INVALID_CONTROL},
		{USB_DIR_IN, UVC_GET_CUR, UVC_PU_BACKLIGHT_COMPENSATION_CONTROL,
		 UVCD_PROCESSING_UNIT_ID, UVC_INTF_CONTROL, UVCD_REQUEST_ERROR_INVALID_CONTROL},
		/* a real control, asked something it doesn't answer */
		{USB_DIR_OUT, UVC_GET_CUR, UVC_PU_BRIGHTNESS_CONTROL, UVCD_PROCESSING_UNIT_ID,
		 UVC_INTF_CONTROL, UVCD_REQUEST_ERROR_INVALID_REQUEST},
		{USB_DIR_IN, UVC_RC_UNDEFINED, UVC_CT_ZOOM_ABSOLUTE_CONTROL, UVCD_CAMERA_TERMINAL_ID,
		 UVC_INTF_CONTROL, UVCD_REQUEST_ERROR_INVALID_REQUEST},
		/* the firmware exchange with no update service behind it */
		{USB_DIR_IN, UVC_GET_LEN, UVCD_FW_STATUS_SELECTOR, UVCD_CUSTOM_UNIT_ID,
		 UVC_INTF_CONTROL, UVCD_REQUEST_ERROR_INVALID_CONTROL},
		/* the output terminal, and the error code control itself */
		{USB_DIR_IN, UVC_GET_CUR, 1, 3, UVC_INTF_CONTROL, UVCD_REQUEST_ERROR_INVALID_UNIT},
		{USB_DIR_IN, UVC_GET_CUR, 1, 0, UVC_INTF_CONTROL, UVCD_REQUEST_ERROR_INVALID_CONTROL},
		{USB_DIR_OUT, UVC_SET_CUR, UVC_VC_REQUEST_ERROR_CODE_CONTROL, 0, UVC_INTF_CONTROL,
		 UVCD_REQUEST_ERROR_INVALID_REQUEST},
		{USB_DIR_IN, UVC_GET_MIN, UVC_VC_REQUEST_ERROR_CODE_CONTROL, 0, UVC_INTF_CONTROL,
		 UVCD_REQUEST_ERROR_INVALID_REQUEST},
		/* streaming: probe asked GET_INFO, an unknown selector */
		{USB_DIR_IN, UVC_GET_INFO, UVC_VS_PROBE_CONTROL, 0, UVC_INTF_STREAMING,
		 UVCD_REQUEST_ERROR_INVALID_REQUEST},
		{USB_DIR_IN, UVC_GET_CUR, 9, 0, UVC_INTF_STREAMING, UVCD_REQUEST_ERROR_INVALID_CONTROL},
	};
	for (size_t i = 0; i < sizeof(stalls) / sizeof(stalls[0]); i++) {
		assert(setup(&g, stalls[i].dir, stalls[i].request, stalls[i].cs, stalls[i].unit,
			     stalls[i].intf, 2) == -1);
		assert(error_code(&g) == stalls[i].code);
		assert(error_code(&g) == UVCD_REQUEST_ERROR_NONE);
	}

	/* Any other request that succeeds clears it as well. */
	assert(setup(&g, USB_DIR_IN, UVC_GET_CUR, 8, UVCD_CUSTOM_UNIT_ID, UVC_INTF_CONTROL, 2) == -1);
	assert(setup(&g, USB_DIR_IN, UVC_GET_CUR, UVC_PU_BRIGHTNESS_CONTROL,
		     UVCD_PROCESSING_UNIT_ID, UVC_INTF_CONTROL, 2) == 2);
	assert(error_code(&g) == UVCD_REQUEST_ERROR_NONE);
}

/* Under auto white balance the temperature reads what auto measured, and
 * GET_INFO flags it auto-update so uvcvideo asks again instead of caching
 * the first answer. */
static void test_live_wb_temperature(void)
{
	uvcd_pipeline_t p = {0};
	gadget_t g = {.pipe = &p};
	uvcd_config_defaults(&p.controls);
	p.controls.wb_temp = 5000;
	assert(p.controls.wb_auto);

	static const struct {
		bool wb_auto;
		int ct, reads;
	} cases[] = {
		{true, 4200, 4200},
		{true, 9000, UVCD_WB_TEMP_MAX},
		{true, 1000, UVCD_WB_TEMP_MIN},
		{true, -1, 5000}, /* no estimate: the stored value */
		{false, 4200, 5000},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		p.controls.wb_auto = cases[i].wb_auto;
		awb_ct = cases[i].ct;
		assert(setup(&g, USB_DIR_IN, UVC_GET_CUR, UVC_PU_WHITE_BALANCE_TEMPERATURE_CONTROL,
			     UVCD_PROCESSING_UNIT_ID, UVC_INTF_CONTROL, 2) == 2);
		assert((last_response.data[0] | last_response.data[1] << 8) == cases[i].reads);
	}
	/* The rest of the control, and the stored value, stay as they were. */
	p.controls.wb_auto = 1;
	awb_ct = 4200;
	assert(setup(&g, USB_DIR_IN, UVC_GET_DEF, UVC_PU_WHITE_BALANCE_TEMPERATURE_CONTROL,
		     UVCD_PROCESSING_UNIT_ID, UVC_INTF_CONTROL, 2) == 2);
	assert((last_response.data[0] | last_response.data[1] << 8) == UVCD_WB_TEMP_DEF);
	assert(p.controls.wb_temp == 5000);
	awb_ct = -1;

	for (uint8_t cs = 1; cs <= UVC_PU_WHITE_BALANCE_COMPONENT_AUTO_CONTROL; cs++) {
		if (setup(&g, USB_DIR_IN, UVC_GET_INFO, cs, UVCD_PROCESSING_UNIT_ID,
			  UVC_INTF_CONTROL, 1) < 0)
			continue;
		bool autoupdate = last_response.data[0] & UVC_CONTROL_CAP_AUTOUPDATE;
		assert(autoupdate == (cs == UVC_PU_WHITE_BALANCE_TEMPERATURE_CONTROL));
	}
}

/* MJPEG frames that keep outgrowing the cap cost this stream 10 quality
 * at a time, down to 10, instead of a dead stream and the watchdog. */
static void test_frame_size_fallback(void)
{
	uvcd_pipeline_t p = {0};
	pthread_mutex_init(&p.frame.lock, NULL);
	gadget_t g = {.pipe = &p, .streaming = true};
	uvcd_config_defaults(&p.controls);
	p.controls.mjpeg_quality = 100;
	p.running = true;
	p.cur_format = UVCD_FMT_MJPEG;
	p.frame.cap = 2048 * 1024;
	int before = restarts;

	p.frame.too_large = UVCD_TOO_LARGE_RUN - 1;
	frame_size_tick(&g);
	assert(restarts == before && p.mjpeg_quality_cut == 0);
	p.frame.too_large = UVCD_TOO_LARGE_RUN;
	frame_size_tick(&g);
	assert(restarts == before + 1 && p.mjpeg_quality_cut == 10);
	/* The restart starts a fresh run; a frame that fits ends one too. */
	p.frame.too_large = 0;
	frame_size_tick(&g);
	assert(restarts == before + 1);
	/* Down to the floor, never past it. */
	p.controls.mjpeg_quality = 25;
	p.mjpeg_quality_cut = 10;
	p.frame.too_large = UVCD_TOO_LARGE_RUN;
	frame_size_tick(&g);
	assert(restarts == before + 2 && p.mjpeg_quality_cut == 15);
	frame_size_tick(&g);
	assert(restarts == before + 2 && p.mjpeg_quality_cut == 15);
	/* H.264 is bitrate-controlled: a drop only asks for an IDR. */
	p.cur_format = UVCD_FMT_H264;
	p.controls.mjpeg_quality = 100;
	p.mjpeg_quality_cut = 0;
	frame_size_tick(&g);
	assert(restarts == before + 2 && p.mjpeg_quality_cut == 0);

	/* A new quality starts from scratch. */
	p.cur_format = UVCD_FMT_MJPEG;
	p.mjpeg_quality_cut = 30;
	int applied;
	assert(uvcd_ctrl_set(&g, uvcd_ctrl_find("mjpeg-quality"), 90, &applied) == 0);
	assert(applied == 90 && p.mjpeg_quality_cut == 0 && restarts == before + 3);
	pthread_mutex_destroy(&p.frame.lock);
}

static void test_gain_headroom(void)
{
	uvcd_pipeline_t p = {0};
	gadget_t g = {.pipe = &p};
	uvcd_config_defaults(&p.controls);
	const struct uvcd_ctrl_def *again = uvcd_ctrl_find("max-analog-gain");
	const struct uvcd_ctrl_def *dgain = uvcd_ctrl_find("max-digital-gain");
	assert(custom_control_max(UVCD_CUSTOM_MAX_AGAIN) == 223);
	assert(custom_control_def(UVCD_CUSTOM_MAX_AGAIN) == 160);
	assert(custom_control_max(UVCD_CUSTOM_MAX_DGAIN) == 160);
	assert(p.controls.max_again == 160 && p.controls.max_dgain == 80);
	int applied;
	assert(uvcd_ctrl_set(&g, again, 223, &applied) == 0 && applied == 223);
	assert(p.controls.max_again == 223);
	g.config_dirty = false;
	apply_error = -EIO;
	assert(uvcd_ctrl_set(&g, again, 192, &applied) == -1);
	assert(p.controls.max_again == 223 && !g.config_dirty);
	apply_error = 0;
	/* 224, the old maximum, which libimp ignored, clamps to the real one. */
	assert(uvcd_ctrl_set(&g, again, 224, &applied) == 0 && applied == 223);
	assert(uvcd_ctrl_set(&g, dgain, 224, &applied) == 0 && applied == 160);
}

static void test_encoder_failures(void)
{
	static const char *names[] = {"h264-rate-control", "h264-min-qp", "h264-max-qp",
		"h264-profile", "mjpeg-quality"};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		uvcd_pipeline_t p = {.running = true, .cur_format = i == 4 ? UVCD_FMT_MJPEG : UVCD_FMT_H264,
			.cur_frame = UVCD_FRAME_720P, .cur_interval = 333333};
		uvcd_config_defaults(&p.controls);
		struct uvcd_control_state old = p.controls;
		gadget_t g = {.pipe = &p, .streaming = true, .read_seq = 1000};
		restart_errors = 1;
		int starts = start_calls, applied = -123;
		assert(uvcd_ctrl_set(&g, uvcd_ctrl_find(names[i]), i == 4 ? 60 : 1, &applied) == -1);
		assert(p.running && start_calls == starts + 1 && !g.config_dirty);
		assert(applied == -123 && g.read_seq == 0);
		assert(memcmp(&old, &p.controls, sizeof(old)) == 0);
		assert(memcmp(&old, &restarted_controls, sizeof(old)) == 0);
	}
	uvcd_pipeline_t p = {.running = true, .cur_format = UVCD_FMT_H264};
	uvcd_config_defaults(&p.controls);
	gadget_t g = {.pipe = &p, .streaming = true};
	int calls = bitrate_calls, applied;
	bitrate_errors = 1;
	assert(uvcd_ctrl_set(&g, uvcd_ctrl_find("h264-bitrate-kbps"), 2000, &applied) == -1);
	assert(bitrate_calls == calls + 2 && restarted_controls.h264_bitrate_kbps == 0);
	assert(p.controls.h264_bitrate_kbps == 0 && !g.config_dirty);
	/* An inactive format is staged, and an unchanged value never restarts. */
	calls = restarts;
	p.cur_format = UVCD_FMT_MJPEG;
	assert(uvcd_ctrl_set(&g, uvcd_ctrl_find("h264-profile"), 0, &applied) == 0);
	assert(restarts == calls && p.controls.h264_profile == 0);
	assert(uvcd_ctrl_set(&g, uvcd_ctrl_find("mjpeg-quality"), 80, &applied) == 0);
	assert(restarts == calls);
	/* Crossed bounds are rejected before they reach the pipeline. */
	p.controls.h264_min_qp = 30;
	calls = applies;
	assert(uvcd_ctrl_set(&g, uvcd_ctrl_find("h264-max-qp"), 20, &applied) == -1);
	assert(applies == calls && p.controls.h264_max_qp == -1);

	char reply_buf[128];
	assert(uvcd_ctl_command(&g, "info h264-max-qp", reply_buf, sizeof(reply_buf)) == 0);
	assert(!strcmp(reply_buf, "-1 -1 51 -1\nok\n"));
	assert(uvcd_ctl_command(&g, "info auto-exposure", reply_buf, sizeof(reply_buf)) == 0);
	assert(!strcmp(reply_buf, "8 1 8 8\nok\n"));
	assert(uvcd_ctl_command(&g, "info unknown", reply_buf, sizeof(reply_buf)) == 0);
	assert(!strcmp(reply_buf, "err unknown control\n"));
	assert(uvcd_ctl_command(&g, "info brightness", reply_buf, 4) == -1);
	assert(uvcd_ctl_command(&g, "set brightness 9999999999999999999999999999", reply_buf, sizeof(reply_buf)) == 0);
	assert(!strcmp(reply_buf, "err bad value\n"));
	int before = good_saves;
	apply_error = -EIO;
	assert(uvcd_ctl_command(&g, "reset", reply_buf, sizeof(reply_buf)) == 0);
	assert(!strcmp(reply_buf, "err reset incomplete\n") && good_saves == before);
	apply_error = 0;
	p.running = false;
	save_errors = 1;
	assert(uvcd_ctrl_reset(&g) == -1 && good_saves == before);
}

int main(void)
{
	uvcd_pipeline_t p = {0};
	pthread_mutex_init(&p.frame.lock, NULL);
	gadget_t g = {.pipe = &p};

	/* Sharpness stops where frames still fit the USB cap. Backlight and
	 * defog are gone, and their requests stall like any unknown one. */
	assert(standard_ctrl_def(UVC_PU_SHARPNESS_CONTROL)->max == 180);
	assert(standard_ctrl_def(UVC_PU_BACKLIGHT_COMPENSATION_CONTROL) == NULL);
	assert(control_value(&g, UVC_PU_BACKLIGHT_COMPENSATION_CONTROL) == NULL);
	assert(custom_ctrl_def(8) == NULL && custom_control_value(&g, 8) == NULL);
	uvcd_config_defaults(&p.controls);
	p.good = p.controls;
	int applied;
	const struct uvcd_ctrl_def *brightness = uvcd_ctrl_find("brightness");

	/* Every change persists, idle ones too, once a burst has settled. */
	assert(uvcd_ctrl_set(&g, brightness, 140, &applied) == 0 && applied == 140);
	assert(uvcd_ctrl_set(&g, brightness, 150, &applied) == 0 && applied == 150);
	config_flush(&g, false);
	assert(saves == 0 && g.config_dirty);
	g.config_dirty_at -= UVCD_CONFIG_SETTLE_MS;
	config_flush(&g, false);
	assert(saves == 1 && last_saved.brightness == 150 && !g.config_dirty);
	config_flush(&g, true);
	assert(saves == 1); /* nothing pending */
	/* Rewriting a value it already has changes nothing. */
	assert(uvcd_ctrl_set(&g, brightness, 150, &applied) == 0 && applied == 150);
	assert(!g.config_dirty);
	/* Idle changes go to the camera-side guard in RAM, never the stream's
	 * on flash: an idle camera unplugged is not a crash. */
	assert(streak_writes == 0 && unlinks == 0);
	assert(camera_guard_arms == 2);

	/* A change the camera rejects is neither stored nor persisted. */
	int was_applies = applies;
	apply_error = -EIO;
	assert(uvcd_ctrl_set(&g, brightness, 99, &applied) == -1);
	apply_error = 0;
	assert(applies == was_applies + 1 && last_field == brightness->state_offset);
	assert(p.controls.brightness == 150 && !g.config_dirty);
	/* It was counted before it was applied: a crash while applying it is
	 * caught too. */
	assert(camera_guard_arms == 3);
	/* What the camera filled in is stored with the change. */
	const struct uvcd_ctrl_def *wb_auto = uvcd_ctrl_find("white-balance-auto");
	assert(uvcd_ctrl_set(&g, wb_auto, 0, &applied) == 0 && applied == 0);
	assert(p.controls.wb_auto == 0 && p.controls.wb_temp == 3300 && g.config_dirty);
	g.config_dirty_at -= UVCD_CONFIG_SETTLE_MS;
	config_flush(&g, false);
	assert(last_saved.wb_temp == 3300 && last_saved.wb_auto == 0);
	int arms = camera_guard_arms;

	/* A stream is counted in the crash guard from its start, once, on top
	 * of the streams before it that died unproven. */
	p.unproven_streak = 1;
	stream_guard_arm(&g);
	stream_guard_arm(&g);
	assert(streak_writes == 1 && last_streak == 2 && g.guard_armed);

	g.streaming = true;
	g.healthy_since_ms = uvcd_monotonic_ms() - 9000;
	g.last_frame_ms = uvcd_monotonic_ms();
	stream_guard_tick(&g);
	assert(good_saves == 0 && g.guard_armed && unlinks == 0);
	g.healthy_since_ms -= 2000;
	stream_guard_tick(&g);
	/* Proven healthy: the running controls become what a crash falls
	 * back to, the guard file goes and the streak resets. */
	assert(good_saves == 1 && last_good.brightness == 150 && p.good.brightness == 150);
	assert(!g.guard_armed && p.unproven_streak == 0 && unlinks == 1);
	stream_guard_clear(&g);
	assert(unlinks == 1); /* nothing armed, nothing to clear */
	stream_guard_tick(&g);
	assert(good_saves == 1); /* proven once is enough */

	/* A change mid-stream has to prove itself again before it is what a
	 * crash falls back to; a rewrite of the same value does not. */
	assert(uvcd_ctrl_set(&g, brightness, 150, &applied) == 0);
	assert(!g.guard_armed && streak_writes == 1);
	assert(uvcd_ctrl_set(&g, brightness, 160, &applied) == 0);
	assert(g.guard_armed && streak_writes == 2 && last_streak == 1);
	assert(camera_guard_arms == arms);
	assert(g.healthy_since_ms == 0 && p.good.brightness == 150);
	g.healthy_since_ms = uvcd_monotonic_ms() - UVCD_CONFIG_HEALTHY_MS;
	stream_guard_tick(&g);
	assert(good_saves == 2 && p.good.brightness == 160 && !g.guard_armed);
	/* ...and a clean stop before then does not prove it. */
	assert(uvcd_ctrl_set(&g, brightness, 170, &applied) == 0 && g.guard_armed);
	stream_guard_clear(&g);
	assert(good_saves == 2 && p.good.brightness == 160);
	g.config_dirty_at -= UVCD_CONFIG_SETTLE_MS;
	config_flush(&g, false);
	assert(last_saved.brightness == 170);

	uint8_t data[] = {1, 2, 3, 4}, output[4] = {0};
	p.frame.data = data;
	p.frame.len = sizeof(data);
	p.frame.seq = 1;
	g.cur_format = UVCD_FMT_H264;
	g.buf_count = 1;
	g.buf_queued[0] = true;
	g.buffers[0].start = output;
	g.buffers[0].length = sizeof(output);
	g.stalled_since_ms = uvcd_monotonic_ms() - 10000;
	deliver_frame(&g);
	assert(g.streaming && queued == 0 && g.read_seq == 0);
	host_reading = true;
	deliver_frame(&g);
	assert(g.streaming && queued == 1 && g.read_seq == 1 && idrs == 1);
	assert(memcmp(data, output, sizeof(data)) == 0);

	/* A frame bigger than the gadget buffer is dropped whole: never
	 * truncated, and never answered by lowering the quality. */
	uint8_t big[8] = {0};
	g.cur_format = UVCD_FMT_MJPEG;
	p.controls.mjpeg_quality = 80;
	p.frame.data = big;
	p.frame.len = sizeof(big);
	p.frame.seq = 2;
	int was_queued = queued, was_idrs = idrs;
	deliver_frame(&g);
	assert(queued == was_queued && idrs == was_idrs + 1 && restarts == 0);
	assert(g.read_seq == 2 && p.controls.mjpeg_quality == 80);
	assert(g.healthy_since_ms == 0);
	/* The next frame that fits goes out on the buffer the drop kept. */
	p.frame.data = data;
	p.frame.len = sizeof(data);
	p.frame.seq = 3;
	deliver_frame(&g);
	assert(queued == was_queued + 1 && g.read_seq == 3);
	assert(uvcd_ctrl_find("power-line-frequency")->def == 0);
	assert(uvcd_ctrl_find("spatial-denoise")->def == 0);

	/* Factory reset is written at once, and replaces the proven controls
	 * so a later crash cannot bring back what it threw away. Even when
	 * nothing differs from factory, it leaves the file at factory. */
	g.streaming = false;
	int was_saves = saves, was_good = good_saves;
	uvcd_ctrl_reset(&g);
	assert(saves == was_saves + 1 && last_saved.brightness == 128 && !g.config_dirty);
	assert(good_saves == was_good + 1 && last_good.brightness == 128);
	assert(p.good.brightness == 128 && p.controls.brightness == 128);
	uvcd_ctrl_reset(&g);
	assert(saves == was_saves + 2 && good_saves == was_good + 2);

	/* The retired explicit-save and GOP selectors answer like any unknown
	 * one, and a saved GOP no longer names a control. */
	for (unsigned sel = 23; sel <= 25; sel++)
		assert(custom_ctrl_def(sel) == NULL && custom_control_value(&g, sel) == NULL);
	assert(custom_ctrl_def(15) == NULL && custom_control_value(&g, 15) == NULL);
	assert(uvcd_ctrl_find("h264-gop-frames") == NULL);
	test_rejected_view();
	test_request_error_code();
	test_live_wb_temperature();
	test_frame_size_fallback();
	test_view_throttle();
	test_gain_headroom();
	test_encoder_failures();
	test_firmware_routing();
	test_trial_selftest();
	puts("gadget regression tests passed");
	pthread_mutex_destroy(&p.frame.lock);
	return 0;
}
