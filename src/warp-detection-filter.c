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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <media-playback/media-playback.h>

#include "warp-events.h"
#include "warp-zoom.h"

#define WARP_DETECT_LOG(level, format, ...) \
	blog(level, "[Warp Detection '%s']: " format, obs_source_get_name(f->source), ##__VA_ARGS__)

/* settings */
#define S_TARGET "warp_source"
#define S_EVENT "event"
#define S_SPEED_VALUE "speed_value"
#define S_FRAME_VALUE "frame_value"
#define S_ANY_VALUE "any_value"
#define S_ACTION "action"
#define S_GLOBAL_HOTKEY "global_hotkey"
#define S_HOTKEY_SOURCE "hotkey_source"
#define S_SOURCE_HOTKEY "source_hotkey"
#define S_FILTER_SOURCE "filter_source"
#define S_FILTER_NAME "filter_name"
#define S_FILTER_MODE "filter_mode"
#define S_FILTER_HOTKEY "filter_hotkey"
#define S_PRESET_VALUE "preset_value"
#define S_ZOOM_SOURCE "zoom_source"
#define S_ZOOM_FILTER "zoom_filter"
#define S_ZOOM_PRESET "zoom_preset"

/* The event listened for. Each speed and stepping hotkey has an event of its
 * own, saved as the kind with the hotkey's value on the end - "speed_set_50",
 * "step_forward_5". The bare kinds are the general ones, which take the value
 * to match from the properties instead. */
#define EVENT_SPEED_SET "speed_set"
#define EVENT_SPEED_UP "speed_up"
#define EVENT_SPEED_DOWN "speed_down"
#define EVENT_SPEED_INCREASE "speed_increase"
#define EVENT_SPEED_DECREASE "speed_decrease"
#define EVENT_STEP_FORWARD "step_forward"
#define EVENT_STEP_BACKWARD "step_backward"
#define EVENT_MEDIA_PLAY "media_play"
#define EVENT_MEDIA_PAUSE "media_pause"
#define EVENT_MEDIA_RESTART "media_restart"
#define EVENT_MEDIA_LOADED "media_loaded"
#define EVENT_ZOOM_CHANGED "zoom_changed"
#define EVENT_ZOOM_PRESET "zoom_preset"
#define EVENT_ZOOM_RESET "zoom_reset"

/* the speed Reset Speed sets, which has an event like the presets do */
#define WARP_DETECT_RESET_SPEED 100

enum warp_detect_kind {
	WARP_DETECT_KIND_NONE,
	WARP_DETECT_KIND_SPEED_SET,
	WARP_DETECT_KIND_SPEED_UP,
	WARP_DETECT_KIND_SPEED_DOWN,
	WARP_DETECT_KIND_SPEED_INCREASE,
	WARP_DETECT_KIND_SPEED_DECREASE,
	WARP_DETECT_KIND_STEP_FORWARD,
	WARP_DETECT_KIND_STEP_BACKWARD,
	WARP_DETECT_KIND_MEDIA_PLAY,
	WARP_DETECT_KIND_MEDIA_PAUSE,
	WARP_DETECT_KIND_MEDIA_RESTART,
	WARP_DETECT_KIND_MEDIA_LOADED,
	WARP_DETECT_KIND_ZOOM_CHANGED,
	WARP_DETECT_KIND_ZOOM_PRESET,
	WARP_DETECT_KIND_ZOOM_RESET,
};

/* what is done about it */
#define ACTION_GLOBAL_HOTKEY "global_hotkey"
#define ACTION_SOURCE_HOTKEY "source_hotkey"
#define ACTION_FILTER "filter"
#define ACTION_ZOOM_PRESET "zoom_preset"

#define FILTER_MODE_ENABLE "enable"
#define FILTER_MODE_DISABLE "disable"
#define FILTER_MODE_TOGGLE "toggle"
#define FILTER_MODE_HOTKEY "hotkey"

/* how often the source being listened to is looked up again, in seconds, so a
 * filter set up before its source exists finds it on its own */
#define WARP_DETECT_RESOLVE_SECONDS 0.5f

/* An action that drives the source it listens to builds a loop the operator did
 * not mean to build. A filter never reacts to an event its own action caused,
 * which covers a filter chasing itself; this is the backstop for a pair of
 * filters chasing each other. */
#define WARP_DETECT_MAX_PER_SECOND 20

struct warp_detect_filter {
	obs_source_t *source;

	/* the source being listened to; only ever touched from the tick and
	 * from destroy, never with the mutex held */
	obs_weak_source_t *listening_to;
	float resolve_delay;
	volatile bool needs_resolve;

	/* set while an action is on its way to the UI thread and running there */
	volatile bool triggering;

	pthread_mutex_t mutex;
	/* the event that set 'triggering', for the line the action logs */
	char event_desc[96];
	/* runaway brake */
	uint64_t window_start;
	int window_count;
};

static const char *warp_detect_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Warp.DetectionFilter.Name");
}

/* ------------------------------------------------------------------------- */
/* hotkeys */

/* A hotkey belongs either to a source or to OBS itself - its frontend, an
 * output, an encoder or a service - which are the ones the hotkey menu lists
 * outside of any source. */
static bool warp_detect_hotkey_owned_by(obs_hotkey_t *key, obs_source_t *owner)
{
	bool of_source = obs_hotkey_get_registerer_type(key) == OBS_HOTKEY_REGISTERER_SOURCE;

	if (!owner)
		return !of_source;

	if (!of_source)
		return false;

	obs_weak_source_t *registerer = obs_hotkey_get_registerer(key);

	return registerer && obs_weak_source_references_source(registerer, owner);
}

struct warp_detect_hotkey_search {
	const char *name;
	obs_source_t *owner;
	obs_hotkey_id id;
};

static bool warp_detect_hotkey_search_cb(void *data, obs_hotkey_id id, obs_hotkey_t *key)
{
	struct warp_detect_hotkey_search *search = data;
	const char *name = obs_hotkey_get_name(key);

	if (!name || strcmp(name, search->name) != 0 || !warp_detect_hotkey_owned_by(key, search->owner))
		return true;

	search->id = id;
	return false;
}

/* Hotkey ids are handed out at registration and differ from one run of OBS to
 * the next, so what is saved is the hotkey's name, along with the source it
 * belongs to when it has one (sources share hotkey names such as "libobs.mute").
 * This looks the id back up. */
static obs_hotkey_id warp_detect_find_hotkey(const char *name, obs_source_t *owner)
{
	struct warp_detect_hotkey_search search = {
		.name = name,
		.owner = owner,
		.id = OBS_INVALID_HOTKEY_ID,
	};

	if (!name || !*name)
		return OBS_INVALID_HOTKEY_ID;

	obs_enum_hotkeys(warp_detect_hotkey_search_cb, &search);

	return search.id;
}

/* Presses the hotkey and lets it go, the way the key itself would. Enumerating
 * hotkeys holds the lock this takes as well, so it has to run once the search
 * has finished rather than from inside it. */
static void warp_detect_trigger_hotkey(obs_hotkey_id id)
{
	obs_hotkey_trigger_routed_callback(id, true);
	obs_hotkey_trigger_routed_callback(id, false);
}

/* ------------------------------------------------------------------------- */
/* acting on an event */

static void warp_detect_do_hotkey(struct warp_detect_filter *f, const char *event, const char *name,
				  obs_source_t *owner)
{
	if (!name || !*name)
		return;

	obs_hotkey_id id = warp_detect_find_hotkey(name, owner);

	if (id == OBS_INVALID_HOTKEY_ID) {
		WARP_DETECT_LOG(LOG_WARNING, "%s: no hotkey '%s'%s%s", event, name, owner ? " on " : "",
				owner ? obs_source_get_name(owner) : "");
		return;
	}

	warp_detect_trigger_hotkey(id);

	WARP_DETECT_LOG(LOG_INFO, "%s: triggered hotkey '%s'", event, name);
}

static void warp_detect_do_filter(struct warp_detect_filter *f, const char *event, obs_data_t *settings)
{
	const char *source_name = obs_data_get_string(settings, S_FILTER_SOURCE);
	const char *filter_name = obs_data_get_string(settings, S_FILTER_NAME);
	const char *mode = obs_data_get_string(settings, S_FILTER_MODE);

	if (!*source_name || !*filter_name)
		return;

	obs_source_t *owner = obs_get_source_by_name(source_name);

	if (!owner) {
		WARP_DETECT_LOG(LOG_WARNING, "%s: no source named '%s'", event, source_name);
		return;
	}

	obs_source_t *target = obs_source_get_filter_by_name(owner, filter_name);

	if (!target) {
		WARP_DETECT_LOG(LOG_WARNING, "%s: '%s' has no filter named '%s'", event, source_name, filter_name);
		obs_source_release(owner);
		return;
	}

	if (strcmp(mode, FILTER_MODE_HOTKEY) == 0) {
		warp_detect_do_hotkey(f, event, obs_data_get_string(settings, S_FILTER_HOTKEY), target);
	} else {
		bool enable = strcmp(mode, FILTER_MODE_TOGGLE) == 0 ? !obs_source_enabled(target)
								    : strcmp(mode, FILTER_MODE_ENABLE) == 0;

		obs_source_set_enabled(target, enable);

		WARP_DETECT_LOG(LOG_INFO, "%s: %s filter '%s' on '%s'", event, enable ? "enabled" : "disabled",
				filter_name, source_name);
	}

	obs_source_release(target);
	obs_source_release(owner);
}

/* The zoom a filter's action frames: the source it names when that is a Warp
 * source or a Warp Zoom filter, the filter it names on it, or the Warp Zoom
 * filter that source carries. The caller releases the reference. */
static obs_source_t *warp_detect_zoom_target(obs_source_t *owner, const char *filter_name)
{
	if (filter_name && *filter_name) {
		obs_source_t *filter = obs_source_get_filter_by_name(owner, filter_name);

		if (filter && warp_zoom_source_capable(filter))
			return filter;

		obs_source_release(filter);
		return NULL;
	}

	if (warp_zoom_source_capable(owner))
		return obs_source_get_ref(owner);

	return warp_zoom_filter_find_operable(owner);
}

static void warp_detect_do_zoom(struct warp_detect_filter *f, const char *event, obs_data_t *settings)
{
	const char *source_name = obs_data_get_string(settings, S_ZOOM_SOURCE);
	const char *filter_name = obs_data_get_string(settings, S_ZOOM_FILTER);
	const char *preset = obs_data_get_string(settings, S_ZOOM_PRESET);
	obs_source_t *owner;
	obs_source_t *zoom;

	if (!*source_name || !*preset)
		return;

	owner = obs_get_source_by_name(source_name);

	if (!owner) {
		WARP_DETECT_LOG(LOG_WARNING, "%s: no source named '%s'", event, source_name);
		return;
	}

	zoom = warp_detect_zoom_target(owner, filter_name);

	if (!zoom) {
		WARP_DETECT_LOG(LOG_WARNING, "%s: '%s' cannot be zoomed", event, source_name);
		obs_source_release(owner);
		return;
	}

	if (warp_zoom_source_recall(zoom, preset))
		WARP_DETECT_LOG(LOG_INFO, "%s: framed '%s' with zoom preset '%s'", event, source_name, preset);
	else
		WARP_DETECT_LOG(LOG_WARNING, "%s: '%s' has no zoom preset called '%s'", event, source_name, preset);

	obs_source_release(zoom);
	obs_source_release(owner);
}

static void warp_detect_perform(struct warp_detect_filter *f)
{
	obs_data_t *settings = obs_source_get_settings(f->source);
	const char *action = obs_data_get_string(settings, S_ACTION);
	char event[sizeof(f->event_desc)];

	pthread_mutex_lock(&f->mutex);
	snprintf(event, sizeof(event), "%s", f->event_desc);
	pthread_mutex_unlock(&f->mutex);

	if (strcmp(action, ACTION_GLOBAL_HOTKEY) == 0) {
		warp_detect_do_hotkey(f, event, obs_data_get_string(settings, S_GLOBAL_HOTKEY), NULL);
	} else if (strcmp(action, ACTION_SOURCE_HOTKEY) == 0) {
		const char *source_name = obs_data_get_string(settings, S_HOTKEY_SOURCE);

		if (*source_name) {
			obs_source_t *owner = obs_get_source_by_name(source_name);

			if (owner) {
				warp_detect_do_hotkey(f, event, obs_data_get_string(settings, S_SOURCE_HOTKEY), owner);
				obs_source_release(owner);
			} else {
				WARP_DETECT_LOG(LOG_WARNING, "%s: no source named '%s'", event, source_name);
			}
		}
	} else if (strcmp(action, ACTION_FILTER) == 0) {
		warp_detect_do_filter(f, event, settings);
	} else if (strcmp(action, ACTION_ZOOM_PRESET) == 0) {
		warp_detect_do_zoom(f, event, settings);
	}

	obs_data_release(settings);
}

/* what is handed to the UI thread: the filter, and the reference that keeps it
 * alive until the action has run */
struct warp_detect_task {
	struct warp_detect_filter *f;
	obs_source_t *ref;
};

static void warp_detect_run_action(void *param)
{
	struct warp_detect_task *task = param;
	struct warp_detect_filter *f = task->f;

	warp_detect_perform(f);

	/* cleared once the action, and anything it set off, is done: until then
	 * the filter ignores what its own action caused */
	os_atomic_set_bool(&f->triggering, false);

	obs_source_release(task->ref);
	bfree(task);
}

static bool warp_detect_within_rate_limit(struct warp_detect_filter *f)
{
	uint64_t now = os_gettime_ns();
	bool ok = true;

	pthread_mutex_lock(&f->mutex);

	if (now - f->window_start >= 1000000000ULL) {
		f->window_start = now;
		f->window_count = 0;
	}

	if (++f->window_count > WARP_DETECT_MAX_PER_SECOND) {
		ok = false;

		if (f->window_count == WARP_DETECT_MAX_PER_SECOND + 1)
			WARP_DETECT_LOG(LOG_WARNING,
					"more than %d triggers in a second, ignoring the rest: is this filter's "
					"action driving the source it listens to?",
					WARP_DETECT_MAX_PER_SECOND);
	}

	pthread_mutex_unlock(&f->mutex);

	return ok;
}

static void warp_detect_fire(struct warp_detect_filter *f, const char *event)
{
	/* an action of this filter's is already running: whatever it did to get
	 * this event back is not a new event */
	if (os_atomic_set_bool(&f->triggering, true))
		return;

	if (!warp_detect_within_rate_limit(f)) {
		os_atomic_set_bool(&f->triggering, false);
		return;
	}

	pthread_mutex_lock(&f->mutex);
	snprintf(f->event_desc, sizeof(f->event_desc), "%s", event);
	pthread_mutex_unlock(&f->mutex);

	obs_source_t *ref = obs_source_get_ref(f->source);

	if (!ref) {
		os_atomic_set_bool(&f->triggering, false);
		return;
	}

	struct warp_detect_task *task = bmalloc(sizeof(struct warp_detect_task));

	task->f = f;
	task->ref = ref;

	/* OBS Studio runs hotkeys on its UI thread; the action runs there too,
	 * rather than on the hotkey thread the event arrived on */
	obs_queue_task(OBS_TASK_UI, warp_detect_run_action, task, false);
}

/* ------------------------------------------------------------------------- */
/* listening */

/* Matches the saved event against one of the kinds, and says which value that
 * event is about: the one on the end of the event when it stands for a single
 * hotkey ("speed_set_50"), and the one in the properties when it is the general
 * event ("speed_set"). */
static bool warp_detect_event_is(const char *event, const char *kind, int *hotkey_value)
{
	size_t len = strlen(kind);

	if (strncmp(event, kind, len) != 0)
		return false;

	if (event[len] == '\0')
		*hotkey_value = 0;
	else if (event[len] == '_')
		*hotkey_value = atoi(event + len + 1);
	else
		return false;

	return true;
}

static enum warp_detect_kind warp_detect_event_kind(obs_data_t *settings, int *value, bool *any)
{
	const char *event = obs_data_get_string(settings, S_EVENT);
	int hotkey_value = 0;

	*value = 0;
	*any = false;

	if (warp_detect_event_is(event, EVENT_SPEED_SET, &hotkey_value)) {
		*value = hotkey_value ? hotkey_value : (int)obs_data_get_int(settings, S_SPEED_VALUE);
		*any = !hotkey_value && obs_data_get_bool(settings, S_ANY_VALUE);
		return WARP_DETECT_KIND_SPEED_SET;
	}

	if (strcmp(event, EVENT_SPEED_UP) == 0)
		return WARP_DETECT_KIND_SPEED_UP;

	if (strcmp(event, EVENT_SPEED_DOWN) == 0)
		return WARP_DETECT_KIND_SPEED_DOWN;

	if (strcmp(event, EVENT_SPEED_INCREASE) == 0)
		return WARP_DETECT_KIND_SPEED_INCREASE;

	if (strcmp(event, EVENT_SPEED_DECREASE) == 0)
		return WARP_DETECT_KIND_SPEED_DECREASE;

	if (strcmp(event, EVENT_MEDIA_PLAY) == 0)
		return WARP_DETECT_KIND_MEDIA_PLAY;

	if (strcmp(event, EVENT_MEDIA_PAUSE) == 0)
		return WARP_DETECT_KIND_MEDIA_PAUSE;

	if (strcmp(event, EVENT_MEDIA_RESTART) == 0)
		return WARP_DETECT_KIND_MEDIA_RESTART;

	if (strcmp(event, EVENT_MEDIA_LOADED) == 0)
		return WARP_DETECT_KIND_MEDIA_LOADED;

	if (strcmp(event, EVENT_ZOOM_CHANGED) == 0)
		return WARP_DETECT_KIND_ZOOM_CHANGED;

	if (strcmp(event, EVENT_ZOOM_PRESET) == 0) {
		*any = obs_data_get_bool(settings, S_ANY_VALUE);
		return WARP_DETECT_KIND_ZOOM_PRESET;
	}

	if (strcmp(event, EVENT_ZOOM_RESET) == 0)
		return WARP_DETECT_KIND_ZOOM_RESET;

	if (warp_detect_event_is(event, EVENT_STEP_FORWARD, &hotkey_value)) {
		*value = hotkey_value ? hotkey_value : (int)obs_data_get_int(settings, S_FRAME_VALUE);
		*any = !hotkey_value && obs_data_get_bool(settings, S_ANY_VALUE);
		return WARP_DETECT_KIND_STEP_FORWARD;
	}

	if (warp_detect_event_is(event, EVENT_STEP_BACKWARD, &hotkey_value)) {
		*value = hotkey_value ? hotkey_value : (int)obs_data_get_int(settings, S_FRAME_VALUE);
		*any = !hotkey_value && obs_data_get_bool(settings, S_ANY_VALUE);
		return WARP_DETECT_KIND_STEP_BACKWARD;
	}

	return WARP_DETECT_KIND_NONE;
}

static bool warp_detect_speed_matches(obs_data_t *settings, int speed, int prev_speed, const char *change)
{
	int value;
	bool any;

	switch (warp_detect_event_kind(settings, &value, &any)) {
	case WARP_DETECT_KIND_SPEED_UP:
		return strcmp(change, WARP_SPEED_CHANGE_INCREASED) == 0;
	case WARP_DETECT_KIND_SPEED_DOWN:
		return strcmp(change, WARP_SPEED_CHANGE_DECREASED) == 0;
	/* the speed going up whichever way it got there: a speed up press, or
	 * a value - a preset, Reset Speed, the Speed property - set above the
	 * speed being played at */
	case WARP_DETECT_KIND_SPEED_INCREASE:
		return speed > prev_speed;
	case WARP_DETECT_KIND_SPEED_DECREASE:
		return speed < prev_speed;
	case WARP_DETECT_KIND_SPEED_SET:
		/* "set to X%" is the speed being put at a value outright: a
		 * preset hotkey, Reset Speed, or the Speed property. Landing on
		 * X by stepping up or down is the increased/decreased event. */
		return strcmp(change, WARP_SPEED_CHANGE_SET) == 0 && (any || speed == value);
	default:
		return false;
	}
}

static bool warp_detect_frames_match(obs_data_t *settings, int frames)
{
	int value;
	bool any;
	enum warp_detect_kind kind = warp_detect_event_kind(settings, &value, &any);
	bool forward = kind == WARP_DETECT_KIND_STEP_FORWARD;

	if (!forward && kind != WARP_DETECT_KIND_STEP_BACKWARD)
		return false;

	if (forward != (frames > 0))
		return false;

	return any || (frames < 0 ? -frames : frames) == value;
}

static bool warp_detect_media_matches(obs_data_t *settings, const char *action)
{
	int value;
	bool any;

	switch (warp_detect_event_kind(settings, &value, &any)) {
	case WARP_DETECT_KIND_MEDIA_PLAY:
		return strcmp(action, WARP_MEDIA_ACTION_PLAY) == 0;
	case WARP_DETECT_KIND_MEDIA_PAUSE:
		return strcmp(action, WARP_MEDIA_ACTION_PAUSE) == 0;
	case WARP_DETECT_KIND_MEDIA_RESTART:
		return strcmp(action, WARP_MEDIA_ACTION_RESTART) == 0;
	case WARP_DETECT_KIND_MEDIA_LOADED:
		return strcmp(action, WARP_MEDIA_ACTION_LOADED) == 0;
	default:
		return false;
	}
}

/* A zoom is one of three things to react to: the framing moving at all, a
 * preset being recalled - one preset by name, or any of them - and the picture
 * being put back to the whole frame. */
static bool warp_detect_zoom_matches(obs_data_t *settings, const char *change, const char *preset)
{
	int value;
	bool any;

	switch (warp_detect_event_kind(settings, &value, &any)) {
	case WARP_DETECT_KIND_ZOOM_CHANGED:
		return true;
	case WARP_DETECT_KIND_ZOOM_PRESET: {
		const char *wanted = obs_data_get_string(settings, S_PRESET_VALUE);

		if (strcmp(change, WARP_ZOOM_CHANGE_PRESET) != 0)
			return false;

		return any || (preset && *preset && astrcmpi(preset, wanted) == 0);
	}
	case WARP_DETECT_KIND_ZOOM_RESET:
		return strcmp(change, WARP_ZOOM_CHANGE_RESET) == 0;
	default:
		return false;
	}
}

static void warp_detect_zoom_signal(void *param, calldata_t *cd)
{
	struct warp_detect_filter *f = param;
	const char *change = NULL;
	const char *preset = NULL;
	double zoom = 1.0;

	if (!obs_source_enabled(f->source))
		return;

	if (!calldata_get_string(cd, "change", &change) || !change)
		return;

	calldata_get_string(cd, "preset", &preset);
	calldata_get_float(cd, "zoom", &zoom);

	obs_data_t *settings = obs_source_get_settings(f->source);
	bool matched = warp_detect_zoom_matches(settings, change, preset);

	obs_data_release(settings);

	if (!matched)
		return;

	char event[sizeof(f->event_desc)];

	if (preset && *preset)
		snprintf(event, sizeof(event), "zoom %s to '%s' (%d%%)", change, preset, (int)(zoom * 100.0));
	else
		snprintf(event, sizeof(event), "zoom %s to %d%%", change, (int)(zoom * 100.0));

	warp_detect_fire(f, event);
}

static void warp_detect_speed_signal(void *param, calldata_t *cd)
{
	struct warp_detect_filter *f = param;
	long long speed = 0;
	long long prev_speed = 0;
	const char *change = NULL;

	if (!obs_source_enabled(f->source))
		return;

	calldata_get_int(cd, "speed", &speed);
	calldata_get_int(cd, "prev_speed", &prev_speed);

	if (!calldata_get_string(cd, "change", &change) || !change)
		return;

	obs_data_t *settings = obs_source_get_settings(f->source);
	bool matched = warp_detect_speed_matches(settings, (int)speed, (int)prev_speed, change);

	obs_data_release(settings);

	if (!matched)
		return;

	char event[sizeof(f->event_desc)];

	snprintf(event, sizeof(event), "speed %s from %d%% to %d%%", change, (int)prev_speed, (int)speed);
	warp_detect_fire(f, event);
}

static void warp_detect_frames_signal(void *param, calldata_t *cd)
{
	struct warp_detect_filter *f = param;
	long long frames = 0;

	if (!obs_source_enabled(f->source))
		return;

	if (!calldata_get_int(cd, "frames", &frames) || frames == 0)
		return;

	obs_data_t *settings = obs_source_get_settings(f->source);
	bool matched = warp_detect_frames_match(settings, (int)frames);

	obs_data_release(settings);

	if (!matched)
		return;

	char event[sizeof(f->event_desc)];

	snprintf(event, sizeof(event), "skipped %d frames %s", (int)(frames < 0 ? -frames : frames),
		 frames < 0 ? "backward" : "forward");
	warp_detect_fire(f, event);
}

static void warp_detect_media_signal(void *param, calldata_t *cd)
{
	struct warp_detect_filter *f = param;
	const char *action = NULL;

	if (!obs_source_enabled(f->source))
		return;

	if (!calldata_get_string(cd, "action", &action) || !action)
		return;

	obs_data_t *settings = obs_source_get_settings(f->source);
	bool matched = warp_detect_media_matches(settings, action);

	obs_data_release(settings);

	if (!matched)
		return;

	char event[sizeof(f->event_desc)];

	snprintf(event, sizeof(event), "media %s", action);
	warp_detect_fire(f, event);
}

static bool warp_detect_is_warp_source(obs_source_t *source)
{
	const char *id = obs_source_get_id(source);

	return id && (strcmp(id, WARP_MEDIA_SOURCE_ID) == 0 || strcmp(id, WARP_PLAYLIST_SOURCE_ID) == 0);
}

/* Connecting and disconnecting is done without f->mutex held: disconnecting
 * waits for a signal that is being delivered, and delivering one wants the
 * mutex. */
static void warp_detect_disconnect(struct warp_detect_filter *f)
{
	if (!f->listening_to)
		return;

	obs_source_t *source = obs_weak_source_get_source(f->listening_to);

	if (source) {
		signal_handler_t *handler = obs_source_get_signal_handler(source);

		signal_handler_disconnect(handler, WARP_SIGNAL_SPEED_CHANGED, warp_detect_speed_signal, f);
		signal_handler_disconnect(handler, WARP_SIGNAL_FRAMES_STEPPED, warp_detect_frames_signal, f);
		signal_handler_disconnect(handler, WARP_SIGNAL_MEDIA_ACTION, warp_detect_media_signal, f);
		signal_handler_disconnect(handler, WARP_SIGNAL_ZOOM_CHANGED, warp_detect_zoom_signal, f);
		obs_source_release(source);
	}

	obs_weak_source_release(f->listening_to);
	f->listening_to = NULL;
}

/* the source the settings name, or the one the filter is on when they name
 * none; NULL when it is not there or is not a Warp source (returns a reference) */
static obs_source_t *warp_detect_wanted_source(struct warp_detect_filter *f)
{
	obs_data_t *settings = obs_source_get_settings(f->source);
	const char *name = obs_data_get_string(settings, S_TARGET);
	obs_source_t *source;

	if (name && *name) {
		source = obs_get_source_by_name(name);
	} else {
		obs_source_t *parent = obs_filter_get_parent(f->source);

		source = parent ? obs_source_get_ref(parent) : NULL;
	}

	obs_data_release(settings);

	/* A source that is not a Warp source is listened to through the Warp
	 * Zoom filter on it, which is where its framing is changed and where it
	 * says so. */
	if (source && !warp_detect_is_warp_source(source)) {
		obs_source_t *zoom = warp_zoom_filter_find_operable(source);

		obs_source_release(source);
		source = zoom;
	}

	return source;
}

static void warp_detect_resolve(struct warp_detect_filter *f)
{
	obs_source_t *wanted = warp_detect_wanted_source(f);

	if (wanted && f->listening_to && obs_weak_source_references_source(f->listening_to, wanted)) {
		obs_source_release(wanted);
		return;
	}

	warp_detect_disconnect(f);

	if (!wanted)
		return;

	signal_handler_t *handler = obs_source_get_signal_handler(wanted);

	signal_handler_connect(handler, WARP_SIGNAL_SPEED_CHANGED, warp_detect_speed_signal, f);
	signal_handler_connect(handler, WARP_SIGNAL_FRAMES_STEPPED, warp_detect_frames_signal, f);
	signal_handler_connect(handler, WARP_SIGNAL_MEDIA_ACTION, warp_detect_media_signal, f);
	signal_handler_connect(handler, WARP_SIGNAL_ZOOM_CHANGED, warp_detect_zoom_signal, f);
	f->listening_to = obs_source_get_weak_source(wanted);

	WARP_DETECT_LOG(LOG_INFO, "listening to '%s'", obs_source_get_name(wanted));

	obs_source_release(wanted);
}

static void warp_detect_tick(void *data, float seconds)
{
	struct warp_detect_filter *f = data;
	bool forced = os_atomic_set_bool(&f->needs_resolve, false);

	f->resolve_delay -= seconds;

	if (!forced && f->resolve_delay > 0.0f)
		return;

	f->resolve_delay = WARP_DETECT_RESOLVE_SECONDS;
	warp_detect_resolve(f);
}

/* ------------------------------------------------------------------------- */
/* properties */

/* The sources there is something to listen to on: the Warp sources, and any
 * source carrying a Warp Zoom filter, whose framing is worth reacting to the
 * same way a Warp source's is. */
static bool warp_detect_can_listen(obs_source_t *source)
{
	obs_source_t *zoom;

	if (warp_detect_is_warp_source(source))
		return true;

	zoom = warp_zoom_filter_find_operable(source);
	obs_source_release(zoom);

	return zoom != NULL;
}

static bool warp_detect_add_warp_source(void *data, obs_source_t *source)
{
	obs_property_t *list = data;

	if (warp_detect_can_listen(source)) {
		const char *name = obs_source_get_name(source);

		if (name && *name)
			obs_property_list_add_string(list, name, name);
	}

	return true;
}

static bool warp_detect_add_source(void *data, obs_source_t *source)
{
	obs_property_t *list = data;
	const char *name = obs_source_get_name(source);

	if (name && *name)
		obs_property_list_add_string(list, name, name);

	return true;
}

static void warp_detect_add_filter(obs_source_t *parent, obs_source_t *child, void *param)
{
	obs_property_t *list = param;
	const char *name = obs_source_get_name(child);

	UNUSED_PARAMETER(parent);

	if (name && *name)
		obs_property_list_add_string(list, name, name);
}

struct warp_detect_hotkey_list {
	obs_property_t *list;
	obs_source_t *owner;
};

static bool warp_detect_add_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *key)
{
	struct warp_detect_hotkey_list *ctx = data;
	const char *name = obs_hotkey_get_name(key);
	const char *description = obs_hotkey_get_description(key);

	UNUSED_PARAMETER(id);

	if (!name || !warp_detect_hotkey_owned_by(key, ctx->owner))
		return true;

	/* the description is the translated text the hotkey menu shows */
	obs_property_list_add_string(ctx->list, description && *description ? description : name, name);

	return true;
}

/* A source, filter or hotkey that is not there right now - a scene collection
 * that is still loading, something renamed since - must not be dropped from the
 * settings just because the properties were opened, so what is saved is always
 * in the list. */
static void warp_detect_keep_value(obs_property_t *list, const char *value)
{
	if (!value || !*value)
		return;

	for (size_t i = 0; i < obs_property_list_item_count(list); i++) {
		const char *item = obs_property_list_item_string(list, i);

		if (item && strcmp(item, value) == 0)
			return;
	}

	obs_property_list_add_string(list, value, value);
}

static void warp_detect_start_list(obs_property_t *list)
{
	obs_property_list_clear(list);
	obs_property_list_add_string(list, obs_module_text("Warp.Detect.None"), "");
}

static void warp_detect_fill_sources(obs_property_t *list, const char *selected)
{
	warp_detect_start_list(list);
	obs_enum_sources(warp_detect_add_source, list);
	obs_enum_scenes(warp_detect_add_source, list);
	warp_detect_keep_value(list, selected);
}

/* 'owner' is the source whose hotkeys are listed, or NULL for OBS's own */
static void warp_detect_fill_hotkeys(obs_property_t *list, obs_source_t *owner, bool have_owner, const char *selected)
{
	struct warp_detect_hotkey_list ctx = {.list = list, .owner = owner};

	warp_detect_start_list(list);

	/* the source whose hotkeys were asked for is gone: listing OBS's own
	 * hotkeys instead would be the wrong list entirely */
	if (!have_owner || owner)
		obs_enum_hotkeys(warp_detect_add_hotkey, &ctx);

	warp_detect_keep_value(list, selected);
}

static void warp_detect_fill_source_hotkeys(obs_property_t *list, const char *source_name, const char *selected)
{
	obs_source_t *owner = *source_name ? obs_get_source_by_name(source_name) : NULL;

	warp_detect_fill_hotkeys(list, owner, true, selected);
	obs_source_release(owner);
}

static void warp_detect_fill_filters(obs_property_t *list, const char *source_name, const char *selected)
{
	obs_source_t *owner = *source_name ? obs_get_source_by_name(source_name) : NULL;

	warp_detect_start_list(list);

	if (owner) {
		obs_source_enum_filters(owner, warp_detect_add_filter, list);
		obs_source_release(owner);
	}

	warp_detect_keep_value(list, selected);
}

static void warp_detect_fill_filter_hotkeys(obs_property_t *list, const char *source_name, const char *filter_name,
					    const char *selected)
{
	obs_source_t *owner = *source_name ? obs_get_source_by_name(source_name) : NULL;
	obs_source_t *target = owner && *filter_name ? obs_source_get_filter_by_name(owner, filter_name) : NULL;

	warp_detect_fill_hotkeys(list, target, true, selected);

	obs_source_release(target);
	obs_source_release(owner);
}

/* the events that stand for a single hotkey are labelled with that hotkey's own
 * name, so the list reads like the one in Settings -> Hotkeys */
static void warp_detect_add_speed_event(obs_property_t *list, int speed)
{
	char event[32];
	char text_key[64];
	const char *label;

	snprintf(event, sizeof(event), "%s_%d", EVENT_SPEED_SET, speed);

	if (speed == WARP_DETECT_RESET_SPEED) {
		label = obs_module_text("Warp.Hotkey.Speed.Reset");
	} else {
		snprintf(text_key, sizeof(text_key), "Warp.Hotkey.Speed.Preset%d", speed);
		label = obs_module_text(text_key);
	}

	obs_property_list_add_string(list, label, event);
}

static void warp_detect_add_step_event(obs_property_t *list, const char *kind, const char *direction, int frames)
{
	char event[32];
	char text_key[64];

	snprintf(event, sizeof(event), "%s_%d", kind, frames);
	snprintf(text_key, sizeof(text_key), "Warp.Hotkey.Step.%s%d", direction, frames);

	obs_property_list_add_string(list, obs_module_text(text_key), event);
}

static void warp_detect_add_events(obs_property_t *list)
{
	static const int speed_presets[WARP_NUM_SPEED_PRESETS] = {WARP_SPEED_PRESET_LIST};
	static const int step_counts[WARP_NUM_STEP_COUNTS] = {WARP_STEP_COUNT_LIST};
	bool reset_added = false;

	/* the general event first, then one per hotkey that sets a speed: the
	 * presets, and the 100% Reset Speed sets, in speed order */
	obs_property_list_add_string(list, obs_module_text("Warp.Detect.Event.SpeedSet"), EVENT_SPEED_SET);

	for (size_t i = 0; i < WARP_NUM_SPEED_PRESETS; i++) {
		if (!reset_added && speed_presets[i] > WARP_DETECT_RESET_SPEED) {
			warp_detect_add_speed_event(list, WARP_DETECT_RESET_SPEED);
			reset_added = true;
		}

		warp_detect_add_speed_event(list, speed_presets[i]);
	}

	if (!reset_added)
		warp_detect_add_speed_event(list, WARP_DETECT_RESET_SPEED);

	obs_property_list_add_string(list, obs_module_text("Warp.Hotkey.Speed.Up"), EVENT_SPEED_UP);
	obs_property_list_add_string(list, obs_module_text("Warp.Hotkey.Speed.Down"), EVENT_SPEED_DOWN);

	/* the speed moving in a direction, however it was asked to */
	obs_property_list_add_string(list, obs_module_text("Warp.Detect.Event.SpeedIncrease"), EVENT_SPEED_INCREASE);
	obs_property_list_add_string(list, obs_module_text("Warp.Detect.Event.SpeedDecrease"), EVENT_SPEED_DECREASE);

	obs_property_list_add_string(list, obs_module_text("Warp.Detect.Event.StepForward"), EVENT_STEP_FORWARD);

	for (size_t i = 0; i < WARP_NUM_STEP_COUNTS; i++)
		warp_detect_add_step_event(list, EVENT_STEP_FORWARD, "Forward", step_counts[i]);

	obs_property_list_add_string(list, obs_module_text("Warp.Detect.Event.StepBackward"), EVENT_STEP_BACKWARD);

	for (size_t i = 0; i < WARP_NUM_STEP_COUNTS; i++)
		warp_detect_add_step_event(list, EVENT_STEP_BACKWARD, "Backward", step_counts[i]);

	obs_property_list_add_string(list, obs_module_text("Warp.Detect.Event.MediaPlay"), EVENT_MEDIA_PLAY);
	obs_property_list_add_string(list, obs_module_text("Warp.Detect.Event.MediaPause"), EVENT_MEDIA_PAUSE);
	obs_property_list_add_string(list, obs_module_text("Warp.Detect.Event.MediaRestart"), EVENT_MEDIA_RESTART);
	obs_property_list_add_string(list, obs_module_text("Warp.Detect.Event.MediaLoaded"), EVENT_MEDIA_LOADED);

	obs_property_list_add_string(list, obs_module_text("Warp.Detect.Event.ZoomChanged"), EVENT_ZOOM_CHANGED);
	obs_property_list_add_string(list, obs_module_text("Warp.Detect.Event.ZoomPreset"), EVENT_ZOOM_PRESET);
	obs_property_list_add_string(list, obs_module_text("Warp.Detect.Event.ZoomReset"), EVENT_ZOOM_RESET);
}

/* the presets of the source an action names, so the preset to recall is picked
 * from the ones that are there rather than typed in */
static void warp_detect_fill_presets(obs_property_t *list, const char *source_name, const char *filter_name,
				     const char *selected)
{
	obs_source_t *owner = *source_name ? obs_get_source_by_name(source_name) : NULL;
	obs_source_t *zoom = owner ? warp_detect_zoom_target(owner, filter_name) : NULL;
	obs_data_array_t *presets = zoom ? warp_zoom_source_presets(zoom) : NULL;

	warp_detect_start_list(list);

	for (size_t i = 0; presets && i < obs_data_array_count(presets); i++) {
		obs_data_t *preset = obs_data_array_item(presets, i);
		const char *name = obs_data_get_string(preset, WARP_ZOOM_P_NAME);

		/* Presets are recalled by name here rather than by id: a filter
		 * saying "frame it the way Wide does" survives the preset being
		 * remade, and reads as what it does in the settings. */
		if (name && *name)
			obs_property_list_add_string(list, name, name);

		obs_data_release(preset);
	}

	obs_data_array_release(presets);
	obs_source_release(zoom);
	obs_source_release(owner);

	warp_detect_keep_value(list, selected);
}

/* the Warp Zoom filters on a source, for an action that names one rather than
 * taking whichever the source carries */
static void warp_detect_add_zoom_filter(obs_source_t *parent, obs_source_t *child, void *param)
{
	obs_property_t *list = param;
	const char *id = obs_source_get_id(child);
	const char *name = obs_source_get_name(child);

	UNUSED_PARAMETER(parent);

	if (id && strcmp(id, WARP_ZOOM_FILTER_ID) == 0 && name && *name)
		obs_property_list_add_string(list, name, name);
}

static void warp_detect_fill_zoom_filters(obs_property_t *list, const char *source_name, const char *selected)
{
	obs_source_t *owner = *source_name ? obs_get_source_by_name(source_name) : NULL;

	warp_detect_start_list(list);

	if (owner) {
		obs_source_enum_filters(owner, warp_detect_add_zoom_filter, list);
		obs_source_release(owner);
	}

	warp_detect_keep_value(list, selected);
}

static bool warp_detect_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	const char *event = obs_data_get_string(settings, S_EVENT);
	const char *action = obs_data_get_string(settings, S_ACTION);
	const char *filter_mode = obs_data_get_string(settings, S_FILTER_MODE);
	bool speed_value = strcmp(event, EVENT_SPEED_SET) == 0;
	bool frame_value = strcmp(event, EVENT_STEP_FORWARD) == 0 || strcmp(event, EVENT_STEP_BACKWARD) == 0;
	bool any_value = obs_data_get_bool(settings, S_ANY_VALUE);
	bool global_hotkey = strcmp(action, ACTION_GLOBAL_HOTKEY) == 0;
	bool source_hotkey = strcmp(action, ACTION_SOURCE_HOTKEY) == 0;
	bool filter = strcmp(action, ACTION_FILTER) == 0;
	bool filter_hotkey = filter && strcmp(filter_mode, FILTER_MODE_HOTKEY) == 0;
	bool zoom_action = strcmp(action, ACTION_ZOOM_PRESET) == 0;
	bool preset_value = strcmp(event, EVENT_ZOOM_PRESET) == 0;
	obs_property_t *list;

	UNUSED_PARAMETER(property);

	/* reacting to any value leaves nothing for the value to do */
	obs_property_set_visible(obs_properties_get(props, S_SPEED_VALUE), speed_value && !any_value);
	obs_property_set_visible(obs_properties_get(props, S_FRAME_VALUE), frame_value && !any_value);
	obs_property_set_visible(obs_properties_get(props, S_PRESET_VALUE), preset_value && !any_value);
	obs_property_set_visible(obs_properties_get(props, S_ANY_VALUE), speed_value || frame_value || preset_value);

	obs_property_set_visible(obs_properties_get(props, S_GLOBAL_HOTKEY), global_hotkey);
	obs_property_set_visible(obs_properties_get(props, S_HOTKEY_SOURCE), source_hotkey);
	obs_property_set_visible(obs_properties_get(props, S_SOURCE_HOTKEY), source_hotkey);
	obs_property_set_visible(obs_properties_get(props, S_FILTER_SOURCE), filter);
	obs_property_set_visible(obs_properties_get(props, S_FILTER_NAME), filter);
	obs_property_set_visible(obs_properties_get(props, S_FILTER_MODE), filter);
	obs_property_set_visible(obs_properties_get(props, S_FILTER_HOTKEY), filter_hotkey);
	obs_property_set_visible(obs_properties_get(props, S_ZOOM_SOURCE), zoom_action);
	obs_property_set_visible(obs_properties_get(props, S_ZOOM_FILTER), zoom_action);
	obs_property_set_visible(obs_properties_get(props, S_ZOOM_PRESET), zoom_action);

	/* every list is built from what is in OBS right now */
	list = obs_properties_get(props, S_TARGET);
	obs_property_list_clear(list);
	obs_property_list_add_string(list, obs_module_text("Warp.Detect.Source.Parent"), "");
	obs_enum_sources(warp_detect_add_warp_source, list);
	warp_detect_keep_value(list, obs_data_get_string(settings, S_TARGET));

	if (global_hotkey)
		warp_detect_fill_hotkeys(obs_properties_get(props, S_GLOBAL_HOTKEY), NULL, false,
					 obs_data_get_string(settings, S_GLOBAL_HOTKEY));

	if (source_hotkey) {
		const char *owner = obs_data_get_string(settings, S_HOTKEY_SOURCE);

		warp_detect_fill_sources(obs_properties_get(props, S_HOTKEY_SOURCE), owner);
		warp_detect_fill_source_hotkeys(obs_properties_get(props, S_SOURCE_HOTKEY), owner,
						obs_data_get_string(settings, S_SOURCE_HOTKEY));
	}

	if (filter) {
		const char *owner = obs_data_get_string(settings, S_FILTER_SOURCE);
		const char *name = obs_data_get_string(settings, S_FILTER_NAME);

		warp_detect_fill_sources(obs_properties_get(props, S_FILTER_SOURCE), owner);
		warp_detect_fill_filters(obs_properties_get(props, S_FILTER_NAME), owner, name);

		if (filter_hotkey)
			warp_detect_fill_filter_hotkeys(obs_properties_get(props, S_FILTER_HOTKEY), owner, name,
							obs_data_get_string(settings, S_FILTER_HOTKEY));
	}

	if (zoom_action) {
		const char *owner = obs_data_get_string(settings, S_ZOOM_SOURCE);
		const char *zoom_filter = obs_data_get_string(settings, S_ZOOM_FILTER);

		warp_detect_fill_sources(obs_properties_get(props, S_ZOOM_SOURCE), owner);
		warp_detect_fill_zoom_filters(obs_properties_get(props, S_ZOOM_FILTER), owner, zoom_filter);
		warp_detect_fill_presets(obs_properties_get(props, S_ZOOM_PRESET), owner, zoom_filter,
					 obs_data_get_string(settings, S_ZOOM_PRESET));
	}

	return true;
}

static obs_properties_t *warp_detect_getproperties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *prop;

	UNUSED_PARAMETER(data);

	prop = obs_properties_add_list(props, S_TARGET, obs_module_text("Warp.Detect.Source"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_STRING);
	obs_property_set_long_description(prop, obs_module_text("Warp.Detect.Source.Desc"));
	obs_property_set_modified_callback(prop, warp_detect_modified);

	prop = obs_properties_add_list(props, S_EVENT, obs_module_text("Warp.Detect.Event"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_STRING);
	warp_detect_add_events(prop);
	obs_property_set_long_description(prop, obs_module_text("Warp.Detect.Event.Desc"));
	obs_property_set_modified_callback(prop, warp_detect_modified);

	prop = obs_properties_add_int_slider(props, S_SPEED_VALUE, obs_module_text("Warp.Video.Speed"), MP_SPEED_MIN,
					     MP_SPEED_MAX, 1);
	obs_property_int_set_suffix(prop, "%");

	obs_properties_add_int(props, S_FRAME_VALUE, obs_module_text("Warp.Detect.Frames"), 1, 10000, 1);

	obs_properties_add_text(props, S_PRESET_VALUE, obs_module_text("Warp.Detect.Preset"), OBS_TEXT_DEFAULT);

	prop = obs_properties_add_bool(props, S_ANY_VALUE, obs_module_text("Warp.Detect.AnyValue"));
	obs_property_set_modified_callback(prop, warp_detect_modified);

	prop = obs_properties_add_list(props, S_ACTION, obs_module_text("Warp.Detect.Action"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, obs_module_text("Warp.Detect.Action.GlobalHotkey"), ACTION_GLOBAL_HOTKEY);
	obs_property_list_add_string(prop, obs_module_text("Warp.Detect.Action.SourceHotkey"), ACTION_SOURCE_HOTKEY);
	obs_property_list_add_string(prop, obs_module_text("Warp.Detect.Action.Filter"), ACTION_FILTER);
	obs_property_list_add_string(prop, obs_module_text("Warp.Detect.Action.ZoomPreset"), ACTION_ZOOM_PRESET);
	obs_property_set_modified_callback(prop, warp_detect_modified);

	obs_properties_add_list(props, S_GLOBAL_HOTKEY, obs_module_text("Warp.Detect.Hotkey"), OBS_COMBO_TYPE_LIST,
				OBS_COMBO_FORMAT_STRING);

	prop = obs_properties_add_list(props, S_HOTKEY_SOURCE, obs_module_text("Warp.Detect.HotkeySource"),
				       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_set_modified_callback(prop, warp_detect_modified);

	obs_properties_add_list(props, S_SOURCE_HOTKEY, obs_module_text("Warp.Detect.Hotkey"), OBS_COMBO_TYPE_LIST,
				OBS_COMBO_FORMAT_STRING);

	prop = obs_properties_add_list(props, S_FILTER_SOURCE, obs_module_text("Warp.Detect.FilterSource"),
				       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_set_modified_callback(prop, warp_detect_modified);

	prop = obs_properties_add_list(props, S_FILTER_NAME, obs_module_text("Warp.Detect.Filter"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_STRING);
	obs_property_set_modified_callback(prop, warp_detect_modified);

	prop = obs_properties_add_list(props, S_FILTER_MODE, obs_module_text("Warp.Detect.FilterMode"),
				       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, obs_module_text("Warp.Detect.FilterMode.Enable"), FILTER_MODE_ENABLE);
	obs_property_list_add_string(prop, obs_module_text("Warp.Detect.FilterMode.Disable"), FILTER_MODE_DISABLE);
	obs_property_list_add_string(prop, obs_module_text("Warp.Detect.FilterMode.Toggle"), FILTER_MODE_TOGGLE);
	obs_property_list_add_string(prop, obs_module_text("Warp.Detect.FilterMode.Hotkey"), FILTER_MODE_HOTKEY);
	obs_property_set_modified_callback(prop, warp_detect_modified);

	obs_properties_add_list(props, S_FILTER_HOTKEY, obs_module_text("Warp.Detect.Hotkey"), OBS_COMBO_TYPE_LIST,
				OBS_COMBO_FORMAT_STRING);

	prop = obs_properties_add_list(props, S_ZOOM_SOURCE, obs_module_text("Warp.Detect.ZoomSource"),
				       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_set_long_description(prop, obs_module_text("Warp.Detect.ZoomSource.Desc"));
	obs_property_set_modified_callback(prop, warp_detect_modified);

	prop = obs_properties_add_list(props, S_ZOOM_FILTER, obs_module_text("Warp.Detect.ZoomFilter"),
				       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_set_long_description(prop, obs_module_text("Warp.Detect.ZoomFilter.Desc"));
	obs_property_set_modified_callback(prop, warp_detect_modified);

	obs_properties_add_list(props, S_ZOOM_PRESET, obs_module_text("Warp.Detect.ZoomPreset"), OBS_COMBO_TYPE_LIST,
				OBS_COMBO_FORMAT_STRING);

	return props;
}

static void warp_detect_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_EVENT, EVENT_SPEED_SET);
	obs_data_set_default_int(settings, S_SPEED_VALUE, 100);
	obs_data_set_default_int(settings, S_FRAME_VALUE, 1);
	obs_data_set_default_string(settings, S_ACTION, ACTION_GLOBAL_HOTKEY);
	obs_data_set_default_string(settings, S_FILTER_MODE, FILTER_MODE_TOGGLE);
}

/* ------------------------------------------------------------------------- */
/* obs source callbacks */

static void warp_detect_update(void *data, obs_data_t *settings)
{
	struct warp_detect_filter *f = data;

	UNUSED_PARAMETER(settings);

	/* the settings are read where they are used; all an update has to do is
	 * point the filter at whatever source it now names */
	os_atomic_set_bool(&f->needs_resolve, true);
}

static void *warp_detect_create(obs_data_t *settings, obs_source_t *source)
{
	struct warp_detect_filter *f = bzalloc(sizeof(struct warp_detect_filter));

	UNUSED_PARAMETER(settings);

	f->source = source;

	if (pthread_mutex_init(&f->mutex, NULL)) {
		blog(LOG_ERROR, "[Warp Detection]: failed to initialize mutex");
		bfree(f);
		return NULL;
	}

	f->window_start = os_gettime_ns();
	os_atomic_set_bool(&f->needs_resolve, true);

	return f;
}

static void warp_detect_destroy(void *data)
{
	struct warp_detect_filter *f = data;

	warp_detect_disconnect(f);
	pthread_mutex_destroy(&f->mutex);
	bfree(f);
}

/* the filter watches playback, it does not change the picture */
static void warp_detect_video_render(void *data, gs_effect_t *effect)
{
	struct warp_detect_filter *f = data;

	UNUSED_PARAMETER(effect);

	obs_source_skip_video_filter(f->source);
}

struct obs_source_info warp_detection_filter_info = {
	.id = "warp_detection_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = warp_detect_getname,
	.create = warp_detect_create,
	.destroy = warp_detect_destroy,
	.get_defaults = warp_detect_defaults,
	.get_properties = warp_detect_getproperties,
	.update = warp_detect_update,
	.video_render = warp_detect_video_render,
	.video_tick = warp_detect_tick,
};
