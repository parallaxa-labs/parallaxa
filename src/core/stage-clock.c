/*
 * stage-clock.c — monotonic millisecond clock for the core (no libobs).
 * Kept in its own translation unit so <windows.h> never meets pthread.h.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "stage-core.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

uint64_t stage_core_now_ms(void)
{
	static LARGE_INTEGER freq; /* constant after boot; benign race */
	LARGE_INTEGER now;
	if (!freq.QuadPart)
		QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&now);
	/* Split to avoid overflow of now * 1000 on long uptimes. */
	uint64_t f = (uint64_t)freq.QuadPart;
	uint64_t t = (uint64_t)now.QuadPart;
	return (t / f) * 1000u + ((t % f) * 1000u) / f;
}

#else
#include <time.h>

uint64_t stage_core_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
#endif
