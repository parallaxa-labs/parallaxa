/*
 * stage-vendor-events.c — vendor events from a tick callback
 * (docs/protocol/vendor-api.md section 7):
 *   state_changed  per rig, only on change, at most STAGE_STATE_EVENT_MAX_HZ;
 *                  values are compared quantized, so a settling spring or
 *                  float noise does not keep an idle camera "changing"
 *   move_finished  a running move id disappeared from the rig snapshot, or
 *                  the snapshot's last started move began and ended between
 *                  two pumps; cancelled unless the rig says it ran to its
 *                  end (else: a newer move replaced it, or a camera reset /
 *                  view / panic was requested meanwhile), or when it never
 *                  started (frozen, superseded before a tick, or expired /
 *                  purged in the queue)
 *   rig_changed    a rig appeared, disappeared, or its enabled flag flipped
 *   perf_warning   a Parallaxa FX reported a newly paused part
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Threads: the pump runs on the graphics thread (tick callback) and is the
 * only user of the track tables. Requests (obs-websocket thread) add
 * pending moves and cancel notes under `pend_lock`. Events are emitted with
 * no lock of ours held.
 */
#include <math.h>
#include <obs.h>
#include <pthread.h>
#include <string.h>

#include "presets/stage-target.h"
#include "stage-vendor.h"

#define STATE_INTERVAL_MS (1000u / STAGE_STATE_EVENT_MAX_HZ)
/* A queued move that never shows up in a rig snapshot is reported finished,
 * cancelled, after this long. Rigs drain the queue every tick, and an
 * undrained command expires after STAGE_CMDQ_DEFAULT_MAX_AGE_MS, so by then
 * the move can no longer start: it was expired, purged (rig removed) or
 * dropped. One that ran was seen, running or as the last started move. */
#define PENDING_TIMEOUT_MS (STAGE_CMDQ_DEFAULT_MAX_AGE_MS + 1000u)
#define MAX_PENDING 32
#define MAX_CANCEL_NOTES 16

/* Comparable (quantized) form of a state_changed payload. Built with
 * memset so memcmp is exact. */
struct snap {
	int32_t x, y;   /* 1/100 px    */
	int32_t zoom;   /* 1/10000     */
	int32_t rot;    /* 1/1000 deg  */
	int32_t shake;  /* 1/1000      */
	int32_t auto_i; /* 1/1000      */
	int32_t focus, amount;
	uint8_t auto_mode, view;
	bool moving, frozen;
	char move[STAGE_ID_MAX];
	char fx[STAGE_ID_MAX];
};

struct rig_track {
	bool used, seen;
	char uuid[STAGE_SCENE_UUID_MAX];
	char name[256];
	bool enabled;
	bool have_sent;
	struct snap sent;
	uint64_t last_emit_ms;
	uint64_t move_id; /* running move (0 = none) */
	char move[STAGE_ID_MAX];
	uint64_t max_seen_id;
	uint64_t last_id;   /* last started move id in the previous snapshot */
	uint64_t cancel_id; /* move id a reset / view / panic cancelled */
};

struct fx_track {
	bool used, seen;
	char uuid[STAGE_SCENE_UUID_MAX];
	char paused[16];
};

struct pending_move {
	bool used;
	char uuid[STAGE_SCENE_UUID_MAX];
	uint64_t id;
	char move[STAGE_ID_MAX];
	bool frozen;
	uint64_t at_ms;
};

struct finish_evt {
	char uuid[STAGE_SCENE_UUID_MAX];
	uint64_t id;
	char move[STAGE_ID_MAX];
	bool cancelled;
};

/* Graphics-thread state. */
static struct rig_track rigs[STAGE_MAX_RIGS];
static struct fx_track fxs[STAGE_MAX_FX];
static uint64_t last_panic_gen;

/* Shared with request threads. */
static pthread_mutex_t pend_lock = PTHREAD_MUTEX_INITIALIZER;
static struct pending_move pending[MAX_PENDING];
static char cancel_notes[MAX_CANCEL_NOTES][STAGE_SCENE_UUID_MAX];
static size_t ncancel;
static bool cancel_all;

static bool tick_added;

/* ------------------------------------------------------------------------- */
/* Notes from requests                                                        */
/* ------------------------------------------------------------------------- */

void stage_vendor_note_move(const char *scene_uuid, uint64_t id, const char *move, bool frozen)
{
	if (!scene_uuid || !id)
		return;
	uint64_t now = stage_core_now_ms();
	pthread_mutex_lock(&pend_lock);
	/* Reuse a free slot, else the oldest entry. */
	size_t pick = 0;
	for (size_t i = 0; i < MAX_PENDING; i++) {
		if (!pending[i].used) {
			pick = i;
			break;
		}
		if (pending[i].at_ms < pending[pick].at_ms)
			pick = i;
	}
	struct pending_move *p = &pending[pick];
	memset(p, 0, sizeof(*p));
	p->used = true;
	stage_strlcpy(p->uuid, scene_uuid, sizeof(p->uuid));
	p->id = id;
	stage_strlcpy(p->move, move, sizeof(p->move));
	p->frozen = frozen;
	p->at_ms = now;
	pthread_mutex_unlock(&pend_lock);
}

void stage_vendor_note_cancel(const char *scene_uuid)
{
	pthread_mutex_lock(&pend_lock);
	if (!scene_uuid || ncancel >= MAX_CANCEL_NOTES)
		cancel_all = true;
	else
		stage_strlcpy(cancel_notes[ncancel++], scene_uuid, STAGE_SCENE_UUID_MAX);
	pthread_mutex_unlock(&pend_lock);
}

/* ------------------------------------------------------------------------- */
/* Emission helpers                                                           */
/* ------------------------------------------------------------------------- */

static void emit_move_finished(const char *uuid, const char *name, uint64_t id, const char *move,
			       bool cancelled)
{
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "scene", name);
	obs_data_set_string(d, "scene_uuid", uuid);
	obs_data_set_int(d, "id", (long long)id);
	obs_data_set_string(d, "move", move);
	obs_data_set_bool(d, "cancelled", cancelled);
	stage_vendor_emit(STAGE_EVT_MOVE_FINISHED, d);
	obs_data_release(d);
}

static void emit_rig_changed(const struct rig_track *t, bool enabled)
{
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "scene", t->name);
	obs_data_set_string(d, "scene_uuid", t->uuid);
	obs_data_set_bool(d, "enabled", enabled);
	stage_vendor_emit(STAGE_EVT_RIG_CHANGED, d);
	obs_data_release(d);
}

static int32_t q(double v, double scale)
{
	if (!isfinite(v))
		return 0;
	double r = round(v * scale);
	if (r > 2.0e9)
		r = 2.0e9;
	if (r < -2.0e9)
		r = -2.0e9;
	return (int32_t)r;
}

static void make_snap(struct snap *s, const struct stage_state *st, const char *fx)
{
	memset(s, 0, sizeof(*s));
	s->x = q(st->x, 100.0);
	s->y = q(st->y, 100.0);
	s->zoom = q(st->zoom, 10000.0);
	s->rot = q(st->rot, 1000.0);
	s->shake = q(st->shake, 1000.0);
	s->auto_i = q(st->auto_intensity, 1000.0);
	s->focus = q(st->focus_depth, 1000.0);
	s->amount = q(st->dof_amount, 1000.0);
	s->auto_mode = st->auto_mode;
	s->view = st->view;
	s->moving = st->moving;
	s->frozen = st->frozen;
	if (st->moving)
		stage_strlcpy(s->move, st->move, sizeof(s->move));
	stage_strlcpy(s->fx, fx ? fx : st->fx_preset, sizeof(s->fx));
}

/* ------------------------------------------------------------------------- */
/* Pump                                                                       */
/* ------------------------------------------------------------------------- */

struct uuid_list {
	char ids[STAGE_MAX_FX > STAGE_MAX_RIGS ? STAGE_MAX_FX : STAGE_MAX_RIGS][STAGE_SCENE_UUID_MAX];
	size_t n, cap;
};

/* Pump-thread scratch (no per-frame allocation). */
static struct uuid_list scratch_list;

static bool collect_cb(const char *uuid, void *param)
{
	struct uuid_list *l = param;
	if (l->n < l->cap)
		stage_strlcpy(l->ids[l->n++], uuid, STAGE_SCENE_UUID_MAX);
	return l->n < l->cap;
}

static struct rig_track *rig_track_get(const char *uuid, bool *created)
{
	struct rig_track *free_slot = NULL;
	*created = false;
	for (size_t i = 0; i < STAGE_MAX_RIGS; i++) {
		if (rigs[i].used && strcmp(rigs[i].uuid, uuid) == 0)
			return &rigs[i];
		if (!rigs[i].used && !free_slot)
			free_slot = &rigs[i];
	}
	if (!free_slot)
		return NULL;
	memset(free_slot, 0, sizeof(*free_slot));
	free_slot->used = true;
	stage_strlcpy(free_slot->uuid, uuid, sizeof(free_slot->uuid));
	stage_source_name_by_uuid(uuid, free_slot->name, sizeof(free_slot->name));
	*created = true;
	return free_slot;
}

/* Applies cancel notes and pending-move bookkeeping; collects finish events
 * for queued moves that never ran. */
static size_t process_pending(uint64_t now, struct finish_evt *out, size_t cap)
{
	size_t n = 0;
	pthread_mutex_lock(&pend_lock);
	for (size_t r = 0; r < STAGE_MAX_RIGS; r++) {
		struct rig_track *t = &rigs[r];
		if (!t->used || !t->move_id)
			continue;
		bool hit = cancel_all;
		for (size_t c = 0; c < ncancel && !hit; c++)
			hit = strcmp(cancel_notes[c], t->uuid) == 0;
		if (hit)
			t->cancel_id = t->move_id;
	}
	ncancel = 0;
	cancel_all = false;

	for (size_t i = 0; i < MAX_PENDING; i++) {
		struct pending_move *p = &pending[i];
		if (!p->used)
			continue;
		struct rig_track *t = NULL;
		for (size_t r = 0; r < STAGE_MAX_RIGS && !t; r++) {
			if (rigs[r].used && strcmp(rigs[r].uuid, p->uuid) == 0)
				t = &rigs[r];
		}
		bool done = false;
		if (t && t->max_seen_id >= p->id) {
			/* Seen running (normal tracking reports it), or a later move
			 * started first: superseded without ever running. */
			done = true;
			if (t->move_id == p->id || t->max_seen_id == p->id)
				p->used = false; /* tracked as a running move */
		} else if (p->frozen) {
			done = true; /* camera_move while frozen: finished at once */
		} else if (now > p->at_ms && now - p->at_ms > PENDING_TIMEOUT_MS) {
			done = true; /* never ran (see PENDING_TIMEOUT_MS) */
		}
		if (done && p->used) {
			if (n < cap) {
				struct finish_evt *e = &out[n++];
				stage_strlcpy(e->uuid, p->uuid, sizeof(e->uuid));
				e->id = p->id;
				stage_strlcpy(e->move, p->move, sizeof(e->move));
				e->cancelled = true; /* every case here never ran */
			}
			p->used = false;
		}
	}
	pthread_mutex_unlock(&pend_lock);
	return n;
}

static void pump_rig(struct rig_track *t, bool created, const struct stage_state *st, uint64_t now)
{
	if (created || st->enabled != t->enabled) {
		t->enabled = st->enabled;
		emit_rig_changed(t, st->enabled);
	}

	/* Moves the rig ran before this track existed are not ours to report. */
	if (created)
		t->last_id = st->last_move_id;

	if (st->move_id != t->move_id) {
		if (t->move_id) {
			bool cancelled;
			if (st->last_move_id == t->move_id)
				cancelled = !st->last_move_done; /* the rig knows */
			else
				cancelled = st->move_id != 0 || st->last_move_id > t->move_id ||
					    t->cancel_id == t->move_id;
			emit_move_finished(t->uuid, t->name, t->move_id, t->move, cancelled);
		}
		t->move_id = st->move_id;
		stage_strlcpy(t->move, st->move, sizeof(t->move));
		if (st->move_id > t->max_seen_id)
			t->max_seen_id = st->move_id;
	}

	/* A move that started and ended between two pumps (a short move during
	 * a hitch) was never seen running: report it from the last started id.
	 * process_pending then drops its pending entry as seen. */
	uint64_t last = st->last_move_id;
	if (last != t->last_id && last != st->move_id && last > t->max_seen_id) {
		emit_move_finished(t->uuid, t->name, last, st->last_move, !st->last_move_done);
		t->max_seen_id = last;
	}
	t->last_id = last;

	struct stage_fx_info fi;
	const char *fx = stage_fx_get_info(t->uuid, &fi) ? fi.preset : NULL;
	struct snap s;
	make_snap(&s, st, fx);
	bool changed = !t->have_sent || memcmp(&s, &t->sent, sizeof(s)) != 0;
	if (changed && (!t->have_sent || now - t->last_emit_ms >= STATE_INTERVAL_MS)) {
		stage_source_name_by_uuid(t->uuid, t->name, sizeof(t->name)); /* renames */
		obs_data_t *d = obs_data_create();
		stage_vendor_fill_state(d, t->name, t->uuid, st, fx);
		stage_vendor_emit(STAGE_EVT_STATE_CHANGED, d);
		obs_data_release(d);
		t->sent = s;
		t->have_sent = true;
		t->last_emit_ms = now;
	}
}

static void pump_fx(uint64_t now)
{
	(void)now;
	struct uuid_list *l = &scratch_list;
	l->n = 0;
	l->cap = STAGE_MAX_FX;
	stage_fx_for_each(collect_cb, l);
	for (size_t i = 0; i < STAGE_MAX_FX; i++)
		fxs[i].seen = false;
	for (size_t k = 0; k < l->n; k++) {
		struct stage_fx_info fi;
		if (!stage_fx_get_info(l->ids[k], &fi))
			continue;
		struct fx_track *t = NULL, *free_slot = NULL;
		for (size_t i = 0; i < STAGE_MAX_FX && !t; i++) {
			if (fxs[i].used && strcmp(fxs[i].uuid, l->ids[k]) == 0)
				t = &fxs[i];
			else if (!fxs[i].used && !free_slot)
				free_slot = &fxs[i];
		}
		if (!t) {
			if (!free_slot)
				continue;
			t = free_slot;
			memset(t, 0, sizeof(*t));
			t->used = true;
			stage_strlcpy(t->uuid, l->ids[k], sizeof(t->uuid));
		}
		t->seen = true;
		fi.paused[sizeof(fi.paused) - 1] = '\0';
		if (fi.paused[0] && strcmp(fi.paused, t->paused) != 0) {
			char name[256];
			stage_source_name_by_uuid(t->uuid, name, sizeof(name));
			obs_data_t *d = obs_data_create();
			obs_data_set_string(d, "source", name);
			obs_data_set_string(d, "source_uuid", t->uuid);
			obs_data_set_string(d, "filter", STAGE_FILTER_FX);
			/* stage_fx_info has no measured cost; the guard only pauses
			 * above the budget, so the budget is a lower bound. */
			obs_data_set_double(d, "cost_ms", STAGE_PERF_BUDGET_MS);
			obs_data_set_double(d, "budget_ms", STAGE_PERF_BUDGET_MS);
			obs_data_set_int(d, "window_s", STAGE_PERF_WINDOW_S);
			obs_data_set_string(d, "paused", fi.paused);
			stage_vendor_emit(STAGE_EVT_PERF_WARNING, d);
			obs_data_release(d);
		}
		stage_strlcpy(t->paused, fi.paused, sizeof(t->paused));
	}
	for (size_t i = 0; i < STAGE_MAX_FX; i++) {
		if (fxs[i].used && !fxs[i].seen)
			fxs[i].used = false;
	}
}

void stage_vendor_events_pump(uint64_t now_ms)
{
	if (!stage_vendor_can_emit())
		return;

	/* A panic (vendor, hotkey or Tools menu) cancels every running move. */
	uint64_t gen = stage_panic_generation();
	if (gen != last_panic_gen) {
		last_panic_gen = gen;
		for (size_t i = 0; i < STAGE_MAX_RIGS; i++) {
			if (rigs[i].used && rigs[i].move_id)
				rigs[i].cancel_id = rigs[i].move_id;
		}
	}

	struct finish_evt fin[MAX_PENDING];
	size_t nfin = process_pending(now_ms, fin, MAX_PENDING);

	struct uuid_list *l = &scratch_list;
	l->n = 0;
	l->cap = STAGE_MAX_RIGS;
	stage_rig_for_each(collect_cb, l);
	for (size_t i = 0; i < STAGE_MAX_RIGS; i++)
		rigs[i].seen = false;

	for (size_t k = 0; k < l->n; k++) {
		struct stage_state st;
		if (!stage_rig_get_state(l->ids[k], &st))
			continue;
		bool created;
		struct rig_track *t = rig_track_get(l->ids[k], &created);
		if (!t)
			continue;
		t->seen = true;
		pump_rig(t, created, &st, now_ms);
	}

	/* Rigs that went away: their move ends cancelled, the rig is disabled. */
	for (size_t i = 0; i < STAGE_MAX_RIGS; i++) {
		struct rig_track *t = &rigs[i];
		if (!t->used || t->seen)
			continue;
		if (t->move_id)
			emit_move_finished(t->uuid, t->name, t->move_id, t->move, true);
		if (t->enabled)
			emit_rig_changed(t, false);
		t->used = false;
	}

	for (size_t i = 0; i < nfin; i++) {
		char name[256] = "";
		for (size_t r = 0; r < STAGE_MAX_RIGS && !name[0]; r++) {
			if (rigs[r].used && strcmp(rigs[r].uuid, fin[i].uuid) == 0)
				stage_strlcpy(name, rigs[r].name, sizeof(name));
		}
		if (!name[0])
			stage_source_name_by_uuid(fin[i].uuid, name, sizeof(name));
		emit_move_finished(fin[i].uuid, name, fin[i].id, fin[i].move, fin[i].cancelled);
	}

	pump_fx(now_ms);
}

void stage_vendor_events_reset(void)
{
	memset(rigs, 0, sizeof(rigs));
	memset(fxs, 0, sizeof(fxs));
	last_panic_gen = stage_panic_generation();
	pthread_mutex_lock(&pend_lock);
	memset(pending, 0, sizeof(pending));
	ncancel = 0;
	cancel_all = false;
	pthread_mutex_unlock(&pend_lock);
}

/* ------------------------------------------------------------------------- */
/* Tick callback                                                              */
/* ------------------------------------------------------------------------- */

static void events_tick(void *param, float seconds)
{
	(void)param;
	(void)seconds;
	stage_vendor_events_pump(stage_core_now_ms());
}

void stage_vendor_events_start(void)
{
	if (tick_added)
		return;
	stage_vendor_events_reset();
	obs_add_tick_callback(events_tick, NULL);
	tick_added = true;
}

void stage_vendor_events_stop(void)
{
	if (!tick_added)
		return;
	obs_remove_tick_callback(events_tick, NULL);
	tick_added = false;
}
