/*
 * stage-cam-moves.c — camera move table: compiled-in built-ins plus the move
 * files of data/presets/moves and the user's config folder (vendor-api.md
 * section 6.1, schema move.json).
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <obs-module.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <util/dstr.h>
#include <util/platform.h>

#include "stage-camera.h"
#include "stage-core.h"

/* ------------------------------------------------------------------------- */
/* Built-in fallbacks (identical to the shipped data/presets/moves files)    */
/* ------------------------------------------------------------------------- */

#define K0 {0.0f, 0, 0, 0, 0, 0, STAGE_EASE_DEFAULT, 0}
#define CH(mask) (uint8_t)(mask)

struct builtin_move {
	struct stage_move_def def;
	const char *en, *ja, *id;
};

static const struct builtin_move builtins[STAGE_MOVE_COUNT] = {
	{{"zoom_in", 800, true, true, 2,
	  {K0, {1.0f, 0, 0, 0.25f, 0, 0, STAGE_EASE_OUT_CUBIC, CH(STAGE_CH_ZOOM)}}},
	 "Zoom In", "ズームイン", "Perbesar"},
	{{"zoom_out", 800, true, true, 2,
	  {K0, {1.0f, 0, 0, -0.25f, 0, 0, STAGE_EASE_OUT_CUBIC, CH(STAGE_CH_ZOOM)}}},
	 "Zoom Out", "ズームアウト", "Perkecil"},
	{{"pan_left", 1200, true, true, 2,
	  {K0, {1.0f, -0.08f, 0, 0, 0, 0, STAGE_EASE_IN_OUT_CUBIC, CH(STAGE_CH_X)}}},
	 "Pan Left", "左へパン", "Geser Kiri"},
	{{"pan_right", 1200, true, true, 2,
	  {K0, {1.0f, 0.08f, 0, 0, 0, 0, STAGE_EASE_IN_OUT_CUBIC, CH(STAGE_CH_X)}}},
	 "Pan Right", "右へパン", "Geser Kanan"},
	{{"shake", 600, true, false, 3,
	  {{0.0f, 0, 0, 0, 0, 0, STAGE_EASE_DEFAULT, CH(STAGE_CH_SHAKE)},
	   {0.12f, 0, 0, 0, 0, 0.8f, STAGE_EASE_OUT_QUAD, CH(STAGE_CH_SHAKE)},
	   {1.0f, 0, 0, 0, 0, 0, STAGE_EASE_OUT_CUBIC, CH(STAGE_CH_SHAKE)}}},
	 "Shake", "シェイク", "Guncang"},
	{{"punch", 380, true, false, 3,
	  {{0.0f, 0, 0, 0, 0, 0, STAGE_EASE_DEFAULT, CH(STAGE_CH_ZOOM)},
	   {0.3f, 0, 0, 0.18f, 0, 0, STAGE_EASE_OUT_BACK, CH(STAGE_CH_ZOOM)},
	   {1.0f, 0, 0, 0, 0, 0, STAGE_EASE_IN_OUT_QUAD, CH(STAGE_CH_ZOOM)}}},
	 "Punch In", "パンチイン", "Hentakan"},
	{{"dolly", 2500, true, true, 2,
	  {K0, {1.0f, 0, -0.02f, 0.15f, 0, 0, STAGE_EASE_IN_OUT_CUBIC,
		CH(STAGE_CH_Y | STAGE_CH_ZOOM)}}},
	 "Dolly In", "ドリーイン", "Dolly Maju"},
	{{"nod", 700, true, false, 4,
	  {{0.0f, 0, 0, 0, 0, 0, STAGE_EASE_DEFAULT, CH(STAGE_CH_Y | STAGE_CH_ZOOM)},
	   {0.35f, 0, 0.012f, 0.02f, 0, 0, STAGE_EASE_OUT_QUAD, CH(STAGE_CH_Y | STAGE_CH_ZOOM)},
	   {0.7f, 0, -0.004f, 0.005f, 0, 0, STAGE_EASE_IN_OUT_QUAD, CH(STAGE_CH_Y | STAGE_CH_ZOOM)},
	   {1.0f, 0, 0, 0, 0, 0, STAGE_EASE_OUT_QUAD, CH(STAGE_CH_Y | STAGE_CH_ZOOM)}}},
	 "Nod", "うなずき", "Angguk"},
	{{"reset", 700, false, true, 2,
	  {K0, {1.0f, 0, 0, 0, 0, 0, STAGE_EASE_IN_OUT_CUBIC, CH(STAGE_CH_ALL)}}},
	 "Reset Camera", "カメラをリセット", "Atur Ulang Kamera"},
};

/* ------------------------------------------------------------------------- */
/* Table                                                                     */
/* ------------------------------------------------------------------------- */

static pthread_mutex_t moves_lock = PTHREAD_MUTEX_INITIALIZER;
static struct stage_cam_move moves[STAGE_CAM_MOVES_MAX];
static size_t moves_n;

static void fail(char *err, size_t cap, const char *fmt, ...)
{
	if (!err || !cap)
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(err, cap, fmt, ap);
	va_end(ap);
}

static size_t utf8_len(const char *s)
{
	size_t n = 0;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++)
		if ((*p & 0xC0) != 0x80)
			n++;
	return n;
}

static bool is_integral(double v)
{
	return v == (double)(long long)v;
}

/* Strict number read of an obs_data item into [lo, hi]. */
static bool item_num(obs_data_item_t *it, double lo, double hi, double *out)
{
	if (obs_data_item_gettype(it) != OBS_DATA_NUMBER)
		return false;
	double v = obs_data_item_get_double(it);
	if (!(v >= lo && v <= hi))
		return false;
	*out = v;
	return true;
}

static bool parse_name(obs_data_t *nm, struct stage_cam_move *out, char *err, size_t cap)
{
	bool have[3] = {false, false, false};
	char *dst[3] = {out->name_en, out->name_ja, out->name_id};
	static const char *const langs[3] = {"en", "ja", "id"};
	for (obs_data_item_t *it = obs_data_first(nm); it; obs_data_item_next(&it)) {
		const char *k = obs_data_item_get_name(it);
		int idx = -1;
		for (int i = 0; i < 3; i++)
			if (strcmp(k, langs[i]) == 0)
				idx = i;
		if (idx < 0) {
			fail(err, cap, "unknown field 'name.%s'", k);
			obs_data_item_release(&it);
			return false;
		}
		const char *v = obs_data_item_gettype(it) == OBS_DATA_STRING
					? obs_data_item_get_string(it)
					: NULL;
		size_t n = v ? utf8_len(v) : 0;
		if (!v || n < 1 || n > 64 || strlen(v) >= STAGE_CAM_NAME_MAX) {
			fail(err, cap, "'name.%s' must be 1..64 characters", k);
			obs_data_item_release(&it);
			return false;
		}
		stage_strlcpy(dst[idx], v, STAGE_CAM_NAME_MAX);
		have[idx] = true;
	}
	if (!have[0] || !have[1] || !have[2]) {
		fail(err, cap, "'name' needs en, ja and id");
		return false;
	}
	return true;
}

static bool parse_key(obs_data_t *kd, struct stage_key *k, char *err, size_t cap, size_t idx)
{
	stage_key_init(k);
	bool have_t = false;
	for (obs_data_item_t *it = obs_data_first(kd); it; obs_data_item_next(&it)) {
		const char *name = obs_data_item_get_name(it);
		double v = 0.0;
		bool ok = true;
		if (strcmp(name, "t") == 0) {
			ok = item_num(it, 0.0, 1.0, &v);
			k->t = (float)v;
			have_t = ok;
		} else if (strcmp(name, "dx") == 0) {
			ok = item_num(it, -1.0, 1.0, &v);
			k->dx = (float)v;
			k->mask |= STAGE_CH_X;
		} else if (strcmp(name, "dy") == 0) {
			ok = item_num(it, -1.0, 1.0, &v);
			k->dy = (float)v;
			k->mask |= STAGE_CH_Y;
		} else if (strcmp(name, "dzoom") == 0) {
			ok = item_num(it, -0.9, 3.0, &v);
			k->dzoom = (float)v;
			k->mask |= STAGE_CH_ZOOM;
		} else if (strcmp(name, "drot") == 0) {
			ok = item_num(it, STAGE_ROT_MIN, STAGE_ROT_MAX, &v);
			k->drot = (float)v;
			k->mask |= STAGE_CH_ROT;
		} else if (strcmp(name, "shake") == 0) {
			ok = item_num(it, 0.0, 1.0, &v);
			k->shake = (float)v;
			k->mask |= STAGE_CH_SHAKE;
		} else if (strcmp(name, "ease") == 0) {
			int e = obs_data_item_gettype(it) == OBS_DATA_STRING
					? stage_ease_from_name(obs_data_item_get_string(it))
					: -1;
			ok = e >= 0;
			if (ok)
				k->ease = (uint8_t)e;
		} else {
			fail(err, cap, "keys[%zu]: unknown field '%s'", idx, name);
			obs_data_item_release(&it);
			return false;
		}
		if (!ok) {
			fail(err, cap, "keys[%zu]: invalid '%s'", idx, name);
			obs_data_item_release(&it);
			return false;
		}
	}
	if (!have_t) {
		fail(err, cap, "keys[%zu]: missing 't'", idx);
		return false;
	}
	return true;
}

static bool parse_keys(obs_data_array_t *arr, struct stage_move_def *def, char *err, size_t cap)
{
	size_t n = obs_data_array_count(arr);
	if (n < 2 || n > STAGE_MOVE_MAX_KEYS) {
		fail(err, cap, "'keys' must hold 2..%d objects", STAGE_MOVE_MAX_KEYS);
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		obs_data_t *kd = obs_data_array_item(arr, i);
		bool ok = kd && parse_key(kd, &def->keys[i], err, cap, i);
		obs_data_release(kd);
		if (!ok)
			return false;
	}
	def->nkeys = (uint8_t)n;
	return true;
}

static bool parse_move(obs_data_t *d, const char *expect_id, struct stage_cam_move *out, char *err,
		       size_t cap)
{
	memset(out, 0, sizeof(*out));
	struct stage_move_def *def = &out->def;
	def->relative = true; /* schema defaults */
	def->hold = false;
	bool have_id = false, have_name = false, have_dur = false, have_keys = false;

	for (obs_data_item_t *it = obs_data_first(d); it; obs_data_item_next(&it)) {
		const char *k = obs_data_item_get_name(it);
		enum obs_data_type ty = obs_data_item_gettype(it);
		bool ok = true;
		if (strcmp(k, "id") == 0) {
			const char *v = ty == OBS_DATA_STRING ? obs_data_item_get_string(it) : NULL;
			ok = v && stage_id_valid(v);
			if (ok)
				stage_strlcpy(def->id, v, sizeof(def->id));
			have_id = ok;
		} else if (strcmp(k, "name") == 0) {
			obs_data_t *nm = ty == OBS_DATA_OBJECT ? obs_data_item_get_obj(it) : NULL;
			ok = nm && parse_name(nm, out, err, cap);
			obs_data_release(nm);
			if (!ok) {
				if (!nm)
					fail(err, cap, "'name' must be an object");
				obs_data_item_release(&it);
				return false;
			}
			have_name = true;
		} else if (strcmp(k, "duration_ms") == 0) {
			double v = 0;
			ok = item_num(it, STAGE_MOVE_DURATION_MIN, STAGE_MOVE_DURATION_MAX, &v) &&
			     is_integral(v);
			def->duration_ms = (uint32_t)v;
			have_dur = ok;
		} else if (strcmp(k, "relative") == 0 || strcmp(k, "hold") == 0 ||
			   strcmp(k, "pro") == 0) {
			ok = ty == OBS_DATA_BOOLEAN;
			if (ok && k[0] == 'r')
				def->relative = obs_data_item_get_bool(it);
			else if (ok && k[0] == 'h')
				def->hold = obs_data_item_get_bool(it);
		} else if (strcmp(k, "tags") == 0) {
			/* obs_data drops arrays of strings; only the type is checked. */
			ok = ty == OBS_DATA_ARRAY;
		} else if (strcmp(k, "keys") == 0) {
			obs_data_array_t *arr = ty == OBS_DATA_ARRAY ? obs_data_item_get_array(it) : NULL;
			ok = arr && parse_keys(arr, def, err, cap);
			obs_data_array_release(arr);
			if (!ok) {
				if (!arr)
					fail(err, cap, "'keys' must be an array");
				obs_data_item_release(&it);
				return false;
			}
			have_keys = true;
		} else {
			fail(err, cap, "unknown field '%s'", k);
			obs_data_item_release(&it);
			return false;
		}
		if (!ok) {
			fail(err, cap, "invalid '%s'", k);
			obs_data_item_release(&it);
			return false;
		}
	}
	if (!have_id || !have_name || !have_dur || !have_keys) {
		fail(err, cap, "missing %s", !have_id     ? "'id'"
					     : !have_name ? "'name'"
					     : !have_dur  ? "'duration_ms'"
							  : "'keys'");
		return false;
	}
	if (expect_id && strcmp(expect_id, def->id) != 0) {
		fail(err, cap, "id '%s' does not match the file name", def->id);
		return false;
	}
	if (!stage_move_valid(def)) {
		fail(err, cap, "keys must have strictly increasing t in [0, 1]");
		return false;
	}
	out->builtin = stage_move_from_name(def->id) >= 0;
	return true;
}

bool stage_cam_move_parse_file(const char *path, const char *expect_id, struct stage_cam_move *out,
			       char *err, size_t err_size)
{
	if (err && err_size)
		err[0] = '\0';
	obs_data_t *d = obs_data_create_from_json_file_safe(path, "bak");
	if (!d) {
		fail(err, err_size, "not a JSON object");
		return false;
	}
	bool ok = parse_move(d, expect_id, out, err, err_size);
	obs_data_release(d);
	return ok;
}

static int find_locked(const char *id)
{
	for (size_t i = 0; i < moves_n; i++)
		if (strcmp(moves[i].def.id, id) == 0)
			return (int)i;
	return -1;
}

static int cmp_move_id(const void *a, const void *b)
{
	const struct stage_cam_move *x = a, *y = b;
	return strcmp(x->def.id, y->def.id);
}

/* Loads every <id>.json of dir. Shipped files (user = false) may replace a
 * compiled built-in of the same id; user files never replace anything. */
static void load_dir_locked(const char *dir, bool user)
{
	if (!dir || !dir[0])
		return;
	os_dir_t *od = os_opendir(dir);
	if (!od)
		return;
	size_t first_new = moves_n;
	struct os_dirent *ent;
	while ((ent = os_readdir(od)) != NULL) {
		if (ent->directory)
			continue;
		const char *name = ent->d_name;
		size_t len = strlen(name);
		if (len <= 5 || astrcmpi(name + len - 5, ".json") != 0)
			continue;
		char id[STAGE_ID_MAX];
		if (len - 5 >= sizeof(id)) {
			SLOG_WARN("move file '%s' skipped: id too long", name);
			continue;
		}
		memcpy(id, name, len - 5);
		id[len - 5] = '\0';

		struct dstr path = {0};
		dstr_printf(&path, "%s/%s", dir, name);
		struct stage_cam_move mv;
		char err[192];
		bool ok = stage_cam_move_parse_file(path.array, id, &mv, err, sizeof(err));
		dstr_free(&path);
		if (!ok) {
			SLOG_WARN("move file '%s' skipped: %s", name, err);
			continue;
		}
		mv.user = user;
		int at = find_locked(mv.def.id);
		if (at >= 0) {
			if (!user && moves[at].builtin && !moves[at].user) {
				moves[at] = mv; /* shipped file wins over the compiled copy */
				moves[at].builtin = true;
			} else {
				SLOG_WARN("move file '%s' skipped: id '%s' already exists", name,
					  mv.def.id);
			}
			continue;
		}
		if (moves_n >= STAGE_CAM_MOVES_MAX) {
			SLOG_WARN("move file '%s' skipped: more than %d moves", name,
				  STAGE_CAM_MOVES_MAX);
			continue;
		}
		moves[moves_n++] = mv;
	}
	os_closedir(od);
	/* readdir order is unspecified: keep each folder's moves sorted. */
	if (moves_n > first_new)
		qsort(&moves[first_new], moves_n - first_new, sizeof(moves[0]), cmp_move_id);
}

size_t stage_cam_moves_load(const char *builtin_dir, const char *user_dir)
{
	pthread_mutex_lock(&moves_lock);
	moves_n = 0;
	for (size_t i = 0; i < STAGE_MOVE_COUNT; i++) {
		struct stage_cam_move *m = &moves[moves_n++];
		memset(m, 0, sizeof(*m));
		m->def = builtins[i].def;
		stage_strlcpy(m->name_en, builtins[i].en, sizeof(m->name_en));
		stage_strlcpy(m->name_ja, builtins[i].ja, sizeof(m->name_ja));
		stage_strlcpy(m->name_id, builtins[i].id, sizeof(m->name_id));
		m->builtin = true;
		if (!stage_move_valid(&m->def))
			SLOG_ERR("built-in move '%s' is invalid", m->def.id);
	}
	load_dir_locked(builtin_dir, false);
	load_dir_locked(user_dir, true);
	size_t n = moves_n;
	pthread_mutex_unlock(&moves_lock);
	return n;
}

void stage_cam_moves_free(void)
{
	pthread_mutex_lock(&moves_lock);
	moves_n = 0;
	pthread_mutex_unlock(&moves_lock);
}

size_t stage_cam_moves_count(void)
{
	pthread_mutex_lock(&moves_lock);
	size_t n = moves_n;
	pthread_mutex_unlock(&moves_lock);
	return n;
}

bool stage_cam_moves_at(size_t i, struct stage_cam_move *out)
{
	bool ok = false;
	pthread_mutex_lock(&moves_lock);
	if (i < moves_n) {
		*out = moves[i];
		ok = true;
	}
	pthread_mutex_unlock(&moves_lock);
	return ok;
}

bool stage_cam_moves_find(const char *id, struct stage_move_def *out)
{
	if (!id)
		return false;
	bool ok = false;
	pthread_mutex_lock(&moves_lock);
	int at = find_locked(id);
	if (at >= 0) {
		*out = moves[at].def;
		ok = true;
	}
	pthread_mutex_unlock(&moves_lock);
	return ok;
}

const char *stage_cam_move_local_name(const struct stage_cam_move *m)
{
	const char *loc = obs_get_locale();
	if (loc && strncmp(loc, "ja", 2) == 0 && m->name_ja[0])
		return m->name_ja;
	if (loc && strncmp(loc, "id", 2) == 0 && m->name_id[0])
		return m->name_id;
	return m->name_en[0] ? m->name_en : m->def.id;
}
