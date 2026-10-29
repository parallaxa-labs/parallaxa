/*
 * stage-tools.c — presets module entry (stage_presets_init) and the OBS
 * Tools menu items (docs/ux/stage-plugin-ux.md section 4):
 *   Parallaxa: Set up current scene   - adds Camera Rig + FX if missing,
 *                                       applies cozy_chat (roles auto-guessed)
 *   Parallaxa: Restore all rest poses - auto off, camera home at once and
 *                                       "Bake rest pose" on every rig
 *   Parallaxa: Panic reset            - global panic
 *   Parallaxa: Import sample scene    - loads the sample collection's scene
 *                                       into this collection (stage-sample.h)
 * Menu callbacks run on the UI thread; results go to the OBS log.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <string.h>
#include <util/bmem.h>

#include "stage-core.h"
#include "stage-modules.h"
#include "stage-presets.h"
#include "stage-sample.h"
#include "stage-target.h"

#define SETUP_RECIPE "cozy_chat"
/* Camera Rig "Bake rest pose" button (stage-plugin-ux.md section 1). */
#define RIG_BUTTON_BAKE_REST "bake_rest"

static bool filter_type_registered(const char *id)
{
	return obs_source_get_display_name(id) != NULL;
}

/* Adds a filter of type id to parent unless it already has one. Returns true
 * when the parent has (or now has) such a filter. */
static bool ensure_filter(obs_source_t *parent, const char *id, const char *name_key, bool *added)
{
	*added = false;
	obs_source_t *f = stage_find_filter(parent, id);
	if (f) {
		obs_source_release(f);
		return true;
	}
	if (!filter_type_registered(id)) {
		SLOG_WARN("tools: filter '%s' is not available in this build", id);
		return false;
	}
	f = obs_source_create(id, obs_module_text(name_key), NULL, NULL);
	if (!f)
		return false;
	obs_source_filter_add(parent, f);
	obs_source_release(f);
	*added = true;
	return true;
}

static void tool_setup_scene(void *param)
{
	(void)param;
	obs_source_t *scene = obs_frontend_get_current_scene();
	if (!scene) {
		SLOG_WARN("tools: set up scene: no current scene");
		return;
	}
	const char *name = obs_source_get_name(scene);
	bool rig_added = false, fx_added = false;
	bool rig = ensure_filter(scene, STAGE_FILTER_CAMERA_RIG, "Stage.Filter.CameraRig", &rig_added);
	ensure_filter(scene, STAGE_FILTER_FX, "Stage.Filter.FX", &fx_added);

	if (rig && !rig_added) {
		/* An existing rig: apply the default look now. A new rig runs its
		 * own first-add (auto-assign roles + cozy_chat) when it starts. */
		struct stage_recipe_result res;
		char err[128] = "";
		int rc = stage_recipe_apply_id(obs_source_get_uuid(scene), SETUP_RECIPE,
					       STAGE_RECIPE_DURATION_DEFAULT, &res, err, sizeof(err));
		if (rc < 0)
			SLOG_INFO("tools: '%s': applied %s (%d layers)", name, SETUP_RECIPE, res.layers);
		else
			SLOG_WARN("tools: '%s': could not apply %s: %s", name, SETUP_RECIPE, err);
	}
	SLOG_INFO("tools: set up '%s' (camera rig %s, stage fx %s)", name,
		  rig_added ? "added" : (rig ? "present" : "missing"), fx_added ? "added" : "present");
	obs_source_release(scene);
}

static void push_simple(const char *scene_uuid, enum stage_cmd_type type, uint8_t auto_mode)
{
	struct stage_cmd c;
	memset(&c, 0, sizeof(c));
	c.type = type;
	stage_strlcpy(c.scene_uuid, scene_uuid, sizeof(c.scene_uuid));
	c.ease = STAGE_EASE_DEFAULT;
	c.intensity = -1.0f;
	c.duration_ms = 0;
	c.u.autom.mode = auto_mode;
	stage_cmdq_push(&c);
}

static bool restore_rig(const char *scene_uuid, void *param)
{
	int *count = param;
	push_simple(scene_uuid, STAGE_CMD_CAMERA_AUTO, STAGE_AUTO_OFF);
	push_simple(scene_uuid, STAGE_CMD_CAMERA_RESET, 0);

	obs_source_t *scene = obs_get_source_by_uuid(scene_uuid);
	obs_source_t *rig = stage_find_filter(scene, STAGE_FILTER_CAMERA_RIG);
	if (rig) {
		obs_properties_t *props = obs_source_properties(rig);
		obs_property_t *p = obs_properties_get(props, RIG_BUTTON_BAKE_REST);
		if (p && obs_property_get_type(p) == OBS_PROPERTY_BUTTON)
			obs_property_button_clicked(p, rig);
		obs_properties_destroy(props);
	}
	obs_source_release(rig);
	obs_source_release(scene);
	(*count)++;
	return true;
}

static void tool_restore_rest(void *param)
{
	(void)param;
	int count = 0;
	stage_rig_for_each(restore_rig, &count);
	SLOG_INFO("tools: restored rest poses on %d rig(s)", count);
}

static void tool_panic(void *param)
{
	(void)param;
	stage_panic_trigger();
	SLOG_INFO("tools: panic reset");
}

static void tool_import_sample(void *param)
{
	(void)param;
	char *path = obs_module_file(STAGE_SAMPLE_FILE);
	if (!path) {
		SLOG_WARN("tools: import sample: %s not found", STAGE_SAMPLE_FILE);
		return;
	}
	struct sj_err e = {""};
	obs_source_t *scene = stage_sample_import(path, &e);
	bfree(path);
	if (!scene) {
		SLOG_WARN("tools: import sample: %s", e.msg);
		return;
	}
	/* Never on air by itself: only the studio-mode preview switches. */
	if (obs_frontend_preview_program_mode_active())
		obs_frontend_set_current_preview_scene(scene);
	SLOG_INFO("tools: imported the sample scene as '%s'", obs_source_get_name(scene));
	obs_source_release(scene);
}

/* ------------------------------------------------------------------------- */
/* Module entry                                                               */
/* ------------------------------------------------------------------------- */

static void presets_unload(void *ud)
{
	(void)ud;
	stage_presets_free();
	stage_views_discard_pending();
}

/* obs_module_config_path("") without the trailing slash; NULL when OBS has
 * no module config path (it would be relative). Caller bfree()s. */
static char *user_root(void)
{
	char *p = obs_module_config_path("");
	if (!p)
		return NULL;
	size_t n = strlen(p);
	while (n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\'))
		p[--n] = '\0';
	bool absolute = p[0] == '/' || (n > 2 && p[1] == ':');
	if (!absolute) {
		bfree(p);
		return NULL;
	}
	return p;
}

void stage_presets_init(void)
{
	char *builtin = obs_module_file("presets");
	char *user = user_root();
	if (!builtin)
		SLOG_WARN("presets: data/presets folder not found");
	stage_presets_load(builtin, user);
	bfree(builtin);
	bfree(user);
	stage_core_on_unload(presets_unload, NULL);

	obs_frontend_add_tools_menu_item(obs_module_text("Stage.Tools.SetupScene"), tool_setup_scene,
					 NULL);
	obs_frontend_add_tools_menu_item(obs_module_text("Stage.Tools.RestoreRest"),
					 tool_restore_rest, NULL);
	obs_frontend_add_tools_menu_item(obs_module_text("Stage.Tools.Panic"), tool_panic, NULL);
	obs_frontend_add_tools_menu_item(obs_module_text("Stage.Tools.ImportSample"),
					 tool_import_sample, NULL);
}
