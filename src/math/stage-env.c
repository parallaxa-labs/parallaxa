/*
 * stage-env.c — envelope follower, dB conversion and kick detector.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "stage-env.h"

#include <math.h>

float stage_db_to_lin(float db)
{
	if (isnan(db) || db < STAGE_DB_FLOOR)
		return 0.0f;
	return powf(10.0f, db / 20.0f);
}

float stage_lin_to_db(float lin)
{
	if (!(lin > 0.0f))
		return STAGE_DB_FLOOR;
	float db = 20.0f * log10f(lin);
	return db < STAGE_DB_FLOOR ? STAGE_DB_FLOOR : db;
}

float stage_smooth_coef(float time_ms, float dt)
{
	if (!(dt > 0.0f))
		return 0.0f;
	if (!(time_ms > 0.0f))
		return 1.0f;
	return 1.0f - expf(-dt * 1000.0f / time_ms);
}

float stage_env_step(struct stage_env *e, float input, float attack_ms, float release_ms,
		     float dt)
{
	if (isnan(input))
		input = 0.0f;
	float tau = input > e->value ? attack_ms : release_ms;
	e->value += (input - e->value) * stage_smooth_coef(tau, dt);
	return e->value;
}

void stage_kick_init(struct stage_kick *k)
{
	k->avg = 0.0f;
	k->since_ms = 0.0f;
	k->primed = false;
}

bool stage_kick_step(struct stage_kick *k, const struct stage_kick_cfg *cfg, float energy,
		     float dt)
{
	if (!(energy > 0.0f))
		energy = 0.0f;
	if (!(dt > 0.0f))
		dt = 0.0f;
	if (!k->primed) {
		k->avg = energy;
		k->since_ms = 0.0f;
		k->primed = true;
		return false;
	}
	k->since_ms += dt * 1000.0f;
	/* Saturate at the refractory window: only "since >= refractory" matters,
	 * and this keeps the value bounded over long sessions. */
	if (k->since_ms > cfg->refractory_ms)
		k->since_ms = cfg->refractory_ms;

	bool kick = energy >= cfg->min_energy && energy > cfg->ratio * k->avg &&
		    k->since_ms >= cfg->refractory_ms;
	if (kick)
		k->since_ms = 0.0f;

	/* Update the average after the test so the spike is judged against the
	 * past, not against itself. */
	k->avg += (energy - k->avg) * stage_smooth_coef(cfg->window_ms, dt);
	return kick;
}

float stage_kick_ratio_from_sensitivity(float sensitivity)
{
	if (isnan(sensitivity))
		sensitivity = 0.5f;
	if (sensitivity < 0.0f)
		sensitivity = 0.0f;
	if (sensitivity > 1.0f)
		sensitivity = 1.0f;
	return 2.5f - 1.4f * sensitivity;
}
