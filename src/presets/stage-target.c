/*
 * stage-target.c — source lookup helpers and saved views storage.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <obs.h>
#include <pthread.h>
#include <string.h>
#include <util/bmem.h>

#include "stage-core.h"
#include "stage-json.h"
#include "stage-target.h"

obs_source_t *stage_source_resolve(const char *name_or_uuid)
{
	if (!name_or_uuid || !name_or_uuid[0])
		return NULL;
	obs_source_t *s = NULL;
	/* uuids are 36 characters; anything else can only be a name. */
	if (strlen(name_or_uuid) == 36)
		s = obs_get_source_by_uuid(name_or_uuid);
	if (!s)
		s = obs_get_source_by_name(name_or_uuid);
	return s;
}

obs_source_t *stage_program_scene_source(void)
{
	char uuid[STAGE_SCENE_UUID_MAX];
	if (!stage_current_scene_uuid(uuid))
		return NULL;
	return obs_get_source_by_uuid(uuid);
}

struct find_filter_ctx {
	const char *id;
	obs_source_t *found;
};

static void find_filter_cb(obs_source_t *parent, obs_source_t *child, void *param)
{
	(void)parent;
	struct find_filter_ctx *c = param;
	if (c->found)
		return;
	const char *id = obs_source_get_unversioned_id(child);
	if (id && strcmp(id, c->id) == 0)
		c->found = obs_source_get_ref(child);
}

obs_source_t *stage_find_filter(obs_source_t *parent, const char *filter_id)
{
	if (!parent || !filter_id)
		return NULL;
	struct find_filter_ctx c = {filter_id, NULL};
	obs_source_enum_filters(parent, find_filter_cb, &c);
	return c.found;
}

void stage_source_name_by_uuid(const char *uuid, char *out, size_t cap)
{
	if (!out || !cap)
		return;
	out[0] = '\0';
	if (!uuid || !uuid[0])
		return;
	obs_source_t *s = obs_get_source_by_uuid(uuid);
	if (s) {
		stage_strlcpy(out, obs_source_get_name(s), cap);
		obs_source_release(s);
	}
}

/* ------------------------------------------------------------------------- */
/* Views: STAGE_SCENE_KEY_VIEWS = [{slot, name, camera:{x,y,zoom,rot},       */
/*        transition, duration_ms}, ...] (the view_info shape)                */
/* ------------------------------------------------------------------------- */

static pthread_mutex_t views_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *const view_keys[] = {"slot", "name", "camera", "transition", "duration_ms"};
static const char *const cam_keys[] = {"x", "y", "zoom", "rot"};

/* Parses one stored view; false when malformed (it is then ignored). */
static bool view_from_data(obs_data_t *d, struct stage_view *v)
{
	struct sj_err e;
	long long slot = 0, dur = 0;
	const char *name = NULL;
	int tr = 0;
	obs_data_t *cam = NULL;
	double x = 0, y = 0, zoom = 1, rot = 0;
	bool ok = sj_only_keys(d, view_keys, 5, &e) &&
		  sj_int(d, "slot", STAGE_VIEW_SLOT_MIN, STAGE_VIEW_SLOT_MAX, &slot, &e) == SJ_OK &&
		  sj_str(d, "name", STAGE_VIEW_NAME_MAX - 1, &name, &e) == SJ_OK &&
		  stage_view_name_valid(name) &&
		  sj_enum(d, "transition", stage_transition_names, STAGE_TRANSITION_COUNT, &tr, &e) ==
			  SJ_OK &&
		  sj_int(d, "duration_ms", 0, STAGE_SET_DURATION_MAX, &dur, &e) == SJ_OK &&
		  sj_obj(d, "camera", &cam, &e) == SJ_OK;
	ok = ok && sj_only_keys(cam, cam_keys, 4, &e) &&
	     sj_num(cam, "x", -1e5, 1e5, &x, &e) == SJ_OK &&
	     sj_num(cam, "y", -1e5, 1e5, &y, &e) == SJ_OK &&
	     sj_num(cam, "zoom", STAGE_ZOOM_MIN, STAGE_ZOOM_MAX, &zoom, &e) == SJ_OK &&
	     sj_num(cam, "rot", STAGE_ROT_MIN, STAGE_ROT_MAX, &rot, &e) == SJ_OK;
	obs_data_release(cam);
	if (!ok)
		return false;
	memset(v, 0, sizeof(*v));
	v->slot = (uint8_t)slot;
	stage_strlcpy(v->name, name, sizeof(v->name));
	v->x = (float)x;
	v->y = (float)y;
	v->zoom = (float)zoom;
	v->rot = (float)rot;
	v->transition = (uint8_t)tr;
	v->duration_ms = (uint32_t)dur;
	return true;
}

static obs_data_t *view_to_data(const struct stage_view *v)
{
	obs_data_t *d = obs_data_create();
	obs_data_t *cam = obs_data_create();
	obs_data_set_int(d, "slot", v->slot);
	obs_data_set_string(d, "name", v->name);
	obs_data_set_double(cam, "x", v->x);
	obs_data_set_double(cam, "y", v->y);
	obs_data_set_double(cam, "zoom", v->zoom);
	obs_data_set_double(cam, "rot", v->rot);
	obs_data_set_obj(d, "camera", cam);
	obs_data_set_string(d, "transition",
			    stage_transition_names[v->transition < STAGE_TRANSITION_COUNT ? v->transition
										  : 1]);
	obs_data_set_int(d, "duration_ms", v->duration_ms);
	obs_data_release(cam);
	return d;
}

/* Writes go to the scene's private settings on the UI thread only: libobs
 * obs_data has no lock of its own and the frontend serializes the same
 * private settings there (scene collection save). A write from another
 * thread (obs-websocket, hotkeys) becomes a pending overlay under
 * views_lock, so every reader sees it at once, and a UI task persists it.
 * Reads of the private settings happen under views_lock, so they never
 * overlap one of our writes; overlapping a frontend save is read/read. */
struct views_pending {
	struct views_pending *next;
	char uuid[STAGE_SCENE_UUID_MAX];
	struct stage_view all[STAGE_VIEW_SLOT_MAX];
	bool valid[STAGE_VIEW_SLOT_MAX];
};
static struct views_pending *pending_head; /* views_lock */

static struct views_pending **pending_find_locked(const char *uuid)
{
	struct views_pending **pp = &pending_head;
	while (*pp && strcmp((*pp)->uuid, uuid) != 0)
		pp = &(*pp)->next;
	return pp;
}

static void pending_drop_locked(const char *uuid)
{
	struct views_pending **pp = pending_find_locked(uuid);
	if (*pp) {
		struct views_pending *p = *pp;
		*pp = p->next;
		bfree(p);
	}
}

/* Write generations: every put / edit / delete gives its slot a new value
 * of one process-wide counter (never 0), so the rig's later re-store of a
 * vendor write can tell that a newer write (the panel's Save view) landed
 * in between and leave it alone. Kept per scene until module unload. */
struct views_gen {
	struct views_gen *next;
	char uuid[STAGE_SCENE_UUID_MAX];
	uint32_t gen[STAGE_VIEW_SLOT_MAX];
};
static struct views_gen *gen_head; /* views_lock */
static uint32_t gen_seq;           /* views_lock */

/* The slot's generation (0: never written since load). views_lock held. */
static uint32_t gen_get_locked(obs_source_t *scene, uint8_t slot)
{
	const char *uuid = obs_source_get_uuid(scene);
	for (const struct views_gen *g = gen_head; g && uuid; g = g->next)
		if (strcmp(g->uuid, uuid) == 0)
			return g->gen[slot - 1];
	return 0;
}

/* Gives the slot a new generation and returns it. views_lock held. */
static uint32_t gen_bump_locked(obs_source_t *scene, uint8_t slot)
{
	const char *uuid = obs_source_get_uuid(scene);
	if (!uuid || !uuid[0])
		return 0;
	struct views_gen *g = gen_head;
	while (g && strcmp(g->uuid, uuid) != 0)
		g = g->next;
	if (!g) {
		g = bzalloc(sizeof(*g));
		stage_strlcpy(g->uuid, uuid, sizeof(g->uuid));
		g->next = gen_head;
		gen_head = g;
	}
	if (++gen_seq == 0)
		gen_seq = 1;
	g->gen[slot - 1] = gen_seq;
	return gen_seq;
}

/* Reads the stored views (the pending overlay first) into out[slot-1]
 * (valid[] marks filled slots). views_lock held. */
static void views_read_locked(obs_source_t *scene, struct stage_view out[STAGE_VIEW_SLOT_MAX],
			      bool valid[STAGE_VIEW_SLOT_MAX])
{
	memset(valid, 0, sizeof(bool) * STAGE_VIEW_SLOT_MAX);
	const char *uuid = obs_source_get_uuid(scene);
	const struct views_pending *p = uuid ? *pending_find_locked(uuid) : NULL;
	if (p) {
		memcpy(out, p->all, sizeof(p->all));
		memcpy(valid, p->valid, sizeof(p->valid));
		return;
	}
	obs_data_t *priv = obs_source_get_private_settings(scene);
	if (!priv)
		return;
	obs_data_array_t *arr = obs_data_get_array(priv, STAGE_SCENE_KEY_VIEWS);
	size_t n = obs_data_array_count(arr);
	for (size_t i = 0; i < n; i++) {
		obs_data_t *d = obs_data_array_item(arr, i);
		struct stage_view v;
		if (d && view_from_data(d, &v) && !valid[v.slot - 1]) {
			out[v.slot - 1] = v;
			valid[v.slot - 1] = true;
		}
		obs_data_release(d);
	}
	obs_data_array_release(arr);
	obs_data_release(priv);
}

/* Writes the private settings. UI thread, views_lock held. */
static void views_store_locked(obs_source_t *scene, const struct stage_view in[STAGE_VIEW_SLOT_MAX],
			       const bool valid[STAGE_VIEW_SLOT_MAX])
{
	obs_data_t *priv = obs_source_get_private_settings(scene);
	if (!priv)
		return;
	obs_data_array_t *arr = obs_data_array_create();
	for (int i = 0; i < STAGE_VIEW_SLOT_MAX; i++) {
		if (!valid[i])
			continue;
		obs_data_t *d = view_to_data(&in[i]);
		obs_data_array_push_back(arr, d);
		obs_data_release(d);
	}
	obs_data_set_array(priv, STAGE_SCENE_KEY_VIEWS, arr);
	obs_data_array_release(arr);
	obs_data_release(priv);
}

/* UI task: persists every pending overlay (param unused). It owns no
 * memory, so a task that never runs (OBS exiting before Qt dispatches it)
 * leaks nothing; stage_views_discard_pending() frees the overlays. With
 * several tasks queued the first writes and the others find nothing. */
static void views_persist_task(void *param)
{
	(void)param;
	pthread_mutex_lock(&views_lock);
	while (pending_head) {
		struct views_pending *p = pending_head;
		pending_head = p->next;
		obs_source_t *scene = obs_get_source_by_uuid(p->uuid);
		if (scene)
			views_store_locked(scene, p->all, p->valid);
		obs_source_release(scene);
		bfree(p);
	}
	pthread_mutex_unlock(&views_lock);
}

/* Stores new views. On the UI thread they are written at once (and replace
 * any overlay); on any other thread they become the scene's overlay and the
 * caller must call views_queue() with the returned true AFTER views_lock is
 * released (a UI handler may run the task inline). views_lock held. */
static bool views_write_locked(obs_source_t *scene, const struct stage_view in[STAGE_VIEW_SLOT_MAX],
			       const bool valid[STAGE_VIEW_SLOT_MAX])
{
	const char *uuid = obs_source_get_uuid(scene);
	if (!uuid || !uuid[0])
		return false;
	if (obs_in_task_thread(OBS_TASK_UI)) {
		views_store_locked(scene, in, valid);
		pending_drop_locked(uuid);
		return false;
	}
	struct views_pending *p = *pending_find_locked(uuid);
	if (!p) {
		p = bzalloc(sizeof(*p));
		stage_strlcpy(p->uuid, uuid, sizeof(p->uuid));
		p->next = pending_head;
		pending_head = p;
	}
	memcpy(p->all, in, sizeof(p->all));
	memcpy(p->valid, valid, sizeof(p->valid));
	return true;
}

static void views_queue(bool queue)
{
	if (queue)
		obs_queue_task(OBS_TASK_UI, views_persist_task, NULL, false);
}

void stage_views_discard_pending(void)
{
	pthread_mutex_lock(&views_lock);
	while (pending_head) {
		struct views_pending *p = pending_head;
		pending_head = p->next;
		bfree(p);
	}
	while (gen_head) {
		struct views_gen *g = gen_head;
		gen_head = g->next;
		bfree(g);
	}
	pthread_mutex_unlock(&views_lock);
}

size_t stage_views_list(obs_source_t *scene, struct stage_view out[STAGE_VIEW_SLOT_MAX])
{
	if (!scene || !out)
		return 0;
	struct stage_view all[STAGE_VIEW_SLOT_MAX];
	bool valid[STAGE_VIEW_SLOT_MAX];
	pthread_mutex_lock(&views_lock);
	views_read_locked(scene, all, valid);
	pthread_mutex_unlock(&views_lock);
	size_t n = 0;
	for (int i = 0; i < STAGE_VIEW_SLOT_MAX; i++) {
		if (valid[i])
			out[n++] = all[i];
	}
	return n;
}

bool stage_views_get(obs_source_t *scene, uint8_t slot, struct stage_view *out)
{
	if (!scene || slot < STAGE_VIEW_SLOT_MIN || slot > STAGE_VIEW_SLOT_MAX)
		return false;
	struct stage_view all[STAGE_VIEW_SLOT_MAX];
	bool valid[STAGE_VIEW_SLOT_MAX];
	pthread_mutex_lock(&views_lock);
	views_read_locked(scene, all, valid);
	pthread_mutex_unlock(&views_lock);
	if (!valid[slot - 1])
		return false;
	if (out)
		*out = all[slot - 1];
	return true;
}

/* Sets v's slot. only_gen != 0: only while that is still the slot's
 * generation (a re-store, which keeps it); otherwise a new write whose
 * generation goes to *gen. */
static bool views_put(obs_source_t *scene, const struct stage_view *v, uint32_t only_gen,
		      uint32_t *gen)
{
	if (!scene || !v || v->slot < STAGE_VIEW_SLOT_MIN || v->slot > STAGE_VIEW_SLOT_MAX ||
	    !stage_view_name_valid(v->name))
		return false;
	struct stage_view all[STAGE_VIEW_SLOT_MAX];
	bool valid[STAGE_VIEW_SLOT_MAX];
	bool queued = false;
	pthread_mutex_lock(&views_lock);
	bool ok = !only_gen || gen_get_locked(scene, v->slot) == only_gen;
	if (ok) {
		views_read_locked(scene, all, valid);
		all[v->slot - 1] = *v;
		valid[v->slot - 1] = true;
		queued = views_write_locked(scene, all, valid);
		uint32_t g = only_gen ? only_gen : gen_bump_locked(scene, v->slot);
		if (gen)
			*gen = g;
	}
	pthread_mutex_unlock(&views_lock);
	views_queue(queued);
	return ok;
}

bool stage_views_put(obs_source_t *scene, const struct stage_view *v, uint32_t *gen)
{
	return views_put(scene, v, 0, gen);
}

bool stage_views_put_if(obs_source_t *scene, const struct stage_view *v, uint32_t gen)
{
	return views_put(scene, v, gen, NULL);
}

bool stage_views_edit(obs_source_t *scene, uint8_t slot, const char *name, int transition,
		      long long duration_ms, struct stage_view *out, uint32_t *gen)
{
	if (!scene || slot < STAGE_VIEW_SLOT_MIN || slot > STAGE_VIEW_SLOT_MAX ||
	    (name && !stage_view_name_valid(name)))
		return false;
	struct stage_view all[STAGE_VIEW_SLOT_MAX];
	bool valid[STAGE_VIEW_SLOT_MAX];
	bool queued = false;
	pthread_mutex_lock(&views_lock);
	views_read_locked(scene, all, valid);
	bool had = valid[slot - 1];
	if (had) {
		/* Merged into the stored view in this one hold: the pose is
		 * never rewritten, whatever canvas it was saved on. */
		struct stage_view *v = &all[slot - 1];
		if (name)
			stage_strlcpy(v->name, name, sizeof(v->name));
		if (transition >= 0 && transition < STAGE_TRANSITION_COUNT)
			v->transition = (uint8_t)transition;
		if (duration_ms >= 0)
			v->duration_ms = (uint32_t)duration_ms;
		if (out)
			*out = *v;
		queued = views_write_locked(scene, all, valid);
		uint32_t g = gen_bump_locked(scene, slot);
		if (gen)
			*gen = g;
	}
	pthread_mutex_unlock(&views_lock);
	views_queue(queued);
	return had;
}

/* Clears the slot; only_gen as in views_put. False when it was empty (or
 * the generation moved on). */
static bool views_delete(obs_source_t *scene, uint8_t slot, uint32_t only_gen, uint32_t *gen)
{
	if (!scene || slot < STAGE_VIEW_SLOT_MIN || slot > STAGE_VIEW_SLOT_MAX)
		return false;
	struct stage_view all[STAGE_VIEW_SLOT_MAX];
	bool valid[STAGE_VIEW_SLOT_MAX];
	bool queued = false, had = false;
	pthread_mutex_lock(&views_lock);
	if (!only_gen || gen_get_locked(scene, slot) == only_gen) {
		views_read_locked(scene, all, valid);
		had = valid[slot - 1];
	}
	if (had) {
		valid[slot - 1] = false;
		queued = views_write_locked(scene, all, valid);
		uint32_t g = only_gen ? only_gen : gen_bump_locked(scene, slot);
		if (gen)
			*gen = g;
	}
	pthread_mutex_unlock(&views_lock);
	views_queue(queued);
	return had;
}

bool stage_views_delete(obs_source_t *scene, uint8_t slot, uint32_t *gen)
{
	return views_delete(scene, slot, 0, gen);
}

bool stage_views_delete_if(obs_source_t *scene, uint8_t slot, uint32_t gen)
{
	return views_delete(scene, slot, gen, NULL);
}

void stage_view_make_apply_cmd(const char *scene_uuid, const struct stage_view *v, int transition,
			       long long duration_ms, struct stage_cmd *out)
{
	memset(out, 0, sizeof(*out));
	out->type = STAGE_CMD_VIEW_APPLY;
	stage_strlcpy(out->scene_uuid, scene_uuid, sizeof(out->scene_uuid));
	out->intensity = -1.0f;
	uint8_t tr = (uint8_t)(transition >= 0 && transition < STAGE_TRANSITION_COUNT ? transition
										     : v->transition);
	if (tr >= STAGE_TRANSITION_COUNT)
		tr = STAGE_TRANSITION_SMOOTH;
	out->u.view.slot = v->slot;
	out->u.view.transition = tr;
	out->u.view.has_transition = true;
	stage_strlcpy(out->u.view.name, v->name, sizeof(out->u.view.name));
	out->u.view.cam.x = v->x;
	out->u.view.cam.y = v->y;
	out->u.view.cam.zoom = v->zoom;
	out->u.view.cam.rot = v->rot;
	out->u.view.cam.mask = STAGE_CAM_X | STAGE_CAM_Y | STAGE_CAM_ZOOM | STAGE_CAM_ROT;
	out->duration_ms = tr == STAGE_TRANSITION_CUT
				   ? 0
				   : (uint32_t)(duration_ms >= 0 ? duration_ms : v->duration_ms);
	out->ease = stage_transition_ease[tr];
}
