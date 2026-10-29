/*
 * stage-solver.c — layer solver and coverage helpers (see stage-solver.h).
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "stage-solver.h"

#include <float.h>
#include <math.h>

#define DEG2RAD 0.017453292519943295
#define FACTOR_MAX 10.0 /* f = 1 / STAGE_DEPTH_MIN */
#define ROT_SAMPLES 17
#define CLAMP_ROUNDS 32
#define COVER_EPS_PX 0.01
#define CLAMP_EPS 1e-9 /* relative slack of the half-plane tests */

/* ---------------------------------------------------------------------- */
/* Depth, roles and UI mapping                                            */
/* ---------------------------------------------------------------------- */

static double clampd(double v, double lo, double hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

float stage_depth_clamp(float depth)
{
	if (isnan(depth))
		return (float)STAGE_DEPTH_CHAR;
	return (float)clampd(depth, STAGE_DEPTH_MIN, STAGE_DEPTH_MAX);
}

float stage_depth_factor(float depth)
{
	return (float)(1.0 / (double)stage_depth_clamp(depth));
}

float stage_role_factor(int role, float depth)
{
	switch (role) {
	case STAGE_ROLE_BG:
	case STAGE_ROLE_CHAR:
	case STAGE_ROLE_FG:
		return stage_depth_factor(depth);
	default: /* sky, static, none, unknown */
		return 0.0f;
	}
}

float stage_role_default_depth(int role)
{
	switch (role) {
	case STAGE_ROLE_BG:
		return 3.0f;
	case STAGE_ROLE_FG:
		return 0.6f;
	case STAGE_ROLE_SKY:
		return (float)STAGE_DEPTH_MAX;
	default: /* char, static, none, unknown */
		return (float)STAGE_DEPTH_CHAR;
	}
}

float stage_depth_from_ui(float v)
{
	if (isnan(v))
		return (float)STAGE_DEPTH_CHAR;
	/* Saturate first so pow never overflows for huge |v|. */
	double vv = clampd(v, -STAGE_UI_DEPTH_SCALE, STAGE_UI_DEPTH_SCALE);
	return (float)clampd(pow(10.0, -vv / STAGE_UI_DEPTH_SCALE), STAGE_DEPTH_MIN,
			     STAGE_DEPTH_MAX);
}

float stage_ui_from_depth(float depth)
{
	if (isnan(depth))
		return 0.0f;
	double d = clampd(depth, STAGE_DEPTH_MIN, STAGE_DEPTH_MAX);
	double v = -STAGE_UI_DEPTH_SCALE * log10(d);
	return v == 0.0 ? 0.0f : (float)v; /* no -0 */
}

float stage_dof_strength(float depth, float focus_depth, float amount)
{
	if (isnan(depth) || isnan(focus_depth) || isnan(amount))
		return 0.0f;
	double d = clampd(depth, STAGE_DEPTH_MIN, STAGE_DEPTH_MAX);
	double fd = clampd(focus_depth, STAGE_DEPTH_MIN, STAGE_DEPTH_MAX);
	double a = clampd(amount, STAGE_DOF_AMOUNT_MIN, STAGE_DOF_AMOUNT_MAX);
	return (float)clampd(fabs(d - fd) * a, 0.0, 1.0);
}

static double sane_factor(float f)
{
	if (!(f > 0.0f)) /* also NaN */
		return 0.0;
	return f > FACTOR_MAX ? FACTOR_MAX : (double)f;
}

float stage_layer_zoom(float zoom, float f)
{
	double z = isnan(zoom) ? 1.0 : (double)zoom;
	return (float)stage_layer_scale(z, sane_factor(f));
}

/* ---------------------------------------------------------------------- */
/* Solver                                                                 */
/* ---------------------------------------------------------------------- */

/* Per-layer camera terms: rotation, pan offset t = (x, y) f and scale s. */
struct layer_cam {
	double cosr, sinr; /* R(rot f) */
	double tx, ty;
	double s;
	double cx, cy; /* canvas centre */
};

static void layer_cam_init(struct layer_cam *lc, double f, const struct stage_cam *cam,
			   float canvas_w, float canvas_h)
{
	double theta = (double)cam->rot * f * DEG2RAD;
	lc->cosr = cos(theta);
	lc->sinr = sin(theta);
	lc->tx = (double)cam->x * f;
	lc->ty = (double)cam->y * f;
	lc->s = stage_layer_scale(isnan(cam->zoom) ? 1.0 : (double)cam->zoom, f);
	lc->cx = (double)canvas_w * 0.5;
	lc->cy = (double)canvas_h * 0.5;
}

/* Layer (rest) point -> canvas point. */
static void map_fwd(const struct layer_cam *lc, double px, double py, double *ox, double *oy)
{
	double dx = (px - lc->cx) * lc->s, dy = (py - lc->cy) * lc->s;
	*ox = lc->cx + lc->cosr * dx - lc->sinr * dy - lc->tx;
	*oy = lc->cy + lc->sinr * dx + lc->cosr * dy - lc->ty;
}

/* Canvas point -> layer (rest) point. */
static void map_inv(const struct layer_cam *lc, double px, double py, double *ox, double *oy)
{
	double dx = px + lc->tx - lc->cx, dy = py + lc->ty - lc->cy;
	*ox = lc->cx + (lc->cosr * dx + lc->sinr * dy) / lc->s;
	*oy = lc->cy + (-lc->sinr * dx + lc->cosr * dy) / lc->s;
}

void stage_solve_layer_f(const struct stage_xform *rest, float f, const struct stage_cam *cam,
			 float canvas_w, float canvas_h, struct stage_xform *out)
{
	double ff = sane_factor(f);
	struct layer_cam lc;
	layer_cam_init(&lc, ff, cam, canvas_w, canvas_h);
	double ox, oy;
	map_fwd(&lc, rest->px, rest->py, &ox, &oy);
	struct stage_xform r;
	r.px = (float)ox;
	r.py = (float)oy;
	r.sx = (float)((double)rest->sx * lc.s);
	r.sy = (float)((double)rest->sy * lc.s);
	r.rot = (float)((double)rest->rot + (double)cam->rot * ff);
	*out = r;
}

void stage_unsolve_layer_f(const struct stage_xform *cur, float f, const struct stage_cam *cam,
			   float canvas_w, float canvas_h, struct stage_xform *rest_out)
{
	double ff = sane_factor(f);
	struct layer_cam lc;
	layer_cam_init(&lc, ff, cam, canvas_w, canvas_h);
	double ox, oy;
	map_inv(&lc, cur->px, cur->py, &ox, &oy);
	struct stage_xform r;
	r.px = (float)ox;
	r.py = (float)oy;
	r.sx = (float)((double)cur->sx / lc.s);
	r.sy = (float)((double)cur->sy / lc.s);
	r.rot = (float)((double)cur->rot - (double)cam->rot * ff);
	*rest_out = r;
}

void stage_solve_layer(const struct stage_xform *rest, float depth, const struct stage_cam *cam,
		       float canvas_w, float canvas_h, struct stage_xform *out)
{
	stage_solve_layer_f(rest, stage_depth_factor(depth), cam, canvas_w, canvas_h, out);
}

void stage_unsolve_layer(const struct stage_xform *cur, float depth, const struct stage_cam *cam,
			 float canvas_w, float canvas_h, struct stage_xform *rest_out)
{
	stage_unsolve_layer_f(cur, stage_depth_factor(depth), cam, canvas_w, canvas_h, rest_out);
}

/* ---------------------------------------------------------------------- */
/* Edge coverage                                                          */
/* ---------------------------------------------------------------------- */

static bool rect_valid(const struct stage_rect *r)
{
	return r && r->r > r->l && r->b > r->t;
}

/* Canvas corners mapped back into layer space. */
static void canvas_corners_inv(const struct layer_cam *lc, float w, float h, double qx[4],
			       double qy[4])
{
	const double kx[4] = {0.0, (double)w, 0.0, (double)w};
	const double ky[4] = {0.0, 0.0, (double)h, (double)h};
	for (int i = 0; i < 4; i++)
		map_inv(lc, kx[i], ky[i], &qx[i], &qy[i]);
}

float stage_cover_margin(const struct stage_rect *rect, float f, const struct stage_cam *cam,
			 float canvas_w, float canvas_h)
{
	if (!rect_valid(rect))
		return -FLT_MAX;
	struct layer_cam lc;
	layer_cam_init(&lc, sane_factor(f), cam, canvas_w, canvas_h);
	double qx[4], qy[4];
	canvas_corners_inv(&lc, canvas_w, canvas_h, qx, qy);
	double m = DBL_MAX;
	for (int i = 0; i < 4; i++) {
		double d = fmin(fmin(qx[i] - rect->l, rect->r - qx[i]),
				fmin(qy[i] - rect->t, rect->b - qy[i]));
		m = fmin(m, d);
	}
	return (float)(m * lc.s); /* layer px -> screen px */
}

/* Oversize needed for one camera (layer_cam already built). */
static double oversize_for(const struct stage_rect *rect, const struct layer_cam *lc, float w,
			   float h)
{
	double mx = 0.5 * ((double)rect->l + rect->r), my = 0.5 * ((double)rect->t + rect->b);
	double hw = 0.5 * ((double)rect->r - rect->l), hh = 0.5 * ((double)rect->b - rect->t);
	double qx[4], qy[4];
	canvas_corners_inv(lc, w, h, qx, qy);
	double need = 0.0;
	for (int i = 0; i < 4; i++) {
		need = fmax(need, fabs(qx[i] - mx) / hw);
		need = fmax(need, fabs(qy[i] - my) / hh);
	}
	return need > 1.0 ? need - 1.0 : 0.0;
}

float stage_cover_min_oversize(const struct stage_rect *rect, float f, const struct stage_cam *cam,
			       float canvas_w, float canvas_h)
{
	if (!rect_valid(rect))
		return 0.0f;
	struct layer_cam lc;
	layer_cam_init(&lc, sane_factor(f), cam, canvas_w, canvas_h);
	return (float)oversize_for(rect, &lc, canvas_w, canvas_h);
}

float stage_cover_min_oversize_range(const struct stage_rect *rect, float f,
				     const struct stage_cover_range *range, float canvas_w,
				     float canvas_h)
{
	if (!rect_valid(rect) || !range)
		return 0.0f;
	double ff = sane_factor(f);
	float px = fabsf(range->max_pan_x), py = fabsf(range->max_pan_y);
	float rmax = fabsf(range->max_rot);
	float zs[2] = {range->zoom_min, range->zoom_max};
	double worst = 0.0;
	/* |q - centre| is bilinear in (pan, 1/s) per axis, so its maximum over
	 * the pan/zoom box is at a corner; rotation is sampled. */
	for (int iz = 0; iz < 2; iz++)
		for (int ix = -1; ix <= 1; ix += 2)
			for (int iy = -1; iy <= 1; iy += 2)
				for (int ir = 0; ir < ROT_SAMPLES; ir++) {
					struct stage_cam c;
					c.x = px * (float)ix;
					c.y = py * (float)iy;
					c.zoom = zs[iz];
					c.rot = rmax * (2.0f * (float)ir / (ROT_SAMPLES - 1) - 1.0f);
					struct layer_cam lc;
					layer_cam_init(&lc, ff, &c, canvas_w, canvas_h);
					worst = fmax(worst,
						     oversize_for(rect, &lc, canvas_w, canvas_h));
				}
	return (float)worst;
}

/*
 * Pan feasibility in the layer's rotated frame u = R(-theta) (x, y) f.
 * With a_k = R(-theta)(corner_k - C), coverage needs for each axis
 *   s (l - Cx) - min_k a_kx <= u_x <= s (r - Cx) - max_k a_kx
 * (and the same for y with t/b). ext = max a - min a is the rotated canvas
 * extent; the box is non-empty iff s * width >= ext.
 */
struct pan_frame {
	double cosr, sinr;
	double ulo_x, uhi_x, ulo_y, uhi_y; /* in u units (px * f) */
	double ext_x, ext_y;
};

static void pan_frame_init(struct pan_frame *pf, const struct stage_rect *rect, double f,
			   double s, float rot, float w, float h)
{
	double theta = (double)rot * f * DEG2RAD;
	pf->cosr = cos(theta);
	pf->sinr = sin(theta);
	double cx = (double)w * 0.5, cy = (double)h * 0.5;
	double amin_x = DBL_MAX, amax_x = -DBL_MAX, amin_y = DBL_MAX, amax_y = -DBL_MAX;
	const double kx[4] = {-cx, cx, -cx, cx};
	const double ky[4] = {-cy, -cy, cy, cy};
	for (int i = 0; i < 4; i++) {
		double ax = pf->cosr * kx[i] + pf->sinr * ky[i];
		double ay = -pf->sinr * kx[i] + pf->cosr * ky[i];
		amin_x = fmin(amin_x, ax);
		amax_x = fmax(amax_x, ax);
		amin_y = fmin(amin_y, ay);
		amax_y = fmax(amax_y, ay);
	}
	pf->ext_x = amax_x - amin_x;
	pf->ext_y = amax_y - amin_y;
	pf->ulo_x = s * ((double)rect->l - cx) - amin_x;
	pf->uhi_x = s * ((double)rect->r - cx) - amax_x;
	pf->ulo_y = s * ((double)rect->t - cy) - amin_y;
	pf->uhi_y = s * ((double)rect->b - cy) - amax_y;
}

static double cam_zoom(float zoom)
{
	return isnan(zoom) ? 1.0 : clampd(zoom, STAGE_ZOOM_MIN, STAGE_ZOOM_MAX);
}

float stage_cover_min_zoom(const struct stage_rect *rect, float f, float rot, float canvas_w,
			   float canvas_h)
{
	if (!rect_valid(rect))
		return FLT_MAX;
	double ff = sane_factor(f);
	if (ff <= 0.0) {
		struct stage_cam home = STAGE_CAM_HOME_INIT;
		return stage_cover_margin(rect, 0.0f, &home, canvas_w, canvas_h) >= -COVER_EPS_PX
			       ? (float)STAGE_ZOOM_MIN
			       : FLT_MAX;
	}
	struct pan_frame pf;
	pan_frame_init(&pf, rect, ff, 1.0, rot, canvas_w, canvas_h);
	double s_min = fmax(pf.ext_x / ((double)rect->r - rect->l),
			    pf.ext_y / ((double)rect->b - rect->t));
	if (s_min > STAGE_LAYER_SCALE_MAX)
		return FLT_MAX;
	/* s = zoom^f (the lower scale clamp only helps coverage). */
	double z = pow(s_min, 1.0 / ff);
	if (z < STAGE_ZOOM_MIN)
		z = STAGE_ZOOM_MIN;
	return z > (double)FLT_MAX ? FLT_MAX : (float)z;
}

bool stage_cover_pan_box(const struct stage_rect *rect, float f, float zoom, float rot,
			 float canvas_w, float canvas_h, struct stage_pan_box *out)
{
	struct stage_pan_box b = {0.0f, 0.0f, 0.0f, 0.0f};
	if (!rect_valid(rect)) {
		*out = b;
		return false;
	}
	double ff = sane_factor(f);
	if (ff <= 0.0) {
		/* Pan has no effect on this layer: unlimited when covered. */
		struct stage_cam home = STAGE_CAM_HOME_INIT;
		bool ok = stage_cover_margin(rect, 0.0f, &home, canvas_w, canvas_h) >=
			  -COVER_EPS_PX;
		if (ok) {
			b.x_min = b.y_min = -FLT_MAX;
			b.x_max = b.y_max = FLT_MAX;
		}
		*out = b;
		return ok;
	}
	double s = stage_layer_scale(cam_zoom(zoom), ff);
	struct pan_frame pf;
	pan_frame_init(&pf, rect, ff, s, rot, canvas_w, canvas_h);
	bool ok = true;
	if (pf.ulo_x > pf.uhi_x) {
		pf.ulo_x = pf.uhi_x = 0.5 * (pf.ulo_x + pf.uhi_x);
		ok = false;
	}
	if (pf.ulo_y > pf.uhi_y) {
		pf.ulo_y = pf.uhi_y = 0.5 * (pf.ulo_y + pf.uhi_y);
		ok = false;
	}
	b.x_min = (float)(pf.ulo_x / ff);
	b.x_max = (float)(pf.uhi_x / ff);
	b.y_min = (float)(pf.ulo_y / ff);
	b.y_max = (float)(pf.uhi_y / ff);
	*out = b;
	return ok;
}

/* Projects camera pan (x, y) onto one layer's feasible box (closest point,
 * rotation preserves distances). Empty axes go to their midpoint. */
static void project_pan(const struct stage_rect *rect, double f, double zoom, float rot, float w,
			float h, double *x, double *y)
{
	double s = stage_layer_scale(zoom, f);
	struct pan_frame pf;
	pan_frame_init(&pf, rect, f, s, rot, w, h);
	double wx = *x * f, wy = *y * f;
	double ux = pf.cosr * wx + pf.sinr * wy;
	double uy = -pf.sinr * wx + pf.cosr * wy;
	ux = pf.ulo_x <= pf.uhi_x ? clampd(ux, pf.ulo_x, pf.uhi_x) : 0.5 * (pf.ulo_x + pf.uhi_x);
	uy = pf.ulo_y <= pf.uhi_y ? clampd(uy, pf.ulo_y, pf.uhi_y) : 0.5 * (pf.ulo_y + pf.uhi_y);
	wx = pf.cosr * ux - pf.sinr * uy;
	wy = pf.sinr * ux + pf.cosr * uy;
	*x = wx / f;
	*y = wy / f;
}

/*
 * Pan constraints of all covering layers at one zoom, as half-planes
 * nx*x + ny*y <= c in camera px (unit normals). Each layer contributes the
 * four sides of its feasible box in its rotated frame.
 */
struct half_plane {
	double nx, ny, c;
};

struct cover_set {
	struct half_plane hp[4 * STAGE_COVER_MAX_LAYERS];
	int m;
};

static void cover_set_build(struct cover_set *cs, const struct stage_rect *rects,
			    const double *fs, const size_t *idx, int nl, double zoom, float rot,
			    float w, float h)
{
	cs->m = 0;
	for (int i = 0; i < nl; i++) {
		const struct stage_rect *r = &rects[idx[i]];
		double f = fs[i];
		struct pan_frame pf;
		pan_frame_init(&pf, r, f, stage_layer_scale(zoom, f), rot, w, h);
		/* u_x = f (cos x + sin y), u_y = f (-sin x + cos y). */
		double ax = pf.cosr, ay = pf.sinr, bx = -pf.sinr, by = pf.cosr;
		struct half_plane *hp = &cs->hp[cs->m];
		hp[0] = (struct half_plane){ax, ay, pf.uhi_x / f};
		hp[1] = (struct half_plane){-ax, -ay, -pf.ulo_x / f};
		hp[2] = (struct half_plane){bx, by, pf.uhi_y / f};
		hp[3] = (struct half_plane){-bx, -by, -pf.ulo_y / f};
		cs->m += 4;
	}
}

static bool cover_set_contains(const struct cover_set *cs, double x, double y)
{
	for (int k = 0; k < cs->m; k++) {
		const struct half_plane *h = &cs->hp[k];
		if (h->nx * x + h->ny * y > h->c + CLAMP_EPS * (1.0 + fabs(h->c)))
			return false;
	}
	return true;
}

/*
 * Exact closest feasible point to (x0, y0) for a convex polygon given as
 * half-planes: the optimum is p0 itself, its projection onto one boundary
 * line, or a vertex (intersection of two lines). Returns false when the
 * polygon is empty. O(m^3) with m <= 4 * STAGE_COVER_MAX_LAYERS; only runs
 * on ticks where the camera actually needs clamping.
 */
static bool cover_set_nearest(const struct cover_set *cs, double x0, double y0, double *ox,
			      double *oy)
{
	if (cover_set_contains(cs, x0, y0)) {
		*ox = x0;
		*oy = y0;
		return true;
	}
	double best = DBL_MAX;
	bool found = false;
	for (int a = 0; a < cs->m; a++) {
		const struct half_plane *ha = &cs->hp[a];
		double d = ha->nx * x0 + ha->ny * y0 - ha->c;
		double px = x0 - d * ha->nx, py = y0 - d * ha->ny;
		double dist = (px - x0) * (px - x0) + (py - y0) * (py - y0);
		if (dist < best && cover_set_contains(cs, px, py)) {
			best = dist;
			*ox = px;
			*oy = py;
			found = true;
		}
		for (int b = a + 1; b < cs->m; b++) {
			const struct half_plane *hb = &cs->hp[b];
			double det = ha->nx * hb->ny - ha->ny * hb->nx;
			if (fabs(det) < 1e-12)
				continue; /* parallel */
			double vx = (ha->c * hb->ny - ha->ny * hb->c) / det;
			double vy = (ha->nx * hb->c - ha->c * hb->nx) / det;
			dist = (vx - x0) * (vx - x0) + (vy - y0) * (vy - y0);
			if (dist < best && cover_set_contains(cs, vx, vy)) {
				best = dist;
				*ox = vx;
				*oy = vy;
				found = true;
			}
		}
	}
	return found;
}

bool stage_cover_clamp(const struct stage_rect *rects, const float *fs, size_t n,
		       const struct stage_cam *in, float canvas_w, float canvas_h,
		       struct stage_cam *out)
{
	struct stage_cam c = *in;
	if (isnan(c.zoom))
		c.zoom = 1.0f;
	if (isnan(c.rot))
		c.rot = 0.0f;
	if (isnan(c.x))
		c.x = 0.0f;
	if (isnan(c.y))
		c.y = 0.0f;

	/* Covering layers that the camera can influence. */
	size_t idx[STAGE_COVER_MAX_LAYERS];
	double ff[STAGE_COVER_MAX_LAYERS];
	int nl = 0;
	for (size_t i = 0; i < n && nl < STAGE_COVER_MAX_LAYERS; i++) {
		double f = sane_factor(fs[i]);
		if (f <= 0.0 || !rect_valid(&rects[i]))
			continue;
		idx[nl] = i;
		ff[nl] = f;
		nl++;
	}

	/* Already covered (the common case): leave the camera untouched. */
	bool covered = true;
	for (int i = 0; i < nl && covered; i++)
		covered = stage_cover_margin(&rects[idx[i]], (float)ff[i], &c, canvas_w,
					     canvas_h) >= -COVER_EPS_PX;
	if (covered) {
		*out = c;
		return true;
	}

	/* 1. Zoom: at least every layer's own minimum ... */
	double zoom = cam_zoom(c.zoom);
	for (int i = 0; i < nl; i++) {
		double zmin = stage_cover_min_zoom(&rects[idx[i]], (float)ff[i], c.rot, canvas_w,
						   canvas_h);
		if (zmin > zoom)
			zoom = zmin;
	}
	if (zoom > STAGE_ZOOM_MAX)
		zoom = STAGE_ZOOM_MAX;

	/* ... and enough for the layers' pan boxes to intersect. Feasibility
	 * only grows with zoom (each feasible set scales up about the canvas
	 * centre), so bisect on it. */
	struct cover_set cs;
	double x = c.x, y = c.y, nx, ny;
	cover_set_build(&cs, rects, ff, idx, nl, zoom, c.rot, canvas_w, canvas_h);
	bool ok = cover_set_nearest(&cs, x, y, &nx, &ny);
	if (!ok) {
		double lo = zoom, hi = STAGE_ZOOM_MAX;
		cover_set_build(&cs, rects, ff, idx, nl, hi, c.rot, canvas_w, canvas_h);
		ok = cover_set_nearest(&cs, x, y, &nx, &ny);
		if (ok) {
			for (int it = 0; it < 30 && hi - lo > 1e-7; it++) {
				double mid = 0.5 * (lo + hi);
				cover_set_build(&cs, rects, ff, idx, nl, mid, c.rot, canvas_w,
						canvas_h);
				if (cover_set_nearest(&cs, x, y, &nx, &ny))
					hi = mid;
				else
					lo = mid;
			}
			cover_set_build(&cs, rects, ff, idx, nl, hi, c.rot, canvas_w, canvas_h);
			ok = cover_set_nearest(&cs, x, y, &nx, &ny);
		}
		zoom = hi;
	}
	if (ok) {
		x = nx;
		y = ny;
	} else {
		/* Impossible even at maximum zoom: best effort, alternating
		 * projections (each layer's box collapses to its midpoint on an
		 * empty axis). The caller should oversize the layers. */
		for (int round = 0; round < CLAMP_ROUNDS; round++)
			for (int i = 0; i < nl; i++)
				project_pan(&rects[idx[i]], ff[i], zoom, c.rot, canvas_w, canvas_h,
					    &x, &y);
	}

	c.x = (float)x;
	c.y = (float)y;
	c.zoom = (float)zoom;
	*out = c;

	for (int i = 0; i < nl; i++)
		if (stage_cover_margin(&rects[idx[i]], (float)ff[i], &c, canvas_w, canvas_h) <
		    -COVER_EPS_PX)
			return false;
	return true;
}
