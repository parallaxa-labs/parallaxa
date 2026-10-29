/*
 * stage-vendor.h — internals of the obs-websocket vendor "parallaxa"
 * (docs/protocol/vendor-api.md). The module entry points stage_vendor_init()
 * and stage_vendor_shutdown() are declared in stage-modules.h.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Files:
 *   stage-vendor.c          registration, event emission (the only file that
 *                           includes obs-websocket-api.h: its _ph is static)
 *   stage-vendor-requests.c request validation and handlers
 *   stage-vendor-events.c   tick-driven state_changed / move_finished /
 *                           rig_changed / perf_warning
 *   stage-hotkeys.c         Parallaxa and "AI Live Studio" OBS hotkeys
 *
 * Threads: requests run on the obs-websocket thread, events are produced on
 * the graphics thread (tick callback), hotkeys on the hotkey thread. None of
 * them mutates scene items: state changes go through the command queue.
 */
#ifndef STAGE_VENDOR_H
#define STAGE_VENDOR_H

#include <obs.h>
#include <stdbool.h>
#include <stdint.h>

#include "stage-core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* stage_cmd.ease for camera_move without "ease": keep every key's own ease
 * (stage_move_eval_ex ease_override < 0). Any value >= STAGE_EASE_COUNT
 * means "no override". */
#define STAGE_CMD_EASE_NONE 0xFFu

/* ---- requests (stage-vendor-requests.c) --------------------------------- */

/* Runs one request synchronously: validates req, fills res with the
 * documented responseData (ok, ... or ok:false, code, error). Unknown
 * request types answer bad_param. Any thread; used by the obs-websocket
 * callbacks and by the unit tests. */
void stage_vendor_dispatch(const char *request_type, obs_data_t *req, obs_data_t *res);

/* ---- events (stage-vendor-events.c) -------------------------------------- */

typedef void (*stage_vendor_emit_fn)(const char *event_type, obs_data_t *data, void *ud);

/* Replaces the event sink (default: obs-websocket). Tests only. */
void stage_vendor_set_emit_sink(stage_vendor_emit_fn fn, void *ud);

/* Emits one vendor event now (any thread). False when no sink is active. */
bool stage_vendor_emit(const char *event_type, obs_data_t *data);
/* True when events have somewhere to go (vendor registered or test sink). */
bool stage_vendor_can_emit(void);

/* Writes the get_state / state_changed body (without "ok") into d.
 * fx_preset: the Parallaxa FX preset of the scene, or NULL to use st's. */
void stage_vendor_fill_state(obs_data_t *d, const char *scene_name, const char *scene_uuid,
			     const struct stage_state *st, const char *fx_preset);
/* obs-websocket's own "server_enabled" setting from its config file
 * (<plugin_config>/obs-websocket/config.json, obs-websocket >= 5.5):
 * 1 enabled, 0 disabled, -1 unknown (no file, unreadable, no key). */
int stage_vendor_ws_server_enabled(const char *config_json_path);
/* Rounds a value for JSON output (4 decimals; non-finite -> 0). */
double stage_vendor_r4(double v);

/* One event pass: compares every rig / FX snapshot with the last one sent.
 * Called from the tick callback with stage_core_now_ms(); tests call it
 * with a fake clock. Graphics thread (or the test thread). */
void stage_vendor_events_pump(uint64_t now_ms);
/* Forgets all tracked state (shutdown, tests). */
void stage_vendor_events_reset(void);

/* Requests tell the event tracker about the moves they queued and about
 * commands that cancel a running move (camera_reset, view_apply, panic), so
 * move_finished can report `cancelled` (vendor-api.md section 7). */
void stage_vendor_note_move(const char *scene_uuid, uint64_t id, const char *move, bool frozen);
void stage_vendor_note_cancel(const char *scene_uuid /* NULL = every rig */);

/* Starts / stops the tick callback that drives the pump. */
void stage_vendor_events_start(void);
void stage_vendor_events_stop(void);

/* ---- hotkeys (stage-hotkeys.c) ------------------------------------------- */

void stage_hotkeys_register(void);
void stage_hotkeys_unregister(void);

#ifdef __cplusplus
}
#endif

#endif /* STAGE_VENDOR_H */
