/*
 * stage-move.h — camera move timeline evaluator.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Pure C17, no libobs, no allocation. File format and semantics:
 * docs/protocol/vendor-api.md section 6.1 (schema move.json).
 *
 * Each channel (x, y, zoom, rot, shake) is evaluated on its own. A key keys
 * a channel only when its mask bit (STAGE_CH_*) is set; keys that do not
 * key a channel are passed through. Every channel implicitly starts at its
 * start (anchor) value at t = 0 unless a key at t = 0 keys it, and holds the
 * last keyed value after its last key. A channel that no key sets stays at
 * its start value for the whole move. Between two keyed values the ease of
 * the destination key ("ease into that key") applies.
 *
 * The start value depends on the move's `relative` flag (vendor-api 6.1):
 *
 *   relative:true  - key values are deltas added to the camera target at
 *                    the moment the move starts, so every start value is 0:
 *                    pass start = NULL (stage_move_eval does this).
 *                    Camera target = start pose + out.
 *   relative:false - key values are absolute offsets from home
 *                    (x 0, y 0, zoom 1, rot 0, shake 0). The start is the
 *                    camera target when the move starts, minus home, in key
 *                    units, so the move eases from there instead of
 *                    snapping:
 *                      start.dx    = cam.x / canvas_w      (fraction)
 *                      start.dy    = cam.y / canvas_h      (fraction)
 *                      start.dzoom = cam.zoom - 1
 *                      start.drot  = stage_wrap_deg(cam.rot)
 *                      start.shake = current shake amplitude (0..1)
 *                    and call stage_move_eval_ex with it. Camera target =
 *                    home + out: x = out.dx * canvas_w, y = out.dy *
 *                    canvas_h, zoom = 1 + out.dzoom, rot = out.drot.
 *
 * Example: the built-in `reset` (relative:false, key 0 {t:0} with no
 * channels, key 1 all zeros with ease_in_out_cubic, 700 ms) eases every
 * channel from the start pose to home over 700 ms.
 */
#ifndef STAGE_MOVE_H
#define STAGE_MOVE_H

#include <stdbool.h>
#include <stdint.h>

#include "stage-protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One keyframe.
 *   t     : normalized time 0..1 (fraction of duration), strictly increasing
 *           across keys.
 *   dx,dy : pan delta as a fraction of canvas width / height (-1..1).
 *   dzoom : added to zoom (-0.9..3).
 *   drot  : degrees (-45..45).
 *   shake : shake amplitude 0..1 (the caller multiplies it by noise).
 *   ease  : enum stage_ease used to arrive at this key.
 *   mask  : STAGE_CH_* bits of the channels this key sets.
 */
struct stage_key {
	float t;
	float dx, dy, dzoom, drot, shake;
	uint8_t ease;
	uint8_t mask;
};

/*
 * A move.
 *   id          : [a-z0-9][a-z0-9_-]{0,30}, NUL-terminated.
 *   duration_ms : STAGE_MOVE_DURATION_MIN..MAX (50..10000).
 *   relative    : true = deltas add to the current camera; false = offsets
 *                 from home. (Applied by the caller.)
 *   hold        : true keeps the end pose; false springs back. (Caller.)
 *   nkeys       : 2..STAGE_MOVE_MAX_KEYS valid entries in keys[].
 */
struct stage_move_def {
	char id[STAGE_ID_MAX];
	uint32_t duration_ms;
	bool relative;
	bool hold;
	uint8_t nkeys;
	struct stage_key keys[STAGE_MOVE_MAX_KEYS];
};

/* Evaluated deltas, units as in stage_key. Key values are scaled by
 * intensity; start (anchor) values are not. Also the type of the start
 * values passed to stage_move_eval_ex. */
struct stage_move_delta {
	float dx, dy, dzoom, drot, shake;
};

/* Fills *k with t = 0, all deltas 0, mask 0 and the default ease
 * (STAGE_EASE_DEFAULT). */
void stage_key_init(struct stage_key *k);

/*
 * Validates a move against the move.json limits: id valid, duration in
 * range, 2..16 keys, every t in [0, 1] and strictly increasing, eases known,
 * channel values finite and in range, no unknown mask bits. Returns true
 * when valid. Loaders log and skip invalid files.
 */
bool stage_move_valid(const struct stage_move_def *def);

/*
 * Evaluates `def` at elapsed time t_ms with every start value 0 (the
 * relative:true case, see the file comment). Same as
 * stage_move_eval_ex(def, t_ms, 0, intensity, -1, NULL, out).
 *   t_ms      : elapsed milliseconds since the move started; clamped to
 *               [0, duration_ms]. NaN -> 0.
 *   intensity : multiplier for every key value, 0..STAGE_MOVE_INTENSITY_MAX
 *               (clamped; NaN -> 1).
 *   out       : deltas at that time.
 *   return    : true once t_ms >= duration_ms (the move has finished; out
 *               then holds the end values).
 * Keys must satisfy stage_move_valid (out-of-order keys give undefined but
 * finite results).
 */
bool stage_move_eval(const struct stage_move_def *def, float t_ms, float intensity,
		     struct stage_move_delta *out);

/*
 * Full evaluator: stage_move_eval plus the camera_move request overrides
 * and the start (anchor) values.
 *   duration_ms   : 0 = use def->duration_ms, otherwise the duration to use.
 *   ease_override : < 0 = per-key eases; otherwise this enum stage_ease
 *                   replaces every key's ease.
 *   start         : value of each channel at t = 0, used for channels that
 *                   no key at t = 0 sets; NOT scaled by intensity. NULL =
 *                   all 0 (relative:true). For relative:false build it from
 *                   the camera target minus home as the file comment says.
 *                   NaN fields are treated as 0.
 */
bool stage_move_eval_ex(const struct stage_move_def *def, float t_ms, uint32_t duration_ms,
			float intensity, int ease_override,
			const struct stage_move_delta *start, struct stage_move_delta *out);

#ifdef __cplusplus
}
#endif

#endif /* STAGE_MOVE_H */
