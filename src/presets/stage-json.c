/*
 * stage-json.c — strict typed reads from obs_data, compact JSON writer.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "stage-json.h"

void sj_fail(struct sj_err *e, const char *fmt, ...)
{
	if (!e)
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(e->msg, sizeof(e->msg), fmt, ap);
	va_end(ap);
}

bool sj_only_keys(obs_data_t *d, const char *const *keys, size_t n, struct sj_err *e)
{
	if (!d)
		return true;
	for (obs_data_item_t *it = obs_data_first(d); it; obs_data_item_next(&it)) {
		const char *name = obs_data_item_get_name(it);
		bool known = false;
		for (size_t i = 0; i < n && !known; i++)
			known = strcmp(keys[i], name) == 0;
		if (!known) {
			sj_fail(e, "unknown field '%.64s'", name);
			obs_data_item_release(&it);
			return false;
		}
	}
	return true;
}

/* Looks up key; returns the item (caller releases) or NULL when absent. */
static obs_data_item_t *item_get(obs_data_t *d, const char *key)
{
	if (!d || !key)
		return NULL;
	obs_data_item_t *it = obs_data_item_byname(d, key);
	if (it && !obs_data_item_has_user_value(it)) {
		obs_data_item_release(&it);
		return NULL;
	}
	return it;
}

int sj_num(obs_data_t *d, const char *key, double lo, double hi, double *out, struct sj_err *e)
{
	obs_data_item_t *it = item_get(d, key);
	if (!it)
		return SJ_ABSENT;
	int r = SJ_BAD;
	if (obs_data_item_gettype(it) != OBS_DATA_NUMBER) {
		sj_fail(e, "'%s' must be a number", key);
	} else {
		double v = obs_data_item_numtype(it) == OBS_DATA_NUM_INT
				   ? (double)obs_data_item_get_int(it)
				   : obs_data_item_get_double(it);
		if (!isfinite(v) || v < lo || v > hi) {
			sj_fail(e, "'%s' must be in %g..%g", key, lo, hi);
		} else {
			if (out)
				*out = v;
			r = SJ_OK;
		}
	}
	obs_data_item_release(&it);
	return r;
}

int sj_int(obs_data_t *d, const char *key, long long lo, long long hi, long long *out,
	   struct sj_err *e)
{
	obs_data_item_t *it = item_get(d, key);
	if (!it)
		return SJ_ABSENT;
	int r = SJ_BAD;
	if (obs_data_item_gettype(it) != OBS_DATA_NUMBER) {
		sj_fail(e, "'%s' must be an integer", key);
	} else {
		long long v = 0;
		bool ok = true;
		if (obs_data_item_numtype(it) == OBS_DATA_NUM_INT) {
			v = obs_data_item_get_int(it);
		} else {
			double dv = obs_data_item_get_double(it);
			ok = isfinite(dv) && dv == floor(dv) && fabs(dv) < 9.0e15;
			v = ok ? (long long)dv : 0;
		}
		if (!ok) {
			sj_fail(e, "'%s' must be an integer", key);
		} else if (v < lo || v > hi) {
			sj_fail(e, "'%s' must be in %lld..%lld", key, lo, hi);
		} else {
			if (out)
				*out = v;
			r = SJ_OK;
		}
	}
	obs_data_item_release(&it);
	return r;
}

int sj_bool(obs_data_t *d, const char *key, bool *out, struct sj_err *e)
{
	obs_data_item_t *it = item_get(d, key);
	if (!it)
		return SJ_ABSENT;
	int r = SJ_BAD;
	if (obs_data_item_gettype(it) != OBS_DATA_BOOLEAN) {
		sj_fail(e, "'%s' must be a boolean", key);
	} else {
		if (out)
			*out = obs_data_item_get_bool(it);
		r = SJ_OK;
	}
	obs_data_item_release(&it);
	return r;
}

int sj_str(obs_data_t *d, const char *key, size_t max_bytes, const char **out, struct sj_err *e)
{
	obs_data_item_t *it = item_get(d, key);
	if (!it)
		return SJ_ABSENT;
	int r = SJ_BAD;
	if (obs_data_item_gettype(it) != OBS_DATA_STRING) {
		sj_fail(e, "'%s' must be a string", key);
	} else {
		const char *s = obs_data_item_get_string(it);
		if (!s)
			s = "";
		if (max_bytes && strlen(s) > max_bytes) {
			sj_fail(e, "'%s' is too long", key);
		} else {
			/* The string is owned by d, which outlives the item ref. */
			if (out)
				*out = s;
			r = SJ_OK;
		}
	}
	obs_data_item_release(&it);
	return r;
}

bool sj_is_null(obs_data_t *d, const char *key)
{
	obs_data_item_t *it = item_get(d, key);
	if (!it)
		return false;
	bool null = false;
	if (obs_data_item_gettype(it) == OBS_DATA_OBJECT) {
		obs_data_t *o = obs_data_item_get_obj(it);
		null = o == NULL;
		obs_data_release(o);
	}
	obs_data_item_release(&it);
	return null;
}

int sj_obj(obs_data_t *d, const char *key, obs_data_t **out, struct sj_err *e)
{
	obs_data_item_t *it = item_get(d, key);
	if (!it)
		return SJ_ABSENT;
	int r = SJ_BAD;
	obs_data_t *o = obs_data_item_gettype(it) == OBS_DATA_OBJECT ? obs_data_item_get_obj(it) : NULL;
	if (!o) {
		sj_fail(e, "'%s' must be an object", key);
	} else {
		if (out)
			*out = o;
		else
			obs_data_release(o);
		r = SJ_OK;
	}
	obs_data_item_release(&it);
	return r;
}

int sj_enum(obs_data_t *d, const char *key, const char *const *names, size_t n, int *out,
	    struct sj_err *e)
{
	const char *s = NULL;
	int r = sj_str(d, key, 64, &s, e);
	if (r != SJ_OK)
		return r;
	int idx = stage_name_index(names, n, s);
	if (idx < 0) {
		sj_fail(e, "'%s' has an unknown value '%.32s'", key, s);
		return SJ_BAD;
	}
	if (out)
		*out = idx;
	return SJ_OK;
}

int sj_id(obs_data_t *d, const char *key, bool allow_empty, char out[STAGE_ID_MAX],
	  struct sj_err *e)
{
	const char *s = NULL;
	int r = sj_str(d, key, 256, &s, e);
	if (r != SJ_OK)
		return r;
	if (!(allow_empty && !s[0]) && !stage_id_valid(s)) {
		sj_fail(e, "'%s' is not a valid id", key);
		return SJ_BAD;
	}
	if (out) {
		strncpy(out, s, STAGE_ID_MAX - 1);
		out[STAGE_ID_MAX - 1] = '\0';
	}
	return SJ_OK;
}

size_t sj_utf8_len(const char *s)
{
	if (!s)
		return (size_t)-1;
	const unsigned char *p = (const unsigned char *)s;
	size_t n = 0;
	while (*p) {
		unsigned int c = *p, cp;
		int extra;
		if (c < 0x80) {
			cp = c;
			extra = 0;
		} else if (c >= 0xC2 && c <= 0xDF) {
			cp = c & 0x1F;
			extra = 1;
		} else if (c >= 0xE0 && c <= 0xEF) {
			cp = c & 0x0F;
			extra = 2;
		} else if (c >= 0xF0 && c <= 0xF4) {
			cp = c & 0x07;
			extra = 3;
		} else {
			return (size_t)-1;
		}
		p++;
		for (int i = 0; i < extra; i++, p++) {
			if ((*p & 0xC0) != 0x80)
				return (size_t)-1;
			cp = (cp << 6) | (*p & 0x3F);
		}
		if ((extra == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) ||
		    (extra == 3 && (cp < 0x10000 || cp > 0x10FFFF)))
			return (size_t)-1;
		n++;
	}
	return n;
}

int sj_localized(obs_data_t *d, const char *key, size_t min_cp, size_t max_cp, char *en, char *ja,
		 char *id, size_t cap, struct sj_err *e)
{
	obs_data_t *o = NULL;
	int r = sj_obj(d, key, &o, e);
	if (r != SJ_OK)
		return r;
	static const char *const langs[] = {"en", "ja", "id"};
	char *outs[3] = {en, ja, id};
	r = SJ_OK;
	if (!sj_only_keys(o, langs, 3, e)) {
		r = SJ_BAD;
		goto done;
	}
	for (int i = 0; i < 3; i++) {
		const char *s = NULL;
		int sr = sj_str(o, langs[i], 0, &s, e);
		if (sr != SJ_OK) {
			if (sr == SJ_ABSENT)
				sj_fail(e, "'%s.%s' is required", key, langs[i]);
			r = SJ_BAD;
			goto done;
		}
		size_t cp = sj_utf8_len(s);
		if (cp == (size_t)-1 || cp < min_cp || cp > max_cp || strlen(s) >= cap) {
			sj_fail(e, "'%s.%s' must be %zu..%zu characters", key, langs[i], min_cp, max_cp);
			r = SJ_BAD;
			goto done;
		}
		if (outs[i])
			memcpy(outs[i], s, strlen(s) + 1);
	}
done:
	obs_data_release(o);
	return r;
}

/* ------------------------------------------------------------------------- */
/* Compact JSON writer                                                       */
/* ------------------------------------------------------------------------- */

static bool json_append(char *buf, size_t cap, const char *key, const char *val, bool quote)
{
	size_t len = strlen(buf);
	char tmp[160];
	int n = snprintf(tmp, sizeof(tmp), "%s\"%s\":%s%s%s", len ? "," : "{", key,
			 quote ? "\"" : "", val, quote ? "\"" : "");
	if (n < 0 || (size_t)n >= sizeof(tmp) || len + (size_t)n + 2 > cap) /* +2: '}' + NUL */
		return false;
	memcpy(buf + len, tmp, (size_t)n + 1);
	return true;
}

bool sj_json_add_num(char *buf, size_t cap, const char *key, double v)
{
	if (!isfinite(v))
		v = 0.0;
	char num[48];
	snprintf(num, sizeof(num), "%.4f", v);
	/* trim trailing zeros and a dangling '.' */
	char *dot = strchr(num, '.');
	if (dot) {
		char *end = num + strlen(num) - 1;
		while (end > dot && *end == '0')
			*end-- = '\0';
		if (end == dot)
			*end = '\0';
	}
	if (strcmp(num, "-0") == 0)
		strcpy(num, "0");
	return json_append(buf, cap, key, num, false);
}

bool sj_json_add_str(char *buf, size_t cap, const char *key, const char *v)
{
	char esc[96];
	size_t o = 0;
	for (const char *p = v ? v : ""; *p && o + 7 < sizeof(esc); p++) {
		unsigned char c = (unsigned char)*p;
		if (c == '"' || c == '\\') {
			esc[o++] = '\\';
			esc[o++] = (char)c;
		} else if (c < 0x20) {
			o += (size_t)snprintf(esc + o, sizeof(esc) - o, "\\u%04x", c);
		} else {
			esc[o++] = (char)c;
		}
	}
	esc[o] = '\0';
	return json_append(buf, cap, key, esc, true);
}

bool sj_json_close(char *buf, size_t cap)
{
	size_t len = strlen(buf);
	if (!len) {
		if (cap < 3)
			return false;
		memcpy(buf, "{}", 3);
		return true;
	}
	if (len + 2 > cap)
		return false;
	buf[len] = '}';
	buf[len + 1] = '\0';
	return true;
}
