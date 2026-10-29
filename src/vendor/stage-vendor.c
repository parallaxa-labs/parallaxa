/*
 * stage-vendor.c — obs-websocket vendor "parallaxa": registration,
 * shutdown and event emission.
 *
 * Copyright (C) 2026 Parallaxa Labs
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Lifecycle:
 *   obs_module_post_load -> stage_vendor_init(): registers the vendor and
 *     every request. When obs-websocket is missing or OBS runs in Safe Mode,
 *     registration fails: one warning, the plugin keeps working. When
 *     obs-websocket is loaded but its server is disabled (Tools > WebSocket
 *     Server Settings), registration SUCCEEDS (the vendor table lives in the
 *     plugin, not the server), so the server_enabled flag is read from
 *     obs-websocket's config file and a disabled server gets the one
 *     warning instead. Enabling the server later needs no restart.
 *   OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN / EXIT -> requests unregistered
 *     while obs-websocket is certainly still loaded, every call guarded by
 *     obs_websocket_get_api_version().
 *   obs_module_unload -> stage_vendor_shutdown() again (idempotent). Module
 *     unload order is not guaranteed, so by then the vendor is normally
 *     already detached and nothing calls into obs-websocket.
 *
 * This is the only translation unit that includes obs-websocket-api.h (the
 * header keeps its proc handler in a file-static variable).
 */
#include <assert.h>
#include <obs-module.h>
#include <pthread.h>
#include <string.h>

#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <obs-websocket-api.h>

#include "stage-frontend.h"
#include "stage-modules.h"
#include "stage-vendor.h"

static pthread_mutex_t vendor_lock = PTHREAD_MUTEX_INITIALIZER;
static obs_websocket_vendor vendor;       /* guarded by vendor_lock */
static stage_vendor_emit_fn sink_fn;      /* test sink, guarded by vendor_lock */
static void *sink_ud;
static bool initialized, detached;

/* obs-websocket request callback; priv is the request name. */
static void request_cb(obs_data_t *request_data, obs_data_t *response_data, void *priv)
{
	stage_vendor_dispatch((const char *)priv, request_data, response_data);
}

void stage_vendor_set_emit_sink(stage_vendor_emit_fn fn, void *ud)
{
	pthread_mutex_lock(&vendor_lock);
	sink_fn = fn;
	sink_ud = ud;
	pthread_mutex_unlock(&vendor_lock);
}

bool stage_vendor_can_emit(void)
{
	pthread_mutex_lock(&vendor_lock);
	bool ok = sink_fn != NULL || vendor != NULL;
	pthread_mutex_unlock(&vendor_lock);
	return ok;
}

bool stage_vendor_emit(const char *event_type, obs_data_t *data)
{
	bool ok = false;
	/* Held while emitting so a concurrent detach cannot pull the vendor
	 * from under the call. obs-websocket never calls back into us here. */
	pthread_mutex_lock(&vendor_lock);
	if (sink_fn) {
		sink_fn(event_type, data, sink_ud);
		ok = true;
	} else if (vendor) {
		ok = obs_websocket_vendor_emit_event(vendor, event_type, data);
	}
	pthread_mutex_unlock(&vendor_lock);
	return ok;
}

/* Unregisters the requests (guarded) and stops the events. Idempotent. */
static void vendor_detach(void)
{
	if (detached)
		return;
	detached = true;
	stage_vendor_events_stop();

	pthread_mutex_lock(&vendor_lock);
	obs_websocket_vendor v = vendor;
	vendor = NULL;
	pthread_mutex_unlock(&vendor_lock);

	if (v && obs_websocket_get_api_version() > 0) {
		for (size_t i = 0; i < STAGE_REQUEST_COUNT; i++)
			obs_websocket_vendor_unregister_request(v, stage_request_names[i]);
		SLOG_INFO("obs-websocket vendor '%s' detached", STAGE_VENDOR_NAME);
	}
	stage_vendor_events_reset();
}

int stage_vendor_ws_server_enabled(const char *config_json_path)
{
	if (!config_json_path || !config_json_path[0] || !os_file_exists(config_json_path))
		return -1;
	obs_data_t *d = obs_data_create_from_json_file(config_json_path);
	if (!d)
		return -1;
	int r = -1;
	if (obs_data_has_user_value(d, "server_enabled"))
		r = obs_data_get_bool(d, "server_enabled") ? 1 : 0;
	obs_data_release(d);
	return r;
}

/* obs-websocket (>= 5.5) keeps its settings in
 * <plugin_config>/obs-websocket/config.json, a sibling of our own module
 * config folder. -1 when unknown. */
static int ws_server_state(void)
{
	char *own = obs_module_config_path("");
	if (!own)
		return -1;
	struct dstr path = {0};
	dstr_copy(&path, own);
	bfree(own);
	size_t n = path.len;
	while (n && (path.array[n - 1] == '/' || path.array[n - 1] == '\\'))
		n--; /* trailing separators */
	while (n && path.array[n - 1] != '/' && path.array[n - 1] != '\\')
		n--; /* our module folder name */
	int r = -1;
	if (n) {
		dstr_resize(&path, n);
		dstr_cat(&path, "obs-websocket/config.json");
		r = stage_vendor_ws_server_enabled(path.array);
	}
	dstr_free(&path);
	return r;
}

static void on_frontend_event(enum obs_frontend_event event, void *ud)
{
	(void)ud;
	if (event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN || event == OBS_FRONTEND_EVENT_EXIT)
		vendor_detach();
}

static void hotkeys_unload(void *ud)
{
	(void)ud;
	stage_hotkeys_unregister();
}

void stage_vendor_init(void)
{
	if (initialized)
		return;
	initialized = true;
	detached = false;

	/* Hotkeys work without obs-websocket (only "AI Live Studio" ones need it).
	 * Registered here, after every module's obs_module_load, so the
	 * duplicate check sees the Camera Rig's own hotkeys. */
	stage_hotkeys_register();
	stage_core_on_unload(hotkeys_unload, NULL);
	stage_frontend_add_event_cb(on_frontend_event, NULL);

	obs_websocket_vendor v = obs_websocket_register_vendor(STAGE_VENDOR_NAME);
	if (!v) {
		SLOG_WARN("obs-websocket is not available (missing or Safe Mode); "
			  "remote control of Parallaxa is off");
		return;
	}
	size_t ok = 0;
	for (size_t i = 0; i < STAGE_REQUEST_COUNT; i++) {
		if (obs_websocket_vendor_register_request(v, stage_request_names[i], request_cb,
							  (void *)stage_request_names[i]))
			ok++;
		else
			SLOG_WARN("obs-websocket: could not register request '%s'",
				  stage_request_names[i]);
	}
	pthread_mutex_lock(&vendor_lock);
	vendor = v;
	pthread_mutex_unlock(&vendor_lock);
	stage_vendor_events_start();
	if (ws_server_state() == 0)
		SLOG_WARN("obs-websocket server is disabled (Tools > WebSocket Server Settings); "
			  "remote control of Parallaxa is off until it is enabled");
	SLOG_INFO("obs-websocket vendor '%s' registered (%zu/%zu requests, api v%u)",
		  STAGE_VENDOR_NAME, ok, (size_t)STAGE_REQUEST_COUNT, obs_websocket_get_api_version());
}

void stage_vendor_shutdown(void)
{
	if (!initialized)
		return;
	vendor_detach();
	stage_frontend_remove_event_cb(on_frontend_event, NULL);
}
