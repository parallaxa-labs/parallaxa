/*
 * camera_model_test.c — unit tests of the Camera Rig motion model
 * (stage-rig-model.c): moves, relative:false anchors, freeze, reduce motion,
 * Live-safe caps, coverage clamp, max zoom, audio bounce, manual pause,
 * views (and next / previous view picks), depth of field and frame-rate
 * independence. No libobs.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "stage-rig-model.h"
#include "stage-solver.h"

static int failures, checks;

#define CHECK(cond, ...)                                              \
	do {                                                          \
		checks++;                                             \
		if (!(cond)) {                                        \
			failures++;                                   \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
			printf(__VA_ARGS__);                          \
			printf("\n");                                 \
		}                                                     \
	} while (0)

#define NEAR(a, b, eps) (fabs((double)(a) - (double)(b)) <= (eps))

/* Built-in moves (same data as data/presets/moves). */
static struct stage_move_def mv_zoom_in(void)
{
	struct stage_move_def d;
	memset(&d, 0, sizeof(d));
	strcpy(d.id, "zoom_in");
	d.duration_ms = 800;
	d.relative = true;
	d.hold = true;
	d.nkeys = 2;
	stage_key_init(&d.keys[0]);
	stage_key_init(&d.keys[1]);
	d.keys[1].t = 1.0f;
	d.keys[1].dzoom = 0.25f;
	d.keys[1].ease = STAGE_EASE_OUT_CUBIC;
	d.keys[1].mask = STAGE_CH_ZOOM;
	return d;
}

static struct stage_move_def mv_punch(void)
{
	struct stage_move_def d;
	memset(&d, 0, sizeof(d));
	strcpy(d.id, "punch");
	d.duration_ms = 380;
	d.relative = true;
	d.hold = false;
	d.nkeys = 3;
	for (int i = 0; i < 3; i++) {
		stage_key_init(&d.keys[i]);
		d.keys[i].mask = STAGE_CH_ZOOM;
	}
	d.keys[1].t = 0.3f;
	d.keys[1].dzoom = 0.18f;
	d.keys[1].ease = STAGE_EASE_OUT_BACK;
	d.keys[2].t = 1.0f;
	d.keys[2].ease = STAGE_EASE_IN_OUT_QUAD;
	return d;
}

static struct stage_move_def mv_reset(void)
{
	struct stage_move_def d;
	memset(&d, 0, sizeof(d));
	strcpy(d.id, "reset");
	d.duration_ms = 700;
	d.relative = false;
	d.hold = true;
	d.nkeys = 2;
	stage_key_init(&d.keys[0]);
	stage_key_init(&d.keys[1]);
	d.keys[1].t = 1.0f;
	d.keys[1].ease = STAGE_EASE_IN_OUT_CUBIC;
	d.keys[1].mask = STAGE_CH_ALL;
	return d;
}

static struct stage_move_def mv_shake(void)
{
	struct stage_move_def d;
	memset(&d, 0, sizeof(d));
	strcpy(d.id, "shake");
	d.duration_ms = 600;
	d.relative = true;
	d.nkeys = 3;
	for (int i = 0; i < 3; i++) {
		stage_key_init(&d.keys[i]);
		d.keys[i].mask = STAGE_CH_SHAKE;
	}
	d.keys[1].t = 0.12f;
	d.keys[1].shake = 0.8f;
	d.keys[2].t = 1.0f;
	return d;
}

static void init(struct stage_rig_model *m, bool live_safe)
{
	struct stage_rig_params p;
	stage_rig_params_default(&p);
	p.live_safe = live_safe;
	stage_rig_model_init(m, &p);
}

static void run(struct stage_rig_model *m, float seconds, float hz, const struct stage_rig_inputs *tmpl)
{
	struct stage_rig_inputs in;
	if (tmpl) {
		in = *tmpl;
	} else {
		memset(&in, 0, sizeof(in));
		in.audio_db = NAN;
		in.char_depth = NAN;
		in.far_bg_depth = NAN;
		in.dof_slider = 0.3f;
	}
	in.dt = 1.0f / hz;
	int n = (int)lroundf(seconds * hz);
	for (int i = 0; i < n; i++)
		stage_rig_model_step(m, &in);
}

static void test_zoom_in_hold(void)
{
	struct stage_rig_model m;
	init(&m, true);
	struct stage_move_def d = mv_zoom_in();
	CHECK(stage_move_valid(&d), "zoom_in valid");
	CHECK(stage_rig_model_move(&m, &d, 7, 0, 1.0f, STAGE_CAM_EASE_NONE), "move accepted");
	run(&m, 0.1f, 60, NULL);
	CHECK(stage_rig_model_moving(&m), "moving during the move");
	CHECK(m.move_cmd_id == 7, "move id kept");
	run(&m, 3.0f, 60, NULL);
	CHECK(!stage_rig_model_moving(&m), "move finished");
	CHECK(NEAR(m.target.zoom, 1.25, 1e-5), "hold keeps target zoom 1.25 (got %f)", m.target.zoom);
	CHECK(NEAR(m.out.zoom, 1.25, 1e-3), "camera reaches zoom 1.25 (got %f)", m.out.zoom);
	CHECK(NEAR(m.out.x, 0, 1e-3) && NEAR(m.out.y, 0, 1e-3), "zoom_in does not pan");
}

/* A move shorter than one step (50 ms at 15 fps) starts and ends inside it;
 * its id and how it ended stay readable for the state snapshot. */
static void test_sub_tick_move(void)
{
	struct stage_rig_model m;
	init(&m, false);
	struct stage_move_def d = mv_punch();
	stage_rig_model_move(&m, &d, 11, 50, 1.0f, STAGE_CAM_EASE_NONE);
	CHECK(!m.move_done, "a started move is not done");
	run(&m, 1.0f / 15.0f, 15, NULL);
	CHECK(!m.move_active && m.move_done, "sub-tick move done after one step");
	CHECK(m.move_cmd_id == 11 && strcmp(m.move.id, "punch") == 0, "last move id and name kept");

	stage_rig_model_move(&m, &d, 12, 0, 1.0f, STAGE_CAM_EASE_NONE);
	stage_rig_model_reset(&m, 0, STAGE_EASE_DEFAULT);
	run(&m, 1.0f / 15.0f, 15, NULL);
	CHECK(m.move_cmd_id == 12 && !m.move_done, "a move stopped by reset is not done");
}

static void test_punch_springs_back(void)
{
	struct stage_rig_model m;
	init(&m, false);
	struct stage_move_def d = mv_punch();
	stage_rig_model_move(&m, &d, 1, 0, 1.0f, STAGE_CAM_EASE_NONE);
	float peak = 1.0f;
	struct stage_rig_inputs in = {0};
	in.audio_db = NAN;
	in.char_depth = in.far_bg_depth = NAN;
	in.dt = 1.0f / 60.0f;
	for (int i = 0; i < 120; i++) {
		stage_rig_model_step(&m, &in);
		peak = fmaxf(peak, m.out.zoom);
	}
	CHECK(peak > 1.08f, "punch peaks (got %f)", peak);
	CHECK(NEAR(m.out.zoom, 1.0, 2e-3), "punch (hold:false) springs back (got %f)", m.out.zoom);
	CHECK(NEAR(m.target.zoom, 1.0, 1e-6), "target unchanged by a hold:false move");
}

static void test_reset_relative_false(void)
{
	struct stage_rig_model m;
	init(&m, false);
	struct stage_cam_target t = {200.0f, -50.0f, 1.5f, 3.0f, STAGE_CAM_X | STAGE_CAM_Y | STAGE_CAM_ZOOM | STAGE_CAM_ROT, false};
	stage_rig_model_set(&m, &t, 0, STAGE_EASE_DEFAULT);
	run(&m, 2.0f, 60, NULL);
	CHECK(NEAR(m.out.x, 200, 0.05) && NEAR(m.out.zoom, 1.5, 1e-4), "camera_set reached (x %f zoom %f)",
	      m.out.x, m.out.zoom);
	struct stage_move_def d = mv_reset();
	CHECK(stage_move_valid(&d), "reset valid");
	stage_rig_model_move(&m, &d, 2, 0, 1.0f, STAGE_CAM_EASE_NONE);
	struct stage_cam before = m.out;
	run(&m, 1.0f / 60.0f, 60, NULL);
	CHECK(fabsf(m.out.x - before.x) < 5.0f && fabsf(m.out.zoom - before.zoom) < 0.01f,
	      "relative:false eases from the start pose (dx %f dzoom %f)", m.out.x - before.x,
	      m.out.zoom - before.zoom);
	run(&m, 2.0f, 60, NULL);
	CHECK(NEAR(m.out.x, 0, 0.05) && NEAR(m.out.y, 0, 0.05) && NEAR(m.out.zoom, 1, 1e-4) &&
		      NEAR(m.out.rot, 0, 1e-3),
	      "reset reaches home (%f %f %f %f)", m.out.x, m.out.y, m.out.zoom, m.out.rot);
}

static void test_freeze(void)
{
	struct stage_rig_model m;
	init(&m, false);
	stage_rig_model_auto(&m, STAGE_AUTO_DRIFT, 1.0f);
	run(&m, 6.0f, 60, NULL);
	CHECK(m.auto_weight > 0.99f, "auto fully blended in (%f)", m.auto_weight);
	stage_rig_model_freeze(&m, STAGE_FREEZE_TOGGLE);
	CHECK(m.frozen, "frozen");
	CHECK(m.changed & STAGE_RIG_CH_FREEZE, "freeze change flagged");
	struct stage_move_def d = mv_zoom_in();
	CHECK(!stage_rig_model_move(&m, &d, 3, 0, 1.0f, STAGE_CAM_EASE_NONE), "camera_move refused while frozen");
	run(&m, 0.5f, 60, NULL);
	struct stage_cam a = m.out;
	run(&m, 2.0f, 60, NULL);
	CHECK(NEAR(a.x, m.out.x, 1e-3) && NEAR(a.y, m.out.y, 1e-3) && NEAR(a.zoom, m.out.zoom, 1e-5),
	      "frozen camera stays still");
	struct stage_cam_target t = {0, 0, 1.3f, 0, STAGE_CAM_ZOOM, false};
	stage_rig_model_set(&m, &t, 0, STAGE_EASE_DEFAULT);
	run(&m, 2.0f, 60, NULL);
	CHECK(NEAR(m.out.zoom, 1.3, 1e-3), "explicit camera_set applies while frozen (%f)", m.out.zoom);
	stage_rig_model_panic(&m);
	CHECK(!m.frozen, "panic clears freeze");
}

static void test_motion_scale_zero(void)
{
	struct stage_rig_model m;
	init(&m, false);
	m.p.motion_scale = 0.0f;
	stage_rig_model_auto(&m, STAGE_AUTO_DRIFT, 1.0f);
	struct stage_move_def d = mv_shake();
	stage_rig_model_move(&m, &d, 4, 0, 2.0f, STAGE_CAM_EASE_NONE);
	float maxdev = 0.0f;
	struct stage_rig_inputs in = {0};
	in.audio_db = NAN;
	in.char_depth = in.far_bg_depth = NAN;
	in.dt = 1.0f / 60.0f;
	for (int i = 0; i < 600; i++) {
		stage_rig_model_step(&m, &in);
		maxdev = fmaxf(maxdev, fabsf(m.out.x) + fabsf(m.out.y) + fabsf(m.out.zoom - 1.0f) +
					       fabsf(m.out.rot));
	}
	CHECK(maxdev < 1e-5f, "shake and drift fully off at 0 %% (dev %g)", maxdev);
	CHECK(m.out_shake == 0.0f, "reported shake 0");

	/* Below 100 %: shake still present but slow (stage-plugin-ux.md: "Below
	 * 100 % shake stays under 3 Hz"). 8 s of x at 240 Hz: the power above
	 * 3 Hz (DFT) and the zero-crossing rate (a 3 Hz sine crosses 6 times a
	 * second). */
	init(&m, false);
	m.p.motion_scale = 0.5f;
	m.p.smoothing_ms = 0.0f;
	d.keys[0].shake = d.keys[2].shake = 0.8f; /* steady shake */
	stage_rig_model_move(&m, &d, 5, 20000, 1.0f, STAGE_CAM_EASE_NONE);
	enum { FS = 240, NS = 8 * FS };
	static float xs[NS];
	in.dt = 1.0f / (float)FS;
	for (int i = 0; i < FS; i++) /* settle */
		stage_rig_model_step(&m, &in);
	double mean = 0.0;
	for (int i = 0; i < NS; i++) {
		stage_rig_model_step(&m, &in);
		xs[i] = m.out.x;
		mean += xs[i];
	}
	mean /= NS;
	int crossings = 0;
	for (int i = 1; i < NS; i++)
		if ((xs[i - 1] < mean) != (xs[i] < mean))
			crossings++;
	double p_lo = 0.0, p_hi = 0.0;
	for (int k = 1; k <= NS / 2; k++) { /* bin k = k / 8 Hz */
		double re = 0.0, im = 0.0;
		for (int i = 0; i < NS; i++) {
			double a = 2.0 * 3.14159265358979 * (double)k * (double)i / NS;
			re += (xs[i] - mean) * cos(a);
			im -= (xs[i] - mean) * sin(a);
		}
		double pw = re * re + im * im;
		if ((double)k / 8.0 > 3.0)
			p_hi += pw;
		else
			p_lo += pw;
	}
	double frac = p_hi / fmax(p_lo + p_hi, 1e-30);
	CHECK(p_lo > 0.0, "reduced shake is present");
	CHECK(frac < 0.02, "reduced shake: %.2f %% of its power above 3 Hz (< 2 %%)", frac * 100.0);
	CHECK(crossings < 8 * 6, "reduced shake: %d zero crossings in 8 s (a 3 Hz sine: 48)", crossings);
	printf("  reduced shake: %.2f %% of power above 3 Hz, %d crossings in 8 s\n", frac * 100.0,
	       crossings);
}

static void test_live_safe_caps(void)
{
	struct stage_rig_model m;
	init(&m, true);
	struct stage_cam_target t = {0, 0, 3.0f, 30.0f, STAGE_CAM_ZOOM | STAGE_CAM_ROT, false};
	stage_rig_model_set(&m, &t, 0, STAGE_EASE_DEFAULT);
	struct stage_rig_inputs in = {0};
	in.audio_db = NAN;
	in.char_depth = in.far_bg_depth = NAN;
	in.dt = 1.0f / 60.0f;
	float max_dz = 0, max_dr = 0, max_roll = 0;
	struct stage_cam prev = m.out;
	for (int i = 0; i < 600; i++) {
		stage_rig_model_step(&m, &in);
		max_dz = fmaxf(max_dz, fabsf(m.out.zoom - prev.zoom));
		max_dr = fmaxf(max_dr, fabsf(m.out.rot - prev.rot));
		max_roll = fmaxf(max_roll, fabsf(m.out.rot));
		prev = m.out;
	}
	CHECK(max_dz <= 0.5f / 60.0f + 1e-5f, "zoom speed <= 0.5/s (step %f)", max_dz);
	CHECK(max_dr <= 20.0f / 60.0f + 1e-4f, "rotation speed <= 20 deg/s (step %f)", max_dr);
	CHECK(max_roll <= 8.0f + 1e-4f, "roll <= 8 deg (%f)", max_roll);
	CHECK(NEAR(m.out.rot, 8.0, 1e-3), "roll held at the cap (%f)", m.out.rot);

	/* Off: the explicit target is reached. */
	init(&m, false);
	stage_rig_model_set(&m, &t, 0, STAGE_EASE_DEFAULT);
	run(&m, 2.0f, 60, NULL);
	CHECK(NEAR(m.out.rot, 30.0, 1e-2) && NEAR(m.out.zoom, 3.0, 1e-3), "no caps when Live-safe is off");
}

static void test_coverage_clamp(void)
{
	struct stage_rig_model m;
	init(&m, true);
	m.p.max_pan = 0.5f;
	/* bg covering the canvas, oversized 8 % about the centre, depth 3. */
	struct stage_rig_cover cov;
	float k = 1.08f, W = 1920, H = 1080;
	cov.rect.l = W / 2 - W / 2 * k;
	cov.rect.r = W / 2 + W / 2 * k;
	cov.rect.t = H / 2 - H / 2 * k;
	cov.rect.b = H / 2 + H / 2 * k;
	cov.f = 1.0f / 3.0f;
	struct stage_cam_target t = {900.0f, 0, 1.0f, 0, STAGE_CAM_X, false};
	stage_rig_model_set(&m, &t, 0, STAGE_EASE_DEFAULT);
	struct stage_rig_inputs in = {0};
	in.audio_db = NAN;
	in.char_depth = in.far_bg_depth = NAN;
	in.covers = &cov;
	in.ncovers = 1;
	in.dt = 1.0f / 60.0f;
	float worst = 1e9f;
	for (int i = 0; i < 300; i++) {
		stage_rig_model_step(&m, &in);
		worst = fminf(worst, stage_cover_margin(&cov.rect, cov.f, &m.out, W, H));
	}
	CHECK(worst >= -0.05f, "canvas stays covered (worst margin %f)", worst);
	CHECK(m.out.x < 900.0f && m.out.x > 200.0f, "pan clamped near the coverage limit (x %f)", m.out.x);

	/* A small sticker tagged bg must not force a zoom. */
	struct stage_rig_cover small = {{100, 100, 300, 300}, 1.0f / 3.0f};
	init(&m, true);
	in.covers = &small;
	run(&m, 1.0f, 60, &in);
	CHECK(NEAR(m.out.zoom, 1.0, 1e-5), "non-covering layer ignored (zoom %f)", m.out.zoom);
}

static void test_max_zoom(void)
{
	struct stage_rig_model m;
	init(&m, false);
	m.p.max_zoom = 1.2f;
	struct stage_move_def d = mv_zoom_in();
	stage_rig_model_move(&m, &d, 1, 0, 1.0f, STAGE_CAM_EASE_NONE);
	run(&m, 3.0f, 60, NULL);
	CHECK(m.out.zoom <= 1.2f + 1e-4f, "move capped by Max zoom (%f)", m.out.zoom);
	struct stage_cam_target t = {0, 0, 2.0f, 0, STAGE_CAM_ZOOM, false};
	stage_rig_model_set(&m, &t, 0, STAGE_EASE_DEFAULT);
	run(&m, 2.0f, 60, NULL);
	CHECK(NEAR(m.out.zoom, 2.0, 1e-3), "explicit camera_set may exceed Max zoom (%f)", m.out.zoom);
}

static void test_frame_rate_independence(void)
{
	struct stage_rig_model a, b;
	init(&a, false);
	init(&b, false);
	struct stage_move_def d = mv_zoom_in();
	stage_rig_model_move(&a, &d, 1, 0, 1.0f, STAGE_CAM_EASE_NONE);
	stage_rig_model_move(&b, &d, 1, 0, 1.0f, STAGE_CAM_EASE_NONE);
	run(&a, 0.6f, 30, NULL);
	run(&b, 0.6f, 144, NULL);
	CHECK(fabsf(a.out.zoom - b.out.zoom) < 0.004f, "30 Hz vs 144 Hz mid-move (%f vs %f)", a.out.zoom,
	      b.out.zoom);
}

static void test_audio_bounce(void)
{
	struct stage_rig_model m;
	init(&m, false);
	m.p.audio_amount = 1.0f;
	struct stage_rig_inputs in = {0};
	in.char_depth = in.far_bg_depth = NAN;
	in.dt = 1.0f / 60.0f;
	in.audio_db = -6.0f;
	for (int i = 0; i < 18; i++) /* 0.3 s loud */
		stage_rig_model_step(&m, &in);
	CHECK(m.bounce > 0.12f, "bounce follows a loud mic (%f)", m.bounce);
	CHECK(m.out.zoom > 1.12f, "camera zooms with the bounce (%f)", m.out.zoom);
	in.audio_db = -100.0f;
	for (int i = 0; i < 90; i++)
		stage_rig_model_step(&m, &in);
	CHECK(m.bounce < 0.01f, "bounce releases in silence (%f)", m.bounce);
	in.audio_db = NAN;
	stage_rig_model_step(&m, &in);
	CHECK(m.bounce == 0.0f, "no audio source: no bounce");
	stage_rig_model_freeze(&m, STAGE_FREEZE_ON);
	in.audio_db = 0.0f;
	run(&m, 0.5f, 60, &in);
	CHECK(m.bounce == 0.0f, "no bounce while frozen");
}

static void test_manual_pause(void)
{
	struct stage_rig_model m;
	init(&m, false);
	m.p.manual_pause_s = 1.0f;
	stage_rig_model_auto(&m, STAGE_AUTO_DRIFT, 1.0f);
	run(&m, 3.0f, 60, NULL);
	CHECK(m.auto_weight > 0.99f, "auto on");
	struct stage_cam_target t = {10, 0, 1, 0, STAGE_CAM_X, false};
	stage_rig_model_set(&m, &t, 0, STAGE_EASE_DEFAULT);
	run(&m, 0.6f, 60, NULL);
	CHECK(m.auto_weight < 0.01f, "manual input pauses auto (%f)", m.auto_weight);
	run(&m, 3.0f, 60, NULL);
	CHECK(m.auto_weight > 0.99f, "auto blends back in (%f)", m.auto_weight);
}

static void test_auto_toggle_next(void)
{
	struct stage_rig_model m;
	init(&m, false);
	CHECK(m.auto_mode == STAGE_AUTO_OFF, "starts off");
	stage_rig_model_auto(&m, STAGE_CAM_AUTO_TOGGLE, -1.0f);
	CHECK(m.auto_mode == STAGE_AUTO_DRIFT, "toggle -> drift");
	stage_rig_model_auto(&m, STAGE_AUTO_SINGING, 0.5f);
	stage_rig_model_auto(&m, STAGE_CAM_AUTO_TOGGLE, -1.0f);
	CHECK(m.auto_mode == STAGE_AUTO_OFF, "toggle -> off");
	stage_rig_model_auto(&m, STAGE_CAM_AUTO_TOGGLE, -1.0f);
	CHECK(m.auto_mode == STAGE_AUTO_SINGING, "toggle -> last mode (singing)");
	CHECK(NEAR(m.auto_intensity, 0.5, 1e-6), "intensity kept when < 0");
	stage_rig_model_auto(&m, STAGE_CAM_AUTO_NEXT, -1.0f);
	CHECK(m.auto_mode == STAGE_AUTO_OFF, "next wraps singing -> off");
	stage_rig_model_auto(&m, STAGE_CAM_AUTO_NEXT, -1.0f);
	CHECK(m.auto_mode == STAGE_AUTO_DRIFT, "next off -> drift");
}

static void test_views(void)
{
	struct stage_rig_model m;
	init(&m, false);
	struct stage_cam pose = {100, -20, 1.5f, 2};
	stage_rig_model_view_apply(&m, 2, &pose, STAGE_TRANSITION_CUT, 600);
	run(&m, 1.0f / 60.0f, 60, NULL);
	CHECK(NEAR(m.out.x, 100, 0.01) && NEAR(m.out.zoom, 1.5, 1e-4), "cut is instant (%f %f)", m.out.x,
	      m.out.zoom);
	CHECK(m.view == 2, "view slot reported");
	struct stage_cam tgt;
	stage_rig_model_target(&m, &tgt);
	CHECK(NEAR(tgt.zoom, 1.5, 1e-6), "target = view pose");
	struct stage_cam pose2 = {0, 0, 1.0f, 0};
	stage_rig_model_view_apply(&m, 3, &pose2, STAGE_TRANSITION_SMOOTH, 600);
	stage_rig_model_target(&m, &tgt);
	CHECK(NEAR(tgt.zoom, 1.0, 1e-6), "view_save stores the destination, not a mid pose");
	run(&m, 0.3f, 60, NULL);
	CHECK(m.out.zoom < 1.5f && m.out.zoom > 1.0f, "smooth transition in progress (%f)", m.out.zoom);
	struct stage_cam_target t = {0, 0, 1.1f, 0, STAGE_CAM_ZOOM, false};
	stage_rig_model_set(&m, &t, 0, STAGE_EASE_DEFAULT);
	CHECK(m.view == 0, "camera_set clears the view");
}

/* Next / previous view hotkeys: the tick picks the slot from the saved
 * slots and the view last applied. */
static void test_view_step(void)
{
	struct stage_rig_model m;
	init(&m, false);
	const unsigned saved = (1u << 1) | (1u << 4) | (1u << 8); /* slots 2, 5, 9 */
	CHECK(stage_rig_model_view_step(&m, 1, 0) == 0, "no saved views: nothing");
	CHECK(stage_rig_model_view_step(&m, 1, saved) == 2, "no current view: next = first");
	CHECK(stage_rig_model_view_step(&m, -1, saved) == 9, "no current view: previous = last");
	struct stage_cam pose = {0, 0, 1.2f, 0};
	stage_rig_model_view_apply(&m, 5, &pose, STAGE_TRANSITION_CUT, 0);
	CHECK(stage_rig_model_view_step(&m, 1, saved) == 9, "5 -> next 9");
	CHECK(stage_rig_model_view_step(&m, -1, saved) == 2, "5 -> previous 2");
	stage_rig_model_view_apply(&m, 9, &pose, STAGE_TRANSITION_CUT, 0);
	CHECK(stage_rig_model_view_step(&m, 1, saved) == 2, "9 -> next wraps to 2");
	stage_rig_model_view_apply(&m, 2, &pose, STAGE_TRANSITION_CUT, 0);
	CHECK(stage_rig_model_view_step(&m, -1, saved) == 9, "2 -> previous wraps to 9");
	CHECK(stage_rig_model_view_step(&m, 1, 1u << 1) == 2, "only the current one: it again");
	stage_rig_model_view_apply(&m, 4, &pose, STAGE_TRANSITION_CUT, 0); /* deleted since */
	CHECK(stage_rig_model_view_step(&m, 1, saved) == 5 && stage_rig_model_view_step(&m, -1, saved) == 2,
	      "from an unsaved slot: its neighbours");
}

static void test_dof(void)
{
	struct stage_rig_model m;
	init(&m, false);
	struct stage_rig_inputs in = {0};
	in.audio_db = NAN;
	in.char_depth = 1.0f;
	in.far_bg_depth = 3.0f;
	in.dof_slider = 0.4f;
	CHECK(m.amount == 0.0f && m.follow_char, "DoF starts off, follow on");
	stage_rig_model_dof(&m, STAGE_CAM_DOF_RACK_BG, NAN, NAN, 0, STAGE_EASE_DEFAULT, 1.0f, 3.0f, 0.4f);
	CHECK(!m.follow_char, "rack to background turns follow off");
	run(&m, 1.3f, 60, &in);
	CHECK(NEAR(m.focus, 3.0, 1e-4) && NEAR(m.amount, 0.4, 1e-4), "rack bg: focus 3, amount from slider (%f %f)",
	      m.focus, m.amount);
	stage_rig_model_dof(&m, STAGE_CAM_DOF_RACK_CHAR, NAN, NAN, 0, STAGE_EASE_DEFAULT, 1.0f, 3.0f, 0.4f);
	run(&m, 1.3f, 60, &in);
	CHECK(m.follow_char && NEAR(m.focus, 1.0, 1e-4), "rack char: follow on, focus 1 (%f)", m.focus);
	in.char_depth = 1.5f;
	run(&m, 0.1f, 60, &in);
	CHECK(NEAR(m.focus, 1.5, 1e-4), "focus follows the character depth (%f)", m.focus);
	stage_rig_model_dof(&m, STAGE_DOF_FOCUS, 2.0f, NAN, 0, STAGE_EASE_DEFAULT, 1.5f, 3.0f, 0.4f);
	CHECK(!m.follow_char && NEAR(m.focus, 2.0, 1e-6), "dof_set focus away from char: follow off");
	stage_rig_model_dof(&m, STAGE_DOF_FOCUS, 1.5f, NAN, 0, STAGE_EASE_DEFAULT, 1.5f, 3.0f, 0.4f);
	CHECK(m.follow_char, "dof_set focus == char depth: follow on");
	m.changed = 0;
	stage_rig_model_panic(&m);
	CHECK(m.amount == 0.0f && m.follow_char && (m.changed & STAGE_RIG_CH_DOF), "panic: DoF off, follow on");
	CHECK(m.auto_mode == STAGE_AUTO_OFF, "panic: auto off");
}

static void test_snap_home(void)
{
	struct stage_rig_model m;
	init(&m, false);
	struct stage_cam_target t = {300, 100, 2.0f, 5, STAGE_CAM_X | STAGE_CAM_Y | STAGE_CAM_ZOOM | STAGE_CAM_ROT, false};
	stage_rig_model_set(&m, &t, 0, STAGE_EASE_DEFAULT);
	run(&m, 2.0f, 60, NULL);
	stage_rig_model_snap_home(&m);
	run(&m, 1.0f / 60.0f, 60, NULL);
	CHECK(NEAR(m.out.x, 0, 1e-4) && NEAR(m.out.zoom, 1, 1e-6) && NEAR(m.out.rot, 0, 1e-5),
	      "snap home is immediate");
}

static void test_relative_set_clamped(void)
{
	struct stage_rig_model m;
	init(&m, false);
	struct stage_cam_target t = {0, 0, 10.0f, 0, STAGE_CAM_ZOOM, true};
	stage_rig_model_set(&m, &t, 0, STAGE_EASE_DEFAULT);
	struct stage_cam tgt;
	stage_rig_model_target(&m, &tgt);
	CHECK(NEAR(tgt.zoom, STAGE_ZOOM_MAX, 1e-6), "relative set clamped to the camera range (%f)", tgt.zoom);
	struct stage_move_def d = mv_zoom_in();
	d.hold = true;
	stage_rig_model_move(&m, &d, 9, 0, 1.0f, STAGE_CAM_EASE_NONE);
	run(&m, 0.4f, 60, NULL);
	stage_rig_model_reset(&m, 0, STAGE_EASE_DEFAULT);
	CHECK(!m.move_active, "reset cancels the move");
}

int main(void)
{
	test_zoom_in_hold();
	test_sub_tick_move();
	test_punch_springs_back();
	test_reset_relative_false();
	test_freeze();
	test_motion_scale_zero();
	test_live_safe_caps();
	test_coverage_clamp();
	test_max_zoom();
	test_frame_rate_independence();
	test_audio_bounce();
	test_manual_pause();
	test_auto_toggle_next();
	test_views();
	test_view_step();
	test_dof();
	test_snap_home();
	test_relative_set_clamped();
	printf("camera_model_test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
