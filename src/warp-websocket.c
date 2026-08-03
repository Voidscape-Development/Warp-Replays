/*
Warp
Copyright (C) 2026 Voidscape Development

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <obs-module.h>
#include <plugin-support.h>

#include <media-playback/media-playback.h>

#include "obs-websocket-api.h"
#include "warp-events.h"
#include "warp-websocket.h"

#ifdef WARP_HAVE_FRONTEND_API
#include "warp-flow.h"
#endif

/* The vendor obs-websocket clients name in a CallVendorRequest, alongside one
 * of the request types below:
 *
 *   {"vendorName": "warp", "requestType": "SpeedUp",
 *    "requestData": {"sourceName": "Replay"}}
 *
 * Every request works on one Warp source, named by "sourceName" (or by
 * "sourceUuid"), and answers with "success", the error when there is one, and
 * where playback stands once the request has been carried out. */
#define WARP_WS_VENDOR "warp"

/* what a request does with the source it names; the value a request carries is
 * read from its request data */
enum warp_ws_action {
	WARP_WS_PLAY,
	WARP_WS_PAUSE,
	WARP_WS_TOGGLE_PLAY_PAUSE,
	WARP_WS_STOP,
	WARP_WS_RESTART,
	WARP_WS_SET_CURSOR,
	WARP_WS_SET_SPEED,
	WARP_WS_SPEED_UP,
	WARP_WS_SLOW_DOWN,
	WARP_WS_RESET_SPEED,
	WARP_WS_STEP_FORWARD,
	WARP_WS_STEP_BACKWARD,
	WARP_WS_NEXT,
	WARP_WS_PREVIOUS,
	WARP_WS_FIRST,
	WARP_WS_RESTART_CURRENT,
	WARP_WS_CLEAR_PLAYLIST,
	/* reports where playback stands without changing anything: every
	 * response carries that, so this request has nothing else to do */
	WARP_WS_GET_STATUS,
};

struct warp_ws_request {
	const char *type;
	enum warp_ws_action action;
	/* the source this request needs, when it only fits one of the two */
	const char *source_id;
};

/* One request per action the sources have, named after the hotkey it stands
 * in for. The hotkeys that differ only in how far they move - the speed
 * presets, and the four step sizes each way - are the same request here, with
 * the value in the request data. */
static struct warp_ws_request warp_ws_requests[] = {
	{"Play", WARP_WS_PLAY, NULL},
	{"Pause", WARP_WS_PAUSE, NULL},
	{"TogglePlayPause", WARP_WS_TOGGLE_PLAY_PAUSE, NULL},
	{"Stop", WARP_WS_STOP, NULL},
	{"Restart", WARP_WS_RESTART, NULL},
	{"SetCursor", WARP_WS_SET_CURSOR, NULL},
	{"SetSpeed", WARP_WS_SET_SPEED, NULL},
	{"SpeedUp", WARP_WS_SPEED_UP, NULL},
	{"SlowDown", WARP_WS_SLOW_DOWN, NULL},
	{"ResetSpeed", WARP_WS_RESET_SPEED, NULL},
	{"StepForward", WARP_WS_STEP_FORWARD, NULL},
	{"StepBackward", WARP_WS_STEP_BACKWARD, NULL},
	{"Next", WARP_WS_NEXT, WARP_PLAYLIST_SOURCE_ID},
	{"Previous", WARP_WS_PREVIOUS, WARP_PLAYLIST_SOURCE_ID},
	{"First", WARP_WS_FIRST, WARP_PLAYLIST_SOURCE_ID},
	{"RestartCurrent", WARP_WS_RESTART_CURRENT, WARP_PLAYLIST_SOURCE_ID},
	{"ClearPlaylist", WARP_WS_CLEAR_PLAYLIST, WARP_PLAYLIST_SOURCE_ID},
	{"GetStatus", WARP_WS_GET_STATUS, NULL},
};

static obs_websocket_vendor warp_ws_vendor = NULL;

/* ------------------------------------------------------------------------- */
/* answering a request */

static void warp_ws_fail(obs_data_t *response, const char *format, ...)
{
	char error[256];
	va_list args;

	va_start(args, format);
	vsnprintf(error, sizeof(error), format, args);
	va_end(args);

	obs_data_set_bool(response, "success", false);
	obs_data_set_string(response, "error", error);
}

/* The Warp source the request names, with a reference the caller releases, or
 * NULL with the response saying what was wrong with it. */
static obs_source_t *warp_ws_get_source(obs_data_t *request, obs_data_t *response, const struct warp_ws_request *req)
{
	const char *uuid = obs_data_get_string(request, "sourceUuid");
	const char *name = obs_data_get_string(request, "sourceName");
	obs_source_t *source;

	if (uuid && *uuid) {
		source = obs_get_source_by_uuid(uuid);
	} else if (name && *name) {
		source = obs_get_source_by_name(name);
	} else {
		warp_ws_fail(response, "sourceName or sourceUuid is required");
		return NULL;
	}

	if (!source) {
		warp_ws_fail(response, "there is no source called '%s'", (uuid && *uuid) ? uuid : name);
		return NULL;
	}

	const char *id = obs_source_get_id(source);

	if (!id || (strcmp(id, WARP_MEDIA_SOURCE_ID) != 0 && strcmp(id, WARP_PLAYLIST_SOURCE_ID) != 0)) {
		warp_ws_fail(response, "'%s' is not a Warp Media or Warp Playlist source", obs_source_get_name(source));
		obs_source_release(source);
		return NULL;
	}

	if (req->source_id && strcmp(id, req->source_id) != 0) {
		warp_ws_fail(response, "%s needs a %s source", req->type, obs_source_get_display_name(req->source_id));
		obs_source_release(source);
		return NULL;
	}

	return source;
}

/* an int field of the request, with 'fallback' when the client left it out */
static long long warp_ws_field(obs_data_t *request, const char *field, long long fallback)
{
	return obs_data_has_user_value(request, field) ? obs_data_get_int(request, field) : fallback;
}

static void warp_ws_call(obs_source_t *source, const char *proc)
{
	calldata_t cd = {0};

	calldata_init(&cd);
	proc_handler_call(obs_source_get_proc_handler(source), proc, &cd);
	calldata_free(&cd);
}

static void warp_ws_call_int(obs_source_t *source, const char *proc, const char *arg, long long value)
{
	calldata_t cd = {0};

	calldata_init(&cd);
	calldata_set_int(&cd, arg, value);
	proc_handler_call(obs_source_get_proc_handler(source), proc, &cd);
	calldata_free(&cd);
}

/* the same names obs-websocket's own GetMediaInputStatus reports */
static const char *warp_ws_media_state(enum obs_media_state state)
{
	switch (state) {
	case OBS_MEDIA_STATE_NONE:
		return "OBS_MEDIA_STATE_NONE";
	case OBS_MEDIA_STATE_PLAYING:
		return "OBS_MEDIA_STATE_PLAYING";
	case OBS_MEDIA_STATE_OPENING:
		return "OBS_MEDIA_STATE_OPENING";
	case OBS_MEDIA_STATE_BUFFERING:
		return "OBS_MEDIA_STATE_BUFFERING";
	case OBS_MEDIA_STATE_PAUSED:
		return "OBS_MEDIA_STATE_PAUSED";
	case OBS_MEDIA_STATE_STOPPED:
		return "OBS_MEDIA_STATE_STOPPED";
	case OBS_MEDIA_STATE_ENDED:
		return "OBS_MEDIA_STATE_ENDED";
	case OBS_MEDIA_STATE_ERROR:
		return "OBS_MEDIA_STATE_ERROR";
	}

	return "OBS_MEDIA_STATE_NONE";
}

/* Where playback stands, put on every response so that a control surface can
 * follow the source without asking again. */
static void warp_ws_add_status(obs_source_t *source, obs_data_t *response)
{
	const char *id = obs_source_get_id(source);
	calldata_t cd = {0};

	obs_data_set_string(response, "sourceName", obs_source_get_name(source));
	obs_data_set_string(response, "sourceUuid", obs_source_get_uuid(source));
	obs_data_set_string(response, "sourceKind", id);
	obs_data_set_string(response, "mediaState", warp_ws_media_state(obs_source_media_get_state(source)));
	obs_data_set_int(response, "cursor", obs_source_media_get_time(source));
	obs_data_set_int(response, "duration", obs_source_media_get_duration(source));

	calldata_init(&cd);
	if (proc_handler_call(obs_source_get_proc_handler(source), "warp_get_speed", &cd))
		obs_data_set_int(response, "speed", calldata_int(&cd, "speed"));
	calldata_free(&cd);

	if (!id || strcmp(id, WARP_PLAYLIST_SOURCE_ID) != 0)
		return;

	calldata_init(&cd);
	if (proc_handler_call(obs_source_get_proc_handler(source), "warp_playlist_status", &cd)) {
		obs_data_set_int(response, "playlistIndex", calldata_int(&cd, "index"));
		obs_data_set_int(response, "playlistLength", calldata_int(&cd, "count"));
		obs_data_set_string(response, "currentFile", calldata_string(&cd, "current_file"));
	}
	calldata_free(&cd);
}

/* Carries out the request. Returns false with the response filled in when the
 * value the request came with is not one the action can be given.
 *
 * The requests do what the hotkeys do, with one difference: a hotkey only
 * applies to a source that is on screen, because the operator is pressing it
 * at whatever is in front of them, while a request names the source it means
 * and so is carried out whether or not it is being shown. Restarting a Warp
 * Media source is the exception, as the source itself only restarts playback
 * while it is being shown. */
static bool warp_ws_run(const struct warp_ws_request *req, obs_source_t *source, obs_data_t *request,
			obs_data_t *response)
{
	switch (req->action) {
	case WARP_WS_PLAY:
		obs_source_media_play_pause(source, false);
		return true;
	case WARP_WS_PAUSE:
		obs_source_media_play_pause(source, true);
		return true;
	case WARP_WS_TOGGLE_PLAY_PAUSE:
		obs_source_media_play_pause(source, obs_source_media_get_state(source) == OBS_MEDIA_STATE_PLAYING);
		return true;
	case WARP_WS_STOP:
		obs_source_media_stop(source);
		return true;
	case WARP_WS_RESTART:
		obs_source_media_restart(source);
		return true;
	case WARP_WS_SET_CURSOR: {
		long long cursor = warp_ws_field(request, "cursor", -1);

		if (cursor < 0) {
			warp_ws_fail(response, "cursor must be a position in the file, in milliseconds");
			return false;
		}

		obs_source_media_set_time(source, cursor);
		return true;
	}
	case WARP_WS_SET_SPEED: {
		long long speed = warp_ws_field(request, "speed", 0);

		if (speed < MP_SPEED_MIN || speed > MP_SPEED_MAX) {
			warp_ws_fail(response, "speed must be between %d and %d percent", MP_SPEED_MIN, MP_SPEED_MAX);
			return false;
		}

		warp_ws_call_int(source, "warp_set_speed", "speed", speed);
		return true;
	}
	case WARP_WS_SPEED_UP:
	case WARP_WS_SLOW_DOWN: {
		long long amount = warp_ws_field(request, "amount", WARP_SPEED_STEP);

		if (amount <= 0) {
			warp_ws_fail(response, "amount must be a number of percentage points to move the speed by");
			return false;
		}

		warp_ws_call_int(source, "warp_adjust_speed", "delta",
				 req->action == WARP_WS_SPEED_UP ? amount : -amount);
		return true;
	}
	case WARP_WS_RESET_SPEED:
		warp_ws_call_int(source, "warp_set_speed", "speed", 100);
		return true;
	case WARP_WS_STEP_FORWARD:
	case WARP_WS_STEP_BACKWARD: {
		long long frames = warp_ws_field(request, "frames", 1);

		if (frames <= 0) {
			warp_ws_fail(response, "frames must be a number of frames to step by");
			return false;
		}

		warp_ws_call_int(source, "warp_step_frames", "frames",
				 req->action == WARP_WS_STEP_FORWARD ? frames : -frames);
		return true;
	}
	case WARP_WS_NEXT:
		obs_source_media_next(source);
		return true;
	case WARP_WS_PREVIOUS:
		obs_source_media_previous(source);
		return true;
	case WARP_WS_FIRST:
		warp_ws_call(source, "warp_playlist_first");
		return true;
	case WARP_WS_RESTART_CURRENT:
		warp_ws_call(source, "warp_playlist_restart_current");
		return true;
	case WARP_WS_CLEAR_PLAYLIST:
		warp_ws_call(source, "warp_playlist_clear");
		return true;
	case WARP_WS_GET_STATUS:
		return true;
	}

	return true;
}

#ifdef WARP_HAVE_FRONTEND_API

/* ------------------------------------------------------------------------- */
/* the flows
 *
 * These name a flow rather than a source, with "flowName" or "flowId", and do
 * what its hotkeys do:
 *
 *   {"vendorName": "warp", "requestType": "SaveToFlow",
 *    "requestData": {"flowName": "Match Replays"}} */

enum warp_ws_flow_action {
	WARP_WS_FLOW_SAVE,
	WARP_WS_FLOW_ADD_LAST,
	WARP_WS_FLOW_LIST,
};

struct warp_ws_flow_request {
	const char *type;
	enum warp_ws_flow_action action;
};

static struct warp_ws_flow_request warp_ws_flow_requests[] = {
	{"SaveToFlow", WARP_WS_FLOW_SAVE},
	{"AddLastReplayToFlow", WARP_WS_FLOW_ADD_LAST},
	{"GetFlows", WARP_WS_FLOW_LIST},
};

/* the flow the request names, which the caller releases, or NULL with the
 * response saying what was wrong with it */
static obs_data_t *warp_ws_get_flow(obs_data_t *request, obs_data_t *response)
{
	const char *id = obs_data_get_string(request, "flowId");
	const char *name = obs_data_get_string(request, "flowName");
	obs_data_t *flow;

	if (id && *id) {
		flow = warp_flow_get(id);
	} else if (name && *name) {
		flow = warp_flow_get_by_name(name);
	} else {
		warp_ws_fail(response, "flowName or flowId is required");
		return NULL;
	}

	if (!flow) {
		warp_ws_fail(response, "there is no flow called '%s'", (id && *id) ? id : name);
		return NULL;
	}

	return flow;
}

static void warp_ws_add_flow_status(obs_data_t *flow, obs_data_t *response)
{
	const char *id = obs_data_get_string(flow, WARP_FLOW_ID);

	obs_data_set_string(response, "flowId", id);
	obs_data_set_string(response, "flowName", obs_data_get_string(flow, WARP_FLOW_NAME));
	obs_data_set_string(response, "flowKind", obs_data_get_string(flow, WARP_FLOW_KIND));
	obs_data_set_string(response, "targetName", obs_data_get_string(flow, WARP_FLOW_TARGET_NAME));
	obs_data_set_int(response, "clipCount", warp_flow_clip_count(id));
}

static void warp_ws_flow_cb(obs_data_t *request, obs_data_t *response, void *priv_data)
{
	const struct warp_ws_flow_request *req = priv_data;

	if (req->action == WARP_WS_FLOW_LIST) {
		obs_data_array_t *flows = warp_flow_list();
		obs_data_array_t *listed = obs_data_array_create();
		size_t count = obs_data_array_count(flows);

		for (size_t i = 0; i < count; i++) {
			obs_data_t *flow = obs_data_array_item(flows, i);
			obs_data_t *entry = obs_data_create();

			warp_ws_add_flow_status(flow, entry);
			obs_data_array_push_back(listed, entry);

			obs_data_release(entry);
			obs_data_release(flow);
		}

		obs_data_set_array(response, "flows", listed);
		obs_data_set_bool(response, "success", true);

		obs_data_array_release(listed);
		obs_data_array_release(flows);
		return;
	}

	obs_data_t *flow = warp_ws_get_flow(request, response);

	if (!flow)
		return;

	const char *id = obs_data_get_string(flow, WARP_FLOW_ID);
	bool carried_out;

	if (req->action == WARP_WS_FLOW_SAVE) {
		carried_out = warp_flow_save_replay(id);

		if (!carried_out)
			warp_ws_fail(response, "the replay buffer is not running");
	} else {
		carried_out = warp_flow_promote_last(id);

		if (!carried_out)
			warp_ws_fail(response, "there is no saved replay to add");
	}

	if (carried_out) {
		obs_data_set_bool(response, "success", true);
		warp_ws_add_flow_status(flow, response);
	}

	obs_data_release(flow);
}

#endif /* WARP_HAVE_FRONTEND_API */

/* ------------------------------------------------------------------------- */

static void warp_ws_request_cb(obs_data_t *request, obs_data_t *response, void *priv_data)
{
	const struct warp_ws_request *req = priv_data;
	obs_source_t *source = warp_ws_get_source(request, response, req);

	if (!source)
		return;

	if (warp_ws_run(req, source, request, response)) {
		obs_data_set_bool(response, "success", true);
		warp_ws_add_status(source, response);
	}

	obs_source_release(source);
}

/* ------------------------------------------------------------------------- */

void warp_websocket_register(void)
{
	unsigned int api_version = obs_websocket_get_api_version();

	if (!api_version) {
		obs_log(LOG_INFO, "obs-websocket not found, the Warp vendor requests are unavailable");
		return;
	}

	warp_ws_vendor = obs_websocket_register_vendor(WARP_WS_VENDOR);

	if (!warp_ws_vendor) {
		obs_log(LOG_WARNING, "could not register the '%s' obs-websocket vendor", WARP_WS_VENDOR);
		return;
	}

	size_t registered = 0;

	for (size_t i = 0; i < sizeof(warp_ws_requests) / sizeof(*warp_ws_requests); i++) {
		struct warp_ws_request *req = &warp_ws_requests[i];

		if (obs_websocket_vendor_register_request(warp_ws_vendor, req->type, warp_ws_request_cb, req))
			registered++;
		else
			obs_log(LOG_WARNING, "could not register the '%s' vendor request", req->type);
	}

#ifdef WARP_HAVE_FRONTEND_API
	for (size_t i = 0; i < sizeof(warp_ws_flow_requests) / sizeof(*warp_ws_flow_requests); i++) {
		struct warp_ws_flow_request *req = &warp_ws_flow_requests[i];

		if (obs_websocket_vendor_register_request(warp_ws_vendor, req->type, warp_ws_flow_cb, req))
			registered++;
		else
			obs_log(LOG_WARNING, "could not register the '%s' vendor request", req->type);
	}
#endif

	/* The requests are left registered at unload: obs-websocket drops its
	 * vendors itself, and its proc handler may already be gone by the time
	 * this module is unloaded. */
	obs_log(LOG_INFO, "registered %d obs-websocket vendor requests under '%s' (obs-websocket API v%u)",
		(int)registered, WARP_WS_VENDOR, api_version);
}
