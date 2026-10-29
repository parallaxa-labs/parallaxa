/*
 * stage-hotkeys.c — OBS frontend hotkeys (docs/ux/stage-plugin-ux.md
 * section 5), all acting on the current program scene
 * and unbound by default:
 *   Parallaxa: camera views 1-9, next/previous view, home, toggle / next auto
 *   camera, freeze, zoom punch-in (hold), FX toggles 1-4, all effects off,
 *   pulse, rack focus to character / background, panic;
 *   AI Live Studio: Stop / Skip / Mute AI / Panic / Approve -> vendor event
 *   hotkey_forward {action}.
 * The per-move hotkeys ("Parallaxa: Move – %1") belong to the Camera Rig.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Callbacks run on the OBS hotkey thread: they only read registry snapshots
 * (stage_rig_get_state) and push commands. No obs_data is read there:
 * saved views, the next / previous view and the rack focus depth and amount
 * are resolved by the rig tick, the FX toggles by the FX tick (in queue
 * order, so quick presses flip in turn). A hotkey whose description is already
 * registered (e.g. by the Camera Rig) is skipped, so nothing shows twice in
 * Settings > Hotkeys. Bindings are saved in the scene collection under
 * "stage.vendor.hotkeys" through the core save-callback observer.
 */
#include <math.h>
#include <obs-module.h>
#include <stdio.h>
#include <string.h>
#include <util/bmem.h>
#include <util/dstr.h>

#include "stage-frontend.h"
#include "presets/stage-presets.h"
#include "stage-vendor.h"

#define SAVE_KEY "stage.vendor.hotkeys"
#define PUNCH_ZOOM 0.18f /* the punch move's peak */
#define PUNCH_IN_MS 120
#define PUNCH_OUT_MS 200
#define RACK_MS 1200
#define FX_TOGGLE_MS 200
#define HOME_MS 600
#define PULSE_MS 300

enum hk_action {
	HK_VIEW,
	HK_VIEW_NEXT,
	HK_VIEW_PREV,
	HK_HOME,
	HK_AUTO_TOGGLE,
	HK_AUTO_NEXT,
	HK_FREEZE,
	HK_PUNCH_HOLD,
	HK_FX_TOGGLE,
	HK_FX_OFF,
	HK_PULSE,
	HK_RACK_CHAR,
	HK_RACK_BG,
	HK_PANIC,
	HK_AI,
};

struct hotkey {
	char name[32];
	const char *desc_key;
	enum hk_action action;
	int arg; /* view slot, fx toggle index, enum stage_hotkey_action */
	obs_hotkey_id id;
};

#define MAX_HOTKEYS 32
static struct hotkey hotkeys[MAX_HOTKEYS];
static size_t nhotkeys;
static bool registered;

/* Hotkey-thread state. */
static uint8_t last_auto_mode = STAGE_AUTO_DRIFT;
static bool punch_active;
static char punch_scene[STAGE_SCENE_UUID_MAX];
static struct stage_cam_target punch_pose;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

/* Program scene with an enabled rig. */
static bool program_rig(char uuid[STAGE_SCENE_UUID_MAX], struct stage_state *st)
{
	if (!stage_current_scene_uuid(uuid))
		return false;
	if (!stage_rig_get_state(uuid, st) || !st->enabled) {
		SLOG_DEBUG("hotkey: the program scene has no enabled Parallaxa Camera Rig");
		return false;
	}
	return true;
}

static struct stage_cmd cmd_for(enum stage_cmd_type type, const char *uuid)
{
	struct stage_cmd c;
	memset(&c, 0, sizeof(c));
	c.type = type;
	stage_strlcpy(c.scene_uuid, uuid, sizeof(c.scene_uuid));
	c.ease = STAGE_EASE_DEFAULT;
	c.intensity = -1.0f;
	return c;
}

static void push(const struct stage_cmd *c)
{
	if (!stage_cmdq_push(c))
		SLOG_DEBUG("hotkey: command not queued");
}

/* View `slot` (step 0) or the next (step +1) / previous (-1) saved view:
 * the rig tick reads the stored views and picks the slot. */
static void apply_view(const char *uuid, uint8_t slot, int8_t step)
{
	struct stage_cmd c = cmd_for(STAGE_CMD_VIEW_APPLY, uuid);
	c.u.view.slot = slot;
	c.u.view.step = step;
	push(&c);
	stage_vendor_note_cancel(uuid);
}

/* FX toggle idx (enum stage_fx_toggle), all effects off (STAGE_CMD_FX_SET,
 * idx < 0) or pulse. The FX tick flips a toggle against the target the
 * commands queued before it left, so two presses within one tick turn it
 * off and on again, as across ticks. */
static void fx_cmd(enum stage_cmd_type type, int idx)
{
	char uuid[STAGE_SCENE_UUID_MAX];
	if (!stage_current_scene_uuid(uuid) || !stage_fx_exists(uuid)) {
		SLOG_DEBUG("hotkey: the program scene has no Parallaxa FX");
		return;
	}
	struct stage_cmd c = cmd_for(type, uuid);
	if (idx >= 0) {
		c.u.fx.toggle = (uint8_t)(idx + 1);
		c.duration_ms = FX_TOGGLE_MS;
	} else if (type == STAGE_CMD_FX_SET) {
		stage_strlcpy(c.u.fx.preset, "neutral", sizeof(c.u.fx.preset));
	} else {
		stage_strlcpy(c.u.pulse.effect, stage_pulse_names[STAGE_PULSE_VIGNETTE_PULSE],
			      sizeof(c.u.pulse.effect));
		c.intensity = (float)STAGE_PULSE_INTENSITY_DEFAULT;
		c.duration_ms = PULSE_MS;
	}
	push(&c);
}

/* Rack focus to the char layer's depth (1 if none) or the farthest bg
 * layer's depth (3 if none); the rig tick picks the depth and, with DoF
 * off, turns it on at the panel's Blur amount. */
static void rack_focus(const char *uuid, bool to_bg)
{
	struct stage_cmd c = cmd_for(STAGE_CMD_DOF_SET, uuid);
	c.u.dof.mask = (uint8_t)(to_bg ? STAGE_DOF_RACK_BG : STAGE_DOF_RACK_CHAR);
	c.duration_ms = RACK_MS;
	push(&c);
}

static void punch(bool pressed)
{
	char uuid[STAGE_SCENE_UUID_MAX];
	struct stage_state st;
	if (pressed) {
		if (punch_active || !program_rig(uuid, &st))
			return;
		punch_active = true;
		stage_strlcpy(punch_scene, uuid, sizeof(punch_scene));
		memset(&punch_pose, 0, sizeof(punch_pose));
		punch_pose.x = st.x;
		punch_pose.y = st.y;
		punch_pose.zoom = st.zoom > 0 ? st.zoom : 1.0f;
		punch_pose.rot = st.rot;
		punch_pose.mask = STAGE_CAM_X | STAGE_CAM_Y | STAGE_CAM_ZOOM | STAGE_CAM_ROT;
		struct stage_cmd c = cmd_for(STAGE_CMD_CAMERA_SET, uuid);
		c.u.set.zoom = PUNCH_ZOOM;
		c.u.set.mask = STAGE_CAM_ZOOM;
		c.u.set.relative = true;
		c.duration_ms = PUNCH_IN_MS;
		c.ease = STAGE_EASE_OUT_BACK;
		push(&c);
	} else if (punch_active) {
		punch_active = false;
		struct stage_cmd c = cmd_for(STAGE_CMD_CAMERA_SET, punch_scene);
		c.u.set = punch_pose;
		c.duration_ms = PUNCH_OUT_MS;
		push(&c);
	}
}

static void ai_forward(int action)
{
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "action", stage_hotkey_action_names[action]);
	if (!stage_vendor_emit(STAGE_EVT_HOTKEY_FORWARD, d))
		SLOG_INFO("hotkey: %s pressed, but obs-websocket is not available",
			  stage_hotkey_action_names[action]);
	obs_data_release(d);
}

/* ------------------------------------------------------------------------- */
/* Dispatch                                                                   */
/* ------------------------------------------------------------------------- */

static void hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *key, bool pressed)
{
	(void)id;
	(void)key;
	const struct hotkey *hk = data;
	if (hk->action == HK_PUNCH_HOLD) {
		punch(pressed);
		return;
	}
	if (!pressed)
		return;

	char uuid[STAGE_SCENE_UUID_MAX];
	struct stage_state st;
	struct stage_cmd c;
	switch (hk->action) {
	case HK_AI:
		ai_forward(hk->arg);
		return;
	case HK_PANIC:
		stage_panic_trigger();
		SLOG_INFO("hotkey: panic reset");
		return;
	case HK_FX_TOGGLE:
		fx_cmd(STAGE_CMD_FX_SET, hk->arg);
		return;
	case HK_FX_OFF:
		fx_cmd(STAGE_CMD_FX_SET, -1);
		return;
	case HK_PULSE:
		fx_cmd(STAGE_CMD_FX_PULSE, -1);
		return;
	default:
		break;
	}

	if (!program_rig(uuid, &st))
		return;
	switch (hk->action) {
	case HK_VIEW:
		apply_view(uuid, (uint8_t)hk->arg, 0);
		break;
	case HK_VIEW_NEXT:
		apply_view(uuid, 0, 1);
		break;
	case HK_VIEW_PREV:
		apply_view(uuid, 0, -1);
		break;
	case HK_HOME:
		c = cmd_for(STAGE_CMD_CAMERA_RESET, uuid);
		c.duration_ms = HOME_MS;
		push(&c);
		stage_vendor_note_cancel(uuid);
		break;
	case HK_AUTO_TOGGLE:
		c = cmd_for(STAGE_CMD_CAMERA_AUTO, uuid);
		if (st.auto_mode != STAGE_AUTO_OFF && st.auto_mode < STAGE_AUTO_COUNT) {
			last_auto_mode = st.auto_mode;
			c.u.autom.mode = STAGE_AUTO_OFF;
		} else {
			c.u.autom.mode = last_auto_mode;
		}
		push(&c);
		break;
	case HK_AUTO_NEXT:
		c = cmd_for(STAGE_CMD_CAMERA_AUTO, uuid);
		c.u.autom.mode = (uint8_t)((st.auto_mode + 1u) % STAGE_AUTO_COUNT);
		push(&c);
		break;
	case HK_FREEZE:
		c = cmd_for(STAGE_CMD_FREEZE, uuid);
		c.u.freeze.mode = (uint8_t)(st.frozen ? STAGE_FREEZE_OFF : STAGE_FREEZE_ON);
		push(&c);
		break;
	case HK_RACK_CHAR:
		rack_focus(uuid, false);
		break;
	case HK_RACK_BG:
		rack_focus(uuid, true);
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------------- */
/* Registration, save / load                                                  */
/* ------------------------------------------------------------------------- */

struct desc_find {
	const char *desc;
	bool found;
};

static bool desc_enum_cb(void *data, obs_hotkey_id id, obs_hotkey_t *key)
{
	struct desc_find *f = data;
	for (size_t i = 0; i < nhotkeys; i++) {
		if (hotkeys[i].id == id)
			return true; /* our own */
	}
	const char *d = obs_hotkey_get_description(key);
	if (d && strcmp(d, f->desc) == 0) {
		f->found = true;
		return false;
	}
	return true;
}

static void add(const char *name, const char *desc_key, enum hk_action action, int arg)
{
	if (nhotkeys >= MAX_HOTKEYS)
		return;
	struct dstr desc = {0};
	dstr_copy(&desc, obs_module_text(desc_key));
	if (action == HK_VIEW) {
		char slot[8];
		snprintf(slot, sizeof(slot), "%d", arg);
		dstr_replace(&desc, "%1", slot);
	}
	struct desc_find f = {desc.array, false};
	obs_enum_hotkeys(desc_enum_cb, &f);
	if (f.found) {
		SLOG_DEBUG("hotkey '%s' already registered by another module", desc.array);
		dstr_free(&desc);
		return;
	}
	struct hotkey *hk = &hotkeys[nhotkeys];
	snprintf(hk->name, sizeof(hk->name), "%s", name);
	hk->desc_key = desc_key;
	hk->action = action;
	hk->arg = arg;
	hk->id = obs_hotkey_register_frontend(hk->name, desc.array, hotkey_cb, hk);
	dstr_free(&desc);
	if (hk->id != OBS_INVALID_HOTKEY_ID)
		nhotkeys++;
}

static void on_save(obs_data_t *save_data, bool saving, void *ud)
{
	(void)ud;
	if (saving) {
		obs_data_t *h = obs_data_create();
		for (size_t i = 0; i < nhotkeys; i++) {
			obs_data_array_t *a = obs_hotkey_save(hotkeys[i].id);
			obs_data_set_array(h, hotkeys[i].name, a);
			obs_data_array_release(a);
		}
		obs_data_set_obj(save_data, SAVE_KEY, h);
		obs_data_release(h);
	} else {
		obs_data_t *h = obs_data_get_obj(save_data, SAVE_KEY);
		for (size_t i = 0; h && i < nhotkeys; i++) {
			obs_data_array_t *a = obs_data_get_array(h, hotkeys[i].name);
			obs_hotkey_load(hotkeys[i].id, a);
			obs_data_array_release(a);
		}
		obs_data_release(h);
	}
}

void stage_hotkeys_register(void)
{
	if (registered)
		return;
	registered = true;
	nhotkeys = 0;
	for (int s = STAGE_VIEW_SLOT_MIN; s <= STAGE_VIEW_SLOT_MAX; s++) {
		char name[32];
		snprintf(name, sizeof(name), "stage.view_%d", s);
		add(name, "Stage.Hotkey.View", HK_VIEW, s);
	}
	add("stage.view_next", "Stage.Hotkey.ViewNext", HK_VIEW_NEXT, 0);
	add("stage.view_prev", "Stage.Hotkey.ViewPrev", HK_VIEW_PREV, 0);
	add("stage.home", "Stage.Hotkey.Home", HK_HOME, 0);
	add("stage.auto_toggle", "Stage.Hotkey.AutoToggle", HK_AUTO_TOGGLE, 0);
	add("stage.auto_next", "Stage.Hotkey.AutoNext", HK_AUTO_NEXT, 0);
	add("stage.freeze", "Stage.Hotkey.Freeze", HK_FREEZE, 0);
	add("stage.punch_hold", "Stage.Hotkey.PunchHold", HK_PUNCH_HOLD, 0);
	add("stage.fx_bloom", "Stage.Hotkey.FxBloom", HK_FX_TOGGLE, 0);
	add("stage.fx_vignette", "Stage.Hotkey.FxVignette", HK_FX_TOGGLE, 1);
	add("stage.fx_lut", "Stage.Hotkey.FxLut", HK_FX_TOGGLE, 2);
	add("stage.fx_leak", "Stage.Hotkey.FxLeak", HK_FX_TOGGLE, 3);
	add("stage.fx_off", "Stage.Hotkey.FxOff", HK_FX_OFF, 0);
	add("stage.pulse", "Stage.Hotkey.Pulse", HK_PULSE, 0);
	add("stage.rack_char", "Stage.Hotkey.RackChar", HK_RACK_CHAR, 0);
	add("stage.rack_bg", "Stage.Hotkey.RackBg", HK_RACK_BG, 0);
	add("stage.panic", "Stage.Hotkey.Panic", HK_PANIC, 0);
	add("stage.ai_stop", "Stage.Hotkey.AiStop", HK_AI, STAGE_HOTKEY_AI_STOP);
	add("stage.ai_skip", "Stage.Hotkey.AiSkip", HK_AI, STAGE_HOTKEY_AI_SKIP);
	add("stage.ai_mute", "Stage.Hotkey.AiMute", HK_AI, STAGE_HOTKEY_AI_MUTE);
	add("stage.ai_panic", "Stage.Hotkey.AiPanic", HK_AI, STAGE_HOTKEY_AI_PANIC);
	add("stage.ai_approve", "Stage.Hotkey.AiApprove", HK_AI, STAGE_HOTKEY_AI_APPROVE);
	stage_frontend_add_save_cb(on_save, NULL);
	SLOG_DEBUG("registered %zu hotkeys", nhotkeys);
}

void stage_hotkeys_unregister(void)
{
	if (!registered)
		return;
	stage_frontend_remove_save_cb(on_save, NULL);
	for (size_t i = 0; i < nhotkeys; i++)
		obs_hotkey_unregister(hotkeys[i].id);
	nhotkeys = 0;
	registered = false;
}
