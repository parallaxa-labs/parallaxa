/*
 * stage-fx.h — public API of the Parallaxa FX module (src/fx) for the other
 * plugin modules. Include it only under #ifdef STAGE_HAS_FX.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The FX filter itself is reached through the core FX registry
 * (stage_fx_attach / stage_fx_get_info / stage_cmdq_push with
 * STAGE_CMD_FX_SET / STAGE_CMD_FX_PULSE). This header only adds what the
 * registry cannot carry: performance-guard events for the vendor's
 * `perf_warning` (vendor-api.md section 7).
 */
#ifndef STAGE_FX_H
#define STAGE_FX_H

#include <stdbool.h>

#include "stage-protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STAGE_FX_PERF_QUEUE_MAX 8

/* One perf_warning payload: {source, source_uuid, filter, cost_ms,
 * budget_ms, window_s, paused}. */
struct stage_fx_perf_event {
	char source[256];                      /* scene or source name (UTF-8)  */
	char source_uuid[STAGE_SCENE_UUID_MAX];
	char filter[STAGE_ID_MAX];             /* STAGE_FILTER_FX               */
	double cost_ms;                        /* measured cost per frame       */
	double budget_ms;                      /* STAGE_PERF_BUDGET_MS          */
	int window_s;                          /* STAGE_PERF_WINDOW_S           */
	char paused[16];                       /* STAGE_PERF_PAUSED_* or ""     */
};

/* Pops the oldest pending perf event (FIFO of STAGE_FX_PERF_QUEUE_MAX; the
 * oldest is dropped on overflow). Any thread; the vendor calls it from its
 * frontend tick and emits `perf_warning` for each event. Returns false when
 * the queue is empty. */
bool stage_fx_perf_pop(struct stage_fx_perf_event *out);

#ifdef __cplusplus
}
#endif

#endif /* STAGE_FX_H */
