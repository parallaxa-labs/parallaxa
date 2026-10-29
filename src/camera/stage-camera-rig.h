/*
 * stage-camera-rig.h — private to src/camera: the stage_camera_rig filter
 * instance, shared by the runtime (stage-camera-rig.c) and the properties
 * panel (stage-camera-props.c).
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Threads:
 *   graphics  video_tick, update (deferred by libobs to the tick): the only
 *             place that moves scene items and the motion model.
 *   UI        properties, buttons, filter_add/remove, load, save, UI tasks:
 *             writes item and scene private settings (stage.rest,
 *             stage.views), other filters' settings and our own settings
 *             (UI tasks queued by the tick).
 *   audio     volmeter callback: two atomics only.
 *   any       registry ops (snapshot copies), signal handlers (flags).
 * rig->lock (recursive) guards the model and the layer cache; snap_lock the
 * layer_list snapshot; audio_lock the volmeter. Lock order: registry ->
 * rig->lock -> snap_lock. Registry functions are never called with
 * rig->lock held.
 */
#ifndef STAGE_CAMERA_RIG_H
#define STAGE_CAMERA_RIG_H

#include <obs-module.h>
#include <pthread.h>
#include <util/darray.h>

#include "stage-camera.h"
#include "stage-core.h"
#include "stage-rig-model.h"

/* Settings keys (docs/ux/stage-plugin-ux.md section 1). */
#define RS_LOOK "look"
#define RS_APPLY_LOOK "apply_look"
#define RS_SAVE_AS_LOOK "save_as_look"
#define RS_NEW_LOOK_NAME "new_look_name"
#define RS_SAVE_LOOK "save_look"
#define RS_STATUS "status_info"
#define RS_LIVE_SAFE "live_safe"
#define RS_GRP_CAMERA "grp_camera"
#define RS_VIEW_SLOT "view_slot"
#define RS_VIEW_GO "view_go"
#define RS_VIEW_SAVE "view_save_btn"
#define RS_VIEW_NAME "view_name"
#define RS_VIEW_TRANSITION "view_transition"
#define RS_VIEW_DURATION "view_duration_ms"
#define RS_AUTO_MODE "auto_mode"
#define RS_AUTO_INTENSITY "auto_intensity"
#define RS_FREEZE "freeze"
#define RS_GRP_MANUAL "grp_manual"
#define RS_CAM_X "cam_x"
#define RS_CAM_Y "cam_y"
#define RS_CAM_ZOOM "cam_zoom"
#define RS_CAM_ROT "cam_rot"
#define RS_CAM_HOME "cam_home"
#define RS_GRP_LAYERS "grp_layers"
#define RS_LAYERS_HELP "layers_help"
#define RS_AUTO_ASSIGN "auto_assign"
#define RS_REFRESH_LAYERS "refresh_layers"
#define RS_ROLE_PREFIX "role_"
#define RS_DEPTH_PREFIX "depth_"
#define RS_DOF_ENABLED "dof_enabled"
#define RS_DOF_FOCUS "dof_focus"
#define RS_DOF_AMOUNT "dof_amount"
#define RS_DOF_FOLLOW "dof_follow_char"
#define RS_DOF_INFO "dof_info"
#define RS_GRP_AUDIO "grp_audio"
#define RS_AUDIO_SOURCE "audio_source"
#define RS_AUDIO_AMOUNT "audio_amount"
#define RS_AUDIO_ATTACK "audio_attack_ms"
#define RS_AUDIO_RELEASE "audio_release_ms"
#define RS_GRP_ADVANCED "grp_advanced"
#define RS_SMOOTHING "smoothing_ms"
#define RS_OVERSCAN "overscan_pct"
#define RS_MAX_ZOOM "max_zoom"
#define RS_MAX_PAN "max_pan_pct"
#define RS_MOTION_SCALE "motion_scale"
#define RS_MANUAL_PAUSE "manual_pause_s"
#define RS_BAKE_REST "bake_rest"
#define RS_RESET_CAMERA "reset_camera"
#define RS_ABOUT "about"
#define RS_DOCS "docs"
#define RS_REPORT_BUG "report_bug"
/* Hidden: set after the first-add flow ran (never on load). */
#define RS_INITIALIZED "stage_initialized"
/* Hidden: the Save as look... fields are shown. */
#define RS_SAVE_OPEN "save_look_open"

/* stage_depth_blur "driven_by" (stage-plugin-ux.md section 3; proposed
 * STAGE_DEPTH_BLUR_KEY_DRIVEN_BY in stage-protocol.h). */
#ifndef STAGE_DEPTH_BLUR_KEY_DRIVEN_BY
#define STAGE_DEPTH_BLUR_KEY_DRIVEN_BY "driven_by"
#endif

/* One top-level scene item (enrolled or not). */
struct rig_layer {
	obs_sceneitem_t *item; /* our reference */
	int64_t id;
	char name[STAGE_LAYER_NAME_MAX];
	char src_uuid[STAGE_SCENE_UUID_MAX];
	bool seen; /* rebuild mark */
	bool visible, locked;

	bool enrolled;
	uint8_t role;
	float depth;

	bool has_rest;
	bool fresh; /* rest read from stage.rest, not yet checked against the item */
	struct stage_xform rest; /* sx/sy hold the bounds size when bounds */
	bool bounds;             /* bounds_type != OBS_BOUNDS_NONE */
	bool rest_dirty;         /* not yet persisted to stage.rest */

	bool applied;            /* applied_x is on the item */
	struct stage_xform applied_x;
	struct stage_cam applied_cam;
	float applied_f, applied_o, applied_ocx, applied_ocy;

	/* This tick: the item's transform before applying, and the overscan
	 * (fraction and centre) used for it. */
	struct obs_transform_info cur;
	float o_eff, ocx, ocy;

	/* Coverage (bg). */
	bool rect_valid;
	struct stage_rect rect; /* rest rect, not oversized */
	float o_min;            /* computed minimum overscan */
	struct stage_rect o_rect;
	float o_f, o_key[5];

	/* Depth of field. */
	obs_weak_source_t *blur; /* stage_depth_blur on the item source */
	obs_weak_source_t *sig_src; /* source whose filter signals we hold */
	bool driving;
	float blur_written;
};

/* Panel values the rig mirrors (to tell user edits from its own writes). */
struct rig_settings {
	bool live_safe;
	float smoothing_ms, overscan, max_zoom, max_pan, motion_scale, manual_pause_s;
	float audio_amount, audio_attack, audio_release;
	char audio_uuid[STAGE_SCENE_UUID_MAX];
	int auto_mode;
	long long auto_intensity;
	bool freeze;
	double cam_x, cam_y, cam_zoom, cam_rot;
	bool dof_enabled;
	long long dof_amount, dof_focus;
	bool dof_follow;
};

/* Work for the UI thread (one task in flight per rig). */
enum persist_kind { PERSIST_REST, PERSIST_ENROL, PERSIST_UNENROL };
struct persist_job {
	obs_sceneitem_t *item; /* reference */
	enum persist_kind kind;
	struct stage_xform rest;
	bool has_rest;
	uint8_t role;
	float depth;
};
enum blur_op { BLUR_START, BLUR_UPDATE, BLUR_STOP };
struct blur_job {
	obs_weak_source_t *filter; /* reference */
	enum blur_op op;
	float strength;
};

/* Scene view write (stage.views lives in the scene's private settings, which
 * only the UI thread writes). */
enum view_op { VIEW_PUT, VIEW_DELETE };
struct view_job {
	enum view_op op;
	uint8_t slot, transition;
	uint32_t duration_ms;
	uint32_t gen; /* stage_cmd u.view.gen: 0 = not stored yet */
	struct stage_cam pose;
	char name[STAGE_VIEW_NAME_MAX];
};

/* A layer change the panel pushed that the tick has not applied yet. The
 * panel shows it until then; a rebuild before the tick would otherwise show
 * the old role. */
struct rig_pending_layer {
	int64_t id;
	bool enrolled;
	uint8_t role;
	float depth;
	uint64_t until_ms;
};

#define RIG_UI_SETTINGS 0x01u /* sync auto/freeze/dof panel values      */
#define RIG_UI_STATUS 0x02u   /* write the status line                   */
#define RIG_UI_PROPS 0x04u    /* refresh an open properties panel        */

struct stage_rig {
	volatile long refs;
	obs_source_t *source; /* the filter (not owned; its own callbacks only) */
	obs_weak_source_t *self; /* the filter, for UI tasks that may outlive it */
	pthread_mutex_t lock;  /* recursive */
	pthread_mutex_t snap_lock;
	struct stage_state_buf state;

	/* Parent scene. */
	obs_weak_source_t *parent;
	char scene_uuid[STAGE_SCENE_UUID_MAX];
	signal_handler_t *parent_sh; /* referenced while our handlers are on it */
	bool is_scene;

	/* Lifecycle. */
	bool registered, second, was_active, removed, destroyed, suspended;
	bool warned_second;
	uint64_t next_register_ms;
	volatile long dirty;
	volatile long bake_req, punch_req;
	float rebuild_timer, fx_timer;
	uint64_t panic_gen;
	char fx_preset[STAGE_ID_MAX];

	/* Motion. */
	struct stage_rig_model m;
	struct rig_settings set;
	float overscan_eff;

	/* Layers. */
	DARRAY(struct rig_layer) layers;
	struct stage_layer_info *snap;
	size_t nsnap, snapcap;

	/* Audio. */
	pthread_mutex_t audio_lock;
	obs_volmeter_t *vm;
	signal_handler_t *audio_sh; /* referenced while our handlers are on it */
	volatile long audio_detached;
	volatile long audio_db_bits; /* float bits of the latest peak dBFS */
	volatile long audio_count;   /* callbacks so far */
	long audio_seen_count;
	float audio_quiet_s;
	float audio_retry_s; /* tick: attach a selected source that appears later */

	/* UI work. */
	uint32_t ui_bits;
	bool ui_queued;
	DARRAY(struct persist_job) persist;
	DARRAY(struct blur_job) blur;
	DARRAY(struct view_job) views;
	DARRAY(struct rig_pending_layer) pending; /* panel layer edits in flight */
	char status_key[48];
	char status_a1[200];
	char status_a2[32];
	enum obs_text_info_type status_type;

	/* Look (panel only). */
	char look_applied[STAGE_ID_MAX];
	bool look_modified;

	/* Tick cost, logged at debug level every RIG_PERF_TICKS active ticks. */
	uint64_t perf_sum_ns, perf_max_ns;
	uint32_t perf_n;
};

#define RIG_PERF_TICKS 300

/* Reference counting (UI tasks keep the rig alive after destroy). */
void rig_addref(struct stage_rig *r);
void rig_release(struct stage_rig *r);

/* Pushes a command for this rig's scene (fills scene_uuid). False when the
 * scene is unknown or the queue refused it. Any thread. */
bool rig_push(struct stage_rig *r, struct stage_cmd *c);

/* Sets the status line (Stage.Info.* key with %1/%2 arguments). Any thread;
 * the text is written to the settings by a UI task. */
void rig_set_status(struct stage_rig *r, const char *key, const char *a1, const char *a2,
		    enum obs_text_info_type type);
/* Formats a locale text with %1 / %2 replaced. */
void rig_format(char *out, size_t cap, const char *key, const char *a1, const char *a2);

/* Copies the layer snapshot (layer_list shape). Returns the count. */
size_t rig_snapshot(struct stage_rig *r, struct stage_layer_info *out, size_t cap);

/* Requests (any thread; handled on the next tick). */
void rig_request_bake(struct stage_rig *r);

/* Records a layer_set the panel is pushing (same arguments as the command)
 * so that panel rebuilds show it before the tick applies it. UI thread. */
void rig_pending_layer_set(struct stage_rig *r, int64_t id, int role, float depth, unsigned mask);
/* rig_snapshot() with the pending panel edits applied on top. */
size_t rig_panel_snapshot(struct stage_rig *r, struct stage_layer_info *out, size_t cap);
/* Applies the pending panel edits to rows read some other way (the scene
 * scan used before the rig's first tick). */
void rig_pending_apply(struct stage_rig *r, struct stage_layer_info *rows, size_t n);

/* Reads saved view `slot` of a scene (presets/stage-target.c storage when
 * that module is built, else the same layout). False when empty. */
bool stage_cam_view_get(obs_source_t *scene, uint8_t slot, float *x, float *y, float *zoom,
			float *rot, char *name, size_t name_cap);
/* Stores a view (slot, name, pose, transition, duration). */
bool stage_cam_view_put(obs_source_t *scene, uint8_t slot, const char *name,
			const struct stage_cam *pose, uint8_t transition, uint32_t duration_ms);

/* Role guess from a source name (vendor-api.md 4.13). */
int stage_cam_guess_role(const char *name);

/* Properties panel (stage-camera-props.c). */
obs_properties_t *rig_properties(void *data);
void rig_defaults(obs_data_t *settings);
/* First add of a rig to a scene (UI thread, from filter_add). */
void rig_first_add(struct stage_rig *r, obs_source_t *scene_source);

/* Settings sync from the model into the filter's settings (`self`), run by
 * the UI task (stage-camera-rig.c). */
void rig_sync_settings_ui(struct stage_rig *r, obs_source_t *self);

#endif /* STAGE_CAMERA_RIG_H */
