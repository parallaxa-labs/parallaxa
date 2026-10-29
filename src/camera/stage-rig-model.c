/*
 * stage-rig-model.c — Camera Rig motion model (see stage-rig-model.h).
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "stage-rig-model.h"

#include <float.h>
#include <math.h>
#include <string.h>

#include "stage-ease.h"
#include "stage-noise.h"

/* ------------------------------------------------------------------------- */
/* Tuning (at intensity 1, motion scale 100 %)                               */
/* ------------------------------------------------------------------------- */

/* Auto drift amplitudes: pan as a fraction of the canvas, zoom delta, deg. */
#define DRIFT_AMP_X 0.025f
#define DRIFT_AMP_Y 0.018f
#define DRIFT_AMP_ZOOM 0.035f
#define DRIFT_AMP_ROT 0.8f
/* Drift frequencies (noise lattice units per second ~ wiggles per second). */
static const float drift_hz[4] = {0.09f, 0.07f, 0.05f, 0.06f};

/* Chatting: quicker, smaller drift plus a speech-driven push and nod. */
#define CHAT_HZ_MUL 1.6f
#define CHAT_AMP_MUL 0.7f
#define CHAT_TALK_ZOOM 0.05f
#define CHAT_TALK_NOD 0.006f /* fraction of canvas height */

/* Singing: slower sway, more roll, beat (kick) pulses on zoom. */
#define SING_HZ_MUL 0.7f
#define SING_ROT_MUL 1.75f
#define SING_KICK_ZOOM 0.07f
#define KICK_DECAY_S 0.18f

/* Audio bounce: zoom delta at full envelope and amount 100 %. */
#define BOUNCE_ZOOM 0.2f
/* Audio level normalisation: dBFS -> 0..1 over [-45, 0] dB. */
#define AUDIO_FLOOR_DB (-45.0f)

/* Shake: amplitude as a fraction of the canvas height, frequencies. */
#define SHAKE_AMP 0.015f
#define SHAKE_HZ 9.0f
/* "Shake and drift" < 100 %: shake stays below 3 Hz. One noise octave (the
 * upper octaves of an fBm add components at 2x and 4x the base rate) at
 * 2 lattice units per second: gradient noise puts nearly all its power below
 * its lattice rate (camera_model_test measures the spectrum). */
#define SHAKE_HZ_REDUCED 2.0f
#define SHAKE_OCTAVES_REDUCED 1

/* Auto weight blend rates (per second). */
#define AUTO_FADE_OUT 2.0f
#define AUTO_FADE_IN 0.5f

/* Largest step accepted (a stalled OBS must not make the springs jump). */
#define MAX_DT 0.25f

#define NOISE_OCTAVES 3

static inline float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static inline float finite_or(float v, float d)
{
	return isfinite(v) ? v : d;
}

static int ease_or_default(unsigned ease)
{
	return ease < STAGE_EASE_COUNT ? (int)ease : STAGE_EASE_DEFAULT;
}

void stage_rig_params_default(struct stage_rig_params *p)
{
	p->canvas_w = 1920.0f;
	p->canvas_h = 1080.0f;
	p->smoothing_ms = 120.0f;
	p->live_safe = true;
	p->max_zoom = 2.0f;
	p->max_pan = 0.15f;
	p->motion_scale = 1.0f;
	p->manual_pause_s = (float)STAGE_MANUAL_AUTO_PAUSE_S;
	p->audio_amount = 0.25f;
	p->audio_attack_ms = 30.0f;
	p->audio_release_ms = 250.0f;
}

void stage_rig_cam_sanitize(struct stage_cam *c, float canvas_w, float canvas_h)
{
	c->x = clampf(finite_or(c->x, 0.0f), -canvas_w, canvas_w);
	c->y = clampf(finite_or(c->y, 0.0f), -canvas_h, canvas_h);
	c->zoom = clampf(finite_or(c->zoom, 1.0f), (float)STAGE_ZOOM_MIN, (float)STAGE_ZOOM_MAX);
	c->rot = clampf(finite_or(c->rot, 0.0f), (float)STAGE_ROT_MIN, (float)STAGE_ROT_MAX);
}

static const struct stage_cam cam_home = STAGE_CAM_HOME_INIT;

void stage_rig_model_init(struct stage_rig_model *m, const struct stage_rig_params *p)
{
	memset(m, 0, sizeof(*m));
	if (p)
		m->p = *p;
	else
		stage_rig_params_default(&m->p);
	m->target = cam_home;
	m->out = cam_home;
	m->move_pose = cam_home;
	m->auto_mode = STAGE_AUTO_OFF;
	m->last_auto_mode = STAGE_AUTO_DRIFT;
	m->auto_intensity = (float)STAGE_AUTO_INTENSITY_DEFAULT;
	m->focus = (float)STAGE_DEPTH_CHAR;
	m->amount = 0.0f;
	m->follow_char = true;
	stage_kick_init(&m->kick);
	/* Spread the noise phases so the channels are uncorrelated at t = 0. */
	for (int i = 0; i < 6; i++)
		m->ph[i] = 17.0f * (float)(i + 1);
	m->shake_ph = 101.0f;
	/* The camera starts at home, so even the first step obeys the
	 * Live-safe speed caps. */
	m->sp_pan.x = m->sp_pan.y = m->sp_pan.vx = m->sp_pan.vy = 0.0f;
	m->sp_zoom.x = 1.0f;
	m->sp_rot.x = 0.0f;
	m->spring_init = true;
}

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static void pause_auto(struct stage_rig_model *m)
{
	double until = m->now + (double)clampf(m->p.manual_pause_s, 0.0f, 600.0f);
	if (until > m->auto_pause_until)
		m->auto_pause_until = until;
}

/* Current value of the (possibly tweening) target. */
static struct stage_cam tween_value(const struct stage_rig_tween *tw)
{
	if (!tw->active || tw->dur <= 0.0f)
		return tw->to;
	float e = stage_ease_apply(tw->ease, tw->t / tw->dur);
	struct stage_cam c;
	c.x = tw->from.x + (tw->to.x - tw->from.x) * e;
	c.y = tw->from.y + (tw->to.y - tw->from.y) * e;
	c.zoom = tw->from.zoom + (tw->to.zoom - tw->from.zoom) * e;
	c.rot = tw->from.rot + (tw->to.rot - tw->from.rot) * e;
	/* Back/elastic eases overshoot: keep zoom strictly inside its range. */
	c.zoom = clampf(c.zoom, (float)STAGE_ZOOM_MIN, (float)STAGE_ZOOM_MAX);
	return c;
}

/* Stops the running move. When keep_pose is set (a new command interrupts
 * it) a hold:true move leaves its current pose as the target, so the next
 * command starts from what is seen; a hold:false move (shake, punch, nod)
 * leaves the target where it started, as if it had finished. */
static void cancel_move(struct stage_rig_model *m, bool keep_pose)
{
	if (!m->move_active)
		return;
	if (keep_pose && m->move.hold)
		m->target = m->move_pose;
	m->move_active = false;
	m->move_shake = 0.0f;
}

/* Freezes the tween at its current value (a new command starts from it). */
static void settle_tween(struct stage_rig_model *m)
{
	if (m->tween.active) {
		m->target = tween_value(&m->tween);
		m->tween.active = false;
	}
}

static void start_tween(struct stage_rig_model *m, const struct stage_cam *to, uint32_t duration_ms,
			int ease)
{
	struct stage_cam dst = *to;
	stage_rig_cam_sanitize(&dst, m->p.canvas_w, m->p.canvas_h);
	if (duration_ms == 0) {
		m->tween.active = false;
		m->target = dst;
		return;
	}
	m->tween.active = true;
	m->tween.from = m->target;
	m->tween.to = dst;
	m->tween.t = 0.0f;
	m->tween.dur = (float)duration_ms / 1000.0f;
	m->tween.ease = ease;
}

static void snap_springs(struct stage_rig_model *m, const struct stage_cam *c)
{
	m->sp_pan.x = c->x;
	m->sp_pan.y = c->y;
	m->sp_pan.vx = m->sp_pan.vy = 0.0f;
	m->sp_zoom.x = c->zoom;
	m->sp_zoom.v = 0.0f;
	m->sp_rot.x = c->rot;
	m->sp_rot.v = 0.0f;
	m->out = *c;
	m->spring_init = true;
}

static void set_auto_mode(struct stage_rig_model *m, uint8_t mode)
{
	if (mode >= STAGE_AUTO_COUNT)
		return;
	if (mode != m->auto_mode)
		m->changed |= STAGE_RIG_CH_AUTO;
	m->auto_mode = mode;
	if (mode != STAGE_AUTO_OFF)
		m->last_auto_mode = mode;
}

/* ------------------------------------------------------------------------- */
/* Commands                                                                  */
/* ------------------------------------------------------------------------- */

void stage_rig_model_manual(struct stage_rig_model *m)
{
	pause_auto(m);
}

bool stage_rig_model_move(struct stage_rig_model *m, const struct stage_move_def *def,
			  uint64_t cmd_id, uint32_t duration_ms, float intensity, unsigned ease)
{
	if (!def || m->frozen)
		return false;
	/* A new move replaces the running one and starts from what is seen. */
	cancel_move(m, true);
	settle_tween(m);
	m->punch_held = false;

	m->move = *def;
	m->move_cmd_id = cmd_id;
	m->move_done = false;
	m->move_start = m->target;
	m->move_pose = m->target;
	m->move_t_ms = 0.0f;
	m->move_dur_ms = duration_ms ? duration_ms : def->duration_ms;
	m->move_intensity = (isfinite(intensity) && intensity >= 0.0f) ? intensity : 1.0f;
	m->move_ease = ease < STAGE_EASE_COUNT ? (int)ease : -1;
	m->move_shake = 0.0f;
	/* relative:false: the move eases from the current target (minus home,
	 * in key units) to its absolute offsets (stage-move.h). */
	m->move_anchor.dx = m->target.x / m->p.canvas_w;
	m->move_anchor.dy = m->target.y / m->p.canvas_h;
	m->move_anchor.dzoom = m->target.zoom - 1.0f;
	m->move_anchor.drot = stage_wrap_deg(m->target.rot);
	m->move_anchor.shake = 0.0f;
	m->move_active = true;
	m->view = 0;
	pause_auto(m);
	return true;
}

void stage_rig_model_set(struct stage_rig_model *m, const struct stage_cam_target *t,
			 uint32_t duration_ms, unsigned ease)
{
	if (!t)
		return;
	cancel_move(m, true);
	settle_tween(m);
	m->punch_held = false;
	struct stage_cam dst = m->target;
	if (t->relative) {
		if (t->mask & STAGE_CAM_X)
			dst.x += finite_or(t->x, 0.0f);
		if (t->mask & STAGE_CAM_Y)
			dst.y += finite_or(t->y, 0.0f);
		if (t->mask & STAGE_CAM_ZOOM)
			dst.zoom += finite_or(t->zoom, 0.0f);
		if (t->mask & STAGE_CAM_ROT)
			dst.rot += finite_or(t->rot, 0.0f);
	} else {
		if (t->mask & STAGE_CAM_X)
			dst.x = finite_or(t->x, dst.x);
		if (t->mask & STAGE_CAM_Y)
			dst.y = finite_or(t->y, dst.y);
		if (t->mask & STAGE_CAM_ZOOM)
			dst.zoom = finite_or(t->zoom, dst.zoom);
		if (t->mask & STAGE_CAM_ROT)
			dst.rot = finite_or(t->rot, dst.rot);
	}
	start_tween(m, &dst, duration_ms, ease_or_default(ease));
	m->view = 0;
	pause_auto(m);
}

void stage_rig_model_reset(struct stage_rig_model *m, uint32_t duration_ms, unsigned ease)
{
	cancel_move(m, true);
	settle_tween(m);
	m->punch_held = false;
	start_tween(m, &cam_home, duration_ms, ease_or_default(ease));
	m->view = 0;
	pause_auto(m);
}

void stage_rig_model_auto(struct stage_rig_model *m, unsigned mode, float intensity)
{
	if (isfinite(intensity) && intensity >= 0.0f) {
		float v = clampf(intensity, 0.0f, 1.0f);
		if (v != m->auto_intensity)
			m->changed |= STAGE_RIG_CH_AUTO;
		m->auto_intensity = v;
	}
	if (mode == STAGE_CAM_AUTO_TOGGLE) {
		set_auto_mode(m, m->auto_mode == STAGE_AUTO_OFF ? m->last_auto_mode
								 : (uint8_t)STAGE_AUTO_OFF);
	} else if (mode == STAGE_CAM_AUTO_NEXT) {
		set_auto_mode(m, (uint8_t)((m->auto_mode + 1u) % STAGE_AUTO_COUNT));
	} else if (mode < STAGE_AUTO_COUNT) {
		set_auto_mode(m, (uint8_t)mode);
	}
}

void stage_rig_model_freeze(struct stage_rig_model *m, unsigned mode)
{
	bool on = mode == STAGE_FREEZE_ON || (mode == STAGE_FREEZE_TOGGLE && !m->frozen);
	if (on == m->frozen)
		return;
	m->changed |= STAGE_RIG_CH_FREEZE;
	m->frozen = on;
	if (on) {
		/* Hold what is seen (without shake and bounce): it becomes the
		 * target, moves stop, auto and bounce drop out at once. */
		if (m->spring_init) {
			m->target.x = m->sp_pan.x;
			m->target.y = m->sp_pan.y;
			m->target.zoom = m->sp_zoom.x;
			m->target.rot = m->sp_rot.x;
			stage_rig_cam_sanitize(&m->target, m->p.canvas_w, m->p.canvas_h);
		}
		m->tween.active = false;
		m->move_active = false;
		m->move_shake = 0.0f;
		m->punch_held = false;
		m->auto_weight = 0.0f;
		m->kick_pulse = 0.0f;
	}
}

void stage_rig_model_view_apply(struct stage_rig_model *m, uint8_t slot, const struct stage_cam *pose,
				unsigned transition, uint32_t duration_ms)
{
	if (!pose)
		return;
	cancel_move(m, true);
	settle_tween(m);
	m->punch_held = false;
	if (transition >= STAGE_TRANSITION_COUNT)
		transition = STAGE_TRANSITION_SMOOTH;
	bool cut = transition == STAGE_TRANSITION_CUT || duration_ms == 0;
	start_tween(m, pose, cut ? 0 : duration_ms, stage_transition_ease[transition]);
	if (cut)
		snap_springs(m, &m->target);
	m->view = slot;
	pause_auto(m);
}

uint8_t stage_rig_model_view_step(const struct stage_rig_model *m, int dir, unsigned filled)
{
	const int n = STAGE_VIEW_SLOT_MAX;
	int cur = m->view >= STAGE_VIEW_SLOT_MIN && m->view <= n ? m->view : 0;
	dir = dir < 0 ? -1 : 1;
	for (int k = 1; k <= n; k++) {
		int slot = cur == 0 ? (dir > 0 ? k : n + 1 - k) : ((cur - 1 + dir * k) % n + n) % n + 1;
		if (filled & (1u << (slot - 1)))
			return (uint8_t)slot;
	}
	return 0;
}

static void ftween_start(struct stage_rig_ftween *tw, float from, float to, uint32_t duration_ms,
			 int ease)
{
	tw->active = duration_ms > 0;
	tw->from = from;
	tw->to = to;
	tw->t = 0.0f;
	tw->dur = (float)duration_ms / 1000.0f;
	tw->ease = ease;
}

static float ftween_target(const struct stage_rig_ftween *tw, float cur)
{
	return tw->active ? tw->to : cur;
}

static float sane_depth(float d, float dflt)
{
	return isfinite(d) ? stage_depth_clamp(d) : dflt;
}

void stage_rig_model_dof(struct stage_rig_model *m, unsigned mask, float focus, float amount,
			 uint32_t duration_ms, unsigned ease, float char_depth, float far_bg_depth,
			 float dof_slider)
{
	int e = ease_or_default(ease);
	float cd = sane_depth(char_depth, (float)STAGE_DEPTH_CHAR);
	bool rack = (mask & (STAGE_CAM_DOF_RACK_CHAR | STAGE_CAM_DOF_RACK_BG)) != 0;
	float new_focus = ftween_target(&m->focus_tw, m->focus);
	float new_amount = ftween_target(&m->amount_tw, m->amount);
	bool set_focus = false, set_amount = false;

	if (mask & STAGE_CAM_DOF_RACK_CHAR) {
		new_focus = cd;
		set_focus = true;
	} else if (mask & STAGE_CAM_DOF_RACK_BG) {
		new_focus = sane_depth(far_bg_depth, 3.0f);
		set_focus = true;
	} else if ((mask & STAGE_DOF_FOCUS) && isfinite(focus)) {
		new_focus = stage_depth_clamp(focus);
		set_focus = true;
	}
	if (rack) {
		if (duration_ms == 0)
			duration_ms = STAGE_CAM_RACK_MS;
		/* With depth of field off, a rack focus turns it on at the
		 * panel's Blur amount. */
		if (new_amount <= 0.0f) {
			float s = isfinite(dof_slider) ? clampf(dof_slider, 0.0f, 1.0f) : 0.3f;
			new_amount = s > 0.0f ? s : 0.3f;
			set_amount = true;
		}
	} else if ((mask & STAGE_DOF_AMOUNT) && isfinite(amount)) {
		new_amount = clampf(amount, 0.0f, 1.0f);
		set_amount = true;
	}

	if (set_focus) {
		/* Focus on the character keeps (or turns) "follow" on; anything
		 * else turns it off, so rack focus works with default settings. */
		bool follow = fabsf(new_focus - cd) <= 0.01f;
		if (mask & STAGE_CAM_DOF_RACK_CHAR)
			follow = true;
		if (follow != m->follow_char)
			m->changed |= STAGE_RIG_CH_DOF;
		m->follow_char = follow;
		ftween_start(&m->focus_tw, m->focus, new_focus, duration_ms, e);
		if (!m->focus_tw.active && m->focus != new_focus) {
			m->focus = new_focus;
			m->changed |= STAGE_RIG_CH_DOF;
		}
	}
	if (set_amount) {
		ftween_start(&m->amount_tw, m->amount, new_amount, duration_ms, e);
		if (!m->amount_tw.active && m->amount != new_amount) {
			m->amount = new_amount;
			m->changed |= STAGE_RIG_CH_DOF;
		}
	}
}

void stage_rig_model_panic(struct stage_rig_model *m)
{
	cancel_move(m, true);
	settle_tween(m);
	m->punch_held = false;
	if (m->frozen) {
		m->frozen = false;
		m->changed |= STAGE_RIG_CH_FREEZE;
	}
	set_auto_mode(m, STAGE_AUTO_OFF);
	start_tween(m, &cam_home, STAGE_CAM_PANIC_MS, STAGE_EASE_IN_OUT_CUBIC);
	m->view = 0;
	m->focus_tw.active = m->amount_tw.active = false;
	if (m->amount != 0.0f || !m->follow_char)
		m->changed |= STAGE_RIG_CH_DOF;
	m->amount = 0.0f;
	m->follow_char = true;
	m->kick_pulse = 0.0f;
}

void stage_rig_model_snap_home(struct stage_rig_model *m)
{
	cancel_move(m, false);
	m->tween.active = false;
	m->punch_held = false;
	m->target = cam_home;
	m->auto_off = (struct stage_cam){0.0f, 0.0f, 0.0f, 0.0f};
	m->auto_weight = 0.0f;
	m->kick_pulse = 0.0f;
	m->env.value = 0.0f;
	m->bounce = 0.0f;
	m->view = 0;
	snap_springs(m, &cam_home);
}

void stage_rig_model_punch(struct stage_rig_model *m, bool press)
{
	if (press) {
		if (m->punch_held || m->frozen)
			return;
		cancel_move(m, true);
		settle_tween(m);
		m->punch_before = m->target;
		struct stage_cam dst = m->target;
		dst.zoom += STAGE_CAM_PUNCH_ZOOM;
		start_tween(m, &dst, STAGE_CAM_PUNCH_IN_MS, STAGE_EASE_OUT_BACK);
		m->punch_held = true;
		m->view = 0;
		pause_auto(m);
	} else if (m->punch_held) {
		cancel_move(m, true);
		settle_tween(m);
		start_tween(m, &m->punch_before, STAGE_CAM_PUNCH_OUT_MS, STAGE_EASE_DEFAULT);
		m->punch_held = false;
		pause_auto(m);
	}
}

/* ------------------------------------------------------------------------- */
/* Step                                                                      */
/* ------------------------------------------------------------------------- */

static float fbm(uint32_t seed, float x)
{
	return stage_noise_fbm(seed, x, NOISE_OCTAVES, 2.0f, 0.5f);
}

/* Auto camera offset for this step (x/y px, zoom delta, rot deg), already
 * scaled by intensity, motion scale and the auto weight. */
static struct stage_cam auto_offset(struct stage_rig_model *m, float dt, float env)
{
	struct stage_cam o = {0.0f, 0.0f, 0.0f, 0.0f};
	float hz_mul = 1.0f, amp_mul = 1.0f, rot_mul = 1.0f;
	switch (m->auto_mode) {
	case STAGE_AUTO_CHATTING:
		hz_mul = CHAT_HZ_MUL;
		amp_mul = CHAT_AMP_MUL;
		break;
	case STAGE_AUTO_SINGING:
		hz_mul = SING_HZ_MUL;
		rot_mul = SING_ROT_MUL;
		break;
	default:
		break;
	}
	/* Phases advance even while paused so a resumed drift is continuous
	 * with the weight fade. */
	for (int i = 0; i < 4; i++)
		m->ph[i] = stage_noise_advance(m->ph[i], drift_hz[i] * hz_mul, dt);

	float amp = m->auto_intensity * clampf(m->p.motion_scale, 0.0f, 1.0f) * m->auto_weight;
	if (amp <= 0.0f)
		return o;
	o.x = amp * amp_mul * DRIFT_AMP_X * m->p.canvas_w * fbm(1u, m->ph[0]);
	o.y = amp * amp_mul * DRIFT_AMP_Y * m->p.canvas_h * fbm(2u, m->ph[1]);
	/* Drift zooms in only (0..amp), so it never reveals frame edges. */
	o.zoom = amp * amp_mul * DRIFT_AMP_ZOOM * (0.5f + 0.5f * fbm(3u, m->ph[2]));
	o.rot = amp * rot_mul * DRIFT_AMP_ROT * fbm(4u, m->ph[3]);

	float react = m->auto_intensity * m->auto_weight;
	if (m->auto_mode == STAGE_AUTO_CHATTING) {
		o.zoom += react * CHAT_TALK_ZOOM * env;
		o.y -= react * CHAT_TALK_NOD * m->p.canvas_h * env;
	} else if (m->auto_mode == STAGE_AUTO_SINGING) {
		o.zoom += react * SING_KICK_ZOOM * m->kick_pulse;
	}
	return o;
}

/* Advances a depth-of-field tween. The change flag (panel sync) is raised
 * when the tween lands, not on every intermediate value. */
static void step_ftween(struct stage_rig_ftween *tw, float *v, float dt, uint32_t *changed)
{
	if (!tw->active)
		return;
	tw->t += dt;
	float u = tw->dur > 0.0f ? tw->t / tw->dur : 1.0f;
	if (u >= 1.0f) {
		*v = tw->to;
		tw->active = false;
		*changed |= STAGE_RIG_CH_DOF;
	} else {
		*v = tw->from + (tw->to - tw->from) * stage_ease_apply(tw->ease, u);
	}
}

static void step_dof(struct stage_rig_model *m, const struct stage_rig_inputs *in, float dt)
{
	step_ftween(&m->focus_tw, &m->focus, dt, &m->changed);
	step_ftween(&m->amount_tw, &m->amount, dt, &m->changed);
	m->focus = stage_depth_clamp(m->focus);
	m->amount = clampf(m->amount, 0.0f, 1.0f);
	if (m->follow_char && !m->focus_tw.active) {
		float cd = sane_depth(in->char_depth, (float)STAGE_DEPTH_CHAR);
		if (fabsf(m->focus - cd) > 1e-4f) {
			m->focus = cd;
			m->changed |= STAGE_RIG_CH_DOF;
		}
	}
}

/* Covering layers that cover the canvas at rest (others would force a
 * large zoom: a small "bg" sticker must not zoom the whole camera). */
static size_t usable_covers(const struct stage_rig_model *m, const struct stage_rig_inputs *in,
			    struct stage_rect *rects, float *fs)
{
	size_t n = 0;
	for (size_t i = 0; i < in->ncovers && n < STAGE_COVER_MAX_LAYERS; i++) {
		const struct stage_rig_cover *c = &in->covers[i];
		if (!(c->f > 0.0f))
			continue;
		if (stage_cover_margin(&c->rect, c->f, &cam_home, m->p.canvas_w, m->p.canvas_h) < -0.5f)
			continue;
		rects[n] = c->rect;
		fs[n] = c->f;
		n++;
	}
	return n;
}

void stage_rig_model_step(struct stage_rig_model *m, const struct stage_rig_inputs *in)
{
	float dt = in ? in->dt : 0.0f;
	if (!isfinite(dt) || dt < 0.0f)
		dt = 0.0f;
	if (dt > MAX_DT)
		dt = MAX_DT;
	m->now += dt;
	const float W = m->p.canvas_w > 0.0f ? m->p.canvas_w : 1920.0f;
	const float H = m->p.canvas_h > 0.0f ? m->p.canvas_h : 1080.0f;
	m->p.canvas_w = W;
	m->p.canvas_h = H;

	/* 1. Target tween. */
	if (m->tween.active) {
		m->tween.t += dt;
		if (m->tween.t >= m->tween.dur) {
			m->target = m->tween.to;
			m->tween.active = false;
		} else {
			m->target = tween_value(&m->tween);
		}
	}

	/* 2. Move. */
	struct stage_cam base = m->target;
	if (m->move_active) {
		m->move_t_ms += dt * 1000.0f;
		struct stage_move_delta d;
		bool done = stage_move_eval_ex(&m->move, m->move_t_ms, m->move_dur_ms,
					       m->move_intensity, m->move_ease,
					       m->move.relative ? NULL : &m->move_anchor, &d);
		struct stage_cam pose;
		if (m->move.relative) {
			pose.x = m->move_start.x + d.dx * W;
			pose.y = m->move_start.y + d.dy * H;
			pose.zoom = m->move_start.zoom + d.dzoom;
			pose.rot = m->move_start.rot + d.drot;
		} else {
			pose.x = d.dx * W;
			pose.y = d.dy * H;
			pose.zoom = 1.0f + d.dzoom;
			pose.rot = d.drot;
		}
		stage_rig_cam_sanitize(&pose, W, H);
		m->move_pose = pose;
		m->move_shake = clampf(finite_or(d.shake, 0.0f), 0.0f, 1.0f);
		base = pose;
		if (done) {
			m->move_active = false;
			m->move_done = true;
			m->move_shake = 0.0f;
			if (m->move.hold) {
				/* The end pose becomes the target, still within the
				 * Max zoom / Max pan that limited the move. */
				float zl = fmaxf(clampf(m->p.max_zoom, 1.0f, (float)STAGE_ZOOM_MAX),
						 m->move_start.zoom);
				float px = fmaxf(clampf(m->p.max_pan, 0.0f, 1.0f) * W, fabsf(m->move_start.x));
				float py = fmaxf(clampf(m->p.max_pan, 0.0f, 1.0f) * H, fabsf(m->move_start.y));
				pose.zoom = fminf(pose.zoom, zl);
				pose.x = clampf(pose.x, -px, px);
				pose.y = clampf(pose.y, -py, py);
				m->target = pose;
			} /* else the spring returns to the start */
		}
	}

	/* 3. Audio envelope and beats. */
	float env_in = 0.0f;
	bool have_audio = isfinite(in ? in->audio_db : NAN);
	if (have_audio)
		env_in = clampf((in->audio_db - AUDIO_FLOOR_DB) / -AUDIO_FLOOR_DB, 0.0f, 1.0f);
	float env = stage_env_step(&m->env, env_in, m->p.audio_attack_ms, m->p.audio_release_ms, dt);
	m->kick_pulse *= expf(-dt / KICK_DECAY_S);
	if (have_audio) {
		static const struct stage_kick_cfg kcfg = STAGE_KICK_CFG_DEFAULT_INIT;
		float lin = stage_db_to_lin(in->audio_db);
		if (stage_kick_step(&m->kick, &kcfg, lin * lin, dt))
			m->kick_pulse = 1.0f;
	}

	/* 4. Auto camera (blended by its weight; manual input pauses it). */
	bool auto_on = m->auto_mode != STAGE_AUTO_OFF && !m->frozen && m->now >= m->auto_pause_until;
	if (auto_on)
		m->auto_weight = fminf(1.0f, m->auto_weight + AUTO_FADE_IN * dt);
	else
		m->auto_weight = fmaxf(0.0f, m->auto_weight - AUTO_FADE_OUT * dt);
	if (m->frozen)
		m->auto_weight = 0.0f;
	m->auto_off = auto_offset(m, dt, env);
	base.x += m->auto_off.x;
	base.y += m->auto_off.y;
	base.zoom += m->auto_off.zoom;
	base.rot += m->auto_off.rot;

	/* 5. Max zoom / max pan for auto, moves and bounce; an explicit target
	 * may go beyond them (vendor-api 4.4 keeps the protocol range). */
	struct stage_cam tgt;
	stage_rig_model_target(m, &tgt);
	float zoom_lim = fmaxf(clampf(m->p.max_zoom, 1.0f, (float)STAGE_ZOOM_MAX), tgt.zoom);
	float pan_x = fmaxf(clampf(m->p.max_pan, 0.0f, 1.0f) * W, fabsf(tgt.x));
	float pan_y = fmaxf(clampf(m->p.max_pan, 0.0f, 1.0f) * H, fabsf(tgt.y));
	base.zoom = clampf(base.zoom, (float)STAGE_ZOOM_MIN, zoom_lim);
	base.x = clampf(base.x, -pan_x, pan_x);
	base.y = clampf(base.y, -pan_y, pan_y);
	stage_rig_cam_sanitize(&base, W, H);
	if (m->p.live_safe)
		base.rot = clampf(base.rot, -(float)STAGE_LIVE_SAFE_MAX_ROLL,
				  (float)STAGE_LIVE_SAFE_MAX_ROLL);

	/* 6. Spring smoothing. */
	struct stage_cam prev = m->out;
	bool first = !m->spring_init;
	if (first) {
		snap_springs(m, &base);
	} else {
		float omega = stage_spring_omega(m->p.smoothing_ms / 1000.0f);
		stage_spring2_step(&m->sp_pan, base.x, base.y, omega, dt);
		stage_spring_step(&m->sp_zoom, base.zoom, omega, dt);
		stage_spring_angle_step(&m->sp_rot, base.rot, omega, dt);
	}

	/* 7. Shake (x/y noise) and audio bounce (zoom) on top of the spring. */
	float ms = clampf(m->p.motion_scale, 0.0f, 1.0f);
	float shake = m->frozen ? 0.0f : m->move_shake * ms;
	bool reduced = ms < 1.0f;
	m->shake_ph = stage_noise_advance(m->shake_ph, reduced ? SHAKE_HZ_REDUCED : SHAKE_HZ, dt);
	float sx = 0.0f, sy = 0.0f;
	if (shake > 0.0f) {
		int oct = reduced ? SHAKE_OCTAVES_REDUCED : NOISE_OCTAVES;
		sx = shake * SHAKE_AMP * H * stage_noise_fbm(11u, m->shake_ph, oct, 2.0f, 0.5f);
		sy = shake * SHAKE_AMP * H * stage_noise_fbm(12u, m->shake_ph + 57.0f, oct, 2.0f, 0.5f);
	}
	m->bounce = (have_audio && !m->frozen)
			    ? clampf(m->p.audio_amount, 0.0f, 1.0f) * BOUNCE_ZOOM * env
			    : 0.0f;

	struct stage_cam out;
	out.x = m->sp_pan.x + sx;
	out.y = m->sp_pan.y + sy;
	out.zoom = m->sp_zoom.x + m->bounce;
	out.rot = m->sp_rot.x;
	out.zoom = fminf(out.zoom, fmaxf(zoom_lim, m->sp_zoom.x));

	/* 8. Live-safe: speed caps, roll cap, edge coverage. */
	if (m->p.live_safe) {
		if (!first) {
			out.zoom = stage_rate_limit(prev.zoom, out.zoom,
						    (float)STAGE_LIVE_SAFE_MAX_ZOOM_SPEED, dt);
			out.rot = stage_rate_limit(prev.rot, out.rot,
						   (float)STAGE_LIVE_SAFE_MAX_ROT_SPEED, dt);
		}
		out.rot = clampf(out.rot, -(float)STAGE_LIVE_SAFE_MAX_ROLL,
				 (float)STAGE_LIVE_SAFE_MAX_ROLL);
	}
	stage_rig_cam_sanitize(&out, W, H);
	if (m->p.live_safe && in && in->covers && in->ncovers) {
		struct stage_rect rects[STAGE_COVER_MAX_LAYERS];
		float fs[STAGE_COVER_MAX_LAYERS];
		size_t n = usable_covers(m, in, rects, fs);
		if (n)
			stage_cover_clamp(rects, fs, n, &out, W, H, &out);
	}
	m->out = out;
	m->out_shake = shake;

	/* 9. Depth of field. */
	if (in)
		step_dof(m, in, dt);
}

void stage_rig_model_target(const struct stage_rig_model *m, struct stage_cam *out)
{
	*out = m->tween.active ? m->tween.to : m->target;
}

bool stage_rig_model_moving(const struct stage_rig_model *m)
{
	return m->move_active || m->tween.active;
}
