/*
 * stage-fx-module.c — Parallaxa FX module entry (stage_fx_register), perf
 * events for the vendor, and render / timer / locale helpers shared by the
 * stage_fx and stage_depth_blur filters.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "stage-fx-internal.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

#include <stdio.h>
#include <string.h>
#include <util/platform.h>
#include <util/task.h>
#include <util/threading.h>

#include "stage-modules.h"

/* ------------------------------------------------------------------------- */
/* Perf events (graphics thread -> vendor frontend tick)                     */
/* ------------------------------------------------------------------------- */

static pthread_mutex_t perf_lock = PTHREAD_MUTEX_INITIALIZER;
static struct stage_fx_perf_event perf_q[STAGE_FX_PERF_QUEUE_MAX];
static size_t perf_head, perf_num;

void stfx_perf_push(const struct stage_fx_perf_event *e)
{
	pthread_mutex_lock(&perf_lock);
	if (perf_num == STAGE_FX_PERF_QUEUE_MAX) {
		perf_head = (perf_head + 1) % STAGE_FX_PERF_QUEUE_MAX; /* drop oldest */
		perf_num--;
	}
	perf_q[(perf_head + perf_num) % STAGE_FX_PERF_QUEUE_MAX] = *e;
	perf_num++;
	pthread_mutex_unlock(&perf_lock);
}

bool stage_fx_perf_pop(struct stage_fx_perf_event *out)
{
	bool ok = false;
	pthread_mutex_lock(&perf_lock);
	if (perf_num) {
		if (out)
			*out = perf_q[perf_head];
		perf_head = (perf_head + 1) % STAGE_FX_PERF_QUEUE_MAX;
		perf_num--;
		ok = true;
	}
	pthread_mutex_unlock(&perf_lock);
	return ok;
}

void stfx_perf_clear(void)
{
	pthread_mutex_lock(&perf_lock);
	perf_head = perf_num = 0;
	pthread_mutex_unlock(&perf_lock);
}

/* ------------------------------------------------------------------------- */
/* Image loader: PNG decode on a worker thread                               */
/* ------------------------------------------------------------------------- */

struct stfx_image {
	volatile long refs; /* the owner + the pending task */
	volatile bool done;
	char path[1024];
	uint8_t *data; /* bmalloc'd pixels, NULL when the decode failed */
	enum gs_color_format format;
	uint32_t cx, cy;
};

static pthread_mutex_t loader_lock = PTHREAD_MUTEX_INITIALIZER;
static os_task_queue_t *loader; /* created on first use, destroyed on unload */

static void image_unref(struct stfx_image *im)
{
	if (im && os_atomic_dec_long(&im->refs) == 0) {
		bfree(im->data);
		bfree(im);
	}
}

static void image_task(void *param)
{
	struct stfx_image *im = param;
	enum gs_color_format format = GS_UNKNOWN;
	uint32_t cx = 0, cy = 0;
	/* Same decode as gs_texture_create_from_file, minus the upload; needs
	 * no graphics context (never gs_image_file*: its layout changed in 33). */
	uint8_t *data = gs_create_texture_file_data(im->path, &format, &cx, &cy);
	im->format = format;
	im->cx = cx;
	im->cy = cy;
	im->data = data;
	os_atomic_set_bool(&im->done, true); /* full barrier: publishes the fields */
	image_unref(im);
}

struct stfx_image *stfx_image_load(const char *path)
{
	struct stfx_image *im = bzalloc(sizeof(*im));
	im->refs = 2;
	stage_strlcpy(im->path, path, sizeof(im->path));

	pthread_mutex_lock(&loader_lock);
	if (!loader)
		loader = os_task_queue_create();
	const bool queued = loader && os_task_queue_queue_task(loader, image_task, im);
	pthread_mutex_unlock(&loader_lock);
	if (!queued)
		image_task(im); /* no worker thread: decode here */
	return im;
}

bool stfx_image_done(const struct stfx_image *im)
{
	return im && os_atomic_load_bool(&im->done);
}

gs_texture_t *stfx_image_texture(struct stfx_image *im, uint32_t *cx, uint32_t *cy)
{
	if (!stfx_image_done(im) || !im->data)
		return NULL;
	const uint8_t *planes[1] = {im->data};
	gs_texture_t *tex = gs_texture_create(im->cx, im->cy, im->format, 1, planes, 0);
	if (cx)
		*cx = im->cx;
	if (cy)
		*cy = im->cy;
	return tex;
}

const char *stfx_image_path(const struct stfx_image *im)
{
	return im ? im->path : "";
}

void stfx_image_release(struct stfx_image *im)
{
	image_unref(im);
}

static void loader_free(void)
{
	pthread_mutex_lock(&loader_lock);
	os_task_queue_t *q = loader;
	loader = NULL;
	pthread_mutex_unlock(&loader_lock);
	os_task_queue_destroy(q); /* runs the queued decodes, then joins */
}

/* ------------------------------------------------------------------------- */
/* Module entry                                                              */
/* ------------------------------------------------------------------------- */

static void fx_module_unload(void *ud)
{
	(void)ud;
	stfx_settings_writes_free();
	loader_free();
	stfx_catalog_free();
	stfx_perf_clear();
}

void stage_fx_register(void)
{
	stfx_catalog_load();
	stfx_register_fx_filter();
	stfx_register_depth_blur();
	if (!stage_core_on_unload(fx_module_unload, NULL))
		SLOG_WARN("fx: unload hook table full; catalog freed at process exit only");
	SLOG_INFO("fx: %zu FX presets, %zu LUTs, %zu light leaks", stfx_preset_count(),
		  stfx_asset_count(STFX_ASSET_LUT), stfx_asset_count(STFX_ASSET_LEAK));
}

/* ------------------------------------------------------------------------- */
/* Rendering helpers                                                         */
/* ------------------------------------------------------------------------- */

enum gs_color_space stfx_target_space(obs_source_t *target)
{
	static const enum gs_color_space pref[] = {GS_CS_SRGB, GS_CS_SRGB_16F, GS_CS_709_EXTENDED};
	return obs_source_get_color_space(target, sizeof(pref) / sizeof(pref[0]), pref);
}

bool stfx_render_target(obs_source_t *filter, gs_texrender_t **tr, enum gs_color_space space,
			uint32_t w, uint32_t h)
{
	obs_source_t *target = obs_filter_get_target(filter);
	obs_source_t *parent = obs_filter_get_parent(filter);
	if (!target || !parent)
		return false;

	const enum gs_color_format format = gs_get_format_from_space(space);
	if (*tr && gs_texrender_get_format(*tr) != format) {
		gs_texrender_destroy(*tr);
		*tr = NULL;
	}
	if (!*tr)
		*tr = gs_texrender_create(format, GS_ZS_NONE);
	if (!*tr)
		return false;

	gs_texrender_reset(*tr);
	if (!gs_texrender_begin_with_color_space(*tr, w, h, space))
		return false;

	/* Same as obs_source_process_filter_begin_with_color_space. */
	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE,
				   GS_BLEND_INVSRCALPHA);
	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);

	const uint32_t pf = obs_source_get_output_flags(parent);
	const bool custom_draw = (pf & OBS_SOURCE_CUSTOM_DRAW) != 0;
	const bool async = (pf & OBS_SOURCE_ASYNC) != 0;
	if (target == parent && !custom_draw && !async)
		obs_source_default_render(target);
	else
		obs_source_video_render(target);

	gs_blend_state_pop();
	gs_texrender_end(*tr);
	return true;
}

bool stfx_pass(gs_texrender_t *tr, uint32_t w, uint32_t h, gs_effect_t *fx, const char *tech,
	       const struct stfx_pass_params *pp, gs_texture_t *src, bool srgb, float offset)
{
	if (!tr || !src)
		return false;
	gs_texrender_reset(tr);
	if (!gs_texrender_begin_with_color_space(tr, w, h, GS_CS_SRGB_16F))
		return false;
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);

	gs_blend_state_push();
	gs_reset_blend_state();
	gs_enable_blending(false);
	const bool prev_fb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);

	if (srgb)
		gs_effect_set_texture_srgb(pp->image, src);
	else
		gs_effect_set_texture(pp->image, src);
	if (pp->texel) {
		struct vec2 t;
		vec2_set(&t, 1.0f / (float)gs_texture_get_width(src),
			 1.0f / (float)gs_texture_get_height(src));
		gs_effect_set_vec2(pp->texel, &t);
	}
	if (pp->offset)
		gs_effect_set_float(pp->offset, offset);
	while (gs_effect_loop(fx, tech))
		gs_draw_sprite(src, 0, w, h);

	gs_enable_framebuffer_srgb(prev_fb);
	gs_blend_state_pop();
	gs_texrender_end(tr);
	return true;
}

gs_effect_t *stfx_load_effect(const char *file, bool required)
{
	char rel[64];
	snprintf(rel, sizeof(rel), "effects/%s", file);
	char *path = obs_module_file(rel);
	if (!path) {
		SLOG_ERR("effect %s not found in the plugin data folder", file);
		return NULL;
	}
	char *errors = NULL;
	gs_effect_t *fx = gs_effect_create_from_file(path, &errors);
	if (!fx) {
		if (required)
			SLOG_ERR("effect %s failed to compile: %s", file, errors ? errors : "(no log)");
		else
			SLOG_WARN("effect %s failed to compile: %s", file, errors ? errors : "(no log)");
	}
	bfree(errors);
	bfree(path);
	return fx;
}

/* ------------------------------------------------------------------------- */
/* GPU timers                                                                */
/* ------------------------------------------------------------------------- */

void stfx_timers_init(struct stfx_timers *t)
{
	memset(t, 0, sizeof(*t));
	t->available = true;
	for (int i = 0; i < STFX_TIMER_SLOTS; i++) {
		/* The OpenGL renderer has no disjoint query: range is NULL there
		 * and the range calls report a 1 GHz clock. */
		t->range[i] = gs_timer_range_create();
		t->timer[i] = gs_timer_create();
		t->tail[i] = gs_timer_create();
		if (!t->timer[i] || !t->tail[i])
			t->available = false;
	}
	if (!t->available)
		stfx_timers_free(t);
}

void stfx_timers_free(struct stfx_timers *t)
{
	for (int i = 0; i < STFX_TIMER_SLOTS; i++) {
		gs_timer_destroy(t->timer[i]);
		gs_timer_destroy(t->tail[i]);
		gs_timer_range_destroy(t->range[i]);
		t->timer[i] = NULL;
		t->tail[i] = NULL;
		t->range[i] = NULL;
		t->pending[i] = false;
		t->split[i] = false;
	}
	t->available = false;
}

void stfx_timers_tick(struct stfx_timers *t)
{
	t->frame++;
}

int stfx_timers_begin(struct stfx_timers *t)
{
	if (!t->available)
		return -1;
	if (t->begun_once && t->frame - t->last_begin < STFX_TIMER_PERIOD)
		return -1;
	for (int i = 0; i < STFX_TIMER_SLOTS; i++) {
		if (!t->pending[i]) {
			gs_timer_range_begin(t->range[i]);
			gs_timer_begin(t->timer[i]);
			t->split[i] = false;
			t->issued[i] = t->frame;
			t->last_begin = t->frame;
			t->begun_once = true;
			return i;
		}
	}
	return -1;
}

void stfx_timers_split(struct stfx_timers *t, int slot)
{
	if (slot < 0 || slot >= STFX_TIMER_SLOTS || !t->available || t->split[slot])
		return;
	gs_timer_begin(t->tail[slot]);
	t->split[slot] = true;
}

void stfx_timers_end(struct stfx_timers *t, int slot)
{
	if (slot < 0 || slot >= STFX_TIMER_SLOTS || !t->available)
		return;
	if (t->split[slot])
		gs_timer_end(t->tail[slot]);
	gs_timer_end(t->timer[slot]);
	gs_timer_range_end(t->range[slot]);
	t->pending[slot] = true;
}

int stfx_timers_poll(struct stfx_timers *t, double *out, double *tail_out, int cap)
{
	int n = 0;
	if (!t->available)
		return 0;
	for (int i = 0; i < STFX_TIMER_SLOTS; i++) {
		/* Blocking reads (see stage-fx-internal.h): old queries only. */
		if (!t->pending[i] || t->frame - t->issued[i] < STFX_TIMER_MIN_AGE)
			continue;
		bool disjoint = false;
		uint64_t freq = 0, ticks = 0, tail = 0;
		const bool ok = gs_timer_range_get_data(t->range[i], &disjoint, &freq) &&
				gs_timer_get_data(t->timer[i], &ticks) &&
				(!t->split[i] || gs_timer_get_data(t->tail[i], &tail));
		t->pending[i] = false; /* a failed read frees the slot too */
		if (!ok || disjoint || !freq || n >= cap)
			continue;
		if (!t->split[i] || tail > ticks)
			tail = ticks;
		if (tail_out)
			tail_out[n] = (double)tail * 1000.0 / (double)freq;
		out[n++] = (double)ticks * 1000.0 / (double)freq;
	}
	return n;
}

bool stfx_space_is_hdr(enum gs_color_space space)
{
	/* obs_source_get_color_space does not remap a source's own scRGB
	 * (display / game capture on an HDR monitor), so both appear here. */
	return space == GS_CS_709_EXTENDED || space == GS_CS_709_SCRGB;
}

/* ------------------------------------------------------------------------- */
/* Folders                                                                   */
/* ------------------------------------------------------------------------- */

bool stfx_open_folder(const char *dir)
{
	if (!dir || !dir[0])
		return false;
	os_mkdirs(dir);
#ifdef _WIN32
	wchar_t *w = NULL;
	if (!os_utf8_to_wcs_ptr(dir, 0, &w) || !w)
		return false;
	for (wchar_t *c = w; *c; c++) {
		if (*c == L'/')
			*c = L'\\';
	}
	/* Explorer, like OBS's own "Show Settings Folder". */
	const INT_PTR r = (INT_PTR)ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
	bfree(w);
	if (r <= 32) {
		SLOG_WARN("could not open the folder %s (error %d)", dir, (int)r);
		return false;
	}
	return true;
#else
	SLOG_INFO("folder: %s", dir); /* no file manager hook outside Windows */
	return false;
#endif
}

/* ------------------------------------------------------------------------- */
/* Locale                                                                    */
/* ------------------------------------------------------------------------- */

void stfx_set_desc(obs_property_t *p, const char *label_key)
{
	char key[96];
	snprintf(key, sizeof(key), "%s.Desc", label_key);
	const char *s = NULL;
	if (obs_module_get_string(key, &s) && s)
		obs_property_set_long_description(p, s);
}

void stfx_format1(char *out, size_t cap, const char *fmt, const char *arg)
{
	const char *at = strstr(fmt, "%1");
	if (!at) {
		stage_strlcpy(out, fmt, cap);
		return;
	}
	snprintf(out, cap, "%.*s%s%s", (int)(at - fmt), fmt, arg ? arg : "", at + 2);
}
