/*
 * stage-spring.h — critically damped springs (frame-rate independent).
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Pure C17, no libobs. Each step applies the exact closed-form solution of
 *   x'' = -omega^2 (x - target) - 2 omega x'
 * over dt (the "smooth damp" form, see Ryan Juckett, "Damped Springs"), so
 * the trajectory is the same whether it is advanced at 30, 60 or 144 Hz:
 * the only difference is float rounding.
 */
#ifndef STAGE_SPRING_H
#define STAGE_SPRING_H

#ifdef __cplusplus
extern "C" {
#endif

/* 1D spring state. x in caller units (px, zoom, ...), v in units/s. */
struct stage_spring {
	float x;
	float v;
};

/* 2D spring state (for example the camera pan x/y in canvas px). */
struct stage_spring2 {
	float x, y;
	float vx, vy;
};

/*
 * Stiffness for a smoothing time: returns omega (rad/s) such that a spring
 * at rest covers 90 % of a step change in `smoothing_s` seconds
 * (omega = 3.88972 / smoothing_s). smoothing_s <= 0 (or NaN) returns 0,
 * which the step functions treat as "snap to target, no smoothing".
 * Example: the UX default Smoothing 120 ms -> omega ~= 32.4 rad/s.
 */
float stage_spring_omega(float smoothing_s);

/*
 * Advances a 1D critically damped spring toward `target`.
 *   s      : state, updated in place (x and v).
 *   target : goal value, same units as s->x.
 *   omega  : angular frequency in rad/s, > 0. Larger = stiffer/faster.
 *            omega <= 0 (or NaN) snaps: x = target, v = 0.
 *   dt     : time step in seconds. dt <= 0 (or NaN) leaves the state as is.
 *            Any dt is stable (exact solution, no overshoot from rest).
 */
void stage_spring_step(struct stage_spring *s, float target, float omega, float dt);

/* 2D variant of stage_spring_step (independent axes, same omega and dt). */
void stage_spring2_step(struct stage_spring2 *s, float tx, float ty, float omega, float dt);

/*
 * Angle variant (degrees). The spring always takes the shortest way round:
 * from 170 to -170 it travels +20, not -340. s->x is kept wrapped to
 * [-180, 180) after the step; s->v is in deg/s. Same omega/dt rules as
 * stage_spring_step.
 */
void stage_spring_angle_step(struct stage_spring *s, float target_deg, float omega, float dt);

/* Wraps an angle in degrees to [-180, 180). NaN/inf return 0. */
float stage_wrap_deg(float deg);

/*
 * Rate limiter (Live-safe speed caps): moves `current` toward `desired` by
 * at most max_rate * dt.
 *   max_rate : units per second, >= 0. max_rate < 0 (or NaN) means no limit.
 *   dt       : seconds; dt <= 0 returns current unchanged.
 * Example: zoom with max_rate 0.5 (STAGE_LIVE_SAFE_MAX_ZOOM_SPEED).
 */
float stage_rate_limit(float current, float desired, float max_rate, float dt);

#ifdef __cplusplus
}
#endif

#endif /* STAGE_SPRING_H */
