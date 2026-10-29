/*
 * stage-env.h — audio envelope follower, dB conversion and beat ("kick")
 * detector for audio-reactive camera and FX.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Pure C17, no libobs, no allocation. All smoothing is exponential with
 * exact per-step coefficients, so results do not depend on the callback
 * rate (volmeter blocks, video ticks, ...).
 */
#ifndef STAGE_ENV_H
#define STAGE_ENV_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Level returned by stage_lin_to_db for silence (0 or negative input). */
#define STAGE_DB_FLOOR (-100.0f)

/* dB -> linear amplitude: 10^(db / 20). -inf/NaN -> 0; db >= 0 allowed.
 * Values below STAGE_DB_FLOOR return 0 (OBS volmeter reports -inf as
 * silence). */
float stage_db_to_lin(float db);

/* Linear amplitude -> dB: 20 log10(lin), never below STAGE_DB_FLOOR.
 * lin <= 0 or NaN -> STAGE_DB_FLOOR. */
float stage_lin_to_db(float lin);

/*
 * One-pole smoothing coefficient for a time constant: 1 - e^(-dt / tau).
 *   time_ms : time constant tau in ms; <= 0 (or NaN) -> 1 (no smoothing).
 *   dt      : seconds; <= 0 (or NaN) -> 0 (no change).
 *   return  : 0..1, the fraction of the gap closed in this step.
 */
float stage_smooth_coef(float time_ms, float dt);

/* Envelope follower state. value is in the units of the input (typically
 * linear amplitude 0..1). Zero-initialize, or set value to a start level. */
struct stage_env {
	float value;
};

/*
 * Advances the envelope toward `input`, rising with the attack time constant
 * and falling with the release time constant.
 *   input      : current level (for example stage_db_to_lin(magnitude_db)).
 *   attack_ms  : rise time constant in ms (typ. 5..50); <= 0 = instant.
 *   release_ms : fall time constant in ms (typ. 100..500); <= 0 = instant.
 *   dt         : seconds since the previous step.
 *   return     : the new envelope value (also stored in e->value).
 */
float stage_env_step(struct stage_env *e, float input, float attack_ms, float release_ms,
		     float dt);

/*
 * Beat detector settings.
 *   ratio         : kick when energy > ratio * moving average (> 1, typ. 1.5).
 *   window_ms     : moving-average time constant in ms (typ. 1000).
 *   refractory_ms : minimum time between kicks in ms (typ. 150).
 *   min_energy    : energy gate; quieter input never kicks (typ. 1e-4,
 *                   about -40 dBFS when energy = amplitude^2).
 */
struct stage_kick_cfg {
	float ratio;
	float window_ms;
	float refractory_ms;
	float min_energy;
};

#define STAGE_KICK_CFG_DEFAULT_INIT {1.5f, 1000.0f, 150.0f, 1e-4f}

/* Beat detector state; initialize with stage_kick_init. */
struct stage_kick {
	float avg;      /* moving average of energy */
	float since_ms; /* time since the last kick, saturating at the
			   refractory window */
	bool primed;    /* false until the first sample seeds avg */
};

/* Resets a beat detector: average 0, not primed (the next stage_kick_step
 * seeds the average and does not kick). */
void stage_kick_init(struct stage_kick *k);

/*
 * Feeds one energy sample (for example amplitude^2 of a volmeter block).
 *   energy : >= 0 (negative/NaN treated as 0).
 *   dt     : seconds since the previous sample.
 *   return : true when this sample is a kick (energy above ratio x moving
 *            average, above the gate, and outside the refractory window).
 * The first sample only seeds the average and never kicks.
 */
bool stage_kick_step(struct stage_kick *k, const struct stage_kick_cfg *cfg, float energy,
		     float dt);

/*
 * Maps the UX "Beat sensitivity" 0..1 (clamped; NaN -> 0.5) to a kick
 * ratio: 0 -> 2.5 (only strong beats), 0.5 -> 1.8, 1 -> 1.1 (very
 * sensitive).
 */
float stage_kick_ratio_from_sensitivity(float sensitivity);

#ifdef __cplusplus
}
#endif

#endif /* STAGE_ENV_H */
