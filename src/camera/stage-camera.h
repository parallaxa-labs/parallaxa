/*
 * stage-camera.h — Parallaxa Camera Rig module (src/camera): the stage_camera_rig
 * scene filter, its move table, frontend hotkeys and the Tools menu item
 * "Parallaxa: Restore all rest poses".
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Public to the other plugin modules:
 *   - stage_camera_restore_all_rest_poses(): what the Tools item does.
 *   - stage_cam_moves_*(): the loaded move table (built-ins compiled in +
 *     data/presets/moves + user moves); read-only copies, any thread.
 *
 * The camera_move ease sentinel and the hotkey-only command extensions are
 * in stage-rig-model.h (STAGE_CAM_EASE_NONE, STAGE_CAM_AUTO_*,
 * STAGE_CAM_DOF_RACK_*).
 */
#ifndef STAGE_CAMERA_H
#define STAGE_CAMERA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stage-protocol.h"
#include "stage-move.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Move table (stage-cam-moves.c)                                            */
/* ------------------------------------------------------------------------- */

#define STAGE_CAM_MOVES_MAX 64
#define STAGE_CAM_NAME_MAX 260 /* 64 code points x 4 bytes + NUL */

struct stage_cam_move {
	struct stage_move_def def;
	char name_en[STAGE_CAM_NAME_MAX];
	char name_ja[STAGE_CAM_NAME_MAX];
	char name_id[STAGE_CAM_NAME_MAX];
	bool builtin; /* one of stage_move_names[] */
	bool user;    /* loaded from obs_module_config_path("moves") */
};

/* Loads the table: the nine compiled-in built-ins, then every valid
 * <builtin_dir>/<id>.json (a valid file replaces the compiled built-in of the
 * same id), then <user_dir>/<id>.json (a user file never replaces a built-in
 * or shipped move). Invalid files are logged (warning) and skipped. Either
 * directory may be NULL or missing. Returns the number of moves. */
size_t stage_cam_moves_load(const char *builtin_dir, const char *user_dir);
void stage_cam_moves_free(void);

size_t stage_cam_moves_count(void);
bool stage_cam_moves_at(size_t i, struct stage_cam_move *out);
/* Copies the move definition for id; false when unknown. */
bool stage_cam_moves_find(const char *id, struct stage_move_def *out);
/* Name for the current OBS locale (ja-*, id-* or English). */
const char *stage_cam_move_local_name(const struct stage_cam_move *m);

/* Parses one move file (strict: unknown fields, wrong types and values out
 * of the move.json ranges are errors). expect_id: the id the file name
 * implies (NULL = no check). On error returns false and writes a short
 * reason into err. */
bool stage_cam_move_parse_file(const char *path, const char *expect_id, struct stage_cam_move *out,
			       char *err, size_t err_size);

/* ------------------------------------------------------------------------- */
/* Rig filter (stage-camera-rig.c)                                           */
/* ------------------------------------------------------------------------- */

/* Registers the stage_camera_rig source type. */
void stage_camera_rig_register_source(void);

/* Every rig restores its items to their rest poses now (synchronously when
 * called on the UI thread), snaps its camera home and turns its auto camera
 * off. Used by Tools > "Parallaxa: Restore all rest poses" and before a scene
 * collection change. UI thread. */
void stage_camera_restore_all_rest_poses(void);

/* Frontend events seen by the camera module (collection change, exit). */
void stage_camera_rig_frontend_event(int event);

/* ------------------------------------------------------------------------- */
/* Hotkeys and Tools menu (stage-camera-hotkeys.c)                           */
/* ------------------------------------------------------------------------- */

void stage_camera_hotkeys_init(void);
void stage_camera_hotkeys_free(void);

/* Docs / bug report links of the About row. STAGE_PROJECT_URL is the public
 * repository: CMake sets it (STAGE_PROJECT_URL cache variable; release builds
 * pass the repository they are built from). The default is the upstream
 * repository. */
#ifndef STAGE_PROJECT_URL
#define STAGE_PROJECT_URL "https://github.com/parallaxa-labs/parallaxa"
#endif
#define STAGE_CAM_DOCS_URL STAGE_PROJECT_URL "#readme"
#define STAGE_CAM_BUGS_URL STAGE_PROJECT_URL "/issues"

#ifdef __cplusplus
}
#endif

#endif /* STAGE_CAMERA_H */
