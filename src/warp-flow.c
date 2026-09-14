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

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <media-playback/media-playback.h>
#include <plugin-support.h>

#include "warp-events.h"
#include "warp-flow.h"
#include "warp-trim.h"

/* the speed a flow plays a clip at is the source's speed, so the range the UI
 * offers has to be the range the source takes */
_Static_assert(WARP_FLOW_SPEED_MIN == MP_SPEED_MIN && WARP_FLOW_SPEED_MAX == MP_SPEED_MAX,
	       "a flow's speed range has drifted from the one Warp sources play at");

#define WARP_FLOW_LOG(level, format, ...) blog(level, "[Warp Flow]: " format, ##__VA_ARGS__)

/* Where the flows live in the scene collection: the frontend hands every
 * plugin the same data object to save into, so everything Warp keeps there
 * goes under one key of its own. */
#define WARP_FLOW_MODULE_KEY "warp"
#define WARP_FLOW_ARRAY_KEY "flows"

/* How long a replay buffer save stays attributed to the flow that asked for
 * it. Writing the buffer out takes as long as the buffer is long, so this is
 * generous; it is only here so that a save that never arrives - the output
 * stopped, the disk filled up - cannot hand the next save someone else makes
 * to the wrong flow. */
#define WARP_FLOW_CLAIM_NS (30ULL * 1000000000ULL)

/* the settings key of the Warp Playlist source's file list */
#define WARP_FLOW_PLAYLIST_KEY "playlist"
/* and of an item in it */
#define WARP_FLOW_PLAYLIST_VALUE "value"

/* one of the lengths a flow offers on top of its own, and the hotkey that asks
 * for it */
struct warp_flow_length {
	int seconds;
	obs_hotkey_id hotkey;
};

struct warp_flow {
	/* everything about the flow, and exactly what it is saved as */
	obs_data_t *config;
	obs_hotkey_id save_hotkey;
	obs_hotkey_id promote_hotkey;
	/* in step with the WARP_FLOW_LENGTHS array of the configuration: the
	 * hotkey each of those lengths is asked for with */
	DARRAY(struct warp_flow_length) lengths;
};

/* what feeding one flow a clip comes down to, worked out under the lock and
 * carried out once it has been dropped */
struct warp_flow_delivery {
	char *flow_id;
	char *flow_name;
	char *target_uuid;
	char *target_name;
	/* how much of the clip this flow is fed, and the file that turned out
	 * to be: the whole one, or the cut made from it */
	int seconds;
	char *path;
	/* a Warp Media source holding the one clip, rather than a Warp Playlist
	 * source holding a list of them */
	bool instant;
	/* the list ones */
	bool newest_first;
	int max_clips;
	/* the instant one */
	char *playback;
	int speed;
};

struct warp_flow_plan {
	DARRAY(struct warp_flow_delivery) items;
	/* ids already in the plan, so a link that points back at a flow
	 * already being fed does not feed it twice */
	DARRAY(char *) visited;
};

/* Guards the flow list and every flow's configuration.
 *
 * Nothing that reaches back into libobs' source graph is done with it held:
 * feeding a playlist looks its source up and updates it, which takes libobs'
 * own locks, so the flows to feed are worked out under the lock and the
 * feeding itself happens after it has been dropped. */
static pthread_mutex_t warp_flow_mutex;
static bool warp_flow_ready = false;
static DARRAY(struct warp_flow *) warp_flows;

/* the clip the replay buffer wrote last, kept so it can be promoted into a
 * flow after the fact */
static char *warp_flow_last_path = NULL;
/* the flow that asked for the save that is on its way, when it asked, and the
 * length it asked for */
static char *warp_flow_claim_id = NULL;
static uint64_t warp_flow_claim_ns = 0;
static int warp_flow_claim_seconds = WARP_FLOW_LENGTH_DEFAULT;

static uint64_t warp_flow_id_seq = 0;

/* how the UI is told a hotkey press found no replay buffer to save; set once,
 * while the module loads, and only ever called from the UI thread */
static warp_flow_buffer_prompt_t warp_flow_buffer_prompt = NULL;

/* ------------------------------------------------------------------------- */
/* small helpers */

static obs_data_t *warp_flow_data_copy(obs_data_t *data)
{
	obs_data_t *copy;

	if (!data)
		return NULL;

	copy = obs_data_create();
	obs_data_apply(copy, data);

	return copy;
}

static inline bool warp_flow_str_eq(const char *a, const char *b)
{
	return a && b && strcmp(a, b) == 0;
}

/* expects the lock to be held */
static struct warp_flow *warp_flow_find(const char *id)
{
	if (!id || !*id)
		return NULL;

	for (size_t i = 0; i < warp_flows.num; i++) {
		struct warp_flow *flow = warp_flows.array[i];

		if (warp_flow_str_eq(obs_data_get_string(flow->config, WARP_FLOW_ID), id))
			return flow;
	}

	return NULL;
}

/* The flow a hotkey belongs to, with 'seconds' left holding the length that key
 * asks for: one of the flow's extra lengths, or the flow's own where the key is
 * its plain one. Expects the lock to be held. */
static struct warp_flow *warp_flow_find_by_hotkey(obs_hotkey_id id, bool promote, int *seconds)
{
	*seconds = WARP_FLOW_LENGTH_DEFAULT;

	if (id == OBS_INVALID_HOTKEY_ID)
		return NULL;

	for (size_t i = 0; i < warp_flows.num; i++) {
		struct warp_flow *flow = warp_flows.array[i];

		if (promote) {
			if (flow->promote_hotkey == id)
				return flow;

			continue;
		}

		if (flow->save_hotkey == id)
			return flow;

		for (size_t j = 0; j < flow->lengths.num; j++) {
			if (flow->lengths.array[j].hotkey != id)
				continue;

			*seconds = flow->lengths.array[j].seconds;

			return flow;
		}
	}

	return NULL;
}

/* ------------------------------------------------------------------------- */
/* lengths */

void warp_flow_length_label(int seconds, char *buffer, size_t size)
{
	if (!buffer || !size)
		return;

	if (seconds <= 0)
		buffer[0] = '\0';
	else if (seconds < 60)
		snprintf(buffer, size, "%ds", seconds);
	else if (seconds % 60 == 0)
		snprintf(buffer, size, "%dm", seconds / 60);
	else
		snprintf(buffer, size, "%dm%ds", seconds / 60, seconds % 60);
}

/* a length as it is worth keeping: inside the range, and no length at all
 * rather than a negative one */
static int warp_flow_clamp_length(int seconds)
{
	if (seconds <= 0)
		return WARP_FLOW_LENGTH_WHOLE;

	return seconds > WARP_FLOW_LENGTH_MAX ? WARP_FLOW_LENGTH_MAX : seconds;
}

/* what the flow's own length comes to; expects the lock to be held */
static int warp_flow_own_length(struct warp_flow *flow)
{
	return warp_flow_clamp_length((int)obs_data_get_int(flow->config, WARP_FLOW_LENGTH));
}

static char *warp_flow_make_id(void)
{
	struct dstr id = {0};

	dstr_printf(&id, "flow_%" PRIx64 "_%" PRIx64, (uint64_t)os_gettime_ns(), ++warp_flow_id_seq);

	return id.array;
}

/* ------------------------------------------------------------------------- */
/* hotkeys
 *
 * A hotkey press arrives on the hotkey thread, with libobs' hotkey lock held,
 * and the flow it belongs to may be edited or removed from the UI thread at
 * any moment. So the press carries nothing but the id of the hotkey it came
 * from: the flow is looked up by that id once the work is back on the UI
 * thread, and a flow that has gone in the meantime is simply not there. */

struct warp_flow_hotkey_press {
	obs_hotkey_id hotkey;
	bool promote;
};

static void warp_flow_hotkey_task(void *param)
{
	struct warp_flow_hotkey_press *press = param;
	char *id = NULL;
	char *name = NULL;
	int seconds = WARP_FLOW_LENGTH_DEFAULT;

	pthread_mutex_lock(&warp_flow_mutex);
	struct warp_flow *flow = warp_flow_find_by_hotkey(press->hotkey, press->promote, &seconds);

	if (flow) {
		id = bstrdup(obs_data_get_string(flow->config, WARP_FLOW_ID));
		name = bstrdup(obs_data_get_string(flow->config, WARP_FLOW_NAME));
	}
	pthread_mutex_unlock(&warp_flow_mutex);

	if (id) {
		if (press->promote) {
			warp_flow_promote_last_length(id, seconds);
		} else if (!warp_flow_save_replay_length(id, seconds) && !obs_frontend_replay_buffer_active()) {
			/* the press did nothing because there was no buffer to
			 * save: the user asked for a clip, so they are told */
			if (warp_flow_buffer_prompt)
				warp_flow_buffer_prompt(name);
		}

		bfree(id);
	}

	bfree(name);
	bfree(press);
}

static void warp_flow_queue_press(obs_hotkey_id hotkey, bool promote)
{
	struct warp_flow_hotkey_press *press = bmalloc(sizeof(struct warp_flow_hotkey_press));

	press->hotkey = hotkey;
	press->promote = promote;

	obs_queue_task(OBS_TASK_UI, warp_flow_hotkey_task, press, false);
}

static void warp_flow_save_hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(hotkey);

	if (pressed)
		warp_flow_queue_press(id, false);
}

static void warp_flow_promote_hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(hotkey);

	if (pressed)
		warp_flow_queue_press(id, true);
}

static void warp_flow_hotkey_names(const char *flow_id, const char *flow_name, struct dstr *save_name,
				   struct dstr *save_desc, struct dstr *promote_name, struct dstr *promote_desc)
{
	dstr_printf(save_name, "Warp.Flow.%s.Save", flow_id);
	dstr_printf(promote_name, "Warp.Flow.%s.Promote", flow_id);
	dstr_printf(save_desc, obs_module_text("Warp.Flow.Hotkey.Save"), flow_name);
	dstr_printf(promote_desc, obs_module_text("Warp.Flow.Hotkey.Promote"), flow_name);
}

/* What one of a flow's extra lengths is called. The registry name is made of
 * the flow and the length rather than of where it stands in the list, so the
 * key it is bound to is still its own after the flow has been edited. */
static void warp_flow_length_hotkey_names(const char *flow_id, const char *flow_name, int seconds, struct dstr *name,
					  struct dstr *desc)
{
	char label[WARP_FLOW_LENGTH_LABEL_SIZE];

	warp_flow_length_label(seconds, label, sizeof(label));

	dstr_printf(name, "Warp.Flow.%s.Save.%d", flow_id, seconds);
	dstr_printf(desc, obs_module_text("Warp.Flow.Hotkey.SaveLength"), label, flow_name);
}

/* expects the lock to be held */
static void warp_flow_clear_length_hotkeys(struct warp_flow *flow)
{
	for (size_t i = 0; i < flow->lengths.num; i++) {
		if (flow->lengths.array[i].hotkey != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(flow->lengths.array[i].hotkey);
	}

	da_free(flow->lengths);
}

/* Gives each of the lengths the flow offers on top of its own a hotkey. A
 * length that is out of range, one the flow offers already, and anything past
 * the handful a flow is allowed is dropped as it is read, so the list that is
 * kept is exactly the list with keys against it. Expects the lock to be held. */
static void warp_flow_register_length_hotkeys(struct warp_flow *flow)
{
	const char *flow_id = obs_data_get_string(flow->config, WARP_FLOW_ID);
	const char *flow_name = obs_data_get_string(flow->config, WARP_FLOW_NAME);
	obs_data_array_t *lengths = obs_data_get_array(flow->config, WARP_FLOW_LENGTHS);
	obs_data_array_t *kept = obs_data_array_create();
	size_t count = lengths ? obs_data_array_count(lengths) : 0;

	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(lengths, i);
		const int seconds = warp_flow_clamp_length((int)obs_data_get_int(item, WARP_FLOW_LENGTH_SECONDS));
		struct warp_flow_length length = {0};
		struct dstr name = {0};
		struct dstr desc = {0};
		obs_data_array_t *keys;
		bool repeat = false;

		for (size_t j = 0; j < flow->lengths.num; j++)
			repeat = repeat || flow->lengths.array[j].seconds == seconds;

		if (seconds <= 0 || repeat || flow->lengths.num >= WARP_FLOW_LENGTHS_MAX) {
			obs_data_release(item);
			continue;
		}

		warp_flow_length_hotkey_names(flow_id, flow_name, seconds, &name, &desc);

		length.seconds = seconds;
		length.hotkey = obs_hotkey_register_frontend(name.array, desc.array, warp_flow_save_hotkey_cb, NULL);

		keys = obs_data_get_array(item, "hotkey");

		if (keys)
			obs_hotkey_load(length.hotkey, keys);

		obs_data_array_release(keys);
		da_push_back(flow->lengths, &length);

		obs_data_set_int(item, WARP_FLOW_LENGTH_SECONDS, seconds);
		obs_data_array_push_back(kept, item);

		dstr_free(&name);
		dstr_free(&desc);
		obs_data_release(item);
	}

	obs_data_set_array(flow->config, WARP_FLOW_LENGTHS, kept);

	obs_data_array_release(kept);
	obs_data_array_release(lengths);
}

/* expects the lock to be held */
static void warp_flow_rename_length_hotkeys(struct warp_flow *flow)
{
	const char *flow_id = obs_data_get_string(flow->config, WARP_FLOW_ID);
	const char *flow_name = obs_data_get_string(flow->config, WARP_FLOW_NAME);

	for (size_t i = 0; i < flow->lengths.num; i++) {
		struct dstr name = {0};
		struct dstr desc = {0};

		warp_flow_length_hotkey_names(flow_id, flow_name, flow->lengths.array[i].seconds, &name, &desc);

		if (flow->lengths.array[i].hotkey != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_set_description(flow->lengths.array[i].hotkey, desc.array);

		dstr_free(&name);
		dstr_free(&desc);
	}
}

/* Reads the keys the flow's length hotkeys are bound to back into its
 * configuration, where they are saved with it. The list and the hotkeys are
 * built together, so they stand in the same order. Expects the lock to be
 * held. */
static void warp_flow_capture_length_hotkeys(struct warp_flow *flow)
{
	obs_data_array_t *lengths = obs_data_get_array(flow->config, WARP_FLOW_LENGTHS);
	size_t count = lengths ? obs_data_array_count(lengths) : 0;

	for (size_t i = 0; i < count && i < flow->lengths.num; i++) {
		obs_data_t *item = obs_data_array_item(lengths, i);
		obs_data_array_t *keys = obs_hotkey_save(flow->lengths.array[i].hotkey);

		obs_data_set_array(item, "hotkey", keys);

		obs_data_array_release(keys);
		obs_data_release(item);
	}

	obs_data_array_release(lengths);
}

/* Registers the flow's length hotkeys again after its lengths have been
 * changed, giving each length that is still offered the key it was bound to
 * before: a length that is still there is the same length. Expects the lock to
 * be held, and 'before' to be the list as it was. */
static void warp_flow_carry_length_hotkeys(struct warp_flow *flow, obs_data_array_t *before)
{
	obs_data_array_t *after = obs_data_get_array(flow->config, WARP_FLOW_LENGTHS);
	size_t count = after ? obs_data_array_count(after) : 0;
	size_t was = before ? obs_data_array_count(before) : 0;

	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(after, i);
		const int seconds = (int)obs_data_get_int(item, WARP_FLOW_LENGTH_SECONDS);

		if (obs_data_has_user_value(item, "hotkey")) {
			obs_data_release(item);
			continue;
		}

		for (size_t j = 0; j < was; j++) {
			obs_data_t *old = obs_data_array_item(before, j);
			obs_data_array_t *keys = NULL;

			if ((int)obs_data_get_int(old, WARP_FLOW_LENGTH_SECONDS) == seconds)
				keys = obs_data_get_array(old, "hotkey");

			obs_data_release(old);

			if (!keys)
				continue;

			obs_data_set_array(item, "hotkey", keys);
			obs_data_array_release(keys);
			break;
		}

		obs_data_release(item);
	}

	obs_data_array_release(after);

	warp_flow_clear_length_hotkeys(flow);
	warp_flow_register_length_hotkeys(flow);
}

/* expects the lock to be held */
static void warp_flow_register_hotkeys(struct warp_flow *flow)
{
	const char *id = obs_data_get_string(flow->config, WARP_FLOW_ID);
	const char *name = obs_data_get_string(flow->config, WARP_FLOW_NAME);
	struct dstr save_name = {0};
	struct dstr save_desc = {0};
	struct dstr promote_name = {0};
	struct dstr promote_desc = {0};

	warp_flow_hotkey_names(id, name, &save_name, &save_desc, &promote_name, &promote_desc);

	flow->save_hotkey =
		obs_hotkey_register_frontend(save_name.array, save_desc.array, warp_flow_save_hotkey_cb, NULL);
	flow->promote_hotkey =
		obs_hotkey_register_frontend(promote_name.array, promote_desc.array, warp_flow_promote_hotkey_cb, NULL);

	/* the keys these were bound to, saved alongside the flow rather than
	 * left to the frontend: a flow is registered when its scene collection
	 * loads, which is long after the frontend has read its own hotkeys */
	obs_data_array_t *save_keys = obs_data_get_array(flow->config, "hotkey_save");
	obs_data_array_t *promote_keys = obs_data_get_array(flow->config, "hotkey_promote");

	if (save_keys)
		obs_hotkey_load(flow->save_hotkey, save_keys);
	if (promote_keys)
		obs_hotkey_load(flow->promote_hotkey, promote_keys);

	obs_data_array_release(save_keys);
	obs_data_array_release(promote_keys);

	dstr_free(&save_name);
	dstr_free(&save_desc);
	dstr_free(&promote_name);
	dstr_free(&promote_desc);

	warp_flow_register_length_hotkeys(flow);
}

/* expects the lock to be held */
static void warp_flow_rename_hotkeys(struct warp_flow *flow)
{
	const char *id = obs_data_get_string(flow->config, WARP_FLOW_ID);
	const char *name = obs_data_get_string(flow->config, WARP_FLOW_NAME);
	struct dstr save_name = {0};
	struct dstr save_desc = {0};
	struct dstr promote_name = {0};
	struct dstr promote_desc = {0};

	warp_flow_hotkey_names(id, name, &save_name, &save_desc, &promote_name, &promote_desc);

	if (flow->save_hotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_set_description(flow->save_hotkey, save_desc.array);
	if (flow->promote_hotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_set_description(flow->promote_hotkey, promote_desc.array);

	warp_flow_rename_length_hotkeys(flow);

	dstr_free(&save_name);
	dstr_free(&save_desc);
	dstr_free(&promote_name);
	dstr_free(&promote_desc);
}

/* Reads the keys the flow's hotkeys are bound to back into its configuration,
 * so they are saved with it. Expects the lock to be held. */
static void warp_flow_capture_hotkeys(struct warp_flow *flow)
{
	if (flow->save_hotkey != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *keys = obs_hotkey_save(flow->save_hotkey);

		obs_data_set_array(flow->config, "hotkey_save", keys);
		obs_data_array_release(keys);
	}

	if (flow->promote_hotkey != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *keys = obs_hotkey_save(flow->promote_hotkey);

		obs_data_set_array(flow->config, "hotkey_promote", keys);
		obs_data_array_release(keys);
	}

	warp_flow_capture_length_hotkeys(flow);
}

/* ------------------------------------------------------------------------- */
/* the flow list */

/* expects the lock to be held */
static void warp_flow_destroy(struct warp_flow *flow)
{
	if (flow->save_hotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(flow->save_hotkey);
	if (flow->promote_hotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(flow->promote_hotkey);

	warp_flow_clear_length_hotkeys(flow);

	obs_data_release(flow->config);
	bfree(flow);
}

/* expects the lock to be held */
static void warp_flow_clear(void)
{
	for (size_t i = 0; i < warp_flows.num; i++)
		warp_flow_destroy(warp_flows.array[i]);

	da_free(warp_flows);
}

static void warp_flow_set_defaults(obs_data_t *config)
{
	obs_data_set_default_string(config, WARP_FLOW_KIND, WARP_FLOW_KIND_REPLAY);
	obs_data_set_default_string(config, WARP_FLOW_TRIGGER, WARP_FLOW_TRIGGER_HOTKEY);
	obs_data_set_default_string(config, WARP_FLOW_ORDER, WARP_FLOW_ORDER_OLDEST_FIRST);
	obs_data_set_default_int(config, WARP_FLOW_MAX_CLIPS, 0);
	obs_data_set_default_bool(config, WARP_FLOW_ENABLED, true);
	obs_data_set_default_string(config, WARP_FLOW_PLAYBACK, WARP_MEDIA_LOAD_KEEP);
	obs_data_set_default_int(config, WARP_FLOW_SPEED, 0);
	obs_data_set_default_int(config, WARP_FLOW_LENGTH, WARP_FLOW_LENGTH_WHOLE);
}

/* A flow's configuration for the outside, defaults and all: applying one data
 * object to another copies what was set on it, not what it falls back to, so a
 * bare copy of a flow that was never told whether it is switched on reads as
 * switched off. */
static obs_data_t *warp_flow_config_copy(obs_data_t *config)
{
	obs_data_t *copy = warp_flow_data_copy(config);

	if (copy)
		warp_flow_set_defaults(copy);

	return copy;
}

/* expects the lock to be held; takes ownership of nothing */
static struct warp_flow *warp_flow_add_locked(obs_data_t *config)
{
	struct warp_flow *flow = bzalloc(sizeof(struct warp_flow));

	flow->config = warp_flow_data_copy(config);
	flow->save_hotkey = OBS_INVALID_HOTKEY_ID;
	flow->promote_hotkey = OBS_INVALID_HOTKEY_ID;

	da_init(flow->lengths);

	warp_flow_set_defaults(flow->config);

	const char *id = obs_data_get_string(flow->config, WARP_FLOW_ID);

	if (!id || !*id) {
		char *made = warp_flow_make_id();

		obs_data_set_string(flow->config, WARP_FLOW_ID, made);
		bfree(made);
	}

	warp_flow_register_hotkeys(flow);

	da_push_back(warp_flows, &flow);

	return flow;
}

char *warp_flow_add(obs_data_t *config)
{
	char *id;

	if (!config)
		return NULL;

	pthread_mutex_lock(&warp_flow_mutex);
	struct warp_flow *flow = warp_flow_add_locked(config);

	id = bstrdup(obs_data_get_string(flow->config, WARP_FLOW_ID));

	WARP_FLOW_LOG(LOG_INFO, "added flow '%s' (%s), feeding '%s'", obs_data_get_string(flow->config, WARP_FLOW_NAME),
		      obs_data_get_string(flow->config, WARP_FLOW_KIND),
		      obs_data_get_string(flow->config, WARP_FLOW_TARGET_NAME));

	pthread_mutex_unlock(&warp_flow_mutex);

	return id;
}

bool warp_flow_update(const char *id, obs_data_t *config)
{
	bool found = false;

	if (!config)
		return false;

	pthread_mutex_lock(&warp_flow_mutex);
	struct warp_flow *flow = warp_flow_find(id);

	if (flow) {
		/* The lengths it offers are hotkeys, so an edit that says
		 * nothing about them leaves them where they are, and one that
		 * does is read against the keys they are bound to now. */
		const bool lengths_given = obs_data_has_user_value(config, WARP_FLOW_LENGTHS);
		obs_data_array_t *before = NULL;

		if (lengths_given) {
			warp_flow_capture_length_hotkeys(flow);
			before = obs_data_get_array(flow->config, WARP_FLOW_LENGTHS);
		}

		/* the id is the flow, and links point at it: whatever the
		 * caller put in the object it handed over, it stays the one it
		 * was made with */
		obs_data_apply(flow->config, config);
		obs_data_set_string(flow->config, WARP_FLOW_ID, id);

		if (lengths_given) {
			warp_flow_carry_length_hotkeys(flow, before);
			obs_data_array_release(before);
		}

		warp_flow_rename_hotkeys(flow);
		found = true;
	}

	pthread_mutex_unlock(&warp_flow_mutex);

	return found;
}

bool warp_flow_remove(const char *id)
{
	bool found = false;

	pthread_mutex_lock(&warp_flow_mutex);

	for (size_t i = 0; i < warp_flows.num; i++) {
		struct warp_flow *flow = warp_flows.array[i];

		if (!warp_flow_str_eq(obs_data_get_string(flow->config, WARP_FLOW_ID), id))
			continue;

		WARP_FLOW_LOG(LOG_INFO, "removed flow '%s'", obs_data_get_string(flow->config, WARP_FLOW_NAME));

		warp_flow_destroy(flow);
		da_erase(warp_flows, i);
		found = true;
		break;
	}

	/* a flow that is gone is no longer worth feeding through a link */
	if (found) {
		for (size_t i = 0; i < warp_flows.num; i++) {
			obs_data_t *config = warp_flows.array[i]->config;
			obs_data_array_t *links = obs_data_get_array(config, WARP_FLOW_LINKS);
			size_t count = links ? obs_data_array_count(links) : 0;

			for (size_t j = count; j > 0; j--) {
				obs_data_t *item = obs_data_array_item(links, j - 1);

				if (warp_flow_str_eq(obs_data_get_string(item, WARP_FLOW_LINK_ID), id))
					obs_data_array_erase(links, j - 1);

				obs_data_release(item);
			}

			obs_data_array_release(links);
		}
	}

	pthread_mutex_unlock(&warp_flow_mutex);

	return found;
}

obs_data_array_t *warp_flow_list(void)
{
	obs_data_array_t *array = obs_data_array_create();

	pthread_mutex_lock(&warp_flow_mutex);

	for (size_t i = 0; i < warp_flows.num; i++) {
		obs_data_t *copy = warp_flow_config_copy(warp_flows.array[i]->config);

		obs_data_array_push_back(array, copy);
		obs_data_release(copy);
	}

	pthread_mutex_unlock(&warp_flow_mutex);

	return array;
}

obs_data_t *warp_flow_get(const char *id)
{
	obs_data_t *copy = NULL;

	pthread_mutex_lock(&warp_flow_mutex);
	struct warp_flow *flow = warp_flow_find(id);

	if (flow)
		copy = warp_flow_config_copy(flow->config);
	pthread_mutex_unlock(&warp_flow_mutex);

	return copy;
}

obs_data_t *warp_flow_get_by_name(const char *name)
{
	obs_data_t *copy = NULL;

	if (!name || !*name)
		return NULL;

	pthread_mutex_lock(&warp_flow_mutex);

	for (size_t i = 0; i < warp_flows.num; i++) {
		struct warp_flow *flow = warp_flows.array[i];

		if (warp_flow_str_eq(obs_data_get_string(flow->config, WARP_FLOW_NAME), name)) {
			copy = warp_flow_config_copy(flow->config);
			break;
		}
	}

	pthread_mutex_unlock(&warp_flow_mutex);

	return copy;
}

/* ------------------------------------------------------------------------- */
/* feeding a source */

/* The Warp source the flow feeds - a Warp Playlist source for a list, a Warp
 * Media source for an instant replay, which is what 'source_id' says. Looked up
 * by uuid, which survives the source being renamed, and by name for a flow
 * whose configuration was written somewhere the uuid does not mean anything. */
static obs_source_t *warp_flow_resolve_target(const char *uuid, const char *name, const char *source_id)
{
	obs_source_t *source = NULL;

	if (uuid && *uuid)
		source = obs_get_source_by_uuid(uuid);

	if (!source && name && *name)
		source = obs_get_source_by_name(name);

	if (!source)
		return NULL;

	if (!warp_flow_str_eq(obs_source_get_id(source), source_id)) {
		obs_source_release(source);
		return NULL;
	}

	return source;
}

/* keeps the flow pointing at the source it feeds through a rename, and through
 * being looked up by name */
static void warp_flow_note_target(const char *flow_id, obs_source_t *target)
{
	const char *uuid = obs_source_get_uuid(target);
	const char *name = obs_source_get_name(target);

	pthread_mutex_lock(&warp_flow_mutex);
	struct warp_flow *flow = warp_flow_find(flow_id);

	if (flow) {
		if (uuid && *uuid)
			obs_data_set_string(flow->config, WARP_FLOW_TARGET_UUID, uuid);
		if (name && *name)
			obs_data_set_string(flow->config, WARP_FLOW_TARGET_NAME, name);
	}

	pthread_mutex_unlock(&warp_flow_mutex);
}

static void warp_flow_count_clip(const char *flow_id)
{
	pthread_mutex_lock(&warp_flow_mutex);
	struct warp_flow *flow = warp_flow_find(flow_id);

	if (flow)
		obs_data_set_int(flow->config, WARP_FLOW_CLIPS_ADDED,
				 obs_data_get_int(flow->config, WARP_FLOW_CLIPS_ADDED) + 1);

	pthread_mutex_unlock(&warp_flow_mutex);
}

/* Puts 'path' in the flow's playlist. The file list is edited through the
 * source's settings, the same way the playlist's own Clear Playlist does, so
 * the source picks the change up and writes it to the scene collection like
 * any other settings change.
 *
 * Nothing about playback is touched: the playlist keeps playing what it is
 * playing, and reaches the new clip in its turn. */
static void warp_flow_deliver_playlist(const struct warp_flow_delivery *d, const char *path)
{
	obs_source_t *target = warp_flow_resolve_target(d->target_uuid, d->target_name, WARP_PLAYLIST_SOURCE_ID);

	if (!target) {
		WARP_FLOW_LOG(LOG_WARNING, "flow '%s': no Warp Playlist source called '%s' to add '%s' to",
			      d->flow_name, d->target_name ? d->target_name : "", path);
		return;
	}

	warp_flow_note_target(d->flow_id, target);

	obs_data_t *settings = obs_source_get_settings(target);
	obs_data_array_t *current = obs_data_get_array(settings, WARP_FLOW_PLAYLIST_KEY);
	size_t count = current ? obs_data_array_count(current) : 0;
	bool duplicate = false;

	DARRAY(char *) paths;
	da_init(paths);
	da_reserve(paths, count + 1);

	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(current, i);
		const char *value = obs_data_get_string(item, WARP_FLOW_PLAYLIST_VALUE);

		if (value && *value) {
			char *copy = bstrdup(value);

			if (strcmp(value, path) == 0)
				duplicate = true;

			da_push_back(paths, &copy);
		}

		obs_data_release(item);
	}

	obs_data_array_release(current);

	/* A clip only belongs in a list once. The replay buffer never writes
	 * the same file twice, so this is the promote hotkey being pressed
	 * again for a clip that is already there. */
	if (duplicate) {
		WARP_FLOW_LOG(LOG_INFO, "flow '%s': '%s' is already in '%s'", d->flow_name, path, d->target_name);
	} else {
		char *added = bstrdup(path);

		if (d->newest_first)
			da_insert(paths, 0, &added);
		else
			da_push_back(paths, &added);

		/* Over the limit, the far end of the list gives way: the clip
		 * that would be played last is the oldest one, whichever way
		 * round the list is built. */
		while (d->max_clips > 0 && paths.num > (size_t)d->max_clips) {
			size_t drop = d->newest_first ? paths.num - 1 : 0;

			WARP_FLOW_LOG(LOG_INFO, "flow '%s': over %d clips, dropping '%s'", d->flow_name, d->max_clips,
				      paths.array[drop]);

			bfree(paths.array[drop]);
			da_erase(paths, drop);
		}

		obs_data_array_t *next = obs_data_array_create();

		for (size_t i = 0; i < paths.num; i++) {
			obs_data_t *item = obs_data_create();

			obs_data_set_string(item, WARP_FLOW_PLAYLIST_VALUE, paths.array[i]);
			obs_data_array_push_back(next, item);
			obs_data_release(item);
		}

		obs_data_set_array(settings, WARP_FLOW_PLAYLIST_KEY, next);
		obs_data_array_release(next);
		obs_source_update(target, settings);

		warp_flow_count_clip(d->flow_id);

		WARP_FLOW_LOG(LOG_INFO, "flow '%s': added '%s' to '%s' (%s, %d file%s)", d->flow_name, path,
			      d->target_name, d->newest_first ? "newest first" : "oldest first", (int)paths.num,
			      paths.num == 1 ? "" : "s");
	}

	for (size_t i = 0; i < paths.num; i++)
		bfree(paths.array[i]);
	da_free(paths);

	obs_data_release(settings);
	obs_source_release(target);
}

/* Loads 'path' into the flow's Warp Media source, so the clip that was just
 * saved is the one that source is holding. The source is handed the clip
 * through its own warp_media_load proc, which is what decides whether it plays
 * there and then, waits for the source to be brought on screen, or is parked on
 * its first frame; the source emits its loaded action either way, so a Warp
 * Detection filter can bring it on however the operator has set that up.
 *
 * There is no list to add to and nothing to keep: an instant replay source
 * holds the last clip, and the one before it is let go. */
static void warp_flow_deliver_instant(const struct warp_flow_delivery *d, const char *path)
{
	obs_source_t *target = warp_flow_resolve_target(d->target_uuid, d->target_name, WARP_MEDIA_SOURCE_ID);

	if (!target) {
		WARP_FLOW_LOG(LOG_WARNING, "flow '%s': no Warp Media source called '%s' to load '%s' into",
			      d->flow_name, d->target_name ? d->target_name : "", path);
		return;
	}

	warp_flow_note_target(d->flow_id, target);

	calldata_t cd = {0};

	calldata_set_string(&cd, "path", path);
	calldata_set_string(&cd, "playback", d->playback ? d->playback : WARP_MEDIA_LOAD_KEEP);
	calldata_set_int(&cd, "speed", d->speed);

	bool loaded = proc_handler_call(obs_source_get_proc_handler(target), WARP_MEDIA_LOAD_PROC, &cd);

	calldata_free(&cd);

	if (!loaded) {
		WARP_FLOW_LOG(LOG_WARNING, "flow '%s': '%s' would not take '%s'", d->flow_name, d->target_name, path);
		obs_source_release(target);
		return;
	}

	warp_flow_count_clip(d->flow_id);

	if (d->speed > 0)
		WARP_FLOW_LOG(LOG_INFO, "flow '%s': loaded '%s' into '%s' (%s, %d%%)", d->flow_name, path,
			      d->target_name, d->playback, d->speed);
	else
		WARP_FLOW_LOG(LOG_INFO, "flow '%s': loaded '%s' into '%s' (%s)", d->flow_name, path, d->target_name,
			      d->playback);

	obs_source_release(target);
}

static void warp_flow_deliver_one(const struct warp_flow_delivery *d)
{
	if (!d->path)
		return;

	if (d->instant)
		warp_flow_deliver_instant(d, d->path);
	else
		warp_flow_deliver_playlist(d, d->path);
}

/* ------------------------------------------------------------------------- */
/* working out who gets a clip */

static void warp_flow_plan_free(struct warp_flow_plan *plan)
{
	for (size_t i = 0; i < plan->items.num; i++) {
		struct warp_flow_delivery *d = &plan->items.array[i];

		bfree(d->flow_id);
		bfree(d->flow_name);
		bfree(d->target_uuid);
		bfree(d->target_name);
		bfree(d->playback);
		bfree(d->path);
	}

	da_free(plan->items);

	for (size_t i = 0; i < plan->visited.num; i++)
		bfree(plan->visited.array[i]);

	da_free(plan->visited);
}

/* expects the lock to be held */
static bool warp_flow_plan_seen(struct warp_flow_plan *plan, const char *id)
{
	for (size_t i = 0; i < plan->visited.num; i++) {
		if (warp_flow_str_eq(plan->visited.array[i], id))
			return true;
	}

	return false;
}

/* Adds a flow to the plan, then everything it is linked to. 'seconds' is the
 * length the save was asked for, which stands for every flow it reaches, or
 * WARP_FLOW_LENGTH_DEFAULT to leave each flow to its own. Expects the lock to
 * be held. */
static void warp_flow_plan_add(struct warp_flow_plan *plan, struct warp_flow *flow, int depth, int seconds)
{
	const char *id = obs_data_get_string(flow->config, WARP_FLOW_ID);

	if (depth > WARP_FLOW_MAX_LINK_DEPTH) {
		WARP_FLOW_LOG(LOG_WARNING, "flow '%s': links go more than %d deep, following no further",
			      obs_data_get_string(flow->config, WARP_FLOW_NAME), WARP_FLOW_MAX_LINK_DEPTH);
		return;
	}

	if (warp_flow_plan_seen(plan, id))
		return;

	char *seen = bstrdup(id);
	da_push_back(plan->visited, &seen);

	/* a flow that is switched off is still followed through: it is the
	 * flow that takes no clips, not the ones it links to */
	if (obs_data_get_bool(flow->config, WARP_FLOW_ENABLED)) {
		struct warp_flow_delivery d = {0};

		d.flow_id = bstrdup(id);
		d.flow_name = bstrdup(obs_data_get_string(flow->config, WARP_FLOW_NAME));
		d.target_uuid = bstrdup(obs_data_get_string(flow->config, WARP_FLOW_TARGET_UUID));
		d.target_name = bstrdup(obs_data_get_string(flow->config, WARP_FLOW_TARGET_NAME));
		d.instant = warp_flow_str_eq(obs_data_get_string(flow->config, WARP_FLOW_KIND), WARP_FLOW_KIND_INSTANT);
		d.newest_first = warp_flow_str_eq(obs_data_get_string(flow->config, WARP_FLOW_ORDER),
						  WARP_FLOW_ORDER_NEWEST_FIRST);
		d.max_clips = (int)obs_data_get_int(flow->config, WARP_FLOW_MAX_CLIPS);
		d.playback = bstrdup(obs_data_get_string(flow->config, WARP_FLOW_PLAYBACK));
		d.speed = (int)obs_data_get_int(flow->config, WARP_FLOW_SPEED);
		d.seconds = seconds >= 0 ? warp_flow_clamp_length(seconds) : warp_flow_own_length(flow);

		da_push_back(plan->items, &d);
	}

	obs_data_array_t *links = obs_data_get_array(flow->config, WARP_FLOW_LINKS);
	size_t count = links ? obs_data_array_count(links) : 0;

	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(links, i);
		struct warp_flow *linked = warp_flow_find(obs_data_get_string(item, WARP_FLOW_LINK_ID));

		if (linked)
			warp_flow_plan_add(plan, linked, depth + 1, seconds);

		obs_data_release(item);
	}

	obs_data_array_release(links);
}

/* ------------------------------------------------------------------------- */
/* carrying a plan out
 *
 * A flow that takes less than the whole buffer has its clip cut out of the file
 * the buffer wrote, which is disk work: it is done on a thread of its own so
 * that saving never holds the UI up, and on one thread so that clips land in
 * the order they were saved. The deliveries themselves go back to the UI
 * thread, which is where a source is touched from.
 *
 * A plan with nothing to cut has nothing to wait for, and is carried out where
 * it stands. */

struct warp_flow_job {
	struct warp_flow_plan plan;
	/* the file the replay buffer wrote, which every cut is made from */
	char *path;
};

static pthread_mutex_t warp_flow_job_mutex;
static pthread_cond_t warp_flow_job_cond;
static DARRAY(struct warp_flow_job *) warp_flow_jobs;
static pthread_t warp_flow_job_thread;
static bool warp_flow_job_running = false;
static bool warp_flow_job_stopping = false;

static void warp_flow_job_free(struct warp_flow_job *job)
{
	warp_flow_plan_free(&job->plan);
	bfree(job->path);
	bfree(job);
}

/* back on the UI thread, with every file in hand */
static void warp_flow_deliver_task(void *param)
{
	struct warp_flow_job *job = param;

	/* Everything Warp had was given back while the clip was being cut.
	 * Both this and the shutdown that clears the flows run on the UI
	 * thread, so one is never half way through the other. */
	if (warp_flow_ready) {
		for (size_t i = 0; i < job->plan.items.num; i++)
			warp_flow_deliver_one(&job->plan.items.array[i]);
	}

	warp_flow_job_free(job);
}

/* Works out the file each flow in the plan is fed: the one the replay buffer
 * wrote, or the last few seconds of it cut into a file of their own. Flows
 * asking for the same length share a cut, so a save is one cut per length
 * however many flows it reaches. */
static void warp_flow_job_cut(struct warp_flow_job *job)
{
	for (size_t i = 0; i < job->plan.items.num; i++) {
		struct warp_flow_delivery *d = &job->plan.items.array[i];
		char *cut;

		if (d->seconds <= 0) {
			d->path = bstrdup(job->path);
			continue;
		}

		for (size_t j = 0; j < i; j++) {
			const struct warp_flow_delivery *other = &job->plan.items.array[j];

			if (other->seconds == d->seconds && other->path) {
				d->path = bstrdup(other->path);
				break;
			}
		}

		if (d->path)
			continue;

		cut = warp_trim_tail(job->path, d->seconds);

		/* A cut that could not be made is not a clip lost, and neither
		 * is one there was nothing to take off: the flow is fed the
		 * whole clip instead. */
		d->path = cut ? cut : bstrdup(job->path);
	}
}

static void *warp_flow_job_thread_run(void *param)
{
	UNUSED_PARAMETER(param);

	os_set_thread_name("warp-flow-cut");

	for (;;) {
		struct warp_flow_job *job;

		pthread_mutex_lock(&warp_flow_job_mutex);

		while (!warp_flow_job_stopping && !warp_flow_jobs.num)
			pthread_cond_wait(&warp_flow_job_cond, &warp_flow_job_mutex);

		if (warp_flow_job_stopping) {
			pthread_mutex_unlock(&warp_flow_job_mutex);
			break;
		}

		job = warp_flow_jobs.array[0];
		da_erase(warp_flow_jobs, 0);

		pthread_mutex_unlock(&warp_flow_job_mutex);

		warp_flow_job_cut(job);
		obs_queue_task(OBS_TASK_UI, warp_flow_deliver_task, job, false);
	}

	return NULL;
}

static void warp_flow_jobs_start(void)
{
	if (warp_flow_job_running)
		return;

	pthread_mutex_init(&warp_flow_job_mutex, NULL);
	pthread_cond_init(&warp_flow_job_cond, NULL);
	da_init(warp_flow_jobs);

	warp_flow_job_stopping = false;

	if (pthread_create(&warp_flow_job_thread, NULL, warp_flow_job_thread_run, NULL) == 0) {
		warp_flow_job_running = true;
		return;
	}

	WARP_FLOW_LOG(LOG_WARNING, "there is no thread to cut clips on, so they will be fed whole");

	pthread_cond_destroy(&warp_flow_job_cond);
	pthread_mutex_destroy(&warp_flow_job_mutex);
	da_free(warp_flow_jobs);
}

static void warp_flow_jobs_stop(void)
{
	if (!warp_flow_job_running)
		return;

	pthread_mutex_lock(&warp_flow_job_mutex);
	warp_flow_job_stopping = true;
	pthread_cond_signal(&warp_flow_job_cond);
	pthread_mutex_unlock(&warp_flow_job_mutex);

	pthread_join(warp_flow_job_thread, NULL);
	warp_flow_job_running = false;

	/* a clip still waiting to be cut is not going anywhere now */
	for (size_t i = 0; i < warp_flow_jobs.num; i++)
		warp_flow_job_free(warp_flow_jobs.array[i]);

	da_free(warp_flow_jobs);
	pthread_cond_destroy(&warp_flow_job_cond);
	pthread_mutex_destroy(&warp_flow_job_mutex);
}

/* Carries the plan out and takes it over: whatever has to be cut is cut first,
 * and the plan is freed once every flow in it has been fed. */
static void warp_flow_run_plan(struct warp_flow_plan *plan, const char *path)
{
	struct warp_flow_job *job;
	bool cutting = false;

	for (size_t i = 0; i < plan->items.num; i++)
		cutting = cutting || plan->items.array[i].seconds > 0;

	if (!cutting || !warp_flow_job_running) {
		if (cutting)
			WARP_FLOW_LOG(LOG_WARNING, "'%s' cannot be cut just now, feeding it whole", path);

		for (size_t i = 0; i < plan->items.num; i++) {
			plan->items.array[i].path = bstrdup(path);
			warp_flow_deliver_one(&plan->items.array[i]);
		}

		warp_flow_plan_free(plan);
		return;
	}

	job = bzalloc(sizeof(struct warp_flow_job));
	job->plan = *plan;
	job->path = bstrdup(path);

	/* the plan belongs to the job from here */
	da_init(plan->items);
	da_init(plan->visited);

	pthread_mutex_lock(&warp_flow_job_mutex);
	da_push_back(warp_flow_jobs, &job);
	pthread_cond_signal(&warp_flow_job_cond);
	pthread_mutex_unlock(&warp_flow_job_mutex);
}

/* Hands 'path' to the flow that asked for the save, or to every listening flow
 * when nobody did, along with everything those flows are linked to. 'seconds'
 * is the length that was asked for, which stands for every flow the clip
 * reaches, or WARP_FLOW_LENGTH_DEFAULT to leave each of them to its own. */
static void warp_flow_deliver(const char *path, const char *claimed_by, int seconds)
{
	struct warp_flow_plan plan;

	da_init(plan.items);
	da_init(plan.visited);

	pthread_mutex_lock(&warp_flow_mutex);

	if (claimed_by && *claimed_by) {
		struct warp_flow *flow = warp_flow_find(claimed_by);

		if (flow)
			warp_flow_plan_add(&plan, flow, 0, seconds);
	} else {
		for (size_t i = 0; i < warp_flows.num; i++) {
			struct warp_flow *flow = warp_flows.array[i];

			if (warp_flow_str_eq(obs_data_get_string(flow->config, WARP_FLOW_TRIGGER),
					     WARP_FLOW_TRIGGER_LISTEN))
				warp_flow_plan_add(&plan, flow, 0, seconds);
		}
	}

	pthread_mutex_unlock(&warp_flow_mutex);

	if (!plan.items.num)
		WARP_FLOW_LOG(LOG_INFO, "no flow takes '%s'", path);

	warp_flow_run_plan(&plan, path);
}

/* ------------------------------------------------------------------------- */
/* the replay buffer */

/* the file the replay buffer wrote last, which it reports through its own
 * proc handler once it has finished writing it */
static char *warp_flow_read_last_replay(void)
{
	obs_output_t *output = obs_frontend_get_replay_buffer_output();

	if (!output)
		return NULL;

	calldata_t cd = {0};
	char *path = NULL;

	if (proc_handler_call(obs_output_get_proc_handler(output), "get_last_replay", &cd)) {
		const char *last = calldata_string(&cd, "path");

		if (last && *last)
			path = bstrdup(last);
	}

	calldata_free(&cd);
	obs_output_release(output);

	return path;
}

static void warp_flow_replay_saved(void)
{
	char *path = warp_flow_read_last_replay();

	if (!path) {
		WARP_FLOW_LOG(LOG_WARNING, "the replay buffer saved a clip but did not say where");
		return;
	}

	char *claimed_by = NULL;
	int seconds = WARP_FLOW_LENGTH_DEFAULT;

	pthread_mutex_lock(&warp_flow_mutex);

	bfree(warp_flow_last_path);
	warp_flow_last_path = bstrdup(path);

	if (warp_flow_claim_id) {
		if (os_gettime_ns() - warp_flow_claim_ns <= WARP_FLOW_CLAIM_NS) {
			claimed_by = warp_flow_claim_id;
			seconds = warp_flow_claim_seconds;
		} else {
			bfree(warp_flow_claim_id);
		}

		warp_flow_claim_id = NULL;
		warp_flow_claim_seconds = WARP_FLOW_LENGTH_DEFAULT;
	}

	pthread_mutex_unlock(&warp_flow_mutex);

	warp_flow_deliver(path, claimed_by, seconds);

	bfree(claimed_by);
	bfree(path);
}

void warp_flow_set_buffer_prompt(warp_flow_buffer_prompt_t prompt)
{
	warp_flow_buffer_prompt = prompt;
}

bool warp_flow_replay_buffer_active(void)
{
	return obs_frontend_replay_buffer_active();
}

char *warp_flow_last_clip(void)
{
	char *path;

	pthread_mutex_lock(&warp_flow_mutex);
	path = bstrdup(warp_flow_last_path);
	pthread_mutex_unlock(&warp_flow_mutex);

	return path;
}

bool warp_flow_save_replay(const char *id)
{
	return warp_flow_save_replay_length(id, WARP_FLOW_LENGTH_DEFAULT);
}

bool warp_flow_save_replay_length(const char *id, int seconds)
{
	char label[WARP_FLOW_LENGTH_LABEL_SIZE];

	pthread_mutex_lock(&warp_flow_mutex);
	struct warp_flow *flow = warp_flow_find(id);

	if (!flow) {
		pthread_mutex_unlock(&warp_flow_mutex);
		return false;
	}

	char *name = bstrdup(obs_data_get_string(flow->config, WARP_FLOW_NAME));
	const int asked = seconds >= 0 ? warp_flow_clamp_length(seconds) : warp_flow_own_length(flow);

	pthread_mutex_unlock(&warp_flow_mutex);

	warp_flow_length_label(asked, label, sizeof(label));

	/* Nothing to save, and nothing to claim: a claim left standing for a
	 * save that never happens would take the next one somebody else made. */
	if (!obs_frontend_replay_buffer_active()) {
		WARP_FLOW_LOG(LOG_WARNING, "flow '%s': the replay buffer is not running", name);
		bfree(name);
		return false;
	}

	/* The length goes with the claim rather than with the flow: it is the
	 * length of the save that is on its way, so every flow the clip reaches
	 * is fed the same cut. */
	pthread_mutex_lock(&warp_flow_mutex);
	bfree(warp_flow_claim_id);
	warp_flow_claim_id = bstrdup(id);
	warp_flow_claim_ns = os_gettime_ns();
	warp_flow_claim_seconds = seconds;
	pthread_mutex_unlock(&warp_flow_mutex);

	if (asked > 0)
		WARP_FLOW_LOG(LOG_INFO, "flow '%s': saving the replay buffer, keeping the last %s", name, label);
	else
		WARP_FLOW_LOG(LOG_INFO, "flow '%s': saving the replay buffer", name);

	bfree(name);

	obs_frontend_replay_buffer_save();

	return true;
}

bool warp_flow_promote_last(const char *id)
{
	return warp_flow_promote_last_length(id, WARP_FLOW_LENGTH_DEFAULT);
}

bool warp_flow_promote_last_length(const char *id, int seconds)
{
	struct warp_flow_plan plan;
	char *path;

	da_init(plan.items);
	da_init(plan.visited);

	pthread_mutex_lock(&warp_flow_mutex);

	path = bstrdup(warp_flow_last_path);

	struct warp_flow *flow = warp_flow_find(id);

	if (flow && path)
		warp_flow_plan_add(&plan, flow, 0, seconds);

	pthread_mutex_unlock(&warp_flow_mutex);

	if (!path) {
		WARP_FLOW_LOG(LOG_WARNING, "no replay has been saved yet, nothing to add");
		warp_flow_plan_free(&plan);
		return false;
	}

	/* A clip that has to be cut is fed once the cut is made rather than
	 * here and now, so this is the flow taking the clip on, not the clip
	 * having landed. */
	bool delivered = plan.items.num > 0;

	warp_flow_run_plan(&plan, path);
	bfree(path);

	return delivered;
}

int warp_flow_clip_count(const char *id)
{
	char *uuid;
	char *name;
	bool instant;

	pthread_mutex_lock(&warp_flow_mutex);
	struct warp_flow *flow = warp_flow_find(id);

	if (!flow) {
		pthread_mutex_unlock(&warp_flow_mutex);
		return -1;
	}

	uuid = bstrdup(obs_data_get_string(flow->config, WARP_FLOW_TARGET_UUID));
	name = bstrdup(obs_data_get_string(flow->config, WARP_FLOW_TARGET_NAME));
	instant = warp_flow_str_eq(obs_data_get_string(flow->config, WARP_FLOW_KIND), WARP_FLOW_KIND_INSTANT);

	pthread_mutex_unlock(&warp_flow_mutex);

	obs_source_t *target =
		warp_flow_resolve_target(uuid, name, instant ? WARP_MEDIA_SOURCE_ID : WARP_PLAYLIST_SOURCE_ID);

	bfree(uuid);
	bfree(name);

	if (!target)
		return -1;

	int count = -1;

	if (instant) {
		/* one clip at a time: the source is either holding one or it is
		 * still empty */
		obs_data_t *settings = obs_source_get_settings(target);
		const char *file = obs_data_get_string(settings, "local_file");

		count = (file && *file) ? 1 : 0;
		obs_data_release(settings);
	} else {
		calldata_t cd = {0};

		if (proc_handler_call(obs_source_get_proc_handler(target), "warp_playlist_status", &cd))
			count = (int)calldata_int(&cd, "count");

		calldata_free(&cd);
	}

	obs_source_release(target);

	return count;
}

/* ------------------------------------------------------------------------- */
/* saving and loading, with the scene collection */

static void warp_flow_save_to(obs_data_t *save_data)
{
	obs_data_t *warp = obs_data_create();
	obs_data_array_t *array = obs_data_array_create();

	pthread_mutex_lock(&warp_flow_mutex);

	for (size_t i = 0; i < warp_flows.num; i++) {
		struct warp_flow *flow = warp_flows.array[i];

		warp_flow_capture_hotkeys(flow);

		obs_data_t *copy = warp_flow_data_copy(flow->config);

		obs_data_array_push_back(array, copy);
		obs_data_release(copy);
	}

	pthread_mutex_unlock(&warp_flow_mutex);

	obs_data_set_array(warp, WARP_FLOW_ARRAY_KEY, array);
	obs_data_set_obj(save_data, WARP_FLOW_MODULE_KEY, warp);

	obs_data_array_release(array);
	obs_data_release(warp);
}

static void warp_flow_load_from(obs_data_t *save_data)
{
	obs_data_t *warp = obs_data_get_obj(save_data, WARP_FLOW_MODULE_KEY);
	obs_data_array_t *array = warp ? obs_data_get_array(warp, WARP_FLOW_ARRAY_KEY) : NULL;
	size_t count = array ? obs_data_array_count(array) : 0;

	pthread_mutex_lock(&warp_flow_mutex);

	warp_flow_clear();

	for (size_t i = 0; i < count; i++) {
		obs_data_t *config = obs_data_array_item(array, i);

		warp_flow_add_locked(config);
		obs_data_release(config);
	}

	pthread_mutex_unlock(&warp_flow_mutex);

	obs_data_array_release(array);
	obs_data_release(warp);

	if (count)
		WARP_FLOW_LOG(LOG_INFO, "loaded %d flow%s from the scene collection", (int)count,
			      count == 1 ? "" : "s");
}

static void warp_flow_save_cb(obs_data_t *save_data, bool saving, void *private_data)
{
	UNUSED_PARAMETER(private_data);

	if (saving)
		warp_flow_save_to(save_data);
	else
		warp_flow_load_from(save_data);
}

static void warp_flow_frontend_event(enum obs_frontend_event event, void *private_data)
{
	UNUSED_PARAMETER(private_data);

	switch (event) {
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED:
		warp_flow_replay_saved();
		break;
	/* On the way out the flows go for good - the hotkeys they registered
	 * are given back while libobs is still there to take them, rather than
	 * at module unload - and so does anything still being cut: there is
	 * nowhere left for it to land. */
	case OBS_FRONTEND_EVENT_EXIT:
		warp_flow_jobs_stop();
		pthread_mutex_lock(&warp_flow_mutex);
		warp_flow_clear();
		pthread_mutex_unlock(&warp_flow_mutex);
		break;
	/* the flows of the collection being left go with it; the one being
	 * loaded brings its own */
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		pthread_mutex_lock(&warp_flow_mutex);
		warp_flow_clear();
		pthread_mutex_unlock(&warp_flow_mutex);
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------------- */

void warp_flow_init(void)
{
	if (warp_flow_ready)
		return;

	pthread_mutex_init(&warp_flow_mutex, NULL);
	da_init(warp_flows);
	warp_flow_jobs_start();

	warp_flow_ready = true;

	obs_frontend_add_event_callback(warp_flow_frontend_event, NULL);
	obs_frontend_add_save_callback(warp_flow_save_cb, NULL);
}

void warp_flow_shutdown(void)
{
	if (!warp_flow_ready)
		return;

	warp_flow_ready = false;
	warp_flow_buffer_prompt = NULL;

	obs_frontend_remove_event_callback(warp_flow_frontend_event, NULL);
	obs_frontend_remove_save_callback(warp_flow_save_cb, NULL);

	warp_flow_jobs_stop();

	pthread_mutex_lock(&warp_flow_mutex);
	warp_flow_clear();

	bfree(warp_flow_last_path);
	warp_flow_last_path = NULL;
	bfree(warp_flow_claim_id);
	warp_flow_claim_id = NULL;
	warp_flow_claim_seconds = WARP_FLOW_LENGTH_DEFAULT;

	pthread_mutex_unlock(&warp_flow_mutex);

	pthread_mutex_destroy(&warp_flow_mutex);
}
