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
#include <media-playback/media.h>

#include "warp-events.h"

#define FF_LOG_S(source, level, format, ...)      \
	blog(level, "[Warp Media '%s']: " format, \
	     obs_source_get_name(source), ##__VA_ARGS__)
#define FF_BLOG(level, format, ...) \
	FF_LOG_S(s->source, level, format, ##__VA_ARGS__)

struct warp_source;

/* per-hotkey context for parametrized hotkeys: 'value' is a signed frame
 * count for step hotkeys and a speed percentage for preset hotkeys */
struct warp_hotkey_binding {
	struct warp_source *s;
	int value;
};

struct warp_source {
	media_playback_t *media;
	bool destroy_media;

	enum video_range_type range;
	bool is_linear_alpha;
	obs_source_t *source;
	obs_hotkey_id hotkey;

	char *input;
	char *input_format;
	char *ffmpeg_options;
	int buffering_mb;
	int speed_percent;
	bool is_looping;
	bool is_local_file;
	bool is_hw_decoding;
	bool full_decode;
	bool is_clear_on_media_end;
	bool restart_on_activate;
	bool close_when_inactive;
	bool seekable;
	bool is_stinger;
	bool is_track_matte;
	bool log_changes;

	pthread_t reconnect_thread;
	pthread_mutex_t reconnect_mutex;
	bool reconnect_thread_valid;
	os_event_t *reconnect_stop_event;
	volatile bool reconnecting;
	int reconnect_delay_sec;

	/* Whether there is picture in the source's texture right now. libobs
	 * uploads a decoded frame to the texture only while the source is being
	 * rendered, so a source that is not on screen holds a texture that has
	 * been allocated and never written to, however many frames it has
	 * decoded -- and that texture renders as nothing at all. So "has
	 * something to show" has to mean the picture is in the texture, not
	 * that a frame has been handed over. Written from the media thread and
	 * from the graphics thread, read by warp_media_ready. */
	volatile bool has_picture;
	/* Whether the source is holding a frame handed to it ahead of being
	 * shown, so that obs_source_show_preloaded_video() has one to put up.
	 * Every call that hands one over sets this; clearing the source does not
	 * take it back, so it stays set once a frame has been handed over. */
	volatile bool preloaded_frame;

	enum obs_media_state state;
	obs_hotkey_pair_id play_pause_hotkey;
	obs_hotkey_id stop_hotkey;

	/* Set while the source drives its own playback instead of carrying out
	 * a command someone sent: restarting as it goes on screen, and the
	 * pause a frame step does first. The media action signal is about
	 * commands, so those are not reported as ones. Only ever set and
	 * cleared around the synchronous call it covers. */
	bool internal_command;

	/* A clip warp_media_load has handed the source and what playback still
	 * owes it. libobs defers a video source's settings update to its next
	 * tick, so the file is not open when the load returns: the load leaves
	 * these behind instead, and the tick carries them out once the update
	 * has been applied.
	 *
	 * start_pending is a clip that is to be played from the top rather than
	 * left to the source's own settings; hold_clip is one that is to be
	 * parked on its first frame, which it cannot be until it has one. Set
	 * from whichever thread handed the clip over and read on the graphics
	 * thread, so both are set and read atomically, and hold_clip is set
	 * first: the tick that sees a clip to start has to see how it is to be
	 * left as well. */
	volatile bool start_pending;
	volatile bool hold_clip;

	struct warp_hotkey_binding speed_bindings[WARP_NUM_SPEED_PRESETS];
	struct warp_hotkey_binding step_bindings[WARP_NUM_STEP_HOTKEYS];
};

// Used to safely cancel and join any active reconnect threads
// Use this to join any finished reconnect thread too!
static void stop_reconnect_thread(struct warp_source *s)
{
	if (s->is_local_file)
		return;
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

static void set_media_state(void *data, enum obs_media_state state)
{
	struct warp_source *s = data;
	s->state = state;
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

	if (s && s->input && *s->input) {
		const char *slash;

		dstr_copy(&path, s->input);
		dstr_replace(&path, "\\", "/");
		slash = strrchr(path.array, '/');
		if (slash)
			dstr_resize(&path, slash - path.array + 1);
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

static void dump_source_info(struct warp_source *s, const char *input, const char *input_format)
{
	if (!s->log_changes)
		return;
	FF_BLOG(LOG_INFO,
		"settings:\n"
		"\tinput:                   %s\n"
		"\tinput_format:            %s\n"
		"\tspeed:                   %d\n"
		"\tis_looping:              %s\n"
		"\tis_linear_alpha:         %s\n"
		"\tis_hw_decoding:          %s\n"
		"\tis_clear_on_media_end:   %s\n"
		"\trestart_on_activate:     %s\n"
		"\tclose_when_inactive:     %s\n"
		"\tfull_decode:             %s\n"
		"\tffmpeg_options:          %s",
		input ? input : "(null)", input_format ? input_format : "(null)", s->speed_percent,
		s->is_looping ? "yes" : "no", s->is_linear_alpha ? "yes" : "no", s->is_hw_decoding ? "yes" : "no",
		s->is_clear_on_media_end ? "yes" : "no", s->restart_on_activate ? "yes" : "no",
		s->close_when_inactive ? "yes" : "no", s->full_decode ? "yes" : "no", s->ffmpeg_options);
}

/* Drops the picture the source is holding. Every place that clears the source
 * goes through here, so that s->has_picture cannot drift from what is actually
 * in the source's texture. */
static void clear_video(struct warp_source *s)
{
	os_atomic_set_bool(&s->has_picture, false);
	obs_source_output_video(s->source, NULL);
}

static void get_frame(void *opaque, struct obs_source_frame *f)
{
	struct warp_source *s = opaque;

	/* The first frame of a run is written into the source's texture here
	 * rather than left to the render path. libobs uploads an async frame
	 * only while the source is being rendered, and only on a tick that has
	 * a frame ready for it, so a source that has decoded picture but has
	 * not been rendered yet still has an empty texture -- and libobs draws
	 * that texture all the same, because the source counts as having video.
	 * The Warp Playlist opens the file it is switching to off screen and
	 * hands it straight to its transition, so an item has to have real
	 * picture in it before it is composited, not merely a frame that has
	 * been handed over. Only the first frame pays for this; every one after
	 * it is uploaded by the render path as usual. */
	if (!os_atomic_load_bool(&s->has_picture)) {
		obs_source_set_video_frame(s->source, f);
		os_atomic_set_bool(&s->preloaded_frame, true);
		os_atomic_set_bool(&s->has_picture, true);
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

	if (!s->is_local_file && os_atomic_set_bool(&s->reconnecting, false))
		FF_BLOG(LOG_INFO, "Reconnected.");
}

static void seek_frame(void *opaque, struct obs_source_frame *f)
{
	struct warp_source *s = opaque;

	/* a seek puts its frame straight into the texture, and leaves it with the
	 * source as the one to put back up when playback resumes */
	obs_source_set_video_frame(s->source, f);
	os_atomic_set_bool(&s->preloaded_frame, true);
	os_atomic_set_bool(&s->has_picture, true);
}

static void get_audio(void *opaque, struct obs_source_audio *a)
{
	struct warp_source *s = opaque;
	obs_source_output_audio(s->source, a);

	if (!s->is_local_file && os_atomic_set_bool(&s->reconnecting, false))
		FF_BLOG(LOG_INFO, "Reconnected.");
}

/* Playback has resumed and its timestamps have been re-anchored to the current
 * clock; the frame seek_frame() was just handed is the one the source times
 * everything that follows against. Bring the audio clock to the same anchor:
 * it is timed against the frames rather than against a sample of its own, so
 * without this the audio keeps the offset the pause introduced for the rest of
 * the file. */
static void media_resumed(void *opaque)
{
	struct warp_source *s = opaque;

	obs_source_show_preloaded_video(s->source);
}

static void media_stopped(void *opaque)
{
	struct warp_source *s = opaque;
	if (s->is_clear_on_media_end && !s->is_track_matte) {
		clear_video(s);
	}

	if ((s->close_when_inactive || !s->is_local_file) && s->media)
		s->destroy_media = true;

	if (s->state != OBS_MEDIA_STATE_STOPPED) {
		set_media_state(s, OBS_MEDIA_STATE_ENDED);
		obs_source_media_ended(s->source);
	}
}

static void warp_source_open(struct warp_source *s)
{
	if (s->input && *s->input) {
		struct mp_media_info info = {
			.opaque = s,
			.v_cb = get_frame,
			.v_preload_cb = preload_frame,
			.v_seek_cb = seek_frame,
			.a_cb = get_audio,
			.stop_cb = media_stopped,
			.resume_cb = media_resumed,
			.path = s->input,
			.format = s->input_format,
			.buffering = s->buffering_mb * 1024 * 1024,
			.speed = s->speed_percent,
			.force_range = s->range,
			.is_linear_alpha = s->is_linear_alpha,
			.hardware_decoding = s->is_hw_decoding,
			.ffmpeg_options = s->ffmpeg_options,
			.is_local_file = s->is_local_file || s->seekable,
			.reconnecting = s->reconnecting,
			.request_preload = s->is_stinger,
			.full_decode = s->full_decode,
		};

		s->media = media_playback_create(&info);
	}
}

static void warp_source_start(struct warp_source *s)
{
	if (!s->media)
		warp_source_open(s);

	if (!s->media)
		return;

	media_playback_play(s->media, s->is_looping, s->reconnecting);

	if (s->is_local_file && media_playback_has_video(s->media) && (s->is_clear_on_media_end || s->is_looping)) {
		/* the frame decoded when the file was opened, so there is
		 * picture from the moment playback starts */
		if (os_atomic_load_bool(&s->preloaded_frame)) {
			obs_source_show_preloaded_video(s->source);
			os_atomic_set_bool(&s->has_picture, true);
		}
	} else if (!s->is_local_file || !os_atomic_load_bool(&s->has_picture)) {
		/* Nothing to put up yet, and nothing worth keeping. A local file
		 * that is holding picture keeps it instead: a source set not to
		 * clear at the end of a file has no reason to blank while the
		 * file is reopened and its first frame decoded, which is what
		 * restarting an item the playlist is showing used to do. */
		clear_video(s);
	}

	set_media_state(s, OBS_MEDIA_STATE_PLAYING);
	obs_source_media_started(s->source);
}

static void *warp_source_reconnect(void *data)
{
	struct warp_source *s = data;

	int ret = os_event_timedwait(s->reconnect_stop_event, s->reconnect_delay_sec * 1000);
	if (ret == 0 || s->media)
		return NULL;

	bool active = obs_source_active(s->source);
	if (!s->close_when_inactive || active)
		warp_source_open(s);

	if (!s->restart_on_activate || active)
		warp_source_start(s);

	return NULL;
}

/* Whether the source has something to put on screen yet. Opening a file is
 * asynchronous: the source exists as soon as it is created, but its media
 * thread still has to read the header and decode a frame before anything can
 * be rendered from it, and until then the source draws nothing at all. The
 * Warp Playlist waits for this before handing an item to its transition, so a
 * switch never starts against a source that is still empty, and a held clip
 * waits for it before being parked on its first frame. */
static bool warp_source_ready(struct warp_source *s)
{
	if (!s->media || !media_playback_is_open(s->media))
		/* Nothing the media says about itself means anything until its
		 * streams have been worked out. In particular it reports no
		 * video, because it does not know yet that it has any, and a
		 * duration, because a container gives that up as soon as its
		 * header has been read -- which together used to read as "an
		 * audio-only file, ready to go" for the first few frames of
		 * every video file that was opened. */
		return false;

	if (!media_playback_has_video(s->media))
		/* audio only: there is no picture to wait for */
		return true;

	/* The picture is there once a decoded frame has been written into the
	 * source's texture. A non-zero width is not enough on its own: a source
	 * that is off screen reports the size of a frame it has decoded while
	 * its texture is still empty. */
	return os_atomic_load_bool(&s->has_picture);
}

/* Parks a clip that was loaded to be held on its first frame, once the file has
 * opened far enough to have one. libobs carries the pause and the seek out on
 * the media's own thread, so this is done from the tick rather than from the
 * call that asked for the clip to be held. */
static void warp_source_hold_tick(struct warp_source *s)
{
	if (!warp_source_ready(s))
		return;

	/* the source is holding the clip, not being told to pause by anyone: the
	 * media action signal is about commands */
	s->internal_command = true;
	obs_source_media_play_pause(s->source, true);
	s->internal_command = false;

	media_playback_seek(s->media, 0);

	os_atomic_set_bool(&s->hold_clip, false);
}

static void warp_source_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);

	struct warp_source *s = data;

	/* A clip that was loaded to be played or held. libobs applies a deferred
	 * settings update just before this tick, so the file the load asked for
	 * is the one being started here. */
	if (os_atomic_set_bool(&s->start_pending, false)) {
		/* A held clip is parked on its own first frame, so the picture
		 * of the one before it goes now: the source holds on to what it
		 * has while a new file opens, and the hold would otherwise park
		 * on that rather than waiting for the clip it was given. */
		if (os_atomic_load_bool(&s->hold_clip))
			clear_video(s);

		warp_source_start(s);
	}

	if (os_atomic_load_bool(&s->hold_clip))
		warp_source_hold_tick(s);

	if (s->destroy_media) {
		if (s->media) {
			media_playback_destroy(s->media);
			s->media = NULL;
		}

		s->destroy_media = false;

		if (!s->is_local_file) {
			pthread_mutex_lock(&s->reconnect_mutex);
			if (!os_atomic_set_bool(&s->reconnecting, true))
				FF_BLOG(LOG_WARNING, "Disconnected. "
						     "Reconnecting...");
			if (s->reconnect_thread_valid) {
				os_event_signal(s->reconnect_stop_event);
				pthread_join(s->reconnect_thread, NULL);
				s->reconnect_thread_valid = false;
				os_event_reset(s->reconnect_stop_event);
			}
			if (pthread_create(&s->reconnect_thread, NULL, warp_source_reconnect, s) != 0) {
				FF_BLOG(LOG_WARNING, "Could not create "
						     "reconnect thread");
				pthread_mutex_unlock(&s->reconnect_mutex);
				return;
			}
			s->reconnect_thread_valid = true;
			pthread_mutex_unlock(&s->reconnect_mutex);
		}
	}
}

#define RIST_PROTO "rist"

static void warp_source_update(void *data, obs_data_t *settings)
{
	struct warp_source *s = data;

	/* zero until the first update, which is the one create() makes: the
	 * speed a source starts at is not a change anyone can react to */
	int prev_speed = s->speed_percent;
	bool active = obs_source_active(s->source);
	bool is_local_file = obs_data_get_bool(settings, "is_local_file");
	bool is_stinger = obs_data_get_bool(settings, "is_stinger");
	bool is_track_matte = obs_data_get_bool(settings, "is_track_matte");
	bool should_restart_media = (is_local_file != s->is_local_file) || (is_stinger != s->is_stinger);

	const char *input;
	const char *input_format;
	const char *ffmpeg_options;

	bool is_hw_decoding;
	enum video_range_type range;
	bool is_linear_alpha;
	int speed_percent;
	bool is_looping;

	bfree(s->input_format);

	if (is_local_file) {
		input = obs_data_get_string(settings, "local_file");
		input_format = NULL;
		is_looping = obs_data_get_bool(settings, "looping");

		if (s->input && !should_restart_media)
			should_restart_media |= strcmp(s->input, input) != 0;
	} else {
		should_restart_media = true;
		input = obs_data_get_string(settings, "input");
		input_format = obs_data_get_string(settings, "input_format");
		s->reconnect_delay_sec = (int)obs_data_get_int(settings, "reconnect_delay_sec");
		s->reconnect_delay_sec = s->reconnect_delay_sec == 0 ? 10 : s->reconnect_delay_sec;
		is_looping = false;
	}

	stop_reconnect_thread(s);

	is_hw_decoding = obs_data_get_bool(settings, "hw_decode");
	range = obs_data_get_int(settings, "color_range");
	speed_percent = (int)obs_data_get_int(settings, "speed_percent");
	if (speed_percent < MP_SPEED_MIN || speed_percent > MP_SPEED_MAX)
		speed_percent = 100;
	ffmpeg_options = obs_data_get_string(settings, "ffmpeg_options");

	/* Restart media source if these properties are changed (speed is
	 * intentionally absent: it is applied to running playback below) */
	if (s->is_hw_decoding != is_hw_decoding || s->range != range ||
	    (s->ffmpeg_options && strcmp(s->ffmpeg_options, ffmpeg_options) != 0))
		should_restart_media = true;

	/* If media has ended and user enables looping, user expects that it restarts.
	 * Should still check if is_looping was changed, because users may stop them
	 * intentionally, which is why we only check for ENDED and not STOPPED. */
	if (active && s->state == OBS_MEDIA_STATE_ENDED && is_looping == true && s->is_looping == false) {
		should_restart_media = true;
	}

	bfree(s->input);
	bfree(s->ffmpeg_options);

	s->is_looping = is_looping;
	s->close_when_inactive = obs_data_get_bool(settings, "close_when_inactive");
	s->input = input ? bstrdup(input) : NULL;
	s->input_format = input_format ? bstrdup(input_format) : NULL;
	s->is_hw_decoding = is_hw_decoding;
	s->full_decode = obs_data_get_bool(settings, "full_decode");
	s->is_clear_on_media_end = obs_data_get_bool(settings, "clear_on_media_end");
	s->restart_on_activate = !astrcmpi_n(input, RIST_PROTO, sizeof(RIST_PROTO) - 1)
					 ? false
					 : obs_data_get_bool(settings, "restart_on_activate");
	s->range = range;
	is_linear_alpha = obs_data_get_bool(settings, "linear_alpha");
	s->is_linear_alpha = is_linear_alpha;
	s->buffering_mb = (int)obs_data_get_int(settings, "buffering_mb");
	s->speed_percent = speed_percent;
	s->is_local_file = is_local_file;
	s->seekable = obs_data_get_bool(settings, "seekable");
	s->ffmpeg_options = ffmpeg_options ? bstrdup(ffmpeg_options) : NULL;
	s->is_stinger = is_stinger;
	s->is_track_matte = is_track_matte;
	s->log_changes = obs_data_get_bool(settings, "log_changes");

	if (s->speed_percent < MP_SPEED_MIN || s->speed_percent > MP_SPEED_MAX)
		s->speed_percent = 100;

	if (s->media && should_restart_media) {
		media_playback_destroy(s->media);
		s->media = NULL;
	}

	/* directly set options if media is playing */
	if (s->media) {
		media_playback_set_looping(s->media, is_looping);
		media_playback_set_is_linear_alpha(s->media, is_linear_alpha);
		media_playback_set_speed(s->media, speed_percent);
	}
	if ((!s->close_when_inactive || active) && should_restart_media)
		warp_source_open(s);

	dump_source_info(s, input, input_format);

	/* A clip that was loaded to be played or held is started by the tick
	 * this update is being applied on, from the top and whether or not the
	 * source is on screen, so it is not started here as well. */
	if ((!s->restart_on_activate || active) && should_restart_media && !os_atomic_load_bool(&s->start_pending))
		warp_source_start(s);

	/* the Speed property applies live, so it is a speed change like any
	 * hotkey-driven one */
	if (prev_speed && prev_speed != s->speed_percent)
		warp_signal_speed_changed(s->source, s->speed_percent, prev_speed, WARP_SPEED_CHANGE_SET);
}

static const char *warp_source_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Warp.MediaSource.Name");
}

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

static void preload_first_frame_proc(void *data, calldata_t *cd)
{
	struct warp_source *s = data;
	if (s->is_track_matte)
		clear_video(s);
	media_playback_preload_frame(s->media);
	UNUSED_PARAMETER(cd);
}

static void get_duration(void *data, calldata_t *cd)
{
	struct warp_source *s = data;
	int64_t dur = 0;
	if (s->media)
		dur = media_playback_get_duration(s->media);

	calldata_set_int(cd, "duration", dur * 1000);
}

static void get_nb_frames(void *data, calldata_t *cd)
{
	struct warp_source *s = data;
	int64_t frames = media_playback_get_frames(s->media);
	calldata_set_int(cd, "num_frames", frames);
}

static void media_ready_proc(void *data, calldata_t *cd)
{
	calldata_set_bool(cd, "ready", warp_source_ready(data));
}

static bool warp_source_play_hotkey(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return false;

	struct warp_source *s = data;

	if (s->state == OBS_MEDIA_STATE_PLAYING || !obs_source_showing(s->source))
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

	if (s->state != OBS_MEDIA_STATE_PLAYING || !obs_source_showing(s->source))
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

/* 'change' is one of the WARP_SPEED_CHANGE_* kinds, and says how the speed was
 * asked to move: a listener can tell a preset from a step that happens to land
 * on the same value */
static void warp_source_apply_speed(struct warp_source *s, int speed, const char *change)
{
	if (speed < MP_SPEED_MIN)
		speed = MP_SPEED_MIN;
	else if (speed > MP_SPEED_MAX)
		speed = MP_SPEED_MAX;

	if (!s->is_local_file || speed == s->speed_percent)
		return;

	int prev_speed = s->speed_percent;

	s->speed_percent = speed;

	if (s->media)
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

	warp_source_apply_speed(s, s->speed_percent + WARP_SPEED_STEP, WARP_SPEED_CHANGE_INCREASED);
}

static void warp_source_speed_down_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct warp_source *s = data;

	if (!pressed || !obs_source_showing(s->source))
		return;

	warp_source_apply_speed(s, s->speed_percent - WARP_SPEED_STEP, WARP_SPEED_CHANGE_DECREASED);
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
	if (!s->media || !s->is_local_file)
		return;

	/* stepping while playing pauses first, then steps */
	if (s->state == OBS_MEDIA_STATE_PLAYING) {
		s->internal_command = true;
		obs_source_media_play_pause(s->source, true);
		s->internal_command = false;
	} else if (s->state != OBS_MEDIA_STATE_PAUSED) {
		return;
	}

	media_playback_step_frames(s->media, frames);

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

/* Procs used by the Warp Playlist source to drive its private media items, and
 * by the obs-websocket vendor requests to drive the source from outside OBS.
 * They apply the same changes the hotkeys do, and report them the same way. */
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

	warp_source_apply_speed(s, s->speed_percent + (int)delta,
				delta > 0 ? WARP_SPEED_CHANGE_INCREASED : WARP_SPEED_CHANGE_DECREASED);
}

static void get_speed_proc(void *data, calldata_t *cd)
{
	struct warp_source *s = data;

	calldata_set_int(cd, "speed", s->speed_percent);
}

static void step_frames_proc(void *data, calldata_t *cd)
{
	long long frames;

	if (calldata_get_int(cd, "frames", &frames))
		warp_source_do_step(data, (int)frames);
}

/* Puts a file in the source and says what playback is to do with it, which is
 * what a Warp Instant Replay flow calls when a clip lands. The file goes in
 * through the source's settings, the way changing it in the properties does, so
 * it is saved with the scene collection and the source reopens it like any
 * other change of file. */
static void warp_media_load_proc(void *data, calldata_t *cd)
{
	struct warp_source *s = data;
	const char *path = NULL;
	const char *playback = NULL;
	long long speed = 0;

	if (!calldata_get_string(cd, "path", &path) || !path || !*path) {
		FF_BLOG(LOG_WARNING, "asked to load a clip without saying which");
		return;
	}

	calldata_get_string(cd, "playback", &playback);
	calldata_get_int(cd, "speed", &speed);

	if (!playback || !*playback)
		playback = WARP_MEDIA_LOAD_KEEP;

	const bool play = strcmp(playback, WARP_MEDIA_LOAD_PLAY) == 0;
	const bool hold = strcmp(playback, WARP_MEDIA_LOAD_HOLD) == 0;

	obs_data_t *settings = obs_source_get_settings(s->source);

	/* a clip is a file on disk, whatever the source was pointed at before */
	if (!s->is_local_file)
		FF_BLOG(LOG_INFO, "loading a clip, so the source is no longer reading '%s'", s->input ? s->input : "");

	obs_data_set_bool(settings, "is_local_file", true);
	obs_data_set_string(settings, "local_file", path);

	if (speed >= MP_SPEED_MIN && speed <= MP_SPEED_MAX)
		obs_data_set_int(settings, "speed_percent", (int)speed);

	obs_source_update(s->source, settings);
	obs_data_release(settings);

	/* Playback is left to the update and the source's own settings unless
	 * the clip was asked to be played or held; both of those are started by
	 * the tick that applies the update, whether or not the source is on
	 * screen, because a clip that is not decoding cannot play now and has no
	 * frame to be held on. */
	os_atomic_set_bool(&s->hold_clip, hold);
	os_atomic_set_bool(&s->start_pending, play || hold);

	FF_BLOG(LOG_INFO, "loaded '%s' (%s)", path, playback);

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

static void *warp_source_create(obs_data_t *settings, obs_source_t *source)
{
	static const char *signals[] = {WARP_SIGNAL_DECL_SPEED_CHANGED, WARP_SIGNAL_DECL_FRAMES_STEPPED,
					WARP_SIGNAL_DECL_MEDIA_ACTION, NULL};

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
	proc_handler_add(ph, "void preload_first_frame()", preload_first_frame_proc, s);
	proc_handler_add(ph, "void get_duration(out int duration)", get_duration, s);
	proc_handler_add(ph, "void get_nb_frames(out int num_frames)", get_nb_frames, s);
	proc_handler_add(ph, "void warp_media_ready(out bool ready)", media_ready_proc, s);
	proc_handler_add(ph, "void warp_set_speed(int speed)", set_speed_proc, s);
	proc_handler_add(ph, "void warp_adjust_speed(int delta)", adjust_speed_proc, s);
	proc_handler_add(ph, "void warp_get_speed(out int speed)", get_speed_proc, s);
	proc_handler_add(ph, "void warp_step_frames(int frames)", step_frames_proc, s);
	proc_handler_add(ph, "void " WARP_MEDIA_LOAD_PROC "(string path, int speed, string playback)",
			 warp_media_load_proc, s);

	warp_source_update(s, settings);
	return s;
}

static void warp_source_destroy(void *data)
{
	struct warp_source *s = data;

	stop_reconnect_thread(s);

	if (s->hotkey)
		obs_hotkey_unregister(s->hotkey);
	if (s->media)
		media_playback_destroy(s->media);

	pthread_mutex_destroy(&s->reconnect_mutex);
	os_event_destroy(s->reconnect_stop_event);
	bfree(s->input);
	bfree(s->input_format);
	bfree(s->ffmpeg_options);
	bfree(s);
}

static void warp_source_activate(void *data)
{
	struct warp_source *s = data;

	if (!s->restart_on_activate)
		return;

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
		if (s->media) {
			media_playback_stop(s->media);

			if (s->is_clear_on_media_end)
				clear_video(s);
		}
	}
}

/* A clip that was loaded to be played or held is waiting on the tick, which is
 * long enough for the operator to get in first. Whatever they asked for is what
 * the source does: the load does not come back and take playback off them. */
static void warp_source_drop_pending_load(struct warp_source *s)
{
	if (s->internal_command)
		return;

	os_atomic_set_bool(&s->start_pending, false);
	os_atomic_set_bool(&s->hold_clip, false);
}

static void warp_source_play_pause(void *data, bool pause)
{
	struct warp_source *s = data;

	warp_source_drop_pending_load(s);

	if (!s->media)
		warp_source_open(s);

	if (!s->media)
		return;

	media_playback_play_pause(s->media, pause);

	if (pause) {

		set_media_state(s, OBS_MEDIA_STATE_PAUSED);
	} else {

		set_media_state(s, OBS_MEDIA_STATE_PLAYING);
		obs_source_media_started(s->source);
	}

	if (!s->internal_command)
		warp_signal_media_action(s->source, pause ? WARP_MEDIA_ACTION_PAUSE : WARP_MEDIA_ACTION_PLAY);
}

static void warp_source_stop(void *data)
{
	struct warp_source *s = data;

	warp_source_drop_pending_load(s);

	if (s->media) {
		media_playback_stop(s->media);
		clear_video(s);
		set_media_state(s, OBS_MEDIA_STATE_STOPPED);
	}
}

static void warp_source_restart(void *data)
{
	struct warp_source *s = data;

	warp_source_drop_pending_load(s);

	if (obs_source_showing(s->source))
		warp_source_start(s);

	set_media_state(s, OBS_MEDIA_STATE_PLAYING);

	if (!s->internal_command)
		warp_signal_media_action(s->source, WARP_MEDIA_ACTION_RESTART);
}

static int64_t warp_source_get_duration(void *data)
{
	struct warp_source *s = data;
	int64_t dur = 0;

	if (s->media)
		dur = media_playback_get_duration(s->media) / INT64_C(1000);

	return dur;
}

static int64_t warp_source_get_time(void *data)
{
	struct warp_source *s = data;

	return media_playback_get_current_time(s->media);
}

static void warp_source_set_time(void *data, int64_t ms)
{
	struct warp_source *s = data;

	if (!s->media)
		return;

	media_playback_seek(s->media, ms);
}

static enum obs_media_state warp_source_get_state(void *data)
{
	struct warp_source *s = data;

	return s->state;
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

	if (s->is_local_file && strcmp(s->input, "") != 0) {
		if (!os_file_exists(s->input)) {
			obs_missing_file_t *file = obs_missing_file_create(s->input, missing_file_callback,
									   OBS_MISSING_FILE_SOURCE, s->source, NULL);

			obs_missing_files_add_file(files, file);
		}
	}

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
