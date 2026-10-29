/*
 * stage-sample.c — sample scene collection import (stage-sample.h).
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <string.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>

#include "stage-core.h"
#include "stage-sample.h"

bool stage_unique_name(const char *base, stage_name_taken_fn taken, void *ud, char *out, size_t cap)
{
	for (int i = 1; i <= 999 && cap; i++) {
		int n = i == 1 ? snprintf(out, cap, "%s", base) : snprintf(out, cap, "%s %d", base, i);
		if (n < 0 || (size_t)n >= cap)
			break;
		if (!taken(out, ud))
			return true;
	}
	if (cap)
		out[0] = '\0';
	return false;
}

static bool path_is_absolute(const char *p)
{
	return p[0] == '/' || p[0] == '\\' ||
	       (((p[0] | 0x20) >= 'a' && (p[0] | 0x20) <= 'z') && p[1] == ':');
}

/* settings.file relative -> "<dir>/<file>". 1 when rewritten. */
static int rewrite_file(obs_data_t *settings, const char *dir)
{
	const char *f = settings ? obs_data_get_string(settings, "file") : "";
	if (!*f || path_is_absolute(f))
		return 0;
	struct dstr p = {0};
	dstr_copy(&p, dir);
	while (p.len && (p.array[p.len - 1] == '/' || p.array[p.len - 1] == '\\'))
		dstr_resize(&p, p.len - 1);
	dstr_cat_ch(&p, '/');
	dstr_cat(&p, f);
	dstr_replace(&p, "\\", "/");
	obs_data_set_string(settings, "file", p.array);
	dstr_free(&p);
	return 1;
}

static void drop_filter_uuids(obs_data_t *source)
{
	obs_data_array_t *filters = obs_data_get_array(source, "filters");
	for (size_t i = 0; i < obs_data_array_count(filters); i++) {
		obs_data_t *f = obs_data_array_item(filters, i);
		obs_data_erase(f, "uuid");
		obs_data_release(f);
	}
	obs_data_array_release(filters);
}

struct sample_src {
	char old_uuid[STAGE_SCENE_UUID_MAX];
	char old_name[STAGE_SAMPLE_NAME_MAX];
	char uuid[STAGE_SCENE_UUID_MAX];
	char name[STAGE_SAMPLE_NAME_MAX];
};

struct taken_ctx {
	stage_name_taken_fn taken;
	void *ud;
	const struct sample_src *srcs;
	size_t n; /* sources named so far */
};

static bool batch_taken(const char *name, void *ud)
{
	const struct taken_ctx *c = ud;
	for (size_t i = 0; i < c->n; i++)
		if (strcmp(c->srcs[i].name, name) == 0)
			return true;
	return c->taken && c->taken(name, c->ud);
}

/* Points the items of one scene (or group) source at the renamed sources. */
static bool rewrite_items(obs_data_t *source, const struct sample_src *srcs, size_t n,
			  struct sj_err *e)
{
	const char *id = obs_data_get_string(source, "id");
	if (strcmp(id, "scene") != 0 && strcmp(id, "group") != 0)
		return true;
	obs_data_t *settings = obs_data_get_obj(source, "settings");
	obs_data_array_t *items = obs_data_get_array(settings, "items");
	bool ok = true;
	for (size_t i = 0; ok && i < obs_data_array_count(items); i++) {
		obs_data_t *it = obs_data_array_item(items, i);
		const char *ref = obs_data_get_string(it, "source_uuid");
		const char *name = obs_data_get_string(it, "name");
		const struct sample_src *hit = NULL;
		for (size_t j = 0; j < n && !hit; j++)
			if (*ref ? strcmp(srcs[j].old_uuid, ref) == 0 : strcmp(srcs[j].old_name, name) == 0)
				hit = &srcs[j];
		if (hit) {
			obs_data_set_string(it, "source_uuid", hit->uuid);
			obs_data_set_string(it, "name", hit->name);
		} else {
			sj_fail(e, "scene '%.64s': item '%.64s' is not in the collection",
				obs_data_get_string(source, "name"), name);
			ok = false;
		}
		obs_data_release(it);
	}
	obs_data_array_release(items);
	obs_data_release(settings);
	return ok;
}

int stage_sample_prepare(obs_data_t *col, const char *dir, stage_name_taken_fn taken, void *ud,
			 struct sj_err *e)
{
	obs_data_array_t *arr = obs_data_get_array(col, "sources");
	obs_data_array_t *groups = obs_data_get_array(col, "groups");
	size_t n = obs_data_array_count(arr);
	bool has_groups = obs_data_array_count(groups) > 0;
	obs_data_array_release(groups);
	struct sample_src *srcs = NULL;
	int files = -1;

	if (!n)
		sj_fail(e, "the collection has no sources");
	else if (has_groups)
		sj_fail(e, "groups are not supported");
	else if (n > STAGE_SAMPLE_MAX_SOURCES)
		sj_fail(e, "more than %d sources", STAGE_SAMPLE_MAX_SOURCES);
	else
		srcs = bzalloc(sizeof(*srcs) * n);
	if (!srcs)
		goto out;
	struct taken_ctx tc = {taken, ud, srcs, 0};
	int rewritten = 0;
	for (size_t i = 0; i < n; i++) {
		obs_data_t *s = obs_data_array_item(arr, i);
		const char *name = obs_data_get_string(s, "name");
		const char *uuid = obs_data_get_string(s, "uuid");
		struct sample_src *d = &srcs[i];
		tc.n = i;
		bool ok = *name && strlen(name) < sizeof(d->old_name) && strlen(uuid) < sizeof(d->old_uuid);
		if (ok) {
			stage_strlcpy(d->old_name, name, sizeof(d->old_name));
			stage_strlcpy(d->old_uuid, uuid, sizeof(d->old_uuid));
			ok = stage_unique_name(name, batch_taken, &tc, d->name, sizeof(d->name));
		}
		if (!ok) {
			sj_fail(e, "source %zu has no usable name", i);
			obs_data_release(s);
			goto out;
		}
		char *nu = os_generate_uuid();
		stage_strlcpy(d->uuid, nu, sizeof(d->uuid));
		bfree(nu);
		obs_data_set_string(s, "name", d->name);
		obs_data_set_string(s, "uuid", d->uuid);
		obs_data_t *settings = obs_data_get_obj(s, "settings");
		rewritten += rewrite_file(settings, dir);
		obs_data_release(settings);
		drop_filter_uuids(s);
		obs_data_release(s);
	}
	for (size_t i = 0; i < n; i++) {
		obs_data_t *s = obs_data_array_item(arr, i);
		bool ok = rewrite_items(s, srcs, n, e);
		obs_data_release(s);
		if (!ok)
			goto out;
	}
	files = rewritten;
out:
	bfree(srcs);
	obs_data_array_release(arr);
	return files;
}

static bool obs_name_taken(const char *name, void *ud)
{
	(void)ud;
	obs_source_t *s = obs_get_source_by_name(name);
	obs_source_release(s);
	return s != NULL;
}

static void loaded_cb(void *ud, obs_source_t *source)
{
	obs_source_t **scene = ud;
	if (!*scene && obs_scene_from_source(source))
		*scene = obs_source_get_ref(source);
	/* Types register under their versioned id (color_source_v3); a missing
	 * plugin's source keeps the stored id as both. */
	if (!obs_source_get_display_name(obs_source_get_id(source)))
		SLOG_INFO("sample: '%s' needs the source type '%s' (another plugin)",
			  obs_source_get_name(source), obs_source_get_unversioned_id(source));
}

obs_source_t *stage_sample_import(const char *json_path, struct sj_err *e)
{
	char *path = os_get_abs_path_ptr(json_path);
	obs_data_t *col = path ? obs_data_create_from_json_file(path) : NULL;
	obs_source_t *scene = NULL;
	if (!col) {
		sj_fail(e, "cannot read %.128s", json_path);
		goto out;
	}
	char *slash = strrchr(path, '/');
	char *bslash = strrchr(path, '\\');
	if (bslash > slash)
		slash = bslash;
	if (slash)
		*slash = '\0';
	if (stage_sample_prepare(col, path, obs_name_taken, NULL, e) < 0)
		goto out;
	obs_data_array_t *sources = obs_data_get_array(col, "sources");
	obs_load_sources(sources, loaded_cb, &scene);
	obs_data_array_release(sources);
	if (!scene)
		sj_fail(e, "the collection has no scene");
out:
	obs_data_release(col);
	bfree(path);
	return scene;
}
