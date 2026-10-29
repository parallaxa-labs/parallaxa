/*
 * stage-noise.c — 1D gradient noise and fBm.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "stage-noise.h"

#include <math.h>

#define NOISE_MASK ((uint32_t)STAGE_NOISE_PERIOD - 1u)

/* "lowbias32" integer hash (Chris Wellons, public domain). */
static uint32_t hash32(uint32_t x)
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

/* Gradient (slope) at lattice point `cell`, uniform in [-1, 1). */
static float gradient(uint32_t seed_hash, uint32_t cell)
{
	uint32_t h = hash32(seed_hash ^ (cell & NOISE_MASK));
	return (float)(h >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

/* Reduces x to [0, PERIOD). Returns 0 and sets *ok = 0 when not finite. */
static float wrap_period(float x, int *ok)
{
	if (!isfinite(x)) {
		*ok = 0;
		return 0.0f;
	}
	*ok = 1;
	float r = fmodf(x, (float)STAGE_NOISE_PERIOD);
	if (r < 0.0f)
		r += (float)STAGE_NOISE_PERIOD;
	if (r >= (float)STAGE_NOISE_PERIOD) /* -tiny + PERIOD rounds up */
		r = 0.0f;
	return r;
}

static float noise_hashed(uint32_t seed_hash, float x)
{
	int ok;
	x = wrap_period(x, &ok);
	if (!ok)
		return 0.0f;
	float x0 = floorf(x);
	uint32_t cell = (uint32_t)x0;
	float fr = x - x0;
	float g0 = gradient(seed_hash, cell);
	float g1 = gradient(seed_hash, cell + 1u);
	float a = g0 * fr;
	float b = g1 * (fr - 1.0f);
	/* Quintic fade 6t^5 - 15t^4 + 10t^3 (C2 continuous). */
	float u = fr * fr * fr * (fr * (fr * 6.0f - 15.0f) + 10.0f);
	/* The lerp peaks at |0.5|; scale to [-1, 1]. */
	return 2.0f * (a + (b - a) * u);
}

float stage_noise1(uint32_t seed, float x)
{
	return noise_hashed(hash32(seed), x);
}

float stage_noise_fbm(uint32_t seed, float x, int octaves, float lacunarity, float gain)
{
	if (octaves < 1)
		octaves = 1;
	if (octaves > STAGE_NOISE_MAX_OCTAVES)
		octaves = STAGE_NOISE_MAX_OCTAVES;
	if (!(gain >= 0.0f))
		gain = 0.0f;
	if (gain > 1.0f)
		gain = 1.0f;
	if (!isfinite(lacunarity))
		lacunarity = 2.0f;

	float sum = 0.0f, norm = 0.0f, amp = 1.0f, freq = 1.0f;
	for (int o = 0; o < octaves; o++) {
		/* Each octave gets its own pattern so they do not line up. */
		uint32_t sh = hash32(seed + (uint32_t)o * 0x9E3779B9u);
		sum += amp * noise_hashed(sh, x * freq);
		norm += amp;
		amp *= gain;
		freq *= lacunarity;
	}
	return norm > 0.0f ? sum / norm : 0.0f;
}

float stage_noise_advance(float phase, float freq_hz, float dt)
{
	int ok;
	float r = wrap_period(phase + freq_hz * dt, &ok);
	return ok ? r : 0.0f;
}
