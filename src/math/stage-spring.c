/*
 * stage-spring.c — critically damped springs.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "stage-spring.h"

#include <math.h>

/* u solving (1 + u) e^-u = 0.1: a critically damped spring from rest has
 * covered 90 % of the step after t = u / omega. */
#define SPRING_U90 3.88972016f

float stage_spring_omega(float smoothing_s)
{
	if (!(smoothing_s > 0.0f))
		return 0.0f;
	return SPRING_U90 / smoothing_s;
}

/*
 * Exact solution of the critically damped oscillator. With d = x0 - g:
 *   x(t) = g + (d + (v0 + w d) t) e^{-w t}
 *   v(t) = (v0 - w (v0 + w d) t) e^{-w t}
 * Evaluated in double so that many small steps match one big step.
 */
static void spring_axis(float *x, float *v, float target, float omega, float dt)
{
	double w = omega;
	double d = (double)*x - (double)target;
	double t = dt;
	double k = (double)*v + w * d;
	double e = exp(-w * t);
	*x = (float)((double)target + (d + k * t) * e);
	*v = (float)(((double)*v - w * k * t) * e);
}

/* Shared argument policy. Returns 1 when the caller should integrate. */
static int spring_policy(float omega, float dt)
{
	if (!(dt > 0.0f))
		return 0;
	if (!(omega > 0.0f))
		return -1; /* snap */
	return 1;
}

void stage_spring_step(struct stage_spring *s, float target, float omega, float dt)
{
	int p = spring_policy(omega, dt);
	if (p == 0)
		return;
	if (p < 0) {
		s->x = target;
		s->v = 0.0f;
		return;
	}
	spring_axis(&s->x, &s->v, target, omega, dt);
}

void stage_spring2_step(struct stage_spring2 *s, float tx, float ty, float omega, float dt)
{
	int p = spring_policy(omega, dt);
	if (p == 0)
		return;
	if (p < 0) {
		s->x = tx;
		s->y = ty;
		s->vx = 0.0f;
		s->vy = 0.0f;
		return;
	}
	spring_axis(&s->x, &s->vx, tx, omega, dt);
	spring_axis(&s->y, &s->vy, ty, omega, dt);
}

float stage_wrap_deg(float deg)
{
	if (!isfinite(deg))
		return 0.0f;
	double r = fmod((double)deg + 180.0, 360.0);
	if (r < 0.0)
		r += 360.0;
	float out = (float)(r - 180.0);
	/* Rounding can land exactly on +180; keep the half-open range. */
	return out >= 180.0f ? -180.0f : out;
}

void stage_spring_angle_step(struct stage_spring *s, float target_deg, float omega, float dt)
{
	float x = stage_wrap_deg(s->x);
	/* Equivalent target nearest to x: x + shortest signed difference. */
	float goal = x + stage_wrap_deg(target_deg - x);
	s->x = x;
	stage_spring_step(s, goal, omega, dt);
	s->x = stage_wrap_deg(s->x);
}

float stage_rate_limit(float current, float desired, float max_rate, float dt)
{
	if (!(dt > 0.0f))
		return current;
	if (!(max_rate >= 0.0f))
		return desired;
	float step = max_rate * dt;
	float diff = desired - current;
	if (diff > step)
		return current + step;
	if (diff < -step)
		return current - step;
	return desired;
}
