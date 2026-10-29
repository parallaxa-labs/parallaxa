/*
 * fx_params_test.c — unit tests for src/fx/stage-fx-params.c (no libobs):
 * stage-plugin-ux.md 2.1 unit conversion, neutral detection, tweens and id cross-fades,
 * FX toggle hotkeys, pulse decay, the 3-flashes-per-second cap and the
 * performance guard.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "stage-fx-params.h"

static int failures;

#define CHECK(cond)                                                                   \
	do {                                                                          \
		if (!(cond)) {                                                        \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
				#cond);                                               \
			failures++;                                                   \
		}                                                                     \
	} while (0)

#define NEAR(a, b, eps) CHECK(fabs((double)(a) - (double)(b)) <= (eps))

static void test_table(void)
{
	/* Every protocol param maps to its UX settings key and scale. */
	static const struct {
		const char *name, *key;
		double scale, lo, hi, neutral;
	} want[] = {
		{"exposure", "exposure", 1, -2, 2, 0},
		{"contrast", "contrast_pct", 100, -1, 1, 0},
		{"saturation", "saturation_pct", 100, -1, 1, 0},
		{"temperature", "temperature_pct", 100, -1, 1, 0},
		{"tint", "tint_pct", 100, -1, 1, 0},
		{"vignette", "vignette_pct", 100, 0, 1, 0},
		{"vignette_soft", "vignette_soft_pct", 100, 0, 1, 0.5},
		{"vignette_round", "vignette_round_pct", 100, 0, 1, 1},
		{"chroma", "chroma_pct", 100, 0, 1, 0},
		{"grain", "grain_pct", 100, 0, 1, 0},
		{"grain_size", "grain_size", 1, 0.5, 4, 1},
		{"lut_amount", "lut_amount_pct", 100, 0, 1, 0},
		{"leak_amount", "leak_amount_pct", 100, 0, 1, 0},
		{"leak_speed", "leak_speed_pct", 100, 0, 2, 0.2},
		{"bloom", "bloom_pct", 100, 0, 1, 0},
		{"bloom_threshold", "bloom_threshold_pct", 100, 0, 1, 0.8},
	};
	_Static_assert(sizeof(want) / sizeof(want[0]) == STFX_NUM_COUNT, "table size");
	for (size_t k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
		int i = stfx_num_find(want[k].name);
		CHECK(i >= 0);
		if (i < 0)
			continue;
		CHECK(!strcmp(stfx_num_defs[i].key, want[k].key));
		NEAR(stfx_num_defs[i].scale, want[k].scale, 0);
		NEAR(stfx_num_defs[i].lo, want[k].lo, 0);
		NEAR(stfx_num_defs[i].hi, want[k].hi, 0);
		NEAR(stfx_num_defs[i].neutral, want[k].neutral, 0);
	}
	CHECK(stfx_num_find("lut") < 0 && stfx_num_find("nope") < 0 && stfx_num_find(NULL) < 0);

	/* Units round-trip: vignette 0.3 <-> 30 %. */
	int v = stfx_num_find("vignette");
	NEAR(stfx_setting_from_param(v, 0.3), 30.0, 1e-9);
	NEAR(stfx_param_from_setting(v, 30.0), 0.3, 1e-12);
	int e = stfx_num_find("exposure");
	NEAR(stfx_setting_from_param(e, -1.25), -1.25, 0);
	NEAR(stfx_num_clamp(e, 5.0), 2.0, 0);
	NEAR(stfx_num_clamp(e, NAN), 0.0, 0);

	/* Every checkable group has an amount param in that group. */
	for (int g = 0; g < STFX_GROUP_COUNT; g++) {
		const struct stfx_group_def *d = &stfx_group_defs[g];
		if (!d->enabled_key) {
			CHECK(g == STFX_GROUP_COLOUR);
			continue;
		}
		CHECK(d->amount >= 0 && stfx_num_defs[d->amount].group == g);
	}
}

static void test_neutral(void)
{
	struct stfx_params p;
	stfx_params_neutral(&p);
	CHECK(stfx_params_is_neutral(&p));

	struct stfx_uniforms u;
	stfx_uniforms_compute(&p, NULL, 0, true, true, &u);
	CHECK(stfx_uniforms_neutral(&u));
	CHECK(u.exposure_mul == 1.0f && u.wb_gain[0] == 1.0f && u.wb_gain[1] == 1.0f &&
	      u.wb_gain[2] == 1.0f);
	CHECK(stfx_uniforms_parts(&u) == 0);

	/* Dependent params alone never make the filter render. */
	p.v[STFX_VIGNETTE_SOFT] = 0.9f;
	p.v[STFX_GRAIN_SIZE] = 3.0f;
	p.v[STFX_BLOOM_THRESHOLD] = 0.2f;
	strcpy(p.lut, "film"); /* lut_amount 0 */
	CHECK(stfx_params_is_neutral(&p));
	stfx_uniforms_compute(&p, NULL, 0, true, true, &u);
	CHECK(stfx_uniforms_neutral(&u));

	/* An amount, a grade or a pulse does. */
	struct stfx_params q = p;
	q.v[STFX_LUT_AMOUNT] = 0.5f;
	CHECK(!stfx_params_is_neutral(&q));
	stfx_uniforms_compute(&q, NULL, 0, true, true, &u);
	CHECK(!stfx_uniforms_neutral(&u) && u.lut_amount == 0.5f);
	/* ...unless the LUT texture is missing or the LUT is paused. */
	stfx_uniforms_compute(&q, NULL, 0, false, true, &u);
	CHECK(stfx_uniforms_neutral(&u));
	stfx_uniforms_compute(&q, NULL, STFX_PART_BIT(STFX_PART_LUT), true, true, &u);
	CHECK(stfx_uniforms_neutral(&u));

	q = p;
	q.v[STFX_TEMPERATURE] = 0.25f;
	stfx_uniforms_compute(&q, NULL, 0, true, true, &u);
	CHECK(!stfx_uniforms_neutral(&u));
	CHECK(u.wb_gain[0] > 1.0f && u.wb_gain[2] < 1.0f);
	/* Luminance of the gains stays 1. */
	NEAR(0.2126 * u.wb_gain[0] + 0.7152 * u.wb_gain[1] + 0.0722 * u.wb_gain[2], 1.0, 1e-5);

	float pulses[STAGE_PULSE_COUNT] = {0.5f, 0.0f, 0.0f};
	stfx_uniforms_compute(&p, pulses, 0, true, true, &u);
	CHECK(!stfx_uniforms_neutral(&u));
	NEAR(u.flash, 0.5f * STFX_FLASH_GAIN, 1e-6);

	/* Paused parts are off; parts() lists what renders. */
	q = p;
	q.v[STFX_BLOOM] = 0.3f;
	q.v[STFX_GRAIN] = 0.1f;
	stfx_uniforms_compute(&q, NULL, 0, true, true, &u);
	CHECK(stfx_uniforms_parts(&u) ==
	      (STFX_PART_BIT(STFX_PART_BLOOM) | STFX_PART_BIT(STFX_PART_GRAIN)));
	stfx_uniforms_compute(&q, NULL, STFX_PART_BIT(STFX_PART_BLOOM), true, true, &u);
	CHECK(u.bloom == 0.0f && u.grain > 0.0f);
	CHECK(!strcmp(stfx_part_name(STFX_PART_BLOOM), "bloom"));
	CHECK(!strcmp(stfx_part_name(STFX_PART_CHROMA), "chroma"));
}

static void test_equal(void)
{
	struct stfx_params a, b;
	stfx_params_neutral(&a);
	b = a;
	a.v[STFX_VIGNETTE] = 0.0f;
	a.v[STFX_VIGNETTE_SOFT] = 0.6f; /* invisible while the amount is 0 */
	CHECK(stfx_params_equal(&a, &b));
	a.v[STFX_VIGNETTE] = 0.25f;
	CHECK(!stfx_params_equal(&a, &b));
	b = a;
	CHECK(stfx_params_equal(&a, &b));
	strcpy(a.lut, "film");
	a.v[STFX_LUT_AMOUNT] = 0.5f;
	CHECK(!stfx_params_equal(&a, &b));
}

/* FX toggle hotkeys: each press flips the target the one before it left,
 * so N presses within one FX tick end where N presses across ticks do. */
static void test_toggle(void)
{
	struct stfx_params n, p;
	stfx_params_neutral(&n);
	p = n;
	p.v[STFX_BLOOM] = 0.4f;
	float restore[STFX_TOGGLE_COUNT] = {0};

	stfx_toggle_apply(&p, 0, restore, 0.0f);
	CHECK(p.v[STFX_BLOOM] == 0.0f && restore[0] == 0.4f);
	stfx_toggle_apply(&p, 0, restore, 0.0f);
	CHECK(p.v[STFX_BLOOM] == 0.4f); /* back on at its own amount */

	/* 1..6 presses in one tick alternate off / on (never off, off, off). */
	for (int k = 1; k <= 6; k++) {
		struct stfx_params q = n;
		q.v[STFX_VIGNETTE] = 0.25f;
		float r[STFX_TOGGLE_COUNT] = {0};
		for (int i = 0; i < k; i++)
			stfx_toggle_apply(&q, 1, r, 0.0f);
		NEAR(q.v[STFX_VIGNETTE], k % 2 ? 0.0 : 0.25, 0);
	}

	/* Nothing to restore: the look's own amount, else the fallback. */
	float none[STFX_TOGGLE_COUNT] = {0};
	p = n;
	stfx_toggle_apply(&p, 2, none, 0.8f);
	CHECK(p.v[STFX_LUT_AMOUNT] == 0.8f);
	p = n;
	stfx_toggle_apply(&p, 3, none, 0.0f);
	CHECK(p.v[STFX_LEAK_AMOUNT] == stfx_toggle_defs[3].fallback);
	/* Other params and out-of-range toggles are left alone. */
	CHECK(p.v[STFX_LUT_AMOUNT] == 0.0f && p.v[STFX_BLOOM] == 0.0f);
	struct stfx_params before = p;
	stfx_toggle_apply(&p, -1, none, 0.0f);
	stfx_toggle_apply(&p, STFX_TOGGLE_COUNT, none, 0.0f);
	CHECK(stfx_params_equal(&p, &before));
	CHECK(stfx_toggle_defs[0].num == STFX_BLOOM && stfx_toggle_defs[1].num == STFX_VIGNETTE &&
	      stfx_toggle_defs[2].num == STFX_LUT_AMOUNT && stfx_toggle_defs[3].num == STFX_LEAK_AMOUNT);
}

static void tick_for(struct stfx_anim *a, double seconds, double dt)
{
	for (double t = 0; t < seconds - 1e-9; t += dt)
		stfx_anim_tick(a, (float)dt);
}

static void test_tween(void)
{
	struct stfx_params n, target;
	stfx_params_neutral(&n);
	target = n;
	target.v[STFX_VIGNETTE] = 0.4f;
	target.v[STFX_EXPOSURE] = 1.0f;

	struct stfx_anim a;
	stfx_anim_init(&a, &n);
	stfx_anim_set(&a, &target, 1000, STAGE_EASE_LINEAR);
	CHECK(a.tweening);
	tick_for(&a, 0.5, 0.01);
	NEAR(a.cur.v[STFX_VIGNETTE], 0.2, 1e-3);
	NEAR(a.cur.v[STFX_EXPOSURE], 0.5, 1e-3);
	tick_for(&a, 0.6, 0.01);
	CHECK(!a.tweening);
	CHECK(stfx_params_equal(&a.cur, &target));

	/* duration 0 jumps. */
	stfx_anim_set(&a, &n, 0, STAGE_EASE_LINEAR);
	CHECK(!a.tweening && stfx_params_is_neutral(&a.cur));

	/* Overshooting eases stay inside the protocol range. */
	target = n;
	target.v[STFX_VIGNETTE] = 1.0f;
	stfx_anim_set(&a, &target, 1000, STAGE_EASE_OUT_ELASTIC);
	for (int i = 0; i < 100; i++) {
		stfx_anim_tick(&a, 0.01f);
		CHECK(a.cur.v[STFX_VIGNETTE] >= 0.0f && a.cur.v[STFX_VIGNETTE] <= 1.0f);
	}

	/* LUT change cross-fades: old LUT out over the first half, new in. */
	struct stfx_params l1 = n, l2 = n;
	strcpy(l1.lut, "film");
	l1.v[STFX_LUT_AMOUNT] = 0.8f;
	strcpy(l2.lut, "night");
	l2.v[STFX_LUT_AMOUNT] = 0.6f;
	stfx_anim_init(&a, &l1);
	stfx_anim_set(&a, &l2, 1000, STAGE_EASE_LINEAR);
	tick_for(&a, 0.25, 0.01);
	CHECK(!strcmp(a.cur.lut, "film"));
	NEAR(a.cur.v[STFX_LUT_AMOUNT], 0.4, 0.02);
	tick_for(&a, 0.5, 0.01);
	CHECK(!strcmp(a.cur.lut, "night"));
	NEAR(a.cur.v[STFX_LUT_AMOUNT], 0.3, 0.02);
	tick_for(&a, 0.3, 0.01);
	CHECK(!strcmp(a.cur.lut, "night") && a.cur.v[STFX_LUT_AMOUNT] == 0.6f);

	/* From no LUT: the new one fades in from 0 over the whole tween. */
	stfx_anim_init(&a, &n);
	stfx_anim_set(&a, &l2, 1000, STAGE_EASE_LINEAR);
	CHECK(!strcmp(a.cur.lut, "night") && a.cur.v[STFX_LUT_AMOUNT] == 0.0f);
	tick_for(&a, 0.5, 0.01);
	NEAR(a.cur.v[STFX_LUT_AMOUNT], 0.3, 0.02);
}

static void test_pulse_decay(void)
{
	struct stfx_params n;
	stfx_params_neutral(&n);
	struct stfx_anim a;
	stfx_anim_init(&a, &n);
	float pv[STAGE_PULSE_COUNT];

	CHECK(stfx_anim_pulse(&a, STAGE_PULSE_FLASH, 1.0f, 250));
	stfx_anim_pulse_values(&a, pv);
	NEAR(pv[STAGE_PULSE_FLASH], 1.0, 1e-6);
	float prev = pv[STAGE_PULSE_FLASH];
	/* Monotone decay, still > 0 just before 250 ms, exactly 0 at 250 ms. */
	for (int i = 1; i <= 24; i++) {
		stfx_anim_tick(&a, 0.01f);
		stfx_anim_pulse_values(&a, pv);
		CHECK(pv[STAGE_PULSE_FLASH] <= prev);
		prev = pv[STAGE_PULSE_FLASH];
	}
	CHECK(prev > 0.0f);
	stfx_anim_tick(&a, 0.0101f);
	stfx_anim_pulse_values(&a, pv);
	CHECK(pv[STAGE_PULSE_FLASH] == 0.0f);
	CHECK(!stfx_anim_pulses_active(&a));

	/* Vignette pulse rises first, then decays to 0 in duration. */
	CHECK(stfx_anim_pulse(&a, STAGE_PULSE_VIGNETTE_PULSE, 0.8f, 400));
	stfx_anim_pulse_values(&a, pv);
	CHECK(pv[STAGE_PULSE_VIGNETTE_PULSE] == 0.0f);
	tick_for(&a, 0.06, 0.01);
	stfx_anim_pulse_values(&a, pv);
	NEAR(pv[STAGE_PULSE_VIGNETTE_PULSE], 0.8, 0.02);
	tick_for(&a, 0.35, 0.01);
	stfx_anim_pulse_values(&a, pv);
	CHECK(pv[STAGE_PULSE_VIGNETTE_PULSE] == 0.0f);

	/* Durations are clamped to 50..2000 ms; 0 means the 300 ms default. */
	stfx_anim_pulse(&a, STAGE_PULSE_CHROMA_KICK, 1.0f, 0);
	NEAR(a.pulse[STAGE_PULSE_CHROMA_KICK].dur, 0.3, 1e-6);
	stfx_anim_pulse(&a, STAGE_PULSE_CHROMA_KICK, 1.0f, 10);
	NEAR(a.pulse[STAGE_PULSE_CHROMA_KICK].dur, 0.05, 1e-6);
	stfx_anim_pulse(&a, STAGE_PULSE_CHROMA_KICK, 1.0f, 99999);
	NEAR(a.pulse[STAGE_PULSE_CHROMA_KICK].dur, 2.0, 1e-6);
	/* Intensity 0 / NaN shows nothing. */
	stfx_anim_cancel_pulses(&a);
	stfx_anim_pulse(&a, STAGE_PULSE_FLASH, 0.0f, 300);
	stfx_anim_pulse(&a, STAGE_PULSE_FLASH, NAN, 300);
	CHECK(!stfx_anim_pulses_active(&a));
}

/* Counts rising edges of the flash envelope sampled every dt: with the cap
 * at most 3 may start in any 1 s window, however fast pulses arrive. */
static void test_flash_cap(void)
{
	struct stfx_params n;
	stfx_params_neutral(&n);
	struct stfx_anim a;
	stfx_anim_init(&a, &n);

	const double dt = 1.0 / 60.0;
	double edges[256];
	int n_edges = 0, merged = 0;
	float prev = 0.0f;
	for (int frame = 0; frame < 60 * 5; frame++) {
		/* A pulse request every 3 frames (20 per second), varying strength. */
		if (frame % 3 == 0) {
			float in = (frame % 2) ? 1.0f : 0.6f;
			if (!stfx_anim_pulse(&a, STAGE_PULSE_FLASH, in, 200))
				merged++;
		}
		float pv[STAGE_PULSE_COUNT];
		stfx_anim_pulse_values(&a, pv);
		if (pv[STAGE_PULSE_FLASH] > prev + 1e-6f && n_edges < 256)
			edges[n_edges++] = frame * dt;
		prev = pv[STAGE_PULSE_FLASH];
		CHECK(stfx_anim_flashes_in_window(&a) <= STAGE_FLASH_MAX_PER_SEC);
		stfx_anim_tick(&a, (float)dt);
	}
	CHECK(merged > 0);
	CHECK(a.flash_merged == (uint64_t)merged);
	CHECK(n_edges >= 3);
	for (int i = 0; i + STAGE_FLASH_MAX_PER_SEC < n_edges; i++) {
		/* The 4th edge after edge i is at least one second later. */
		CHECK(edges[i + STAGE_FLASH_MAX_PER_SEC] - edges[i] >= 1.0 - 1e-6);
	}
	/* Other pulses are not capped. */
	for (int i = 0; i < 10; i++)
		CHECK(stfx_anim_pulse(&a, STAGE_PULSE_VIGNETTE_PULSE, 1.0f, 300));
}

static void test_guard(void)
{
	struct stfx_guard g;
	stfx_guard_init(&g, STAGE_PERF_BUDGET_MS, STAGE_PERF_WINDOW_S);
	unsigned paused = 0;
	const unsigned active = STFX_PART_BIT(STFX_PART_BLOOM) | STFX_PART_BIT(STFX_PART_GRAIN) |
				STFX_PART_BIT(STFX_PART_LUT);
	double cost = 0;

	/* Under budget for 20 s: nothing paused. */
	for (int f = 0; f < 60 * 20; f++) {
		stfx_guard_render(&g, true);
		stfx_guard_sample(&g, 1.2, 1.2);
		CHECK(stfx_guard_tick(&g, 1.0f / 60, active, &paused, &cost) < 0);
	}
	CHECK(paused == 0);
	NEAR(g.last_cost_ms, 1.2, 1e-6);

	/* 2.6 ms per frame: bloom is paused after 5 s, then LUT 5 s later. */
	int first = -1, second = -1;
	double t_first = 0, t_second = 0;
	for (int f = 0; f < 60 * 12; f++) {
		stfx_guard_render(&g, true);
		stfx_guard_sample(&g, 2.6, 2.6);
		int p = stfx_guard_tick(&g, 1.0f / 60, active & ~paused, &paused, &cost);
		if (p >= 0 && first < 0) {
			first = p;
			t_first = f / 60.0;
		} else if (p >= 0 && second < 0) {
			second = p;
			t_second = f / 60.0;
		}
	}
	CHECK(first == STFX_PART_BLOOM);
	CHECK(second == STFX_PART_LUT);
	NEAR(t_first, 5.0, 0.1);
	NEAR(t_second, 10.0, 0.1);
	NEAR(cost, 2.6, 1e-6);
	CHECK(paused == (STFX_PART_BIT(STFX_PART_BLOOM) | STFX_PART_BIT(STFX_PART_LUT)));

	/* Two renders per frame (preview + program) count twice; a partial
	 * sample set is scaled up to all renders. */
	stfx_guard_reset(&g);
	paused = 0;
	first = -1;
	for (int f = 0; f < 60 * 6; f++) {
		stfx_guard_render(&g, true);
		stfx_guard_render(&g, true);
		if (f % 2)
			stfx_guard_sample(&g, 1.1, 1.1);
		int p = stfx_guard_tick(&g, 1.0f / 60, active, &paused, &cost);
		if (p >= 0 && first < 0)
			first = p;
	}
	CHECK(first == STFX_PART_BLOOM);
	NEAR(cost, 2.2, 1e-6);

	/* Bloom with a second render per frame that reuses the cached bloom:
	 * 1.6 ms full render of which 0.3 ms is the final pass -> 1.6 + 0.3 =
	 * 1.9 ms per frame (under the 2 ms budget; not 2 x 1.6 = 3.2 ms). */
	stfx_guard_reset(&g);
	paused = 0;
	for (int f = 0; f < 60 * 6; f++) {
		stfx_guard_render(&g, true);
		stfx_guard_render(&g, false);
		if (f % 4 == 0)
			stfx_guard_sample(&g, 1.6, 0.3);
		CHECK(stfx_guard_tick(&g, 1.0f / 60, active, &paused, &cost) < 0);
	}
	CHECK(paused == 0);
	NEAR(g.last_cost_ms, 1.9, 1e-6);
	/* A pass_ms above ms is clamped (no cached part). */
	stfx_guard_reset(&g);
	for (int f = 0; f < 70; f++) {
		stfx_guard_render(&g, true);
		stfx_guard_render(&g, false);
		stfx_guard_sample(&g, 1.0, 3.0);
		stfx_guard_tick(&g, 1.0f / 60, active, &paused, &cost);
	}
	NEAR(g.last_cost_ms, 2.0, 1e-6);

	/* Nothing active: over budget pauses nothing. */
	stfx_guard_reset(&g);
	paused = 0;
	for (int f = 0; f < 60 * 6; f++) {
		stfx_guard_render(&g, true);
		stfx_guard_sample(&g, 5.0, 5.0);
		CHECK(stfx_guard_tick(&g, 1.0f / 60, 0, &paused, &cost) < 0);
	}
	CHECK(paused == 0);
}

int main(void)
{
	test_table();
	test_neutral();
	test_equal();
	test_toggle();
	test_tween();
	test_pulse_decay();
	test_flash_cap();
	test_guard();
	if (failures) {
		fprintf(stderr, "fx_params_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("fx_params_test: OK\n");
	return 0;
}
