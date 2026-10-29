/*
 * stage-fx-params.c — Parallaxa FX parameter model, animator, uniforms and
 * performance guard (pure C, no libobs). See stage-fx-params.h.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "stage-fx-params.h"

#include <math.h>
#include <string.h>

#include "stage-ease.h"

/* ------------------------------------------------------------------------- */
/* Tables (docs/ux/stage-plugin-ux.md 2.1, vendor-api.md 4.7)                */
/* ------------------------------------------------------------------------- */

const struct stfx_num_def stfx_num_defs[STFX_NUM_COUNT] = {
	[STFX_EXPOSURE] = {"exposure", "exposure", 1.0, -2.0, 2.0, 0.0, STFX_GROUP_COLOUR},
	[STFX_CONTRAST] = {"contrast", "contrast_pct", 100.0, -1.0, 1.0, 0.0, STFX_GROUP_COLOUR},
	[STFX_SATURATION] = {"saturation", "saturation_pct", 100.0, -1.0, 1.0, 0.0, STFX_GROUP_COLOUR},
	[STFX_TEMPERATURE] = {"temperature", "temperature_pct", 100.0, -1.0, 1.0, 0.0, STFX_GROUP_COLOUR},
	[STFX_TINT] = {"tint", "tint_pct", 100.0, -1.0, 1.0, 0.0, STFX_GROUP_COLOUR},
	[STFX_VIGNETTE] = {"vignette", "vignette_pct", 100.0, 0.0, 1.0, 0.0, STFX_GROUP_VIGNETTE},
	[STFX_VIGNETTE_SOFT] = {"vignette_soft", "vignette_soft_pct", 100.0, 0.0, 1.0, 0.5,
				STFX_GROUP_VIGNETTE},
	[STFX_VIGNETTE_ROUND] = {"vignette_round", "vignette_round_pct", 100.0, 0.0, 1.0, 1.0,
				 STFX_GROUP_VIGNETTE},
	[STFX_CHROMA] = {"chroma", "chroma_pct", 100.0, 0.0, 1.0, 0.0, STFX_GROUP_CHROMA},
	[STFX_GRAIN] = {"grain", "grain_pct", 100.0, 0.0, 1.0, 0.0, STFX_GROUP_GRAIN},
	[STFX_GRAIN_SIZE] = {"grain_size", "grain_size", 1.0, 0.5, 4.0, 1.0, STFX_GROUP_GRAIN},
	[STFX_LUT_AMOUNT] = {"lut_amount", "lut_amount_pct", 100.0, 0.0, 1.0, 0.0, STFX_GROUP_LUT},
	[STFX_LEAK_AMOUNT] = {"leak_amount", "leak_amount_pct", 100.0, 0.0, 1.0, 0.0, STFX_GROUP_LEAK},
	[STFX_LEAK_SPEED] = {"leak_speed", "leak_speed_pct", 100.0, 0.0, 2.0, 0.2, STFX_GROUP_LEAK},
	[STFX_BLOOM] = {"bloom", "bloom_pct", 100.0, 0.0, 1.0, 0.0, STFX_GROUP_BLOOM},
	[STFX_BLOOM_THRESHOLD] = {"bloom_threshold", "bloom_threshold_pct", 100.0, 0.0, 1.0, 0.8,
				  STFX_GROUP_BLOOM},
};

const struct stfx_group_def stfx_group_defs[STFX_GROUP_COUNT] = {
	[STFX_GROUP_COLOUR] = {NULL, -1},
	[STFX_GROUP_LUT] = {"lut_enabled", STFX_LUT_AMOUNT},
	[STFX_GROUP_VIGNETTE] = {"vignette_enabled", STFX_VIGNETTE},
	[STFX_GROUP_BLOOM] = {"bloom_enabled", STFX_BLOOM},
	[STFX_GROUP_CHROMA] = {"chroma_enabled", STFX_CHROMA},
	[STFX_GROUP_GRAIN] = {"grain_enabled", STFX_GRAIN},
	[STFX_GROUP_LEAK] = {"leak_enabled", STFX_LEAK_AMOUNT},
};

int stfx_num_find(const char *name)
{
	if (!name)
		return -1;
	for (int i = 0; i < STFX_NUM_COUNT; i++) {
		if (strcmp(stfx_num_defs[i].name, name) == 0)
			return i;
	}
	return -1;
}

double stfx_num_clamp(int i, double v)
{
	const struct stfx_num_def *d = &stfx_num_defs[i];
	if (v != v)
		return d->neutral;
	return v < d->lo ? d->lo : (v > d->hi ? d->hi : v);
}

double stfx_setting_from_param(int i, double param)
{
	return param * stfx_num_defs[i].scale;
}

double stfx_param_from_setting(int i, double setting)
{
	return setting / stfx_num_defs[i].scale;
}

/* ------------------------------------------------------------------------- */
/* Params                                                                    */
/* ------------------------------------------------------------------------- */

void stfx_params_neutral(struct stfx_params *p)
{
	memset(p, 0, sizeof(*p));
	for (int i = 0; i < STFX_NUM_COUNT; i++)
		p->v[i] = (float)stfx_num_defs[i].neutral;
}

static bool group_off(const struct stfx_params *p, int g)
{
	int amount = stfx_group_defs[g].amount;
	if (amount < 0)
		return false;
	if (!(p->v[amount] > 0.0f))
		return true;
	if (g == STFX_GROUP_LUT && !p->lut[0])
		return true;
	if (g == STFX_GROUP_LEAK && !p->leak[0])
		return true;
	return false;
}

void stfx_params_canon(struct stfx_params *p)
{
	for (int g = 0; g < STFX_GROUP_COUNT; g++) {
		if (!group_off(p, g))
			continue;
		for (int i = 0; i < STFX_NUM_COUNT; i++) {
			if (stfx_num_defs[i].group == g)
				p->v[i] = (float)stfx_num_defs[i].neutral;
		}
		if (g == STFX_GROUP_LUT)
			p->lut[0] = '\0';
		if (g == STFX_GROUP_LEAK)
			p->leak[0] = '\0';
	}
}

bool stfx_params_equal(const struct stfx_params *a, const struct stfx_params *b)
{
	struct stfx_params ca = *a, cb = *b;
	stfx_params_canon(&ca);
	stfx_params_canon(&cb);
	for (int i = 0; i < STFX_NUM_COUNT; i++) {
		if (fabsf(ca.v[i] - cb.v[i]) > 1e-4f)
			return false;
	}
	return strcmp(ca.lut, cb.lut) == 0 && strcmp(ca.leak, cb.leak) == 0;
}

bool stfx_params_is_neutral(const struct stfx_params *p)
{
	struct stfx_params n;
	stfx_params_neutral(&n);
	return stfx_params_equal(p, &n);
}

const struct stfx_toggle_def stfx_toggle_defs[STFX_TOGGLE_COUNT] = {
	{STFX_BLOOM, 0.30f},
	{STFX_VIGNETTE, 0.30f},
	{STFX_LUT_AMOUNT, 0.60f},
	{STFX_LEAK_AMOUNT, 0.50f},
};

void stfx_toggle_apply(struct stfx_params *p, int t, float restore[STFX_TOGGLE_COUNT],
		       float preset_amount)
{
	if (t < 0 || t >= STFX_TOGGLE_COUNT)
		return;
	float *v = &p->v[stfx_toggle_defs[t].num];
	if (*v > 0.0005f) {
		restore[t] = *v;
		*v = 0.0f;
	} else {
		*v = restore[t] > 0.0f ? restore[t]
		     : preset_amount > 0.0f ? preset_amount
					    : stfx_toggle_defs[t].fallback;
	}
}

/* ------------------------------------------------------------------------- */
/* Animator                                                                  */
/* ------------------------------------------------------------------------- */

void stfx_anim_init(struct stfx_anim *a, const struct stfx_params *p)
{
	memset(a, 0, sizeof(*a));
	a->from = a->to = a->cur = *p;
	for (int i = 0; i < STAGE_FLASH_MAX_PER_SEC; i++)
		a->flash_start[i] = -1e9;
	a->last_merge = -1.0;
}

void stfx_anim_jump(struct stfx_anim *a, const struct stfx_params *p)
{
	a->from = a->to = a->cur = *p;
	a->tweening = false;
	a->t = a->dur = 0.0f;
}

static float clamp01(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* Cross-fades an id param (lut / leak) and its amount at eased progress e. */
static void blend_id(const char *from_id, float from_amt, const char *to_id, float to_amt, float e,
		     char out_id[STAGE_ID_MAX], float *out_amt)
{
	const bool from_on = from_id[0] && from_amt > 0.0f;
	const bool to_on = to_id[0] && to_amt > 0.0f;
	if (strcmp(from_id, to_id) == 0 || !from_on) {
		strcpy(out_id, to_id);
		*out_amt = from_on ? from_amt + (to_amt - from_amt) * e : to_amt * clamp01(e);
	} else if (!to_on) {
		strcpy(out_id, from_id);
		*out_amt = from_amt * clamp01(1.0f - e);
	} else if (e < 0.5f) {
		strcpy(out_id, from_id);
		*out_amt = from_amt * clamp01(1.0f - 2.0f * e);
	} else {
		strcpy(out_id, to_id);
		*out_amt = to_amt * clamp01(2.0f * e - 1.0f);
	}
	if (*out_amt < 0.0f)
		*out_amt = 0.0f;
}

static void anim_eval(struct stfx_anim *a)
{
	const float u = a->dur > 0.0f ? a->t / a->dur : 1.0f;
	const float e = stage_ease_apply(a->ease, u);
	for (int i = 0; i < STFX_NUM_COUNT; i++) {
		float v = a->from.v[i] + (a->to.v[i] - a->from.v[i]) * e;
		a->cur.v[i] = (float)stfx_num_clamp(i, v);
	}
	blend_id(a->from.lut, a->from.v[STFX_LUT_AMOUNT], a->to.lut, a->to.v[STFX_LUT_AMOUNT], e,
		 a->cur.lut, &a->cur.v[STFX_LUT_AMOUNT]);
	blend_id(a->from.leak, a->from.v[STFX_LEAK_AMOUNT], a->to.leak, a->to.v[STFX_LEAK_AMOUNT], e,
		 a->cur.leak, &a->cur.v[STFX_LEAK_AMOUNT]);
}

void stfx_anim_set(struct stfx_anim *a, const struct stfx_params *target, uint32_t duration_ms,
		   int ease)
{
	if (duration_ms == 0) {
		stfx_anim_jump(a, target);
		return;
	}
	a->from = a->cur;
	a->to = *target;
	a->t = 0.0f;
	a->dur = (float)duration_ms / 1000.0f;
	a->ease = ease;
	a->tweening = true;
	anim_eval(a);
}

static float pulse_value(const struct stfx_pulse *p, double clock)
{
	if (!p->active || p->dur <= 0.0f)
		return 0.0f;
	float u = (float)((clock - p->start) / p->dur);
	if (u >= 1.0f)
		return 0.0f;
	if (u < 0.0f)
		u = 0.0f;
	if (p->attack) {
		const float k = STFX_VIGNETTE_PULSE_ATTACK;
		if (u < k) {
			float x = u / k;
			return p->peak * x * x * (3.0f - 2.0f * x);
		}
		u = (u - k) / (1.0f - k);
	}
	float d = 1.0f - u;
	return p->peak * d * d;
}

int stfx_anim_flashes_in_window(const struct stfx_anim *a)
{
	int n = 0;
	for (int i = 0; i < STAGE_FLASH_MAX_PER_SEC; i++) {
		double age = a->clock - a->flash_start[i];
		if (age >= 0.0 && age < STFX_FLASH_WINDOW_S)
			n++;
	}
	return n;
}

bool stfx_anim_pulse(struct stfx_anim *a, int effect, float intensity, uint32_t duration_ms)
{
	if (effect < 0 || effect >= STAGE_PULSE_COUNT)
		return true;
	if (!(intensity > 0.0f))
		return true; /* nothing to show; also maps NaN to "no pulse" */
	if (intensity > 1.0f)
		intensity = 1.0f;
	if (duration_ms < STAGE_PULSE_DURATION_MIN)
		duration_ms = duration_ms == 0 ? STFX_PULSE_DURATION_DEFAULT : STAGE_PULSE_DURATION_MIN;
	if (duration_ms > STAGE_PULSE_DURATION_MAX)
		duration_ms = STAGE_PULSE_DURATION_MAX;
	const float dur = (float)duration_ms / 1000.0f;

	struct stfx_pulse *p = &a->pulse[effect];
	const float now_v = pulse_value(p, a->clock);

	if (effect == STAGE_PULSE_FLASH &&
	    stfx_anim_flashes_in_window(a) >= STAGE_FLASH_MAX_PER_SEC) {
		/* Photosensitivity cap: coalesce into the running flash. Restart
		 * its decay from the current value (continuous, still falling)
		 * so no new flash edge appears. */
		if (now_v > 0.0f) {
			float remaining = (float)(p->start + p->dur - a->clock);
			p->peak = now_v;
			p->start = a->clock;
			p->dur = remaining > dur ? remaining : dur;
			p->attack = false;
		}
		a->flash_merged++;
		a->last_merge = a->clock;
		return false;
	}

	if (effect == STAGE_PULSE_FLASH) {
		a->flash_start[a->flash_head] = a->clock;
		a->flash_head = (a->flash_head + 1) % STAGE_FLASH_MAX_PER_SEC;
	}
	p->active = true;
	p->peak = intensity > now_v ? intensity : now_v;
	p->start = a->clock;
	p->dur = dur;
	/* Vignette pulse rises first, unless it is re-triggered while visible
	 * (a dip back to 0 would read as a stutter). */
	p->attack = effect == STAGE_PULSE_VIGNETTE_PULSE && now_v <= 0.0f;
	return true;
}

void stfx_anim_cancel_pulses(struct stfx_anim *a)
{
	for (int i = 0; i < STAGE_PULSE_COUNT; i++)
		a->pulse[i].active = false;
}

void stfx_anim_tick(struct stfx_anim *a, float dt)
{
	if (!(dt > 0.0f))
		return;
	a->clock += dt;
	if (a->tweening) {
		a->t += dt;
		if (a->t >= a->dur) {
			a->cur = a->to;
			a->tweening = false;
		} else {
			anim_eval(a);
		}
	}
	for (int i = 0; i < STAGE_PULSE_COUNT; i++) {
		struct stfx_pulse *p = &a->pulse[i];
		if (p->active && a->clock - p->start >= p->dur)
			p->active = false;
	}
}

void stfx_anim_pulse_values(const struct stfx_anim *a, float out[STAGE_PULSE_COUNT])
{
	for (int i = 0; i < STAGE_PULSE_COUNT; i++)
		out[i] = pulse_value(&a->pulse[i], a->clock);
}

bool stfx_anim_pulses_active(const struct stfx_anim *a)
{
	for (int i = 0; i < STAGE_PULSE_COUNT; i++) {
		if (a->pulse[i].active)
			return true;
	}
	return false;
}

/* ------------------------------------------------------------------------- */
/* Uniforms                                                                  */
/* ------------------------------------------------------------------------- */

static const char *const part_names[STFX_PART_COUNT] = {
	STAGE_PERF_PAUSED_BLOOM, STAGE_PERF_PAUSED_LUT,    STAGE_PERF_PAUSED_LEAK,
	STAGE_PERF_PAUSED_GRAIN, STAGE_PERF_PAUSED_CHROMA,
};

const char *stfx_part_name(int part)
{
	return part >= 0 && part < STFX_PART_COUNT ? part_names[part] : "";
}

void stfx_uniforms_compute(const struct stfx_params *p, const float pulses[STAGE_PULSE_COUNT],
			   unsigned paused_mask, bool has_lut, bool has_leak, struct stfx_uniforms *u)
{
	struct stfx_params c = *p;
	stfx_params_canon(&c);
	const float flash = pulses ? clamp01(pulses[STAGE_PULSE_FLASH]) : 0.0f;
	const float kick = pulses ? clamp01(pulses[STAGE_PULSE_CHROMA_KICK]) : 0.0f;
	const float vpulse = pulses ? clamp01(pulses[STAGE_PULSE_VIGNETTE_PULSE]) : 0.0f;

	memset(u, 0, sizeof(*u));
	u->exposure_mul = exp2f(c.v[STFX_EXPOSURE]);

	/* White balance as linear RGB gains, normalised to keep luminance. */
	const float t = c.v[STFX_TEMPERATURE], m = c.v[STFX_TINT];
	if (t == 0.0f && m == 0.0f) {
		u->wb_gain[0] = u->wb_gain[1] = u->wb_gain[2] = 1.0f;
	} else {
		float r = (1.0f + 0.20f * t) * (1.0f + 0.04f * m);
		float g = 1.0f - 0.15f * m;
		float b = (1.0f - 0.20f * t) * (1.0f + 0.04f * m);
		float k = 1.0f / (0.2126f * r + 0.7152f * g + 0.0722f * b);
		u->wb_gain[0] = r * k;
		u->wb_gain[1] = g * k;
		u->wb_gain[2] = b * k;
	}
	u->contrast = 1.0f + c.v[STFX_CONTRAST];
	u->saturation = 1.0f + c.v[STFX_SATURATION];

	const bool lut_paused = (paused_mask & STFX_PART_BIT(STFX_PART_LUT)) != 0;
	u->lut_amount = has_lut && !lut_paused && c.lut[0] ? c.v[STFX_LUT_AMOUNT] : 0.0f;

	u->vignette = clamp01(c.v[STFX_VIGNETTE] + vpulse * STFX_VIGNETTE_PULSE_GAIN);
	u->vignette_soft = c.v[STFX_VIGNETTE_SOFT];
	u->vignette_round = c.v[STFX_VIGNETTE_ROUND];

	if (!(paused_mask & STFX_PART_BIT(STFX_PART_CHROMA)))
		u->chroma = clamp01(c.v[STFX_CHROMA] + kick * STFX_CHROMA_KICK_GAIN);
	if (!(paused_mask & STFX_PART_BIT(STFX_PART_GRAIN)))
		u->grain = c.v[STFX_GRAIN];
	u->grain_size = c.v[STFX_GRAIN_SIZE];

	const bool leak_paused = (paused_mask & STFX_PART_BIT(STFX_PART_LEAK)) != 0;
	u->leak_amount = has_leak && !leak_paused && c.leak[0] ? c.v[STFX_LEAK_AMOUNT] : 0.0f;
	u->leak_speed = c.v[STFX_LEAK_SPEED];

	u->flash = flash * STFX_FLASH_GAIN;

	if (!(paused_mask & STFX_PART_BIT(STFX_PART_BLOOM)))
		u->bloom = c.v[STFX_BLOOM];
	u->bloom_threshold = c.v[STFX_BLOOM_THRESHOLD];
}

bool stfx_uniforms_neutral(const struct stfx_uniforms *u)
{
	const float eps = 1e-6f;
	if (fabsf(u->exposure_mul - 1.0f) > eps || fabsf(u->contrast - 1.0f) > eps ||
	    fabsf(u->saturation - 1.0f) > eps)
		return false;
	for (int i = 0; i < 3; i++) {
		if (fabsf(u->wb_gain[i] - 1.0f) > eps)
			return false;
	}
	return !(u->lut_amount > 0.0f) && !(u->vignette > 0.0f) && !(u->chroma > 0.0f) &&
	       !(u->grain > 0.0f) && !(u->leak_amount > 0.0f) && !(u->flash > 0.0f) &&
	       !(u->bloom > 0.0f);
}

unsigned stfx_uniforms_parts(const struct stfx_uniforms *u)
{
	unsigned m = 0;
	if (u->bloom > 0.0f)
		m |= STFX_PART_BIT(STFX_PART_BLOOM);
	if (u->lut_amount > 0.0f)
		m |= STFX_PART_BIT(STFX_PART_LUT);
	if (u->leak_amount > 0.0f)
		m |= STFX_PART_BIT(STFX_PART_LEAK);
	if (u->grain > 0.0f)
		m |= STFX_PART_BIT(STFX_PART_GRAIN);
	if (u->chroma > 0.0f)
		m |= STFX_PART_BIT(STFX_PART_CHROMA);
	return m;
}

/* ------------------------------------------------------------------------- */
/* Performance guard                                                         */
/* ------------------------------------------------------------------------- */

void stfx_guard_init(struct stfx_guard *g, double budget_ms, int window_s)
{
	memset(g, 0, sizeof(*g));
	g->budget_ms = budget_ms;
	g->window_s = window_s > 0 ? window_s : 1;
}

void stfx_guard_reset(struct stfx_guard *g)
{
	stfx_guard_init(g, g->budget_ms, g->window_s);
}

void stfx_guard_render(struct stfx_guard *g, bool full)
{
	g->renders++;
	if (full)
		g->full++;
}

void stfx_guard_sample(struct stfx_guard *g, double ms, double pass_ms)
{
	if (ms >= 0.0 && ms < 1000.0) {
		if (pass_ms < 0.0 || pass_ms > ms)
			pass_ms = ms;
		g->sum_ms += ms;
		g->sum_pass_ms += pass_ms;
		g->measured++;
	}
}

int stfx_guard_tick(struct stfx_guard *g, float dt, unsigned active_mask, unsigned *paused_mask,
		    double *cost_ms)
{
	if (dt > 0.0f)
		g->bucket_t += dt;
	g->ticks++;
	if (g->bucket_t < 1.0)
		return -1;

	/* Per-frame cost: the chain part once per full render, the final pass
	 * once per render (see stage-fx-params.h). */
	double cost = 0.0;
	if (g->measured > 0 && g->ticks > 0) {
		const double pass = g->sum_pass_ms / g->measured;
		const double chain = g->sum_ms / g->measured - pass;
		cost = (chain * (double)g->full + pass * (double)g->renders) / (double)g->ticks;
	}
	g->last_cost_ms = cost;
	if (g->measured > 0 && cost > g->budget_ms)
		g->over_s++;
	else
		g->over_s = 0;

	g->bucket_t = g->bucket_t >= 2.0 ? 0.0 : g->bucket_t - 1.0;
	g->sum_ms = g->sum_pass_ms = 0.0;
	g->measured = g->renders = g->full = g->ticks = 0;

	if (g->over_s < g->window_s)
		return -1;
	g->over_s = 0;
	for (int p = 0; p < STFX_PART_COUNT; p++) {
		unsigned bit = STFX_PART_BIT(p);
		if ((active_mask & bit) && !(*paused_mask & bit)) {
			*paused_mask |= bit;
			if (cost_ms)
				*cost_ms = cost;
			return p;
		}
	}
	return -1;
}
