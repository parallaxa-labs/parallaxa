/*
 * obs_load_smoke.c — loads parallaxa.dll into a headless libobs (the
 * installed OBS's obs.dll; no OBS UI, no graphics, no config written) and
 * checks the load behaviour that does not need the OBS application:
 *   - obs_open_module succeeds (no MODULE_INCOMPATIBLE_VER),
 *   - the log shows "[parallaxa] loaded v<version> (protocol 1)",
 *   - the module description comes from data/locale/en-US.ini,
 *   - post_load and unload run, and unloading leaks nothing
 *     (bnum_allocs() after obs_shutdown equals a baseline run without the
 *     plugin, which is 0 on a clean libobs).
 *
 * Usage: obs_load_smoke <path/to/parallaxa.dll> <path/to/data>
 * PATH must contain the OBS bin\64bit folder (ctest sets it).
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <obs.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <util/bmem.h>

#include "stage-protocol.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifndef STAGE_VERSION
#define STAGE_VERSION "0.0.0-dev"
#endif

static char loaded_line[256];
static bool saw_loaded, saw_unloaded, saw_frontend_warning;
static int errors_logged;

static void log_handler(int level, const char *fmt, va_list args, void *param)
{
	(void)param;
	char msg[1024];
	vsnprintf(msg, sizeof(msg), fmt, args);
	if (strstr(msg, "[parallaxa]"))
		printf("  log: %s\n", msg);
	if (strstr(msg, "[parallaxa] loaded v")) {
		saw_loaded = true;
		snprintf(loaded_line, sizeof(loaded_line), "%s", msg);
	}
	if (strstr(msg, "[parallaxa] unloaded"))
		saw_unloaded = true;
	if (strstr(msg, "no callbacks"))
		saw_frontend_warning = true; /* expected: no OBS frontend here */
	if (level <= LOG_ERROR && strstr(msg, "[parallaxa]"))
		errors_logged++;
}

static long run(const char *dll, const char *data, bool load_plugin, int *failures)
{
	if (!obs_startup("en-US", NULL, NULL)) {
		fprintf(stderr, "FAIL obs_startup\n");
		(*failures)++;
		return -1;
	}
	if (load_plugin) {
		obs_module_t *mod = NULL;
		int r = obs_open_module(&mod, dll, data);
		if (r != MODULE_SUCCESS) {
			fprintf(stderr, "FAIL obs_open_module = %d%s\n", r,
				r == MODULE_INCOMPATIBLE_VER ? " (MODULE_INCOMPATIBLE_VER)" : "");
			(*failures)++;
		} else {
			if (!obs_init_module(mod)) {
				fprintf(stderr, "FAIL obs_init_module\n");
				(*failures)++;
			}
			obs_post_load_modules();
			const char *name = obs_get_module_name(mod);
			const char *desc = obs_get_module_description(mod);
			printf("  module: %s — %s\n", name ? name : "(null)", desc ? desc : "(null)");
			if (!name || strcmp(name, "Parallaxa") != 0) {
				fprintf(stderr, "FAIL module name\n");
				(*failures)++;
			}
			/* The en-US text, not the raw key, proves the locale loaded. */
			if (!desc || strcmp(desc, "Stage.Description") == 0 || !strstr(desc, "VTuber")) {
				fprintf(stderr, "FAIL module description not localized\n");
				(*failures)++;
			}
		}
	}
	obs_shutdown();
	return bnum_allocs();
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: obs_load_smoke <parallaxa.dll> <data>\n");
		return 2;
	}
	base_set_log_handler(log_handler, NULL);
	int failures = 0;

#ifdef _WIN32
	/* libobs loads modules with LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, which skips
	 * PATH. In OBS, obs-frontend-api.dll sits next to obs64.exe; here we
	 * preload it from PATH (the OBS bin folder) so the import resolves. */
	if (!LoadLibraryA("obs-frontend-api.dll")) {
		fprintf(stderr, "FAIL obs-frontend-api.dll not found on PATH\n");
		return 1;
	}
#endif

	long baseline = run(argv[1], argv[2], false, &failures);
	long with_plugin = run(argv[1], argv[2], true, &failures);
	printf("  libobs %s; allocations after shutdown: baseline %ld, with plugin %ld\n",
	       obs_get_version_string(), baseline, with_plugin);

	char want[64];
	snprintf(want, sizeof(want), "[parallaxa] loaded v%s (protocol %d)", STAGE_VERSION,
		 STAGE_PROTOCOL_VERSION);
	if (!saw_loaded || !strstr(loaded_line, want)) {
		fprintf(stderr, "FAIL expected log line '%s'\n", want);
		failures++;
	}
	if (!saw_unloaded) {
		fprintf(stderr, "FAIL obs_module_unload did not run\n");
		failures++;
	}
	if (with_plugin != baseline) {
		fprintf(stderr, "FAIL plugin leaked %ld allocation(s)\n", with_plugin - baseline);
		failures++;
	}
	if (errors_logged) {
		fprintf(stderr, "FAIL %d [parallaxa] error line(s) logged\n", errors_logged);
		failures++;
	}
	(void)saw_frontend_warning;
	printf("%s\n", failures ? "obs_load_smoke: FAILED" : "obs_load_smoke: OK");
	return failures ? 1 : 0;
}
