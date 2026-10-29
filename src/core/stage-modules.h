/*
 * stage-modules.h — entry points of the optional plugin modules.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * CMake defines STAGE_HAS_<MOD>=1 for every src/<mod>/ directory that
 * contains .c files, and plugin-main.c calls the matching entry point only
 * under that #ifdef, so each module builds (and ships) independently.
 *
 * Call order:
 *   obs_module_load:      stage_core_init, frontend observers,
 *                         stage_fx_register, stage_camera_register,
 *                         stage_presets_init
 *   obs_module_post_load: stage_vendor_init
 *   OBS_FRONTEND_EVENT_EXIT / obs_module_unload:
 *                         stage_vendor_shutdown (idempotent, may be called
 *                         twice; must guard obs-websocket calls)
 *   obs_module_unload:    stage_core_on_unload hooks (LIFO), core shutdown
 *
 * Modules that allocate at load time register their cleanup with
 * stage_core_on_unload() (stage-core.h). Frontend events and save callbacks
 * reach modules through stage-frontend.h observers.
 */
#ifndef STAGE_MODULES_H
#define STAGE_MODULES_H

#ifdef __cplusplus
extern "C" {
#endif

/* src/camera: registers the stage_camera_rig filter, hotkeys, moves. */
void stage_camera_register(void);

/* src/fx: registers stage_fx (and stage_depth_blur). */
void stage_fx_register(void);

/* src/vendor: obs-websocket vendor "parallaxa" (post_load). */
void stage_vendor_init(void);
void stage_vendor_shutdown(void);

/* src/presets: recipes / FX presets loader, Tools menu items. */
void stage_presets_init(void);

#ifdef __cplusplus
}
#endif

#endif /* STAGE_MODULES_H */
