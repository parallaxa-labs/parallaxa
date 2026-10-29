/*
 * stage-fx-internal.h — shared helpers of the Parallaxa FX module (src/fx):
 * preset / LUT / light-leak catalog, render helpers, GPU timers, locale.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef STAGE_FX_INTERNAL_H
#define STAGE_FX_INTERNAL_H

#include <obs-module.h>
#include <graphics/graphics.h>

#include "stage-core.h"
#include "stage-fx-params.h"
#include "stage-fx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Depth blur settings key (docs/ux/stage-plugin-ux.md section 3), not yet a
 * protocol constant: uuid of the scene whose rig drives a depth blur layer. */
#ifndef STAGE_DEPTH_BLUR_KEY_DRIVEN_BY
#define STAGE_DEPTH_BLUR_KEY_DRIVEN_BY "driven_by"
#endif

/* ------------------------------------------------------------------------- */
/* Catalog: FX presets (data/presets/fx), LUTs (luts), light leaks (leaks)   */
/* ------------------------------------------------------------------------- */

enum stfx_asset_kind { STFX_ASSET_LUT, STFX_ASSET_LEAK, STFX_ASSET_COUNT };

#define STFX_NAME_MAX 260 /* 64 code points x 4 bytes + NUL */

struct stfx_preset_info {
	char id[STAGE_ID_MAX];
	char name[STFX_NAME_MAX]; /* in the OBS UI language */
	bool user;                /* from the user config folder */
	bool pro;
	struct stfx_params params; /* absolute: absent params are neutral */
};

/* (Re)scans the built-in folder (<module data>/presets) and the user folder
 * (obs_module_config_path). Cheap (a few small files); UI thread or module
 * load. Thread-safe against the getters. */
void stfx_catalog_load(void);
void stfx_catalog_free(void);
/* Changes on every stfx_catalog_load (any thread): a texture cache retries
 * an asset that was missing or failed to load. */
long stfx_catalog_generation(void);

size_t stfx_preset_count(void);
bool stfx_preset_at(size_t i, struct stfx_preset_info *out);
bool stfx_preset_get(const char *id, struct stfx_preset_info *out);

size_t stfx_asset_count(int kind);
bool stfx_asset_at(int kind, size_t i, char id[STAGE_ID_MAX], bool *user);
/* Full path of asset id; false when unknown. */
bool stfx_asset_path(int kind, const char *id, char *path, size_t cap);
/* User folder of a kind ("fx", "luts", "leaks"); bfree() the result. NULL
 * when OBS has no module config path. */
char *stfx_user_dir(const char *kind);

/* ------------------------------------------------------------------------- */
/* Rendering helpers (graphics thread)                                        */
/* ------------------------------------------------------------------------- */

/* Color space the target renders in: SRGB, SRGB_16F, 709_EXTENDED, or a
 * source's own 709_SCRGB (not remapped by libobs). See stfx_space_is_hdr. */
enum gs_color_space stfx_target_space(obs_source_t *target);

/* Renders the filter's target into tr (recreated when its format does not
 * match the space), exactly like obs_source_process_filter_begin does. */
bool stfx_render_target(obs_source_t *filter, gs_texrender_t **tr, enum gs_color_space space,
			uint32_t w, uint32_t h);

/* Params of a Kawase-style pass. gs_technique_end() clears every param of
 * an effect, so stfx_pass sets these again for each pass. */
struct stfx_pass_params {
	gs_eparam_t *image;  /* texture2d: src                       */
	gs_eparam_t *texel;  /* float2: 1 / src size (may be NULL)    */
	gs_eparam_t *offset; /* float: tap distance (may be NULL)     */
};

/* One full-screen pass of technique `tech` into tr (RGBA16F, blending off)
 * reading src (through its sRGB view when srgb). Any other param of the
 * technique must be set by the caller right before. */
bool stfx_pass(gs_texrender_t *tr, uint32_t w, uint32_t h, gs_effect_t *fx, const char *tech,
	       const struct stfx_pass_params *pp, gs_texture_t *src, bool srgb, float offset);

/* Loads an effect from data/effects (inside obs_enter_graphics). Logs the
 * compiler output on failure. */
gs_effect_t *stfx_load_effect(const char *file, bool required);

/* GPU timers (gs_timer + gs_timer_range), sampled: one measurement every
 * STFX_TIMER_PERIOD frames, read back STFX_TIMER_MIN_AGE frames later.
 *
 * Why: on D3D11 gs_timer_get_data / gs_timer_range_get_data call GetData
 * with flags 0 (a flush) and spin while the query is not finished, so
 * reading a query issued in the current frame (a second render of the
 * filter in the same frame: projectors, multiview, studio mode) or while
 * the GPU runs a frame behind (a game) would stall the graphics thread.
 * A query several frames old is finished long before it is read. */
#define STFX_TIMER_SLOTS 4
#define STFX_TIMER_PERIOD 4  /* frames between two measurements */
#define STFX_TIMER_MIN_AGE 3 /* frames before a query is read   */
struct stfx_timers {
	gs_timer_range_t *range[STFX_TIMER_SLOTS];
	gs_timer_t *timer[STFX_TIMER_SLOTS]; /* begin .. end              */
	gs_timer_t *tail[STFX_TIMER_SLOTS];  /* split .. end (final pass) */
	bool pending[STFX_TIMER_SLOTS];
	bool split[STFX_TIMER_SLOTS];
	uint64_t issued[STFX_TIMER_SLOTS]; /* frame of the measurement */
	uint64_t frame;                    /* stfx_timers_tick count   */
	uint64_t last_begin;               /* frame of the last begin  */
	bool begun_once;
	bool available;
};
void stfx_timers_init(struct stfx_timers *t);  /* graphics context */
void stfx_timers_free(struct stfx_timers *t);  /* graphics context */
/* Once per video_tick: advances the frame counter. */
void stfx_timers_tick(struct stfx_timers *t);
/* Begins a measurement when one is due; returns the slot or -1 (not due,
 * no free slot or no timers). Only the first render of a sampled frame is
 * measured. */
int stfx_timers_begin(struct stfx_timers *t);
/* Optional, between begin and end: marks the start of the final pass, so
 * the measurement also reports that pass alone (tail). */
void stfx_timers_split(struct stfx_timers *t, int slot);
void stfx_timers_end(struct stfx_timers *t, int slot);
/* Collects measurements (ms) at least STFX_TIMER_MIN_AGE frames old into
 * out, and their tail (split .. end; = out without a split) into tail_out
 * (may be NULL); returns their count. Never reads a query of the current
 * frame. */
int stfx_timers_poll(struct stfx_timers *t, double *out, double *tail_out, int cap);

/* HDR (scRGB / extended 709) spaces: the FX filters pass them through. */
bool stfx_space_is_hdr(enum gs_color_space space);

/* Image loader: decodes a PNG on a worker thread so the graphics thread
 * never waits for disk I/O; the texture upload stays on the graphics thread.
 * Refcounted: release it at any time, even before the decode is done. */
struct stfx_image;
struct stfx_image *stfx_image_load(const char *path); /* any thread */
bool stfx_image_done(const struct stfx_image *im);
/* New texture from the decoded pixels (graphics context); NULL while not
 * done or when the decode failed. cx / cy (may be NULL) get the size. */
gs_texture_t *stfx_image_texture(struct stfx_image *im, uint32_t *cx, uint32_t *cy);
const char *stfx_image_path(const struct stfx_image *im);
void stfx_image_release(struct stfx_image *im);

/* ------------------------------------------------------------------------- */
/* Misc                                                                      */
/* ------------------------------------------------------------------------- */

/* Creates dir if needed and opens it in the file manager (UI thread).
 * Windows only; elsewhere it logs the path and returns false. */
bool stfx_open_folder(const char *dir);
/* Sets the tooltip of p from "<label_key>.Desc" when that key exists. */
void stfx_set_desc(obs_property_t *p, const char *label_key);
/* out = fmt with the first "%1" replaced by arg. */
void stfx_format1(char *out, size_t cap, const char *fmt, const char *arg);

/* Perf events for the vendor (stage-fx.h). */
void stfx_perf_push(const struct stage_fx_perf_event *e);
void stfx_perf_clear(void);

/* Settings writes queued to the UI thread by Parallaxa FX (fx_set, panic) that
 * have not run yet: released on module unload. */
void stfx_settings_writes_free(void);

/* Filter registration. */
void stfx_register_fx_filter(void);
void stfx_register_depth_blur(void);

#ifdef __cplusplus
}
#endif

#endif /* STAGE_FX_INTERNAL_H */
