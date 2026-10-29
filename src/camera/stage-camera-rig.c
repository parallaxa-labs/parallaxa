/*
 * stage-camera-rig.c — the stage_camera_rig scene filter: zero render passes,
 * it moves the scene's own items (vendor-api.md section 3).
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Rest poses. Each enrolled item keeps the user's own transform in
 * stage.rest. Every tick compares the item's transform with the one the rig
 * applied last: a difference means the user moved it, so the rest pose is
 * recomputed (stage_unsolve_layer_f with the camera of that application)
 * and persisted. Removing, disabling or destroying the rig, Bake rest pose,
 * Tools > Restore all rest poses and a scene collection change put every
 * item back to its rest pose.
 *
 * Saving. libobs serializes a scene's items (obs_source_save -> scene_save)
 * before the scene's filters, in the same obs_save_source() call, and keeps
 * the serialized objects by reference until the file is written. The rig's
 * save callback therefore rewrites its displaced items in the scene's
 * just-written "items" array to their rest poses (absolute pos/scale/bounds;
 * the relative copies are dropped so libobs reads the absolute ones, its own
 * backwards-compatible load path). Scene collection files (and undo states
 * built with obs_save_source, such as a deleted scene) never hold a displaced
 * pose, an uninstalled plugin finds the user's layout, and save/load cycles
 * cannot drift. Only an active rig writes them: an inactive one does not
 * track edits, so its rest poses may be stale.
 *
 * Loading. An item whose loaded transform is neither its stage.rest nor that
 * rest with the background overscan at home was moved while no rig applied
 * it (rig disabled, plugin not loaded): that transform becomes the rest.
 *
 * Not covered: OBS's own item-transform undo (obs_scene_save_transform_states)
 * records the live, possibly displaced transform. Undoing a drag made during
 * camera motion restores that pose, which the rig then reads as an edit (a
 * slightly offset rest pose).
 */
#include "stage-camera-rig.h"

#include <float.h>
#include <math.h>
#include <obs-frontend-api.h>
#include <stdio.h>
#include <string.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include "stage-solver.h"

/* User-edit detection tolerances. libobs stores positions relative to the
 * canvas and reports them rounded to 0.5 px (obs-scene.c nudge_abs_values). */
#define EDIT_TOL_PX 0.6f
#define EDIT_TOL_SCALE 1e-4f
#define EDIT_TOL_ROT 0.01f
/* Apply thresholds: smaller changes are not written to the scene item. */
#define APPLY_EPS_PX 0.01f
#define APPLY_EPS_SCALE 0.0001f
#define APPLY_EPS_ROT 0.0005f
/* Depth blur strength change worth a settings write. */
#define BLUR_EPS 0.002f
/* Overscan cap (UX: 0 to 25 %). */
#define OVERSCAN_MAX 0.25f
/* Rebuild the layer cache at least this often (missed signals, visibility). */
#define REBUILD_PERIOD_S 1.0f
#define FX_POLL_S 0.25f
#define REGISTER_RETRY_MS 2000
#define AUDIO_RETRY_S 1.0f

static pthread_mutex_t g_rigs_lock = PTHREAD_MUTEX_INITIALIZER;
static DARRAY(struct stage_rig *) g_rigs;

static const struct stage_cam cam_home = STAGE_CAM_HOME_INIT;

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static inline float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static void atomic_or(volatile long *v, long bits)
{
	long old = os_atomic_load_long(v);
	while (!os_atomic_compare_exchange_long(v, &old, old | bits))
		;
}

void rig_addref(struct stage_rig *r)
{
	os_atomic_inc_long(&r->refs);
}

static void layer_free(struct rig_layer *L);

void rig_release(struct stage_rig *r)
{
	if (!r || os_atomic_dec_long(&r->refs) > 0)
		return;
	for (size_t i = 0; i < r->layers.num; i++)
		layer_free(&r->layers.array[i]);
	da_free(r->layers);
	for (size_t i = 0; i < r->persist.num; i++)
		obs_sceneitem_release(r->persist.array[i].item);
	da_free(r->persist);
	for (size_t i = 0; i < r->blur.num; i++)
		obs_weak_source_release(r->blur.array[i].filter);
	da_free(r->blur);
	da_free(r->views);
	da_free(r->pending);
	obs_weak_source_release(r->parent);
	obs_weak_source_release(r->self);
	bfree(r->snap);
	stage_state_buf_free(&r->state);
	pthread_mutex_destroy(&r->lock);
	pthread_mutex_destroy(&r->snap_lock);
	pthread_mutex_destroy(&r->audio_lock);
	bfree(r);
}

bool rig_push(struct stage_rig *r, struct stage_cmd *c)
{
	pthread_mutex_lock(&r->lock);
	stage_strlcpy(c->scene_uuid, r->scene_uuid, sizeof(c->scene_uuid));
	pthread_mutex_unlock(&r->lock);
	if (!c->scene_uuid[0])
		return false;
	return stage_cmdq_push(c);
}

void rig_format(char *out, size_t cap, const char *key, const char *a1, const char *a2)
{
	const char *fmt = obs_module_text(key);
	struct dstr s = {0};
	dstr_copy(&s, fmt);
	dstr_replace(&s, "%1", a1 ? a1 : "");
	dstr_replace(&s, "%2", a2 ? a2 : "");
	stage_strlcpy(out, s.array ? s.array : "", cap);
	dstr_free(&s);
}

static void rig_kick_ui(struct stage_rig *r);

void rig_set_status(struct stage_rig *r, const char *key, const char *a1, const char *a2,
		    enum obs_text_info_type type)
{
	pthread_mutex_lock(&r->lock);
	stage_strlcpy(r->status_key, key ? key : "", sizeof(r->status_key));
	stage_strlcpy(r->status_a1, a1 ? a1 : "", sizeof(r->status_a1));
	stage_strlcpy(r->status_a2, a2 ? a2 : "", sizeof(r->status_a2));
	r->status_type = type;
	r->ui_bits |= RIG_UI_STATUS;
	pthread_mutex_unlock(&r->lock);
	rig_kick_ui(r);
}

void rig_request_bake(struct stage_rig *r)
{
	os_atomic_set_long(&r->bake_req, 1);
}

size_t rig_snapshot(struct stage_rig *r, struct stage_layer_info *out, size_t cap)
{
	pthread_mutex_lock(&r->snap_lock);
	size_t n = r->nsnap < cap ? r->nsnap : cap;
	if (n && out)
		memcpy(out, r->snap, n * sizeof(*out));
	pthread_mutex_unlock(&r->snap_lock);
	return n;
}

/* ------------------------------------------------------------------------- */
/* Role guess (vendor-api.md 4.13)                                           */
/* ------------------------------------------------------------------------- */

static bool has_token(const char *lower, const char *tok)
{
	size_t n = strlen(tok);
	for (const char *p = lower; (p = strstr(p, tok)) != NULL; p++) {
		bool left = p == lower || !((p[-1] >= 'a' && p[-1] <= 'z') || (p[-1] >= '0' && p[-1] <= '9'));
		char c = p[n];
		bool right = !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'));
		if (left && right)
			return true;
	}
	return false;
}

int stage_cam_guess_role(const char *name)
{
	if (!name || !name[0])
		return STAGE_ROLE_NONE;
	char lower[256];
	size_t i = 0;
	for (; name[i] && i < sizeof(lower) - 1; i++) {
		char c = name[i];
		lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
	}
	lower[i] = '\0';
	if (strstr(lower, "background") || strstr(lower, "\xE8\x83\x8C\xE6\x99\xAF") /* 背景 */ ||
	    has_token(lower, "bg"))
		return STAGE_ROLE_BG;
	if (strstr(lower, "spout") || strstr(lower, "avatar") || strstr(lower, "model") ||
	    strstr(lower, "vtube") || has_token(lower, "vts"))
		return STAGE_ROLE_CHAR;
	if (strstr(lower, "foreground") || strstr(lower, "\xE5\x89\x8D\xE6\x99\xAF") /* 前景 */ ||
	    has_token(lower, "fg"))
		return STAGE_ROLE_FG;
	return STAGE_ROLE_NONE;
}

/* ------------------------------------------------------------------------- */
/* Transforms                                                                */
/* ------------------------------------------------------------------------- */

static void xform_from_info(const struct obs_transform_info *info, struct stage_xform *x)
{
	bool bounds = info->bounds_type != OBS_BOUNDS_NONE;
	x->px = info->pos.x;
	x->py = info->pos.y;
	x->sx = bounds ? info->bounds.x : info->scale.x;
	x->sy = bounds ? info->bounds.y : info->scale.y;
	x->rot = info->rot;
}

static void xform_to_info(const struct stage_xform *x, bool bounds, struct obs_transform_info *info)
{
	info->pos.x = x->px;
	info->pos.y = x->py;
	info->rot = x->rot;
	if (bounds) {
		info->bounds.x = x->sx;
		info->bounds.y = x->sy;
	} else {
		info->scale.x = x->sx;
		info->scale.y = x->sy;
	}
}

/* Bounds sizes are pixels, scales are multipliers. */
static bool xform_differs(const struct stage_xform *a, const struct stage_xform *b, bool bounds,
			  float tol_px, float tol_scale, float tol_rot)
{
	if (fabsf(a->px - b->px) > tol_px || fabsf(a->py - b->py) > tol_px)
		return true;
	float ts_x = bounds ? tol_px : tol_scale * fmaxf(1.0f, fabsf(b->sx));
	float ts_y = bounds ? tol_px : tol_scale * fmaxf(1.0f, fabsf(b->sy));
	if (fabsf(a->sx - b->sx) > ts_x || fabsf(a->sy - b->sy) > ts_y)
		return true;
	return fabsf(stage_wrap_deg(a->rot - b->rot)) > tol_rot;
}

/* Background overscan: the rest pose grown by (1 + o) about (cx, cy). */
static void oversize(const struct stage_xform *in, float o, float cx, float cy,
		     struct stage_xform *out)
{
	float k = 1.0f + o;
	out->px = cx + (in->px - cx) * k;
	out->py = cy + (in->py - cy) * k;
	out->sx = in->sx * k;
	out->sy = in->sy * k;
	out->rot = in->rot;
}

static void undersize(const struct stage_xform *in, float o, float cx, float cy,
		      struct stage_xform *out)
{
	float k = 1.0f + o;
	out->px = cx + (in->px - cx) / k;
	out->py = cy + (in->py - cy) / k;
	out->sx = in->sx / k;
	out->sy = in->sy / k;
	out->rot = in->rot;
}

static float align_off(uint32_t align, uint32_t lo_flag, uint32_t hi_flag, float size)
{
	if (align & lo_flag)
		return 0.0f;
	if (align & hi_flag)
		return size;
	return size * 0.5f;
}

/* Axis-aligned rect of the item's visible content at its rest pose (rotation
 * ignored by the caller). False when the source has no size yet. */
static bool layer_rest_rect(const struct rig_layer *L, const struct obs_transform_info *cur,
			    struct stage_rect *out)
{
	obs_source_t *src = obs_sceneitem_get_source(L->item);
	struct obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(L->item, &crop);
	float cw = (float)obs_source_get_width(src) - (float)(crop.left + crop.right);
	float ch = (float)obs_source_get_height(src) - (float)(crop.top + crop.bottom);
	if (!(cw > 0.0f && ch > 0.0f))
		return false;

	float box_w, box_h, w, h, ox = 0.0f, oy = 0.0f;
	if (!L->bounds) {
		box_w = w = cw * fabsf(L->rest.sx);
		box_h = h = ch * fabsf(L->rest.sy);
	} else {
		box_w = fabsf(L->rest.sx);
		box_h = fabsf(L->rest.sy);
		float sx = box_w / cw, sy = box_h / ch, s;
		switch (cur->bounds_type) {
		case OBS_BOUNDS_STRETCH:
			w = box_w;
			h = box_h;
			break;
		case OBS_BOUNDS_SCALE_OUTER:
			s = fmaxf(sx, sy);
			w = cur->crop_to_bounds ? box_w : cw * s;
			h = cur->crop_to_bounds ? box_h : ch * s;
			break;
		case OBS_BOUNDS_SCALE_TO_WIDTH:
			w = box_w;
			h = ch * sx;
			break;
		case OBS_BOUNDS_SCALE_TO_HEIGHT:
			w = cw * sy;
			h = box_h;
			break;
		case OBS_BOUNDS_MAX_ONLY:
			s = fminf(1.0f, fminf(sx, sy));
			w = cw * s;
			h = ch * s;
			break;
		case OBS_BOUNDS_SCALE_INNER:
		default:
			s = fminf(sx, sy);
			w = cw * s;
			h = ch * s;
			break;
		}
		ox = align_off(cur->bounds_alignment, OBS_ALIGN_LEFT, OBS_ALIGN_RIGHT, box_w - w);
		oy = align_off(cur->bounds_alignment, OBS_ALIGN_TOP, OBS_ALIGN_BOTTOM, box_h - h);
		if (!(cur->bounds_alignment & (OBS_ALIGN_LEFT | OBS_ALIGN_RIGHT)))
			ox = (box_w - w) * 0.5f;
		if (!(cur->bounds_alignment & (OBS_ALIGN_TOP | OBS_ALIGN_BOTTOM)))
			oy = (box_h - h) * 0.5f;
	}
	float left = L->rest.px - align_off(cur->alignment, OBS_ALIGN_LEFT, OBS_ALIGN_RIGHT, box_w) + ox;
	float top = L->rest.py - align_off(cur->alignment, OBS_ALIGN_TOP, OBS_ALIGN_BOTTOM, box_h) + oy;
	out->l = left;
	out->t = top;
	out->r = left + w;
	out->b = top + h;
	return w > 0.0f && h > 0.0f;
}

/* ------------------------------------------------------------------------- */
/* Layer cache                                                               */
/* ------------------------------------------------------------------------- */

static void layer_src_signal(void *data, calldata_t *cd)
{
	(void)cd;
	struct stage_rig *r = data;
	os_atomic_set_long(&r->dirty, 1);
}

static void layer_disconnect(struct stage_rig *r, struct rig_layer *L)
{
	if (!L->sig_src)
		return;
	obs_source_t *s = obs_weak_source_get_source(L->sig_src);
	if (s) {
		signal_handler_t *sh = obs_source_get_signal_handler(s);
		signal_handler_disconnect(sh, "filter_add", layer_src_signal, r);
		signal_handler_disconnect(sh, "filter_remove", layer_src_signal, r);
		obs_source_release(s);
	}
	obs_weak_source_release(L->sig_src);
	L->sig_src = NULL;
}

static void layer_free(struct rig_layer *L)
{
	obs_weak_source_release(L->blur);
	L->blur = NULL;
	obs_weak_source_release(L->sig_src);
	L->sig_src = NULL;
	obs_sceneitem_release(L->item);
	L->item = NULL;
}

static struct rig_layer *find_layer(struct stage_rig *r, int64_t id)
{
	for (size_t i = 0; i < r->layers.num; i++)
		if (r->layers.array[i].id == id)
			return &r->layers.array[i];
	return NULL;
}

static bool role_blurs(uint8_t role)
{
	return role == STAGE_ROLE_BG || role == STAGE_ROLE_FG || role == STAGE_ROLE_SKY;
}

static float role_f(const struct rig_layer *L)
{
	return stage_role_factor(L->role, L->depth);
}

static void queue_persist(struct stage_rig *r, struct rig_layer *L, enum persist_kind kind)
{
	struct persist_job *j = da_push_back_new(r->persist);
	obs_sceneitem_addref(L->item);
	j->item = L->item;
	j->kind = kind;
	j->rest = L->rest;
	j->has_rest = L->has_rest;
	j->role = L->role;
	j->depth = L->depth;
	if (kind != PERSIST_UNENROL)
		L->rest_dirty = false;
}

static void queue_blur(struct stage_rig *r, obs_weak_source_t *filter, enum blur_op op, float s)
{
	if (!filter)
		return;
	struct blur_job *j = da_push_back_new(r->blur);
	obs_weak_source_addref(filter);
	j->filter = filter;
	j->op = op;
	j->strength = s;
}

static void stop_driving(struct stage_rig *r, struct rig_layer *L)
{
	if (L->driving)
		queue_blur(r, L->blur, BLUR_STOP, 0.0f);
	L->driving = false;
	L->blur_written = -1.0f;
}

/* Reads stage.depth / stage.role / stage.rest of a newly seen item. */
static void layer_read_private(struct rig_layer *L)
{
	obs_data_t *priv = obs_sceneitem_get_private_settings(L->item);
	if (!priv)
		return;
	if (obs_data_has_user_value(priv, STAGE_ITEM_KEY_DEPTH)) {
		double d = obs_data_get_double(priv, STAGE_ITEM_KEY_DEPTH);
		L->enrolled = true;
		L->depth = stage_depth_clamp(isfinite(d) ? (float)d : 1.0f);
		int role = stage_role_from_name(obs_data_get_string(priv, STAGE_ITEM_KEY_ROLE));
		if (role < 0 || role == STAGE_ROLE_NONE) /* depth without a role */
			role = L->depth > 1.0f ? STAGE_ROLE_BG
			       : L->depth < 1.0f ? STAGE_ROLE_FG
						 : STAGE_ROLE_CHAR;
		L->role = (uint8_t)role;
		obs_data_t *rest = obs_data_get_obj(priv, STAGE_ITEM_KEY_REST);
		if (rest && obs_data_has_user_value(rest, STAGE_REST_KEY_PX)) {
			L->rest.px = (float)obs_data_get_double(rest, STAGE_REST_KEY_PX);
			L->rest.py = (float)obs_data_get_double(rest, STAGE_REST_KEY_PY);
			L->rest.sx = (float)obs_data_get_double(rest, STAGE_REST_KEY_SX);
			L->rest.sy = (float)obs_data_get_double(rest, STAGE_REST_KEY_SY);
			L->rest.rot = (float)obs_data_get_double(rest, STAGE_REST_KEY_ROT);
			L->has_rest = isfinite(L->rest.px) && isfinite(L->rest.py) &&
				      isfinite(L->rest.sx) && isfinite(L->rest.sy) &&
				      isfinite(L->rest.rot);
			L->fresh = L->has_rest; /* checked on the first active tick */
		}
		obs_data_release(rest);
	}
	obs_data_release(priv);
}

/* The item's own transform becomes its rest pose (the item is assumed to be
 * where the user put it: new enrolment, or no stored rest). */
static void layer_capture_rest(struct rig_layer *L)
{
	struct obs_transform_info info;
	obs_sceneitem_get_info2(L->item, &info);
	L->bounds = info.bounds_type != OBS_BOUNDS_NONE;
	xform_from_info(&info, &L->rest);
	L->has_rest = true;
	L->fresh = false;
	L->rest_dirty = true;
	L->applied = false;
}

struct find_blur_ctx {
	obs_weak_source_t *found;
};

static void find_blur_cb(obs_source_t *parent, obs_source_t *child, void *param)
{
	(void)parent;
	struct find_blur_ctx *c = param;
	if (c->found)
		return;
	const char *id = obs_source_get_unversioned_id(child);
	if (id && strcmp(id, STAGE_FILTER_DEPTH_BLUR) == 0)
		c->found = obs_source_get_weak_source(child);
}

/* Depth blur filter lookup and filter-add/remove signals for an enrolled
 * layer; releases both for an un-enrolled one. */
static void layer_refresh_blur(struct stage_rig *r, struct rig_layer *L)
{
	obs_source_t *src = obs_sceneitem_get_source(L->item);
	struct find_blur_ctx c = {NULL};
	if (L->enrolled && role_blurs(L->role) && src)
		obs_source_enum_filters(src, find_blur_cb, &c);
	if (L->blur) {
		/* Keep the current filter (and its driving state) when it is
		 * still the one on the source. */
		obs_source_t *old = obs_weak_source_get_source(L->blur);
		obs_source_t *now = c.found ? obs_weak_source_get_source(c.found) : NULL;
		bool keep = old && old == now;
		obs_source_release(old);
		obs_source_release(now);
		if (keep) {
			obs_weak_source_release(c.found);
			c.found = NULL;
		} else {
			stop_driving(r, L);
			obs_weak_source_release(L->blur);
			L->blur = NULL;
		}
	}
	if (c.found) {
		obs_weak_source_release(L->blur);
		L->blur = c.found;
		L->driving = false;
		L->blur_written = -1.0f;
	}

	if (L->enrolled && src && !L->sig_src) {
		signal_handler_t *sh = obs_source_get_signal_handler(src);
		signal_handler_connect(sh, "filter_add", layer_src_signal, r);
		signal_handler_connect(sh, "filter_remove", layer_src_signal, r);
		L->sig_src = obs_source_get_weak_source(src);
	} else if (!L->enrolled && L->sig_src) {
		layer_disconnect(r, L);
	}
}

static void layer_restore(struct rig_layer *L)
{
	if (!L->enrolled || !L->has_rest)
		return;
	struct obs_transform_info info;
	obs_sceneitem_get_info2(L->item, &info);
	if ((info.bounds_type != OBS_BOUNDS_NONE) != L->bounds)
		return; /* the user changed the bounds mode: the current pose is theirs */
	xform_to_info(&L->rest, L->bounds, &info);
	obs_sceneitem_set_info2(L->item, &info);
	L->applied = true;
	L->applied_x = L->rest;
	L->applied_cam = cam_home;
	L->applied_f = role_f(L);
	L->applied_o = 0.0f;
	L->applied_ocx = L->applied_ocy = 0.0f;
}

static void restore_all_locked(struct stage_rig *r)
{
	for (size_t i = 0; i < r->layers.num; i++) {
		struct rig_layer *L = &r->layers.array[i];
		if (L->applied)
			layer_restore(L);
		stop_driving(r, L);
	}
}

/* Rows the panel shows: membership, names, roles, blur. Depth is left out so
 * that the panel's own slider drags do not rebuild it. */
static bool snap_row_differs(const struct stage_layer_info *a, const struct stage_layer_info *b)
{
	return a->item_id != b->item_id || a->enrolled != b->enrolled || a->role != b->role ||
	       a->depth_blur != b->depth_blur || strcmp(a->name, b->name) != 0;
}

/* rig->lock held. notify: refresh an open panel when its rows changed (off
 * for edits the panel made itself: it already shows them). */
static void update_snapshot(struct stage_rig *r, bool notify)
{
	bool changed = false;
	pthread_mutex_lock(&r->snap_lock);
	if (r->snapcap < r->layers.num) {
		r->snapcap = r->layers.num + 8;
		r->snap = brealloc(r->snap, r->snapcap * sizeof(*r->snap));
	}
	changed = r->nsnap != r->layers.num;
	for (size_t i = 0; i < r->layers.num; i++) {
		const struct rig_layer *L = &r->layers.array[i];
		struct stage_layer_info *o = &r->snap[i];
		struct stage_layer_info prev = {0};
		if (!changed) /* same count: row i existed before */
			prev = *o;
		memset(o, 0, sizeof(*o));
		o->item_id = L->id;
		stage_strlcpy(o->name, L->name, sizeof(o->name));
		stage_strlcpy(o->source_uuid, L->src_uuid, sizeof(o->source_uuid));
		o->enrolled = L->enrolled;
		o->depth = L->enrolled ? (double)L->depth : 0.0;
		o->role = L->enrolled ? L->role : (uint8_t)STAGE_ROLE_NONE;
		o->visible = L->visible;
		o->locked = L->locked;
		o->depth_blur = L->blur != NULL;
		if (!changed && snap_row_differs(o, &prev))
			changed = true;
	}
	r->nsnap = r->layers.num;
	pthread_mutex_unlock(&r->snap_lock);
	if (changed && notify)
		r->ui_bits |= RIG_UI_PROPS;
}

struct rebuild_ctx {
	struct stage_rig *r;
	DARRAY(struct rig_layer) next;
};

static bool rebuild_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	(void)scene;
	struct rebuild_ctx *c = param;
	struct stage_rig *r = c->r;
	int64_t id = obs_sceneitem_get_id(item);
	struct rig_layer *old = find_layer(r, id);
	struct rig_layer L;
	if (old && old->item == item) {
		L = *old;
		old->seen = true; /* moved into next */
	} else {
		memset(&L, 0, sizeof(L));
		obs_sceneitem_addref(item);
		L.item = item;
		L.id = id;
		L.blur_written = -1.0f;
		L.depth = 1.0f;
		layer_read_private(&L);
	}
	obs_source_t *src = obs_sceneitem_get_source(item);
	stage_strlcpy(L.name, src ? obs_source_get_name(src) : "", sizeof(L.name));
	stage_strlcpy(L.src_uuid, src ? obs_source_get_uuid(src) : "", sizeof(L.src_uuid));
	L.visible = obs_sceneitem_visible(item);
	L.locked = obs_sceneitem_locked(item);
	da_push_back(c->next, &L);
	return true;
}

static void rebuild_layers(struct stage_rig *r, obs_scene_t *scene)
{
	for (size_t i = 0; i < r->layers.num; i++)
		r->layers.array[i].seen = false;
	struct rebuild_ctx c = {r, {0}};
	da_init(c.next);
	obs_scene_enum_items(scene, rebuild_cb, &c);
	/* Items that left the scene (or were replaced). */
	for (size_t i = 0; i < r->layers.num; i++) {
		struct rig_layer *L = &r->layers.array[i];
		if (L->seen)
			continue;
		stop_driving(r, L);
		layer_disconnect(r, L);
		layer_free(L);
	}
	da_free(r->layers);
	r->layers.da = c.next.da;
	for (size_t i = 0; i < r->layers.num; i++) {
		struct rig_layer *L = &r->layers.array[i];
		if (L->enrolled && !L->has_rest)
			layer_capture_rest(L);
		layer_refresh_blur(r, L);
	}
	update_snapshot(r, true);
}

/* The state a layer_set leads to (vendor-api 4.11): role NONE or UNENROL
 * un-enrols; depth alone on an un-enrolled item picks the role from it; a
 * role alone on an un-enrolled item takes the role's default depth. */
struct layer_state {
	bool enrolled;
	uint8_t role;
	float depth;
};

static struct layer_state layer_set_result(struct layer_state cur, int role_arg, float depth_arg,
					   unsigned mask)
{
	struct layer_state n = {false, STAGE_ROLE_NONE, 1.0f};
	if ((mask & STAGE_LAYER_UNENROL) || ((mask & STAGE_LAYER_ROLE) && role_arg == STAGE_ROLE_NONE))
		return n;
	uint8_t role = cur.enrolled ? cur.role : (uint8_t)STAGE_ROLE_NONE;
	float depth = cur.enrolled ? cur.depth : 1.0f;
	if ((mask & STAGE_LAYER_ROLE) && role_arg >= 0 && role_arg < STAGE_ROLE_COUNT)
		role = (uint8_t)role_arg;
	if (mask & STAGE_LAYER_DEPTH)
		depth = stage_depth_clamp(depth_arg);
	else if (!cur.enrolled)
		depth = stage_role_default_depth(role);
	if (role == STAGE_ROLE_NONE) /* depth only on an un-enrolled item */
		role = depth > 1.0f ? STAGE_ROLE_BG : depth < 1.0f ? STAGE_ROLE_FG : STAGE_ROLE_CHAR;
	n.enrolled = true;
	n.role = role;
	n.depth = depth;
	return n;
}

static struct rig_pending_layer *find_pending(struct stage_rig *r, int64_t id)
{
	for (size_t i = 0; i < r->pending.num; i++)
		if (r->pending.array[i].id == id)
			return &r->pending.array[i];
	return NULL;
}

/* rig->lock held. Drops expired panel edits (their command never ran). */
static void prune_pending(struct stage_rig *r, uint64_t now)
{
	for (size_t i = r->pending.num; i > 0; i--)
		if (r->pending.array[i - 1].until_ms <= now)
			da_erase(r->pending, i - 1);
}

#define PENDING_TTL_MS 3000

void rig_pending_layer_set(struct stage_rig *r, int64_t id, int role, float depth, unsigned mask)
{
	pthread_mutex_lock(&r->lock);
	uint64_t now = stage_core_now_ms();
	prune_pending(r, now);
	struct rig_pending_layer *p = find_pending(r, id);
	struct layer_state cur = {false, STAGE_ROLE_NONE, 1.0f};
	struct rig_layer *L = find_layer(r, id);
	if (p) {
		cur = (struct layer_state){p->enrolled, p->role, p->depth};
	} else if (L) {
		cur = (struct layer_state){L->enrolled, L->role, L->depth};
	} /* else not in the layer cache yet: the panel only pushes roles for
	   * un-enrolled items then (Auto-assign, first add). */
	struct layer_state n = layer_set_result(cur, role, depth, mask);
	if (!p) {
		p = da_push_back_new(r->pending);
		p->id = id;
	}
	p->enrolled = n.enrolled;
	p->role = n.role;
	p->depth = n.depth;
	p->until_ms = now + PENDING_TTL_MS;
	pthread_mutex_unlock(&r->lock);
}

/* rig->lock held. */
static void pending_apply_locked(struct stage_rig *r, struct stage_layer_info *rows, size_t n)
{
	prune_pending(r, stage_core_now_ms());
	for (size_t i = 0; i < n && r->pending.num; i++) {
		const struct rig_pending_layer *p = find_pending(r, rows[i].item_id);
		if (p) {
			rows[i].enrolled = p->enrolled;
			rows[i].role = p->enrolled ? p->role : (uint8_t)STAGE_ROLE_NONE;
			rows[i].depth = p->enrolled ? (double)p->depth : 0.0;
		}
	}
}

void rig_pending_apply(struct stage_rig *r, struct stage_layer_info *rows, size_t n)
{
	pthread_mutex_lock(&r->lock);
	pending_apply_locked(r, rows, n);
	pthread_mutex_unlock(&r->lock);
}

/* Snapshot and overlay under one rig->lock: the tick swaps a pending entry
 * for the applied state atomically, so a row is never shown stale. */
size_t rig_panel_snapshot(struct stage_rig *r, struct stage_layer_info *out, size_t cap)
{
	pthread_mutex_lock(&r->lock);
	size_t n = rig_snapshot(r, out, cap);
	pending_apply_locked(r, out, n);
	pthread_mutex_unlock(&r->lock);
	return n;
}

/* layer_set (vendor-api 4.11), also used by the panel and Auto-assign. */
static void do_layer_set(struct stage_rig *r, const struct stage_cmd *cmd)
{
	int64_t id = cmd->u.layer.item_id;
	struct rig_layer *L = find_layer(r, id);
	if (!L) {
		SLOG_DEBUG("layer_set: item %lld not in the rig's scene", (long long)id);
		return;
	}
	struct layer_state cur = {L->enrolled, L->role, L->depth};
	struct layer_state n = layer_set_result(cur, cmd->u.layer.role, cmd->u.layer.depth,
						cmd->u.layer.mask);
	if (!n.enrolled) {
		if (L->enrolled) {
			if (L->applied)
				layer_restore(L);
			stop_driving(r, L);
			L->enrolled = false;
			L->has_rest = false;
			L->fresh = false;
			L->applied = false;
			L->role = STAGE_ROLE_NONE;
			queue_persist(r, L, PERSIST_UNENROL);
			layer_refresh_blur(r, L);
		}
	} else {
		if (!L->enrolled) {
			L->enrolled = true;
			layer_capture_rest(L);
		}
		L->role = n.role;
		L->depth = n.depth;
		queue_persist(r, L, PERSIST_ENROL);
		layer_refresh_blur(r, L);
	}
	/* A panel edit: the panel already shows it. The entry goes once the
	 * last of its commands (a slider drag sends many) has been applied. */
	struct rig_pending_layer *p = find_pending(r, id);
	bool from_panel = p != NULL;
	if (p && p->enrolled == L->enrolled && (!p->enrolled || (p->role == L->role &&
								 fabsf(p->depth - L->depth) < 1e-4f)))
		da_erase(r->pending, (size_t)(p - r->pending.array));
	update_snapshot(r, !from_panel);
}

/* ------------------------------------------------------------------------- */
/* Views                                                                     */
/* ------------------------------------------------------------------------- */

#ifdef STAGE_HAS_PRESETS
#include "presets/stage-target.h"
#else
/* Minimal view storage when the presets module is not built: the same
 * scene private-settings layout as presets/stage-target.c. */
struct stage_view {
	uint8_t slot;
	char name[STAGE_VIEW_NAME_MAX];
	float x, y, zoom, rot;
	uint8_t transition;
	uint32_t duration_ms;
};
static pthread_mutex_t views_lock = PTHREAD_MUTEX_INITIALIZER;

/* Every valid saved view, from one locked read of the scene's views. */
static size_t stage_views_list(obs_source_t *scene, struct stage_view out[STAGE_VIEW_SLOT_MAX])
{
	size_t k = 0;
	pthread_mutex_lock(&views_lock);
	obs_data_t *priv = obs_source_get_private_settings(scene);
	obs_data_array_t *arr = priv ? obs_data_get_array(priv, STAGE_SCENE_KEY_VIEWS) : NULL;
	for (size_t i = 0, n = obs_data_array_count(arr); i < n && k < STAGE_VIEW_SLOT_MAX; i++) {
		obs_data_t *d = obs_data_array_item(arr, i);
		long long slot = obs_data_get_int(d, "slot");
		if (slot >= STAGE_VIEW_SLOT_MIN && slot <= STAGE_VIEW_SLOT_MAX) {
			struct stage_view *v = &out[k];
			obs_data_t *cam = obs_data_get_obj(d, "camera");
			memset(v, 0, sizeof(*v));
			v->slot = (uint8_t)slot;
			stage_strlcpy(v->name, obs_data_get_string(d, "name"), sizeof(v->name));
			v->x = (float)obs_data_get_double(cam, "x");
			v->y = (float)obs_data_get_double(cam, "y");
			v->zoom = (float)obs_data_get_double(cam, "zoom");
			v->rot = (float)obs_data_get_double(cam, "rot");
			int tr = stage_transition_from_name(obs_data_get_string(d, "transition"));
			v->transition = (uint8_t)(tr < 0 ? STAGE_TRANSITION_SMOOTH : tr);
			v->duration_ms = (uint32_t)obs_data_get_int(d, "duration_ms");
			if (v->zoom >= STAGE_ZOOM_MIN && v->zoom <= STAGE_ZOOM_MAX)
				k++;
			obs_data_release(cam);
		}
		obs_data_release(d);
	}
	obs_data_array_release(arr);
	obs_data_release(priv);
	pthread_mutex_unlock(&views_lock);
	return k;
}

static bool stage_views_get(obs_source_t *scene, uint8_t slot, struct stage_view *out)
{
	struct stage_view all[STAGE_VIEW_SLOT_MAX];
	size_t n = stage_views_list(scene, all);
	for (size_t i = 0; i < n; i++) {
		if (all[i].slot == slot) {
			*out = all[i];
			return true;
		}
	}
	return false;
}

static bool stage_views_put(obs_source_t *scene, const struct stage_view *v, uint32_t *gen)
{
	if (gen)
		*gen = 0; /* no write generations without the vendor */
	pthread_mutex_lock(&views_lock);
	obs_data_t *priv = obs_source_get_private_settings(scene);
	obs_data_array_t *old = obs_data_get_array(priv, STAGE_SCENE_KEY_VIEWS);
	obs_data_array_t *arr = obs_data_array_create();
	for (size_t i = 0, n = obs_data_array_count(old); i < n; i++) {
		obs_data_t *d = obs_data_array_item(old, i);
		if (obs_data_get_int(d, "slot") != v->slot)
			obs_data_array_push_back(arr, d);
		obs_data_release(d);
	}
	obs_data_t *d = obs_data_create();
	obs_data_t *cam = obs_data_create();
	obs_data_set_int(d, "slot", v->slot);
	obs_data_set_string(d, "name", v->name);
	obs_data_set_double(cam, "x", v->x);
	obs_data_set_double(cam, "y", v->y);
	obs_data_set_double(cam, "zoom", v->zoom);
	obs_data_set_double(cam, "rot", v->rot);
	obs_data_set_obj(d, "camera", cam);
	obs_data_set_string(d, "transition", stage_transition_names[v->transition % STAGE_TRANSITION_COUNT]);
	obs_data_set_int(d, "duration_ms", v->duration_ms);
	obs_data_array_push_back(arr, d);
	obs_data_set_array(priv, STAGE_SCENE_KEY_VIEWS, arr);
	obs_data_release(cam);
	obs_data_release(d);
	obs_data_array_release(arr);
	obs_data_array_release(old);
	obs_data_release(priv);
	pthread_mutex_unlock(&views_lock);
	return true;
}

static bool stage_views_delete(obs_source_t *scene, uint8_t slot, uint32_t *gen)
{
	if (gen)
		*gen = 0;
	struct stage_view v;
	if (!stage_views_get(scene, slot, &v))
		return false;
	pthread_mutex_lock(&views_lock);
	obs_data_t *priv = obs_source_get_private_settings(scene);
	obs_data_array_t *old = obs_data_get_array(priv, STAGE_SCENE_KEY_VIEWS);
	obs_data_array_t *arr = obs_data_array_create();
	for (size_t i = 0, n = obs_data_array_count(old); i < n; i++) {
		obs_data_t *d = obs_data_array_item(old, i);
		if (obs_data_get_int(d, "slot") != slot)
			obs_data_array_push_back(arr, d);
		obs_data_release(d);
	}
	obs_data_set_array(priv, STAGE_SCENE_KEY_VIEWS, arr);
	obs_data_array_release(arr);
	obs_data_array_release(old);
	obs_data_release(priv);
	pthread_mutex_unlock(&views_lock);
	return true;
}

static bool stage_views_put_if(obs_source_t *scene, const struct stage_view *v, uint32_t gen)
{
	(void)gen;
	return stage_views_put(scene, v, NULL);
}

static bool stage_views_delete_if(obs_source_t *scene, uint8_t slot, uint32_t gen)
{
	(void)gen;
	return stage_views_delete(scene, slot, NULL);
}
#endif

static void cam_view_make(uint8_t slot, const char *name, const struct stage_cam *pose,
			  uint8_t transition, uint32_t duration_ms, struct stage_view *v)
{
	memset(v, 0, sizeof(*v));
	v->slot = slot;
	if (name && name[0] && stage_view_name_valid(name))
		stage_strlcpy(v->name, name, sizeof(v->name));
	else
		snprintf(v->name, sizeof(v->name), "View %u", (unsigned)slot);
	v->x = pose->x;
	v->y = pose->y;
	v->zoom = pose->zoom;
	v->rot = pose->rot;
	v->transition = transition < STAGE_TRANSITION_COUNT ? transition : (uint8_t)STAGE_TRANSITION_SMOOTH;
	v->duration_ms = duration_ms <= STAGE_SET_DURATION_MAX ? duration_ms : STAGE_VIEW_DURATION_DEFAULT;
}

bool stage_cam_view_put(obs_source_t *scene, uint8_t slot, const char *name,
			const struct stage_cam *pose, uint8_t transition, uint32_t duration_ms)
{
	if (!scene || slot < STAGE_VIEW_SLOT_MIN || slot > STAGE_VIEW_SLOT_MAX || !pose)
		return false;
	struct stage_view v;
	cam_view_make(slot, name, pose, transition, duration_ms, &v);
	return stage_views_put(scene, &v, NULL);
}

bool stage_cam_view_get(obs_source_t *scene, uint8_t slot, float *x, float *y, float *zoom,
			float *rot, char *name, size_t name_cap)
{
	struct stage_view v;
	if (!scene || !stage_views_get(scene, slot, &v))
		return false;
	if (x)
		*x = v.x;
	if (y)
		*y = v.y;
	if (zoom)
		*zoom = v.zoom;
	if (rot)
		*rot = v.rot;
	if (name && name_cap)
		stage_strlcpy(name, v.name, name_cap);
	return true;
}

static void do_view_save(struct stage_rig *r, const struct stage_cmd *cmd)
{
	uint8_t slot = cmd->u.view.slot;
	struct stage_cam t;
	stage_rig_model_target(&r->m, &t);
	/* The vendor resolves the pose from the published target and answers
	 * with it; mask 0 (no pose) still means "the target now". */
	const struct stage_cam_target *c = &cmd->u.view.cam;
	struct stage_cam pose = {
		(c->mask & STAGE_CAM_X) ? c->x : t.x,
		(c->mask & STAGE_CAM_Y) ? c->y : t.y,
		(c->mask & STAGE_CAM_ZOOM) ? c->zoom : t.zoom,
		(c->mask & STAGE_CAM_ROT) ? c->rot : t.rot,
	};
	/* An edit re-stores the stored pose exactly, even one saved on a larger
	 * canvas (vendor-api.md 5.1: keep_pose never changes the pose). */
	if (!cmd->u.view.keep_pose)
		stage_rig_cam_sanitize(&pose, r->m.p.canvas_w, r->m.p.canvas_h);
	uint8_t tr = cmd->u.view.has_transition ? cmd->u.view.transition
						: (uint8_t)STAGE_TRANSITION_SMOOTH;
	/* The vendor already applied the default: 0 is a stored "instant". */
	uint32_t dur = cmd->duration_ms;
	if (slot < STAGE_VIEW_SLOT_MIN || slot > STAGE_VIEW_SLOT_MAX) {
		SLOG_WARN("view_save: slot %u out of range", (unsigned)slot);
		return;
	}
	/* Stored by the UI task (scene private settings are written there). */
	struct view_job *j = da_push_back_new(r->views);
	j->op = VIEW_PUT;
	j->slot = slot;
	j->transition = tr;
	j->duration_ms = dur;
	j->gen = cmd->u.view.gen;
	j->pose = pose;
	stage_strlcpy(j->name, cmd->u.view.name, sizeof(j->name));
}

static void do_view_delete(struct stage_rig *r, const struct stage_cmd *cmd)
{
	struct view_job *j = da_push_back_new(r->views);
	j->op = VIEW_DELETE;
	j->slot = cmd->u.view.slot;
	j->gen = cmd->u.view.gen;
}

/* Every saved view by slot: one read of the stored views with the writes
 * not stored yet on top, so a next/previous pick and its apply see the same
 * set (rig->lock held). A job with a generation is stored already (or
 * overwritten since, by a newer write the read holds): the read wins. */
static void views_snapshot(const struct stage_rig *r, obs_source_t *parent,
			   struct stage_view all[STAGE_VIEW_SLOT_MAX], bool valid[STAGE_VIEW_SLOT_MAX])
{
	struct stage_view list[STAGE_VIEW_SLOT_MAX];
	struct stage_view stored[STAGE_VIEW_SLOT_MAX] = {0};
	bool stored_valid[STAGE_VIEW_SLOT_MAX] = {0};
	size_t n = parent ? stage_views_list(parent, list) : 0;
	for (size_t i = 0; i < n; i++) {
		stored[list[i].slot - 1] = list[i];
		stored_valid[list[i].slot - 1] = true;
	}
	memcpy(all, stored, sizeof(stored));
	memcpy(valid, stored_valid, sizeof(stored_valid));
	for (size_t i = 0; i < r->views.num; i++) { /* oldest first: the latest wins */
		const struct view_job *j = &r->views.array[i];
		if (j->slot < STAGE_VIEW_SLOT_MIN || j->slot > STAGE_VIEW_SLOT_MAX)
			continue;
		if (j->gen) {
			all[j->slot - 1] = stored[j->slot - 1];
			valid[j->slot - 1] = stored_valid[j->slot - 1];
			continue;
		}
		struct stage_view *v = &all[j->slot - 1];
		memset(v, 0, sizeof(*v));
		v->slot = j->slot;
		v->x = j->pose.x;
		v->y = j->pose.y;
		v->zoom = j->pose.zoom;
		v->rot = j->pose.rot;
		v->transition = j->transition;
		v->duration_ms = j->duration_ms;
		valid[j->slot - 1] = j->op == VIEW_PUT;
	}
}

static void do_view_apply(struct stage_rig *r, obs_source_t *parent, const struct stage_cmd *cmd)
{
	uint8_t slot = cmd->u.view.slot;
	struct stage_view all[STAGE_VIEW_SLOT_MAX];
	bool valid[STAGE_VIEW_SLOT_MAX];
	views_snapshot(r, parent, all, valid);
	if (cmd->u.view.step) { /* next / previous hotkey: picked here, not on its thread */
		unsigned filled = 0;
		for (int i = 0; i < STAGE_VIEW_SLOT_MAX; i++)
			if (valid[i])
				filled |= 1u << i;
		slot = stage_rig_model_view_step(&r->m, cmd->u.view.step, filled);
		if (!slot) {
			SLOG_DEBUG("view_apply: no saved views");
			return;
		}
	}
	bool have = slot >= STAGE_VIEW_SLOT_MIN && slot <= STAGE_VIEW_SLOT_MAX && valid[slot - 1];
	struct stage_view v = have ? all[slot - 1] : (struct stage_view){0};
	const struct stage_cam_target *c = &cmd->u.view.cam;
	struct stage_cam pose;
	if (c->mask == (STAGE_CAM_X | STAGE_CAM_Y | STAGE_CAM_ZOOM | STAGE_CAM_ROT)) {
		pose = (struct stage_cam){c->x, c->y, c->zoom, c->rot}; /* resolved by the caller */
	} else if (have) {
		pose = (struct stage_cam){v.x, v.y, v.zoom, v.rot};
	} else {
		SLOG_DEBUG("view_apply: slot %u is empty", (unsigned)slot);
		return;
	}
	/* The view's own transition and time unless overridden; duration_ms 0
	 * means "not given" (a cut is chosen with the cut transition). */
	unsigned tr = cmd->u.view.has_transition ? cmd->u.view.transition
		      : have                     ? v.transition
						 : (unsigned)STAGE_TRANSITION_SMOOTH;
	uint32_t dur = cmd->duration_ms ? cmd->duration_ms
		       : have          ? v.duration_ms
				       : STAGE_VIEW_DURATION_DEFAULT;
	stage_rig_model_view_apply(&r->m, slot, &pose, tr, dur);
}

/* ------------------------------------------------------------------------- */
/* Commands                                                                  */
/* ------------------------------------------------------------------------- */

struct cmd_ctx {
	struct stage_rig *r;
	obs_source_t *parent;
	float char_depth, far_bg_depth;
};

static void layer_depth_refs(struct stage_rig *r, float *char_depth, float *far_bg)
{
	*char_depth = NAN;
	*far_bg = NAN;
	for (size_t i = 0; i < r->layers.num; i++) {
		const struct rig_layer *L = &r->layers.array[i];
		if (!L->enrolled)
			continue;
		if (L->role == STAGE_ROLE_CHAR && isnan(*char_depth))
			*char_depth = L->depth;
		if (L->role == STAGE_ROLE_BG && (isnan(*far_bg) || L->depth > *far_bg))
			*far_bg = L->depth;
	}
}

static void on_cmd(const struct stage_cmd *cmd, void *ud)
{
	struct cmd_ctx *c = ud;
	struct stage_rig *r = c->r;
	struct stage_rig_model *m = &r->m;
	switch (cmd->type) {
	case STAGE_CMD_CAMERA_MOVE: {
		struct stage_move_def def;
		if (!stage_cam_moves_find(cmd->u.move.move, &def)) {
			SLOG_WARN("camera_move: unknown move '%s'", cmd->u.move.move);
			break;
		}
		stage_rig_model_move(m, &def, cmd->id, cmd->duration_ms, cmd->intensity, cmd->ease);
		break;
	}
	case STAGE_CMD_CAMERA_SET:
		stage_rig_model_set(m, &cmd->u.set, cmd->duration_ms, cmd->ease);
		break;
	case STAGE_CMD_CAMERA_RESET:
		stage_rig_model_reset(m, cmd->duration_ms, cmd->ease);
		break;
	case STAGE_CMD_CAMERA_AUTO:
		stage_rig_model_auto(m, cmd->u.autom.mode, cmd->intensity);
		break;
	case STAGE_CMD_LAYER_SET:
		do_layer_set(r, cmd);
		layer_depth_refs(r, &c->char_depth, &c->far_bg_depth);
		break;
	case STAGE_CMD_DOF_SET:
		stage_rig_model_dof(m, cmd->u.dof.mask, cmd->u.dof.focus_depth, cmd->u.dof.amount,
				    cmd->duration_ms, cmd->ease, c->char_depth, c->far_bg_depth,
				    (float)r->set.dof_amount / 100.0f);
		break;
	case STAGE_CMD_VIEW_SAVE:
		do_view_save(r, cmd);
		break;
	case STAGE_CMD_VIEW_APPLY:
		do_view_apply(r, c->parent, cmd);
		break;
	case STAGE_CMD_VIEW_DELETE:
		do_view_delete(r, cmd);
		break;
	case STAGE_CMD_FREEZE:
		stage_rig_model_freeze(m, cmd->u.freeze.mode);
		break;
	case STAGE_CMD_PANIC:
		stage_rig_model_panic(m);
		break;
	default:
		break;
	}
}

/* Registry op: graphics thread only (stage_rig_apply). */
static void op_apply_cmd(void *ud, const struct stage_cmd *cmd)
{
	struct stage_rig *r = ud;
	pthread_mutex_lock(&r->lock);
	obs_source_t *parent = obs_weak_source_get_source(r->parent);
	if (parent && !r->destroyed) {
		struct cmd_ctx c = {r, parent, NAN, NAN};
		layer_depth_refs(r, &c.char_depth, &c.far_bg_depth);
		on_cmd(cmd, &c);
	}
	obs_source_release(parent);
	pthread_mutex_unlock(&r->lock);
}

static void op_get_state(void *ud, struct stage_state *out)
{
	struct stage_rig *r = ud;
	stage_state_read(&r->state, out, NULL);
}

static size_t op_list_layers(void *ud, struct stage_layer_info *out, size_t cap)
{
	return rig_snapshot(ud, out, cap);
}

static const struct stage_rig_ops rig_ops = {
	.get_state = op_get_state,
	.list_layers = op_list_layers,
	.apply_cmd = op_apply_cmd,
};

/* ------------------------------------------------------------------------- */
/* UI tasks                                                                  */
/* ------------------------------------------------------------------------- */

static void write_rest(obs_data_t *priv, const struct stage_xform *x)
{
	obs_data_t *rest = obs_data_get_obj(priv, STAGE_ITEM_KEY_REST);
	bool created = !rest;
	if (created)
		rest = obs_data_create();
	obs_data_set_double(rest, STAGE_REST_KEY_PX, x->px);
	obs_data_set_double(rest, STAGE_REST_KEY_PY, x->py);
	obs_data_set_double(rest, STAGE_REST_KEY_SX, x->sx);
	obs_data_set_double(rest, STAGE_REST_KEY_SY, x->sy);
	obs_data_set_double(rest, STAGE_REST_KEY_ROT, x->rot);
	if (created)
		obs_data_set_obj(priv, STAGE_ITEM_KEY_REST, rest);
	obs_data_release(rest);
}

static void run_persist(const struct persist_job *j)
{
	obs_data_t *priv = obs_sceneitem_get_private_settings(j->item);
	if (!priv)
		return;
	switch (j->kind) {
	case PERSIST_UNENROL:
		obs_data_erase(priv, STAGE_ITEM_KEY_DEPTH);
		obs_data_erase(priv, STAGE_ITEM_KEY_ROLE);
		obs_data_erase(priv, STAGE_ITEM_KEY_REST);
		break;
	case PERSIST_ENROL:
		obs_data_set_double(priv, STAGE_ITEM_KEY_DEPTH, j->depth);
		obs_data_set_string(priv, STAGE_ITEM_KEY_ROLE,
				    stage_role_names[j->role < STAGE_ROLE_COUNT ? j->role : 0]);
		if (j->has_rest)
			write_rest(priv, &j->rest);
		break;
	case PERSIST_REST:
		if (j->has_rest)
			write_rest(priv, &j->rest);
		break;
	}
	obs_data_release(priv);
}

static void run_blur(const struct blur_job *j, const char *scene_uuid)
{
	obs_source_t *f = obs_weak_source_get_source(j->filter);
	if (!f)
		return;
	obs_data_t *cur = obs_source_get_settings(f);
	const char *owner = obs_data_get_string(cur, STAGE_DEPTH_BLUR_KEY_DRIVEN_BY);
	bool mine = owner && strcmp(owner, scene_uuid) == 0;
	obs_data_t *upd = NULL;
	if (j->op == BLUR_START) {
		upd = obs_data_create();
		obs_data_set_string(upd, STAGE_DEPTH_BLUR_KEY_DRIVEN_BY, scene_uuid);
		obs_data_set_double(upd, STAGE_DEPTH_BLUR_KEY_AUTO_STRENGTH, j->strength);
	} else if (j->op == BLUR_UPDATE && mine) {
		upd = obs_data_create();
		obs_data_set_double(upd, STAGE_DEPTH_BLUR_KEY_AUTO_STRENGTH, j->strength);
	} else if (j->op == BLUR_STOP && mine) {
		upd = obs_data_create();
		obs_data_set_string(upd, STAGE_DEPTH_BLUR_KEY_DRIVEN_BY, "");
		obs_data_set_double(upd, STAGE_DEPTH_BLUR_KEY_AUTO_STRENGTH, 0.0);
	}
	if (upd) {
		obs_source_update(f, upd);
		obs_data_release(upd);
	}
	obs_data_release(cur);
	obs_source_release(f);
}

static void rig_write_status_ui(obs_source_t *self, const char *key, const char *a1, const char *a2)
{
	char text[512];
	if (key && key[0])
		rig_format(text, sizeof(text), key, a1, a2);
	else
		text[0] = '\0';
	obs_data_t *s = obs_source_get_settings(self);
	obs_data_set_string(s, RS_STATUS, text);
	obs_data_release(s);
}

/* A vendor write (j->gen) is stored already: its re-store is skipped when
 * a newer write (the panel's Save view) landed since, so it never brings
 * back an older view or deletes a newer one. */
static void run_view(const struct view_job *j, obs_source_t *scene)
{
	if (j->op == VIEW_PUT) {
		struct stage_view v;
		cam_view_make(j->slot, j->name, &j->pose, j->transition, j->duration_ms, &v);
		if (!stage_views_put_if(scene, &v, j->gen) && !j->gen)
			SLOG_WARN("view_save: slot %u could not be stored", (unsigned)j->slot);
	} else {
		stage_views_delete_if(scene, j->slot, j->gen);
	}
}

static void rig_ui_task(void *param)
{
	struct stage_rig *r = param;
	pthread_mutex_lock(&r->lock);
	uint32_t bits = r->ui_bits;
	r->ui_bits = 0;
	r->ui_queued = false;
	DARRAY(struct persist_job) persist = {0};
	DARRAY(struct blur_job) blur = {0};
	DARRAY(struct view_job) views = {0};
	persist.da = r->persist.da;
	blur.da = r->blur.da;
	views.da = r->views.da;
	da_init(r->persist);
	da_init(r->blur);
	da_init(r->views);
	char key[48], a1[200], a2[32], uuid[STAGE_SCENE_UUID_MAX];
	stage_strlcpy(key, r->status_key, sizeof(key));
	stage_strlcpy(a1, r->status_a1, sizeof(a1));
	stage_strlcpy(a2, r->status_a2, sizeof(a2));
	stage_strlcpy(uuid, r->scene_uuid, sizeof(uuid));
	obs_source_t *scene = views.num ? obs_weak_source_get_source(r->parent) : NULL;
	pthread_mutex_unlock(&r->lock);

	for (size_t i = 0; i < persist.num; i++) {
		run_persist(&persist.array[i]);
		obs_sceneitem_release(persist.array[i].item);
	}
	for (size_t i = 0; i < blur.num; i++) {
		run_blur(&blur.array[i], uuid);
		obs_weak_source_release(blur.array[i].filter);
	}
	for (size_t i = 0; i < views.num && scene; i++)
		run_view(&views.array[i], scene);
	obs_source_release(scene);
	da_free(persist);
	da_free(blur);
	da_free(views);

	/* The filter itself through its weak reference: it may have been
	 * destroyed since this task was queued (NULL then). */
	obs_source_t *self = obs_weak_source_get_source(r->self);
	if (self) {
		if (bits & RIG_UI_SETTINGS)
			rig_sync_settings_ui(r, self);
		if (bits & RIG_UI_STATUS)
			rig_write_status_ui(self, key, a1, a2);
		if (bits & (RIG_UI_SETTINGS | RIG_UI_STATUS | RIG_UI_PROPS))
			obs_source_update_properties(self);
		obs_source_release(self);
	}
	rig_release(r);
}

/* Queues the UI task when work is pending and none is queued. Must be
 * called WITHOUT rig->lock held. */
static void rig_kick_ui(struct stage_rig *r)
{
	pthread_mutex_lock(&r->lock);
	bool work = r->ui_bits || r->persist.num || r->blur.num || r->views.num;
	bool queue = work && !r->ui_queued;
	if (queue) {
		r->ui_queued = true;
		rig_addref(r);
	}
	pthread_mutex_unlock(&r->lock);
	if (queue)
		obs_queue_task(OBS_TASK_UI, rig_ui_task, r, false);
}

/* Panel values that follow the model (UX: every control is a view of the
 * rig's state, so panel, hotkeys, docks and recipes never disagree). */
void rig_sync_settings_ui(struct stage_rig *r, obs_source_t *self)
{
	pthread_mutex_lock(&r->lock);
	struct stage_rig_model *m = &r->m;
	int mode = m->auto_mode;
	long long inten = (long long)lroundf(m->auto_intensity * 100.0f);
	bool frozen = m->frozen;
	bool dof_on = m->amount > 0.0f;
	long long amount = dof_on ? (long long)lroundf(m->amount * 100.0f) : r->set.dof_amount;
	long long focus = (long long)lroundf(stage_ui_from_depth(m->focus));
	bool follow = m->follow_char;
	r->set.auto_mode = mode;
	r->set.auto_intensity = inten;
	r->set.freeze = frozen;
	r->set.dof_enabled = dof_on;
	r->set.dof_amount = amount;
	r->set.dof_focus = focus;
	r->set.dof_follow = follow;
	pthread_mutex_unlock(&r->lock);

	obs_data_t *s = obs_source_get_settings(self);
	obs_data_set_string(s, RS_AUTO_MODE, stage_auto_mode_names[mode % STAGE_AUTO_COUNT]);
	obs_data_set_int(s, RS_AUTO_INTENSITY, inten);
	obs_data_set_bool(s, RS_FREEZE, frozen);
	obs_data_set_bool(s, RS_DOF_ENABLED, dof_on);
	obs_data_set_int(s, RS_DOF_AMOUNT, amount);
	obs_data_set_int(s, RS_DOF_FOCUS, focus);
	obs_data_set_bool(s, RS_DOF_FOLLOW, follow);
	obs_data_release(s);
}

/* ------------------------------------------------------------------------- */
/* Audio (volmeter on the audio thread: atomics only)                        */
/* ------------------------------------------------------------------------- */

union fbits {
	float f;
	long l;
};

static void vm_cb(void *param, const float magnitude[MAX_AUDIO_CHANNELS],
		  const float peak[MAX_AUDIO_CHANNELS], const float input_peak[MAX_AUDIO_CHANNELS])
{
	(void)magnitude;
	(void)input_peak;
	struct stage_rig *r = param;
	float db = -INFINITY;
	for (int i = 0; i < MAX_AUDIO_CHANNELS; i++)
		if (peak[i] > db)
			db = peak[i];
	union fbits u;
	u.f = db;
	os_atomic_set_long(&r->audio_db_bits, u.l);
	os_atomic_inc_long(&r->audio_count);
}

/* The audio source is being removed or destroyed: flag only (the signal
 * holds the source's signal mutex, so no lock may be taken here). The tick
 * detaches; the volmeter itself also lets go of the source on "destroy". */
static void audio_src_gone(void *data, calldata_t *cd)
{
	(void)cd;
	struct stage_rig *r = data;
	os_atomic_set_long(&r->audio_detached, 1);
}

static const char *const audio_signals[] = {"remove", "destroy"};

/* audio_lock held. The source's signal handler was connected with
 * signal_handler_connect_ref, so it stays valid until this disconnect even
 * when the source itself is already being destroyed (its weak reference
 * then yields NULL). */
static void audio_detach_locked(struct stage_rig *r)
{
	if (r->vm) {
		obs_volmeter_remove_callback(r->vm, vm_cb, r);
		obs_volmeter_detach_source(r->vm);
		obs_volmeter_destroy(r->vm);
		r->vm = NULL;
	}
	if (r->audio_sh) {
		for (size_t i = 0; i < sizeof(audio_signals) / sizeof(audio_signals[0]); i++)
			signal_handler_disconnect(r->audio_sh, audio_signals[i], audio_src_gone, r);
		r->audio_sh = NULL;
	}
	os_atomic_set_long(&r->audio_detached, 0);
}

static void audio_set_source(struct stage_rig *r, const char *uuid)
{
	pthread_mutex_lock(&r->audio_lock);
	audio_detach_locked(r);
	obs_source_t *src = (uuid && uuid[0]) ? obs_get_source_by_uuid(uuid) : NULL;
	if (src && (obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) && !obs_source_removed(src)) {
		r->vm = obs_volmeter_create(OBS_FADER_LOG);
		obs_volmeter_add_callback(r->vm, vm_cb, r);
		if (!obs_volmeter_attach_source(r->vm, src)) {
			obs_volmeter_remove_callback(r->vm, vm_cb, r);
			obs_volmeter_destroy(r->vm);
			r->vm = NULL;
		} else {
			r->audio_sh = obs_source_get_signal_handler(src);
			for (size_t i = 0; i < sizeof(audio_signals) / sizeof(audio_signals[0]); i++)
				signal_handler_connect_ref(r->audio_sh, audio_signals[i], audio_src_gone, r);
		}
	}
	obs_source_release(src);
	os_atomic_set_long(&r->audio_count, 0);
	r->audio_seen_count = 0;
	r->audio_quiet_s = 10.0f;
	pthread_mutex_unlock(&r->audio_lock);
}

/* Latest audio peak in dBFS, or NaN when no source is attached. */
static float audio_read(struct stage_rig *r, float dt)
{
	if (os_atomic_load_long(&r->audio_detached)) {
		pthread_mutex_lock(&r->audio_lock);
		audio_detach_locked(r);
		pthread_mutex_unlock(&r->audio_lock);
	}
	if (!r->vm)
		return NAN;
	long n = os_atomic_load_long(&r->audio_count);
	if (n != r->audio_seen_count) {
		r->audio_seen_count = n;
		r->audio_quiet_s = 0.0f;
	} else {
		r->audio_quiet_s += dt;
	}
	if (r->audio_quiet_s > 0.5f) /* no audio packets: silence */
		return STAGE_DB_FLOOR;
	union fbits u;
	u.l = os_atomic_load_long(&r->audio_db_bits);
	return isfinite(u.f) ? u.f : STAGE_DB_FLOOR;
}

/* Attaches the selected audio source when it is not attached: a source
 * saved after the scene does not exist yet when libobs creates this filter
 * on load, and one may also come back later (undo of a delete). */
static void audio_retry(struct stage_rig *r)
{
	char uuid[STAGE_SCENE_UUID_MAX];
	pthread_mutex_lock(&r->lock);
	stage_strlcpy(uuid, r->set.audio_uuid, sizeof(uuid));
	pthread_mutex_unlock(&r->lock);
	if (!uuid[0])
		return;
	pthread_mutex_lock(&r->audio_lock);
	bool attached = r->vm && !os_atomic_load_long(&r->audio_detached);
	pthread_mutex_unlock(&r->audio_lock);
	if (!attached)
		audio_set_source(r, uuid);
}

/* ------------------------------------------------------------------------- */
/* Parent scene                                                              */
/* ------------------------------------------------------------------------- */

static void parent_signal(void *data, calldata_t *cd)
{
	(void)cd;
	struct stage_rig *r = data;
	os_atomic_set_long(&r->dirty, 1);
}

static const char *const parent_signals[] = {"item_add", "item_remove", "reorder", "refresh"};

/* Connected with signal_handler_connect_ref and disconnected through the
 * stored handler: while a scene is destroyed with the rig still attached,
 * its refcount is spent (the weak reference yields NULL) yet it still emits
 * item_remove for every item after this filter is gone. */
static void parent_connect(struct stage_rig *r, obs_source_t *parent)
{
	if (r->parent_sh)
		return;
	r->parent_sh = obs_source_get_signal_handler(parent);
	for (size_t i = 0; i < sizeof(parent_signals) / sizeof(parent_signals[0]); i++)
		signal_handler_connect_ref(r->parent_sh, parent_signals[i], parent_signal, r);
}

static void parent_disconnect(struct stage_rig *r)
{
	if (!r->parent_sh)
		return;
	for (size_t i = 0; i < sizeof(parent_signals) / sizeof(parent_signals[0]); i++)
		signal_handler_disconnect(r->parent_sh, parent_signals[i], parent_signal, r);
	r->parent_sh = NULL;
}

/* rig->lock held. */
static void attach_parent_locked(struct stage_rig *r, obs_source_t *parent)
{
	obs_weak_source_t *w = obs_source_get_weak_source(parent);
	if (r->parent && obs_weak_source_references_source(r->parent, parent)) {
		obs_weak_source_release(w);
	} else {
		parent_disconnect(r);
		obs_weak_source_release(r->parent);
		r->parent = w;
	}
	stage_strlcpy(r->scene_uuid, obs_source_get_uuid(parent), sizeof(r->scene_uuid));
	r->is_scene = obs_scene_from_source(parent) != NULL;
	if (r->is_scene)
		parent_connect(r, parent);
	os_atomic_set_long(&r->dirty, 1);
}

/* ------------------------------------------------------------------------- */
/* Tick                                                                      */
/* ------------------------------------------------------------------------- */

static void publish_state(struct stage_rig *r, bool active)
{
	struct stage_state s;
	memset(&s, 0, sizeof(s));
	stage_strlcpy(s.scene_uuid, r->scene_uuid, sizeof(s.scene_uuid));
	s.enabled = active;
	const struct stage_rig_model *m = &r->m;
	s.x = m->out.x;
	s.y = m->out.y;
	s.zoom = m->out.zoom;
	s.rot = m->out.rot;
	struct stage_cam t;
	stage_rig_model_target(m, &t);
	s.tx = t.x;
	s.ty = t.y;
	s.tzoom = t.zoom;
	s.trot = t.rot;
	s.shake = m->out_shake;
	s.auto_mode = m->auto_mode;
	s.auto_intensity = m->auto_intensity;
	stage_strlcpy(s.fx_preset, r->fx_preset, sizeof(s.fx_preset));
	s.focus_depth = m->focus;
	s.dof_amount = m->amount;
	s.moving = stage_rig_model_moving(m);
	if (m->move_active) {
		stage_strlcpy(s.move, m->move.id, sizeof(s.move));
		s.move_id = m->move_cmd_id;
	}
	s.last_move_id = m->move_cmd_id;
	stage_strlcpy(s.last_move, m->move.id, sizeof(s.last_move));
	s.last_move_done = m->move_done;
	s.frozen = m->frozen;
	s.view = m->view;
	stage_state_publish(&r->state, &s);
}

/* Effective overscan of a layer (fraction) and the centre it grows about.
 * bg only: the user's Overscan, raised under Live-safe to the computed
 * minimum for the camera range (Max pan, zoom 1..Max zoom, +-8 deg roll). */
static void layer_overscan(struct stage_rig *r, struct rig_layer *L)
{
	float W = r->m.p.canvas_w, H = r->m.p.canvas_h;
	L->ocx = W * 0.5f;
	L->ocy = H * 0.5f;
	L->o_eff = 0.0f;
	L->rect_valid = false;
	if (L->role != STAGE_ROLE_BG)
		return;
	L->rect_valid = layer_rest_rect(L, &L->cur, &L->rect) &&
			fabsf(stage_wrap_deg(L->rest.rot)) < 0.5f;
	float o = clampf(r->set.overscan, 0.0f, OVERSCAN_MAX);
	if (L->rect_valid) {
		L->ocx = (L->rect.l + L->rect.r) * 0.5f;
		L->ocy = (L->rect.t + L->rect.b) * 0.5f;
		if (r->set.live_safe) {
			float f = role_f(L);
			float key[5] = {r->m.p.max_pan * W, r->m.p.max_pan * H, r->m.p.max_zoom, W, H};
			if (memcmp(&L->o_rect, &L->rect, sizeof(L->rect)) != 0 || L->o_f != f ||
			    memcmp(L->o_key, key, sizeof(key)) != 0) {
				L->o_rect = L->rect;
				L->o_f = f;
				memcpy(L->o_key, key, sizeof(key));
				L->o_min = 0.0f;
				/* Only a layer that covers the canvas at rest can
				 * keep covering it; others are not oversized. */
				if (stage_cover_margin(&L->rect, f, &cam_home, W, H) >= -0.5f) {
					struct stage_cover_range range = {
						key[0], key[1], 1.0f, key[2],
						(float)STAGE_LIVE_SAFE_MAX_ROLL};
					L->o_min = stage_cover_min_oversize_range(&L->rect, f, &range,
										  W, H);
				}
			}
			o = fmaxf(o, L->o_min);
		}
	}
	L->o_eff = clampf(o, 0.0f, OVERSCAN_MAX);
}

/* First sight of a layer whose rest came from stage.rest (load, undo, a
 * re-enabled rig): a loaded transform that is neither the rest nor the rest
 * with the background overscan at home was set while no rig applied it (rig
 * disabled or inactive, plugin not loaded), so it is the user's and becomes
 * the rest. L->cur holds the item's transform. */
static void layer_check_loaded(struct stage_rig *r, struct rig_layer *L)
{
	L->fresh = false;
	L->bounds = L->cur.bounds_type != OBS_BOUNDS_NONE; /* not stored */
	struct stage_xform cx;
	xform_from_info(&L->cur, &cx);
	if (!xform_differs(&cx, &L->rest, L->bounds, EDIT_TOL_PX, EDIT_TOL_SCALE, EDIT_TOL_ROT))
		return;
	if (L->role == STAGE_ROLE_BG) {
		layer_overscan(r, L);
		struct stage_xform ro;
		oversize(&L->rest, L->o_eff, L->ocx, L->ocy, &ro);
		if (!xform_differs(&cx, &ro, L->bounds, EDIT_TOL_PX, EDIT_TOL_SCALE, EDIT_TOL_ROT))
			return;
	}
	SLOG_DEBUG("layer '%s' was moved while the rig was not applying: new rest pose", L->name);
	L->rest = cx;
	L->rest_dirty = true;
}

static void tick_active(struct stage_rig *r, obs_source_t *parent, float dt)
{
	obs_scene_t *scene = obs_scene_from_source(parent);
	struct stage_rig_model *m = &r->m;
	float W = (float)obs_source_get_width(parent), H = (float)obs_source_get_height(parent);
	if (W > 0.0f && H > 0.0f) {
		m->p.canvas_w = W;
		m->p.canvas_h = H;
	}
	W = m->p.canvas_w;
	H = m->p.canvas_h;

	r->rebuild_timer += dt;
	if (os_atomic_set_long(&r->dirty, 0) || r->rebuild_timer >= REBUILD_PERIOD_S || !r->was_active) {
		r->rebuild_timer = 0.0f;
		rebuild_layers(r, scene);
	}

	/* Commands for this scene, then hotkey requests. */
	struct cmd_ctx cc = {r, parent, NAN, NAN};
	layer_depth_refs(r, &cc.char_depth, &cc.far_bg_depth);
	stage_cmdq_drain_types(r->scene_uuid, STAGE_CMD_MASK_RIG, on_cmd, &cc);
	long punch = os_atomic_set_long(&r->punch_req, 0);
	if (punch & 1)
		stage_rig_model_punch(m, true);
	if (punch & 2)
		stage_rig_model_punch(m, false);
	if (os_atomic_set_long(&r->bake_req, 0)) {
		stage_rig_model_snap_home(m);
		restore_all_locked(r);
	}

	/* 1. Rest-pose rule: a transform that differs from the one applied last
	 * was set by the user, so it defines the new rest pose. */
	for (size_t i = 0; i < r->layers.num; i++) {
		struct rig_layer *L = &r->layers.array[i];
		if (!L->enrolled || !L->has_rest)
			continue;
		obs_sceneitem_get_info2(L->item, &L->cur);
		if (L->fresh)
			layer_check_loaded(r, L);
		if (!L->applied)
			continue;
		bool bounds_now = L->cur.bounds_type != OBS_BOUNDS_NONE;
		struct stage_xform cx;
		xform_from_info(&L->cur, &cx);
		if (bounds_now != L->bounds ||
		    xform_differs(&cx, &L->applied_x, L->bounds, EDIT_TOL_PX, EDIT_TOL_SCALE, EDIT_TOL_ROT)) {
			struct stage_xform un;
			stage_unsolve_layer_f(&cx, L->applied_f, &L->applied_cam, W, H, &un);
			undersize(&un, L->applied_o, L->applied_ocx, L->applied_ocy, &L->rest);
			L->bounds = bounds_now;
			L->rest_dirty = true;
			L->applied_x = cx; /* what the item shows now */
		}
	}

	/* 2. Overscan and the covering (bg) layers for the Live-safe clamp. */
	struct stage_rig_cover covers[STAGE_COVER_MAX_LAYERS];
	size_t ncov = 0;
	for (size_t i = 0; i < r->layers.num; i++) {
		struct rig_layer *L = &r->layers.array[i];
		if (!L->enrolled || !L->has_rest)
			continue;
		layer_overscan(r, L);
		if (L->rect_valid && L->visible && ncov < STAGE_COVER_MAX_LAYERS) {
			float k = 1.0f + L->o_eff;
			covers[ncov].rect.l = L->ocx + (L->rect.l - L->ocx) * k;
			covers[ncov].rect.r = L->ocx + (L->rect.r - L->ocx) * k;
			covers[ncov].rect.t = L->ocy + (L->rect.t - L->ocy) * k;
			covers[ncov].rect.b = L->ocy + (L->rect.b - L->ocy) * k;
			covers[ncov].f = role_f(L);
			ncov++;
		}
	}

	/* 3. Motion. */
	struct stage_rig_inputs in = {0};
	in.dt = dt;
	in.audio_db = audio_read(r, dt);
	layer_depth_refs(r, &in.char_depth, &in.far_bg_depth);
	in.dof_slider = (float)r->set.dof_amount / 100.0f;
	in.covers = covers;
	in.ncovers = ncov;
	stage_rig_model_step(m, &in);
	const struct stage_cam cam = m->out;

	/* 4. Solve and apply (only real changes: each set_info2 is an
	 * item_transform signal and an obs-websocket event). */
	for (size_t i = 0; i < r->layers.num; i++) {
		struct rig_layer *L = &r->layers.array[i];
		if (!L->enrolled || !L->has_rest)
			continue;
		if ((L->cur.bounds_type != OBS_BOUNDS_NONE) != L->bounds)
			continue;
		float f = role_f(L);
		struct stage_xform ro, out;
		oversize(&L->rest, L->o_eff, L->ocx, L->ocy, &ro);
		stage_solve_layer_f(&ro, f, &cam, W, H, &out);
		if (L->applied && !xform_differs(&out, &L->applied_x, L->bounds, APPLY_EPS_PX,
						 APPLY_EPS_SCALE, APPLY_EPS_ROT))
			continue;
		struct obs_transform_info info = L->cur;
		xform_to_info(&out, L->bounds, &info);
		obs_sceneitem_set_info2(L->item, &info);
		L->applied = true;
		L->applied_x = out;
		L->applied_cam = cam;
		L->applied_f = f;
		L->applied_o = L->o_eff;
		L->applied_ocx = L->ocx;
		L->applied_ocy = L->ocy;
	}

	/* 5. Persist new rest poses; drive depth blur (stage-plugin-ux.md 3). */
	for (size_t i = 0; i < r->layers.num; i++) {
		struct rig_layer *L = &r->layers.array[i];
		if (L->enrolled && L->rest_dirty)
			queue_persist(r, L, PERSIST_REST);
		bool want = L->enrolled && m->amount > 0.0f && role_blurs(L->role) && L->blur &&
			    !obs_weak_source_expired(L->blur);
		if (!want) {
			stop_driving(r, L);
			continue;
		}
		float s = stage_dof_strength(L->role == STAGE_ROLE_SKY ? (float)STAGE_DEPTH_MAX : L->depth,
					     m->focus, m->amount);
		if (!L->driving) {
			queue_blur(r, L->blur, BLUR_START, s);
			L->driving = true;
			L->blur_written = s;
		} else if (fabsf(s - L->blur_written) > BLUR_EPS ||
			   (s == 0.0f && L->blur_written != 0.0f)) {
			queue_blur(r, L->blur, BLUR_UPDATE, s);
			L->blur_written = s;
		}
	}

	if (m->changed) {
		m->changed = 0;
		r->ui_bits |= RIG_UI_SETTINGS;
	}
}

static void rig_tick(void *data, float seconds)
{
	struct stage_rig *r = data;
	if (r->destroyed || r->removed)
		return;
	obs_source_t *parent = obs_filter_get_parent(r->source);
	if (!parent)
		return;

	pthread_mutex_lock(&r->lock);
	if (!r->parent || !obs_weak_source_references_source(r->parent, parent))
		attach_parent_locked(r, parent);
	char uuid[STAGE_SCENE_UUID_MAX];
	stage_strlcpy(uuid, r->scene_uuid, sizeof(uuid));
	bool want = obs_source_enabled(r->source) && r->is_scene && !r->suspended;
	bool registered = r->registered;
	pthread_mutex_unlock(&r->lock);

	/* Registry calls happen without rig->lock (lock order). */
	if (want && !registered) {
		uint64_t now = stage_core_now_ms();
		if (now >= r->next_register_ms) {
			registered = stage_rig_register(uuid, &rig_ops, r);
			if (!registered) {
				r->next_register_ms = now + REGISTER_RETRY_MS;
				if (!r->second) {
					r->second = true;
					if (!r->warned_second) {
						r->warned_second = true;
						SLOG_WARN("scene '%s' already has a Parallaxa Camera Rig; this one stays inactive",
							  obs_source_get_name(parent));
					}
					rig_set_status(r, "Stage.Info.SecondRig", NULL, NULL,
						       OBS_TEXT_INFO_WARNING);
				}
			} else if (r->second) {
				r->second = false;
				rig_set_status(r, "", NULL, NULL, OBS_TEXT_INFO_NORMAL);
			}
		}
	} else if (!want && registered) {
		stage_rig_unregister(uuid, r);
		registered = false;
	}
	r->audio_retry_s += seconds;
	if (r->audio_retry_s >= AUDIO_RETRY_S) {
		r->audio_retry_s = 0.0f;
		audio_retry(r);
	}
	r->fx_timer += seconds;
	if (r->fx_timer >= FX_POLL_S) {
		r->fx_timer = 0.0f;
		struct stage_fx_info fi;
		char preset[STAGE_ID_MAX] = "";
		if (stage_fx_get_info(uuid, &fi))
			stage_strlcpy(preset, fi.preset, sizeof(preset));
		pthread_mutex_lock(&r->lock);
		stage_strlcpy(r->fx_preset, preset, sizeof(r->fx_preset));
		pthread_mutex_unlock(&r->lock);
	}
	uint64_t gen = stage_panic_generation();

	pthread_mutex_lock(&r->lock);
	r->registered = registered;
	bool active = registered && want && !r->destroyed && !r->removed;
	if (gen != r->panic_gen) {
		r->panic_gen = gen;
		if (active)
			stage_rig_model_panic(&r->m);
	}
	if (active) {
		uint64_t t0 = os_gettime_ns();
		tick_active(r, parent, seconds);
		uint64_t ns = os_gettime_ns() - t0;
		r->perf_sum_ns += ns;
		if (ns > r->perf_max_ns)
			r->perf_max_ns = ns;
		if (++r->perf_n >= RIG_PERF_TICKS) {
			SLOG_DEBUG("rig '%s': tick %.1f us avg, %.1f us max, %zu items",
				   obs_source_get_name(parent),
				   (double)r->perf_sum_ns / r->perf_n / 1000.0,
				   (double)r->perf_max_ns / 1000.0, r->layers.num);
			r->perf_sum_ns = r->perf_max_ns = 0;
			r->perf_n = 0;
		}
	} else if (r->was_active) {
		/* Disabled, second rig or suspended: items back to rest. */
		restore_all_locked(r);
		r->m.changed = 0;
	}
	r->was_active = active;
	publish_state(r, active);
	pthread_mutex_unlock(&r->lock);
	rig_kick_ui(r);
}

static void rig_render(void *data, gs_effect_t *effect)
{
	(void)effect;
	struct stage_rig *r = data;
	obs_source_skip_video_filter(r->source);
}

/* ------------------------------------------------------------------------- */
/* Settings                                                                  */
/* ------------------------------------------------------------------------- */

static void read_settings(obs_data_t *s, struct rig_settings *o)
{
	memset(o, 0, sizeof(*o));
	o->live_safe = obs_data_get_bool(s, RS_LIVE_SAFE);
	o->smoothing_ms = (float)obs_data_get_int(s, RS_SMOOTHING);
	o->overscan = (float)obs_data_get_int(s, RS_OVERSCAN) / 100.0f;
	o->max_zoom = (float)obs_data_get_double(s, RS_MAX_ZOOM);
	o->max_pan = (float)obs_data_get_int(s, RS_MAX_PAN) / 100.0f;
	o->motion_scale = (float)obs_data_get_int(s, RS_MOTION_SCALE) / 100.0f;
	o->manual_pause_s = (float)obs_data_get_int(s, RS_MANUAL_PAUSE);
	o->audio_amount = (float)obs_data_get_int(s, RS_AUDIO_AMOUNT) / 100.0f;
	o->audio_attack = (float)obs_data_get_int(s, RS_AUDIO_ATTACK);
	o->audio_release = (float)obs_data_get_int(s, RS_AUDIO_RELEASE);
	stage_strlcpy(o->audio_uuid, obs_data_get_string(s, RS_AUDIO_SOURCE), sizeof(o->audio_uuid));
	int mode = stage_auto_mode_from_name(obs_data_get_string(s, RS_AUTO_MODE));
	o->auto_mode = mode < 0 ? STAGE_AUTO_OFF : mode;
	o->auto_intensity = obs_data_get_int(s, RS_AUTO_INTENSITY);
	o->freeze = obs_data_get_bool(s, RS_FREEZE);
	o->cam_x = obs_data_get_double(s, RS_CAM_X);
	o->cam_y = obs_data_get_double(s, RS_CAM_Y);
	o->cam_zoom = obs_data_get_double(s, RS_CAM_ZOOM);
	o->cam_rot = obs_data_get_double(s, RS_CAM_ROT);
	o->dof_enabled = obs_data_get_bool(s, RS_DOF_ENABLED);
	o->dof_amount = obs_data_get_int(s, RS_DOF_AMOUNT);
	o->dof_focus = obs_data_get_int(s, RS_DOF_FOCUS);
	o->dof_follow = obs_data_get_bool(s, RS_DOF_FOLLOW);
}

static void apply_params(struct stage_rig *r, const struct rig_settings *n)
{
	struct stage_rig_params *p = &r->m.p;
	p->smoothing_ms = clampf(n->smoothing_ms, 0.0f, 1000.0f);
	p->live_safe = n->live_safe;
	p->max_zoom = clampf(n->max_zoom, 1.0f, (float)STAGE_ZOOM_MAX);
	p->max_pan = clampf(n->max_pan, 0.0f, 0.5f);
	p->motion_scale = clampf(n->motion_scale, 0.0f, 1.0f);
	p->manual_pause_s = clampf(n->manual_pause_s, 0.0f, 60.0f);
	p->audio_amount = clampf(n->audio_amount, 0.0f, 1.0f);
	p->audio_attack_ms = clampf(n->audio_attack, 1.0f, 500.0f);
	p->audio_release_ms = clampf(n->audio_release, 10.0f, 2000.0f);
}

/* first: values come from a load or create, not from a user edit. */
static void rig_apply_settings(struct stage_rig *r, obs_data_t *settings, bool first)
{
	struct rig_settings n;
	read_settings(settings, &n);
	bool audio_changed;

	pthread_mutex_lock(&r->lock);
	struct rig_settings o = r->set;
	struct stage_rig_model *m = &r->m;
	apply_params(r, &n);
	audio_changed = first || strcmp(o.audio_uuid, n.audio_uuid) != 0;
	if (first) {
		stage_rig_model_auto(m, (unsigned)n.auto_mode, (float)n.auto_intensity / 100.0f);
		m->follow_char = n.dof_follow;
		if (n.dof_enabled && n.dof_amount > 0)
			m->amount = (float)n.dof_amount / 100.0f;
		if (!n.dof_follow)
			m->focus = stage_depth_from_ui((float)n.dof_focus);
		m->changed = 0;
	} else {
		if (n.auto_mode != o.auto_mode || n.auto_intensity != o.auto_intensity)
			stage_rig_model_auto(m, (unsigned)n.auto_mode, (float)n.auto_intensity / 100.0f);
		if (n.freeze != o.freeze)
			stage_rig_model_freeze(m, n.freeze ? STAGE_FREEZE_ON : STAGE_FREEZE_OFF);
		struct stage_cam_target t = {0};
		if (n.cam_x != o.cam_x) {
			t.x = (float)n.cam_x;
			t.mask |= STAGE_CAM_X;
		}
		if (n.cam_y != o.cam_y) {
			t.y = (float)n.cam_y;
			t.mask |= STAGE_CAM_Y;
		}
		if (n.cam_zoom != o.cam_zoom) {
			t.zoom = (float)n.cam_zoom;
			t.mask |= STAGE_CAM_ZOOM;
		}
		if (n.cam_rot != o.cam_rot) {
			t.rot = (float)n.cam_rot;
			t.mask |= STAGE_CAM_ROT;
		}
		if (t.mask)
			stage_rig_model_set(m, &t, 0, STAGE_EASE_DEFAULT);
		/* Depth of field: the group mirrors amount > 0 (stage-plugin-ux.md section 1). */
		float cd, fb;
		layer_depth_refs(r, &cd, &fb);
		bool follow_before = m->follow_char;
		if (n.dof_enabled != o.dof_enabled || (n.dof_enabled && n.dof_amount != o.dof_amount)) {
			float a = n.dof_enabled ? (float)n.dof_amount / 100.0f : 0.0f;
			stage_rig_model_dof(m, STAGE_DOF_AMOUNT, NAN, a, 0, STAGE_EASE_DEFAULT, cd, fb,
					    (float)n.dof_amount / 100.0f);
		}
		if (n.dof_focus != o.dof_focus)
			stage_rig_model_dof(m, STAGE_DOF_FOCUS, stage_depth_from_ui((float)n.dof_focus),
					    NAN, 0, STAGE_EASE_DEFAULT, cd, fb, NAN);
		if (n.dof_follow != o.dof_follow && n.dof_follow != m->follow_char) {
			m->follow_char = n.dof_follow;
			m->changed |= STAGE_RIG_CH_DOF;
		}
		/* Values the panel itself just set need no write-back (a refresh
		 * would also interrupt a slider drag); only a "follow" flipped
		 * by a focus edit is shown. */
		m->changed &= ~(uint32_t)(STAGE_RIG_CH_AUTO | STAGE_RIG_CH_FREEZE | STAGE_RIG_CH_DOF);
		if (m->follow_char != follow_before && n.dof_follow == o.dof_follow)
			m->changed |= STAGE_RIG_CH_DOF;
	}
	r->set = n;
	pthread_mutex_unlock(&r->lock);

	if (audio_changed)
		audio_set_source(r, n.audio_uuid);
}

static void rig_update(void *data, obs_data_t *settings)
{
	rig_apply_settings(data, settings, false);
}

/* After every source of a collection exists (obs_load_sources' second
 * pass): the audio source may have been created after this filter. */
static void rig_load(void *data, obs_data_t *settings)
{
	(void)settings;
	audio_retry(data);
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

static const char *rig_get_name(void *unused)
{
	(void)unused;
	return obs_module_text("Stage.Filter.CameraRig");
}

static void *rig_create(obs_data_t *settings, obs_source_t *source)
{
	struct stage_rig *r = bzalloc(sizeof(*r));
	r->refs = 1;
	r->source = source;
	r->self = obs_source_get_weak_source(source);
	pthread_mutex_init_recursive(&r->lock);
	pthread_mutex_init(&r->snap_lock, NULL);
	pthread_mutex_init(&r->audio_lock, NULL);
	stage_state_buf_init(&r->state);
	da_init(r->layers);
	da_init(r->persist);
	da_init(r->blur);
	da_init(r->views);
	da_init(r->pending);
	r->panic_gen = stage_panic_generation();
	r->dirty = 1;
	stage_rig_model_init(&r->m, NULL);

	/* Freeze is never saved: always off after OBS starts (UX). */
	obs_data_set_bool(settings, RS_FREEZE, false);
	/* Nor is the manual position: the camera starts at home (the panel
	 * would otherwise show, say, Zoom 1.5x with the camera at 1x). */
	obs_data_set_double(settings, RS_CAM_X, 0.0);
	obs_data_set_double(settings, RS_CAM_Y, 0.0);
	obs_data_set_double(settings, RS_CAM_ZOOM, 1.0);
	obs_data_set_double(settings, RS_CAM_ROT, 0.0);
	obs_data_set_string(settings, RS_STATUS, "");
	stage_strlcpy(r->look_applied, obs_data_get_string(settings, RS_LOOK), sizeof(r->look_applied));
	rig_apply_settings(r, settings, true);

	pthread_mutex_lock(&g_rigs_lock);
	da_push_back(g_rigs, &r);
	pthread_mutex_unlock(&g_rigs_lock);
	return r;
}

static void rig_filter_add(void *data, obs_source_t *parent)
{
	struct stage_rig *r = data;
	pthread_mutex_lock(&r->lock);
	attach_parent_locked(r, parent);
	bool is_scene = r->is_scene;
	char uuid[STAGE_SCENE_UUID_MAX];
	stage_strlcpy(uuid, r->scene_uuid, sizeof(uuid));
	pthread_mutex_unlock(&r->lock);

	if (!is_scene) {
		rig_set_status(r, "Stage.Info.NotScene", NULL, NULL, OBS_TEXT_INFO_WARNING);
		return;
	}
	/* Register now so that a Look applied right away finds the rig. */
	if (obs_source_enabled(r->source) && !r->registered) {
		bool ok = stage_rig_register(uuid, &rig_ops, r);
		pthread_mutex_lock(&r->lock);
		r->registered = ok;
		pthread_mutex_unlock(&r->lock);
	}
	obs_data_t *s = obs_source_get_settings(r->source);
	bool initialized = obs_data_get_bool(s, RS_INITIALIZED);
	if (!initialized)
		obs_data_set_bool(s, RS_INITIALIZED, true);
	obs_data_release(s);
	if (!initialized && r->registered)
		rig_first_add(r, parent);
}

static void detach_all_locked(struct stage_rig *r)
{
	parent_disconnect(r);
	for (size_t i = 0; i < r->layers.num; i++)
		layer_disconnect(r, &r->layers.array[i]);
}

static void rig_filter_remove(void *data, obs_source_t *parent)
{
	(void)parent;
	struct stage_rig *r = data;
	pthread_mutex_lock(&r->lock);
	if (!r->removed) {
		restore_all_locked(r);
		r->removed = true;
		detach_all_locked(r);
		publish_state(r, false);
	}
	char uuid[STAGE_SCENE_UUID_MAX];
	stage_strlcpy(uuid, r->scene_uuid, sizeof(uuid));
	pthread_mutex_unlock(&r->lock);
	stage_rig_unregister_all(r);
	pthread_mutex_lock(&r->lock);
	r->registered = false;
	pthread_mutex_unlock(&r->lock);
	rig_kick_ui(r);
}

static void rig_destroy(void *data)
{
	struct stage_rig *r = data;
	pthread_mutex_lock(&g_rigs_lock);
	da_erase_item(g_rigs, &r);
	if (!g_rigs.num)
		da_free(g_rigs);
	pthread_mutex_unlock(&g_rigs_lock);

	stage_rig_unregister_all(r);
	pthread_mutex_lock(&r->lock);
	if (!r->removed)
		restore_all_locked(r);
	r->removed = true;
	r->destroyed = true;
	detach_all_locked(r); /* normally done by filter_remove already */
	pthread_mutex_unlock(&r->lock);

	pthread_mutex_lock(&r->audio_lock);
	audio_detach_locked(r);
	pthread_mutex_unlock(&r->audio_lock);

	rig_kick_ui(r); /* stops driving depth blur layers */
	rig_release(r);
}

/* Rewrites this rig's displaced items in the scene's freshly serialized
 * "items" array to their rest poses (see the file comment). UI thread. */
static void rig_save(void *data, obs_data_t *settings)
{
	(void)settings;
	struct stage_rig *r = data;
	pthread_mutex_lock(&r->lock);
	obs_source_t *parent = obs_weak_source_get_source(r->parent);
	/* Only an active rig tracks edits: an inactive one's rest poses may be
	 * stale, and its items are at rest already (restored when it stopped). */
	if (!parent || !r->is_scene || r->destroyed || !r->was_active) {
		obs_source_release(parent);
		pthread_mutex_unlock(&r->lock);
		return;
	}
	/* Every rest pose goes into the item private settings now (a queued UI
	 * task may not have run yet), so the saved stage.rest always matches
	 * the patched pose below. */
	for (size_t i = 0; i < r->layers.num; i++) {
		struct rig_layer *L = &r->layers.array[i];
		if (L->enrolled && L->has_rest) {
			obs_data_t *priv = obs_sceneitem_get_private_settings(L->item);
			if (priv)
				write_rest(priv, &L->rest);
			obs_data_release(priv);
			L->rest_dirty = false;
		}
	}
	obs_data_t *ps = obs_source_get_settings(parent);
	obs_data_array_t *items = obs_data_get_array(ps, "items");
	size_t n = obs_data_array_count(items);
	for (size_t i = 0; i < n; i++) {
		obs_data_t *it = obs_data_array_item(items, i);
		struct rig_layer *L = NULL;
		if (it && !obs_data_get_bool(it, "group_item_backup"))
			L = find_layer(r, obs_data_get_int(it, "id"));
		if (L && L->enrolled && L->has_rest && L->applied &&
		    xform_differs(&L->applied_x, &L->rest, L->bounds, 1e-3f, 1e-6f, 1e-4f) &&
		    (obs_data_get_int(it, "bounds_type") != OBS_BOUNDS_NONE) == L->bounds) {
			struct vec2 v;
			vec2_set(&v, L->rest.px, L->rest.py);
			obs_data_set_vec2(it, "pos", &v);
			obs_data_set_double(it, "rot", L->rest.rot);
			vec2_set(&v, L->rest.sx, L->rest.sy);
			obs_data_set_vec2(it, L->bounds ? "bounds" : "scale", &v);
			obs_data_erase(it, "pos_rel");
			obs_data_erase(it, "scale_rel");
			obs_data_erase(it, "bounds_rel");
		}
		obs_data_release(it);
	}
	obs_data_array_release(items);
	obs_data_release(ps);
	obs_source_release(parent);
	pthread_mutex_unlock(&r->lock);
}

/* ------------------------------------------------------------------------- */
/* Module-wide                                                               */
/* ------------------------------------------------------------------------- */

static size_t copy_rigs(struct stage_rig **out, size_t cap)
{
	pthread_mutex_lock(&g_rigs_lock);
	size_t n = g_rigs.num < cap ? g_rigs.num : cap;
	for (size_t i = 0; i < n; i++) {
		out[i] = g_rigs.array[i];
		rig_addref(out[i]);
	}
	pthread_mutex_unlock(&g_rigs_lock);
	return n;
}

static void restore_rigs(bool suspend)
{
	struct stage_rig *rigs[256];
	size_t n = copy_rigs(rigs, 256);
	for (size_t i = 0; i < n; i++) {
		struct stage_rig *r = rigs[i];
		pthread_mutex_lock(&r->lock);
		if (!r->destroyed && !r->removed) {
			stage_rig_model_snap_home(&r->m);
			stage_rig_model_auto(&r->m, STAGE_AUTO_OFF, NAN);
			restore_all_locked(r);
			if (suspend)
				r->suspended = true;
			r->ui_bits |= RIG_UI_SETTINGS;
		}
		pthread_mutex_unlock(&r->lock);
		rig_kick_ui(r);
		rig_release(r);
	}
}

void stage_camera_restore_all_rest_poses(void)
{
	restore_rigs(false);
	SLOG_INFO("restored all rest poses");
}

void stage_camera_rig_frontend_event(int event)
{
	switch ((enum obs_frontend_event)event) {
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_EXIT:
		/* Scenes are about to be saved and unloaded: rest poses on, no
		 * re-application until these rigs are destroyed. */
		restore_rigs(true);
		break;
	default:
		break;
	}
}

void stage_camera_rig_register_source(void)
{
	struct obs_source_info info = {0};
	info.id = STAGE_FILTER_CAMERA_RIG;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = rig_get_name;
	info.create = rig_create;
	info.destroy = rig_destroy;
	info.update = rig_update;
	info.get_defaults = rig_defaults;
	info.get_properties = rig_properties;
	info.video_tick = rig_tick;
	info.video_render = rig_render;
	info.filter_add = rig_filter_add;
	info.filter_remove = rig_filter_remove;
	info.save = rig_save;
	info.load = rig_load;
	obs_register_source(&info);
}
