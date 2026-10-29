/*
 * stage-presets.c — preset catalog: recipes, move list, FX presets, LUT and
 * light-leak ids. Loaded at module load, reloadable, copied out to callers.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <obs-module.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>

#include "stage-core.h"
#include "stage-json.h"
#include "stage-presets.h"
#ifdef STAGE_HAS_MATH
#include "stage-move.h"
#endif

/* Preset files larger than this are not presets (and are not parsed). */
#define MAX_PRESET_FILE_BYTES (256 * 1024)

/* ------------------------------------------------------------------------- */
/* fx_set parameter table (vendor-api.md 4.7)                                 */
/* ------------------------------------------------------------------------- */

const struct stage_fx_param_def stage_fx_params[] = {
	{"exposure", -2.0, 2.0, 0.0, false},
	{"contrast", -1.0, 1.0, 0.0, false},
	{"saturation", -1.0, 1.0, 0.0, false},
	{"temperature", -1.0, 1.0, 0.0, false},
	{"tint", -1.0, 1.0, 0.0, false},
	{"vignette", 0.0, 1.0, 0.0, false},
	{"vignette_soft", 0.0, 1.0, 0.5, false},
	{"vignette_round", 0.0, 1.0, 1.0, false},
	{"chroma", 0.0, 1.0, 0.0, false},
	{"grain", 0.0, 1.0, 0.0, false},
	{"grain_size", 0.5, 4.0, 1.0, false},
	{"lut", 0.0, 0.0, 0.0, true},
	{"lut_amount", 0.0, 1.0, 0.0, false},
	{"leak", 0.0, 0.0, 0.0, true},
	{"leak_amount", 0.0, 1.0, 0.0, false},
	{"leak_speed", 0.0, 2.0, 0.2, false},
	{"bloom", 0.0, 1.0, 0.0, false},
	{"bloom_threshold", 0.0, 1.0, 0.8, false},
};
#define FX_PARAM_COUNT (sizeof(stage_fx_params) / sizeof(stage_fx_params[0]))
const size_t stage_fx_param_count = FX_PARAM_COUNT;

const struct stage_fx_param_def *stage_fx_param_find(const char *name)
{
	for (size_t i = 0; name && i < FX_PARAM_COUNT; i++) {
		if (strcmp(stage_fx_params[i].name, name) == 0)
			return &stage_fx_params[i];
	}
	return NULL;
}

/* ------------------------------------------------------------------------- */
/* Catalog                                                                    */
/* ------------------------------------------------------------------------- */

struct fx_preset {
	char id[STAGE_ID_MAX];
	double value[FX_PARAM_COUNT]; /* numeric params; neutral when absent */
};

typedef char stage_id_t[STAGE_ID_MAX];

struct catalog {
	struct stage_recipe *recipes;
	size_t nrecipes;
	struct stage_move_info *moves;
	size_t nmoves;
	struct fx_preset *fx;
	size_t nfx;
	stage_id_t *luts;
	size_t nluts;
	stage_id_t *leaks;
	size_t nleaks;
};

static pthread_mutex_t cat_lock = PTHREAD_MUTEX_INITIALIZER;
static struct catalog cat;
static char *cat_builtin_root;
static char *cat_user_root;

static void catalog_free(struct catalog *c)
{
	bfree(c->recipes);
	bfree(c->moves);
	bfree(c->fx);
	bfree(c->luts);
	bfree(c->leaks);
	memset(c, 0, sizeof(*c));
}

/* Grows *arr (elements of size sz) by one; returns the new zeroed element. */
static void *grow(void **arr, size_t *n, size_t sz)
{
	*arr = brealloc(*arr, (*n + 1) * sz);
	void *el = (char *)*arr + *n * sz;
	memset(el, 0, sz);
	(*n)++;
	return el;
}

static int strcmp_ptr_cmp(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Calls fn(path, id, user) for every <id><ext> file in dir with a valid id. */
static void scan_dir(const char *dir, const char *ext, bool user,
		     void (*fn)(struct catalog *c, const char *path, const char *id, bool user),
		     struct catalog *c)
{
	if (!dir)
		return;
	os_dir_t *d = os_opendir(dir);
	if (!d)
		return;
	size_t ext_len = strlen(ext);
	struct os_dirent *ent;
	/* Sorted order makes the catalog (and move_list/recipe_list) stable. */
	char **names = NULL;
	size_t count = 0;
	while ((ent = os_readdir(d)) != NULL) {
		if (ent->directory)
			continue;
		size_t len = strlen(ent->d_name);
		if (len <= ext_len || strcmp(ent->d_name + len - ext_len, ext) != 0)
			continue;
		names = brealloc(names, (count + 1) * sizeof(char *));
		names[count++] = bstrdup(ent->d_name);
	}
	os_closedir(d);
	if (count > 1)
		qsort(names, count, sizeof(char *), strcmp_ptr_cmp);
	for (size_t i = 0; i < count; i++) {
		char id[STAGE_ID_MAX * 2];
		size_t len = strlen(names[i]) - ext_len;
		if (len >= sizeof(id) || len >= STAGE_ID_MAX) {
			SLOG_WARN("preset '%s/%s' skipped: file name is not a valid id", dir, names[i]);
			bfree(names[i]);
			continue;
		}
		memcpy(id, names[i], len);
		id[len] = '\0';
		if (!stage_id_valid(id)) {
			SLOG_WARN("preset '%s/%s' skipped: file name is not a valid id", dir, names[i]);
		} else {
			struct dstr path = {0};
			dstr_printf(&path, "%s/%s", dir, names[i]);
			fn(c, path.array, id, user);
			dstr_free(&path);
		}
		bfree(names[i]);
	}
	bfree(names);
}

/* Reads a JSON preset file (size-capped). NULL on error (logged). */
static obs_data_t *read_json(const char *path)
{
	int64_t size = os_get_file_size(path);
	if (size <= 0 || size > MAX_PRESET_FILE_BYTES) {
		SLOG_WARN("preset '%s' skipped: empty or larger than %d bytes", path,
			  MAX_PRESET_FILE_BYTES);
		return NULL;
	}
	obs_data_t *d = obs_data_create_from_json_file(path);
	if (!d)
		SLOG_WARN("preset '%s' skipped: not valid JSON", path);
	return d;
}

static void skip(const char *path, const struct sj_err *e)
{
	SLOG_WARN("preset '%s' skipped: %s", path, e->msg);
}

/* Common header: id must equal the file name; name is a localized name;
 * pro (optional bool); tags (optional array: strings are dropped by
 * obs_data, so only the type is checked). */
static bool check_header(obs_data_t *d, const char *id, struct stage_loc_name *name, bool *pro,
			 bool pro_required, struct sj_err *e)
{
	char fid[STAGE_ID_MAX];
	if (sj_id(d, "id", false, fid, e) != SJ_OK) {
		if (!e->msg[0])
			sj_fail(e, "'id' is required");
		return false;
	}
	if (strcmp(fid, id) != 0) {
		sj_fail(e, "id '%s' does not match the file name", fid);
		return false;
	}
	if (sj_localized(d, "name", 1, 64, name->en, name->ja, name->id, STAGE_LOC_NAME_MAX, e) !=
	    SJ_OK) {
		if (!e->msg[0])
			sj_fail(e, "'name' is required");
		return false;
	}
	int r = sj_bool(d, "pro", pro, e);
	if (r == SJ_BAD || (r == SJ_ABSENT && pro_required)) {
		if (r == SJ_ABSENT)
			sj_fail(e, "'pro' is required");
		return false;
	}
	obs_data_item_t *tags = obs_data_item_byname(d, "tags");
	bool tags_ok = !tags || obs_data_item_gettype(tags) == OBS_DATA_ARRAY;
	obs_data_item_release(&tags);
	if (!tags_ok) {
		sj_fail(e, "'tags' must be an array");
		return false;
	}
	return true;
}

/* --- recipes ------------------------------------------------------------- */

static bool parse_recipe(obs_data_t *d, const char *id, struct stage_recipe *r, struct sj_err *e)
{
	static const char *const keys[] = {"id",  "name", "description", "pro", "tags",   "roles",
					   "auto", "fx",  "audio_react", "dof", "camera", "overscan"};
	memset(r, 0, sizeof(*r));
	e->msg[0] = '\0';
	if (!sj_only_keys(d, keys, sizeof(keys) / sizeof(keys[0]), e) ||
	    !check_header(d, id, &r->name, &r->pro, true, e))
		return false;
	stage_strlcpy(r->id, id, sizeof(r->id));

	/* description: validated, not kept (no protocol request returns it). */
	if (sj_localized(d, "description", 0, 280, NULL, NULL, NULL, 2048, e) == SJ_BAD)
		return false;

	obs_data_t *o = NULL;
	bool ok = true;

	/* roles (required, >= 1 of bg/char/fg) */
	static const char *const role_keys[] = {"bg", "char", "fg"};
	if (sj_obj(d, "roles", &o, e) != SJ_OK) {
		if (!e->msg[0])
			sj_fail(e, "'roles' is required");
		return false;
	}
	ok = sj_only_keys(o, role_keys, 3, e);
	for (int i = 0; ok && i < STAGE_RROLE_COUNT; i++) {
		int g = sj_num(o, role_keys[i], STAGE_DEPTH_MIN, STAGE_DEPTH_MAX, &r->role_depth[i], e);
		ok = g != SJ_BAD;
		r->has_role[i] = g == SJ_OK;
	}
	obs_data_release(o);
	o = NULL;
	if (ok && !r->has_role[0] && !r->has_role[1] && !r->has_role[2]) {
		sj_fail(e, "'roles' must map at least one role");
		ok = false;
	}
	if (!ok)
		return false;

	/* auto {mode, intensity} */
	int g = sj_obj(d, "auto", &o, e);
	if (g == SJ_BAD)
		return false;
	if (g == SJ_OK) {
		static const char *const ak[] = {"mode", "intensity"};
		int mode = 0;
		double inten = 0;
		ok = sj_only_keys(o, ak, 2, e) &&
		     sj_enum(o, "mode", stage_auto_mode_names, STAGE_AUTO_COUNT, &mode, e) == SJ_OK &&
		     sj_num(o, "intensity", 0, 1, &inten, e) == SJ_OK;
		obs_data_release(o);
		o = NULL;
		if (!ok) {
			if (!e->msg[0])
				sj_fail(e, "'auto' needs mode and intensity");
			return false;
		}
		r->has_auto = true;
		r->auto_mode = (uint8_t)mode;
		r->auto_intensity = (float)inten;
	}

	if (sj_id(d, "fx", false, r->fx, e) == SJ_BAD)
		return false;

	/* audio_react {amount, attack_ms?, release_ms?} */
	g = sj_obj(d, "audio_react", &o, e);
	if (g == SJ_BAD)
		return false;
	if (g == SJ_OK) {
		static const char *const ak[] = {"amount", "attack_ms", "release_ms"};
		double amount = 0;
		long long att = 0, rel = 0;
		ok = sj_only_keys(o, ak, 3, e) && sj_num(o, "amount", 0, 1, &amount, e) == SJ_OK &&
		     sj_int(o, "attack_ms", 1, 1000, &att, e) != SJ_BAD &&
		     sj_int(o, "release_ms", 1, 5000, &rel, e) != SJ_BAD;
		obs_data_release(o);
		o = NULL;
		if (!ok) {
			if (!e->msg[0])
				sj_fail(e, "'audio_react' needs amount");
			return false;
		}
		r->has_audio = true;
		r->audio_amount = (float)amount;
		r->audio_attack_ms = (int)att;
		r->audio_release_ms = (int)rel;
	}

	/* dof {focus_depth, amount} */
	g = sj_obj(d, "dof", &o, e);
	if (g == SJ_BAD)
		return false;
	if (g == SJ_OK) {
		static const char *const dk[] = {"focus_depth", "amount"};
		double fd = 0, am = 0;
		ok = sj_only_keys(o, dk, 2, e) &&
		     sj_num(o, "focus_depth", STAGE_DEPTH_MIN, STAGE_DEPTH_MAX, &fd, e) == SJ_OK &&
		     sj_num(o, "amount", 0, 1, &am, e) == SJ_OK;
		obs_data_release(o);
		o = NULL;
		if (!ok) {
			if (!e->msg[0])
				sj_fail(e, "'dof' needs focus_depth and amount");
			return false;
		}
		r->has_dof = true;
		r->focus_depth = (float)fd;
		r->dof_amount = (float)am;
	}

	/* camera {x, y, zoom, rot} */
	g = sj_obj(d, "camera", &o, e);
	if (g == SJ_BAD)
		return false;
	if (g == SJ_OK) {
		static const char *const ck[] = {"x", "y", "zoom", "rot"};
		double x = 0, y = 0, z = 1, rot = 0;
		ok = sj_only_keys(o, ck, 4, e) && sj_num(o, "x", -1e5, 1e5, &x, e) == SJ_OK &&
		     sj_num(o, "y", -1e5, 1e5, &y, e) == SJ_OK &&
		     sj_num(o, "zoom", STAGE_ZOOM_MIN, STAGE_ZOOM_MAX, &z, e) == SJ_OK &&
		     sj_num(o, "rot", STAGE_ROT_MIN, STAGE_ROT_MAX, &rot, e) == SJ_OK;
		obs_data_release(o);
		o = NULL;
		if (!ok) {
			if (!e->msg[0])
				sj_fail(e, "'camera' needs x, y, zoom and rot");
			return false;
		}
		r->has_camera = true;
		r->cam_x = (float)x;
		r->cam_y = (float)y;
		r->cam_zoom = (float)z;
		r->cam_rot = (float)rot;
	}

	double ov = 0;
	g = sj_num(d, "overscan", 0, 0.25, &ov, e);
	if (g == SJ_BAD)
		return false;
	r->has_overscan = g == SJ_OK;
	r->overscan = (float)ov;
	return true;
}

static bool recipe_id_taken(const struct catalog *c, const char *id)
{
	for (size_t i = 0; i < c->nrecipes; i++) {
		if (strcmp(c->recipes[i].id, id) == 0)
			return true;
	}
	return false;
}

static void load_recipe(struct catalog *c, const char *path, const char *id, bool user)
{
	obs_data_t *d = read_json(path);
	if (!d)
		return;
	struct stage_recipe r;
	struct sj_err e = {{0}};
	if (!parse_recipe(d, id, &r, &e)) {
		skip(path, &e);
	} else if (recipe_id_taken(c, id)) {
		SLOG_WARN("preset '%s' skipped: recipe '%s' already exists", path, id);
	} else {
		r.user = user;
		*(struct stage_recipe *)grow((void **)&c->recipes, &c->nrecipes, sizeof(r)) = r;
	}
	obs_data_release(d);
}

/* --- moves (metadata + validity) ----------------------------------------- */

static bool parse_move(obs_data_t *d, const char *id, struct stage_move_info *m, struct sj_err *e)
{
	static const char *const keys[] = {"id",  "name", "duration_ms", "relative",
					   "hold", "pro", "tags",        "keys"};
	memset(m, 0, sizeof(*m));
	e->msg[0] = '\0';
	bool pro = false, rel = true, hold = false;
	long long dur = 0;
	if (!sj_only_keys(d, keys, sizeof(keys) / sizeof(keys[0]), e) ||
	    !check_header(d, id, &m->name, &pro, false, e))
		return false;
	if (sj_int(d, "duration_ms", STAGE_MOVE_DURATION_MIN, STAGE_MOVE_DURATION_MAX, &dur, e) !=
	    SJ_OK) {
		if (!e->msg[0])
			sj_fail(e, "'duration_ms' is required");
		return false;
	}
	if (sj_bool(d, "relative", &rel, e) == SJ_BAD || sj_bool(d, "hold", &hold, e) == SJ_BAD)
		return false;

	obs_data_item_t *it = obs_data_item_byname(d, "keys");
	bool is_arr = it && obs_data_item_gettype(it) == OBS_DATA_ARRAY;
	obs_data_array_t *arr = is_arr ? obs_data_item_get_array(it) : NULL;
	obs_data_item_release(&it);
	size_t n = obs_data_array_count(arr);
	bool ok = true;
	if (!arr || n < 2 || n > STAGE_MOVE_MAX_KEYS) {
		sj_fail(e, "'keys' must hold 2..%d keys", STAGE_MOVE_MAX_KEYS);
		ok = false;
	}
#ifdef STAGE_HAS_MATH
	struct stage_move_def def;
	memset(&def, 0, sizeof(def));
	stage_strlcpy(def.id, id, sizeof(def.id));
	def.duration_ms = (uint32_t)dur;
	def.relative = rel;
	def.hold = hold;
	def.nkeys = (uint8_t)(ok ? n : 0);
#endif
	static const char *const kk[] = {"t", "dx", "dy", "dzoom", "drot", "shake", "ease"};
	static const struct {
		const char *name;
		double lo, hi;
		uint8_t bit;
	} ch[] = {
		{"dx", -1, 1, STAGE_CH_X},          {"dy", -1, 1, STAGE_CH_Y},
		{"dzoom", -0.9, 3, STAGE_CH_ZOOM},  {"drot", -45, 45, STAGE_CH_ROT},
		{"shake", 0, 1, STAGE_CH_SHAKE},
	};
	double prev_t = -1.0;
	for (size_t i = 0; ok && i < n; i++) {
		obs_data_t *k = obs_data_array_item(arr, i);
		double t = 0, v[5] = {0};
		int ease = STAGE_EASE_DEFAULT;
		uint8_t mask = 0;
		ok = sj_only_keys(k, kk, 7, e) && sj_num(k, "t", 0, 1, &t, e) == SJ_OK;
		for (size_t c = 0; ok && c < 5; c++) {
			int g = sj_num(k, ch[c].name, ch[c].lo, ch[c].hi, &v[c], e);
			ok = g != SJ_BAD;
			if (g == SJ_OK)
				mask |= ch[c].bit;
		}
		ok = ok && sj_enum(k, "ease", stage_ease_names, STAGE_EASE_COUNT, &ease, e) != SJ_BAD;
		if (ok && t <= prev_t) {
			sj_fail(e, "key t values must be strictly increasing");
			ok = false;
		}
		if (!ok && !e->msg[0])
			sj_fail(e, "key %zu needs t", i);
		prev_t = t;
#ifdef STAGE_HAS_MATH
		if (ok) {
			struct stage_key *key = &def.keys[i];
			key->t = (float)t;
			key->dx = (float)v[0];
			key->dy = (float)v[1];
			key->dzoom = (float)v[2];
			key->drot = (float)v[3];
			key->shake = (float)v[4];
			key->ease = (uint8_t)ease;
			key->mask = mask;
		}
#else
		(void)mask;
#endif
		obs_data_release(k);
	}
	obs_data_array_release(arr);
	if (!ok)
		return false;
#ifdef STAGE_HAS_MATH
	if (!stage_move_valid(&def)) {
		sj_fail(e, "move fails stage_move_valid");
		return false;
	}
#endif
	stage_strlcpy(m->id, id, sizeof(m->id));
	m->duration_ms = (uint32_t)dur;
	m->builtin = stage_move_from_name(id) >= 0;
	return true;
}

static void load_move(struct catalog *c, const char *path, const char *id, bool user)
{
	for (size_t i = 0; i < c->nmoves; i++) {
		if (strcmp(c->moves[i].id, id) == 0) {
			SLOG_WARN("preset '%s' skipped: move '%s' already exists", path, id);
			return;
		}
	}
	obs_data_t *d = read_json(path);
	if (!d)
		return;
	struct stage_move_info m;
	struct sj_err e = {{0}};
	if (parse_move(d, id, &m, &e)) {
		m.user = user;
		*(struct stage_move_info *)grow((void **)&c->moves, &c->nmoves, sizeof(m)) = m;
	} else {
		skip(path, &e);
	}
	obs_data_release(d);
}

/* Built-in moves always exist (the rig compiles fallbacks in); give them a
 * list entry when their file is missing. Durations match the shipped files. */
static void add_builtin_move_fallbacks(struct catalog *c)
{
	static const uint32_t dur[STAGE_MOVE_COUNT] = {800, 800, 1200, 1200, 600, 380, 2500, 700, 700};
	for (int b = 0; b < STAGE_MOVE_COUNT; b++) {
		bool have = false;
		for (size_t i = 0; i < c->nmoves && !have; i++)
			have = strcmp(c->moves[i].id, stage_move_names[b]) == 0;
		if (have)
			continue;
		struct stage_move_info *m = grow((void **)&c->moves, &c->nmoves, sizeof(*m));
		stage_strlcpy(m->id, stage_move_names[b], sizeof(m->id));
		stage_strlcpy(m->name.en, stage_move_names[b], sizeof(m->name.en));
		stage_strlcpy(m->name.ja, stage_move_names[b], sizeof(m->name.ja));
		stage_strlcpy(m->name.id, stage_move_names[b], sizeof(m->name.id));
		m->duration_ms = dur[b];
		m->builtin = true;
	}
}

/* --- FX presets ------------------------------------------------------------ */

static bool parse_fx(obs_data_t *d, const char *id, struct fx_preset *f, struct sj_err *e)
{
	static const char *const keys[] = {"id", "name", "pro", "tags", "params"};
	struct stage_loc_name name;
	bool pro = false;
	memset(f, 0, sizeof(*f));
	e->msg[0] = '\0';
	if (!sj_only_keys(d, keys, 5, e) || !check_header(d, id, &name, &pro, false, e))
		return false;
	obs_data_t *p = NULL;
	if (sj_obj(d, "params", &p, e) != SJ_OK) {
		if (!e->msg[0])
			sj_fail(e, "'params' is required");
		return false;
	}
	bool ok = true;
	for (obs_data_item_t *it = obs_data_first(p); ok && it; obs_data_item_next(&it)) {
		if (!stage_fx_param_find(obs_data_item_get_name(it))) {
			sj_fail(e, "unknown param '%.64s'", obs_data_item_get_name(it));
			ok = false;
			obs_data_item_release(&it);
			break;
		}
	}
	for (size_t i = 0; ok && i < FX_PARAM_COUNT; i++) {
		const struct stage_fx_param_def *pd = &stage_fx_params[i];
		f->value[i] = pd->neutral;
		if (pd->is_id) {
			ok = sj_id(p, pd->name, true, NULL, e) != SJ_BAD;
		} else {
			double v;
			int g = sj_num(p, pd->name, pd->lo, pd->hi, &v, e);
			ok = g != SJ_BAD;
			if (g == SJ_OK)
				f->value[i] = v;
		}
	}
	obs_data_release(p);
	if (ok)
		stage_strlcpy(f->id, id, sizeof(f->id));
	return ok;
}

static void load_fx(struct catalog *c, const char *path, const char *id, bool user)
{
	(void)user;
	for (size_t i = 0; i < c->nfx; i++) {
		if (strcmp(c->fx[i].id, id) == 0) {
			SLOG_WARN("preset '%s' skipped: FX preset '%s' already exists", path, id);
			return;
		}
	}
	obs_data_t *d = read_json(path);
	if (!d)
		return;
	struct fx_preset f;
	struct sj_err e = {{0}};
	if (parse_fx(d, id, &f, &e))
		*(struct fx_preset *)grow((void **)&c->fx, &c->nfx, sizeof(f)) = f;
	else
		skip(path, &e);
	obs_data_release(d);
}

/* --- LUT / leak textures (ids only) ---------------------------------------- */

static void add_id(stage_id_t **arr, size_t *n, const char *id)
{
	for (size_t i = 0; i < *n; i++) {
		if (strcmp((*arr)[i], id) == 0)
			return;
	}
	stage_id_t *slot = grow((void **)arr, n, sizeof(stage_id_t));
	stage_strlcpy(*slot, id, STAGE_ID_MAX);
}

static void load_lut(struct catalog *c, const char *path, const char *id, bool user)
{
	(void)path;
	(void)user;
	add_id(&c->luts, &c->nluts, id);
}

static void load_leak(struct catalog *c, const char *path, const char *id, bool user)
{
	(void)path;
	(void)user;
	add_id(&c->leaks, &c->nleaks, id);
}

/* ------------------------------------------------------------------------- */
/* Load / reload / free                                                       */
/* ------------------------------------------------------------------------- */

static void load_kind(struct catalog *c, const char *builtin_root, const char *user_root,
		      const char *sub, const char *ext,
		      void (*fn)(struct catalog *c, const char *path, const char *id, bool user))
{
	struct dstr dir = {0};
	if (builtin_root) {
		dstr_printf(&dir, "%s/%s", builtin_root, sub);
		scan_dir(dir.array, ext, false, fn, c);
	}
	if (user_root) {
		dstr_printf(&dir, "%s/%s", user_root, sub);
		scan_dir(dir.array, ext, true, fn, c);
	}
	dstr_free(&dir);
}

size_t stage_presets_load(const char *builtin_root, const char *user_root)
{
	struct catalog c;
	memset(&c, 0, sizeof(c));
	/* FX first so recipes can be cross-checked. */
	load_kind(&c, builtin_root, user_root, "fx", ".json", load_fx);
	load_kind(&c, builtin_root, user_root, "luts", ".png", load_lut);
	load_kind(&c, builtin_root, user_root, "leaks", ".png", load_leak);
	load_kind(&c, builtin_root, user_root, "moves", ".json", load_move);
	add_builtin_move_fallbacks(&c);
	load_kind(&c, builtin_root, user_root, "recipes", ".json", load_recipe);

	for (size_t i = 0; i < c.nrecipes; i++) {
		const char *fx = c.recipes[i].fx;
		bool known = !fx[0];
		for (size_t k = 0; k < c.nfx && !known; k++)
			known = strcmp(c.fx[k].id, fx) == 0;
		if (!known)
			SLOG_WARN("recipe '%s' uses unknown FX preset '%s'; it will be skipped",
				  c.recipes[i].id, fx);
	}

	char *b = builtin_root ? bstrdup(builtin_root) : NULL;
	char *u = user_root ? bstrdup(user_root) : NULL;
	struct catalog old;
	pthread_mutex_lock(&cat_lock);
	old = cat;
	cat = c;
	if (b != cat_builtin_root) {
		bfree(cat_builtin_root);
		cat_builtin_root = b;
	}
	if (u != cat_user_root) {
		bfree(cat_user_root);
		cat_user_root = u;
	}
	pthread_mutex_unlock(&cat_lock);
	catalog_free(&old);

	SLOG_INFO("presets: %zu recipes, %zu moves, %zu FX presets, %zu LUTs, %zu light leaks",
		  c.nrecipes, c.nmoves, c.nfx, c.nluts, c.nleaks);
	return c.nrecipes;
}

void stage_presets_reload(void)
{
	pthread_mutex_lock(&cat_lock);
	char *b = cat_builtin_root ? bstrdup(cat_builtin_root) : NULL;
	char *u = cat_user_root ? bstrdup(cat_user_root) : NULL;
	pthread_mutex_unlock(&cat_lock);
	stage_presets_load(b, u);
	bfree(b);
	bfree(u);
}

bool stage_presets_user_dir(const char *sub, char *out, size_t cap)
{
	if (!out || !cap)
		return false;
	out[0] = 0;
	pthread_mutex_lock(&cat_lock);
	if (cat_user_root)
		snprintf(out, cap, "%s/%s", cat_user_root, sub ? sub : "");
	pthread_mutex_unlock(&cat_lock);
	return out[0] != 0;
}

void stage_presets_free(void)
{
	pthread_mutex_lock(&cat_lock);
	struct catalog old = cat;
	memset(&cat, 0, sizeof(cat));
	bfree(cat_builtin_root);
	bfree(cat_user_root);
	cat_builtin_root = NULL;
	cat_user_root = NULL;
	pthread_mutex_unlock(&cat_lock);
	catalog_free(&old);
}

/* ------------------------------------------------------------------------- */
/* Queries                                                                    */
/* ------------------------------------------------------------------------- */

size_t stage_recipe_count(void)
{
	pthread_mutex_lock(&cat_lock);
	size_t n = cat.nrecipes;
	pthread_mutex_unlock(&cat_lock);
	return n;
}

bool stage_recipe_at(size_t i, struct stage_recipe *out)
{
	bool ok = false;
	pthread_mutex_lock(&cat_lock);
	if (i < cat.nrecipes) {
		if (out)
			*out = cat.recipes[i];
		ok = true;
	}
	pthread_mutex_unlock(&cat_lock);
	return ok;
}

bool stage_recipe_find(const char *id, struct stage_recipe *out)
{
	bool ok = false;
	if (!id)
		return false;
	pthread_mutex_lock(&cat_lock);
	for (size_t i = 0; i < cat.nrecipes && !ok; i++) {
		if (strcmp(cat.recipes[i].id, id) == 0) {
			if (out)
				*out = cat.recipes[i];
			ok = true;
		}
	}
	pthread_mutex_unlock(&cat_lock);
	return ok;
}

size_t stage_move_info_count(void)
{
	pthread_mutex_lock(&cat_lock);
	size_t n = cat.nmoves;
	pthread_mutex_unlock(&cat_lock);
	return n;
}

bool stage_move_info_at(size_t i, struct stage_move_info *out)
{
	bool ok = false;
	pthread_mutex_lock(&cat_lock);
	if (i < cat.nmoves) {
		if (out)
			*out = cat.moves[i];
		ok = true;
	}
	pthread_mutex_unlock(&cat_lock);
	return ok;
}

bool stage_move_known(const char *id)
{
	if (!id)
		return false;
	if (stage_move_from_name(id) >= 0)
		return true;
	bool ok = false;
	pthread_mutex_lock(&cat_lock);
	for (size_t i = 0; i < cat.nmoves && !ok; i++)
		ok = strcmp(cat.moves[i].id, id) == 0;
	pthread_mutex_unlock(&cat_lock);
	return ok;
}

static bool id_in(const stage_id_t *arr, size_t n, const char *id)
{
	for (size_t i = 0; i < n; i++) {
		if (strcmp(arr[i], id) == 0)
			return true;
	}
	return false;
}

bool stage_fx_preset_known(const char *id)
{
	return stage_fx_preset_param(id, NULL, NULL);
}

bool stage_lut_known(const char *id)
{
	if (!id)
		return false;
	pthread_mutex_lock(&cat_lock);
	bool ok = id_in(cat.luts, cat.nluts, id);
	pthread_mutex_unlock(&cat_lock);
	return ok;
}

bool stage_leak_known(const char *id)
{
	if (!id)
		return false;
	pthread_mutex_lock(&cat_lock);
	bool ok = id_in(cat.leaks, cat.nleaks, id);
	pthread_mutex_unlock(&cat_lock);
	return ok;
}

/* param NULL: only checks that the preset exists. */
bool stage_fx_preset_param(const char *preset_id, const char *param, double *out)
{
	if (!preset_id)
		return false;
	size_t pi = FX_PARAM_COUNT;
	if (param) {
		const struct stage_fx_param_def *pd = stage_fx_param_find(param);
		if (!pd || pd->is_id)
			return false;
		pi = (size_t)(pd - stage_fx_params);
	}
	bool ok = false;
	pthread_mutex_lock(&cat_lock);
	for (size_t i = 0; i < cat.nfx && !ok; i++) {
		if (strcmp(cat.fx[i].id, preset_id) == 0) {
			ok = true;
			if (pi < FX_PARAM_COUNT && out)
				*out = cat.fx[i].value[pi];
		}
	}
	pthread_mutex_unlock(&cat_lock);
	return ok;
}
