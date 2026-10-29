/*
 * stage-ease.h — easing curves for camera moves and transitions.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Pure C17, no libobs. The curve ids are enum stage_ease from
 * include/stage-protocol.h (string names in stage_ease_names[]).
 */
#ifndef STAGE_EASE_H
#define STAGE_EASE_H

#include <stdbool.h>

#include "stage-protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Evaluates easing curve `ease` (enum stage_ease) at normalized time t.
 *
 *   t     : progress, unitless. Clamped to [0, 1]; NaN is treated as 0.
 *   ease  : enum stage_ease value. Unknown values (>= STAGE_EASE_COUNT,
 *           negative) fall back to STAGE_EASE_LINEAR.
 *   return: eased progress. Exactly 0 at t <= 0 and exactly 1 at t >= 1.
 *           Inside (0, 1) the result stays in [0, 1] except for the
 *           back curves (overshoot down to about -0.1 / up to about 1.1)
 *           and the elastic curves (about -0.37 .. 1.37).
 */
float stage_ease_apply(int ease, float t);

/*
 * True when curve `ease` is monotonically non-decreasing on [0, 1]
 * (every curve except the back and elastic families). Unknown ids return
 * true (they evaluate as linear).
 */
bool stage_ease_is_monotonic(int ease);

#ifdef __cplusplus
}
#endif

#endif /* STAGE_EASE_H */
