/*
 * stage-presets.h — preset catalog (recipes, move list, FX preset / LUT /
 * light-leak ids), recipe application and user recipes ("Looks").
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Files (docs/protocol/vendor-api.md section 6, schemas recipe.json,
 * move.json, fx-preset.json):
 *   built-in  <module data>/presets/{recipes,moves,fx,luts,leaks}/
 *   user      obs_module_config_path("{recipes,moves,fx,luts,leaks}")
 * Each preset file is <id>.json and must match its schema strictly; files
 * with errors are logged (warning) and skipped. A user file with the id of a
 * built-in one is skipped too (built-ins win, so a Look id is never
 * ambiguous). LUTs and leaks are <id>.png.
 *
 * Thread safety: every function may be called from any thread. The catalog
 * is guarded by one mutex; getters copy entries out. No function here
 * touches scene items (recipe application only pushes stage_cmd commands
 * and updates the rig filter's settings via obs_source_update, which libobs
 * defers to the video tick).
 *
 * This header is public to the other plugin modules: the Camera Rig panel
 * uses the recipe list, stage_recipe_apply_id() for its Look list and
 * stage_recipe_save_user() for "Save look".
 */
#ifndef STAGE_PRESETS_H
#define STAGE_PRESETS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stage-protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A localized display name {en, ja, id}: <= 64 code points each. */
#define STAGE_LOC_NAME_MAX 260
struct stage_loc_name {
	char en[STAGE_LOC_NAME_MAX];
	char ja[STAGE_LOC_NAME_MAX];
	char id[STAGE_LOC_NAME_MAX];
};

/* Recipe role slots (the only roles a recipe maps to depths). */
enum stage_recipe_role { STAGE_RROLE_BG, STAGE_RROLE_CHAR, STAGE_RROLE_FG, STAGE_RROLE_COUNT };

struct stage_recipe {
	char id[STAGE_ID_MAX];
	struct stage_loc_name name;
	bool pro;
	bool user; /* loaded from the user config folder */
	/* No tags: obs_data drops arrays of strings (both when libobs reads the
	 * file and when obs-websocket serializes a response), so "tags" is only
	 * checked to be an array. */

	bool has_role[STAGE_RROLE_COUNT];
	double role_depth[STAGE_RROLE_COUNT];

	bool has_auto;
	uint8_t auto_mode; /* enum stage_auto_mode */
	float auto_intensity;

	char fx[STAGE_ID_MAX]; /* FX preset id, "" = none */

	bool has_audio;
	float audio_amount;
	int audio_attack_ms;  /* 0 = not given */
	int audio_release_ms; /* 0 = not given */

	bool has_dof;
	float focus_depth, dof_amount;

	bool has_camera;
	float cam_x, cam_y, cam_zoom, cam_rot;

	bool has_overscan;
	float overscan; /* 0..0.25 */
};

/* move_list entry (metadata only; the Camera Rig owns the move curves). */
struct stage_move_info {
	char id[STAGE_ID_MAX];
	struct stage_loc_name name;
	uint32_t duration_ms;
	bool builtin; /* one of stage_move_names[] */
	bool user;
};

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

/* stage_presets_init() (stage-modules.h) resolves the module folders, loads
 * the catalog, adds the Tools menu items and registers its unload hook. */

/* Loads (or reloads) the catalog from explicit roots. builtin_root is the
 * folder holding recipes/, moves/, fx/, luts/, leaks/ (data/presets);
 * user_root the user config folder (may be NULL). Returns the number of
 * recipes loaded. Used by stage_presets_init and by the unit tests. */
size_t stage_presets_load(const char *builtin_root, const char *user_root);
/* Reloads from the roots of the last load. */
void stage_presets_reload(void);
/* Frees the catalog (module unload). */
void stage_presets_free(void);

/* Copies "<user root>/<sub>" (e.g. sub = "luts") into out. False when there
 * is no user folder (headless libobs without a module config path). */
bool stage_presets_user_dir(const char *sub, char *out, size_t cap);

/* ------------------------------------------------------------------------- */
/* Catalog queries (copies; any thread)                                      */
/* ------------------------------------------------------------------------- */

size_t stage_recipe_count(void);
bool stage_recipe_at(size_t i, struct stage_recipe *out);
bool stage_recipe_find(const char *id, struct stage_recipe *out);

size_t stage_move_info_count(void);
bool stage_move_info_at(size_t i, struct stage_move_info *out);
/* True for the 9 built-in move names (always available: the rig compiles
 * fallbacks in) and for every valid move file. */
bool stage_move_known(const char *id);

bool stage_fx_preset_known(const char *id);
bool stage_lut_known(const char *id);
bool stage_leak_known(const char *id);

/* Value of one numeric fx_set parameter in an FX preset file (absent
 * parameters take the neutral value). False when the preset or the param is
 * unknown or is an id param (lut, leak). Used by the FX toggle hotkeys. */
bool stage_fx_preset_param(const char *preset_id, const char *param, double *out);

/* fx_set parameter table (vendor-api.md 4.7). is_id: value is an id string
 * ("" allowed) resolved against the LUT / leak catalog. */
struct stage_fx_param_def {
	const char *name;
	double lo, hi, neutral;
	bool is_id;
};
extern const struct stage_fx_param_def stage_fx_params[];
extern const size_t stage_fx_param_count;
const struct stage_fx_param_def *stage_fx_param_find(const char *name);

/* ------------------------------------------------------------------------- */
/* Recipe application                                                        */
/* ------------------------------------------------------------------------- */

struct stage_recipe_result {
	uint64_t id;       /* command id shared by every command of the batch */
	int layers;        /* items whose depth was set                       */
	uint8_t auto_mode; /* resulting auto mode (recipe's, else current)     */
	char fx[STAGE_ID_MAX]; /* FX preset applied, "" when none / no Parallaxa FX */
};

/* Role guess from a source name (vendor-api.md 4.13), case-insensitive:
 *   bg|background|背景 -> bg, vts|spout|avatar|model|vtube -> char,
 *   fg|foreground|前景 -> fg. Returns STAGE_ROLE_NONE when nothing matches.
 * Short tokens (bg, fg, vts) must be whole words, so "Background" is bg but
 * "Bigfoot" is not fg. */
int stage_guess_role(const char *name);

/*
 * Applies recipe r to the rig of scene_uuid as one command batch (all with
 * the same id) over duration_ms: layer depths (for items whose role is set,
 * or guessed when the item is not enrolled), auto mode, FX preset (only when
 * the scene carries Parallaxa FX), DoF and the start pose; overscan and audio
 * react go to the rig filter's settings. Returns -1 on success (out filled)
 * or an enum stage_error (STAGE_ERROR_NO_RIG, STAGE_ERROR_BUSY,
 * STAGE_ERROR_INTERNAL), with a short message in err.
 * Any thread except the graphics thread (it may call obs_source_update).
 */
int stage_recipe_apply(const char *scene_uuid, const struct stage_recipe *r, uint32_t duration_ms,
		       struct stage_recipe_result *out, char *err, size_t err_size);
/* Same, by recipe id (STAGE_ERROR_NOT_FOUND when unknown). */
int stage_recipe_apply_id(const char *scene_uuid, const char *recipe_id, uint32_t duration_ms,
			  struct stage_recipe_result *out, char *err, size_t err_size);

/* Default duration of recipe_apply (vendor-api.md 4.13). */
#define STAGE_RECIPE_DURATION_DEFAULT 1000

/* ------------------------------------------------------------------------- */
/* User recipes ("Save look")                                                */
/* ------------------------------------------------------------------------- */

enum stage_save_result {
	STAGE_SAVE_OK,
	STAGE_SAVE_NAME_EMPTY, /* Stage.Info.LookNameEmpty */
	STAGE_SAVE_NAME_TAKEN, /* Stage.Info.LookNameTaken */
	STAGE_SAVE_NAME_INVALID,
	STAGE_SAVE_NO_RIG,
	STAGE_SAVE_IO,
};

/* Turns a display name into a protocol id: ASCII letters/digits lowercased,
 * runs of anything else become '-', trimmed, cut to 31 bytes; a name with no
 * ASCII letters or digits (e.g. Japanese) becomes "look-<8 hex>" from a hash
 * of the name. */
void stage_recipe_id_from_name(const char *name, char out[STAGE_ID_MAX]);

/* Saves the current look of scene_uuid's rig (layer depths per role, auto
 * mode, FX preset, DoF, overscan and audio react from the rig settings) as a
 * user recipe named `name` (1..47 code points) and reloads the catalog.
 * out_id (optional) receives the new id. */
enum stage_save_result stage_recipe_save_user(const char *scene_uuid, const char *name,
					      char out_id[STAGE_ID_MAX]);

#ifdef __cplusplus
}
#endif

#endif /* STAGE_PRESETS_H */
