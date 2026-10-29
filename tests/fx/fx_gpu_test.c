/*
 * fx_gpu_test.c — renders Parallaxa FX and Parallaxa Depth Blur in a headless libobs
 * (the installed OBS's obs.dll + libobs-d3d11 or libobs-opengl; no OBS UI,
 * nothing written outside the work folder) and checks the FX behaviour
 * that does not need the OBS application:
 *
 *   - the effects compile on the renderer (no [parallaxa] errors / warnings),
 *   - neutral settings take the skip path and are bit-identical to the
 *     unfiltered image (source and scene),
 *   - the identity LUT is visually identical; a sepia LUT applies at
 *     lut_amount 1 (compared with the analytic sepia matrix),
 *   - exposure and vignette behave, bloom renders,
 *   - a test-pulse flash decays to 0 within its duration and the flash cap
 *     keeps at most 3 flashes per second,
 *   - GPU cost of the Cozy look at 1080p (with and without bloom): median of
 *     per-frame paired gs_timer differences against the same scene without
 *     the filter, under a GPU load (the filter's own probe is printed too),
 *   - fx_set / panic (source procs, the same path as the protocol) reach
 *     the settings through the UI task queue (played here by the main
 *     thread); the filter's own writes are not mistaken for the user's and
 *     the user's later picks in the panel (a look that matches an earlier
 *     write, Reset to preset, Neutral after a panic, an edit made before
 *     the deferred update) always win,
 *   - Parallaxa Depth Blur: strength 0 skips, manual strength blurs, driven_by
 *     is cleared on create, a driven layer with auto_strength 0 skips and
 *     its Strength slider is disabled,
 *   - nothing leaks (bnum_allocs after obs_shutdown equals a baseline run).
 *
 * Usage: fx_gpu_test <parallaxa.dll> <data> <obs bin\64bit dir>
 *                    <d3d11|opengl> <work dir> [--strict-perf]
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <obs.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define W 1920
#define H 1080
#define PX (W * H * 4)

static int failures;
static int stage_errors, stage_warnings;
static bool verbose;

#define FAIL(...)                                  \
	do {                                       \
		fprintf(stderr, "FAIL: " __VA_ARGS__); \
		fprintf(stderr, "\n");             \
		failures++;                        \
	} while (0)

static void log_handler(int level, const char *fmt, va_list args, void *param)
{
	(void)param;
	char msg[4096];
	vsnprintf(msg, sizeof(msg), fmt, args);
	/* Lines of the fx module. Other modules' headless warnings (such as
	 * "obs-websocket is not available") are not checked here. */
	static const char *const fx_prefixes[] = {
		"[parallaxa] fx",         "[parallaxa] effect",         "[parallaxa] LUT",  "[parallaxa] light leak",
		"[parallaxa] depth blur", "[parallaxa] could not load", "[parallaxa] luts", "[parallaxa] leaks"};
	bool fx_line = strstr(msg, "stage-uber") || strstr(msg, "stage-bloom") ||
		       strstr(msg, "stage-depth-blur");
	for (size_t i = 0; i < sizeof(fx_prefixes) / sizeof(fx_prefixes[0]); i++)
		fx_line = fx_line || strstr(msg, fx_prefixes[i]) != NULL;
	const bool stage = strstr(msg, "[parallaxa]") != NULL;
	if ((stage && level <= LOG_INFO) || fx_line || level <= LOG_ERROR || verbose)
		printf("  log(%d): %s\n", level, msg);
	if ((stage || fx_line) && level <= LOG_ERROR)
		stage_errors++;
	else if (fx_line && level == LOG_WARNING)
		stage_warnings++;
}

/* ------------------------------------------------------------------------- */
/* sRGB helpers and the test pattern                                         */
/* ------------------------------------------------------------------------- */

static double dec(double v)
{
	return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static double enc(double v)
{
	v = v < 0 ? 0 : v;
	return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

static uint8_t to8(double v)
{
	v = v < 0 ? 0 : (v > 1 ? 1 : v);
	return (uint8_t)floor(v * 255.0 + 0.5);
}

/* Left third: grey ramp; middle: 8 colour bars (sharp edges); right: 2D
 * gradient with a half-transparent band at the bottom. Premultiplied in
 * linear light, stored sRGB-encoded (like OBS image sources). */
static void make_pattern(uint8_t *px)
{
	static const uint8_t bars[8][3] = {{235, 235, 235}, {235, 235, 16}, {16, 235, 235},
					   {16, 235, 16},   {235, 16, 235},  {235, 16, 16},
					   {16, 16, 235},   {16, 16, 16}};
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			uint8_t *p = px + ((size_t)y * W + x) * 4;
			double r, g, b, a = 1.0;
			if (x < W / 3) {
				r = g = b = (double)x / (W / 3 - 1);
			} else if (x < 2 * W / 3) {
				const uint8_t *c = bars[((x - W / 3) / 80) % 8];
				r = c[0] / 255.0;
				g = c[1] / 255.0;
				b = c[2] / 255.0;
			} else {
				r = (double)(x - 2 * W / 3) / (W / 3);
				g = (double)y / H;
				b = 0.5;
				if (y >= H - 100)
					a = 0.5;
			}
			p[0] = to8(enc(dec(r) * a));
			p[1] = to8(enc(dec(g) * a));
			p[2] = to8(enc(dec(b) * a));
			p[3] = to8(a);
		}
	}
}

struct pattern {
	gs_texture_t *tex;
};

static const char *tp_name(void *u)
{
	(void)u;
	return "Parallaxa FX test pattern";
}

static void *tp_create(obs_data_t *s, obs_source_t *src)
{
	(void)s;
	(void)src;
	struct pattern *t = bzalloc(sizeof(*t));
	uint8_t *px = bmalloc(PX);
	make_pattern(px);
	const uint8_t *planes[1] = {px};
	obs_enter_graphics();
	t->tex = gs_texture_create(W, H, GS_RGBA, 1, planes, 0);
	obs_leave_graphics();
	bfree(px);
	return t;
}

static void tp_destroy(void *data)
{
	struct pattern *t = data;
	obs_enter_graphics();
	gs_texture_destroy(t->tex);
	obs_leave_graphics();
	bfree(t);
}

static uint32_t tp_w(void *d)
{
	(void)d;
	return W;
}

static uint32_t tp_h(void *d)
{
	(void)d;
	return H;
}

/* Same drawing as the OBS image source (premultiplied, sRGB-aware). */
static void tp_render(void *data, gs_effect_t *effect)
{
	struct pattern *t = data;
	const bool prev = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), t->tex);
	gs_draw_sprite(t->tex, 0, W, H);
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(prev);
}

static void register_pattern(void)
{
	struct obs_source_info info = {
		.id = "stage_fx_test_pattern",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
		.get_name = tp_name,
		.create = tp_create,
		.destroy = tp_destroy,
		.get_width = tp_w,
		.get_height = tp_h,
		.video_render = tp_render,
	};
	obs_register_source(&info);
}

/* ------------------------------------------------------------------------- */
/* Rendering on the graphics thread (main render callback)                   */
/* ------------------------------------------------------------------------- */

#define TIMER_SLOTS 48
#define TIMER_MIN_AGE 3 /* frames: never read a query the GPU may not have run */
#define MAX_TIMED 1024  /* frames of one timed job */
enum timer_role { ROLE_REF, ROLE_SRC, ROLE_LOAD };

static struct {
	pthread_mutex_t lock;
	os_event_t *done;
	obs_source_t *src;   /* source to render                       */
	uint8_t *out;        /* NULL = render only (timing)            */
	int wait_frames;     /* frames to skip first (ticks apply)     */
	int frames;          /* frames to render                       */
	bool active;
	gs_texrender_t *tr, *ref_tr, *load_tr;
	gs_stagesurf_t *stage;

	/* Timing (timed_pair): per frame, `ref` and `src` are rendered and
	 * timed back to back (alternating order), after untimed renders of
	 * `load` that keep the GPU at working clocks, as in OBS next to a game.
	 * The added cost of src is the median of the per-frame differences, so
	 * clock changes and other processes on the GPU cancel out. */
	bool timing;
	obs_source_t *ref, *load;
	int load_n;          /* adaptive: about LOAD_TARGET_MS of load per frame */
	double load_ms;      /* smoothed load time                               */
	uint64_t frame;      /* render callback count                            */
	int timed_idx;       /* index of the current timed frame                  */
	double ref_ms[MAX_TIMED], src_ms[MAX_TIMED];
	gs_timer_range_t *range[TIMER_SLOTS];
	gs_timer_t *timer[TIMER_SLOTS];
	bool pending[TIMER_SLOTS];
	uint64_t issued[TIMER_SLOTS];
	int role[TIMER_SLOTS], idx[TIMER_SLOTS];
} job;

#define LOAD_TARGET_MS 5.0
#define LOAD_MAX 400

/* Reads queries at least TIMER_MIN_AGE frames old (gs_timer_get_data spins
 * on D3D11 until the query is done). */
static void poll_timers(void)
{
	for (int i = 0; i < TIMER_SLOTS; i++) {
		if (!job.pending[i] || job.frame - job.issued[i] < TIMER_MIN_AGE)
			continue;
		bool disjoint = false;
		uint64_t freq = 0, ticks = 0;
		const bool ok = gs_timer_range_get_data(job.range[i], &disjoint, &freq) &&
				gs_timer_get_data(job.timer[i], &ticks);
		job.pending[i] = false;
		if (!ok || disjoint || !freq)
			continue;
		const double ms = (double)ticks * 1000.0 / (double)freq;
		if (job.role[i] == ROLE_LOAD)
			job.load_ms = job.load_ms * 0.8 + ms * 0.2;
		else if (job.role[i] == ROLE_REF)
			job.ref_ms[job.idx[i]] = ms;
		else
			job.src_ms[job.idx[i]] = ms;
	}
}

static int begin_timer(int role, int idx)
{
	for (int i = 0; i < TIMER_SLOTS; i++) {
		if (!job.timer[i]) {
			job.range[i] = gs_timer_range_create();
			job.timer[i] = gs_timer_create();
		}
		if (job.timer[i] && !job.pending[i]) {
			gs_timer_range_begin(job.range[i]);
			gs_timer_begin(job.timer[i]);
			job.issued[i] = job.frame;
			job.role[i] = role;
			job.idx[i] = idx;
			return i;
		}
	}
	return -1;
}

static void end_timer(int i)
{
	if (i < 0)
		return;
	gs_timer_end(job.timer[i]);
	gs_timer_range_end(job.range[i]);
	job.pending[i] = true;
}

static void render_into(gs_texrender_t *tr, obs_source_t *src)
{
	gs_texrender_reset(tr);
	if (gs_texrender_begin(tr, W, H)) {
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)W, 0.0f, (float)H, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		obs_source_video_render(src);
		gs_blend_state_pop();
		gs_texrender_end(tr);
	}
}

static void timed_render_into(gs_texrender_t *tr, obs_source_t *src, int role, int idx)
{
	int t = begin_timer(role, idx);
	render_into(tr, src);
	end_timer(t);
}

static void render_once(obs_source_t *src)
{
	if (!job.tr)
		job.tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!job.timing) {
		render_into(job.tr, src);
		return;
	}
	if (!job.ref_tr)
		job.ref_tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!job.load_tr)
		job.load_tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

	/* Load, timed as a whole to adapt its size. */
	if (job.load) {
		int t = begin_timer(ROLE_LOAD, 0);
		for (int i = 0; i < job.load_n; i++)
			render_into(job.load_tr, job.load);
		end_timer(t);
		if (job.load_ms < LOAD_TARGET_MS * 0.8 && job.load_n < LOAD_MAX)
			job.load_n += 4;
		else if (job.load_ms > LOAD_TARGET_MS * 1.25 && job.load_n > 4)
			job.load_n -= 4;
	}

	const int idx = job.timed_idx < MAX_TIMED ? job.timed_idx++ : -1;
	if (idx < 0) {
		render_into(job.ref_tr, job.ref);
		render_into(job.tr, src);
	} else if (idx & 1) {
		timed_render_into(job.tr, src, ROLE_SRC, idx);
		timed_render_into(job.ref_tr, job.ref, ROLE_REF, idx);
	} else {
		timed_render_into(job.ref_tr, job.ref, ROLE_REF, idx);
		timed_render_into(job.tr, src, ROLE_SRC, idx);
	}
}

static void readback(uint8_t *out)
{
	if (!job.stage)
		job.stage = gs_stagesurface_create(W, H, GS_RGBA);
	gs_stage_texture(job.stage, gs_texrender_get_texture(job.tr));
	uint8_t *data;
	uint32_t linesize;
	if (gs_stagesurface_map(job.stage, &data, &linesize)) {
		for (int y = 0; y < H; y++)
			memcpy(out + (size_t)y * W * 4, data + (size_t)y * linesize, W * 4);
		gs_stagesurface_unmap(job.stage);
	}
}

static void on_draw(void *param, uint32_t cx, uint32_t cy)
{
	(void)param;
	(void)cx;
	(void)cy;
	pthread_mutex_lock(&job.lock);
	job.frame++;
	poll_timers();
	if (job.active) {
		if (job.wait_frames > 0) {
			/* Rendered too, untimed: like OBS, which renders every frame
			 * (LUT / leak decodes start on the first render). */
			job.wait_frames--;
			if (!job.tr)
				job.tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
			render_into(job.tr, job.src);
		} else {
			render_once(job.src);
			if (--job.frames <= 0) {
				if (job.out)
					readback(job.out);
				job.active = false;
				os_event_signal(job.done);
			}
		}
	}
	pthread_mutex_unlock(&job.lock);
}

/* Renders src for `frames` frames after `wait` frames; captures the last one
 * into out (when not NULL). Blocks until done. */
static bool run_job(obs_source_t *src, uint8_t *out, int wait, int frames)
{
	pthread_mutex_lock(&job.lock);
	job.src = src;
	job.out = out;
	job.wait_frames = wait;
	job.frames = frames;
	job.active = true;
	pthread_mutex_unlock(&job.lock);
	if (os_event_timedwait(job.done, 30000) != 0) {
		FAIL("render job timed out");
		return false;
	}
	return true;
}

/* A few frames first: settings and commands apply in video_tick, LUT and
 * light-leak PNGs decode on the plugin's loader thread. */
static bool capture(obs_source_t *src, uint8_t *out)
{
	return run_job(src, out, 8, 1);
}

static int cmp_double(const void *a, const void *b)
{
	const double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

struct timing {
	double added_ms; /* median of src - ref per frame */
	double ref_ms;   /* median of ref                 */
	int n;           /* frames with both samples      */
	int load_n;      /* final load size               */
};

/* Added GPU time of rendering src instead of ref (see job.timing). */
static struct timing timed_pair(obs_source_t *src, obs_source_t *ref, obs_source_t *load,
				int frames)
{
	struct timing t = {-1.0, -1.0, 0, 0};
	if (frames > MAX_TIMED)
		frames = MAX_TIMED;
	pthread_mutex_lock(&job.lock);
	job.timing = true;
	job.ref = ref;
	job.load = load;
	job.timed_idx = MAX_TIMED; /* warm-up frames are not timed */
	pthread_mutex_unlock(&job.lock);
	run_job(src, NULL, 0, 120); /* warm-up: clocks ramp, the load adapts */

	pthread_mutex_lock(&job.lock);
	for (int i = 0; i < MAX_TIMED; i++)
		job.ref_ms[i] = job.src_ms[i] = -1.0;
	job.timed_idx = 0;
	pthread_mutex_unlock(&job.lock);
	run_job(src, NULL, 0, frames);
	pthread_mutex_lock(&job.lock);
	job.timed_idx = MAX_TIMED;
	pthread_mutex_unlock(&job.lock);
	run_job(src, NULL, 0, TIMER_MIN_AGE + 2); /* read the last queries */

	static double diff[MAX_TIMED], refs[MAX_TIMED];
	pthread_mutex_lock(&job.lock);
	for (int i = 0; i < frames; i++) {
		if (job.ref_ms[i] >= 0.0 && job.src_ms[i] >= 0.0) {
			diff[t.n] = job.src_ms[i] - job.ref_ms[i];
			refs[t.n++] = job.ref_ms[i];
		}
	}
	t.load_n = job.load_n;
	job.timing = false;
	job.ref = job.load = NULL;
	pthread_mutex_unlock(&job.lock);
	if (t.n) {
		qsort(diff, (size_t)t.n, sizeof(diff[0]), cmp_double);
		qsort(refs, (size_t)t.n, sizeof(refs[0]), cmp_double);
		t.added_ms = diff[t.n / 2];
		t.ref_ms = refs[t.n / 2];
	}
	return t;
}

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

struct stats {
	long long rendered, skipped, gpu_total_n, flashes_1s, flash_merged;
	double gpu_total_ms, flash;
	bool bloom, neutral;
	char preset[64];
};

static struct stats get_stats(obs_source_t *fx)
{
	struct stats s = {0};
	calldata_t cd;
	calldata_init(&cd);
	if (!proc_handler_call(obs_source_get_proc_handler(fx), "get_stats", &cd)) {
		FAIL("get_stats proc missing");
	} else {
		s.rendered = calldata_int(&cd, "rendered");
		s.skipped = calldata_int(&cd, "skipped");
		s.gpu_total_ms = calldata_float(&cd, "gpu_total_ms");
		s.gpu_total_n = calldata_int(&cd, "gpu_total_n");
		s.flash = calldata_float(&cd, "flash");
		s.flashes_1s = calldata_int(&cd, "flashes_1s");
		s.flash_merged = calldata_int(&cd, "flash_merged");
		s.bloom = calldata_bool(&cd, "bloom");
		s.neutral = calldata_bool(&cd, "neutral");
		const char *preset = calldata_string(&cd, "preset");
		snprintf(s.preset, sizeof(s.preset), "%s", preset ? preset : "");
	}
	calldata_free(&cd);
	return s;
}

/* All-neutral Parallaxa FX settings (every group unticked). */
static void set_neutral(obs_data_t *s)
{
	obs_data_set_string(s, "fx_preset", "neutral");
	obs_data_set_string(s, "fx_preset_applied", "neutral");
	obs_data_set_double(s, "exposure", 0);
	const char *pct[] = {"contrast_pct", "saturation_pct", "temperature_pct", "tint_pct",
			     "vignette_pct", "chroma_pct",     "grain_pct",       "lut_amount_pct",
			     "leak_amount_pct", "bloom_pct"};
	for (size_t i = 0; i < sizeof(pct) / sizeof(pct[0]); i++)
		obs_data_set_double(s, pct[i], 0);
	const char *groups[] = {"lut_enabled",   "vignette_enabled", "bloom_enabled",
				"chroma_enabled", "grain_enabled",    "leak_enabled"};
	for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); i++)
		obs_data_set_bool(s, groups[i], false);
	obs_data_set_string(s, "lut", "");
	obs_data_set_string(s, "leak", "");
}

static void update_json(obs_source_t *src, const char *json)
{
	obs_data_t *d = obs_data_create_from_json(json);
	obs_source_update(src, d);
	obs_data_release(d);
}

static void reset_neutral(obs_source_t *fx)
{
	obs_data_t *d = obs_data_create();
	set_neutral(d);
	obs_source_update(fx, d);
	obs_data_release(d);
}

struct diff {
	int max;
	double mean;
	long long differing;
};

static struct diff compare(const uint8_t *a, const uint8_t *b, bool opaque_only)
{
	struct diff d = {0};
	long long n = 0;
	double sum = 0;
	for (size_t i = 0; i < PX; i += 4) {
		if (opaque_only && (a[i + 3] != 255 || b[i + 3] != 255))
			continue;
		bool any = false;
		for (int c = 0; c < 4; c++) {
			int v = abs((int)a[i + c] - (int)b[i + c]);
			if (v > d.max)
				d.max = v;
			if (v)
				any = true;
			sum += v;
		}
		d.differing += any;
		n += 4;
	}
	d.mean = n ? sum / n : 0;
	return d;
}

static const uint8_t *pixel(const uint8_t *img, int x, int y)
{
	return img + ((size_t)y * W + x) * 4;
}

/* ------------------------------------------------------------------------- */
/* Sepia LUT strip writer (PNG, stored deflate blocks)                       */
/* ------------------------------------------------------------------------- */

static uint32_t crc_table[256];

static uint32_t crc32_buf(uint32_t crc, const uint8_t *p, size_t n)
{
	if (!crc_table[1]) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			crc_table[i] = c;
		}
	}
	crc = ~crc;
	for (size_t i = 0; i < n; i++)
		crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void write_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
	uint8_t hdr[8];
	put32(hdr, len);
	memcpy(hdr + 4, type, 4);
	fwrite(hdr, 1, 8, f);
	if (len)
		fwrite(data, 1, len, f);
	uint32_t crc = crc32_buf(0, (const uint8_t *)type, 4);
	crc = crc32_buf(crc, data, len);
	uint8_t c[4];
	put32(c, crc);
	fwrite(c, 1, 4, f);
}

static void sepia(const double in[3], double out[3])
{
	const double r = in[0], g = in[1], b = in[2];
	out[0] = fmin(1.0, 0.393 * r + 0.769 * g + 0.189 * b);
	out[1] = fmin(1.0, 0.349 * r + 0.686 * g + 0.168 * b);
	out[2] = fmin(1.0, 0.272 * r + 0.534 * g + 0.131 * b);
}

static bool write_sepia_lut(const char *path)
{
	const size_t row = 1 + 512 * 3, raw_len = row * 512;
	uint8_t *raw = bmalloc(raw_len);
	for (int y = 0; y < 512; y++) {
		uint8_t *r = raw + (size_t)y * row;
		r[0] = 0; /* filter: none */
		for (int x = 0; x < 512; x++) {
			const int ri = x % 64, gi = y % 64, bi = (y / 64) * 8 + x / 64;
			double in[3] = {ri / 63.0, gi / 63.0, bi / 63.0}, out[3];
			sepia(in, out);
			for (int c = 0; c < 3; c++)
				r[1 + x * 3 + c] = to8(out[c]);
		}
	}
	/* zlib stream of stored blocks. */
	const size_t blocks = (raw_len + 65534) / 65535;
	const size_t z_len = 2 + blocks * 5 + raw_len + 4;
	uint8_t *z = bmalloc(z_len), *zp = z;
	*zp++ = 0x78;
	*zp++ = 0x01;
	uint32_t a = 1, b = 0;
	for (size_t off = 0; off < raw_len; off += 65535) {
		const size_t n = raw_len - off < 65535 ? raw_len - off : 65535;
		*zp++ = off + n >= raw_len ? 1 : 0;
		*zp++ = (uint8_t)(n & 0xFF);
		*zp++ = (uint8_t)(n >> 8);
		*zp++ = (uint8_t)(~n & 0xFF);
		*zp++ = (uint8_t)((~n >> 8) & 0xFF);
		memcpy(zp, raw + off, n);
		zp += n;
	}
	for (size_t i = 0; i < raw_len; i++) {
		a = (a + raw[i]) % 65521;
		b = (b + a) % 65521;
	}
	put32(zp, (b << 16) | a);

	FILE *f = os_fopen(path, "wb");
	if (!f) {
		bfree(raw);
		bfree(z);
		return false;
	}
	static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	fwrite(sig, 1, 8, f);
	uint8_t ihdr[13];
	put32(ihdr, 512);
	put32(ihdr + 4, 512);
	ihdr[8] = 8;  /* bit depth */
	ihdr[9] = 2;  /* RGB */
	ihdr[10] = 0; /* deflate */
	ihdr[11] = 0; /* filter */
	ihdr[12] = 0; /* no interlace */
	write_chunk(f, "IHDR", ihdr, 13);
	write_chunk(f, "IDAT", z, (uint32_t)z_len);
	write_chunk(f, "IEND", NULL, 0);
	fclose(f);
	bfree(raw);
	bfree(z);
	return true;
}

/* ------------------------------------------------------------------------- */
/* Test cases                                                                */
/* ------------------------------------------------------------------------- */

static uint8_t *img_base, *img, *img2;

static void test_fx_source(obs_source_t *tp, bool strict_perf)
{
	(void)strict_perf;
	printf("[fx on a source]\n");
	capture(tp, img_base);

	obs_data_t *s = obs_data_create();
	set_neutral(s);
	obs_source_t *fx = obs_source_create("stage_fx", "Parallaxa FX test", s, NULL);
	obs_data_release(s);
	if (!fx) {
		FAIL("stage_fx not registered");
		return;
	}
	obs_source_filter_add(tp, fx);

	/* Neutral: skip path, bit-identical. */
	capture(tp, img);
	struct stats st = get_stats(fx);
	struct diff d = compare(img_base, img, false);
	printf("  neutral: %lld differing pixels, rendered %lld, skipped %lld\n", d.differing,
	       st.rendered, st.skipped);
	if (d.differing)
		FAIL("neutral output differs from the unfiltered image");
	if (st.rendered != 0 || st.skipped == 0)
		FAIL("neutral settings did not take obs_source_skip_video_filter");

	/* Identity LUT: visually identical. */
	update_json(fx, "{\"lut_enabled\":true,\"lut\":\"identity\",\"lut_amount_pct\":100}");
	capture(tp, img);
	st = get_stats(fx);
	d = compare(img_base, img, false);
	printf("  identity LUT: max diff %d, mean %.4f, rendered %lld\n", d.max, d.mean,
	       st.rendered);
	if (st.rendered == 0)
		FAIL("identity LUT did not render");
	if (d.max > 2 || d.mean > 0.25)
		FAIL("identity LUT is not visually identical (max %d, mean %.4f)", d.max, d.mean);

	/* Sepia LUT (written by this test into the user LUT folder). */
	update_json(fx, "{\"lut\":\"sepia\"}");
	capture(tp, img);
	int worst = 0;
	long long checked = 0;
	for (int y = 0; y < H; y += 7) {
		for (int x = 0; x < W; x += 7) {
			const uint8_t *b = pixel(img_base, x, y), *o = pixel(img, x, y);
			if (b[3] != 255)
				continue;
			double in[3] = {b[0] / 255.0, b[1] / 255.0, b[2] / 255.0}, out[3];
			sepia(in, out);
			for (int c = 0; c < 3; c++) {
				int e = abs((int)o[c] - (int)to8(out[c]));
				worst = e > worst ? e : worst;
			}
			checked++;
		}
	}
	d = compare(img_base, img, true);
	printf("  sepia LUT: worst error vs analytic sepia %d (of %lld px); differs from base: %lld px\n",
	       worst, checked, d.differing);
	if (worst > 4)
		FAIL("sepia LUT does not match the analytic sepia (worst %d)", worst);
	if (d.differing < 100000)
		FAIL("sepia LUT has no visible effect");
	/* Alpha is preserved. */
	for (int y = H - 50; y < H; y += 10) {
		if (pixel(img, W - 10, y)[3] != pixel(img_base, W - 10, y)[3])
			FAIL("alpha changed by the LUT");
	}

	/* Exposure +1 EV doubles linear light. */
	reset_neutral(fx);
	update_json(fx, "{\"exposure\":1.0}");
	capture(tp, img);
	worst = 0;
	for (int x = 0; x < W / 3; x += 13) {
		const uint8_t *b = pixel(img_base, x, 500), *o = pixel(img, x, 500);
		int want = to8(enc(fmin(1.0, dec(b[0] / 255.0) * 2.0)));
		int e = abs((int)o[0] - want);
		worst = e > worst ? e : worst;
	}
	printf("  exposure +1 EV: worst error %d\n", worst);
	if (worst > 2)
		FAIL("exposure +1 EV is off by %d", worst);

	/* Vignette: corners darker, centre unchanged. */
	reset_neutral(fx);
	update_json(fx, "{\"vignette_enabled\":true,\"vignette_pct\":100}");
	capture(tp, img);
	const uint8_t *bc = pixel(img_base, 40, 40), *oc = pixel(img, 40, 40);
	const uint8_t *bm = pixel(img_base, W / 2, H / 2), *om = pixel(img, W / 2, H / 2);
	printf("  vignette: corner %d -> %d, centre %d -> %d\n", bc[0] + bc[1] + bc[2],
	       oc[0] + oc[1] + oc[2], bm[0] + bm[1] + bm[2], om[0] + om[1] + om[2]);
	if (!(oc[0] + oc[1] + oc[2] + 30 < bc[0] + bc[1] + bc[2]) && bc[0] > 40)
		FAIL("vignette does not darken the corners");
	if (abs((int)om[0] - (int)bm[0]) > 1)
		FAIL("vignette changes the centre");

	/* Film grain, chromatic aberration: render and change the picture. */
	reset_neutral(fx);
	update_json(fx, "{\"grain_enabled\":true,\"grain_pct\":30,\"chroma_enabled\":true,"
			"\"chroma_pct\":100}");
	capture(tp, img);
	d = compare(img_base, img, false);
	printf("  grain + chroma: %lld differing pixels\n", d.differing);
	if (d.differing < 100000)
		FAIL("grain/chroma have no visible effect");

	/* Test pulse: flash decays to 0 within 300 ms. */
	reset_neutral(fx);
	run_job(tp, NULL, 3, 1);
	obs_properties_t *props = obs_source_properties(fx);
	obs_property_t *flash_btn = obs_properties_get(props, "pulse_flash");
	if (!flash_btn)
		FAIL("pulse_flash button missing");
	obs_property_button_clicked(flash_btn, fx);
	run_job(tp, img, 2, 1);
	st = get_stats(fx);
	const double f0 = st.flash;
	d = compare(img_base, img, false);
	os_sleep_ms(400);
	run_job(tp, img2, 2, 1);
	struct stats st2 = get_stats(fx);
	struct diff d2 = compare(img_base, img2, false);
	printf("  flash: value %.3f (image diff %lld px), after 400 ms %.3f (diff %lld px, "
	       "skipped again: %s)\n",
	       f0, d.differing, st2.flash, d2.differing, st2.skipped > st.skipped ? "yes" : "no");
	if (!(f0 > 0.0))
		FAIL("flash pulse did not start");
	if (st2.flash != 0.0 || d2.differing)
		FAIL("flash did not decay to 0 within its duration");

	/* Flash cap: 12 clicks 40 ms apart -> at most 3 flashes per second. */
	long long max_1s = 0;
	for (int i = 0; i < 12; i++) {
		obs_property_button_clicked(flash_btn, fx);
		os_sleep_ms(40);
		st = get_stats(fx);
		max_1s = st.flashes_1s > max_1s ? st.flashes_1s : max_1s;
	}
	run_job(tp, NULL, 2, 1);
	st = get_stats(fx);
	printf("  flash cap: max %lld flashes in 1 s, %lld merged\n", max_1s, st.flash_merged);
	if (max_1s > 3)
		FAIL("more than 3 flashes per second");
	if (st.flash_merged < 1)
		FAIL("no pulse was merged by the flash cap");
	obs_properties_destroy(props);

	obs_source_filter_remove(tp, fx);
	obs_source_release(fx);
}

/* Cozy look on a 1080p scene: bloom on, then bloom off; added GPU cost
 * against the same scene without the filter (timed_pair). */
static void test_fx_scene(obs_source_t *tp, bool d3d11, bool strict_perf)
{
	printf("[fx on a scene]\n");
	obs_scene_t *scene = obs_scene_create_private("Parallaxa FX test scene");
	obs_source_t *ss = obs_scene_get_source(scene);
	obs_scene_add(scene, tp);
	capture(ss, img_base);

	obs_data_t *s = obs_data_create();
	set_neutral(s);
	obs_source_t *fx = obs_source_create_private("stage_fx", "Parallaxa FX scene", s);
	obs_data_release(s);
	obs_source_filter_add(ss, fx);

	capture(ss, img);
	struct diff d = compare(img_base, img, false);
	printf("  neutral on scene: %lld differing pixels\n", d.differing);
	if (d.differing)
		FAIL("neutral scene output differs");

	/* Cozy (defaults): pick it like the panel does. */
	update_json(fx, "{\"fx_preset\":\"cozy\",\"fx_preset_applied\":\"\"}");
	obs_properties_t *props = obs_source_properties(fx); /* runs the preset callback */
	obs_properties_destroy(props);
	obs_source_update(fx, NULL);
	capture(ss, img);
	struct stats st = get_stats(fx);
	d = compare(img_base, img, false);
	printf("  cozy: bloom %s, %lld differing pixels\n", st.bloom ? "on" : "off", d.differing);
	if (!st.bloom)
		FAIL("cozy did not enable the bloom passes");
	if (d.differing < 100000)
		FAIL("cozy has no visible effect");

	/* Added GPU time = scene render with the look - render of an identical
	 * scene without the filter, timed back to back in the same frame (median
	 * over the frames) while an untimed load keeps the GPU at working clocks.
	 * The filter's own probe is printed for reference. */
	obs_scene_t *ref_scene = obs_scene_create_private("Parallaxa FX reference scene");
	obs_scene_add(ref_scene, tp);
	obs_source_t *ref = obs_scene_get_source(ref_scene);

	struct stats a = get_stats(fx);
	const struct timing t_bloom = timed_pair(ss, ref, tp, 600);
	struct stats b = get_stats(fx);
	const double probe_bloom = b.gpu_total_n > a.gpu_total_n
					   ? (b.gpu_total_ms - a.gpu_total_ms) /
						     (double)(b.gpu_total_n - a.gpu_total_n)
					   : -1;
	update_json(fx, "{\"bloom_enabled\":false}");
	a = get_stats(fx);
	const struct timing t_plain = timed_pair(ss, ref, tp, 600);
	b = get_stats(fx);
	const double probe_plain = b.gpu_total_n > a.gpu_total_n
					   ? (b.gpu_total_ms - a.gpu_total_ms) /
						     (double)(b.gpu_total_n - a.gpu_total_n)
					   : -1;
	/* For information only: the same without the load, at whatever (often
	 * idle) clocks the GPU picks for a nearly empty frame. */
	const struct timing t_idle = timed_pair(ss, ref, NULL, 300);
	reset_neutral(fx);
	const struct timing t_neutral = timed_pair(ss, ref, tp, 300);
	printf("  scene render at %dx%d: reference %.3f ms (median of %d frames, load %d renders)\n",
	       W, H, t_plain.ref_ms, t_plain.n, t_plain.load_n);
	printf("  added GPU time (median): cozy %.3f ms (budget 1.0), without bloom %.3f ms "
	       "(budget 0.5), neutral %.3f ms; filter probe %.3f / %.3f ms\n",
	       t_bloom.added_ms, t_plain.added_ms, t_neutral.added_ms, probe_bloom, probe_plain);
	printf("  without the GPU load (not checked): reference %.3f ms, without bloom %.3f ms "
	       "added\n",
	       t_idle.ref_ms, t_idle.added_ms);
	if (t_bloom.n < 50 || t_plain.n < 50 || t_neutral.n < 50) {
		if (d3d11)
			FAIL("too few GPU timer samples on D3D11");
		else
			printf("  (no GPU timer samples on this renderer)\n");
	} else if (t_bloom.added_ms > 1.0 || t_plain.added_ms > 0.5) {
		if (strict_perf || d3d11)
			FAIL("GPU budget exceeded");
	}
	if (d3d11 && (probe_bloom < 0 || probe_plain < 0))
		FAIL("the filter's GPU probe produced no samples");
	obs_scene_release(ref_scene);

	obs_source_filter_remove(ss, fx);
	obs_source_release(fx);
	obs_scene_release(scene);
}

/* ------------------------------------------------------------------------- */
/* fx_set / panic and the settings round trip                                */
/* ------------------------------------------------------------------------- */

/* UI task queue: the main thread of this test plays the OBS UI thread. */
#define UI_TASKS_MAX 256
static struct {
	pthread_mutex_t lock;
	obs_task_t task[UI_TASKS_MAX];
	void *param[UI_TASKS_MAX];
	int n;
	long long queued;
} ui;

static void ui_task_handler(obs_task_t task, void *param, bool wait)
{
	if (wait) {
		task(param); /* not used by the plugin */
		return;
	}
	pthread_mutex_lock(&ui.lock);
	if (ui.n < UI_TASKS_MAX) {
		ui.task[ui.n] = task;
		ui.param[ui.n++] = param;
		ui.queued++;
	}
	pthread_mutex_unlock(&ui.lock);
}

static int pump_ui(void)
{
	obs_task_t task[UI_TASKS_MAX];
	void *param[UI_TASKS_MAX];
	pthread_mutex_lock(&ui.lock);
	const int n = ui.n;
	memcpy(task, ui.task, sizeof(task[0]) * (size_t)n);
	memcpy(param, ui.param, sizeof(param[0]) * (size_t)n);
	ui.n = 0;
	pthread_mutex_unlock(&ui.lock);
	for (int i = 0; i < n; i++)
		task[i](param[i]);
	return n;
}

/* Drops queued UI tasks without running them (as at OBS shutdown). */
static void discard_ui(void)
{
	pthread_mutex_lock(&ui.lock);
	ui.n = 0;
	pthread_mutex_unlock(&ui.lock);
}

static void proc_fx_set(obs_source_t *fx, const char *preset, const char *params, int ms)
{
	calldata_t cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "preset", preset);
	calldata_set_string(&cd, "params", params);
	calldata_set_int(&cd, "duration_ms", ms);
	if (!proc_handler_call(obs_source_get_proc_handler(fx), "fx_set", &cd))
		FAIL("fx_set proc missing");
	calldata_free(&cd);
}

static void proc_panic(obs_source_t *fx)
{
	calldata_t cd;
	calldata_init(&cd);
	if (!proc_handler_call(obs_source_get_proc_handler(fx), "panic", &cd))
		FAIL("panic proc missing");
	calldata_free(&cd);
}

/* A few frames: the tick drains the command and queues the settings write. */
static void frames(obs_source_t *src)
{
	run_job(src, NULL, 3, 1);
}

/* Command -> settings write on the UI thread -> deferred update(). */
static void settle(obs_source_t *src)
{
	frames(src);
	pump_ui();
	frames(src);
}

static void expect_preset(obs_source_t *fx, const char *step, const char *want)
{
	struct stats st = get_stats(fx);
	obs_data_t *s = obs_source_get_settings(fx);
	const char *in_settings = obs_data_get_string(s, "fx_preset");
	printf("  %s: filter look '%s' (panel '%s')\n", step, st.preset, in_settings);
	if (strcmp(st.preset, want) != 0)
		FAIL("%s: the filter shows '%s', expected '%s'", step, st.preset, want);
	obs_data_release(s);
}

/* The panel's preset list: pick id (on_preset_modified copies its values). */
static void user_pick(obs_source_t *fx, const char *id)
{
	obs_data_t *s = obs_source_get_settings(fx);
	obs_properties_t *props = obs_source_properties(fx);
	obs_property_t *list = obs_properties_get(props, "fx_preset");
	obs_data_set_string(s, "fx_preset", id);
	if (list)
		obs_property_modified(list, s);
	else
		FAIL("fx_preset list missing");
	obs_properties_destroy(props);
	obs_data_release(s);
	obs_source_update(fx, NULL);
}

static void user_reset(obs_source_t *fx)
{
	obs_properties_t *props = obs_source_properties(fx);
	obs_property_t *btn = obs_properties_get(props, "fx_reset");
	if (btn)
		obs_property_button_clicked(btn, fx);
	else
		FAIL("fx_reset button missing");
	obs_properties_destroy(props);
}

static void test_fx_commands(obs_source_t *tp)
{
	printf("[fx_set / panic]\n");
	obs_data_t *s = obs_data_create();
	set_neutral(s);
	obs_source_t *fx = obs_source_create_private("stage_fx", "Parallaxa FX commands", s);
	obs_data_release(s);
	obs_source_filter_add(tp, fx);
	frames(tp);
	const long long queued0 = ui.queued;

	proc_fx_set(fx, "hype", "", 0);
	settle(tp);
	expect_preset(fx, "fx_set hype", "hype");
	proc_fx_set(fx, "cozy", "", 0);
	settle(tp);
	expect_preset(fx, "fx_set cozy", "cozy");
	if (ui.queued - queued0 != 2)
		FAIL("expected 2 settings writes on the UI queue, got %lld", ui.queued - queued0);
	obs_data_t *cur = obs_source_get_settings(fx);
	if (strcmp(obs_data_get_string(cur, "fx_preset"), "cozy") != 0)
		FAIL("fx_set did not reach the settings");
	obs_data_release(cur);

	/* The user picks a look the director sent earlier: it must apply. */
	user_pick(fx, "hype");
	frames(tp);
	expect_preset(fx, "panel pick hype after fx_set hype, cozy", "hype");

	/* Params on top of a look, then Reset to preset. */
	proc_fx_set(fx, "cozy", "", 0);
	settle(tp);
	proc_fx_set(fx, "", "{\"exposure\":0.37}", 0);
	settle(tp);
	expect_preset(fx, "fx_set cozy + exposure", "");
	user_reset(fx);
	frames(tp);
	expect_preset(fx, "Reset to preset", "cozy");

	/* Panic, a new look, then the user picks Neutral. */
	proc_panic(fx);
	settle(tp);
	expect_preset(fx, "panic", "neutral");
	proc_fx_set(fx, "cozy", "", 0);
	settle(tp);
	user_pick(fx, "neutral");
	frames(tp);
	struct stats st = get_stats(fx);
	expect_preset(fx, "panel pick Neutral after panic + fx_set", "neutral");
	if (!st.neutral)
		FAIL("Neutral picked in the panel does not take the skip path");

	/* The user edits the panel right after a write landed (possibly before
	 * the deferred update ran): the edit wins. */
	proc_fx_set(fx, "hype", "", 0);
	frames(tp);
	pump_ui();
	update_json(fx, "{\"exposure\":0.21}");
	frames(tp);
	expect_preset(fx, "edit right after a landed write", "");

	/* An own write still tweening is not cut short by its landing: from
	 * neutral to +1.5 EV over 2 s (nothing else changes between frames). */
	proc_panic(fx);
	settle(tp);
	proc_fx_set(fx, "", "{\"exposure\":1.5}", 2000);
	settle(tp);
	capture(tp, img);
	run_job(tp, img2, 30, 1);
	struct diff d = compare(img, img2, false);
	printf("  tween across its own write: %lld pixels changed\n", d.differing);
	if (d.differing < 1000)
		FAIL("the fx_set tween stopped when its settings write landed");

	/* A write whose UI task never runs is released on unload (leak check). */
	proc_fx_set(fx, "hype", "", 0);
	frames(tp);
	discard_ui();

	obs_source_filter_remove(tp, fx);
	obs_source_release(fx);
}

/* Steepest red step across the colour bars (one row): 219 per pixel when
 * sharp, much less once blurred. */
static double edge_strength(const uint8_t *im)
{
	int best = 0;
	for (int y = 280; y < 320; y += 10) {
		for (int x = W / 3 + 1; x < 2 * W / 3; x++) {
			int d = abs((int)pixel(im, x, y)[0] - (int)pixel(im, x - 1, y)[0]);
			best = d > best ? d : best;
		}
	}
	return best;
}

static void test_depth_blur(obs_source_t *tp)
{
	printf("[depth blur]\n");
	capture(tp, img_base);

	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "driven_by", "stale-scene-uuid");
	obs_data_set_double(s, "strength_pct", 0);
	obs_source_t *db = obs_source_create("stage_depth_blur", "Parallaxa Depth Blur test", s, NULL);
	obs_data_release(s);
	if (!db) {
		FAIL("stage_depth_blur not registered");
		return;
	}
	obs_data_t *cur = obs_source_get_settings(db);
	if (strcmp(obs_data_get_string(cur, "driven_by"), "") != 0)
		FAIL("driven_by not cleared on create");
	obs_data_release(cur);
	obs_source_filter_add(tp, db);

	capture(tp, img);
	struct diff d = compare(img_base, img, false);
	printf("  strength 0: %lld differing pixels\n", d.differing);
	if (d.differing)
		FAIL("strength 0 is not a pass-through");

	const double sharp = edge_strength(img_base);
	update_json(db, "{\"strength_pct\":60}");
	capture(tp, img);
	const double half_q = edge_strength(img);
	update_json(db, "{\"quality\":4,\"passes\":4}");
	capture(tp, img2);
	const double quarter = edge_strength(img2);
	printf("  steepest edge: sharp %.0f, 60%% half/3 passes %.0f, quarter/4 passes %.0f\n", sharp,
	       half_q, quarter);
	if (!(half_q < sharp * 0.8) || !(quarter < sharp * 0.8))
		FAIL("manual strength does not blur");
	/* Energy is preserved in linear light (premultiplied, no darkening). */
	double mb[3] = {0}, mo[3] = {0};
	for (int y = 200; y < 880; y += 3) {
		for (int x = W / 3 + 160; x < 2 * W / 3 - 160; x += 3) {
			for (int c = 0; c < 3; c++) {
				mb[c] += dec(pixel(img_base, x, y)[c] / 255.0);
				mo[c] += dec(pixel(img, x, y)[c] / 255.0);
			}
		}
	}
	for (int c = 0; c < 3; c++) {
		const double rel = fabs(mo[c] - mb[c]) / mb[c];
		if (rel > 0.03)
			FAIL("blur changes the mean linear level of channel %d by %.1f%%", c, rel * 100);
	}

	/* Driven with auto_strength 0: skip, slider disabled. */
	update_json(db, "{\"driven_by\":\"scene-uuid\",\"auto_strength\":0.0}");
	capture(tp, img);
	d = compare(img_base, img, false);
	obs_properties_t *props = obs_source_properties(db);
	obs_property_t *p = obs_properties_get(props, "strength_pct");
	const bool enabled = p ? obs_property_enabled(p) : true;
	obs_properties_destroy(props);
	printf("  driven, auto 0: %lld differing pixels, Strength enabled: %s\n", d.differing,
	       enabled ? "yes" : "no");
	if (d.differing)
		FAIL("driven layer with auto_strength 0 is not a pass-through");
	if (enabled)
		FAIL("Strength must be disabled while driven");

	update_json(db, "{\"auto_strength\":0.5}");
	capture(tp, img);
	if (!(edge_strength(img) < sharp * 0.8))
		FAIL("auto_strength does not blur while driven");

	/* Rig stops driving: manual strength again. */
	update_json(db, "{\"driven_by\":\"\",\"auto_strength\":0.0,\"strength_pct\":0}");
	capture(tp, img);
	if (compare(img_base, img, false).differing)
		FAIL("not a pass-through after driving stopped");

	obs_source_filter_remove(tp, db);
	obs_source_release(db);
}

/* ------------------------------------------------------------------------- */
/* Main                                                                      */
/* ------------------------------------------------------------------------- */

static bool start_obs(const char *obs_bin, const char *renderer, const char *config)
{
	if (!obs_startup("en-US", config, NULL)) {
		FAIL("obs_startup");
		return false;
	}
	char module[1024];
	snprintf(module, sizeof(module), "%s/%s", obs_bin,
		 !strcmp(renderer, "opengl") ? "libobs-opengl" : "libobs-d3d11");
	struct obs_video_info ovi = {
		.graphics_module = module,
		.fps_num = 60,
		.fps_den = 1,
		.base_width = W,
		.base_height = H,
		.output_width = W,
		.output_height = H,
		.output_format = VIDEO_FORMAT_NV12,
		.adapter = 0,
		.gpu_conversion = true,
		.colorspace = VIDEO_CS_709,
		.range = VIDEO_RANGE_PARTIAL,
		.scale_type = OBS_SCALE_BICUBIC,
	};
	int r = obs_reset_video(&ovi);
	if (r != OBS_VIDEO_SUCCESS) {
		FAIL("obs_reset_video(%s) = %d", renderer, r);
		return false;
	}
	return true;
}

int main(int argc, char **argv)
{
	if (argc < 6) {
		fprintf(stderr, "usage: fx_gpu_test <dll> <data> <obs bin dir> <d3d11|opengl> <work dir> "
				"[--strict-perf] [--verbose]\n");
		return 2;
	}
	const char *dll = argv[1], *data = argv[2], *obs_bin = argv[3], *renderer = argv[4];
	bool strict_perf = false;
	for (int i = 6; i < argc; i++) {
		if (!strcmp(argv[i], "--strict-perf"))
			strict_perf = true;
		if (!strcmp(argv[i], "--verbose"))
			verbose = true;
	}
	const bool d3d11 = strcmp(renderer, "opengl") != 0;
	base_set_log_handler(log_handler, NULL);

	char work[1024], config[1024], luts[1024], lut_path[1100];
	snprintf(work, sizeof(work), "%s", argv[5]);
	snprintf(config, sizeof(config), "%s/config", work);
	snprintf(luts, sizeof(luts), "%s/parallaxa/luts", config);
	snprintf(lut_path, sizeof(lut_path), "%s/sepia.png", luts);
	os_mkdirs(luts);
	if (!write_sepia_lut(lut_path)) {
		FAIL("cannot write %s", lut_path);
		return 1;
	}

#ifdef _WIN32
	/* libobs finds its own data (default effects) at ../../data/libobs
	 * relative to the working directory, as for obs64.exe. */
	if (!SetCurrentDirectoryA(obs_bin)) {
		FAIL("cannot enter %s", obs_bin);
		return 1;
	}
	if (!LoadLibraryA("obs-frontend-api.dll")) {
		FAIL("obs-frontend-api.dll not found on PATH");
		return 1;
	}
#endif

	/* Baseline: the same libobs life cycle without the plugin. Always on
	 * D3D11: libobs-opengl cannot be started twice in one process (its
	 * dummy window class stays registered). */
	if (!start_obs(obs_bin, "d3d11", config))
		return 1;
	obs_shutdown();
	const long baseline = bnum_allocs();

	pthread_mutex_init(&job.lock, NULL);
	os_event_init(&job.done, OS_EVENT_TYPE_AUTO);
	img_base = bmalloc(PX);
	img = bmalloc(PX);
	img2 = bmalloc(PX);
	const long buffers = bnum_allocs();

	if (!start_obs(obs_bin, renderer, config))
		return 1;
	obs_module_t *mod = NULL;
	int r = obs_open_module(&mod, dll, data);
	if (r != MODULE_SUCCESS || !obs_init_module(mod)) {
		FAIL("loading %s: %d", dll, r);
		obs_shutdown();
		return 1;
	}
	obs_post_load_modules();
	pthread_mutex_init(&ui.lock, NULL);
	obs_set_ui_task_handler(ui_task_handler);
	register_pattern();
	obs_add_main_render_callback(on_draw, NULL);

	obs_source_t *tp = obs_source_create("stage_fx_test_pattern", "pattern", NULL, NULL);
	test_fx_source(tp, strict_perf);
	test_fx_scene(tp, d3d11, strict_perf);
	test_fx_commands(tp);
	test_depth_blur(tp);
	obs_source_release(tp);

	obs_remove_main_render_callback(on_draw, NULL);
	obs_enter_graphics();
	gs_texrender_destroy(job.tr);
	gs_texrender_destroy(job.load_tr);
	gs_texrender_destroy(job.ref_tr);
	gs_stagesurface_destroy(job.stage);
	for (int i = 0; i < TIMER_SLOTS; i++) {
		gs_timer_destroy(job.timer[i]);
		gs_timer_range_destroy(job.range[i]);
	}
	obs_leave_graphics();
	job.tr = job.load_tr = job.ref_tr = NULL;
	job.stage = NULL;
	for (int i = 0; i < TIMER_SLOTS; i++) {
		job.timer[i] = NULL;
		job.range[i] = NULL;
	}
	obs_shutdown();

	const long after = bnum_allocs() - (buffers - baseline);
	printf("[leaks] allocations after shutdown: baseline %ld, with plugin %ld\n", baseline, after);
	if (after != baseline)
		FAIL("%ld allocation(s) leaked", after - baseline);
	if (stage_errors)
		FAIL("%d [parallaxa] error line(s) logged", stage_errors);
	if (stage_warnings)
		FAIL("%d [parallaxa] warning line(s) logged", stage_warnings);

	bfree(img_base);
	bfree(img);
	bfree(img2);
	os_event_destroy(job.done);
	pthread_mutex_destroy(&job.lock);
	pthread_mutex_destroy(&ui.lock);
	printf("fx_gpu_test (%s): %s\n", renderer, failures ? "FAILED" : "OK");
	return failures ? 1 : 0;
}
