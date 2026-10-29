/*
 * stage-json.h — strict, typed reads from obs_data (vendor requests and
 * preset files).
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * obs_data_get_*() silently returns 0/""/false for missing keys and wrong
 * types. The protocol is strict (vendor-api.md section 2: unknown fields,
 * wrong types and out-of-range values are errors), so every read goes
 * through these helpers, which tell "absent" from "invalid".
 *
 * Getter result: SJ_ABSENT (0) when the key is missing, SJ_OK (1) when it is
 * present and valid (*out set), SJ_BAD (-1) when present but invalid
 * (e->msg describes why, in short English).
 *
 * Transport notes (obs-websocket converts the request JSON to obs_data
 * before calling a vendor, src/utils/Json.cpp):
 *   - array elements that are not objects are dropped;
 *   - current obs-websocket stores a JSON null as an OBJECT item holding a
 *     NULL obs_data (sj_is_null() detects it); older releases drop nulls,
 *     so a null may also arrive as an absent key.
 */
#ifndef STAGE_JSON_H
#define STAGE_JSON_H

#include <obs.h>
#include <stdbool.h>
#include <stddef.h>

#include "stage-protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SJ_ABSENT 0
#define SJ_OK 1
#define SJ_BAD (-1)

struct sj_err {
	char msg[192];
};

void sj_fail(struct sj_err *e, const char *fmt, ...);

/* False (and e->msg = "unknown field 'x'") when d holds a key that is not in
 * keys[0..n). NULL d is valid (no keys). */
bool sj_only_keys(obs_data_t *d, const char *const *keys, size_t n, struct sj_err *e);

/* Number (int or double) in [lo, hi]. */
int sj_num(obs_data_t *d, const char *key, double lo, double hi, double *out, struct sj_err *e);
/* Integer in [lo, hi]. A JSON real with an integral value (600.0) counts. */
int sj_int(obs_data_t *d, const char *key, long long lo, long long hi, long long *out,
	   struct sj_err *e);
int sj_bool(obs_data_t *d, const char *key, bool *out, struct sj_err *e);
/* String of at most max_bytes bytes (0 = any). *out points into d. */
int sj_str(obs_data_t *d, const char *key, size_t max_bytes, const char **out, struct sj_err *e);
/* True when key holds a JSON null (see the transport notes). */
bool sj_is_null(obs_data_t *d, const char *key);
/* Object (not null); *out is a new reference the caller releases. */
int sj_obj(obs_data_t *d, const char *key, obs_data_t **out, struct sj_err *e);
/* String that must be one of names[0..n); *out = its index. */
int sj_enum(obs_data_t *d, const char *key, const char *const *names, size_t n, int *out,
	    struct sj_err *e);
/* Protocol id ([a-z0-9][a-z0-9_-]{0,30}); allow_empty also accepts "". */
int sj_id(obs_data_t *d, const char *key, bool allow_empty, char out[STAGE_ID_MAX],
	  struct sj_err *e);

/* Localized name/text object {en, ja, id}: all three required, each 1..max_cp
 * Unicode code points (min_cp 0 allows ""), valid UTF-8. Copies into the
 * three buffers (cap bytes each). */
int sj_localized(obs_data_t *d, const char *key, size_t min_cp, size_t max_cp, char *en, char *ja,
		 char *id, size_t cap, struct sj_err *e);

/* Number of Unicode code points in a valid UTF-8 string, or (size_t)-1 when
 * it is not valid UTF-8. */
size_t sj_utf8_len(const char *s);

/* Appends `"key":value` to a compact JSON object being built in buf (cap
 * bytes, NUL-terminated, starts as "" and is closed by sj_json_close).
 * Numbers are printed with up to 4 decimals, trailing zeros trimmed. Return
 * false when it would not fit (buf unchanged). */
bool sj_json_add_num(char *buf, size_t cap, const char *key, double v);
bool sj_json_add_str(char *buf, size_t cap, const char *key, const char *v);
/* Appends the closing '}' ("{}" for an empty object). False if it does not fit. */
bool sj_json_close(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* STAGE_JSON_H */
