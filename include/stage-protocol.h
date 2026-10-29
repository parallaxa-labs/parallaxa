/*
 * stage-protocol.h — Parallaxa shared protocol names, enums and limits.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The protocol itself (names, payloads, semantics) is documented under the
 * MIT license in docs/protocol/vendor-api.md; this header is the C mirror of
 * that document. Names here MUST stay identical to:
 *   - docs/protocol/vendor-api.md            (normative)
 *   - docs/protocol/names.vendor.json        (every vendor constant)
 *   - docs/protocol/vendor-tables.json       (enum orders and limits)
 * tests/contracts/protocol_check.mjs (ctest protocol_parity) checks that every
 * vendor constant appears here verbatim and that the tables and limits match.
 * Clients (e.g. AI Live Studio) keep their own mirrors of docs/protocol.
 *
 * Pure C17, no libobs includes. Safe to include from any translation unit,
 * including the libobs-free math library and unit tests.
 */
#ifndef STAGE_PROTOCOL_H
#define STAGE_PROTOCOL_H

#include <math.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Identity                                                                  */
/* ------------------------------------------------------------------------- */

/* obs-websocket vendor name (global namespace in obs-websocket, so branded). */
#define STAGE_VENDOR_NAME "parallaxa"
/* OBS module file name (parallaxa.dll) and locale/module id. */
#define STAGE_MODULE_NAME "parallaxa"
/* Vendor protocol version, returned by get_info as "protocol". */
#define STAGE_PROTOCOL_VERSION 1

/* OBS source ids registered by the plugin. */
#define STAGE_FILTER_CAMERA_RIG "stage_camera_rig" /* scene filter, zero passes   */
#define STAGE_FILTER_FX "stage_fx"                 /* scene/source uber FX filter */
#define STAGE_FILTER_DEPTH_BLUR "stage_depth_blur" /* per-source DoF blur filter  */

/* stage_depth_blur settings written by the rig (double, 0..1). */
#define STAGE_DEPTH_BLUR_KEY_AUTO_STRENGTH "auto_strength"

/* Scene-item private settings keys (obs_sceneitem_get_private_settings). */
#define STAGE_ITEM_KEY_DEPTH "stage.depth" /* double, STAGE_DEPTH_MIN..MAX       */
#define STAGE_ITEM_KEY_ROLE "stage.role"   /* string, stage_role_names[]          */
#define STAGE_ITEM_KEY_REST "stage.rest"   /* object {px,py,sx,sy,rot}            */
#define STAGE_REST_KEY_PX "px"
#define STAGE_REST_KEY_PY "py"
#define STAGE_REST_KEY_SX "sx"
#define STAGE_REST_KEY_SY "sy"
#define STAGE_REST_KEY_ROT "rot"
/* Scene private settings key holding saved views (array of view objects). */
#define STAGE_SCENE_KEY_VIEWS "stage.views"

/* ------------------------------------------------------------------------- */
/* Vendor requests (CallVendorRequest requestType)                           */
/* ------------------------------------------------------------------------- */

#define STAGE_REQ_GET_INFO "get_info"
#define STAGE_REQ_GET_STATE "get_state"
#define STAGE_REQ_CAMERA_MOVE "camera_move"
#define STAGE_REQ_CAMERA_SET "camera_set"
#define STAGE_REQ_CAMERA_RESET "camera_reset"
#define STAGE_REQ_CAMERA_AUTO "camera_auto"
#define STAGE_REQ_FX_SET "fx_set"
#define STAGE_REQ_FX_PULSE "fx_pulse"
#define STAGE_REQ_DOF_SET "dof_set"
#define STAGE_REQ_LAYER_LIST "layer_list"
#define STAGE_REQ_LAYER_SET "layer_set"
#define STAGE_REQ_MOVE_LIST "move_list"
#define STAGE_REQ_RECIPE_LIST "recipe_list"
#define STAGE_REQ_RECIPE_APPLY "recipe_apply"
#define STAGE_REQ_VIEW_SAVE "view_save"
#define STAGE_REQ_VIEW_APPLY "view_apply"
#define STAGE_REQ_VIEW_LIST "view_list"
#define STAGE_REQ_VIEW_DELETE "view_delete"
#define STAGE_REQ_FREEZE "freeze"
#define STAGE_REQ_PANIC "panic"

/* All request names, in documentation order (for registration loops). */
static const char *const stage_request_names[] = {
	STAGE_REQ_GET_INFO,     STAGE_REQ_GET_STATE,   STAGE_REQ_CAMERA_MOVE,
	STAGE_REQ_CAMERA_SET,   STAGE_REQ_CAMERA_RESET, STAGE_REQ_CAMERA_AUTO,
	STAGE_REQ_FX_SET,       STAGE_REQ_FX_PULSE,    STAGE_REQ_DOF_SET,
	STAGE_REQ_LAYER_LIST,   STAGE_REQ_LAYER_SET,   STAGE_REQ_MOVE_LIST,
	STAGE_REQ_RECIPE_LIST,  STAGE_REQ_RECIPE_APPLY, STAGE_REQ_VIEW_SAVE,
	STAGE_REQ_VIEW_APPLY,   STAGE_REQ_VIEW_LIST,   STAGE_REQ_VIEW_DELETE,
	STAGE_REQ_FREEZE,       STAGE_REQ_PANIC,
};
#define STAGE_REQUEST_COUNT (sizeof(stage_request_names) / sizeof(stage_request_names[0]))

/* ------------------------------------------------------------------------- */
/* Vendor events (VendorEvent eventType)                                     */
/* ------------------------------------------------------------------------- */

#define STAGE_EVT_STATE_CHANGED "state_changed"   /* <= 10 Hz, only on change  */
#define STAGE_EVT_MOVE_FINISHED "move_finished"
#define STAGE_EVT_RIG_CHANGED "rig_changed"
#define STAGE_EVT_PERF_WARNING "perf_warning"
#define STAGE_EVT_HOTKEY_FORWARD "hotkey_forward" /* "AI Live Studio: ..." keys */

/* Maximum state_changed rate. */
#define STAGE_STATE_EVENT_MAX_HZ 10

/* ------------------------------------------------------------------------- */
/* Error codes (responseData.code when ok == false)                          */
/* ------------------------------------------------------------------------- */

#define STAGE_ERR_BAD_PARAM "bad_param" /* wrong type or out of range        */
#define STAGE_ERR_NO_RIG "no_rig"       /* scene has no (active) Camera Rig  */
#define STAGE_ERR_NO_FX "no_fx"         /* target has no Parallaxa FX filter */
#define STAGE_ERR_NOT_FOUND "not_found" /* unknown move/recipe/preset/view/item/scene */
#define STAGE_ERR_BUSY "busy"           /* command queue full                */
#define STAGE_ERR_INTERNAL "internal"

enum stage_error {
	STAGE_ERROR_BAD_PARAM,
	STAGE_ERROR_NO_RIG,
	STAGE_ERROR_NO_FX,
	STAGE_ERROR_NOT_FOUND,
	STAGE_ERROR_BUSY,
	STAGE_ERROR_INTERNAL,
	STAGE_ERROR_COUNT
};

static const char *const stage_error_names[STAGE_ERROR_COUNT] = {
	STAGE_ERR_BAD_PARAM, STAGE_ERR_NO_RIG, STAGE_ERR_NO_FX,
	STAGE_ERR_NOT_FOUND, STAGE_ERR_BUSY,   STAGE_ERR_INTERNAL,
};

/* get_info "features" strings. */
#define STAGE_FEATURE_CAMERA "camera"
#define STAGE_FEATURE_FX "fx"
#define STAGE_FEATURE_AUDIO "audio"
#define STAGE_FEATURE_DOF "dof"
#define STAGE_FEATURE_VIEWS "views"

/* ------------------------------------------------------------------------- */
/* Enums with string tables. The enum value is the table index.              */
/* ------------------------------------------------------------------------- */

/* Built-in camera moves. Custom move ids (data/presets/moves) are strings. */
enum stage_move {
	STAGE_MOVE_ZOOM_IN,
	STAGE_MOVE_ZOOM_OUT,
	STAGE_MOVE_PAN_LEFT,
	STAGE_MOVE_PAN_RIGHT,
	STAGE_MOVE_SHAKE,
	STAGE_MOVE_PUNCH,
	STAGE_MOVE_DOLLY,
	STAGE_MOVE_NOD,
	STAGE_MOVE_RESET,
	STAGE_MOVE_COUNT
};

static const char *const stage_move_names[STAGE_MOVE_COUNT] = {
	"zoom_in", "zoom_out", "pan_left", "pan_right", "shake",
	"punch",   "dolly",    "nod",      "reset",
};

/* Easing curves (math in src/math/stage-ease.c). */
enum stage_ease {
	STAGE_EASE_LINEAR,
	STAGE_EASE_IN_QUAD,
	STAGE_EASE_OUT_QUAD,
	STAGE_EASE_IN_OUT_QUAD,
	STAGE_EASE_IN_CUBIC,
	STAGE_EASE_OUT_CUBIC,
	STAGE_EASE_IN_OUT_CUBIC,
	STAGE_EASE_IN_QUINT,
	STAGE_EASE_OUT_QUINT,
	STAGE_EASE_IN_OUT_QUINT,
	STAGE_EASE_IN_BACK,
	STAGE_EASE_OUT_BACK,
	STAGE_EASE_IN_OUT_BACK,
	STAGE_EASE_IN_ELASTIC,
	STAGE_EASE_OUT_ELASTIC,
	STAGE_EASE_IN_OUT_ELASTIC,
	STAGE_EASE_SMOOTHSTEP,
	STAGE_EASE_COUNT
};

static const char *const stage_ease_names[STAGE_EASE_COUNT] = {
	"linear",
	"ease_in_quad",    "ease_out_quad",    "ease_in_out_quad",
	"ease_in_cubic",   "ease_out_cubic",   "ease_in_out_cubic",
	"ease_in_quint",   "ease_out_quint",   "ease_in_out_quint",
	"ease_in_back",    "ease_out_back",    "ease_in_out_back",
	"ease_in_elastic", "ease_out_elastic", "ease_in_out_elastic",
	"smoothstep",
};

/* Default ease when a request omits "ease". */
#define STAGE_EASE_DEFAULT STAGE_EASE_IN_OUT_CUBIC

/* Auto camera modes. UX labels: Off / Gentle / Talk-reactive / Music-reactive. */
enum stage_auto_mode {
	STAGE_AUTO_OFF,
	STAGE_AUTO_DRIFT,
	STAGE_AUTO_CHATTING,
	STAGE_AUTO_SINGING,
	STAGE_AUTO_COUNT
};

static const char *const stage_auto_mode_names[STAGE_AUTO_COUNT] = {
	"off", "drift", "chatting", "singing",
};

/*
 * Layer roles.
 *   none   : not enrolled, never touched by the rig.
 *   bg/char/fg : parallax with f = 1/depth (continuous over [0.1, 10]).
 *   sky    : infinitely far backdrop, f = 0 (never moves); DoF-eligible.
 *   static : pinned to screen (frames, HUD), f = 0; never blurred/oversized.
 * Depth blur (stage_depth_blur) is driven only for bg, fg and sky.
 */
enum stage_role {
	STAGE_ROLE_NONE,
	STAGE_ROLE_BG,
	STAGE_ROLE_CHAR,
	STAGE_ROLE_FG,
	STAGE_ROLE_SKY,
	STAGE_ROLE_STATIC,
	STAGE_ROLE_COUNT
};

static const char *const stage_role_names[STAGE_ROLE_COUNT] = {
	"none", "bg", "char", "fg", "sky", "static",
};

/* View transitions (UX "Transition curve": Cut / Smooth / Whip / Punch).
 * cut = duration 0; smooth = ease_in_out_cubic; whip = ease_in_out_quint;
 * punch = ease_out_back. */
enum stage_transition {
	STAGE_TRANSITION_CUT,
	STAGE_TRANSITION_SMOOTH,
	STAGE_TRANSITION_WHIP,
	STAGE_TRANSITION_PUNCH,
	STAGE_TRANSITION_COUNT
};

static const char *const stage_transition_names[STAGE_TRANSITION_COUNT] = {
	"cut", "smooth", "whip", "punch",
};

/* Ease used by each transition (index = enum stage_transition). */
static const unsigned char stage_transition_ease[STAGE_TRANSITION_COUNT] = {
	STAGE_EASE_LINEAR, STAGE_EASE_IN_OUT_CUBIC, STAGE_EASE_IN_OUT_QUINT,
	STAGE_EASE_OUT_BACK,
};

/* fx_pulse effects. */
enum stage_pulse {
	STAGE_PULSE_FLASH,
	STAGE_PULSE_CHROMA_KICK,
	STAGE_PULSE_VIGNETTE_PULSE,
	STAGE_PULSE_COUNT
};

static const char *const stage_pulse_names[STAGE_PULSE_COUNT] = {
	"flash", "chroma_kick", "vignette_pulse",
};

/* hotkey_forward actions ("AI Live Studio: Stop / Skip / Mute AI / Panic / Approve"). */
enum stage_hotkey_action {
	STAGE_HOTKEY_AI_STOP,
	STAGE_HOTKEY_AI_SKIP,
	STAGE_HOTKEY_AI_MUTE,
	STAGE_HOTKEY_AI_PANIC,
	STAGE_HOTKEY_AI_APPROVE,
	STAGE_HOTKEY_COUNT
};

static const char *const stage_hotkey_action_names[STAGE_HOTKEY_COUNT] = {
	"ai_stop", "ai_skip", "ai_mute", "ai_panic", "ai_approve",
};

/* perf_warning "paused" values: the FX part that was paused ("" = none). */
#define STAGE_PERF_PAUSED_BLOOM "bloom"
#define STAGE_PERF_PAUSED_LUT "lut"
#define STAGE_PERF_PAUSED_LEAK "leak"
#define STAGE_PERF_PAUSED_GRAIN "grain"
#define STAGE_PERF_PAUSED_CHROMA "chroma"

/* Move keyframe channel mask bits (struct stage_key.mask). A channel present
 * in a JSON key sets its bit; absent channels are interpolated through. */
#define STAGE_CH_X 0x01u
#define STAGE_CH_Y 0x02u
#define STAGE_CH_ZOOM 0x04u
#define STAGE_CH_ROT 0x08u
#define STAGE_CH_SHAKE 0x10u
#define STAGE_CH_ALL 0x1Fu

/* ------------------------------------------------------------------------- */
/* Limits (requests outside these ranges get bad_param, never clamping,      */
/* except where vendor-api.md says "clamped").                               */
/* ------------------------------------------------------------------------- */

/* Depth: character plane 1.0; bg > 1; fg < 1. f = 1/depth. */
#define STAGE_DEPTH_MIN 0.1
#define STAGE_DEPTH_MAX 10.0
#define STAGE_DEPTH_CHAR 1.0

/*
 * UI depth slider v in [-100, +100] (UX: -100 far, 0 character plane,
 * +100 near) maps to depth d = 10^(-v / 100), then clamped to
 * [STAGE_DEPTH_MIN, STAGE_DEPTH_MAX]. Inverse: v = -100 * log10(d).
 * So v=0 -> 1, v=-100 -> 10, v=+100 -> 0.1: the whole travel is live.
 * UI mapping only; the layer_set depth on the wire is unchanged.
 */
#define STAGE_UI_DEPTH_MIN (-100.0)
#define STAGE_UI_DEPTH_MAX 100.0
#define STAGE_UI_DEPTH_SCALE 100.0

/* Camera. x/y are canvas pixels (0 = neutral), zoom 1 = neutral, rot degrees. */
#define STAGE_ZOOM_MIN 0.5
#define STAGE_ZOOM_MAX 4.0

/*
 * Solver per-layer zoom factor (vendor-api.md section 3):
 * s = clamp(zoom^f, STAGE_LAYER_SCALE_MIN, STAGE_LAYER_SCALE_MAX), strictly
 * positive for every zoom and f = 1/depth; see stage_layer_scale().
 */
#define STAGE_LAYER_SCALE_MIN 0.25
#define STAGE_LAYER_SCALE_MAX 8.0
#define STAGE_ROT_MIN (-45.0)
#define STAGE_ROT_MAX 45.0

/* Durations (ms). */
#define STAGE_MOVE_DURATION_MIN 50
#define STAGE_MOVE_DURATION_MAX 10000
#define STAGE_SET_DURATION_MAX 10000
#define STAGE_PULSE_DURATION_MIN 50
#define STAGE_PULSE_DURATION_MAX 2000
#define STAGE_VIEW_DURATION_DEFAULT 600

/* Intensities. */
#define STAGE_MOVE_INTENSITY_MAX 2.0 /* camera_move, default 1 */
#define STAGE_AUTO_INTENSITY_DEFAULT 0.35
#define STAGE_PULSE_INTENSITY_DEFAULT 1.0

/* Depth of field (driven by the rig onto stage_depth_blur filters). */
#define STAGE_DOF_AMOUNT_MIN 0.0
#define STAGE_DOF_AMOUNT_MAX 1.0

/* Saved views (absolute camera poses per scene). */
#define STAGE_VIEW_SLOT_MIN 1
#define STAGE_VIEW_SLOT_MAX 9
/*
 * View names are limited to STAGE_VIEW_NAME_MAX_CHARS Unicode code points
 * (the schema's maxLength). The buffer holds the worst case of 4 UTF-8 bytes
 * per code point plus NUL. Names that are not valid UTF-8 or are too long get
 * bad_param (stage_view_name_valid); the plugin never truncates.
 */
#define STAGE_VIEW_NAME_MAX_CHARS 47
#define STAGE_VIEW_NAME_MAX 192 /* bytes incl. NUL */

/* Identifiers (move/recipe/fx preset/lut/leak ids): [a-z0-9][a-z0-9_-]{0,30}. */
#define STAGE_ID_MAX 32 /* bytes incl. NUL */
#define STAGE_MOVE_MAX_KEYS 16
#define STAGE_SCENE_UUID_MAX 40

/* Live-safe caps (vendor-api.md section 9). */
#define STAGE_LIVE_SAFE_MAX_ROT_SPEED 20.0  /* deg/s */
#define STAGE_LIVE_SAFE_MAX_ZOOM_SPEED 0.5  /* zoom units/s */
#define STAGE_LIVE_SAFE_MAX_ROLL 8.0        /* deg */
#define STAGE_OVERSCAN_DEFAULT 0.08         /* bg auto-oversize fraction */
#define STAGE_MANUAL_AUTO_PAUSE_S 10        /* manual input pauses auto */
#define STAGE_FLASH_MAX_PER_SEC 3           /* photosensitivity cap */

/* Performance guard (vendor-api.md section 9): > budget for window -> pause most expensive FX. */
#define STAGE_PERF_BUDGET_MS 2.0
#define STAGE_PERF_WINDOW_S 5

/* ------------------------------------------------------------------------- */
/* Name lookups. Return the enum index, or -1 when unknown/NULL.             */
/* ------------------------------------------------------------------------- */

static inline int stage_name_index(const char *const *table, size_t n, const char *name)
{
	if (!name)
		return -1;
	for (size_t i = 0; i < n; i++) {
		if (strcmp(table[i], name) == 0)
			return (int)i;
	}
	return -1;
}

static inline int stage_move_from_name(const char *name)
{
	return stage_name_index(stage_move_names, STAGE_MOVE_COUNT, name);
}

static inline int stage_ease_from_name(const char *name)
{
	return stage_name_index(stage_ease_names, STAGE_EASE_COUNT, name);
}

static inline int stage_auto_mode_from_name(const char *name)
{
	return stage_name_index(stage_auto_mode_names, STAGE_AUTO_COUNT, name);
}

static inline int stage_role_from_name(const char *name)
{
	return stage_name_index(stage_role_names, STAGE_ROLE_COUNT, name);
}

static inline int stage_transition_from_name(const char *name)
{
	return stage_name_index(stage_transition_names, STAGE_TRANSITION_COUNT, name);
}

static inline int stage_pulse_from_name(const char *name)
{
	return stage_name_index(stage_pulse_names, STAGE_PULSE_COUNT, name);
}

static inline int stage_hotkey_action_from_name(const char *name)
{
	return stage_name_index(stage_hotkey_action_names, STAGE_HOTKEY_COUNT, name);
}

/* True when id matches [a-z0-9][a-z0-9_-]{0,30} (fits char[STAGE_ID_MAX]). */
static inline int stage_id_valid(const char *id)
{
	if (!id || !id[0])
		return 0;
	size_t n = 0;
	for (const char *p = id; *p; p++, n++) {
		char c = *p;
		int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
			 (n > 0 && (c == '_' || c == '-'));
		if (!ok || n >= STAGE_ID_MAX - 1)
			return 0;
	}
	return 1;
}

/*
 * True when name is valid UTF-8 (no overlongs, no surrogates, <= U+10FFFF),
 * has at most STAGE_VIEW_NAME_MAX_CHARS code points and so fits
 * char[STAGE_VIEW_NAME_MAX]. An empty name is valid ("no name").
 */
static inline int stage_view_name_valid(const char *name)
{
	if (!name)
		return 0;
	const unsigned char *p = (const unsigned char *)name;
	size_t chars = 0;
	while (*p) {
		unsigned int c = *p, cp;
		int extra;
		if (c < 0x80) {
			cp = c;
			extra = 0;
		} else if (c >= 0xC2 && c <= 0xDF) {
			cp = c & 0x1F;
			extra = 1;
		} else if (c >= 0xE0 && c <= 0xEF) {
			cp = c & 0x0F;
			extra = 2;
		} else if (c >= 0xF0 && c <= 0xF4) {
			cp = c & 0x07;
			extra = 3;
		} else {
			return 0;
		}
		p++;
		for (int i = 0; i < extra; i++, p++) {
			if ((*p & 0xC0) != 0x80)
				return 0; /* also stops at NUL */
			cp = (cp << 6) | (*p & 0x3F);
		}
		if ((extra == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) ||
		    (extra == 3 && (cp < 0x10000 || cp > 0x10FFFF)))
			return 0;
		if (++chars > STAGE_VIEW_NAME_MAX_CHARS)
			return 0;
	}
	return 1;
}

/* Solver layer zoom factor: clamp(zoom^f) with zoom clamped to its range. */
static inline double stage_layer_scale(double zoom, double f)
{
	if (zoom < STAGE_ZOOM_MIN)
		zoom = STAGE_ZOOM_MIN;
	if (zoom > STAGE_ZOOM_MAX)
		zoom = STAGE_ZOOM_MAX;
	double s = pow(zoom, f);
	if (!(s == s)) /* NaN f */
		return 1.0;
	if (s < STAGE_LAYER_SCALE_MIN)
		return STAGE_LAYER_SCALE_MIN;
	if (s > STAGE_LAYER_SCALE_MAX)
		return STAGE_LAYER_SCALE_MAX;
	return s;
}

#ifdef __cplusplus
}
#endif

#endif /* STAGE_PROTOCOL_H */
