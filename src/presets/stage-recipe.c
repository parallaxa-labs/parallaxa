/*
 * stage-recipe.c — recipe application (one command batch) and user recipes.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <obs.h>
#include <stdio.h>
#include <string.h>
#include <util/bmem.h>
#include <util/platform.h>

#include "stage-core.h"
#include "stage-json.h"
#include "stage-presets.h"
#include "stage-target.h"

/* Rig settings keys (docs/ux/stage-plugin-ux.md section 1, owned by the
 * Camera Rig): recipe overscan and audio react are panel settings, not
 * commands. */
#define RIG_KEY_OVERSCAN_PCT "overscan_pct"
#define RIG_KEY_AUDIO_AMOUNT "audio_amount"
#define RIG_KEY_AUDIO_ATTACK "audio_attack_ms"
#define RIG_KEY_AUDIO_RELEASE "audio_release_ms"

/* Layers read per recipe (more than the queue could take anyway). */
#define MAX_RECIPE_LAYERS 96

static void set_err(char *err, size_t cap, const char *msg)
{
	if (err && cap)
		snprintf(err, cap, "%s", msg);
}

/* ------------------------------------------------------------------------- */
/* Role guess                                                                 */
/* ------------------------------------------------------------------------- */

static bool is_letter(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* Case-insensitive search of an ASCII token in s; whole_word requires
 * non-letters (or the ends) around it. */
static bool has_token(const char *s, const char *tok, bool whole_word)
{
	size_t n = strlen(tok);
	for (const char *p = s; *p; p++) {
		size_t i = 0;
		while (i < n && p[i] && (p[i] | 0x20) == tok[i])
			i++;
		if (i < n)
			continue;
		if (!whole_word)
			return true;
		bool left = p == s || !is_letter(p[-1]);
		bool right = !is_letter(p[n]);
		if (left && right)
			return true;
	}
	return false;
}

int stage_guess_role(const char *name)
{
	if (!name)
		return STAGE_ROLE_NONE;
	if (has_token(name, "background", false) || strstr(name, "背景") ||
	    has_token(name, "bg", true))
		return STAGE_ROLE_BG;
	if (has_token(name, "foreground", false) || strstr(name, "前景") ||
	    has_token(name, "fg", true))
		return STAGE_ROLE_FG;
	if (has_token(name, "vtube", false) || has_token(name, "spout", false) ||
	    has_token(name, "avatar", false) || has_token(name, "model", false) ||
	    has_token(name, "vts", true))
		return STAGE_ROLE_CHAR;
	return STAGE_ROLE_NONE;
}

static int recipe_role_slot(int role)
{
	switch (role) {
	case STAGE_ROLE_BG:
		return STAGE_RROLE_BG;
	case STAGE_ROLE_CHAR:
		return STAGE_RROLE_CHAR;
	case STAGE_ROLE_FG:
		return STAGE_RROLE_FG;
	default:
		return -1;
	}
}

/* ------------------------------------------------------------------------- */
/* Apply                                                                      */
/* ------------------------------------------------------------------------- */

static struct stage_cmd make_cmd(enum stage_cmd_type type, const char *scene_uuid, uint64_t id,
				 uint32_t duration_ms)
{
	struct stage_cmd c;
	memset(&c, 0, sizeof(c));
	c.type = type;
	c.id = id;
	stage_strlcpy(c.scene_uuid, scene_uuid, sizeof(c.scene_uuid));
	c.duration_ms = duration_ms;
	c.ease = STAGE_EASE_DEFAULT;
	c.intensity = -1.0f;
	return c;
}

/* Recipe overscan / audio react -> rig filter settings (deferred update). */
static void update_rig_settings(const char *scene_uuid, const struct stage_recipe *r)
{
	if (!r->has_overscan && !r->has_audio)
		return;
	obs_source_t *scene = obs_get_source_by_uuid(scene_uuid);
	obs_source_t *rig = stage_find_filter(scene, STAGE_FILTER_CAMERA_RIG);
	if (rig) {
		obs_data_t *s = obs_data_create();
		if (r->has_overscan)
			obs_data_set_int(s, RIG_KEY_OVERSCAN_PCT, (long long)lround(r->overscan * 100.0));
		if (r->has_audio) {
			obs_data_set_int(s, RIG_KEY_AUDIO_AMOUNT,
					 (long long)lround(r->audio_amount * 100.0));
			if (r->audio_attack_ms > 0)
				obs_data_set_int(s, RIG_KEY_AUDIO_ATTACK, r->audio_attack_ms);
			if (r->audio_release_ms > 0)
				obs_data_set_int(s, RIG_KEY_AUDIO_RELEASE, r->audio_release_ms);
		}
		obs_source_update(rig, s);
		obs_data_release(s);
	}
	obs_source_release(rig);
	obs_source_release(scene);
}

int stage_recipe_apply(const char *scene_uuid, const struct stage_recipe *r, uint32_t duration_ms,
		       struct stage_recipe_result *out, char *err, size_t err_size)
{
	struct stage_recipe_result res;
	memset(&res, 0, sizeof(res));
	if (!r || !scene_uuid || !scene_uuid[0]) {
		set_err(err, err_size, "no scene");
		return STAGE_ERROR_NO_RIG;
	}
	struct stage_state st;
	if (!stage_rig_get_state(scene_uuid, &st)) {
		set_err(err, err_size, "scene has no Parallaxa Camera Rig");
		return STAGE_ERROR_NO_RIG;
	}
	if (duration_ms > STAGE_SET_DURATION_MAX)
		duration_ms = STAGE_SET_DURATION_MAX;

	struct stage_layer_info *layers = bmalloc(sizeof(*layers) * MAX_RECIPE_LAYERS);
	struct stage_cmd *cmds = bmalloc(sizeof(*cmds) * (MAX_RECIPE_LAYERS + 4));
	size_t nlayers = 0, ncmd = 0;
	stage_rig_list_layers(scene_uuid, layers, MAX_RECIPE_LAYERS, &nlayers);

	uint64_t id = stage_cmdq_next_id();
	res.id = id;

	/* 1. Layer depths: role set on the item, else guessed from its name. */
	for (size_t i = 0; i < nlayers; i++) {
		const struct stage_layer_info *l = &layers[i];
		int role = l->enrolled ? l->role : stage_guess_role(l->name);
		int slot = recipe_role_slot(role);
		if (slot < 0 || !r->has_role[slot])
			continue;
		double depth = r->role_depth[slot];
		res.layers++;
		if (l->enrolled && l->role == role && fabs(l->depth - depth) < 1e-6)
			continue; /* already there: nothing to queue */
		struct stage_cmd c = make_cmd(STAGE_CMD_LAYER_SET, scene_uuid, id, duration_ms);
		c.u.layer.item_id = l->item_id;
		c.u.layer.depth = (float)depth;
		c.u.layer.role = (uint8_t)role;
		c.u.layer.mask = STAGE_LAYER_DEPTH | STAGE_LAYER_ROLE;
		cmds[ncmd++] = c;
	}

	/* 2. Auto camera. */
	res.auto_mode = st.auto_mode < STAGE_AUTO_COUNT ? st.auto_mode : STAGE_AUTO_OFF;
	if (r->has_auto) {
		struct stage_cmd c = make_cmd(STAGE_CMD_CAMERA_AUTO, scene_uuid, id, 0);
		c.u.autom.mode = r->auto_mode;
		c.intensity = r->auto_intensity;
		cmds[ncmd++] = c;
		res.auto_mode = r->auto_mode;
	}

	/* 3. FX preset, only when this scene carries Parallaxa FX. */
	if (r->fx[0] && stage_fx_preset_known(r->fx) && stage_fx_exists(scene_uuid)) {
		struct stage_cmd c = make_cmd(STAGE_CMD_FX_SET, scene_uuid, id, duration_ms);
		stage_strlcpy(c.u.fx.preset, r->fx, sizeof(c.u.fx.preset));
		cmds[ncmd++] = c;
		stage_strlcpy(res.fx, r->fx, sizeof(res.fx));
	}

	/* 4. Depth of field. */
	if (r->has_dof) {
		struct stage_cmd c = make_cmd(STAGE_CMD_DOF_SET, scene_uuid, id, duration_ms);
		c.u.dof.focus_depth = r->focus_depth;
		c.u.dof.amount = r->dof_amount;
		c.u.dof.mask = STAGE_DOF_FOCUS | STAGE_DOF_AMOUNT;
		cmds[ncmd++] = c;
	}

	/* 5. Start pose. */
	if (r->has_camera) {
		struct stage_cmd c = make_cmd(STAGE_CMD_CAMERA_SET, scene_uuid, id, duration_ms);
		c.u.set.x = r->cam_x;
		c.u.set.y = r->cam_y;
		c.u.set.zoom = r->cam_zoom;
		c.u.set.rot = r->cam_rot;
		c.u.set.mask = STAGE_CAM_X | STAGE_CAM_Y | STAGE_CAM_ZOOM | STAGE_CAM_ROT;
		cmds[ncmd++] = c;
	}

	/* Queue the batch only if it fits as a whole (best effort: another
	 * producer may still fill the ring between the check and the pushes). */
	int rc = -1;
	struct stage_cmdq_stats qs;
	stage_cmdq_get_stats(&qs);
	if (qs.depth + ncmd > STAGE_CMDQ_CAPACITY) {
		set_err(err, err_size, "command queue is full");
		rc = STAGE_ERROR_BUSY;
	} else {
		for (size_t i = 0; i < ncmd; i++) {
			if (!stage_cmdq_try_push(&cmds[i])) {
				set_err(err, err_size, "command queue is full");
				rc = STAGE_ERROR_BUSY;
				break;
			}
		}
	}
	bfree(cmds);
	bfree(layers);
	if (rc >= 0)
		return rc;

	update_rig_settings(scene_uuid, r);
	if (out)
		*out = res;
	return -1;
}

int stage_recipe_apply_id(const char *scene_uuid, const char *recipe_id, uint32_t duration_ms,
			  struct stage_recipe_result *out, char *err, size_t err_size)
{
	struct stage_recipe r;
	if (!stage_recipe_find(recipe_id, &r)) {
		if (err && err_size)
			snprintf(err, err_size, "unknown recipe '%.32s'", recipe_id ? recipe_id : "");
		return STAGE_ERROR_NOT_FOUND;
	}
	return stage_recipe_apply(scene_uuid, &r, duration_ms, out, err, err_size);
}

/* ------------------------------------------------------------------------- */
/* User recipes                                                               */
/* ------------------------------------------------------------------------- */

void stage_recipe_id_from_name(const char *name, char out[STAGE_ID_MAX])
{
	size_t o = 0;
	bool dash = false;
	for (const unsigned char *p = (const unsigned char *)(name ? name : ""); *p; p++) {
		unsigned char c = *p;
		if (c >= 'A' && c <= 'Z')
			c = (unsigned char)(c | 0x20);
		bool keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
		if (!keep) {
			dash = o > 0;
			continue;
		}
		if (dash && o < STAGE_ID_MAX - 2) {
			out[o++] = '-';
		}
		dash = false;
		if (o < STAGE_ID_MAX - 1)
			out[o++] = (char)c;
	}
	while (o > 0 && out[o - 1] == '-')
		o--;
	out[o] = '\0';
	if (!o) {
		/* No ASCII letters/digits: FNV-1a of the name. */
		uint32_t h = 2166136261u;
		for (const unsigned char *p = (const unsigned char *)(name ? name : ""); *p; p++)
			h = (h ^ *p) * 16777619u;
		snprintf(out, STAGE_ID_MAX, "look-%08x", h);
	}
}

static double clampd(double v, double lo, double hi)
{
	if (!(v == v))
		return lo;
	return v < lo ? lo : (v > hi ? hi : v);
}

enum stage_save_result stage_recipe_save_user(const char *scene_uuid, const char *name,
					      char out_id[STAGE_ID_MAX])
{
	size_t cp = sj_utf8_len(name);
	if (!name || cp == 0)
		return STAGE_SAVE_NAME_EMPTY;
	if (cp == (size_t)-1 || cp > STAGE_VIEW_NAME_MAX_CHARS)
		return STAGE_SAVE_NAME_INVALID;
	char id[STAGE_ID_MAX];
	stage_recipe_id_from_name(name, id);
	if (stage_recipe_find(id, NULL))
		return STAGE_SAVE_NAME_TAKEN;
	struct stage_state st;
	if (!scene_uuid || !stage_rig_get_state(scene_uuid, &st))
		return STAGE_SAVE_NO_RIG;

	char dir[512];
	if (!stage_presets_user_dir("recipes", dir, sizeof(dir)))
		return STAGE_SAVE_IO;

	struct stage_layer_info *layers = bmalloc(sizeof(*layers) * MAX_RECIPE_LAYERS);
	size_t nlayers = 0;
	stage_rig_list_layers(scene_uuid, layers, MAX_RECIPE_LAYERS, &nlayers);
	static const double def_depth[STAGE_RROLE_COUNT] = {3.0, 1.0, 0.6};
	static const char *const role_keys[STAGE_RROLE_COUNT] = {"bg", "char", "fg"};
	double depth[STAGE_RROLE_COUNT];
	bool have[STAGE_RROLE_COUNT] = {false, false, false};
	for (size_t i = 0; i < nlayers; i++) {
		int slot = layers[i].enrolled ? recipe_role_slot(layers[i].role) : -1;
		if (slot >= 0 && !have[slot]) {
			depth[slot] = clampd(layers[i].depth, STAGE_DEPTH_MIN, STAGE_DEPTH_MAX);
			have[slot] = true;
		}
	}
	bfree(layers);

	obs_data_t *d = obs_data_create();
	obs_data_t *nm = obs_data_create();
	obs_data_t *roles = obs_data_create();
	obs_data_t *au = obs_data_create();
	obs_data_t *dof = obs_data_create();
	obs_data_set_string(d, "id", id);
	obs_data_set_string(nm, "en", name);
	obs_data_set_string(nm, "ja", name);
	obs_data_set_string(nm, "id", name);
	obs_data_set_obj(d, "name", nm);
	obs_data_set_bool(d, "pro", false);
	bool any = have[0] || have[1] || have[2];
	for (int i = 0; i < STAGE_RROLE_COUNT; i++) {
		if (have[i] || !any)
			obs_data_set_double(roles, role_keys[i], have[i] ? depth[i] : def_depth[i]);
	}
	obs_data_set_obj(d, "roles", roles);
	obs_data_set_string(au, "mode",
			    stage_auto_mode_names[st.auto_mode < STAGE_AUTO_COUNT ? st.auto_mode : 0]);
	obs_data_set_double(au, "intensity", clampd(st.auto_intensity, 0, 1));
	obs_data_set_obj(d, "auto", au);

	struct stage_fx_info fi;
	if (stage_fx_get_info(scene_uuid, &fi) && fi.preset[0] && stage_fx_preset_known(fi.preset))
		obs_data_set_string(d, "fx", fi.preset);
	obs_data_set_double(dof, "focus_depth",
			    st.focus_depth > 0 ? clampd(st.focus_depth, STAGE_DEPTH_MIN, STAGE_DEPTH_MAX)
					       : STAGE_DEPTH_CHAR);
	obs_data_set_double(dof, "amount", clampd(st.dof_amount, 0, 1));
	obs_data_set_obj(d, "dof", dof);

	/* Overscan and audio react live in the rig settings. */
	obs_source_t *scene = obs_get_source_by_uuid(scene_uuid);
	obs_source_t *rig = stage_find_filter(scene, STAGE_FILTER_CAMERA_RIG);
	if (rig) {
		obs_data_t *rs = obs_source_get_settings(rig);
		if (obs_data_has_user_value(rs, RIG_KEY_OVERSCAN_PCT))
			obs_data_set_double(d, "overscan",
					    clampd(obs_data_get_double(rs, RIG_KEY_OVERSCAN_PCT) / 100.0, 0,
						   0.25));
		if (obs_data_has_user_value(rs, RIG_KEY_AUDIO_AMOUNT)) {
			obs_data_t *ar = obs_data_create();
			obs_data_set_double(ar, "amount",
					    clampd(obs_data_get_double(rs, RIG_KEY_AUDIO_AMOUNT) / 100.0,
						   0, 1));
			long long a = obs_data_get_int(rs, RIG_KEY_AUDIO_ATTACK);
			long long rl = obs_data_get_int(rs, RIG_KEY_AUDIO_RELEASE);
			if (a >= 1 && a <= 1000)
				obs_data_set_int(ar, "attack_ms", a);
			if (rl >= 1 && rl <= 5000)
				obs_data_set_int(ar, "release_ms", rl);
			obs_data_set_obj(d, "audio_react", ar);
			obs_data_release(ar);
		}
		obs_data_release(rs);
	}
	obs_source_release(rig);
	obs_source_release(scene);

	char path[600];
	snprintf(path, sizeof(path), "%s/%s.json", dir, id);
	enum stage_save_result res = STAGE_SAVE_OK;
	if (os_mkdirs(dir) == MKDIR_ERROR || !obs_data_save_json_safe(d, path, "tmp", "bak")) {
		SLOG_WARN("could not write user recipe '%s'", path);
		res = STAGE_SAVE_IO;
	}
	obs_data_release(dof);
	obs_data_release(au);
	obs_data_release(roles);
	obs_data_release(nm);
	obs_data_release(d);

	if (res == STAGE_SAVE_OK) {
		SLOG_INFO("saved user recipe '%s'", id);
		stage_presets_reload();
		if (out_id)
			stage_strlcpy(out_id, id, STAGE_ID_MAX);
	}
	return res;
}
