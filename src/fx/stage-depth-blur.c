/*
 * stage-depth-blur.c — Parallaxa Depth Blur (`stage_depth_blur`): per-layer
 * depth of field, dual-Kawase at 1/2 or 1/4 resolution with 2..4 passes.
 * docs/ux/stage-plugin-ux.md section 3, vendor-api.md section 3.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Strength (0..1) comes from `auto_strength` while `driven_by` names the
 * scene whose Camera Rig drives the layer, else from `strength_pct` / 100.
 * The filter never writes either key; it only clears `driven_by` when it is
 * created (a crash or a rig deleted while the collection was closed must not
 * leave a layer stuck as driven). Strength 0 = obs_source_skip_video_filter
 * (zero cost). Texrenders are cached per tick (a layer shown in preview and
 * program is blurred once).
 */
#include "stage-fx-internal.h"

#include <string.h>


#define KEY_STATUS "status_info"
#define KEY_QUALITY "quality"
#define KEY_PASSES "passes"
#define KEY_STRENGTH "strength_pct"
#define KEY_AUTO STAGE_DEPTH_BLUR_KEY_AUTO_STRENGTH
#define KEY_DRIVEN STAGE_DEPTH_BLUR_KEY_DRIVEN_BY

#define QUALITY_HALF 2
#define QUALITY_QUARTER 4
#define PASSES_MIN 2
#define PASSES_MAX 4
#define STRENGTH_EPS 0.0005f

struct dblur {
	obs_source_t *self;

	gs_effect_t *fx;
	gs_eparam_t *p_image, *p_blur, *p_texel, *p_offset, *p_mix;
	gs_texrender_t *input_tr;
	gs_texrender_t *chain[2 * PASSES_MAX];
	gs_texture_t *result;
	bool processed_frame;
	enum gs_color_space cached_space;
	uint32_t cached_w, cached_h;
	bool gpu_failed, hdr_warned;

	/* Settings (written by update(): create, then graphics thread). */
	int quality, passes;
	float manual, auto_strength;
	char driven_by[STAGE_SCENE_UUID_MAX];
	bool created;
};

static const char *db_get_name(void *type_data)
{
	(void)type_data;
	return obs_module_text("Stage.Filter.DepthBlur");
}

static float clamp01(double v)
{
	return v != v ? 0.0f : (float)(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
}

static float db_strength(const struct dblur *f)
{
	return f->driven_by[0] ? f->auto_strength : f->manual;
}

static void db_update(void *data, obs_data_t *settings)
{
	struct dblur *f = data;
	f->quality = obs_data_get_int(settings, KEY_QUALITY) == QUALITY_QUARTER ? QUALITY_QUARTER
										  : QUALITY_HALF;
	long long passes = obs_data_get_int(settings, KEY_PASSES);
	f->passes = passes < PASSES_MIN ? PASSES_MIN : (passes > PASSES_MAX ? PASSES_MAX : (int)passes);
	f->manual = clamp01(obs_data_get_double(settings, KEY_STRENGTH) / 100.0);
	f->auto_strength = clamp01(obs_data_get_double(settings, KEY_AUTO));

	const char *by = obs_data_get_string(settings, KEY_DRIVEN);
	if (!by)
		by = "";
	if (strcmp(by, f->driven_by) != 0) {
		const bool was = f->driven_by[0] != '\0';
		stage_strlcpy(f->driven_by, by, sizeof(f->driven_by));
		/* Start / stop of driving only (never per tick): grey or enable
		 * Strength in an open panel at once. */
		if (f->created && was != (by[0] != '\0'))
			obs_source_update_properties(f->self);
	}
}

static void *db_create(obs_data_t *settings, obs_source_t *source)
{
	struct dblur *f = bzalloc(sizeof(*f));
	f->self = source;
	/* Nothing drives a layer until a rig says so again on its first tick. */
	obs_data_set_string(settings, KEY_DRIVEN, "");

	obs_enter_graphics();
	f->fx = stfx_load_effect("stage-depth-blur.effect", true);
	obs_leave_graphics();
	if (f->fx) {
		f->p_image = gs_effect_get_param_by_name(f->fx, "image");
		f->p_blur = gs_effect_get_param_by_name(f->fx, "blur_tex");
		f->p_texel = gs_effect_get_param_by_name(f->fx, "texel");
		f->p_offset = gs_effect_get_param_by_name(f->fx, "offset");
		f->p_mix = gs_effect_get_param_by_name(f->fx, "mix_amount");
	} else {
		f->gpu_failed = true;
		SLOG_ERR("depth blur '%s': effect unavailable on this GPU; layer shown sharp",
			 obs_source_get_name(source));
	}
	db_update(f, settings);
	f->created = true;
	return f;
}

static void db_destroy(void *data)
{
	struct dblur *f = data;
	obs_enter_graphics();
	gs_effect_destroy(f->fx);
	gs_texrender_destroy(f->input_tr);
	for (int i = 0; i < 2 * PASSES_MAX; i++)
		gs_texrender_destroy(f->chain[i]);
	obs_leave_graphics();
	bfree(f);
}

static void db_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, KEY_QUALITY, QUALITY_HALF);
	obs_data_set_default_int(settings, KEY_PASSES, 3);
	obs_data_set_default_double(settings, KEY_STRENGTH, 0.0);
	obs_data_set_default_double(settings, KEY_AUTO, 0.0);
	obs_data_set_default_string(settings, KEY_DRIVEN, "");
}

static void db_tick(void *data, float seconds)
{
	(void)seconds;
	struct dblur *f = data;
	f->processed_frame = false;
}

static uint32_t half_of(uint32_t v)
{
	return v / 2 ? v / 2 : 1;
}

/* Input (full res) -> L0 (1/2 or 1/4) -> passes-1 downs -> ups back to L0. */
static gs_texture_t *blur_chain(struct dblur *f, gs_texture_t *input, uint32_t w, uint32_t h,
				float strength)
{
	const int levels = f->passes; /* L0 .. L(passes-1) */
	uint32_t lw[PASSES_MAX], lh[PASSES_MAX];
	lw[0] = w / (uint32_t)f->quality ? w / (uint32_t)f->quality : 1;
	lh[0] = h / (uint32_t)f->quality ? h / (uint32_t)f->quality : 1;
	for (int i = 1; i < levels; i++) {
		lw[i] = half_of(lw[i - 1]);
		lh[i] = half_of(lh[i - 1]);
	}
	for (int i = 0; i < 2 * levels - 1; i++) {
		if (!f->chain[i])
			f->chain[i] = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
		if (!f->chain[i])
			return NULL;
	}

	/* First step is a prefilter: at quarter resolution the taps spread over
	 * a 4x4 footprint so nothing is skipped. */
	const struct stfx_pass_params pp = {f->p_image, f->p_texel, f->p_offset};
	if (!stfx_pass(f->chain[0], lw[0], lh[0], f->fx, "Down", &pp, input, true,
		       f->quality == QUALITY_QUARTER ? 2.0f : 1.0f))
		return NULL;
	gs_texture_t *src = gs_texrender_get_texture(f->chain[0]);

	const float offset = 0.75f + 1.75f * strength;
	int k = 1;
	for (int i = 1; i < levels; i++, k++) {
		if (!stfx_pass(f->chain[k], lw[i], lh[i], f->fx, "Down", &pp, src, false, offset))
			return NULL;
		src = gs_texrender_get_texture(f->chain[k]);
	}
	for (int i = levels - 2; i >= 0; i--, k++) {
		if (!stfx_pass(f->chain[k], lw[i], lh[i], f->fx, "Up", &pp, src, false, offset))
			return NULL;
		src = gs_texrender_get_texture(f->chain[k]);
	}
	return src;
}

static void db_render(void *data, gs_effect_t *effect)
{
	(void)effect;
	struct dblur *f = data;
	const float strength = db_strength(f);
	obs_source_t *target = obs_filter_get_target(f->self);
	obs_source_t *parent = obs_filter_get_parent(f->self);
	if (!(strength > STRENGTH_EPS) || f->gpu_failed || !target || !parent) {
		obs_source_skip_video_filter(f->self);
		return;
	}
	const uint32_t w = obs_source_get_base_width(target);
	const uint32_t h = obs_source_get_base_height(target);
	const enum gs_color_space space = stfx_target_space(target);
	const bool hdr = stfx_space_is_hdr(space);
	if (hdr && !f->hdr_warned) {
		SLOG_WARN("depth blur '%s': HDR (scRGB) input is passed through unchanged",
			  obs_source_get_name(f->self));
		f->hdr_warned = true;
	}
	if (!w || !h || hdr) {
		obs_source_skip_video_filter(f->self);
		return;
	}

	if (!f->processed_frame || f->cached_space != space || f->cached_w != w ||
	    f->cached_h != h || !f->result) {
		f->result = NULL;
		if (stfx_render_target(f->self, &f->input_tr, space, w, h)) {
			gs_texture_t *in = gs_texrender_get_texture(f->input_tr);
			f->result = in ? blur_chain(f, in, w, h, strength) : NULL;
		}
		f->processed_frame = true;
		f->cached_space = space;
		f->cached_w = w;
		f->cached_h = h;
	}
	gs_texture_t *input = f->input_tr ? gs_texrender_get_texture(f->input_tr) : NULL;
	if (!input || !f->result) {
		obs_source_skip_video_filter(f->self);
		return;
	}

	const float mix = strength * 5.0f > 1.0f ? 1.0f : strength * 5.0f;
	const bool prev_fb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_effect_set_texture_srgb(f->p_image, input);
	gs_effect_set_texture(f->p_blur, f->result);
	gs_effect_set_float(f->p_mix, mix);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(f->fx, "Mix"))
		gs_draw_sprite(input, 0, w, h);
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(prev_fb);
}

static enum gs_color_space db_color_space(void *data, size_t count,
					  const enum gs_color_space *preferred_spaces)
{
	(void)count;
	(void)preferred_spaces;
	struct dblur *f = data;
	obs_source_t *target = obs_filter_get_target(f->self);
	return target ? stfx_target_space(target) : GS_CS_SRGB;
}

static obs_properties_t *db_properties(void *data)
{
	struct dblur *f = data;
	obs_data_t *settings = obs_source_get_settings(f->self);
	const bool driven = obs_data_get_string(settings, KEY_DRIVEN)[0] != '\0';
	obs_properties_t *props = obs_properties_create();

	obs_data_set_default_string(settings, KEY_STATUS,
				    driven ? obs_module_text("Stage.Info.DepthBlurDriven")
					   : obs_module_text("Stage.Info.DepthBlurManual"));
	obs_properties_add_text(props, KEY_STATUS, obs_module_text("Stage.Prop.Status"), OBS_TEXT_INFO);

	obs_property_t *p = obs_properties_add_list(props, KEY_QUALITY, obs_module_text("Stage.Prop.BlurQuality"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Stage.List.Quality.Half"), QUALITY_HALF);
	obs_property_list_add_int(p, obs_module_text("Stage.List.Quality.Quarter"), QUALITY_QUARTER);
	stfx_set_desc(p, "Stage.Prop.BlurQuality");

	p = obs_properties_add_int_slider(props, KEY_PASSES, obs_module_text("Stage.Prop.BlurPasses"), PASSES_MIN,
					  PASSES_MAX, 1);
	stfx_set_desc(p, "Stage.Prop.BlurPasses");

	p = obs_properties_add_float_slider(props, KEY_STRENGTH, obs_module_text("Stage.Prop.BlurStrength"), 0.0,
					    100.0, 1.0);
	obs_property_float_set_suffix(p, " %");
	stfx_set_desc(p, "Stage.Prop.BlurStrength");
	obs_property_set_enabled(p, !driven);

	obs_data_release(settings);
	return props;
}

void stfx_register_depth_blur(void)
{
	struct obs_source_info info = {
		.id = STAGE_FILTER_DEPTH_BLUR,
		.type = OBS_SOURCE_TYPE_FILTER,
		.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
		.get_name = db_get_name,
		.create = db_create,
		.destroy = db_destroy,
		.update = db_update,
		.get_defaults = db_defaults,
		.get_properties = db_properties,
		.video_tick = db_tick,
		.video_render = db_render,
		.video_get_color_space = db_color_space,
	};
	obs_register_source(&info);
}
