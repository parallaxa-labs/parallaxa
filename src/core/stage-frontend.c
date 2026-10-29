/*
 * stage-frontend.c — frontend event / save callback observers and the
 * program-scene cache refresh (UI thread).
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <obs-frontend-api.h>
#include <obs.h>
#include <pthread.h>
#include <string.h>

#include "stage-core.h"
#include "stage-frontend.h"

/* Exactly one of ev / save is set. */
struct observer {
	stage_frontend_event_cb ev;
	stage_frontend_save_cb save;
	void *ud;
};

static inline bool observer_eq(const struct observer *a, const struct observer *b)
{
	return a->ev == b->ev && a->save == b->save && a->ud == b->ud;
}

struct observer_list {
	struct observer items[STAGE_MAX_FRONTEND_OBSERVERS];
	size_t count;
};

static pthread_mutex_t obs_lock = PTHREAD_MUTEX_INITIALIZER;
static struct observer_list event_obs;
static struct observer_list save_obs;
static bool attached;

static bool list_add(struct observer_list *l, struct observer o)
{
	if (!o.ev && !o.save)
		return false;
	bool ok = false;
	pthread_mutex_lock(&obs_lock);
	for (size_t i = 0; i < l->count; i++) {
		if (observer_eq(&l->items[i], &o)) {
			ok = true;
			goto out;
		}
	}
	if (l->count < STAGE_MAX_FRONTEND_OBSERVERS) {
		l->items[l->count++] = o;
		ok = true;
	}
out:
	pthread_mutex_unlock(&obs_lock);
	return ok;
}

static void list_remove(struct observer_list *l, struct observer o)
{
	pthread_mutex_lock(&obs_lock);
	for (size_t i = 0; i < l->count; i++) {
		if (observer_eq(&l->items[i], &o)) {
			/* keep registration order */
			memmove(&l->items[i], &l->items[i + 1], (l->count - i - 1) * sizeof(l->items[0]));
			l->count--;
			break;
		}
	}
	pthread_mutex_unlock(&obs_lock);
}

/* Snapshot of a list so observers run without the lock (they may add or
 * remove observers while being called). */
static size_t list_copy(struct observer_list *l, struct observer *out)
{
	pthread_mutex_lock(&obs_lock);
	size_t n = l->count;
	memcpy(out, l->items, n * sizeof(out[0]));
	pthread_mutex_unlock(&obs_lock);
	return n;
}

bool stage_frontend_add_event_cb(stage_frontend_event_cb cb, void *ud)
{
	return list_add(&event_obs, (struct observer){.ev = cb, .ud = ud});
}

void stage_frontend_remove_event_cb(stage_frontend_event_cb cb, void *ud)
{
	list_remove(&event_obs, (struct observer){.ev = cb, .ud = ud});
}

bool stage_frontend_add_save_cb(stage_frontend_save_cb cb, void *ud)
{
	return list_add(&save_obs, (struct observer){.save = cb, .ud = ud});
}

void stage_frontend_remove_save_cb(stage_frontend_save_cb cb, void *ud)
{
	list_remove(&save_obs, (struct observer){.save = cb, .ud = ud});
}

void stage_frontend_refresh_program_scene(void)
{
	obs_source_t *scene = obs_frontend_get_current_scene();
	const char *uuid = scene ? obs_source_get_uuid(scene) : NULL;
	stage_core_set_program_scene(uuid);
	obs_source_release(scene);
}

static void on_frontend_event(enum obs_frontend_event event, void *private_data)
{
	(void)private_data;

	/* 1. Keep the program-scene cache current before observers run. */
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		stage_frontend_refresh_program_scene();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		/* scenes are about to be destroyed; nothing may target them */
		stage_core_set_program_scene(NULL);
		break;
	default:
		break;
	}

	/* 2. Fan out. */
	struct observer obs[STAGE_MAX_FRONTEND_OBSERVERS];
	size_t n = list_copy(&event_obs, obs);
	for (size_t i = 0; i < n; i++)
		obs[i].ev(event, obs[i].ud);

	if (event == OBS_FRONTEND_EVENT_EXIT)
		stage_core_set_program_scene(NULL);
}

static void on_frontend_save(obs_data_t *save_data, bool saving, void *private_data)
{
	(void)private_data;
	struct observer obs[STAGE_MAX_FRONTEND_OBSERVERS];
	size_t n = list_copy(&save_obs, obs);
	for (size_t i = 0; i < n; i++)
		obs[i].save(save_data, saving, obs[i].ud);
}

void stage_frontend_attach(void)
{
	if (attached)
		return;
	obs_frontend_add_event_callback(on_frontend_event, NULL);
	obs_frontend_add_save_callback(on_frontend_save, NULL);
	attached = true;
}

void stage_frontend_detach(void)
{
	if (!attached)
		return;
	obs_frontend_remove_save_callback(on_frontend_save, NULL);
	obs_frontend_remove_event_callback(on_frontend_event, NULL);
	attached = false;

	pthread_mutex_lock(&obs_lock);
	event_obs.count = 0;
	save_obs.count = 0;
	pthread_mutex_unlock(&obs_lock);
}
