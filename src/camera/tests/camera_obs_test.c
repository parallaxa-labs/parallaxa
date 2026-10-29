/*
 * camera_obs_test.c — black-box test of stage_camera_rig inside a headless
 * libobs (the installed OBS's obs.dll + libobs-d3d11, no OBS UI, nothing
 * written to the OBS config). Loads parallaxa.dll and drives the rig
 * only through public OBS APIs (settings, properties, filters, items):
 *   - rest poses at home (bg overscan), parallax on zoom (fg > char > bg),
 *   - dragging an item updates its rest pose (stage.rest) and keeps it put,
 *   - the saved scene JSON holds rest poses, not the animated pose,
 *   - Bake rest pose / disable / remove restore rest poses (< 0.5 px),
 *   - 10 save/load cycles with a zoomed camera do not drift,
 *   - a second rig on the same scene stays inactive,
 *   - audio bounce follows an audio source; removing it 20 times while
 *     audio flows does not crash,
 *   - an idle camera writes no transforms; a tick with 10 moving layers
 *     costs < 0.05 ms; the rig leaks no allocation.
 *
 * Usage: camera_obs_test <parallaxa.dll> <data> <OBS install dir> [work dir]
 * PATH must contain the OBS bin\64bit folder (ctest sets it). The test data
 * folder goes to the work dir (default: the temp folder). Exits 77 (ctest
 * SKIP_RETURN_CODE) when no D3D11 device is available.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <obs.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <util/platform.h>

#include "stage-protocol.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static int failures, checks, stage_errors, move_warnings;

#define CHECK(cond, ...)                                            \
	do {                                                        \
		checks++;                                           \
		if (!(cond)) {                                      \
			failures++;                                 \
			printf("FAIL line %d: ", __LINE__);         \
			printf(__VA_ARGS__);                        \
			printf("\n");                               \
		}                                                   \
	} while (0)

/* Average tick cost of the rig on the perf scene, from its debug line. */
static volatile float perf_tick_us = -1.0f;

static void log_handler(int level, const char *fmt, va_list args, void *param)
{
	(void)param;
	char msg[2048];
	vsnprintf(msg, sizeof(msg), fmt, args);
	if (getenv("STAGE_CAM_TEST_VERBOSE"))
		printf("  obs: %s\n", msg);
	if (strstr(msg, "[parallaxa]")) {
		printf("  log: %s\n", msg);
		if (level <= LOG_ERROR)
			stage_errors++;
		if (level == LOG_WARNING && strstr(msg, "move file '"))
			move_warnings++;
		const char *p = strstr(msg, "rig 'Perf Test': tick ");
		float us;
		if (p && sscanf(p + strlen("rig 'Perf Test': tick "), "%f", &us) == 1)
			perf_tick_us = us;
	}
}

static volatile long transform_signals;

static void on_item_transform(void *data, calldata_t *cd)
{
	(void)data;
	(void)cd;
	transform_signals++;
}

/* UI tasks run at once on the calling thread (no Qt here). */
static void ui_handler(obs_task_t task, void *param, bool wait)
{
	(void)wait;
	task(param);
}

static void wait_ms(int ms)
{
	os_sleep_ms((uint32_t)ms);
}

/* ------------------------------------------------------------------------- */
/* A test audio source (sine tone pushed from the test thread)               */
/* ------------------------------------------------------------------------- */

static const char *tone_name(void *unused)
{
	(void)unused;
	return "Test tone";
}
static void *tone_create(obs_data_t *settings, obs_source_t *source)
{
	(void)settings;
	return source;
}
static void tone_destroy(void *data)
{
	(void)data;
}

static void tone_push(obs_source_t *src, float amp, int ms)
{
	static float buf[480];
	static double phase;
	struct obs_source_audio a = {0};
	a.speakers = SPEAKERS_MONO;
	a.format = AUDIO_FORMAT_FLOAT;
	a.samples_per_sec = 48000;
	a.frames = 480;
	a.data[0] = (const uint8_t *)buf;
	for (int t = 0; t < ms; t += 10) {
		for (int i = 0; i < 480; i++) {
			buf[i] = amp * (float)sin(phase);
			phase += 2.0 * 3.14159265 * 220.0 / 48000.0;
		}
		a.timestamp = os_gettime_ns();
		obs_source_output_audio(src, &a);
		wait_ms(10);
	}
}

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

struct item_xf {
	float px, py, sx, sy, rot;
};

static struct item_xf item_now(obs_sceneitem_t *it)
{
	struct obs_transform_info info;
	obs_sceneitem_get_info2(it, &info);
	struct item_xf x = {info.pos.x, info.pos.y, info.scale.x, info.scale.y, info.rot};
	return x;
}

static bool item_rest(obs_sceneitem_t *it, struct item_xf *out)
{
	obs_data_t *priv = obs_sceneitem_get_private_settings(it);
	obs_data_t *rest = obs_data_get_obj(priv, STAGE_ITEM_KEY_REST);
	bool ok = rest && obs_data_has_user_value(rest, STAGE_REST_KEY_PX);
	if (ok) {
		out->px = (float)obs_data_get_double(rest, STAGE_REST_KEY_PX);
		out->py = (float)obs_data_get_double(rest, STAGE_REST_KEY_PY);
		out->sx = (float)obs_data_get_double(rest, STAGE_REST_KEY_SX);
		out->sy = (float)obs_data_get_double(rest, STAGE_REST_KEY_SY);
		out->rot = (float)obs_data_get_double(rest, STAGE_REST_KEY_ROT);
	}
	obs_data_release(rest);
	obs_data_release(priv);
	return ok;
}

static bool near_pos(struct item_xf a, float x, float y, float tol)
{
	return fabsf(a.px - x) <= tol && fabsf(a.py - y) <= tol;
}

static obs_sceneitem_t *add_layer(obs_scene_t *scene, obs_source_t *child, float x, float y, float s,
				  const char *role, double depth)
{
	obs_sceneitem_t *it = obs_scene_add(scene, child);
	struct vec2 v;
	vec2_set(&v, x, y);
	obs_sceneitem_set_pos(it, &v);
	vec2_set(&v, s, s);
	obs_sceneitem_set_scale(it, &v);
	obs_data_t *priv = obs_sceneitem_get_private_settings(it);
	obs_data_set_double(priv, STAGE_ITEM_KEY_DEPTH, depth);
	obs_data_set_string(priv, STAGE_ITEM_KEY_ROLE, role);
	obs_data_release(priv);
	return it;
}

static obs_source_t *add_rig(obs_source_t *scene_src, const char *name, double zoom)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "auto_mode", "off");
	obs_data_set_bool(s, "stage_initialized", true); /* no first-add flow */
	obs_data_set_double(s, "cam_zoom", zoom);
	obs_source_t *rig = obs_source_create(STAGE_FILTER_CAMERA_RIG, name, s, NULL);
	obs_data_release(s);
	if (rig)
		obs_source_filter_add(scene_src, rig);
	return rig;
}

static void set_zoom(obs_source_t *rig, double zoom)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_double(s, "cam_zoom", zoom);
	obs_source_update(rig, s);
	obs_data_release(s);
}

static void click(obs_source_t *src, const char *button)
{
	obs_properties_t *props = obs_source_properties(src);
	obs_property_t *p = obs_properties_get(props, button);
	CHECK(p != NULL, "button '%s' exists", button);
	if (p)
		obs_property_button_clicked(p, src);
	obs_properties_destroy(props);
}

static obs_sceneitem_t *find_item(obs_scene_t *scene, const char *src_name)
{
	obs_sceneitem_t *it = obs_scene_find_source(scene, src_name);
	return it;
}

/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* Test data folder: the real locale and moves plus extra move files         */
/* ------------------------------------------------------------------------- */

static bool copy_file(const char *from, const char *to)
{
	FILE *in = fopen(from, "rb");
	if (!in)
		return false;
	FILE *out = fopen(to, "wb");
	if (!out) {
		fclose(in);
		return false;
	}
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
		fwrite(buf, 1, n, out);
	fclose(in);
	fclose(out);
	return true;
}

static void write_text(const char *dir, const char *name, const char *text)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *f = fopen(path, "wb");
	if (f) {
		fputs(text, f);
		fclose(f);
	}
}

#define NAME3 "\"name\":{\"en\":\"T\",\"ja\":\"T\",\"id\":\"T\"}"

/* Builds <work>/stage_cam_test_data (work: the temp folder when NULL) with
 * locale/en-US.ini and presets/moves: every shipped move, one valid custom
 * move and three invalid files (bad JSON, unknown field, id not matching the
 * file name). */
static bool make_data_dir(const char *src, const char *work, char *out, size_t cap)
{
	char tmp[512] = "";
	if (work) {
		os_mkdirs(work);
		snprintf(tmp, sizeof(tmp), "%s/", work);
	}
#ifdef _WIN32
	else
		GetTempPathA(sizeof(tmp), tmp);
#endif
	snprintf(out, cap, "%sstage_cam_test_data", tmp[0] ? tmp : "./");
	char moves[1024], locale[1024], from[1024], to[1024];
	snprintf(moves, sizeof(moves), "%s/presets/moves", out);
	snprintf(locale, sizeof(locale), "%s/locale", out);
	os_mkdirs(moves);
	os_mkdirs(locale);
	snprintf(from, sizeof(from), "%s/locale/en-US.ini", src);
	snprintf(to, sizeof(to), "%s/en-US.ini", locale);
	if (!copy_file(from, to))
		return false;
	/* Shipped moves, and the effects other modules of a full build load. */
	static const char *const dirs[2] = {"presets/moves", "effects"};
	for (int k = 0; k < 2; k++) {
		char sdir[1024], ddir[1024];
		snprintf(sdir, sizeof(sdir), "%s/%s", src, dirs[k]);
		snprintf(ddir, sizeof(ddir), "%s/%s", out, dirs[k]);
		os_dir_t *d = os_opendir(sdir);
		if (!d)
			continue;
		os_mkdirs(ddir);
		struct os_dirent *e;
		while ((e = os_readdir(d)) != NULL) {
			if (e->directory)
				continue;
			snprintf(from, sizeof(from), "%s/%s", sdir, e->d_name);
			snprintf(to, sizeof(to), "%s/%s", ddir, e->d_name);
			copy_file(from, to);
		}
		os_closedir(d);
	}
	write_text(moves, "orbit_test.json",
		   "{\"id\":\"orbit_test\"," NAME3 ",\"duration_ms\":900,\"relative\":false,"
		   "\"keys\":[{\"t\":0},{\"t\":0.5,\"dx\":0.02,\"drot\":2,\"ease\":\"smoothstep\"},"
		   "{\"t\":1,\"dx\":0,\"drot\":0}]}");
	write_text(moves, "broken.json", "{ \"id\": \"broken\", ");
	write_text(moves, "unknown_field.json",
		   "{\"id\":\"unknown_field\"," NAME3 ",\"duration_ms\":500,\"spin\":true,"
		   "\"keys\":[{\"t\":0},{\"t\":1,\"dzoom\":0.1}]}");
	write_text(moves, "wrong_id.json",
		   "{\"id\":\"other_id\"," NAME3 ",\"duration_ms\":500,"
		   "\"keys\":[{\"t\":0},{\"t\":1,\"dzoom\":0.1}]}");
	return true;
}

struct hk_count {
	int moves;
	bool custom;
};

static bool hk_enum(void *data, obs_hotkey_id id, obs_hotkey_t *key)
{
	(void)id;
	struct hk_count *c = data;
	const char *name = obs_hotkey_get_name(key);
	if (name && strncmp(name, "stage_camera.move_", 18) == 0) {
		c->moves++;
		if (strcmp(name, "stage_camera.move_orbit_test") == 0)
			c->custom = true;
	}
	return true;
}

static void check_moves(int shipped)
{
	struct hk_count c = {0, false};
	obs_enum_hotkeys(hk_enum, &c);
	CHECK(c.moves == shipped + 1, "one move hotkey per valid move (%d, want %d)", c.moves, shipped + 1);
	CHECK(c.custom, "custom move file loaded");
	CHECK(move_warnings == 3, "three invalid move files logged and skipped (%d)", move_warnings);
}

/* obs_load_sources callback: keeps the reloaded test scene and tone alive
 * (obs_load_sources drops its own references when it returns). */
static void load_keep_cb(void *param, obs_source_t *source)
{
	obs_source_t **got = param; /* [0] scene, [1] tone */
	const char *name = obs_source_get_name(source);
	if (strcmp(name, "Stage Test") == 0)
		got[0] = obs_source_get_ref(source);
	else if (strcmp(name, "tone2") == 0)
		got[1] = obs_source_get_ref(source);
}

/* STAGE_CAM_TEST_PHASES=n runs phases 1..n only (leak bisection). */
static int phase_max = 99;
/* Baseline session: the same scenes and items, no rig. */
static bool scaffold_only;

static void run_tests(void)
{
	if (phase_max < 1)
		return;
	const float W = 1920.0f, H = 1080.0f, CX = W / 2, CY = H / 2;
	obs_source_t *c_bg = obs_source_create("scene", "bg-art", NULL, NULL);
	obs_source_t *c_ch = obs_source_create("scene", "avatar", NULL, NULL);
	obs_source_t *c_fg = obs_source_create("scene", "desk", NULL, NULL);
	obs_scene_t *scene = obs_scene_create("Stage Test");
	obs_source_t *ss = obs_scene_get_source(scene);

	obs_sceneitem_t *A = add_layer(scene, c_bg, 0, 0, 1.0f, "bg", 3.0);
	obs_sceneitem_t *B = add_layer(scene, c_ch, 600, 200, 0.5f, "char", 1.0);
	obs_sceneitem_t *C = add_layer(scene, c_fg, 100, 700, 0.3f, "fg", 0.6);

	obs_source_t *rig = scaffold_only ? NULL : add_rig(ss, "Parallaxa Camera Rig", 1.0);
	CHECK(rig || scaffold_only, "rig filter created");
	if (!rig)
		goto done;
	wait_ms(400);

	/* 1. Home: char/fg at rest, bg oversized 8 % about the canvas centre. */
	struct item_xf b = item_now(B), a = item_now(A), r;
	CHECK(near_pos(b, 600, 200, 0.5f) && fabsf(b.sx - 0.5f) < 1e-4f, "char at rest at home (%f %f %f)",
	      b.px, b.py, b.sx);
	/* Overscan 8 %, raised by Live-safe to the computed minimum for the
	 * camera range (max pan 15 %, max zoom 2, roll 8 deg), capped at 25 %. */
	CHECK(a.sx >= 1.08f - 1e-3f && a.sx <= 1.25f + 1e-3f && near_pos(a, CX - CX * a.sx, CY - CY * a.sx, 0.5f),
	      "bg oversized about the canvas centre (%f %f %f)", a.px, a.py, a.sx);
	const float bg_o = a.sx;
	CHECK(item_rest(B, &r) && near_pos(r, 600, 200, 0.01f), "rest pose captured into stage.rest");

	if (phase_max < 2)
		goto done;
	/* 2. Zoom 1.25: parallax fg > char > bg. */
	set_zoom(rig, 1.25);
	wait_ms(1500);
	b = item_now(B);
	struct item_xf c = item_now(C);
	a = item_now(A);
	CHECK(fabsf(b.sx - 0.625f) < 2e-3f, "char scale follows the zoom (%f)", b.sx);
	CHECK(near_pos(b, CX + (600 - CX) * 1.25f, CY + (200 - CY) * 1.25f, 1.0f), "char position (%f %f)", b.px,
	      b.py);
	float s_fg = c.sx / 0.3f, s_ch = b.sx / 0.5f, s_bg = a.sx / bg_o;
	CHECK(s_fg > s_ch && s_ch > s_bg && s_bg > 1.0f, "parallax order fg %f > char %f > bg %f", s_fg, s_ch, s_bg);
	CHECK(fabsf(s_fg - powf(1.25f, 1.0f / 0.6f)) < 0.01f, "fg zoom^(1/depth) (%f)", s_fg);

	/* 2b. A settled camera writes no transforms (no item_transform signal,
	 * hence no obs-websocket SceneItemTransformChanged storm). */
	signal_handler_t *sh = obs_source_get_signal_handler(ss);
	signal_handler_connect(sh, "item_transform", on_item_transform, NULL);
	wait_ms(1500);
	transform_signals = 0;
	wait_ms(1000);
	CHECK(transform_signals == 0, "idle camera: %ld item_transform signals in 1 s", transform_signals);
	signal_handler_disconnect(sh, "item_transform", on_item_transform, NULL);

	if (phase_max < 3)
		goto done;
	/* 3. Drag while zoomed: rest = unsolve(new pose), item stays put. */
	struct vec2 v;
	vec2_set(&v, 700, 300);
	obs_sceneitem_set_pos(B, &v);
	wait_ms(300);
	b = item_now(B);
	CHECK(near_pos(b, 700, 300, 0.5f), "dragged item stays where the user put it (%f %f)", b.px, b.py);
	CHECK(item_rest(B, &r) && near_pos(r, CX + (700 - CX) / 1.25f, CY + (300 - CY) / 1.25f, 0.5f),
	      "rest pose updated from the drag (%f %f)", r.px, r.py);
	struct item_xf rest_b = r;

	if (phase_max < 4)
		goto done;
	/* 4. The saved scene holds rest poses, not the animated pose. */
	obs_data_t *saved = obs_save_source(ss);
	obs_data_t *st = obs_data_get_obj(saved, "settings");
	obs_data_array_t *items = obs_data_get_array(st, "items");
	bool found = false;
	for (size_t i = 0; i < obs_data_array_count(items); i++) {
		obs_data_t *it = obs_data_array_item(items, i);
		if (obs_data_get_int(it, "id") == obs_sceneitem_get_id(B)) {
			struct vec2 p;
			obs_data_get_vec2(it, "pos", &p);
			found = true;
			CHECK(fabsf(p.x - rest_b.px) < 0.01f && fabsf(p.y - rest_b.py) < 0.01f,
			      "saved pos is the rest pose (%f %f)", p.x, p.y);
			CHECK(!obs_data_has_user_value(it, "pos_rel"), "relative copy dropped");
		}
		obs_data_release(it);
	}
	CHECK(found, "item found in the saved scene");
	obs_data_array_release(items);
	obs_data_release(st);
	obs_data_release(saved);

	if (phase_max < 5)
		goto done;
	/* 5. Bake rest pose: items back to rest, camera home. */
	click(rig, "bake_rest");
	wait_ms(300);
	b = item_now(B);
	CHECK(near_pos(b, rest_b.px, rest_b.py, 0.5f) && fabsf(b.sx - 0.5f) < 1e-3f,
	      "bake restores the rest pose (%f %f)", b.px, b.py);

	if (phase_max < 6)
		goto done;
	/* 6. Disable: rest poses (bg without overscan). */
	set_zoom(rig, 1.5);
	wait_ms(1500);
	obs_source_set_enabled(rig, false);
	wait_ms(300);
	a = item_now(A);
	b = item_now(B);
	CHECK(near_pos(a, 0, 0, 0.5f) && fabsf(a.sx - 1.0f) < 1e-4f, "disabled rig: bg at rest (%f %f %f)", a.px,
	      a.py, a.sx);
	CHECK(near_pos(b, rest_b.px, rest_b.py, 0.5f), "disabled rig: char at rest");
	/* Move it while disabled: becomes the new rest when re-enabled. */
	vec2_set(&v, 650, 250);
	obs_sceneitem_set_pos(B, &v);
	obs_source_set_enabled(rig, true);
	wait_ms(1500);
	CHECK(item_rest(B, &r) && near_pos(r, 650, 250, 0.5f), "edit while disabled kept as rest (%f %f)", r.px,
	      r.py);
	rest_b = r;

	if (phase_max < 7)
		goto done;
	/* 7. Second rig: inactive. */
	obs_source_t *rig2 = add_rig(ss, "Parallaxa Camera Rig 2", 3.0);
	wait_ms(500);
	b = item_now(B);
	CHECK(fabsf(b.sx - 0.5f * 1.5f) < 5e-3f, "second rig stays inactive (scale %f)", b.sx);
	obs_source_filter_remove(ss, rig2);
	obs_source_release(rig2);

	if (phase_max < 8)
		goto done;
	/* 8. Remove: rest poses. */
	obs_source_filter_remove(ss, rig);
	obs_source_release(rig);
	rig = NULL;
	wait_ms(200);
	a = item_now(A);
	b = item_now(B);
	CHECK(near_pos(a, 0, 0, 0.5f) && fabsf(a.sx - 1.0f) < 1e-4f, "removed rig: bg at rest");
	CHECK(near_pos(b, rest_b.px, rest_b.py, 0.5f) && fabsf(b.sx - 0.5f) < 1e-4f, "removed rig: char at rest");

	if (phase_max < 9)
		goto done;
	/* 9. Ten save/load cycles with a zoomed camera: no drift. */
	rig = add_rig(ss, "Parallaxa Camera Rig", 1.0);
	wait_ms(300);
	struct item_xf rest0;
	item_rest(B, &rest0);
	for (int cycle = 0; cycle < 10; cycle++) {
		set_zoom(rig, cycle % 2 ? 1.4 : 1.2);
		wait_ms(900);
		obs_data_t *data = obs_save_source(ss);
		obs_source_release(rig);
		rig = NULL;
		obs_source_remove(ss);
		obs_scene_release(scene);
		scene = NULL;
		obs_source_t *ns = obs_load_source(data);
		obs_data_release(data);
		obs_source_load2(ns);
		scene = obs_scene_from_source(ns);
		ss = ns;
		rig = obs_source_get_filter_by_name(ss, "Parallaxa Camera Rig");
		wait_ms(400);
	}
	B = find_item(scene, "avatar");
	A = find_item(scene, "bg-art");
	CHECK(B && A, "items reloaded");
	if (B && A) {
		CHECK(item_rest(B, &r) && fabsf(r.px - rest0.px) < 1e-3f && fabsf(r.py - rest0.py) < 1e-3f &&
			      fabsf(r.sx - rest0.sx) < 1e-6f,
		      "no drift after 10 save/load cycles (%f %f vs %f %f)", r.px, r.py, rest0.px, rest0.py);
		b = item_now(B);
		CHECK(near_pos(b, rest0.px, rest0.py, 0.5f), "reloaded camera starts home (%f %f)", b.px, b.py);
		obs_data_t *priv = obs_sceneitem_get_private_settings(B);
		CHECK(strcmp(obs_data_get_string(priv, STAGE_ITEM_KEY_ROLE), "char") == 0 &&
			      fabs(obs_data_get_double(priv, STAGE_ITEM_KEY_DEPTH) - 1.0) < 1e-9,
		      "role and depth kept");
		obs_data_release(priv);
	}

	if (phase_max < 10)
		goto done;
	/* 10. Audio bounce, and removing the audio source 20 times. */
	if (rig && B) {
		obs_data_t *s = obs_data_create();
		obs_data_set_int(s, "audio_amount", 100);
		obs_source_update(rig, s);
		obs_data_release(s);
		float base = item_now(B).sx;
		for (int i = 0; i < 20; i++) {
			obs_source_t *tone = obs_source_create("stage_test_tone", "tone", NULL, NULL);
			s = obs_data_create();
			obs_data_set_string(s, "audio_source", obs_source_get_uuid(tone));
			obs_source_update(rig, s);
			obs_data_release(s);
			tone_push(tone, 0.9f, i == 0 ? 600 : 80);
			if (i == 0) {
				float sc = item_now(B).sx;
				CHECK(sc > base * 1.01f, "audio bounce zooms the camera (%f -> %f)", base, sc);
			}
			obs_source_remove(tone);
			obs_source_release(tone);
		}
		wait_ms(1500);
		CHECK(fabsf(item_now(B).sx - base) < 2e-3f, "bounce released after the source is gone");
	}

	if (phase_max < 12)
		goto done;
	/* 12. Depth of field drives stage_depth_blur (FX module builds only). */
	if (rig && obs_source_get_display_name(STAGE_FILTER_DEPTH_BLUR)) {
		obs_source_t *blur = obs_source_create(STAGE_FILTER_DEPTH_BLUR, "blur", NULL, NULL);
		obs_source_filter_add(c_bg, blur);
		obs_data_t *s = obs_data_create();
		obs_data_set_bool(s, "dof_enabled", true);
		obs_data_set_int(s, "dof_amount", 50);
		obs_source_update(rig, s);
		obs_data_release(s);
		wait_ms(1500);
		s = obs_source_get_settings(blur);
		CHECK(strcmp(obs_data_get_string(s, "driven_by"), obs_source_get_uuid(ss)) == 0,
		      "rig marks the bg blur as driven (%s)", obs_data_get_string(s, "driven_by"));
		CHECK(fabs(obs_data_get_double(s, STAGE_DEPTH_BLUR_KEY_AUTO_STRENGTH) - 1.0) < 1e-3,
		      "auto_strength = clamp(|3 - 1| x 0.5) (%f)",
		      obs_data_get_double(s, STAGE_DEPTH_BLUR_KEY_AUTO_STRENGTH));
		obs_data_release(s);
		s = obs_data_create();
		obs_data_set_bool(s, "dof_enabled", false);
		obs_source_update(rig, s);
		obs_data_release(s);
		wait_ms(500);
		s = obs_source_get_settings(blur);
		CHECK(obs_data_get_string(s, "driven_by")[0] == '\0' &&
			      obs_data_get_double(s, STAGE_DEPTH_BLUR_KEY_AUTO_STRENGTH) == 0.0,
		      "depth of field off: blur no longer driven");
		obs_data_release(s);
		obs_source_filter_remove(c_bg, blur);
		obs_source_release(blur);
	} else {
		printf("  (depth blur check skipped: no stage_depth_blur in this build)\n");
	}

	if (phase_max < 11)
		goto done;
	/* 11. Tick cost with 10 layers, auto drift on (every layer moves every
	 * tick): < 0.05 ms per tick. */
	{
		obs_scene_t *ps = obs_scene_create("Perf Test");
		obs_source_t *pss = obs_scene_get_source(ps);
		static const char *const roles[10] = {"bg", "bg", "bg", "char", "char",
						      "fg", "fg", "fg", "sky", "static"};
		static const double depths[10] = {5.0, 3.0, 2.0, 1.0, 1.2, 0.8, 0.6, 0.4, 10.0, 1.0};
		for (int i = 0; i < 10; i++)
			add_layer(ps, i < 3 ? c_bg : i < 5 ? c_ch : c_fg, 50.0f * (float)i, 30.0f * (float)i,
				  i < 3 ? 1.0f : 0.4f, roles[i], depths[i]);
		obs_data_t *s = obs_data_create();
		obs_data_set_string(s, "auto_mode", "drift");
		obs_data_set_int(s, "auto_intensity", 100);
		obs_data_set_bool(s, "stage_initialized", true);
		obs_source_t *prig = obs_source_create(STAGE_FILTER_CAMERA_RIG, "Perf Rig", s, NULL);
		obs_data_release(s);
		obs_source_filter_add(pss, prig);
		perf_tick_us = -1.0f;
		wait_ms(6000);
		CHECK(perf_tick_us >= 0.0f && perf_tick_us < 50.0f, "tick cost with 10 layers: %.1f us (< 50)",
		      perf_tick_us);
		obs_source_filter_remove(pss, prig);
		obs_source_release(prig);
		obs_source_remove(pss);
		obs_scene_release(ps);
	}

	if (phase_max < 13)
		goto done;
	/* 13. An edit made while the rig is disabled survives save/load: the
	 * loaded transform is not the (stale) stage.rest, so it becomes the
	 * rest when the rig applies again. */
	if (rig && B) {
		obs_source_set_enabled(rig, false);
		wait_ms(300);
		struct item_xf before = item_now(B);
		vec2_set(&v, 900, 500);
		obs_sceneitem_set_pos(B, &v);
		wait_ms(100);
		obs_data_t *data = obs_save_source(ss);
		obs_source_release(rig);
		rig = NULL;
		obs_source_remove(ss);
		obs_scene_release(scene);
		ss = obs_load_source(data);
		obs_data_release(data);
		obs_source_load2(ss);
		scene = obs_scene_from_source(ss);
		rig = obs_source_get_filter_by_name(ss, "Parallaxa Camera Rig");
		B = find_item(scene, "avatar");
		CHECK(rig && B, "rig and item reloaded");
		if (rig && B) {
			b = item_now(B);
			CHECK(near_pos(b, 900, 500, 0.5f), "disabled rig: item saved where the user put it (%f %f)",
			      b.px, b.py);
			obs_source_set_enabled(rig, true);
			wait_ms(1200);
			b = item_now(B);
			CHECK(near_pos(b, 900, 500, 0.5f) && !near_pos(b, before.px, before.py, 1.0f),
			      "edit made while disabled kept after reload (%f %f)", b.px, b.py);
			CHECK(item_rest(B, &r) && near_pos(r, 900, 500, 0.5f), "stage.rest follows it (%f %f)", r.px,
			      r.py);
		}
	}

	if (phase_max < 14)
		goto done;
	/* 14. Audio bounce after a collection load where the audio input is
	 * saved after the scene (the rig is created before the input exists). */
	if (rig && B) {
		obs_source_t *tone = obs_source_create("stage_test_tone", "tone2", NULL, NULL);
		obs_data_t *s = obs_data_create();
		obs_data_set_string(s, "audio_source", obs_source_get_uuid(tone));
		obs_data_set_int(s, "audio_amount", 100);
		obs_source_update(rig, s);
		obs_data_release(s);
		wait_ms(300);
		obs_data_array_t *arr = obs_data_array_create();
		obs_data_t *d1 = obs_save_source(ss);
		obs_data_t *d2 = obs_save_source(tone);
		obs_data_array_push_back(arr, d1);
		obs_data_array_push_back(arr, d2);
		obs_data_release(d1);
		obs_data_release(d2);
		obs_source_release(rig);
		rig = NULL;
		obs_source_remove(ss);
		obs_scene_release(scene);
		scene = NULL;
		obs_source_remove(tone);
		obs_source_release(tone);
		tone = NULL;
		obs_source_t *got[2] = {NULL, NULL};
		obs_load_sources(arr, load_keep_cb, got);
		obs_data_array_release(arr);
		ss = got[0];
		tone = got[1];
		scene = ss ? obs_scene_from_source(ss) : NULL;
		rig = ss ? obs_source_get_filter_by_name(ss, "Parallaxa Camera Rig") : NULL;
		B = scene ? find_item(scene, "avatar") : NULL;
		CHECK(ss && tone && rig && B, "scene, tone and rig loaded");
		if (ss && tone && rig && B) {
			wait_ms(400);
			float base = item_now(B).sx;
			tone_push(tone, 0.9f, 600);
			float sc = item_now(B).sx;
			CHECK(sc > base * 1.01f, "audio bounce works after load (%f -> %f)", base, sc);
		}
		if (tone) {
			obs_source_remove(tone);
			obs_source_release(tone);
		}
		if (!ss) { /* keep the cleanup below valid */
			scene = obs_scene_create("Stage Test");
			ss = obs_scene_get_source(scene);
		}
		wait_ms(1200); /* bounce released before the cleanup restores */
	}

	if (phase_max < 15)
		goto done;
	/* 15. Layers panel: a role picked in the panel survives a panel rebuild
	 * that comes before the tick applies it (no snap back to None). */
	if (rig && scene) {
		obs_sceneitem_t *D = obs_scene_add(scene, c_fg);
		wait_ms(1200); /* in the rig's layer cache */
		char key[64];
		snprintf(key, sizeof(key), "role_%lld", (long long)obs_sceneitem_get_id(D));
		obs_properties_t *props = obs_source_properties(rig);
		obs_property_t *p = obs_properties_get(props, key);
		CHECK(p != NULL, "layer row '%s' exists", key);
		obs_data_t *s = obs_source_get_settings(rig);
		if (p) {
			obs_data_set_string(s, key, "fg");
			obs_property_modified(p, s);
			obs_properties_t *again = obs_source_properties(rig); /* a rebuild */
			CHECK(strcmp(obs_data_get_string(s, key), "fg") == 0, "picked role kept by a rebuild (%s)",
			      obs_data_get_string(s, key));
			obs_properties_destroy(again);
			wait_ms(300);
			obs_data_t *priv = obs_sceneitem_get_private_settings(D);
			CHECK(strcmp(obs_data_get_string(priv, STAGE_ITEM_KEY_ROLE), "fg") == 0,
			      "the rig enrolled it as fg");
			obs_data_release(priv);
			again = obs_source_properties(rig);
			CHECK(strcmp(obs_data_get_string(s, key), "fg") == 0, "row shows fg after the tick");
			obs_properties_destroy(again);
		}
		obs_data_release(s);
		obs_properties_destroy(props);
		obs_sceneitem_remove(D);
		wait_ms(300);
	}

done:
	if (rig) {
		obs_source_filter_remove(ss, rig);
		obs_source_release(rig);
	}
	obs_source_remove(ss);
	obs_scene_release(scene);
	obs_source_release(c_bg);
	obs_source_release(c_ch);
	obs_source_release(c_fg);
}

/* One headless OBS session: startup, D3D11 video, audio, the plugin, the
 * tests (or, for the baseline, only their scenes and items), shutdown.
 * Returns the allocation count after shutdown, or -1 when video could not
 * start (no D3D11 device). Comparing both sessions isolates what the rig
 * leaks from libobs' own leftovers. */
static long session(const char *dll, const char *data, const char *gfx, const char *libdata,
		    bool baseline)
{
	if (!obs_startup("en-US", NULL, NULL)) {
		fprintf(stderr, "FAIL obs_startup\n");
		failures++;
		return 0;
	}
	obs_set_ui_task_handler(ui_handler);
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) /* deprecated, but how a headless libobs finds its effects */
#endif
	obs_add_data_path(libdata);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
	struct obs_video_info ovi = {0};
	ovi.graphics_module = gfx;
	ovi.fps_num = 60;
	ovi.fps_den = 1;
	ovi.base_width = ovi.output_width = 1920;
	ovi.base_height = ovi.output_height = 1080;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.gpu_conversion = true;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	int vr = obs_reset_video(&ovi);
	if (vr != OBS_VIDEO_SUCCESS) {
		printf("SKIP: obs_reset_video failed (%d): no D3D11 device\n", vr);
		obs_shutdown();
		return -1;
	}
	struct obs_audio_info oai = {48000, SPEAKERS_STEREO};
	obs_reset_audio(&oai);

	struct obs_source_info tone = {0};
	tone.id = "stage_test_tone";
	tone.type = OBS_SOURCE_TYPE_INPUT;
	tone.output_flags = OBS_SOURCE_AUDIO;
	tone.get_name = tone_name;
	tone.create = tone_create;
	tone.destroy = tone_destroy;
	obs_register_source(&tone);

	scaffold_only = baseline;
	{
		obs_module_t *mod = NULL;
		int rc = obs_open_module(&mod, dll, data);
		if (rc != MODULE_SUCCESS || !obs_init_module(mod)) {
			fprintf(stderr, "FAIL loading %s (%d)\n", dll, rc);
			failures++;
		} else {
			obs_post_load_modules();
			if (!baseline)
				check_moves(15);
			uint64_t t0 = os_gettime_ns();
			run_tests();
			printf("  tests took %.1f s\n", (double)(os_gettime_ns() - t0) / 1e9);
		}
	}
	obs_shutdown();
	return bnum_allocs();
}

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: camera_obs_test <parallaxa.dll> <data> <OBS dir> [work dir]\n");
		return 2;
	}
	char gfx[1024], libdata[1024];
	snprintf(gfx, sizeof(gfx), "%s/bin/64bit/libobs-d3d11.dll", argv[3]);
	snprintf(libdata, sizeof(libdata), "%s/data/libobs/", argv[3]);
	setvbuf(stdout, NULL, _IONBF, 0); /* a crash keeps the output so far */
	const char *ph = getenv("STAGE_CAM_TEST_PHASES");
	if (ph)
		phase_max = atoi(ph);
	base_set_log_handler(log_handler, NULL);
#ifdef _WIN32
	if (!LoadLibraryA("obs-frontend-api.dll")) {
		fprintf(stderr, "FAIL obs-frontend-api.dll not found on PATH\n");
		return 1;
	}
#endif
	char data[1024];
	if (!make_data_dir(argv[2], argc > 4 ? argv[4] : NULL, data, sizeof(data))) {
		fprintf(stderr, "FAIL cannot build the test data folder\n");
		return 1;
	}
	/* libobs keeps a few allocations per session (bnum_allocs is a process
	 * counter): two baseline sessions measure that per-session amount, then
	 * the rig session must add exactly the same. */
	long n1 = session(argv[1], data, gfx, libdata, true);
	if (n1 < 0)
		return 77; /* skipped (SKIP_RETURN_CODE) */
	long n2 = session(argv[1], data, gfx, libdata, true);
	move_warnings = 0;
	long n3 = session(argv[1], data, gfx, libdata, false);
	printf("  allocations after shutdown: baseline %ld, %ld; with rig %ld\n", n1, n2, n3);
	CHECK(n3 - n2 == n2 - n1, "no allocations leaked by the rig (%ld)", (n3 - n2) - (n2 - n1));
	CHECK(stage_errors == 0, "no [parallaxa] errors logged (%d)", stage_errors);
	printf("camera_obs_test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
