/*
 * stage-vendor-requests.c — obs-websocket vendor request handlers
 * (docs/protocol/vendor-api.md section 4 and 5).
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Every handler runs on the obs-websocket thread and follows the same
 * order: strict field validation (bad_param), target resolution (not_found /
 * no_rig / no_fx), id lookups (not_found), then either a read of the core
 * snapshots / registries or one push onto the command queue answered with
 * {ok:true, id}. Nothing here mutates scene items or filter state directly;
 * saved views are the exception by design (scene private settings, through
 * the locked stage_views_* store).
 *
 * Transport limits of obs-websocket (obs_data on both sides): a response
 * cannot hold JSON null or arrays of strings (docs/protocol/vendor-api.md
 * section 1.1). The contract makes those fields optional: layer_list sends
 * "depth" only for enrolled items, and get_info "features" / recipe_list
 * "tags" are never sent.
 */
#include <math.h>
#include <obs.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <util/bmem.h>

#include "presets/stage-json.h"
#include "presets/stage-presets.h"
#include "presets/stage-target.h"
#include "stage-vendor.h"

/* Max layers reported by layer_list. */
#define MAX_LIST_LAYERS 256
/* Max bytes of one fx_set params chunk (stage_cmd.u.fx.params_json). */
#define FX_JSON_CAP STAGE_FX_PARAMS_JSON_MAX
#define CAMERA_RESET_DEFAULT_MS 600
#define PULSE_DURATION_DEFAULT_MS 300

/* ------------------------------------------------------------------------- */
/* Responses                                                                  */
/* ------------------------------------------------------------------------- */

static void reply_err(obs_data_t *res, enum stage_error code, const char *fmt, ...)
{
	char msg[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (!msg[0])
		snprintf(msg, sizeof(msg), "%s", stage_error_names[code]);
	obs_data_clear(res);
	obs_data_set_bool(res, "ok", false);
	obs_data_set_string(res, "code", stage_error_names[code]);
	obs_data_set_string(res, "error", msg);
}

static void reply_bad(obs_data_t *res, const struct sj_err *e)
{
	reply_err(res, STAGE_ERROR_BAD_PARAM, "%s", e->msg[0] ? e->msg : "invalid request");
}

static void reply_ok(obs_data_t *res)
{
	obs_data_set_bool(res, "ok", true);
}

double stage_vendor_r4(double v)
{
	if (!isfinite(v))
		return 0.0;
	double r = round(v * 10000.0) / 10000.0;
	return r == 0.0 ? 0.0 : r; /* no -0 */
}

static double clampd(double v, double lo, double hi)
{
	if (!(v == v))
		return lo;
	return v < lo ? lo : (v > hi ? hi : v);
}

static bool only(obs_data_t *req, obs_data_t *res, const char *const *keys, size_t n)
{
	struct sj_err e = {{0}};
	if (sj_only_keys(req, keys, n, &e))
		return true;
	reply_bad(res, &e);
	return false;
}
#define ONLY(...)                                                                        \
	do {                                                                             \
		static const char *const keys_[] = {__VA_ARGS__};                        \
		if (!only(req, res, keys_, sizeof(keys_) / sizeof(keys_[0])))            \
			return;                                                          \
	} while (0)

/* Field getters that answer bad_param themselves; they return false when
 * the handler must stop. *present (optional) tells whether the key was set. */
static bool get_num(obs_data_t *req, obs_data_t *res, const char *key, double lo, double hi,
		    double *out, bool *present)
{
	struct sj_err e = {{0}};
	int g = sj_num(req, key, lo, hi, out, &e);
	if (present)
		*present = g == SJ_OK;
	if (g == SJ_BAD)
		reply_bad(res, &e);
	return g != SJ_BAD;
}

static bool get_int(obs_data_t *req, obs_data_t *res, const char *key, long long lo, long long hi,
		    long long *out, bool *present)
{
	struct sj_err e = {{0}};
	int g = sj_int(req, key, lo, hi, out, &e);
	if (present)
		*present = g == SJ_OK;
	if (g == SJ_BAD)
		reply_bad(res, &e);
	return g != SJ_BAD;
}

static bool get_enum(obs_data_t *req, obs_data_t *res, const char *key, const char *const *names,
		     size_t n, int *out, bool *present)
{
	struct sj_err e = {{0}};
	int g = sj_enum(req, key, names, n, out, &e);
	if (present)
		*present = g == SJ_OK;
	if (g == SJ_BAD)
		reply_bad(res, &e);
	return g != SJ_BAD;
}

static bool get_bool(obs_data_t *req, obs_data_t *res, const char *key, bool *out, bool *present)
{
	struct sj_err e = {{0}};
	int g = sj_bool(req, key, out, &e);
	if (present)
		*present = g == SJ_OK;
	if (g == SJ_BAD)
		reply_bad(res, &e);
	return g != SJ_BAD;
}

static bool get_id(obs_data_t *req, obs_data_t *res, const char *key, char out[STAGE_ID_MAX],
		   bool required)
{
	struct sj_err e = {{0}};
	int g = sj_id(req, key, false, out, &e);
	if (g == SJ_ABSENT && required) {
		reply_err(res, STAGE_ERROR_BAD_PARAM, "'%s' is required", key);
		return false;
	}
	if (g == SJ_BAD) {
		reply_bad(res, &e);
		return false;
	}
	if (g == SJ_ABSENT)
		out[0] = '\0';
	return true;
}

/* ------------------------------------------------------------------------- */
/* Targets                                                                    */
/* ------------------------------------------------------------------------- */

enum target_kind { TARGET_RIG, TARGET_FX };

struct target {
	obs_source_t *src; /* reference, released by target_free */
	char uuid[STAGE_SCENE_UUID_MAX];
	char name[256];
	struct stage_state st; /* TARGET_RIG: the rig snapshot */
};

static void target_free(struct target *t)
{
	obs_source_release(t->src);
	t->src = NULL;
}

/* Resolves requestData.scene (name or uuid; empty = program scene). */
static bool resolve(obs_data_t *req, obs_data_t *res, enum target_kind kind, struct target *t)
{
	memset(t, 0, sizeof(*t));
	struct sj_err e = {{0}};
	const char *s = NULL;
	int g = sj_str(req, "scene", 256, &s, &e);
	if (g == SJ_BAD) {
		reply_bad(res, &e);
		return false;
	}
	enum stage_error none = kind == TARGET_FX ? STAGE_ERROR_NO_FX : STAGE_ERROR_NO_RIG;
	if (g == SJ_ABSENT || !s[0]) {
		t->src = stage_program_scene_source();
		if (!t->src) {
			reply_err(res, none, "no current program scene");
			return false;
		}
	} else {
		t->src = stage_source_resolve(s);
		if (!t->src) {
			reply_err(res, STAGE_ERROR_NOT_FOUND, "unknown scene '%.64s'", s);
			return false;
		}
	}
	stage_strlcpy(t->uuid, obs_source_get_uuid(t->src), sizeof(t->uuid));
	stage_strlcpy(t->name, obs_source_get_name(t->src), sizeof(t->name));

	if (kind == TARGET_RIG) {
		if (obs_source_get_type(t->src) != OBS_SOURCE_TYPE_SCENE ||
		    !stage_rig_get_state(t->uuid, &t->st) || !t->st.enabled) {
			reply_err(res, STAGE_ERROR_NO_RIG, "'%.64s' has no enabled Parallaxa Camera Rig", t->name);
			target_free(t);
			return false;
		}
	} else if (!stage_fx_exists(t->uuid)) {
		reply_err(res, STAGE_ERROR_NO_FX, "'%.64s' has no Parallaxa FX", t->name);
		target_free(t);
		return false;
	}
	return true;
}

static struct stage_cmd new_cmd(enum stage_cmd_type type, const struct target *t)
{
	struct stage_cmd c;
	memset(&c, 0, sizeof(c));
	c.type = type;
	stage_strlcpy(c.scene_uuid, t->uuid, sizeof(c.scene_uuid));
	c.ease = STAGE_EASE_DEFAULT;
	c.intensity = -1.0f;
	return c;
}

/* try_push (never drops other commands) and answer {ok, id} or busy.
 * Returns the id, 0 when busy. */
static uint64_t push_reply(obs_data_t *res, const struct stage_cmd *c)
{
	uint64_t id = 0;
	if (!stage_cmdq_push_id(c, false, &id)) {
		reply_err(res, STAGE_ERROR_BUSY, "command queue is full");
		return 0;
	}
	reply_ok(res);
	obs_data_set_int(res, "id", (long long)id);
	return id;
}

/* ------------------------------------------------------------------------- */
/* Shared state body (get_state, state_changed)                               */
/* ------------------------------------------------------------------------- */

void stage_vendor_fill_state(obs_data_t *d, const char *scene_name, const char *scene_uuid,
			     const struct stage_state *st, const char *fx_preset)
{
	obs_data_t *cam = obs_data_create();
	obs_data_t *au = obs_data_create();
	obs_data_t *fx = obs_data_create();
	obs_data_t *dof = obs_data_create();

	obs_data_set_string(d, "scene", scene_name ? scene_name : "");
	obs_data_set_string(d, "scene_uuid", scene_uuid ? scene_uuid : "");

	obs_data_set_double(cam, "x", stage_vendor_r4(st->x));
	obs_data_set_double(cam, "y", stage_vendor_r4(st->y));
	obs_data_set_double(cam, "zoom", stage_vendor_r4(st->zoom > 0 ? st->zoom : 1.0));
	obs_data_set_double(cam, "rot", stage_vendor_r4(st->rot));
	obs_data_set_double(cam, "shake", stage_vendor_r4(clampd(st->shake, 0, 1)));
	obs_data_set_obj(d, "camera", cam);

	obs_data_set_string(au, "mode",
			    stage_auto_mode_names[st->auto_mode < STAGE_AUTO_COUNT ? st->auto_mode : 0]);
	obs_data_set_double(au, "intensity", stage_vendor_r4(clampd(st->auto_intensity, 0, 1)));
	obs_data_set_obj(d, "auto", au);

	const char *preset = fx_preset ? fx_preset : st->fx_preset;
	obs_data_set_string(fx, "preset", stage_id_valid(preset) ? preset : "");
	obs_data_set_obj(d, "fx", fx);

	double focus = st->focus_depth > 0 ? st->focus_depth : STAGE_DEPTH_CHAR;
	obs_data_set_double(dof, "focus_depth",
			    stage_vendor_r4(clampd(focus, STAGE_DEPTH_MIN, STAGE_DEPTH_MAX)));
	obs_data_set_double(dof, "amount", stage_vendor_r4(clampd(st->dof_amount, 0, 1)));
	obs_data_set_obj(d, "dof", dof);

	obs_data_set_bool(d, "moving", st->moving);
	if (st->moving && stage_id_valid(st->move))
		obs_data_set_string(d, "move", st->move);
	obs_data_set_bool(d, "frozen", st->frozen);
	obs_data_set_int(d, "view", st->view <= STAGE_VIEW_SLOT_MAX ? st->view : 0);

	obs_data_release(dof);
	obs_data_release(fx);
	obs_data_release(au);
	obs_data_release(cam);
}

/* FX preset reported for a scene: its Parallaxa FX filter if any. */
static const char *scene_fx_preset(const char *uuid, struct stage_fx_info *fi)
{
	return stage_fx_get_info(uuid, fi) ? fi->preset : NULL;
}

/* ------------------------------------------------------------------------- */
/* get_info                                                                   */
/* ------------------------------------------------------------------------- */

static bool info_rig_cb(const char *uuid, void *param)
{
	obs_data_array_t *arr = param;
	struct stage_state st;
	if (!stage_rig_get_state(uuid, &st))
		return true;
	obs_data_t *o = obs_data_create();
	char name[256];
	stage_source_name_by_uuid(uuid, name, sizeof(name));
	obs_data_set_string(o, "scene", name);
	obs_data_set_string(o, "scene_uuid", uuid);
	obs_data_set_bool(o, "enabled", st.enabled);
	obs_data_array_push_back(arr, o);
	obs_data_release(o);
	return true;
}

static bool info_fx_cb(const char *uuid, void *param)
{
	obs_data_array_t *arr = param;
	struct stage_fx_info fi;
	if (!stage_fx_get_info(uuid, &fi))
		return true;
	obs_data_t *o = obs_data_create();
	char name[256];
	stage_source_name_by_uuid(uuid, name, sizeof(name));
	obs_data_set_string(o, "source", name);
	obs_data_set_string(o, "source_uuid", uuid);
	obs_data_set_bool(o, "enabled", fi.enabled);
	obs_data_array_push_back(arr, o);
	obs_data_release(o);
	return true;
}

static void h_get_info(obs_data_t *req, obs_data_t *res)
{
	ONLY("");
	reply_ok(res);
	obs_data_set_string(res, "version", STAGE_VERSION);
	obs_data_set_int(res, "protocol", STAGE_PROTOCOL_VERSION);
	obs_data_set_string(res, "obs", obs_get_version_string());

	struct obs_video_info ovi;
	long long w = 1920, h = 1080; /* only without a video context (headless) */
	if (obs_get_video_info(&ovi) && ovi.base_width && ovi.base_height) {
		w = ovi.base_width;
		h = ovi.base_height;
	}
	obs_data_t *canvas = obs_data_create();
	obs_data_set_int(canvas, "width", w);
	obs_data_set_int(canvas, "height", h);
	obs_data_set_obj(res, "canvas", canvas);
	obs_data_release(canvas);

	/* No "features": optional, cannot cross obs_data (vendor-api.md 1.1). */

	obs_data_array_t *rigs = obs_data_array_create();
	stage_rig_for_each(info_rig_cb, rigs);
	obs_data_set_array(res, "rigs", rigs);
	obs_data_array_release(rigs);

	obs_data_array_t *fx = obs_data_array_create();
	stage_fx_for_each(info_fx_cb, fx);
	obs_data_set_array(res, "fx", fx);
	obs_data_array_release(fx);
}

/* ------------------------------------------------------------------------- */
/* get_state                                                                  */
/* ------------------------------------------------------------------------- */

static void h_get_state(obs_data_t *req, obs_data_t *res)
{
	ONLY("scene");
	struct target t;
	if (!resolve(req, res, TARGET_RIG, &t))
		return;
	struct stage_fx_info fi;
	reply_ok(res);
	stage_vendor_fill_state(res, t.name, t.uuid, &t.st, scene_fx_preset(t.uuid, &fi));
	target_free(&t);
}

/* ------------------------------------------------------------------------- */
/* Camera                                                                     */
/* ------------------------------------------------------------------------- */

static void h_camera_move(obs_data_t *req, obs_data_t *res)
{
	ONLY("scene", "move", "duration_ms", "intensity", "ease");
	char move[STAGE_ID_MAX];
	long long dur = 0;
	double inten = 1.0;
	int ease = -1;
	bool has_ease = false;
	if (!get_id(req, res, "move", move, true) ||
	    !get_int(req, res, "duration_ms", STAGE_MOVE_DURATION_MIN, STAGE_MOVE_DURATION_MAX, &dur,
		     NULL) ||
	    !get_num(req, res, "intensity", 0.0, STAGE_MOVE_INTENSITY_MAX, &inten, NULL) ||
	    !get_enum(req, res, "ease", stage_ease_names, STAGE_EASE_COUNT, &ease, &has_ease))
		return;
	struct target t;
	if (!resolve(req, res, TARGET_RIG, &t))
		return;
	if (!stage_move_known(move)) {
		reply_err(res, STAGE_ERROR_NOT_FOUND, "unknown move '%s'", move);
		target_free(&t);
		return;
	}
	struct stage_cmd c = new_cmd(STAGE_CMD_CAMERA_MOVE, &t);
	stage_strlcpy(c.u.move.move, move, sizeof(c.u.move.move));
	c.duration_ms = (uint32_t)dur; /* 0 = the move's own duration */
	c.intensity = (float)inten;
	c.ease = has_ease ? (uint8_t)ease : (uint8_t)STAGE_CMD_EASE_NONE;
	uint64_t id = push_reply(res, &c);
	if (id)
		stage_vendor_note_move(t.uuid, id, move, t.st.frozen);
	target_free(&t);
}

static void h_camera_set(obs_data_t *req, obs_data_t *res)
{
	ONLY("scene", "x", "y", "zoom", "rot", "relative", "duration_ms", "ease");
	bool relative = false;
	if (!get_bool(req, res, "relative", &relative, NULL))
		return;
	double x = 0, y = 0, zoom = 1, rot = 0;
	bool hx, hy, hz, hr;
	long long dur = 0;
	int ease = STAGE_EASE_DEFAULT;
	if (!get_num(req, res, "x", -100000.0, 100000.0, &x, &hx) ||
	    !get_num(req, res, "y", -100000.0, 100000.0, &y, &hy) ||
	    !get_num(req, res, "zoom", relative ? -3.5 : STAGE_ZOOM_MIN, STAGE_ZOOM_MAX, &zoom, &hz) ||
	    !get_num(req, res, "rot", relative ? -90.0 : STAGE_ROT_MIN, relative ? 90.0 : STAGE_ROT_MAX,
		     &rot, &hr) ||
	    !get_int(req, res, "duration_ms", 0, STAGE_SET_DURATION_MAX, &dur, NULL) ||
	    !get_enum(req, res, "ease", stage_ease_names, STAGE_EASE_COUNT, &ease, NULL))
		return;
	struct target t;
	if (!resolve(req, res, TARGET_RIG, &t))
		return;
	struct stage_cmd c = new_cmd(STAGE_CMD_CAMERA_SET, &t);
	c.u.set.x = (float)x;
	c.u.set.y = (float)y;
	c.u.set.zoom = (float)zoom;
	c.u.set.rot = (float)rot;
	c.u.set.mask = (uint8_t)((hx ? STAGE_CAM_X : 0) | (hy ? STAGE_CAM_Y : 0) |
				 (hz ? STAGE_CAM_ZOOM : 0) | (hr ? STAGE_CAM_ROT : 0));
	c.u.set.relative = relative;
	c.duration_ms = (uint32_t)dur;
	c.ease = (uint8_t)ease;
	push_reply(res, &c);
	target_free(&t);
}

static void h_camera_reset(obs_data_t *req, obs_data_t *res)
{
	ONLY("scene", "duration_ms");
	long long dur = CAMERA_RESET_DEFAULT_MS;
	if (!get_int(req, res, "duration_ms", 0, STAGE_SET_DURATION_MAX, &dur, NULL))
		return;
	struct target t;
	if (!resolve(req, res, TARGET_RIG, &t))
		return;
	struct stage_cmd c = new_cmd(STAGE_CMD_CAMERA_RESET, &t);
	c.duration_ms = (uint32_t)dur;
	if (push_reply(res, &c))
		stage_vendor_note_cancel(t.uuid);
	target_free(&t);
}

static void h_camera_auto(obs_data_t *req, obs_data_t *res)
{
	ONLY("scene", "mode", "intensity");
	int mode = 0;
	bool has_mode = false;
	double inten = -1.0;
	if (!get_enum(req, res, "mode", stage_auto_mode_names, STAGE_AUTO_COUNT, &mode, &has_mode) ||
	    !get_num(req, res, "intensity", 0.0, 1.0, &inten, NULL))
		return;
	if (!has_mode) {
		reply_err(res, STAGE_ERROR_BAD_PARAM, "'mode' is required");
		return;
	}
	struct target t;
	if (!resolve(req, res, TARGET_RIG, &t))
		return;
	struct stage_cmd c = new_cmd(STAGE_CMD_CAMERA_AUTO, &t);
	c.u.autom.mode = (uint8_t)mode;
	c.intensity = (float)inten; /* < 0 keeps the current intensity */
	push_reply(res, &c);
	target_free(&t);
}

/* ------------------------------------------------------------------------- */
/* FX                                                                         */
/* ------------------------------------------------------------------------- */

/* Validates fx_set.params and serializes it into compact JSON chunks of at
 * most FX_JSON_CAP bytes (normally one). Returns the chunk count, -1 after
 * answering an error. */
static int fx_params_chunks(obs_data_t *params, obs_data_t *res, char chunks[][FX_JSON_CAP],
			    int max_chunks)
{
	struct sj_err e = {{0}};
	for (obs_data_item_t *it = obs_data_first(params); it; obs_data_item_next(&it)) {
		if (!stage_fx_param_find(obs_data_item_get_name(it))) {
			reply_err(res, STAGE_ERROR_BAD_PARAM, "unknown param '%.64s'",
				  obs_data_item_get_name(it));
			obs_data_item_release(&it);
			return -1;
		}
	}
	int n = 0;
	chunks[0][0] = '\0';
	for (size_t i = 0; i < stage_fx_param_count; i++) {
		const struct stage_fx_param_def *pd = &stage_fx_params[i];
		bool ok;
		for (int attempt = 0; attempt < 2; attempt++) {
			char *buf = chunks[n];
			if (pd->is_id) {
				char id[STAGE_ID_MAX];
				int g = sj_id(params, pd->name, true, id, &e);
				if (g == SJ_BAD) {
					reply_bad(res, &e);
					return -1;
				}
				if (g == SJ_ABSENT)
					break;
				bool lut = strcmp(pd->name, "lut") == 0;
				if (id[0] && !(lut ? stage_lut_known(id) : stage_leak_known(id))) {
					reply_err(res, STAGE_ERROR_NOT_FOUND, "unknown %s '%s'",
						  lut ? "LUT" : "light leak", id);
					return -1;
				}
				ok = sj_json_add_str(buf, FX_JSON_CAP, pd->name, id);
			} else {
				double v;
				int g = sj_num(params, pd->name, pd->lo, pd->hi, &v, &e);
				if (g == SJ_BAD) {
					reply_bad(res, &e);
					return -1;
				}
				if (g == SJ_ABSENT)
					break;
				ok = sj_json_add_num(buf, FX_JSON_CAP, pd->name, v);
			}
			if (ok)
				break;
			/* Chunk full: close it and continue in the next one. */
			if (attempt == 1 || n + 1 >= max_chunks) {
				reply_err(res, STAGE_ERROR_INTERNAL, "params too large");
				return -1;
			}
			sj_json_close(chunks[n], FX_JSON_CAP);
			chunks[++n][0] = '\0';
		}
	}
	sj_json_close(chunks[n], FX_JSON_CAP);
	return n + 1;
}

static void h_fx_set(obs_data_t *req, obs_data_t *res)
{
	ONLY("scene", "preset", "params", "duration_ms");
	char preset[STAGE_ID_MAX];
	long long dur = 0;
	obs_data_t *params = NULL;
	struct sj_err e = {{0}};
	if (!get_id(req, res, "preset", preset, false) ||
	    !get_int(req, res, "duration_ms", 0, STAGE_SET_DURATION_MAX, &dur, NULL))
		return;
	int gp = sj_obj(req, "params", &params, &e);
	if (gp == SJ_BAD) {
		reply_bad(res, &e);
		return;
	}
	if (!preset[0] && gp == SJ_ABSENT) {
		reply_err(res, STAGE_ERROR_BAD_PARAM, "'preset' or 'params' is required");
		return;
	}
	char chunks[4][FX_JSON_CAP];
	int nchunks = 0;
	struct target t = {0};
	if (params) {
		nchunks = fx_params_chunks(params, res, chunks, 4);
		obs_data_release(params);
		if (nchunks < 0)
			return;
	}
	if (!resolve(req, res, TARGET_FX, &t))
		return;
	if (preset[0] && !stage_fx_preset_known(preset)) {
		reply_err(res, STAGE_ERROR_NOT_FOUND, "unknown FX preset '%s'", preset);
		target_free(&t);
		return;
	}
	/* One command per params chunk (normally one), all with the same id;
	 * the preset rides on the first so params apply on top of it. */
	int ncmd = nchunks ? nchunks : 1;
	struct stage_cmdq_stats qs;
	stage_cmdq_get_stats(&qs);
	if (qs.depth + (size_t)ncmd > STAGE_CMDQ_CAPACITY) {
		reply_err(res, STAGE_ERROR_BUSY, "command queue is full");
		target_free(&t);
		return;
	}
	uint64_t id = stage_cmdq_next_id();
	for (int i = 0; i < ncmd; i++) {
		struct stage_cmd c = new_cmd(STAGE_CMD_FX_SET, &t);
		c.id = id;
		c.duration_ms = (uint32_t)dur;
		if (i == 0)
			stage_strlcpy(c.u.fx.preset, preset, sizeof(c.u.fx.preset));
		if (nchunks)
			stage_strlcpy(c.u.fx.params_json, chunks[i], sizeof(c.u.fx.params_json));
		if (!stage_cmdq_try_push(&c)) {
			reply_err(res, STAGE_ERROR_BUSY, "command queue is full");
			target_free(&t);
			return;
		}
	}
	reply_ok(res);
	obs_data_set_int(res, "id", (long long)id);
	target_free(&t);
}

static void h_fx_pulse(obs_data_t *req, obs_data_t *res)
{
	ONLY("scene", "effect", "intensity", "duration_ms");
	int effect = 0;
	bool has_effect = false;
	double inten = STAGE_PULSE_INTENSITY_DEFAULT;
	long long dur = PULSE_DURATION_DEFAULT_MS;
	if (!get_enum(req, res, "effect", stage_pulse_names, STAGE_PULSE_COUNT, &effect,
		      &has_effect) ||
	    !get_num(req, res, "intensity", 0.0, 1.0, &inten, NULL) ||
	    !get_int(req, res, "duration_ms", STAGE_PULSE_DURATION_MIN, STAGE_PULSE_DURATION_MAX, &dur,
		     NULL))
		return;
	if (!has_effect) {
		reply_err(res, STAGE_ERROR_BAD_PARAM, "'effect' is required");
		return;
	}
	struct target t;
	if (!resolve(req, res, TARGET_FX, &t))
		return;
	struct stage_cmd c = new_cmd(STAGE_CMD_FX_PULSE, &t);
	stage_strlcpy(c.u.pulse.effect, stage_pulse_names[effect], sizeof(c.u.pulse.effect));
	c.intensity = (float)inten;
	c.duration_ms = (uint32_t)dur;
	push_reply(res, &c);
	target_free(&t);
}

static void h_dof_set(obs_data_t *req, obs_data_t *res)
{
	ONLY("scene", "focus_depth", "amount", "duration_ms", "ease");
	double focus = STAGE_DEPTH_CHAR, amount = 0;
	bool hf, ha;
	long long dur = 0;
	int ease = STAGE_EASE_DEFAULT;
	if (!get_num(req, res, "focus_depth", STAGE_DEPTH_MIN, STAGE_DEPTH_MAX, &focus, &hf) ||
	    !get_num(req, res, "amount", STAGE_DOF_AMOUNT_MIN, STAGE_DOF_AMOUNT_MAX, &amount, &ha) ||
	    !get_int(req, res, "duration_ms", 0, STAGE_SET_DURATION_MAX, &dur, NULL) ||
	    !get_enum(req, res, "ease", stage_ease_names, STAGE_EASE_COUNT, &ease, NULL))
		return;
	if (!hf && !ha) {
		reply_err(res, STAGE_ERROR_BAD_PARAM, "'focus_depth' or 'amount' is required");
		return;
	}
	struct target t;
	if (!resolve(req, res, TARGET_RIG, &t))
		return;
	struct stage_cmd c = new_cmd(STAGE_CMD_DOF_SET, &t);
	c.u.dof.focus_depth = (float)focus;
	c.u.dof.amount = (float)amount;
	c.u.dof.mask = (uint8_t)((hf ? STAGE_DOF_FOCUS : 0) | (ha ? STAGE_DOF_AMOUNT : 0));
	c.duration_ms = (uint32_t)dur;
	c.ease = (uint8_t)ease;
	push_reply(res, &c);
	target_free(&t);
}

/* ------------------------------------------------------------------------- */
/* Layers                                                                     */
/* ------------------------------------------------------------------------- */

static void h_layer_list(obs_data_t *req, obs_data_t *res)
{
	ONLY("scene");
	struct target t;
	if (!resolve(req, res, TARGET_RIG, &t))
		return;
	struct stage_layer_info *layers = bmalloc(sizeof(*layers) * MAX_LIST_LAYERS);
	size_t n = 0;
	stage_rig_list_layers(t.uuid, layers, MAX_LIST_LAYERS, &n);
	obs_data_array_t *arr = obs_data_array_create();
	for (size_t i = 0; i < n; i++) {
		const struct stage_layer_info *l = &layers[i];
		obs_data_t *o = obs_data_create();
		obs_data_set_int(o, "item_id", l->item_id);
		obs_data_set_string(o, "name", l->name);
		obs_data_set_string(o, "source_uuid", l->source_uuid);
		bool enrolled = l->enrolled && l->role != STAGE_ROLE_NONE;
		if (enrolled) /* absent = not enrolled (vendor-api.md 1.1) */
			obs_data_set_double(o, "depth",
					    stage_vendor_r4(clampd(l->depth, STAGE_DEPTH_MIN,
								   STAGE_DEPTH_MAX)));
		obs_data_set_string(o, "role",
				    stage_role_names[enrolled && l->role < STAGE_ROLE_COUNT ? l->role
											    : 0]);
		obs_data_set_bool(o, "visible", l->visible);
		obs_data_set_bool(o, "locked", l->locked);
		obs_data_set_bool(o, "depth_blur", l->depth_blur);
		obs_data_array_push_back(arr, o);
		obs_data_release(o);
	}
	bfree(layers);
	reply_ok(res);
	obs_data_set_string(res, "scene", t.name);
	obs_data_set_string(res, "scene_uuid", t.uuid);
	obs_data_set_array(res, "layers", arr);
	obs_data_array_release(arr);
	target_free(&t);
}

static void h_layer_set(obs_data_t *req, obs_data_t *res)
{
	ONLY("scene", "item_id", "depth", "role");
	long long item_id = 0;
	bool has_item = false, has_depth = false, has_role = false;
	double depth = 0;
	int role = 0;
	bool depth_null = sj_is_null(req, "depth");
	if (!get_int(req, res, "item_id", 0, INT64_MAX, &item_id, &has_item) ||
	    (!depth_null &&
	     !get_num(req, res, "depth", STAGE_DEPTH_MIN, STAGE_DEPTH_MAX, &depth, &has_depth)) ||
	    !get_enum(req, res, "role", stage_role_names, STAGE_ROLE_COUNT, &role, &has_role))
		return;
	if (!has_item) {
		reply_err(res, STAGE_ERROR_BAD_PARAM, "'item_id' is required");
		return;
	}
	if (!has_depth && !depth_null && !has_role) {
		reply_err(res, STAGE_ERROR_BAD_PARAM, "'depth' or 'role' is required");
		return;
	}
	if (depth_null && has_role && role != STAGE_ROLE_NONE) {
		reply_err(res, STAGE_ERROR_BAD_PARAM, "depth null un-enrols; role must be absent or none");
		return;
	}
	struct target t;
	if (!resolve(req, res, TARGET_RIG, &t))
		return;
	obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(obs_scene_from_source(t.src), item_id);
	if (!item) {
		reply_err(res, STAGE_ERROR_NOT_FOUND, "unknown item_id %lld", item_id);
		target_free(&t);
		return;
	}
	struct stage_cmd c = new_cmd(STAGE_CMD_LAYER_SET, &t);
	c.u.layer.item_id = item_id;
	if (depth_null || (has_role && role == STAGE_ROLE_NONE)) {
		c.u.layer.mask = STAGE_LAYER_UNENROL;
		c.u.layer.role = STAGE_ROLE_NONE;
	} else {
		if (has_depth) {
			c.u.layer.mask |= STAGE_LAYER_DEPTH;
			c.u.layer.depth = (float)depth;
		}
		if (has_role) {
			c.u.layer.mask |= STAGE_LAYER_ROLE;
			c.u.layer.role = (uint8_t)role;
		}
	}
	push_reply(res, &c);
	target_free(&t);
}

/* ------------------------------------------------------------------------- */
/* Catalog lists and recipes                                                  */
/* ------------------------------------------------------------------------- */

static void set_loc_name(obs_data_t *o, const struct stage_loc_name *n)
{
	obs_data_t *nm = obs_data_create();
	obs_data_set_string(nm, "en", n->en);
	obs_data_set_string(nm, "ja", n->ja);
	obs_data_set_string(nm, "id", n->id);
	obs_data_set_obj(o, "name", nm);
	obs_data_release(nm);
}

static void h_move_list(obs_data_t *req, obs_data_t *res)
{
	ONLY("");
	obs_data_array_t *arr = obs_data_array_create();
	struct stage_move_info m;
	for (size_t i = 0; stage_move_info_at(i, &m); i++) {
		obs_data_t *o = obs_data_create();
		obs_data_set_string(o, "id", m.id);
		set_loc_name(o, &m.name);
		obs_data_set_int(o, "duration_ms", m.duration_ms);
		obs_data_set_bool(o, "builtin", m.builtin);
		obs_data_array_push_back(arr, o);
		obs_data_release(o);
	}
	reply_ok(res);
	obs_data_set_array(res, "moves", arr);
	obs_data_array_release(arr);
}

static void h_recipe_list(obs_data_t *req, obs_data_t *res)
{
	ONLY("");
	obs_data_array_t *arr = obs_data_array_create();
	struct stage_recipe *r = bmalloc(sizeof(*r));
	for (size_t i = 0; stage_recipe_at(i, r); i++) {
		obs_data_t *o = obs_data_create();
		obs_data_set_string(o, "id", r->id);
		set_loc_name(o, &r->name);
		obs_data_set_bool(o, "pro", r->pro);
		/* No "tags": optional, cannot cross obs_data (vendor-api.md 1.1). */
		obs_data_set_bool(o, "user", r->user);
		obs_data_array_push_back(arr, o);
		obs_data_release(o);
	}
	bfree(r);
	reply_ok(res);
	obs_data_set_array(res, "recipes", arr);
	obs_data_array_release(arr);
}

static void h_recipe_apply(obs_data_t *req, obs_data_t *res)
{
	ONLY("scene", "recipe", "duration_ms");
	char recipe[STAGE_ID_MAX];
	long long dur = STAGE_RECIPE_DURATION_DEFAULT;
	if (!get_id(req, res, "recipe", recipe, true) ||
	    !get_int(req, res, "duration_ms", 0, STAGE_SET_DURATION_MAX, &dur, NULL))
		return;
	struct target t;
	if (!resolve(req, res, TARGET_RIG, &t))
		return;
	struct stage_recipe_result rr;
	char err[160] = "";
	int rc = stage_recipe_apply_id(t.uuid, recipe, (uint32_t)dur, &rr, err, sizeof(err));
	if (rc >= 0) {
		reply_err(res, (enum stage_error)rc, "%s", err);
	} else {
		reply_ok(res);
		obs_data_set_int(res, "id", (long long)rr.id);
		obs_data_t *ap = obs_data_create();
		obs_data_set_int(ap, "layers", rr.layers);
		obs_data_set_string(ap, "auto", stage_auto_mode_names[rr.auto_mode]);
		obs_data_set_string(ap, "fx", rr.fx);
		obs_data_set_obj(res, "applied", ap);
		obs_data_release(ap);
	}
	target_free(&t);
}

/* ------------------------------------------------------------------------- */
/* Views, freeze, panic                                                       */
/* ------------------------------------------------------------------------- */

static void view_to_obj(obs_data_t *o, const struct stage_view *v)
{
	obs_data_t *cam = obs_data_create();
	obs_data_set_int(o, "slot", v->slot);
	obs_data_set_string(o, "name", v->name);
	obs_data_set_double(cam, "x", stage_vendor_r4(v->x));
	obs_data_set_double(cam, "y", stage_vendor_r4(v->y));
	obs_data_set_double(cam, "zoom", stage_vendor_r4(v->zoom));
	obs_data_set_double(cam, "rot", stage_vendor_r4(v->rot));
	obs_data_set_obj(o, "camera", cam);
	obs_data_set_string(o, "transition", stage_transition_names[v->transition]);
	obs_data_set_int(o, "duration_ms", v->duration_ms);
	obs_data_release(cam);
}

static void h_view_save(obs_data_t *req, obs_data_t *res)
{
	ONLY("scene", "slot", "name", "transition", "duration_ms", "keep_pose");
	long long slot = 0, dur = STAGE_VIEW_DURATION_DEFAULT;
	bool has_slot = false, has_tr = false, has_dur = false, keep = false;
	int tr = STAGE_TRANSITION_SMOOTH;
	const char *name = NULL;
	struct sj_err e = {{0}};
	if (!get_int(req, res, "slot", STAGE_VIEW_SLOT_MIN, STAGE_VIEW_SLOT_MAX, &slot, &has_slot) ||
	    !get_enum(req, res, "transition", stage_transition_names, STAGE_TRANSITION_COUNT, &tr,
		      &has_tr) ||
	    !get_int(req, res, "duration_ms", 0, STAGE_SET_DURATION_MAX, &dur, &has_dur) ||
	    !get_bool(req, res, "keep_pose", &keep, NULL))
		return;
	if (!has_slot) {
		reply_err(res, STAGE_ERROR_BAD_PARAM, "'slot' is required");
		return;
	}
	int gn = sj_str(req, "name", STAGE_VIEW_NAME_MAX - 1, &name, &e);
	if (gn == SJ_BAD || (gn == SJ_OK && !stage_view_name_valid(name))) {
		reply_err(res, STAGE_ERROR_BAD_PARAM, "'name' must be valid UTF-8, at most %d characters",
			  STAGE_VIEW_NAME_MAX_CHARS);
		return;
	}
	struct target t;
	if (!resolve(req, res, TARGET_RIG, &t))
		return;

	struct stage_view v;
	memset(&v, 0, sizeof(v));
	uint32_t gen = 0;
	if (keep) {
		/* Edit in place (vendor-api.md 5.1): the stored pose stays, and so
		 * does every field the request leaves out. One locked
		 * read-merge-write, so a Save view from the panel cannot land in
		 * between. */
		if (!stage_views_edit(t.src, (uint8_t)slot, gn == SJ_OK ? name : NULL,
				      has_tr ? tr : -1, has_dur ? dur : -1, &v, &gen)) {
			reply_err(res, STAGE_ERROR_NOT_FOUND, "view %lld is empty", slot);
			target_free(&t);
			return;
		}
	} else {
		v.slot = (uint8_t)slot;
		if (gn == SJ_OK)
			stage_strlcpy(v.name, name, sizeof(v.name));
		else
			snprintf(v.name, sizeof(v.name), "View %lld", slot);
		/* The camera target from the snapshot, not the pose seen
		 * mid-transition (vendor-api.md 5.1): what is stored is what the
		 * reply shows. */
		v.x = (float)stage_vendor_r4(t.st.tx);
		v.y = (float)stage_vendor_r4(t.st.ty);
		v.zoom = (float)stage_vendor_r4(clampd(t.st.tzoom > 0 ? t.st.tzoom : 1.0,
						       STAGE_ZOOM_MIN, STAGE_ZOOM_MAX));
		v.rot = (float)stage_vendor_r4(clampd(t.st.trot, STAGE_ROT_MIN, STAGE_ROT_MAX));
		v.transition = (uint8_t)tr;
		v.duration_ms = (uint32_t)dur;
		if (!stage_views_put(t.src, &v, &gen)) {
			reply_err(res, STAGE_ERROR_INTERNAL, "could not store the view");
			target_free(&t);
			return;
		}
	}
	/* Tell the rig (panel refresh; its UI task stores the same pose again
	 * unless a newer write took the slot since: gen), so a full queue does
	 * not matter: the slot is already stored. */
	struct stage_cmd c = new_cmd(STAGE_CMD_VIEW_SAVE, &t);
	c.u.view.slot = v.slot;
	c.u.view.gen = gen;
	c.u.view.transition = v.transition;
	c.u.view.has_transition = true;
	c.u.view.keep_pose = keep;
	stage_strlcpy(c.u.view.name, v.name, sizeof(c.u.view.name));
	c.u.view.cam.x = v.x;
	c.u.view.cam.y = v.y;
	c.u.view.cam.zoom = v.zoom;
	c.u.view.cam.rot = v.rot;
	c.u.view.cam.mask = STAGE_CAM_X | STAGE_CAM_Y | STAGE_CAM_ZOOM | STAGE_CAM_ROT;
	c.duration_ms = v.duration_ms;
	stage_cmdq_try_push(&c);
	reply_ok(res);
	obs_data_t *vo = obs_data_create();
	view_to_obj(vo, &v);
	obs_data_set_obj(res, "view", vo);
	obs_data_release(vo);
	target_free(&t);
}

static void h_view_apply(obs_data_t *req, obs_data_t *res)
{
	ONLY("scene", "slot", "transition", "duration_ms");
	long long slot = 0, dur = 0;
	bool has_slot = false, has_tr = false, has_dur = false;
	int tr = 0;
	if (!get_int(req, res, "slot", STAGE_VIEW_SLOT_MIN, STAGE_VIEW_SLOT_MAX, &slot, &has_slot) ||
	    !get_enum(req, res, "transition", stage_transition_names, STAGE_TRANSITION_COUNT, &tr,
		      &has_tr) ||
	    !get_int(req, res, "duration_ms", 0, STAGE_SET_DURATION_MAX, &dur, &has_dur))
		return;
	if (!has_slot) {
		reply_err(res, STAGE_ERROR_BAD_PARAM, "'slot' is required");
		return;
	}
	struct target t;
	if (!resolve(req, res, TARGET_RIG, &t))
		return;
	struct stage_view v;
	if (!stage_views_get(t.src, (uint8_t)slot, &v)) {
		reply_err(res, STAGE_ERROR_NOT_FOUND, "view %lld is empty", slot);
		target_free(&t);
		return;
	}
	struct stage_cmd c;
	stage_view_make_apply_cmd(t.uuid, &v, has_tr ? tr : -1, has_dur ? dur : -1, &c);
	if (push_reply(res, &c))
		stage_vendor_note_cancel(t.uuid);
	target_free(&t);
}

static void h_view_list(obs_data_t *req, obs_data_t *res)
{
	ONLY("scene");
	struct target t;
	if (!resolve(req, res, TARGET_RIG, &t))
		return;
	struct stage_view views[STAGE_VIEW_SLOT_MAX];
	size_t n = stage_views_list(t.src, views);
	obs_data_array_t *arr = obs_data_array_create();
	for (size_t i = 0; i < n; i++) {
		obs_data_t *o = obs_data_create();
		view_to_obj(o, &views[i]);
		obs_data_array_push_back(arr, o);
		obs_data_release(o);
	}
	reply_ok(res);
	obs_data_set_string(res, "scene", t.name);
	obs_data_set_string(res, "scene_uuid", t.uuid);
	obs_data_set_array(res, "views", arr);
	obs_data_array_release(arr);
	target_free(&t);
}

static void h_view_delete(obs_data_t *req, obs_data_t *res)
{
	ONLY("scene", "slot");
	long long slot = 0;
	bool has_slot = false;
	if (!get_int(req, res, "slot", STAGE_VIEW_SLOT_MIN, STAGE_VIEW_SLOT_MAX, &slot, &has_slot))
		return;
	if (!has_slot) {
		reply_err(res, STAGE_ERROR_BAD_PARAM, "'slot' is required");
		return;
	}
	struct target t;
	if (!resolve(req, res, TARGET_RIG, &t))
		return;
	uint32_t gen = 0;
	if (!stage_views_delete(t.src, (uint8_t)slot, &gen)) {
		reply_err(res, STAGE_ERROR_NOT_FOUND, "view %lld is empty", slot);
		target_free(&t);
		return;
	}
	struct stage_cmd c = new_cmd(STAGE_CMD_VIEW_DELETE, &t);
	c.u.view.slot = (uint8_t)slot;
	c.u.view.gen = gen;
	stage_cmdq_try_push(&c); /* notification only */
	reply_ok(res);
	target_free(&t);
}

static void h_freeze(obs_data_t *req, obs_data_t *res)
{
	ONLY("scene", "on");
	bool on = false, has_on = false;
	if (!get_bool(req, res, "on", &on, &has_on))
		return;
	struct target t;
	if (!resolve(req, res, TARGET_RIG, &t))
		return;
	/* Resolve a toggle here so the answer states the resulting value. */
	bool frozen = has_on ? on : !t.st.frozen;
	struct stage_cmd c = new_cmd(STAGE_CMD_FREEZE, &t);
	c.u.freeze.mode = (uint8_t)(frozen ? STAGE_FREEZE_ON : STAGE_FREEZE_OFF);
	if (stage_cmdq_try_push(&c)) {
		reply_ok(res);
		obs_data_set_bool(res, "frozen", frozen);
	} else {
		reply_err(res, STAGE_ERROR_BUSY, "command queue is full");
	}
	target_free(&t);
}

static void h_panic(obs_data_t *req, obs_data_t *res)
{
	ONLY("");
	stage_panic_trigger(); /* bypasses (and empties) the queue */
	stage_vendor_note_cancel(NULL);
	reply_ok(res);
}

/* ------------------------------------------------------------------------- */
/* Dispatch                                                                   */
/* ------------------------------------------------------------------------- */

struct handler {
	const char *name;
	void (*fn)(obs_data_t *req, obs_data_t *res);
};

static const struct handler handlers[] = {
	{STAGE_REQ_GET_INFO, h_get_info},       {STAGE_REQ_GET_STATE, h_get_state},
	{STAGE_REQ_CAMERA_MOVE, h_camera_move}, {STAGE_REQ_CAMERA_SET, h_camera_set},
	{STAGE_REQ_CAMERA_RESET, h_camera_reset}, {STAGE_REQ_CAMERA_AUTO, h_camera_auto},
	{STAGE_REQ_FX_SET, h_fx_set},           {STAGE_REQ_FX_PULSE, h_fx_pulse},
	{STAGE_REQ_DOF_SET, h_dof_set},         {STAGE_REQ_LAYER_LIST, h_layer_list},
	{STAGE_REQ_LAYER_SET, h_layer_set},     {STAGE_REQ_MOVE_LIST, h_move_list},
	{STAGE_REQ_RECIPE_LIST, h_recipe_list}, {STAGE_REQ_RECIPE_APPLY, h_recipe_apply},
	{STAGE_REQ_VIEW_SAVE, h_view_save},     {STAGE_REQ_VIEW_APPLY, h_view_apply},
	{STAGE_REQ_VIEW_LIST, h_view_list},     {STAGE_REQ_VIEW_DELETE, h_view_delete},
	{STAGE_REQ_FREEZE, h_freeze},           {STAGE_REQ_PANIC, h_panic},
};

void stage_vendor_dispatch(const char *request_type, obs_data_t *req, obs_data_t *res)
{
	if (!res)
		return;
	for (size_t i = 0; request_type && i < sizeof(handlers) / sizeof(handlers[0]); i++) {
		if (strcmp(handlers[i].name, request_type) == 0) {
			handlers[i].fn(req, res);
			return;
		}
	}
	reply_err(res, STAGE_ERROR_BAD_PARAM, "unknown request '%.48s'",
		  request_type ? request_type : "");
}
