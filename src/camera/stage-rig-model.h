/*
 * stage-rig-model.h — the Camera Rig's motion model: camera target, tweens,
 * moves, auto camera, audio bounce, shake, freeze, depth of field, springs
 * and the Live-safe limits. Pure C17 (no libobs), one instance per rig, used
 * only from the graphics thread (the glue in stage-camera-rig.c owns it).
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Pipeline of one step (stage_rig_model_step):
 *
 *   base   = target T (or its running tween) + running move (vendor-api 6.1)
 *            + auto camera offset x auto weight (drift / chatting / singing)
 *   base   is clamped to Max zoom / Max pan (explicit targets may exceed
 *            them, see vendor-api 4.4) and to the protocol camera range
 *   spring = critically damped spring toward base (Smoothing)
 *   out    = spring + shake (x/y noise) + audio bounce (zoom)
 *   Live-safe: zoom speed <= 0.5/s, rotation speed <= 20 deg/s, roll <= 8 deg,
 *            then the coverage clamp (the covering layers given by the glue
 *            must keep the canvas covered).
 *
 * Units: x/y canvas px (0 = neutral), zoom 1 = neutral, rot degrees.
 */
#ifndef STAGE_RIG_MODEL_H
#define STAGE_RIG_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stage-protocol.h"
#include "stage-core.h" /* struct stage_cam_target, STAGE_CAM_*, STAGE_DOF_* */
#include "stage-env.h"
#include "stage-move.h"
#include "stage-solver.h"
#include "stage-spring.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Extensions of struct stage_cmd that only the Camera Rig understands (the
 * camera module's own hotkeys send them; the vendor never does). */
#define STAGE_CAM_EASE_NONE 0xFFu      /* camera_move: keep the per-key eases */
#define STAGE_CAM_AUTO_TOGGLE 0xF0u    /* camera_auto mode: off <-> last mode  */
#define STAGE_CAM_AUTO_NEXT 0xF1u      /* camera_auto mode: off>drift>chat>sing */
/* dof_set rack focus masks (the vendor's hotkeys send them too). */
#define STAGE_CAM_DOF_RACK_CHAR STAGE_DOF_RACK_CHAR
#define STAGE_CAM_DOF_RACK_BG STAGE_DOF_RACK_BG

/* Rack focus (hotkeys) duration and the punch-in hold (stage-plugin-ux.md section 5). */
#define STAGE_CAM_RACK_MS 1200u
#define STAGE_CAM_PUNCH_ZOOM 0.18f
#define STAGE_CAM_PUNCH_IN_MS 120u
#define STAGE_CAM_PUNCH_OUT_MS 200u
/* Panic: camera home over this many ms (vendor-api 5.3). */
#define STAGE_CAM_PANIC_MS 200u
/* Default camera_reset / view durations. */
#define STAGE_CAM_RESET_MS 600u

/* Settings that the panel (or a recipe) controls. Defaults: stage-plugin-ux.md section 1. */
struct stage_rig_params {
	float canvas_w, canvas_h; /* > 0 */
	float smoothing_ms;       /* 0..1000 (0 = no smoothing)          */
	bool live_safe;
	float max_zoom;           /* 1..4: cap for auto, moves and bounce */
	float max_pan;            /* 0..0.5 of the canvas size, same scope */
	float motion_scale;       /* 0..1: shake and drift scale          */
	float manual_pause_s;     /* 0..60                                */
	float audio_amount;       /* 0..1 bounce amount                   */
	float audio_attack_ms;    /* 1..500                               */
	float audio_release_ms;   /* 10..2000                             */
};

void stage_rig_params_default(struct stage_rig_params *p);

/* An animated value pair from -> to over dur seconds with an ease. */
struct stage_rig_tween {
	bool active;
	struct stage_cam from, to;
	float t, dur; /* seconds */
	int ease;
};

/* Scalar tween (depth of field). */
struct stage_rig_ftween {
	bool active;
	float from, to;
	float t, dur;
	int ease;
};

/* A covering layer for the Live-safe coverage clamp (role bg, visible):
 * its oversized rest rect and parallax factor. */
struct stage_rig_cover {
	struct stage_rect rect;
	float f;
};

/* Per-step inputs from the glue. */
struct stage_rig_inputs {
	float dt;            /* seconds since the previous step              */
	float audio_db;      /* audio peak in dBFS; NaN = no audio source     */
	float char_depth;    /* depth of the char layer, NaN = none (-> 1)    */
	float far_bg_depth;  /* depth of the farthest bg layer, NaN = none (-> 3) */
	float dof_slider;    /* panel "Blur amount" 0..1 (restore value)      */
	const struct stage_rig_cover *covers;
	size_t ncovers;
};

/* Model change flags (stage_rig_model.changed), cleared by the glue. */
#define STAGE_RIG_CH_AUTO 0x01u   /* auto mode or intensity changed  */
#define STAGE_RIG_CH_FREEZE 0x02u /* frozen changed                  */
#define STAGE_RIG_CH_DOF 0x04u    /* focus/amount/follow changed     */

struct stage_rig_model {
	struct stage_rig_params p;
	double now; /* seconds since init */

	/* Explicit camera target and its tween. */
	struct stage_cam target;
	struct stage_rig_tween tween;

	/* Running move. */
	bool move_active;
	struct stage_move_def move;
	uint64_t move_cmd_id; /* kept after the move ends, as is move.id */
	bool move_done;       /* the last move ran to its end */
	struct stage_cam move_start; /* target when the move started */
	struct stage_move_delta move_anchor;
	float move_t_ms;
	uint32_t move_dur_ms;
	float move_intensity;
	int move_ease;     /* < 0 = per-key eases */
	float move_shake;  /* current shake amplitude 0..1 from the move */
	struct stage_cam move_pose; /* last evaluated move pose */

	/* Punch-in hold. */
	bool punch_held;
	struct stage_cam punch_before;

	/* Auto camera. */
	uint8_t auto_mode;      /* enum stage_auto_mode */
	uint8_t last_auto_mode; /* last mode other than off (toggle) */
	float auto_intensity;   /* 0..1 */
	float auto_weight;      /* 0..1 blend (manual pause fades it) */
	double auto_pause_until;
	float ph[6];            /* noise phases */
	struct stage_cam auto_off; /* last auto offset (x,y px, zoom delta, rot) */

	/* Audio. */
	struct stage_env env;
	struct stage_kick kick;
	float kick_pulse; /* 0..1, decays */
	float bounce;     /* last bounce zoom delta */

	/* Shake. */
	float shake_ph;

	/* Springs and output. */
	bool spring_init;
	struct stage_spring2 sp_pan;
	struct stage_spring sp_zoom, sp_rot;
	struct stage_cam out;
	float out_shake;
	bool frozen;
	uint8_t view; /* slot last applied while T unchanged, else 0 */

	/* Depth of field. */
	float focus;  /* 0.1..10 */
	float amount; /* 0..1    */
	bool follow_char;
	struct stage_rig_ftween focus_tw, amount_tw;

	uint32_t changed; /* STAGE_RIG_CH_* */
};

/* Home pose, defaults, auto off at intensity 0.35, DoF focus 1 amount 0 with
 * focus follows character on. */
void stage_rig_model_init(struct stage_rig_model *m, const struct stage_rig_params *p);

/* ---- commands (all "manual" ones pause the auto camera) ---------------- */

/* camera_move. def must be valid (stage_move_valid). duration_ms 0 = the
 * move's own; intensity < 0 or NaN = 1; ease >= STAGE_EASE_COUNT = per-key
 * eases. While frozen the move is ignored (returns false = "cancelled"). */
bool stage_rig_model_move(struct stage_rig_model *m, const struct stage_move_def *def,
			  uint64_t cmd_id, uint32_t duration_ms, float intensity, unsigned ease);

/* camera_set: masked channels (STAGE_CAM_*), absolute or relative (then
 * clamped to the camera range). duration_ms 0 = follow through the spring. */
void stage_rig_model_set(struct stage_rig_model *m, const struct stage_cam_target *t,
			 uint32_t duration_ms, unsigned ease);

/* camera_reset: target home over duration_ms; cancels the move. */
void stage_rig_model_reset(struct stage_rig_model *m, uint32_t duration_ms, unsigned ease);

/* camera_auto: mode is enum stage_auto_mode, STAGE_CAM_AUTO_TOGGLE or
 * STAGE_CAM_AUTO_NEXT; intensity < 0 (or NaN) keeps the current one. */
void stage_rig_model_auto(struct stage_rig_model *m, unsigned mode, float intensity);

/* freeze: enum stage_freeze_mode. */
void stage_rig_model_freeze(struct stage_rig_model *m, unsigned mode);

/* view_apply with a resolved pose; transition gives the ease; duration 0 or
 * transition cut = instant (spring snapped too). */
void stage_rig_model_view_apply(struct stage_rig_model *m, uint8_t slot, const struct stage_cam *pose,
				unsigned transition, uint32_t duration_ms);

/* Next (dir > 0) or previous (dir < 0) saved view after the one last applied
 * (m->view; with none, the first or the last saved one), wrapping around.
 * filled: bit (slot - 1) set for each saved slot. 0 when none is saved. */
uint8_t stage_rig_model_view_step(const struct stage_rig_model *m, int dir, unsigned filled);

/* dof_set: mask STAGE_DOF_FOCUS / STAGE_DOF_AMOUNT / STAGE_CAM_DOF_RACK_*.
 * char_depth / far_bg_depth: see stage_rig_inputs (NaN = none). */
void stage_rig_model_dof(struct stage_rig_model *m, unsigned mask, float focus, float amount,
			 uint32_t duration_ms, unsigned ease, float char_depth, float far_bg_depth,
			 float dof_slider);

/* Panic: home over 200 ms, auto off, unfreeze, DoF amount 0 and follow on,
 * move and punch cancelled. */
void stage_rig_model_panic(struct stage_rig_model *m);

/* Snap the camera home now (Bake rest pose / Restore all rest poses): no
 * tween, springs at home, move cancelled. */
void stage_rig_model_snap_home(struct stage_rig_model *m);

/* Punch-in hold: press = relative zoom +0.18 over 120 ms (ease_out_back);
 * release = back to the target from before the press over 200 ms. */
void stage_rig_model_punch(struct stage_rig_model *m, bool press);

/* Marks a manual input (panel sliders): pauses the auto camera. */
void stage_rig_model_manual(struct stage_rig_model *m);

/* ---- step and queries ------------------------------------------------- */

void stage_rig_model_step(struct stage_rig_model *m, const struct stage_rig_inputs *in);

/* The camera target T as view_save stores it (a running tween's
 * destination, not a mid-transition pose). */
void stage_rig_model_target(const struct stage_rig_model *m, struct stage_cam *out);

/* True while a move, a tween or a punch transition runs. */
bool stage_rig_model_moving(const struct stage_rig_model *m);

/* Clamp a camera to the protocol range (zoom 0.5..4, rot -45..45, |x| <= W,
 * |y| <= H); NaN fields become home values. */
void stage_rig_cam_sanitize(struct stage_cam *c, float canvas_w, float canvas_h);

#ifdef __cplusplus
}
#endif

#endif /* STAGE_RIG_MODEL_H */
