/*
 * stage-camera-props.c — Parallaxa Camera Rig properties panel, defaults and the
 * first-add flow (docs/ux/stage-plugin-ux.md section 1). UI thread.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Per-item rows (role_<item_id>, depth_<item_id>) are views of the rig's
 * layer snapshot; edits go to the rig as layer_set commands, so the item
 * private settings are only ever written by the rig (one writer).
 */
#include "stage-camera-rig.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <util/dstr.h>
#include <util/threading.h>

#ifdef STAGE_HAS_PRESETS
#include "presets/stage-presets.h"
#endif

#define T_(k) obs_module_text(k)
#define MAX_PANEL_LAYERS 128

static const char *const role_keys[STAGE_ROLE_COUNT] = {
	"Stage.List.Role.None", "Stage.List.Role.Bg",  "Stage.List.Role.Char",
	"Stage.List.Role.Fg",   "Stage.List.Role.Sky", "Stage.List.Role.Static",
};
static const char *const auto_keys[STAGE_AUTO_COUNT] = {
	"Stage.List.Auto.Off",
	"Stage.List.Auto.Drift",
	"Stage.List.Auto.Chatting",
	"Stage.List.Auto.Singing",
};
static const char *const transition_keys[STAGE_TRANSITION_COUNT] = {
	"Stage.List.Transition.Cut",
	"Stage.List.Transition.Smooth",
	"Stage.List.Transition.Whip",
	"Stage.List.Transition.Punch",
};

static void set_desc(obs_property_t *p, const char *key)
{
	struct dstr k = {0};
	dstr_printf(&k, "%s.Desc", key);
	obs_property_set_long_description(p, T_(k.array));
	dstr_free(&k);
}

static bool role_has_depth(int role)
{
	return role == STAGE_ROLE_BG || role == STAGE_ROLE_CHAR || role == STAGE_ROLE_FG;
}

/* ------------------------------------------------------------------------- */
/* Defaults                                                                  */
/* ------------------------------------------------------------------------- */

void rig_defaults(obs_data_t *s)
{
	obs_data_set_default_string(s, RS_LOOK, "cozy_chat");
	obs_data_set_default_bool(s, RS_LIVE_SAFE, true);
	obs_data_set_default_int(s, RS_VIEW_SLOT, 1);
	obs_data_set_default_string(s, RS_VIEW_TRANSITION, "smooth");
	obs_data_set_default_int(s, RS_VIEW_DURATION, STAGE_VIEW_DURATION_DEFAULT);
	obs_data_set_default_string(s, RS_AUTO_MODE, "drift");
	obs_data_set_default_int(s, RS_AUTO_INTENSITY, 35);
	obs_data_set_default_bool(s, RS_FREEZE, false);
	obs_data_set_default_double(s, RS_CAM_X, 0.0);
	obs_data_set_default_double(s, RS_CAM_Y, 0.0);
	obs_data_set_default_double(s, RS_CAM_ZOOM, 1.0);
	obs_data_set_default_double(s, RS_CAM_ROT, 0.0);
	obs_data_set_default_bool(s, RS_DOF_ENABLED, false);
	obs_data_set_default_int(s, RS_DOF_FOCUS, 0);
	obs_data_set_default_int(s, RS_DOF_AMOUNT, 30);
	obs_data_set_default_bool(s, RS_DOF_FOLLOW, true);
	obs_data_set_default_string(s, RS_AUDIO_SOURCE, "");
	obs_data_set_default_int(s, RS_AUDIO_AMOUNT, 25);
	obs_data_set_default_int(s, RS_AUDIO_ATTACK, 30);
	obs_data_set_default_int(s, RS_AUDIO_RELEASE, 250);
	obs_data_set_default_int(s, RS_SMOOTHING, 120);
	obs_data_set_default_int(s, RS_OVERSCAN, (long long)lround(STAGE_OVERSCAN_DEFAULT * 100.0));
	obs_data_set_default_double(s, RS_MAX_ZOOM, 2.0);
	obs_data_set_default_int(s, RS_MAX_PAN, 15);
	obs_data_set_default_int(s, RS_MOTION_SCALE, 100);
	obs_data_set_default_int(s, RS_MANUAL_PAUSE, STAGE_MANUAL_AUTO_PAUSE_S);
}

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static obs_source_t *rig_scene(struct stage_rig *r)
{
	obs_source_t *p = obs_filter_get_parent(r->source);
	return (p && obs_scene_from_source(p)) ? p : NULL;
}

static void scene_uuid(struct stage_rig *r, char out[STAGE_SCENE_UUID_MAX])
{
	pthread_mutex_lock(&r->lock);
	stage_strlcpy(out, r->scene_uuid, STAGE_SCENE_UUID_MAX);
	pthread_mutex_unlock(&r->lock);
}

static void push_layer(struct stage_rig *r, int64_t id, int role, float depth, unsigned mask)
{
	struct stage_cmd c;
	memset(&c, 0, sizeof(c));
	c.type = STAGE_CMD_LAYER_SET;
	c.u.layer.item_id = id;
	c.u.layer.role = (uint8_t)(role >= 0 ? role : 0);
	c.u.layer.depth = depth;
	c.u.layer.mask = (uint8_t)mask;
	if (rig_push(r, &c))
		rig_pending_layer_set(r, id, role, depth, mask);
}

static void mark_modified(struct stage_rig *r)
{
	pthread_mutex_lock(&r->lock);
	r->look_modified = true;
	pthread_mutex_unlock(&r->lock);
}

/* Layer rows: the rig's snapshot, or (before its first tick) the scene. */
struct scan_ctx {
	struct stage_layer_info *out;
	size_t n, cap;
};

static bool scan_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	(void)scene;
	struct scan_ctx *c = param;
	if (c->n >= c->cap)
		return false;
	struct stage_layer_info *o = &c->out[c->n++];
	memset(o, 0, sizeof(*o));
	o->item_id = obs_sceneitem_get_id(item);
	obs_source_t *src = obs_sceneitem_get_source(item);
	stage_strlcpy(o->name, src ? obs_source_get_name(src) : "", sizeof(o->name));
	obs_data_t *priv = obs_sceneitem_get_private_settings(item);
	if (priv && obs_data_has_user_value(priv, STAGE_ITEM_KEY_DEPTH)) {
		o->enrolled = true;
		o->depth = stage_depth_clamp((float)obs_data_get_double(priv, STAGE_ITEM_KEY_DEPTH));
		int role = stage_role_from_name(obs_data_get_string(priv, STAGE_ITEM_KEY_ROLE));
		o->role = (uint8_t)(role > 0 ? role : STAGE_ROLE_CHAR);
	}
	obs_data_release(priv);
	return true;
}

static size_t panel_layers(struct stage_rig *r, obs_source_t *scene, struct stage_layer_info *out,
			   size_t cap)
{
	size_t n = rig_panel_snapshot(r, out, cap);
	if (n || !scene)
		return n;
	struct scan_ctx c = {out, 0, cap};
	obs_scene_enum_items(obs_scene_from_source(scene), scan_cb, &c);
	rig_pending_apply(r, out, c.n);
	return c.n;
}

/* ------------------------------------------------------------------------- */
/* Look (recipes, presets module)                                            */
/* ------------------------------------------------------------------------- */

#ifdef STAGE_HAS_PRESETS
static const char *loc_name(const struct stage_loc_name *n)
{
	const char *loc = obs_get_locale();
	if (loc && strncmp(loc, "ja", 2) == 0 && n->ja[0])
		return n->ja;
	if (loc && strncmp(loc, "id", 2) == 0 && n->id[0])
		return n->id;
	return n->en;
}

static void apply_look(struct stage_rig *r, const char *id)
{
	char uuid[STAGE_SCENE_UUID_MAX], err[160];
	scene_uuid(r, uuid);
	struct stage_recipe_result res;
	int rc = stage_recipe_apply_id(uuid, id, STAGE_RECIPE_DURATION_DEFAULT, &res, err, sizeof(err));
	if (rc >= 0) {
		SLOG_WARN("look '%s' not applied: %s", id, err);
		return;
	}
	struct stage_recipe rec;
	char layers[16];
	snprintf(layers, sizeof(layers), "%d", res.layers);
	pthread_mutex_lock(&r->lock);
	stage_strlcpy(r->look_applied, id, sizeof(r->look_applied));
	r->look_modified = false;
	pthread_mutex_unlock(&r->lock);
	rig_set_status(r, "Stage.Info.LookApplied", stage_recipe_find(id, &rec) ? loc_name(&rec.name) : id,
		       layers, OBS_TEXT_INFO_NORMAL);
}

static bool look_modified_cb(void *priv, obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	(void)props;
	(void)p;
	struct stage_rig *r = priv;
	const char *id = obs_data_get_string(s, RS_LOOK);
	pthread_mutex_lock(&r->lock);
	bool same = strcmp(id, r->look_applied) == 0;
	pthread_mutex_unlock(&r->lock);
	if (same || !id[0])
		return false;
	apply_look(r, id);
	return true;
}

static bool apply_look_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	(void)props;
	(void)p;
	struct stage_rig *r = data;
	obs_data_t *s = obs_source_get_settings(r->source);
	char id[STAGE_ID_MAX];
	stage_strlcpy(id, obs_data_get_string(s, RS_LOOK), sizeof(id));
	obs_data_release(s);
	if (id[0])
		apply_look(r, id);
	return true;
}

static bool save_as_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	(void)p;
	struct stage_rig *r = data;
	obs_data_t *s = obs_source_get_settings(r->source);
	bool open = !obs_data_get_bool(s, RS_SAVE_OPEN);
	obs_data_set_bool(s, RS_SAVE_OPEN, open);
	obs_data_release(s);
	obs_property_set_visible(obs_properties_get(props, RS_NEW_LOOK_NAME), open);
	obs_property_set_visible(obs_properties_get(props, RS_SAVE_LOOK), open);
	return true;
}

static bool save_look_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	(void)props;
	(void)p;
	struct stage_rig *r = data;
	obs_data_t *s = obs_source_get_settings(r->source);
	char name[STAGE_VIEW_NAME_MAX];
	stage_strlcpy(name, obs_data_get_string(s, RS_NEW_LOOK_NAME), sizeof(name));
	char uuid[STAGE_SCENE_UUID_MAX], id[STAGE_ID_MAX] = "";
	scene_uuid(r, uuid);
	enum stage_save_result res = stage_recipe_save_user(uuid, name, id);
	switch (res) {
	case STAGE_SAVE_OK:
		obs_data_set_string(s, RS_LOOK, id);
		obs_data_set_bool(s, RS_SAVE_OPEN, false);
		obs_data_set_string(s, RS_NEW_LOOK_NAME, "");
		pthread_mutex_lock(&r->lock);
		stage_strlcpy(r->look_applied, id, sizeof(r->look_applied));
		r->look_modified = false;
		pthread_mutex_unlock(&r->lock);
		rig_set_status(r, "Stage.Info.LookSaved", name, NULL, OBS_TEXT_INFO_NORMAL);
		break;
	case STAGE_SAVE_NAME_EMPTY:
	case STAGE_SAVE_NAME_INVALID:
		rig_set_status(r, "Stage.Info.LookNameEmpty", NULL, NULL, OBS_TEXT_INFO_ERROR);
		break;
	case STAGE_SAVE_NAME_TAKEN:
		rig_set_status(r, "Stage.Info.LookNameTaken", name, NULL, OBS_TEXT_INFO_ERROR);
		break;
	default:
		SLOG_WARN("saving look '%s' failed (%d)", name, (int)res);
		break;
	}
	obs_data_release(s);
	return true;
}

static void add_look_rows(struct stage_rig *r, obs_properties_t *props, obs_data_t *s)
{
	obs_property_t *p = obs_properties_add_list(props, RS_LOOK, T_("Stage.Prop.Look"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	set_desc(p, "Stage.Prop.Look");
	pthread_mutex_lock(&r->lock);
	char applied[STAGE_ID_MAX];
	stage_strlcpy(applied, r->look_applied, sizeof(applied));
	bool modified = r->look_modified;
	pthread_mutex_unlock(&r->lock);
	size_t n = stage_recipe_count();
	bool sep = false;
	for (int pass = 0; pass < 2; pass++) {
		for (size_t i = 0; i < n; i++) {
			struct stage_recipe rec;
			if (!stage_recipe_at(i, &rec) || rec.user != (pass == 1))
				continue;
			if (pass == 1 && !sep) {
				size_t idx = obs_property_list_add_string(p, T_("Stage.List.YourLooks"), "");
				obs_property_list_item_disable(p, idx, true);
				sep = true;
			}
			char label[STAGE_LOC_NAME_MAX + 64];
			if (modified && strcmp(rec.id, applied) == 0)
				rig_format(label, sizeof(label), "Stage.List.Modified", loc_name(&rec.name), NULL);
			else
				stage_strlcpy(label, loc_name(&rec.name), sizeof(label));
			obs_property_list_add_string(p, label, rec.id);
		}
	}
	obs_property_set_modified_callback2(p, look_modified_cb, r);
	obs_properties_add_button2(props, RS_APPLY_LOOK, T_("Stage.Button.ApplyLook"), apply_look_clicked, r);
	obs_properties_add_button2(props, RS_SAVE_AS_LOOK, T_("Stage.Button.SaveAsLook"), save_as_clicked, r);
	bool open = obs_data_get_bool(s, RS_SAVE_OPEN);
	p = obs_properties_add_text(props, RS_NEW_LOOK_NAME, T_("Stage.Prop.NewLookName"), OBS_TEXT_DEFAULT);
	set_desc(p, "Stage.Prop.NewLookName");
	obs_property_set_visible(p, open);
	p = obs_properties_add_button2(props, RS_SAVE_LOOK, T_("Stage.Button.SaveLook"), save_look_clicked, r);
	obs_property_set_visible(p, open);
}
#endif

/* ------------------------------------------------------------------------- */
/* Callbacks                                                                 */
/* ------------------------------------------------------------------------- */

static bool live_safe_modified(void *priv, obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	(void)props;
	(void)p;
	struct stage_rig *r = priv;
	bool on = obs_data_get_bool(s, RS_LIVE_SAFE);
	pthread_mutex_lock(&r->lock);
	bool showing = strcmp(r->status_key, "Stage.LivesafeOffWarning") == 0;
	pthread_mutex_unlock(&r->lock);
	if (!on && !showing)
		rig_set_status(r, "Stage.LivesafeOffWarning", NULL, NULL, OBS_TEXT_INFO_WARNING);
	else if (on && showing)
		rig_set_status(r, "", NULL, NULL, OBS_TEXT_INFO_NORMAL);
	return false;
}

static int64_t id_from_prop(obs_property_t *p, const char *prefix)
{
	const char *name = obs_property_name(p);
	size_t n = strlen(prefix);
	if (!name || strncmp(name, prefix, n) != 0)
		return -1;
	return (int64_t)strtoll(name + n, NULL, 10);
}

static bool snapshot_find(struct stage_rig *r, int64_t id, struct stage_layer_info *out)
{
	struct stage_layer_info *all = bmalloc(sizeof(*all) * MAX_PANEL_LAYERS);
	size_t n = rig_panel_snapshot(r, all, MAX_PANEL_LAYERS);
	bool ok = false;
	for (size_t i = 0; i < n && !ok; i++) {
		if (all[i].item_id == id) {
			*out = all[i];
			ok = true;
		}
	}
	bfree(all);
	return ok;
}

static bool role_modified(void *priv, obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	struct stage_rig *r = priv;
	int64_t id = id_from_prop(p, RS_ROLE_PREFIX);
	if (id < 0)
		return false;
	int role = stage_role_from_name(obs_data_get_string(s, obs_property_name(p)));
	if (role < 0)
		role = STAGE_ROLE_NONE;
	char dkey[64];
	snprintf(dkey, sizeof(dkey), RS_DEPTH_PREFIX "%lld", (long long)id);
	obs_property_set_visible(obs_properties_get(props, dkey), role_has_depth(role));

	struct stage_layer_info L;
	if (!snapshot_find(r, id, &L))
		return true;
	int cur = L.enrolled ? L.role : STAGE_ROLE_NONE;
	if (cur == role)
		return true;
	if (role == STAGE_ROLE_NONE) {
		push_layer(r, id, STAGE_ROLE_NONE, 0.0f, STAGE_LAYER_UNENROL);
	} else {
		push_layer(r, id, role, 0.0f, STAGE_LAYER_ROLE);
		if (!L.enrolled) /* the slider shows the role's default depth */
			obs_data_set_int(s, dkey, lroundf(stage_ui_from_depth(stage_role_default_depth(role))));
	}
	mark_modified(r);
	return true;
}

static bool depth_modified(void *priv, obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	(void)props;
	struct stage_rig *r = priv;
	int64_t id = id_from_prop(p, RS_DEPTH_PREFIX);
	struct stage_layer_info L;
	if (id < 0 || !snapshot_find(r, id, &L) || !L.enrolled)
		return false;
	long long v = obs_data_get_int(s, obs_property_name(p));
	if (llabs(v - lroundf(stage_ui_from_depth((float)L.depth))) < 1)
		return false;
	push_layer(r, id, 0, stage_depth_from_ui((float)v), STAGE_LAYER_DEPTH);
	mark_modified(r);
	return false;
}

static bool auto_assign_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	(void)p;
	struct stage_rig *r = data;
	obs_source_t *scene = rig_scene(r);
	struct stage_layer_info *all = bmalloc(sizeof(*all) * MAX_PANEL_LAYERS);
	size_t n = panel_layers(r, scene, all, MAX_PANEL_LAYERS);
	obs_data_t *s = obs_source_get_settings(r->source);
	int assigned = 0;
	for (size_t i = 0; i < n; i++) {
		if (all[i].enrolled)
			continue;
		int role = stage_cam_guess_role(all[i].name);
		if (role == STAGE_ROLE_NONE)
			continue;
		push_layer(r, all[i].item_id, role, 0.0f, STAGE_LAYER_ROLE);
		/* The rows show it now (the panel is redrawn from the settings
		 * when this returns true, before the tick applies the roles). */
		char rkey[64], dkey[64];
		snprintf(rkey, sizeof(rkey), RS_ROLE_PREFIX "%lld", (long long)all[i].item_id);
		snprintf(dkey, sizeof(dkey), RS_DEPTH_PREFIX "%lld", (long long)all[i].item_id);
		obs_data_set_string(s, rkey, stage_role_names[role]);
		obs_data_set_int(s, dkey, lroundf(stage_ui_from_depth(stage_role_default_depth(role))));
		obs_property_set_visible(obs_properties_get(props, dkey), role_has_depth(role));
		assigned++;
	}
	obs_data_release(s);
	bfree(all);
	if (!assigned)
		rig_set_status(r, "Stage.Info.NoRoles", NULL, NULL, OBS_TEXT_INFO_NORMAL);
	else
		mark_modified(r);
	return true;
}

static bool refresh_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	(void)props;
	(void)p;
	struct stage_rig *r = data;
	os_atomic_set_long(&r->dirty, 1);
	return true;
}

static bool view_go_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	(void)props;
	(void)p;
	struct stage_rig *r = data;
	obs_data_t *s = obs_source_get_settings(r->source);
	uint8_t slot = (uint8_t)obs_data_get_int(s, RS_VIEW_SLOT);
	obs_data_release(s);
	char sl[8];
	snprintf(sl, sizeof(sl), "%u", (unsigned)slot);
	obs_source_t *scene = rig_scene(r);
	if (!scene || !stage_cam_view_get(scene, slot, NULL, NULL, NULL, NULL, NULL, 0)) {
		rig_set_status(r, "Stage.Info.ViewEmpty", sl, NULL, OBS_TEXT_INFO_NORMAL);
		return false;
	}
	struct stage_cmd c;
	memset(&c, 0, sizeof(c));
	c.type = STAGE_CMD_VIEW_APPLY;
	c.u.view.slot = slot;
	rig_push(r, &c);
	return false;
}

static bool view_save_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	(void)props;
	(void)p;
	struct stage_rig *r = data;
	obs_source_t *scene = rig_scene(r);
	if (!scene)
		return false;
	obs_data_t *s = obs_source_get_settings(r->source);
	uint8_t slot = (uint8_t)obs_data_get_int(s, RS_VIEW_SLOT);
	char sl[8];
	snprintf(sl, sizeof(sl), "%u", (unsigned)slot);
	char name[STAGE_VIEW_NAME_MAX];
	stage_strlcpy(name, obs_data_get_string(s, RS_VIEW_NAME), sizeof(name));
	if (!name[0] || !stage_view_name_valid(name))
		rig_format(name, sizeof(name), "Stage.List.ViewDefault", sl, NULL);
	if (!stage_view_name_valid(name))
		snprintf(name, sizeof(name), "View %s", sl);
	int tr = stage_transition_from_name(obs_data_get_string(s, RS_VIEW_TRANSITION));
	uint32_t dur = (uint32_t)obs_data_get_int(s, RS_VIEW_DURATION);
	obs_data_release(s);

	/* Saves the camera target (not a mid-transition pose), like view_save. */
	struct stage_cam t;
	pthread_mutex_lock(&r->lock);
	stage_rig_model_target(&r->m, &t);
	pthread_mutex_unlock(&r->lock);
	if (stage_cam_view_put(scene, slot, name, &t, (uint8_t)(tr < 0 ? STAGE_TRANSITION_SMOOTH : tr), dur))
		rig_set_status(r, "Stage.Info.ViewSaved", sl, NULL, OBS_TEXT_INFO_NORMAL);
	return true;
}

static bool view_slot_modified(void *priv, obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	(void)props;
	(void)p;
	struct stage_rig *r = priv;
	obs_source_t *scene = rig_scene(r);
	uint8_t slot = (uint8_t)obs_data_get_int(s, RS_VIEW_SLOT);
	char name[STAGE_VIEW_NAME_MAX];
	if (scene && stage_cam_view_get(scene, slot, NULL, NULL, NULL, NULL, name, sizeof(name)))
		obs_data_set_string(s, RS_VIEW_NAME, name);
	return true;
}

static void reset_cam_fields(struct stage_rig *r, obs_data_t *s)
{
	obs_data_set_double(s, RS_CAM_X, 0.0);
	obs_data_set_double(s, RS_CAM_Y, 0.0);
	obs_data_set_double(s, RS_CAM_ZOOM, 1.0);
	obs_data_set_double(s, RS_CAM_ROT, 0.0);
	pthread_mutex_lock(&r->lock);
	r->set.cam_x = r->set.cam_y = r->set.cam_rot = 0.0;
	r->set.cam_zoom = 1.0;
	pthread_mutex_unlock(&r->lock);
}

static bool home_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	(void)props;
	(void)p;
	struct stage_rig *r = data;
	obs_data_t *s = obs_source_get_settings(r->source);
	reset_cam_fields(r, s);
	obs_data_release(s);
	struct stage_cmd c;
	memset(&c, 0, sizeof(c));
	c.type = STAGE_CMD_CAMERA_RESET;
	c.duration_ms = STAGE_CAM_RESET_MS;
	c.ease = STAGE_EASE_DEFAULT;
	rig_push(r, &c);
	return true;
}

static bool reset_camera_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	home_clicked(props, p, data);
	struct stage_rig *r = data;
	obs_data_t *s = obs_source_get_settings(r->source);
	obs_data_set_string(s, RS_AUTO_MODE, "off");
	obs_data_release(s);
	pthread_mutex_lock(&r->lock);
	r->set.auto_mode = STAGE_AUTO_OFF;
	pthread_mutex_unlock(&r->lock);
	struct stage_cmd c;
	memset(&c, 0, sizeof(c));
	c.type = STAGE_CMD_CAMERA_AUTO;
	c.u.autom.mode = STAGE_AUTO_OFF;
	c.intensity = -1.0f;
	rig_push(r, &c);
	return true;
}

static bool bake_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	(void)props;
	(void)p;
	struct stage_rig *r = data;
	obs_data_t *s = obs_source_get_settings(r->source);
	reset_cam_fields(r, s);
	obs_data_release(s);
	rig_request_bake(r);
	return true;
}

struct audio_enum_ctx {
	obs_property_t *list;
};

static bool audio_enum_cb(void *param, obs_source_t *src)
{
	struct audio_enum_ctx *c = param;
	if ((obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) &&
	    obs_source_get_type(src) != OBS_SOURCE_TYPE_FILTER)
		obs_property_list_add_string(c->list, obs_source_get_name(src), obs_source_get_uuid(src));
	return true;
}

/* ------------------------------------------------------------------------- */
/* Panel                                                                     */
/* ------------------------------------------------------------------------- */

static obs_property_t *add_int_pct(obs_properties_t *g, const char *key, const char *label, int lo,
				   int hi, const char *suffix)
{
	obs_property_t *p = obs_properties_add_int_slider(g, key, T_(label), lo, hi, 1);
	obs_property_int_set_suffix(p, suffix);
	set_desc(p, label);
	return p;
}

static void add_layer_rows(struct stage_rig *r, obs_properties_t *g, obs_source_t *scene, obs_data_t *s)
{
	struct stage_layer_info *all = bmalloc(sizeof(*all) * MAX_PANEL_LAYERS);
	size_t n = panel_layers(r, scene, all, MAX_PANEL_LAYERS);

	/* Drop the rows of items that are gone (keys collected first: erasing
	 * while iterating would invalidate the iterator). */
	struct dstr stale = {0};
	for (obs_data_item_t *it = obs_data_first(s); it; obs_data_item_next(&it)) {
		const char *k = obs_data_item_get_name(it);
		const char *num = NULL;
		if (strncmp(k, RS_ROLE_PREFIX, strlen(RS_ROLE_PREFIX)) == 0)
			num = k + strlen(RS_ROLE_PREFIX);
		else if (strncmp(k, RS_DEPTH_PREFIX, strlen(RS_DEPTH_PREFIX)) == 0)
			num = k + strlen(RS_DEPTH_PREFIX);
		if (!num)
			continue;
		int64_t id = strtoll(num, NULL, 10);
		bool gone = true;
		for (size_t i = 0; i < n && gone; i++)
			gone = all[i].item_id != id;
		if (gone) {
			dstr_cat(&stale, k);
			dstr_cat_ch(&stale, '\n');
		}
	}
	for (char *k = stale.array; k && *k;) {
		char *nl = strchr(k, '\n');
		*nl = '\0';
		obs_data_erase(s, k);
		k = nl + 1;
	}
	dstr_free(&stale);

	int blur_layers = 0;
	for (size_t i = 0; i < n; i++) {
		const struct stage_layer_info *L = &all[i];
		char rkey[64], dkey[64];
		snprintf(rkey, sizeof(rkey), RS_ROLE_PREFIX "%lld", (long long)L->item_id);
		snprintf(dkey, sizeof(dkey), RS_DEPTH_PREFIX "%lld", (long long)L->item_id);
		int role = L->enrolled ? L->role : STAGE_ROLE_NONE;
		obs_data_set_string(s, rkey, stage_role_names[role % STAGE_ROLE_COUNT]);
		obs_data_set_int(s, dkey,
				 L->enrolled ? lroundf(stage_ui_from_depth((float)L->depth))
					     : lroundf(stage_ui_from_depth(stage_role_default_depth(role))));
		if (L->depth_blur && L->enrolled && (role == STAGE_ROLE_BG || role == STAGE_ROLE_FG ||
						     role == STAGE_ROLE_SKY))
			blur_layers++;

		obs_property_t *p = obs_properties_add_list(g, rkey, L->name[0] ? L->name : "-",
							    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		for (int k = 0; k < STAGE_ROLE_COUNT; k++)
			obs_property_list_add_string(p, T_(role_keys[k]), stage_role_names[k]);
		obs_property_set_modified_callback2(p, role_modified, r);

		p = obs_properties_add_int_slider(g, dkey, T_("Stage.Prop.LayerDepth"),
						  (int)STAGE_UI_DEPTH_MIN, (int)STAGE_UI_DEPTH_MAX, 1);
		set_desc(p, "Stage.Prop.LayerDepth");
		obs_property_set_visible(p, role_has_depth(role));
		obs_property_set_modified_callback2(p, depth_modified, r);
	}
	bfree(all);

	char cnt[16];
	char info[256];
	snprintf(cnt, sizeof(cnt), "%d", blur_layers);
	if (blur_layers)
		rig_format(info, sizeof(info), "Stage.Info.DofLayers", cnt, NULL);
	else
		rig_format(info, sizeof(info), "Stage.Info.DofNoBlur", NULL, NULL);
	obs_data_set_string(s, RS_DOF_INFO, info);
}

obs_properties_t *rig_properties(void *data)
{
	struct stage_rig *r = data;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;
	if (!r)
		return props;
	obs_source_t *scene = rig_scene(r);
	obs_data_t *s = obs_source_get_settings(r->source);

	if (!scene) {
		char text[256];
		rig_format(text, sizeof(text), "Stage.Info.NotScene", NULL, NULL);
		obs_data_set_string(s, RS_STATUS, text);
		p = obs_properties_add_text(props, RS_STATUS, T_("Stage.Prop.Status"), OBS_TEXT_INFO);
		obs_property_text_set_info_type(p, OBS_TEXT_INFO_WARNING);
		obs_data_release(s);
		return props;
	}

#ifdef STAGE_HAS_PRESETS
	add_look_rows(r, props, s);
#endif
	p = obs_properties_add_text(props, RS_STATUS, T_("Stage.Prop.Status"), OBS_TEXT_INFO);
	pthread_mutex_lock(&r->lock);
	enum obs_text_info_type st = r->status_type;
	bool has_status = r->status_key[0] != '\0';
	pthread_mutex_unlock(&r->lock);
	obs_property_text_set_info_type(p, st);
	obs_property_set_visible(p, has_status);

	p = obs_properties_add_bool(props, RS_LIVE_SAFE, T_("Stage.Prop.LiveSafe"));
	set_desc(p, "Stage.Prop.LiveSafe");
	obs_property_set_modified_callback2(p, live_safe_modified, r);

	/* Camera */
	obs_properties_t *g = obs_properties_create();
	p = obs_properties_add_list(g, RS_VIEW_SLOT, T_("Stage.Prop.View"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	set_desc(p, "Stage.Prop.View");
	for (int slot = STAGE_VIEW_SLOT_MIN; slot <= STAGE_VIEW_SLOT_MAX; slot++) {
		char sl[8], name[STAGE_VIEW_NAME_MAX], label[STAGE_VIEW_NAME_MAX + 32];
		snprintf(sl, sizeof(sl), "%d", slot);
		if (stage_cam_view_get(scene, (uint8_t)slot, NULL, NULL, NULL, NULL, name, sizeof(name)))
			snprintf(label, sizeof(label), "%d \xC2\xB7 %s", slot, name);
		else
			rig_format(label, sizeof(label), "Stage.List.ViewEmpty", sl, NULL);
		obs_property_list_add_int(p, label, slot);
	}
	obs_property_set_modified_callback2(p, view_slot_modified, r);
	obs_properties_add_button2(g, RS_VIEW_GO, T_("Stage.Button.GoToView"), view_go_clicked, r);
	obs_properties_add_button2(g, RS_VIEW_SAVE, T_("Stage.Button.SaveView"), view_save_clicked, r);
	p = obs_properties_add_text(g, RS_VIEW_NAME, T_("Stage.Prop.ViewName"), OBS_TEXT_DEFAULT);
	set_desc(p, "Stage.Prop.ViewName");
	p = obs_properties_add_list(g, RS_VIEW_TRANSITION, T_("Stage.Prop.ViewTransition"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	set_desc(p, "Stage.Prop.ViewTransition");
	for (int i = 0; i < STAGE_TRANSITION_COUNT; i++)
		obs_property_list_add_string(p, T_(transition_keys[i]), stage_transition_names[i]);
	p = obs_properties_add_int(g, RS_VIEW_DURATION, T_("Stage.Prop.TransitionTime"), 0,
				   STAGE_SET_DURATION_MAX, 50);
	obs_property_int_set_suffix(p, " ms");
	set_desc(p, "Stage.Prop.TransitionTime");
	p = obs_properties_add_list(g, RS_AUTO_MODE, T_("Stage.Prop.AutoCamera"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	set_desc(p, "Stage.Prop.AutoCamera");
	for (int i = 0; i < STAGE_AUTO_COUNT; i++)
		obs_property_list_add_string(p, T_(auto_keys[i]), stage_auto_mode_names[i]);
	add_int_pct(g, RS_AUTO_INTENSITY, "Stage.Prop.MotionIntensity", 0, 100, " %");
	p = obs_properties_add_bool(g, RS_FREEZE, T_("Stage.Prop.Freeze"));
	set_desc(p, "Stage.Prop.Freeze");
	obs_properties_add_group(props, RS_GRP_CAMERA, T_("Stage.Group.Camera"), OBS_GROUP_NORMAL, g);

	/* Manual position */
	float W = (float)obs_source_get_width(scene), H = (float)obs_source_get_height(scene);
	if (!(W > 0.0f && H > 0.0f)) {
		W = 1920.0f;
		H = 1080.0f;
	}
	g = obs_properties_create();
	p = obs_properties_add_float_slider(g, RS_CAM_X, T_("Stage.Prop.PanX"), -W, W, 1.0);
	obs_property_float_set_suffix(p, " px");
	set_desc(p, "Stage.Prop.PanX");
	p = obs_properties_add_float_slider(g, RS_CAM_Y, T_("Stage.Prop.PanY"), -H, H, 1.0);
	obs_property_float_set_suffix(p, " px");
	set_desc(p, "Stage.Prop.PanY");
	p = obs_properties_add_float_slider(g, RS_CAM_ZOOM, T_("Stage.Prop.Zoom"), STAGE_ZOOM_MIN,
					    STAGE_ZOOM_MAX, 0.01);
	obs_property_float_set_suffix(p, "x");
	set_desc(p, "Stage.Prop.Zoom");
	p = obs_properties_add_float_slider(g, RS_CAM_ROT, T_("Stage.Prop.Roll"), STAGE_ROT_MIN,
					    STAGE_ROT_MAX, 0.1);
	obs_property_float_set_suffix(p, "\xC2\xB0");
	set_desc(p, "Stage.Prop.Roll");
	obs_properties_add_button2(g, RS_CAM_HOME, T_("Stage.Button.Home"), home_clicked, r);
	obs_properties_add_group(props, RS_GRP_MANUAL, T_("Stage.Group.Manual"), OBS_GROUP_NORMAL, g);

	/* Layers */
	g = obs_properties_create();
	obs_properties_add_text(g, RS_LAYERS_HELP, T_("Stage.Info.LayersHelp"), OBS_TEXT_INFO);
	obs_properties_add_button2(g, RS_AUTO_ASSIGN, T_("Stage.Button.AutoAssign"), auto_assign_clicked, r);
	obs_properties_add_button2(g, RS_REFRESH_LAYERS, T_("Stage.Button.RefreshLayers"), refresh_clicked, r);
	add_layer_rows(r, g, scene, s);
	obs_properties_add_group(props, RS_GRP_LAYERS, T_("Stage.Group.Layers"), OBS_GROUP_NORMAL, g);

	/* Depth of field (checkable group = amount > 0) */
	g = obs_properties_create();
	p = obs_properties_add_int_slider(g, RS_DOF_FOCUS, T_("Stage.Prop.DofFocus"), (int)STAGE_UI_DEPTH_MIN,
					  (int)STAGE_UI_DEPTH_MAX, 1);
	set_desc(p, "Stage.Prop.DofFocus");
	add_int_pct(g, RS_DOF_AMOUNT, "Stage.Prop.DofAmount", 0, 100, " %");
	p = obs_properties_add_bool(g, RS_DOF_FOLLOW, T_("Stage.Prop.DofFollowChar"));
	set_desc(p, "Stage.Prop.DofFollowChar");
	obs_properties_add_text(g, RS_DOF_INFO, T_("Stage.Prop.DofStatus"), OBS_TEXT_INFO);
	obs_properties_add_group(props, RS_DOF_ENABLED, T_("Stage.Group.Dof"), OBS_GROUP_CHECKABLE, g);

	/* Audio bounce */
	g = obs_properties_create();
	p = obs_properties_add_list(g, RS_AUDIO_SOURCE, T_("Stage.Prop.AudioSource"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	set_desc(p, "Stage.Prop.AudioSource");
	obs_property_list_add_string(p, T_("Stage.List.None"), "");
	struct audio_enum_ctx ac = {p};
	obs_enum_sources(audio_enum_cb, &ac);
	add_int_pct(g, RS_AUDIO_AMOUNT, "Stage.Prop.AudioAmount", 0, 100, " %");
	add_int_pct(g, RS_AUDIO_ATTACK, "Stage.Prop.AudioAttack", 1, 500, " ms");
	add_int_pct(g, RS_AUDIO_RELEASE, "Stage.Prop.AudioRelease", 10, 2000, " ms");
	obs_properties_add_group(props, RS_GRP_AUDIO, T_("Stage.Group.Audio"), OBS_GROUP_NORMAL, g);

	/* Advanced */
	g = obs_properties_create();
	add_int_pct(g, RS_SMOOTHING, "Stage.Prop.Smoothing", 0, 1000, " ms");
	add_int_pct(g, RS_OVERSCAN, "Stage.Prop.Overscan", 0, 25, " %");
	p = obs_properties_add_float_slider(g, RS_MAX_ZOOM, T_("Stage.Prop.MaxZoom"), 1.0, STAGE_ZOOM_MAX, 0.05);
	obs_property_float_set_suffix(p, "x");
	set_desc(p, "Stage.Prop.MaxZoom");
	add_int_pct(g, RS_MAX_PAN, "Stage.Prop.MaxPan", 0, 50, " %");
	add_int_pct(g, RS_MOTION_SCALE, "Stage.Prop.MotionScale", 0, 100, " %");
	add_int_pct(g, RS_MANUAL_PAUSE, "Stage.Prop.ManualPause", 0, 60, " s");
	obs_properties_add_button2(g, RS_BAKE_REST, T_("Stage.Button.BakeRest"), bake_clicked, r);
	obs_properties_add_button2(g, RS_RESET_CAMERA, T_("Stage.Button.ResetCamera"), reset_camera_clicked, r);
	obs_properties_add_group(props, RS_GRP_ADVANCED, T_("Stage.Group.Advanced"), OBS_GROUP_NORMAL, g);

	/* About */
	char about[256], proto[8];
	snprintf(proto, sizeof(proto), "%d", STAGE_PROTOCOL_VERSION);
	rig_format(about, sizeof(about), "Stage.Info.About", STAGE_VERSION, proto);
	obs_data_set_string(s, RS_ABOUT, about);
	obs_properties_add_text(props, RS_ABOUT, T_("Stage.Prop.About"), OBS_TEXT_INFO);
	p = obs_properties_add_button2(props, RS_DOCS, T_("Stage.Button.Docs"), NULL, NULL);
	obs_property_button_set_type(p, OBS_BUTTON_URL);
	obs_property_button_set_url(p, (char *)STAGE_CAM_DOCS_URL);
	p = obs_properties_add_button2(props, RS_REPORT_BUG, T_("Stage.Button.ReportBug"), NULL, NULL);
	obs_property_button_set_type(p, OBS_BUTTON_URL);
	obs_property_button_set_url(p, (char *)STAGE_CAM_BUGS_URL);

	obs_data_release(s);
	return props;
}

/* ------------------------------------------------------------------------- */
/* First add                                                                 */
/* ------------------------------------------------------------------------- */

void rig_first_add(struct stage_rig *r, obs_source_t *scene_source)
{
	struct stage_layer_info *all = bmalloc(sizeof(*all) * MAX_PANEL_LAYERS);
	struct scan_ctx c = {all, 0, MAX_PANEL_LAYERS};
	obs_scene_enum_items(obs_scene_from_source(scene_source), scan_cb, &c);
	bool any_enrolled = false;
	for (size_t i = 0; i < c.n; i++)
		any_enrolled |= all[i].enrolled;
	int assigned = 0;
	if (!any_enrolled) {
		for (size_t i = 0; i < c.n; i++) {
			int role = stage_cam_guess_role(all[i].name);
			if (role == STAGE_ROLE_NONE)
				continue;
			push_layer(r, all[i].item_id, role, 0.0f, STAGE_LAYER_ROLE);
			assigned++;
		}
	}
	bfree(all);
	if (any_enrolled)
		return;
	if (!assigned) {
		rig_set_status(r, "Stage.Info.NoRoles", NULL, NULL, OBS_TEXT_INFO_NORMAL);
		return;
	}
#ifdef STAGE_HAS_PRESETS
	obs_data_t *s = obs_source_get_settings(r->source);
	obs_data_set_string(s, RS_LOOK, "cozy_chat");
	obs_data_release(s);
	apply_look(r, "cozy_chat");
#endif
	char uuid[STAGE_SCENE_UUID_MAX];
	scene_uuid(r, uuid);
	if (!stage_fx_exists(uuid))
		rig_set_status(r, "Stage.Info.NeedFx", NULL, NULL, OBS_TEXT_INFO_NORMAL);
}
