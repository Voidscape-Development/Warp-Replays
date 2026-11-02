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
#include <util/util_uint64.h>

#include <media-playback/media-playback.h>

#ifdef WARP_HAVE_FRONTEND_API
/* the transition's own properties are shown in the window OBS opens for the
 * transitions in its own list */
#include <obs-frontend-api.h>
#endif

#include "warp-events.h"
#include "warp-status.h"
#include "warp-zoom.h"

#define WARP_PL_LOG(level, format, ...) \
	blog(level, "[Warp Playlist '%s']: " format, obs_source_get_name(s->source), ##__VA_ARGS__)

/* How far ahead of the switch point the next file is opened, in wall-clock
 * milliseconds. The item is opened, allowed to decode its first frame, then
 * paused at the start, so the incoming half of the transition has picture
 * from its very first frame. */
#define WARP_PL_PRELOAD_LEAD_MS 1000

/* How long an item that is being switched to is given to open and produce a
 * picture before it is put on screen regardless, in seconds. Nothing is
 * expected to take anywhere near this long; it is only there so that a file
 * that never opens cannot hold a playlist on the item before it for good. */
#define WARP_PL_OPEN_WAIT_SECONDS 2.0f

/* how long an item is given to start playing before it is written off and the
 * playlist moves on, in seconds */
#define WARP_PL_STALL_SECONDS 3.0f

/* what the volume properties are allowed to ask for, in percent. Unity is the
 * ceiling: the files are played as they are at 100%, and the properties are
 * there to turn them down rather than to boost them into clipping. */
#define WARP_PL_VOLUME_MIN 0
#define WARP_PL_VOLUME_MAX 100

/* How many sources the mixer takes audio from at once: the item being
 * transitioned away from, the one coming in, and whatever the transition plays
 * of its own - a stinger has a video of its own and, with a track matte on, a
 * second one alongside it. */
#define WARP_PL_NUM_DECKS 4

/* How far a source's audio may drift from where it was anchored before the
 * mixer anchors it again, in nanoseconds. Audio arrives as it is decoded, so a
 * drift this large is a clock that has been put back rather than one that is
 * merely running late. */
#define WARP_PL_DECK_RESYNC_NS UINT64_C(500000000)

/* How long the mixer will hold a sample waiting on an item that still has
 * audio to come, in nanoseconds. It only ever waits during a transition, when
 * both halves have to be in before either can be mixed; with one item playing
 * there is nothing to wait for and its audio goes out as it arrives. The bound
 * is what keeps an item that stops dead halfway through a crossfade from
 * taking the mix down with it. */
#define WARP_PL_MIX_HOLD_NS UINT64_C(60000000)

/* How long an item that has stopped handing audio over is waited on before the
 * mixer writes it off, in nanoseconds. A file with no audio track never hands
 * anything over at all, and must not hold up the other half of a transition. */
#define WARP_PL_DECK_IDLE_NS UINT64_C(200000000)

/* the most audio the mixer will hold at once, in nanoseconds */
#define WARP_PL_MIX_MAX_NS UINT64_C(1000000000)

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

/* audio_fade_style values of the stinger transition: the outgoing file fades
 * out and the incoming one fades back in behind the sting, or the two cross
 * over the way every other transition does */
#define WARP_PL_STINGER_FADE_OUT_IN 0
#define WARP_PL_STINGER_FADE_CROSS 1

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

/* Which way through the playlist a switch is going. A playlist may be set up
 * with a transition of its own for going back, in which case both are live at
 * once; with that off, everything runs through the forward one. */
#define WARP_PL_DIR_FORWARD 0
#define WARP_PL_DIR_BACKWARD 1
#define WARP_PL_NUM_DIRS 2

struct warp_playlist_source;

/* per-hotkey context for parametrized hotkeys: 'value' is a signed frame
 * count for step hotkeys and a speed percentage for preset hotkeys */
struct warp_pl_hotkey_binding {
	struct warp_playlist_source *s;
	int value;
};

/* The settings one direction's transition is configured through. The forward
 * ones are the keys the playlist has always used, so a playlist saved before
 * going back could have a transition of its own loads unchanged. 'timing' is
 * NULL for going back: only automatic advance is placed against the end of a
 * file, and that only ever moves forward. */
struct warp_pl_transition_keys {
	const char *id;
	const char *duration;
	const char *timing;
	const char *scale;
	const char *alignment;
	const char *store;
	const char *properties;
};

static const struct warp_pl_transition_keys warp_pl_keys[WARP_PL_NUM_DIRS] = {
	{"transition", "transition_duration", "transition_timing", "transition_scale", "transition_alignment",
	 "transition_settings", "transition_properties"},
	{"back_transition", "back_transition_duration", NULL, "back_transition_scale", "back_transition_alignment",
	 "back_transition_settings", "back_transition_properties"},
};

/* A transition the playlist renders through, and everything it is set up with.
 * There is one of these per direction. */
struct warp_pl_transition {
	/* the live transition, and the libobs id it was created with */
	obs_source_t *source;
	char *id;
	/* transition the settings ask for, picked up by the next tick */
	char *pending_id;
	/* settings for the transition itself, handed to it by the next tick */
	obs_data_t *pending_settings;
	/* alignment and scaling wait for the next tick along with them */
	bool pending_layout;
	/* the transition is not wanted any more; the next tick drops it */
	bool pending_release;

	/* What every transition that has been used is configured with, keyed by
	 * libobs id. A transition is configured through its own properties,
	 * which write to the transition rather than to this source, so this is
	 * read back from the one that is running rather than the other way
	 * round. Keeping the ones that are not running means picking a
	 * transition back up brings its settings with it. */
	obs_data_t *store;
	/* the store as it last stood in the source settings, to tell settings
	 * being loaded, or set from the outside, from the copy this source
	 * wrote there itself */
	char *store_json;
	bool store_loaded;

	uint32_t alignment;
	enum obs_transition_scale_type scale;

	uint32_t ms;
	/* where a stinger swaps the incoming file in, in milliseconds */
	uint32_t stinger_point_ms;
	/* whether a stinger crosses its two files over rather than fading one
	 * out and the other back in behind the sting */
	bool stinger_cross_fade;
	bool is_cut;
	bool is_stinger;
	/* Whether the transition overlaps the end of the file or runs once it
	 * is over. Only read for the forward transition: it places automatic
	 * advance, and a move back through the playlist is always asked for. */
	bool overlap;
};

/* One item the mixer is taking audio from. There is one deck per half of a
 * transition, and a single one while nothing is running. */
struct warp_pl_deck {
	/* the item being tapped; the deck holds a reference while it is */
	obs_source_t *source;
	/* Whether this is something the transition brought along rather than a
	 * file out of the playlist, which is what decides the level it is
	 * played at: the two are set apart so a playlist of silent replays can
	 * still be stingered with sound. */
	bool from_transition;
	/* what its samples are scaled by, written by the tick */
	float gain;
	/* the gain the last packet ended on, so a change rides across the next
	 * one rather than stepping and clicking */
	float applied_gain;
	/* What has to be added to this source's timestamps to put them on the
	 * system clock. Every source counts from a base of its own - media
	 * playback in one plugin knows nothing of the copy in another - so the
	 * mixer works in the clock and anchors each source to it as its first
	 * packet arrives, which is what libobs does with a source of its own. */
	int64_t offset;
	bool anchored;
	/* just past the last sample handed over, on the system clock */
	uint64_t end_ts;
	/* when that arrived */
	uint64_t arrived;
	/* whether this source is handing audio over at all */
	bool producing;
};

/* Where the audio of the items being played is added up before it is handed to
 * OBS. See the audio section further down for what it is for. */
struct warp_pl_audio {
	/* Guards everything below. It is a leaf: nothing else is taken under
	 * it, and it is never held across a call into libobs. */
	pthread_mutex_t mutex;

	struct warp_pl_deck deck[WARP_PL_NUM_DECKS];

	/* Samples that have been handed over but not let out yet, planar float
	 * in the mix format; 'ts' is where the first of them sits. */
	float *buf[MAX_AUDIO_CHANNELS];
	size_t capacity;
	size_t frames;
	uint64_t ts;
	bool have;

	size_t channels;
	size_t sample_rate;
	enum speaker_layout speakers;

	/* just past the last sample let out, so audio that turns up behind it
	 * is dropped rather than mixed in under what has already gone */
	uint64_t next_ts;
	bool flushed;

	/* what the volume properties scale the files, and whatever the
	 * transition plays of its own, by */
	float volume;
	float transition_volume;
};

/* ------------------------------------------------------------------------- */
/* commands and the snapshot */

enum warp_pl_cmd_type {
	WARP_PL_CMD_NEXT,
	WARP_PL_CMD_PREVIOUS,
	WARP_PL_CMD_FIRST,
	WARP_PL_CMD_RESTART_CURRENT,
	WARP_PL_CMD_CLEAR,
	WARP_PL_CMD_SET_SPEED,
	WARP_PL_CMD_ADJUST_SPEED,
	WARP_PL_CMD_STEP,
	WARP_PL_CMD_QUEUE,
	WARP_PL_CMD_CLEAR_QUEUE,
};

/* Something asked of the playlist from outside the graphics thread, carried
 * out by the next tick. 'change' is one of the WARP_SPEED_CHANGE_* kinds. */
struct warp_pl_cmd {
	enum warp_pl_cmd_type type;
	int value;
	const char *change;
	char *path;
};

/* Where the playlist stands, published by the graphics thread for the other
 * threads to read: the media controls' queries, the procs, the hotkeys that
 * look before they act, and the properties. */
struct warp_pl_snapshot {
	enum obs_media_state state;
	int speed;
	long long index;
	size_t count;
	size_t queued;
	char *current_file;
	/* the item playing, held so its position can be read off it */
	obs_source_t *current;
	DARRAY(char *) paths;
};

/* what one file in the list is played as */
struct warp_pl_clip {
	char *path;
	int64_t in_ms;
	int64_t out_ms;
	int64_t tail_ms;
	/* percent, or 0 for the playlist's own */
	int speed;
};

struct warp_playlist_source {
	obs_source_t *source;

	/* The transitions render the playlist; every item is handed to one of
	 * them. There is one per direction, and only the one a switch calls for
	 * is on screen; the other holds nothing until it is switched to. */
	struct warp_pl_transition tr[WARP_PL_NUM_DIRS];
	/* whether going back through the playlist has a transition of its own,
	 * rather than running through the forward one like everything else */
	bool separate_back_transition;
	/* the transition that is on screen */
	size_t active_dir;
	/* item currently played */
	obs_source_t *current;
	/* Its zoom filter, kept here so the tick does not have to go looking
	 * for it every frame. Every item is given one as it is opened, which is
	 * what makes the framing belong to the video rather than to the
	 * playlist: the file being transitioned away from keeps the zoom it was
	 * being watched at while the one coming in is already at the top. */
	obs_source_t *current_zoom;
	/* item transitioned away from, kept alive until the transition ends */
	obs_source_t *prev;
	/* Next item, opened early and parked on its first frame, so the
	 * transition into it has picture from the start. It is loaded to be
	 * held there, which is one command to its engine rather than a pause
	 * and a seek to be asked for and waited on once it has opened. */
	obs_source_t *preloaded;
	char *preloaded_path;
	/* The item before the one that is playing, parked back on its first
	 * frame once its transition out has played, so going back to it is as
	 * quick as going on. */
	obs_source_t *back;
	char *back_path;
	/* Items that have been played and let go of, kept with their file
	 * closed to load the next file into. A file is loaded into an item
	 * rather than a source being made for every one, so the item's media
	 * thread, hardware decoder and zoom filter carry over, and nothing has
	 * to be created or released in the middle of a switch. */
	DARRAY(obs_source_t *) idle;
	/* the files of the items on screen, and of the one waiting to go up */
	char *current_path;
	char *prev_path;
	char *pending_path;
	/* Files in a row that would not open. A playlist none of whose files
	 * open gives up rather than going round them for good. */
	size_t fail_streak;

	/* what each file is cut to and played at, by path; see
	 * WARP_PL_CLIPS_KEY */
	DARRAY(struct warp_pl_clip) clips;

	/* Files asked to be played next, in the order they were asked for,
	 * ahead of whatever the playlist would have gone on to. Once they have
	 * played, the playlist picks up where it would have been: resume_pos
	 * is that, or SIZE_MAX when nothing was put in front of it. */
	DARRAY(char *) queue;
	size_t resume_pos;
	/* Item that has been opened and is waiting to go on screen. Opening a
	 * file is asynchronous: creating the source is back long before its
	 * media thread has decoded anything, and a transition started against a
	 * source that has no picture shows through to nothing at all. So the
	 * item is held here until it has something to show. */
	obs_source_t *pending;
	size_t pending_order_pos;
	bool pending_transition;
	size_t pending_dir;
	float pending_age;

	/* ----------------------------------------------------------------- */
	/* Threads. Everything in this struct belongs to the graphics thread,
	 * which is where libobs ticks the source, applies its settings, carries
	 * out its media controls and activates it - so none of it is locked.
	 * Anything asked of the playlist from another thread is posted to
	 * 'cmds' and carried out by the next tick; anything read from another
	 * thread is read off 'snap'. The three locks are leaves: none is held
	 * while another is taken, or across a call into libobs. */

	/* guards 'cmds' */
	pthread_mutex_t cmd_mutex;
	DARRAY(struct warp_pl_cmd) cmds;

	/* guards 'snap' */
	pthread_mutex_t snap_mutex;
	struct warp_pl_snapshot snap;
	/* the file list has changed since 'snap' was last given it */
	bool snap_paths_dirty;

	/* Guards the transitions themselves, which the audio thread enumerates
	 * and saving reads back from the UI thread: each direction's source,
	 * id, store, store_json and stinger timing, and active_dir. Only the
	 * graphics thread writes them, so it reads them without it. */
	pthread_mutex_t tr_mutex;

	/* ----------------------------------------------------------------- */
	/* work handed to warp_pl_flush() to carry out once the change that
	 * asked for it has been made */

	/* sources to release */
	DARRAY(obs_source_t *) retired;
	/* an item to open and play, whether to animate the switch, and which
	 * direction's transition to animate it with */
	bool play_armed;
	size_t play_order_pos;
	bool play_transition;
	size_t play_dir;
	/* an item to open ahead of the switch point */
	bool preload_requested;
	size_t preload_order_pos;
	/* Bumped every time what the playlist is meant to be playing changes.
	 * Opening a file takes long enough for a stop, or another switch, to
	 * land while it is going on; the item is thrown away rather than put on
	 * screen when the count it was opened for is no longer the current
	 * one. */
	uint64_t play_gen;
	/* what to hand the transition, and which of them; the target holds a
	 * reference, and is NULL to show nothing */
	bool transition_armed;
	bool transition_use;
	size_t transition_dir;
	obs_source_t *transition_target;
	/* media signals: whatever reacts to one is free to drive this playlist
	 * straight back, so they are never emitted under the lock */
	bool signal_started;
	bool signal_ended;
	/* set while the outermost warp_pl_flush() works through the above */
	bool deferring;

	/* playlist as configured, and the order it is played back in */
	DARRAY(char *) paths;
	DARRAY(size_t) order;
	/* index into 'order' of the current item, or DARRAY_INVALID */
	size_t pos;

	uint32_t cx;
	uint32_t cy;

	/* speed every item starts at, and the live speed of the current item */
	int base_speed;
	int speed;

	/* How the file that is playing is framed, along with the presets it can
	 * be sent to. It guards itself, and is put back to the whole picture as
	 * each file goes up, the same way the speed is. */
	struct warp_zoom_control zoom;

	/* where the items' audio is mixed on its way out to OBS; guards itself */
	struct warp_pl_audio audio;

	bool auto_advance;
	bool loop_playlist;
	bool shuffle;
	bool hw_decode;
	bool gpu_direct;
	/* sound played at another speed keeps its pitch */
	bool preserve_pitch;
	bool restart_on_activate;
	bool clear_on_media_end;

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
/* threads
 *
 * The playlist's state belongs to the graphics thread. libobs ticks the source
 * there, applies its settings there (a video source's update is deferred to
 * its tick), carries out its media controls there and activates and shows it
 * there, so all of that works on the state directly, with nothing locked.
 *
 * The rest comes in from other threads: hotkeys on the hotkey thread, procs
 * and websocket requests on whichever thread calls them, the properties and
 * saving on the UI thread, the audio tree on the audio thread. A hotkey or a
 * proc that changes something posts a command, which the next tick carries
 * out (warp_pl_post, warp_pl_run_commands); one that only asks reads the
 * snapshot the graphics thread publishes (warp_pl_publish). Signals are
 * emitted as commands are carried out, with nothing held, so whatever reacts to
 * one is free to drive the playlist straight back - it posts a command of its
 * own.
 *
 * This used to be one lock over all of it, and a set of rules about what could
 * not be done while holding it: libobs calls into a source with its own global
 * locks held, from threads that then wanted the playlist's, so creating or
 * releasing a source under it could wedge the graphics, UI and hotkey threads
 * together. With no lock held over the state there is no such rule left to
 * break. The locks that remain are leaves, each over a few fields that another
 * thread reads, never held across a call into libobs. */

static void warp_pl_start(struct warp_playlist_source *s, size_t order_pos, bool use_transition, size_t dir,
			  uint64_t gen);
static void warp_pl_open_preload(struct warp_playlist_source *s, size_t order_pos, uint64_t gen);
static void warp_pl_end(struct warp_playlist_source *s);
static void warp_pl_advance(struct warp_playlist_source *s, int dir);

/* queues 'source' for release;  Graphics thread. */
static void warp_pl_retire(struct warp_playlist_source *s, obs_source_t *source)
{
	if (source)
		da_push_back(s->retired, &source);
}

/* Which transition a move in 'dir' runs through: the one of its own when going
 * back has been given one, and the forward one for everything else. Graphics
 * thread. */
static inline size_t warp_pl_dir_slot(struct warp_playlist_source *s, size_t dir)
{
	return dir == WARP_PL_DIR_BACKWARD && s->separate_back_transition ? WARP_PL_DIR_BACKWARD : WARP_PL_DIR_FORWARD;
}

/* Hands 'item' to the transition for 'dir' once the lock is dropped; 'item' may
 * be NULL, to show nothing.  Graphics thread. */
static void warp_pl_arm_transition(struct warp_playlist_source *s, obs_source_t *item, bool use_transition, size_t dir)
{
	warp_pl_retire(s, s->transition_target);

	s->transition_target = item ? obs_source_get_ref(item) : NULL;
	s->transition_armed = true;
	s->transition_use = use_transition;
	s->transition_dir = warp_pl_dir_slot(s, dir);
}

/* how long the transition for 'dir' runs for, in milliseconds; a cut is instant
 * whatever the duration property is set to.  Graphics thread. */
static inline uint32_t warp_pl_transition_ms(struct warp_playlist_source *s, size_t dir)
{
	return s->tr[dir].is_cut ? 0 : s->tr[dir].ms;
}

/* the state as last published; any thread */
static enum obs_media_state warp_pl_state(struct warp_playlist_source *s)
{
	enum obs_media_state state;

	pthread_mutex_lock(&s->snap_mutex);
	state = s->snap.state;
	pthread_mutex_unlock(&s->snap_mutex);

	return state;
}

/* the transition for 'dir', with a reference held: they are swapped out from
 * the tick, so the threads that enumerate and save them cannot use them bare;
 * any thread */
static obs_source_t *warp_pl_get_transition_dir(struct warp_playlist_source *s, size_t dir)
{
	obs_source_t *transition;

	pthread_mutex_lock(&s->tr_mutex);
	transition = obs_source_get_ref(s->tr[dir].source);
	pthread_mutex_unlock(&s->tr_mutex);

	return transition;
}

/* What the transition on screen does to the audio of the two items it holds.
 * Read alongside the transition itself so the shape the gains are worked out
 * with belongs to the transition they are worked out for. */
struct warp_pl_transition_audio {
	bool is_stinger;
	bool stinger_cross_fade;
	uint32_t stinger_point_ms;
	uint32_t ms;
};

/* The transition that is on screen, with a reference held, along with what it
 * does to the audio of its two halves. */
static obs_source_t *warp_pl_get_transition_audio(struct warp_playlist_source *s, struct warp_pl_transition_audio *info)
{
	obs_source_t *transition;

	pthread_mutex_lock(&s->tr_mutex);
	transition = obs_source_get_ref(s->tr[s->active_dir].source);
	if (info) {
		info->is_stinger = s->tr[s->active_dir].is_stinger;
		info->stinger_cross_fade = s->tr[s->active_dir].stinger_cross_fade;
		info->stinger_point_ms = s->tr[s->active_dir].stinger_point_ms;
		info->ms = s->tr[s->active_dir].ms;
	}
	pthread_mutex_unlock(&s->tr_mutex);

	return transition;
}

/* the transition that is on screen, with a reference held */
static obs_source_t *warp_pl_get_transition(struct warp_playlist_source *s)
{
	return warp_pl_get_transition_audio(s, NULL);
}

static void warp_pl_publish(struct warp_playlist_source *s);

/* Carries out the work a change to the playlist left behind it - starting the
 * transition, opening the file that is to play, letting go of what is not
 * wanted, emitting the media signals - then publishes where the playlist
 * stands. Anything that work queues in turn is picked up in the same pass, so a
 * nested call returns straight away. Graphics thread. */
static void warp_pl_flush(struct warp_playlist_source *s)
{
	if (s->deferring)
		return;

	s->deferring = true;

	for (;;) {
		DARRAY(obs_source_t *) retired;

		bool transition_armed = s->transition_armed;
		bool transition_use = s->transition_use;
		size_t transition_dir = s->transition_dir;
		uint32_t transition_ms;
		obs_source_t *transition = NULL;
		obs_source_t *handover = NULL;
		obs_source_t *target = NULL;
		bool switch_dir = false;

		bool play = s->play_armed;
		size_t play_order_pos = s->play_order_pos;
		bool play_transition = s->play_transition;
		size_t play_dir = s->play_dir;

		bool preload = s->preload_requested;
		size_t preload_order_pos = s->preload_order_pos;
		uint64_t gen = s->play_gen;

		bool started = s->signal_started;
		bool ended = s->signal_ended;

		retired.da = s->retired.da;
		da_init(s->retired);

		if (transition_armed) {
			/* the transition of the direction being moved in, or
			 * the forward one when there is none of its own yet */
			if (!s->tr[transition_dir].source)
				transition_dir = WARP_PL_DIR_FORWARD;

			transition = obs_source_get_ref(s->tr[transition_dir].source);

			/* A switch that runs through the other transition takes
			 * what is on screen over to it first, so its outgoing
			 * half has the picture that is up rather than nothing. */
			if (transition && transition_dir != s->active_dir) {
				switch_dir = true;
				handover = obs_source_get_ref(s->tr[s->active_dir].source);
			}

			target = s->transition_target;
			s->transition_target = NULL;
		}

		transition_ms = warp_pl_transition_ms(s, transition_dir);

		s->transition_armed = false;
		s->play_armed = false;
		s->preload_requested = false;
		s->signal_started = false;
		s->signal_ended = false;

		if (!retired.num && !transition_armed && !play && !preload && !started && !ended) {
			s->deferring = false;
			da_free(retired);
			warp_pl_publish(s);
			return;
		}

		if (switch_dir) {
			/* handing the picture over is instant, so the swap to
			 * the other transition costs no frame of its own */
			if (handover) {
				obs_source_t *showing = obs_transition_get_active_source(handover);

				if (showing) {
					obs_transition_set(transition, showing);
					obs_source_release(showing);
				}
			}

			pthread_mutex_lock(&s->tr_mutex);
			s->active_dir = transition_dir;
			pthread_mutex_unlock(&s->tr_mutex);
		}

		/* The transition takes its own references, so it is told about
		 * the change before the outgoing items are let go of. A switch
		 * that is not animated is made by setting what the transition
		 * shows outright: starting a transition of no length from
		 * nothing has libobs swap its two halves over on the tick after,
		 * and the frame that swap lands on can render empty - a blink to
		 * black just after the first file of a playlist goes up. */
		if (transition && transition_use)
			obs_transition_start(transition, OBS_TRANSITION_MODE_AUTO, transition_ms, target);
		else if (transition)
			obs_transition_set(transition, target);

		/* whatever the transition that stepped aside was holding is not
		 * on screen any more */
		if (handover) {
			obs_transition_clear(handover);
			obs_source_release(handover);
		}

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
			warp_pl_start(s, play_order_pos, play_transition, play_dir, gen);
		if (preload)
			warp_pl_open_preload(s, preload_order_pos, gen);
	}
}

/* ------------------------------------------------------------------------- */
/* audio
 *
 * The playlist hands OBS its own audio rather than leaving the items to be
 * mixed up the source tree through the transition. That is what puts it in the
 * audio mixer next to every other source, with a fader, a mute, monitoring,
 * audio filters and its own tracks: a composite source may not carry
 * OBS_SOURCE_AUDIO at all, so for as long as the items were mixed for it, the
 * playlist had none of that and the volume property was the only level it had.
 *
 * It also closes the hole that made that level a suggestion. libobs tags any
 * source it meets twice in one pass over the audio tree and mixes it straight
 * into the program mix as a root node instead of letting its parents mix it,
 * at the file's own level. An item is met twice whenever the tree is walked
 * more than once - a second video mix looking at the same scene, which is what
 * studio mode and every extra canvas do, or both direction transitions holding
 * the item across a hand-over - and the playlist's volume never touched those
 * samples, so a playlist turned down to 0% could still be heard.
 *
 * So the items are taken out of libobs' audio altogether: each is opened with
 * no audio mixers of its own, which leaves it nothing to mix anywhere, and the
 * playlist taps its samples as they are decoded instead. What is tapped is
 * already in the mix format, because libobs resamples on the way into
 * obs_source_output_audio(), so the mixer below only has to line the two items
 * of a transition up in time, scale them by the crossfade, and pass the result
 * on. */

static inline uint64_t warp_pl_frames_to_ns(size_t sample_rate, uint64_t frames)
{
	return util_mul_div64(frames, UINT64_C(1000000000), sample_rate);
}

static inline uint64_t warp_pl_ns_to_frames(size_t sample_rate, uint64_t ns)
{
	return util_mul_div64(ns, sample_rate, UINT64_C(1000000000));
}

/* Throws away what the mixer is holding and sets the format it works in.
 * Expects the mixer's lock to be held. */
static void warp_pl_mix_reset(struct warp_pl_audio *a, size_t channels, size_t sample_rate)
{
	for (size_t ch = 0; ch < MAX_AUDIO_CHANNELS; ch++) {
		bfree(a->buf[ch]);
		a->buf[ch] = NULL;
	}

	a->capacity = 0;
	a->frames = 0;
	a->ts = 0;
	a->have = false;
	a->next_ts = 0;
	a->flushed = false;
	a->channels = channels;
	a->sample_rate = sample_rate;
}

/* Makes room for 'frames' samples and clears whatever of them is new, so that
 * a gap between two items is silence rather than the last thing that stood
 * there. Returns false when that is more than the mixer will hold. Expects the
 * mixer's lock to be held. */
static bool warp_pl_mix_reserve(struct warp_pl_audio *a, size_t frames)
{
	size_t limit = (size_t)warp_pl_ns_to_frames(a->sample_rate, WARP_PL_MIX_MAX_NS);

	if (!a->channels || frames > limit)
		return false;

	if (frames > a->capacity) {
		size_t capacity = a->capacity ? a->capacity : 1024;

		while (capacity < frames)
			capacity *= 2;
		if (capacity > limit)
			capacity = limit;

		for (size_t ch = 0; ch < a->channels; ch++) {
			a->buf[ch] = brealloc(a->buf[ch], capacity * sizeof(float));
			memset(a->buf[ch] + a->frames, 0, (capacity - a->frames) * sizeof(float));
		}

		a->capacity = capacity;
	} else if (frames > a->frames) {
		for (size_t ch = 0; ch < a->channels; ch++)
			memset(a->buf[ch] + a->frames, 0, (frames - a->frames) * sizeof(float));
	}

	if (frames > a->frames)
		a->frames = frames;

	return true;
}

/* Hands OBS whatever of the mix nothing is going to add to any more: everything
 * up to the point the item that is furthest behind has reached, and never more
 * than the hold behind the clock, so an item that stops handing audio over
 * cannot wedge the ones that have not. Expects the mixer's lock to be held. */
static void warp_pl_mix_flush(struct warp_playlist_source *s)
{
	struct warp_pl_audio *a = &s->audio;

	if (!a->have || !a->frames || !a->sample_rate || !a->channels)
		return;

	uint64_t now = os_gettime_ns();
	uint64_t horizon = a->ts + warp_pl_frames_to_ns(a->sample_rate, a->frames);

	for (size_t i = 0; i < WARP_PL_NUM_DECKS; i++) {
		struct warp_pl_deck *deck = &a->deck[i];

		if (!deck->source || !deck->producing)
			continue;

		if (now - deck->arrived > WARP_PL_DECK_IDLE_NS) {
			deck->producing = false;
			continue;
		}

		if (deck->end_ts < horizon)
			horizon = deck->end_ts;
	}

	/* nothing is waited on for longer than the hold, so a source that stops
	 * handing audio over cannot wedge the ones that have not */
	if (now > WARP_PL_MIX_HOLD_NS && horizon < now - WARP_PL_MIX_HOLD_NS)
		horizon = now - WARP_PL_MIX_HOLD_NS;

	if (horizon <= a->ts)
		return;

	size_t frames = (size_t)warp_pl_ns_to_frames(a->sample_rate, horizon - a->ts);

	if (frames > a->frames)
		frames = a->frames;
	if (!frames)
		return;

	struct obs_source_audio out = {0};

	for (size_t ch = 0; ch < a->channels; ch++)
		out.data[ch] = (const uint8_t *)a->buf[ch];

	out.frames = (uint32_t)frames;
	out.speakers = a->speakers;
	out.format = AUDIO_FORMAT_FLOAT_PLANAR;
	out.samples_per_sec = (uint32_t)a->sample_rate;
	out.timestamp = a->ts;

	obs_source_output_audio(s->source, &out);

	a->ts += warp_pl_frames_to_ns(a->sample_rate, frames);
	a->next_ts = a->ts;
	a->flushed = true;
	a->frames -= frames;

	if (a->frames) {
		for (size_t ch = 0; ch < a->channels; ch++)
			memmove(a->buf[ch], a->buf[ch] + frames, a->frames * sizeof(float));
	} else {
		a->have = false;
	}
}

/* Takes a packet of one item's audio into the mix. Runs on that item's own
 * media thread, which is why the mixer has a lock of its own, and why it is a
 * leaf that is only held long enough to touch the mix: audio cannot wait. */
static void warp_pl_mix_submit(struct warp_playlist_source *s, obs_source_t *source, const struct audio_data *audio,
			       bool muted)
{
	audio_t *obs_audio = obs_get_audio();

	if (!obs_audio || !audio || !audio->frames)
		return;

	const struct audio_output_info *info = audio_output_get_info(obs_audio);
	size_t channels = audio_output_get_channels(obs_audio);
	size_t sample_rate = audio_output_get_sample_rate(obs_audio);

	if (!info || !channels || channels > MAX_AUDIO_CHANNELS || !sample_rate)
		return;

	struct warp_pl_audio *a = &s->audio;
	struct warp_pl_deck *deck = NULL;

	pthread_mutex_lock(&a->mutex);

	for (size_t i = 0; i < WARP_PL_NUM_DECKS; i++) {
		if (a->deck[i].source == source) {
			deck = &a->deck[i];
			break;
		}
	}

	/* the item was taken off the mixer between the packet being handed over
	 * and this getting to look at it */
	if (deck) {
		if (a->channels != channels || a->sample_rate != sample_rate)
			warp_pl_mix_reset(a, channels, sample_rate);

		a->speakers = info->speakers;

		/* put the packet on the clock the mixer works in */
		uint64_t arrived = os_gettime_ns();
		int64_t raw = (int64_t)audio->timestamp;

		if (!deck->anchored) {
			deck->offset = (int64_t)arrived - raw;
			deck->anchored = true;
		} else {
			uint64_t placed = (uint64_t)(raw + deck->offset);
			uint64_t drift = placed > arrived ? placed - arrived : arrived - placed;

			if (drift > WARP_PL_DECK_RESYNC_NS)
				deck->offset = (int64_t)arrived - raw;
		}

		uint64_t ts = (uint64_t)(raw + deck->offset);
		size_t frames = audio->frames;
		size_t skipped = 0;

		/* Where the mix will take samples from: never behind what has
		 * already gone out, and never behind what is being held, so a
		 * packet that turns up late is trimmed rather than mixed in
		 * under audio that has moved on. */
		uint64_t floor_ts = a->flushed ? a->next_ts : 0;
		bool have_floor = a->flushed;

		if (a->have && (!have_floor || a->ts > floor_ts)) {
			floor_ts = a->ts;
			have_floor = true;
		}

		if (have_floor && ts < floor_ts) {
			skipped = (size_t)warp_pl_ns_to_frames(sample_rate, floor_ts - ts);

			if (skipped >= frames) {
				/* all of it is behind; the deck has still been
				 * heard from, so it is not waited on for this */
				skipped = frames;
				frames = 0;
			} else {
				frames -= skipped;
			}

			ts = floor_ts;
		}

		if (frames) {
			if (!a->have) {
				a->ts = ts;
				a->frames = 0;
				a->have = true;
			}

			size_t offset = (size_t)warp_pl_ns_to_frames(sample_rate, ts - a->ts);

			if (!warp_pl_mix_reserve(a, offset + frames)) {
				/* Further ahead than the mixer will hold, which
				 * takes an item whose timestamps have run away.
				 * Start again from this packet rather than
				 * growing without end. */
				warp_pl_mix_reset(a, channels, sample_rate);
				a->ts = ts;
				a->have = true;
				offset = 0;

				if (!warp_pl_mix_reserve(a, frames))
					frames = 0;
			}

			float level = deck->from_transition ? a->transition_volume : a->volume;
			float target = muted ? 0.0f : deck->gain * level;
			float step = frames ? (target - deck->applied_gain) / (float)frames : 0.0f;

			for (size_t ch = 0; ch < channels && frames; ch++) {
				const float *in = (const float *)audio->data[ch];
				float *out = a->buf[ch] + offset;
				float gain = deck->applied_gain;

				if (!in)
					continue;

				in += skipped;

				for (size_t i = 0; i < frames; i++) {
					out[i] += in[i] * gain;
					gain += step;
				}
			}

			if (frames)
				deck->applied_gain = target;
		}

		deck->end_ts = ts + warp_pl_frames_to_ns(sample_rate, frames);
		deck->arrived = arrived;
		deck->producing = true;

		warp_pl_mix_flush(s);
	}

	pthread_mutex_unlock(&a->mutex);
}

static void warp_pl_audio_capture(void *param, obs_source_t *source, const struct audio_data *audio, bool muted)
{
	warp_pl_mix_submit(param, source, audio, muted);
}

/* What the two halves of a running transition are played at, worked out the
 * way the transition itself would have worked them out: everything crosses the
 * two files over, and a stinger set to fade out and back in instead takes the
 * outgoing file down to nothing by the point it swaps at and brings the
 * incoming one back up from there. */
static void warp_pl_transition_gains(const struct warp_pl_transition_audio *info, float t, float *out, float *in)
{
	if (t < 0.0f)
		t = 0.0f;
	else if (t > 1.0f)
		t = 1.0f;

	if (info->is_stinger && !info->stinger_cross_fade) {
		float point = info->ms ? (float)info->stinger_point_ms / (float)info->ms : 0.5f;

		if (point < 0.001f)
			point = 0.001f;
		else if (point > 0.999f)
			point = 0.999f;

		float faded_out = t / point;
		float faded_in = (1.0f - t) / (1.0f - point);

		*out = 1.0f - (faded_out > 1.0f ? 1.0f : faded_out);
		*in = 1.0f - (faded_in > 1.0f ? 1.0f : faded_in);
		return;
	}

	*out = 1.0f - t;
	*in = t;
}

/* What the mixer should be listening to: the two halves of the transition and
 * whatever it plays of its own on top of them. */
struct warp_pl_audio_scan {
	obs_source_t *want[WARP_PL_NUM_DECKS];
	float gain[WARP_PL_NUM_DECKS];
	bool from_transition[WARP_PL_NUM_DECKS];
	size_t num;
};

static size_t warp_pl_audio_scan_find(const struct warp_pl_audio_scan *scan, obs_source_t *source)
{
	for (size_t i = 0; i < scan->num; i++) {
		if (scan->want[i] == source)
			return i;
	}

	return DARRAY_INVALID;
}

/* Expects 'source' to be one the caller has a reference to for the length of
 * the call, which is what obs_source_enum_active_sources() guarantees. A source
 * that stands in more than one place is played once, at everything it is asked
 * for put together: the two halves of a transition are the same item for a
 * moment whenever the picture is handed from one transition to the other. */
static void warp_pl_audio_scan_add(struct warp_pl_audio_scan *scan, obs_source_t *source, float gain,
				   bool from_transition)
{
	if (!source)
		return;

	size_t found = warp_pl_audio_scan_find(scan, source);

	if (found != DARRAY_INVALID) {
		scan->gain[found] += gain;
		return;
	}

	if (scan->num == WARP_PL_NUM_DECKS)
		return;

	scan->want[scan->num] = obs_source_get_ref(source);
	scan->gain[scan->num] = gain;
	scan->from_transition[scan->num] = from_transition;
	scan->num++;
}

/* Picks up whatever the transition itself is playing on top of the two halves
 * it is holding, which for a stinger is the video it swaps over behind and the
 * matte alongside it. The matte is muted by the transition that made it, and a
 * muted source is mixed in as the silence it is, so the two need not be told
 * apart. */
static void warp_pl_audio_scan_transition(obs_source_t *parent, obs_source_t *child, void *param)
{
	struct warp_pl_audio_scan *scan = param;

	UNUSED_PARAMETER(parent);

	/* the halves are in already, at what the crossfade asks for */
	if (warp_pl_audio_scan_find(scan, child) != DARRAY_INVALID)
		return;

	warp_pl_audio_scan_add(scan, child, 1.0f, true);
}

/* Points the mixer at the sources the transition on screen is playing and sets
 * what each of them is played at. Runs on the tick, with no lock held: putting
 * a tap on a source takes that source's own lock, so it is never done under the
 * mixer's. */
static void warp_pl_audio_sync(struct warp_playlist_source *s)
{
	struct warp_pl_audio *a = &s->audio;
	struct warp_pl_transition_audio info;
	struct warp_pl_audio_scan scan = {0};
	obs_source_t **want = scan.want;
	float *gain = scan.gain;
	bool placed[WARP_PL_NUM_DECKS] = {false};
	obs_source_t *taken[WARP_PL_NUM_DECKS] = {NULL};
	obs_source_t *dropped[WARP_PL_NUM_DECKS] = {NULL};

	obs_source_t *transition = warp_pl_get_transition_audio(s, &info);

	if (transition) {
		obs_source_t *from = obs_transition_get_source(transition, OBS_TRANSITION_SOURCE_A);
		obs_source_t *into = NULL;
		float from_gain = 1.0f;
		float into_gain = 0.0f;

		if (obs_transition_is_active(transition)) {
			into = obs_transition_get_source(transition, OBS_TRANSITION_SOURCE_B);
			warp_pl_transition_gains(&info, obs_transition_get_time(transition), &from_gain, &into_gain);
		}

		warp_pl_audio_scan_add(&scan, from, from_gain, false);
		warp_pl_audio_scan_add(&scan, into, into_gain, false);

		/* the two halves are already in, so this only adds what the
		 * transition brought along itself */
		obs_source_enum_active_sources(transition, warp_pl_audio_scan_transition, &scan);

		obs_source_release(from);
		obs_source_release(into);
		obs_source_release(transition);
	}

	for (size_t i = 0; i < WARP_PL_NUM_DECKS; i++) {
		obs_source_t *held = a->deck[i].source;
		bool keep = false;

		if (!held)
			continue;

		for (size_t j = 0; j < WARP_PL_NUM_DECKS; j++) {
			if (want[j] == held)
				keep = true;
		}

		if (!keep) {
			obs_source_remove_audio_capture_callback(held, warp_pl_audio_capture, s);
			dropped[i] = held;
		}
	}

	pthread_mutex_lock(&a->mutex);

	for (size_t i = 0; i < WARP_PL_NUM_DECKS; i++) {
		if (dropped[i])
			memset(&a->deck[i], 0, sizeof(a->deck[i]));
	}

	/* an item that is already tapped keeps its deck, so a crossfade that is
	 * running rides on rather than starting over from silence */
	for (size_t j = 0; j < WARP_PL_NUM_DECKS; j++) {
		if (!want[j])
			continue;

		for (size_t i = 0; i < WARP_PL_NUM_DECKS; i++) {
			if (a->deck[i].source != want[j])
				continue;

			a->deck[i].gain = gain[j];
			a->deck[i].from_transition = scan.from_transition[j];
			placed[j] = true;
			break;
		}
	}

	for (size_t j = 0; j < WARP_PL_NUM_DECKS; j++) {
		if (!want[j] || placed[j])
			continue;

		for (size_t i = 0; i < WARP_PL_NUM_DECKS; i++) {
			if (a->deck[i].source)
				continue;

			memset(&a->deck[i], 0, sizeof(a->deck[i]));
			a->deck[i].source = obs_source_get_ref(want[j]);
			a->deck[i].gain = gain[j];
			a->deck[i].from_transition = scan.from_transition[j];

			taken[i] = a->deck[i].source;
			placed[j] = true;
			break;
		}
	}

	pthread_mutex_unlock(&a->mutex);

	for (size_t i = 0; i < WARP_PL_NUM_DECKS; i++) {
		if (!taken[i])
			continue;

		/* Everything the playlist listens to is played by the playlist
		 * and by nothing else. The items are opened without mixers of
		 * their own; a transition's own video is not the playlist's to
		 * open, so it is quietened here as it is picked up. */
		obs_source_set_audio_mixers(taken[i], 0);
		obs_source_add_audio_capture_callback(taken[i], warp_pl_audio_capture, s);
	}

	for (size_t i = 0; i < WARP_PL_NUM_DECKS; i++) {
		if (dropped[i])
			obs_source_release(dropped[i]);
	}

	for (size_t j = 0; j < WARP_PL_NUM_DECKS; j++) {
		if (want[j])
			obs_source_release(want[j]);
	}

	/* an item that has gone quiet, or gone entirely, leaves the last of its
	 * audio behind; nothing is going to add to it, so it goes out now */
	pthread_mutex_lock(&a->mutex);
	warp_pl_mix_flush(s);
	pthread_mutex_unlock(&a->mutex);
}

/* takes every tap off and drops what the mixer is holding */
static void warp_pl_audio_free(struct warp_playlist_source *s)
{
	struct warp_pl_audio *a = &s->audio;
	obs_source_t *held[WARP_PL_NUM_DECKS];

	for (size_t i = 0; i < WARP_PL_NUM_DECKS; i++) {
		held[i] = a->deck[i].source;

		if (held[i])
			obs_source_remove_audio_capture_callback(held[i], warp_pl_audio_capture, s);
	}

	pthread_mutex_lock(&a->mutex);
	memset(a->deck, 0, sizeof(a->deck));
	warp_pl_mix_reset(a, 0, 0);
	pthread_mutex_unlock(&a->mutex);

	for (size_t i = 0; i < WARP_PL_NUM_DECKS; i++) {
		if (held[i])
			obs_source_release(held[i]);
	}
}

/* ------------------------------------------------------------------------- */
/* playlist bookkeeping (graphics thread) */

static uint64_t warp_pl_rand(struct warp_playlist_source *s)
{
	uint64_t x = s->rand_state;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	s->rand_state = x;

	return x;
}

/* Rebuilds the play order; shuffled orders are reshuffled on every pass so a
 * looping playlist does not repeat the same random order forever. 'last' is the
 * file the pass before ended on, which the new pass does not start with: a
 * reshuffle that put it first would play it twice in a row. SIZE_MAX when there
 * is none. */
static void warp_pl_build_order(struct warp_playlist_source *s, size_t last)
{
	/* a position in the order that is being replaced means nothing in the
	 * new one */
	s->resume_pos = SIZE_MAX;

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

	if (s->order.array[0] == last) {
		size_t j = 1 + (size_t)(warp_pl_rand(s) % (uint64_t)(s->order.num - 1));
		size_t tmp = s->order.array[0];

		s->order.array[0] = s->order.array[j];
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

/* what an item is opened with, read before the item is made */
struct warp_pl_item_config {
	int speed;
	bool hw_decode;
	bool gpu_direct;
	bool preserve_pitch;
	int64_t in_ms;
	int64_t out_ms;
	int64_t tail_ms;
};

/*  Graphics thread. */
static const struct warp_pl_clip *warp_pl_find_clip(struct warp_playlist_source *s, const char *path)
{
	for (size_t i = 0; path && i < s->clips.num; i++) {
		if (strcmp(s->clips.array[i].path, path) == 0)
			return &s->clips.array[i];
	}

	return NULL;
}

/* the speed 'path' starts at: its own, or the playlist's; graphics thread */
static int warp_pl_start_speed(struct warp_playlist_source *s, const char *path)
{
	const struct warp_pl_clip *clip = warp_pl_find_clip(s, path);

	return clip && clip->speed ? clip->speed : s->base_speed;
}

/* what 'path' is loaded with, or what an item is made with when 'path' is
 * NULL;  Graphics thread. */
static void warp_pl_read_item_config(struct warp_playlist_source *s, const char *path, struct warp_pl_item_config *cfg)
{
	const struct warp_pl_clip *clip = warp_pl_find_clip(s, path);

	cfg->speed = warp_pl_start_speed(s, path);
	cfg->hw_decode = s->hw_decode;
	cfg->gpu_direct = s->gpu_direct;
	cfg->preserve_pitch = s->preserve_pitch;
	cfg->in_ms = clip ? clip->in_ms : 0;
	cfg->out_ms = clip ? clip->out_ms : 0;
	cfg->tail_ms = clip ? clip->tail_ms : 0;
}

/* Makes an item to play files in, with no file in it yet: files are loaded
 * into it with warp_pl_load_item().  Graphics thread. */
static obs_source_t *warp_pl_create_item(struct warp_playlist_source *s, const struct warp_pl_item_config *cfg)
{
	obs_data_t *settings = obs_data_create();

	obs_data_set_bool(settings, "is_local_file", true);
	obs_data_set_string(settings, "local_file", "");
	obs_data_set_bool(settings, "looping", false);
	/* the item holds its last frame instead of going black: the playlist
	 * decides what is shown once an item is done */
	obs_data_set_bool(settings, "clear_on_media_end", false);
	obs_data_set_bool(settings, "restart_on_activate", false);
	obs_data_set_bool(settings, "close_when_inactive", false);
	obs_data_set_bool(settings, "hw_decode", cfg->hw_decode);
	obs_data_set_bool(settings, "gpu_direct", cfg->gpu_direct);
	obs_data_set_bool(settings, "preserve_pitch", cfg->preserve_pitch);
	obs_data_set_int(settings, "speed_percent", cfg->speed);
	/* one settings dump per playlist item would drown the log */
	obs_data_set_bool(settings, "log_changes", false);

	obs_source_t *item = obs_source_create_private(WARP_MEDIA_SOURCE_ID, "warp-playlist-item", settings);
	obs_data_release(settings);

	if (!item) {
		WARP_PL_LOG(LOG_WARNING, "could not make an item to play files in");
		return item;
	}

	/* The item is played through the playlist's own mixer and nowhere else.
	 * Leaving it no mixers of its own is what makes that true: libobs mixes
	 * a source it meets twice in the audio tree straight into the program
	 * mix by itself, past every parent that would have scaled it, and an
	 * item is met twice as soon as anything looks at the scene a second
	 * time. With no mixers there is nothing for that path to carry. */
	obs_source_set_audio_mixers(item, 0);

	/* Every item has a zoom filter of its own, driven by this playlist. It
	 * costs nothing while the file is being watched whole - an unzoomed
	 * filter steps out of the way - and it is what keeps a framing with the
	 * video it was set on: the file going up is framed on its own filter,
	 * so it comes up showing the whole picture however far into the last
	 * one the operator had zoomed. */
	obs_source_t *zoom = warp_zoom_filter_create_driven(obs_module_text("Warp.ZoomFilter.Name"));

	if (zoom) {
		obs_source_filter_add(item, zoom);
		obs_source_release(zoom);
	} else {
		WARP_PL_LOG(LOG_WARNING, "could not put a zoom on a playlist item");
	}

	return item;
}

/* Loads 'path' into 'item', to be played or held on its first frame as
 * 'playback' says, cut and at the speed 'cfg' says. */
static void warp_pl_load_item(obs_source_t *item, const char *path, const char *playback,
			      const struct warp_pl_item_config *cfg)
{
	calldata_t cd = {0};

	calldata_init(&cd);
	calldata_set_string(&cd, "path", path);
	calldata_set_string(&cd, "playback", playback);
	calldata_set_int(&cd, "speed", cfg->speed);
	calldata_set_int(&cd, "in_ms", cfg->in_ms);
	calldata_set_int(&cd, "out_ms", cfg->out_ms);
	calldata_set_int(&cd, "tail_ms", cfg->tail_ms);
	proc_handler_call(obs_source_get_proc_handler(item), WARP_MEDIA_LOAD_PROC, &cd);
	calldata_free(&cd);
}

static void warp_pl_call_item_proc(obs_source_t *item, const char *proc, const char *arg, long long value)
{
	if (!item)
		return;

	calldata_t cd = {0};

	calldata_init(&cd);
	if (arg)
		calldata_set_int(&cd, arg, value);
	proc_handler_call(obs_source_get_proc_handler(item), proc, &cd);
	calldata_free(&cd);
}

static void warp_pl_call_item_bool(obs_source_t *item, const char *proc, const char *arg, bool value)
{
	if (!item)
		return;

	calldata_t cd = {0};

	calldata_init(&cd);
	calldata_set_bool(&cd, arg, value);
	proc_handler_call(obs_source_get_proc_handler(item), proc, &cd);
	calldata_free(&cd);
}

/* Whether 'item' has the file it was last loaded with ready to put on screen:
 * open and, when it has video, a frame of that file written into the item's
 * texture, which an item waiting for its switch is not rendered to have.
 * 'failed' is set once the file has turned out not to open. */
static bool warp_pl_item_ready(obs_source_t *item, bool *failed)
{
	calldata_t cd = {0};
	bool ready = false;

	*failed = false;

	if (!item)
		return false;

	calldata_init(&cd);

	if (!proc_handler_call(obs_source_get_proc_handler(item), WARP_MEDIA_READY_PROC, &cd) ||
	    !calldata_get_bool(&cd, "ready", &ready))
		/* not a source that can answer: fall back to it having picture */
		ready = obs_source_get_width(item) != 0;
	else
		calldata_get_bool(&cd, "failed", failed);

	calldata_free(&cd);

	return ready;
}

/* the most items kept with nothing in them, for the next file to be loaded
 * into */
#define WARP_PL_MAX_IDLE 2

/* Lets go of an item the playlist has finished with: its file is closed and
 * the item is kept for the next file, or released once enough are kept.
 * Graphics thread. */
static void warp_pl_recycle(struct warp_playlist_source *s, obs_source_t *item)
{
	if (!item)
		return;

	warp_pl_call_item_proc(item, WARP_MEDIA_CLOSE_PROC, NULL, 0);

	if (s->idle.num < WARP_PL_MAX_IDLE)
		da_push_back(s->idle, &item);
	else
		warp_pl_retire(s, item);
}

/* an item to load a file into, if one is kept;  Graphics thread. */
static obs_source_t *warp_pl_take_idle(struct warp_playlist_source *s)
{
	if (!s->idle.num)
		return NULL;

	obs_source_t *item = s->idle.array[s->idle.num - 1];
	da_pop_back(s->idle);
	return item;
}

/* releases every kept item, whose settings are no longer what a file would be
 * loaded with;  Graphics thread. */
static void warp_pl_flush_idle(struct warp_playlist_source *s)
{
	for (size_t i = 0; i < s->idle.num; i++)
		warp_pl_retire(s, s->idle.array[i]);
	da_resize(s->idle, 0);
}

/*  Graphics thread. */
static void warp_pl_drop_back(struct warp_playlist_source *s)
{
	warp_pl_recycle(s, s->back);
	s->back = NULL;
	bfree(s->back_path);
	s->back_path = NULL;
}

/*  Graphics thread. */
static void warp_pl_drop_preloaded(struct warp_playlist_source *s)
{
	warp_pl_recycle(s, s->preloaded);
	s->preloaded = NULL;

	bfree(s->preloaded_path);
	s->preloaded_path = NULL;
}

/* Throws away an item that was opened for a switch that is no longer wanted;
 * Graphics thread. */
static void warp_pl_drop_pending(struct warp_playlist_source *s)
{
	if (!s->pending)
		return;

	warp_pl_recycle(s, s->pending);
	s->pending = NULL;
	bfree(s->pending_path);
	s->pending_path = NULL;
}

/* Puts 'item' (which may be NULL, to show nothing), playing 'path', on screen
 * and lets go of whatever was shown before it, taking over the caller's
 * reference to 'item' and 'path'. The transition for 'dir' is told about the
 * change by warp_pl_flush(). Graphics thread. */
static void warp_pl_show(struct warp_playlist_source *s, obs_source_t *item, char *path, bool use_transition,
			 size_t dir)
{
	warp_pl_arm_transition(s, item, use_transition, dir);

	/* the outgoing item stays until its transition has played out; the
	 * transition holds its own reference, this one keeps it for the tick to
	 * park or let go of once it has */
	warp_pl_recycle(s, s->prev);

	bfree(s->prev_path);
	s->prev = s->current;
	s->prev_path = s->current_path;
	s->current = item;
	s->current_path = path;
	s->showing_nothing = item == NULL;

	/* the zoom of the file that is going up; the one stepping aside keeps
	 * its own, so it plays out the transition framed the way it was watched */
	warp_pl_retire(s, s->current_zoom);
	s->current_zoom = item ? warp_zoom_filter_find(item) : NULL;
}

/* Puts the item that is waiting for the switch on screen, once it has picture
 * to transition into. An item that never opens is put on screen anyway rather
 * than held forever, and is then written off by the stall check like any other
 * file that does not play.  Graphics thread. */
static void warp_pl_promote_pending(struct warp_playlist_source *s)
{
	obs_source_t *item;
	char *path;
	size_t order_pos;

	if (!s->pending)
		return;

	order_pos = s->pending_order_pos;
	path = s->pending_path;

	bool failed;

	if (!warp_pl_item_ready(s->pending, &failed)) {
		if (failed) {
			/* A file that will not open is skipped there and then,
			 * rather than shown as nothing until the stall check
			 * writes it off. */
			WARP_PL_LOG(LOG_WARNING, "'%s' could not be opened, skipping it", path ? path : "");

			int dir = s->pending_dir == WARP_PL_DIR_BACKWARD ? -1 : 1;

			warp_pl_drop_pending(s);

			if (++s->fail_streak >= s->order.num) {
				WARP_PL_LOG(LOG_WARNING, "none of the files in the playlist could be opened");
				s->fail_streak = 0;
				warp_pl_end(s);
				return;
			}

			s->pos = order_pos;
			warp_pl_advance(s, dir);
			return;
		}

		if (s->pending_age < WARP_PL_OPEN_WAIT_SECONDS)
			return;

		WARP_PL_LOG(LOG_WARNING, "'%s' had nothing to show in time, switching to it anyway", path ? path : "");
	}

	s->fail_streak = 0;

	item = s->pending;
	s->pending = NULL;
	s->pending_path = NULL;

	s->pos = order_pos;
	s->cur_age = 0.0f;
	s->speed = warp_pl_start_speed(s, path);

	/* every item starts at its own speed, or the configured one, whatever
	 * the previous item was being played back at */
	warp_pl_call_item_proc(item, "warp_set_speed", "speed", s->speed);

	/* and framed the way every file starts, however far into the last one
	 * the operator had zoomed: the framing was that video's, not this one's */
	warp_zoom_control_restore_default(&s->zoom);

	WARP_PL_LOG(LOG_INFO, "playing %d/%d: %s", (int)(order_pos + 1), (int)s->order.num, path ? path : "");

	warp_pl_show(s, item, path, s->pending_transition, s->pending_dir);

	s->state = OBS_MEDIA_STATE_PLAYING;
	s->signal_started = true;
}

/* Asks for the item at 'order_pos' to be played, switched to with the
 * transition for 'dir'. The file is opened, and the item put on screen, by
 * warp_pl_start() when the change is flushed. Graphics thread. */
static void warp_pl_play_pos(struct warp_playlist_source *s, size_t order_pos, bool use_transition, size_t dir)
{
	if (!warp_pl_path_at(s, order_pos))
		return;

	/* an item opened for the switch this one replaces is not wanted */
	warp_pl_drop_pending(s);

	s->play_gen++;
	s->play_armed = true;
	s->play_order_pos = order_pos;
	s->play_transition = use_transition;
	s->play_dir = dir;
}

/* end of the playlist, or end of an item with auto advance turned off;
 * Graphics thread. */
static void warp_pl_end(struct warp_playlist_source *s)
{
	if (s->state == OBS_MEDIA_STATE_ENDED)
		return;

	/* the item that ended is kept, so it can be restarted or resumed;
	 * it is only taken off screen */
	if (s->clear_on_media_end) {
		warp_pl_arm_transition(s, NULL, true, WARP_PL_DIR_FORWARD);
		s->showing_nothing = true;
	}

	warp_pl_drop_pending(s);

	s->play_gen++;
	s->state = OBS_MEDIA_STATE_ENDED;
	s->signal_ended = true;
}

/* Where the first file asked to be played next sits in the play order, if one
 * is still in the list; files that have been taken out of it since are dropped
 * from the queue.  Graphics thread. */
static bool warp_pl_queued_pos(struct warp_playlist_source *s, size_t *out)
{
	while (s->queue.num) {
		for (size_t i = 0; i < s->order.num; i++) {
			const char *path = warp_pl_path_at(s, i);

			if (path && strcmp(path, s->queue.array[0]) == 0) {
				*out = i;
				return true;
			}
		}

		bfree(s->queue.array[0]);
		da_erase(s->queue, 0);
	}

	return false;
}

/* where going forward goes, without going there: a file asked to be played
 * next, then wherever the playlist was before one was, then the next file in
 * the order.  Graphics thread. */
static bool warp_pl_forward_target(struct warp_playlist_source *s, size_t *out)
{
	if (warp_pl_queued_pos(s, out))
		return true;

	if (s->resume_pos < s->order.num) {
		*out = s->resume_pos;
		return true;
	}

	return warp_pl_step_pos(s, 1, out);
}

/*  Graphics thread. */
static void warp_pl_advance(struct warp_playlist_source *s, int dir)
{
	size_t target;

	if (dir > 0 && warp_pl_queued_pos(s, &target)) {
		/* the playlist carries on from where it was once the files
		 * put in front of it have played */
		size_t natural;

		if (s->resume_pos >= s->order.num && warp_pl_step_pos(s, 1, &natural))
			s->resume_pos = natural;

		bfree(s->queue.array[0]);
		da_erase(s->queue, 0);

		warp_pl_play_pos(s, target, true, WARP_PL_DIR_FORWARD);
		return;
	}

	if (dir > 0 && s->resume_pos < s->order.num) {
		target = s->resume_pos;
		s->resume_pos = SIZE_MAX;
		warp_pl_play_pos(s, target, true, WARP_PL_DIR_FORWARD);
		return;
	}

	/* going back is a move through the list as it stands */
	s->resume_pos = SIZE_MAX;

	if (!warp_pl_step_pos(s, dir, &target)) {
		warp_pl_end(s);
		return;
	}

	/* a shuffled playlist gets a fresh order every time it wraps */
	if (s->shuffle && s->order.num > 1 && ((dir >= 0 && target == 0) || (dir < 0 && target + 1 == s->order.num))) {
		warp_pl_build_order(s, s->pos < s->order.num ? s->order.array[s->pos] : SIZE_MAX);
		s->pos = s->order.num;
	}

	warp_pl_play_pos(s, target, true, dir >= 0 ? WARP_PL_DIR_FORWARD : WARP_PL_DIR_BACKWARD);
}

/*  Graphics thread. */
static void warp_pl_stop(struct warp_playlist_source *s)
{
	warp_pl_drop_preloaded(s);
	warp_pl_drop_pending(s);
	warp_pl_drop_back(s);
	s->resume_pos = SIZE_MAX;

	/* nothing queued up, or already being opened, is wanted any more */
	s->play_gen++;
	s->play_armed = false;
	s->preload_requested = false;

	warp_pl_arm_transition(s, NULL, false, WARP_PL_DIR_FORWARD);

	warp_pl_recycle(s, s->prev);
	warp_pl_recycle(s, s->current);
	warp_pl_retire(s, s->current_zoom);
	s->prev = NULL;
	s->current = NULL;
	s->current_zoom = NULL;
	bfree(s->prev_path);
	bfree(s->current_path);
	s->prev_path = NULL;
	s->current_path = NULL;
	s->showing_nothing = true;

	/* whatever was framed is not being played any more, so the next file to
	 * go up starts from the whole picture like any other */
	warp_zoom_control_restore_default(&s->zoom);

	s->pos = s->order.num;
	s->state = OBS_MEDIA_STATE_STOPPED;
}

/* starts the playlist over from its first item;  Graphics thread. */
static void warp_pl_restart_playlist(struct warp_playlist_source *s)
{
	if (!s->order.num)
		return;

	if (s->shuffle)
		warp_pl_build_order(s, SIZE_MAX);

	/* starting over is a place of its own to carry on from */
	s->resume_pos = SIZE_MAX;

	warp_pl_drop_preloaded(s);
	warp_pl_play_pos(s, 0, true, WARP_PL_DIR_FORWARD);
}

/* restarts the item that is playing right now, at the configured speed;
 * Graphics thread. */
static void warp_pl_restart_current(struct warp_playlist_source *s)
{
	if (!s->current) {
		warp_pl_restart_playlist(s);
		return;
	}

	/* the item is not held by the transition once the playlist has ended
	 * with "show nothing", so put it back before restarting it */
	if (s->showing_nothing) {
		warp_pl_arm_transition(s, s->current, true, WARP_PL_DIR_FORWARD);
		s->showing_nothing = false;
	}

	warp_pl_drop_preloaded(s);
	warp_pl_drop_pending(s);

	/* an item that is still being opened is not wanted: the one on screen
	 * is the one being restarted */
	s->play_gen++;
	s->play_armed = false;

	s->speed = warp_pl_start_speed(s, s->current_path);
	warp_pl_call_item_proc(s->current, "warp_set_speed", "speed", s->speed);
	obs_source_media_restart(s->current);

	s->state = OBS_MEDIA_STATE_PLAYING;
	s->signal_started = true;
}

/*  Graphics thread. */
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

/* takes the item parked with 'path' in it, if there is one: the next file,
 * opened ahead of its switch, or the last one, parked once it was switched
 * away from.  Graphics thread. */
static obs_source_t *warp_pl_take_parked(struct warp_playlist_source *s, const char *path)
{
	obs_source_t *item = NULL;

	if (s->preloaded && s->preloaded_path && strcmp(s->preloaded_path, path) == 0) {
		item = s->preloaded;
		s->preloaded = NULL;
	} else if (s->back && s->back_path && strcmp(s->back_path, path) == 0) {
		item = s->back;
		s->back = NULL;
		bfree(s->back_path);
		s->back_path = NULL;
	}

	return item;
}

/* Gets the item warp_pl_play_pos() asked for playing and hands it to
 * warp_pl_promote_pending(), which puts it on screen once it has picture. A
 * file that is parked already - the next one, preloaded, or the last one - is
 * started where it sits; anything else is loaded into a kept item, or a new
 * one. Graphics thread. */
static void warp_pl_start(struct warp_playlist_source *s, size_t order_pos, bool use_transition, size_t dir,
			  uint64_t gen)
{
	struct warp_pl_item_config cfg;
	obs_source_t *item = NULL;
	bool parked = false;
	const char *at;
	char *path = NULL;

	at = warp_pl_path_at(s, order_pos);

	if (at) {
		path = bstrdup(at);
		item = warp_pl_take_parked(s, path);
		parked = item != NULL;
	}

	/* anything still preloaded is not what is being played */
	warp_pl_drop_preloaded(s);

	if (path && !item)
		item = warp_pl_take_idle(s);

	warp_pl_read_item_config(s, path, &cfg);

	warp_pl_flush(s);

	if (!path)
		return;

	if (!item)
		item = warp_pl_create_item(s, &cfg);

	if (!item) {
		bfree(path);
		return;
	}

	if (parked)
		obs_source_media_play_pause(item, false);
	else
		warp_pl_load_item(item, path, WARP_MEDIA_LOAD_PLAY, &cfg);

	at = warp_pl_path_at(s, order_pos);

	/* the playlist can be edited, or stopped, while the file is opening */
	if (s->play_gen != gen || !at || strcmp(at, path) != 0) {
		warp_pl_recycle(s, item);
		warp_pl_flush(s);
		bfree(path);
		return;
	}

	/* A parked item is ready right away, so the switch still lands on the
	 * tick it was asked for; one that is loading is held back for the few
	 * frames its file takes to open, which is what keeps the transition
	 * from starting against a source that has nothing to show. */
	warp_pl_drop_pending(s);

	s->pending = item;
	s->pending_path = path;
	s->pending_order_pos = order_pos;
	s->pending_transition = use_transition;
	s->pending_dir = dir;
	s->pending_age = 0.0f;

	warp_pl_promote_pending(s);

	warp_pl_flush(s);
}

/* Opens the item warp_pl_preload_next() asked for, ahead of the switch point,
 * held on its first frame.  Graphics thread. */
static void warp_pl_open_preload(struct warp_playlist_source *s, size_t order_pos, uint64_t gen)
{
	struct warp_pl_item_config cfg;
	obs_source_t *item = NULL;
	const char *at;
	char *path = NULL;

	at = warp_pl_path_at(s, order_pos);

	if (at && !s->preloaded && s->play_gen == gen) {
		/* the file before this one, parked, is the next one round in a
		 * playlist of two */
		if (s->back && s->back_path && strcmp(s->back_path, at) == 0) {
			s->preloaded = s->back;
			s->preloaded_path = s->back_path;
			s->back = NULL;
			s->back_path = NULL;
		} else {
			path = bstrdup(at);
			item = warp_pl_take_idle(s);
		}
	}

	warp_pl_read_item_config(s, path, &cfg);

	warp_pl_flush(s);

	if (!path)
		return;

	if (!item)
		item = warp_pl_create_item(s, &cfg);

	if (!item) {
		bfree(path);
		return;
	}

	warp_pl_load_item(item, path, WARP_MEDIA_LOAD_HOLD, &cfg);

	at = warp_pl_path_at(s, order_pos);

	if (s->preloaded || s->play_gen != gen || !at || strcmp(at, path) != 0) {
		/* the playlist moved on while the file was opening */
		warp_pl_recycle(s, item);
	} else {
		s->preloaded = item;
		s->preloaded_path = path;
		path = NULL;
	}

	warp_pl_flush(s);
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

/* Reads back what the transition for 'dir' is configured with: its own
 * properties write to the transition itself, so the store, and with it the
 * settings the source is saved with, only catch up when they are read off it.
 * Graphics thread. */
static void warp_pl_capture_transition_settings(struct warp_playlist_source *s, size_t dir)
{
	obs_source_t *tr = warp_pl_get_transition_dir(s, dir);
	obs_data_t *live;
	obs_data_t *copy;
	char *id;

	if (!tr)
		return;

	pthread_mutex_lock(&s->tr_mutex);
	id = bstrdup(s->tr[dir].id);
	pthread_mutex_unlock(&s->tr_mutex);

	if (id) {
		live = obs_source_get_settings(tr);
		copy = warp_pl_data_copy(live);

		pthread_mutex_lock(&s->tr_mutex);

		/* the transitions that have nothing to configure are left out
		 * of the settings the source is saved with entirely */
		if (!warp_pl_data_empty(copy))
			obs_data_set_obj(s->tr[dir].store, id, copy);

		/* where a stinger swaps the incoming file in, and how it takes
		 * the sound over, are its own settings, so the playlist picks
		 * them up along with them */
		if (strcmp(id, WARP_PL_TR_STINGER) == 0) {
			s->tr[dir].stinger_point_ms = warp_pl_stinger_point_ms(copy);
			s->tr[dir].stinger_cross_fade = obs_data_get_int(copy, "audio_fade_style") ==
							WARP_PL_STINGER_FADE_CROSS;
		}

		pthread_mutex_unlock(&s->tr_mutex);

		obs_data_release(copy);
		obs_data_release(live);
		bfree(id);
	}

	obs_source_release(tr);
}

/* reads back what every transition the playlist has is configured with; any
 * thread */
static void warp_pl_capture_transitions(struct warp_playlist_source *s)
{
	for (size_t dir = 0; dir < WARP_PL_NUM_DIRS; dir++)
		warp_pl_capture_transition_settings(s, dir);
}

/* Takes the transition settings the source was saved, or set from the outside,
 * with over the store of 'dir', and returns whether it did, so that the
 * transition that is running is handed them. The store is where the
 * transitions' own properties end up, so the copy this source wrote to the
 * settings itself is left alone.  Graphics thread. */
static bool warp_pl_load_transition_store(struct warp_playlist_source *s, obs_data_t *settings, size_t dir,
					  const char *id)
{
	struct warp_pl_transition *cfg = &s->tr[dir];
	obs_data_t *stored = obs_data_get_obj(settings, warp_pl_keys[dir].store);
	const char *json = stored ? obs_data_get_json(stored) : NULL;
	bool loaded = false;

	pthread_mutex_lock(&s->tr_mutex);

	if (json && (!cfg->store_json || strcmp(cfg->store_json, json) != 0)) {
		obs_data_t *store = obs_data_create_from_json(json);

		if (store) {
			obs_data_release(cfg->store);
			cfg->store = store;

			bfree(cfg->store_json);
			cfg->store_json = bstrdup(json);

			loaded = true;
		}
	} else if (!stored && !cfg->store_loaded && dir == WARP_PL_DIR_FORWARD) {
		/* the flat settings were only ever there for the one transition
		 * the playlist had, which is the one it goes forward with */
		obs_data_t *legacy = warp_pl_legacy_transition_settings(id, settings);

		if (legacy) {
			obs_data_set_obj(cfg->store, id, legacy);
			obs_data_release(legacy);

			loaded = true;
		}
	}

	cfg->store_loaded = true;

	pthread_mutex_unlock(&s->tr_mutex);

	obs_data_release(stored);

	return loaded;
}

/* The settings the transition for 'dir' is created with: the ones it was last
 * configured with, or, for a stinger that has never been configured, a
 * transition point that leaves room for the swap behind it rather than the zero
 * it would otherwise start out at. The caller owns the reference. */
static obs_data_t *warp_pl_transition_settings(struct warp_playlist_source *s, size_t dir, const char *id)
{
	pthread_mutex_lock(&s->tr_mutex);

	obs_data_t *tr_settings = obs_data_get_obj(s->tr[dir].store, id);

	if (!tr_settings && strcmp(id, WARP_PL_TR_STINGER) == 0) {
		tr_settings = obs_data_create();

		obs_data_set_int(tr_settings, "tp_type", WARP_PL_STINGER_TP_TIME);
		obs_data_set_int(tr_settings, "transition_point", WARP_PL_DEFAULT_STINGER_MS);

		obs_data_set_obj(s->tr[dir].store, id, tr_settings);
	}

	pthread_mutex_unlock(&s->tr_mutex);

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

/* Brings the live transition for 'dir' up to date with the settings without
 * swapping it out: the type is the one that is already running, only what it is
 * configured with changed.  Graphics thread. */
static void warp_pl_configure_transition(struct warp_playlist_source *s, size_t dir, obs_data_t *tr_settings,
					 bool layout, uint32_t alignment, enum obs_transition_scale_type scale)
{
	obs_source_t *tr;

	if (!tr_settings && !layout)
		return;

	tr = warp_pl_get_transition_dir(s, dir);

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

/* Drops the transition for 'dir', which is no longer wanted: going back has
 * been put back on the forward transition. Whatever it is showing goes over to
 * the forward one, so the picture does not blink out with it. Graphics
 * thread. */
static void warp_pl_release_transition(struct warp_playlist_source *s, size_t dir)
{
	obs_source_t *into = NULL;
	obs_source_t *old;
	bool was_active;

	pthread_mutex_lock(&s->tr_mutex);

	old = s->tr[dir].source;
	s->tr[dir].source = NULL;
	bfree(s->tr[dir].id);
	s->tr[dir].id = NULL;

	was_active = old && s->active_dir == dir;

	if (was_active)
		into = obs_source_get_ref(s->tr[WARP_PL_DIR_FORWARD].source);

	pthread_mutex_unlock(&s->tr_mutex);

	if (!old)
		return;

	if (into) {
		obs_source_t *showing = obs_transition_get_active_source(old);

		if (showing) {
			obs_transition_set(into, showing);
			obs_source_release(showing);
		}

		obs_source_release(into);
	}

	if (was_active) {
		pthread_mutex_lock(&s->tr_mutex);
		s->active_dir = WARP_PL_DIR_FORWARD;
		pthread_mutex_unlock(&s->tr_mutex);
	}

	obs_transition_clear(old);
	obs_source_remove_active_child(s->source, old);
	obs_source_release(old);
}

/* Creates the transition the settings ask for and moves whatever is on screen
 * over to it, configures the one already running when only its settings
 * changed, or drops it when the direction it belongs to has none of its own any
 * more. Runs from the tick, so it cannot race with another swap. */
static void warp_pl_swap_transition(struct warp_playlist_source *s, size_t dir)
{
	struct warp_pl_transition *cfg = &s->tr[dir];
	enum obs_transition_scale_type scale;
	obs_data_t *tr_settings;
	obs_source_t *carry = NULL;
	obs_source_t *old;
	uint32_t alignment;
	uint32_t cx;
	uint32_t cy;
	bool release;
	bool layout;
	bool active;
	char *id;

	id = cfg->pending_id;
	cfg->pending_id = NULL;
	tr_settings = cfg->pending_settings;
	cfg->pending_settings = NULL;
	layout = cfg->pending_layout;
	cfg->pending_layout = false;
	release = cfg->pending_release;
	cfg->pending_release = false;
	alignment = cfg->alignment;
	scale = cfg->scale;
	cx = s->cx;
	cy = s->cy;

	if (release) {
		obs_data_release(tr_settings);
		bfree(id);
		warp_pl_release_transition(s, dir);
		return;
	}

	if (!id) {
		warp_pl_configure_transition(s, dir, tr_settings, layout, alignment, scale);
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

	active = s->active_dir == dir;
	if (active && s->current && !s->showing_nothing)
		carry = obs_source_get_ref(s->current);

	/* carry whatever is on screen over, before anything can render the
	 * new transition and find it empty; the one that is not on screen has
	 * nothing to carry, and is handed the picture when it is switched to */
	if (carry) {
		obs_transition_set(tr, carry);
		obs_source_release(carry);
	}

	pthread_mutex_lock(&s->tr_mutex);
	old = cfg->source;
	cfg->source = tr;
	bfree(cfg->id);
	cfg->id = id;
	pthread_mutex_unlock(&s->tr_mutex);

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
	obs_data_set_default_bool(settings, "separate_back_transition", false);
	obs_data_set_default_string(settings, "back_transition", WARP_PL_DEFAULT_TRANSITION);
	obs_data_set_default_int(settings, "back_transition_duration", WARP_PL_DEFAULT_TRANSITION_MS);
	obs_data_set_default_string(settings, "back_transition_scale", WARP_PL_SCALE_FIT);
	obs_data_set_default_string(settings, "back_transition_alignment", "center");
	obs_data_set_default_int(settings, "speed_percent", 100);
	obs_data_set_default_int(settings, "volume_percent", 100);
	obs_data_set_default_int(settings, "transition_volume_percent", 100);
	obs_data_set_default_bool(settings, "restart_on_activate", true);
	obs_data_set_default_bool(settings, "clear_on_media_end", true);
	obs_data_set_default_bool(settings, "preserve_pitch", true);

	/* the framing itself is not saved with the playlist: it belongs to the
	 * file that is playing, and the next file starts from the top */
	warp_zoom_control_defaults(settings, false);
}

static const char *media_filter =
	" (*.mp4 *.m4v *.ts *.mov *.mxf *.flv *.mkv *.avi *.mp3 *.ogg *.aac *.wav *.gif *.webm);;";
static const char *video_filter = " (*.mp4 *.m4v *.ts *.mov *.mxf *.flv *.mkv *.avi *.gif *.webm);;";
static const char *audio_filter = " (*.mp3 *.aac *.ogg *.wav);;";

/* Only what the playlist puts around the transition is shown here; what the
 * transition itself is configured with belongs to the transition. */
/* Shows the level a transition's own sound is played at, which only a stinger
 * has: everything else is nothing but the two files it is holding, and there is
 * no second level for it to set. Going back is only asked about when it has a
 * transition of its own; without one it runs through the forward transition
 * like everything else. */
static void warp_pl_show_transition_volume(obs_properties_t *props, obs_data_t *settings)
{
	obs_property_t *prop = obs_properties_get(props, "transition_volume_percent");
	bool stinger =
		strcmp(obs_data_get_string(settings, warp_pl_keys[WARP_PL_DIR_FORWARD].id), WARP_PL_TR_STINGER) == 0;

	if (!stinger && obs_data_get_bool(settings, "separate_back_transition"))
		stinger = strcmp(obs_data_get_string(settings, warp_pl_keys[WARP_PL_DIR_BACKWARD].id),
				 WARP_PL_TR_STINGER) == 0;

	if (prop)
		obs_property_set_visible(prop, stinger);
}

static bool warp_pl_transition_changed_dir(obs_properties_t *props, obs_data_t *settings, size_t dir)
{
	const char *id = obs_data_get_string(settings, warp_pl_keys[dir].id);
	bool is_cut = strcmp(id, WARP_PL_TR_CUT) == 0;
	bool is_stinger = strcmp(id, WARP_PL_TR_STINGER) == 0;

	/* a cut is instant, and a stinger runs for as long as its video */
	obs_property_set_visible(obs_properties_get(props, warp_pl_keys[dir].duration), !is_cut && !is_stinger);

	if (warp_pl_keys[dir].timing)
		obs_property_set_visible(obs_properties_get(props, warp_pl_keys[dir].timing), !is_cut);

	warp_pl_show_transition_volume(props, settings);

	return true;
}

static bool warp_pl_transition_changed(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);

	return warp_pl_transition_changed_dir(props, settings, WARP_PL_DIR_FORWARD);
}

static bool warp_pl_back_transition_changed(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);

	return warp_pl_transition_changed_dir(props, settings, WARP_PL_DIR_BACKWARD);
}

/* Shows the second section, the one going back through the playlist is
 * configured in, and says as much in the heading of the first: with a
 * transition of its own for going back, the transition the playlist has always
 * had is the one it goes forward with. */
static bool warp_pl_separate_back_changed(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	bool separate = obs_data_get_bool(settings, "separate_back_transition");
	obs_property_t *forward = obs_properties_get(props, "transition_group");

	UNUSED_PARAMETER(prop);

	if (forward)
		obs_property_set_description(forward,
					     obs_module_text(separate ? "Warp.Playlist.Group.Transition.Forward"
								      : "Warp.Playlist.Group.Transition"));

	obs_property_set_visible(obs_properties_get(props, "back_transition_group"), separate);

	/* a stinger for going back is only played when going back has a
	 * transition of its own */
	warp_pl_show_transition_volume(props, settings);

	return true;
}

#ifdef WARP_HAVE_FRONTEND_API
/* Opens the properties of the transition for 'dir', the same window OBS opens
 * for the transitions in its own list, preview and all. What is changed there
 * is applied to the transition on the spot, and read back into the settings
 * this source is saved with. */
static bool warp_pl_open_transition_props(struct warp_playlist_source *s, size_t dir)
{
	obs_source_t *transition;

	/* no source of its own to configure: the properties were asked for by
	 * id rather than for a playlist that exists */
	if (!s)
		return false;

	transition = warp_pl_get_transition_dir(s, dir);

	if (!transition)
		return false;

	obs_frontend_open_source_properties(transition);
	obs_source_release(transition);

	return false;
}

static bool warp_pl_transition_props_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);

	return warp_pl_open_transition_props(data, WARP_PL_DIR_FORWARD);
}

static bool warp_pl_back_transition_props_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);

	return warp_pl_open_transition_props(data, WARP_PL_DIR_BACKWARD);
}
#endif

static obs_properties_t *warp_pl_transition_properties(struct warp_playlist_source *s, size_t dir)
{
	bool back = dir == WARP_PL_DIR_BACKWARD;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *prop;
	const char *id;

	prop = obs_properties_add_list(props, warp_pl_keys[dir].id,
				       obs_module_text(back ? "Warp.Playlist.Transition.Back"
							    : "Warp.Playlist.Transition"),
				       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	for (size_t i = 0; obs_enum_transition_types(i, &id); i++) {
		const char *name = obs_source_get_display_name(id);

		obs_property_list_add_string(prop, name ? name : id, id);
	}

	obs_property_set_modified_callback(prop, back ? warp_pl_back_transition_changed : warp_pl_transition_changed);

#ifdef WARP_HAVE_FRONTEND_API
	prop = obs_properties_add_button2(
		props, warp_pl_keys[dir].properties, obs_module_text("Warp.Playlist.Transition.Properties"),
		back ? warp_pl_back_transition_props_clicked : warp_pl_transition_props_clicked, s);
	obs_property_set_long_description(prop, obs_module_text(back ? "Warp.Playlist.Transition.Back.Properties.Desc"
								     : "Warp.Playlist.Transition.Properties.Desc"));
#else
	UNUSED_PARAMETER(s);
#endif

	prop = obs_properties_add_int_slider(props, warp_pl_keys[dir].duration,
					     obs_module_text("Warp.Playlist.TransitionDuration"), 0, 10000, 50);
	obs_property_int_set_suffix(prop, " ms");

	/* Where the transition sits against the end of the file only applies to
	 * automatic advance, which only ever moves forward: a move back through
	 * the playlist is asked for, and runs from the moment it is. */
	if (warp_pl_keys[dir].timing) {
		prop = obs_properties_add_list(props, warp_pl_keys[dir].timing,
					       obs_module_text("Warp.Playlist.TransitionTiming"), OBS_COMBO_TYPE_LIST,
					       OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(prop, obs_module_text("Warp.Playlist.TransitionTiming.Overlap"),
					     WARP_PL_TIMING_OVERLAP);
		obs_property_list_add_string(prop, obs_module_text("Warp.Playlist.TransitionTiming.After"),
					     WARP_PL_TIMING_AFTER);
		obs_property_set_long_description(prop, obs_module_text("Warp.Playlist.TransitionTiming.Desc"));
	}

	prop = obs_properties_add_list(props, warp_pl_keys[dir].scale,
				       obs_module_text("Warp.Playlist.Transition.Scale"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, obs_module_text("Warp.Playlist.Transition.Scale.Fit"), WARP_PL_SCALE_FIT);
	obs_property_list_add_string(prop, obs_module_text("Warp.Playlist.Transition.Scale.Stretch"),
				     WARP_PL_SCALE_STRETCH);
	obs_property_list_add_string(prop, obs_module_text("Warp.Playlist.Transition.Scale.DownOnly"),
				     WARP_PL_SCALE_DOWN_ONLY);
	obs_property_set_long_description(prop, obs_module_text("Warp.Playlist.Transition.Scale.Desc"));

	prop = obs_properties_add_list(props, warp_pl_keys[dir].alignment,
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

/* drawing off the GPU is something hardware decoding does */
static bool warp_pl_hw_decode_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);
	obs_property_set_visible(obs_properties_get(props, "gpu_direct"), obs_data_get_bool(settings, "hw_decode"));
	return true;
}

static obs_properties_t *warp_playlist_getproperties(void *data)
{
	struct warp_playlist_source *s = data;
	struct dstr filter = {0};
	struct dstr path = {0};
	obs_property_t *prop;

	obs_properties_t *props = obs_properties_create();
	obs_properties_t *playback;

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
		pthread_mutex_lock(&s->snap_mutex);
		if (s->snap.paths.num) {
			const char *slash;

			dstr_copy(&path, s->snap.paths.array[s->snap.paths.num - 1]);
			dstr_replace(&path, "\\", "/");
			slash = strrchr(path.array, '/');
			if (slash)
				dstr_resize(&path, slash - path.array + 1);
		}
		pthread_mutex_unlock(&s->snap_mutex);
	}

	/* Speed and the two levels ride at the top, in a group of their own:
	 * they are what an operator reaches for while a clip is up, and
	 * everything below them is set once when the source is built. */
	playback = obs_properties_create();

	prop = obs_properties_add_int_slider(playback, "speed_percent", obs_module_text("Warp.Video.Speed"),
					     MP_SPEED_MIN, MP_SPEED_MAX, 1);
	obs_property_int_set_suffix(prop, "%");
	obs_property_set_long_description(prop, obs_module_text("Warp.Playlist.Speed.Desc"));

	prop = obs_properties_add_int_slider(playback, "volume_percent", obs_module_text("Warp.Playlist.Volume"),
					     WARP_PL_VOLUME_MIN, WARP_PL_VOLUME_MAX, 1);
	obs_property_int_set_suffix(prop, "%");
	obs_property_set_long_description(prop, obs_module_text("Warp.Playlist.Volume.Desc"));

	prop = obs_properties_add_int_slider(playback, "transition_volume_percent",
					     obs_module_text("Warp.Playlist.TransitionVolume"), WARP_PL_VOLUME_MIN,
					     WARP_PL_VOLUME_MAX, 1);
	obs_property_int_set_suffix(prop, "%");
	obs_property_set_long_description(prop, obs_module_text("Warp.Playlist.TransitionVolume.Desc"));

	obs_properties_add_group(props, "playback_group", obs_module_text("Warp.Playlist.Group.Playback"),
				 OBS_GROUP_NORMAL, playback);

	/* The three switches that say what the playlist does around the edges of
	 * a show - how it starts, how it ends, and what decodes it - stand above
	 * the file list rather than at the foot of the window. They are settings
	 * an operator reaches for between clips, and hunting for them past every
	 * transition setting is not something to be doing with a show running. */
	obs_properties_add_bool(props, "restart_on_activate", obs_module_text("Warp.Playlist.RestartOnActivate"));

	obs_properties_add_bool(props, "clear_on_media_end", obs_module_text("Warp.Playlist.ClearOnEnd"));

	obs_property_t *hw = obs_properties_add_bool(props, "hw_decode", obs_module_text("Warp.Video.HardwareDecode"));
	obs_property_set_modified_callback(hw, warp_pl_hw_decode_modified);

	obs_property_t *direct = obs_properties_add_bool(props, "gpu_direct", obs_module_text("Warp.Video.GpuDirect"));
	obs_property_set_long_description(direct, obs_module_text("Warp.Video.GpuDirect.Desc"));

	obs_property_t *pitch =
		obs_properties_add_bool(props, "preserve_pitch", obs_module_text("Warp.Media.PreservePitch"));
	obs_property_set_long_description(pitch, obs_module_text("Warp.Media.PreservePitch.Desc"));

	obs_properties_add_editable_list(props, "playlist", obs_module_text("Warp.Playlist.Files"),
					 OBS_EDITABLE_LIST_TYPE_FILES, filter.array, path.array);
	dstr_free(&filter);
	dstr_free(&path);

	obs_properties_add_bool(props, "shuffle", obs_module_text("Warp.Playlist.Shuffle"));

	prop = obs_properties_add_bool(props, "auto_advance", obs_module_text("Warp.Playlist.AutoAdvance"));
	obs_property_set_long_description(prop, obs_module_text("Warp.Playlist.AutoAdvance.Desc"));

	obs_properties_add_bool(props, "loop_playlist", obs_module_text("Warp.Playlist.Loop"));

	/* Asked before either section rather than between them: whether going
	 * back has a transition of its own is what says how many sections there
	 * are to read, so it is answered on the way in. Sat between them it read
	 * as a footnote to the first, which is not where anybody looking for it
	 * thinks to look. */
	prop = obs_properties_add_bool(props, "separate_back_transition",
				       obs_module_text("Warp.Playlist.SeparateBackTransition"));
	obs_property_set_long_description(prop, obs_module_text("Warp.Playlist.SeparateBackTransition.Desc"));
	obs_property_set_modified_callback(prop, warp_pl_separate_back_changed);

	obs_properties_add_group(props, "transition_group", obs_module_text("Warp.Playlist.Group.Transition"),
				 OBS_GROUP_NORMAL, warp_pl_transition_properties(s, WARP_PL_DIR_FORWARD));

	obs_properties_add_group(props, "back_transition_group", obs_module_text("Warp.Playlist.Group.Transition.Back"),
				 OBS_GROUP_NORMAL, warp_pl_transition_properties(s, WARP_PL_DIR_BACKWARD));

	warp_zoom_control_properties(props, false);

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
	s->snap_paths_dirty = true;

	return true;
}

static void warp_pl_free_clips(struct warp_playlist_source *s)
{
	for (size_t i = 0; i < s->clips.num; i++)
		bfree(s->clips.array[i].path);
	da_resize(s->clips, 0);
}

/* Reads what each file is cut to out of the settings. Returns true when that
 * differs from what was read before.  Graphics thread. */
static bool warp_pl_load_clips(struct warp_playlist_source *s, obs_data_t *settings)
{
	obs_data_array_t *array = obs_data_get_array(settings, WARP_PL_CLIPS_KEY);
	size_t count = array ? obs_data_array_count(array) : 0;
	bool changed = count != s->clips.num;

	DARRAY(struct warp_pl_clip) clips;
	da_init(clips);

	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(array, i);
		const char *path = obs_data_get_string(item, WARP_PL_CLIP_PATH);
		long long speed = obs_data_get_int(item, WARP_PL_CLIP_SPEED);

		if (path && *path) {
			struct warp_pl_clip clip = {
				.path = bstrdup(path),
				.in_ms = obs_data_get_int(item, WARP_PL_CLIP_IN_MS),
				.out_ms = obs_data_get_int(item, WARP_PL_CLIP_OUT_MS),
				.tail_ms = obs_data_get_int(item, WARP_PL_CLIP_TAIL_MS),
				.speed = speed >= MP_SPEED_MIN && speed <= MP_SPEED_MAX ? (int)speed : 0,
			};
			const struct warp_pl_clip *was = warp_pl_find_clip(s, path);

			if (!was || was->in_ms != clip.in_ms || was->out_ms != clip.out_ms ||
			    was->tail_ms != clip.tail_ms || was->speed != clip.speed)
				changed = true;

			da_push_back(clips, &clip);
		}

		obs_data_release(item);
	}

	obs_data_array_release(array);

	warp_pl_free_clips(s);
	da_free(s->clips);
	s->clips.array = clips.array;
	s->clips.num = clips.num;
	s->clips.capacity = clips.capacity;

	return changed;
}

/* The transition of 'dir' as the settings ask for it. The tick picks the type
 * and the settings up; everything the playlist itself needs to know is applied
 * on the spot.  Graphics thread. */
static void warp_pl_apply_transition(struct warp_playlist_source *s, size_t dir, const char *id, uint32_t ms,
				     bool overlap, uint32_t alignment, enum obs_transition_scale_type scale,
				     bool store_loaded)
{
	struct warp_pl_transition *cfg = &s->tr[dir];

	cfg->ms = ms;
	cfg->is_cut = strcmp(id, WARP_PL_TR_CUT) == 0;
	cfg->is_stinger = strcmp(id, WARP_PL_TR_STINGER) == 0;
	cfg->overlap = overlap;
	/* it is wanted after all */
	cfg->pending_release = false;

	/* The tick creates the transition when the type changed and hands
	 * whichever one ends up running its settings. What the settings ask for
	 * is compared against the transition that is queued for the next tick
	 * when there is one, so that picking a transition and going back to the
	 * one that is already running before the tick comes around leaves it
	 * alone rather than swapping in the one in between. */
	const char *wanted = cfg->pending_id ? cfg->pending_id : cfg->id;
	bool type_changed = !wanted || strcmp(wanted, id) != 0;

	if (type_changed) {
		bfree(cfg->pending_id);
		cfg->pending_id = NULL;

		/* nothing to swap when it is the one already running */
		if (!cfg->id || strcmp(cfg->id, id) != 0)
			cfg->pending_id = bstrdup(id);
	}

	/* What the transition is configured with only has to be handed over
	 * when a transition is about to be created, or when settings were just
	 * loaded: everything else that configures one goes through the
	 * transition's own properties, which apply to it directly. A stinger
	 * reloads its video every time it is updated, which is not something an
	 * unrelated property being edited should set off. */
	obs_data_t *tr_settings = warp_pl_transition_settings(s, dir, id);

	cfg->stinger_point_ms = cfg->is_stinger ? warp_pl_stinger_point_ms(tr_settings) : 0;
	cfg->stinger_cross_fade = cfg->is_stinger &&
				  obs_data_get_int(tr_settings, "audio_fade_style") == WARP_PL_STINGER_FADE_CROSS;

	if (type_changed || store_loaded) {
		obs_data_release(cfg->pending_settings);
		cfg->pending_settings = warp_pl_data_copy(tr_settings);
	}

	obs_data_release(tr_settings);

	if (cfg->alignment != alignment || cfg->scale != scale)
		cfg->pending_layout = true;

	cfg->alignment = alignment;
	cfg->scale = scale;
}

/* Gives up the transition of 'dir': the direction runs through the forward one
 * from here on, and the tick drops the live transition. What it was configured
 * with is kept, so turning it back on picks it up again. Graphics thread. */
static void warp_pl_drop_transition(struct warp_playlist_source *s, size_t dir)
{
	struct warp_pl_transition *cfg = &s->tr[dir];

	bfree(cfg->pending_id);
	cfg->pending_id = NULL;

	obs_data_release(cfg->pending_settings);
	cfg->pending_settings = NULL;

	cfg->pending_layout = false;

	if (cfg->source)
		cfg->pending_release = true;
}

static void warp_playlist_update(void *data, obs_data_t *settings)
{
	struct warp_playlist_source *s = data;

	bool active = obs_source_active(s->source);
	bool separate_back = obs_data_get_bool(settings, "separate_back_transition");
	int speed = (int)obs_data_get_int(settings, "speed_percent");
	int volume = (int)obs_data_get_int(settings, "volume_percent");
	int transition_volume = (int)obs_data_get_int(settings, "transition_volume_percent");

	if (speed < MP_SPEED_MIN || speed > MP_SPEED_MAX)
		speed = 100;

	if (volume < WARP_PL_VOLUME_MIN)
		volume = WARP_PL_VOLUME_MIN;
	else if (volume > WARP_PL_VOLUME_MAX)
		volume = WARP_PL_VOLUME_MAX;

	if (transition_volume < WARP_PL_VOLUME_MIN)
		transition_volume = WARP_PL_VOLUME_MIN;
	else if (transition_volume > WARP_PL_VOLUME_MAX)
		transition_volume = WARP_PL_VOLUME_MAX;

	/* the transitions' own properties write to the transitions rather than
	 * to these settings, so what they are running with is read back before
	 * the settings they were saved with are looked at */
	warp_pl_capture_transitions(s);

	/* the zoom guards itself, and registers hotkeys as its presets change,
	 * so it is brought up to date with the playlist's lock dropped */
	warp_zoom_control_update(&s->zoom, settings);

	const char *transition_id[WARP_PL_NUM_DIRS];
	bool store_loaded[WARP_PL_NUM_DIRS];
	uint32_t alignment[WARP_PL_NUM_DIRS];
	enum obs_transition_scale_type scale[WARP_PL_NUM_DIRS];

	for (size_t dir = 0; dir < WARP_PL_NUM_DIRS; dir++) {
		const char *id = obs_data_get_string(settings, warp_pl_keys[dir].id);

		if (!id || !*id)
			id = WARP_PL_DEFAULT_TRANSITION;

		transition_id[dir] = id;
		/* the store of a direction that has no transition of its own is
		 * loaded all the same, so that turning one on brings back what
		 * it was set up with */
		store_loaded[dir] = warp_pl_load_transition_store(s, settings, dir, id);
		alignment[dir] =
			warp_pl_alignment_from_string(obs_data_get_string(settings, warp_pl_keys[dir].alignment));
		scale[dir] = warp_pl_scale_from_string(obs_data_get_string(settings, warp_pl_keys[dir].scale));
	}

	bool overlap = strcmp(obs_data_get_string(settings, "transition_timing"), WARP_PL_TIMING_AFTER) != 0;

	/* The next packet of everything that is playing rides to the new
	 * level, so this applies to the file that is up as well as to the ones
	 * after it. */
	pthread_mutex_lock(&s->audio.mutex);
	s->audio.volume = (float)volume / 100.0f;
	s->audio.transition_volume = (float)transition_volume / 100.0f;
	pthread_mutex_unlock(&s->audio.mutex);

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
	s->separate_back_transition = separate_back;
	s->restart_on_activate = obs_data_get_bool(settings, "restart_on_activate");
	s->clear_on_media_end = obs_data_get_bool(settings, "clear_on_media_end");
	bool hw_decode = obs_data_get_bool(settings, "hw_decode");
	bool gpu_direct = hw_decode && obs_data_get_bool(settings, "gpu_direct");

	/* kept items were made to decode the other way */
	if (hw_decode != s->hw_decode || gpu_direct != s->gpu_direct)
		warp_pl_flush_idle(s);

	s->hw_decode = hw_decode;
	s->gpu_direct = gpu_direct;
	s->base_speed = speed;

	/* every item the playlist is holding plays at the new setting, the
	 * one on screen included */
	bool preserve_pitch = obs_data_get_bool(settings, "preserve_pitch");

	if (preserve_pitch != s->preserve_pitch) {
		obs_source_t *items[] = {s->current, s->prev, s->pending, s->preloaded, s->back};

		s->preserve_pitch = preserve_pitch;

		for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++)
			warp_pl_call_item_bool(items[i], "warp_set_preserve_pitch", "preserve", preserve_pitch);
		for (size_t i = 0; i < s->idle.num; i++)
			warp_pl_call_item_bool(s->idle.array[i], "warp_set_preserve_pitch", "preserve", preserve_pitch);
	}

	size_t prev_count = s->paths.num;

	if (warp_pl_load_playlist(s, settings))
		order_changed = true;

	/* the files that were opened ahead were cut the way they were */
	if (warp_pl_load_clips(s, settings)) {
		warp_pl_drop_preloaded(s);
		warp_pl_drop_back(s);
		s->preload_requested = false;
	}

	if (order_changed) {
		warp_pl_build_order(s, SIZE_MAX);
		warp_pl_drop_preloaded(s);
		warp_pl_drop_pending(s);
		warp_pl_drop_back(s);

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

		/* A playlist that has run out, or that is not on air, is not in
		 * the middle of anything, so files added to it send it back to
		 * the top rather than leaving it wherever it had got to. That is
		 * the case of one being built up from empty: the first file
		 * starts playing as it lands, and by the time the rest are in it
		 * has run on, or run out, partway down the list. */
		if (s->pos < s->order.num && s->paths.num > prev_count &&
		    (s->state == OBS_MEDIA_STATE_ENDED || !active))
			warp_pl_stop(s);
	}

	for (size_t dir = 0; dir < WARP_PL_NUM_DIRS; dir++) {
		if (dir == WARP_PL_DIR_BACKWARD && !separate_back) {
			warp_pl_drop_transition(s, dir);
			continue;
		}

		warp_pl_apply_transition(s, dir, transition_id[dir],
					 (uint32_t)obs_data_get_int(settings, warp_pl_keys[dir].duration),
					 dir == WARP_PL_DIR_FORWARD ? overlap : true, alignment[dir], scale[dir],
					 store_loaded[dir]);
	}

	/* the property applies to the file that is playing as well as to the
	 * ones that follow it */
	s->speed = warp_pl_start_speed(s, s->current_path);
	warp_pl_call_item_proc(s->current, "warp_set_speed", "speed", s->speed);

	/* start at the top when nothing is playing: either the source was just
	 * created, or the file that was playing is no longer in the list */
	if (s->order.num && s->pos >= s->order.num && !s->pending && (!s->restart_on_activate || active))
		warp_pl_play_pos(s, 0, false, WARP_PL_DIR_FORWARD);

	int speed_now = s->speed;

	warp_pl_flush(s);

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

	/* the transitions' own properties write to the transitions, so what
	 * they are running with is read back before they are written out */
	warp_pl_capture_transitions(s);

	/* the zoom presets are edited in the dock and the Warp window rather
	 * than through the properties, so they are written out from the control
	 * the same way */
	warp_zoom_control_save(&s->zoom, settings);

	for (size_t dir = 0; dir < WARP_PL_NUM_DIRS; dir++) {
		obs_data_t *store = obs_data_create();

		pthread_mutex_lock(&s->tr_mutex);

		obs_data_apply(store, s->tr[dir].store);

		/* what the settings are left holding, so that the next update
		 * tells settings being loaded from the copy written here */
		bfree(s->tr[dir].store_json);
		s->tr[dir].store_json = bstrdup(obs_data_get_json(store));

		pthread_mutex_unlock(&s->tr_mutex);

		obs_data_set_obj(settings, warp_pl_keys[dir].store, store);
		obs_data_release(store);
	}
}

/* ------------------------------------------------------------------------- */
/* commands and the snapshot */

/* Asks the graphics thread to do something on the next tick; 'path' is taken
 * over. Any thread. */
static void warp_pl_post(struct warp_playlist_source *s, enum warp_pl_cmd_type type, int value, const char *change,
			 char *path)
{
	struct warp_pl_cmd cmd = {.type = type, .value = value, .change = change, .path = path};

	pthread_mutex_lock(&s->cmd_mutex);
	da_push_back(s->cmds, &cmd);
	pthread_mutex_unlock(&s->cmd_mutex);
}

/* Publishes where the playlist stands for the other threads. Graphics thread. */
static void warp_pl_publish(struct warp_playlist_source *s)
{
	const char *path = s->pos < s->order.num ? warp_pl_path_at(s, s->pos) : NULL;
	obs_source_t *release = NULL;

	pthread_mutex_lock(&s->snap_mutex);

	s->snap.state = s->state;
	s->snap.speed = s->speed;
	s->snap.index = s->pos < s->order.num ? (long long)s->pos : -1;
	s->snap.count = s->order.num;
	s->snap.queued = s->queue.num;

	if (!path != !s->snap.current_file || (path && strcmp(path, s->snap.current_file) != 0)) {
		bfree(s->snap.current_file);
		s->snap.current_file = path ? bstrdup(path) : NULL;
	}

	if (s->snap.current != s->current) {
		release = s->snap.current;
		s->snap.current = obs_source_get_ref(s->current);
	}

	if (s->snap_paths_dirty) {
		for (size_t i = 0; i < s->snap.paths.num; i++)
			bfree(s->snap.paths.array[i]);
		da_resize(s->snap.paths, 0);

		for (size_t i = 0; i < s->paths.num; i++) {
			char *copy = bstrdup(s->paths.array[i]);
			da_push_back(s->snap.paths, &copy);
		}

		s->snap_paths_dirty = false;
	}

	pthread_mutex_unlock(&s->snap_mutex);

	obs_source_release(release);
}

/* the item playing, as last published, with a reference held; any thread */
static obs_source_t *warp_pl_snap_current(struct warp_playlist_source *s)
{
	pthread_mutex_lock(&s->snap_mutex);
	obs_source_t *current = obs_source_get_ref(s->snap.current);
	pthread_mutex_unlock(&s->snap_mutex);

	return current;
}

static void warp_pl_restart_current_command(struct warp_playlist_source *s);
static void warp_pl_clear_playlist(struct warp_playlist_source *s);
static void warp_pl_change_speed(struct warp_playlist_source *s, int value, bool absolute, const char *change);
static void warp_pl_step_frames(struct warp_playlist_source *s, int frames);

/* Carries out what has been posted since the last tick, in the order it was
 * posted. Graphics thread. */
static void warp_pl_run_commands(struct warp_playlist_source *s)
{
	for (;;) {
		struct warp_pl_cmd cmd;

		pthread_mutex_lock(&s->cmd_mutex);
		if (!s->cmds.num) {
			pthread_mutex_unlock(&s->cmd_mutex);
			return;
		}
		cmd = s->cmds.array[0];
		da_erase(s->cmds, 0);
		pthread_mutex_unlock(&s->cmd_mutex);

		switch (cmd.type) {
		case WARP_PL_CMD_NEXT:
			warp_pl_advance(s, 1);
			warp_pl_flush(s);
			break;
		case WARP_PL_CMD_PREVIOUS:
			warp_pl_advance(s, -1);
			warp_pl_flush(s);
			break;
		case WARP_PL_CMD_FIRST:
			warp_pl_restart_playlist(s);
			warp_pl_flush(s);
			break;
		case WARP_PL_CMD_RESTART_CURRENT:
			warp_pl_restart_current_command(s);
			break;
		case WARP_PL_CMD_CLEAR:
			warp_pl_clear_playlist(s);
			break;
		case WARP_PL_CMD_SET_SPEED:
			warp_pl_change_speed(s, cmd.value, true, cmd.change);
			break;
		case WARP_PL_CMD_ADJUST_SPEED:
			warp_pl_change_speed(s, cmd.value, false, cmd.change);
			break;
		case WARP_PL_CMD_STEP:
			warp_pl_step_frames(s, cmd.value);
			break;
		case WARP_PL_CMD_QUEUE:
			da_push_back(s->queue, &cmd.path);
			WARP_PL_LOG(LOG_INFO, "'%s' plays next (%d queued)", cmd.path, (int)s->queue.num);
			cmd.path = NULL;

			/* what was opened ahead is not next any more */
			warp_pl_drop_preloaded(s);
			s->preload_requested = false;
			warp_pl_flush(s);
			break;
		case WARP_PL_CMD_CLEAR_QUEUE:
			for (size_t i = 0; i < s->queue.num; i++)
				bfree(s->queue.array[i]);
			da_resize(s->queue, 0);

			/* the playlist carries on from the file that is up */
			s->resume_pos = SIZE_MAX;

			warp_pl_drop_preloaded(s);
			s->preload_requested = false;
			warp_pl_flush(s);
			break;
		}

		bfree(cmd.path);
	}
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

	warp_pl_post(s, WARP_PL_CMD_NEXT, 0, NULL, NULL);
}

static void warp_playlist_prev_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct warp_playlist_source *s = data;

	if (!pressed || !obs_source_showing(s->source))
		return;

	warp_pl_post(s, WARP_PL_CMD_PREVIOUS, 0, NULL, NULL);
}

static void warp_playlist_first_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct warp_playlist_source *s = data;

	if (!pressed || !obs_source_showing(s->source))
		return;

	warp_pl_post(s, WARP_PL_CMD_FIRST, 0, NULL, NULL);
}

/* Restarts the file that is playing, the way Restart Current Video does.
 * Graphics thread. */
static void warp_pl_restart_current_command(struct warp_playlist_source *s)
{
	warp_pl_restart_current(s);
	bool restarted = s->current || s->pending || s->play_armed;
	warp_pl_flush(s);

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

	warp_pl_post(s, WARP_PL_CMD_RESTART_CURRENT, 0, NULL, NULL);
}

/* empties the source's file list for good; graphics thread */
static void warp_pl_clear_playlist(struct warp_playlist_source *s)
{
	if (!s->paths.num)
		return;

	/* clearing empties the source's file list for good, so write what was
	 * in it to the log: a playlist cleared by a mis-hit mid-show can then
	 * be rebuilt from there */
	WARP_PL_LOG(LOG_INFO, "clearing playlist (%d files)", (int)s->paths.num);
	for (size_t i = 0; i < s->paths.num; i++)
		WARP_PL_LOG(LOG_INFO, "  cleared: %s", s->paths.array[i]);

	/* and say so where it will be seen, since the press that did it may
	 * well not have been meant */
	warp_status_post(WARP_STATUS_LONG_MS, obs_module_text("Warp.Status.Cleared"), obs_source_get_name(s->source));

	warp_pl_stop(s);
	warp_pl_flush(s);

	obs_data_t *settings = obs_source_get_settings(s->source);
	obs_data_array_t *empty = obs_data_array_create();

	obs_data_set_array(settings, "playlist", empty);
	obs_data_array_release(empty);
	obs_source_update(s->source, settings);
	obs_data_release(settings);
}

/* Clearing is the one hotkey that is not gated on the source being shown. The
 * others drive playback, so they are aimed at whatever the operator has in
 * front of them; this one edits the source's file list, which is a settings
 * change that stands whether or not the playlist happens to be on screen. A
 * playlist is most often emptied while it is off air — between rounds, or
 * before the next thing is queued into it — so gating it there left the press
 * doing nothing at all. */
static void warp_playlist_clear_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct warp_playlist_source *s = data;

	if (!pressed)
		return;

	warp_pl_post(s, WARP_PL_CMD_CLEAR, 0, NULL, NULL);
}

/* Changes the speed of the file that is playing and reports what it did.
 * 'value' is the speed to play at when 'absolute' is set, and the number of
 * points to move by otherwise. Graphics thread. */
static void warp_pl_change_speed(struct warp_playlist_source *s, int value, bool absolute, const char *change)
{
	int prev_speed = s->speed;

	warp_pl_apply_speed(s, absolute ? value : s->speed + value);
	warp_pl_flush(s);

	if (s->speed != prev_speed)
		warp_signal_speed_changed(s->source, s->speed, prev_speed, change);
}

static void warp_playlist_speed_up_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct warp_playlist_source *s = data;

	if (!pressed || !obs_source_showing(s->source))
		return;

	warp_pl_post(s, WARP_PL_CMD_ADJUST_SPEED, WARP_SPEED_STEP, WARP_SPEED_CHANGE_INCREASED, NULL);
}

static void warp_playlist_speed_down_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct warp_playlist_source *s = data;

	if (!pressed || !obs_source_showing(s->source))
		return;

	warp_pl_post(s, WARP_PL_CMD_ADJUST_SPEED, -WARP_SPEED_STEP, WARP_SPEED_CHANGE_DECREASED, NULL);
}

static void warp_playlist_speed_reset_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct warp_playlist_source *s = data;

	if (!pressed || !obs_source_showing(s->source))
		return;

	warp_pl_post(s, WARP_PL_CMD_SET_SPEED, 100, WARP_SPEED_CHANGE_SET, NULL);
}

static void warp_playlist_speed_preset_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct warp_pl_hotkey_binding *b = data;
	struct warp_playlist_source *s = b->s;

	if (!pressed || !obs_source_showing(s->source))
		return;

	warp_pl_post(s, WARP_PL_CMD_SET_SPEED, b->value, WARP_SPEED_CHANGE_SET, NULL);
}

/* Steps the file that is playing by 'frames', negative to step backward.
 * Graphics thread. */
static void warp_pl_step_frames(struct warp_playlist_source *s, int frames)
{
	bool stepped = false;

	if (s->current) {
		/* the item pauses itself before stepping; auto advance must not
		 * kick in while the operator is stepping around */
		warp_pl_call_item_proc(s->current, "warp_step_frames", "frames", frames);
		s->state = obs_source_media_get_state(s->current);
		stepped = true;
	}

	warp_pl_flush(s);

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

	warp_pl_post(s, WARP_PL_CMD_STEP, b->value, NULL, NULL);
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
		warp_pl_post(data, WARP_PL_CMD_SET_SPEED, (int)speed, WARP_SPEED_CHANGE_SET, NULL);
}

static void warp_pl_adjust_speed_proc(void *data, calldata_t *cd)
{
	long long delta;

	if (!calldata_get_int(cd, "delta", &delta) || !delta)
		return;

	warp_pl_post(data, WARP_PL_CMD_ADJUST_SPEED, (int)delta,
		     delta > 0 ? WARP_SPEED_CHANGE_INCREASED : WARP_SPEED_CHANGE_DECREASED, NULL);
}

static void warp_pl_get_speed_proc(void *data, calldata_t *cd)
{
	struct warp_playlist_source *s = data;

	pthread_mutex_lock(&s->snap_mutex);
	calldata_set_int(cd, "speed", s->snap.speed);
	pthread_mutex_unlock(&s->snap_mutex);
}

static void warp_pl_step_frames_proc(void *data, calldata_t *cd)
{
	long long frames;

	if (calldata_get_int(cd, "frames", &frames))
		warp_pl_post(data, WARP_PL_CMD_STEP, (int)frames, NULL, NULL);
}

static void warp_pl_first_proc(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	warp_pl_post(data, WARP_PL_CMD_FIRST, 0, NULL, NULL);
}

static void warp_pl_restart_current_proc(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	warp_pl_post(data, WARP_PL_CMD_RESTART_CURRENT, 0, NULL, NULL);
}

static void warp_pl_clear_proc(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	warp_pl_post(data, WARP_PL_CMD_CLEAR, 0, NULL, NULL);
}

/* Puts a file in front of whatever the playlist would play next. The file is
 * named by its path or, with no path, by its place in the file list, counting
 * from 0; it has to be in the list. */
static void warp_pl_queue_proc(void *data, calldata_t *cd)
{
	struct warp_playlist_source *s = data;
	const char *path = NULL;
	long long index = -1;
	bool queued = false;

	calldata_get_string(cd, "path", &path);
	calldata_get_int(cd, "index", &index);

	/* checked against the file list as it was last published, so the
	 * caller is told there and then whether the file is in it */
	char *file = NULL;

	pthread_mutex_lock(&s->snap_mutex);
	if (path && *path) {
		for (size_t i = 0; i < s->snap.paths.num && !file; i++) {
			if (strcmp(s->snap.paths.array[i], path) == 0)
				file = bstrdup(path);
		}
	} else if (index >= 0 && (size_t)index < s->snap.paths.num) {
		file = bstrdup(s->snap.paths.array[index]);
	}
	pthread_mutex_unlock(&s->snap_mutex);

	if (file) {
		warp_pl_post(s, WARP_PL_CMD_QUEUE, 0, NULL, file);
		queued = true;
	} else {
		WARP_PL_LOG(LOG_WARNING, "asked to play a file next that is not in the playlist");
	}

	calldata_set_bool(cd, "queued", queued);
}

static void warp_pl_clear_queue_proc(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	warp_pl_post(data, WARP_PL_CMD_CLEAR_QUEUE, 0, NULL, NULL);
}

/* Sets what a file in the list is played as, through the settings so it is
 * saved with the scene collection; see WARP_PL_CLIPS_KEY. It applies the next
 * time the file is played. */
static void warp_pl_set_clip_proc(void *data, calldata_t *cd)
{
	struct warp_playlist_source *s = data;
	const char *path = NULL;
	long long in_ms = 0, out_ms = 0, tail_ms = 0, speed = 0;

	if (!calldata_get_string(cd, "path", &path) || !path || !*path)
		return;

	calldata_get_int(cd, "in_ms", &in_ms);
	calldata_get_int(cd, "out_ms", &out_ms);
	calldata_get_int(cd, "tail_ms", &tail_ms);
	calldata_get_int(cd, "speed", &speed);

	obs_data_t *settings = obs_source_get_settings(s->source);

	warp_pl_clips_set(settings, path, in_ms > 0 ? in_ms : 0, out_ms > 0 ? out_ms : 0, tail_ms > 0 ? tail_ms : 0,
			  speed >= MP_SPEED_MIN && speed <= MP_SPEED_MAX ? speed : 0);
	obs_source_update(s->source, settings);
	obs_data_release(settings);
}

/* where in the playlist playback is; 'index' is -1 when nothing is playing */
static void warp_pl_status_proc(void *data, calldata_t *cd)
{
	struct warp_playlist_source *s = data;

	pthread_mutex_lock(&s->snap_mutex);
	calldata_set_int(cd, "index", s->snap.index);
	calldata_set_int(cd, "count", (long long)s->snap.count);
	calldata_set_string(cd, "current_file", s->snap.current_file ? s->snap.current_file : "");
	calldata_set_int(cd, "queued", (long long)s->snap.queued);
	pthread_mutex_unlock(&s->snap_mutex);
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
	proc_handler_add(ph, "void warp_playlist_queue(string path, int index, out bool queued)", warp_pl_queue_proc,
			 s);
	proc_handler_add(ph, "void warp_playlist_clear_queue()", warp_pl_clear_queue_proc, s);
	proc_handler_add(ph, "void warp_playlist_set_clip(string path, int in_ms, int out_ms, int tail_ms, int speed)",
			 warp_pl_set_clip_proc, s);
	proc_handler_add(
		ph, "void warp_playlist_status(out int index, out int count, out string current_file, out int queued)",
		warp_pl_status_proc, s);
}

/* ------------------------------------------------------------------------- */

static void *warp_playlist_create(obs_data_t *settings, obs_source_t *source)
{
	static const char *signals[] = {WARP_SIGNAL_DECL_SPEED_CHANGED, WARP_SIGNAL_DECL_FRAMES_STEPPED,
					WARP_SIGNAL_DECL_MEDIA_ACTION,  WARP_SIGNAL_DECL_ZOOM_CHANGED,
					WARP_SIGNAL_DECL_ZOOM_STAGED,   NULL};

	struct warp_playlist_source *s = bzalloc(sizeof(struct warp_playlist_source));

	s->source = source;
	s->state = OBS_MEDIA_STATE_NONE;
	s->base_speed = 100;
	s->audio.volume = 1.0f;
	s->audio.transition_volume = 1.0f;
	/* s->speed is left at zero: the update below fills it in, and a speed
	 * that was never played at is not a change to report */
	s->rand_state = os_gettime_ns() | 1;

	for (size_t dir = 0; dir < WARP_PL_NUM_DIRS; dir++) {
		s->tr[dir].ms = WARP_PL_DEFAULT_TRANSITION_MS;
		s->tr[dir].store = obs_data_create();
		s->tr[dir].overlap = true;
		s->tr[dir].alignment = OBS_ALIGN_CENTER;
		s->tr[dir].scale = OBS_TRANSITION_SCALE_ASPECT;
	}

	if (pthread_mutex_init(&s->audio.mutex, NULL) || pthread_mutex_init(&s->cmd_mutex, NULL) ||
	    pthread_mutex_init(&s->snap_mutex, NULL) || pthread_mutex_init(&s->tr_mutex, NULL)) {
		blog(LOG_ERROR, "[Warp Playlist]: failed to initialize its locks");

		for (size_t dir = 0; dir < WARP_PL_NUM_DIRS; dir++)
			obs_data_release(s->tr[dir].store);

		bfree(s);
		return NULL;
	}

	da_init(s->cmds);
	da_init(s->snap.paths);
	s->snap.index = -1;

	da_init(s->paths);
	da_init(s->order);
	da_init(s->retired);
	da_init(s->idle);
	da_init(s->clips);
	da_init(s->queue);
	s->resume_pos = SIZE_MAX;

	signal_handler_add_array(obs_source_get_signal_handler(source), signals);
	warp_playlist_register_hotkeys(s, source);
	warp_playlist_register_procs(s, source);

	warp_zoom_control_init(&s->zoom, source, "WarpPlaylist", true, false);
	warp_zoom_control_register_procs(&s->zoom, source);

	warp_playlist_update(s, settings);
	return s;
}

static void warp_playlist_destroy(void *data)
{
	struct warp_playlist_source *s = data;
	DARRAY(obs_source_t *) retired;
	obs_source_t *transitions[WARP_PL_NUM_DIRS];
	obs_source_t *target;
	obs_source_t *current;
	obs_source_t *current_zoom;
	obs_source_t *pending;
	obs_source_t *prev;

	warp_zoom_control_free(&s->zoom);

	/* the items stop being tapped before anything lets go of them */
	warp_pl_audio_free(s);

	warp_pl_drop_preloaded(s);
	warp_pl_drop_back(s);
	warp_pl_flush_idle(s);

	bfree(s->current_path);
	bfree(s->prev_path);
	bfree(s->pending_path);
	s->current_path = NULL;
	s->prev_path = NULL;
	s->pending_path = NULL;

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
	current_zoom = s->current_zoom;
	pending = s->pending;
	prev = s->prev;

	s->transition_target = NULL;
	s->current = NULL;
	s->current_zoom = NULL;
	s->pending = NULL;
	s->prev = NULL;

	for (size_t i = 0; i < s->paths.num; i++)
		bfree(s->paths.array[i]);
	da_free(s->paths);
	da_free(s->order);
	da_free(s->idle);
	warp_pl_free_clips(s);
	da_free(s->clips);
	for (size_t i = 0; i < s->queue.num; i++)
		bfree(s->queue.array[i]);
	da_free(s->queue);

	for (size_t dir = 0; dir < WARP_PL_NUM_DIRS; dir++) {
		transitions[dir] = s->tr[dir].source;
		s->tr[dir].source = NULL;

		bfree(s->tr[dir].id);
		s->tr[dir].id = NULL;
		bfree(s->tr[dir].pending_id);
		s->tr[dir].pending_id = NULL;

		obs_data_release(s->tr[dir].pending_settings);
		s->tr[dir].pending_settings = NULL;
		obs_data_release(s->tr[dir].store);
		s->tr[dir].store = NULL;

		bfree(s->tr[dir].store_json);
		s->tr[dir].store_json = NULL;
	}

	for (size_t i = 0; i < retired.num; i++)
		obs_source_release(retired.array[i]);
	da_free(retired);

	if (target)
		obs_source_release(target);
	if (current)
		obs_source_release(current);
	if (current_zoom)
		obs_source_release(current_zoom);
	if (pending)
		obs_source_release(pending);
	if (prev)
		obs_source_release(prev);

	for (size_t dir = 0; dir < WARP_PL_NUM_DIRS; dir++) {
		if (!transitions[dir])
			continue;

		obs_transition_clear(transitions[dir]);
		obs_source_remove_active_child(s->source, transitions[dir]);
		obs_source_release(transitions[dir]);
	}

	/* what was posted and never carried out, and what was published */
	for (size_t i = 0; i < s->cmds.num; i++)
		bfree(s->cmds.array[i].path);
	da_free(s->cmds);

	obs_source_release(s->snap.current);
	bfree(s->snap.current_file);
	for (size_t i = 0; i < s->snap.paths.num; i++)
		bfree(s->snap.paths.array[i]);
	da_free(s->snap.paths);

	pthread_mutex_destroy(&s->audio.mutex);
	pthread_mutex_destroy(&s->cmd_mutex);
	pthread_mutex_destroy(&s->snap_mutex);
	pthread_mutex_destroy(&s->tr_mutex);
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

static void warp_playlist_enum_active_sources(void *data, obs_source_enum_proc_t cb, void *param)
{
	struct warp_playlist_source *s = data;

	for (size_t dir = 0; dir < WARP_PL_NUM_DIRS; dir++) {
		obs_source_t *transition = warp_pl_get_transition_dir(s, dir);

		if (transition) {
			cb(s->source, transition, param);
			obs_source_release(transition);
		}
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
 * item rather than overlap it. Automatic advance is the only switch placed
 * against the end of a file, and it only ever moves forward, so this is the
 * forward transition's.  Graphics thread. */
static uint32_t warp_pl_transition_lead_ms(struct warp_playlist_source *s)
{
	const struct warp_pl_transition *cfg = &s->tr[WARP_PL_DIR_FORWARD];
	uint32_t lead = 0;

	if (cfg->overlap && !cfg->is_cut)
		lead = cfg->is_stinger ? cfg->stinger_point_ms : cfg->ms;

	return lead > WARP_PL_END_GUARD_MS ? lead : WARP_PL_END_GUARD_MS;
}

/* Asks for the item after the current one to be opened. The file is opened by
 * warp_pl_open_preload() when the change is flushed. Graphics thread. */
static void warp_pl_preload_next(struct warp_playlist_source *s)
{
	size_t target;

	if (s->preloaded || s->preload_requested || !warp_pl_forward_target(s, &target))
		return;

	if (!warp_pl_path_at(s, target))
		return;

	s->preload_requested = true;
	s->preload_order_pos = target;
}

static void warp_playlist_tick(void *data, float seconds)
{
	struct warp_playlist_source *s = data;

	/* what was asked of the playlist from other threads since the last tick */
	warp_pl_run_commands(s);

	for (size_t dir = 0; dir < WARP_PL_NUM_DIRS; dir++)
		warp_pl_swap_transition(s, dir);

	obs_source_t *transition = warp_pl_get_transition(s);
	float transition_time = transition ? obs_transition_get_time(transition) : 1.0f;
	bool resized = false;

	obs_source_t *park = NULL;
	char *park_path = NULL;
	struct warp_pl_item_config park_cfg;

	/* Once its transition has played out, the outgoing item is parked back
	 * on its first frame when it is the file before the one playing - the
	 * one Previous goes to - and let go of otherwise. */
	if (s->prev && transition_time >= 1.0f) {
		size_t back_pos;
		const char *back_at = s->pos < s->order.num && warp_pl_step_pos(s, -1, &back_pos)
					      ? warp_pl_path_at(s, back_pos)
					      : NULL;

		if (back_at && s->prev_path && strcmp(back_at, s->prev_path) == 0) {
			park = s->prev;
			park_path = s->prev_path;
			warp_pl_read_item_config(s, park_path, &park_cfg);
			s->prev_path = NULL;
		} else {
			warp_pl_recycle(s, s->prev);
		}

		s->prev = NULL;
		bfree(s->prev_path);
		s->prev_path = NULL;
	}

	/* the item a switch is waiting on goes up as soon as it has picture */
	s->pending_age += seconds;
	warp_pl_promote_pending(s);

	/* Size the transition, and this source, to the item on screen. This is
	 * measured after the switch above rather than before it, so that an item
	 * going up is sized on the tick it goes up: the transition renders on
	 * this tick, and one whose size is still zero, or still that of a file
	 * of another resolution, lays its halves out against the wrong bounds
	 * for a frame. The first item of all is the case that showed: nothing
	 * had ever set a size, so the playlist reported none and its very first
	 * frame was drawn as nothing. */
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

	/* Nothing is asked for while a switch is waiting to land: the item on
	 * screen is the one being transitioned away from, and running its
	 * numbers again would only ask for the same switch a second time. */
	if (!s->pending && s->current && s->state == OBS_MEDIA_STATE_PLAYING) {
		enum obs_media_state item_state = obs_source_media_get_state(s->current);
		bool item_done = item_state == OBS_MEDIA_STATE_ENDED || item_state == OBS_MEDIA_STATE_STOPPED ||
				 item_state == OBS_MEDIA_STATE_ERROR;

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

	warp_pl_flush(s);

	if (park) {
		warp_pl_load_item(park, park_path, WARP_MEDIA_LOAD_HOLD, &park_cfg);

		warp_pl_drop_back(s);
		s->back = park;
		s->back_path = park_path;
		warp_pl_flush(s);
	}

	obs_source_release(transition);

	/* Follow the transition with the audio, after the switch above rather
	 * than before it, so the item going up is being listened to from the
	 * tick it goes up. */
	warp_pl_audio_sync(s);

	/* Frame the file that is playing. This is done after the switch above
	 * rather than before it so that a file going up is framed on the tick
	 * it goes up, and it is only ever handed to the item that is current:
	 * one being transitioned away from keeps the last view it was handed,
	 * so the outgoing half of a transition plays out at the zoom it was
	 * being watched at while the incoming half is already at the top. */
	struct warp_zoom_view view;
	obs_source_t *zoom;

	warp_zoom_control_tick(&s->zoom, seconds, &view);

	zoom = obs_source_get_ref(s->current_zoom);

	if (zoom) {
		warp_zoom_filter_apply(zoom, &view);
		obs_source_release(zoom);
	}

	/* both transitions are sized, so the one that is not on screen is laid
	 * out against the right bounds the moment it is switched to */
	if (resized) {
		for (size_t dir = 0; dir < WARP_PL_NUM_DIRS; dir++) {
			obs_source_t *tr = warp_pl_get_transition_dir(s, dir);

			if (tr) {
				obs_transition_set_size(tr, cx, cy);
				obs_source_release(tr);
			}
		}
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

	if (s->restart_on_activate)
		warp_pl_restart_playlist(s);
	warp_pl_flush(s);
}

static void warp_playlist_deactivate(void *data)
{
	struct warp_playlist_source *s = data;

	if (s->restart_on_activate)
		warp_pl_stop(s);
	warp_pl_flush(s);
}

static void warp_playlist_play_pause(void *data, bool pause)
{
	struct warp_playlist_source *s = data;

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
		acted = s->current || s->pending || s->play_armed;
	} else if (s->current) {
		obs_source_media_play_pause(s->current, pause);
		s->state = pause ? OBS_MEDIA_STATE_PAUSED : OBS_MEDIA_STATE_PLAYING;

		if (!pause)
			s->signal_started = true;

		acted = true;
	}

	warp_pl_flush(s);

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

	warp_pl_restart_playlist(s);
	restarted = s->play_armed;
	warp_pl_flush(s);

	if (restarted)
		warp_signal_media_action(s->source, WARP_MEDIA_ACTION_RESTART);
}

static void warp_playlist_stop_media(void *data)
{
	struct warp_playlist_source *s = data;

	warp_pl_stop(s);
	warp_pl_flush(s);
}

static void warp_playlist_next(void *data)
{
	struct warp_playlist_source *s = data;

	warp_pl_advance(s, 1);
	warp_pl_flush(s);
}

static void warp_playlist_previous(void *data)
{
	struct warp_playlist_source *s = data;

	warp_pl_advance(s, -1);
	warp_pl_flush(s);
}

static int64_t warp_playlist_get_duration(void *data)
{
	obs_source_t *current = warp_pl_snap_current(data);
	int64_t duration = current ? obs_source_media_get_duration(current) : 0;

	obs_source_release(current);
	return duration;
}

static int64_t warp_playlist_get_time(void *data)
{
	obs_source_t *current = warp_pl_snap_current(data);
	int64_t time = current ? obs_source_media_get_time(current) : 0;

	obs_source_release(current);
	return time;
}

static void warp_playlist_set_time(void *data, int64_t ms)
{
	struct warp_playlist_source *s = data;

	if (s->current)
		obs_source_media_set_time(s->current, ms);
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

/* Which stinger file of the playlist's a missing one is: the transition it
 * belongs to, and the setting of that transition it was found under. */
struct warp_pl_stinger_file {
	size_t dir;
	const char *key;
};

static const struct warp_pl_stinger_file warp_pl_stinger_video[WARP_PL_NUM_DIRS] = {
	{WARP_PL_DIR_FORWARD, "path"},
	{WARP_PL_DIR_BACKWARD, "path"},
};

static const struct warp_pl_stinger_file warp_pl_stinger_matte[WARP_PL_NUM_DIRS] = {
	{WARP_PL_DIR_FORWARD, "track_matte_path"},
	{WARP_PL_DIR_BACKWARD, "track_matte_path"},
};

/* 'data' is the stinger file of the playlist's that was found again */
static void missing_stinger_callback(void *src, const char *new_path, void *data)
{
	struct warp_playlist_source *s = src;
	const struct warp_pl_stinger_file *which = data;
	struct warp_pl_transition *cfg = &s->tr[which->dir];
	obs_data_t *fix = obs_data_create();
	obs_data_t *stored;

	obs_data_set_string(fix, which->key, new_path);

	/* into the settings the stinger is saved with, and to the stinger that
	 * is running, whose own update libobs carries out on its tick */
	pthread_mutex_lock(&s->tr_mutex);

	stored = obs_data_get_obj(cfg->store, WARP_PL_TR_STINGER);

	if (stored) {
		obs_data_set_string(stored, which->key, new_path);
		obs_data_release(stored);
	}

	obs_source_t *running = cfg->id && strcmp(cfg->id, WARP_PL_TR_STINGER) == 0 ? obs_source_get_ref(cfg->source)
										    : NULL;

	pthread_mutex_unlock(&s->tr_mutex);

	if (running) {
		obs_source_update(running, fix);
		obs_source_release(running);
	}

	obs_data_release(fix);
}

/* reports a file of the stinger settings when it has gone missing */
static void warp_pl_add_missing_stinger(struct warp_playlist_source *s, obs_missing_files_t *files,
					obs_data_t *tr_settings, const struct warp_pl_stinger_file *which)
{
	const char *path = obs_data_get_string(tr_settings, which->key);
	obs_missing_file_t *file;

	if (!path || !*path || os_file_exists(path))
		return;

	file = obs_missing_file_create(path, missing_stinger_callback, OBS_MISSING_FILE_SOURCE, s->source,
				       (void *)which);

	obs_missing_files_add_file(files, file);
}

static obs_missing_files_t *warp_playlist_missingfiles(void *data)
{
	struct warp_playlist_source *s = data;
	obs_missing_files_t *files = obs_missing_files_create();
	obs_data_t *settings;
	bool separate_back;

	pthread_mutex_lock(&s->snap_mutex);

	for (size_t i = 0; i < s->snap.paths.num; i++) {
		const char *path = s->snap.paths.array[i];

		if (path && *path && !os_file_exists(path)) {
			obs_missing_file_t *file = obs_missing_file_create(
				path, missing_file_callback, OBS_MISSING_FILE_SOURCE, s->source, (void *)(uintptr_t)i);

			obs_missing_files_add_file(files, file);
		}
	}

	pthread_mutex_unlock(&s->snap_mutex);

	/* The stinger's videos are files the playlist plays as much as the ones
	 * in the list, so missing ones are worth reporting too. The transitions
	 * belong to this source alone, so nothing else reports them. */
	warp_pl_capture_transitions(s);

	settings = obs_source_get_settings(s->source);
	separate_back = obs_data_get_bool(settings, "separate_back_transition");

	for (size_t dir = 0; dir < WARP_PL_NUM_DIRS; dir++) {
		obs_data_t *tr_settings;

		if (dir == WARP_PL_DIR_BACKWARD && !separate_back)
			continue;

		if (strcmp(obs_data_get_string(settings, warp_pl_keys[dir].id), WARP_PL_TR_STINGER) != 0)
			continue;

		pthread_mutex_lock(&s->tr_mutex);
		tr_settings = obs_data_get_obj(s->tr[dir].store, WARP_PL_TR_STINGER);
		pthread_mutex_unlock(&s->tr_mutex);

		if (!tr_settings)
			continue;

		warp_pl_add_missing_stinger(s, files, tr_settings, &warp_pl_stinger_video[dir]);

		if (obs_data_get_bool(tr_settings, "track_matte_enabled"))
			warp_pl_add_missing_stinger(s, files, tr_settings, &warp_pl_stinger_matte[dir]);

		obs_data_release(tr_settings);
	}

	obs_data_release(settings);

	return files;
}

struct obs_source_info warp_playlist_source_info = {
	.id = "warp_playlist_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	/* The playlist renders its picture itself and hands its audio over
	 * itself, the way the browser source does. It is deliberately not
	 * composite: obs_register_source() refuses OBS_SOURCE_AUDIO alongside
	 * OBS_SOURCE_COMPOSITE, and without OBS_SOURCE_AUDIO there is no
	 * playlist in the OBS audio mixer to fade, mute, monitor, filter or
	 * route. The items are mixed by the audio section above instead. */
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE |
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
