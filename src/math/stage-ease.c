/*
 * stage-ease.c — easing curves: an independent implementation of the
 * published formulas (Robert Penner's easing equations, in the form
 * easings.net documents them).
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "stage-ease.h"

#include <math.h>

#define EASE_PI 3.14159265358979323846f

/* Back overshoot constants (10 % overshoot), the published values. */
#define BACK_C1 1.70158f
#define BACK_C2 (BACK_C1 * 1.525f)
#define BACK_C3 (BACK_C1 + 1.0f)
/* Elastic angular constants. */
#define ELASTIC_C4 ((2.0f * EASE_PI) / 3.0f)
#define ELASTIC_C5 ((2.0f * EASE_PI) / 4.5f)

static float pow_n(float x, int n)
{
	float r = 1.0f;
	for (int i = 0; i < n; i++)
		r *= x;
	return r;
}

/* Polynomial family of degree n (2 = quad, 3 = cubic, 5 = quint). */
static float poly_in(float t, int n)
{
	return pow_n(t, n);
}

static float poly_out(float t, int n)
{
	return 1.0f - pow_n(1.0f - t, n);
}

static float poly_in_out(float t, int n)
{
	if (t < 0.5f)
		return pow_n(2.0f, n - 1) * pow_n(t, n);
	return 1.0f - pow_n(-2.0f * t + 2.0f, n) * 0.5f;
}

static float back_in(float t)
{
	return BACK_C3 * t * t * t - BACK_C1 * t * t;
}

static float back_out(float t)
{
	float u = t - 1.0f;
	return 1.0f + BACK_C3 * u * u * u + BACK_C1 * u * u;
}

static float back_in_out(float t)
{
	if (t < 0.5f) {
		float u = 2.0f * t;
		return (u * u * ((BACK_C2 + 1.0f) * u - BACK_C2)) * 0.5f;
	}
	float u = 2.0f * t - 2.0f;
	return (u * u * ((BACK_C2 + 1.0f) * u + BACK_C2) + 2.0f) * 0.5f;
}

static float elastic_in(float t)
{
	return -powf(2.0f, 10.0f * t - 10.0f) * sinf((10.0f * t - 10.75f) * ELASTIC_C4);
}

static float elastic_out(float t)
{
	return powf(2.0f, -10.0f * t) * sinf((10.0f * t - 0.75f) * ELASTIC_C4) + 1.0f;
}

static float elastic_in_out(float t)
{
	float s = sinf((20.0f * t - 11.125f) * ELASTIC_C5);
	if (t < 0.5f)
		return -(powf(2.0f, 20.0f * t - 10.0f) * s) * 0.5f;
	return (powf(2.0f, -20.0f * t + 10.0f) * s) * 0.5f + 1.0f;
}

float stage_ease_apply(int ease, float t)
{
	/* Exact endpoints for every curve; also maps NaN to 0. */
	if (!(t > 0.0f))
		return 0.0f;
	if (t >= 1.0f)
		return 1.0f;

	switch (ease) {
	case STAGE_EASE_IN_QUAD:
		return poly_in(t, 2);
	case STAGE_EASE_OUT_QUAD:
		return poly_out(t, 2);
	case STAGE_EASE_IN_OUT_QUAD:
		return poly_in_out(t, 2);
	case STAGE_EASE_IN_CUBIC:
		return poly_in(t, 3);
	case STAGE_EASE_OUT_CUBIC:
		return poly_out(t, 3);
	case STAGE_EASE_IN_OUT_CUBIC:
		return poly_in_out(t, 3);
	case STAGE_EASE_IN_QUINT:
		return poly_in(t, 5);
	case STAGE_EASE_OUT_QUINT:
		return poly_out(t, 5);
	case STAGE_EASE_IN_OUT_QUINT:
		return poly_in_out(t, 5);
	case STAGE_EASE_IN_BACK:
		return back_in(t);
	case STAGE_EASE_OUT_BACK:
		return back_out(t);
	case STAGE_EASE_IN_OUT_BACK:
		return back_in_out(t);
	case STAGE_EASE_IN_ELASTIC:
		return elastic_in(t);
	case STAGE_EASE_OUT_ELASTIC:
		return elastic_out(t);
	case STAGE_EASE_IN_OUT_ELASTIC:
		return elastic_in_out(t);
	case STAGE_EASE_SMOOTHSTEP:
		return t * t * (3.0f - 2.0f * t);
	case STAGE_EASE_LINEAR:
	default:
		return t;
	}
}

bool stage_ease_is_monotonic(int ease)
{
	switch (ease) {
	case STAGE_EASE_IN_BACK:
	case STAGE_EASE_OUT_BACK:
	case STAGE_EASE_IN_OUT_BACK:
	case STAGE_EASE_IN_ELASTIC:
	case STAGE_EASE_OUT_ELASTIC:
	case STAGE_EASE_IN_OUT_ELASTIC:
		return false;
	default:
		return true;
	}
}
