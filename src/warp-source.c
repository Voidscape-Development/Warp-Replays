/*
 * Copyright (c) 2015 John R. Bradley <jrb@turrettech.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>

#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/dstr.h>

#include <media-playback/media-playback.h>

#include "warp-events.h"
#include "warp-zoom.h"

#define FF_LOG_S(source, level, format, ...)      \
	blog(level, "[Warp Media '%s']: " format, \
	     obs_source_get_name(source), ##__VA_ARGS__)
#define FF_BLOG(level, format, ...) \
	FF_LOG_S(s->source, level, format, ##__VA_ARGS__)

/* Where a clip sits in its file, kept with the source's settings so a trimmed
 * instant replay comes back trimmed with the scene collection. They are written
 * by warp_media_load rather than set from the properties, and only stand for
 * the file they were written with: clip_path is that file, and a file picked
 * from the properties afterwards is played whole. */
#define WARP_CLIP_PATH "warp_clip_path"
#define WARP_CLIP_IN_MS "warp_clip_in_ms"
#define WARP_CLIP_OUT_MS "warp_clip_out_ms"
#define WARP_CLIP_TAIL_MS "warp_clip_tail_ms"

struct warp_source;

/* per-hotkey context for parametrized hotkeys: 'value' is a signed frame
 * count for step hotkeys and a speed percentage for preset hotkeys */
struct warp_hotkey_binding {
	struct warp_source *s;
	int value;
};

/* what a file is loaded with, read out of the settings */
struct warp_source_config {
	char *input;
	char *input_format;
	char *ffmpeg_options;
	int buffering_mb;
	bool is_local_file;
	bool is_hw_decoding;
	enum video_range_type range;
	int64_t in_ms;
	int64_t out_ms;
	int64_t tail_ms;
};

struct warp_source {
	obs_source_t *source;

	/* The playback engine. It is made with the source and lives as long as
	 * it does: files are loaded into it rather than an engine being made
	 * for each one, so the media thread, the hardware decoder and - for a
	 * file encoded the way the last one was - the decoder itself carry
	 * over from one clip to the next. */
	media_playback_t *media;

	/* Guards 'cfg' and the load numbers below it. Settings are applied on
	 * the graphics thread, a clip is handed over from the UI's or the
	 * websocket's, and both end in a load. */
	pthread_mutex_t mutex;
	struct warp_source_config cfg;
	/* the number of the last load asked of the engine, and whether one
	 * has been asked at all */
	uint64_t load_seq;

	/* The load the engine is handing out frames for, set on the media
	 * thread as each load starts, and the one the source's texture last
	 * had a picture of that load written to. Picture is "there" for the
	 * load that was asked for only when they agree. */
	volatile long media_seq;
	volatile long picture_seq;

	bool is_looping;
	bool is_linear_alpha;
	bool is_clear_on_media_end;
	bool restart_on_activate;
	bool close_when_inactive;
	bool seekable;
	bool log_changes;
	volatile long speed_percent;

	pthread_t reconnect_thread;
	pthread_mutex_t reconnect_mutex;
	bool reconnect_thread_valid;
	os_event_t *reconnect_stop_event;
	volatile bool reconnecting;
	volatile bool reconnect_pending;
	int reconnect_delay_sec;

	/* Whether the source holds a picture written into its texture, rather
	 * than a frame merely handed over. libobs uploads a decoded frame only
	 * while the source is being rendered, so a source that is not on
	 * screen holds an allocated texture that has never been written to -
	 * and that renders as nothing at all. Written from the media thread
	 * and the graphics thread. */
	volatile bool has_picture;
	/* whether a cued first frame has been handed over for
	 * obs_source_show_preloaded_video() to put up */
	volatile bool preloaded_frame;

	/* The OBS media state. Set as commands are given, so the frontend sees
	 * what it asked for straight away, and by the engine's events as
	 * playback ends or fails. */
	volatile long state;

	obs_hotkey_id hotkey;
	obs_hotkey_pair_id play_pause_hotkey;
	obs_hotkey_id stop_hotkey;

	/* How the clip in the source is framed, and the filter that draws it.
	 * An async source has no picture of its own to work on, so the zoom is
	 * drawn by a filter the source puts on itself the first time anything
	 * asks it to zoom, and drives from its own tick. A clip landing puts the
	 * framing back to the whole picture: an instant replay arrives ready to
	 * go on screen rather than still zoomed into the last one. */
	struct warp_zoom_control zoom;
	obs_source_t *zoom_filter;
	/* said once when the source stands aside for a Warp Zoom filter the
	 * operator put on it themselves */
	bool zoom_yielded;

	/* Set while the source drives its own playback instead of carrying out
	 * a command someone sent: restarting as it goes on screen, and the
	 * pause a frame step does first. The media action signal is about
	 * commands, so those are not reported as ones. */
	bool internal_command;

	struct warp_hotkey_binding speed_bindings[WARP_NUM_SPEED_PRESETS];
	struct warp_hotkey_binding step_bindings[WARP_NUM_STEP_HOTKEYS];
};

static inline enum obs_media_state get_state(struct warp_source *s)
{
	return (enum obs_media_state)os_atomic_load_long(&s->state);
}

static inline void set_state(struct warp_source *s, enum obs_media_state state)
{
	os_atomic_set_long(&s->state, (long)state);
}

static inline int get_speed(struct warp_source *s)
{
	return (int)os_atomic_load_long(&s->speed_percent);
}

static void config_free(struct warp_source_config *cfg)
{
	bfree(cfg->input);
	bfree(cfg->input_format);
	bfree(cfg->ffmpeg_options);
	memset(cfg, 0, sizeof(*cfg));
}

static inline bool str_differs(const char *a, const char *b)
{
	return strcmp(a ? a : "", b ? b : "") != 0;
}

/* whether a file loaded with 'a' would be loaded differently with 'b' */
static bool config_differs(const struct warp_source_config *a, const struct warp_source_config *b)
{
	return a->is_local_file != b->is_local_file || a->is_hw_decoding != b->is_hw_decoding || a->range != b->range ||
	       a->buffering_mb != b->buffering_mb || a->in_ms != b->in_ms || a->out_ms != b->out_ms ||
	       a->tail_ms != b->tail_ms || str_differs(a->input, b->input) ||
	       str_differs(a->input_format, b->input_format) || str_differs(a->ffmpeg_options, b->ffmpeg_options);
}

// Used to safely cancel and join any active reconnect threads
// Use this to join any finished reconnect thread too!
static void stop_reconnect_thread(struct warp_source *s)
{
	pthread_mutex_lock(&s->reconnect_mutex);
	if (s->reconnect_thread_valid) {
		os_event_signal(s->reconnect_stop_event);
		pthread_join(s->reconnect_thread, NULL);
		s->reconnect_thread_valid = false;
		os_atomic_set_bool(&s->reconnecting, false);
		os_event_reset(s->reconnect_stop_event);
	}
	pthread_mutex_unlock(&s->reconnect_mutex);
}

static bool is_local_file_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);

	bool enabled = obs_data_get_bool(settings, "is_local_file");
	obs_property_t *input = obs_properties_get(props, "input");
	obs_property_t *input_format = obs_properties_get(props, "input_format");
	obs_property_t *local_file = obs_properties_get(props, "local_file");
	obs_property_t *looping = obs_properties_get(props, "looping");
	obs_property_t *buffering = obs_properties_get(props, "buffering_mb");
	obs_property_t *seekable = obs_properties_get(props, "seekable");
	obs_property_t *speed = obs_properties_get(props, "speed_percent");
	obs_property_t *reconnect_delay_sec = obs_properties_get(props, "reconnect_delay_sec");
	obs_property_set_visible(input, !enabled);
	obs_property_set_visible(input_format, !enabled);
	obs_property_set_visible(buffering, !enabled);
	obs_property_set_visible(local_file, enabled);
	obs_property_set_visible(looping, enabled);
	obs_property_set_visible(speed, enabled);
	obs_property_set_visible(seekable, !enabled);
	obs_property_set_visible(reconnect_delay_sec, !enabled);

	return true;
}

static void warp_source_defaults(obs_data_t *settings)
{
	/* the framing is not saved with the source: it belongs to the clip that
	 * is in it, and the next clip to land starts from the whole picture */
	warp_zoom_control_defaults(settings, false);

	obs_data_set_default_bool(settings, "is_local_file", true);
	obs_data_set_default_bool(settings, "looping", false);
	obs_data_set_default_bool(settings, "clear_on_media_end", true);
	obs_data_set_default_bool(settings, "restart_on_activate", true);
	obs_data_set_default_bool(settings, "linear_alpha", false);
	obs_data_set_default_int(settings, "reconnect_delay_sec", 10);
	obs_data_set_default_int(settings, "buffering_mb", 2);
	obs_data_set_default_int(settings, "speed_percent", 100);
	obs_data_set_default_bool(settings, "log_changes", true);
}

static const char *media_filter =
	" (*.mp4 *.m4v *.ts *.mov *.mxf *.flv *.mkv *.avi *.mp3 *.ogg *.aac *.wav *.gif *.webm);;";
static const char *video_filter = " (*.mp4 *.m4v *.ts *.mov *.mxf *.flv *.mkv *.avi *.gif *.webm);;";
static const char *audio_filter = " (*.mp3 *.aac *.ogg *.wav);;";

static obs_properties_t *warp_source_getproperties(void *data)
{
	struct warp_source *s = data;
	struct dstr filter = {0};
	struct dstr path = {0};

	obs_properties_t *props = obs_properties_create();

	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_property_t *prop;
	// use this when obs allows non-readonly paths
	prop = obs_properties_add_bool(props, "is_local_file", obs_module_text("Warp.Media.LocalFile"));

	obs_property_set_modified_callback(prop, is_local_file_modified);

	dstr_copy(&filter, obs_module_text("Warp.FileFilter.AllMedia"));
	dstr_cat(&filter, media_filter);
	dstr_cat(&filter, obs_module_text("Warp.FileFilter.Video"));
	dstr_cat(&filter, video_filter);
	dstr_cat(&filter, obs_module_text("Warp.FileFilter.Audio"));
	dstr_cat(&filter, audio_filter);
	dstr_cat(&filter, obs_module_text("Warp.FileFilter.All"));
	dstr_cat(&filter, " (*.*)");

	if (s) {
		pthread_mutex_lock(&s->mutex);
		if (s->cfg.input && *s->cfg.input) {
			const char *slash;

			dstr_copy(&path, s->cfg.input);
			dstr_replace(&path, "\\", "/");
			slash = strrchr(path.array, '/');
			if (slash)
				dstr_resize(&path, slash - path.array + 1);
		}
		pthread_mutex_unlock(&s->mutex);
	}

	obs_properties_add_path(props, "local_file", obs_module_text("Warp.Media.LocalFile"), OBS_PATH_FILE,
				filter.array, path.array);
	dstr_free(&filter);
	dstr_free(&path);

	obs_properties_add_bool(props, "looping", obs_module_text("Warp.Media.Looping"));

	obs_properties_add_bool(props, "restart_on_activate", obs_module_text("Warp.Media.RestartOnActivate"));

	prop = obs_properties_add_int_slider(props, "buffering_mb", obs_module_text("Warp.Media.Buffering"), 0, 16, 1);
	obs_property_int_set_suffix(prop, " MB");

	obs_properties_add_text(props, "input", obs_module_text("Warp.Media.Input"), OBS_TEXT_DEFAULT);

	obs_properties_add_text(props, "input_format", obs_module_text("Warp.Media.InputFormat"), OBS_TEXT_DEFAULT);

	prop = obs_properties_add_int_slider(props, "reconnect_delay_sec", obs_module_text("Warp.Media.ReconnectDelay"),
					     1, 60, 1);
	obs_property_int_set_suffix(prop, " S");

	obs_properties_add_bool(props, "hw_decode", obs_module_text("Warp.Video.HardwareDecode"));

	obs_properties_add_bool(props, "clear_on_media_end", obs_module_text("Warp.Media.ClearOnEnd"));

	prop = obs_properties_add_bool(props, "close_when_inactive", obs_module_text("Warp.Media.CloseWhenInactive"));

	obs_property_set_long_description(prop, obs_module_text("Warp.Media.CloseWhenInactive.Desc"));

	prop = obs_properties_add_int_slider(props, "speed_percent", obs_module_text("Warp.Video.Speed"), MP_SPEED_MIN,
					     MP_SPEED_MAX, 1);
	obs_property_int_set_suffix(prop, "%");

	warp_zoom_control_properties(props, false);

	prop = obs_properties_add_list(props, "color_range", obs_module_text("Warp.Video.ColorRange"),
				       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("Warp.Video.ColorRange.Auto"), VIDEO_RANGE_DEFAULT);
	obs_property_list_add_int(prop, obs_module_text("Warp.Video.ColorRange.Partial"), VIDEO_RANGE_PARTIAL);
	obs_property_list_add_int(prop, obs_module_text("Warp.Video.ColorRange.Full"), VIDEO_RANGE_FULL);

	obs_properties_add_bool(props, "linear_alpha", obs_module_text("Warp.Video.LinearAlpha"));

	obs_properties_add_bool(props, "seekable", obs_module_text("Warp.Media.Seekable"));

	prop = obs_properties_add_text(props, "ffmpeg_options", obs_module_text("Warp.Media.FFmpegOptions"),
				       OBS_TEXT_DEFAULT);
	obs_property_set_long_description(prop, obs_module_text("Warp.Media.FFmpegOptions.Desc"));

	return props;
}

static void dump_source_info(struct warp_source *s, const struct warp_source_config *cfg)
{
	if (!s->log_changes)
		return;
	FF_BLOG(LOG_INFO,
		"settings:\n"
		"\tinput:                   %s\n"
		"\tinput_format:            %s\n"
		"\tclip:                    %lld..%lld ms, last %lld ms\n"
		"\tspeed:                   %d\n"
		"\tis_looping:              %s\n"
		"\tis_linear_alpha:         %s\n"
		"\tis_hw_decoding:          %s\n"
		"\tis_clear_on_media_end:   %s\n"
		"\trestart_on_activate:     %s\n"
		"\tclose_when_inactive:     %s\n"
		"\tffmpeg_options:          %s",
		cfg->input ? cfg->input : "(null)", cfg->input_format ? cfg->input_format : "(null)",
		(long long)cfg->in_ms, (long long)cfg->out_ms, (long long)cfg->tail_ms, get_speed(s),
		s->is_looping ? "yes" : "no", s->is_linear_alpha ? "yes" : "no", cfg->is_hw_decoding ? "yes" : "no",
		s->is_clear_on_media_end ? "yes" : "no", s->restart_on_activate ? "yes" : "no",
		s->close_when_inactive ? "yes" : "no", cfg->ffmpeg_options ? cfg->ffmpeg_options : "");
}

/* Drops the picture the source is holding. Every place that clears the source
 * goes through here, so that s->has_picture cannot drift from what is actually
 * in the source's texture. */
static void clear_video(struct warp_source *s)
{
	os_atomic_set_bool(&s->has_picture, false);
	obs_source_output_video(s->source, NULL);
}

/* ------------------------------------------------------------------------- */
/* what the engine hands over, on its thread */

static void note_picture(struct warp_source *s)
{
	os_atomic_set_long(&s->picture_seq, os_atomic_load_long(&s->media_seq));
	os_atomic_set_bool(&s->has_picture, true);
}

static void get_frame(void *opaque, struct obs_source_frame *f)
{
	struct warp_source *s = opaque;

	/* The first frame of a load is written into the source's texture here
	 * rather than left to the render path. libobs uploads an async frame
	 * only while the source is being rendered, so a source that has decoded
	 * picture but has not been rendered yet still has an empty texture - or
	 * the last clip's picture in it - and libobs draws that texture all the
	 * same. The Warp Playlist opens the file it is switching to off screen
	 * and hands it straight to its transition, so an item has to have its
	 * own picture in it before it is composited. Only the first frame of a
	 * load pays for this; every one after it is uploaded by the render path
	 * as usual. */
	if (!os_atomic_load_bool(&s->has_picture) ||
	    os_atomic_load_long(&s->picture_seq) != os_atomic_load_long(&s->media_seq)) {
		obs_source_set_video_frame(s->source, f);
		os_atomic_set_bool(&s->preloaded_frame, true);
		note_picture(s);
	}

	obs_source_output_video(s->source, f);
}

static void preload_frame(void *opaque, struct obs_source_frame *f)
{
	struct warp_source *s = opaque;

	if (s->close_when_inactive)
		return;

	if (s->is_clear_on_media_end || s->is_looping) {
		obs_source_preload_video(s->source, f);
		os_atomic_set_bool(&s->preloaded_frame, true);
	}
}

static void still_frame(void *opaque, struct obs_source_frame *f)
{
	struct warp_source *s = opaque;

	/* A seek, a step or a held clip puts its frame straight into the
	 * texture, so it is there whether or not the source is being rendered.
	 * It is handed over as an ordinary frame as well: libobs only draws an
	 * async source's texture once a frame has come in that way, and a clip
	 * that is held from the moment it is loaded has had no other. */
	obs_source_set_video_frame(s->source, f);
	obs_source_output_video(s->source, f);
	os_atomic_set_bool(&s->preloaded_frame, true);
	note_picture(s);
}

static void get_audio(void *opaque, struct obs_source_audio *a)
{
	struct warp_source *s = opaque;
	obs_source_output_audio(s->source, a);
}

static void media_event(void *opaque, enum mp_event event, uint64_t seq)
{
	struct warp_source *s = opaque;

	switch (event) {
	case MP_EVENT_LOADING:
		/* Everything handed over from here on is this load's. What is
		 * on screen is left up until the new file has a frame to put
		 * there, so a clip landing in a source that is on air is a cut
		 * from one picture to the next rather than a flash of nothing. */
		os_atomic_set_long(&s->media_seq, (long)seq);
		break;

	case MP_EVENT_LOADED:
		if (os_atomic_set_bool(&s->reconnecting, false))
			FF_BLOG(LOG_INFO, "Reconnected.");
		break;

	case MP_EVENT_LOAD_FAILED: {
		bool is_local;

		pthread_mutex_lock(&s->mutex);
		is_local = s->cfg.is_local_file;
		pthread_mutex_unlock(&s->mutex);

		/* a stream that is not there yet is tried again; a file that
		 * will not open is an error until something else is loaded */
		if (!is_local)
			os_atomic_set_bool(&s->reconnect_pending, true);
		else
			set_state(s, OBS_MEDIA_STATE_ERROR);
		break;
	}

	case MP_EVENT_ENDED:
	case MP_EVENT_ERROR: {
		bool is_local;

		pthread_mutex_lock(&s->mutex);
		is_local = s->cfg.is_local_file;
		pthread_mutex_unlock(&s->mutex);

		if (s->is_clear_on_media_end)
			clear_video(s);

		/* a stream that has stopped coming is gone until it is
		 * connected to again */
		if (!is_local) {
			os_atomic_set_bool(&s->reconnect_pending, true);
			break;
		}

		if (get_state(s) != OBS_MEDIA_STATE_STOPPED) {
			set_state(s, OBS_MEDIA_STATE_ENDED);
			obs_source_media_ended(s->source);
		}
		break;
	}
	}
}

/* ------------------------------------------------------------------------- */
/* loading */

/* Hands the engine the file the settings name, to be started 'start'.
 * Returns the number of the load, or 0 when there is no file to load. */
static uint64_t warp_source_load(struct warp_source *s, enum mp_start start)
{
	uint64_t seq = 0;

	pthread_mutex_lock(&s->mutex);

	if (s->cfg.input && *s->cfg.input) {
		struct mp_load_info info = {
			.path = s->cfg.input,
			.format = s->cfg.input_format,
			.ffmpeg_options = s->cfg.ffmpeg_options,
			.buffering = s->cfg.buffering_mb * 1024 * 1024,
			.speed = get_speed(s),
			.is_local_file = s->cfg.is_local_file || s->seekable,
			.looping = s->is_looping,
			.hardware_decoding = s->cfg.is_hw_decoding,
			.reconnecting = os_atomic_load_bool(&s->reconnecting),
			.force_range = s->cfg.range,
			.is_linear_alpha = s->is_linear_alpha,
			.in_ms = s->cfg.in_ms,
			.out_ms = s->cfg.out_ms,
			.tail_ms = s->cfg.tail_ms,
			.start = start,
		};

		/* the picture that is up is not this load's */
		os_atomic_set_bool(&s->preloaded_frame, false);

		seq = media_playback_load(s->media, &info);
		s->load_seq = seq;
	}

	pthread_mutex_unlock(&s->mutex);

	return seq;
}

/* whether the engine has the file the settings name open, or is opening it */
static bool warp_source_loaded(struct warp_source *s)
{
	struct mp_status st;
	uint64_t seq;

	pthread_mutex_lock(&s->mutex);
	seq = s->load_seq;
	pthread_mutex_unlock(&s->mutex);

	if (!seq)
		return false;

	media_playback_get_status(s->media, &st);
	return st.load_seq == seq && (st.loaded_seq != seq || st.open);
}

static void warp_source_start(struct warp_source *s)
{
	bool is_local;

	pthread_mutex_lock(&s->mutex);
	is_local = s->cfg.is_local_file;
	pthread_mutex_unlock(&s->mutex);

	if (!warp_source_loaded(s)) {
		warp_source_load(s, MP_START_PLAY);
	} else {
		struct mp_status st;

		media_playback_get_status(s->media, &st);
		media_playback_restart(s->media);

		if (is_local && st.has_video && (s->is_clear_on_media_end || s->is_looping)) {
			/* the first frame was cued as the clip was opened or
			 * last stopped, so there is picture from the moment
			 * playback starts */
			if (os_atomic_load_bool(&s->preloaded_frame)) {
				obs_source_show_preloaded_video(s->source);
				os_atomic_set_bool(&s->has_picture, true);
			}
		} else if (!is_local || !os_atomic_load_bool(&s->has_picture)) {
			/* Nothing to put up yet, and nothing worth keeping. A
			 * local file that is holding picture keeps it instead:
			 * a source set not to clear at the end of a file has no
			 * reason to blank while it goes back to the top. */
			clear_video(s);
		}
	}

	set_state(s, OBS_MEDIA_STATE_PLAYING);
	obs_source_media_started(s->source);
}

static void *warp_source_reconnect(void *data)
{
	struct warp_source *s = data;

	int ret = os_event_timedwait(s->reconnect_stop_event, s->reconnect_delay_sec * 1000);
	if (ret == 0)
		return NULL;

	bool active = obs_source_active(s->source);

	if (!s->close_when_inactive || active)
		warp_source_load(s, !s->restart_on_activate || active ? MP_START_PLAY : MP_START_CUE);

	return NULL;
}

/* Whether the source has the clip it was last asked for ready to put on
 * screen: the file is open and, when it has video, a frame of it has been
 * written into the source's texture. Opening is asynchronous: the source has
 * nothing of a new file to show until its media thread has read the header and
 * decoded a frame, and a frame of the clip before it does not count. The Warp
 * Playlist waits for this before handing an item to its transition. */
static bool warp_source_ready(struct warp_source *s, bool *failed)
{
	struct mp_status st;
	uint64_t seq;

	pthread_mutex_lock(&s->mutex);
	seq = s->load_seq;
	pthread_mutex_unlock(&s->mutex);

	*failed = false;

	if (!seq)
		return false;

	media_playback_get_status(s->media, &st);

	if (st.loaded_seq != seq)
		return false;

	if (!st.open) {
		*failed = true;
		return false;
	}

	if (!st.has_video)
		/* audio only: there is no picture to wait for */
		return true;

	return os_atomic_load_bool(&s->has_picture) && os_atomic_load_long(&s->picture_seq) == (long)seq;
}

/* ------------------------------------------------------------------------- */
/* zoom */

/* The filter the zoom is drawn by, made the first time anything asks the source
 * to zoom rather than put on every source that is never framed. One the source
 * was made with - a playlist opens its files with a zoom of their own - is
 * picked up instead of a second one being added. Expects to be called from the
 * graphics thread. */
static obs_source_t *warp_source_zoom_filter(struct warp_source *s)
{
	obs_source_t *filter;

	/* an operator who removed the filter from the source's filter list gets
	 * a new one the next time they ask for a zoom, rather than a zoom that
	 * quietly does nothing from then on */
	if (s->zoom_filter && obs_filter_get_parent(s->zoom_filter) != s->source) {
		obs_source_release(s->zoom_filter);
		s->zoom_filter = NULL;
	}

	if (s->zoom_filter)
		return s->zoom_filter;

	filter = warp_zoom_filter_find(s->source);

	if (!filter) {
		/* A Warp Zoom filter the operator added themselves is the zoom
		 * of this source: it has its own presets and its own hotkeys,
		 * and a second one underneath it would zoom what it had already
		 * zoomed. So the source stands aside rather than stacking. */
		obs_source_t *theirs = warp_zoom_filter_find_operable(s->source);

		if (theirs) {
			if (!s->zoom_yielded) {
				FF_BLOG(LOG_INFO, "'%s' frames this source, so its own zoom is left alone",
					obs_source_get_name(theirs));
				s->zoom_yielded = true;
			}

			obs_source_release(theirs);
			return NULL;
		}

		filter = warp_zoom_filter_create_driven(obs_module_text("Warp.ZoomFilter.Name"));

		if (!filter) {
			FF_BLOG(LOG_WARNING, "could not put a zoom on the source");
			return NULL;
		}

		obs_source_filter_add(s->source, filter);
	}

	s->zoom_filter = filter;

	return filter;
}

/* frames the clip in the source with wherever the zoom has got to */
static void warp_source_zoom_tick(struct warp_source *s, float seconds)
{
	struct warp_zoom_view view;
	obs_source_t *filter;

	warp_zoom_control_tick(&s->zoom, seconds, &view);

	/* a source that has never been zoomed is left without a filter at all,
	 * so nothing is added to a scene collection that does not use zoom */
	if (warp_zoom_view_is_default(&view) && !s->zoom_filter)
		return;

	filter = warp_source_zoom_filter(s);

	if (filter)
		warp_zoom_filter_apply(filter, &view);
}

static void warp_source_tick(void *data, float seconds)
{
	struct warp_source *s = data;

	warp_source_zoom_tick(s, seconds);

	/* A stream that dropped, or could not be opened, is tried again after
	 * the reconnect delay, from a thread of its own so the wait holds
	 * nothing up. */
	if (os_atomic_set_bool(&s->reconnect_pending, false)) {
		media_playback_close(s->media);

		pthread_mutex_lock(&s->reconnect_mutex);
		if (!os_atomic_set_bool(&s->reconnecting, true))
			FF_BLOG(LOG_WARNING, "Disconnected. Reconnecting...");
		if (s->reconnect_thread_valid) {
			os_event_signal(s->reconnect_stop_event);
			pthread_join(s->reconnect_thread, NULL);
			s->reconnect_thread_valid = false;
			os_event_reset(s->reconnect_stop_event);
		}
		if (pthread_create(&s->reconnect_thread, NULL, warp_source_reconnect, s) != 0)
			FF_BLOG(LOG_WARNING, "Could not create reconnect thread");
		else
			s->reconnect_thread_valid = true;
		pthread_mutex_unlock(&s->reconnect_mutex);
	}
}

/* ------------------------------------------------------------------------- */
/* settings */

#define RIST_PROTO "rist"

static void read_config(obs_data_t *settings, struct warp_source_config *cfg)
{
	cfg->is_local_file = obs_data_get_bool(settings, "is_local_file");

	if (cfg->is_local_file) {
		const char *path = obs_data_get_string(settings, "local_file");

		cfg->input = bstrdup(path);

		/* a clip handed over by warp_media_load is played as it was
		 * handed over; a file picked from the properties is played
		 * whole */
		if (strcmp(obs_data_get_string(settings, WARP_CLIP_PATH), path) == 0) {
			cfg->in_ms = obs_data_get_int(settings, WARP_CLIP_IN_MS);
			cfg->out_ms = obs_data_get_int(settings, WARP_CLIP_OUT_MS);
			cfg->tail_ms = obs_data_get_int(settings, WARP_CLIP_TAIL_MS);
		}
	} else {
		const char *format = obs_data_get_string(settings, "input_format");

		cfg->input = bstrdup(obs_data_get_string(settings, "input"));
		cfg->input_format = format && *format ? bstrdup(format) : NULL;
	}

	const char *options = obs_data_get_string(settings, "ffmpeg_options");

	cfg->ffmpeg_options = options && *options ? bstrdup(options) : NULL;
	cfg->buffering_mb = (int)obs_data_get_int(settings, "buffering_mb");
	cfg->is_hw_decoding = obs_data_get_bool(settings, "hw_decode");
	cfg->range = (enum video_range_type)obs_data_get_int(settings, "color_range");
}

static void warp_source_update(void *data, obs_data_t *settings)
{
	struct warp_source *s = data;

	/* the zoom guards itself, and registers hotkeys as its presets change,
	 * so it is brought up to date before anything else is touched */
	warp_zoom_control_update(&s->zoom, settings);

	struct warp_source_config cfg = {0};
	read_config(settings, &cfg);

	/* zero until the first update, which is the one create() makes: the
	 * speed a source starts at is not a change anyone can react to */
	int prev_speed = get_speed(s);
	int speed = (int)obs_data_get_int(settings, "speed_percent");
	bool active = obs_source_active(s->source);
	bool was_looping = s->is_looping;

	if (speed < MP_SPEED_MIN || speed > MP_SPEED_MAX)
		speed = 100;

	s->is_looping = cfg.is_local_file && obs_data_get_bool(settings, "looping");
	s->is_linear_alpha = obs_data_get_bool(settings, "linear_alpha");
	s->is_clear_on_media_end = obs_data_get_bool(settings, "clear_on_media_end");
	s->close_when_inactive = obs_data_get_bool(settings, "close_when_inactive");
	s->restart_on_activate = astrcmpi_n(cfg.input ? cfg.input : "", RIST_PROTO, sizeof(RIST_PROTO) - 1) != 0 &&
				 obs_data_get_bool(settings, "restart_on_activate");
	s->seekable = obs_data_get_bool(settings, "seekable");
	s->log_changes = obs_data_get_bool(settings, "log_changes");
	s->reconnect_delay_sec = (int)obs_data_get_int(settings, "reconnect_delay_sec");
	if (s->reconnect_delay_sec <= 0)
		s->reconnect_delay_sec = 10;
	os_atomic_set_long(&s->speed_percent, speed);

	stop_reconnect_thread(s);

	/* Only a change to what is loaded, or how, loads the file again; the
	 * rest applies to playback as it runs. A clip handed over by
	 * warp_media_load has already been loaded with these settings, so the
	 * update that saves them changes nothing here. Network input is always
	 * connected to afresh. */
	pthread_mutex_lock(&s->mutex);
	bool reload = !s->load_seq || !cfg.is_local_file || config_differs(&s->cfg, &cfg);
	config_free(&s->cfg);
	s->cfg = cfg;
	pthread_mutex_unlock(&s->mutex);

	/* A clip that has ended and has looping switched on is expected to go
	 * round. Only ENDED: a clip someone stopped stays stopped. */
	if (active && get_state(s) == OBS_MEDIA_STATE_ENDED && s->is_looping && !was_looping)
		reload = true;

	media_playback_set_looping(s->media, s->is_looping);
	media_playback_set_is_linear_alpha(s->media, s->is_linear_alpha);
	media_playback_set_speed(s->media, speed);

	if (reload) {
		if (!s->close_when_inactive || active) {
			bool play = !s->restart_on_activate || active;

			warp_source_load(s, play ? MP_START_PLAY : MP_START_CUE);

			if (play) {
				set_state(s, OBS_MEDIA_STATE_PLAYING);
				obs_source_media_started(s->source);
			} else {
				set_state(s, OBS_MEDIA_STATE_STOPPED);
			}
		} else {
			media_playback_close(s->media);
			set_state(s, OBS_MEDIA_STATE_NONE);
		}
	}

	pthread_mutex_lock(&s->mutex);
	dump_source_info(s, &s->cfg);
	pthread_mutex_unlock(&s->mutex);

	/* the Speed property applies live, so it is a speed change like any
	 * hotkey-driven one */
	if (prev_speed && prev_speed != speed)
		warp_signal_speed_changed(s->source, speed, prev_speed, WARP_SPEED_CHANGE_SET);
}

static const char *warp_source_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Warp.MediaSource.Name");
}

/* ------------------------------------------------------------------------- */
/* hotkeys and procs */

static void restart_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return;

	struct warp_source *s = data;
	if (obs_source_showing(s->source))
		obs_source_media_restart(s->source);
}

static void restart_proc(void *data, calldata_t *cd)
{
	restart_hotkey(data, 0, NULL, true);
	UNUSED_PARAMETER(cd);
}

static void get_duration(void *data, calldata_t *cd)
{
	struct warp_source *s = data;
	struct mp_status st;

	media_playback_get_status(s->media, &st);
	calldata_set_int(cd, "duration", st.duration_ms * 1000000);
}

static void get_nb_frames(void *data, calldata_t *cd)
{
	struct warp_source *s = data;
	struct mp_status st;

	media_playback_get_status(s->media, &st);
	calldata_set_int(cd, "num_frames", st.frames);
}

static void media_ready_proc(void *data, calldata_t *cd)
{
	struct warp_source *s = data;
	bool failed;
	uint64_t seq;

	bool ready = warp_source_ready(s, &failed);

	pthread_mutex_lock(&s->mutex);
	seq = s->load_seq;
	pthread_mutex_unlock(&s->mutex);

	calldata_set_bool(cd, "ready", ready);
	calldata_set_bool(cd, "failed", failed);
	calldata_set_int(cd, "seq", (long long)seq);
}

static void media_close_proc(void *data, calldata_t *cd)
{
	struct warp_source *s = data;

	UNUSED_PARAMETER(cd);

	stop_reconnect_thread(s);
	media_playback_close(s->media);
	clear_video(s);

	pthread_mutex_lock(&s->mutex);
	s->load_seq = 0;
	pthread_mutex_unlock(&s->mutex);

	set_state(s, OBS_MEDIA_STATE_NONE);
}

static bool warp_source_play_hotkey(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return false;

	struct warp_source *s = data;

	if (get_state(s) == OBS_MEDIA_STATE_PLAYING || !obs_source_showing(s->source))
		return false;

	obs_source_media_play_pause(s->source, false);
	return true;
}

static bool warp_source_pause_hotkey(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return false;

	struct warp_source *s = data;

	if (get_state(s) != OBS_MEDIA_STATE_PLAYING || !obs_source_showing(s->source))
		return false;

	obs_source_media_play_pause(s->source, true);
	return true;
}

static void warp_source_stop_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return;

	struct warp_source *s = data;

	if (obs_source_showing(s->source))
		obs_source_media_stop(s->source);
}

static bool warp_source_is_local(struct warp_source *s)
{
	pthread_mutex_lock(&s->mutex);
	bool is_local = s->cfg.is_local_file;
	pthread_mutex_unlock(&s->mutex);
	return is_local;
}

/* 'change' is one of the WARP_SPEED_CHANGE_* kinds, and says how the speed was
 * asked to move: a listener can tell a preset from a step that happens to land
 * on the same value */
static void warp_source_apply_speed(struct warp_source *s, int speed, const char *change)
{
	if (speed < MP_SPEED_MIN)
		speed = MP_SPEED_MIN;
	else if (speed > MP_SPEED_MAX)
		speed = MP_SPEED_MAX;

	int prev_speed = get_speed(s);

	if (!warp_source_is_local(s) || speed == prev_speed)
		return;

	os_atomic_set_long(&s->speed_percent, speed);
	media_playback_set_speed(s->media, speed);

	/* keep the saved settings in sync so the next update() and scene
	 * collection saves reflect the hotkey-driven speed */
	obs_data_t *settings = obs_source_get_settings(s->source);
	obs_data_set_int(settings, "speed_percent", speed);
	obs_data_release(settings);

	FF_BLOG(LOG_INFO, "speed set to %d%%", speed);

	warp_signal_speed_changed(s->source, speed, prev_speed, change);
}

static void warp_source_speed_up_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct warp_source *s = data;

	if (!pressed || !obs_source_showing(s->source))
		return;

	warp_source_apply_speed(s, get_speed(s) + WARP_SPEED_STEP, WARP_SPEED_CHANGE_INCREASED);
}

static void warp_source_speed_down_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct warp_source *s = data;

	if (!pressed || !obs_source_showing(s->source))
		return;

	warp_source_apply_speed(s, get_speed(s) - WARP_SPEED_STEP, WARP_SPEED_CHANGE_DECREASED);
}

static void warp_source_speed_reset_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct warp_source *s = data;

	if (!pressed || !obs_source_showing(s->source))
		return;

	warp_source_apply_speed(s, 100, WARP_SPEED_CHANGE_SET);
}

static void warp_source_speed_preset_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct warp_hotkey_binding *b = data;

	if (!pressed || !obs_source_showing(b->s->source))
		return;

	warp_source_apply_speed(b->s, b->value, WARP_SPEED_CHANGE_SET);
}

static void warp_source_do_step(struct warp_source *s, int frames)
{
	if (!warp_source_is_local(s) || !frames)
		return;

	enum obs_media_state state = get_state(s);

	/* Stepping while playing pauses first, through OBS so the pause is
	 * seen like any other. A clip that is stopped or has ended steps from
	 * the start of the clip. */
	if (state == OBS_MEDIA_STATE_PLAYING) {
		s->internal_command = true;
		obs_source_media_play_pause(s->source, true);
		s->internal_command = false;
	} else if (state != OBS_MEDIA_STATE_PAUSED && state != OBS_MEDIA_STATE_STOPPED &&
		   state != OBS_MEDIA_STATE_ENDED) {
		return;
	}

	media_playback_step_frames(s->media, frames);
	set_state(s, OBS_MEDIA_STATE_PAUSED);

	warp_signal_frames_stepped(s->source, frames);
}

static void warp_source_step_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct warp_hotkey_binding *b = data;
	struct warp_source *s = b->s;

	if (!pressed || !obs_source_showing(s->source))
		return;

	warp_source_do_step(s, b->value);
}

/* Procs used by the Warp Playlist source to drive its media items, and by the
 * obs-websocket vendor requests to drive the source from outside OBS. They
 * apply the same changes the hotkeys do, and report them the same way. */
static void set_speed_proc(void *data, calldata_t *cd)
{
	long long speed;

	if (calldata_get_int(cd, "speed", &speed))
		warp_source_apply_speed(data, (int)speed, WARP_SPEED_CHANGE_SET);
}

/* moves the speed by 'delta' points, the way the speed up and slow down
 * hotkeys do */
static void adjust_speed_proc(void *data, calldata_t *cd)
{
	struct warp_source *s = data;
	long long delta;

	if (!calldata_get_int(cd, "delta", &delta) || !delta)
		return;

	warp_source_apply_speed(s, get_speed(s) + (int)delta,
				delta > 0 ? WARP_SPEED_CHANGE_INCREASED : WARP_SPEED_CHANGE_DECREASED);
}

static void get_speed_proc(void *data, calldata_t *cd)
{
	struct warp_source *s = data;

	calldata_set_int(cd, "speed", get_speed(s));
}

static void step_frames_proc(void *data, calldata_t *cd)
{
	long long frames;

	if (calldata_get_int(cd, "frames", &frames))
		warp_source_do_step(data, (int)frames);
}

/* Puts a clip in the source and says what playback is to do with it, which is
 * what a Warp Instant Replay flow, and the Warp Playlist with each of its items,
 * call. The clip is loaded into the engine there and then; the settings are
 * brought up to date as well, so it is saved with the scene collection, but
 * without the settings update a change of file from the properties makes - it
 * would only load the same clip a second time. */
static void warp_media_load_proc(void *data, calldata_t *cd)
{
	struct warp_source *s = data;
	const char *path = NULL;
	const char *playback = NULL;
	long long speed = 0;
	long long in_ms = 0;
	long long out_ms = 0;
	long long tail_ms = 0;

	if (!calldata_get_string(cd, "path", &path) || !path || !*path) {
		FF_BLOG(LOG_WARNING, "asked to load a clip without saying which");
		return;
	}

	calldata_get_string(cd, "playback", &playback);
	calldata_get_int(cd, "speed", &speed);
	calldata_get_int(cd, "in_ms", &in_ms);
	calldata_get_int(cd, "out_ms", &out_ms);
	calldata_get_int(cd, "tail_ms", &tail_ms);

	if (!playback || !*playback)
		playback = WARP_MEDIA_LOAD_KEEP;

	const bool play = strcmp(playback, WARP_MEDIA_LOAD_PLAY) == 0;
	const bool hold = strcmp(playback, WARP_MEDIA_LOAD_HOLD) == 0;
	const bool active = obs_source_active(s->source);
	const int prev_speed = get_speed(s);

	if (in_ms < 0)
		in_ms = 0;
	if (out_ms < 0 || (out_ms && out_ms <= in_ms))
		out_ms = 0;
	if (tail_ms < 0)
		tail_ms = 0;

	obs_data_t *settings = obs_source_get_settings(s->source);

	pthread_mutex_lock(&s->mutex);

	/* a clip is a file on disk, whatever the source was pointed at before */
	if (!s->cfg.is_local_file)
		FF_BLOG(LOG_INFO, "loading a clip, so the source is no longer reading '%s'",
			s->cfg.input ? s->cfg.input : "");

	bfree(s->cfg.input);
	bfree(s->cfg.input_format);
	s->cfg.input = bstrdup(path);
	s->cfg.input_format = NULL;
	s->cfg.is_local_file = true;
	s->cfg.in_ms = in_ms;
	s->cfg.out_ms = out_ms;
	s->cfg.tail_ms = tail_ms;

	obs_data_set_bool(settings, "is_local_file", true);
	obs_data_set_string(settings, "local_file", path);
	obs_data_set_string(settings, WARP_CLIP_PATH, path);
	obs_data_set_int(settings, WARP_CLIP_IN_MS, in_ms);
	obs_data_set_int(settings, WARP_CLIP_OUT_MS, out_ms);
	obs_data_set_int(settings, WARP_CLIP_TAIL_MS, tail_ms);

	if (speed >= MP_SPEED_MIN && speed <= MP_SPEED_MAX) {
		os_atomic_set_long(&s->speed_percent, (long)speed);
		obs_data_set_int(settings, "speed_percent", (int)speed);
	}

	pthread_mutex_unlock(&s->mutex);
	obs_data_release(settings);

	stop_reconnect_thread(s);

	/* 'keep' leaves it to the source's own settings, the way a file picked
	 * from the properties is: played if the source is on screen or does
	 * not wait to be, cued to start when it is brought on otherwise */
	enum mp_start start;
	enum obs_media_state state;

	if (hold) {
		start = MP_START_HOLD;
		state = OBS_MEDIA_STATE_PAUSED;
	} else if (play || active || !s->restart_on_activate) {
		start = MP_START_PLAY;
		state = OBS_MEDIA_STATE_PLAYING;
	} else {
		start = MP_START_CUE;
		state = OBS_MEDIA_STATE_STOPPED;
	}

	uint64_t seq = warp_source_load(s, start);

	set_state(s, state);
	if (state == OBS_MEDIA_STATE_PLAYING)
		obs_source_media_started(s->source);

	calldata_set_int(cd, "seq", (long long)seq);

	/* a clip that has just landed is a new video, so it arrives framed the
	 * way every clip arrives rather than zoomed into wherever the last one
	 * was being watched */
	warp_zoom_control_restore_default(&s->zoom);

	if (tail_ms)
		FF_BLOG(LOG_INFO, "loaded the last %lld ms of '%s' (%s)", tail_ms, path, playback);
	else if (in_ms || out_ms)
		FF_BLOG(LOG_INFO, "loaded '%s' from %lld to %lld ms (%s)", path, in_ms, out_ms, playback);
	else
		FF_BLOG(LOG_INFO, "loaded '%s' (%s)", path, playback);

	if (prev_speed && prev_speed != get_speed(s))
		warp_signal_speed_changed(s->source, get_speed(s), prev_speed, WARP_SPEED_CHANGE_SET);

	/* Said as soon as the clip is in rather than once it has decoded, so
	 * that a source which is only being loaded says so too: one that is off
	 * screen and set to restart as it is brought on has nothing to decode
	 * until it is, and waiting for picture would leave whoever is listening
	 * for this - a Warp Detection filter bringing the source on - waiting
	 * for something that only their own reaction can bring about. */
	warp_signal_media_action(s->source, WARP_MEDIA_ACTION_LOADED);
}

static void warp_source_register_warp_hotkeys(struct warp_source *s, obs_source_t *source)
{
	static const int speed_presets[WARP_NUM_SPEED_PRESETS] = {WARP_SPEED_PRESET_LIST};
	static const int step_counts[WARP_NUM_STEP_COUNTS] = {WARP_STEP_COUNT_LIST};

	obs_hotkey_register_source(source, "WarpMedia.SpeedUp", obs_module_text("Warp.Hotkey.Speed.Up"),
				   warp_source_speed_up_hotkey, s);
	obs_hotkey_register_source(source, "WarpMedia.SpeedDown", obs_module_text("Warp.Hotkey.Speed.Down"),
				   warp_source_speed_down_hotkey, s);
	obs_hotkey_register_source(source, "WarpMedia.SpeedReset", obs_module_text("Warp.Hotkey.Speed.Reset"),
				   warp_source_speed_reset_hotkey, s);

	for (size_t i = 0; i < WARP_NUM_SPEED_PRESETS; i++) {
		char name[64];
		char text_key[64];

		s->speed_bindings[i].s = s;
		s->speed_bindings[i].value = speed_presets[i];

		snprintf(name, sizeof(name), "WarpMedia.SpeedPreset%d", speed_presets[i]);
		snprintf(text_key, sizeof(text_key), "Warp.Hotkey.Speed.Preset%d", speed_presets[i]);
		obs_hotkey_register_source(source, name, obs_module_text(text_key), warp_source_speed_preset_hotkey,
					   &s->speed_bindings[i]);
	}

	for (size_t i = 0; i < WARP_NUM_STEP_COUNTS; i++) {
		char name[64];
		char text_key[64];

		struct warp_hotkey_binding *fwd = &s->step_bindings[i * 2];
		struct warp_hotkey_binding *back = &s->step_bindings[i * 2 + 1];

		fwd->s = s;
		fwd->value = step_counts[i];
		snprintf(name, sizeof(name), "WarpMedia.StepForward%d", step_counts[i]);
		snprintf(text_key, sizeof(text_key), "Warp.Hotkey.Step.Forward%d", step_counts[i]);
		obs_hotkey_register_source(source, name, obs_module_text(text_key), warp_source_step_hotkey, fwd);

		back->s = s;
		back->value = -step_counts[i];
		snprintf(name, sizeof(name), "WarpMedia.StepBackward%d", step_counts[i]);
		snprintf(text_key, sizeof(text_key), "Warp.Hotkey.Step.Backward%d", step_counts[i]);
		obs_hotkey_register_source(source, name, obs_module_text(text_key), warp_source_step_hotkey, back);
	}
}

/* The zoom presets are made in the dock rather than through the properties, so
 * they are written out from the control the way the playlist writes out what
 * its transitions are set up with. */
static void warp_source_save(void *data, obs_data_t *settings)
{
	struct warp_source *s = data;

	warp_zoom_control_save(&s->zoom, settings);
}

/* ------------------------------------------------------------------------- */
/* lifetime */

static void *warp_source_create(obs_data_t *settings, obs_source_t *source)
{
	static const char *signals[] = {WARP_SIGNAL_DECL_SPEED_CHANGED, WARP_SIGNAL_DECL_FRAMES_STEPPED,
					WARP_SIGNAL_DECL_MEDIA_ACTION,  WARP_SIGNAL_DECL_ZOOM_CHANGED,
					WARP_SIGNAL_DECL_ZOOM_STAGED,   NULL};

	struct warp_source *s = bzalloc(sizeof(struct warp_source));
	s->source = source;

	signal_handler_add_array(obs_source_get_signal_handler(source), signals);

	// Manual type since the event can be signalled without an active thread
	if (os_event_init(&s->reconnect_stop_event, OS_EVENT_TYPE_MANUAL)) {
		FF_BLOG(LOG_ERROR, "Failed to initialize reconnect stop event");
		bfree(s);
		return NULL;
	}

	if (pthread_mutex_init(&s->reconnect_mutex, NULL)) {
		FF_BLOG(LOG_ERROR, "Failed to initialize reconnect mutex");
		os_event_destroy(s->reconnect_stop_event);
		bfree(s);
		return NULL;
	}

	if (pthread_mutex_init(&s->mutex, NULL)) {
		FF_BLOG(LOG_ERROR, "Failed to initialize mutex");
		pthread_mutex_destroy(&s->reconnect_mutex);
		os_event_destroy(s->reconnect_stop_event);
		bfree(s);
		return NULL;
	}

	struct mp_media_info info = {
		.opaque = s,
		.v_cb = get_frame,
		.v_preload_cb = preload_frame,
		.v_still_cb = still_frame,
		.a_cb = get_audio,
		.event_cb = media_event,
	};

	s->media = media_playback_create(&info);
	if (!s->media) {
		FF_BLOG(LOG_ERROR, "Failed to start the playback engine");
		pthread_mutex_destroy(&s->mutex);
		pthread_mutex_destroy(&s->reconnect_mutex);
		os_event_destroy(s->reconnect_stop_event);
		bfree(s);
		return NULL;
	}

	s->hotkey = obs_hotkey_register_source(source, "WarpMedia.Restart", obs_module_text("Warp.Hotkey.Restart"),
					       restart_hotkey, s);

	s->play_pause_hotkey = obs_hotkey_pair_register_source(s->source, "WarpMedia.Play",
							       obs_module_text("Warp.Hotkey.Play"), "WarpMedia.Pause",
							       obs_module_text("Warp.Hotkey.Pause"),
							       warp_source_play_hotkey, warp_source_pause_hotkey, s, s);

	s->stop_hotkey = obs_hotkey_register_source(source, "WarpMedia.Stop", obs_module_text("Warp.Hotkey.Stop"),
						    warp_source_stop_hotkey, s);

	warp_source_register_warp_hotkeys(s, source);

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void restart()", restart_proc, s);
	proc_handler_add(ph, "void get_duration(out int duration)", get_duration, s);
	proc_handler_add(ph, "void get_nb_frames(out int num_frames)", get_nb_frames, s);
	proc_handler_add(ph, "void " WARP_MEDIA_READY_PROC "(out bool ready, out bool failed, out int seq)",
			 media_ready_proc, s);
	proc_handler_add(ph, "void " WARP_MEDIA_CLOSE_PROC "()", media_close_proc, s);
	proc_handler_add(ph, "void warp_set_speed(int speed)", set_speed_proc, s);
	proc_handler_add(ph, "void warp_adjust_speed(int delta)", adjust_speed_proc, s);
	proc_handler_add(ph, "void warp_get_speed(out int speed)", get_speed_proc, s);
	proc_handler_add(ph, "void warp_step_frames(int frames)", step_frames_proc, s);
	proc_handler_add(ph,
			 "void " WARP_MEDIA_LOAD_PROC
			 "(string path, int speed, string playback, int in_ms, int out_ms, "
			 "int tail_ms, out int seq)",
			 warp_media_load_proc, s);

	warp_zoom_control_init(&s->zoom, source, "WarpMedia", true, false);
	warp_zoom_control_register_procs(&s->zoom, source);

	warp_source_update(s, settings);
	return s;
}

static void warp_source_destroy(void *data)
{
	struct warp_source *s = data;

	stop_reconnect_thread(s);

	/* the engine goes first: its thread calls back into the source */
	media_playback_destroy(s->media);

	warp_zoom_control_free(&s->zoom);

	if (s->zoom_filter)
		obs_source_release(s->zoom_filter);

	if (s->hotkey)
		obs_hotkey_unregister(s->hotkey);

	pthread_mutex_destroy(&s->mutex);
	pthread_mutex_destroy(&s->reconnect_mutex);
	os_event_destroy(s->reconnect_stop_event);
	config_free(&s->cfg);
	bfree(s);
}

static void warp_source_activate(void *data)
{
	struct warp_source *s = data;

	if (!s->restart_on_activate) {
		/* a source that let go of its file while it was off screen
		 * opens it again, ready to be started */
		if (s->close_when_inactive && !warp_source_loaded(s))
			warp_source_load(s, MP_START_CUE);
		return;
	}

	/* going on screen restarts playback by itself; obs_source_media_restart
	 * is still what does it, so OBS's own media_restart signal is emitted
	 * the way it is for any media source */
	s->internal_command = true;
	obs_source_media_restart(s->source);
	s->internal_command = false;
}

static void warp_source_deactivate(void *data)
{
	struct warp_source *s = data;

	if (s->restart_on_activate) {
		media_playback_stop(s->media);

		if (s->is_clear_on_media_end)
			clear_video(s);
	}

	if (s->close_when_inactive) {
		media_playback_close(s->media);
		pthread_mutex_lock(&s->mutex);
		s->load_seq = 0;
		pthread_mutex_unlock(&s->mutex);
	}
}

/* ------------------------------------------------------------------------- */
/* media controls */

static void warp_source_play_pause(void *data, bool pause)
{
	struct warp_source *s = data;

	if (!warp_source_loaded(s)) {
		if (pause)
			return;
		warp_source_load(s, MP_START_PLAY);
	} else {
		media_playback_pause(s->media, pause);
	}

	if (pause) {
		set_state(s, OBS_MEDIA_STATE_PAUSED);
	} else {
		set_state(s, OBS_MEDIA_STATE_PLAYING);
		obs_source_media_started(s->source);
	}

	if (!s->internal_command)
		warp_signal_media_action(s->source, pause ? WARP_MEDIA_ACTION_PAUSE : WARP_MEDIA_ACTION_PLAY);
}

static void warp_source_stop(void *data)
{
	struct warp_source *s = data;

	media_playback_stop(s->media);
	clear_video(s);
	set_state(s, OBS_MEDIA_STATE_STOPPED);
}

static void warp_source_restart(void *data)
{
	struct warp_source *s = data;

	if (obs_source_showing(s->source))
		warp_source_start(s);

	set_state(s, OBS_MEDIA_STATE_PLAYING);

	if (!s->internal_command)
		warp_signal_media_action(s->source, WARP_MEDIA_ACTION_RESTART);
}

static int64_t warp_source_get_duration(void *data)
{
	struct warp_source *s = data;
	struct mp_status st;

	media_playback_get_status(s->media, &st);
	return st.duration_ms;
}

static int64_t warp_source_get_time(void *data)
{
	struct warp_source *s = data;
	struct mp_status st;

	media_playback_get_status(s->media, &st);
	return st.time_ms;
}

static void warp_source_set_time(void *data, int64_t ms)
{
	struct warp_source *s = data;

	media_playback_seek(s->media, ms);

	/* a clip that was not playing is left on the frame it was moved to */
	enum obs_media_state state = get_state(s);
	if (state == OBS_MEDIA_STATE_STOPPED || state == OBS_MEDIA_STATE_ENDED)
		set_state(s, OBS_MEDIA_STATE_PAUSED);
}

static enum obs_media_state warp_source_get_state(void *data)
{
	struct warp_source *s = data;

	return get_state(s);
}

static void missing_file_callback(void *src, const char *new_path, void *data)
{
	struct warp_source *s = src;

	obs_source_t *source = s->source;
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_set_string(settings, "local_file", new_path);
	obs_source_update(source, settings);
	obs_data_release(settings);

	UNUSED_PARAMETER(data);
}

static obs_missing_files_t *warp_source_missingfiles(void *data)
{
	struct warp_source *s = data;
	obs_missing_files_t *files = obs_missing_files_create();

	pthread_mutex_lock(&s->mutex);
	if (s->cfg.is_local_file && s->cfg.input && *s->cfg.input && !os_file_exists(s->cfg.input)) {
		obs_missing_file_t *file = obs_missing_file_create(s->cfg.input, missing_file_callback,
								   OBS_MISSING_FILE_SOURCE, s->source, NULL);

		obs_missing_files_add_file(files, file);
	}
	pthread_mutex_unlock(&s->mutex);

	return files;
}

struct obs_source_info warp_media_source_info = {
	.id = "warp_media_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE |
			OBS_SOURCE_CONTROLLABLE_MEDIA,
	.get_name = warp_source_getname,
	.create = warp_source_create,
	.destroy = warp_source_destroy,
	.get_defaults = warp_source_defaults,
	.get_properties = warp_source_getproperties,
	.save = warp_source_save,
	.activate = warp_source_activate,
	.deactivate = warp_source_deactivate,
	.video_tick = warp_source_tick,
	.missing_files = warp_source_missingfiles,
	.update = warp_source_update,
	.icon_type = OBS_ICON_TYPE_MEDIA,
	.media_play_pause = warp_source_play_pause,
	.media_restart = warp_source_restart,
	.media_stop = warp_source_stop,
	.media_get_duration = warp_source_get_duration,
	.media_get_time = warp_source_get_time,
	.media_set_time = warp_source_set_time,
	.media_get_state = warp_source_get_state,
};
