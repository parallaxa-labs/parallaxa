/*
 * stage-frontend.h — fan-out of OBS frontend events and save callbacks to
 * the plugin modules (one OBS registration, owned by plugin-main.c).
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Observers run on the thread OBS uses for the frontend callback (the UI
 * thread), in registration order, with no core lock held. The core updates
 * its program-scene cache BEFORE observers see SCENE_CHANGED, so
 * stage_current_scene_uuid() is already current inside an observer.
 *
 * Save callbacks get the frontend save data object: each module stores its
 * data (e.g. hotkey bindings) under its own key, such as "stage.camera".
 */
#ifndef STAGE_FRONTEND_H
#define STAGE_FRONTEND_H

#include <obs-frontend-api.h>
#include <obs.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STAGE_MAX_FRONTEND_OBSERVERS 16

typedef void (*stage_frontend_event_cb)(enum obs_frontend_event event, void *ud);
typedef void (*stage_frontend_save_cb)(obs_data_t *save_data, bool saving, void *ud);

/* Return false when the table is full or cb is NULL. Adding the same
 * (cb, ud) twice is a no-op that returns true. Any thread. */
bool stage_frontend_add_event_cb(stage_frontend_event_cb cb, void *ud);
void stage_frontend_remove_event_cb(stage_frontend_event_cb cb, void *ud);
bool stage_frontend_add_save_cb(stage_frontend_save_cb cb, void *ud);
void stage_frontend_remove_save_cb(stage_frontend_save_cb cb, void *ud);

/* plugin-main.c: register / unregister the single OBS frontend callbacks. */
void stage_frontend_attach(void);
void stage_frontend_detach(void);

/* Re-reads the current program scene into the core cache. UI thread only
 * (calls obs_frontend_get_current_scene). */
void stage_frontend_refresh_program_scene(void);

#ifdef __cplusplus
}
#endif

#endif /* STAGE_FRONTEND_H */
