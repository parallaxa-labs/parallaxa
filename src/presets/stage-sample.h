/*
 * stage-sample.h — Tools > Parallaxa: Import sample scene
 * (data/presets/samples/parallaxa_sample.json, docs/ux/stage-plugin-ux.md
 * section 4).
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * OBS's own Scene Collection > Import keeps the relative image paths of the
 * sample and asks the user to find them (Missing Files). The import here
 * loads the collection's sources into the current collection instead, with
 * every relative "file" setting made absolute in the installed data folder,
 * new names where an existing source already uses one ("Background 2") and
 * new uuids, so importing twice never touches the first copy.
 */
#ifndef STAGE_SAMPLE_H
#define STAGE_SAMPLE_H

#include <obs.h>
#include <stdbool.h>
#include <stddef.h>

#include "stage-json.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Module data path of the sample (obs_module_file). */
#define STAGE_SAMPLE_FILE "presets/samples/parallaxa_sample.json"
/* Limits of stage_sample_prepare (the sample has 4 sources). */
#define STAGE_SAMPLE_MAX_SOURCES 32
#define STAGE_SAMPLE_NAME_MAX 256

typedef bool (*stage_name_taken_fn)(const char *name, void *ud);

/* base when taken() rejects it, else "base 2", "base 3", ... "base 999"
 * (the frontend's own duplicate style). False ("" in out) when none is free
 * or the name does not fit in cap bytes. */
bool stage_unique_name(const char *base, stage_name_taken_fn taken, void *ud, char *out, size_t cap);

/* Rewrites a parsed scene collection (its "sources" array) in place for
 * loading next to the current collection:
 *   - a relative "file" source setting becomes "<dir>/<file>", slashes
 *     forward (absolute paths stay as they are);
 *   - each source gets a name that taken() and no earlier source of the
 *     collection use, and a new uuid;
 *   - scene items follow their source (source_uuid and name); filters drop
 *     their uuid so libobs makes new ones.
 * Returns the number of rewritten paths, or -1 (e->msg) when the collection
 * has no sources, groups, more than STAGE_SAMPLE_MAX_SOURCES sources, a
 * source without a name, or a scene item whose source is not in it. On -1
 * col may be partly rewritten. Pure obs_data: no source is created. */
int stage_sample_prepare(obs_data_t *col, const char *dir, stage_name_taken_fn taken, void *ud,
			 struct sj_err *e);

/* Reads the collection at json_path, prepares it (dir = its folder, made
 * absolute; names checked against obs_get_source_by_name) and creates its
 * sources with obs_load_sources. Returns a new reference to its first scene,
 * or NULL (e->msg). UI thread (the frontend adds the scene to its list from
 * the source_create signal). */
obs_source_t *stage_sample_import(const char *json_path, struct sj_err *e);

#ifdef __cplusplus
}
#endif

#endif /* STAGE_SAMPLE_H */
