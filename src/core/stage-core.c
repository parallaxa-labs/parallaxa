/*
 * stage-core.c — rig / FX registries, state snapshots, program-scene cache,
 * unload hooks. No libobs calls (logging aside), so it is unit-testable.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * All mutexes are statically initialized and live for the whole process, so
 * the core is usable from the first obs_module_load line until the DLL is
 * unmapped (late hotkey or websocket threads never see a destroyed lock).
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "stage-core.h"

/* stage-cmdq.c internals */
void stage_cmdq_shutdown_internal(void);

void stage_strlcpy(char *dst, const char *src, size_t dst_size)
{
	if (!dst || !dst_size)
		return;
	if (!src)
		src = "";
	size_t n = strlen(src);
	if (n >= dst_size)
		n = dst_size - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static bool uuid_ok(const char *uuid)
{
	if (!uuid || !uuid[0])
		return false;
	return strlen(uuid) < STAGE_SCENE_UUID_MAX;
}

/* ------------------------------------------------------------------------- */
/* Generic registry (rigs and FX filters)                                    */
/* ------------------------------------------------------------------------- */

#define REG_MAX (STAGE_MAX_FX > STAGE_MAX_RIGS ? STAGE_MAX_FX : STAGE_MAX_RIGS)

struct reg_entry {
	char uuid[STAGE_SCENE_UUID_MAX];
	const void *ops;
	void *ud;
	bool used;
};

struct registry {
	pthread_mutex_t lock;
	size_t cap;
	const char *what;
	uint32_t purge_mask; /* command types purged when an entry goes away */
	struct reg_entry *e; /* cap entries */
};

/* The entry tables live in zero-initialised storage (.bss): embedding them in
 * the initialised registry structs (the mutex initialiser is non-zero) would
 * store every byte of them in the DLL image. */
static struct reg_entry rig_entries[STAGE_MAX_RIGS];
static struct reg_entry fx_entries[STAGE_MAX_FX];

static struct registry rigs = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.cap = STAGE_MAX_RIGS,
	.what = "rig",
	.purge_mask = STAGE_CMD_MASK_RIG,
	.e = rig_entries,
};

static struct registry fxs = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.cap = STAGE_MAX_FX,
	.what = "fx",
	.purge_mask = STAGE_CMD_MASK_FX,
	.e = fx_entries,
};

static int reg_find_locked(struct registry *r, const char *uuid)
{
	for (size_t i = 0; i < r->cap; i++) {
		if (r->e[i].used && strcmp(r->e[i].uuid, uuid) == 0)
			return (int)i;
	}
	return -1;
}

static bool reg_add(struct registry *r, const char *uuid, const void *ops, void *ud)
{
	if (!uuid_ok(uuid) || !ops || !ud)
		return false;

	bool ok = false;
	pthread_mutex_lock(&r->lock);
	int i = reg_find_locked(r, uuid);
	if (i >= 0) {
		/* Same owner re-registering: refresh ops. Other owner: refuse. */
		if (r->e[i].ud == ud) {
			r->e[i].ops = ops;
			ok = true;
		}
	} else {
		for (size_t k = 0; k < r->cap; k++) {
			if (!r->e[k].used) {
				stage_strlcpy(r->e[k].uuid, uuid, sizeof(r->e[k].uuid));
				r->e[k].ops = ops;
				r->e[k].ud = ud;
				r->e[k].used = true;
				ok = true;
				break;
			}
		}
	}
	pthread_mutex_unlock(&r->lock);
	return ok;
}

static bool reg_remove(struct registry *r, const char *uuid, void *ud)
{
	if (!uuid_ok(uuid))
		return false;
	bool removed = false;
	pthread_mutex_lock(&r->lock);
	int i = reg_find_locked(r, uuid);
	if (i >= 0 && r->e[i].ud == ud) {
		memset(&r->e[i], 0, sizeof(r->e[i]));
		removed = true;
	}
	pthread_mutex_unlock(&r->lock);
	if (removed)
		stage_cmdq_purge_types(uuid, r->purge_mask);
	return removed;
}

static void reg_remove_all(struct registry *r, void *ud)
{
	char gone[REG_MAX][STAGE_SCENE_UUID_MAX];
	size_t n = 0;
	pthread_mutex_lock(&r->lock);
	for (size_t i = 0; i < r->cap; i++) {
		if (r->e[i].used && r->e[i].ud == ud) {
			stage_strlcpy(gone[n++], r->e[i].uuid, STAGE_SCENE_UUID_MAX);
			memset(&r->e[i], 0, sizeof(r->e[i]));
		}
	}
	pthread_mutex_unlock(&r->lock);
	for (size_t i = 0; i < n; i++)
		stage_cmdq_purge_types(gone[i], r->purge_mask);
}

/* Calls fn(entry) under the lock; false when uuid is not registered. */
static bool reg_with(struct registry *r, const char *uuid,
		     void (*fn)(const struct reg_entry *e, void *param), void *param)
{
	if (!uuid_ok(uuid))
		return false;
	bool found = false;
	pthread_mutex_lock(&r->lock);
	int i = reg_find_locked(r, uuid);
	if (i >= 0) {
		found = true;
		if (fn)
			fn(&r->e[i], param);
	}
	pthread_mutex_unlock(&r->lock);
	return found;
}

static size_t reg_for_each(struct registry *r, bool (*cb)(const char *uuid, void *param), void *param)
{
	char ids[REG_MAX][STAGE_SCENE_UUID_MAX];
	size_t n = 0;
	pthread_mutex_lock(&r->lock);
	for (size_t i = 0; i < r->cap; i++) {
		if (r->e[i].used)
			stage_strlcpy(ids[n++], r->e[i].uuid, STAGE_SCENE_UUID_MAX);
	}
	pthread_mutex_unlock(&r->lock);

	size_t calls = 0;
	for (size_t i = 0; i < n && cb; i++) {
		calls++;
		if (!cb(ids[i], param))
			break;
	}
	return cb ? calls : n;
}

static size_t reg_count(struct registry *r)
{
	size_t n = 0;
	pthread_mutex_lock(&r->lock);
	for (size_t i = 0; i < r->cap; i++)
		n += r->e[i].used ? 1 : 0;
	pthread_mutex_unlock(&r->lock);
	return n;
}

/* Empties the registry at unload; leftovers mean a filter outlived OBS's
 * source teardown, which is logged (outside the lock). */
static size_t reg_clear(struct registry *r)
{
	char left[REG_MAX][STAGE_SCENE_UUID_MAX];
	size_t n = 0;
	pthread_mutex_lock(&r->lock);
	for (size_t i = 0; i < r->cap; i++) {
		if (r->e[i].used)
			stage_strlcpy(left[n++], r->e[i].uuid, STAGE_SCENE_UUID_MAX);
		memset(&r->e[i], 0, sizeof(r->e[i]));
	}
	pthread_mutex_unlock(&r->lock);
	for (size_t i = 0; i < n; i++)
		SLOG_WARN("%s for '%s' still registered at unload", r->what, left[i]);
	return n;
}

/* ------------------------------------------------------------------------- */
/* Rig registry                                                              */
/* ------------------------------------------------------------------------- */

struct rig_call_args {
	struct stage_state *state;
	struct stage_layer_info *layers;
	size_t cap;
	size_t count;
	const struct stage_cmd *cmd;
	void (*fn)(void *ud, void *param);
	void *param;
};

static void rig_get_state_cb(const struct reg_entry *e, void *p)
{
	struct rig_call_args *a = p;
	const struct stage_rig_ops *ops = e->ops;
	memset(a->state, 0, sizeof(*a->state));
	if (ops->get_state)
		ops->get_state(e->ud, a->state);
}

static void rig_list_layers_cb(const struct reg_entry *e, void *p)
{
	struct rig_call_args *a = p;
	const struct stage_rig_ops *ops = e->ops;
	a->count = 0;
	if (ops->list_layers && a->layers && a->cap) {
		a->count = ops->list_layers(e->ud, a->layers, a->cap);
		if (a->count > a->cap)
			a->count = a->cap;
	}
}

static void rig_apply_cb(const struct reg_entry *e, void *p)
{
	struct rig_call_args *a = p;
	const struct stage_rig_ops *ops = e->ops;
	if (ops->apply_cmd)
		ops->apply_cmd(e->ud, a->cmd);
}

static void generic_call_cb(const struct reg_entry *e, void *p)
{
	struct rig_call_args *a = p;
	a->fn(e->ud, a->param);
}

bool stage_rig_register(const char *scene_uuid, const struct stage_rig_ops *ops, void *ud)
{
	return reg_add(&rigs, scene_uuid, ops, ud);
}

void stage_rig_unregister(const char *scene_uuid, void *ud)
{
	reg_remove(&rigs, scene_uuid, ud);
}

void stage_rig_unregister_all(void *ud)
{
	reg_remove_all(&rigs, ud);
}

bool stage_rig_exists(const char *scene_uuid)
{
	return reg_with(&rigs, scene_uuid, NULL, NULL);
}

bool stage_rig_get_state(const char *scene_uuid, struct stage_state *out)
{
	struct stage_state tmp;
	struct rig_call_args a = {.state = out ? out : &tmp};
	return reg_with(&rigs, scene_uuid, rig_get_state_cb, &a);
}

bool stage_rig_list_layers(const char *scene_uuid, struct stage_layer_info *out, size_t cap,
			   size_t *count)
{
	struct rig_call_args a = {.layers = out, .cap = cap};
	bool found = reg_with(&rigs, scene_uuid, rig_list_layers_cb, &a);
	if (count)
		*count = found ? a.count : 0;
	return found;
}

bool stage_rig_apply(const char *scene_uuid, const struct stage_cmd *cmd)
{
	if (!cmd)
		return false;
	struct rig_call_args a = {.cmd = cmd};
	return reg_with(&rigs, scene_uuid, rig_apply_cb, &a);
}

bool stage_rig_call(const char *scene_uuid, void (*fn)(void *rig_ud, void *param), void *param)
{
	if (!fn)
		return false;
	struct rig_call_args a = {.fn = fn, .param = param};
	return reg_with(&rigs, scene_uuid, generic_call_cb, &a);
}

size_t stage_rig_for_each(bool (*cb)(const char *scene_uuid, void *param), void *param)
{
	return reg_for_each(&rigs, cb, param);
}

size_t stage_rig_count(void)
{
	return reg_count(&rigs);
}

/* ------------------------------------------------------------------------- */
/* FX registry                                                               */
/* ------------------------------------------------------------------------- */

struct fx_call_args {
	struct stage_fx_info *info;
	const struct stage_cmd *cmd;
};

static void fx_get_info_cb(const struct reg_entry *e, void *p)
{
	struct fx_call_args *a = p;
	const struct stage_fx_ops *ops = e->ops;
	memset(a->info, 0, sizeof(*a->info));
	if (ops->get_info)
		ops->get_info(e->ud, a->info);
}

static void fx_apply_cb(const struct reg_entry *e, void *p)
{
	struct fx_call_args *a = p;
	const struct stage_fx_ops *ops = e->ops;
	if (ops->apply_cmd)
		ops->apply_cmd(e->ud, a->cmd);
}

bool stage_fx_attach(const char *target_uuid, const struct stage_fx_ops *ops, void *ud)
{
	return reg_add(&fxs, target_uuid, ops, ud);
}

void stage_fx_detach(const char *target_uuid, void *ud)
{
	reg_remove(&fxs, target_uuid, ud);
}

void stage_fx_detach_all(void *ud)
{
	reg_remove_all(&fxs, ud);
}

bool stage_fx_exists(const char *target_uuid)
{
	return reg_with(&fxs, target_uuid, NULL, NULL);
}

bool stage_fx_get_info(const char *target_uuid, struct stage_fx_info *out)
{
	struct stage_fx_info tmp;
	struct fx_call_args a = {.info = out ? out : &tmp};
	return reg_with(&fxs, target_uuid, fx_get_info_cb, &a);
}

bool stage_fx_apply(const char *target_uuid, const struct stage_cmd *cmd)
{
	if (!cmd)
		return false;
	struct fx_call_args a = {.cmd = cmd};
	return reg_with(&fxs, target_uuid, fx_apply_cb, &a);
}

bool stage_fx_call(const char *target_uuid, void (*fn)(void *fx_ud, void *param), void *param)
{
	if (!fn)
		return false;
	struct rig_call_args a = {.fn = fn, .param = param};
	return reg_with(&fxs, target_uuid, generic_call_cb, &a);
}

size_t stage_fx_for_each(bool (*cb)(const char *target_uuid, void *param), void *param)
{
	return reg_for_each(&fxs, cb, param);
}

size_t stage_fx_count(void)
{
	return reg_count(&fxs);
}

/* ------------------------------------------------------------------------- */
/* Double-buffered state snapshot                                            */
/* ------------------------------------------------------------------------- */

bool stage_state_buf_init(struct stage_state_buf *sb)
{
	if (!sb)
		return false;
	memset(sb, 0, sizeof(*sb));
	pthread_mutex_t *m = malloc(sizeof(*m));
	if (!m)
		return false;
	if (pthread_mutex_init(m, NULL) != 0) {
		free(m);
		return false;
	}
	sb->lock_ = m;
	return true;
}

void stage_state_buf_free(struct stage_state_buf *sb)
{
	if (!sb || !sb->lock_)
		return;
	pthread_mutex_destroy((pthread_mutex_t *)sb->lock_);
	free(sb->lock_);
	sb->lock_ = NULL;
}

bool stage_state_publish(struct stage_state_buf *sb, const struct stage_state *s)
{
	if (!sb || !s || !sb->lock_)
		return false;
	/* Only this (single) writer changes `front`, so reading it unlocked is
	 * safe; readers only ever touch buf[front] under the lock. */
	int front = sb->front;
	if (memcmp(&sb->buf[front], s, sizeof(*s)) == 0)
		return false;
	int back = front ^ 1;
	sb->buf[back] = *s;

	pthread_mutex_lock((pthread_mutex_t *)sb->lock_);
	sb->front = back;
	sb->seq++;
	pthread_mutex_unlock((pthread_mutex_t *)sb->lock_);
	return true;
}

void stage_state_read(struct stage_state_buf *sb, struct stage_state *out, uint64_t *seq)
{
	if (!out)
		return;
	if (!sb || !sb->lock_) {
		memset(out, 0, sizeof(*out));
		if (seq)
			*seq = 0;
		return;
	}
	pthread_mutex_lock((pthread_mutex_t *)sb->lock_);
	*out = sb->buf[sb->front];
	if (seq)
		*seq = sb->seq;
	pthread_mutex_unlock((pthread_mutex_t *)sb->lock_);
}

/* ------------------------------------------------------------------------- */
/* Program scene cache                                                       */
/* ------------------------------------------------------------------------- */

static pthread_mutex_t scene_lock = PTHREAD_MUTEX_INITIALIZER;
static char program_scene[STAGE_SCENE_UUID_MAX];

bool stage_current_scene_uuid(char out[STAGE_SCENE_UUID_MAX])
{
	if (!out)
		return false;
	pthread_mutex_lock(&scene_lock);
	memcpy(out, program_scene, STAGE_SCENE_UUID_MAX);
	pthread_mutex_unlock(&scene_lock);
	out[STAGE_SCENE_UUID_MAX - 1] = '\0';
	return out[0] != '\0';
}

void stage_core_set_program_scene(const char *uuid)
{
	pthread_mutex_lock(&scene_lock);
	stage_strlcpy(program_scene, uuid_ok(uuid) ? uuid : "", sizeof(program_scene));
	pthread_mutex_unlock(&scene_lock);
}

/* ------------------------------------------------------------------------- */
/* Lifecycle and unload hooks                                                */
/* ------------------------------------------------------------------------- */

#define MAX_UNLOAD_HOOKS 16

static pthread_mutex_t hook_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
	void (*fn)(void *ud);
	void *ud;
} hooks[MAX_UNLOAD_HOOKS];
static size_t hook_count;

bool stage_core_on_unload(void (*fn)(void *ud), void *ud)
{
	if (!fn)
		return false;
	bool ok = false;
	pthread_mutex_lock(&hook_lock);
	if (hook_count < MAX_UNLOAD_HOOKS) {
		hooks[hook_count].fn = fn;
		hooks[hook_count].ud = ud;
		hook_count++;
		ok = true;
	}
	pthread_mutex_unlock(&hook_lock);
	return ok;
}

void stage_core_init(void)
{
	stage_core_set_program_scene(NULL);
}

void stage_core_shutdown(void)
{
	/* Run hooks LIFO; each one is popped before it runs, without the lock,
	 * so a hook may itself call core functions. */
	for (;;) {
		void (*fn)(void *) = NULL;
		void *ud = NULL;
		pthread_mutex_lock(&hook_lock);
		if (hook_count) {
			hook_count--;
			fn = hooks[hook_count].fn;
			ud = hooks[hook_count].ud;
		}
		pthread_mutex_unlock(&hook_lock);
		if (!fn)
			break;
		fn(ud);
	}

	reg_clear(&rigs);
	reg_clear(&fxs);

	struct stage_cmdq_stats st;
	stage_cmdq_get_stats(&st);
	SLOG_INFO("cmdq: pushed %llu, drained %llu, dropped %llu, rejected %llu, expired %llu, "
		  "purged %llu, high-water %zu/%d",
		  (unsigned long long)st.pushed, (unsigned long long)st.drained,
		  (unsigned long long)st.dropped, (unsigned long long)st.rejected,
		  (unsigned long long)st.expired, (unsigned long long)st.purged, st.high_water,
		  STAGE_CMDQ_CAPACITY);
	stage_cmdq_shutdown_internal();
	stage_core_set_program_scene(NULL);
}
