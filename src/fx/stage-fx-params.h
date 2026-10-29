/*
 * stage-fx-params.h — Parallaxa FX parameter model (pure C, no libobs).
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Everything here is libobs-free so it can be unit tested standalone
 * (tests/fx/fx_params_test.c):
 *
 *   - the fx_set parameter table with the protocol <-> settings conversion
 *     of docs/ux/stage-plugin-ux.md section 2.1 ({param, settings_key, scale},
 *     used in both directions),
 *   - struct stfx_params (one complete FX state in protocol units),
 *   - the animator: fx_set tweens (stage_ease), fx_pulse envelopes and the
 *     photosensitivity flash cap (STAGE_FLASH_MAX_PER_SEC, always on),
 *   - the shader uniform computation (including perf-guard pauses),
 *   - the performance guard (STAGE_PERF_BUDGET_MS for STAGE_PERF_WINDOW_S).
 *
 * Symbols use the stfx_ prefix: the public stage_fx_param* names belong to
 * the presets module.
 */
#ifndef STAGE_FX_PARAMS_H
#define STAGE_FX_PARAMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stage-protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Parameter table                                                           */
/* ------------------------------------------------------------------------- */

/* Numeric fx_set parameters (vendor-api.md 4.7), in table order. */
enum stfx_num {
	STFX_EXPOSURE,
	STFX_CONTRAST,
	STFX_SATURATION,
	STFX_TEMPERATURE,
	STFX_TINT,
	STFX_VIGNETTE,
	STFX_VIGNETTE_SOFT,
	STFX_VIGNETTE_ROUND,
	STFX_CHROMA,
	STFX_GRAIN,
	STFX_GRAIN_SIZE,
	STFX_LUT_AMOUNT,
	STFX_LEAK_AMOUNT,
	STFX_LEAK_SPEED,
	STFX_BLOOM,
	STFX_BLOOM_THRESHOLD,
	STFX_NUM_COUNT
};

/* Property groups. Every group except COLOUR is a checkable group whose bool
 * settings key is enabled_key; an unticked group renders its params at their
 * neutral values (stage-plugin-ux.md section 2). */
enum stfx_group {
	STFX_GROUP_COLOUR,
	STFX_GROUP_LUT,
	STFX_GROUP_VIGNETTE,
	STFX_GROUP_BLOOM,
	STFX_GROUP_CHROMA,
	STFX_GROUP_GRAIN,
	STFX_GROUP_LEAK,
	STFX_GROUP_COUNT
};

struct stfx_num_def {
	const char *name; /* protocol param name (fx_set, preset files) */
	const char *key;  /* filter settings key                       */
	double scale;     /* setting = param * scale                   */
	double lo, hi;    /* protocol range                            */
	double neutral;   /* protocol neutral value                    */
	uint8_t group;    /* enum stfx_group                           */
};

struct stfx_group_def {
	const char *enabled_key; /* NULL for COLOUR (not checkable)      */
	int amount;              /* enum stfx_num ticking the group, -1  */
};

extern const struct stfx_num_def stfx_num_defs[STFX_NUM_COUNT];
extern const struct stfx_group_def stfx_group_defs[STFX_GROUP_COUNT];

/* Id parameters (not in the numeric table). */
#define STFX_PARAM_LUT "lut"
#define STFX_PARAM_LEAK "leak"
#define STFX_KEY_LUT "lut"
#define STFX_KEY_LEAK "leak"

/* Index of a numeric param by protocol name, or -1. */
int stfx_num_find(const char *name);
/* Clamps v into the protocol range of param i (NaN -> neutral). */
double stfx_num_clamp(int i, double v);
/* Protocol <-> settings units (stage-plugin-ux.md 2.1). */
double stfx_setting_from_param(int i, double param);
double stfx_param_from_setting(int i, double setting);

/* ------------------------------------------------------------------------- */
/* FX state                                                                  */
/* ------------------------------------------------------------------------- */

struct stfx_params {
	float v[STFX_NUM_COUNT];
	char lut[STAGE_ID_MAX];  /* "" = none */
	char leak[STAGE_ID_MAX]; /* "" = none */
};

void stfx_params_neutral(struct stfx_params *p);
/* Canonical form for comparisons: a group whose amount is 0 (or whose id is
 * empty) has its other params reset to neutral, so two states that render
 * identically compare equal. */
void stfx_params_canon(struct stfx_params *p);
/* True when a and b render identically (canonical forms equal within eps). */
bool stfx_params_equal(const struct stfx_params *a, const struct stfx_params *b);
/* True when p renders as a pass-through (every amount 0, neutral grade). */
bool stfx_params_is_neutral(const struct stfx_params *p);

/* FX toggle hotkeys, in enum stage_fx_toggle order (stage-core.h): the
 * amount each flips and the amount it turns on at with nothing to restore. */
#define STFX_TOGGLE_COUNT 4
struct stfx_toggle_def {
	int num;        /* enum stfx_num */
	float fallback;
};
extern const struct stfx_toggle_def stfx_toggle_defs[STFX_TOGGLE_COUNT];

/* Flips toggle t of target p: an amount above 0 is remembered in
 * restore[t] and set to 0; 0 comes back at restore[t], else at
 * preset_amount (the look's own, when above 0), else at the fallback. The
 * FX tick applies queued toggles in order to the target the commands
 * before them left, so N presses within one tick end where N presses
 * across ticks would. */
void stfx_toggle_apply(struct stfx_params *p, int t, float restore[STFX_TOGGLE_COUNT],
		       float preset_amount);

/* ------------------------------------------------------------------------- */
/* Animator: tweens, pulses, flash cap                                       */
/* ------------------------------------------------------------------------- */

/* Pulse shaping (fraction of intensity added to the base value). */
#define STFX_PULSE_DURATION_DEFAULT 300 /* ms, vendor-api.md 4.8 */
#define STFX_FLASH_GAIN 0.75f            /* flash: mix toward white      */
#define STFX_CHROMA_KICK_GAIN 0.6f       /* chroma_kick: added to chroma */
#define STFX_VIGNETTE_PULSE_GAIN 0.45f   /* vignette_pulse: added        */
#define STFX_VIGNETTE_PULSE_ATTACK 0.15f /* rise share of the duration   */
#define STFX_FLASH_WINDOW_S 1.0

struct stfx_pulse {
	bool active;
	float peak;    /* value at the start of the decay       */
	double start;  /* anim clock (s) at the start           */
	float dur;     /* seconds                               */
	bool attack;   /* rises first (vignette_pulse)          */
};

struct stfx_anim {
	struct stfx_params from, to, cur;
	bool tweening;
	float t, dur; /* seconds */
	int ease;     /* enum stage_ease */

	struct stfx_pulse pulse[STAGE_PULSE_COUNT];
	double clock; /* seconds since init */

	/* Start times of the last STAGE_FLASH_MAX_PER_SEC flashes (ring). */
	double flash_start[STAGE_FLASH_MAX_PER_SEC];
	int flash_head;
	uint64_t flash_merged; /* pulses coalesced by the cap (total)   */
	double last_merge;     /* clock of the last merge, -1 = never   */
};

void stfx_anim_init(struct stfx_anim *a, const struct stfx_params *p);
/* Jumps to p at once (no tween); pulses keep running. */
void stfx_anim_jump(struct stfx_anim *a, const struct stfx_params *p);
/* Tweens from the current value to target over duration_ms with ease.
 * duration 0 jumps. LUT and leak id changes cross-fade: the old texture
 * fades out over the first half, the new one fades in over the second. */
void stfx_anim_set(struct stfx_anim *a, const struct stfx_params *target, uint32_t duration_ms,
		   int ease);
/* Starts pulse `effect` (enum stage_pulse). intensity 0..1, duration_ms is
 * clamped to STAGE_PULSE_DURATION_MIN..MAX. Returns false when a flash was
 * coalesced by the 3-per-second cap: it then extends the running flash's
 * decay instead of starting a new peak (no new flash edge). */
bool stfx_anim_pulse(struct stfx_anim *a, int effect, float intensity, uint32_t duration_ms);
void stfx_anim_cancel_pulses(struct stfx_anim *a);
/* Advances the clock by dt seconds (tween and pulse expiry). */
void stfx_anim_tick(struct stfx_anim *a, float dt);
/* Current envelope value of each pulse (0..1). */
void stfx_anim_pulse_values(const struct stfx_anim *a, float out[STAGE_PULSE_COUNT]);
bool stfx_anim_pulses_active(const struct stfx_anim *a);
/* Flashes started within the last STFX_FLASH_WINDOW_S seconds. */
int stfx_anim_flashes_in_window(const struct stfx_anim *a);

/* ------------------------------------------------------------------------- */
/* Shader uniforms                                                           */
/* ------------------------------------------------------------------------- */

/* Pausable parts (perf guard), in pause order: most expensive first. */
enum stfx_part {
	STFX_PART_BLOOM,
	STFX_PART_LUT,
	STFX_PART_LEAK,
	STFX_PART_GRAIN,
	STFX_PART_CHROMA,
	STFX_PART_COUNT
};
#define STFX_PART_BIT(p) (1u << (unsigned)(p))
/* STAGE_PERF_PAUSED_* name of a part. */
const char *stfx_part_name(int part);

struct stfx_uniforms {
	float exposure_mul;  /* 2^exposure                          */
	float wb_gain[3];    /* temperature/tint gains (linear)     */
	float contrast;      /* 1 + contrast                        */
	float saturation;    /* 1 + saturation                      */
	float lut_amount;    /* 0 when no LUT or paused             */
	float vignette;      /* amount incl. vignette_pulse         */
	float vignette_soft;
	float vignette_round;
	float chroma;        /* amount incl. chroma_kick            */
	float grain;
	float grain_size;
	float leak_amount;
	float leak_speed;
	float flash;         /* 0..STFX_FLASH_GAIN                  */
	float bloom;         /* 0 = no bloom passes                 */
	float bloom_threshold;
};

/* Computes the uniforms of state p with the pulse values (may be NULL) and
 * the parts in paused_mask (STFX_PART_BIT) forced off. has_lut / has_leak
 * say whether the textures are loaded (a missing texture counts as off). */
void stfx_uniforms_compute(const struct stfx_params *p, const float pulses[STAGE_PULSE_COUNT],
			   unsigned paused_mask, bool has_lut, bool has_leak,
			   struct stfx_uniforms *u);
/* True when u is a pass-through (the filter calls obs_source_skip_video_filter). */
bool stfx_uniforms_neutral(const struct stfx_uniforms *u);
/* STFX_PART_BIT mask of the parts u actually uses. */
unsigned stfx_uniforms_parts(const struct stfx_uniforms *u);

/* ------------------------------------------------------------------------- */
/* Performance guard                                                         */
/* ------------------------------------------------------------------------- */

/* Fed with GPU timer samples of the filter's own passes and the number of
 * renders and ticks; closes a bucket every second. When the per-frame cost
 * stays above budget_ms for window_s consecutive buckets it pauses the most
 * expensive active part.
 *
 * A render either runs the full chain (input + bloom + the final pass) or,
 * for a later render in the same frame (studio mode, projectors), reuses the
 * cached bloom and redoes only the final pass. Per-frame cost =
 *   mean(chain part) x full renders / ticks + mean(final pass) x renders / ticks
 * where chain part = sample - final pass of a measured full render. */
struct stfx_guard {
	double budget_ms;
	int window_s;
	double bucket_t;     /* seconds in the current bucket          */
	double sum_ms;       /* measured cost in the bucket            */
	double sum_pass_ms;  /* of which the per-render final pass     */
	uint32_t measured;   /* renders measured in the bucket         */
	uint32_t renders;    /* renders in the bucket                  */
	uint32_t full;       /* of which ran the full chain            */
	uint32_t ticks;      /* ticks (frames) in the bucket           */
	int over_s;          /* consecutive buckets over budget        */
	double last_cost_ms; /* per-frame cost of the last bucket      */
};

void stfx_guard_init(struct stfx_guard *g, double budget_ms, int window_s);
void stfx_guard_reset(struct stfx_guard *g);
/* One render of the filter's passes (measured or not); full: it ran the
 * whole chain, false: it reused this frame's cached bloom. */
void stfx_guard_render(struct stfx_guard *g, bool full);
/* A GPU timer sample of one earlier full render: ms in total, pass_ms of
 * which is the final pass that every render repeats (pass_ms = ms when the
 * render has no cached part). */
void stfx_guard_sample(struct stfx_guard *g, double ms, double pass_ms);
/*
 * One tick of dt seconds. active_mask: parts in use (stfx_uniforms_parts),
 * *paused_mask: parts already paused. Returns the enum stfx_part that was
 * just paused (and adds it to *paused_mask), or -1. *cost_ms receives the
 * per-frame cost of the bucket that triggered the pause.
 */
int stfx_guard_tick(struct stfx_guard *g, float dt, unsigned active_mask, unsigned *paused_mask,
		    double *cost_ms);

#ifdef __cplusplus
}
#endif

#endif /* STAGE_FX_PARAMS_H */
