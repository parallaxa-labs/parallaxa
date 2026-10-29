/*
 * plugin-main.c — Parallaxa OBS module entry.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */
#include <obs-module.h>
#include <obs-frontend-api.h>

#include "stage-core.h"
#include "stage-frontend.h"
#include "stage-modules.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(STAGE_MODULE_NAME, "en-US")
OBS_MODULE_AUTHOR("Parallaxa Labs")

const char *obs_module_name(void)
{
	return "Parallaxa";
}

const char *obs_module_description(void)
{
	return obs_module_text("Stage.Description");
}

/* Comma-separated list of the modules compiled into this build (for the log). */
static const char *compiled_modules(void)
{
	return "core"
#ifdef STAGE_HAS_MATH
	       ",math"
#endif
#ifdef STAGE_HAS_CAMERA
	       ",camera"
#endif
#ifdef STAGE_HAS_FX
	       ",fx"
#endif
#ifdef STAGE_HAS_VENDOR
	       ",vendor"
#endif
#ifdef STAGE_HAS_PRESETS
	       ",presets"
#endif
		;
}

static void on_exit_event(enum obs_frontend_event event, void *ud)
{
	(void)ud;
	/* obs-websocket may be unloaded before us, so the vendor detaches while
	 * the frontend still exists (stage_vendor_shutdown is idempotent). */
	if (event == OBS_FRONTEND_EVENT_EXIT) {
#ifdef STAGE_HAS_VENDOR
		stage_vendor_shutdown();
#endif
	}
}

bool obs_module_load(void)
{
	stage_core_init();
	stage_frontend_attach();
	stage_frontend_add_event_cb(on_exit_event, NULL);

#ifdef STAGE_HAS_FX
	stage_fx_register();
#endif
#ifdef STAGE_HAS_CAMERA
	stage_camera_register();
#endif
#ifdef STAGE_HAS_PRESETS
	stage_presets_init();
#endif

	SLOG_INFO("loaded v%s (protocol %d) [modules: %s; built against libobs %d.%d.%d, running %s]",
		  STAGE_VERSION, STAGE_PROTOCOL_VERSION, compiled_modules(), LIBOBS_API_MAJOR_VER,
		  LIBOBS_API_MINOR_VER, LIBOBS_API_PATCH_VER, obs_get_version_string());
	return true;
}

void obs_module_post_load(void)
{
#ifdef STAGE_HAS_VENDOR
	stage_vendor_init();
#endif
}

void obs_module_unload(void)
{
#ifdef STAGE_HAS_VENDOR
	stage_vendor_shutdown();
#endif
	stage_frontend_remove_event_cb(on_exit_event, NULL);
	stage_frontend_detach();
	stage_core_shutdown(); /* runs module unload hooks, logs queue stats */
	SLOG_INFO("unloaded");
}
