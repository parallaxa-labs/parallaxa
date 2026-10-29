/*
 * stage-atomic.h — minimal 64-bit atomics for the core (no libobs, no C11
 * <stdatomic.h>, which MSVC only offers behind /experimental:c11atomics).
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef STAGE_ATOMIC_H
#define STAGE_ATOMIC_H

#include <stdint.h>

#if defined(_MSC_VER)
#include <intrin.h>

/* Returns the incremented value. Full barrier. */
static inline uint64_t stage_atomic_inc64(volatile uint64_t *v)
{
	return (uint64_t)_InterlockedIncrement64((volatile long long *)v);
}

/* Full-barrier load. */
static inline uint64_t stage_atomic_load64(volatile uint64_t *v)
{
	return (uint64_t)_InterlockedCompareExchange64((volatile long long *)v, 0, 0);
}

#else

static inline uint64_t stage_atomic_inc64(volatile uint64_t *v)
{
	return __atomic_add_fetch(v, 1, __ATOMIC_SEQ_CST);
}

static inline uint64_t stage_atomic_load64(volatile uint64_t *v)
{
	return __atomic_load_n(v, __ATOMIC_SEQ_CST);
}

#endif

#endif /* STAGE_ATOMIC_H */
