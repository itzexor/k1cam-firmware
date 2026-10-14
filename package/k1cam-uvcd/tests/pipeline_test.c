/* Host regression tests. Link with --gc-sections to discard hardware paths. */
#include <assert.h>
#include "../src/uvcd_pipeline.c"

/* Smallest sensor crop measured to stream at each output size. One step
 * past these made the ISP fail. At 100 MHz the last working zoom was 131,
 * 250, 342, then 400 for the rest; at 200 MHz, 300 for 1080p and 400 for
 * the rest. */
struct crop {
	int w, h;
};
static const struct crop proven_slow[UVCD_NUM_FRAMES + 1] = {
	[UVCD_FRAME_1080P] = {1464, 824},
	[UVCD_FRAME_1280X960] = {576, 432},
	[UVCD_FRAME_720P] = {560, 314},
	[UVCD_FRAME_800X600] = {360, 270},
	[UVCD_FRAME_640X480] = {360, 270},
	[UVCD_FRAME_360P] = {480, 270},
};
static const struct crop proven_fast[UVCD_NUM_FRAMES + 1] = {
	[UVCD_FRAME_1080P] = {640, 360},
	[UVCD_FRAME_1280X960] = {360, 270},
	[UVCD_FRAME_720P] = {480, 270},
	[UVCD_FRAME_800X600] = {360, 270},
	[UVCD_FRAME_640X480] = {360, 270},
	[UVCD_FRAME_360P] = {480, 270},
};

static rss_fs_config_t fs;
static rss_video_config_t enc;
static uint32_t stream_size;
static int stream_count, buffer_error, encoder_calls, destroyed;

void uvcd_log(int level, const char *fmt, ...)
{
	(void)level;
	(void)fmt;
}

static int channel_ok(void *ctx, int chn)
{
	(void)ctx;
	(void)chn;
	return 0;
}

static int destroy(void *ctx, int chn)
{
	destroyed++;
	return channel_ok(ctx, chn);
}

static int depth_ok(void *ctx, int chn, int depth)
{
	(void)depth;
	return channel_ok(ctx, chn);
}

static int create_fs(void *ctx, int chn, const rss_fs_config_t *cfg)
{
	fs = *cfg;
	return channel_ok(ctx, chn);
}

static int set_size(void *ctx, int chn, uint32_t bytes)
{
	(void)ctx;
	(void)chn;
	stream_size = bytes;
	return buffer_error;
}

static int set_count(void *ctx, int chn, int count)
{
	stream_count = count;
	return channel_ok(ctx, chn);
}

static int create_enc(void *ctx, int chn, const rss_video_config_t *cfg)
{
	(void)ctx;
	(void)chn;
	assert(stream_size > 0 && stream_count > 0);
	enc = *cfg;
	encoder_calls++;
	return -EIO; /* Stop after capturing the complete configuration. */
}

/* Subject metering: fake ISP statistics in, weights and compensation out. */
static uint32_t fake_zone[15][15], fake_hist[256];
static uint8_t written_weight[15][15];
static int weight_writes, written_comp = -1, weight_error, comp_error;

static int get_zone(void *ctx, uint32_t zone[15][15])
{
	(void)ctx;
	memcpy(zone, fake_zone, sizeof(fake_zone));
	return 0;
}

static int get_hist(void *ctx, uint32_t bins[256])
{
	(void)ctx;
	memcpy(bins, fake_hist, sizeof(fake_hist));
	return 0;
}

static int set_weight(void *ctx, const uint8_t weight[15][15])
{
	(void)ctx;
	weight_writes++;
	if (weight_error)
		return weight_error; /* a rejected write leaves the ISP as it was */
	memcpy(written_weight, weight, sizeof(written_weight));
	return 0;
}

static int set_comp(void *ctx, int comp)
{
	(void)ctx;
	if (comp_error)
		return comp_error;
	written_comp = comp;
	return 0;
}

/* A dark bed (zones at 40) with a bright print over a 3x3 block left of
 * centre, and a histogram whose brightest 1% sits at `top_bin`. */
static void fake_scene(uint32_t print, int top_bin)
{
	for (int r = 0; r < 15; r++)
		for (int col = 0; col < 15; col++)
			fake_zone[r][col] = r >= 8 && r <= 10 && col >= 3 && col <= 5 ? print : 40;
	memset(fake_hist, 0, sizeof(fake_hist));
	fake_hist[60] = 990;
	fake_hist[top_bin] = 10;
}

static void test_subject_metering(void)
{
	rss_hal_ops_t ops = {
		.isp_get_ae_zone = get_zone,
		.isp_get_ae_hist_origin = get_hist,
		.isp_set_ae_weight = set_weight,
		.isp_set_ae_comp = set_comp,
	};
	uvcd_pipeline_t p = {.ops = &ops, .sensor_w = 1920, .sensor_h = 1080, .running = true};
	p.view.w = 1920;
	p.view.h = 1080;
	p.controls.ae_mode = UVCD_AE_APERTURE_PRIORITY;
	p.controls.ae_comp = 128;
	p.controls.metering = UVCD_METERING_SUBJECT;

	/* Starts out center-weighted. */
	uint8_t center[15][15];
	metering_weights(&p, UVCD_METERING_CENTER, center);
	assert(uvcd_apply_metering(&p, &p.controls) == 0);
	assert(memcmp(written_weight, center, sizeof(center)) == 0);
	assert(center[9][4] < 8 && center[7][7] == 8 && center[0][0] == 2);

	/* The print eases up to full weight; the bed keeps center weights. */
	fake_scene(400, 175);
	for (int i = 0; i < 8; i++)
		subject_weights(&p);
	assert(written_weight[9][4] == 8 && written_weight[8][3] == 8);
	assert(written_weight[0][0] == center[0][0] && written_weight[12][12] == center[12][12]);
	/* Unchanged statistics write nothing new. */
	int writes = weight_writes;
	subject_weights(&p);
	assert(weight_writes == writes);
	/* A zone just a little brighter than the bed is not a subject. */
	fake_scene(56, 200);
	for (int i = 0; i < 8; i++)
		subject_weights(&p);
	assert(memcmp(written_weight, center, sizeof(center)) == 0);
	/* Nor is near-black noise: 3 against a median of 1 used to be a
	 * full-weight subject. */
	for (int r = 0; r < 15; r++)
		for (int col = 0; col < 15; col++)
			fake_zone[r][col] = 1;
	fake_zone[9][4] = 3;
	for (int i = 0; i < 8; i++)
		subject_weights(&p);
	assert(memcmp(written_weight, center, sizeof(center)) == 0);

	/* Weights the ISP rejected are not taken as written: with the print's
	 * weights still in the ISP, a rejected reset to center-weighted must
	 * be retried, not skipped as already in place. */
	fake_scene(400, 175);
	for (int i = 0; i < 8; i++)
		subject_weights(&p);
	assert(written_weight[9][4] == 8);
	weight_error = -EIO;
	assert(uvcd_apply_metering(&p, &p.controls) == -EIO);
	weight_error = 0;
	fake_scene(56, 175);
	for (int i = 0; i < 8; i++)
		subject_weights(&p);
	assert(memcmp(written_weight, center, sizeof(center)) == 0);

	/* A rejected compensation write leaves the guard where it was. */
	fake_scene(400, 255);
	comp_error = -EIO;
	highlight_guard(&p);
	assert(p.ae_comp_cut == 0.0f);
	comp_error = 0;
	/* A small light source may clip without holding the scene back. */
	memset(fake_hist, 0, sizeof(fake_hist));
	fake_hist[60] = 992;
	fake_hist[255] = 8; /* 0.8% */
	written_comp = -1;
	highlight_guard(&p);
	assert(written_comp == -1 && p.ae_comp_cut == 0.0f);

	/* Clipping highlights hold compensation back, down to the floor. */
	fake_scene(400, 255);
	highlight_guard(&p);
	assert(written_comp == 109); /* 128 * 0.85 */
	for (int i = 0; i < 20; i++)
		highlight_guard(&p);
	assert(written_comp == 64); /* never below half */
	/* Between the thresholds it holds still. */
	fake_scene(400, 175);
	written_comp = -1;
	highlight_guard(&p);
	assert(written_comp == -1);
	/* Well below clipping, it gives the compensation back, never more. */
	fake_scene(400, 120);
	for (int i = 0; i < 30; i++)
		highlight_guard(&p);
	assert(written_comp == 128 && p.ae_comp_cut == 0.0f);

	/* The control itself still applies under the guard ... */
	fake_scene(400, 255);
	highlight_guard(&p);
	struct uvcd_control_state c = p.controls;
	c.ae_comp = 200;
	assert(uvcd_apply_ae_comp(&p, &c) == 0 && written_comp == 170);
	/* ... and leaving subject metering gives back what it held. */
	c.metering = UVCD_METERING_CENTER;
	assert(uvcd_apply_metering(&p, &c) == 0);
	assert(written_comp == 200 && p.ae_comp_cut == 0.0f);
	assert(memcmp(written_weight, center, sizeof(center)) == 0);

	/* Nothing to do in manual exposure or other modes. */
	writes = weight_writes;
	p.subject_checked_ms = 0;
	p.controls.metering = UVCD_METERING_CENTER;
	subject_metering_tick(&p);
	p.controls.metering = UVCD_METERING_SUBJECT;
	p.controls.ae_mode = UVCD_AE_MANUAL;
	subject_metering_tick(&p);
	assert(weight_writes == writes);
}

int main(void)
{
	rss_hal_ops_t ops = {
		.fs_create_channel = create_fs,
		.fs_destroy_channel = destroy,
		.fs_set_fifo = depth_ok,
		.fs_set_frame_depth = depth_ok,
		.enc_create_group = channel_ok,
		.enc_destroy_group = destroy,
		.enc_set_stream_buf_size = set_size,
		.enc_set_max_stream_cnt = set_count,
		.enc_create_channel = create_enc,
	};
	uvcd_pipeline_t p = {.ops = &ops, .sensor_w = 1920, .sensor_h = 1080};
	p.controls.mjpeg_quality = 80;
	p.controls.h264_min_qp = p.controls.h264_max_qp = -1;
	for (int frame = 1; frame <= UVCD_NUM_FRAMES; frame++) {
		const struct uvcd_frame_info *fi = &uvcd_frames[frame];
		/* The pump's copy holds any frame the gadget can send, and the
		 * gadget takes about a byte per pixel. */
		assert(fi->max_size <= UVCD_FRAME_BUF_CAP);
		assert(fi->max_size >= (uint32_t)fi->width * fi->height);
		assert(fi->max_zoom >= UVCD_ZOOM_MIN && fi->max_zoom <= UVCD_ZOOM_MAX);
		assert(fi->max_zoom_slow >= UVCD_ZOOM_MIN && fi->max_zoom_slow <= fi->max_zoom);
	}
	/* 0: the clock couldn't be read, which must get the slow caps. */
	static const int clocks[] = {0, 100000000, UVCD_ISP_FAST_HZ, 250000000};
	for (size_t ci = 0; ci < sizeof(clocks) / sizeof(clocks[0]); ci++) {
		p.isp_clk_hz = clocks[ci];
		const struct crop *proven = p.isp_clk_hz >= UVCD_ISP_FAST_HZ ? proven_fast : proven_slow;
		int smallest_w = 1920;
		for (int frame = 1; frame <= UVCD_NUM_FRAMES; frame++) {
			for (int zoom = 100; zoom <= 400; zoom += 25) {
				for (int pan = -UVCD_PANTILT_MAX; pan <= UVCD_PANTILT_MAX;
				     pan += UVCD_PANTILT_MAX) {
					p.controls.zoom = zoom;
					p.controls.pan = pan;
					p.controls.tilt = -pan;
					assert(configure_channel(&p, UVCD_FMT_MJPEG, frame, 333333) == -EIO);
					assert(!fs.crop.enable);
					assert(fs.width == uvcd_frames[frame].width);
					assert(fs.height == uvcd_frames[frame].height);
					if (fs.fcrop.enable) {
						assert(fs.fcrop.w > 0 && fs.fcrop.h > 0);
						assert(fs.fcrop.x + fs.fcrop.w <= 1920);
						assert(fs.fcrop.y + fs.fcrop.h <= 1080);
					}
					/* Never a smaller crop, i.e. more upscale, than was
					 * seen streaming on the K1 at this output size. */
					int cw = fs.fcrop.enable ? fs.fcrop.w : 1920;
					int ch = fs.fcrop.enable ? fs.fcrop.h : 1080;
					assert(cw >= proven[frame].w && ch >= proven[frame].h);
					if (frame == UVCD_FRAME_1080P && cw < smallest_w)
						smallest_w = cw;
					assert(stream_size >= fs.width * fs.height * 4);
					assert(stream_count == 1);
					assert(enc.init_qp == 80);
				}
			}
		}
		/* The fast clock really does zoom further at 1080p. */
		assert(p.isp_clk_hz >= UVCD_ISP_FAST_HZ ? smallest_w < 700 : smallest_w > 1400);
	}
	p.isp_clk_hz = 0;
	p.controls.zoom = 100;
	p.controls.pan = p.controls.tilt = 0;
	assert(configure_channel(&p, UVCD_FMT_MJPEG, UVCD_FRAME_1080P, 333333) == -EIO);
	assert(!fs.fcrop.enable && !fs.scaler.enable); /* Preserve all 1080 rows. */
	assert(configure_channel(&p, UVCD_FMT_MJPEG, UVCD_FRAME_1280X960, 333333) == -EIO);
	assert(fs.fcrop.x == 240 && fs.fcrop.y == 0);
	assert(fs.fcrop.w == 1440 && fs.fcrop.h == 1080);
	/* The requested JPEG quality goes to the encoder as is. */
	p.controls.mjpeg_quality = 100;
	assert(configure_channel(&p, UVCD_FMT_MJPEG, UVCD_FRAME_720P, 333333) == -EIO);
	assert(enc.init_qp == 100);
	p.controls.mjpeg_quality = 80;
	assert(configure_channel(&p, UVCD_FMT_H264, UVCD_FRAME_1080P, 333333) == -EIO);
	assert(enc.gop_length == 30 && enc.max_same_scene_cnt == 1);
	assert(stream_size == uvcd_frames[UVCD_FRAME_1080P].max_size && stream_count == 2);
	/* An IDR every second at whatever rate was negotiated. */
	assert(configure_channel(&p, UVCD_FMT_H264, UVCD_FRAME_720P, 666666) == -EIO);
	assert(enc.gop_length == 15 && enc.max_same_scene_cnt == 1);
	assert(configure_channel(&p, UVCD_FMT_H264, UVCD_FRAME_720P, 2000000) == -EIO);
	assert(enc.gop_length == 5 && enc.max_same_scene_cnt == 1);
	assert(configure_channel(&p, UVCD_FMT_H264, UVCD_FRAME_720P, 333333) == -EIO);
	/* Automatic min QP: 20, unless an explicit max QP is below it. */
	assert(enc.min_qp == UVCD_H264_AUTO_MIN_QP && enc.max_qp == -1);
	p.controls.h264_max_qp = 12;
	assert(configure_channel(&p, UVCD_FMT_H264, UVCD_FRAME_720P, 333333) == -EIO);
	assert(enc.min_qp == 12 && enc.max_qp == 12);
	p.controls.h264_min_qp = 30;
	p.controls.h264_max_qp = 40;
	assert(configure_channel(&p, UVCD_FMT_H264, UVCD_FRAME_720P, 333333) == -EIO);
	assert(enc.min_qp == 30 && enc.max_qp == 40);
	p.controls.h264_min_qp = p.controls.h264_max_qp = -1;
	int calls = encoder_calls;
	destroyed = 0;
	buffer_error = -ENOMEM;
	assert(configure_channel(&p, UVCD_FMT_MJPEG, UVCD_FRAME_720P, 333333) == -ENOMEM);
	assert(encoder_calls == calls && destroyed == 2);
	for (uint32_t ceiling = 5; ceiling <= 30; ceiling += 5) {
		uint32_t fps = ceiling;
		for (int i = 0; i < 10; i++)
			fps = fps_step(fps, -1, ceiling);
		assert(fps == (ceiling < 15 ? ceiling : 15));
	}
	test_subject_metering();
	puts("pipeline regression tests passed");
	return 0;
}
