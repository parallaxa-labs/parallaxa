/*
 * vendor_test.c — headless test of the obs-websocket vendor, presets and
 * hotkey modules against the installed OBS's obs.dll (no OBS UI, no
 * obs-websocket, nothing written outside the build folder).
 *
 * The vendor, presets and core sources are compiled into this executable;
 * a fake Camera Rig and a fake Parallaxa FX are registered with the core
 * registries, real scenes are created in libobs, and every request of
 * docs/protocol/vendor-api.md is run through stage_vendor_dispatch() (the
 * function obs-websocket's callbacks call). Events go to a test sink and
 * are pumped with a fake clock. A threaded stress test runs camera_move
 * from 4 threads while a "graphics" thread drains the queue and pumps
 * events.
 *
 * Usage: vendor_test <data> <writable scratch folder> [dump.jsonl]
 * PATH must contain the OBS bin\64bit folder (ctest sets it).
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <obs-module.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/text-lookup.h>
#include <util/threading.h>

#include "presets/stage-json.h"
#include "presets/stage-presets.h"
#include "presets/stage-sample.h"
#include "presets/stage-target.h"
#include "stage-core.h"
#include "stage-frontend.h"
#include "stage-modules.h"
#include "vendor/stage-vendor.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(STAGE_MODULE_NAME, "en-US")

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static void sleep_ms(unsigned ms)
{
	Sleep(ms);
}
#else
#include <unistd.h>
static void sleep_ms(unsigned ms)
{
	usleep(ms * 1000);
}
#endif

/* ------------------------------------------------------------------------- */
/* Tiny test harness                                                          */
/* ------------------------------------------------------------------------- */

static int failures, checks;

#define CHECK(cond, ...)                                                    \
	do {                                                                \
		checks++;                                                   \
		if (!(cond)) {                                              \
			failures++;                                         \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__);                       \
			fprintf(stderr, "\n");                              \
		}                                                           \
	} while (0)

static int stage_warnings, stage_errors, sample_missing;
static char last_warning[512], last_missing[512];

static void log_handler(int level, const char *fmt, va_list args, void *param)
{
	(void)param;
	char msg[1024];
	vsnprintf(msg, sizeof(msg), fmt, args);
	if (!strstr(msg, "[parallaxa]"))
		return;
	if (strstr(msg, "needs the source type")) {
		sample_missing++;
		snprintf(last_missing, sizeof(last_missing), "%s", msg);
	}
	if (level <= LOG_ERROR)
		stage_errors++;
	else if (level == LOG_WARNING) {
		stage_warnings++;
		snprintf(last_warning, sizeof(last_warning), "%s", msg);
	}
	if (level <= LOG_INFO)
		printf("  log: %s\n", msg);
}

/* Optional JSON-lines dump of every response and event (argv[3]); ctest
 * validates it against docs/protocol/schemas with smoke.mjs --validate. */
static FILE *dump;
/* Responses (main thread) and events (event thread) share the file. */
static pthread_mutex_t dump_lock = PTHREAD_MUTEX_INITIALIZER;

static void dump_json(const char *kind, const char *name, obs_data_t *d)
{
	if (!dump)
		return;
	pthread_mutex_lock(&dump_lock);
	fprintf(dump, "{\"%s\":\"%s\",\"data\":", kind, name);
	for (const char *j = obs_data_get_json(d); *j; j++) {
		if (*j != '\n' && *j != '\r')
			fputc(*j, dump);
	}
	fputs("}\n", dump);
	pthread_mutex_unlock(&dump_lock);
}

/* Runs one request from JSON text; the caller releases the response. */
static obs_data_t *call_data(const char *type, obs_data_t *req)
{
	obs_data_t *res = obs_data_create();
	stage_vendor_dispatch(type, req, res);
	dump_json("response", type, res);
	return res;
}

static obs_data_t *call(const char *type, const char *json)
{
	obs_data_t *req = obs_data_create_from_json(json);
	if (!req)
		fprintf(stderr, "bad test JSON: %s\n", json);
	obs_data_t *res = call_data(type, req);
	obs_data_release(req);
	return res;
}

static bool is_ok(obs_data_t *res)
{
	return obs_data_has_user_value(res, "ok") && obs_data_get_bool(res, "ok");
}

static const char *code_of(obs_data_t *res)
{
	return obs_data_has_user_value(res, "code") ? obs_data_get_string(res, "code") : "";
}

/* Expects {ok:false, code, error} with exactly these keys. */
static void expect_err(const char *type, const char *json, const char *code)
{
	obs_data_t *res = call(type, json);
	size_t keys = 0;
	for (obs_data_item_t *it = obs_data_first(res); it; obs_data_item_next(&it))
		keys++;
	CHECK(!is_ok(res) && strcmp(code_of(res), code) == 0 && keys == 3 &&
		      obs_data_get_string(res, "error")[0],
	      "%s %s: want %s, got %s", type, json, code, obs_data_get_json(res));
	obs_data_release(res);
}

static bool has(obs_data_t *d, const char *key)
{
	obs_data_item_t *it = obs_data_item_byname(d, key);
	bool ok = it && obs_data_item_has_user_value(it);
	obs_data_item_release(&it);
	return ok;
}

/* Checks that d has exactly the listed keys. */
static bool keys_are(obs_data_t *d, const char *const *keys, size_t n)
{
	size_t count = 0;
	for (obs_data_item_t *it = obs_data_first(d); it; obs_data_item_next(&it))
		count++;
	if (count != n)
		return false;
	for (size_t i = 0; i < n; i++) {
		if (!has(d, keys[i]))
			return false;
	}
	return true;
}
#define KEYS_ARE(d, ...)                                                     \
	keys_are(d, (const char *const[]){__VA_ARGS__},                      \
		 sizeof((const char *const[]){__VA_ARGS__}) / sizeof(const char *))

/* Drains every queued command into out (cap entries). */
struct drained {
	struct stage_cmd c[64];
	size_t n;
};

static void drain_cb(const struct stage_cmd *cmd, void *ud)
{
	struct drained *d = ud;
	if (d->n < 64)
		d->c[d->n++] = *cmd;
}

static size_t drain_all(struct drained *d)
{
	memset(d, 0, sizeof(*d));
	stage_cmdq_drain(NULL, drain_cb, d);
	return d->n;
}

/* ------------------------------------------------------------------------- */
/* Fakes                                                                      */
/* ------------------------------------------------------------------------- */

struct fake_rig {
	pthread_mutex_t lock;
	struct stage_state st;
	struct stage_layer_info layers[8];
	size_t n;
};

static void rig_get_state(void *ud, struct stage_state *out)
{
	struct fake_rig *r = ud;
	pthread_mutex_lock(&r->lock);
	*out = r->st;
	pthread_mutex_unlock(&r->lock);
}

static size_t rig_list_layers(void *ud, struct stage_layer_info *out, size_t cap)
{
	struct fake_rig *r = ud;
	pthread_mutex_lock(&r->lock);
	size_t n = r->n < cap ? r->n : cap;
	memcpy(out, r->layers, n * sizeof(*out));
	pthread_mutex_unlock(&r->lock);
	return n;
}

static const struct stage_rig_ops rig_ops = {rig_get_state, rig_list_layers, NULL};

struct fake_fx {
	struct stage_fx_info info;
};

static void fx_get_info(void *ud, struct stage_fx_info *out)
{
	*out = ((struct fake_fx *)ud)->info;
}

static const struct stage_fx_ops fx_ops = {fx_get_info, NULL};

static struct fake_rig rig;
static struct fake_fx fx;

static void rig_set(void (*fn)(struct stage_state *st, void *p), void *p)
{
	pthread_mutex_lock(&rig.lock);
	fn(&rig.st, p);
	pthread_mutex_unlock(&rig.lock);
}

/* Event sink. */
struct evt {
	char type[32];
	char json[1024];
};
static struct evt events[512];
static size_t nevents;
static pthread_mutex_t evt_lock = PTHREAD_MUTEX_INITIALIZER;

static void sink(const char *type, obs_data_t *data, void *ud)
{
	(void)ud;
	pthread_mutex_lock(&evt_lock);
	if (nevents < 512) {
		snprintf(events[nevents].type, sizeof(events[nevents].type), "%s", type);
		/* Spaces removed: obs_data_get_json() formatting differs between
		 * libobs versions. */
		const char *j = obs_data_get_json(data);
		size_t o = 0;
		for (; *j && o + 1 < sizeof(events[nevents].json); j++) {
			if (*j != ' ' && *j != '\n' && *j != '\t')
				events[nevents].json[o++] = *j;
		}
		events[nevents].json[o] = 0;
		dump_json("event", type, data);
		nevents++;
	}
	pthread_mutex_unlock(&evt_lock);
}

static size_t count_events(const char *type)
{
	size_t n = 0;
	for (size_t i = 0; i < nevents; i++)
		n += strcmp(events[i].type, type) == 0;
	return n;
}

static const struct evt *last_event(const char *type)
{
	for (size_t i = nevents; i-- > 0;) {
		if (strcmp(events[i].type, type) == 0)
			return &events[i];
	}
	return NULL;
}

/* ------------------------------------------------------------------------- */
/* Scene setup                                                                */
/* ------------------------------------------------------------------------- */

static obs_scene_t *main_scene;
static obs_scene_t *layer_scenes[4];
static int64_t item_ids[4];
static char main_uuid[STAGE_SCENE_UUID_MAX];
static char bg_uuid[STAGE_SCENE_UUID_MAX];

static void setup_scenes(void)
{
	static const char *const names[4] = {"BG", "VTube Studio", "FG Desk", "Chat Box"};
	main_scene = obs_scene_create("Just Chatting");
	snprintf(main_uuid, sizeof(main_uuid), "%s",
		 obs_source_get_uuid(obs_scene_get_source(main_scene)));
	pthread_mutex_init(&rig.lock, NULL);
	for (int i = 0; i < 4; i++) {
		layer_scenes[i] = obs_scene_create(names[i]);
		obs_sceneitem_t *item = obs_scene_add(main_scene, obs_scene_get_source(layer_scenes[i]));
		item_ids[i] = obs_sceneitem_get_id(item);
		struct stage_layer_info *l = &rig.layers[i];
		memset(l, 0, sizeof(*l));
		l->item_id = item_ids[i];
		snprintf(l->name, sizeof(l->name), "%s", names[i]);
		snprintf(l->source_uuid, sizeof(l->source_uuid), "%s",
			 obs_source_get_uuid(obs_scene_get_source(layer_scenes[i])));
		l->visible = true;
		l->locked = i == 3;
		l->depth_blur = i == 0 || i == 2;
	}
	rig.n = 4;
	snprintf(bg_uuid, sizeof(bg_uuid), "%s",
		 obs_source_get_uuid(obs_scene_get_source(layer_scenes[0])));

	memset(&rig.st, 0, sizeof(rig.st));
	snprintf(rig.st.scene_uuid, sizeof(rig.st.scene_uuid), "%s", main_uuid);
	rig.st.enabled = true;
	rig.st.zoom = 1.0f;
	rig.st.tzoom = 1.0f;
	rig.st.auto_mode = STAGE_AUTO_DRIFT;
	rig.st.auto_intensity = 0.35f;
	rig.st.focus_depth = 1.0f;
	snprintf(fx.info.preset, sizeof(fx.info.preset), "cozy");
	fx.info.enabled = true;

	stage_core_set_program_scene(main_uuid);
	CHECK(stage_rig_register(main_uuid, &rig_ops, &rig), "rig register");
	CHECK(stage_fx_attach(main_uuid, &fx_ops, &fx), "fx attach");
}

static void teardown_scenes(void)
{
	stage_rig_unregister(main_uuid, &rig);
	stage_fx_detach(main_uuid, &fx);
	for (int i = 0; i < 4; i++)
		obs_scene_release(layer_scenes[i]);
	obs_scene_release(main_scene);
	pthread_mutex_destroy(&rig.lock);
}

/* ------------------------------------------------------------------------- */
/* Tests                                                                      */
/* ------------------------------------------------------------------------- */

static void test_presets(const char *data_dir, const char *user_dir)
{
	char presets[512];
	snprintf(presets, sizeof(presets), "%s/presets", data_dir);
	int warn_before = stage_warnings;
	size_t n = stage_presets_load(presets, user_dir);
	CHECK(n >= 14, "recipes loaded: %zu", n);
	CHECK(stage_warnings == warn_before, "shipped presets produced warnings: %s", last_warning);
	CHECK(stage_move_info_count() >= 15, "moves: %zu", stage_move_info_count());
	CHECK(stage_fx_preset_known("neutral") && stage_fx_preset_known("cozy"), "fx presets");
	CHECK(stage_lut_known("film") && stage_lut_known("identity") && !stage_lut_known("nope"),
	      "luts");
	CHECK(stage_move_known("zoom_in") && stage_move_known("slow_push") && !stage_move_known("spin"),
	      "moves known");

	struct stage_recipe r;
	CHECK(stage_recipe_find("cozy_chat", &r), "cozy_chat");
	CHECK(r.has_role[STAGE_RROLE_BG] && fabs(r.role_depth[STAGE_RROLE_BG] - 3.0) < 1e-9 &&
		      r.has_auto && r.auto_mode == STAGE_AUTO_DRIFT && strcmp(r.fx, "cozy") == 0 &&
		      r.has_dof && r.has_overscan && fabs(r.overscan - 0.08) < 1e-6 && !r.pro && !r.user,
	      "cozy_chat content");
	CHECK(strcmp(r.name.ja, "まったり雑談") == 0, "cozy_chat ja name");
	double v = 0;
	CHECK(stage_fx_preset_param("cozy", "vignette", &v) && fabs(v - 0.25) < 1e-9, "cozy vignette");
	CHECK(stage_fx_preset_param("neutral", "vignette_soft", &v) && fabs(v - 0.5) < 1e-9,
	      "neutral default");

	/* Role guesses (vendor-api.md 4.13). */
	CHECK(stage_guess_role("BG") == STAGE_ROLE_BG, "BG");
	CHECK(stage_guess_role("Room Background") == STAGE_ROLE_BG, "Background");
	CHECK(stage_guess_role("背景イラスト") == STAGE_ROLE_BG, "背景");
	CHECK(stage_guess_role("VTube Studio") == STAGE_ROLE_CHAR, "VTube Studio");
	CHECK(stage_guess_role("VTS capture") == STAGE_ROLE_CHAR, "VTS");
	CHECK(stage_guess_role("Spout2 avatar") == STAGE_ROLE_CHAR, "Spout2");
	CHECK(stage_guess_role("FG Desk") == STAGE_ROLE_FG, "FG Desk");
	CHECK(stage_guess_role("前景") == STAGE_ROLE_FG, "前景");
	CHECK(stage_guess_role("Chat Box") == STAGE_ROLE_NONE, "Chat Box");
	CHECK(stage_guess_role("Bigfoot") == STAGE_ROLE_NONE, "fg must be a word");

	char id[STAGE_ID_MAX];
	stage_recipe_id_from_name("My Cozy Look!", id);
	CHECK(strcmp(id, "my-cozy-look") == 0, "id from name: %s", id);
	stage_recipe_id_from_name("まったり", id);
	CHECK(strncmp(id, "look-", 5) == 0 && stage_id_valid(id), "id from JA name: %s", id);

	/* Invalid user files are skipped with a warning, valid ones load. */
	char dir[600], path[700];
	snprintf(dir, sizeof(dir), "%s/recipes", user_dir);
	os_mkdirs(dir);
	snprintf(path, sizeof(path), "%s/broken.json", dir);
	os_quick_write_utf8_file(path, "{\"id\":\"broken\",\"name\":{\"en\":\"x\"}}", 34, false);
	snprintf(path, sizeof(path), "%s/cozy_chat.json", dir);
	os_quick_write_utf8_file(path, "{}", 2, false);
	warn_before = stage_warnings;
	stage_presets_reload();
	CHECK(stage_warnings == warn_before + 2, "2 bad user recipes -> 2 warnings (%d)",
	      stage_warnings - warn_before);
	CHECK(!stage_recipe_find("broken", NULL), "broken skipped");
	os_unlink(path);
	snprintf(path, sizeof(path), "%s/broken.json", dir);
	os_unlink(path);
}

static void test_requests(void)
{
	struct drained d;
	obs_data_t *res;
	drain_all(&d);

	/* get_info */
	res = call("get_info", "{}");
	CHECK(is_ok(res) && KEYS_ARE(res, "ok", "version", "protocol", "obs", "canvas", "rigs", "fx"),
	      "get_info shape: %s", obs_data_get_json(res));
	CHECK(obs_data_get_int(res, "protocol") == 1 &&
		      strcmp(obs_data_get_string(res, "version"), STAGE_VERSION) == 0,
	      "get_info version");
	{
		obs_data_array_t *rigs = obs_data_get_array(res, "rigs");
		obs_data_t *r0 = obs_data_array_item(rigs, 0);
		CHECK(obs_data_array_count(rigs) == 1 && r0 &&
			      strcmp(obs_data_get_string(r0, "scene"), "Just Chatting") == 0 &&
			      strcmp(obs_data_get_string(r0, "scene_uuid"), main_uuid) == 0 &&
			      obs_data_get_bool(r0, "enabled") &&
			      KEYS_ARE(r0, "scene", "scene_uuid", "enabled"),
		      "get_info rigs");
		obs_data_release(r0);
		obs_data_array_release(rigs);
		obs_data_array_t *fxa = obs_data_get_array(res, "fx");
		obs_data_t *f0 = obs_data_array_item(fxa, 0);
		CHECK(obs_data_array_count(fxa) == 1 && f0 &&
			      KEYS_ARE(f0, "source", "source_uuid", "enabled"),
		      "get_info fx");
		obs_data_release(f0);
		obs_data_array_release(fxa);
		obs_data_t *canvas = obs_data_get_obj(res, "canvas");
		CHECK(obs_data_get_int(canvas, "width") > 0 && obs_data_get_int(canvas, "height") > 0,
		      "canvas");
		obs_data_release(canvas);
	}
	obs_data_release(res);
	expect_err("get_info", "{\"x\":1}", "bad_param");
	expect_err("nope", "{}", "bad_param");

	/* get_state */
	res = call("get_state", "{}");
	CHECK(is_ok(res) && KEYS_ARE(res, "ok", "scene", "scene_uuid", "camera", "auto", "fx", "dof",
				     "moving", "frozen", "view"),
	      "get_state shape: %s", obs_data_get_json(res));
	{
		obs_data_t *cam = obs_data_get_obj(res, "camera");
		obs_data_t *au = obs_data_get_obj(res, "auto");
		obs_data_t *fxo = obs_data_get_obj(res, "fx");
		obs_data_t *dof = obs_data_get_obj(res, "dof");
		CHECK(KEYS_ARE(cam, "x", "y", "zoom", "rot", "shake") && KEYS_ARE(au, "mode", "intensity") &&
			      KEYS_ARE(fxo, "preset") && KEYS_ARE(dof, "focus_depth", "amount"),
		      "get_state sub-objects");
		CHECK(strcmp(obs_data_get_string(au, "mode"), "drift") == 0 &&
			      fabs(obs_data_get_double(au, "intensity") - 0.35) < 1e-9 &&
			      strcmp(obs_data_get_string(fxo, "preset"), "cozy") == 0,
		      "get_state values");
		obs_data_release(cam);
		obs_data_release(au);
		obs_data_release(fxo);
		obs_data_release(dof);
	}
	obs_data_release(res);
	res = call("get_state", "{\"scene\":\"Just Chatting\"}");
	CHECK(is_ok(res), "get_state by name");
	obs_data_release(res);
	{
		char json[128];
		snprintf(json, sizeof(json), "{\"scene\":\"%s\"}", main_uuid);
		res = call("get_state", json);
		CHECK(is_ok(res), "get_state by uuid");
		obs_data_release(res);
	}
	expect_err("get_state", "{\"scene\":\"Nope\"}", "not_found");
	expect_err("get_state", "{\"scene\":\"BG\"}", "no_rig");
	expect_err("get_state", "{\"scene\":5}", "bad_param");

	/* camera_move */
	res = call("camera_move",
		   "{\"move\":\"zoom_in\",\"duration_ms\":800,\"intensity\":1.2,\"ease\":\"ease_out_cubic\"}");
	CHECK(is_ok(res) && obs_data_get_int(res, "id") > 0 && KEYS_ARE(res, "ok", "id"),
	      "camera_move: %s", obs_data_get_json(res));
	obs_data_release(res);
	drain_all(&d);
	CHECK(d.n == 1 && d.c[0].type == STAGE_CMD_CAMERA_MOVE &&
		      strcmp(d.c[0].u.move.move, "zoom_in") == 0 && d.c[0].duration_ms == 800 &&
		      fabsf(d.c[0].intensity - 1.2f) < 1e-6f && d.c[0].ease == STAGE_EASE_OUT_CUBIC &&
		      strcmp(d.c[0].scene_uuid, main_uuid) == 0,
	      "camera_move command");
	res = call("camera_move", "{\"move\":\"slow_push\"}");
	CHECK(is_ok(res), "custom move");
	obs_data_release(res);
	drain_all(&d);
	CHECK(d.n == 1 && d.c[0].ease == STAGE_CMD_EASE_NONE && d.c[0].duration_ms == 0 &&
		      fabsf(d.c[0].intensity - 1.0f) < 1e-6f,
	      "camera_move defaults");
	expect_err("camera_move", "{\"move\":\"spin\"}", "not_found");
	expect_err("camera_move", "{\"move\":\"Zoom In\"}", "bad_param");
	expect_err("camera_move", "{}", "bad_param");
	expect_err("camera_move", "{\"move\":\"zoom_in\",\"duration_ms\":10}", "bad_param");
	expect_err("camera_move", "{\"move\":\"zoom_in\",\"duration_ms\":800.5}", "bad_param");
	expect_err("camera_move", "{\"move\":\"zoom_in\",\"intensity\":3}", "bad_param");
	expect_err("camera_move", "{\"move\":\"zoom_in\",\"ease\":\"bouncy\"}", "bad_param");
	expect_err("camera_move", "{\"move\":\"zoom_in\",\"bogus\":true}", "bad_param");
	expect_err("camera_move", "{\"move\":\"zoom_in\",\"scene\":\"BG\"}", "no_rig");

	/* camera_set */
	res = call("camera_set", "{\"scene\":\"Just Chatting\",\"x\":120,\"y\":-40,\"zoom\":1.5,"
				 "\"rot\":0,\"duration_ms\":600,\"ease\":\"ease_in_out_cubic\"}");
	CHECK(is_ok(res), "camera_set");
	obs_data_release(res);
	drain_all(&d);
	CHECK(d.n == 1 && d.c[0].u.set.mask == 0x0F && !d.c[0].u.set.relative &&
		      d.c[0].u.set.x == 120.0f && d.c[0].u.set.zoom == 1.5f && d.c[0].duration_ms == 600,
	      "camera_set command");
	res = call("camera_set", "{\"x\":-24,\"zoom\":-0.1,\"relative\":true}");
	CHECK(is_ok(res), "camera_set relative");
	obs_data_release(res);
	drain_all(&d);
	CHECK(d.n == 1 && d.c[0].u.set.relative && d.c[0].u.set.mask == (STAGE_CAM_X | STAGE_CAM_ZOOM) &&
		      d.c[0].duration_ms == 0,
	      "camera_set relative command");
	expect_err("camera_set", "{\"zoom\":5}", "bad_param");
	expect_err("camera_set", "{\"zoom\":0.2}", "bad_param");
	expect_err("camera_set", "{\"rot\":60}", "bad_param");
	expect_err("camera_set", "{\"x\":\"1\"}", "bad_param");
	expect_err("camera_set", "{\"duration_ms\":10001}", "bad_param");
	res = call("camera_set", "{\"rot\":60,\"relative\":true}");
	CHECK(is_ok(res), "relative rot 60 allowed");
	obs_data_release(res);
	drain_all(&d);

	/* camera_reset / camera_auto */
	res = call("camera_reset", "{}");
	CHECK(is_ok(res), "camera_reset");
	obs_data_release(res);
	drain_all(&d);
	CHECK(d.n == 1 && d.c[0].type == STAGE_CMD_CAMERA_RESET && d.c[0].duration_ms == 600,
	      "camera_reset default 600");
	expect_err("camera_reset", "{\"duration_ms\":-1}", "bad_param");
	res = call("camera_auto", "{\"mode\":\"chatting\",\"intensity\":0.5}");
	CHECK(is_ok(res), "camera_auto");
	obs_data_release(res);
	res = call("camera_auto", "{\"mode\":\"off\"}");
	obs_data_release(res);
	drain_all(&d);
	CHECK(d.n == 2 && d.c[0].u.autom.mode == STAGE_AUTO_CHATTING && d.c[0].intensity == 0.5f &&
		      d.c[1].u.autom.mode == STAGE_AUTO_OFF && d.c[1].intensity < 0.0f,
	      "camera_auto commands");
	expect_err("camera_auto", "{}", "bad_param");
	expect_err("camera_auto", "{\"mode\":\"spin\"}", "bad_param");
	expect_err("camera_auto", "{\"mode\":\"drift\",\"intensity\":1.5}", "bad_param");

	/* fx_set */
	res = call("fx_set", "{\"preset\":\"cozy\",\"params\":{\"vignette\":0.3,\"lut\":\"film\","
			     "\"lut_amount\":0.6,\"bloom\":0.25},\"duration_ms\":500}");
	CHECK(is_ok(res), "fx_set: %s", obs_data_get_json(res));
	obs_data_release(res);
	drain_all(&d);
	CHECK(d.n == 1 && d.c[0].type == STAGE_CMD_FX_SET && strcmp(d.c[0].u.fx.preset, "cozy") == 0 &&
		      d.c[0].duration_ms == 500,
	      "fx_set command");
	if (d.n == 1) {
		obs_data_t *p = obs_data_create_from_json(d.c[0].u.fx.params_json);
		CHECK(p && fabs(obs_data_get_double(p, "vignette") - 0.3) < 1e-9 &&
			      strcmp(obs_data_get_string(p, "lut"), "film") == 0 &&
			      fabs(obs_data_get_double(p, "bloom") - 0.25) < 1e-9,
		      "fx_set params_json: %s", d.c[0].u.fx.params_json);
		obs_data_release(p);
	}
	/* Every parameter at once still arrives complete (chunked if needed). */
	res = call("fx_set", "{\"params\":{\"exposure\":-1.2345,\"contrast\":-0.1234,\"saturation\":"
			     "-0.5555,\"temperature\":-0.7777,\"tint\":-0.3333,\"vignette\":0.1111,"
			     "\"vignette_soft\":0.2222,\"vignette_round\":0.3333,\"chroma\":0.4444,"
			     "\"grain\":0.5555,\"grain_size\":3.3333,\"lut\":\"identity\",\"lut_amount\":"
			     "0.6666,\"leak\":\"\",\"leak_amount\":0.7777,\"leak_speed\":1.8888,"
			     "\"bloom\":0.9999,\"bloom_threshold\":0.1234}}");
	CHECK(is_ok(res), "fx_set all params: %s", obs_data_get_json(res));
	{
		uint64_t id = (uint64_t)obs_data_get_int(res, "id");
		drain_all(&d);
		size_t params = 0;
		bool same_id = d.n >= 1;
		for (size_t i = 0; i < d.n; i++) {
			same_id = same_id && d.c[i].id == id;
			obs_data_t *p = obs_data_create_from_json(d.c[i].u.fx.params_json);
			for (obs_data_item_t *it = obs_data_first(p); it; obs_data_item_next(&it))
				params++;
			obs_data_release(p);
		}
		CHECK(same_id && params == 18, "all 18 params in %zu command(s) with one id", d.n);
	}
	obs_data_release(res);
	expect_err("fx_set", "{}", "bad_param");
	expect_err("fx_set", "{\"preset\":\"nope\"}", "not_found");
	expect_err("fx_set", "{\"params\":{\"lut\":\"nolut\"}}", "not_found");
	expect_err("fx_set", "{\"params\":{\"vignette\":2}}", "bad_param");
	expect_err("fx_set", "{\"params\":{\"foo\":1}}", "bad_param");
	expect_err("fx_set", "{\"params\":5}", "bad_param");
	expect_err("fx_set", "{\"preset\":\"cozy\",\"scene\":\"BG\"}", "no_fx");

	/* fx_pulse */
	res = call("fx_pulse", "{\"effect\":\"flash\",\"intensity\":0.6,\"duration_ms\":250}");
	CHECK(is_ok(res), "fx_pulse");
	obs_data_release(res);
	drain_all(&d);
	CHECK(d.n == 1 && strcmp(d.c[0].u.pulse.effect, "flash") == 0 && d.c[0].duration_ms == 250,
	      "fx_pulse command");
	res = call("fx_pulse", "{\"effect\":\"vignette_pulse\"}");
	obs_data_release(res);
	drain_all(&d);
	CHECK(d.n == 1 && d.c[0].duration_ms == 300 && d.c[0].intensity == 1.0f, "fx_pulse defaults");
	expect_err("fx_pulse", "{\"effect\":\"boom\"}", "bad_param");
	expect_err("fx_pulse", "{\"effect\":\"flash\",\"duration_ms\":10}", "bad_param");

	/* dof_set */
	res = call("dof_set", "{\"focus_depth\":3,\"amount\":0.5,\"duration_ms\":1200,"
			      "\"ease\":\"ease_in_out_quint\"}");
	CHECK(is_ok(res), "dof_set");
	obs_data_release(res);
	drain_all(&d);
	CHECK(d.n == 1 && d.c[0].u.dof.mask == (STAGE_DOF_FOCUS | STAGE_DOF_AMOUNT) &&
		      d.c[0].u.dof.focus_depth == 3.0f && d.c[0].ease == STAGE_EASE_IN_OUT_QUINT,
	      "dof_set command");
	expect_err("dof_set", "{}", "bad_param");
	expect_err("dof_set", "{\"focus_depth\":0}", "bad_param");

	/* layer_list */
	pthread_mutex_lock(&rig.lock);
	rig.layers[0].enrolled = true;
	rig.layers[0].depth = 3.0;
	rig.layers[0].role = STAGE_ROLE_BG;
	pthread_mutex_unlock(&rig.lock);
	res = call("layer_list", "{}");
	CHECK(is_ok(res) && KEYS_ARE(res, "ok", "scene", "scene_uuid", "layers"), "layer_list");
	{
		obs_data_array_t *arr = obs_data_get_array(res, "layers");
		obs_data_t *l0 = obs_data_array_item(arr, 0);
		obs_data_t *l3 = obs_data_array_item(arr, 3);
		CHECK(obs_data_array_count(arr) == 4, "4 layers");
		CHECK(l0 &&
			      KEYS_ARE(l0, "item_id", "name", "source_uuid", "depth", "role", "visible",
				       "locked", "depth_blur") &&
			      obs_data_get_double(l0, "depth") == 3.0 &&
			      strcmp(obs_data_get_string(l0, "role"), "bg") == 0 &&
			      obs_data_get_bool(l0, "depth_blur"),
		      "enrolled layer");
		CHECK(l3 && !has(l3, "depth") && strcmp(obs_data_get_string(l3, "role"), "none") == 0 &&
			      obs_data_get_bool(l3, "locked") &&
			      obs_data_get_int(l3, "item_id") == item_ids[3],
		      "not enrolled layer (depth omitted = null)");
		obs_data_release(l0);
		obs_data_release(l3);
		obs_data_array_release(arr);
	}
	obs_data_release(res);
	pthread_mutex_lock(&rig.lock);
	rig.layers[0].enrolled = false;
	rig.layers[0].role = STAGE_ROLE_NONE;
	pthread_mutex_unlock(&rig.lock);

	/* layer_set */
	{
		char json[160];
		snprintf(json, sizeof(json), "{\"item_id\":%lld,\"depth\":0.6,\"role\":\"fg\"}",
			 (long long)item_ids[2]);
		res = call("layer_set", json);
		CHECK(is_ok(res), "layer_set");
		obs_data_release(res);
		drain_all(&d);
		CHECK(d.n == 1 && d.c[0].u.layer.item_id == item_ids[2] &&
			      d.c[0].u.layer.mask == (STAGE_LAYER_DEPTH | STAGE_LAYER_ROLE) &&
			      d.c[0].u.layer.role == STAGE_ROLE_FG && fabsf(d.c[0].u.layer.depth - 0.6f) < 1e-6f,
		      "layer_set command");

		/* depth:null (obs-websocket >= 5.7.1 passes an OBJECT item holding NULL) */
		obs_data_t *req = obs_data_create();
		obs_data_set_string(req, "scene", "Just Chatting");
		obs_data_set_int(req, "item_id", item_ids[3]);
		obs_data_set_obj(req, "depth", NULL);
		res = call_data("layer_set", req);
		CHECK(is_ok(res), "layer_set depth null: %s", obs_data_get_json(res));
		obs_data_release(res);
		drain_all(&d);
		CHECK(d.n == 1 && d.c[0].u.layer.mask == STAGE_LAYER_UNENROL, "un-enrol command");
		obs_data_set_string(req, "role", "bg");
		res = call_data("layer_set", req);
		CHECK(!is_ok(res) && strcmp(code_of(res), "bad_param") == 0, "null depth + role bg");
		obs_data_release(res);
		obs_data_release(req);

		snprintf(json, sizeof(json), "{\"item_id\":%lld,\"role\":\"none\"}", (long long)item_ids[1]);
		res = call("layer_set", json);
		obs_data_release(res);
		drain_all(&d);
		CHECK(d.n == 1 && d.c[0].u.layer.mask == STAGE_LAYER_UNENROL, "role none un-enrols");
		snprintf(json, sizeof(json), "{\"item_id\":%lld}", (long long)item_ids[1]);
		expect_err("layer_set", json, "bad_param");
		snprintf(json, sizeof(json), "{\"item_id\":%lld,\"depth\":11}", (long long)item_ids[1]);
		expect_err("layer_set", json, "bad_param");
		expect_err("layer_set", "{\"item_id\":99999,\"depth\":1}", "not_found");
		expect_err("layer_set", "{\"item_id\":-1,\"depth\":1}", "bad_param");
	}

	/* move_list / recipe_list */
	res = call("move_list", "{}");
	CHECK(is_ok(res) && KEYS_ARE(res, "ok", "moves"), "move_list");
	{
		obs_data_array_t *arr = obs_data_get_array(res, "moves");
		size_t n = obs_data_array_count(arr), builtin = 0;
		bool shape = n >= 15;
		for (size_t i = 0; i < n; i++) {
			obs_data_t *m = obs_data_array_item(arr, i);
			obs_data_t *nm = obs_data_get_obj(m, "name");
			shape = shape && KEYS_ARE(m, "id", "name", "duration_ms", "builtin") &&
				KEYS_ARE(nm, "en", "ja", "id");
			builtin += obs_data_get_bool(m, "builtin");
			obs_data_release(nm);
			obs_data_release(m);
		}
		CHECK(shape && builtin == 9, "move_list entries (%zu, %zu builtin)", n, builtin);
		obs_data_array_release(arr);
	}
	obs_data_release(res);
	res = call("recipe_list", "{}");
	CHECK(is_ok(res), "recipe_list");
	{
		obs_data_array_t *arr = obs_data_get_array(res, "recipes");
		obs_data_t *r0 = obs_data_array_item(arr, 0);
		CHECK(obs_data_array_count(arr) >= 14 && r0 && KEYS_ARE(r0, "id", "name", "pro", "user"),
		      "recipe_list entries");
		obs_data_release(r0);
		obs_data_array_release(arr);
	}
	obs_data_release(res);
	expect_err("move_list", "{\"a\":1}", "bad_param");

	/* recipe_apply: BG / VTube Studio / FG Desk get depths by name. */
	res = call("recipe_apply", "{\"scene\":\"Just Chatting\",\"recipe\":\"cozy_chat\"}");
	CHECK(is_ok(res) && KEYS_ARE(res, "ok", "id", "applied"), "recipe_apply: %s",
	      obs_data_get_json(res));
	{
		obs_data_t *ap = obs_data_get_obj(res, "applied");
		CHECK(ap && obs_data_get_int(ap, "layers") == 3 &&
			      strcmp(obs_data_get_string(ap, "auto"), "drift") == 0 &&
			      strcmp(obs_data_get_string(ap, "fx"), "cozy") == 0,
		      "recipe_apply applied");
		obs_data_release(ap);
		uint64_t id = (uint64_t)obs_data_get_int(res, "id");
		drain_all(&d);
		int layer_cmds = 0, auto_cmds = 0, fx_cmds = 0, dof_cmds = 0;
		bool same_id = true, depths_ok = true;
		for (size_t i = 0; i < d.n; i++) {
			same_id = same_id && d.c[i].id == id;
			switch (d.c[i].type) {
			case STAGE_CMD_LAYER_SET: {
				layer_cmds++;
				int64_t iid = d.c[i].u.layer.item_id;
				float want = iid == item_ids[0] ? 3.0f : iid == item_ids[1] ? 1.0f : 0.6f;
				depths_ok = depths_ok && iid != item_ids[3] &&
					    fabsf(d.c[i].u.layer.depth - want) < 1e-6f &&
					    d.c[i].duration_ms == STAGE_RECIPE_DURATION_DEFAULT;
				break;
			}
			case STAGE_CMD_CAMERA_AUTO:
				auto_cmds += d.c[i].u.autom.mode == STAGE_AUTO_DRIFT;
				break;
			case STAGE_CMD_FX_SET:
				fx_cmds += strcmp(d.c[i].u.fx.preset, "cozy") == 0;
				break;
			case STAGE_CMD_DOF_SET:
				dof_cmds++;
				break;
			default:
				break;
			}
		}
		CHECK(layer_cmds == 3 && auto_cmds == 1 && fx_cmds == 1 && dof_cmds == 1 && same_id &&
			      depths_ok,
		      "recipe_apply commands (%d layers, %d auto, %d fx, %d dof)", layer_cmds, auto_cmds,
		      fx_cmds, dof_cmds);
	}
	obs_data_release(res);
	expect_err("recipe_apply", "{\"recipe\":\"nope\"}", "not_found");
	expect_err("recipe_apply", "{}", "bad_param");
	expect_err("recipe_apply", "{\"recipe\":\"cozy_chat\",\"scene\":\"BG\"}", "no_rig");

	/* Views */
	pthread_mutex_lock(&rig.lock);
	rig.st.x = 40.0f;
	rig.st.y = -60.0f;
	rig.st.zoom = 1.6f;
	rig.st.rot = -2.0f;
	rig.st.tx = 40.0f;
	rig.st.ty = -60.0f;
	rig.st.tzoom = 1.6f;
	rig.st.trot = -2.0f;
	pthread_mutex_unlock(&rig.lock);
	res = call("view_save", "{\"slot\":2,\"name\":\"Close-up\",\"transition\":\"punch\","
				"\"duration_ms\":450}");
	CHECK(is_ok(res) && KEYS_ARE(res, "ok", "view"), "view_save: %s", obs_data_get_json(res));
	{
		obs_data_t *v = obs_data_get_obj(res, "view");
		obs_data_t *cam = obs_data_get_obj(v, "camera");
		CHECK(v && KEYS_ARE(v, "slot", "name", "camera", "transition", "duration_ms") &&
			      KEYS_ARE(cam, "x", "y", "zoom", "rot") &&
			      obs_data_get_double(cam, "zoom") == 1.6 &&
			      strcmp(obs_data_get_string(v, "transition"), "punch") == 0,
		      "view_save view");
		obs_data_release(cam);
		obs_data_release(v);
	}
	obs_data_release(res);
	res = call("view_save", "{\"slot\":1}");
	{
		obs_data_t *v = obs_data_get_obj(res, "view");
		CHECK(v && strcmp(obs_data_get_string(v, "name"), "View 1") == 0 &&
			      strcmp(obs_data_get_string(v, "transition"), "smooth") == 0 &&
			      obs_data_get_int(v, "duration_ms") == 600,
		      "view_save defaults");
		obs_data_release(v);
	}
	obs_data_release(res);
	/* 47 code points of 3 bytes = 141 bytes: valid; 48 code points: not. */
	{
		char json[400], name[300] = "";
		for (int i = 0; i < 47; i++)
			strcat(name, "ビ");
		snprintf(json, sizeof(json), "{\"slot\":3,\"name\":\"%s\"}", name);
		res = call("view_save", json);
		CHECK(is_ok(res), "47-character JA view name");
		obs_data_release(res);
		strcat(name, "ビ");
		snprintf(json, sizeof(json), "{\"slot\":3,\"name\":\"%s\"}", name);
		expect_err("view_save", json, "bad_param");
	}
	expect_err("view_save", "{\"slot\":0}", "bad_param");
	expect_err("view_save", "{}", "bad_param");
	expect_err("view_save", "{\"slot\":1,\"transition\":\"wipe\"}", "bad_param");
	drain_all(&d);

	/* keep_pose: edit a stored view without touching its pose; every field
	 * left out keeps its stored value. The camera has moved meanwhile. */
	pthread_mutex_lock(&rig.lock);
	rig.st.tx = -300.0f;
	rig.st.tzoom = 2.5f;
	pthread_mutex_unlock(&rig.lock);
	res = call("view_save", "{\"slot\":2,\"name\":\"Wide\",\"keep_pose\":true}");
	{
		obs_data_t *v = obs_data_get_obj(res, "view");
		obs_data_t *cam = obs_data_get_obj(v, "camera");
		CHECK(is_ok(res) && v && strcmp(obs_data_get_string(v, "name"), "Wide") == 0 &&
			      obs_data_get_double(cam, "x") == 40.0 &&
			      obs_data_get_double(cam, "zoom") == 1.6 &&
			      strcmp(obs_data_get_string(v, "transition"), "punch") == 0 &&
			      obs_data_get_int(v, "duration_ms") == 450,
		      "view_save keep_pose renames only: %s", obs_data_get_json(res));
		obs_data_release(cam);
		obs_data_release(v);
	}
	obs_data_release(res);
	drain_all(&d);
	CHECK(d.n == 1 && d.c[0].type == STAGE_CMD_VIEW_SAVE && d.c[0].u.view.slot == 2 &&
		      d.c[0].u.view.cam.x == 40.0f && fabsf(d.c[0].u.view.cam.zoom - 1.6f) < 1e-6f &&
		      d.c[0].u.view.cam.mask == 0x0F && d.c[0].duration_ms == 450 &&
		      d.c[0].u.view.keep_pose && strcmp(d.c[0].u.view.name, "Wide") == 0,
	      "view_save keep_pose: the command carries the stored pose, marked as an edit");
	res = call("view_save", "{\"slot\":2,\"transition\":\"cut\",\"duration_ms\":0,"
				"\"keep_pose\":true}");
	{
		struct stage_view got;
		CHECK(is_ok(res) &&
			      stage_views_get(obs_scene_get_source(main_scene), 2, &got) &&
			      strcmp(got.name, "Wide") == 0 && got.x == 40.0f && got.zoom == 1.6f &&
			      got.transition == STAGE_TRANSITION_CUT && got.duration_ms == 0,
		      "view_save keep_pose changes the transition and keeps name and pose");
	}
	obs_data_release(res);
	expect_err("view_save", "{\"slot\":6,\"keep_pose\":true}", "not_found");
	expect_err("view_save", "{\"slot\":2,\"keep_pose\":1}", "bad_param");
	/* Put slot 2 back as it was for the view_apply checks below. */
	res = call("view_save", "{\"slot\":2,\"name\":\"Close-up\",\"transition\":\"punch\","
				"\"duration_ms\":450,\"keep_pose\":true}");
	CHECK(is_ok(res), "view_save keep_pose restore");
	obs_data_release(res);
	res = call("view_save", "{\"slot\":6,\"keep_pose\":false}");
	{
		obs_data_t *v = obs_data_get_obj(res, "view");
		obs_data_t *cam = obs_data_get_obj(v, "camera");
		CHECK(is_ok(res) && obs_data_get_double(cam, "x") == -300.0 &&
			      obs_data_get_double(cam, "zoom") == 2.5,
		      "view_save keep_pose false stores the camera");
		obs_data_release(cam);
		obs_data_release(v);
	}
	obs_data_release(res);
	res = call("view_delete", "{\"slot\":6}");
	obs_data_release(res);
	pthread_mutex_lock(&rig.lock);
	rig.st.tx = 40.0f;
	rig.st.tzoom = 1.6f;
	pthread_mutex_unlock(&rig.lock);
	drain_all(&d);

	res = call("view_list", "{}");
	{
		obs_data_array_t *arr = obs_data_get_array(res, "views");
		obs_data_t *v0 = obs_data_array_item(arr, 0);
		CHECK(is_ok(res) && KEYS_ARE(res, "ok", "scene", "scene_uuid", "views") &&
			      obs_data_array_count(arr) == 3 && v0 && obs_data_get_int(v0, "slot") == 1,
		      "view_list: %s", obs_data_get_json(res));
		obs_data_release(v0);
		obs_data_array_release(arr);
	}
	obs_data_release(res);

	res = call("view_apply", "{\"slot\":2,\"transition\":\"whip\",\"duration_ms\":350}");
	CHECK(is_ok(res), "view_apply");
	obs_data_release(res);
	drain_all(&d);
	CHECK(d.n == 1 && d.c[0].type == STAGE_CMD_VIEW_APPLY && d.c[0].u.view.slot == 2 &&
		      d.c[0].u.view.transition == STAGE_TRANSITION_WHIP &&
		      d.c[0].ease == STAGE_EASE_IN_OUT_QUINT && d.c[0].duration_ms == 350 &&
		      fabsf(d.c[0].u.view.cam.zoom - 1.6f) < 1e-6f && d.c[0].u.view.cam.mask == 0x0F,
	      "view_apply command");
	res = call("view_apply", "{\"slot\":2,\"transition\":\"cut\"}");
	obs_data_release(res);
	drain_all(&d);
	CHECK(d.n == 1 && d.c[0].duration_ms == 0, "cut has duration 0");
	res = call("view_apply", "{\"slot\":2}");
	obs_data_release(res);
	drain_all(&d);
	CHECK(d.n == 1 && d.c[0].duration_ms == 450 && d.c[0].ease == STAGE_EASE_OUT_BACK,
	      "view's own transition");
	expect_err("view_apply", "{\"slot\":5}", "not_found");

	res = call("view_delete", "{\"slot\":3}");
	CHECK(is_ok(res) && KEYS_ARE(res, "ok"), "view_delete");
	obs_data_release(res);
	expect_err("view_delete", "{\"slot\":3}", "not_found");
	drain_all(&d);

	/* freeze */
	res = call("freeze", "{\"on\":true}");
	CHECK(is_ok(res) && obs_data_get_bool(res, "frozen") && KEYS_ARE(res, "ok", "frozen"), "freeze on");
	obs_data_release(res);
	pthread_mutex_lock(&rig.lock);
	rig.st.frozen = true;
	pthread_mutex_unlock(&rig.lock);
	res = call("freeze", "{}");
	CHECK(is_ok(res) && !obs_data_get_bool(res, "frozen"), "freeze toggle off");
	obs_data_release(res);
	drain_all(&d);
	CHECK(d.n == 2 && d.c[0].u.freeze.mode == STAGE_FREEZE_ON && d.c[1].u.freeze.mode == STAGE_FREEZE_OFF,
	      "freeze commands");
	pthread_mutex_lock(&rig.lock);
	rig.st.frozen = false;
	pthread_mutex_unlock(&rig.lock);
	expect_err("freeze", "{\"on\":\"yes\"}", "bad_param");

	/* panic */
	uint64_t gen = stage_panic_generation();
	res = call("camera_reset", "{}");
	obs_data_release(res);
	res = call("panic", "{}");
	CHECK(is_ok(res) && KEYS_ARE(res, "ok"), "panic");
	obs_data_release(res);
	CHECK(stage_panic_generation() == gen + 1 && drain_all(&d) == 0, "panic purges and bumps");
	expect_err("panic", "{\"all\":true}", "bad_param");

	/* No program scene: requests without "scene" answer no_rig / no_fx. */
	stage_core_set_program_scene(NULL);
	expect_err("get_state", "{}", "no_rig");
	expect_err("fx_pulse", "{\"effect\":\"flash\"}", "no_fx");
	stage_core_set_program_scene(main_uuid);

	/* Disabled rig -> no_rig. */
	pthread_mutex_lock(&rig.lock);
	rig.st.enabled = false;
	pthread_mutex_unlock(&rig.lock);
	expect_err("camera_move", "{\"move\":\"zoom_in\"}", "no_rig");
	pthread_mutex_lock(&rig.lock);
	rig.st.enabled = true;
	pthread_mutex_unlock(&rig.lock);

	/* Queue saturation -> busy (never drops other producers' commands). */
	stage_cmdq_set_max_age_ms(0);
	for (int i = 0; i < STAGE_CMDQ_CAPACITY; i++) {
		res = call("camera_reset", "{}");
		obs_data_release(res);
	}
	expect_err("camera_reset", "{}", "busy");
	expect_err("recipe_apply", "{\"recipe\":\"cozy_chat\"}", "busy");
	drain_all(&d);
	stage_cmdq_purge(NULL);
	stage_cmdq_set_max_age_ms(STAGE_CMDQ_DEFAULT_MAX_AGE_MS);
}

/* ------------------------------------------------------------------------- */

static void set_x(struct stage_state *st, void *p)
{
	st->x = *(float *)p;
}

/* Starts move id (it becomes the last started one), or 0: the running move
 * stopped before its end. */
static void set_move(struct stage_state *st, void *p)
{
	uint64_t id = *(uint64_t *)p;
	st->move_id = id;
	st->moving = id != 0;
	snprintf(st->move, sizeof(st->move), "%s", id ? "zoom_in" : "");
	if (id) {
		st->last_move_id = id;
		snprintf(st->last_move, sizeof(st->last_move), "zoom_in");
		st->last_move_done = false;
	}
}

/* Snapshot after a tick: the running move (0 = none) and the last started
 * one, which may have started and ended within that tick. */
struct mv_snap {
	uint64_t running, last;
	bool done;
	const char *name;
};

static void set_moves(struct stage_state *st, void *p)
{
	const struct mv_snap *s = p;
	st->move_id = s->running;
	st->moving = s->running != 0;
	snprintf(st->move, sizeof(st->move), "%s", s->running ? s->name : "");
	st->last_move_id = s->last;
	snprintf(st->last_move, sizeof(st->last_move), "%s", s->name);
	st->last_move_done = s->done;
}

static size_t count_finished(uint64_t id, const char *cancelled)
{
	char want[64];
	snprintf(want, sizeof(want), "\"id\":%llu,", (unsigned long long)id);
	size_t n = 0;
	for (size_t i = 0; i < nevents; i++) {
		if (strcmp(events[i].type, "move_finished") == 0 && strstr(events[i].json, want) &&
		    (!cancelled || strstr(events[i].json, cancelled)))
			n++;
	}
	return n;
}

static uint64_t request_move(const char *move)
{
	char body[96];
	snprintf(body, sizeof(body), "{\"move\":\"%s\"}", move);
	obs_data_t *res = call("camera_move", body);
	uint64_t id = (uint64_t)obs_data_get_int(res, "id");
	obs_data_release(res);
	return id;
}

/* Moves that start and end within one rig tick never show as running. */
static void test_sub_tick_moves(uint64_t *t)
{
	const char *no = "\"cancelled\":false", *yes = "\"cancelled\":true";

	/* Fresh tracks: earlier fake ids are above the real command ids. */
	stage_vendor_events_reset();
	stage_vendor_events_pump(*t += 16);

	/* Ran to its end: finished, not cancelled, on the next pump. */
	uint64_t a = request_move("nod");
	struct mv_snap s = {0, a, true, "nod"};
	rig_set(set_moves, &s);
	stage_vendor_events_pump(*t += 16);
	const struct evt *e = last_event("move_finished");
	CHECK(count_finished(a, no) == 1 && e && strstr(e->json, "\"move\":\"nod\""),
	      "sub-tick move finished within one tick: %s", e ? e->json : "(none)");

	/* Replaces a running move: that one cancelled, the short one not. */
	uint64_t b = request_move("zoom_in");
	rig_set(set_move, &b);
	stage_vendor_events_pump(*t += 16);
	uint64_t c = request_move("punch");
	s = (struct mv_snap){0, c, true, "punch"};
	rig_set(set_moves, &s);
	stage_vendor_events_pump(*t += 16);
	CHECK(count_finished(b, yes) == 1, "move replaced by a sub-tick move: cancelled");
	CHECK(count_finished(c, no) == 1, "replacing sub-tick move: not cancelled");

	/* Stopped in the same tick (reset after it): cancelled. */
	uint64_t d = request_move("shake");
	s = (struct mv_snap){0, d, false, "shake"};
	rig_set(set_moves, &s);
	stage_vendor_events_pump(*t += 16);
	CHECK(count_finished(d, yes) == 1, "sub-tick move stopped by a reset: cancelled");

	/* Nothing more once the pending timeout is long past, and an
	 * unchanged snapshot reports nothing twice. */
	uint64_t later = stage_core_now_ms() + STAGE_CMDQ_DEFAULT_MAX_AGE_MS + 2000;
	*t = later > *t ? later : *t + 16;
	stage_vendor_events_pump(*t);
	stage_vendor_events_pump(*t += 16);
	CHECK(count_finished(a, NULL) == 1 && count_finished(b, NULL) == 1 &&
		      count_finished(c, NULL) == 1 && count_finished(d, NULL) == 1,
	      "each move reported once (%zu %zu %zu %zu)", count_finished(a, NULL),
	      count_finished(b, NULL), count_finished(c, NULL), count_finished(d, NULL));
	stage_cmdq_purge(NULL);
}

static void set_frozen(struct stage_state *st, void *p)
{
	st->frozen = *(bool *)p;
}

static void test_events(void)
{
	stage_vendor_set_emit_sink(sink, NULL);
	stage_vendor_events_reset();
	nevents = 0;
	uint64_t t = 100000;

	stage_vendor_events_pump(t);
	CHECK(count_events("rig_changed") == 1 && count_events("state_changed") == 1,
	      "first pump: rig_changed + state_changed (%zu events)", nevents);
	if (last_event("rig_changed"))
		CHECK(strstr(last_event("rig_changed")->json, "\"enabled\":true") != NULL,
		      "rig_changed enabled: %s", last_event("rig_changed")->json);

	/* Idle: nothing. */
	size_t before = nevents;
	for (int i = 0; i < 60; i++)
		stage_vendor_events_pump(t += 16);
	CHECK(nevents == before, "idle camera emits nothing (%zu)", nevents - before);

	/* Moving every frame for 2 s: <= 10 Hz. */
	before = count_events("state_changed");
	uint64_t start = t;
	for (int i = 0; i < 120; i++) {
		float x = (float)i * 3.0f;
		rig_set(set_x, &x);
		stage_vendor_events_pump(t += 16);
	}
	size_t moving = count_events("state_changed") - before;
	double secs = (double)(t - start) / 1000.0;
	CHECK(moving >= 15 && moving <= (size_t)(secs * STAGE_STATE_EVENT_MAX_HZ) + 1,
	      "state_changed while moving: %zu in %.2f s", moving, secs);

	/* Stops: one trailing event at most, then silence; sub-quantum jitter
	 * does not count as a change. */
	before = count_events("state_changed");
	for (int i = 0; i < 30; i++)
		stage_vendor_events_pump(t += 16);
	size_t trailing = count_events("state_changed") - before;
	CHECK(trailing <= 1, "trailing events after stop: %zu", trailing);
	before = nevents;
	for (int i = 0; i < 60; i++) {
		float x = 357.0f + (i & 1 ? 0.001f : 0.0f);
		rig_set(set_x, &x);
		stage_vendor_events_pump(t += 16);
	}
	CHECK(nevents - before <= 1, "float jitter is not a change (%zu)", nevents - before);
	if (last_event("state_changed")) {
		obs_data_t *d = obs_data_create_from_json(last_event("state_changed")->json);
		CHECK(d && KEYS_ARE(d, "scene", "scene_uuid", "camera", "auto", "fx", "dof", "moving",
				    "frozen", "view"),
		      "state_changed shape: %s", last_event("state_changed")->json);
		obs_data_release(d);
	}

	/* A move runs and finishes normally. */
	uint64_t id = 4242424;
	rig_set(set_move, &id);
	stage_vendor_events_pump(t += 16);
	struct mv_snap ended = {0, id, true, "zoom_in"};
	rig_set(set_moves, &ended);
	stage_vendor_events_pump(t += 16);
	const struct evt *e = last_event("move_finished");
	CHECK(e && strstr(e->json, "4242424") && strstr(e->json, "\"cancelled\":false"),
	      "move_finished normal: %s", e ? e->json : "(none)");

	/* A move cancelled by camera_reset. */
	obs_data_t *res = call("camera_move", "{\"move\":\"zoom_in\"}");
	id = (uint64_t)obs_data_get_int(res, "id");
	obs_data_release(res);
	rig_set(set_move, &id);
	stage_vendor_events_pump(t += 16);
	stage_vendor_events_pump(t += 16);
	res = call("camera_reset", "{}");
	obs_data_release(res);
	stage_vendor_events_pump(t += 16);
	uint64_t zero = 0;
	rig_set(set_move, &zero);
	before = count_events("move_finished");
	stage_vendor_events_pump(t += 16);
	e = last_event("move_finished");
	CHECK(count_events("move_finished") == before + 1 && e && strstr(e->json, "\"cancelled\":true"),
	      "move_finished after reset: %s", e ? e->json : "(none)");

	/* camera_move while frozen: finished at once, cancelled. */
	bool fr = true;
	rig_set(set_frozen, &fr);
	res = call("camera_move", "{\"move\":\"nod\"}");
	char want[64];
	snprintf(want, sizeof(want), "\"id\":%lld,", obs_data_get_int(res, "id"));
	obs_data_release(res);
	stage_vendor_events_pump(t += 16);
	e = last_event("move_finished");
	CHECK(e && strstr(e->json, want) && strstr(e->json, "\"cancelled\":true") &&
		      strstr(e->json, "\"move\":\"nod\""),
	      "frozen camera_move: %s", e ? e->json : "(none)");
	fr = false;
	rig_set(set_frozen, &fr);

	/* Superseded before it ever ran. */
	res = call("camera_move", "{\"move\":\"zoom_in\"}");
	uint64_t a = (uint64_t)obs_data_get_int(res, "id");
	obs_data_release(res);
	res = call("camera_move", "{\"move\":\"zoom_out\"}");
	uint64_t b = (uint64_t)obs_data_get_int(res, "id");
	obs_data_release(res);
	rig_set(set_move, &b);
	stage_vendor_events_pump(t += 16);
	stage_vendor_events_pump(t += 16);
	snprintf(want, sizeof(want), "\"id\":%llu,", (unsigned long long)a);
	bool found = false;
	for (size_t i = 0; i < nevents; i++)
		found = found || (strcmp(events[i].type, "move_finished") == 0 &&
				  strstr(events[i].json, want) && strstr(events[i].json, "\"cancelled\":true"));
	CHECK(found, "superseded unseen move reported cancelled");
	rig_set(set_move, &zero);
	stage_vendor_events_pump(t += 16);
	stage_cmdq_purge(NULL);

	test_sub_tick_moves(&t);

	/* A queued move that never shows up in a snapshot (expired or purged)
	 * ends cancelled once the queue's max age is past, rig idle or not. */
	res = call("camera_move", "{\"move\":\"nod\"}");
	snprintf(want, sizeof(want), "\"id\":%lld,", obs_data_get_int(res, "id"));
	obs_data_release(res);
	stage_cmdq_purge(NULL);
	stage_vendor_events_pump(t += 16);
	uint64_t later = stage_core_now_ms() + STAGE_CMDQ_DEFAULT_MAX_AGE_MS + 2000;
	t = later > t ? later : t + 16;
	stage_vendor_events_pump(t);
	bool cancelled = false;
	for (size_t i = 0; i < nevents; i++)
		cancelled = cancelled || (strcmp(events[i].type, "move_finished") == 0 &&
					  strstr(events[i].json, want) &&
					  strstr(events[i].json, "\"cancelled\":true"));
	CHECK(cancelled, "never-run move ends cancelled after the queue max age");

	/* perf_warning once when a part is paused. */
	snprintf(fx.info.paused, sizeof(fx.info.paused), "bloom");
	stage_vendor_events_pump(t += 16);
	stage_vendor_events_pump(t += 16);
	CHECK(count_events("perf_warning") == 1, "perf_warning once");
	if (last_event("perf_warning")) {
		obs_data_t *d = obs_data_create_from_json(last_event("perf_warning")->json);
		CHECK(KEYS_ARE(d, "source", "source_uuid", "filter", "cost_ms", "budget_ms", "window_s",
			       "paused"),
		      "perf_warning shape");
		obs_data_release(d);
	}
	fx.info.paused[0] = '\0';

	/* Rig removed -> rig_changed enabled:false. */
	stage_rig_unregister(main_uuid, &rig);
	stage_vendor_events_pump(t += 16);
	e = last_event("rig_changed");
	CHECK(e && strstr(e->json, "\"enabled\":false"), "rig removed: %s", e ? e->json : "(none)");
	stage_rig_register(main_uuid, &rig_ops, &rig);
	stage_cmdq_purge(NULL);
	stage_vendor_set_emit_sink(NULL, NULL);
}

/* ------------------------------------------------------------------------- */
/* Stress: 4 websocket-like threads, 1 graphics-like drain + pump thread     */
/* ------------------------------------------------------------------------- */

static volatile long stop_flag;
static volatile long ok_count, busy_count, other_count, drained_count, state_reads;

static void *producer(void *p)
{
	(void)p;
	obs_data_t *req = obs_data_create_from_json("{\"move\":\"zoom_in\",\"intensity\":0.5}");
	while (!os_atomic_load_long(&stop_flag)) {
		obs_data_t *res = obs_data_create();
		stage_vendor_dispatch("camera_move", req, res);
		if (is_ok(res))
			os_atomic_inc_long(&ok_count);
		else if (strcmp(code_of(res), "busy") == 0)
			os_atomic_inc_long(&busy_count);
		else
			os_atomic_inc_long(&other_count);
		obs_data_release(res);
		obs_data_t *st = obs_data_create();
		stage_vendor_dispatch("get_state", NULL, st);
		if (is_ok(st))
			os_atomic_inc_long(&state_reads);
		obs_data_release(st);
	}
	obs_data_release(req);
	return NULL;
}

static void count_cb(const struct stage_cmd *cmd, void *ud)
{
	(void)cmd;
	(void)ud;
	os_atomic_inc_long(&drained_count);
}

static void *graphics(void *p)
{
	(void)p;
	uint64_t id = 0;
	while (!os_atomic_load_long(&stop_flag)) {
		stage_cmdq_drain_types(main_uuid, STAGE_CMD_MASK_RIG, count_cb, NULL);
		id++;
		rig_set(set_move, &id); /* a new move every frame */
		stage_vendor_events_pump(stage_core_now_ms());
		sleep_ms(4);
	}
	return NULL;
}

static void test_stress(void)
{
	stage_vendor_set_emit_sink(sink, NULL);
	nevents = 0;
	pthread_t prod[4], gfx;
	stop_flag = 0;
	pthread_create(&gfx, NULL, graphics, NULL);
	for (int i = 0; i < 4; i++)
		pthread_create(&prod[i], NULL, producer, NULL);
	sleep_ms(1500);
	os_atomic_set_long(&stop_flag, 1);
	for (int i = 0; i < 4; i++)
		pthread_join(prod[i], NULL);
	pthread_join(gfx, NULL);
	printf("  stress: %ld ok, %ld busy, %ld other, %ld drained, %ld get_state\n", ok_count,
	       busy_count, other_count, drained_count, state_reads);
	CHECK(ok_count > 1000 && other_count == 0 && state_reads > 1000, "stress results");
	stage_cmdq_purge(NULL);
	stage_vendor_set_emit_sink(NULL, NULL);
	stage_vendor_events_reset();
}

/* ------------------------------------------------------------------------- */

struct hk_count {
	int stage, ai, view3;
};

static bool hk_cb(void *data, obs_hotkey_id id, obs_hotkey_t *key)
{
	(void)id;
	struct hk_count *c = data;
	const char *d = obs_hotkey_get_description(key);
	if (strncmp(d, "Parallaxa: ", 11) == 0)
		c->stage++;
	if (strncmp(d, "AI Live Studio: ", 16) == 0)
		c->ai++;
	if (strcmp(d, "Parallaxa: Camera view 3") == 0)
		c->view3++;
	return true;
}

static void test_user_recipe(void)
{
	char id[STAGE_ID_MAX] = "";
	pthread_mutex_lock(&rig.lock);
	rig.layers[0].enrolled = true;
	rig.layers[0].role = STAGE_ROLE_BG;
	rig.layers[0].depth = 4.0;
	pthread_mutex_unlock(&rig.lock);
	CHECK(stage_recipe_save_user(main_uuid, "", id) == STAGE_SAVE_NAME_EMPTY, "empty name");
	CHECK(stage_recipe_save_user(main_uuid, "Clean", id) == STAGE_SAVE_NAME_TAKEN,
	      "name of a built-in recipe is taken");
	CHECK(stage_recipe_save_user(main_uuid, "My Look", id) == STAGE_SAVE_OK &&
		      strcmp(id, "my-look") == 0,
	      "save user recipe: %s", id);
	struct stage_recipe r;
	CHECK(stage_recipe_find("my-look", &r) && r.user && r.has_role[STAGE_RROLE_BG] &&
		      fabs(r.role_depth[STAGE_RROLE_BG] - 4.0) < 1e-9 && !r.has_role[STAGE_RROLE_CHAR] &&
		      r.has_auto && strcmp(r.fx, "cozy") == 0,
	      "saved user recipe reloads");
	CHECK(stage_recipe_save_user(main_uuid, "My Look", id) == STAGE_SAVE_NAME_TAKEN,
	      "same name again is taken");
	obs_data_t *res = call("recipe_list", "{}");
	obs_data_array_t *arr = obs_data_get_array(res, "recipes");
	bool user_seen = false;
	for (size_t i = 0; i < obs_data_array_count(arr); i++) {
		obs_data_t *o = obs_data_array_item(arr, i);
		user_seen = user_seen || (strcmp(obs_data_get_string(o, "id"), "my-look") == 0 &&
					  obs_data_get_bool(o, "user"));
		obs_data_release(o);
	}
	CHECK(user_seen, "user recipe in recipe_list with user:true");
	obs_data_array_release(arr);
	obs_data_release(res);
	char dir[600], path[700];
	if (stage_presets_user_dir("recipes", dir, sizeof(dir))) {
		snprintf(path, sizeof(path), "%s/my-look.json", dir);
		os_unlink(path);
		snprintf(path, sizeof(path), "%s/my-look.json.bak", dir);
		os_unlink(path);
	}
	pthread_mutex_lock(&rig.lock);
	rig.layers[0].enrolled = false;
	rig.layers[0].role = STAGE_ROLE_NONE;
	pthread_mutex_unlock(&rig.lock);
}

/* ------------------------------------------------------------------------- */
/* Tools > Parallaxa: Import sample scene (stage-sample.h)                   */
/* ------------------------------------------------------------------------- */

static bool fake_taken(const char *name, void *ud)
{
	for (const char *const *n = ud; *n; n++)
		if (strcmp(*n, name) == 0)
			return true;
	return false;
}

/* String key of sources[src] (or of its settings when in_settings). The
 * result is owned by col. */
static const char *sample_src(obs_data_t *col, size_t src, const char *key, bool in_settings)
{
	obs_data_array_t *srcs = obs_data_get_array(col, "sources");
	obs_data_t *s = obs_data_array_item(srcs, src);
	obs_data_t *set = obs_data_get_obj(s, "settings");
	const char *v = obs_data_get_string(in_settings ? set : s, key);
	obs_data_release(set);
	obs_data_release(s);
	obs_data_array_release(srcs);
	return v;
}

/* String key of item `item` of the scene sources[src]. Owned by col. */
static const char *sample_item(obs_data_t *col, size_t src, size_t item, const char *key)
{
	obs_data_array_t *srcs = obs_data_get_array(col, "sources");
	obs_data_t *s = obs_data_array_item(srcs, src);
	obs_data_t *set = obs_data_get_obj(s, "settings");
	obs_data_array_t *items = obs_data_get_array(set, "items");
	obs_data_t *it = obs_data_array_item(items, item);
	const char *v = obs_data_get_string(it, key);
	obs_data_release(it);
	obs_data_array_release(items);
	obs_data_release(set);
	obs_data_release(s);
	obs_data_array_release(srcs);
	return v;
}

struct sample_items {
	int n;
	char names[4][128];
	char roles[4][8];
	char file[4][600];
};

static bool sample_item_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	(void)scene;
	struct sample_items *s = param;
	if (s->n >= 4)
		return false;
	obs_source_t *src = obs_sceneitem_get_source(item);
	obs_data_t *priv = obs_sceneitem_get_private_settings(item);
	obs_data_t *set = obs_source_get_settings(src);
	snprintf(s->names[s->n], sizeof(s->names[0]), "%s", obs_source_get_name(src));
	snprintf(s->roles[s->n], sizeof(s->roles[0]), "%s", obs_data_get_string(priv, "stage.role"));
	snprintf(s->file[s->n], sizeof(s->file[0]), "%s", obs_data_get_string(set, "file"));
	obs_data_release(set);
	obs_data_release(priv);
	s->n++;
	return true;
}

static bool path_absolute(const char *f)
{
	return f[0] == '/' || ((f[0] | 0x20) >= 'a' && (f[0] | 0x20) <= 'z' && f[1] == ':');
}

/* A versioned source type, like color_source_v3, for the missing-plugin log. */
static const char *probe_name(void *type_data)
{
	(void)type_data;
	return "Probe";
}

static void *probe_create(obs_data_t *settings, obs_source_t *source)
{
	(void)settings;
	return source;
}

static void probe_destroy(void *data)
{
	(void)data;
}

static void test_sample(const char *data_dir, const char *scratch)
{
	static const char *const used[] = {"Scene", "Scene 2", "Scene 3", "Pic", NULL};
	char name[16];
	CHECK(stage_unique_name("Free", fake_taken, (void *)used, name, sizeof(name)) &&
		      strcmp(name, "Free") == 0,
	      "unique name: free base kept (%s)", name);
	CHECK(stage_unique_name("Scene", fake_taken, (void *)used, name, sizeof(name)) &&
		      strcmp(name, "Scene 4") == 0,
	      "unique name: next free number (%s)", name);
	CHECK(!stage_unique_name("A name far too long", fake_taken, (void *)used, name, sizeof(name)) &&
		      !*name,
	      "unique name: does not fit");

	/* Path, name and uuid rewriting on a collection, no source created. */
	obs_data_t *col = obs_data_create_from_json(
		"{\"sources\":["
		"{\"name\":\"Scene\",\"uuid\":\"u-scene\",\"id\":\"scene\",\"settings\":{\"items\":["
		"{\"name\":\"Pic\",\"source_uuid\":\"u-pic\"},{\"name\":\"Abs\"}]},"
		"\"filters\":[{\"name\":\"F\",\"uuid\":\"u-f\",\"id\":\"stage_fx\"}]},"
		"{\"name\":\"Pic\",\"uuid\":\"u-pic\",\"id\":\"image_source\","
		"\"settings\":{\"file\":\"art\\\\bg.png\"}},"
		"{\"name\":\"Abs\",\"uuid\":\"u-abs\",\"id\":\"image_source\","
		"\"settings\":{\"file\":\"C:/art/fg.png\"}},"
		"{\"name\":\"Unix\",\"id\":\"image_source\",\"settings\":{\"file\":\"/srv/x.png\"}},"
		"{\"name\":\"Pic 2\",\"id\":\"color_source_v3\",\"settings\":{\"color\":1}}],"
		"\"groups\":[]}");
	struct sj_err e = {""};
	int n = stage_sample_prepare(col, "D:\\data\\samples\\", fake_taken, (void *)used, &e);
	CHECK(n == 1, "prepare: one relative path (%d, %s)", n, e.msg);
	CHECK(strcmp(sample_src(col, 1, "file", true), "D:/data/samples/art/bg.png") == 0,
	      "relative path made absolute, forward slashes: %s", sample_src(col, 1, "file", true));
	CHECK(strcmp(sample_src(col, 2, "file", true), "C:/art/fg.png") == 0 &&
		      strcmp(sample_src(col, 3, "file", true), "/srv/x.png") == 0,
	      "absolute paths kept");
	CHECK(strcmp(sample_src(col, 0, "name", false), "Scene 4") == 0 &&
		      strcmp(sample_src(col, 1, "name", false), "Pic 2") == 0 &&
		      strcmp(sample_src(col, 2, "name", false), "Abs") == 0 &&
		      strcmp(sample_src(col, 4, "name", false), "Pic 2 2") == 0,
	      "unique names, also within the collection: %s, %s, %s",
	      sample_src(col, 0, "name", false), sample_src(col, 1, "name", false),
	      sample_src(col, 4, "name", false));
	CHECK(strlen(sample_src(col, 0, "uuid", false)) == 36 &&
		      strlen(sample_src(col, 3, "uuid", false)) == 36,
	      "new uuids");
	CHECK(strcmp(sample_item(col, 0, 0, "source_uuid"), sample_src(col, 1, "uuid", false)) == 0 &&
		      strcmp(sample_item(col, 0, 0, "name"), "Pic 2") == 0 &&
		      strcmp(sample_item(col, 0, 1, "source_uuid"), sample_src(col, 2, "uuid", false)) == 0,
	      "scene items follow their sources (by uuid, else by name)");
	{
		obs_data_array_t *srcs = obs_data_get_array(col, "sources");
		obs_data_t *s = obs_data_array_item(srcs, 0);
		obs_data_array_t *filters = obs_data_get_array(s, "filters");
		obs_data_t *f = obs_data_array_item(filters, 0);
		CHECK(f && !obs_data_has_user_value(f, "uuid"), "filter uuid dropped");
		obs_data_release(f);
		obs_data_array_release(filters);
		obs_data_release(s);
		obs_data_array_release(srcs);
	}
	obs_data_release(col);

	static const char *const bad[][2] = {
		{"{\"sources\":[{\"name\":\"S\",\"id\":\"scene\",\"settings\":{\"items\":"
		 "[{\"name\":\"Elsewhere\",\"source_uuid\":\"u-x\"}]}}]}",
		 "not in the collection"},
		{"{\"sources\":[{\"name\":\"S\",\"id\":\"scene\"}],\"groups\":[{\"name\":\"G\"}]}", "groups"},
		{"{\"sources\":[]}", "no sources"},
		{"{\"sources\":[{\"id\":\"scene\"}]}", "no usable name"},
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		col = obs_data_create_from_json(bad[i][0]);
		e.msg[0] = '\0';
		CHECK(stage_sample_prepare(col, "D:/x", NULL, NULL, &e) == -1 && strstr(e.msg, bad[i][1]),
		      "bad collection %zu: %s", i, e.msg);
		obs_data_release(col);
	}

	/* The shipped sample, imported twice into the running libobs. */
	char path[700];
	snprintf(path, sizeof(path), "%s/%s", data_dir, STAGE_SAMPLE_FILE);
	obs_source_t *a = stage_sample_import(path, &e);
	obs_source_t *b = stage_sample_import(path, &e);
	CHECK(a && b, "sample imported twice: %s", e.msg);
	if (a && b) {
		CHECK(strcmp(obs_source_get_name(a), "Parallaxa Sample - Just Chatting") == 0 &&
			      strcmp(obs_source_get_name(b), "Parallaxa Sample - Just Chatting 2") == 0,
		      "second import gets a unique scene name: %s", obs_source_get_name(b));
		CHECK(obs_source_filter_count(b) == 2, "sample scene filters: %zu",
		      obs_source_filter_count(b));
		struct sample_items ia = {0}, ib = {0};
		obs_scene_enum_items(obs_scene_from_source(a), sample_item_cb, &ia);
		obs_scene_enum_items(obs_scene_from_source(b), sample_item_cb, &ib);
		CHECK(ia.n == 3 && ib.n == 3, "sample items: %d, %d", ia.n, ib.n);
		CHECK(strcmp(ia.names[0], "Background") == 0 && strcmp(ib.names[0], "Background 2") == 0 &&
			      strcmp(ib.names[1], "VTube Studio (Spout2) 2") == 0 &&
			      strcmp(ib.names[2], "Foreground 2") == 0,
		      "second import's sources are its own: %s, %s, %s", ib.names[0], ib.names[1],
		      ib.names[2]);
		CHECK(strcmp(ib.roles[0], "bg") == 0 && strcmp(ib.roles[1], "char") == 0 &&
			      strcmp(ib.roles[2], "fg") == 0,
		      "item roles kept: %s %s %s", ib.roles[0], ib.roles[1], ib.roles[2]);
		static const char *const want[2] = {"/presets/samples/stage_sample_bg.png",
						    "/presets/samples/stage_sample_fg.png"};
		for (int i = 0; i < 2; i++) {
			const char *f = ib.file[i * 2];
			size_t fl = strlen(f), wl = strlen(want[i]);
			CHECK(fl > wl && strcmp(f + fl - wl, want[i]) == 0 && !strchr(f, '\\') &&
				      path_absolute(f) && os_file_exists(f),
			      "image path absolute and present: %s", f);
		}
	}
	obs_source_release(a);
	obs_source_release(b);
	CHECK(!stage_sample_import("no/such/collection.json", &e) && strstr(e.msg, "cannot read"),
	      "missing collection: %s", e.msg);

	/* Only a type no plugin registers is logged as missing, not a versioned one. */
	struct obs_source_info probe = {.id = "stage_probe", .type = OBS_SOURCE_TYPE_INPUT,
					.version = 2, .get_name = probe_name,
					.create = probe_create, .destroy = probe_destroy};
	obs_register_source(&probe);
	static const char probe_json[] =
		"{\"sources\":[{\"name\":\"Probe Scene\",\"id\":\"scene\",\"settings\":{\"items\":["
		"{\"name\":\"Probe\"},{\"name\":\"Gone\"}]}},"
		"{\"name\":\"Probe\",\"id\":\"stage_probe\",\"versioned_id\":\"stage_probe_v2\"},"
		"{\"name\":\"Gone\",\"id\":\"stage_gone\",\"versioned_id\":\"stage_gone_v2\"}]}";
	os_mkdirs(scratch);
	snprintf(path, sizeof(path), "%s/probe-collection.json", scratch);
	os_quick_write_utf8_file(path, probe_json, sizeof(probe_json) - 1, false);
	int missing_before = sample_missing;
	obs_source_t *p = stage_sample_import(path, &e);
	CHECK(p && sample_missing == missing_before + 1 && strstr(last_missing, "'stage_gone'"),
	      "missing type logged once, by unversioned id (%d): %s", sample_missing - missing_before,
	      last_missing);
	obs_source_release(p);
	os_unlink(path);
}

/* ------------------------------------------------------------------------- */
/* UI thread: the main thread plays it (obs_set_ui_task_handler marks the    */
/* calling thread). Tasks run inline unless deferred mode keeps them.        */
/* ------------------------------------------------------------------------- */

struct ui_task {
	obs_task_t task;
	void *param;
};
static pthread_mutex_t ui_lock = PTHREAD_MUTEX_INITIALIZER;
static bool ui_defer;
static struct ui_task ui_tasks[64];
static size_t ui_ntasks;

static void ui_handler(obs_task_t task, void *param, bool wait)
{
	(void)wait;
	pthread_mutex_lock(&ui_lock);
	if (ui_defer && ui_ntasks < 64) {
		ui_tasks[ui_ntasks].task = task;
		ui_tasks[ui_ntasks++].param = param;
		pthread_mutex_unlock(&ui_lock);
		return;
	}
	pthread_mutex_unlock(&ui_lock);
	task(param);
}

static size_t ui_run_deferred(void)
{
	struct ui_task run[64];
	pthread_mutex_lock(&ui_lock);
	size_t n = ui_ntasks;
	memcpy(run, ui_tasks, n * sizeof(run[0]));
	ui_ntasks = 0;
	pthread_mutex_unlock(&ui_lock);
	for (size_t i = 0; i < n; i++)
		run[i].task(run[i].param);
	return n;
}

/* Whether the main scene's private settings (not the overlay) hold slot. */
static bool priv_has_view(long long slot)
{
	obs_source_t *s = obs_get_source_by_uuid(main_uuid);
	obs_data_t *priv = obs_source_get_private_settings(s);
	obs_data_array_t *arr = obs_data_get_array(priv, STAGE_SCENE_KEY_VIEWS);
	bool found = false;
	for (size_t i = 0; i < obs_data_array_count(arr) && !found; i++) {
		obs_data_t *d = obs_data_array_item(arr, i);
		found = obs_data_get_int(d, "slot") == slot;
		obs_data_release(d);
	}
	obs_data_array_release(arr);
	obs_data_release(priv);
	obs_source_release(s);
	return found;
}

static bool list_has_view(long long slot)
{
	obs_data_t *res = call("view_list", "{}");
	obs_data_array_t *arr = obs_data_get_array(res, "views");
	bool found = false;
	for (size_t i = 0; i < obs_data_array_count(arr) && !found; i++) {
		obs_data_t *d = obs_data_array_item(arr, i);
		found = obs_data_get_int(d, "slot") == slot;
		obs_data_release(d);
	}
	obs_data_array_release(arr);
	obs_data_release(res);
	return found;
}

static volatile long writer_ok;

static void *view_writer(void *p)
{
	(void)p;
	obs_data_t *res = call("view_save", "{\"slot\":7,\"name\":\"From a thread\"}");
	if (is_ok(res))
		os_atomic_inc_long(&writer_ok);
	obs_data_release(res);
	res = call("view_delete", "{\"slot\":1}");
	if (is_ok(res))
		os_atomic_inc_long(&writer_ok);
	obs_data_release(res);
	return NULL;
}

/* Views written off the UI thread: visible at once (overlay), persisted to
 * the scene private settings only by the UI task (obs_data has no
 * lock and the frontend saves the same private settings on the UI thread).
 * Then view_save during a move stores and answers the camera target. */
static void test_views_threads(void)
{
	struct drained d;
	CHECK(list_has_view(1) && priv_has_view(1) && !priv_has_view(7), "views before");

	ui_defer = true;
	pthread_t th;
	pthread_create(&th, NULL, view_writer, NULL);
	pthread_join(th, NULL);
	CHECK(writer_ok == 2, "thread view_save / view_delete");
	CHECK(list_has_view(7) && !list_has_view(1), "off-UI-thread writes visible at once");
	CHECK(!priv_has_view(7) && priv_has_view(1), "private settings untouched off the UI thread");
	ui_defer = false;
	size_t ran = ui_run_deferred();
	CHECK(ran == 2, "one UI task per write (%zu)", ran);
	CHECK(priv_has_view(7) && !priv_has_view(1), "UI task persisted the views");
	CHECK(list_has_view(7) && !list_has_view(1), "views after persisting");
	drain_all(&d);

	/* view_save during a move: the reply, the stored slot and the command
	 * all carry the camera target, not the pose seen mid-transition. */
	uint64_t id = 777;
	rig_set(set_move, &id);
	struct stage_state idle;
	pthread_mutex_lock(&rig.lock);
	idle = rig.st;
	rig.st.x = 12.5f; /* seen: halfway to the target */
	rig.st.zoom = 1.3f;
	rig.st.rot = -1.0f;
	rig.st.tx = 25.0f;
	rig.st.ty = -50.0f;
	rig.st.tzoom = 2.0f;
	rig.st.trot = -4.0f;
	pthread_mutex_unlock(&rig.lock);
	obs_data_t *res =
		call("view_save", "{\"slot\":4,\"transition\":\"cut\",\"duration_ms\":0}");
	CHECK(is_ok(res), "view_save while moving");
	{
		obs_data_t *v = obs_data_get_obj(res, "view");
		obs_data_t *cam = obs_data_get_obj(v, "camera");
		CHECK(obs_data_get_int(v, "duration_ms") == 0, "view_save keeps duration_ms 0");
		CHECK(obs_data_get_double(cam, "x") == 25.0 && obs_data_get_double(cam, "y") == -50.0 &&
			      obs_data_get_double(cam, "zoom") == 2.0 &&
			      obs_data_get_double(cam, "rot") == -4.0,
		      "view_save while moving answers the target: %s", obs_data_get_json(cam));
		obs_data_release(cam);
		obs_data_release(v);
	}
	obs_data_release(res);
	struct stage_view got;
	CHECK(stage_views_get(obs_scene_get_source(main_scene), 4, &got) && got.x == 25.0f &&
		      got.y == -50.0f && got.zoom == 2.0f && got.rot == -4.0f &&
		      got.duration_ms == 0,
	      "view_save while moving stores the target");
	drain_all(&d);
	CHECK(d.n == 1 && d.c[0].type == STAGE_CMD_VIEW_SAVE && d.c[0].u.view.slot == 4 &&
		      d.c[0].u.view.cam.mask == 0x0F && d.c[0].u.view.cam.x == 25.0f &&
		      d.c[0].u.view.cam.zoom == 2.0f && d.c[0].duration_ms == 0 &&
		      !d.c[0].u.view.keep_pose,
	      "view_save while moving: the command carries the target and duration");

	/* Full queue: the slot is stored before the command (a panel refresh),
	 * so the request still succeeds with the target. */
	stage_cmdq_set_max_age_ms(0);
	struct stage_cmd filler;
	memset(&filler, 0, sizeof(filler));
	filler.type = STAGE_CMD_CAMERA_RESET;
	stage_strlcpy(filler.scene_uuid, main_uuid, sizeof(filler.scene_uuid));
	while (stage_cmdq_try_push(&filler))
		;
	res = call("view_save", "{\"slot\":5,\"name\":\"Queued\"}");
	CHECK(is_ok(res), "view_save with a full queue");
	obs_data_release(res);
	CHECK(stage_views_get(obs_scene_get_source(main_scene), 5, &got) &&
		      strcmp(got.name, "Queued") == 0 && got.zoom == 2.0f,
	      "full queue: the target is stored");
	drain_all(&d);
	stage_cmdq_purge(NULL);
	stage_cmdq_set_max_age_ms(STAGE_CMDQ_DEFAULT_MAX_AGE_MS);
	pthread_mutex_lock(&rig.lock);
	rig.st = idle;
	pthread_mutex_unlock(&rig.lock);
	id = 0;
	rig_set(set_move, &id);
	res = call("view_delete", "{\"slot\":4}");
	obs_data_release(res);
	res = call("view_delete", "{\"slot\":5}");
	obs_data_release(res);
	res = call("view_delete", "{\"slot\":7}");
	obs_data_release(res);
	drain_all(&d);
}

static void *view_saver8(void *p)
{
	obs_data_t *res = call("view_save", p);
	if (is_ok(res))
		os_atomic_inc_long(&writer_ok);
	obs_data_release(res);
	return NULL;
}

/* A vendor view_save on its own thread, then its command. */
static uint32_t vendor_save8(const char *json, struct drained *d)
{
	pthread_t th;
	pthread_create(&th, NULL, view_saver8, (void *)json);
	pthread_join(th, NULL);
	drain_all(d);
	return d->n == 1 && d->c[0].type == STAGE_CMD_VIEW_SAVE ? d->c[0].u.view.gen : 0;
}

/* The rig's re-store of a vendor write (its UI task, later) never
 * overwrites a newer write: the panel's Save view (UI thread) landing in
 * between the vendor's store and the re-store wins. */
static void test_views_restore(void)
{
	obs_source_t *scene = obs_scene_get_source(main_scene);
	struct drained d;
	struct stage_view got, panel = {0};
	drain_all(&d);
	CHECK(!stage_views_get(scene, 8, NULL), "restore: slot 8 empty");
	ui_defer = true; /* the vendor's overlay waits for its UI task */

	/* Write generations: new on every write, untouched by a miss. */
	panel.slot = 8;
	stage_strlcpy(panel.name, "Panel", sizeof(panel.name));
	panel.x = 10.0f;
	panel.zoom = 1.25f;
	uint32_t g1 = 0, g2 = 0, g3 = 0;
	CHECK(stage_views_put(scene, &panel, &g1) && g1 != 0, "put gives a generation");
	CHECK(stage_views_edit(scene, 8, "Panel", -1, -1, NULL, &g2) && g2 != 0 && g2 != g1,
	      "edit gives a new generation");
	CHECK(stage_views_delete(scene, 8, &g3) && g3 != 0 && g3 != g2,
	      "delete gives a new generation");
	g1 = 0;
	CHECK(!stage_views_delete(scene, 8, &g1) && g1 == 0 &&
		      !stage_views_edit(scene, 8, NULL, -1, -1, NULL, &g1) && g1 == 0,
	      "no write, no generation");
	CHECK(!stage_views_delete_if(scene, 8, g3), "re-delete of an empty slot");

	/* view_save, then the panel's Save view, then the rig's re-store. */
	pthread_mutex_lock(&rig.lock);
	struct stage_state idle = rig.st;
	rig.st.tx = 70.0f;
	rig.st.tzoom = 1.5f;
	pthread_mutex_unlock(&rig.lock);
	writer_ok = 0;
	uint32_t gv = vendor_save8("{\"slot\":8,\"name\":\"Vendor\"}", &d);
	CHECK(writer_ok == 1 && gv != 0 && d.c[0].u.view.cam.x == 70.0f,
	      "view_save: the command carries the store's generation");
	struct stage_view vend;
	CHECK(stage_views_get(scene, 8, &vend) && vend.x == 70.0f, "vendor view stored");
	uint32_t gp = 0;
	CHECK(stage_views_put(scene, &panel, &gp) && gp != gv, "panel Save view in between");
	CHECK(!stage_views_put_if(scene, &vend, gv), "stale re-store skipped");
	CHECK(stage_views_get(scene, 8, &got) && strcmp(got.name, "Panel") == 0 && got.x == 10.0f,
	      "the panel's view stays");
	CHECK(stage_views_put_if(scene, &panel, gp) && stage_views_put_if(scene, &panel, gp),
	      "a current re-store is done and keeps the generation");
	ui_run_deferred(); /* the vendor's persist task: the panel's write took it over */
	CHECK(priv_has_view(8) && stage_views_get(scene, 8, &got) && got.x == 10.0f,
	      "the panel's view persisted");

	/* keep_pose: the edit and its re-store keep the stored pose, even one
	 * far outside the current canvas; a newer pose is never put back. */
	panel.x = 50000.0f;
	panel.y = -40000.0f;
	CHECK(stage_views_put(scene, &panel, NULL), "pose saved on a larger canvas");
	uint32_t gk = vendor_save8("{\"slot\":8,\"name\":\"Wide\",\"keep_pose\":true}", &d);
	CHECK(gk != 0 && d.c[0].u.view.keep_pose && d.c[0].u.view.cam.x == 50000.0f &&
		      d.c[0].u.view.cam.y == -40000.0f,
	      "keep_pose: the command carries the stored pose untouched");
	CHECK(stage_views_get(scene, 8, &got) && strcmp(got.name, "Wide") == 0 &&
		      got.x == 50000.0f && got.y == -40000.0f && got.zoom == 1.25f,
	      "keep_pose edit keeps the stored pose");
	struct stage_view edited = got;
	panel.x = 5.0f;
	panel.y = 0.0f;
	CHECK(stage_views_put(scene, &panel, NULL), "panel Save view after the edit");
	CHECK(!stage_views_put_if(scene, &edited, gk), "stale keep_pose re-store skipped");
	CHECK(stage_views_get(scene, 8, &got) && got.x == 5.0f && strcmp(got.name, "Panel") == 0,
	      "the panel's pose stays after a keep_pose edit");

	/* view_delete, then the panel saves the slot again, then the rig's
	 * re-delete: the new view stays. */
	obs_data_t *res = call("view_delete", "{\"slot\":8}");
	CHECK(is_ok(res), "view_delete 8");
	obs_data_release(res);
	drain_all(&d);
	uint32_t gd = d.n == 1 && d.c[0].type == STAGE_CMD_VIEW_DELETE ? d.c[0].u.view.gen : 0;
	CHECK(gd != 0, "view_delete: the command carries the store's generation");
	CHECK(stage_views_put(scene, &panel, &gp), "panel saves the slot again");
	CHECK(!stage_views_delete_if(scene, 8, gd) && stage_views_get(scene, 8, NULL),
	      "stale re-delete skipped");
	CHECK(stage_views_delete_if(scene, 8, gp) && !stage_views_get(scene, 8, NULL),
	      "a current re-delete is done");
	CHECK(stage_views_put_if(scene, &panel, 0) && stage_views_delete_if(scene, 8, 0),
	      "generation 0: ordinary put and delete");

	pthread_mutex_lock(&rig.lock);
	rig.st = idle;
	pthread_mutex_unlock(&rig.lock);
	ui_defer = false;
	ui_run_deferred();
	CHECK(!priv_has_view(8), "restore: slot 8 empty again");
	drain_all(&d);
}

/* Hotkeys: callbacks (run here on a thread of their own, as on the OBS
 * hotkey thread) only push commands built from registry snapshots; the
 * rig tick resolves saved views, next / previous and rack focus. */
struct hk_find {
	const char *name;
	obs_hotkey_id id;
};

static bool hk_find_cb(void *data, obs_hotkey_id id, obs_hotkey_t *key)
{
	struct hk_find *f = data;
	if (strcmp(obs_hotkey_get_name(key), f->name) != 0)
		return true;
	f->id = id;
	return false;
}

static void *hk_press(void *name)
{
	struct hk_find f = {name, OBS_INVALID_HOTKEY_ID};
	obs_enum_hotkeys(hk_find_cb, &f);
	if (f.id != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_trigger_routed_callback(f.id, true);
		obs_hotkey_trigger_routed_callback(f.id, false);
	}
	return NULL;
}

static size_t press(const char *name, struct drained *d)
{
	pthread_t th;
	pthread_create(&th, NULL, hk_press, (void *)name);
	pthread_join(th, NULL);
	return drain_all(d);
}

static void test_hotkeys(void)
{
	struct drained d;
	drain_all(&d);
	obs_hotkey_enable_callback_rerouting(true);

	CHECK(press("stage.view_3", &d) == 1 && d.c[0].type == STAGE_CMD_VIEW_APPLY &&
		      d.c[0].u.view.slot == 3 && d.c[0].u.view.step == 0 && d.c[0].u.view.cam.mask == 0 &&
		      strcmp(d.c[0].scene_uuid, main_uuid) == 0,
	      "view 3: slot only, the rig reads the stored view");
	CHECK(press("stage.view_next", &d) == 1 && d.c[0].type == STAGE_CMD_VIEW_APPLY &&
		      d.c[0].u.view.step == 1 && d.c[0].u.view.slot == 0 && d.c[0].u.view.cam.mask == 0,
	      "next view: step +1 picked by the rig");
	CHECK(press("stage.view_prev", &d) == 1 && d.c[0].u.view.step == -1, "previous view: step -1");
	CHECK(press("stage.rack_bg", &d) == 1 && d.c[0].type == STAGE_CMD_DOF_SET &&
		      d.c[0].u.dof.mask == STAGE_DOF_RACK_BG && d.c[0].duration_ms == 1200,
	      "rack focus: depth and amount picked by the rig (mask %u)", d.c[0].u.dof.mask);

	/* FX toggle: a toggle command the FX tick flips in queue order
	 * (fx_params_test), so two presses before one tick queue two flips,
	 * not the same value read twice. */
	pthread_t th;
	for (int i = 0; i < 2; i++) {
		pthread_create(&th, NULL, hk_press, (void *)"stage.fx_bloom");
		pthread_join(th, NULL);
	}
	size_t n = drain_all(&d);
	bool flips = n == 2;
	for (size_t i = 0; flips && i < n; i++)
		flips = d.c[i].type == STAGE_CMD_FX_SET &&
			d.c[i].u.fx.toggle == STAGE_FX_TOGGLE_BLOOM + 1 && !d.c[i].u.fx.preset[0] &&
			!d.c[i].u.fx.params_json[0] && d.c[i].duration_ms == 200 &&
			strcmp(d.c[i].scene_uuid, main_uuid) == 0;
	CHECK(flips, "bloom toggle x2: two toggle commands (%zu queued)", n);
	CHECK(press("stage.fx_leak", &d) == 1 && d.c[0].u.fx.toggle == STAGE_FX_TOGGLE_LEAK + 1,
	      "leak toggle: effect 4");
	CHECK(press("stage.fx_off", &d) == 1 && d.c[0].u.fx.toggle == 0 &&
		      strcmp(d.c[0].u.fx.preset, "neutral") == 0,
	      "all effects off: the neutral look, no toggle");

	obs_hotkey_enable_callback_rerouting(false);
}

/* obs-websocket's server_enabled flag (installed but disabled server). */
static void test_ws_config(const char *scratch)
{
	char path[700];
	os_mkdirs(scratch);
	snprintf(path, sizeof(path), "%s/ws-config.json", scratch);
	os_unlink(path);
	CHECK(stage_vendor_ws_server_enabled(path) == -1, "no obs-websocket config: unknown");
	CHECK(stage_vendor_ws_server_enabled(NULL) == -1, "NULL path: unknown");
	os_quick_write_utf8_file(path, "{\"server_enabled\":false,\"server_port\":4455}", 43, false);
	CHECK(stage_vendor_ws_server_enabled(path) == 0, "server disabled");
	os_quick_write_utf8_file(path, "{\"server_enabled\":true}", 23, false);
	CHECK(stage_vendor_ws_server_enabled(path) == 1, "server enabled");
	os_quick_write_utf8_file(path, "{\"server_port\":4455}", 20, false);
	CHECK(stage_vendor_ws_server_enabled(path) == -1, "no server_enabled key: unknown");
	os_unlink(path);
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: vendor_test <data> <scratch dir> [dump.jsonl]\n");
		return 2;
	}
	base_set_log_handler(log_handler, NULL);
	if (argc > 3) {
		dump = fopen(argv[3], "wb");
		CHECK(dump != NULL, "cannot write %s", argv[3]);
	}
#ifdef _WIN32
	if (!LoadLibraryA("obs-frontend-api.dll")) {
		fprintf(stderr, "FAIL obs-frontend-api.dll not found on PATH\n");
		return 1;
	}
#endif
	/* Baseline: what libobs alone leaves allocated per start/stop cycle
	 * that creates and releases the same scenes (headless libobs keeps a
	 * few scene allocations past obs_shutdown; they are not ours). Two
	 * cycles, so a one-time allocation is not mistaken for a per-run one. */
	long base_runs[2];
	for (int run = 0; run < 2; run++) {
		if (!obs_startup("en-US", NULL, NULL)) {
			fprintf(stderr, "FAIL obs_startup\n");
			return 1;
		}
		obs_scene_t *m = obs_scene_create("Just Chatting");
		for (int i = 0; i < 4; i++) {
			char n[16];
			snprintf(n, sizeof(n), "layer %d", i);
			obs_scene_t *c = obs_scene_create(n);
			obs_scene_add(m, obs_scene_get_source(c));
			obs_scene_release(c);
		}
		obs_scene_release(m);
		obs_shutdown();
		base_runs[run] = bnum_allocs();
	}
	long per_run = base_runs[1] - base_runs[0];
	long baseline = base_runs[1] + per_run;
	if (!obs_startup("en-US", NULL, NULL)) {
		fprintf(stderr, "FAIL obs_startup\n");
		return 1;
	}
	char locale[512];
	snprintf(locale, sizeof(locale), "%s/locale/en-US.ini", argv[1]);
	obs_module_lookup = text_lookup_create(locale);
	CHECK(obs_module_lookup != NULL, "locale %s", locale);

	obs_set_ui_task_handler(ui_handler); /* this thread is the UI thread */
	stage_core_init();
	stage_frontend_attach();

	printf("presets\n");
	test_presets(argv[1], argv[2]);

	printf("vendor init without obs-websocket\n");
	int warn_before = stage_warnings;
	stage_vendor_init();
	CHECK(stage_warnings == warn_before + 1 && strstr(last_warning, "obs-websocket"),
	      "exactly one warning when obs-websocket is missing (%d): %s", stage_warnings - warn_before,
	      last_warning);
	stage_vendor_init(); /* idempotent */
	CHECK(stage_warnings == warn_before + 1, "init twice: still one warning");
	struct hk_count hc = {0};
	obs_enum_hotkeys(hk_cb, &hc);
	CHECK(hc.stage == 25 && hc.ai == 5 && hc.view3 == 1,
	      "hotkeys: %d Parallaxa, %d AI Live Studio", hc.stage, hc.ai);

	setup_scenes();
	printf("requests\n");
	test_requests();
	printf("views off the UI thread\n");
	test_views_threads();
	printf("view re-store after a newer write\n");
	test_views_restore();
	printf("hotkeys\n");
	test_hotkeys();
	printf("obs-websocket config\n");
	test_ws_config(argv[2]);
	printf("user recipes\n");
	test_user_recipe();
	printf("events\n");
	test_events();
	printf("stress\n");
	test_stress();
	printf("sample scene import\n");
	test_sample(argv[1], argv[2]);

	teardown_scenes();
	stage_vendor_shutdown();
	stage_vendor_shutdown(); /* idempotent */
	stage_frontend_detach();
	stage_presets_free();  /* stage_presets_init's unload hook in the plugin */
	stage_views_discard_pending();
	stage_core_shutdown(); /* unload hooks: hotkeys unregister */
	text_lookup_destroy(obs_module_lookup);
	if (dump)
		fclose(dump);
	obs_module_lookup = NULL;
	obs_shutdown();
	long leaks = bnum_allocs() - baseline;
	CHECK(leaks == 0, "allocations after shutdown: %ld more than libobs alone (%ld per run)",
	      leaks, per_run);
	CHECK(stage_errors == 0, "%d [parallaxa] error lines", stage_errors);

	printf("vendor_test: %d checks, %d failures\n", checks, failures);
	printf("%s\n", failures ? "vendor_test: FAILED" : "vendor_test: OK");
	return failures ? 1 : 0;
}
