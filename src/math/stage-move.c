/*
 * stage-move.c — camera move timeline evaluator (see stage-move.h).
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "stage-move.h"

#include <math.h>
#include <stddef.h>

#include "stage-ease.h"

enum { CH_X, CH_Y, CH_ZOOM, CH_ROT, CH_SHAKE, CH_COUNT };

static const uint8_t ch_bits[CH_COUNT] = {STAGE_CH_X, STAGE_CH_Y, STAGE_CH_ZOOM, STAGE_CH_ROT,
					  STAGE_CH_SHAKE};

/* move.json value ranges per channel. */
static const float ch_min[CH_COUNT] = {-1.0f, -1.0f, -0.9f, (float)STAGE_ROT_MIN, 0.0f};
static const float ch_max[CH_COUNT] = {1.0f, 1.0f, 3.0f, (float)STAGE_ROT_MAX, 1.0f};

static float key_value(const struct stage_key *k, int ch)
{
	switch (ch) {
	case CH_X:
		return k->dx;
	case CH_Y:
		return k->dy;
	case CH_ZOOM:
		return k->dzoom;
	case CH_ROT:
		return k->drot;
	default:
		return k->shake;
	}
}

void stage_key_init(struct stage_key *k)
{
	k->t = 0.0f;
	k->dx = k->dy = k->dzoom = k->drot = k->shake = 0.0f;
	k->ease = (uint8_t)STAGE_EASE_DEFAULT;
	k->mask = 0;
}

bool stage_move_valid(const struct stage_move_def *def)
{
	if (!def || !stage_id_valid(def->id))
		return false;
	if (def->duration_ms < STAGE_MOVE_DURATION_MIN || def->duration_ms > STAGE_MOVE_DURATION_MAX)
		return false;
	if (def->nkeys < 2 || def->nkeys > STAGE_MOVE_MAX_KEYS)
		return false;
	float prev_t = -1.0f;
	for (int i = 0; i < def->nkeys; i++) {
		const struct stage_key *k = &def->keys[i];
		if (!(k->t >= 0.0f && k->t <= 1.0f) || !(k->t > prev_t))
			return false;
		prev_t = k->t;
		if (k->ease >= STAGE_EASE_COUNT || (k->mask & ~STAGE_CH_ALL) != 0)
			return false;
		for (int ch = 0; ch < CH_COUNT; ch++) {
			float v = key_value(k, ch);
			if (!(v >= ch_min[ch] && v <= ch_max[ch])) /* also NaN */
				return false;
		}
	}
	return true;
}

/* Value of one channel at normalized time u in [0, 1]. `start` is the
 * implicit anchor at t = 0 (unscaled); key values are multiplied by
 * `scale` (the intensity). */
static float eval_channel(const struct stage_move_def *def, int ch, float u, int ease_override,
			  float start, float scale)
{
	float t0 = 0.0f, v0 = start;
	int n = def->nkeys > STAGE_MOVE_MAX_KEYS ? STAGE_MOVE_MAX_KEYS : def->nkeys;
	for (int i = 0; i < n; i++) {
		const struct stage_key *k = &def->keys[i];
		if (!(k->mask & ch_bits[ch]))
			continue; /* channel passes through this key */
		float t1 = k->t, v1 = key_value(k, ch) * scale;
		if (u <= t1) {
			if (!(t1 > t0))
				return v1; /* keyed at the anchor time */
			int e = ease_override >= 0 ? ease_override : (int)k->ease;
			float p = stage_ease_apply(e, (u - t0) / (t1 - t0));
			return v0 + (v1 - v0) * p;
		}
		t0 = t1;
		v0 = v1;
	}
	return v0; /* hold the last keyed value */
}

bool stage_move_eval_ex(const struct stage_move_def *def, float t_ms, uint32_t duration_ms,
			float intensity, int ease_override,
			const struct stage_move_delta *start, struct stage_move_delta *out)
{
	float s0[CH_COUNT] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
	if (start) {
		s0[CH_X] = start->dx;
		s0[CH_Y] = start->dy;
		s0[CH_ZOOM] = start->dzoom;
		s0[CH_ROT] = start->drot;
		s0[CH_SHAKE] = start->shake;
		for (int ch = 0; ch < CH_COUNT; ch++)
			if (isnan(s0[ch]))
				s0[ch] = 0.0f;
	}
	uint32_t dur = duration_ms ? duration_ms : def->duration_ms;
	if (isnan(t_ms) || t_ms < 0.0f)
		t_ms = 0.0f;
	if (isnan(intensity))
		intensity = 1.0f;
	if (intensity < 0.0f)
		intensity = 0.0f;
	if (intensity > (float)STAGE_MOVE_INTENSITY_MAX)
		intensity = (float)STAGE_MOVE_INTENSITY_MAX;
	if (ease_override >= STAGE_EASE_COUNT)
		ease_override = -1;

	bool done = dur == 0 || t_ms >= (float)dur;
	float u = done ? 1.0f : t_ms / (float)dur;

	float v[CH_COUNT];
	for (int ch = 0; ch < CH_COUNT; ch++)
		v[ch] = eval_channel(def, ch, u, ease_override, s0[ch], intensity);
	out->dx = v[CH_X];
	out->dy = v[CH_Y];
	out->dzoom = v[CH_ZOOM];
	out->drot = v[CH_ROT];
	out->shake = v[CH_SHAKE];
	return done;
}

bool stage_move_eval(const struct stage_move_def *def, float t_ms, float intensity,
		     struct stage_move_delta *out)
{
	return stage_move_eval_ex(def, t_ms, 0, intensity, -1, NULL, out);
}
