/*
 * stage-target.h — scene / source lookup, rig and FX filter lookup, saved
 * views storage. Shared by the vendor requests, hotkeys and Tools menu.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Thread safety: any thread (libobs source lookups use its recursive
 * sources mutex). Views are stored in the scene's private settings
 * (STAGE_SCENE_KEY_VIEWS) and every read/modify/write goes through one
 * process-wide mutex, so the vendor, the hotkeys and the Camera Rig panel
 * never interleave their writes. The private settings are only WRITTEN on
 * the UI thread (obs_data has no lock and the frontend serializes them
 * there when it saves the scene collection): a put/delete from another
 * thread updates an in-memory overlay that every reader sees at once and
 * queues an OBS_TASK_UI task that persists it. Other modules that write
 * views must use stage_views_put()/_delete().
 */
#ifndef STAGE_TARGET_H
#define STAGE_TARGET_H

#include <obs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stage-protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Finds a source by uuid or, failing that, by name. NULL when absent. The
 * caller releases the result. */
obs_source_t *stage_source_resolve(const char *name_or_uuid);

/* The cached program scene (stage_current_scene_uuid). Caller releases. */
obs_source_t *stage_program_scene_source(void);

/* First filter of the given id on parent (e.g. STAGE_FILTER_CAMERA_RIG).
 * Caller releases. */
obs_source_t *stage_find_filter(obs_source_t *parent, const char *filter_id);

/* Copies the source name (or "" when unknown) for a uuid. */
void stage_source_name_by_uuid(const char *uuid, char *out, size_t cap);

/* ------------------------------------------------------------------------- */
/* Saved views (vendor-api.md 5.1)                                           */
/* ------------------------------------------------------------------------- */

struct stage_view {
	uint8_t slot; /* STAGE_VIEW_SLOT_MIN..MAX */
	char name[STAGE_VIEW_NAME_MAX];
	float x, y, zoom, rot;
	uint8_t transition; /* enum stage_transition */
	uint32_t duration_ms;
};

/* Fills out[] (sorted by slot, at most 9) and returns the count. Invalid
 * stored entries are skipped. */
size_t stage_views_list(obs_source_t *scene, struct stage_view out[STAGE_VIEW_SLOT_MAX]);
bool stage_views_get(obs_source_t *scene, uint8_t slot, struct stage_view *out);
/* Every write below gives its slot a new write generation (never 0), put in
 * *gen when gen is not NULL; it orders the writes for stage_views_put_if()
 * and stage_views_delete_if(). */
/* Inserts or replaces the view of v->slot. */
bool stage_views_put(obs_source_t *scene, const struct stage_view *v, uint32_t *gen);
/* Edits a stored view in place under one lock hold (read, merge, write),
 * keeping its pose exactly as stored: name (NULL = keep), transition
 * (< 0 = keep), duration_ms (< 0 = keep). out gets the stored result.
 * False when the slot was empty. */
bool stage_views_edit(obs_source_t *scene, uint8_t slot, const char *name, int transition,
		      long long duration_ms, struct stage_view *out, uint32_t *gen);
/* False when the slot was empty. */
bool stage_views_delete(obs_source_t *scene, uint8_t slot, uint32_t *gen);
/* A later re-store / re-delete of the write that returned gen (the rig's UI
 * task after a vendor view_save / view_delete): done only while that is
 * still the slot's latest write, checked under the same lock, so it never
 * overwrites a newer one (the panel's Save view in between). It keeps the
 * generation. gen 0: an ordinary put / delete. False when skipped. */
bool stage_views_put_if(obs_source_t *scene, const struct stage_view *v, uint32_t gen);
bool stage_views_delete_if(obs_source_t *scene, uint8_t slot, uint32_t gen);
/* Frees overlays whose UI task has not run yet and the write generations
 * (module unload). */
void stage_views_discard_pending(void);

struct stage_cmd;
/* Builds the STAGE_CMD_VIEW_APPLY command for a stored view: transition
 * (< 0 = the view's own), duration_ms (< 0 = the view's own; always 0 for
 * cut) and the transition's ease. */
void stage_view_make_apply_cmd(const char *scene_uuid, const struct stage_view *v, int transition,
			       long long duration_ms, struct stage_cmd *out);

#ifdef __cplusplus
}
#endif

#endif /* STAGE_TARGET_H */
