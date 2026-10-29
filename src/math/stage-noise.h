/*
 * stage-noise.h — deterministic 1D gradient noise and fractal sum (fBm),
 * used for handheld drift and camera shake.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Pure C17, no libobs, no tables, no global state: the same (seed, x)
 * always gives the same value on every platform that has IEEE floats.
 *
 * The noise is periodic with period STAGE_NOISE_PERIOD lattice units, so a
 * caller can keep its phase small (and precise in float) by wrapping it
 * with stage_noise_advance() without any visible seam. At 3 Hz the pattern
 * repeats after about 23 minutes, which is not noticeable on camera.
 */
#ifndef STAGE_NOISE_H
#define STAGE_NOISE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lattice period (power of two). Noise(x) == Noise(x + STAGE_NOISE_PERIOD). */
#define STAGE_NOISE_PERIOD 4096
/* Maximum octave count accepted by stage_noise_fbm (more are clamped). */
#define STAGE_NOISE_MAX_OCTAVES 8

/*
 * 1D gradient (Perlin-style) noise.
 *   seed  : any value; different seeds give uncorrelated patterns.
 *   x     : coordinate in lattice units (one unit ~ one "wiggle"). Any finite
 *           value; it is reduced modulo STAGE_NOISE_PERIOD. NaN/inf -> 0.
 *   return: smooth (C2) value in [-1, 1]; 0 at every integer x.
 */
float stage_noise1(uint32_t seed, float x);

/*
 * Fractal sum (fBm) of `octaves` noise layers, each one with frequency
 * multiplied by `lacunarity` and amplitude by `gain`, normalized by the sum
 * of amplitudes.
 *   octaves    : 1..STAGE_NOISE_MAX_OCTAVES (clamped; < 1 -> 1).
 *   lacunarity : frequency ratio between octaves, typically 2. Use an
 *                integer to keep the STAGE_NOISE_PERIOD periodicity.
 *   gain       : amplitude ratio, typically 0.5, clamped to [0, 1].
 *   return     : value in [-1, 1].
 */
float stage_noise_fbm(uint32_t seed, float x, int octaves, float lacunarity, float gain);

/*
 * Advances a noise phase: returns phase + freq_hz * dt wrapped to
 * [0, STAGE_NOISE_PERIOD).
 *   phase   : current phase, lattice units.
 *   freq_hz : lattice units per second (wiggles per second), >= 0. Callers
 *             implementing "Reduce motion" cap this below 3 Hz.
 *   dt      : seconds. Non-finite results return 0.
 */
float stage_noise_advance(float phase, float freq_hz, float dt);

#ifdef __cplusplus
}
#endif

#endif /* STAGE_NOISE_H */
