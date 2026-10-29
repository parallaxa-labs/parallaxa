/*
 * math_test.c — unit tests for stage_math (single executable, no deps).
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Exit code 0 when every check passes. Run: math_test [-v]
 */
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stage-math.h"

/* ---------------------------------------------------------------------- */
/* Minimal assert framework                                               */
/* ---------------------------------------------------------------------- */

static int g_checks, g_failures, g_verbose;

/* Evaluates to the condition so loops can stop at the first failure. */
#define CHECK(cond, ...) check_impl((cond) ? 1 : 0, __FILE__, __LINE__, #cond, __VA_ARGS__)
#define NEAR(a, b, eps) (fabs((double)(a) - (double)(b)) <= (double)(eps))

static int check_impl(int ok, const char *file, int line, const char *expr, const char *fmt, ...)
{
	g_checks++;
	if (ok)
		return 1;
	g_failures++;
	va_list ap;
	va_start(ap, fmt);
	printf("FAIL %s:%d: %s\n      ", file, line, expr);
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
	return 0;
}

#define RUN(fn)                                                       \
	do {                                                          \
		int before = g_failures;                              \
		fn();                                                 \
		printf("%-28s %s\n", #fn, g_failures == before ? "ok" : "FAILED"); \
	} while (0)

/* Deterministic PRNG (splitmix32-ish) for random-input tests. */
static uint32_t g_rng = 0x12345678u;
static uint32_t rng_u32(void)
{
	uint32_t z = (g_rng += 0x9E3779B9u);
	z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
	z = (z ^ (z >> 13)) * 0xC2B2AE35u;
	return z ^ (z >> 16);
}
static float rng_range(float lo, float hi)
{
	return lo + (hi - lo) * (float)((double)(rng_u32() >> 8) / 16777216.0);
}

/* ---------------------------------------------------------------------- */
/* Easing                                                                 */
/* ---------------------------------------------------------------------- */

static void test_ease(void)
{
	CHECK(STAGE_EASE_COUNT == sizeof(stage_ease_names) / sizeof(stage_ease_names[0]),
	      "ease name table size");
	CHECK(stage_ease_from_name("smoothstep") == STAGE_EASE_SMOOTHSTEP, "name lookup");

	for (int e = 0; e < STAGE_EASE_COUNT; e++) {
		const char *n = stage_ease_names[e];
		CHECK(stage_ease_apply(e, 0.0f) == 0.0f, "%s f(0) = %g", n, stage_ease_apply(e, 0.0f));
		CHECK(stage_ease_apply(e, 1.0f) == 1.0f, "%s f(1) = %g", n, stage_ease_apply(e, 1.0f));
		CHECK(stage_ease_apply(e, -0.5f) == 0.0f, "%s clamps below 0", n);
		CHECK(stage_ease_apply(e, 7.0f) == 1.0f, "%s clamps above 1", n);
		CHECK(stage_ease_apply(e, NAN) == 0.0f, "%s NaN -> 0", n);
		/* Continuous at the endpoints. */
		CHECK(NEAR(stage_ease_apply(e, 1e-4f), 0.0, 0.01), "%s near 0", n);
		CHECK(NEAR(stage_ease_apply(e, 1.0f - 1e-4f), 1.0, 0.01), "%s near 1", n);

		float prev = 0.0f, lo = 0.0f, hi = 1.0f;
		int mono_ok = 1;
		for (int i = 1; i <= 4000; i++) {
			float t = (float)i / 4000.0f;
			float v = stage_ease_apply(e, t);
			if (v < prev - 1e-6f)
				mono_ok = 0;
			if (!isfinite(v))
				mono_ok = 0;
			lo = fminf(lo, v);
			hi = fmaxf(hi, v);
			prev = v;
		}
		if (stage_ease_is_monotonic(e)) {
			CHECK(mono_ok, "%s must be monotonic", n);
			CHECK(lo >= 0.0f && hi <= 1.0f, "%s stays in [0,1]: %g..%g", n, lo, hi);
		} else {
			CHECK(lo < 0.0f || hi > 1.0f, "%s should overshoot", n);
			CHECK(lo > -0.5f && hi < 1.5f, "%s overshoot bounded: %g..%g", n, lo, hi);
		}
	}

	/* In-out curves are point-symmetric: f(t) + f(1 - t) = 1. */
	const int sym[] = {STAGE_EASE_LINEAR,         STAGE_EASE_IN_OUT_QUAD, STAGE_EASE_IN_OUT_CUBIC,
			   STAGE_EASE_IN_OUT_QUINT,   STAGE_EASE_IN_OUT_BACK, STAGE_EASE_IN_OUT_ELASTIC,
			   STAGE_EASE_SMOOTHSTEP};
	for (size_t i = 0; i < sizeof(sym) / sizeof(sym[0]); i++) {
		for (int k = 1; k < 100; k++) {
			float t = (float)k / 100.0f;
			float s = stage_ease_apply(sym[i], t) + stage_ease_apply(sym[i], 1.0f - t);
			if (!CHECK(NEAR(s, 1.0, 2e-5), "%s symmetric at %g: %g",
				   stage_ease_names[sym[i]], t, s))
				break;
		}
	}

	/* Known values. */
	CHECK(NEAR(stage_ease_apply(STAGE_EASE_IN_QUAD, 0.5f), 0.25, 1e-6), "in_quad");
	CHECK(NEAR(stage_ease_apply(STAGE_EASE_OUT_CUBIC, 0.5f), 0.875, 1e-6), "out_cubic");
	CHECK(NEAR(stage_ease_apply(STAGE_EASE_IN_OUT_CUBIC, 0.25f), 0.0625, 1e-6), "in_out_cubic");
	CHECK(NEAR(stage_ease_apply(STAGE_EASE_IN_QUINT, 0.5f), 0.03125, 1e-6), "in_quint");
	CHECK(NEAR(stage_ease_apply(STAGE_EASE_SMOOTHSTEP, 0.25f), 0.15625, 1e-6), "smoothstep");
	CHECK(stage_ease_apply(STAGE_EASE_IN_BACK, 0.3f) < 0.0f, "in_back dips below 0");
	CHECK(stage_ease_apply(STAGE_EASE_OUT_BACK, 0.7f) > 1.0f, "out_back overshoots");
	/* Unknown ids evaluate as linear. */
	CHECK(stage_ease_apply(200, 0.3f) == 0.3f, "unknown -> linear");
	CHECK(stage_ease_apply(-1, 0.3f) == 0.3f, "negative -> linear");
	/* Transition table uses known eases. */
	for (int i = 0; i < STAGE_TRANSITION_COUNT; i++)
		CHECK(stage_transition_ease[i] < STAGE_EASE_COUNT, "transition ease %d", i);
}

/* ---------------------------------------------------------------------- */
/* Springs                                                                */
/* ---------------------------------------------------------------------- */

/* Simulates 1 s toward target 100 (switching to -40 at 0.5 s) at `hz`. */
static struct stage_spring spring_run(float omega, int hz)
{
	struct stage_spring s = {0.0f, 0.0f};
	float dt = 1.0f / (float)hz;
	for (int i = 0; i < hz; i++) {
		float target = i < hz / 2 ? 100.0f : -40.0f;
		stage_spring_step(&s, target, omega, dt);
	}
	return s;
}

static void test_spring(void)
{
	/* Smoothing definition: 90 % of a step after smoothing_s. */
	float w120 = stage_spring_omega(0.12f);
	CHECK(NEAR(w120, 32.414, 0.01), "omega(0.12) = %g", w120);
	struct stage_spring s = {0.0f, 0.0f};
	stage_spring_step(&s, 1.0f, w120, 0.12f);
	CHECK(NEAR(s.x, 0.9, 1e-4), "90%% after smoothing time: %g", s.x);
	CHECK(stage_spring_omega(0.0f) == 0.0f && stage_spring_omega(NAN) == 0.0f, "omega(0)");

	/* Frame-rate independence: 60 Hz vs 144 Hz after 1 s within 1 %
	 * (acceptance), and in fact within 1e-3 of the step size. */
	const float omegas[] = {2.0f, 5.0f, w120};
	for (size_t i = 0; i < 3; i++) {
		struct stage_spring a = spring_run(omegas[i], 60);
		struct stage_spring b = spring_run(omegas[i], 144);
		struct stage_spring c = spring_run(omegas[i], 30);
		double rel = fabs((double)a.x - b.x) / 140.0; /* relative to the step */
		CHECK(rel < 0.01, "omega %g: 60 Hz %g vs 144 Hz %g (%.4f%%)", omegas[i], a.x, b.x,
		      rel * 100.0);
		CHECK(rel < 1e-3, "omega %g: exact closed form, rel %g", omegas[i], rel);
		CHECK(NEAR(a.v, b.v, 0.01 * 140.0 * omegas[i]), "velocity match %g vs %g", a.v, b.v);
		CHECK(fabs((double)a.x - c.x) / 140.0 < 1e-3, "30 Hz also matches");
	}

	/* Jittery frame times (OBS ticks are not perfectly regular). */
	{
		struct stage_spring j = {0.0f, 0.0f}, ref = {0.0f, 0.0f};
		float total = 0.0f;
		while (total < 0.999f) {
			float dt = rng_range(0.004f, 0.034f);
			if (total + dt > 1.0f)
				dt = 1.0f - total;
			stage_spring_step(&j, 100.0f, 5.0f, dt);
			total += dt;
		}
		stage_spring_step(&ref, 100.0f, 5.0f, 1.0f);
		CHECK(NEAR(j.x, ref.x, 0.05), "jittered dt %g vs single step %g", j.x, ref.x);
	}

	/* Critically damped from rest: no overshoot, monotone approach. */
	{
		struct stage_spring m = {0.0f, 0.0f};
		float prev = 0.0f;
		int ok = 1;
		for (int i = 0; i < 600; i++) {
			stage_spring_step(&m, 10.0f, 8.0f, 1.0f / 60.0f);
			if (m.x > 10.0f + 1e-5f || m.x < prev - 1e-6f)
				ok = 0;
			prev = m.x;
		}
		CHECK(ok, "no overshoot from rest");
		CHECK(NEAR(m.x, 10.0, 1e-4), "converges: %g", m.x);
	}

	/* Snap, no-op and huge dt. */
	s.x = 3.0f;
	s.v = 7.0f;
	stage_spring_step(&s, 5.0f, 0.0f, 0.016f);
	CHECK(s.x == 5.0f && s.v == 0.0f, "omega 0 snaps");
	s.x = 3.0f;
	s.v = 1.0f;
	stage_spring_step(&s, 5.0f, 10.0f, 0.0f);
	CHECK(s.x == 3.0f && s.v == 1.0f, "dt 0 is a no-op");
	stage_spring_step(&s, 5.0f, 10.0f, 1e6f);
	CHECK(s.x == 5.0f && s.v == 0.0f, "huge dt lands on target: %g %g", s.x, s.v);

	/* 2D equals two 1D springs. */
	{
		struct stage_spring2 p = {0.0f, 10.0f, 1.0f, -2.0f};
		struct stage_spring sx = {0.0f, 1.0f}, sy = {10.0f, -2.0f};
		for (int i = 0; i < 30; i++) {
			stage_spring2_step(&p, 50.0f, -20.0f, 6.0f, 1.0f / 60.0f);
			stage_spring_step(&sx, 50.0f, 6.0f, 1.0f / 60.0f);
			stage_spring_step(&sy, -20.0f, 6.0f, 1.0f / 60.0f);
		}
		CHECK(p.x == sx.x && p.y == sy.x && p.vx == sx.v && p.vy == sy.v, "2D == 1D x2");
	}

	/* Angles: shortest way round and wrapped state. */
	CHECK(stage_wrap_deg(180.0f) == -180.0f, "wrap 180 -> %g", stage_wrap_deg(180.0f));
	CHECK(stage_wrap_deg(-180.0f) == -180.0f, "wrap -180");
	CHECK(NEAR(stage_wrap_deg(190.0f), -170.0, 1e-4), "wrap 190");
	CHECK(NEAR(stage_wrap_deg(-190.0f), 170.0, 1e-4), "wrap -190");
	CHECK(NEAR(stage_wrap_deg(725.0f), 5.0, 1e-4), "wrap 725");
	CHECK(stage_wrap_deg(NAN) == 0.0f && stage_wrap_deg(INFINITY) == 0.0f, "wrap non-finite");
	{
		struct stage_spring a = {170.0f, 0.0f};
		int short_way = 1;
		for (int i = 0; i < 240; i++) {
			stage_spring_angle_step(&a, -170.0f, 8.0f, 1.0f / 60.0f);
			if (fabsf(a.x) < 169.9f || a.x >= 180.0f || a.x < -180.0f)
				short_way = 0;
		}
		CHECK(short_way, "angle spring crosses 180, not 0");
		CHECK(NEAR(a.x, -170.0, 1e-3), "angle converges: %g", a.x);
	}

	/* Rate limiter (Live-safe caps). */
	CHECK(NEAR(stage_rate_limit(1.0f, 2.0f, 0.5f, 0.1f), 1.05, 1e-6), "rate up");
	CHECK(NEAR(stage_rate_limit(1.0f, 0.0f, 0.5f, 0.1f), 0.95, 1e-6), "rate down");
	CHECK(stage_rate_limit(1.0f, 1.01f, 0.5f, 0.1f) == 1.01f, "rate within step");
	CHECK(stage_rate_limit(1.0f, 9.0f, -1.0f, 0.1f) == 9.0f, "negative rate = no limit");
	CHECK(stage_rate_limit(1.0f, 9.0f, 0.5f, 0.0f) == 1.0f, "dt 0 = no move");
}

/* ---------------------------------------------------------------------- */
/* Noise                                                                  */
/* ---------------------------------------------------------------------- */

static void test_noise(void)
{
	/* Determinism: same seed and x -> bit-identical values. */
	float first[1000];
	for (int i = 0; i < 1000; i++)
		first[i] = stage_noise1(42u, (float)i * 0.0137f - 3.0f);
	int same = 1, differs = 0;
	for (int i = 0; i < 1000; i++) {
		float x = (float)i * 0.0137f - 3.0f;
		if (stage_noise1(42u, x) != first[i])
			same = 0;
		if (fabsf(stage_noise1(43u, x) - first[i]) > 1e-3f)
			differs = 1;
	}
	CHECK(same, "noise is deterministic");
	CHECK(differs, "different seeds give different patterns");
	CHECK(stage_noise_fbm(7u, 12.34f, 4, 2.0f, 0.5f) == stage_noise_fbm(7u, 12.34f, 4, 2.0f, 0.5f),
	      "fbm deterministic");

	/* Reference values: pin the pattern so it never changes silently
	 * between releases (saved looks must replay identically). */
	{
		float r1 = stage_noise1(1u, 0.5f), r2 = stage_noise1(12345u, 10.25f);
		if (g_verbose)
			printf("  noise ref: %.9g %.9g\n", r1, r2);
		CHECK(NEAR(r1, -0.47421515, 1e-6) && NEAR(r2, 0.488014102, 1e-6),
		      "reference values changed: %.9g %.9g", r1, r2);
	}

	/* Range, zero crossings at lattice points and smoothness. */
	float lo = 1.0f, hi = -1.0f, flo = 1.0f, fhi = -1.0f;
	double sum_abs = 0.0;
	int smooth = 1;
	for (int i = 0; i < 100000; i++) {
		float x = (float)i * 0.01937f;
		float v = stage_noise1(99u, x);
		float f = stage_noise_fbm(99u, x, 5, 2.0f, 0.5f);
		lo = fminf(lo, v);
		hi = fmaxf(hi, v);
		flo = fminf(flo, f);
		fhi = fmaxf(fhi, f);
		sum_abs += fabs(v);
		if (fabsf(stage_noise1(99u, x + 1e-3f) - v) > 0.01f)
			smooth = 0;
	}
	CHECK(lo >= -1.0f && hi <= 1.0f, "noise range %g..%g", lo, hi);
	CHECK(flo >= -1.0f && fhi <= 1.0f, "fbm range %g..%g", flo, fhi);
	CHECK(sum_abs / 100000.0 > 0.1, "noise is not flat: mean |n| = %g", sum_abs / 100000.0);
	CHECK(hi - lo > 1.0f, "noise uses its range: %g..%g", lo, hi);
	CHECK(smooth, "noise is continuous (slope bounded)");
	for (int i = -50; i <= 50; i++)
		if (!CHECK(stage_noise1(5u, (float)i) == 0.0f, "zero at lattice %d", i))
			break;

	/* Periodicity and seamless phase wrapping. */
	for (int i = 0; i < 2000; i++) {
		float x = (float)i / 64.0f;
		if (!CHECK(NEAR(stage_noise1(3u, x), stage_noise1(3u, x + (float)STAGE_NOISE_PERIOD),
				1e-6),
			   "periodic at %g", x))
			break;
	}
	float ph = stage_noise_advance((float)STAGE_NOISE_PERIOD - 0.01f, 1.0f, 0.02f);
	CHECK(NEAR(ph, 0.01, 1e-3), "advance wraps: %g", ph);
	CHECK(NEAR(stage_noise1(3u, ph), stage_noise1(3u, 0.01f), 1e-4), "seamless wrap");
	CHECK(stage_noise_advance(1.0f, NAN, 0.1f) == 0.0f, "advance NaN -> 0");
	CHECK(stage_noise1(3u, NAN) == 0.0f && stage_noise1(3u, INFINITY) == 0.0f, "non-finite x");
	CHECK(isfinite(stage_noise1(3u, -1e9f)) && isfinite(stage_noise1(3u, 1e30f)), "huge x");
	CHECK(stage_noise_fbm(3u, 0.3f, 0, 2.0f, 0.5f) == stage_noise_fbm(3u, 0.3f, 1, 2.0f, 0.5f),
	      "octaves clamp to >= 1");
}

/* ---------------------------------------------------------------------- */
/* Solver                                                                 */
/* ---------------------------------------------------------------------- */

static double xform_pos_err(const struct stage_xform *a, const struct stage_xform *b)
{
	return fmax(fabs((double)a->px - b->px), fabs((double)a->py - b->py));
}

static void test_solver_basics(void)
{
	const float W = 1920.0f, H = 1080.0f;
	struct stage_cam home = STAGE_CAM_HOME_INIT;
	struct stage_xform rest = {100.0f, 200.0f, 0.5f, 0.75f, 10.0f}, out;

	/* Home camera is the identity for every depth. */
	const float depths[] = {0.1f, 0.6f, 1.0f, 3.0f, 10.0f};
	for (int i = 0; i < 5; i++) {
		stage_solve_layer(&rest, depths[i], &home, W, H, &out);
		CHECK(xform_pos_err(&rest, &out) < 1e-4 && out.sx == rest.sx && out.sy == rest.sy &&
			      out.rot == rest.rot,
		      "home identity at depth %g", depths[i]);
	}

	/* Character plane (f = 1) follows the camera exactly. */
	struct stage_cam cam = {100.0f, -50.0f, 2.0f, 0.0f};
	struct stage_xform c = {960.0f, 540.0f, 1.0f, 1.0f, 0.0f};
	stage_solve_layer(&c, 1.0f, &cam, W, H, &out);
	CHECK(NEAR(out.px, 860.0, 1e-3) && NEAR(out.py, 590.0, 1e-3), "char pan %g,%g", out.px,
	      out.py);
	CHECK(NEAR(out.sx, 2.0, 1e-6), "char zoom");
	struct stage_xform corner = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f};
	struct stage_cam z2 = {0.0f, 0.0f, 2.0f, 0.0f};
	stage_solve_layer(&corner, 1.0f, &z2, W, H, &out);
	CHECK(NEAR(out.px, -960.0, 1e-3) && NEAR(out.py, -540.0, 1e-3), "zoom about centre");
	struct stage_cam r90 = {0.0f, 0.0f, 1.0f, 90.0f};
	struct stage_xform right = {1060.0f, 540.0f, 1.0f, 1.0f, 0.0f};
	stage_solve_layer(&right, 1.0f, &r90, W, H, &out);
	/* +90 deg clockwise on a y-down canvas: right of centre -> below it. */
	CHECK(NEAR(out.px, 960.0, 1e-3) && NEAR(out.py, 640.0, 1e-3) && NEAR(out.rot, 90.0, 1e-4),
	      "rotation clockwise: %g,%g rot %g", out.px, out.py, out.rot);

	/* Parallax ordering for a pan: far moves less, near moves more. */
	struct stage_cam pan = {90.0f, 0.0f, 1.0f, 0.0f};
	struct stage_xform bg, ch, fg;
	stage_solve_layer(&c, 3.0f, &pan, W, H, &bg);
	stage_solve_layer(&c, 1.0f, &pan, W, H, &ch);
	stage_solve_layer(&c, 0.5f, &pan, W, H, &fg);
	CHECK(NEAR(960.0 - bg.px, 30.0, 1e-3) && NEAR(960.0 - ch.px, 90.0, 1e-3) &&
		      NEAR(960.0 - fg.px, 180.0, 1e-3),
	      "parallax %g %g %g", 960.0 - bg.px, 960.0 - ch.px, 960.0 - fg.px);

	/* Depth clamp and continuity at 10 (no pop at the edge). */
	CHECK(stage_depth_factor(10.0f) == 0.1f, "f(10) = 0.1, not 0");
	CHECK(stage_depth_factor(1000.0f) == 0.1f, "f(>10) clamps to 0.1");
	CHECK(stage_depth_factor(0.01f) == 10.0f, "f(<0.1) clamps to 10");
	CHECK(stage_depth_factor(NAN) == 1.0f, "f(NaN) = 1");
	CHECK(NEAR(stage_depth_factor(9.999f), stage_depth_factor(10.0f), 2e-5), "continuous at 10");
	{
		/* Monotone, and every step bounded by the derivative 1/d^2: no pop. */
		float prev_d = 0.1f, prev = stage_depth_factor(0.1f);
		int ok = 1;
		for (int i = 1; i <= 10000; i++) {
			float d = 0.1f + 9.9f * (float)i / 10000.0f;
			float f = stage_depth_factor(d);
			double max_step = (double)(d - prev_d) / ((double)prev_d * prev_d) * 1.001 + 1e-6;
			if (f > prev || (double)(prev - f) > max_step)
				ok = 0;
			prev = f;
			prev_d = d;
		}
		CHECK(ok, "factor monotone and jump-free over [0.1, 10]");
	}

	/* Roles. */
	CHECK(NEAR(stage_role_factor(STAGE_ROLE_BG, 3.0f), 1.0 / 3.0, 1e-7), "bg factor");
	CHECK(stage_role_factor(STAGE_ROLE_CHAR, 1.0f) == 1.0f, "char factor");
	CHECK(stage_role_factor(STAGE_ROLE_SKY, 3.0f) == 0.0f, "sky factor 0");
	CHECK(stage_role_factor(STAGE_ROLE_STATIC, 0.2f) == 0.0f, "static factor 0");
	CHECK(stage_role_factor(STAGE_ROLE_NONE, 1.0f) == 0.0f, "none factor 0");
	CHECK(stage_role_factor(99, 1.0f) == 0.0f, "unknown role factor 0");
	CHECK(stage_role_default_depth(STAGE_ROLE_BG) == 3.0f &&
		      stage_role_default_depth(STAGE_ROLE_CHAR) == 1.0f &&
		      stage_role_default_depth(STAGE_ROLE_FG) == 0.6f &&
		      stage_role_default_depth(STAGE_ROLE_SKY) == 10.0f &&
		      stage_role_default_depth(STAGE_ROLE_STATIC) == 1.0f,
	      "role default depths");
	/* f = 0 (sky/static) never moves. */
	struct stage_cam wild = {500.0f, -300.0f, 3.5f, 40.0f};
	stage_solve_layer_f(&rest, 0.0f, &wild, W, H, &out);
	CHECK(xform_pos_err(&rest, &out) < 1e-4 && out.sx == rest.sx && out.rot == rest.rot,
	      "f = 0 is pinned");
	stage_solve_layer_f(&rest, NAN, &wild, W, H, &out);
	CHECK(xform_pos_err(&rest, &out) < 1e-4, "f NaN treated as 0");

	/* Layer zoom factor (vendor-api.md section 3 examples). */
	CHECK(stage_layer_zoom(2.0f, 0.0f) == 1.0f, "s(f=0) = 1");
	CHECK(NEAR(stage_layer_zoom(2.0f, 1.0f), 2.0, 1e-6), "s(f=1) = zoom");
	CHECK(NEAR(stage_layer_zoom(0.5f, 10.0f), 0.25, 1e-6), "depth 0.1 at zoom 0.5 clamps");
	CHECK(NEAR(stage_layer_zoom(0.5f, 2.0f), 0.25, 1e-6), "depth 0.5 at zoom 0.5");
	CHECK(NEAR(stage_layer_zoom(0.5f, 1.0f / 0.6f), 0.315, 1e-3), "fg 0.6 at zoom 0.5");
	CHECK(stage_layer_zoom(4.0f, 10.0f) == 8.0f, "upper clamp 8");
	CHECK(stage_layer_zoom(100.0f, 1.0f) == 4.0f, "zoom clamps to 4");
	{
		int ok = 1;
		float prev = stage_layer_zoom(1.7f, 0.0f);
		for (int i = 1; i <= 100; i++) {
			float s = stage_layer_zoom(1.7f, 10.0f * (float)i / 100.0f);
			if (s < prev || s <= 0.0f)
				ok = 0;
			prev = s;
		}
		CHECK(ok, "s monotone in f and positive");
	}
}

/* Distance from |x| to the next float: the storage quantum OBS uses. */
static double ulpf(float x)
{
	float a = fabsf(x);
	return (double)nextafterf(a, INFINITY) - (double)a;
}

/*
 * One random solve/unsolve case on a W x H canvas. Returns the position
 * errors of both directions through *e1 / *e2 and the float-quantum bounds
 * through *b1 / *b2; returns 1 when the solved item lies on or near the
 * canvas (within one canvas size of the centre), i.e. it can be visible,
 * and every stored position is below 4096 px (see the end of the function).
 */
static int roundtrip_case(float W, float H, double *e1, double *e2, double *b1, double *b2,
			  double *es, double *er)
{
	struct stage_xform rest;
	rest.px = rng_range(-0.5f * W, 1.5f * W);
	rest.py = rng_range(-0.5f * H, 1.5f * H);
	rest.sx = rng_range(0.1f, 4.0f) * (rng_u32() & 1 ? 1.0f : -1.0f); /* flips too */
	rest.sy = rng_range(0.1f, 4.0f);
	rest.rot = rng_range(-180.0f, 180.0f);
	float depth = powf(10.0f, rng_range(-1.0f, 1.0f)); /* log-uniform 0.1..10 */
	struct stage_cam cam = {rng_range(-0.5f * W, 0.5f * W), rng_range(-0.5f * H, 0.5f * H),
				rng_range(0.5f, 4.0f), rng_range(-45.0f, 45.0f)};
	double s = stage_layer_zoom(cam.zoom, stage_depth_factor(depth));

	struct stage_xform cur, back;
	stage_solve_layer(&rest, depth, &cam, W, H, &cur);
	stage_unsolve_layer(&cur, depth, &cam, W, H, &back);
	*e1 = xform_pos_err(&rest, &back);
	*b1 = 2.0 * (fmax(ulpf(cur.px), ulpf(cur.py)) / s + fmax(ulpf(rest.px), ulpf(rest.py)));
	*es = fmax(fabs((double)back.sx / rest.sx - 1.0), fabs((double)back.sy / rest.sy - 1.0));
	*er = fabs((double)back.rot - rest.rot);

	/* The other direction: rest = unsolve(user-moved item), solve back. */
	struct stage_xform rest2, cur2;
	stage_unsolve_layer(&rest, depth, &cam, W, H, &rest2);
	stage_solve_layer(&rest2, depth, &cam, W, H, &cur2);
	*e2 = xform_pos_err(&rest, &cur2);
	*b2 = 2.0 * (fmax(ulpf(rest2.px), ulpf(rest2.py)) * s + fmax(ulpf(rest.px), ulpf(rest.py)));

	int near_canvas = fabsf(cur.px - 0.5f * W) <= W && fabsf(cur.py - 0.5f * H) <= H;
	/* Every stored coordinate below 4096 px, where one float quantum is at
	 * most 2^-12 px, so the 1e-3 px limit has a fixed ~2x margin whatever
	 * the sampling ranges. Larger coordinates (possible at 4K: rest up to
	 * 1.5 W = 5760 px) have a quantum of ~0.5e-3..1e-3 px and are held to
	 * the float-quantum bound only. */
	float m = fmaxf(fmaxf(fabsf(rest.px), fabsf(rest.py)), fmaxf(fabsf(cur.px), fabsf(cur.py)));
	m = fmaxf(m, fmaxf(fabsf(rest2.px), fabsf(rest2.py)));
	return near_canvas && m < 4096.0f;
}

static void test_solver_roundtrip(void)
{
	/*
	 * Acceptance: solve -> unsolve returns the rest pose within 1e-3 px
	 * over 10k random cases on each canvas group (720p..1440p, and 4K),
	 * counting cases whose solved item lies on or near the canvas and whose
	 * stored positions are below 4096 px (one float quantum <= 2^-12 px).
	 * The reverse direction (unsolve -> solve, i.e. a user-moved item) is
	 * measured on screen; there the rest pose's float quantum is multiplied
	 * by the layer zoom (up to 8), so it is held to 2e-3 px. The arithmetic
	 * itself runs in double and contributes far less than one float quantum;
	 * off-screen cases (far foreground layers under big pans) are checked
	 * against a 2-quantum bound only.
	 */
	const float sizes[4][2] = {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
	double max_e1 = 0.0, max_e2 = 0.0, max_4k1 = 0.0, max_4k2 = 0.0, max_s = 0.0, max_r = 0.0;
	int visible = 0, visible_4k = 0, bound_ok = 1, tries = 0;
	while ((visible < 10000 || visible_4k < 10000) && tries < 200000) {
		int k = tries++ % 4;
		double e1, e2, b1, b2, es, er;
		int vis = roundtrip_case(sizes[k][0], sizes[k][1], &e1, &e2, &b1, &b2, &es, &er);
		if (e1 > b1 || e2 > b2) {
			if (bound_ok)
				printf("  quantum bound: e1 %g (bound %g) e2 %g (bound %g)\n", e1, b1,
				       e2, b2);
			bound_ok = 0;
		}
		max_s = fmax(max_s, es);
		max_r = fmax(max_r, er);
		if (!vis)
			continue;
		if (k == 3) {
			visible_4k++;
			max_4k1 = fmax(max_4k1, e1);
			max_4k2 = fmax(max_4k2, e2);
		} else {
			visible++;
			max_e1 = fmax(max_e1, e1);
			max_e2 = fmax(max_e2, e2);
		}
	}
	if (g_verbose)
		printf("  roundtrip: %d + %d (4K) cases of %d tried; solve/unsolve max %.3g px "
		       "(4K %.3g), unsolve/solve max %.3g px (4K %.3g), scale %.3g, rot %.3g deg\n",
		       visible, visible_4k, tries, max_e1, max_4k1, max_e2, max_4k2, max_s, max_r);
	CHECK(visible >= 10000 && visible_4k >= 10000, "enough cases %d %d", visible, visible_4k);
	CHECK(max_e1 < 1e-3, "solve/unsolve position error %g px", max_e1);
	CHECK(max_4k1 < 1e-3, "4K solve/unsolve position error %g px", max_4k1);
	CHECK(max_e2 < 2e-3 && max_4k2 < 2e-3, "unsolve/solve error %g / %g px", max_e2, max_4k2);
	CHECK(bound_ok, "every case within 2 float quanta");
	CHECK(max_s < 1e-5, "scale relative error %g", max_s);
	CHECK(max_r < 1e-3, "rotation error %g deg", max_r);

	/* Ten save/load cycles (rest -> solve -> unsolve) accumulate no drift. */
	struct stage_xform r0 = {1234.5f, -321.25f, 1.3f, 1.3f, 12.0f}, r = r0, cur;
	struct stage_cam cam = {-333.0f, 123.0f, 2.7f, -31.0f};
	for (int i = 0; i < 10; i++) {
		stage_solve_layer(&r, 0.37f, &cam, 1920.0f, 1080.0f, &cur);
		stage_unsolve_layer(&cur, 0.37f, &cam, 1920.0f, 1080.0f, &r);
	}
	CHECK(xform_pos_err(&r0, &r) < 1e-3 && NEAR(r.sx, r0.sx, 1e-5) && NEAR(r.rot, r0.rot, 1e-4),
	      "10 cycles drift %g px", xform_pos_err(&r0, &r));
}

static void test_ui_depth_and_dof(void)
{
	/* d = 10^(-v/100) (vendor-api.md section 3), so the slider ends land exactly on the depth range. */
	CHECK(STAGE_UI_DEPTH_SCALE == 100.0 && STAGE_UI_DEPTH_MAX == STAGE_UI_DEPTH_SCALE &&
		      STAGE_UI_DEPTH_MIN == -STAGE_UI_DEPTH_SCALE,
	      "slider range is +-scale");
	CHECK(stage_depth_from_ui(0.0f) == 1.0f, "v 0 -> 1");
	CHECK(stage_depth_from_ui(-100.0f) == 10.0f, "v -100 -> 10 exactly");
	CHECK(stage_depth_from_ui(100.0f) == 0.1f, "v 100 -> 0.1 exactly");
	CHECK(NEAR(stage_depth_from_ui(-50.0f), sqrt(10.0), 1e-5), "v -50 -> sqrt(10)");
	CHECK(NEAR(stage_depth_from_ui(50.0f), 1.0 / sqrt(10.0), 1e-6), "v 50 -> 1/sqrt(10)");
	CHECK(stage_depth_from_ui(-1e30f) == 10.0f && stage_depth_from_ui(1e30f) == 0.1f, "huge v");
	CHECK(stage_depth_from_ui(-150.0f) == 10.0f && stage_depth_from_ui(150.0f) == 0.1f, "|v| > 100 saturates");
	CHECK(stage_depth_from_ui(NAN) == 1.0f, "v NaN -> 1");
	CHECK(NEAR(stage_depth_from_ui(-47.712f), 3.0, 1e-3), "v -47.71 -> bg 3");
	CHECK(stage_ui_from_depth(1.0f) == 0.0f, "d 1 -> 0");
	CHECK(NEAR(stage_ui_from_depth(10.0f), -100.0, 1e-4), "d 10 -> -100");
	CHECK(NEAR(stage_ui_from_depth(0.1f), 100.0, 1e-4), "d 0.1 -> 100");
	CHECK(NEAR(stage_ui_from_depth(100.0f), -100.0, 1e-4) && NEAR(stage_ui_from_depth(0.001f), 100.0, 1e-4),
	      "d clamps");
	CHECK(stage_ui_from_depth(NAN) == 0.0f, "d NaN -> 0");
	/* Role defaults as the integer slider shows them. */
	CHECK(lroundf(stage_ui_from_depth(stage_role_default_depth(STAGE_ROLE_BG))) == -48, "bg 3 -> -48");
	CHECK(lroundf(stage_ui_from_depth(stage_role_default_depth(STAGE_ROLE_FG))) == 22, "fg 0.6 -> +22");
	CHECK(lroundf(stage_ui_from_depth(stage_role_default_depth(STAGE_ROLE_SKY))) == -100, "sky 10 -> -100");
	CHECK(lroundf(stage_ui_from_depth(stage_role_default_depth(STAGE_ROLE_CHAR))) == 0, "char 1 -> 0");
	/* Every integer slider step is live (strictly decreasing depth, no saturated
	 * plateau) and survives the props round trip v -> depth -> lroundf(v). */
	for (int i = (int)STAGE_UI_DEPTH_MIN; i <= (int)STAGE_UI_DEPTH_MAX; i++) {
		float d = stage_depth_from_ui((float)i);
		bool live = i == (int)STAGE_UI_DEPTH_MIN || d < stage_depth_from_ui((float)(i - 1));
		long back = lroundf(stage_ui_from_depth(d));
		if (!CHECK(live && back == i && d >= (float)STAGE_DEPTH_MIN && d <= (float)STAGE_DEPTH_MAX,
			   "slider step %d -> %g -> %ld", i, d, back))
			break;
	}
	for (int i = -200; i <= 200; i++) {
		float v = (float)i * 0.5f;
		float back = stage_ui_from_depth(stage_depth_from_ui(v));
		if (!CHECK(NEAR(back, v, 1e-3), "ui roundtrip %g -> %g", v, back))
			break;
	}
	for (int i = 0; i <= 200; i++) {
		float d = powf(10.0f, -1.0f + 2.0f * (float)i / 200.0f);
		float back = stage_depth_from_ui(stage_ui_from_depth(d));
		if (!CHECK(NEAR(back / d, 1.0, 1e-5), "depth roundtrip %g -> %g", d, back))
			break;
	}

	/* Depth of field strength. */
	CHECK(stage_dof_strength(3.0f, 3.0f, 1.0f) == 0.0f, "in focus");
	CHECK(NEAR(stage_dof_strength(2.0f, 1.0f, 0.5f), 0.5, 1e-6), "|2-1| * 0.5");
	CHECK(stage_dof_strength(10.0f, 1.0f, 0.5f) == 1.0f, "clamped to 1");
	CHECK(stage_dof_strength(0.6f, 1.0f, 0.0f) == 0.0f, "amount 0 = off");
	CHECK(stage_dof_strength(NAN, 1.0f, 0.5f) == 0.0f, "NaN -> 0");
	CHECK(NEAR(stage_dof_strength(0.6f, 1.0f, 2.0f), 0.4, 1e-6), "amount clamps to 1");
}

/* ---------------------------------------------------------------------- */
/* Edge coverage                                                          */
/* ---------------------------------------------------------------------- */

static struct stage_rect grow_rect(struct stage_rect r, float o)
{
	float hw = 0.5f * (r.r - r.l), hh = 0.5f * (r.b - r.t);
	float mx = 0.5f * (r.r + r.l), my = 0.5f * (r.b + r.t);
	struct stage_rect g = {mx - hw * (1 + o), my - hh * (1 + o), mx + hw * (1 + o),
			       my + hh * (1 + o)};
	return g;
}

static void test_coverage(void)
{
	const float W = 1920.0f, H = 1080.0f;
	const struct stage_rect canvas = {0.0f, 0.0f, W, H};
	const float f_bg = 1.0f / 3.0f;
	struct stage_cam home = STAGE_CAM_HOME_INIT;

	CHECK(NEAR(stage_cover_margin(&canvas, f_bg, &home, W, H), 0.0, 1e-3), "home margin 0");
	CHECK(stage_cover_min_oversize(&canvas, f_bg, &home, W, H) == 0.0f, "home needs none");

	/* Panning reveals the edge; the computed oversize fixes it tightly. */
	struct stage_cam pan = {100.0f, 0.0f, 1.0f, 0.0f};
	float m = stage_cover_margin(&canvas, f_bg, &pan, W, H);
	CHECK(NEAR(m, -100.0 / 3.0, 1e-2), "pan margin %g", m);
	float o = stage_cover_min_oversize(&canvas, f_bg, &pan, W, H);
	CHECK(NEAR(o, (100.0 / 3.0) / 960.0, 1e-5), "pan oversize %g", o);
	struct stage_rect g = grow_rect(canvas, o);
	CHECK(stage_cover_margin(&g, f_bg, &pan, W, H) >= -1e-2f, "oversize covers");
	struct stage_rect g99 = grow_rect(canvas, o * 0.99f);
	CHECK(stage_cover_margin(&g99, f_bg, &pan, W, H) < 0.0f, "oversize is minimal");

	/* Rotation and zoom-out need oversize too; check sufficiency at random. */
	for (int i = 0; i < 2000; i++) {
		struct stage_cam c = {rng_range(-300, 300), rng_range(-200, 200), rng_range(0.5f, 4.0f),
				      rng_range(-45, 45)};
		float f = rng_range(0.1f, 2.0f);
		float oo = stage_cover_min_oversize(&canvas, f, &c, W, H);
		struct stage_rect gg = grow_rect(canvas, oo);
		if (!CHECK(stage_cover_margin(&gg, f, &c, W, H) >= -0.02f, "random oversize %d", i))
			break;
	}

	/* Range: Live-safe minimum overscan for a camera envelope. */
	struct stage_cover_range range = {200.0f, 100.0f, 0.8f, 2.0f, 8.0f};
	float orng = stage_cover_min_oversize_range(&canvas, f_bg, &range, W, H);
	CHECK(orng > 0.0f && orng < 0.25f, "range oversize %g", orng);
	struct stage_rect grng = grow_rect(canvas, orng);
	double worst = 0.0;
	for (int i = 0; i < 5000; i++) {
		struct stage_cam c = {rng_range(-200, 200), rng_range(-100, 100), rng_range(0.8f, 2.0f),
				      rng_range(-8, 8)};
		worst = fmin(worst, (double)stage_cover_margin(&grng, f_bg, &c, W, H));
		CHECK(stage_cover_min_oversize(&canvas, f_bg, &c, W, H) <= orng + 1e-4f,
		      "single <= range");
	}
	CHECK(worst >= -0.1, "range oversize covers every sampled camera (worst %g px)", worst);

	/* Minimum zoom. */
	CHECK(NEAR(stage_cover_min_zoom(&canvas, f_bg, 0.0f, W, H), 1.0, 1e-5), "min zoom 1");
	float zr = stage_cover_min_zoom(&canvas, f_bg, 8.0f, W, H);
	CHECK(zr > 1.0f && zr < 4.0f, "rotation needs zoom: %g", zr);
	struct stage_pan_box box;
	CHECK(stage_cover_pan_box(&canvas, f_bg, zr * 1.0001f, 8.0f, W, H, &box), "feasible at zmin");
	CHECK(!stage_cover_pan_box(&canvas, f_bg, zr * 0.99f, 8.0f, W, H, &box), "infeasible below");
	struct stage_rect half = {480.0f, 270.0f, 1440.0f, 810.0f};
	CHECK(stage_cover_min_zoom(&half, 0.1f, 0.0f, W, H) > (float)STAGE_ZOOM_MAX,
	      "impossible within zoom range");

	/* Pan box: 8 % overscan at depth 3 allows +-76.8 px * 3 of camera pan. */
	struct stage_rect os = grow_rect(canvas, 0.08f);
	CHECK(stage_cover_pan_box(&os, f_bg, 1.0f, 0.0f, W, H, &box), "pan box ok");
	CHECK(NEAR(box.x_min, -230.4, 1e-2) && NEAR(box.x_max, 230.4, 1e-2) &&
		      NEAR(box.y_min, -129.6, 1e-2) && NEAR(box.y_max, 129.6, 1e-2),
	      "pan box %g..%g, %g..%g", box.x_min, box.x_max, box.y_min, box.y_max);

	/* Clamp: covered cameras are untouched; others are fixed minimally. */
	struct stage_cam ok_cam = {50.0f, -20.0f, 1.2f, 2.0f}, outc;
	CHECK(stage_cover_clamp(&os, &f_bg, 1, &ok_cam, W, H, &outc), "covered");
	CHECK(memcmp(&ok_cam, &outc, sizeof(outc)) == 0, "covered camera unchanged");
	struct stage_cam far_cam = {1000.0f, 0.0f, 1.0f, 0.0f};
	CHECK(stage_cover_clamp(&os, &f_bg, 1, &far_cam, W, H, &outc), "clamp succeeds");
	CHECK(NEAR(outc.x, 230.4, 1e-2) && outc.y == 0.0f && outc.zoom == 1.0f, "clamped x %g",
	      outc.x);
	struct stage_cam out_cam = {0.0f, 0.0f, 0.6f, 0.0f};
	CHECK(stage_cover_clamp(&os, &f_bg, 1, &out_cam, W, H, &outc), "zoom-out clamp");
	CHECK(outc.zoom >= stage_cover_min_zoom(&os, f_bg, 0.0f, W, H) - 1e-5f &&
		      outc.zoom < 1.0f,
	      "zoom raised to %g", outc.zoom);

	/* Several covering layers (sky/static ignored) under random cameras. */
	struct stage_rect rects[3] = {grow_rect(canvas, 0.10f), grow_rect(canvas, 0.05f), canvas};
	float fs[3] = {1.0f / 3.0f, 1.0f / 6.0f, 0.0f};
	int all_ok = 1;
	for (int i = 0; i < 3000 && all_ok; i++) {
		struct stage_cam c = {rng_range(-1500, 1500), rng_range(-900, 900), rng_range(0.5f, 4.0f),
				      rng_range(-8, 8)};
		struct stage_cam r;
		bool res = stage_cover_clamp(rects, fs, 3, &c, W, H, &r);
		for (int k = 0; k < 2; k++)
			if (stage_cover_margin(&rects[k], fs[k], &r, W, H) < -0.01f)
				all_ok = 0;
		if (!res || r.rot != c.rot || r.zoom < 0.5f || r.zoom > 4.0f)
			all_ok = 0;
		if (!all_ok)
			printf("  cam %g,%g z%g r%g -> %g,%g z%g (res %d)\n", c.x, c.y, c.zoom, c.rot,
			       r.x, r.y, r.zoom, res);
	}
	CHECK(all_ok, "multi-layer clamp keeps the canvas covered");
	CHECK(!stage_cover_clamp(&half, &fs[1], 1, &home, W, H, &outc), "impossible reported");

	/* Invalid rects. */
	struct stage_rect bad = {10.0f, 10.0f, 10.0f, 50.0f};
	CHECK(stage_cover_margin(&bad, 1.0f, &home, W, H) == -FLT_MAX, "invalid margin");
	CHECK(stage_cover_min_oversize(&bad, 1.0f, &home, W, H) == 0.0f, "invalid oversize");
	CHECK(stage_cover_clamp(&bad, &fs[0], 1, &far_cam, W, H, &outc) &&
		      memcmp(&far_cam, &outc, sizeof(outc)) == 0,
	      "invalid rect ignored by clamp");
}

/* ---------------------------------------------------------------------- */
/* Moves                                                                  */
/* ---------------------------------------------------------------------- */

/* Copies a short literal id into a char[STAGE_ID_MAX] (no strcpy: C4996). */
static void set_id(char *dst, const char *src)
{
	size_t n = strlen(src);
	if (n >= STAGE_ID_MAX)
		n = STAGE_ID_MAX - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static void make_zoom_in(struct stage_move_def *d)
{
	memset(d, 0, sizeof(*d));
	set_id(d->id, "zoom_in");
	d->duration_ms = 800;
	d->relative = true;
	d->hold = true;
	d->nkeys = 2;
	stage_key_init(&d->keys[0]);
	stage_key_init(&d->keys[1]);
	d->keys[1].t = 1.0f;
	d->keys[1].dzoom = 0.25f;
	d->keys[1].ease = STAGE_EASE_OUT_CUBIC;
	d->keys[1].mask = STAGE_CH_ZOOM;
}

static void test_move(void)
{
	struct stage_move_def d;
	struct stage_move_delta o;
	make_zoom_in(&d);
	CHECK(stage_move_valid(&d), "zoom_in valid");

	CHECK(!stage_move_eval(&d, 0.0f, 1.0f, &o), "t=0 running");
	CHECK(o.dx == 0.0f && o.dy == 0.0f && o.dzoom == 0.0f && o.drot == 0.0f && o.shake == 0.0f,
	      "start is zero");
	CHECK(stage_move_eval(&d, 800.0f, 1.0f, &o) && o.dzoom == 0.25f, "end value %g", o.dzoom);
	CHECK(stage_move_eval(&d, 5000.0f, 1.0f, &o) && o.dzoom == 0.25f, "after end holds");
	CHECK(!stage_move_eval(&d, -10.0f, 1.0f, &o) && o.dzoom == 0.0f, "negative t clamps");
	CHECK(!stage_move_eval(&d, NAN, 1.0f, &o) && o.dzoom == 0.0f, "NaN t -> start");
	stage_move_eval(&d, 400.0f, 1.0f, &o);
	CHECK(NEAR(o.dzoom, 0.25 * 0.875, 1e-6), "midpoint uses destination ease: %g", o.dzoom);
	stage_move_eval(&d, 800.0f, 1.2f, &o);
	CHECK(NEAR(o.dzoom, 0.3, 1e-6), "intensity scales: %g", o.dzoom);
	stage_move_eval(&d, 800.0f, 9.0f, &o);
	CHECK(NEAR(o.dzoom, 0.5, 1e-6), "intensity clamps to 2");
	stage_move_eval(&d, 800.0f, NAN, &o);
	CHECK(NEAR(o.dzoom, 0.25, 1e-6), "intensity NaN -> 1");

	/* Overrides from camera_move. */
	stage_move_eval_ex(&d, 400.0f, 0, 1.0f, STAGE_EASE_LINEAR, NULL, &o);
	CHECK(NEAR(o.dzoom, 0.125, 1e-6), "ease override");
	CHECK(stage_move_eval_ex(&d, 400.0f, 400, 1.0f, -1, NULL, &o) && o.dzoom == 0.25f,
	      "duration override");
	stage_move_eval_ex(&d, 400.0f, 0, 1.0f, 999, NULL, &o);
	CHECK(NEAR(o.dzoom, 0.25 * 0.875, 1e-6), "bad override ignored");

	/* Per-channel pass-through and keys at t = 0. */
	struct stage_move_def p;
	memset(&p, 0, sizeof(p));
	set_id(p.id, "custom-1");
	p.duration_ms = 1000;
	p.nkeys = 3;
	for (int i = 0; i < 3; i++) {
		stage_key_init(&p.keys[i]);
		p.keys[i].ease = STAGE_EASE_LINEAR;
	}
	p.keys[0].t = 0.0f;
	p.keys[0].shake = 0.5f;
	p.keys[0].mask = STAGE_CH_SHAKE;
	p.keys[1].t = 0.5f;
	p.keys[1].dx = 0.2f;
	p.keys[1].drot = 10.0f;
	p.keys[1].mask = STAGE_CH_X | STAGE_CH_ROT;
	p.keys[2].t = 1.0f;
	p.keys[2].dy = 0.1f;
	p.keys[2].drot = 0.0f;
	p.keys[2].mask = STAGE_CH_Y | STAGE_CH_ROT;
	CHECK(stage_move_valid(&p), "custom valid");
	stage_move_eval(&p, 0.0f, 1.0f, &o);
	CHECK(o.shake == 0.5f && o.dx == 0.0f && o.dy == 0.0f, "keyed at t=0");
	stage_move_eval(&p, 250.0f, 1.0f, &o);
	CHECK(NEAR(o.dx, 0.1, 1e-6) && NEAR(o.dy, 0.025, 1e-6) && NEAR(o.drot, 5.0, 1e-5) &&
		      o.shake == 0.5f,
	      "t=0.25: dx %g dy %g drot %g shake %g", o.dx, o.dy, o.drot, o.shake);
	stage_move_eval(&p, 750.0f, 1.0f, &o);
	CHECK(NEAR(o.dx, 0.2, 1e-6) && NEAR(o.dy, 0.075, 1e-6) && NEAR(o.drot, 5.0, 1e-5),
	      "t=0.75: dx held %g, dy passes key 1 %g, drot %g", o.dx, o.dy, o.drot);
	CHECK(stage_move_eval(&p, 1000.0f, 1.0f, &o) && NEAR(o.dx, 0.2, 1e-6) &&
		      NEAR(o.dy, 0.1, 1e-6) && o.drot == 0.0f && o.shake == 0.5f,
	      "end of custom move");

	/* Validation. */
	struct stage_move_def v = d;
	v.keys[1].t = 0.0f;
	CHECK(!stage_move_valid(&v), "non-increasing t");
	v = d;
	v.keys[1].ease = STAGE_EASE_COUNT;
	CHECK(!stage_move_valid(&v), "bad ease");
	v = d;
	v.nkeys = 1;
	CHECK(!stage_move_valid(&v), "one key");
	v = d;
	v.nkeys = 17;
	CHECK(!stage_move_valid(&v), "too many keys");
	v = d;
	v.duration_ms = 20;
	CHECK(!stage_move_valid(&v), "duration too short");
	v = d;
	set_id(v.id, "Zoom");
	CHECK(!stage_move_valid(&v), "bad id");
	v = d;
	v.keys[1].dzoom = 5.0f;
	CHECK(!stage_move_valid(&v), "dzoom out of range");
	v = d;
	v.keys[1].dx = NAN;
	CHECK(!stage_move_valid(&v), "NaN delta");
	v = d;
	v.keys[1].mask = 0x80;
	CHECK(!stage_move_valid(&v), "unknown mask bit");
	v = d;
	v.keys[1].t = 1.5f;
	CHECK(!stage_move_valid(&v), "t > 1");
	CHECK(!stage_move_valid(NULL), "NULL");
}

/* Built-in `reset` (data/presets/moves/reset.json): relative:false, key 0
 * {t:0} with no channels, key 1 all zeros with ease_in_out_cubic, 700 ms. */
static void make_reset(struct stage_move_def *d)
{
	memset(d, 0, sizeof(*d));
	set_id(d->id, "reset");
	d->duration_ms = 700;
	d->relative = false;
	d->hold = true;
	d->nkeys = 2;
	stage_key_init(&d->keys[0]);
	stage_key_init(&d->keys[1]);
	d->keys[1].t = 1.0f;
	d->keys[1].ease = STAGE_EASE_IN_OUT_CUBIC;
	d->keys[1].mask = STAGE_CH_ALL;
}

static void test_move_absolute(void)
{
	struct stage_move_def d;
	struct stage_move_delta o;
	make_reset(&d);
	CHECK(stage_move_valid(&d), "reset valid");

	/* Camera target at move start on a 1920x1080 canvas: x 480, y -108,
	 * zoom 1.8, rot 20, shake 0.4. Start = pose - home (vendor-api 6.1). */
	const float W = 1920.0f, H = 1080.0f;
	struct stage_move_delta st = {480.0f / W, -108.0f / H, 1.8f - 1.0f, 20.0f, 0.4f};

	CHECK(!stage_move_eval_ex(&d, 0.0f, 0, 1.0f, -1, &st, &o), "reset t=0 running");
	CHECK(o.dx == st.dx && o.dy == st.dy && o.dzoom == st.dzoom && o.drot == st.drot &&
		      o.shake == st.shake,
	      "reset starts at the start pose, not home (dx %g dzoom %g)", o.dx, o.dzoom);
	CHECK(NEAR(o.dx * W, 480.0, 1e-3) && NEAR(1.0 + o.dzoom, 1.8, 1e-6), "camera = home + out");

	/* Eases in-out cubic over 700 ms: 350 ms is halfway, 175 ms is 1/16
	 * of the way (4 t^3 at t = 0.25). */
	stage_move_eval_ex(&d, 350.0f, 0, 1.0f, -1, &st, &o);
	CHECK(NEAR(o.dx, st.dx * 0.5, 1e-6) && NEAR(o.dzoom, 0.4, 1e-6) &&
		      NEAR(o.drot, 10.0, 1e-5) && NEAR(o.shake, 0.2, 1e-6),
	      "reset midpoint dx %g dzoom %g drot %g", o.dx, o.dzoom, o.drot);
	stage_move_eval_ex(&d, 175.0f, 0, 1.0f, -1, &st, &o);
	CHECK(NEAR(o.dzoom, 0.8 * (1.0 - 0.0625), 1e-6) && NEAR(o.drot, 20.0 * 0.9375, 1e-4),
	      "reset quarter dzoom %g drot %g", o.dzoom, o.drot);
	/* Monotone approach to home, no snap on the first frame. */
	{
		float prev = 1e9f;
		int ok = 1;
		for (int i = 0; i <= 70; i++) {
			stage_move_eval_ex(&d, 10.0f * (float)i, 0, 1.0f, -1, &st, &o);
			if (o.dzoom > prev)
				ok = 0;
			prev = o.dzoom;
		}
		CHECK(ok, "reset zoom decreases monotonically");
		stage_move_eval_ex(&d, 1000.0f / 60.0f, 0, 1.0f, -1, &st, &o);
		CHECK(o.dzoom > 0.79f, "first 60 fps frame stays near the start: %g", o.dzoom);
	}
	CHECK(stage_move_eval_ex(&d, 700.0f, 0, 1.0f, -1, &st, &o) && o.dx == 0.0f &&
		      o.dy == 0.0f && o.dzoom == 0.0f && o.drot == 0.0f && o.shake == 0.0f,
	      "reset ends exactly at home");

	/* Intensity scales key values, not the start. */
	stage_move_eval_ex(&d, 0.0f, 0, 2.0f, -1, &st, &o);
	CHECK(o.dzoom == st.dzoom, "start is not scaled by intensity");

	/* NULL start = 0 (the old behaviour); NaN start fields -> 0. */
	stage_move_eval_ex(&d, 0.0f, 0, 1.0f, -1, NULL, &o);
	CHECK(o.dx == 0.0f && o.dzoom == 0.0f, "NULL start");
	struct stage_move_delta bad = {NAN, 0.1f, NAN, 0.0f, 0.0f};
	stage_move_eval_ex(&d, 0.0f, 0, 1.0f, -1, &bad, &o);
	CHECK(o.dx == 0.0f && o.dzoom == 0.0f && o.dy == 0.1f, "NaN start fields -> 0");

	/* Absolute move that keys only zoom: other channels stay at the start;
	 * a key at t = 0 overrides the start for its channel. */
	struct stage_move_def z;
	memset(&z, 0, sizeof(z));
	set_id(z.id, "abs-zoom");
	z.duration_ms = 1000;
	z.relative = false;
	z.nkeys = 2;
	stage_key_init(&z.keys[0]);
	stage_key_init(&z.keys[1]);
	z.keys[0].dx = 0.1f;
	z.keys[0].mask = STAGE_CH_X;
	z.keys[1].t = 1.0f;
	z.keys[1].dzoom = 1.0f;
	z.keys[1].ease = STAGE_EASE_LINEAR;
	z.keys[1].mask = STAGE_CH_ZOOM;
	CHECK(stage_move_valid(&z), "abs-zoom valid");
	stage_move_eval_ex(&z, 500.0f, 0, 1.0f, -1, &st, &o);
	CHECK(o.dx == 0.1f && o.dy == st.dy && o.drot == st.drot && o.shake == st.shake &&
		      NEAR(o.dzoom, 0.9, 1e-6),
	      "abs-zoom mid: dx %g dy %g dzoom %g", o.dx, o.dy, o.dzoom);
}

/* ---------------------------------------------------------------------- */
/* Envelope and kick detector                                             */
/* ---------------------------------------------------------------------- */

static void test_env(void)
{
	CHECK(stage_db_to_lin(0.0f) == 1.0f, "0 dB");
	CHECK(NEAR(stage_db_to_lin(-6.0206f), 0.5, 1e-5), "-6 dB");
	CHECK(NEAR(stage_db_to_lin(20.0f), 10.0, 1e-4), "+20 dB");
	CHECK(stage_db_to_lin(-INFINITY) == 0.0f && stage_db_to_lin(NAN) == 0.0f, "-inf/NaN");
	CHECK(stage_db_to_lin(-120.0f) == 0.0f, "below floor");
	CHECK(stage_lin_to_db(1.0f) == 0.0f, "lin 1");
	CHECK(NEAR(stage_lin_to_db(0.5f), -6.0206, 1e-3), "lin 0.5");
	CHECK(stage_lin_to_db(0.0f) == STAGE_DB_FLOOR && stage_lin_to_db(-1.0f) == STAGE_DB_FLOOR &&
		      stage_lin_to_db(1e-12f) == STAGE_DB_FLOOR,
	      "floor");
	CHECK(stage_smooth_coef(0.0f, 0.01f) == 1.0f && stage_smooth_coef(50.0f, 0.0f) == 0.0f,
	      "coef edges");

	/* Frame-rate independence of the follower: 60 Hz vs 240 Hz vs exact. */
	struct stage_env a = {0.0f}, b = {0.0f};
	for (int i = 0; i < 12; i++)
		stage_env_step(&a, 1.0f, 50.0f, 300.0f, 1.0f / 60.0f);
	for (int i = 0; i < 48; i++)
		stage_env_step(&b, 1.0f, 50.0f, 300.0f, 1.0f / 240.0f);
	double exact = 1.0 - exp(-200.0 / 50.0);
	CHECK(NEAR(a.value, exact, 1e-4) && NEAR(b.value, exact, 1e-4), "attack %g %g vs %g",
	      a.value, b.value, exact);
	/* Release is slower than attack. */
	struct stage_env r = {1.0f};
	stage_env_step(&r, 0.0f, 50.0f, 300.0f, 0.1f);
	CHECK(NEAR(r.value, exp(-100.0 / 300.0), 1e-4), "release %g", r.value);
	struct stage_env up = {0.0f};
	stage_env_step(&up, 1.0f, 50.0f, 300.0f, 0.1f);
	CHECK(up.value > 1.0f - r.value, "attack faster than release");
	struct stage_env inst = {0.2f};
	CHECK(stage_env_step(&inst, 0.7f, 0.0f, 0.0f, 0.01f) == 0.7f, "zero times = instant");
	CHECK(stage_env_step(&inst, NAN, 10.0f, 10.0f, 0.01f) < 0.7f, "NaN input treated as 0");

	/* Kick detector at ~47 Hz volmeter rate. */
	struct stage_kick_cfg cfg = STAGE_KICK_CFG_DEFAULT_INIT;
	struct stage_kick k;
	stage_kick_init(&k);
	const float dt = 1024.0f / 48000.0f;
	CHECK(!stage_kick_step(&k, &cfg, 1.0f, dt), "first sample never kicks");
	stage_kick_init(&k);
	int steady_kicks = 0;
	for (int i = 0; i < 200; i++)
		steady_kicks += stage_kick_step(&k, &cfg, 0.01f, dt);
	CHECK(steady_kicks == 0, "steady signal: %d kicks", steady_kicks);
	CHECK(k.since_ms <= cfg.refractory_ms, "since_ms saturates: %g", k.since_ms);
	CHECK(stage_kick_step(&k, &cfg, 0.05f, dt), "spike kicks");
	CHECK(!stage_kick_step(&k, &cfg, 0.05f, dt), "refractory blocks the next one");
	for (int i = 0; i < 8; i++) /* ~170 ms of steady signal */
		stage_kick_step(&k, &cfg, 0.01f, dt);
	CHECK(stage_kick_step(&k, &cfg, 0.05f, dt), "kicks again after refractory");
	/* Quiet input under the gate never kicks. */
	struct stage_kick q;
	stage_kick_init(&q);
	int quiet = 0;
	for (int i = 0; i < 100; i++)
		quiet += stage_kick_step(&q, &cfg, (i % 10 == 0) ? 5e-5f : 1e-7f, dt);
	CHECK(quiet == 0, "gate: %d kicks", quiet);
	CHECK(stage_kick_ratio_from_sensitivity(0.0f) == 2.5f &&
		      NEAR(stage_kick_ratio_from_sensitivity(0.5f), 1.8, 1e-6) &&
		      NEAR(stage_kick_ratio_from_sensitivity(1.0f), 1.1, 1e-6) &&
		      NEAR(stage_kick_ratio_from_sensitivity(NAN), 1.8, 1e-6),
	      "sensitivity mapping");
}

int main(int argc, char **argv)
{
	g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
	RUN(test_ease);
	RUN(test_spring);
	RUN(test_noise);
	RUN(test_solver_basics);
	RUN(test_solver_roundtrip);
	RUN(test_ui_depth_and_dof);
	RUN(test_coverage);
	RUN(test_move);
	RUN(test_move_absolute);
	RUN(test_env);
	printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
