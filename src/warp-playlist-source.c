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
#include <string.h>

#include <obs-module.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <media-playback/media-playback.h>

#ifdef WARP_HAVE_FRONTEND_API
/* the transition's own properties are shown in the window OBS opens for the
 * transitions in its own list */
#include <obs-frontend-api.h>
#endif

#include "warp-events.h"

#define WARP_PL_LOG(level, format, ...) \
	blog(level, "[Warp Playlist '%s']: " format, obs_source_get_name(s->source), ##__VA_ARGS__)

/* How far ahead of the switch point the next file is opened, in wall-clock
 * milliseconds. The item is opened, allowed to decode its first frame, then
 * paused at the start, so the incoming half of the transition has picture
 * from its very first frame. */
#define WARP_PL_PRELOAD_LEAD_MS 1000

/* how long an item is given to start playing before it is written off and the
 * playlist moves on, in seconds */
#define WARP_PL_STALL_SECONDS 3.0f

/* how long an item plays before it may be transitioned away from early */
#define WARP_PL_MIN_ITEM_SECONDS 0.25f

/* default transition and its duration, in milliseconds */
#define WARP_PL_DEFAULT_TRANSITION "fade_transition"
#define WARP_PL_DEFAULT_TRANSITION_MS 300

/* where a stinger swaps the incoming file in by default, in milliseconds */
#define WARP_PL_DEFAULT_STINGER_MS 1000

/* libobs ids of the two transitions the playlist has to know something about:
 * a cut is instant, and a stinger swaps the incoming file in partway through
 * its video rather than at the end. What either of them, or any other
 * transition, is configured with is left to the transition itself. */
#define WARP_PL_TR_CUT "cut_transition"
#define WARP_PL_TR_STINGER "obs_stinger_transition"

/* the transitions the playlist used to keep settings of its own for, read once
 * more when a playlist saved with them is loaded */
#define WARP_PL_TR_SWIPE "swipe_transition"
#define WARP_PL_TR_SLIDE "slide_transition"
#define WARP_PL_TR_FADE_TO_COLOR "fade_to_color_transition"

/* tp_type values of the stinger transition: its transition point is given
 * either in milliseconds or in frames of the stinger video */
#define WARP_PL_STINGER_TP_TIME 0
#define WARP_PL_STINGER_TP_FRAME 1

/* transition_timing values: overlap the end of the file, or run once it is
 * over */
#define WARP_PL_TIMING_OVERLAP "overlap"
#define WARP_PL_TIMING_AFTER "after"

/* transition_scale values */
#define WARP_PL_SCALE_FIT "fit"
#define WARP_PL_SCALE_STRETCH "stretch"
#define WARP_PL_SCALE_DOWN_ONLY "down_only"

/* How far ahead of the real end of a file the playlist moves on when the
 * transition is not meant to overlap it, in milliseconds. Roughly a frame, so
 * the outgoing half of the transition still has a picture to work with. */
#define WARP_PL_END_GUARD_MS 50

struct warp_playlist_source;

/* per-hotkey context for parametrized hotkeys: 'value' is a signed frame
 * count for step hotkeys and a speed percentage for preset hotkeys */
struct warp_pl_hotkey_binding {
	struct warp_playlist_source *s;
	int value;
};

struct warp_playlist_source {
	obs_source_t *source;

	/* the transition renders the playlist; every item is handed to it */
	obs_source_t *transition;
	/* item currently played */
	obs_source_t *current;
	/* item transitioned away from, kept alive until the transition ends */
	obs_source_t *prev;
	/* next item, opened early so it has picture when the transition starts */
	obs_source_t *preloaded;
	char *preloaded_path;
	bool preload_armed;

	/* Guards everything below it. Creating, releasing and re-parenting a
	 * source all reach into libobs' global source bookkeeping, so none of
	 * that may be done from under this lock: see warp_pl_unlock(). */
	pthread_mutex_t mutex;

	/* ----------------------------------------------------------------- */
	/* work handed to warp_pl_unlock() to carry out once the lock is gone */

	/* sources to release */
	DARRAY(obs_source_t *) retired;
	/* an item to open and play, and whether to animate the switch */
	bool play_armed;
	size_t play_order_pos;
	bool play_transition;
	/* an item to open ahead of the switch point */
	bool preload_requested;
	size_t preload_order_pos;
	/* Bumped every time what the playlist is meant to be playing changes.
	 * Opening a file takes long enough for a stop, or another switch, to
	 * land while it is going on; the item is thrown away rather than put on
	 * screen when the count it was opened for is no longer the current
	 * one. */
	uint64_t play_gen;
	/* what to hand the transition; the target holds a reference, and is
	 * NULL to show nothing */
	bool transition_armed;
	bool transition_use;
	obs_source_t *transition_target;
	/* media signals: whatever reacts to one is free to drive this playlist
	 * straight back, so they are never emitted under the lock */
	bool signal_started;
	bool signal_ended;
	/* set while the outermost warp_pl_unlock() works through the above */
	bool deferring;

	/* playlist as configured, and the order it is played back in */
	DARRAY(char *) paths;
	DARRAY(size_t) order;
	/* index into 'order' of the current item, or DARRAY_INVALID */
	size_t pos;

	uint32_t cx;
	uint32_t cy;

	char *transition_id;
	/* transition the settings ask for, picked up by the next tick */
	char *pending_transition_id;
	/* settings for the transition itself, handed to it by the next tick */
	obs_data_t *pending_transition_settings;
	/* What every transition that has been used is configured with, keyed by
	 * libobs id. A transition is configured through its own properties,
	 * which write to the transition rather than to this source, so this is
	 * read back from the one that is running rather than the other way
	 * round. Keeping the ones that are not running means picking a
	 * transition back up brings its settings with it. */
	obs_data_t *transition_store;
	/* the store as it last stood in the source settings, to tell settings
	 * being loaded, or set from the outside, from the copy this source
	 * wrote there itself */
	char *transition_store_json;
	bool transition_store_loaded;
	/* alignment and scaling wait for the next tick along with them */
	bool pending_transition_layout;
	uint32_t transition_alignment;
	enum obs_transition_scale_type transition_scale;

	uint32_t transition_ms;
	/* where a stinger swaps the incoming file in, in milliseconds */
	uint32_t stinger_point_ms;
	bool transition_is_cut;
	bool transition_is_stinger;
	/* whether the transition overlaps the end of the file or runs after it */
	bool transition_overlap;

	/* speed every item starts at, and the live speed of the current item */
	int base_speed;
	int speed;

	bool auto_advance;
	bool loop_playlist;
	bool shuffle;
	bool hw_decode;
	bool is_linear_alpha;
	bool restart_on_activate;
	bool clear_on_media_end;
	enum video_range_type range;

	bool showing_nothing;
	enum obs_media_state state;
	/* seconds the current item has been current, used to skip files that
	 * never start playing */
	float cur_age;
	uint64_t rand_state;

	obs_hotkey_pair_id play_pause_hotkey;
	obs_hotkey_id stop_hotkey;
	obs_hotkey_id next_hotkey;
	obs_hotkey_id prev_hotkey;
	obs_hotkey_id first_hotkey;
	obs_hotkey_id restart_hotkey;
	obs_hotkey_id clear_hotkey;

	struct warp_pl_hotkey_binding speed_bindings[WARP_NUM_SPEED_PRESETS];
	struct warp_pl_hotkey_binding step_bindings[WARP_NUM_STEP_HOTKEYS];
};

static const char *warp_playlist_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Warp.PlaylistSource.Name");
}

/* ------------------------------------------------------------------------- */
/* locking
 *
 * The playlist opens and closes a source for every file it plays. Creating one
 * takes libobs' global source list lock (and its audio source list lock),
 * releasing one takes them again to unlink it, and handing one to the
 * transition re-parents it. None of that may happen from under s->mutex,
 * because libobs calls back into this source with those same global locks
 * already held, from threads that then want s->mutex:
 *
 *   hotkey thread:   obs->hotkeys.mutex     -> s->mutex (every hotkey below)
 *   UI thread:       obs->data.sources_mutex -> obs->hotkeys.mutex
 *                    (obs_save_sources() -> obs_save_source() ->
 *                     obs_hotkeys_save_source(), on every project save)
 *   graphics thread: s->mutex -> obs->data.sources_mutex, if a source is
 *                    created or released with the lock held
 *
 * That last edge closes the cycle and wedges the graphics, UI and hotkey
 * threads for good, which takes the whole of OBS down with it. So the lock
 * guards this source's own fields and nothing else: work that has to touch
 * libobs' source graph is queued on the struct and run by warp_pl_unlock()
 * once the lock has been dropped. */

static void warp_pl_start(struct warp_playlist_source *s, size_t order_pos, bool use_transition, uint64_t gen);
static void warp_pl_open_preload(struct warp_playlist_source *s, size_t order_pos, uint64_t gen);

/* queues 'source' for release; expects s->mutex to be held */
static void warp_pl_retire(struct warp_playlist_source *s, obs_source_t *source)
{
	if (source)
		da_push_back(s->retired, &source);
}

/* Hands 'item' to the transition once the lock is dropped; 'item' may be NULL,
 * to show nothing. Expects s->mutex to be held. */
static void warp_pl_arm_transition(struct warp_playlist_source *s, obs_source_t *item, bool use_transition)
{
	warp_pl_retire(s, s->transition_target);

	s->transition_target = item ? obs_source_get_ref(item) : NULL;
	s->transition_armed = true;
	s->transition_use = use_transition;
}

/* how long the transition runs for, in milliseconds; a cut is instant whatever
 * the duration property is set to. Expects s->mutex to be held. */
static inline uint32_t warp_pl_transition_ms(struct warp_playlist_source *s)
{
	return s->transition_is_cut ? 0 : s->transition_ms;
}

static enum obs_media_state warp_pl_state(struct warp_playlist_source *s)
{
	enum obs_media_state state;

	pthread_mutex_lock(&s->mutex);
	state = s->state;
	pthread_mutex_unlock(&s->mutex);

	return state;
}

/* the transition, with a reference held: it is swapped out from the tick, so
 * the callbacks that render and enumerate it cannot use it bare */
static obs_source_t *warp_pl_get_transition(struct warp_playlist_source *s)
{
	obs_source_t *transition;

	pthread_mutex_lock(&s->mutex);
	transition = obs_source_get_ref(s->transition);
	pthread_mutex_unlock(&s->mutex);

	return transition;
}

/* Drops s->mutex and works through whatever was queued while it was held.
 * Anything queued by that work is picked up in the same pass, so a nested
 * warp_pl_unlock() only has to drop the lock. */
static void warp_pl_unlock(struct warp_playlist_source *s)
{
	if (s->deferring) {
		pthread_mutex_unlock(&s->mutex);
		return;
	}

	s->deferring = true;

	for (;;) {
		DARRAY(obs_source_t *) retired;

		bool transition_armed = s->transition_armed;
		bool transition_use = s->transition_use;
		uint32_t transition_ms = warp_pl_transition_ms(s);
		obs_source_t *transition = NULL;
		obs_source_t *target = NULL;

		bool play = s->play_armed;
		size_t play_order_pos = s->play_order_pos;
		bool play_transition = s->play_transition;

		bool preload = s->preload_requested;
		size_t preload_order_pos = s->preload_order_pos;
		uint64_t gen = s->play_gen;

		bool started = s->signal_started;
		bool ended = s->signal_ended;

		retired.da = s->retired.da;
		da_init(s->retired);

		if (transition_armed) {
			transition = obs_source_get_ref(s->transition);
			target = s->transition_target;
			s->transition_target = NULL;
		}

		s->transition_armed = false;
		s->play_armed = false;
		s->preload_requested = false;
		s->signal_started = false;
		s->signal_ended = false;

		if (!retired.num && !transition_armed && !play && !preload && !started && !ended) {
			s->deferring = false;
			pthread_mutex_unlock(&s->mutex);
			da_free(retired);
			return;
		}

		pthread_mutex_unlock(&s->mutex);

		/* the transition takes its own references, so it is told about
		 * the change before the outgoing items are let go of */
		if (transition)
			obs_transition_start(transition, OBS_TRANSITION_MODE_AUTO, transition_use ? transition_ms : 0,
					     target);

		if (transition)
			obs_source_release(transition);
		if (target)
			obs_source_release(target);

		for (size_t i = 0; i < retired.num; i++)
			obs_source_release(retired.array[i]);
		da_free(retired);

		if (ended)
			obs_source_media_ended(s->source);
		if (started)
			obs_source_media_started(s->source);

		if (play)
			warp_pl_start(s, play_order_pos, play_transition, gen);
		if (preload)
			warp_pl_open_preload(s, preload_order_pos, gen);

		pthread_mutex_lock(&s->mutex);
	}
}

/* ------------------------------------------------------------------------- */
/* playlist bookkeeping (all of these expect s->mutex to be held) */

static uint64_t warp_pl_rand(struct warp_playlist_source *s)
{
	uint64_t x = s->rand_state;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	s->rand_state = x;

	return x;
}

/* rebuilds the play order; shuffled orders are reshuffled on every pass so a
 * looping playlist does not repeat the same random order forever */
static void warp_pl_build_order(struct warp_playlist_source *s)
{
	da_resize(s->order, s->paths.num);

	for (size_t i = 0; i < s->paths.num; i++)
		s->order.array[i] = i;

	if (!s->shuffle || s->order.num < 2)
		return;

	for (size_t i = s->order.num - 1; i > 0; i--) {
		size_t j = (size_t)(warp_pl_rand(s) % (uint64_t)(i + 1));
		size_t tmp = s->order.array[i];

		s->order.array[i] = s->order.array[j];
		s->order.array[j] = tmp;
	}
}

static const char *warp_pl_path_at(struct warp_playlist_source *s, size_t order_pos)
{
	if (order_pos >= s->order.num)
		return NULL;

	size_t idx = s->order.array[order_pos];

	return idx < s->paths.num ? s->paths.array[idx] : NULL;
}

/* Where playback goes from the current position. 'dir' is +1 for the next item
 * and -1 for the previous one. Returns false when there is nowhere to go, which
 * is the end (or start) of a playlist that is not looping. */
static bool warp_pl_step_pos(struct warp_playlist_source *s, int dir, size_t *out)
{
	if (!s->order.num)
		return false;

	if (s->pos >= s->order.num) {
		*out = dir >= 0 ? 0 : s->order.num - 1;
		return true;
	}

	if (dir >= 0) {
		if (s->pos + 1 < s->order.num) {
			*out = s->pos + 1;
			return true;
		}
		if (!s->loop_playlist)
			return false;
		*out = 0;
	} else {
		if (s->pos > 0) {
			*out = s->pos - 1;
			return true;
		}
		if (!s->loop_playlist)
			return false;
		*out = s->order.num - 1;
	}

	return true;
}

/* ------------------------------------------------------------------------- */
/* playback */

/* what an item is opened with, read under s->mutex so that the item itself can
 * be created with the lock dropped */
struct warp_pl_item_config {
	int speed;
	bool hw_decode;
	bool is_linear_alpha;
	enum video_range_type range;
};

/* expects s->mutex to be held */
static void warp_pl_read_item_config(struct warp_playlist_source *s, struct warp_pl_item_config *cfg)
{
	cfg->speed = s->base_speed;
	cfg->hw_decode = s->hw_decode;
	cfg->is_linear_alpha = s->is_linear_alpha;
	cfg->range = s->range;
}

/* expects s->mutex NOT to be held */
static obs_source_t *warp_pl_create_item(struct warp_playlist_source *s, const char *path,
					 const struct warp_pl_item_config *cfg)
{
	obs_data_t *settings = obs_data_create();

	obs_data_set_bool(settings, "is_local_file", true);
	obs_data_set_string(settings, "local_file", path);
	obs_data_set_bool(settings, "looping", false);
	/* the item holds its last frame instead of going black: the playlist
	 * decides what is shown once an item is done */
	obs_data_set_bool(settings, "clear_on_media_end", false);
	obs_data_set_bool(settings, "restart_on_activate", false);
	obs_data_set_bool(settings, "close_when_inactive", false);
	obs_data_set_bool(settings, "hw_decode", cfg->hw_decode);
	obs_data_set_bool(settings, "linear_alpha", cfg->is_linear_alpha);
	obs_data_set_int(settings, "color_range", cfg->range);
	obs_data_set_int(settings, "speed_percent", cfg->speed);
	/* one settings dump per playlist item would drown the log */
	obs_data_set_bool(settings, "log_changes", false);

	obs_source_t *item = obs_source_create_private(WARP_MEDIA_SOURCE_ID, path, settings);
	obs_data_release(settings);

	if (!item)
		WARP_PL_LOG(LOG_WARNING, "failed to open '%s'", path);

	return item;
}

static void warp_pl_call_item_proc(obs_source_t *item, const char *proc, const char *arg, long long value)
{
	if (!item)
		return;

	calldata_t cd = {0};

	calldata_init(&cd);
	calldata_set_int(&cd, arg, value);
	proc_handler_call(obs_source_get_proc_handler(item), proc, &cd);
	calldata_free(&cd);
}

/* expects s->mutex to be held */
static void warp_pl_drop_preloaded(struct warp_playlist_source *s)
{
	if (s->preloaded) {
		obs_source_media_stop(s->preloaded);
		warp_pl_retire(s, s->preloaded);
		s->preloaded = NULL;
	}

	bfree(s->preloaded_path);
	s->preloaded_path = NULL;
	s->preload_armed = false;
}

/* Puts 'item' (which may be NULL, to show nothing) on screen and retires
 * whatever was shown before it, taking over the caller's reference to 'item'.
 * The transition is told about the change by warp_pl_unlock(). Expects
 * s->mutex to be held. */
static void warp_pl_show(struct warp_playlist_source *s, obs_source_t *item, bool use_transition)
{
	warp_pl_arm_transition(s, item, use_transition);

	/* the outgoing item stays alive until its transition has played out;
	 * the transition holds its own reference, this one just keeps it
	 * around long enough to be stopped cleanly in the tick */
	if (s->prev) {
		obs_source_media_stop(s->prev);
		warp_pl_retire(s, s->prev);
	}

	s->prev = s->current;
	s->current = item;
	s->showing_nothing = item == NULL;
}

/* Asks for the item at 'order_pos' to be played. The file is opened, and the
 * item put on screen, by warp_pl_start() once the lock has been dropped.
 * Expects s->mutex to be held. */
static void warp_pl_play_pos(struct warp_playlist_source *s, size_t order_pos, bool use_transition)
{
	if (!warp_pl_path_at(s, order_pos))
		return;

	s->play_gen++;
	s->play_armed = true;
	s->play_order_pos = order_pos;
	s->play_transition = use_transition;
}

/* end of the playlist, or end of an item with auto advance turned off;
 * expects s->mutex to be held */
static void warp_pl_end(struct warp_playlist_source *s)
{
	if (s->state == OBS_MEDIA_STATE_ENDED)
		return;

	/* the item that ended is kept, so it can be restarted or resumed;
	 * it is only taken off screen */
	if (s->clear_on_media_end) {
		warp_pl_arm_transition(s, NULL, true);
		s->showing_nothing = true;
	}

	s->play_gen++;
	s->state = OBS_MEDIA_STATE_ENDED;
	s->signal_ended = true;
}

/* expects s->mutex to be held */
static void warp_pl_advance(struct warp_playlist_source *s, int dir)
{
	size_t target;

	if (!warp_pl_step_pos(s, dir, &target)) {
		warp_pl_end(s);
		return;
	}

	/* a shuffled playlist gets a fresh order every time it wraps */
	if (s->shuffle && s->order.num > 1 && ((dir >= 0 && target == 0) || (dir < 0 && target + 1 == s->order.num))) {
		warp_pl_build_order(s);
		s->pos = s->order.num;
	}

	warp_pl_play_pos(s, target, true);
}

/* expects s->mutex to be held */
static void warp_pl_stop(struct warp_playlist_source *s)
{
	warp_pl_drop_preloaded(s);

	/* nothing queued up, or already being opened, is wanted any more */
	s->play_gen++;
	s->play_armed = false;
	s->preload_requested = false;

	if (s->current)
		obs_source_media_stop(s->current);
	if (s->prev)
		obs_source_media_stop(s->prev);

	warp_pl_arm_transition(s, NULL, false);

	warp_pl_retire(s, s->prev);
	warp_pl_retire(s, s->current);
	s->prev = NULL;
	s->current = NULL;
	s->showing_nothing = true;

	s->pos = s->order.num;
	s->state = OBS_MEDIA_STATE_STOPPED;
}

/* starts the playlist over from its first item; expects s->mutex to be held */
static void warp_pl_restart_playlist(struct warp_playlist_source *s)
{
	if (!s->order.num)
		return;

	if (s->shuffle)
		warp_pl_build_order(s);

	warp_pl_drop_preloaded(s);
	warp_pl_play_pos(s, 0, true);
}

/* restarts the item that is playing right now, at the configured speed;
 * expects s->mutex to be held */
static void warp_pl_restart_current(struct warp_playlist_source *s)
{
	if (!s->current) {
		warp_pl_restart_playlist(s);
		return;
	}

	/* the item is not held by the transition once the playlist has ended
	 * with "show nothing", so put it back before restarting it */
	if (s->showing_nothing) {
		warp_pl_arm_transition(s, s->current, true);
		s->showing_nothing = false;
	}

	warp_pl_drop_preloaded(s);

	/* an item that is still being opened is not wanted: the one on screen
	 * is the one being restarted */
	s->play_gen++;
	s->play_armed = false;

	s->speed = s->base_speed;
	warp_pl_call_item_proc(s->current, "warp_set_speed", "speed", s->base_speed);
	obs_source_media_restart(s->current);

	s->state = OBS_MEDIA_STATE_PLAYING;
	s->signal_started = true;
}

/* expects s->mutex to be held */
static void warp_pl_apply_speed(struct warp_playlist_source *s, int speed)
{
	if (speed < MP_SPEED_MIN)
		speed = MP_SPEED_MIN;
	else if (speed > MP_SPEED_MAX)
		speed = MP_SPEED_MAX;

	if (!s->current || speed == s->speed)
		return;

	s->speed = speed;
	warp_pl_call_item_proc(s->current, "warp_set_speed", "speed", speed);

	WARP_PL_LOG(LOG_INFO, "speed set to %d%%", speed);
}

/* Opens the item warp_pl_play_pos() asked for and puts it on screen. Expects
 * s->mutex NOT to be held: opening a file means creating a source. */
static void warp_pl_start(struct warp_playlist_source *s, size_t order_pos, bool use_transition, uint64_t gen)
{
	struct warp_pl_item_config cfg;
	obs_source_t *item = NULL;
	const char *at;
	char *path = NULL;

	pthread_mutex_lock(&s->mutex);

	at = warp_pl_path_at(s, order_pos);

	if (at)
		path = bstrdup(at);

	/* Reuse the preloaded item when it is the one being played and it has
	 * been parked at its first frame. One that has not been armed yet is
	 * still running from wherever it got to while it was being opened, so
	 * the file is opened again rather than played from partway in. */
	if (path && s->preloaded && s->preload_armed && s->preloaded_path && strcmp(s->preloaded_path, path) == 0) {
		item = s->preloaded;
		s->preloaded = NULL;
	}

	/* anything still preloaded is not what is being played */
	warp_pl_drop_preloaded(s);
	warp_pl_read_item_config(s, &cfg);

	warp_pl_unlock(s);

	if (!path)
		return;

	if (item)
		obs_source_media_play_pause(item, false);
	else
		item = warp_pl_create_item(s, path, &cfg);

	if (!item) {
		bfree(path);
		return;
	}

	/* every item starts at the configured speed, whatever the previous
	 * item was being played back at */
	warp_pl_call_item_proc(item, "warp_set_speed", "speed", cfg.speed);

	pthread_mutex_lock(&s->mutex);

	at = warp_pl_path_at(s, order_pos);

	/* the playlist can be edited, or stopped, while the file is opening */
	if (s->play_gen != gen || !at || strcmp(at, path) != 0) {
		warp_pl_retire(s, item);
		warp_pl_unlock(s);
		bfree(path);
		return;
	}

	s->pos = order_pos;
	s->cur_age = 0.0f;
	s->speed = cfg.speed;

	warp_pl_show(s, item, use_transition);

	s->state = OBS_MEDIA_STATE_PLAYING;
	s->signal_started = true;

	WARP_PL_LOG(LOG_INFO, "playing %d/%d: %s", (int)(order_pos + 1), (int)s->order.num, path);

	warp_pl_unlock(s);
	bfree(path);
}

/* Opens the item warp_pl_preload_next() asked for, ahead of the switch point.
 * Expects s->mutex NOT to be held. */
static void warp_pl_open_preload(struct warp_playlist_source *s, size_t order_pos, uint64_t gen)
{
	struct warp_pl_item_config cfg;
	const char *at;
	char *path = NULL;

	pthread_mutex_lock(&s->mutex);

	at = warp_pl_path_at(s, order_pos);

	if (at && !s->preloaded && s->play_gen == gen)
		path = bstrdup(at);

	warp_pl_read_item_config(s, &cfg);

	pthread_mutex_unlock(&s->mutex);

	if (!path)
		return;

	obs_source_t *item = warp_pl_create_item(s, path, &cfg);

	if (!item) {
		bfree(path);
		return;
	}

	pthread_mutex_lock(&s->mutex);

	at = warp_pl_path_at(s, order_pos);

	if (s->preloaded || s->play_gen != gen || !at || strcmp(at, path) != 0) {
		/* the playlist moved on while the file was opening */
		obs_source_media_stop(item);
		warp_pl_retire(s, item);
	} else {
		s->preloaded = item;
		s->preloaded_path = path;
		s->preload_armed = false;
		path = NULL;
	}

	warp_pl_unlock(s);
	bfree(path);
}

/* ------------------------------------------------------------------------- */
/* transition */

/* whether 'data' holds nothing at all */
static bool warp_pl_data_empty(obs_data_t *data)
{
	obs_data_item_t *first = obs_data_first(data);
	bool empty = !first;

	obs_data_item_release(&first);

	return empty;
}

/* a copy of 'data' that nothing else holds on to, or NULL for NULL */
static obs_data_t *warp_pl_data_copy(obs_data_t *data)
{
	obs_data_t *copy;

	if (!data)
		return NULL;

	copy = obs_data_create();
	obs_data_apply(copy, data);

	return copy;
}

/* The settings the playlist used to keep flat in its own, one property per
 * transition it knew about. They are moved into the store the first time a
 * playlist saved with them is loaded, so a transition that was set up before
 * the transitions were given their own properties stays set up. The properties
 * themselves are left where they are, so going back to an older build finds
 * them. Returns NULL when none of them were ever set. */
static obs_data_t *warp_pl_legacy_transition_settings(const char *id, obs_data_t *settings)
{
	obs_data_t *tr_settings = obs_data_create();

	if (strcmp(id, WARP_PL_TR_SWIPE) == 0 || strcmp(id, WARP_PL_TR_SLIDE) == 0) {
		if (obs_data_has_user_value(settings, "transition_direction"))
			obs_data_set_string(tr_settings, "direction",
					    obs_data_get_string(settings, "transition_direction"));

		if (obs_data_has_user_value(settings, "transition_swipe_in"))
			obs_data_set_bool(tr_settings, "swipe_in", obs_data_get_bool(settings, "transition_swipe_in"));
	} else if (strcmp(id, WARP_PL_TR_FADE_TO_COLOR) == 0) {
		if (obs_data_has_user_value(settings, "transition_color"))
			obs_data_set_int(tr_settings, "color", obs_data_get_int(settings, "transition_color"));

		if (obs_data_has_user_value(settings, "transition_switch_point"))
			obs_data_set_int(tr_settings, "switch_point",
					 obs_data_get_int(settings, "transition_switch_point"));
	} else if (strcmp(id, WARP_PL_TR_STINGER) == 0) {
		if (obs_data_has_user_value(settings, "stinger_path"))
			obs_data_set_string(tr_settings, "path", obs_data_get_string(settings, "stinger_path"));

		if (obs_data_has_user_value(settings, "stinger_transition_point")) {
			obs_data_set_int(tr_settings, "tp_type", WARP_PL_STINGER_TP_TIME);
			obs_data_set_int(tr_settings, "transition_point",
					 obs_data_get_int(settings, "stinger_transition_point"));
		}

		if (obs_data_has_user_value(settings, "stinger_hw_decode"))
			obs_data_set_bool(tr_settings, "hw_decode", obs_data_get_bool(settings, "stinger_hw_decode"));
	}

	if (warp_pl_data_empty(tr_settings)) {
		obs_data_release(tr_settings);
		return NULL;
	}

	return tr_settings;
}

/* How far into a stinger video the incoming file is swapped in, in
 * milliseconds. The transition takes the point either in milliseconds or in
 * frames; a frame count is turned into a time with the OBS frame rate, which is
 * the one a stinger cut for the stream runs at. */
static uint32_t warp_pl_stinger_point_ms(obs_data_t *tr_settings)
{
	struct obs_video_info ovi;
	int64_t point;

	if (!tr_settings)
		return 0;

	point = obs_data_get_int(tr_settings, "transition_point");

	if (point <= 0)
		return 0;

	if (obs_data_get_int(tr_settings, "tp_type") != WARP_PL_STINGER_TP_FRAME)
		return (uint32_t)point;

	if (!obs_get_video_info(&ovi) || !ovi.fps_num)
		return 0;

	return (uint32_t)(point * 1000 * ovi.fps_den / ovi.fps_num);
}

/* Reads back what the transition that is running is configured with: its own
 * properties write to the transition itself, so the store, and with it the
 * settings the source is saved with, only catch up when they are read off it.
 * Expects s->mutex NOT to be held. */
static void warp_pl_capture_transition_settings(struct warp_playlist_source *s)
{
	obs_source_t *tr = warp_pl_get_transition(s);
	obs_data_t *live;
	obs_data_t *copy;
	char *id;

	if (!tr)
		return;

	pthread_mutex_lock(&s->mutex);
	id = bstrdup(s->transition_id);
	pthread_mutex_unlock(&s->mutex);

	if (id) {
		live = obs_source_get_settings(tr);
		copy = warp_pl_data_copy(live);

		pthread_mutex_lock(&s->mutex);

		/* the transitions that have nothing to configure are left out
		 * of the settings the source is saved with entirely */
		if (!warp_pl_data_empty(copy))
			obs_data_set_obj(s->transition_store, id, copy);

		/* where a stinger swaps the incoming file in is one of its own
		 * settings, so the playlist picks it up along with them */
		if (strcmp(id, WARP_PL_TR_STINGER) == 0)
			s->stinger_point_ms = warp_pl_stinger_point_ms(copy);

		pthread_mutex_unlock(&s->mutex);

		obs_data_release(copy);
		obs_data_release(live);
		bfree(id);
	}

	obs_source_release(tr);
}

/* Takes the transition settings the source was saved, or set from the outside,
 * with over the store, and returns whether it did, so that the transition that
 * is running is handed them. The store is where the transitions' own properties
 * end up, so the copy this source wrote to the settings itself is left alone.
 * Expects s->mutex NOT to be held. */
static bool warp_pl_load_transition_store(struct warp_playlist_source *s, obs_data_t *settings, const char *id)
{
	obs_data_t *stored = obs_data_get_obj(settings, "transition_settings");
	const char *json = stored ? obs_data_get_json(stored) : NULL;
	bool loaded = false;

	pthread_mutex_lock(&s->mutex);

	if (json && (!s->transition_store_json || strcmp(s->transition_store_json, json) != 0)) {
		obs_data_t *store = obs_data_create_from_json(json);

		if (store) {
			obs_data_release(s->transition_store);
			s->transition_store = store;

			bfree(s->transition_store_json);
			s->transition_store_json = bstrdup(json);

			loaded = true;
		}
	} else if (!stored && !s->transition_store_loaded) {
		obs_data_t *legacy = warp_pl_legacy_transition_settings(id, settings);

		if (legacy) {
			obs_data_set_obj(s->transition_store, id, legacy);
			obs_data_release(legacy);

			loaded = true;
		}
	}

	s->transition_store_loaded = true;

	pthread_mutex_unlock(&s->mutex);

	obs_data_release(stored);

	return loaded;
}

/* The settings a transition is created with: the ones it was last configured
 * with, or, for a stinger that has never been configured, a transition point
 * that leaves room for the swap behind it rather than the zero it would
 * otherwise start out at. The caller owns the reference. Expects s->mutex to be
 * held. */
static obs_data_t *warp_pl_transition_settings(struct warp_playlist_source *s, const char *id)
{
	obs_data_t *tr_settings = obs_data_get_obj(s->transition_store, id);

	if (!tr_settings && strcmp(id, WARP_PL_TR_STINGER) == 0) {
		tr_settings = obs_data_create();

		obs_data_set_int(tr_settings, "tp_type", WARP_PL_STINGER_TP_TIME);
		obs_data_set_int(tr_settings, "transition_point", WARP_PL_DEFAULT_STINGER_MS);

		obs_data_set_obj(s->transition_store, id, tr_settings);
	}

	return tr_settings;
}

static uint32_t warp_pl_alignment_from_string(const char *val)
{
	if (strcmp(val, "topleft") == 0)
		return OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
	if (strcmp(val, "top") == 0)
		return OBS_ALIGN_TOP;
	if (strcmp(val, "topright") == 0)
		return OBS_ALIGN_TOP | OBS_ALIGN_RIGHT;
	if (strcmp(val, "left") == 0)
		return OBS_ALIGN_LEFT;
	if (strcmp(val, "right") == 0)
		return OBS_ALIGN_RIGHT;
	if (strcmp(val, "bottomleft") == 0)
		return OBS_ALIGN_BOTTOM | OBS_ALIGN_LEFT;
	if (strcmp(val, "bottom") == 0)
		return OBS_ALIGN_BOTTOM;
	if (strcmp(val, "bottomright") == 0)
		return OBS_ALIGN_BOTTOM | OBS_ALIGN_RIGHT;

	return OBS_ALIGN_CENTER;
}

static enum obs_transition_scale_type warp_pl_scale_from_string(const char *val)
{
	if (strcmp(val, WARP_PL_SCALE_STRETCH) == 0)
		return OBS_TRANSITION_SCALE_STRETCH;
	if (strcmp(val, WARP_PL_SCALE_DOWN_ONLY) == 0)
		return OBS_TRANSITION_SCALE_MAX_ONLY;

	return OBS_TRANSITION_SCALE_ASPECT;
}

/* Brings the live transition up to date with the settings without swapping it
 * out: the type is the one that is already running, only what it is configured
 * with changed. Expects s->mutex NOT to be held. */
static void warp_pl_configure_transition(struct warp_playlist_source *s, obs_data_t *tr_settings, bool layout,
					 uint32_t alignment, enum obs_transition_scale_type scale)
{
	obs_source_t *tr;

	if (!tr_settings && !layout)
		return;

	tr = warp_pl_get_transition(s);

	if (!tr)
		return;

	if (tr_settings)
		obs_source_update(tr, tr_settings);

	if (layout) {
		obs_transition_set_alignment(tr, alignment);
		obs_transition_set_scale_type(tr, scale);
	}

	obs_source_release(tr);
}

/* Creates the transition the settings ask for and moves whatever is on screen
 * over to it, or configures the one already running when only its settings
 * changed. Runs from the tick, so it cannot race with another swap, and with
 * s->mutex dropped, so that creating and releasing the transition does not take
 * libobs' global source lock from under it. */
static void warp_pl_swap_transition(struct warp_playlist_source *s)
{
	enum obs_transition_scale_type scale;
	obs_data_t *tr_settings;
	obs_source_t *carry = NULL;
	obs_source_t *old;
	uint32_t alignment;
	uint32_t cx;
	uint32_t cy;
	bool layout;
	char *id;

	pthread_mutex_lock(&s->mutex);
	id = s->pending_transition_id;
	s->pending_transition_id = NULL;
	tr_settings = s->pending_transition_settings;
	s->pending_transition_settings = NULL;
	layout = s->pending_transition_layout;
	s->pending_transition_layout = false;
	alignment = s->transition_alignment;
	scale = s->transition_scale;
	cx = s->cx;
	cy = s->cy;
	pthread_mutex_unlock(&s->mutex);

	if (!id) {
		warp_pl_configure_transition(s, tr_settings, layout, alignment, scale);
		obs_data_release(tr_settings);
		return;
	}

	obs_source_t *tr = obs_source_create_private(id, id, tr_settings);

	obs_data_release(tr_settings);

	if (!tr) {
		WARP_PL_LOG(LOG_WARNING, "could not create transition '%s'", id);
		bfree(id);
		return;
	}

	obs_transition_set_alignment(tr, alignment);
	obs_transition_set_scale_type(tr, scale);
	obs_transition_set_size(tr, cx, cy);

	pthread_mutex_lock(&s->mutex);
	if (s->current && !s->showing_nothing)
		carry = obs_source_get_ref(s->current);
	pthread_mutex_unlock(&s->mutex);

	/* carry whatever is on screen over, before anything can render the
	 * new transition and find it empty */
	if (carry) {
		obs_transition_set(tr, carry);
		obs_source_release(carry);
	}

	pthread_mutex_lock(&s->mutex);
	old = s->transition;
	s->transition = tr;
	bfree(s->transition_id);
	s->transition_id = id;
	pthread_mutex_unlock(&s->mutex);

	obs_source_add_active_child(s->source, tr);

	if (old) {
		obs_transition_clear(old);
		obs_source_remove_active_child(s->source, old);
		obs_source_release(old);
	}
}

/* ------------------------------------------------------------------------- */
/* obs source callbacks */

static void warp_playlist_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "auto_advance", true);
	obs_data_set_default_bool(settings, "loop_playlist", false);
	obs_data_set_default_bool(settings, "shuffle", false);
	obs_data_set_default_string(settings, "transition", WARP_PL_DEFAULT_TRANSITION);
	obs_data_set_default_int(settings, "transition_duration", WARP_PL_DEFAULT_TRANSITION_MS);
	obs_data_set_default_string(settings, "transition_timing", WARP_PL_TIMING_OVERLAP);
	obs_data_set_default_string(settings, "transition_scale", WARP_PL_SCALE_FIT);
	obs_data_set_default_string(settings, "transition_alignment", "center");
	obs_data_set_default_int(settings, "speed_percent", 100);
	obs_data_set_default_bool(settings, "restart_on_activate", true);
	obs_data_set_default_bool(settings, "clear_on_media_end", true);
	obs_data_set_default_bool(settings, "linear_alpha", false);
}

static const char *media_filter =
	" (*.mp4 *.m4v *.ts *.mov *.mxf *.flv *.mkv *.avi *.mp3 *.ogg *.aac *.wav *.gif *.webm);;";
static const char *video_filter = " (*.mp4 *.m4v *.ts *.mov *.mxf *.flv *.mkv *.avi *.gif *.webm);;";
static const char *audio_filter = " (*.mp3 *.aac *.ogg *.wav);;";

/* Only what the playlist puts around the transition is shown here; what the
 * transition itself is configured with belongs to the transition. */
static bool warp_pl_transition_changed(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	const char *id = obs_data_get_string(settings, "transition");
	bool is_cut = strcmp(id, WARP_PL_TR_CUT) == 0;
	bool is_stinger = strcmp(id, WARP_PL_TR_STINGER) == 0;

	UNUSED_PARAMETER(prop);

	/* a cut is instant, and a stinger runs for as long as its video */
	obs_property_set_visible(obs_properties_get(props, "transition_duration"), !is_cut && !is_stinger);
	obs_property_set_visible(obs_properties_get(props, "transition_timing"), !is_cut);

	return true;
}

#ifdef WARP_HAVE_FRONTEND_API
/* Opens the properties of the transition that is running, the same window OBS
 * opens for the transitions in its own list, preview and all. What is changed
 * there is applied to the transition on the spot, and read back into the
 * settings this source is saved with. */
static bool warp_pl_transition_props_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	struct warp_playlist_source *s = data;
	obs_source_t *transition;

	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);

	/* no source of its own to configure: the properties were asked for by
	 * id rather than for a playlist that exists */
	if (!s)
		return false;

	transition = warp_pl_get_transition(s);

	if (!transition)
		return false;

	obs_frontend_open_source_properties(transition);
	obs_source_release(transition);

	return false;
}
#endif

static obs_properties_t *warp_pl_transition_properties(struct warp_playlist_source *s)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *prop;
	const char *id;

	prop = obs_properties_add_list(props, "transition", obs_module_text("Warp.Playlist.Transition"),
				       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	for (size_t i = 0; obs_enum_transition_types(i, &id); i++) {
		const char *name = obs_source_get_display_name(id);

		obs_property_list_add_string(prop, name ? name : id, id);
	}

	obs_property_set_modified_callback(prop, warp_pl_transition_changed);

#ifdef WARP_HAVE_FRONTEND_API
	prop = obs_properties_add_button2(props, "transition_properties",
					  obs_module_text("Warp.Playlist.Transition.Properties"),
					  warp_pl_transition_props_clicked, s);
	obs_property_set_long_description(prop, obs_module_text("Warp.Playlist.Transition.Properties.Desc"));
#else
	UNUSED_PARAMETER(s);
#endif

	prop = obs_properties_add_int_slider(props, "transition_duration",
					     obs_module_text("Warp.Playlist.TransitionDuration"), 0, 10000, 50);
	obs_property_int_set_suffix(prop, " ms");

	prop = obs_properties_add_list(props, "transition_timing", obs_module_text("Warp.Playlist.TransitionTiming"),
				       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, obs_module_text("Warp.Playlist.TransitionTiming.Overlap"),
				     WARP_PL_TIMING_OVERLAP);
	obs_property_list_add_string(prop, obs_module_text("Warp.Playlist.TransitionTiming.After"),
				     WARP_PL_TIMING_AFTER);
	obs_property_set_long_description(prop, obs_module_text("Warp.Playlist.TransitionTiming.Desc"));

	prop = obs_properties_add_list(props, "transition_scale", obs_module_text("Warp.Playlist.Transition.Scale"),
				       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, obs_module_text("Warp.Playlist.Transition.Scale.Fit"), WARP_PL_SCALE_FIT);
	obs_property_list_add_string(prop, obs_module_text("Warp.Playlist.Transition.Scale.Stretch"),
				     WARP_PL_SCALE_STRETCH);
	obs_property_list_add_string(prop, obs_module_text("Warp.Playlist.Transition.Scale.DownOnly"),
				     WARP_PL_SCALE_DOWN_ONLY);
	obs_property_set_long_description(prop, obs_module_text("Warp.Playlist.Transition.Scale.Desc"));

	prop = obs_properties_add_list(props, "transition_alignment",
				       obs_module_text("Warp.Playlist.Transition.Alignment"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, obs_module_text("Warp.Playlist.Align.Center"), "center");
	obs_property_list_add_string(prop, obs_module_text("Warp.Playlist.Align.TopLeft"), "topleft");
	obs_property_list_add_string(prop, obs_module_text("Warp.Playlist.Align.Top"), "top");
	obs_property_list_add_string(prop, obs_module_text("Warp.Playlist.Align.TopRight"), "topright");
	obs_property_list_add_string(prop, obs_module_text("Warp.Playlist.Align.Left"), "left");
	obs_property_list_add_string(prop, obs_module_text("Warp.Playlist.Align.Right"), "right");
	obs_property_list_add_string(prop, obs_module_text("Warp.Playlist.Align.BottomLeft"), "bottomleft");
	obs_property_list_add_string(prop, obs_module_text("Warp.Playlist.Align.Bottom"), "bottom");
	obs_property_list_add_string(prop, obs_module_text("Warp.Playlist.Align.BottomRight"), "bottomright");

	return props;
}

static obs_properties_t *warp_playlist_getproperties(void *data)
{
	struct warp_playlist_source *s = data;
	struct dstr filter = {0};
	struct dstr path = {0};
	obs_property_t *prop;

	obs_properties_t *props = obs_properties_create();

	dstr_copy(&filter, obs_module_text("Warp.FileFilter.AllMedia"));
	dstr_cat(&filter, media_filter);
	dstr_cat(&filter, obs_module_text("Warp.FileFilter.Video"));
	dstr_cat(&filter, video_filter);
	dstr_cat(&filter, obs_module_text("Warp.FileFilter.Audio"));
	dstr_cat(&filter, audio_filter);
	dstr_cat(&filter, obs_module_text("Warp.FileFilter.All"));
	dstr_cat(&filter, " (*.*)");

	/* the file dialog opens where the last added file came from */
	if (s) {
		pthread_mutex_lock(&s->mutex);
		if (s->paths.num) {
			const char *slash;

			dstr_copy(&path, s->paths.array[s->paths.num - 1]);
			dstr_replace(&path, "\\", "/");
			slash = strrchr(path.array, '/');
			if (slash)
				dstr_resize(&path, slash - path.array + 1);
		}
		pthread_mutex_unlock(&s->mutex);
	}

	obs_properties_add_editable_list(props, "playlist", obs_module_text("Warp.Playlist.Files"),
					 OBS_EDITABLE_LIST_TYPE_FILES, filter.array, path.array);
	dstr_free(&filter);
	dstr_free(&path);

	obs_properties_add_bool(props, "shuffle", obs_module_text("Warp.Playlist.Shuffle"));

	prop = obs_properties_add_bool(props, "auto_advance", obs_module_text("Warp.Playlist.AutoAdvance"));
	obs_property_set_long_description(prop, obs_module_text("Warp.Playlist.AutoAdvance.Desc"));

	obs_properties_add_bool(props, "loop_playlist", obs_module_text("Warp.Playlist.Loop"));

	obs_properties_add_group(props, "transition_group", obs_module_text("Warp.Playlist.Group.Transition"),
				 OBS_GROUP_NORMAL, warp_pl_transition_properties(s));

	prop = obs_properties_add_int_slider(props, "speed_percent", obs_module_text("Warp.Video.Speed"), MP_SPEED_MIN,
					     MP_SPEED_MAX, 1);
	obs_property_int_set_suffix(prop, "%");
	obs_property_set_long_description(prop, obs_module_text("Warp.Playlist.Speed.Desc"));

	obs_properties_add_bool(props, "restart_on_activate", obs_module_text("Warp.Playlist.RestartOnActivate"));

	obs_properties_add_bool(props, "clear_on_media_end", obs_module_text("Warp.Playlist.ClearOnEnd"));

	obs_properties_add_bool(props, "hw_decode", obs_module_text("Warp.Video.HardwareDecode"));

	prop = obs_properties_add_list(props, "color_range", obs_module_text("Warp.Video.ColorRange"),
				       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("Warp.Video.ColorRange.Auto"), VIDEO_RANGE_DEFAULT);
	obs_property_list_add_int(prop, obs_module_text("Warp.Video.ColorRange.Partial"), VIDEO_RANGE_PARTIAL);
	obs_property_list_add_int(prop, obs_module_text("Warp.Video.ColorRange.Full"), VIDEO_RANGE_FULL);

	obs_properties_add_bool(props, "linear_alpha", obs_module_text("Warp.Video.LinearAlpha"));

	return props;
}

/* returns true when the configured file list differs from the loaded one */
static bool warp_pl_load_playlist(struct warp_playlist_source *s, obs_data_t *settings)
{
	obs_data_array_t *array = obs_data_get_array(settings, "playlist");
	size_t count = array ? obs_data_array_count(array) : 0;
	bool changed = count != s->paths.num;

	DARRAY(char *) paths;

	da_init(paths);
	da_reserve(paths, count);

	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(array, i);
		const char *path = obs_data_get_string(item, "value");

		if (path && *path) {
			if (!changed && (i >= s->paths.num || strcmp(s->paths.array[i], path) != 0))
				changed = true;

			char *copy = bstrdup(path);
			da_push_back(paths, &copy);
		} else {
			changed = true;
		}

		obs_data_release(item);
	}

	obs_data_array_release(array);

	if (!changed) {
		for (size_t i = 0; i < paths.num; i++)
			bfree(paths.array[i]);
		da_free(paths);
		return false;
	}

	for (size_t i = 0; i < s->paths.num; i++)
		bfree(s->paths.array[i]);
	da_free(s->paths);

	s->paths.array = paths.array;
	s->paths.num = paths.num;
	s->paths.capacity = paths.capacity;

	return true;
}

static void warp_playlist_update(void *data, obs_data_t *settings)
{
	struct warp_playlist_source *s = data;

	bool active = obs_source_active(s->source);
	const char *transition_id = obs_data_get_string(settings, "transition");
	int speed = (int)obs_data_get_int(settings, "speed_percent");

	if (!transition_id || !*transition_id)
		transition_id = WARP_PL_DEFAULT_TRANSITION;

	if (speed < MP_SPEED_MIN || speed > MP_SPEED_MAX)
		speed = 100;

	/* the transition's own properties write to the transition rather than
	 * to these settings, so what it is running with is read back before the
	 * settings it was saved with are looked at */
	warp_pl_capture_transition_settings(s);

	bool store_loaded = warp_pl_load_transition_store(s, settings, transition_id);

	uint32_t alignment = warp_pl_alignment_from_string(obs_data_get_string(settings, "transition_alignment"));
	enum obs_transition_scale_type scale =
		warp_pl_scale_from_string(obs_data_get_string(settings, "transition_scale"));
	bool overlap = strcmp(obs_data_get_string(settings, "transition_timing"), WARP_PL_TIMING_AFTER) != 0;

	pthread_mutex_lock(&s->mutex);

	/* zero until the first update, which is the one create() makes */
	int prev_speed = s->speed;
	char *playing = NULL;

	if (s->pos < s->order.num) {
		const char *cur = warp_pl_path_at(s, s->pos);

		if (cur)
			playing = bstrdup(cur);
	}

	bool shuffle = obs_data_get_bool(settings, "shuffle");
	bool order_changed = shuffle != s->shuffle;

	s->shuffle = shuffle;
	s->auto_advance = obs_data_get_bool(settings, "auto_advance");
	s->loop_playlist = obs_data_get_bool(settings, "loop_playlist");
	s->transition_ms = (uint32_t)obs_data_get_int(settings, "transition_duration");
	s->transition_is_cut = strcmp(transition_id, WARP_PL_TR_CUT) == 0;
	s->transition_is_stinger = strcmp(transition_id, WARP_PL_TR_STINGER) == 0;
	s->transition_overlap = overlap;
	s->restart_on_activate = obs_data_get_bool(settings, "restart_on_activate");
	s->clear_on_media_end = obs_data_get_bool(settings, "clear_on_media_end");
	s->hw_decode = obs_data_get_bool(settings, "hw_decode");
	s->is_linear_alpha = obs_data_get_bool(settings, "linear_alpha");
	s->range = (enum video_range_type)obs_data_get_int(settings, "color_range");
	s->base_speed = speed;

	if (warp_pl_load_playlist(s, settings))
		order_changed = true;

	if (order_changed) {
		warp_pl_build_order(s);
		warp_pl_drop_preloaded(s);

		/* an item still being opened was picked out of the order that
		 * has just been thrown away */
		s->play_gen++;
		s->play_armed = false;
		s->preload_requested = false;

		/* keep playing the current file if it survived the edit */
		s->pos = s->order.num;
		if (playing) {
			for (size_t i = 0; i < s->order.num; i++) {
				const char *path = warp_pl_path_at(s, i);

				if (path && strcmp(path, playing) == 0) {
					s->pos = i;
					break;
				}
			}

			/* the file that was playing was taken out of the list */
			if (s->pos >= s->order.num)
				warp_pl_stop(s);
		}
	}

	/* The tick creates the transition when the type changed and hands
	 * whichever one ends up running its settings. What the settings ask for
	 * is compared against the transition that is queued for the next tick
	 * when there is one, so that picking a transition and going back to the
	 * one that is already running before the tick comes around leaves it
	 * alone rather than swapping in the one in between. */
	const char *wanted = s->pending_transition_id ? s->pending_transition_id : s->transition_id;
	bool type_changed = !wanted || strcmp(wanted, transition_id) != 0;

	if (type_changed) {
		bfree(s->pending_transition_id);
		s->pending_transition_id = NULL;

		/* nothing to swap when it is the one already running */
		if (!s->transition_id || strcmp(s->transition_id, transition_id) != 0)
			s->pending_transition_id = bstrdup(transition_id);
	}

	/* What the transition is configured with only has to be handed over
	 * when a transition is about to be created, or when settings were just
	 * loaded: everything else that configures one goes through the
	 * transition's own properties, which apply to it directly. A stinger
	 * reloads its video every time it is updated, which is not something an
	 * unrelated property being edited should set off. */
	obs_data_t *tr_settings = warp_pl_transition_settings(s, transition_id);

	s->stinger_point_ms = s->transition_is_stinger ? warp_pl_stinger_point_ms(tr_settings) : 0;

	if (type_changed || store_loaded) {
		obs_data_release(s->pending_transition_settings);
		s->pending_transition_settings = warp_pl_data_copy(tr_settings);
	}

	obs_data_release(tr_settings);

	if (s->transition_alignment != alignment || s->transition_scale != scale)
		s->pending_transition_layout = true;

	s->transition_alignment = alignment;
	s->transition_scale = scale;

	/* the property applies to the file that is playing as well as to the
	 * ones that follow it */
	s->speed = s->base_speed;
	warp_pl_call_item_proc(s->current, "warp_set_speed", "speed", s->base_speed);

	/* start at the top when nothing is playing: either the source was just
	 * created, or the file that was playing is no longer in the list */
	if (s->order.num && s->pos >= s->order.num && (!s->restart_on_activate || active))
		warp_pl_play_pos(s, 0, false);

	int speed_now = s->speed;

	warp_pl_unlock(s);

	/* the Speed property applies to the file that is playing, so it is a
	 * speed change like any hotkey-driven one */
	if (prev_speed && prev_speed != speed_now)
		warp_signal_speed_changed(s->source, speed_now, prev_speed, WARP_SPEED_CHANGE_SET);

	bfree(playing);
}

/* The transitions are created and dropped as the type is picked, so what they
 * are configured with is written out with the playlist rather than left in the
 * transition that happens to be running. */
static void warp_playlist_save(void *data, obs_data_t *settings)
{
	struct warp_playlist_source *s = data;
	obs_data_t *store = obs_data_create();

	/* the transition's own properties write to the transition, so what it
	 * is running with is read back before it is written out */
	warp_pl_capture_transition_settings(s);

	pthread_mutex_lock(&s->mutex);

	obs_data_apply(store, s->transition_store);

	/* what the settings are left holding, so that the next update tells
	 * settings being loaded from the copy written here */
	bfree(s->transition_store_json);
	s->transition_store_json = bstrdup(obs_data_get_json(store));

	pthread_mutex_unlock(&s->mutex);

	obs_data_set_obj(settings, "transition_settings", store);
	obs_data_release(store);
}

/* ------------------------------------------------------------------------- */
/* hotkeys */

static bool warp_playlist_play_hotkey(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct warp_playlist_source *s = data;

	if (!pressed || !obs_source_showing(s->source))
		return false;

	if (warp_pl_state(s) == OBS_MEDIA_STATE_PLAYING)
		return false;

	obs_source_media_play_pause(s->source, false);
	return true;
}

static bool warp_playlist_pause_hotkey(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct warp_playlist_source *s = data;

	if (!pressed || !obs_source_showing(s->source))
		return false;

	if (warp_pl_state(s) != OBS_MEDIA_STATE_PLAYING)
		return false;

	obs_source_media_play_pause(s->source, true);
	return true;
}

static void warp_playlist_stop_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct warp_playlist_source *s = data;

	if (!pressed || !obs_source_showing(s->source))
		return;

	obs_source_media_stop(s->source);
}

static void warp_playlist_next_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct warp_playlist_source *s = data;

	if (!pressed || !obs_source_showing(s->source))
		return;

	pthread_mutex_lock(&s->mutex);
	warp_pl_advance(s, 1);
	warp_pl_unlock(s);
}

static void warp_playlist_prev_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct warp_playlist_source *s = data;

	if (!pressed || !obs_source_showing(s->source))
		return;

	pthread_mutex_lock(&s->mutex);
	warp_pl_advance(s, -1);
	warp_pl_unlock(s);
}

static void warp_playlist_first_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct warp_playlist_source *s = data;

	if (!pressed || !obs_source_showing(s->source))
		return;

	pthread_mutex_lock(&s->mutex);
	warp_pl_restart_playlist(s);
	warp_pl_unlock(s);
}

/* Restarts the file that is playing, the way Restart Current Video does. The
 * signal is emitted with the mutex released, as the speed and stepping ones
 * are: whatever reacts to it is free to drive this playlist straight back. */
static void warp_pl_restart_current_command(struct warp_playlist_source *s)
{
	bool restarted;

	pthread_mutex_lock(&s->mutex);
	warp_pl_restart_current(s);
	restarted = s->current || s->play_armed;
	warp_pl_unlock(s);

	if (restarted)
		warp_signal_media_action(s->source, WARP_MEDIA_ACTION_RESTART);
}

static void warp_playlist_restart_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct warp_playlist_source *s = data;

	if (!pressed || !obs_source_showing(s->source))
		return;

	warp_pl_restart_current_command(s);
}

/* empties the source's file list for good */
static void warp_pl_clear_playlist(struct warp_playlist_source *s)
{
	pthread_mutex_lock(&s->mutex);

	if (!s->paths.num) {
		warp_pl_unlock(s);
		return;
	}

	/* clearing empties the source's file list for good, so write what was
	 * in it to the log: a playlist cleared by a mis-hit mid-show can then
	 * be rebuilt from there */
	WARP_PL_LOG(LOG_INFO, "clearing playlist (%d files)", (int)s->paths.num);
	for (size_t i = 0; i < s->paths.num; i++)
		WARP_PL_LOG(LOG_INFO, "  cleared: %s", s->paths.array[i]);

	warp_pl_stop(s);

	warp_pl_unlock(s);

	obs_data_t *settings = obs_source_get_settings(s->source);
	obs_data_array_t *empty = obs_data_array_create();

	obs_data_set_array(settings, "playlist", empty);
	obs_data_array_release(empty);
	obs_source_update(s->source, settings);
	obs_data_release(settings);
}

static void warp_playlist_clear_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct warp_playlist_source *s = data;

	if (!pressed || !obs_source_showing(s->source))
		return;

	warp_pl_clear_playlist(s);
}

/* Changes the speed of the file that is playing and reports what it did.
 * 'value' is the speed to play at when 'absolute' is set, and the number of
 * points to move by otherwise. The signal is emitted with the mutex released:
 * whatever reacts to it is free to drive this playlist straight back. */
static void warp_pl_change_speed(struct warp_playlist_source *s, int value, bool absolute, const char *change)
{
	int prev_speed;
	int speed;

	pthread_mutex_lock(&s->mutex);
	prev_speed = s->speed;
	warp_pl_apply_speed(s, absolute ? value : s->speed + value);
	speed = s->speed;
	warp_pl_unlock(s);

	if (speed != prev_speed)
		warp_signal_speed_changed(s->source, speed, prev_speed, change);
}

static void warp_playlist_speed_up_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct warp_playlist_source *s = data;

	if (!pressed || !obs_source_showing(s->source))
		return;

	warp_pl_change_speed(s, WARP_SPEED_STEP, false, WARP_SPEED_CHANGE_INCREASED);
}

static void warp_playlist_speed_down_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct warp_playlist_source *s = data;

	if (!pressed || !obs_source_showing(s->source))
		return;

	warp_pl_change_speed(s, -WARP_SPEED_STEP, false, WARP_SPEED_CHANGE_DECREASED);
}

static void warp_playlist_speed_reset_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct warp_playlist_source *s = data;

	if (!pressed || !obs_source_showing(s->source))
		return;

	warp_pl_change_speed(s, 100, true, WARP_SPEED_CHANGE_SET);
}

static void warp_playlist_speed_preset_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct warp_pl_hotkey_binding *b = data;
	struct warp_playlist_source *s = b->s;

	if (!pressed || !obs_source_showing(s->source))
		return;

	warp_pl_change_speed(s, b->value, true, WARP_SPEED_CHANGE_SET);
}

/* Steps the file that is playing by 'frames', negative to step backward. The
 * signal is emitted with the mutex released, for the reason above. */
static void warp_pl_step_frames(struct warp_playlist_source *s, int frames)
{
	bool stepped = false;

	pthread_mutex_lock(&s->mutex);

	if (s->current) {
		/* the item pauses itself before stepping; auto advance must not
		 * kick in while the operator is stepping around */
		warp_pl_call_item_proc(s->current, "warp_step_frames", "frames", frames);
		s->state = obs_source_media_get_state(s->current);
		stepped = true;
	}

	warp_pl_unlock(s);

	if (stepped)
		warp_signal_frames_stepped(s->source, frames);
}

static void warp_playlist_step_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct warp_pl_hotkey_binding *b = data;
	struct warp_playlist_source *s = b->s;

	if (!pressed || !obs_source_showing(s->source))
		return;

	warp_pl_step_frames(s, b->value);
}

static void warp_playlist_register_hotkeys(struct warp_playlist_source *s, obs_source_t *source)
{
	static const int speed_presets[WARP_NUM_SPEED_PRESETS] = {WARP_SPEED_PRESET_LIST};
	static const int step_counts[WARP_NUM_STEP_COUNTS] = {WARP_STEP_COUNT_LIST};

	s->play_pause_hotkey = obs_hotkey_pair_register_source(
		source, "WarpPlaylist.Play", obs_module_text("Warp.Hotkey.Play"), "WarpPlaylist.Pause",
		obs_module_text("Warp.Hotkey.Pause"), warp_playlist_play_hotkey, warp_playlist_pause_hotkey, s, s);
	s->stop_hotkey = obs_hotkey_register_source(source, "WarpPlaylist.Stop", obs_module_text("Warp.Hotkey.Stop"),
						    warp_playlist_stop_hotkey, s);
	s->next_hotkey = obs_hotkey_register_source(source, "WarpPlaylist.Next",
						    obs_module_text("Warp.Hotkey.Playlist.Next"),
						    warp_playlist_next_hotkey, s);
	s->prev_hotkey = obs_hotkey_register_source(source, "WarpPlaylist.Previous",
						    obs_module_text("Warp.Hotkey.Playlist.Previous"),
						    warp_playlist_prev_hotkey, s);
	s->first_hotkey = obs_hotkey_register_source(source, "WarpPlaylist.First",
						     obs_module_text("Warp.Hotkey.Playlist.First"),
						     warp_playlist_first_hotkey, s);
	s->restart_hotkey = obs_hotkey_register_source(source, "WarpPlaylist.RestartCurrent",
						       obs_module_text("Warp.Hotkey.Playlist.RestartCurrent"),
						       warp_playlist_restart_hotkey, s);
	s->clear_hotkey = obs_hotkey_register_source(source, "WarpPlaylist.ClearQueue",
						     obs_module_text("Warp.Hotkey.Playlist.Clear"),
						     warp_playlist_clear_hotkey, s);

	obs_hotkey_register_source(source, "WarpPlaylist.SpeedUp", obs_module_text("Warp.Hotkey.Speed.Up"),
				   warp_playlist_speed_up_hotkey, s);
	obs_hotkey_register_source(source, "WarpPlaylist.SpeedDown", obs_module_text("Warp.Hotkey.Speed.Down"),
				   warp_playlist_speed_down_hotkey, s);
	obs_hotkey_register_source(source, "WarpPlaylist.SpeedReset", obs_module_text("Warp.Hotkey.Speed.Reset"),
				   warp_playlist_speed_reset_hotkey, s);

	for (size_t i = 0; i < WARP_NUM_SPEED_PRESETS; i++) {
		char name[64];
		char text_key[64];

		s->speed_bindings[i].s = s;
		s->speed_bindings[i].value = speed_presets[i];

		snprintf(name, sizeof(name), "WarpPlaylist.SpeedPreset%d", speed_presets[i]);
		snprintf(text_key, sizeof(text_key), "Warp.Hotkey.Speed.Preset%d", speed_presets[i]);
		obs_hotkey_register_source(source, name, obs_module_text(text_key), warp_playlist_speed_preset_hotkey,
					   &s->speed_bindings[i]);
	}

	for (size_t i = 0; i < WARP_NUM_STEP_COUNTS; i++) {
		char name[64];
		char text_key[64];

		struct warp_pl_hotkey_binding *fwd = &s->step_bindings[i * 2];
		struct warp_pl_hotkey_binding *back = &s->step_bindings[i * 2 + 1];

		fwd->s = s;
		fwd->value = step_counts[i];
		snprintf(name, sizeof(name), "WarpPlaylist.StepForward%d", step_counts[i]);
		snprintf(text_key, sizeof(text_key), "Warp.Hotkey.Step.Forward%d", step_counts[i]);
		obs_hotkey_register_source(source, name, obs_module_text(text_key), warp_playlist_step_hotkey, fwd);

		back->s = s;
		back->value = -step_counts[i];
		snprintf(name, sizeof(name), "WarpPlaylist.StepBackward%d", step_counts[i]);
		snprintf(text_key, sizeof(text_key), "Warp.Hotkey.Step.Backward%d", step_counts[i]);
		obs_hotkey_register_source(source, name, obs_module_text(text_key), warp_playlist_step_hotkey, back);
	}
}

/* ------------------------------------------------------------------------- */
/* procs
 *
 * The playlist actions that have no counterpart in the media control API, made
 * callable from outside the source: the obs-websocket vendor requests drive
 * the playlist through these, and scripts can call them too. They do what the
 * matching hotkeys do, and report it the same way, except that they do not
 * require the source to be on screen. */

static void warp_pl_set_speed_proc(void *data, calldata_t *cd)
{
	long long speed;

	if (calldata_get_int(cd, "speed", &speed))
		warp_pl_change_speed(data, (int)speed, true, WARP_SPEED_CHANGE_SET);
}

static void warp_pl_adjust_speed_proc(void *data, calldata_t *cd)
{
	long long delta;

	if (!calldata_get_int(cd, "delta", &delta) || !delta)
		return;

	warp_pl_change_speed(data, (int)delta, false,
			     delta > 0 ? WARP_SPEED_CHANGE_INCREASED : WARP_SPEED_CHANGE_DECREASED);
}

static void warp_pl_get_speed_proc(void *data, calldata_t *cd)
{
	struct warp_playlist_source *s = data;

	pthread_mutex_lock(&s->mutex);
	calldata_set_int(cd, "speed", s->speed);
	pthread_mutex_unlock(&s->mutex);
}

static void warp_pl_step_frames_proc(void *data, calldata_t *cd)
{
	long long frames;

	if (calldata_get_int(cd, "frames", &frames))
		warp_pl_step_frames(data, (int)frames);
}

static void warp_pl_first_proc(void *data, calldata_t *cd)
{
	struct warp_playlist_source *s = data;

	UNUSED_PARAMETER(cd);

	pthread_mutex_lock(&s->mutex);
	warp_pl_restart_playlist(s);
	warp_pl_unlock(s);
}

static void warp_pl_restart_current_proc(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);

	warp_pl_restart_current_command(data);
}

static void warp_pl_clear_proc(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);

	warp_pl_clear_playlist(data);
}

/* where in the playlist playback is; 'index' is -1 when nothing is playing */
static void warp_pl_status_proc(void *data, calldata_t *cd)
{
	struct warp_playlist_source *s = data;

	pthread_mutex_lock(&s->mutex);

	const char *path = warp_pl_path_at(s, s->pos);

	calldata_set_int(cd, "index", s->pos < s->order.num ? (long long)s->pos : -1);
	calldata_set_int(cd, "count", (long long)s->order.num);
	calldata_set_string(cd, "current_file", path ? path : "");

	pthread_mutex_unlock(&s->mutex);
}

static void warp_playlist_register_procs(struct warp_playlist_source *s, obs_source_t *source)
{
	proc_handler_t *ph = obs_source_get_proc_handler(source);

	proc_handler_add(ph, "void warp_set_speed(int speed)", warp_pl_set_speed_proc, s);
	proc_handler_add(ph, "void warp_adjust_speed(int delta)", warp_pl_adjust_speed_proc, s);
	proc_handler_add(ph, "void warp_get_speed(out int speed)", warp_pl_get_speed_proc, s);
	proc_handler_add(ph, "void warp_step_frames(int frames)", warp_pl_step_frames_proc, s);
	proc_handler_add(ph, "void warp_playlist_first()", warp_pl_first_proc, s);
	proc_handler_add(ph, "void warp_playlist_restart_current()", warp_pl_restart_current_proc, s);
	proc_handler_add(ph, "void warp_playlist_clear()", warp_pl_clear_proc, s);
	proc_handler_add(ph, "void warp_playlist_status(out int index, out int count, out string current_file)",
			 warp_pl_status_proc, s);
}

/* ------------------------------------------------------------------------- */

static void *warp_playlist_create(obs_data_t *settings, obs_source_t *source)
{
	static const char *signals[] = {WARP_SIGNAL_DECL_SPEED_CHANGED, WARP_SIGNAL_DECL_FRAMES_STEPPED,
					WARP_SIGNAL_DECL_MEDIA_ACTION, NULL};

	struct warp_playlist_source *s = bzalloc(sizeof(struct warp_playlist_source));

	s->source = source;
	s->state = OBS_MEDIA_STATE_NONE;
	s->base_speed = 100;
	/* s->speed is left at zero: the update below fills it in, and a speed
	 * that was never played at is not a change to report */
	s->transition_ms = WARP_PL_DEFAULT_TRANSITION_MS;
	s->transition_store = obs_data_create();
	s->transition_overlap = true;
	s->transition_alignment = OBS_ALIGN_CENTER;
	s->transition_scale = OBS_TRANSITION_SCALE_ASPECT;
	s->rand_state = os_gettime_ns() | 1;

	if (pthread_mutex_init(&s->mutex, NULL)) {
		blog(LOG_ERROR, "[Warp Playlist]: failed to initialize mutex");
		obs_data_release(s->transition_store);
		bfree(s);
		return NULL;
	}

	da_init(s->paths);
	da_init(s->order);
	da_init(s->retired);

	signal_handler_add_array(obs_source_get_signal_handler(source), signals);
	warp_playlist_register_hotkeys(s, source);
	warp_playlist_register_procs(s, source);

	warp_playlist_update(s, settings);
	return s;
}

static void warp_playlist_destroy(void *data)
{
	struct warp_playlist_source *s = data;
	DARRAY(obs_source_t *) retired;
	obs_source_t *transition;
	obs_source_t *target;
	obs_source_t *current;
	obs_source_t *prev;

	pthread_mutex_lock(&s->mutex);

	warp_pl_drop_preloaded(s);

	/* nothing queued matters any more, but everything it holds a
	 * reference to still has to be let go of */
	s->play_armed = false;
	s->preload_requested = false;
	s->transition_armed = false;
	s->signal_started = false;
	s->signal_ended = false;

	retired.da = s->retired.da;
	da_init(s->retired);

	target = s->transition_target;
	current = s->current;
	prev = s->prev;
	transition = s->transition;

	s->transition_target = NULL;
	s->current = NULL;
	s->prev = NULL;
	s->transition = NULL;

	for (size_t i = 0; i < s->paths.num; i++)
		bfree(s->paths.array[i]);
	da_free(s->paths);
	da_free(s->order);

	bfree(s->transition_id);
	bfree(s->pending_transition_id);

	obs_data_release(s->pending_transition_settings);
	s->pending_transition_settings = NULL;
	obs_data_release(s->transition_store);
	s->transition_store = NULL;
	bfree(s->transition_store_json);

	pthread_mutex_unlock(&s->mutex);

	for (size_t i = 0; i < retired.num; i++)
		obs_source_release(retired.array[i]);
	da_free(retired);

	if (target)
		obs_source_release(target);
	if (current)
		obs_source_release(current);
	if (prev)
		obs_source_release(prev);

	if (transition) {
		obs_transition_clear(transition);
		obs_source_remove_active_child(s->source, transition);
		obs_source_release(transition);
	}

	pthread_mutex_destroy(&s->mutex);
	bfree(s);
}

static void warp_playlist_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	struct warp_playlist_source *s = data;
	obs_source_t *transition = warp_pl_get_transition(s);

	if (transition) {
		obs_source_video_render(transition);
		obs_source_release(transition);
	}
}

static bool warp_playlist_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio_output,
				       uint32_t mixers, size_t channels, size_t sample_rate)
{
	UNUSED_PARAMETER(sample_rate);

	struct warp_playlist_source *s = data;
	/* the tick swaps the transition out, so the audio thread needs a
	 * reference of its own rather than the bare pointer */
	obs_source_t *transition = warp_pl_get_transition(s);
	uint64_t timestamp;

	if (!transition)
		return false;

	if (obs_source_audio_pending(transition)) {
		obs_source_release(transition);
		return false;
	}

	timestamp = obs_source_get_audio_timestamp(transition);

	if (!timestamp) {
		obs_source_release(transition);
		return false;
	}

	struct obs_source_audio_mix child_audio;

	obs_source_get_audio_mix(transition, &child_audio);

	for (size_t mix = 0; mix < MAX_AUDIO_MIXES; mix++) {
		if ((mixers & (1 << mix)) == 0)
			continue;

		for (size_t ch = 0; ch < channels; ch++) {
			float *out = audio_output->output[mix].data[ch];
			float *in = child_audio.output[mix].data[ch];

			memcpy(out, in, AUDIO_OUTPUT_FRAMES * sizeof(float));
		}
	}

	obs_source_release(transition);

	*ts_out = timestamp;
	return true;
}

static void warp_playlist_enum_active_sources(void *data, obs_source_enum_proc_t cb, void *param)
{
	struct warp_playlist_source *s = data;
	obs_source_t *transition = warp_pl_get_transition(s);

	if (transition) {
		cb(s->source, transition, param);
		obs_source_release(transition);
	}
}

/* how much wall-clock time is left of the item that is playing, in
 * milliseconds, or -1 when that cannot be told (a stream, or an item that has
 * not reported its duration yet) */
static int64_t warp_pl_remaining_ms(struct warp_playlist_source *s)
{
	int64_t duration = obs_source_media_get_duration(s->current);
	int64_t time = obs_source_media_get_time(s->current);

	if (duration <= 0 || time < 0)
		return -1;

	int64_t remaining = duration - time;

	if (remaining < 0)
		remaining = 0;

	/* media time runs slower or faster than the clock when the item is not
	 * played back at 100% */
	int speed = s->speed >= MP_SPEED_MIN ? s->speed : 100;

	return remaining * 100 / speed;
}

/* How far ahead of the end of the current item the switch has to start for the
 * transition to land on its last frame: the whole of the transition for most of
 * them, the point at which the incoming item is swapped in for a stinger, and
 * next to nothing for a cut, or when the transition is set to run after the
 * item rather than overlap it. Expects s->mutex to be held. */
static uint32_t warp_pl_transition_lead_ms(struct warp_playlist_source *s)
{
	uint32_t lead = 0;

	if (s->transition_overlap && !s->transition_is_cut)
		lead = s->transition_is_stinger ? s->stinger_point_ms : s->transition_ms;

	return lead > WARP_PL_END_GUARD_MS ? lead : WARP_PL_END_GUARD_MS;
}

/* Asks for the item after the current one to be opened. The file is opened by
 * warp_pl_open_preload() once the lock has been dropped. Expects s->mutex to
 * be held. */
static void warp_pl_preload_next(struct warp_playlist_source *s)
{
	size_t target;

	if (s->preloaded || s->preload_requested || !warp_pl_step_pos(s, 1, &target))
		return;

	if (!warp_pl_path_at(s, target))
		return;

	s->preload_requested = true;
	s->preload_order_pos = target;
}

/* Once the preloaded item has decoded picture, park it on its first frame so
 * the transition into it starts from the top of the file. */
static void warp_pl_arm_preloaded(struct warp_playlist_source *s)
{
	if (!s->preloaded || s->preload_armed)
		return;

	if (obs_source_get_width(s->preloaded) == 0)
		return;

	obs_source_media_play_pause(s->preloaded, true);
	obs_source_media_set_time(s->preloaded, 0);
	s->preload_armed = true;
}

static void warp_playlist_tick(void *data, float seconds)
{
	struct warp_playlist_source *s = data;

	/* creates and releases sources, so it runs with the lock dropped */
	warp_pl_swap_transition(s);

	obs_source_t *transition = warp_pl_get_transition(s);
	float transition_time = transition ? obs_transition_get_time(transition) : 1.0f;
	bool resized = false;

	pthread_mutex_lock(&s->mutex);

	/* retire the outgoing item once its transition has played out */
	if (s->prev && transition_time >= 1.0f) {
		obs_source_media_stop(s->prev);
		warp_pl_retire(s, s->prev);
		s->prev = NULL;
	}

	/* size the transition to the item on screen */
	uint32_t cx = s->current ? obs_source_get_width(s->current) : 0;
	uint32_t cy = s->current ? obs_source_get_height(s->current) : 0;

	if (s->prev) {
		uint32_t prev_cx = obs_source_get_width(s->prev);
		uint32_t prev_cy = obs_source_get_height(s->prev);

		if (prev_cx > cx)
			cx = prev_cx;
		if (prev_cy > cy)
			cy = prev_cy;
	}

	if (cx && cy && (cx != s->cx || cy != s->cy)) {
		s->cx = cx;
		s->cy = cy;
		resized = true;
	}

	warp_pl_arm_preloaded(s);

	if (s->current && s->state == OBS_MEDIA_STATE_PLAYING) {
		enum obs_media_state item_state = obs_source_media_get_state(s->current);
		bool item_done = item_state == OBS_MEDIA_STATE_ENDED || item_state == OBS_MEDIA_STATE_STOPPED;

		s->cur_age += seconds;

		if (!s->auto_advance) {
			if (item_done)
				warp_pl_end(s);
		} else {
			int64_t remaining = item_done ? 0 : warp_pl_remaining_ms(s);

			if (remaining >= 0) {
				/* a file shorter than the transition would be
				 * switched away from the frame it started on,
				 * running through the playlist in seconds */
				bool may_switch = item_done || s->cur_age >= WARP_PL_MIN_ITEM_SECONDS;

				/* an overlapping transition starts early enough
				 * to finish as the current item runs out */
				int64_t lead = (int64_t)warp_pl_transition_lead_ms(s);

				if (remaining <= lead && may_switch)
					warp_pl_advance(s, 1);
				else if (remaining <= lead + WARP_PL_PRELOAD_LEAD_MS)
					warp_pl_preload_next(s);
			} else if (s->cur_age > WARP_PL_STALL_SECONDS && item_state != OBS_MEDIA_STATE_PLAYING) {
				/* a file that never started (missing, or one
				 * FFmpeg cannot open) must not stall a playlist
				 * that is meant to keep running */
				const char *path = warp_pl_path_at(s, s->pos);

				WARP_PL_LOG(LOG_WARNING, "'%s' did not start playing, skipping it", path ? path : "");
				warp_pl_advance(s, 1);
			}
		}
	}

	warp_pl_unlock(s);

	if (transition) {
		if (resized)
			obs_transition_set_size(transition, cx, cy);

		obs_source_release(transition);
	}
}

static uint32_t warp_playlist_getwidth(void *data)
{
	struct warp_playlist_source *s = data;

	return s->cx;
}

static uint32_t warp_playlist_getheight(void *data)
{
	struct warp_playlist_source *s = data;

	return s->cy;
}

static void warp_playlist_activate(void *data)
{
	struct warp_playlist_source *s = data;

	pthread_mutex_lock(&s->mutex);
	if (s->restart_on_activate)
		warp_pl_restart_playlist(s);
	warp_pl_unlock(s);
}

static void warp_playlist_deactivate(void *data)
{
	struct warp_playlist_source *s = data;

	pthread_mutex_lock(&s->mutex);
	if (s->restart_on_activate)
		warp_pl_stop(s);
	warp_pl_unlock(s);
}

static void warp_playlist_play_pause(void *data, bool pause)
{
	struct warp_playlist_source *s = data;

	pthread_mutex_lock(&s->mutex);

	bool finished = s->state == OBS_MEDIA_STATE_ENDED || s->state == OBS_MEDIA_STATE_STOPPED;
	bool acted = false;

	if (!pause && (!s->current || finished)) {
		/* playing again after the playlist ran out replays the file it
		 * ran out on; after a stop it starts over from the top */
		if (s->current && s->state == OBS_MEDIA_STATE_ENDED)
			warp_pl_restart_current(s);
		else
			warp_pl_restart_playlist(s);

		/* an empty playlist has nothing to play, so nothing happened */
		acted = s->current || s->play_armed;
	} else if (s->current) {
		obs_source_media_play_pause(s->current, pause);
		s->state = pause ? OBS_MEDIA_STATE_PAUSED : OBS_MEDIA_STATE_PLAYING;

		if (!pause)
			s->signal_started = true;

		acted = true;
	}

	warp_pl_unlock(s);

	if (acted)
		warp_signal_media_action(s->source, pause ? WARP_MEDIA_ACTION_PAUSE : WARP_MEDIA_ACTION_PLAY);
}

/* the media controls' restart button starts the whole playlist over, the way
 * the VLC playlist source does; restarting only the current file has its own
 * hotkey */
static void warp_playlist_restart(void *data)
{
	struct warp_playlist_source *s = data;
	bool restarted;

	pthread_mutex_lock(&s->mutex);
	warp_pl_restart_playlist(s);
	restarted = s->play_armed;
	warp_pl_unlock(s);

	if (restarted)
		warp_signal_media_action(s->source, WARP_MEDIA_ACTION_RESTART);
}

static void warp_playlist_stop_media(void *data)
{
	struct warp_playlist_source *s = data;

	pthread_mutex_lock(&s->mutex);
	warp_pl_stop(s);
	warp_pl_unlock(s);
}

static void warp_playlist_next(void *data)
{
	struct warp_playlist_source *s = data;

	pthread_mutex_lock(&s->mutex);
	warp_pl_advance(s, 1);
	warp_pl_unlock(s);
}

static void warp_playlist_previous(void *data)
{
	struct warp_playlist_source *s = data;

	pthread_mutex_lock(&s->mutex);
	warp_pl_advance(s, -1);
	warp_pl_unlock(s);
}

static int64_t warp_playlist_get_duration(void *data)
{
	struct warp_playlist_source *s = data;
	int64_t duration = 0;

	pthread_mutex_lock(&s->mutex);
	if (s->current)
		duration = obs_source_media_get_duration(s->current);
	pthread_mutex_unlock(&s->mutex);

	return duration;
}

static int64_t warp_playlist_get_time(void *data)
{
	struct warp_playlist_source *s = data;
	int64_t time = 0;

	pthread_mutex_lock(&s->mutex);
	if (s->current)
		time = obs_source_media_get_time(s->current);
	pthread_mutex_unlock(&s->mutex);

	return time;
}

static void warp_playlist_set_time(void *data, int64_t ms)
{
	struct warp_playlist_source *s = data;

	pthread_mutex_lock(&s->mutex);
	if (s->current)
		obs_source_media_set_time(s->current, ms);
	pthread_mutex_unlock(&s->mutex);
}

static enum obs_media_state warp_playlist_get_state(void *data)
{
	return warp_pl_state(data);
}

static void missing_file_callback(void *src, const char *new_path, void *data)
{
	struct warp_playlist_source *s = src;
	size_t index = (size_t)(uintptr_t)data;

	obs_data_t *settings = obs_source_get_settings(s->source);
	obs_data_array_t *array = obs_data_get_array(settings, "playlist");

	if (array && index < obs_data_array_count(array)) {
		obs_data_t *item = obs_data_array_item(array, index);

		obs_data_set_string(item, "value", new_path);
		obs_data_release(item);
		obs_source_update(s->source, settings);
	}

	obs_data_array_release(array);
	obs_data_release(settings);
}

/* 'data' is the setting of the stinger transition the file was found under */
static void missing_stinger_callback(void *src, const char *new_path, void *data)
{
	struct warp_playlist_source *s = src;
	const char *key = data;
	obs_data_t *fix = obs_data_create();
	obs_data_t *stored;

	obs_data_set_string(fix, key, new_path);

	pthread_mutex_lock(&s->mutex);

	/* into the settings the stinger is saved with, and to the stinger that
	 * is running, which the tick hands it to */
	stored = obs_data_get_obj(s->transition_store, WARP_PL_TR_STINGER);

	if (stored) {
		obs_data_set_string(stored, key, new_path);
		obs_data_release(stored);
	}

	if (s->pending_transition_settings) {
		obs_data_apply(s->pending_transition_settings, fix);
		obs_data_release(fix);
	} else {
		s->pending_transition_settings = fix;
	}

	pthread_mutex_unlock(&s->mutex);
}

/* reports the file 'key' of the stinger settings when it has gone missing */
static void warp_pl_add_missing_stinger(struct warp_playlist_source *s, obs_missing_files_t *files,
					obs_data_t *tr_settings, const char *key)
{
	const char *path = obs_data_get_string(tr_settings, key);
	obs_missing_file_t *file;

	if (!path || !*path || os_file_exists(path))
		return;

	file = obs_missing_file_create(path, missing_stinger_callback, OBS_MISSING_FILE_SOURCE, s->source, (void *)key);

	obs_missing_files_add_file(files, file);
}

static obs_missing_files_t *warp_playlist_missingfiles(void *data)
{
	struct warp_playlist_source *s = data;
	obs_missing_files_t *files = obs_missing_files_create();
	obs_data_t *tr_settings = NULL;
	obs_data_t *settings;

	pthread_mutex_lock(&s->mutex);

	for (size_t i = 0; i < s->paths.num; i++) {
		const char *path = s->paths.array[i];

		if (path && *path && !os_file_exists(path)) {
			obs_missing_file_t *file = obs_missing_file_create(
				path, missing_file_callback, OBS_MISSING_FILE_SOURCE, s->source, (void *)(uintptr_t)i);

			obs_missing_files_add_file(files, file);
		}
	}

	pthread_mutex_unlock(&s->mutex);

	/* The stinger's videos are files the playlist plays as much as the ones
	 * in the list, so missing ones are worth reporting too. The transition
	 * belongs to this source alone, so nothing else reports them. */
	warp_pl_capture_transition_settings(s);

	settings = obs_source_get_settings(s->source);

	if (strcmp(obs_data_get_string(settings, "transition"), WARP_PL_TR_STINGER) == 0) {
		pthread_mutex_lock(&s->mutex);
		tr_settings = obs_data_get_obj(s->transition_store, WARP_PL_TR_STINGER);
		pthread_mutex_unlock(&s->mutex);
	}

	obs_data_release(settings);

	if (tr_settings) {
		warp_pl_add_missing_stinger(s, files, tr_settings, "path");

		if (obs_data_get_bool(tr_settings, "track_matte_enabled"))
			warp_pl_add_missing_stinger(s, files, tr_settings, "track_matte_path");

		obs_data_release(tr_settings);
	}

	return files;
}

struct obs_source_info warp_playlist_source_info = {
	.id = "warp_playlist_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	/* OBS_SOURCE_AUDIO must not be set alongside OBS_SOURCE_COMPOSITE:
	 * obs_register_source() rejects that combination outright, which keeps
	 * the source out of the Add Source menu entirely. A composite source
	 * delivers its audio through audio_render instead, the way scenes and
	 * the slide show source do. */
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_COMPOSITE | OBS_SOURCE_DO_NOT_DUPLICATE |
			OBS_SOURCE_CONTROLLABLE_MEDIA,
	.get_name = warp_playlist_getname,
	.create = warp_playlist_create,
	.destroy = warp_playlist_destroy,
	.get_defaults = warp_playlist_defaults,
	.get_properties = warp_playlist_getproperties,
	.update = warp_playlist_update,
	.save = warp_playlist_save,
	.activate = warp_playlist_activate,
	.deactivate = warp_playlist_deactivate,
	.video_render = warp_playlist_video_render,
	.video_tick = warp_playlist_tick,
	.audio_render = warp_playlist_audio_render,
	.enum_active_sources = warp_playlist_enum_active_sources,
	.get_width = warp_playlist_getwidth,
	.get_height = warp_playlist_getheight,
	.missing_files = warp_playlist_missingfiles,
	.icon_type = OBS_ICON_TYPE_MEDIA,
	.media_play_pause = warp_playlist_play_pause,
	.media_restart = warp_playlist_restart,
	.media_stop = warp_playlist_stop_media,
	.media_next = warp_playlist_next,
	.media_previous = warp_playlist_previous,
	.media_get_duration = warp_playlist_get_duration,
	.media_get_time = warp_playlist_get_time,
	.media_set_time = warp_playlist_set_time,
	.media_get_state = warp_playlist_get_state,
};
