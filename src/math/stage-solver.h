/*
 * stage-solver.h — depth/parallax layer solver, UI depth mapping, depth of
 * field strength and canvas edge-coverage (overscan) helpers.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Pure C17, no libobs. Normative math: docs/protocol/vendor-api.md section 3.
 * Units everywhere:
 *   - positions, pans and rects: canvas pixels, y down, origin top-left;
 *   - angles: degrees, positive = clockwise on screen (OBS convention);
 *   - zoom and scales: unitless multipliers (1 = neutral).
 *
 * For a layer with parallax factor f, canvas centre C = (w/2, h/2) and
 * camera c = {x, y, zoom, rot}:
 *   s      = clamp(zoom ^ f, 0.25, 8)            (stage_layer_scale)
 *   pos    = C + R(rot f) ((rest.pos - C) s) - (x, y) f
 *   scale  = rest.scale * s
 *   rot'   = rest.rot + rot f
 * f = 1 / clamp(depth, 0.1, 10) for roles bg/char/fg (continuous, no jump at
 * 10) and f = 0 for sky/static. The interface is float; the arithmetic runs
 * in double so solve -> unsolve round-trips stay well below 1e-3 px.
 */
#ifndef STAGE_SOLVER_H
#define STAGE_SOLVER_H

#include <stdbool.h>
#include <stddef.h>

#include "stage-protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Virtual camera. x/y canvas px (0 = neutral, +x pans the view right, +y
 * down), zoom 1 = neutral (valid 0.5..4), rot degrees (valid -45..45). */
struct stage_cam {
	float x, y, zoom, rot;
};

/* Scene-item transform: position px, scale (or bounds px when the item uses
 * a bounds type: the solver scales both the same way), rotation degrees. */
struct stage_xform {
	float px, py, sx, sy, rot;
};

/* Axis-aligned rectangle in canvas px: left, top, right, bottom. */
struct stage_rect {
	float l, t, r, b;
};

/* Home camera {0, 0, 1, 0}. */
#define STAGE_CAM_HOME_INIT {0.0f, 0.0f, 1.0f, 0.0f}

/* ---------------------------------------------------------------------- */
/* Depth, roles and UI mapping                                            */
/* ---------------------------------------------------------------------- */

/* Clamps depth to [STAGE_DEPTH_MIN, STAGE_DEPTH_MAX] = [0.1, 10].
 * NaN returns STAGE_DEPTH_CHAR (1). */
float stage_depth_clamp(float depth);

/* Parallax factor f = 1 / stage_depth_clamp(depth), in [0.1, 10].
 * Continuous over the whole range (depth 10 gives 0.1, never 0). */
float stage_depth_factor(float depth);

/*
 * Parallax factor for a layer role (enum stage_role):
 *   bg, char, fg -> stage_depth_factor(depth)    in [0.1, 10]
 *   sky, static  -> 0 (never moves; "infinitely far" is a role, not a depth)
 *   none/unknown -> 0 (not enrolled; callers should not touch the item)
 */
float stage_role_factor(int role, float depth);

/* Default depth for a role when it is set on an item without stage.depth:
 * bg 3, char 1, fg 0.6, sky 10, static 1, none/unknown 1. */
float stage_role_default_depth(int role);

/*
 * UI depth slider -> depth: d = clamp(10^(-v / 100), 0.1, 10).
 *   v : slider value in [-100, +100] (-100 far = 10, 0 character plane = 1,
 *       +100 near = 0.1). |v| > 100 saturates. NaN -> 1.
 */
float stage_depth_from_ui(float v);

/* Depth -> UI slider: v = -100 * log10(clamp(d, 0.1, 10)), in [-100, +100].
 * NaN -> 0. Exact inverse of stage_depth_from_ui for v in [-100, 100]. */
float stage_ui_from_depth(float depth);

/*
 * Depth-of-field blur strength written to stage_depth_blur "auto_strength":
 * clamp(|depth - focus_depth| * amount, 0, 1).
 *   depth, focus_depth : 0.1..10 (both clamped; use 10 for the sky role).
 *   amount             : 0..1 (clamped). NaN inputs return 0 (no blur).
 */
float stage_dof_strength(float depth, float focus_depth, float amount);

/* Per-layer zoom multiplier s = clamp(zoom^f, 0.25, 8) with zoom clamped to
 * [0.5, 4] and f to [0, 10]; float wrapper of stage_layer_scale(). */
float stage_layer_zoom(float zoom, float f);

/* ---------------------------------------------------------------------- */
/* Solver                                                                 */
/* ---------------------------------------------------------------------- */

/*
 * Solves the on-screen transform of a layer from its rest pose.
 *   rest     : rest pose (the user's own transform).
 *   f        : parallax factor (stage_role_factor), clamped to [0, 10];
 *              NaN -> 0. f = 0 returns rest unchanged.
 *   cam      : camera; zoom is clamped to [0.5, 4] through the layer scale,
 *              x/y/rot are used as given.
 *   canvas_w, canvas_h : canvas size in px (> 0).
 *   out      : result (may alias rest).
 */
void stage_solve_layer_f(const struct stage_xform *rest, float f, const struct stage_cam *cam,
			 float canvas_w, float canvas_h, struct stage_xform *out);

/* Exact inverse of stage_solve_layer_f: recovers the rest pose from the
 * current on-screen transform `cur` (may alias rest_out). */
void stage_unsolve_layer_f(const struct stage_xform *cur, float f, const struct stage_cam *cam,
			   float canvas_w, float canvas_h, struct stage_xform *rest_out);

/* stage_solve_layer_f with f = stage_depth_factor(depth); depth 0.1..10
 * (clamped), 1 = character plane. */
void stage_solve_layer(const struct stage_xform *rest, float depth, const struct stage_cam *cam,
		       float canvas_w, float canvas_h, struct stage_xform *out);

/* Inverse of stage_solve_layer (same depth rules). */
void stage_unsolve_layer(const struct stage_xform *cur, float depth, const struct stage_cam *cam,
			 float canvas_w, float canvas_h, struct stage_xform *rest_out);

/* ---------------------------------------------------------------------- */
/* Edge coverage (overscan / Live-safe camera clamp)                      */
/* ---------------------------------------------------------------------- */
/*
 * A covering layer (usually role bg) is described by `rect`: its unrotated
 * axis-aligned bounds at rest in canvas px. For a rotated rest pose pass an
 * axis-aligned rect inscribed in it (conservative). The canvas is covered
 * when all four canvas corners, mapped back into the layer, lie inside rect.
 * Rects with zero or negative width/height are "invalid".
 */

/*
 * Signed coverage margin in canvas px: the smallest distance from a canvas
 * corner to the layer edge (as seen on screen). >= 0 means covered.
 * f <= 0 layers are evaluated with the identity camera. Invalid rect ->
 * -FLT_MAX.
 */
float stage_cover_margin(const struct stage_rect *rect, float f, const struct stage_cam *cam,
			 float canvas_w, float canvas_h);

/*
 * Minimum oversize fraction o >= 0 such that the rect grown about its own
 * centre by (1 + o) covers the canvas for this one camera (0 when already
 * covered). Compare with STAGE_OVERSCAN_DEFAULT (0.08). Invalid rect -> 0.
 */
float stage_cover_min_oversize(const struct stage_rect *rect, float f, const struct stage_cam *cam,
			       float canvas_w, float canvas_h);

/* Range of camera poses a layer must survive (for "Overscan kept at least at
 * the computed minimum"). Pans are symmetric limits in canvas px (>= 0),
 * zoom_min..zoom_max within [0.5, 4], max_rot degrees (>= 0, symmetric). */
struct stage_cover_range {
	float max_pan_x, max_pan_y;
	float zoom_min, zoom_max;
	float max_rot;
};

/*
 * Minimum oversize fraction that keeps the canvas covered for every camera
 * in `range`. Pan and zoom extremes are exact (the requirement is maximal at
 * the box corners); rotation is sampled at 17 angles (error < 0.1 px for
 * |rot| <= 8 deg on a 4K canvas). Invalid rect -> 0.
 */
float stage_cover_min_oversize_range(const struct stage_rect *rect, float f,
				     const struct stage_cover_range *range, float canvas_w,
				     float canvas_h);

/*
 * Smallest camera zoom (>= STAGE_ZOOM_MIN) at which some pan keeps the
 * canvas covered by this layer at camera roll `rot` (degrees). A value above
 * STAGE_ZOOM_MAX means "impossible within the zoom range: oversize the
 * layer". f <= 0 returns STAGE_ZOOM_MIN when covered at rest, else
 * +FLT_MAX. Invalid rect -> +FLT_MAX.
 */
float stage_cover_min_zoom(const struct stage_rect *rect, float f, float rot, float canvas_w,
			   float canvas_h);

/* Axis-aligned pan range in camera px (see stage_cover_pan_box). */
struct stage_pan_box {
	float x_min, x_max, y_min, y_max;
};

/*
 * Allowed camera pan at a given zoom/rot for one layer, in camera px. The
 * box is expressed in the layer's rotated frame (u = R(-rot f) (x, y)); for
 * rot = 0 (or f = 0) it is exactly the allowed x/y range. Returns false (and
 * a collapsed box at the midpoint of the empty axis) when no pan covers.
 */
bool stage_cover_pan_box(const struct stage_rect *rect, float f, float zoom, float rot,
			 float canvas_w, float canvas_h, struct stage_pan_box *out);

/* Covering layers considered by stage_cover_clamp (extra ones are ignored). */
#define STAGE_COVER_MAX_LAYERS 16

/*
 * Live-safe camera clamp for n covering layers (rects[i] with factor fs[i]).
 * Keeps rot; raises zoom to the smallest value at which every layer can
 * cover the canvas (at least each layer's stage_cover_min_zoom, capped at
 * STAGE_ZOOM_MAX); then moves x/y to the closest pan (Euclidean, exact) at
 * which all layers cover. Layers with f <= 0 or invalid rects are ignored,
 * as are layers beyond STAGE_COVER_MAX_LAYERS. A camera that already
 * covers is returned unchanged. NaN fields are treated as home values.
 * `out` may alias `in`. Cost: trivial when covered; otherwise O((4n)^3)
 * per bisection step, microseconds for a few layers.
 * Returns true when the result covers the canvas for every considered layer
 * (within 0.01 px); false when it is impossible even at STAGE_ZOOM_MAX
 * (then the result is a best effort; oversize the layers).
 */
bool stage_cover_clamp(const struct stage_rect *rects, const float *fs, size_t n,
		       const struct stage_cam *in, float canvas_w, float canvas_h,
		       struct stage_cam *out);

#ifdef __cplusplus
}
#endif

#endif /* STAGE_SOLVER_H */
