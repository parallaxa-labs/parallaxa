/*
 * stage-core.h — Parallaxa core runtime: command queue, rig / FX
 * registries, double-buffered state snapshots, program-scene cache, logging.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Threading:
 *   - The command queue is the ONLY path from the obs-websocket, hotkey and UI
 *     threads into tick code. Producers call stage_cmdq_push()/try_push() from
 *     any thread; each rig / FX filter drains the commands addressed to its
 *     target from video_tick (graphics thread) with
 *     stage_cmdq_drain_types(uuid, STAGE_CMD_MASK_RIG or STAGE_CMD_MASK_FX, ...)
 *     and checks stage_panic_generation() for the global panic.
 *   - Registries and snapshots are protected by internal mutexes. Core never
 *     calls into OBS while holding one of its locks. Registry ops (get_state,
 *     list_layers, ...) run while the registry lock is held so that a filter
 *     cannot be destroyed under a reader; ops must therefore be short, must
 *     not call into OBS and must not call back into the registry.
 *
 * Notes on the API:
 *   - Drain with stage_cmdq_drain(scene_uuid, cb, ud), or preferably
 *     stage_cmdq_drain_types.
 *   - Rigs are reached through stage_rig_exists() / stage_rig_get_state() /
 *     stage_rig_call(). No raw rig pointer is handed out: it could be
 *     destroyed under the caller.
 *   - A command that needs a vendor reply gets its id from
 *     stage_cmdq_push_id() (or next_id first).
 *
 * This header does not include libobs (except util/base.h for logging, see
 * STAGE_CORE_NO_OBS) so the core can be unit tested standalone.
 */
#ifndef STAGE_CORE_H
#define STAGE_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stage-protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Version                                                                   */
/* ------------------------------------------------------------------------- */

#ifndef STAGE_VERSION
#define STAGE_VERSION "0.0.0-dev" /* set by CMake from project(VERSION) */
#endif

/* ------------------------------------------------------------------------- */
/* Logging: every line is prefixed "[parallaxa] " (STAGE_LOG_PREFIX).        */
/* ------------------------------------------------------------------------- */

#ifndef STAGE_LOG_PREFIX
#define STAGE_LOG_PREFIX "[parallaxa] "
#endif

#ifndef STAGE_CORE_NO_OBS
#include <util/base.h>
#define STAGE_LOG(level, fmt, ...) blog(level, STAGE_LOG_PREFIX fmt, ##__VA_ARGS__)
#define SLOG_DEBUG(fmt, ...) STAGE_LOG(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define SLOG_INFO(fmt, ...) STAGE_LOG(LOG_INFO, fmt, ##__VA_ARGS__)
#define SLOG_WARN(fmt, ...) STAGE_LOG(LOG_WARNING, fmt, ##__VA_ARGS__)
#define SLOG_ERR(fmt, ...) STAGE_LOG(LOG_ERROR, fmt, ##__VA_ARGS__)
#else
#include <stdio.h>
#define SLOG_DEBUG(fmt, ...) ((void)0)
#define SLOG_INFO(fmt, ...) fprintf(stderr, STAGE_LOG_PREFIX "info: " fmt "\n", ##__VA_ARGS__)
#define SLOG_WARN(fmt, ...) fprintf(stderr, STAGE_LOG_PREFIX "warning: " fmt "\n", ##__VA_ARGS__)
#define SLOG_ERR(fmt, ...) fprintf(stderr, STAGE_LOG_PREFIX "error: " fmt "\n", ##__VA_ARGS__)
#endif

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

/* Called first in obs_module_load / last in obs_module_unload. Shutdown runs
 * the registered unload hooks (LIFO), empties the queue and registries and
 * logs the queue statistics. */
void stage_core_init(void);
void stage_core_shutdown(void);

/* Modules register cleanup to run at obs_module_unload (LIFO order). Use it
 * to free anything allocated at module load (move tables, presets, ...).
 * Returns false when the hook table (16 entries) is full. */
bool stage_core_on_unload(void (*fn)(void *ud), void *ud);

/* Monotonic milliseconds (QueryPerformanceCounter based on Windows). */
uint64_t stage_core_now_ms(void);

/* ------------------------------------------------------------------------- */
/* Command queue                                                             */
/* ------------------------------------------------------------------------- */

enum stage_cmd_type {
	STAGE_CMD_CAMERA_MOVE,   /* u.move                                         */
	STAGE_CMD_CAMERA_SET,    /* u.set                                          */
	STAGE_CMD_CAMERA_RESET,  /* duration_ms only                               */
	STAGE_CMD_CAMERA_AUTO,   /* u.autom; intensity < 0 keeps the current one   */
	STAGE_CMD_FX_SET,        /* u.fx                                           */
	STAGE_CMD_FX_PULSE,      /* u.pulse; intensity                             */
	STAGE_CMD_LAYER_SET,     /* u.layer                                        */
	STAGE_CMD_DOF_SET,       /* u.dof (rack focus = duration_ms + ease)        */
	STAGE_CMD_VIEW_SAVE,     /* u.view: persist view (graphics thread writes)  */
	STAGE_CMD_VIEW_APPLY,    /* u.view: move to view pose                      */
	STAGE_CMD_VIEW_DELETE,   /* u.view.slot                                    */
	STAGE_CMD_FREEZE,        /* u.freeze                                       */
	STAGE_CMD_PANIC,         /* per-target panic (global panic: stage_panic_trigger) */
	STAGE_CMD_TYPE_COUNT
};

/* stage_cam_target.mask bits (absent channels keep the current target). */
#define STAGE_CAM_X 0x01u
#define STAGE_CAM_Y 0x02u
#define STAGE_CAM_ZOOM 0x04u
#define STAGE_CAM_ROT 0x08u

struct stage_cam_target {
	float x, y, zoom, rot;
	uint8_t mask;     /* STAGE_CAM_* */
	bool relative;    /* camera_set relative:true (values are added, then clamped) */
};

/* stage_cmd.u.layer.mask bits. */
#define STAGE_LAYER_DEPTH 0x01u   /* depth is set                          */
#define STAGE_LAYER_ROLE 0x02u    /* role is set                           */
#define STAGE_LAYER_UNENROL 0x04u /* depth:null or role:"none"             */

/* stage_cmd.u.dof.mask bits. */
#define STAGE_DOF_FOCUS 0x01u
#define STAGE_DOF_AMOUNT 0x02u
/* Rack focus (hotkeys): the rig tick picks the character's / farthest
 * background layer's depth and, with depth of field off, the panel's Blur
 * amount, so the hotkey thread reads no settings. */
#define STAGE_DOF_RACK_CHAR 0x40u
#define STAGE_DOF_RACK_BG 0x80u

/* stage_cmd.u.freeze.mode values. */
enum stage_freeze_mode { STAGE_FREEZE_OFF, STAGE_FREEZE_ON, STAGE_FREEZE_TOGGLE };

/* Size of the free-form fx_set params JSON (validated by the vendor module). */
#define STAGE_FX_PARAMS_JSON_MAX 384
#define STAGE_PULSE_NAME_MAX 24

struct stage_cmd {
	enum stage_cmd_type type;
	uint64_t id;          /* stage_cmdq_next_id(); 0 = assigned at push        */
	/* Target scene (rig commands) or scene/source (fx commands) uuid.
	 * "" = current program scene, resolved at push. */
	char scene_uuid[STAGE_SCENE_UUID_MAX];
	uint32_t duration_ms;
	uint8_t ease;         /* enum stage_ease                                    */
	float intensity;      /* move/pulse intensity, auto intensity (< 0 = keep)  */
	union {
		struct {
			char move[STAGE_ID_MAX];
		} move;
		struct stage_cam_target set;
		struct {
			uint8_t mode; /* enum stage_auto_mode */
		} autom;
		struct {
			char preset[STAGE_ID_MAX]; /* "" = params only */
			char params_json[STAGE_FX_PARAMS_JSON_MAX];
			/* FX toggle hotkeys: 1 + enum stage_fx_toggle (0 =
			 * none), flipped by the FX tick against the target the
			 * commands before it left, so presses within one tick
			 * flip in turn. */
			uint8_t toggle;
		} fx;
		struct {
			char effect[STAGE_PULSE_NAME_MAX]; /* stage_pulse_names[] */
		} pulse;
		struct {
			int64_t item_id;
			float depth;
			uint8_t role; /* enum stage_role */
			uint8_t mask; /* STAGE_LAYER_* */
		} layer;
		struct {
			float focus_depth;
			float amount;
			uint8_t mask; /* STAGE_DOF_* */
		} dof;
		struct {
			uint8_t slot;       /* STAGE_VIEW_SLOT_MIN..MAX            */
			uint8_t transition; /* enum stage_transition               */
			bool has_transition;
			/* view_save keep_pose: cam is the stored pose, re-stored
			 * as given (not clamped to the current canvas). */
			bool keep_pose;
			/* view_save / view_delete from the vendor: the write
			 * generation the store gave the slot (it is stored
			 * already); the rig's re-store of it is skipped once a
			 * newer write landed (stage_views_put_if). 0 = none. */
			uint32_t gen;
			/* view_apply from the next / previous view hotkeys: +1 / -1
			 * = the saved view after / before the one last applied,
			 * picked by the rig tick (slot unused), so the hotkey
			 * thread never reads the scene's stored views. */
			int8_t step;
			char name[STAGE_VIEW_NAME_MAX];
			struct stage_cam_target cam; /* resolved pose (save/apply) */
		} view;
		struct {
			uint8_t mode; /* enum stage_freeze_mode */
		} freeze;
	} u;
};

/* Type masks for stage_cmdq_drain_types()/purge_types(). A rig and an FX
 * filter may share a target (both on one scene), so each drains only its
 * own command types: the rig STAGE_CMD_MASK_RIG, Parallaxa FX STAGE_CMD_MASK_FX. */
#define STAGE_CMD_MASK(t) (1u << (unsigned)(t))
#define STAGE_CMD_MASK_ALL ((1u << (unsigned)STAGE_CMD_TYPE_COUNT) - 1u)
#define STAGE_CMD_MASK_FX (STAGE_CMD_MASK(STAGE_CMD_FX_SET) | STAGE_CMD_MASK(STAGE_CMD_FX_PULSE))
#define STAGE_CMD_MASK_RIG (STAGE_CMD_MASK_ALL & ~STAGE_CMD_MASK_FX)

/* Ring capacity. */
#define STAGE_CMDQ_CAPACITY 128
/* Commands not drained within this age are discarded (their target is not
 * ticking, e.g. the rig was removed). See stage_cmdq_set_max_age_ms(). */
#define STAGE_CMDQ_DEFAULT_MAX_AGE_MS 5000

/* Pushes a copy of *c. When the queue is full the OLDEST command is dropped
 * (and counted) so the newest intent always wins. Resolves an empty
 * scene_uuid to the current program scene and assigns an id when c->id == 0.
 * Returns false (and queues nothing) when c is NULL, its type is invalid, or
 * the scene is empty and no program scene is known. Any thread. */
bool stage_cmdq_push(const struct stage_cmd *c);

/* Like stage_cmdq_push() but never drops: returns false when the queue is
 * full (vendor answers "busy"). Any thread. */
bool stage_cmdq_try_push(const struct stage_cmd *c);

/* push (may_drop = true) or try_push (false) that also reports the command id
 * in *out_id (0 when nothing was queued). The vendor needs it for its
 * {ok:true, id} answer; alternatively set c->id = stage_cmdq_next_id() before
 * pushing, since push/try_push take a const command and cannot return the id
 * they assign.
 *
 * Expiry: a full ring first discards commands older than the max age, so a
 * queue that nobody drains does not stay "busy" forever. Commands for a
 * target that never drains still occupy slots for up to the max age, and
 * push() may evict them (or other targets' commands) on overflow, so the
 * vendor must check stage_rig_exists()/stage_fx_exists() and answer
 * "no_rig"/"no_fx" instead of queueing for a target that is not there. */
bool stage_cmdq_push_id(const struct stage_cmd *c, bool may_drop, uint64_t *out_id);

/* Removes every queued command whose scene_uuid equals scene_uuid (NULL =
 * every command) in FIFO order and calls cb for each, WITHOUT holding the
 * queue lock (cb may push new commands; those are delivered by the next
 * drain). Commands older than the max age are discarded instead. Returns the
 * number of commands delivered. Graphics thread. */
size_t stage_cmdq_drain(const char *scene_uuid,
			void (*cb)(const struct stage_cmd *cmd, void *ud), void *ud);

/* stage_cmdq_drain() restricted to the command types in type_mask
 * (STAGE_CMD_MASK_RIG / STAGE_CMD_MASK_FX). Other types stay queued. */
size_t stage_cmdq_drain_types(const char *scene_uuid, uint32_t type_mask,
			      void (*cb)(const struct stage_cmd *cmd, void *ud), void *ud);

/* Discards queued commands for scene_uuid (NULL = all). Returns the count. */
size_t stage_cmdq_purge(const char *scene_uuid);
size_t stage_cmdq_purge_types(const char *scene_uuid, uint32_t type_mask);

/* Next command id: positive, unique and increasing per OBS session. */
uint64_t stage_cmdq_next_id(void);

/* Max age (ms) before an undrained command expires; 0 disables expiry. */
void stage_cmdq_set_max_age_ms(uint32_t ms);

struct stage_cmdq_stats {
	uint64_t pushed;   /* accepted by push/try_push                  */
	uint64_t drained;  /* delivered to a drain callback              */
	uint64_t dropped;  /* oldest dropped on overflow (push)          */
	uint64_t rejected; /* refused by try_push (full) or invalid      */
	uint64_t expired;  /* discarded by age                           */
	uint64_t purged;   /* discarded by purge / panic                 */
	size_t depth;      /* currently queued                           */
	size_t high_water; /* max depth seen                             */
};
void stage_cmdq_get_stats(struct stage_cmdq_stats *out);

/* Global panic (vendor "panic", hotkey): bypasses the queue. Purges every
 * queued command and bumps the panic generation. Every rig / FX filter
 * compares stage_panic_generation() with the value it saw last in its tick
 * and, when it changed, performs the panic (camera home over 200 ms, auto
 * off, unfreeze, FX neutral, DoF 0, cancel moves). Any thread. */
void stage_panic_trigger(void);
uint64_t stage_panic_generation(void);

/* ------------------------------------------------------------------------- */
/* State snapshot (double-buffered, one writer = graphics thread)            */
/* ------------------------------------------------------------------------- */

/* Rig state as reported by vendor get_state / state_changed. Writers must
 * zero-initialize it before filling (change detection compares bytes). */
struct stage_state {
	char scene_uuid[STAGE_SCENE_UUID_MAX];
	bool enabled;          /* rig filter enabled and active          */
	float x, y, zoom, rot; /* current camera (canvas px, 1, degrees) */
	/* Camera target T (a running tween's destination, not the pose seen
	 * mid-transition): what view_save stores and answers with. */
	float tx, ty, tzoom, trot;
	float shake;           /* current shake amplitude, 0..1          */
	uint8_t auto_mode;     /* enum stage_auto_mode                   */
	float auto_intensity;  /* 0..1                                   */
	char fx_preset[STAGE_ID_MAX];
	float focus_depth;     /* DoF focus, 0.1..10                     */
	float dof_amount;      /* 0..1                                   */
	bool moving;           /* a move or transition is running        */
	char move[STAGE_ID_MAX];
	uint64_t move_id;      /* command id of the running move (0 = none) */
	/* The last move started, kept after it ends, so a move that started
	 * and ended within one tick is still reported (0 = none). */
	uint64_t last_move_id;
	char last_move[STAGE_ID_MAX];
	bool last_move_done;   /* it ran to its end (false: running or stopped) */
	bool frozen;
	uint8_t view;          /* slot last applied while unchanged, else 0 */
};

/* Double-buffered snapshot: the writer fills the back buffer without a lock
 * and flips under the lock; readers copy the front buffer under the lock. */
struct stage_state_buf {
	void *lock_; /* pthread_mutex_t*, opaque to keep this header obs-free */
	struct stage_state buf[2];
	int front;
	uint64_t seq; /* incremented on every change */
};

bool stage_state_buf_init(struct stage_state_buf *sb);
void stage_state_buf_free(struct stage_state_buf *sb);
/* Writer (single thread). Returns true (and bumps seq) if s differs from the
 * current front; identical states are not republished. */
bool stage_state_publish(struct stage_state_buf *sb, const struct stage_state *s);
/* Any thread. Copies the front state; *seq (optional) receives its seq. */
void stage_state_read(struct stage_state_buf *sb, struct stage_state *out, uint64_t *seq);

/* ------------------------------------------------------------------------- */
/* Rig registry (one Camera Rig per scene)                                   */
/* ------------------------------------------------------------------------- */

#define STAGE_MAX_RIGS 32
#define STAGE_MAX_FX 64
#define STAGE_LAYER_NAME_MAX 128

/* One scene item as reported by vendor layer_list. */
struct stage_layer_info {
	int64_t item_id;
	char name[STAGE_LAYER_NAME_MAX];
	char source_uuid[STAGE_SCENE_UUID_MAX];
	bool enrolled;  /* false => depth is JSON null, role "none" */
	double depth;   /* STAGE_DEPTH_MIN..MAX when enrolled       */
	uint8_t role;   /* enum stage_role                          */
	bool visible;
	bool locked;
	bool depth_blur; /* source carries a stage_depth_blur filter */
};

/* Ops are called with the registry lock held (see the threading note above).
 * Any op may be NULL. */
struct stage_rig_ops {
	/* Fill *out (typically stage_state_read on the rig's snapshot). */
	void (*get_state)(void *ud, struct stage_state *out);
	/* Fill up to cap layers from the rig's cached layer list; return count. */
	size_t (*list_layers)(void *ud, struct stage_layer_info *out, size_t cap);
	/* Apply a command synchronously. Graphics thread only (stage_rig_apply). */
	void (*apply_cmd)(void *ud, const struct stage_cmd *cmd);
};

/* Registers the rig for scene_uuid. Returns false when the scene already has
 * a rig (the second instance must warn and stay inactive), the table is full
 * or an argument is invalid. Re-registering the same ud for the same scene
 * succeeds. */
bool stage_rig_register(const char *scene_uuid, const struct stage_rig_ops *ops, void *ud);
/* Removes the registration if (scene_uuid, ud) matches, and purges its
 * queued commands. Waits for in-flight ops to finish (registry lock). */
void stage_rig_unregister(const char *scene_uuid, void *ud);
/* Removes every registration of ud (e.g. on destroy when the scene changed). */
void stage_rig_unregister_all(void *ud);

bool stage_rig_exists(const char *scene_uuid);
/* False when no rig is registered for scene_uuid (vendor: "no_rig"). */
bool stage_rig_get_state(const char *scene_uuid, struct stage_state *out);
bool stage_rig_list_layers(const char *scene_uuid, struct stage_layer_info *out, size_t cap,
			   size_t *count);
bool stage_rig_apply(const char *scene_uuid, const struct stage_cmd *cmd);
/* Runs fn(rig_ud, param) under the registry lock; false when no rig. fn must
 * be short and must not call into OBS or the registry. */
bool stage_rig_call(const char *scene_uuid, void (*fn)(void *rig_ud, void *param), void *param);
/* Copies the registered scene uuids, then calls cb(scene_uuid, param) for
 * each WITHOUT any lock held (cb may call any core function). cb returning
 * false stops the iteration. Returns the number of callbacks made. */
size_t stage_rig_for_each(bool (*cb)(const char *scene_uuid, void *param), void *param);
size_t stage_rig_count(void);

/* ------------------------------------------------------------------------- */
/* FX registry (one Parallaxa FX per scene or source)                            */
/* ------------------------------------------------------------------------- */

/* The amounts the FX toggle hotkeys flip (stage_cmd.u.fx.toggle). */
enum stage_fx_toggle {
	STAGE_FX_TOGGLE_BLOOM,
	STAGE_FX_TOGGLE_VIGNETTE,
	STAGE_FX_TOGGLE_LUT,
	STAGE_FX_TOGGLE_LEAK,
	STAGE_FX_TOGGLE_COUNT
};

struct stage_fx_info {
	char preset[STAGE_ID_MAX]; /* "" = custom / none                    */
	bool enabled;              /* filter enabled                         */
	bool neutral;              /* currently skipping (zero cost)         */
	bool bloom;                /* bloom passes active                    */
	char paused[16];           /* STAGE_PERF_PAUSED_* or ""              */
};

struct stage_fx_ops {
	void (*get_info)(void *ud, struct stage_fx_info *out);
	void (*apply_cmd)(void *ud, const struct stage_cmd *cmd); /* graphics thread */
};

/* target_uuid is the uuid of the scene or source carrying the stage_fx filter. */
bool stage_fx_attach(const char *target_uuid, const struct stage_fx_ops *ops, void *ud);
void stage_fx_detach(const char *target_uuid, void *ud);
void stage_fx_detach_all(void *ud);
bool stage_fx_exists(const char *target_uuid);
bool stage_fx_get_info(const char *target_uuid, struct stage_fx_info *out);
bool stage_fx_apply(const char *target_uuid, const struct stage_cmd *cmd);
bool stage_fx_call(const char *target_uuid, void (*fn)(void *fx_ud, void *param), void *param);
size_t stage_fx_for_each(bool (*cb)(const char *target_uuid, void *param), void *param);
size_t stage_fx_count(void);

/* ------------------------------------------------------------------------- */
/* Current program scene                                                     */
/* ------------------------------------------------------------------------- */

/* Copies the cached program scene uuid into out[STAGE_SCENE_UUID_MAX].
 * Safe on ANY thread, including the graphics thread: it never calls
 * obs_frontend_*. The cache is refreshed by plugin-main on the UI thread
 * (OBS_FRONTEND_EVENT_SCENE_CHANGED, FINISHED_LOADING, ...). Returns false
 * (and out = "") when no program scene is known. */
bool stage_current_scene_uuid(char out[STAGE_SCENE_UUID_MAX]);
/* Updates the cache (NULL or "" clears it). UI thread. */
void stage_core_set_program_scene(const char *uuid);

/* Bounded copy that always NUL-terminates (dst_size > 0). */
void stage_strlcpy(char *dst, const char *src, size_t dst_size);

#ifdef __cplusplus
}
#endif

#endif /* STAGE_CORE_H */
