/*
 * stage-fx-filter.c — the Parallaxa FX filter (`stage_fx`): one uber pass
 * (data/effects/stage-uber.effect) plus optional half-resolution bloom
 * (data/effects/stage-bloom.effect).
 * docs/ux/stage-plugin-ux.md section 2, vendor-api.md 4.7 / 4.8 / 5.3.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Threads:
 *   graphics  video_tick, video_render and the deferred update() (libobs
 *             defers update of video sources to the tick): every mutation of
 *             the FX state happens here.
 *   UI        get_properties, property callbacks, buttons (settings only, or
 *             atomic pulse requests), create/destroy, and the settings writes
 *             of fx_set / panic (queued with obs_queue_task(OBS_TASK_UI) so
 *             obs_data is never edited from two threads).
 *   any       registry ops (get_info copies a snapshot under f->lock;
 *             apply_cmd queues into a small local queue drained by the tick)
 *             and the source procs fx_set / panic (same local queue).
 *
 * Own settings writes: each fx_set / panic write carries a sequence number
 * in the hidden setting fx_write_seq. Writes land in order (one UI queue),
 * so an update() whose fx_write_seq is above the highest one seen so far is
 * the landing of the filter's own write: the running tween is kept unless
 * the params differ from that write (the user edited the panel before the
 * deferred update ran). Every other update() (panel, undo, reset) is the
 * user's and jumps to the new look.
 *
 * Render paths:
 *   neutral   obs_source_skip_video_filter (bit-identical, zero cost);
 *   no bloom  obs_source_process_filter_begin_with_color_space +
 *             _end with OBS_ALLOW_DIRECT_RENDERING (one pass);
 *   bloom     own input texrender + bright pass + 3-level dual-Kawase
 *             down/up at half resolution + the uber pass; the texrenders are
 *             cached per tick (processed_frame, reset in video_tick) so a
 *             second render in the same frame (preview + program,
 *             projectors) only redoes the uber pass.
 */
#include "stage-fx-internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <util/platform.h>
#include <util/threading.h>

#define T(key) obs_module_text(key)

#define KEY_PRESET "fx_preset"
#define KEY_PRESET_APPLIED "fx_preset_applied" /* hidden: last preset copied into the sliders */
#define KEY_STATUS "status_info"
#define KEY_BLOOM_INFO "bloom_info"
#define KEY_LUT_DIR "lut_folder_path" /* info line: the user LUT folder */
#define KEY_WRITE_SEQ "fx_write_seq"  /* hidden: sequence number of the last own write */
#define DEFAULT_PRESET "cozy"
#define NEUTRAL_PRESET "neutral"

#define BLOOM_LEVELS 3                        /* dual-Kawase levels below half res */
#define BLOOM_PASSES (1 + 2 * BLOOM_LEVELS)   /* bright + downs + ups             */
#define LOCAL_Q_MAX 8
#define WRITTEN_MAX 8    /* own writes remembered for the params check */
#define SW_QUEUED_MAX 64 /* settings writes waiting for the UI thread (all filters) */
#define FLASH_CAP_STATUS_MS 10000
#define PERF_LOG_PERIOD_S 10.0

#if defined(_DEBUG) || defined(STAGE_FX_PERF_LOG)
#define PERF_LOG SLOG_INFO
#else
#define PERF_LOG SLOG_DEBUG
#endif

struct uber_params {
	gs_eparam_t *image, *lut_tex, *leak_tex, *bloom_tex, *exposure_mul, *wb_gain, *contrast,
		*saturation, *lut_amount, *vignette, *vignette_scale, *vignette_edge, *chroma, *grain,
		*grain_scale, *grain_offset, *leak_amount, *leak_offset, *flash, *bloom_amount;
};

struct bloom_params {
	gs_eparam_t *image, *texel, *offset, *threshold, *knee;
};

/* LUT / light-leak textures: the shown asset and the tween target (decoded
 * ahead on the loader thread, so a look change never waits for the disk). */
#define ASSET_SLOTS 2
struct tex_slot {
	char id[STAGE_ID_MAX]; /* "" = free */
	struct stfx_image *img; /* decode in flight */
	gs_texture_t *tex;
};
struct asset_cache {
	int kind;        /* STFX_ASSET_* */
	long generation; /* stfx_catalog_generation() seen last */
	struct tex_slot s[ASSET_SLOTS];
};
static void asset_cache_free(struct asset_cache *c);

/* Snapshot shared with other threads (f->lock). */
struct fx_shared {
	struct stage_fx_info info;
	bool gpu_failed;
	char paused[16];       /* last part paused by the perf guard ("" = none) */
	uint64_t flash_cap_ms; /* stage_core_now_ms() of the last merged flash  */
	/* get_stats proc */
	uint64_t rendered, skipped;
	double gpu_ms;         /* running average of the current 10 s window */
	uint32_t gpu_samples;
	double gpu_total_ms;   /* every sample since creation */
	uint64_t gpu_total_n;
	float flash;           /* current flash envelope                 */
	int flashes_1s;
	uint64_t flash_merged;
};

struct fx_filter {
	obs_source_t *self;

	/* Graphics resources. */
	gs_effect_t *uber, *bloom_fx;
	struct uber_params up;
	struct bloom_params bp;
	gs_texrender_t *input_tr;
	gs_texrender_t *bloom_tr[BLOOM_PASSES];
	gs_texture_t *bloom_result;
	bool processed_frame;
	enum gs_color_space cached_space;
	uint32_t cached_w, cached_h;
	struct asset_cache luts, leaks;
	gs_texture_t *lut_tex, *leak_tex; /* this render's textures (owned by the caches) */
	struct stfx_timers timers;
	bool gpu_failed;
	bool hdr_warned;

	/* FX state (graphics thread). */
	struct stfx_anim anim;
	/* Own settings writes (see the header comment): the last sequence
	 * number queued, the highest one seen in update(), and the params of
	 * the last WRITTEN_MAX writes not seen yet, oldest first. */
	long long write_seq, seen_seq;
	struct own_write {
		long long seq;
		struct stfx_params p;
	} written[WRITTEN_MAX];
	int written_n;
	char preset[STAGE_ID_MAX];  /* settings fx_preset */
	float toggle_restore[STFX_TOGGLE_COUNT]; /* amounts the FX toggle hotkeys turned off */
	float elapsed;              /* seconds, wraps every hour */
	float leak_phase;
	unsigned paused_mask;       /* STFX_PART_BIT */
	unsigned last_parts;
	bool last_neutral, last_bloom;
	struct stfx_guard guard;
	uint64_t panic_gen;
	char target_uuid[STAGE_SCENE_UUID_MAX];
	bool attached, attach_warned;
	uint64_t rendered, skipped;
	double log_t, win_gpu_sum, win_gpu_max, total_gpu_sum;
	uint32_t win_gpu_n;
	uint64_t total_gpu_n;

	/* Cross-thread. */
	volatile bool pending_pulse[STAGE_PULSE_COUNT]; /* panel buttons */
	pthread_mutex_t lock;
	struct fx_shared sh;
	struct stage_cmd local_q[LOCAL_Q_MAX]; /* stage_fx_apply() */
	size_t local_n;

	/* UI thread only. */
	bool modified_shown;
};

static const char *filter_name(struct fx_filter *f)
{
	const char *n = obs_source_get_name(f->self);
	return n ? n : "";
}

/* ------------------------------------------------------------------------- */
/* Settings <-> params (stage-plugin-ux.md 2.1)                              */
/* ------------------------------------------------------------------------- */

/* Rounds to 6 decimals so 0.3 is stored as 30, not 30.000000000000004. */
static double round6(double v)
{
	return floor(v * 1e6 + 0.5) / 1e6;
}

static void settings_to_params(obs_data_t *s, struct stfx_params *p)
{
	stfx_params_neutral(p);
	for (int i = 0; i < STFX_NUM_COUNT; i++) {
		double v = stfx_param_from_setting(i, obs_data_get_double(s, stfx_num_defs[i].key));
		p->v[i] = (float)stfx_num_clamp(i, v);
	}
	const char *lut = obs_data_get_string(s, STFX_KEY_LUT);
	const char *leak = obs_data_get_string(s, STFX_KEY_LEAK);
	if (lut && stage_id_valid(lut))
		stage_strlcpy(p->lut, lut, sizeof(p->lut));
	if (leak && stage_id_valid(leak))
		stage_strlcpy(p->leak, leak, sizeof(p->leak));

	/* An unticked group renders at its neutral values. */
	for (int g = 0; g < STFX_GROUP_COUNT; g++) {
		const char *key = stfx_group_defs[g].enabled_key;
		if (!key || obs_data_get_bool(s, key))
			continue;
		for (int i = 0; i < STFX_NUM_COUNT; i++) {
			if (stfx_num_defs[i].group == g)
				p->v[i] = (float)stfx_num_defs[i].neutral;
		}
		if (g == STFX_GROUP_LUT)
			p->lut[0] = '\0';
		if (g == STFX_GROUP_LEAK)
			p->leak[0] = '\0';
	}
}

/* Writes p into s (as defaults when defaults is true). A group whose amount
 * is above 0 is ticked, an amount of 0 unticks it (stage-plugin-ux.md 2.1). */
static void params_to_settings(obs_data_t *s, const struct stfx_params *p, bool defaults)
{
	for (int i = 0; i < STFX_NUM_COUNT; i++) {
		double v = round6(stfx_setting_from_param(i, round6((double)p->v[i])));
		if (defaults)
			obs_data_set_default_double(s, stfx_num_defs[i].key, v);
		else
			obs_data_set_double(s, stfx_num_defs[i].key, v);
	}
	if (defaults) {
		obs_data_set_default_string(s, STFX_KEY_LUT, p->lut);
		obs_data_set_default_string(s, STFX_KEY_LEAK, p->leak);
	} else {
		obs_data_set_string(s, STFX_KEY_LUT, p->lut);
		obs_data_set_string(s, STFX_KEY_LEAK, p->leak);
	}
	for (int g = 0; g < STFX_GROUP_COUNT; g++) {
		const struct stfx_group_def *gd = &stfx_group_defs[g];
		if (!gd->enabled_key)
			continue;
		bool on = p->v[gd->amount] > 0.0f;
		if (defaults)
			obs_data_set_default_bool(s, gd->enabled_key, on);
		else
			obs_data_set_bool(s, gd->enabled_key, on);
	}
}

/* Parses fx_set params JSON (already validated by the vendor) on top of p. */
static void apply_params_json(const char *json, struct stfx_params *p)
{
	if (!json || !json[0])
		return;
	obs_data_t *d = obs_data_create_from_json(json);
	if (!d) {
		SLOG_WARN("fx_set: params are not valid JSON");
		return;
	}
	for (obs_data_item_t *it = obs_data_first(d); it; obs_data_item_next(&it)) {
		const char *k = obs_data_item_get_name(it);
		int i = stfx_num_find(k);
		if (i >= 0 && obs_data_item_gettype(it) == OBS_DATA_NUMBER) {
			p->v[i] = (float)stfx_num_clamp(i, obs_data_item_get_double(it));
		} else if ((!strcmp(k, STFX_PARAM_LUT) || !strcmp(k, STFX_PARAM_LEAK)) &&
			   obs_data_item_gettype(it) == OBS_DATA_STRING) {
			const char *v = obs_data_item_get_string(it);
			char *dst = !strcmp(k, STFX_PARAM_LUT) ? p->lut : p->leak;
			if (!v[0] || stage_id_valid(v))
				stage_strlcpy(dst, v, STAGE_ID_MAX);
		} else {
			SLOG_WARN("fx_set: ignoring param '%s'", k);
		}
	}
	obs_data_release(d);
}

/* ------------------------------------------------------------------------- */
/* State changes (graphics thread)                                           */
/* ------------------------------------------------------------------------- */

static void refresh_shared(struct fx_filter *f)
{
	struct stfx_preset_info pi;
	bool exact = f->preset[0] && stfx_preset_get(f->preset, &pi) &&
		     stfx_params_equal(&pi.params, &f->anim.to);
	float pulses[STAGE_PULSE_COUNT];
	stfx_anim_pulse_values(&f->anim, pulses);

	const bool enabled = obs_source_enabled(f->self);

	pthread_mutex_lock(&f->lock);
	stage_strlcpy(f->sh.info.preset, exact ? f->preset : "", sizeof(f->sh.info.preset));
	f->sh.info.enabled = enabled;
	stage_strlcpy(f->sh.info.paused, f->paused_mask ? f->sh.paused : "", sizeof(f->sh.info.paused));
	f->sh.flash = pulses[STAGE_PULSE_FLASH];
	f->sh.flashes_1s = stfx_anim_flashes_in_window(&f->anim);
	f->sh.flash_merged = f->anim.flash_merged;
	pthread_mutex_unlock(&f->lock);
}

static void clear_pauses(struct fx_filter *f)
{
	bool had = f->paused_mask != 0;
	f->paused_mask = 0;
	stfx_guard_reset(&f->guard);
	if (had) {
		pthread_mutex_lock(&f->lock);
		f->sh.paused[0] = '\0';
		pthread_mutex_unlock(&f->lock);
		obs_source_update_properties(f->self);
	}
}

/* Settings writes waiting for the UI thread, owned here (not by the task)
 * so that a task that never runs (shutdown, a host without a UI task
 * handler) leaks nothing: the oldest is dropped past SW_QUEUED_MAX and the
 * rest are released on module unload. The task gets only the id. */
struct settings_write {
	uintptr_t id;
	obs_weak_source_t *weak;
	obs_data_t *data;
};
static pthread_mutex_t sw_lock = PTHREAD_MUTEX_INITIALIZER;
static struct settings_write sw_queued[SW_QUEUED_MAX];
static size_t sw_n;
static uintptr_t sw_next_id;
static bool sw_drop_warned;

static void sw_release(struct settings_write *w)
{
	obs_weak_source_release(w->weak);
	obs_data_release(w->data);
}

/* UI thread: the panel edits the same obs_data, which has no lock. */
static void settings_write_task(void *param)
{
	const uintptr_t id = (uintptr_t)param;
	struct settings_write w = {0};
	bool found = false;
	pthread_mutex_lock(&sw_lock);
	for (size_t i = 0; i < sw_n; i++) {
		if (sw_queued[i].id == id) {
			w = sw_queued[i];
			memmove(&sw_queued[i], &sw_queued[i + 1], (sw_n - i - 1) * sizeof(sw_queued[0]));
			sw_n--;
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&sw_lock);
	if (!found)
		return; /* dropped, or released on unload */
	obs_source_t *src = obs_weak_source_get_source(w.weak);
	if (src) {
		obs_source_update(src, w.data);
		obs_source_update_properties(src); /* an open panel shows the new values */
		obs_source_release(src);
	}
	sw_release(&w);
}

void stfx_settings_writes_free(void)
{
	pthread_mutex_lock(&sw_lock);
	for (size_t i = 0; i < sw_n; i++)
		sw_release(&sw_queued[i]);
	sw_n = 0;
	pthread_mutex_unlock(&sw_lock);
}

/* Writes target params (and the preset id, when given) into the settings so
 * the panel shows them; the deferred update() recognises its own write by
 * its sequence number. Graphics thread. */
static void write_settings(struct fx_filter *f, const struct stfx_params *p, const char *preset)
{
	struct settings_write w;
	w.weak = obs_source_get_weak_source(f->self);
	w.data = obs_data_create();
	params_to_settings(w.data, p, false);
	if (preset) {
		obs_data_set_string(w.data, KEY_PRESET, preset);
		obs_data_set_string(w.data, KEY_PRESET_APPLIED, preset);
		stage_strlcpy(f->preset, preset, sizeof(f->preset));
	}
	const long long seq = ++f->write_seq;
	obs_data_set_int(w.data, KEY_WRITE_SEQ, seq);
	if (f->written_n == WRITTEN_MAX) {
		memmove(&f->written[0], &f->written[1], (WRITTEN_MAX - 1) * sizeof(f->written[0]));
		f->written_n--;
	}
	f->written[f->written_n].seq = seq;
	f->written[f->written_n].p = *p;
	f->written_n++;

	struct settings_write dropped = {0};
	bool drop = false, warn = false;
	pthread_mutex_lock(&sw_lock);
	if (sw_n == SW_QUEUED_MAX) {
		dropped = sw_queued[0];
		memmove(&sw_queued[0], &sw_queued[1], (SW_QUEUED_MAX - 1) * sizeof(sw_queued[0]));
		sw_n--;
		drop = true;
		warn = !sw_drop_warned;
		sw_drop_warned = true;
	}
	w.id = ++sw_next_id;
	sw_queued[sw_n++] = w;
	pthread_mutex_unlock(&sw_lock);
	if (drop) {
		sw_release(&dropped);
		if (warn)
			SLOG_WARN("fx: UI tasks are not running; older effect settings writes dropped");
	}
	obs_queue_task(OBS_TASK_UI, settings_write_task, (void *)w.id, false);
}

/* True when settings s are the landing of one of this filter's own writes
 * that update() has not seen yet, with the params p it wrote. Marks that
 * write (and every older one) as seen. Graphics thread. */
static bool take_own_write(struct fx_filter *f, obs_data_t *s, const struct stfx_params *p)
{
	const long long seq = obs_data_get_int(s, KEY_WRITE_SEQ);
	if (seq <= f->seen_seq)
		return false; /* panel, undo, reset to preset: the user's change */
	f->seen_seq = seq;
	bool own = true; /* forgotten (over WRITTEN_MAX queued): still ours */
	int keep = 0;
	for (int i = 0; i < f->written_n; i++) {
		if (f->written[i].seq == seq)
			own = stfx_params_equal(p, &f->written[i].p);
		else if (f->written[i].seq > seq)
			f->written[keep++] = f->written[i];
	}
	f->written_n = keep;
	return own;
}

_Static_assert(STFX_TOGGLE_COUNT == STAGE_FX_TOGGLE_COUNT, "FX toggle tables");

/* FX toggle hotkey t against target (the target the commands before it in
 * this tick left), so N presses within one tick flip N times. With nothing
 * to restore it turns on at the selected look's own amount. */
static void toggle_target(struct fx_filter *f, int t, struct stfx_params *target)
{
	struct stfx_preset_info pi;
	float preset_amount = 0.0f;
	if (stfx_preset_get(f->preset, &pi))
		preset_amount = pi.params.v[stfx_toggle_defs[t].num];
	stfx_toggle_apply(target, t, f->toggle_restore, preset_amount);
}

static void do_fx_set(struct fx_filter *f, const struct stage_cmd *cmd)
{
	struct stfx_params target = f->anim.to;
	const char *preset = cmd->u.fx.preset[0] ? cmd->u.fx.preset : NULL;
	if (preset) {
		struct stfx_preset_info pi;
		if (!stfx_preset_get(preset, &pi)) {
			SLOG_WARN("fx_set: unknown preset '%s' ignored", preset);
			preset = NULL;
		} else {
			target = pi.params;
		}
	}
	apply_params_json(cmd->u.fx.params_json, &target);
	if (cmd->u.fx.toggle > 0 && cmd->u.fx.toggle <= STFX_TOGGLE_COUNT)
		toggle_target(f, cmd->u.fx.toggle - 1, &target);
	stfx_anim_set(&f->anim, &target, cmd->duration_ms, cmd->ease);
	if (preset)
		clear_pauses(f); /* a new look is an explicit request */
	write_settings(f, &target, preset);
}

static void do_pulse(struct fx_filter *f, int effect, float intensity, uint32_t duration_ms)
{
	if (effect < 0)
		return;
	if (!stfx_anim_pulse(&f->anim, effect, intensity, duration_ms)) {
		uint64_t now = stage_core_now_ms();
		pthread_mutex_lock(&f->lock);
		bool fresh = now - f->sh.flash_cap_ms > FLASH_CAP_STATUS_MS || !f->sh.flash_cap_ms;
		f->sh.flash_cap_ms = now;
		pthread_mutex_unlock(&f->lock);
		if (fresh) {
			SLOG_INFO("fx '%s': flash merged (at most %d flashes per second)", filter_name(f),
				  STAGE_FLASH_MAX_PER_SEC);
			obs_source_update_properties(f->self);
		}
	}
}

static void do_panic(struct fx_filter *f)
{
	struct stfx_params n;
	stfx_params_neutral(&n);
	stfx_anim_cancel_pulses(&f->anim);
	stfx_anim_jump(&f->anim, &n);
	clear_pauses(f);
	write_settings(f, &n, NEUTRAL_PRESET);
}

static void on_cmd(const struct stage_cmd *cmd, void *ud)
{
	struct fx_filter *f = ud;
	switch (cmd->type) {
	case STAGE_CMD_FX_SET:
		do_fx_set(f, cmd);
		break;
	case STAGE_CMD_FX_PULSE:
		do_pulse(f, stage_pulse_from_name(cmd->u.pulse.effect), cmd->intensity,
			 cmd->duration_ms);
		break;
	case STAGE_CMD_PANIC:
		do_panic(f);
		break;
	default:
		break;
	}
}

static void on_paused(struct fx_filter *f, int part, double cost_ms)
{
	obs_source_t *parent = obs_filter_get_parent(f->self);
	struct stage_fx_perf_event e;
	memset(&e, 0, sizeof(e));
	stage_strlcpy(e.source, parent ? obs_source_get_name(parent) : "", sizeof(e.source));
	stage_strlcpy(e.source_uuid, f->target_uuid, sizeof(e.source_uuid));
	stage_strlcpy(e.filter, STAGE_FILTER_FX, sizeof(e.filter));
	e.cost_ms = cost_ms;
	e.budget_ms = STAGE_PERF_BUDGET_MS;
	e.window_s = STAGE_PERF_WINDOW_S;
	stage_strlcpy(e.paused, stfx_part_name(part), sizeof(e.paused));
	stfx_perf_push(&e);

	SLOG_WARN("fx '%s' on '%s': render cost %.2f ms/frame over %.1f ms for %d s; paused %s",
		  filter_name(f), e.source, cost_ms, STAGE_PERF_BUDGET_MS, STAGE_PERF_WINDOW_S,
		  e.paused);
	pthread_mutex_lock(&f->lock);
	stage_strlcpy(f->sh.paused, e.paused, sizeof(f->sh.paused));
	pthread_mutex_unlock(&f->lock);
	obs_source_update_properties(f->self);
}

/* ------------------------------------------------------------------------- */
/* Registry                                                                  */
/* ------------------------------------------------------------------------- */

static void op_get_info(void *ud, struct stage_fx_info *out)
{
	struct fx_filter *f = ud;
	pthread_mutex_lock(&f->lock);
	*out = f->sh.info;
	pthread_mutex_unlock(&f->lock);
}

/* Called under the registry lock: no OBS calls; queue for the next tick. */
static void op_apply_cmd(void *ud, const struct stage_cmd *cmd)
{
	struct fx_filter *f = ud;
	pthread_mutex_lock(&f->lock);
	if (f->local_n < LOCAL_Q_MAX)
		f->local_q[f->local_n++] = *cmd;
	pthread_mutex_unlock(&f->lock);
}

/* Source procs (any thread): the protocol's fx_set / panic for local
 * callers (scripts, other plugins, tests), through the same local queue. */
static void proc_fx_set(void *data, calldata_t *cd)
{
	struct stage_cmd c;
	memset(&c, 0, sizeof(c));
	c.type = STAGE_CMD_FX_SET;
	const char *preset = calldata_string(cd, "preset");
	const char *params = calldata_string(cd, "params");
	stage_strlcpy(c.u.fx.preset, preset ? preset : "", sizeof(c.u.fx.preset));
	stage_strlcpy(c.u.fx.params_json, params ? params : "", sizeof(c.u.fx.params_json));
	const long long ms = calldata_int(cd, "duration_ms");
	c.duration_ms = ms <= 0 ? 0 : ms < STAGE_SET_DURATION_MAX ? (uint32_t)ms : STAGE_SET_DURATION_MAX;
	c.ease = STAGE_EASE_IN_OUT_CUBIC;
	op_apply_cmd(data, &c);
}

static void proc_panic(void *data, calldata_t *cd)
{
	(void)cd;
	struct stage_cmd c;
	memset(&c, 0, sizeof(c));
	c.type = STAGE_CMD_PANIC;
	op_apply_cmd(data, &c);
}

static const struct stage_fx_ops fx_ops = {
	.get_info = op_get_info,
	.apply_cmd = op_apply_cmd,
};

static void attach_check(struct fx_filter *f)
{
	obs_source_t *parent = obs_filter_get_parent(f->self);
	const char *uuid = parent ? obs_source_get_uuid(parent) : NULL;
	if (!uuid)
		uuid = "";
	const bool same = strcmp(uuid, f->target_uuid) == 0;
	if (same && (f->attached || !uuid[0]))
		return;
	if (!same) {
		if (f->attached)
			stage_fx_detach(f->target_uuid, f);
		f->attached = false;
		f->attach_warned = false;
		stage_strlcpy(f->target_uuid, uuid, sizeof(f->target_uuid));
	}
	if (!uuid[0])
		return;
	/* Retried every tick while another Parallaxa FX owns the target, so this
	 * one takes over as soon as the first is removed. */
	f->attached = stage_fx_attach(uuid, &fx_ops, f);
	if (!f->attached && !f->attach_warned) {
		SLOG_WARN("fx '%s': '%s' already has a Parallaxa FX; this one is not remote-controlled",
			  filter_name(f), parent ? obs_source_get_name(parent) : "");
		f->attach_warned = true;
	}
}

/* ------------------------------------------------------------------------- */
/* obs_source_info callbacks                                                 */
/* ------------------------------------------------------------------------- */

static const char *fx_get_name(void *type_data)
{
	(void)type_data;
	return obs_module_text("Stage.Filter.FX");
}

static void fx_update(void *data, obs_data_t *settings)
{
	struct fx_filter *f = data;
	struct stfx_params p;
	settings_to_params(settings, &p);
	const char *preset = obs_data_get_string(settings, KEY_PRESET);
	stage_strlcpy(f->preset, preset ? preset : "", sizeof(f->preset));

	/* Our own write from a command (possibly an older one landing while a
	 * newer one is still queued): the tween keeps running. */
	if (take_own_write(f, settings, &p))
		return;
	stfx_anim_jump(&f->anim, &p);
	clear_pauses(f); /* the user changed something: give every part a new chance */
}

static void cache_params(struct fx_filter *f)
{
	gs_effect_t *u = f->uber;
	if (u) {
		f->up.image = gs_effect_get_param_by_name(u, "image");
		f->up.lut_tex = gs_effect_get_param_by_name(u, "lut_tex");
		f->up.leak_tex = gs_effect_get_param_by_name(u, "leak_tex");
		f->up.bloom_tex = gs_effect_get_param_by_name(u, "bloom_tex");
		f->up.exposure_mul = gs_effect_get_param_by_name(u, "exposure_mul");
		f->up.wb_gain = gs_effect_get_param_by_name(u, "wb_gain");
		f->up.contrast = gs_effect_get_param_by_name(u, "contrast");
		f->up.saturation = gs_effect_get_param_by_name(u, "saturation");
		f->up.lut_amount = gs_effect_get_param_by_name(u, "lut_amount");
		f->up.vignette = gs_effect_get_param_by_name(u, "vignette");
		f->up.vignette_scale = gs_effect_get_param_by_name(u, "vignette_scale");
		f->up.vignette_edge = gs_effect_get_param_by_name(u, "vignette_edge");
		f->up.chroma = gs_effect_get_param_by_name(u, "chroma");
		f->up.grain = gs_effect_get_param_by_name(u, "grain");
		f->up.grain_scale = gs_effect_get_param_by_name(u, "grain_scale");
		f->up.grain_offset = gs_effect_get_param_by_name(u, "grain_offset");
		f->up.leak_amount = gs_effect_get_param_by_name(u, "leak_amount");
		f->up.leak_offset = gs_effect_get_param_by_name(u, "leak_offset");
		f->up.flash = gs_effect_get_param_by_name(u, "flash");
		f->up.bloom_amount = gs_effect_get_param_by_name(u, "bloom_amount");
	}
	gs_effect_t *b = f->bloom_fx;
	if (b) {
		f->bp.image = gs_effect_get_param_by_name(b, "image");
		f->bp.texel = gs_effect_get_param_by_name(b, "texel");
		f->bp.offset = gs_effect_get_param_by_name(b, "offset");
		f->bp.threshold = gs_effect_get_param_by_name(b, "threshold");
		f->bp.knee = gs_effect_get_param_by_name(b, "knee");
	}
}

static void get_stats(void *data, calldata_t *cd)
{
	struct fx_filter *f = data;
	struct fx_shared s;
	pthread_mutex_lock(&f->lock);
	s = f->sh;
	pthread_mutex_unlock(&f->lock);
	calldata_set_int(cd, "rendered", (long long)s.rendered);
	calldata_set_int(cd, "skipped", (long long)s.skipped);
	calldata_set_float(cd, "gpu_ms", s.gpu_ms);
	calldata_set_int(cd, "gpu_samples", s.gpu_samples);
	calldata_set_float(cd, "gpu_total_ms", s.gpu_total_ms);
	calldata_set_int(cd, "gpu_total_n", (long long)s.gpu_total_n);
	calldata_set_float(cd, "flash", s.flash);
	calldata_set_int(cd, "flashes_1s", s.flashes_1s);
	calldata_set_int(cd, "flash_merged", (long long)s.flash_merged);
	calldata_set_bool(cd, "bloom", s.info.bloom);
	calldata_set_bool(cd, "neutral", s.info.neutral);
	calldata_set_string(cd, "paused", s.info.paused);
	calldata_set_string(cd, "preset", s.info.preset);
}

static void *fx_create(obs_data_t *settings, obs_source_t *source)
{
	struct fx_filter *f = bzalloc(sizeof(*f));
	f->self = source;
	pthread_mutex_init(&f->lock, NULL);
	struct stfx_params n;
	stfx_params_neutral(&n);
	stfx_anim_init(&f->anim, &n);
	stfx_guard_init(&f->guard, STAGE_PERF_BUDGET_MS, STAGE_PERF_WINDOW_S);
	f->panic_gen = stage_panic_generation();
	f->last_neutral = true;
	f->sh.info.neutral = true;
	f->luts.kind = STFX_ASSET_LUT;
	f->leaks.kind = STFX_ASSET_LEAK;
	f->luts.generation = f->leaks.generation = stfx_catalog_generation();
	/* Sequence numbers continue from the saved settings, so a write of this
	 * session is never mistaken for an older one. */
	f->write_seq = f->seen_seq = obs_data_get_int(settings, KEY_WRITE_SEQ);

	obs_enter_graphics();
	f->uber = stfx_load_effect("stage-uber.effect", true);
	f->bloom_fx = stfx_load_effect("stage-bloom.effect", false);
	stfx_timers_init(&f->timers);
	obs_leave_graphics();
	cache_params(f);
	if (!f->uber) {
		f->gpu_failed = true;
		f->sh.gpu_failed = true;
		SLOG_ERR("fx '%s': effects unavailable on this GPU; showing the scene without effects",
			 filter_name(f));
	}

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph,
			 "void get_stats(out int rendered, out int skipped, out float gpu_ms, "
			 "out int gpu_samples, out float gpu_total_ms, out int gpu_total_n, "
			 "out float flash, out int flashes_1s, "
			 "out int flash_merged, out bool bloom, out bool neutral, "
			 "out string paused, out string preset)",
			 get_stats, f);
	proc_handler_add(ph, "void fx_set(in string preset, in string params, in int duration_ms)",
			 proc_fx_set, f);
	proc_handler_add(ph, "void panic()", proc_panic, f);

	fx_update(f, settings);
	refresh_shared(f);
	return f;
}

static void fx_destroy(void *data)
{
	struct fx_filter *f = data;
	stage_fx_detach_all(f);
	SLOG_INFO("fx '%s' removed: %llu frames rendered, %llu skipped as neutral", filter_name(f),
		  (unsigned long long)f->rendered, (unsigned long long)f->skipped);

	obs_enter_graphics();
	gs_effect_destroy(f->uber);
	gs_effect_destroy(f->bloom_fx);
	gs_texrender_destroy(f->input_tr);
	for (int i = 0; i < BLOOM_PASSES; i++)
		gs_texrender_destroy(f->bloom_tr[i]);
	asset_cache_free(&f->luts);
	asset_cache_free(&f->leaks);
	stfx_timers_free(&f->timers);
	obs_leave_graphics();

	pthread_mutex_destroy(&f->lock);
	bfree(f);
}

static void fx_defaults(obs_data_t *settings)
{
	struct stfx_preset_info pi;
	const char *id = DEFAULT_PRESET;
	if (!stfx_preset_get(DEFAULT_PRESET, &pi)) {
		id = NEUTRAL_PRESET;
		stfx_params_neutral(&pi.params);
	}
	obs_data_set_default_string(settings, KEY_PRESET, id);
	obs_data_set_default_string(settings, KEY_PRESET_APPLIED, id);
	params_to_settings(settings, &pi.params, true);
}

/* --- tick ---------------------------------------------------------------- */

static void drain_local(struct fx_filter *f)
{
	struct stage_cmd q[LOCAL_Q_MAX];
	size_t n;
	pthread_mutex_lock(&f->lock);
	n = f->local_n;
	memcpy(q, f->local_q, n * sizeof(q[0]));
	f->local_n = 0;
	pthread_mutex_unlock(&f->lock);
	for (size_t i = 0; i < n; i++)
		on_cmd(&q[i], f);
}

/* GPU cost: the running average of the current window feeds get_stats; the
 * window is logged and restarted every PERF_LOG_PERIOD_S seconds. */
static void perf_log(struct fx_filter *f, float seconds)
{
	const double avg = f->win_gpu_n ? f->win_gpu_sum / f->win_gpu_n : 0.0;
	if (f->win_gpu_n) {
		pthread_mutex_lock(&f->lock);
		f->sh.gpu_ms = avg;
		f->sh.gpu_samples = f->win_gpu_n;
		f->sh.gpu_total_ms = f->total_gpu_sum;
		f->sh.gpu_total_n = f->total_gpu_n;
		pthread_mutex_unlock(&f->lock);
	}
	f->log_t += seconds;
	if (f->log_t < PERF_LOG_PERIOD_S)
		return;
	f->log_t = 0.0;
	if (f->win_gpu_n)
		PERF_LOG("fx '%s': GPU %.3f ms avg, %.3f ms max over %u renders (bloom %s); "
			 "%llu rendered, %llu skipped",
			 filter_name(f), avg, f->win_gpu_max, f->win_gpu_n, f->last_bloom ? "on" : "off",
			 (unsigned long long)f->rendered, (unsigned long long)f->skipped);
	f->win_gpu_sum = f->win_gpu_max = 0.0;
	f->win_gpu_n = 0;
}

static void fx_tick(void *data, float seconds)
{
	struct fx_filter *f = data;
	f->processed_frame = false;
	stfx_timers_tick(&f->timers);
	attach_check(f);

	const uint64_t gen = stage_panic_generation();
	if (gen != f->panic_gen) {
		f->panic_gen = gen;
		do_panic(f);
	}
	for (int i = 0; i < STAGE_PULSE_COUNT; i++) {
		if (os_atomic_exchange_bool(&f->pending_pulse[i], false))
			do_pulse(f, i, 1.0f, STFX_PULSE_DURATION_DEFAULT);
	}
	if (f->attached)
		stage_cmdq_drain_types(f->target_uuid, STAGE_CMD_MASK_FX, on_cmd, f);
	drain_local(f);

	stfx_anim_tick(&f->anim, seconds);
	f->elapsed = fmodf(f->elapsed + seconds, 3600.0f);
	f->leak_phase = fmodf(f->leak_phase + seconds * f->anim.cur.v[STFX_LEAK_SPEED] * 0.06f, 1.0f);

	double cost = 0.0;
	int part = stfx_guard_tick(&f->guard, seconds, f->last_parts, &f->paused_mask, &cost);
	if (part >= 0)
		on_paused(f, part, cost);

	perf_log(f, seconds);
	refresh_shared(f);
}

/* --- render -------------------------------------------------------------- */

static const char *asset_label(int kind)
{
	return kind == STFX_ASSET_LUT ? "LUT" : "light leak";
}

static void slot_clear(struct tex_slot *sl)
{
	gs_texture_destroy(sl->tex);
	stfx_image_release(sl->img);
	memset(sl, 0, sizeof(*sl));
}

/* Uploads a finished decode (graphics context). */
static void slot_finish(struct tex_slot *sl, int kind)
{
	if (!sl->img || !stfx_image_done(sl->img))
		return;
	uint32_t cx = 0, cy = 0;
	sl->tex = stfx_image_texture(sl->img, &cx, &cy);
	if (!sl->tex) {
		SLOG_WARN("could not load %s", stfx_image_path(sl->img));
	} else if (kind == STFX_ASSET_LUT && (cx != 512 || cy != 512)) {
		SLOG_WARN("LUT %s is %ux%u; LUT strips must be 512x512 PNG (64^3)",
			  stfx_image_path(sl->img), cx, cy);
		gs_texture_destroy(sl->tex);
		sl->tex = NULL;
	}
	stfx_image_release(sl->img);
	sl->img = NULL;
}

static void slot_start(struct tex_slot *sl, const char *id, int kind)
{
	stage_strlcpy(sl->id, id, sizeof(sl->id));
	char path[1024];
	if (!stfx_asset_path(kind, id, path, sizeof(path))) {
		SLOG_WARN("%s '%s' not found", asset_label(kind), id);
		return; /* the slot remembers the id: warned once */
	}
	sl->img = stfx_image_load(path);
}

/* Texture of `cur` (NULL while it decodes); keeps `next` loaded or loading
 * and drops everything else. Graphics context. */
static gs_texture_t *asset_get(struct asset_cache *c, const char *cur, const char *next)
{
	/* The catalog was rescanned (panel opened): retry ids that were missing
	 * or failed, so a file added later under that id loads without a
	 * reselect. */
	const long gen = stfx_catalog_generation();
	if (gen != c->generation) {
		c->generation = gen;
		for (int i = 0; i < ASSET_SLOTS; i++) {
			if (c->s[i].id[0] && !c->s[i].tex && !c->s[i].img)
				slot_clear(&c->s[i]);
		}
	}
	for (int i = 0; i < ASSET_SLOTS; i++) {
		struct tex_slot *sl = &c->s[i];
		if (!sl->id[0])
			continue;
		if (strcmp(sl->id, cur) != 0 && strcmp(sl->id, next) != 0)
			slot_clear(sl);
		else
			slot_finish(sl, c->kind);
	}
	const char *want[2] = {cur, next};
	for (int w = 0; w < 2; w++) {
		if (!want[w][0])
			continue;
		int have = -1, free_slot = -1;
		for (int i = 0; i < ASSET_SLOTS; i++) {
			if (!strcmp(c->s[i].id, want[w]))
				have = i;
			else if (!c->s[i].id[0] && free_slot < 0)
				free_slot = i;
		}
		if (have < 0 && free_slot >= 0)
			slot_start(&c->s[free_slot], want[w], c->kind);
	}
	for (int i = 0; cur[0] && i < ASSET_SLOTS; i++) {
		if (!strcmp(c->s[i].id, cur))
			return c->s[i].tex;
	}
	return NULL;
}

static void asset_cache_free(struct asset_cache *c)
{
	for (int i = 0; i < ASSET_SLOTS; i++)
		slot_clear(&c->s[i]);
}

static float srgb_to_linear(float v)
{
	return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

static void set_uber_params(struct fx_filter *f, const struct stfx_uniforms *u, uint32_t w,
			    uint32_t h, gs_texture_t *bloom)
{
	struct vec2 leak_off, vig_scale, vig_edge, grain_scale, grain_off;
	struct vec3 wb;
	vec3_set(&wb, u->wb_gain[0], u->wb_gain[1], u->wb_gain[2]);
	vec2_set(&leak_off, f->leak_phase, 0.08f * sinf(f->leak_phase * 6.2831853f));

	/* Vignette shape: radius 1 at the corners; roundness stretches x by the
	 * aspect ratio (1 = circle, 0 = follows the frame). */
	const float aspect = (float)w / (float)(h ? h : 1);
	const float sx = 1.0f + (aspect - 1.0f) * u->vignette_round;
	const float inv_len = 1.0f / sqrtf(0.25f * sx * sx + 0.25f);
	vec2_set(&vig_scale, sx * inv_len, inv_len);
	vec2_set(&vig_edge, 0.75f + (0.15f - 0.75f) * u->vignette_soft, 1.0f + 0.25f * u->vignette_soft);

	/* Grain cells of grain_size output pixels; a new pattern 24 times a second. */
	const float gsz = u->grain_size > 0.01f ? u->grain_size : 0.01f;
	const float seed = fmodf(floorf(f->elapsed * 24.0f), 997.0f);
	vec2_set(&grain_scale, (float)w / gsz, (float)h / gsz);
	vec2_set(&grain_off, seed * 17.0f, seed * 31.0f);

	gs_effect_set_float(f->up.exposure_mul, u->exposure_mul);
	gs_effect_set_vec3(f->up.wb_gain, &wb);
	gs_effect_set_float(f->up.contrast, u->contrast);
	gs_effect_set_float(f->up.saturation, u->saturation);
	gs_effect_set_float(f->up.lut_amount, u->lut_amount);
	gs_effect_set_texture(f->up.lut_tex, u->lut_amount > 0.0f ? f->lut_tex : NULL);
	gs_effect_set_float(f->up.vignette, u->vignette);
	gs_effect_set_vec2(f->up.vignette_scale, &vig_scale);
	gs_effect_set_vec2(f->up.vignette_edge, &vig_edge);
	gs_effect_set_float(f->up.chroma, u->chroma);
	gs_effect_set_float(f->up.grain, u->grain);
	gs_effect_set_vec2(f->up.grain_scale, &grain_scale);
	gs_effect_set_vec2(f->up.grain_offset, &grain_off);
	gs_effect_set_float(f->up.leak_amount, u->leak_amount);
	gs_effect_set_vec2(f->up.leak_offset, &leak_off);
	gs_effect_set_texture(f->up.leak_tex, u->leak_amount > 0.0f ? f->leak_tex : NULL);
	gs_effect_set_float(f->up.flash, u->flash);
	gs_effect_set_float(f->up.bloom_amount, bloom ? u->bloom : 0.0f);
	gs_effect_set_texture(f->up.bloom_tex, bloom);
}

/* Bright pass at half resolution + 3-level dual-Kawase down/up. */
static gs_texture_t *render_bloom(struct fx_filter *f, gs_texture_t *input, uint32_t w, uint32_t h,
				  const struct stfx_uniforms *u)
{
	uint32_t lw[BLOOM_LEVELS + 1], lh[BLOOM_LEVELS + 1];
	lw[0] = w / 2 ? w / 2 : 1;
	lh[0] = h / 2 ? h / 2 : 1;
	for (int i = 1; i <= BLOOM_LEVELS; i++) {
		lw[i] = lw[i - 1] / 2 ? lw[i - 1] / 2 : 1;
		lh[i] = lh[i - 1] / 2 ? lh[i - 1] / 2 : 1;
	}
	for (int i = 0; i < BLOOM_PASSES; i++) {
		if (!f->bloom_tr[i])
			f->bloom_tr[i] = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
		if (!f->bloom_tr[i])
			return NULL;
	}

	const struct stfx_pass_params pp = {f->bp.image, f->bp.texel, f->bp.offset};
	const float thr = srgb_to_linear(u->bloom_threshold);
	gs_effect_set_float(f->bp.threshold, thr);
	gs_effect_set_float(f->bp.knee, thr * 0.5f > 0.05f ? thr * 0.5f : 0.05f);
	if (!stfx_pass(f->bloom_tr[0], lw[0], lh[0], f->bloom_fx, "Bright", &pp, input, true, 1.0f))
		return NULL;

	gs_texture_t *src = gs_texrender_get_texture(f->bloom_tr[0]);
	int k = 1;
	for (int i = 1; i <= BLOOM_LEVELS; i++, k++) {
		if (!stfx_pass(f->bloom_tr[k], lw[i], lh[i], f->bloom_fx, "Down", &pp, src, false,
			       1.0f))
			return NULL;
		src = gs_texrender_get_texture(f->bloom_tr[k]);
	}
	for (int i = BLOOM_LEVELS - 1; i >= 0; i--, k++) {
		if (!stfx_pass(f->bloom_tr[k], lw[i], lh[i], f->bloom_fx, "Up", &pp, src, false, 1.0f))
			return NULL;
		src = gs_texrender_get_texture(f->bloom_tr[k]);
	}
	return src;
}

static void render_direct(struct fx_filter *f, enum gs_color_space space, uint32_t w, uint32_t h,
			  const struct stfx_uniforms *u)
{
	const enum gs_color_format format = gs_get_format_from_space(space);
	if (!obs_source_process_filter_begin_with_color_space(f->self, format, space,
							      OBS_ALLOW_DIRECT_RENDERING))
		return;
	set_uber_params(f, u, w, h, NULL);
	const int slot = stfx_timers_begin(&f->timers);
	stfx_guard_render(&f->guard, true);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	obs_source_process_filter_end(f->self, f->uber, 0, 0);
	gs_blend_state_pop();
	stfx_timers_end(&f->timers, slot);
}

static bool render_with_bloom(struct fx_filter *f, enum gs_color_space space, uint32_t w,
			      uint32_t h, const struct stfx_uniforms *u)
{
	const bool cached = f->processed_frame && f->cached_space == space && f->cached_w == w &&
			    f->cached_h == h && f->bloom_result;
	int slot = -1;
	if (!cached) {
		f->bloom_result = NULL;
		if (!stfx_render_target(f->self, &f->input_tr, space, w, h))
			return false;
		slot = stfx_timers_begin(&f->timers);
		gs_texture_t *in = gs_texrender_get_texture(f->input_tr);
		f->bloom_result = in ? render_bloom(f, in, w, h, u) : NULL;
		f->processed_frame = true;
		f->cached_space = space;
		f->cached_w = w;
		f->cached_h = h;
	}
	/* A cached render (later in the same frame) is not measured; the guard
	 * counts it as the final pass only. */

	gs_texture_t *input = f->input_tr ? gs_texrender_get_texture(f->input_tr) : NULL;
	if (!input || !f->bloom_result) {
		stfx_timers_end(&f->timers, slot);
		return false;
	}
	stfx_guard_render(&f->guard, !cached);
	stfx_timers_split(&f->timers, slot);

	set_uber_params(f, u, w, h, f->bloom_result);
	/* Same state as obs_source_process_filter_end for an sRGB-aware filter. */
	const bool prev_fb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_effect_set_texture_srgb(f->up.image, input);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(f->uber, "Draw"))
		gs_draw_sprite(input, 0, w, h);
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(prev_fb);
	stfx_timers_end(&f->timers, slot);
	return true;
}

static void collect_timers(struct fx_filter *f)
{
	double ms[STFX_TIMER_SLOTS], pass_ms[STFX_TIMER_SLOTS];
	int n = stfx_timers_poll(&f->timers, ms, pass_ms, STFX_TIMER_SLOTS);
	for (int i = 0; i < n; i++) {
		stfx_guard_sample(&f->guard, ms[i], pass_ms[i]);
		f->win_gpu_sum += ms[i];
		if (ms[i] > f->win_gpu_max)
			f->win_gpu_max = ms[i];
		f->win_gpu_n++;
		f->total_gpu_sum += ms[i];
		f->total_gpu_n++;
	}
}

/* Render counters and flags for get_info / get_stats (cheap, once per render). */
static void publish_render(struct fx_filter *f)
{
	pthread_mutex_lock(&f->lock);
	f->sh.rendered = f->rendered;
	f->sh.skipped = f->skipped;
	f->sh.info.neutral = f->last_neutral;
	f->sh.info.bloom = f->last_bloom;
	pthread_mutex_unlock(&f->lock);
}

static void fx_render(void *data, gs_effect_t *effect)
{
	(void)effect;
	struct fx_filter *f = data;
	obs_source_t *target = obs_filter_get_target(f->self);
	obs_source_t *parent = obs_filter_get_parent(f->self);
	if (!target || !parent || f->gpu_failed) {
		obs_source_skip_video_filter(f->self);
		return;
	}
	collect_timers(f);

	f->lut_tex = asset_get(&f->luts, f->anim.cur.lut, f->anim.to.lut);
	f->leak_tex = asset_get(&f->leaks, f->anim.cur.leak, f->anim.to.leak);

	float pulses[STAGE_PULSE_COUNT];
	stfx_anim_pulse_values(&f->anim, pulses);
	struct stfx_uniforms u;
	stfx_uniforms_compute(&f->anim.cur, pulses, f->paused_mask, f->lut_tex != NULL,
			      f->leak_tex != NULL, &u);
	if (!f->bloom_fx)
		u.bloom = 0.0f;
	f->last_parts = stfx_uniforms_parts(&u);

	const uint32_t w = obs_source_get_base_width(target);
	const uint32_t h = obs_source_get_base_height(target);
	const enum gs_color_space space = stfx_target_space(target);
	const bool hdr = stfx_space_is_hdr(space);
	if (hdr && !f->hdr_warned) {
		SLOG_WARN("fx '%s': HDR (scRGB) input is passed through unchanged", filter_name(f));
		f->hdr_warned = true;
	}
	if (stfx_uniforms_neutral(&u) || !w || !h || hdr) {
		/* Neutral: zero cost, bit-identical. */
		f->skipped++;
		f->last_neutral = true;
		f->last_bloom = false;
		publish_render(f);
		obs_source_skip_video_filter(f->self);
		return;
	}
	f->last_neutral = false;
	f->rendered++;

	f->last_bloom = u.bloom > 0.0f;
	if (!f->last_bloom || !render_with_bloom(f, space, w, h, &u)) {
		/* No bloom, or the bloom chain is unavailable this frame. */
		f->last_bloom = false;
		u.bloom = 0.0f;
		render_direct(f, space, w, h, &u);
	}
	publish_render(f);
}

static enum gs_color_space fx_color_space(void *data, size_t count,
					  const enum gs_color_space *preferred_spaces)
{
	(void)count;
	(void)preferred_spaces;
	struct fx_filter *f = data;
	obs_source_t *target = obs_filter_get_target(f->self);
	return target ? stfx_target_space(target) : GS_CS_SRGB;
}

/* ------------------------------------------------------------------------- */
/* Properties (UI thread)                                                    */
/* ------------------------------------------------------------------------- */

static bool settings_modified_vs_preset(obs_data_t *settings, char name_out[STFX_NAME_MAX])
{
	const char *id = obs_data_get_string(settings, KEY_PRESET);
	struct stfx_preset_info pi;
	if (!stfx_preset_get(id, &pi)) {
		if (name_out)
			stage_strlcpy(name_out, id, STFX_NAME_MAX);
		return false;
	}
	if (name_out)
		stage_strlcpy(name_out, pi.name, STFX_NAME_MAX);
	struct stfx_params p;
	settings_to_params(settings, &p);
	return !stfx_params_equal(&p, &pi.params);
}

static void fill_preset_list(struct fx_filter *f, obs_property_t *list, obs_data_t *settings)
{
	obs_property_list_clear(list);
	const char *cur = obs_data_get_string(settings, KEY_PRESET);
	const bool modified = settings_modified_vs_preset(settings, NULL);
	bool found = false;
	struct stfx_preset_info pi;
	for (size_t i = 0; stfx_preset_at(i, &pi); i++) {
		char label[STFX_NAME_MAX + 64];
		if (!strcmp(pi.id, cur)) {
			found = true;
			if (modified) {
				stfx_format1(label, sizeof(label), obs_module_text("Stage.List.Modified"), pi.name);
				obs_property_list_add_string(list, label, pi.id);
				continue;
			}
		}
		obs_property_list_add_string(list, pi.name, pi.id);
	}
	if (!found && cur[0])
		obs_property_list_add_string(list, cur, cur); /* removed user preset */
	f->modified_shown = modified;
}

static bool on_value_modified(void *priv, obs_properties_t *props, obs_property_t *p,
			      obs_data_t *settings)
{
	(void)p;
	struct fx_filter *f = priv;
	if (settings_modified_vs_preset(settings, NULL) == f->modified_shown)
		return false;
	obs_property_t *list = obs_properties_get(props, KEY_PRESET);
	if (list)
		fill_preset_list(f, list, settings);
	return true;
}

static void copy_preset_to(obs_data_t *settings, const char *id)
{
	struct stfx_preset_info pi;
	if (!stfx_preset_get(id, &pi))
		return;
	params_to_settings(settings, &pi.params, false);
	obs_data_set_string(settings, KEY_PRESET_APPLIED, id);
}

static bool on_preset_modified(void *priv, obs_properties_t *props, obs_property_t *p,
			       obs_data_t *settings)
{
	struct fx_filter *f = priv;
	const char *id = obs_data_get_string(settings, KEY_PRESET);
	const char *applied = obs_data_get_string(settings, KEY_PRESET_APPLIED);
	/* Also called for every property when the panel opens: only a newly
	 * picked look overwrites the sliders. */
	if (!strcmp(id, applied))
		return false;
	copy_preset_to(settings, id);
	fill_preset_list(f, p, settings);
	(void)props;
	return true;
}

static bool on_fx_reset(obs_properties_t *props, obs_property_t *p, void *data)
{
	(void)p;
	struct fx_filter *f = data;
	obs_data_t *settings = obs_source_get_settings(f->self);
	copy_preset_to(settings, obs_data_get_string(settings, KEY_PRESET));
	obs_property_t *list = obs_properties_get(props, KEY_PRESET);
	if (list)
		fill_preset_list(f, list, settings);
	obs_data_release(settings);
	obs_source_update(f->self, NULL);
	return true;
}

static bool on_pulse_button(struct fx_filter *f, int effect)
{
	os_atomic_set_bool(&f->pending_pulse[effect], true);
	return false;
}

static bool on_pulse_flash(obs_properties_t *props, obs_property_t *p, void *data)
{
	(void)props;
	(void)p;
	return on_pulse_button(data, STAGE_PULSE_FLASH);
}

static bool on_pulse_chroma(obs_properties_t *props, obs_property_t *p, void *data)
{
	(void)props;
	(void)p;
	return on_pulse_button(data, STAGE_PULSE_CHROMA_KICK);
}

static bool on_pulse_vignette(obs_properties_t *props, obs_property_t *p, void *data)
{
	(void)props;
	(void)p;
	return on_pulse_button(data, STAGE_PULSE_VIGNETTE_PULSE);
}

static bool on_open_lut_folder(obs_properties_t *props, obs_property_t *p, void *data)
{
	(void)props;
	(void)p;
	(void)data;
	char *dir = stfx_user_dir("luts");
	stfx_open_folder(dir);
	bfree(dir);
	return false;
}

static obs_property_t *add_num(struct fx_filter *f, obs_properties_t *props, int i,
			       const char *label_key, double step, const char *suffix)
{
	const struct stfx_num_def *d = &stfx_num_defs[i];
	obs_property_t *p = obs_properties_add_float_slider(props, d->key, T(label_key),
							     d->lo * d->scale, d->hi * d->scale, step);
	obs_property_float_set_suffix(p, suffix);
	stfx_set_desc(p, label_key);
	obs_property_set_modified_callback2(p, on_value_modified, f);
	return p;
}

static obs_property_t *add_asset_list(struct fx_filter *f, obs_properties_t *props, const char *key,
				      const char *label_key, int kind)
{
	obs_property_t *p = obs_properties_add_list(props, key, T(label_key), OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Stage.List.None"), "");
	char id[STAGE_ID_MAX];
	for (size_t i = 0; stfx_asset_at(kind, i, id, NULL); i++)
		obs_property_list_add_string(p, id, id);
	stfx_set_desc(p, label_key);
	obs_property_set_modified_callback2(p, on_value_modified, f);
	return p;
}

static obs_properties_t *add_group(struct fx_filter *f, obs_properties_t *props, int g,
				   const char *label_key)
{
	obs_properties_t *grp = obs_properties_create();
	const char *key = stfx_group_defs[g].enabled_key;
	obs_property_t *p = obs_properties_add_group(props, key ? key : "grp_colour", T(label_key),
						     key ? OBS_GROUP_CHECKABLE : OBS_GROUP_NORMAL, grp);
	if (key)
		obs_property_set_modified_callback2(p, on_value_modified, f);
	return grp;
}

/* Status line (stage-plugin-ux.md section 6): GPU fallback > perf pause > flash cap. */
static void status_text(struct fx_filter *f, char *out, size_t cap, bool *error)
{
	struct fx_shared s;
	pthread_mutex_lock(&f->lock);
	s = f->sh;
	pthread_mutex_unlock(&f->lock);
	*error = false;
	out[0] = '\0';
	if (s.gpu_failed) {
		stage_strlcpy(out, obs_module_text("Stage.GpuFallback"), cap);
		*error = true;
	} else if (s.info.paused[0]) {
		static const char *const part_keys[STFX_PART_COUNT] = {
			"Stage.Fx.Bloom", "Stage.Fx.Lut", "Stage.Fx.Leak", "Stage.Fx.Grain",
			"Stage.Fx.Chroma"};
		const char *part = s.info.paused;
		for (int i = 0; i < STFX_PART_COUNT; i++) {
			if (!strcmp(stfx_part_name(i), s.info.paused))
				part = T(part_keys[i]);
		}
		stfx_format1(out, cap, obs_module_text("Stage.PerfPaused"), part);
		*error = true;
	} else if (s.flash_cap_ms && stage_core_now_ms() - s.flash_cap_ms < FLASH_CAP_STATUS_MS) {
		stage_strlcpy(out, obs_module_text("Stage.FlashCap"), cap);
	}
}

static obs_properties_t *fx_properties(void *data)
{
	struct fx_filter *f = data;
	stfx_catalog_load(); /* pick up user presets, LUTs and leaks added since */
	obs_data_t *settings = obs_source_get_settings(f->self);
	obs_properties_t *props = obs_properties_create();

	obs_property_t *p = obs_properties_add_list(props, KEY_PRESET, obs_module_text("Stage.Prop.FxPreset"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	stfx_set_desc(p, "Stage.Prop.FxPreset");
	fill_preset_list(f, p, settings);
	obs_property_set_modified_callback2(p, on_preset_modified, f);
	obs_properties_add_button2(props, "fx_reset", obs_module_text("Stage.Button.FxReset"), on_fx_reset, f);

	char status[512];
	bool error;
	status_text(f, status, sizeof(status), &error);
	obs_data_set_default_string(settings, KEY_STATUS, status);
	p = obs_properties_add_text(props, KEY_STATUS, obs_module_text("Stage.Prop.Status"), OBS_TEXT_INFO);
	obs_property_text_set_info_type(p, error ? OBS_TEXT_INFO_WARNING : OBS_TEXT_INFO_NORMAL);
	obs_property_set_visible(p, status[0] != '\0');

	obs_properties_t *g = add_group(f, props, STFX_GROUP_COLOUR, "Stage.Group.Colour");
	add_num(f, g, STFX_EXPOSURE, "Stage.Prop.Exposure", 0.05, " EV");
	add_num(f, g, STFX_CONTRAST, "Stage.Prop.Contrast", 1.0, " %");
	add_num(f, g, STFX_SATURATION, "Stage.Prop.Saturation", 1.0, " %");
	add_num(f, g, STFX_TEMPERATURE, "Stage.Prop.Temperature", 1.0, " %");
	add_num(f, g, STFX_TINT, "Stage.Prop.Tint", 1.0, " %");

	g = add_group(f, props, STFX_GROUP_LUT, "Stage.Group.Lut");
	add_asset_list(f, g, STFX_KEY_LUT, "Stage.Prop.Lut", STFX_ASSET_LUT);
	add_num(f, g, STFX_LUT_AMOUNT, "Stage.Prop.LutAmount", 1.0, " %");
	char *lut_dir = stfx_user_dir("luts");
	if (lut_dir) {
		/* A plain button: OBS opens OBS_BUTTON_URL buttons only for http(s),
		 * so a file:/// URL would do nothing. The path is shown below it. */
		p = obs_properties_add_button2(g, "lut_folder", obs_module_text("Stage.Button.OpenLutFolder"),
					       on_open_lut_folder, f);
		obs_property_set_long_description(p, lut_dir);
		obs_data_set_default_string(settings, KEY_LUT_DIR, lut_dir);
		obs_properties_add_text(g, KEY_LUT_DIR, "", OBS_TEXT_INFO);
		bfree(lut_dir);
	}

	g = add_group(f, props, STFX_GROUP_VIGNETTE, "Stage.Group.Vignette");
	add_num(f, g, STFX_VIGNETTE, "Stage.Prop.VignetteAmount", 1.0, " %");
	add_num(f, g, STFX_VIGNETTE_SOFT, "Stage.Prop.VignetteSoft", 1.0, " %");
	add_num(f, g, STFX_VIGNETTE_ROUND, "Stage.Prop.VignetteRound", 1.0, " %");

	g = add_group(f, props, STFX_GROUP_BLOOM, "Stage.Group.Bloom");
	obs_data_set_default_string(settings, KEY_BLOOM_INFO, obs_module_text("Stage.Info.BloomCost"));
	obs_properties_add_text(g, KEY_BLOOM_INFO, "", OBS_TEXT_INFO);
	add_num(f, g, STFX_BLOOM, "Stage.Prop.BloomAmount", 1.0, " %");
	add_num(f, g, STFX_BLOOM_THRESHOLD, "Stage.Prop.BloomThreshold", 1.0, " %");

	g = add_group(f, props, STFX_GROUP_CHROMA, "Stage.Group.Chroma");
	add_num(f, g, STFX_CHROMA, "Stage.Prop.ChromaAmount", 1.0, " %");

	g = add_group(f, props, STFX_GROUP_GRAIN, "Stage.Group.Grain");
	add_num(f, g, STFX_GRAIN, "Stage.Prop.GrainAmount", 1.0, " %");
	add_num(f, g, STFX_GRAIN_SIZE, "Stage.Prop.GrainSize", 0.1, "x");

	g = add_group(f, props, STFX_GROUP_LEAK, "Stage.Group.Leak");
	add_asset_list(f, g, STFX_KEY_LEAK, "Stage.Prop.Leak", STFX_ASSET_LEAK);
	add_num(f, g, STFX_LEAK_AMOUNT, "Stage.Prop.LeakAmount", 1.0, " %");
	add_num(f, g, STFX_LEAK_SPEED, "Stage.Prop.LeakSpeed", 1.0, " %");

	g = obs_properties_create();
	obs_properties_add_button2(g, "pulse_flash", obs_module_text("Stage.Button.PulseFlash"), on_pulse_flash, f);
	obs_properties_add_button2(g, "pulse_chroma_kick", obs_module_text("Stage.Button.PulseChroma"),
				   on_pulse_chroma, f);
	obs_properties_add_button2(g, "pulse_vignette", obs_module_text("Stage.Button.PulseVignette"),
				   on_pulse_vignette, f);
	obs_properties_add_group(props, "grp_pulse", obs_module_text("Stage.Group.Pulse"), OBS_GROUP_NORMAL, g);

	obs_data_release(settings);
	return props;
}

/* ------------------------------------------------------------------------- */
/* Registration                                                              */
/* ------------------------------------------------------------------------- */

void stfx_register_fx_filter(void)
{
	struct obs_source_info info = {
		.id = STAGE_FILTER_FX,
		.type = OBS_SOURCE_TYPE_FILTER,
		.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
		.get_name = fx_get_name,
		.create = fx_create,
		.destroy = fx_destroy,
		.update = fx_update,
		.get_defaults = fx_defaults,
		.get_properties = fx_properties,
		.video_tick = fx_tick,
		.video_render = fx_render,
		.video_get_color_space = fx_color_space,
	};
	obs_register_source(&info);
}
