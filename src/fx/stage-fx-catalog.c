/*
 * stage-fx-catalog.c — FX presets, LUT strips and light-leak textures.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Folders (vendor-api.md section 6):
 *   built-in  <module data>/presets/{fx,luts,leaks}/
 *   user      obs_module_config_path("{fx,luts,leaks}")
 * FX presets are <id>.json (schema fx-preset.json, checked strictly here:
 * unknown fields, wrong types and out-of-range values skip the file with a
 * warning). LUTs and leaks are <id>.png. A user file with a built-in id is
 * skipped, so ids are never ambiguous. Built-in presets are listed first
 * ("neutral" leading), then user presets, each alphabetically.
 */
#include "stage-fx-internal.h"

#include <stdlib.h>
#include <string.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

struct preset_entry {
	char id[STAGE_ID_MAX];
	char name[3][STFX_NAME_MAX]; /* en, ja, id */
	bool user, pro;
	struct stfx_params params;
};

struct asset_entry {
	char id[STAGE_ID_MAX];
	bool user;
	char *path; /* bstrdup */
};

typedef DARRAY(struct preset_entry) preset_array_t;
typedef DARRAY(struct asset_entry) asset_array_t;

static pthread_mutex_t cat_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile long cat_generation; /* +1 per stfx_catalog_load */
static preset_array_t presets;
static asset_array_t assets[STFX_ASSET_COUNT];

static const char *const asset_dirs[STFX_ASSET_COUNT] = {"luts", "leaks"};

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

/* "<dir>/<stem>.<ext>" -> stem when it is a valid id with that extension. */
static bool id_from_file(const char *file, const char *ext, char out[STAGE_ID_MAX])
{
	const char *dot = strrchr(file, '.');
	if (!dot || astrcmpi(dot + 1, ext) != 0)
		return false;
	size_t n = (size_t)(dot - file);
	if (n == 0 || n >= STAGE_ID_MAX)
		return false;
	memcpy(out, file, n);
	out[n] = '\0';
	return stage_id_valid(out) != 0;
}

static bool valid_utf8_name(const char *s)
{
	/* Same rules as view names but up to 64 code points (schema). */
	if (!s || !s[0])
		return false;
	size_t cp = 0;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if ((*p & 0xC0) != 0x80)
			cp++;
	}
	return cp <= 64 && strlen(s) < STFX_NAME_MAX;
}

static bool read_name(obs_data_t *d, struct preset_entry *e, const char *file)
{
	obs_data_item_t *it = obs_data_item_byname(d, "name");
	if (!it || obs_data_item_gettype(it) != OBS_DATA_OBJECT) {
		obs_data_item_release(&it);
		SLOG_WARN("fx preset %s: 'name' must be an object {en, ja, id}", file);
		return false;
	}
	obs_data_item_release(&it);
	obs_data_t *n = obs_data_get_obj(d, "name");
	static const char *const langs[3] = {"en", "ja", "id"};
	bool ok = true;
	for (obs_data_item_t *li = obs_data_first(n); li; obs_data_item_next(&li)) {
		const char *k = obs_data_item_get_name(li);
		if (strcmp(k, "en") && strcmp(k, "ja") && strcmp(k, "id")) {
			SLOG_WARN("fx preset %s: unknown field 'name.%s'", file, k);
			ok = false;
		}
	}
	for (int i = 0; ok && i < 3; i++) {
		obs_data_item_t *li = obs_data_item_byname(n, langs[i]);
		const char *s = (li && obs_data_item_gettype(li) == OBS_DATA_STRING)
					? obs_data_item_get_string(li)
					: NULL;
		if (!valid_utf8_name(s)) {
			SLOG_WARN("fx preset %s: 'name.%s' must be a string of 1..64 characters", file,
				  langs[i]);
			ok = false;
		} else {
			stage_strlcpy(e->name[i], s, STFX_NAME_MAX);
		}
		obs_data_item_release(&li);
	}
	obs_data_release(n);
	return ok;
}

static bool read_params(obs_data_t *d, struct stfx_params *p, const char *file)
{
	obs_data_item_t *it = obs_data_item_byname(d, "params");
	bool is_obj = it && obs_data_item_gettype(it) == OBS_DATA_OBJECT;
	obs_data_item_release(&it);
	if (!is_obj) {
		SLOG_WARN("fx preset %s: 'params' must be an object", file);
		return false;
	}
	obs_data_t *pd = obs_data_get_obj(d, "params");
	bool ok = true;
	stfx_params_neutral(p);
	for (obs_data_item_t *pi = obs_data_first(pd); pi; obs_data_item_next(&pi)) {
		const char *k = obs_data_item_get_name(pi);
		const enum obs_data_type t = obs_data_item_gettype(pi);
		int i = stfx_num_find(k);
		if (i >= 0) {
			double v = t == OBS_DATA_NUMBER ? obs_data_item_get_double(pi) : 0.0;
			if (t != OBS_DATA_NUMBER || v != v || v < stfx_num_defs[i].lo ||
			    v > stfx_num_defs[i].hi) {
				SLOG_WARN("fx preset %s: '%s' must be a number in %g..%g", file, k,
					  stfx_num_defs[i].lo, stfx_num_defs[i].hi);
				ok = false;
				continue;
			}
			p->v[i] = (float)v;
		} else if (!strcmp(k, STFX_PARAM_LUT) || !strcmp(k, STFX_PARAM_LEAK)) {
			const char *s = t == OBS_DATA_STRING ? obs_data_item_get_string(pi) : NULL;
			if (!s || (s[0] && !stage_id_valid(s))) {
				SLOG_WARN("fx preset %s: '%s' must be an id or \"\"", file, k);
				ok = false;
				continue;
			}
			stage_strlcpy(!strcmp(k, STFX_PARAM_LUT) ? p->lut : p->leak, s, STAGE_ID_MAX);
		} else {
			SLOG_WARN("fx preset %s: unknown param '%s'", file, k);
			ok = false;
		}
	}
	obs_data_release(pd);
	return ok;
}

static bool parse_preset(const char *path, const char *file, const char *want_id, bool user,
			 struct preset_entry *e)
{
	obs_data_t *d = obs_data_create_from_json_file(path);
	if (!d) {
		SLOG_WARN("fx preset %s: not valid JSON", file);
		return false;
	}
	memset(e, 0, sizeof(*e));
	e->user = user;
	bool ok = true;

	for (obs_data_item_t *it = obs_data_first(d); it; obs_data_item_next(&it)) {
		const char *k = obs_data_item_get_name(it);
		const enum obs_data_type t = obs_data_item_gettype(it);
		if (!strcmp(k, "id")) {
			const char *s = t == OBS_DATA_STRING ? obs_data_item_get_string(it) : "";
			if (strcmp(s, want_id) != 0) {
				SLOG_WARN("fx preset %s: 'id' must equal the file name", file);
				ok = false;
			}
		} else if (!strcmp(k, "pro")) {
			if (t != OBS_DATA_BOOLEAN) {
				SLOG_WARN("fx preset %s: 'pro' must be a boolean", file);
				ok = false;
			} else {
				e->pro = obs_data_item_get_bool(it);
			}
		} else if (!strcmp(k, "tags")) {
			/* obs_data drops arrays of strings; only the type is checked. */
			if (t != OBS_DATA_ARRAY) {
				SLOG_WARN("fx preset %s: 'tags' must be an array", file);
				ok = false;
			}
		} else if (strcmp(k, "name") && strcmp(k, "params")) {
			SLOG_WARN("fx preset %s: unknown field '%s'", file, k);
			ok = false;
		}
	}
	if (ok && !obs_data_has_user_value(d, "id")) {
		SLOG_WARN("fx preset %s: missing 'id'", file);
		ok = false;
	}
	ok = ok && read_name(d, e, file) && read_params(d, &e->params, file);
	stage_strlcpy(e->id, want_id, sizeof(e->id));
	obs_data_release(d);
	return ok;
}

/* Scans dir for <id>.<ext> files and calls add(path, file, id). */
static void scan_dir(const char *dir, const char *ext,
		     void (*add)(const char *path, const char *file, const char *id, void *ud), void *ud)
{
	if (!dir)
		return;
	os_dir_t *d = os_opendir(dir);
	if (!d)
		return;
	struct os_dirent *ent;
	struct dstr path = {0};
	while ((ent = os_readdir(d)) != NULL) {
		char id[STAGE_ID_MAX];
		if (ent->directory || !id_from_file(ent->d_name, ext, id))
			continue;
		dstr_printf(&path, "%s/%s", dir, ent->d_name);
		add(path.array, ent->d_name, id, ud);
	}
	dstr_free(&path);
	os_closedir(d);
}

struct scan_ctx {
	bool user;
	int kind;
	preset_array_t *out_presets;
	asset_array_t *out_assets;
};

static void add_preset(const char *path, const char *file, const char *id, void *ud)
{
	struct scan_ctx *c = ud;
	for (size_t i = 0; i < c->out_presets->num; i++) {
		if (!strcmp(c->out_presets->array[i].id, id)) {
			if (c->user)
				SLOG_WARN("fx preset %s: a built-in preset has this id; skipped", file);
			return;
		}
	}
	struct preset_entry e;
	if (parse_preset(path, file, id, c->user, &e))
		da_push_back(*c->out_presets, &e);
}

static void add_asset(const char *path, const char *file, const char *id, void *ud)
{
	struct scan_ctx *c = ud;
	for (size_t i = 0; i < c->out_assets->num; i++) {
		if (!strcmp(c->out_assets->array[i].id, id)) {
			if (c->user)
				SLOG_WARN("%s %s: a built-in file has this id; skipped",
					  asset_dirs[c->kind], file);
			return;
		}
	}
	struct asset_entry e = {.user = c->user};
	stage_strlcpy(e.id, id, sizeof(e.id));
	e.path = bstrdup(path);
	da_push_back(*c->out_assets, &e);
}

static int cmp_preset(const void *a, const void *b)
{
	const struct preset_entry *x = a, *y = b;
	if (x->user != y->user)
		return x->user ? 1 : -1;
	bool xn = !strcmp(x->id, "neutral"), yn = !strcmp(y->id, "neutral");
	if (xn != yn)
		return xn ? -1 : 1;
	return strcmp(x->id, y->id);
}

static int cmp_asset(const void *a, const void *b)
{
	const struct asset_entry *x = a, *y = b;
	if (x->user != y->user)
		return x->user ? 1 : -1;
	return strcmp(x->id, y->id);
}

/* Absolute path only: without a module config path (headless libobs) OBS
 * returns a relative one, which must never be scanned or created. */
char *stfx_user_dir(const char *kind)
{
	char *p = obs_module_config_path(kind);
	const bool abs = p && (p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':'));
	if (!abs) {
		bfree(p);
		return NULL;
	}
	return p;
}

static void free_assets(asset_array_t *a)
{
	for (size_t i = 0; i < a->num; i++)
		bfree(a->array[i].path);
	da_free(*a);
}

/* ------------------------------------------------------------------------- */
/* Public                                                                    */
/* ------------------------------------------------------------------------- */

void stfx_catalog_load(void)
{
	preset_array_t new_presets;
	asset_array_t new_assets[STFX_ASSET_COUNT];
	da_init(new_presets);
	for (int k = 0; k < STFX_ASSET_COUNT; k++)
		da_init(new_assets[k]);

	char *root = obs_module_file("presets");
	struct dstr dir = {0};
	for (int pass = 0; pass < 2; pass++) {
		const bool user = pass == 1;
		struct scan_ctx c = {.user = user, .out_presets = &new_presets};
		char *udir = NULL;

		if (user) {
			udir = stfx_user_dir("fx");
			scan_dir(udir, "json", add_preset, &c);
			bfree(udir);
		} else if (root) {
			dstr_printf(&dir, "%s/fx", root);
			scan_dir(dir.array, "json", add_preset, &c);
		}
		for (int k = 0; k < STFX_ASSET_COUNT; k++) {
			c.kind = k;
			c.out_assets = &new_assets[k];
			if (user) {
				udir = stfx_user_dir(asset_dirs[k]);
				scan_dir(udir, "png", add_asset, &c);
				bfree(udir);
			} else if (root) {
				dstr_printf(&dir, "%s/%s", root, asset_dirs[k]);
				scan_dir(dir.array, "png", add_asset, &c);
			}
		}
	}
	dstr_free(&dir);
	bfree(root);

	if (new_presets.num)
		qsort(new_presets.array, new_presets.num, sizeof(*new_presets.array), cmp_preset);
	for (int k = 0; k < STFX_ASSET_COUNT; k++) {
		if (new_assets[k].num)
			qsort(new_assets[k].array, new_assets[k].num, sizeof(*new_assets[k].array),
			      cmp_asset);
	}

	pthread_mutex_lock(&cat_lock);
	da_free(presets);
	presets = new_presets;
	for (int k = 0; k < STFX_ASSET_COUNT; k++) {
		free_assets(&assets[k]);
		assets[k] = new_assets[k];
	}
	pthread_mutex_unlock(&cat_lock);
	os_atomic_inc_long(&cat_generation);
}

long stfx_catalog_generation(void)
{
	return os_atomic_load_long(&cat_generation);
}

void stfx_catalog_free(void)
{
	pthread_mutex_lock(&cat_lock);
	da_free(presets);
	for (int k = 0; k < STFX_ASSET_COUNT; k++)
		free_assets(&assets[k]);
	pthread_mutex_unlock(&cat_lock);
}

static int lang_index(void)
{
	const char *loc = obs_get_locale();
	if (loc && !strncmp(loc, "ja", 2))
		return 1;
	if (loc && !strncmp(loc, "id", 2))
		return 2;
	return 0;
}

static void copy_info(const struct preset_entry *e, struct stfx_preset_info *out)
{
	stage_strlcpy(out->id, e->id, sizeof(out->id));
	stage_strlcpy(out->name, e->name[lang_index()], sizeof(out->name));
	out->user = e->user;
	out->pro = e->pro;
	out->params = e->params;
}

size_t stfx_preset_count(void)
{
	pthread_mutex_lock(&cat_lock);
	size_t n = presets.num;
	pthread_mutex_unlock(&cat_lock);
	return n;
}

bool stfx_preset_at(size_t i, struct stfx_preset_info *out)
{
	bool ok = false;
	pthread_mutex_lock(&cat_lock);
	if (i < presets.num) {
		copy_info(&presets.array[i], out);
		ok = true;
	}
	pthread_mutex_unlock(&cat_lock);
	return ok;
}

bool stfx_preset_get(const char *id, struct stfx_preset_info *out)
{
	if (!id || !id[0])
		return false;
	bool ok = false;
	pthread_mutex_lock(&cat_lock);
	for (size_t i = 0; i < presets.num; i++) {
		if (!strcmp(presets.array[i].id, id)) {
			if (out)
				copy_info(&presets.array[i], out);
			ok = true;
			break;
		}
	}
	pthread_mutex_unlock(&cat_lock);
	return ok;
}

size_t stfx_asset_count(int kind)
{
	if (kind < 0 || kind >= STFX_ASSET_COUNT)
		return 0;
	pthread_mutex_lock(&cat_lock);
	size_t n = assets[kind].num;
	pthread_mutex_unlock(&cat_lock);
	return n;
}

bool stfx_asset_at(int kind, size_t i, char id[STAGE_ID_MAX], bool *user)
{
	if (kind < 0 || kind >= STFX_ASSET_COUNT)
		return false;
	bool ok = false;
	pthread_mutex_lock(&cat_lock);
	if (i < assets[kind].num) {
		stage_strlcpy(id, assets[kind].array[i].id, STAGE_ID_MAX);
		if (user)
			*user = assets[kind].array[i].user;
		ok = true;
	}
	pthread_mutex_unlock(&cat_lock);
	return ok;
}

bool stfx_asset_path(int kind, const char *id, char *path, size_t cap)
{
	if (kind < 0 || kind >= STFX_ASSET_COUNT || !id || !id[0])
		return false;
	bool ok = false;
	pthread_mutex_lock(&cat_lock);
	for (size_t i = 0; i < assets[kind].num; i++) {
		if (!strcmp(assets[kind].array[i].id, id)) {
			stage_strlcpy(path, assets[kind].array[i].path, cap);
			ok = true;
			break;
		}
	}
	pthread_mutex_unlock(&cat_lock);
	return ok;
}
