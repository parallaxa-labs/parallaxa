/*
 * stage-camera-hotkeys.c — camera module entry (stage_camera_register),
 * frontend hotkeys for camera, views, moves and rack focus, the Tools menu
 * item "Parallaxa: Restore all rest poses" and the frontend observers.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Hotkeys act on the current program scene (docs/ux/stage-plugin-ux.md
 * section 5). Callbacks run on the OBS hotkey thread and only push commands
 * (or set the rig's punch request): no obs_data is read or written there;
 * the rig tick resolves anything that needs scene settings. Their bindings are saved with the scene collection through the
 * core save-callback observer under "stage.camera". The FX and "AI Live Studio"
 * hotkeys belong to the FX / vendor modules.
 */
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/threading.h>

#include "stage-camera-rig.h"
#include "stage-camera.h"
#include "stage-core.h"
#include "stage-frontend.h"
#include "stage-modules.h"

#define SAVE_KEY "stage.camera"

enum hk_kind {
	HK_VIEW,
	HK_VIEW_NEXT,
	HK_VIEW_PREV,
	HK_HOME,
	HK_AUTO_TOGGLE,
	HK_AUTO_NEXT,
	HK_FREEZE,
	HK_PUNCH,
	HK_MOVE,
	HK_RACK_CHAR,
	HK_RACK_BG,
	HK_PANIC,
};

struct hk {
	obs_hotkey_id id;
	enum hk_kind kind;
	int slot;               /* HK_VIEW */
	char arg[STAGE_ID_MAX]; /* HK_MOVE: move id */
	char name[64];          /* registration / save key */
};

static DARRAY(struct hk *) hks;

/* ------------------------------------------------------------------------- */
/* Actions                                                                   */
/* ------------------------------------------------------------------------- */

static void push(struct stage_cmd *c)
{
	c->scene_uuid[0] = '\0'; /* current program scene, resolved at push */
	stage_cmdq_push(c);
}

static void punch_fn(void *rig_ud, void *param)
{
	struct stage_rig *r = rig_ud;
	long bit = *(const long *)param;
	/* atomic OR: press and release may both land before the next tick */
	long old = os_atomic_load_long(&r->punch_req);
	while (!os_atomic_compare_exchange_long(&r->punch_req, &old, old | bit))
		;
}

static void hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	(void)id;
	(void)hotkey;
	struct hk *h = data;
	if (h->kind == HK_PUNCH) {
		char uuid[STAGE_SCENE_UUID_MAX];
		long bit = pressed ? 1 : 2;
		if (stage_current_scene_uuid(uuid))
			stage_rig_call(uuid, punch_fn, &bit);
		return;
	}
	if (!pressed)
		return;
	struct stage_cmd c;
	memset(&c, 0, sizeof(c));
	c.ease = STAGE_EASE_DEFAULT;
	c.intensity = -1.0f;
	switch (h->kind) {
	case HK_VIEW:
		c.type = STAGE_CMD_VIEW_APPLY;
		c.u.view.slot = (uint8_t)h->slot;
		push(&c);
		break;
	case HK_VIEW_NEXT:
	case HK_VIEW_PREV:
		/* The rig tick picks the slot (it owns the current view and
		 * reads the stored views under its lock). */
		c.type = STAGE_CMD_VIEW_APPLY;
		c.u.view.step = (int8_t)(h->kind == HK_VIEW_NEXT ? 1 : -1);
		push(&c);
		break;
	case HK_HOME:
		c.type = STAGE_CMD_CAMERA_RESET;
		c.duration_ms = STAGE_CAM_RESET_MS;
		push(&c);
		break;
	case HK_AUTO_TOGGLE:
	case HK_AUTO_NEXT:
		c.type = STAGE_CMD_CAMERA_AUTO;
		c.u.autom.mode = h->kind == HK_AUTO_TOGGLE ? STAGE_CAM_AUTO_TOGGLE : STAGE_CAM_AUTO_NEXT;
		push(&c);
		break;
	case HK_FREEZE:
		c.type = STAGE_CMD_FREEZE;
		c.u.freeze.mode = STAGE_FREEZE_TOGGLE;
		push(&c);
		break;
	case HK_MOVE:
		c.type = STAGE_CMD_CAMERA_MOVE;
		c.intensity = 1.0f;
		c.ease = STAGE_CAM_EASE_NONE;
		stage_strlcpy(c.u.move.move, h->arg, sizeof(c.u.move.move));
		push(&c);
		break;
	case HK_RACK_CHAR:
	case HK_RACK_BG:
		c.type = STAGE_CMD_DOF_SET;
		c.duration_ms = STAGE_CAM_RACK_MS;
		c.u.dof.mask = (uint8_t)(h->kind == HK_RACK_CHAR ? STAGE_CAM_DOF_RACK_CHAR
								  : STAGE_CAM_DOF_RACK_BG);
		push(&c);
		break;
	case HK_PANIC:
		stage_panic_trigger();
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------------- */
/* Registration                                                              */
/* ------------------------------------------------------------------------- */

static void add_hotkey(enum hk_kind kind, const char *name, const char *desc, int slot, const char *arg)
{
	struct hk *h = bzalloc(sizeof(*h));
	h->kind = kind;
	h->slot = slot;
	if (arg)
		stage_strlcpy(h->arg, arg, sizeof(h->arg));
	stage_strlcpy(h->name, name, sizeof(h->name));
	h->id = obs_hotkey_register_frontend(h->name, desc, hotkey_cb, h);
	if (h->id == OBS_INVALID_HOTKEY_ID) {
		bfree(h);
		return;
	}
	da_push_back(hks, &h);
}

static void add_simple(enum hk_kind kind, const char *name, const char *key)
{
	add_hotkey(kind, name, obs_module_text(key), 0, NULL);
}

static void register_hotkeys(void)
{
	char name[64], desc[256], sl[8];
	for (int slot = STAGE_VIEW_SLOT_MIN; slot <= STAGE_VIEW_SLOT_MAX; slot++) {
		snprintf(name, sizeof(name), "stage_camera.view_%d", slot);
		snprintf(sl, sizeof(sl), "%d", slot);
		rig_format(desc, sizeof(desc), "Stage.Hotkey.View", sl, NULL);
		add_hotkey(HK_VIEW, name, desc, slot, NULL);
	}
	add_simple(HK_VIEW_NEXT, "stage_camera.view_next", "Stage.Hotkey.ViewNext");
	add_simple(HK_VIEW_PREV, "stage_camera.view_prev", "Stage.Hotkey.ViewPrev");
	add_simple(HK_HOME, "stage_camera.home", "Stage.Hotkey.Home");
	add_simple(HK_AUTO_TOGGLE, "stage_camera.auto_toggle", "Stage.Hotkey.AutoToggle");
	add_simple(HK_AUTO_NEXT, "stage_camera.auto_next", "Stage.Hotkey.AutoNext");
	add_simple(HK_FREEZE, "stage_camera.freeze", "Stage.Hotkey.Freeze");
	add_simple(HK_PUNCH, "stage_camera.punch_hold", "Stage.Hotkey.PunchHold");
	size_t n = stage_cam_moves_count();
	for (size_t i = 0; i < n; i++) {
		struct stage_cam_move m;
		if (!stage_cam_moves_at(i, &m))
			continue;
		snprintf(name, sizeof(name), "stage_camera.move_%s", m.def.id);
		rig_format(desc, sizeof(desc), "Stage.Hotkey.Move", stage_cam_move_local_name(&m), NULL);
		add_hotkey(HK_MOVE, name, desc, 0, m.def.id);
	}
	add_simple(HK_RACK_CHAR, "stage_camera.rack_char", "Stage.Hotkey.RackChar");
	add_simple(HK_RACK_BG, "stage_camera.rack_bg", "Stage.Hotkey.RackBg");
	add_simple(HK_PANIC, "stage_camera.panic", "Stage.Hotkey.Panic");
}

/* ------------------------------------------------------------------------- */
/* Frontend                                                                  */
/* ------------------------------------------------------------------------- */

static void on_save(obs_data_t *save_data, bool saving, void *ud)
{
	(void)ud;
	if (saving) {
		obs_data_t *obj = obs_data_create();
		for (size_t i = 0; i < hks.num; i++) {
			obs_data_array_t *arr = obs_hotkey_save(hks.array[i]->id);
			obs_data_set_array(obj, hks.array[i]->name, arr);
			obs_data_array_release(arr);
		}
		obs_data_set_obj(save_data, SAVE_KEY, obj);
		obs_data_release(obj);
	} else {
		obs_data_t *obj = obs_data_get_obj(save_data, SAVE_KEY);
		if (!obj)
			return;
		for (size_t i = 0; i < hks.num; i++) {
			obs_data_array_t *arr = obs_data_get_array(obj, hks.array[i]->name);
			if (arr)
				obs_hotkey_load(hks.array[i]->id, arr);
			obs_data_array_release(arr);
		}
		obs_data_release(obj);
	}
}

static void on_event(enum obs_frontend_event event, void *ud)
{
	(void)ud;
	stage_camera_rig_frontend_event((int)event);
}

static void restore_menu_cb(void *data)
{
	(void)data;
	stage_camera_restore_all_rest_poses();
}

void stage_camera_hotkeys_init(void)
{
	da_init(hks);
	register_hotkeys();
	stage_frontend_add_save_cb(on_save, NULL);
	stage_frontend_add_event_cb(on_event, NULL);
#ifdef STAGE_HAS_PRESETS
	/* The presets module owns the Tools menu (stage-plugin-ux.md section 4);
	 * its "Restore all rest poses" presses this rig's Bake rest pose. */
	(void)restore_menu_cb;
#else
	/* Camera-only build: the rig adds the item itself. No frontend
	 * (headless libobs, tests): no Tools menu. */
	if (obs_frontend_get_main_window()) {
		obs_frontend_add_tools_menu_item(obs_module_text("Stage.Tools.RestoreRest"),
						 restore_menu_cb, NULL);
	}
#endif
}

void stage_camera_hotkeys_free(void)
{
	stage_frontend_remove_save_cb(on_save, NULL);
	stage_frontend_remove_event_cb(on_event, NULL);
	for (size_t i = 0; i < hks.num; i++) {
		obs_hotkey_unregister(hks.array[i]->id);
		bfree(hks.array[i]);
	}
	da_free(hks);
}

/* ------------------------------------------------------------------------- */
/* Module entry                                                              */
/* ------------------------------------------------------------------------- */

static void camera_unload(void *ud)
{
	(void)ud;
	stage_camera_hotkeys_free();
	stage_cam_moves_free();
}

void stage_camera_register(void)
{
	char *builtin = obs_module_file("presets/moves");
	char *user = obs_module_config_path("moves");
	size_t n = stage_cam_moves_load(builtin, user);
	bfree(builtin);
	bfree(user);
	SLOG_INFO("camera rig: %zu camera moves", n);

	stage_camera_rig_register_source();
	stage_camera_hotkeys_init();
	stage_core_on_unload(camera_unload, NULL);
}
