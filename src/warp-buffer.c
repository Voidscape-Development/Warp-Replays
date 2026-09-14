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
#include <math.h>
#include <string.h>

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <libavformat/avformat.h>

#include <plugin-support.h>

#include "warp-angles.h"
#include "warp-buffer.h"

#define WARP_BUFFER_LOG(level, format, ...) blog(level, "[Warp Buffer]: " format, ##__VA_ARGS__)

/* How long a save waits for the angles that have not reported yet. Writing a
 * clip out takes a moment, and every angle is writing at once, so this is
 * generous; it is here so that an angle whose encoder has died does not leave
 * the rest of the set waiting for it for ever. A save that runs out of time is
 * handed over with the angles it did get. */
#define WARP_BUFFER_SAVE_TIMEOUT_NS (30ULL * 1000000000ULL)

/* what a save is worth in memory when the profile does not say what it records
 * at, in kilobits per second: enough not to under-report a buffer's cost by an
 * order of magnitude, which is all the estimate is for */
#define WARP_BUFFER_ASSUMED_KBPS 8000

struct warp_buffer;

/* One feed of one buffer: a picture to encode, and the encoder that does it.
 *
 * The program angle reads the mix OBS is already making, so it costs an
 * encoder and nothing else. A source angle has a mix of its own - a view with
 * the source on it, rendered at the source's own size - so that it is held
 * whatever scene is up and whether or not anyone can see it, which is the
 * whole point of hanging a buffer on a player cam. */
struct warp_buffer_angle {
	char *id;
	char *name;
	bool program;
	char *source_uuid;
	char *source_name;

	/* set up while the buffer is running */
	obs_source_t *source;
	obs_view_t *view;
	video_t *video;
	obs_encoder_t *encoder;
};

/* One length of one angle: the seconds it holds, and the output holding them.
 *
 * Every one of these is a replay buffer in its own right, so asking for five
 * seconds writes the five seconds that were kept rather than a longer clip cut
 * down. They share the angle's encoder between them - libobs hands one
 * encoder's packets to every output listening - so a buffer costs one encoder
 * per angle however many lengths it offers. */
struct warp_buffer_out {
	/* what the output's saved signal is handed, so that a signal arriving
	 * as the buffer is being torn down looks up an id that is no longer
	 * there rather than following a pointer that is no longer there */
	uint64_t id;
	size_t angle;
	int seconds;
	obs_output_t *output;
};

/* A save that has been asked for and is still being written. */
struct warp_buffer_pending {
	int seconds;
	/* how many angles are writing, and when they were told to */
	size_t expected;
	uint64_t started_ns;
	char *claim_flow_id;
	/* where each angle put its clip, filled in as they report */
	char *paths[WARP_BUFFER_MAX_ANGLES];
	size_t filled;
};

struct warp_buffer {
	/* everything about the buffer, and exactly what it is saved as */
	obs_data_t *config;
	bool running;

	DARRAY(struct warp_buffer_angle) angles;
	DARRAY(struct warp_buffer_out) outs;
	DARRAY(struct warp_buffer_pending) pending;

	/* the sound every angle carries, which is the programme mix: one
	 * encoder for the buffer rather than one per angle, since it is the
	 * same audio on all of them */
	obs_encoder_t *audio;

	/* one per length, in the order the lengths are configured */
	DARRAY(obs_hotkey_id) save_hotkeys;
	obs_hotkey_id toggle_hotkey;
};

/* Guards the buffer list and everything hanging off it.
 *
 * Buffers are set up and taken down from the UI thread, but a save is reported
 * on the muxer's own thread, so the list is locked for both. Nothing that
 * starts or stops an output is done with the lock held: that reaches back into
 * libobs and takes its locks, so the work is decided under the lock and
 * carried out once it has been dropped. */
static pthread_mutex_t warp_buffer_mutex;
static bool warp_buffer_ready = false;
static DARRAY(struct warp_buffer *) warp_buffers;

static uint64_t warp_buffer_id_seq = 0;
static uint64_t warp_buffer_out_seq = 0;

/* how many saves are waiting on angles, so the sweep that gives up on them
 * costs an atomic read a frame while nothing is being saved */
static volatile long warp_buffer_pending_count = 0;

static warp_buffer_clip_t warp_buffer_clip_handler = NULL;
static void *warp_buffer_clip_param = NULL;
static warp_buffer_changed_t warp_buffer_changed_handler = NULL;
static void *warp_buffer_changed_param = NULL;

/* What a buffer that has stopped hands back to libobs.
 *
 * It is taken out from under the lock and let go once the lock has been
 * dropped, because letting it go cannot be done while the lock is held:
 * disconnecting an output's saved signal waits for a save that is being
 * reported this instant, and that report is waiting for this very lock. Held
 * both ways round, neither ever arrives.
 *
 * So every path that stops a buffer detaches what it was using into one of
 * these, drops the lock, and releases it. */
struct warp_buffer_teardown {
	DARRAY(struct warp_buffer_out) outs;
	DARRAY(obs_encoder_t *) encoders;
	DARRAY(obs_view_t *) views;
	DARRAY(obs_source_t *) sources;
};

static void warp_buffer_detach_locked(struct warp_buffer *buffer, struct warp_buffer_teardown *teardown);
static bool warp_buffer_start_locked(struct warp_buffer *buffer, struct warp_buffer_teardown *teardown);
static void warp_buffer_saved_cb(void *data, calldata_t *cd);

static void warp_buffer_teardown_init(struct warp_buffer_teardown *teardown)
{
	da_init(teardown->outs);
	da_init(teardown->encoders);
	da_init(teardown->views);
	da_init(teardown->sources);
}

/* Hands everything back, in an order that matters.
 *
 * The outputs go first: releasing one blocks until it has stopped writing and
 * has let go of the encoders it was reading, so by the time they are all gone
 * no encoder is being fed. The encoders go next, and only then the views -
 * because a view being removed has its mix, and the video the encoders were
 * reading from it, freed by the graphics thread on the very next frame, and an
 * encoder still holding that video would be reading freed memory.
 *
 * Expects the lock not to be held. */
static void warp_buffer_teardown_release(struct warp_buffer_teardown *teardown)
{
	for (size_t i = 0; i < teardown->outs.num; i++) {
		struct warp_buffer_out *out = &teardown->outs.array[i];

		/* off the signal first: a clip that finishes writing while the
		 * buffer is coming down has nothing left to report to */
		signal_handler_disconnect(obs_output_get_signal_handler(out->output), "saved", warp_buffer_saved_cb,
					  (void *)(uintptr_t)out->id);
		obs_output_stop(out->output);
		obs_output_release(out->output);
	}

	for (size_t i = 0; i < teardown->encoders.num; i++)
		obs_encoder_release(teardown->encoders.array[i]);

	for (size_t i = 0; i < teardown->views.num; i++) {
		obs_view_t *view = teardown->views.array[i];

		obs_view_set_source(view, 0, NULL);
		obs_view_remove(view);
		obs_view_destroy(view);
	}

	for (size_t i = 0; i < teardown->sources.num; i++)
		obs_source_release(teardown->sources.array[i]);

	da_free(teardown->outs);
	da_free(teardown->encoders);
	da_free(teardown->views);
	da_free(teardown->sources);
}

/* ------------------------------------------------------------------------- */
/* small helpers */

static obs_data_t *warp_buffer_data_copy(obs_data_t *data)
{
	obs_data_t *copy;

	if (!data)
		return NULL;

	copy = obs_data_create();
	obs_data_apply(copy, data);

	return copy;
}

static inline bool warp_buffer_str_eq(const char *a, const char *b)
{
	return a && b && strcmp(a, b) == 0;
}

static char *warp_buffer_make_id(const char *prefix)
{
	struct dstr id = {0};

	dstr_printf(&id, "%s_%" PRIx64 "_%" PRIx64, prefix, (uint64_t)os_gettime_ns(), ++warp_buffer_id_seq);

	return id.array;
}

/* expects the lock to be held */
static struct warp_buffer *warp_buffer_find(const char *id)
{
	if (!id || !*id)
		return NULL;

	for (size_t i = 0; i < warp_buffers.num; i++) {
		struct warp_buffer *buffer = warp_buffers.array[i];

		if (warp_buffer_str_eq(obs_data_get_string(buffer->config, WARP_BUFFER_ID), id))
			return buffer;
	}

	return NULL;
}

static void warp_buffer_notify_changed(void)
{
	if (warp_buffer_changed_handler)
		warp_buffer_changed_handler(warp_buffer_changed_param);
}

/* The lengths a buffer offers, in the order they are configured, with
 * duplicates and values outside the range dropped. Answers how many were
 * taken. */
static size_t warp_buffer_lengths(obs_data_t *config, int *out, size_t max)
{
	obs_data_array_t *array = obs_data_get_array(config, WARP_BUFFER_LENGTHS);
	size_t taken = 0;

	if (!array)
		return 0;

	size_t count = obs_data_array_count(array);

	for (size_t i = 0; i < count && taken < max; i++) {
		obs_data_t *item = obs_data_array_item(array, i);
		int seconds = (int)obs_data_get_int(item, WARP_BUFFER_LENGTH_SECONDS);

		obs_data_release(item);

		if (seconds < WARP_BUFFER_LENGTH_MIN || seconds > WARP_BUFFER_LENGTH_MAX)
			continue;

		bool already = false;

		for (size_t j = 0; j < taken; j++)
			already |= out[j] == seconds;

		if (!already)
			out[taken++] = seconds;
	}

	obs_data_array_release(array);

	return taken;
}

/* ------------------------------------------------------------------------- */
/* what the profile records at
 *
 * A Warp buffer holds the same picture OBS would have recorded, so its encoder
 * is built from the recording settings of the profile that is loaded rather
 * than from settings of its own. That means reading the two shapes those
 * settings come in - Simple, where a handful of values stand for a preset, and
 * Advanced, where the encoder's own settings are written out beside the
 * profile - and coming out of both with an encoder id, its settings, a
 * container and a folder. */

struct warp_buffer_recipe {
	const char *video_id;
	obs_data_t *video_settings;
	const char *audio_id;
	obs_data_t *audio_settings;
	struct dstr directory;
	struct dstr extension;
	struct dstr muxer_settings;
	struct dstr filename;
};

static void warp_buffer_recipe_free(struct warp_buffer_recipe *recipe)
{
	obs_data_release(recipe->video_settings);
	obs_data_release(recipe->audio_settings);
	dstr_free(&recipe->directory);
	dstr_free(&recipe->extension);
	dstr_free(&recipe->muxer_settings);
	dstr_free(&recipe->filename);
}

/* whether this OBS has an encoder of that id, which is how the mapping below
 * picks between the two builds of NVENC that ship under different names */
static bool warp_buffer_encoder_available(const char *id)
{
	const char *found;

	for (size_t i = 0; obs_enum_encoder_types(i, &found); i++) {
		if (strcmp(found, id) == 0)
			return true;
	}

	return false;
}

/* the encoder ids the Simple output mode's short names stand for, which is the
 * same mapping OBS's own get_simple_output_encoder() makes */
static const char *warp_buffer_simple_encoder(const char *name)
{
	if (!name || !*name)
		return "obs_x264";
	if (strcmp(name, "x264") == 0 || strcmp(name, "x264_lowcpu") == 0)
		return "obs_x264";
	if (strcmp(name, "qsv") == 0)
		return "obs_qsv11_v2";
	if (strcmp(name, "qsv_av1") == 0)
		return "obs_qsv11_av1";
	if (strcmp(name, "amd") == 0)
		return "h264_texture_amf";
	if (strcmp(name, "amd_hevc") == 0)
		return "h265_texture_amf";
	if (strcmp(name, "amd_av1") == 0)
		return "av1_texture_amf";
	if (strcmp(name, "nvenc") == 0)
		return warp_buffer_encoder_available("obs_nvenc_h264_tex") ? "obs_nvenc_h264_tex" : "ffmpeg_nvenc";
	if (strcmp(name, "nvenc_hevc") == 0)
		return warp_buffer_encoder_available("obs_nvenc_hevc_tex") ? "obs_nvenc_hevc_tex" : "ffmpeg_hevc_nvenc";
	if (strcmp(name, "nvenc_av1") == 0)
		return "obs_nvenc_av1_tex";
	if (strcmp(name, "apple_h264") == 0)
		return "com.apple.videotoolbox.videoencoder.ave.avc";
	if (strcmp(name, "apple_hevc") == 0)
		return "com.apple.videotoolbox.videoencoder.ave.hevc";

	return "obs_x264";
}

/* the file extension a container is written with, as OBS's GetFormatExt() has
 * it: the fragmented and hybrid variants are the same file underneath */
static const char *warp_buffer_format_ext(const char *format)
{
	if (!format || !*format)
		return "mp4";
	if (strcmp(format, "fragmented_mp4") == 0 || strcmp(format, "hybrid_mp4") == 0)
		return "mp4";
	if (strcmp(format, "fragmented_mov") == 0 || strcmp(format, "hybrid_mov") == 0)
		return "mov";
	if (strcmp(format, "hls") == 0)
		return "m3u8";
	if (strcmp(format, "mpegts") == 0)
		return "ts";

	return format;
}

/* the constant-quality figure OBS's Simple mode records at, softened for small
 * canvases exactly as its CalcCRF() does */
static int warp_buffer_simple_crf(config_t *cfg, bool high_quality, bool low_cpu)
{
	int crf = high_quality ? 16 : 23;
	double cx = (double)config_get_uint(cfg, "Video", "OutputCX");
	double cy = (double)config_get_uint(cfg, "Video", "OutputCY");

	if (low_cpu)
		crf -= 2;

	double cross = sqrt(cx * cx + cy * cy);
	double reduction = (1.0 - fmin(2000.0, cross) / 2000.0) * 10.0;

	return crf - (int)reduction;
}

static void warp_buffer_simple_quality(obs_data_t *settings, const char *encoder, int crf, bool low_cpu,
				       bool high_quality)
{
	if (strncmp(encoder, "x264", 4) == 0) {
		obs_data_set_int(settings, "crf", crf);
		obs_data_set_bool(settings, "use_bufsize", true);
		obs_data_set_string(settings, "rate_control", "CRF");
		obs_data_set_string(settings, "profile", "high");
		obs_data_set_string(settings, "preset", low_cpu ? "ultrafast" : "veryfast");
	} else if (strncmp(encoder, "qsv", 3) == 0) {
		obs_data_set_int(settings, "qpi", crf);
		obs_data_set_int(settings, "qpp", crf);
		obs_data_set_int(settings, "qpb", crf);
		obs_data_set_string(settings, "rate_control", "CQP");
	} else if (strncmp(encoder, "amd", 3) == 0) {
		obs_data_set_int(settings, "cqp", crf);
		obs_data_set_string(settings, "rate_control", "CQP");
	} else if (strncmp(encoder, "nvenc", 5) == 0) {
		obs_data_set_int(settings, "cqp", crf);
		obs_data_set_string(settings, "rate_control", "CQP");
		obs_data_set_string(settings, "preset2", "p5");
		obs_data_set_string(settings, "tune", "hq");
		obs_data_set_string(settings, "multipass", "qres");
	} else if (strncmp(encoder, "apple", 5) == 0) {
		obs_data_set_int(settings, "quality", high_quality ? 70 : 50);
		obs_data_set_string(settings, "rate_control", "CRF");
	} else {
		obs_data_set_int(settings, "cqp", crf);
		obs_data_set_string(settings, "rate_control", "CQP");
	}
}

/* the encoder settings written beside an Advanced profile, or NULL */
static obs_data_t *warp_buffer_profile_json(const char *file)
{
	char *dir = obs_frontend_get_current_profile_path();

	if (!dir)
		return NULL;

	struct dstr path = {0};

	dstr_printf(&path, "%s/%s", dir, file);

	obs_data_t *data = obs_data_create_from_json_file_safe(path.array, "bak");

	dstr_free(&path);
	bfree(dir);

	return data;
}

/* Works out what a buffer should encode and where it should write, from the
 * profile that is loaded. Answers false when there is no profile to read,
 * which is every case where there is no frontend to have one. */
static bool warp_buffer_read_recipe(struct warp_buffer_recipe *recipe)
{
	config_t *cfg = obs_frontend_get_profile_config();

	if (!cfg) {
		WARP_BUFFER_LOG(LOG_WARNING, "no profile to take recording settings from");
		return false;
	}

	memset(recipe, 0, sizeof(*recipe));

	const char *mode = config_get_string(cfg, "Output", "Mode");
	bool advanced = mode && strcmp(mode, "Advanced") == 0;
	const char *format;
	const char *directory;
	int audio_bitrate;

	if (advanced) {
		const char *encoder = config_get_string(cfg, "AdvOut", "RecEncoder");

		/* "Use stream encoder" records with the streaming settings,
		 * which are the ones written out under the other name */
		if (!encoder || !*encoder || strcmp(encoder, "none") == 0) {
			recipe->video_id = config_get_string(cfg, "AdvOut", "Encoder");
			recipe->video_settings = warp_buffer_profile_json("streamEncoder.json");
		} else {
			recipe->video_id = encoder;
			recipe->video_settings = warp_buffer_profile_json("recordEncoder.json");
		}

		format = config_get_string(cfg, "AdvOut", "RecFormat2");
		directory = config_get_string(cfg, "AdvOut", "RecFilePath");
		audio_bitrate = (int)config_get_uint(cfg, "AdvOut", "Track1Bitrate");
		recipe->audio_id = "ffmpeg_aac";
	} else {
		const char *quality = config_get_string(cfg, "SimpleOutput", "RecQuality");
		const char *name = config_get_string(cfg, "SimpleOutput", "RecEncoder");
		const char *audio = config_get_string(cfg, "SimpleOutput", "RecAudioEncoder");
		bool stream_quality = quality && strcmp(quality, "Stream") == 0;
		bool lossless = quality && strcmp(quality, "Lossless") == 0;

		if (stream_quality)
			name = config_get_string(cfg, "SimpleOutput", "StreamEncoder");

		bool low_cpu = name && strcmp(name, "x264_lowcpu") == 0;

		recipe->video_id = warp_buffer_simple_encoder(name);
		recipe->video_settings = obs_data_create();

		if (stream_quality) {
			/* recorded at what is being streamed, which is a
			 * bitrate rather than a quality */
			obs_data_set_int(recipe->video_settings, "bitrate",
					 (int)config_get_uint(cfg, "SimpleOutput", "VBitrate"));
			obs_data_set_string(recipe->video_settings, "rate_control", "CBR");
		} else {
			/* Lossless is an FFmpeg output rather than an encoder,
			 * and there is nothing to hold a lossless buffer in, so
			 * it is held at the high quality preset instead */
			if (lossless)
				WARP_BUFFER_LOG(LOG_INFO, "the profile records losslessly, which a buffer cannot hold; "
							  "holding it at the high quality preset instead");

			warp_buffer_simple_quality(
				recipe->video_settings, name ? name : "x264",
				warp_buffer_simple_crf(cfg, lossless || (quality && strcmp(quality, "HQ") == 0),
						       low_cpu),
				low_cpu, true);
		}

		format = config_get_string(cfg, "SimpleOutput", "RecFormat2");
		directory = config_get_string(cfg, "SimpleOutput", "FilePath");
		audio_bitrate = 192;
		recipe->audio_id = (audio && strcmp(audio, "opus") == 0) ? "ffmpeg_opus" : "ffmpeg_aac";
	}

	if (!recipe->video_settings)
		recipe->video_settings = obs_data_create();

	/* A buffer is scrubbed, stepped through and played backwards, so it
	 * wants keyframes close together: they are what a step back seeks to,
	 * and what the oldest end of the buffer is trimmed to, so a long gap
	 * between them is both a coarse scrub and seconds of slack on every
	 * length. One a second costs a little size and buys all of that. */
	obs_data_set_int(recipe->video_settings, "keyint_sec", 1);

	if (!recipe->video_id || !*recipe->video_id)
		recipe->video_id = "obs_x264";

	/* A profile carried over from a machine with an encoder this one does
	 * not have would leave the buffer with nothing to encode with. x264 is
	 * always there, so it is what such a profile falls back to, with its
	 * settings dropped since they belonged to the other encoder. */
	if (!warp_buffer_encoder_available(recipe->video_id)) {
		WARP_BUFFER_LOG(LOG_WARNING, "this OBS has no '%s' encoder; holding buffers with x264 instead",
				recipe->video_id);

		recipe->video_id = "obs_x264";
		obs_data_clear(recipe->video_settings);
		obs_data_set_string(recipe->video_settings, "rate_control", "CRF");
		obs_data_set_int(recipe->video_settings, "crf", 23);
		obs_data_set_string(recipe->video_settings, "preset", "veryfast");
		obs_data_set_string(recipe->video_settings, "profile", "high");
	}

	/* Opus does not go in the MPEG-4 family of containers, so a profile
	 * that records Opus into one is recorded with AAC instead rather than
	 * writing a file nothing will open */
	const char *ext = warp_buffer_format_ext(format);

	if (strcmp(recipe->audio_id, "ffmpeg_opus") == 0 &&
	    (strcmp(ext, "mp4") == 0 || strcmp(ext, "mov") == 0 || strcmp(ext, "ts") == 0))
		recipe->audio_id = "ffmpeg_aac";

	recipe->audio_settings = obs_data_create();
	obs_data_set_int(recipe->audio_settings, "bitrate", audio_bitrate > 0 ? audio_bitrate : 192);
	obs_data_set_string(recipe->audio_settings, "rate_control", "CBR");

	dstr_copy(&recipe->extension, ext);

	/* The profile always has a recording folder, so one that does not is a
	 * profile that has never been through the settings; a buffer with
	 * nowhere to write is refused rather than given a folder of its own,
	 * since a clip an operator cannot find is worse than one that was
	 * never taken. */
	if (!directory || !*directory) {
		WARP_BUFFER_LOG(LOG_WARNING, "the profile has no recording folder to write clips to");
		warp_buffer_recipe_free(recipe);
		memset(recipe, 0, sizeof(*recipe));
		return false;
	}

	dstr_copy(&recipe->directory, directory);

	/* a fragmented container is written so that a file cut short by a
	 * crash still opens, which is what OBS asks of its own recordings */
	if (format && strncmp(format, "fragmented", 10) == 0)
		dstr_copy(&recipe->muxer_settings, "movflags=frag_keyframe+empty_moov+delay_moov");

	const char *filename = config_get_string(cfg, "Output", "FilenameFormatting");

	dstr_copy(&recipe->filename, filename && *filename ? filename : "%CCYY-%MM-%DD %hh-%mm-%ss");

	return true;
}

/* Roughly what one second of one angle costs, in kilobits, so the UI can say
 * what a buffer will hold before it is started. */
static int warp_buffer_kbps(void)
{
	config_t *cfg = obs_frontend_get_profile_config();

	if (!cfg)
		return WARP_BUFFER_ASSUMED_KBPS;

	const char *mode = config_get_string(cfg, "Output", "Mode");
	bool advanced = mode && strcmp(mode, "Advanced") == 0;
	int video = 0;

	if (advanced) {
		obs_data_t *settings = warp_buffer_profile_json("recordEncoder.json");

		if (settings) {
			video = (int)obs_data_get_int(settings, "bitrate");
			obs_data_release(settings);
		}
	} else {
		const char *quality = config_get_string(cfg, "SimpleOutput", "RecQuality");

		if (quality && strcmp(quality, "Stream") == 0)
			video = (int)config_get_uint(cfg, "SimpleOutput", "VBitrate");
	}

	/* A constant-quality encode has no bitrate to read, so what it comes
	 * to is guessed from the canvas: about a megabit per second for every
	 * hundred thousand pixels at 60fps, which is the right order for the
	 * quality presets OBS records at. */
	if (video <= 0) {
		double cx = (double)config_get_uint(cfg, "Video", "OutputCX");
		double cy = (double)config_get_uint(cfg, "Video", "OutputCY");
		double fps = (double)config_get_uint(cfg, "Video", "FPSInt");

		if (cx > 0 && cy > 0) {
			if (fps <= 0)
				fps = 60;

			video = (int)(cx * cy * fps / 6000.0);
		}
	}

	if (video <= 0)
		video = WARP_BUFFER_ASSUMED_KBPS;

	return video + 192;
}

/* Roughly what a buffer holds in memory, in megabytes: every length of every
 * angle keeps its own seconds of encoded picture and sound. Expects the lock
 * to be held. */
static int warp_buffer_memory_locked(struct warp_buffer *buffer)
{
	int lengths[WARP_BUFFER_MAX_LENGTHS];
	size_t count = warp_buffer_lengths(buffer->config, lengths, WARP_BUFFER_MAX_LENGTHS);
	obs_data_array_t *angles = obs_data_get_array(buffer->config, WARP_BUFFER_ANGLES);
	size_t num_angles = angles ? obs_data_array_count(angles) : 0;
	int seconds = 0;

	obs_data_array_release(angles);

	for (size_t i = 0; i < count; i++)
		seconds += lengths[i];

	return warp_buffer_memory_of((int)num_angles, seconds);
}

/* ------------------------------------------------------------------------- */
/* saves that are still being written */

/* Hands a finished save over: the clips are put in the angle register so they
 * find each other again, and the primary one is passed to whoever is taking
 * clips - the flow layer, in practice. Runs on the UI thread. */
struct warp_buffer_delivery {
	char *buffer_id;
	char *buffer_name;
	int seconds;
	char *claim_flow_id;
	size_t count;
	char *names[WARP_BUFFER_MAX_ANGLES];
	char *paths[WARP_BUFFER_MAX_ANGLES];
};

/* How long a clip runs for, in milliseconds, read back from the file that was
 * just written. This is what lines the angles of one save up: they are written
 * from whichever keyframe each buffer still had, so they start up to a
 * keyframe apart, and it is the difference in their lengths that says by how
 * much. */
static int64_t warp_buffer_probe_duration(const char *path)
{
	AVFormatContext *ctx = NULL;
	int64_t ms = 0;

	if (!path || !*path)
		return 0;

	if (avformat_open_input(&ctx, path, NULL, NULL) < 0)
		return 0;

	if (avformat_find_stream_info(ctx, NULL) >= 0 && ctx->duration > 0)
		ms = ctx->duration * 1000 / AV_TIME_BASE;

	avformat_close_input(&ctx);

	return ms;
}

static void warp_buffer_delivery_free(struct warp_buffer_delivery *delivery)
{
	for (size_t i = 0; i < delivery->count; i++) {
		bfree(delivery->names[i]);
		bfree(delivery->paths[i]);
	}

	bfree(delivery->buffer_id);
	bfree(delivery->buffer_name);
	bfree(delivery->claim_flow_id);
	bfree(delivery);
}

static void warp_buffer_deliver_task(void *param)
{
	struct warp_buffer_delivery *delivery = param;
	int64_t durations[WARP_BUFFER_MAX_ANGLES];

	for (size_t i = 0; i < delivery->count; i++)
		durations[i] = warp_buffer_probe_duration(delivery->paths[i]);

	warp_angles_add(delivery->buffer_name, delivery->seconds, delivery->count, (const char *const *)delivery->names,
			(const char *const *)delivery->paths, durations);

	if (warp_buffer_clip_handler)
		warp_buffer_clip_handler(delivery->buffer_id, delivery->buffer_name, delivery->seconds,
					 delivery->paths[0], delivery->claim_flow_id, warp_buffer_clip_param);

	warp_buffer_delivery_free(delivery);
}

/* Turns a save that has finished - or run out of time - into a delivery, and
 * queues it. Expects the lock to be held; takes what the save collected. */
static void warp_buffer_finish_locked(struct warp_buffer *buffer, size_t at)
{
	struct warp_buffer_pending *pending = &buffer->pending.array[at];
	struct warp_buffer_delivery *delivery = bzalloc(sizeof(struct warp_buffer_delivery));

	delivery->buffer_id = bstrdup(obs_data_get_string(buffer->config, WARP_BUFFER_ID));
	delivery->buffer_name = bstrdup(obs_data_get_string(buffer->config, WARP_BUFFER_NAME));
	delivery->seconds = pending->seconds;
	delivery->claim_flow_id = pending->claim_flow_id;

	/* The angles that did not report are left out rather than left as
	 * gaps, so the set is the clips that exist. The primary angle is what
	 * a flow is handed, so a save that lost it is dropped: there is no
	 * clip to hand over, whatever else was written. */
	for (size_t i = 0; i < pending->expected && i < buffer->angles.num; i++) {
		if (!pending->paths[i])
			continue;

		delivery->names[delivery->count] = bstrdup(buffer->angles.array[i].name);
		delivery->paths[delivery->count] = pending->paths[i];
		delivery->count++;
	}

	if (pending->filled < pending->expected)
		WARP_BUFFER_LOG(LOG_WARNING, "'%s': %d of %d angles wrote a clip for the %ds save",
				obs_data_get_string(buffer->config, WARP_BUFFER_NAME), (int)pending->filled,
				(int)pending->expected, pending->seconds);

	da_erase(buffer->pending, at);
	os_atomic_dec_long(&warp_buffer_pending_count);

	if (!delivery->count || !delivery->paths[0]) {
		WARP_BUFFER_LOG(LOG_WARNING, "'%s': the %ds save wrote no clip to hand over",
				obs_data_get_string(buffer->config, WARP_BUFFER_NAME), delivery->seconds);
		warp_buffer_delivery_free(delivery);
		return;
	}

	obs_queue_task(OBS_TASK_UI, warp_buffer_deliver_task, delivery, false);
}

/* Gives up on saves that have been waiting too long, handing over whatever
 * they collected. Expects the lock to be held. */
static void warp_buffer_sweep_locked(void)
{
	uint64_t now = os_gettime_ns();

	for (size_t i = 0; i < warp_buffers.num; i++) {
		struct warp_buffer *buffer = warp_buffers.array[i];

		for (size_t j = buffer->pending.num; j > 0; j--) {
			struct warp_buffer_pending *pending = &buffer->pending.array[j - 1];

			if (now - pending->started_ns > WARP_BUFFER_SAVE_TIMEOUT_NS)
				warp_buffer_finish_locked(buffer, j - 1);
		}
	}
}

static void warp_buffer_tick(void *param, float seconds)
{
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(seconds);

	/* nothing is being saved almost all of the time, and this runs every
	 * frame, so it costs one atomic read to find that out */
	if (os_atomic_load_long(&warp_buffer_pending_count) <= 0)
		return;

	pthread_mutex_lock(&warp_buffer_mutex);
	warp_buffer_sweep_locked();
	pthread_mutex_unlock(&warp_buffer_mutex);
}

/* the path an output wrote last, which it reports through its own proc handler
 * once it has finished writing it */
static char *warp_buffer_read_last(obs_output_t *output)
{
	calldata_t cd = {0};
	char *path = NULL;

	if (proc_handler_call(obs_output_get_proc_handler(output), "get_last_replay", &cd)) {
		const char *last = calldata_string(&cd, "path");

		if (last && *last)
			path = bstrdup(last);
	}

	calldata_free(&cd);

	return path;
}

/* One output has finished writing. Arrives on that output's muxer thread, so
 * it is handed the id of the output rather than the output itself: a signal
 * that overtakes a buffer being torn down looks up an id that is no longer
 * registered and does nothing. */
static void warp_buffer_saved_cb(void *data, calldata_t *cd)
{
	uint64_t out_id = (uint64_t)(uintptr_t)data;

	UNUSED_PARAMETER(cd);

	pthread_mutex_lock(&warp_buffer_mutex);

	struct warp_buffer *buffer = NULL;
	struct warp_buffer_out *out = NULL;

	for (size_t i = 0; i < warp_buffers.num && !out; i++) {
		struct warp_buffer *candidate = warp_buffers.array[i];

		for (size_t j = 0; j < candidate->outs.num; j++) {
			if (candidate->outs.array[j].id == out_id) {
				buffer = candidate;
				out = &candidate->outs.array[j];
				break;
			}
		}
	}

	if (out) {
		/* the oldest save of that length still missing this angle:
		 * saves are answered in the order they were asked for */
		for (size_t i = 0; i < buffer->pending.num; i++) {
			struct warp_buffer_pending *pending = &buffer->pending.array[i];

			if (pending->seconds != out->seconds || out->angle >= pending->expected ||
			    pending->paths[out->angle])
				continue;

			pending->paths[out->angle] = warp_buffer_read_last(out->output);

			if (pending->paths[out->angle]) {
				pending->filled++;

				if (pending->filled >= pending->expected)
					warp_buffer_finish_locked(buffer, i);
			}

			break;
		}
	}

	warp_buffer_sweep_locked();

	pthread_mutex_unlock(&warp_buffer_mutex);
}

/* ------------------------------------------------------------------------- */
/* starting and stopping */

/* expects the lock not to be held for the libobs work, which is why the whole
 * of setting an angle up is done in one go from the start path */
static bool warp_buffer_angle_setup(struct warp_buffer_angle *angle, struct warp_buffer_recipe *recipe,
				    const char *buffer_name)
{
	struct dstr name = {0};
	video_t *video = NULL;

	if (angle->program) {
		video = obs_get_video();

		if (!video) {
			WARP_BUFFER_LOG(LOG_WARNING, "'%s': there is no programme video to hold", buffer_name);
			return false;
		}
	} else {
		obs_source_t *source = NULL;

		if (angle->source_uuid && *angle->source_uuid)
			source = obs_get_source_by_uuid(angle->source_uuid);
		if (!source && angle->source_name && *angle->source_name)
			source = obs_get_source_by_name(angle->source_name);

		if (!source) {
			WARP_BUFFER_LOG(LOG_WARNING, "'%s': angle '%s' has no source called '%s'", buffer_name,
					angle->name, angle->source_name ? angle->source_name : "");
			return false;
		}

		/* A view with the one source on it, rendered at the size that
		 * source puts out. It is its own mix, so it is held whatever
		 * scene is up and whether or not the source is on screen -
		 * which is the whole reason for hanging a buffer on a camera
		 * rather than on the programme. */
		struct obs_video_info ovi;

		if (!obs_get_video_info(&ovi)) {
			obs_source_release(source);
			WARP_BUFFER_LOG(LOG_WARNING, "'%s': there is no video to take a canvas from", buffer_name);
			return false;
		}

		uint32_t width = obs_source_get_width(source);
		uint32_t height = obs_source_get_height(source);

		if (!width || !height) {
			width = ovi.base_width;
			height = ovi.base_height;
		}

		/* encoders want even dimensions, and a source that reports an
		 * odd one is not worth refusing over */
		width &= ~1u;
		height &= ~1u;

		if (!width || !height) {
			obs_source_release(source);
			WARP_BUFFER_LOG(LOG_WARNING, "'%s': angle '%s' has no picture to hold", buffer_name,
					angle->name);
			return false;
		}

		ovi.base_width = width;
		ovi.base_height = height;
		ovi.output_width = width;
		ovi.output_height = height;

		angle->view = obs_view_create();
		obs_view_set_source(angle->view, 0, source);

		video = obs_view_add2(angle->view, &ovi);

		if (!video) {
			obs_source_release(source);
			WARP_BUFFER_LOG(LOG_WARNING, "'%s': angle '%s' could not be given a mix of its own",
					buffer_name, angle->name);
			return false;
		}

		angle->source = source;
		angle->video = video;
	}

	dstr_printf(&name, "warp_buffer_%s_%s", buffer_name, angle->name);

	angle->encoder = obs_video_encoder_create(recipe->video_id, name.array, recipe->video_settings, NULL);
	dstr_free(&name);

	if (!angle->encoder) {
		WARP_BUFFER_LOG(LOG_WARNING, "'%s': angle '%s' could not be encoded with '%s'", buffer_name,
				angle->name, recipe->video_id);
		return false;
	}

	obs_encoder_set_video(angle->encoder, video);

	return true;
}

/* Hands what an angle was using to the teardown, leaving the angle itself
 * intact: it is still one of the buffer's angles, it is just no longer
 * holding. */
static void warp_buffer_angle_detach(struct warp_buffer_angle *angle, struct warp_buffer_teardown *teardown)
{
	if (angle->encoder)
		da_push_back(teardown->encoders, &angle->encoder);
	if (angle->view)
		da_push_back(teardown->views, &angle->view);
	if (angle->source)
		da_push_back(teardown->sources, &angle->source);

	angle->encoder = NULL;
	angle->view = NULL;
	angle->source = NULL;
	angle->video = NULL;
}

static void warp_buffer_angle_free(struct warp_buffer_angle *angle)
{
	bfree(angle->id);
	bfree(angle->name);
	bfree(angle->source_uuid);
	bfree(angle->source_name);
}

/* Reads the angles out of the configuration into the list the running buffer
 * works from. Expects the lock to be held. */
static void warp_buffer_build_angles(struct warp_buffer *buffer, struct warp_buffer_teardown *teardown)
{
	for (size_t i = 0; i < buffer->angles.num; i++) {
		warp_buffer_angle_detach(&buffer->angles.array[i], teardown);
		warp_buffer_angle_free(&buffer->angles.array[i]);
	}

	da_free(buffer->angles);

	obs_data_array_t *array = obs_data_get_array(buffer->config, WARP_BUFFER_ANGLES);

	if (!array)
		return;

	size_t count = obs_data_array_count(array);

	for (size_t i = 0; i < count && buffer->angles.num < WARP_BUFFER_MAX_ANGLES; i++) {
		obs_data_t *item = obs_data_array_item(array, i);
		struct warp_buffer_angle angle = {0};
		const char *feed = obs_data_get_string(item, WARP_BUFFER_ANGLE_FEED);
		const char *name = obs_data_get_string(item, WARP_BUFFER_ANGLE_NAME);
		const char *id = obs_data_get_string(item, WARP_BUFFER_ANGLE_ID);

		angle.program = !feed || !*feed || strcmp(feed, WARP_BUFFER_FEED_SOURCE) != 0;
		angle.source_uuid = bstrdup(obs_data_get_string(item, WARP_BUFFER_ANGLE_SOURCE_UUID));
		angle.source_name = bstrdup(obs_data_get_string(item, WARP_BUFFER_ANGLE_SOURCE_NAME));

		if (name && *name)
			angle.name = bstrdup(name);
		else if (!angle.program && angle.source_name && *angle.source_name)
			angle.name = bstrdup(angle.source_name);
		else
			angle.name = bstrdup(obs_module_text("Warp.Buffer.Angle.Program"));

		angle.id = (id && *id) ? bstrdup(id) : warp_buffer_make_id("ang");

		da_push_back(buffer->angles, &angle);
		obs_data_release(item);
	}

	obs_data_array_release(array);
}

/* Takes a running buffer apart, leaving what it was using in 'teardown' for
 * the caller to release once the lock has been dropped. Expects the lock to be
 * held; does nothing to a buffer that was not running. */
static void warp_buffer_detach_locked(struct warp_buffer *buffer, struct warp_buffer_teardown *teardown)
{
	if (!buffer->running)
		return;

	buffer->running = false;

	for (size_t i = 0; i < buffer->outs.num; i++)
		da_push_back(teardown->outs, &buffer->outs.array[i]);

	da_free(buffer->outs);

	for (size_t i = 0; i < buffer->angles.num; i++)
		warp_buffer_angle_detach(&buffer->angles.array[i], teardown);

	if (buffer->audio) {
		da_push_back(teardown->encoders, &buffer->audio);
		buffer->audio = NULL;
	}

	/* saves that were still being written are never going to be answered
	 * now: the outputs writing them are on their way out */
	for (size_t i = buffer->pending.num; i > 0; i--) {
		struct warp_buffer_pending *pending = &buffer->pending.array[i - 1];

		for (size_t j = 0; j < WARP_BUFFER_MAX_ANGLES; j++)
			bfree(pending->paths[j]);

		bfree(pending->claim_flow_id);
		da_erase(buffer->pending, i - 1);
		os_atomic_dec_long(&warp_buffer_pending_count);
	}
}

static bool warp_buffer_start_locked(struct warp_buffer *buffer, struct warp_buffer_teardown *teardown)
{
	const char *name = obs_data_get_string(buffer->config, WARP_BUFFER_NAME);
	int lengths[WARP_BUFFER_MAX_LENGTHS];
	size_t length_count = warp_buffer_lengths(buffer->config, lengths, WARP_BUFFER_MAX_LENGTHS);

	if (buffer->running)
		return true;

	warp_buffer_build_angles(buffer, teardown);

	if (!buffer->angles.num) {
		WARP_BUFFER_LOG(LOG_WARNING, "'%s' holds no angles, so there is nothing to start", name);
		return false;
	}

	if (!length_count) {
		WARP_BUFFER_LOG(LOG_WARNING, "'%s' offers no lengths, so there is nothing to hold", name);
		return false;
	}

	struct warp_buffer_recipe recipe;

	if (!warp_buffer_read_recipe(&recipe))
		return false;

	audio_t *audio = obs_get_audio();
	bool ok = audio != NULL;

	if (!ok)
		WARP_BUFFER_LOG(LOG_WARNING, "'%s': there is no audio to hold", name);

	if (ok) {
		struct dstr encoder_name = {0};

		dstr_printf(&encoder_name, "warp_buffer_%s_audio", name);
		buffer->audio =
			obs_audio_encoder_create(recipe.audio_id, encoder_name.array, recipe.audio_settings, 0, NULL);
		dstr_free(&encoder_name);

		if (buffer->audio)
			obs_encoder_set_audio(buffer->audio, audio);
		else
			WARP_BUFFER_LOG(LOG_WARNING, "'%s': sound could not be encoded with '%s'", name,
					recipe.audio_id);

		ok = buffer->audio != NULL;
	}

	for (size_t i = 0; ok && i < buffer->angles.num; i++)
		ok = warp_buffer_angle_setup(&buffer->angles.array[i], &recipe, name);

	for (size_t i = 0; ok && i < buffer->angles.num; i++) {
		struct warp_buffer_angle *angle = &buffer->angles.array[i];

		for (size_t j = 0; ok && j < length_count; j++) {
			obs_data_t *settings = obs_data_create();
			struct dstr filename = {0};
			struct dstr output_name = {0};

			/* what the clip is called: the time it was taken, then
			 * the buffer, the angle and the length, so a folder of
			 * them reads as the show it came from and the angles of
			 * one moment sit together */
			dstr_printf(&filename, "%s %s %s %ds", recipe.filename.array, name, angle->name, lengths[j]);
			dstr_replace(&filename, "/", "-");
			dstr_replace(&filename, "\\", "-");
			dstr_replace(&filename, ":", "-");

			obs_data_set_string(settings, "directory", recipe.directory.array);
			obs_data_set_string(settings, "format", filename.array);
			obs_data_set_string(settings, "extension", recipe.extension.array);
			obs_data_set_bool(settings, "allow_spaces", true);
			obs_data_set_int(settings, "max_time_sec", lengths[j]);
			obs_data_set_int(settings, "max_size_mb",
					 obs_data_get_int(buffer->config, WARP_BUFFER_MAX_SIZE_MB));

			if (recipe.muxer_settings.len)
				obs_data_set_string(settings, "muxer_settings", recipe.muxer_settings.array);

			dstr_printf(&output_name, "warp_buffer_%s_%s_%ds", name, angle->name, lengths[j]);

			struct warp_buffer_out out = {0};

			out.id = ++warp_buffer_out_seq;
			out.angle = i;
			out.seconds = lengths[j];
			out.output = obs_output_create("replay_buffer", output_name.array, settings, NULL);

			dstr_free(&output_name);
			dstr_free(&filename);
			obs_data_release(settings);

			if (!out.output) {
				WARP_BUFFER_LOG(LOG_WARNING, "'%s': no replay buffer output to hold '%s' with", name,
						angle->name);
				ok = false;
				break;
			}

			obs_output_set_video_encoder(out.output, angle->encoder);
			obs_output_set_audio_encoder(out.output, buffer->audio, 0);

			signal_handler_connect(obs_output_get_signal_handler(out.output), "saved", warp_buffer_saved_cb,
					       (void *)(uintptr_t)out.id);

			if (!obs_output_start(out.output)) {
				const char *error = obs_output_get_last_error(out.output);

				WARP_BUFFER_LOG(LOG_WARNING, "'%s': '%s' at %ds would not start%s%s", name, angle->name,
						lengths[j], error ? ": " : "", error ? error : "");

				signal_handler_disconnect(obs_output_get_signal_handler(out.output), "saved",
							  warp_buffer_saved_cb, (void *)(uintptr_t)out.id);
				obs_output_release(out.output);
				ok = false;
				break;
			}

			da_push_back(buffer->outs, &out);
		}
	}

	warp_buffer_recipe_free(&recipe);

	if (!ok) {
		/* half a buffer is no buffer: whatever did come up is taken
		 * down again so the operator is not left with some angles
		 * holding and others not */
		buffer->running = true;
		warp_buffer_detach_locked(buffer, teardown);
		WARP_BUFFER_LOG(LOG_WARNING, "'%s' could not be started", name);
		return false;
	}

	buffer->running = true;

	WARP_BUFFER_LOG(LOG_INFO, "'%s' started: %d angle%s at %d length%s, about %dMB", name, (int)buffer->angles.num,
			buffer->angles.num == 1 ? "" : "s", (int)length_count, length_count == 1 ? "" : "s",
			warp_buffer_memory_locked(buffer));

	return true;
}

/* ------------------------------------------------------------------------- */
/* hotkeys
 *
 * A press arrives on the hotkey thread and the buffer it belongs to may be
 * edited or removed from the UI thread at any moment, so the press carries
 * nothing but the id of the hotkey it came from: the buffer is looked up by
 * that id once the work is back on the UI thread, and one that has gone in the
 * meantime is simply not there. This is the same shape warp-flow.c's hotkeys
 * take, for the same reason. */

struct warp_buffer_press {
	obs_hotkey_id hotkey;
};

static void warp_buffer_press_task(void *param)
{
	struct warp_buffer_press *press = param;
	char *id = NULL;
	int seconds = 0;
	bool toggle = false;

	pthread_mutex_lock(&warp_buffer_mutex);

	for (size_t i = 0; i < warp_buffers.num && !id; i++) {
		struct warp_buffer *buffer = warp_buffers.array[i];

		if (buffer->toggle_hotkey == press->hotkey) {
			id = bstrdup(obs_data_get_string(buffer->config, WARP_BUFFER_ID));
			toggle = true;
			break;
		}

		int lengths[WARP_BUFFER_MAX_LENGTHS];
		size_t count = warp_buffer_lengths(buffer->config, lengths, WARP_BUFFER_MAX_LENGTHS);

		for (size_t j = 0; j < buffer->save_hotkeys.num && j < count; j++) {
			if (buffer->save_hotkeys.array[j] == press->hotkey) {
				id = bstrdup(obs_data_get_string(buffer->config, WARP_BUFFER_ID));
				seconds = lengths[j];
				break;
			}
		}
	}

	pthread_mutex_unlock(&warp_buffer_mutex);

	if (id) {
		if (toggle) {
			if (warp_buffer_running(id))
				warp_buffer_stop(id);
			else
				warp_buffer_start(id);
		} else {
			warp_buffer_save(id, seconds, NULL);
		}

		bfree(id);
	}

	bfree(press);
}

static void warp_buffer_hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return;

	struct warp_buffer_press *press = bmalloc(sizeof(struct warp_buffer_press));

	press->hotkey = id;

	obs_queue_task(OBS_TASK_UI, warp_buffer_press_task, press, false);
}

/* the key a length's hotkey is saved under, which is the length itself so that
 * the binding follows the length rather than its place in the list */
static void warp_buffer_hotkey_key(struct dstr *key, int seconds)
{
	dstr_printf(key, "hotkey_save_%d", seconds);
}

/* expects the lock to be held */
static void warp_buffer_unregister_hotkeys(struct warp_buffer *buffer)
{
	for (size_t i = 0; i < buffer->save_hotkeys.num; i++) {
		if (buffer->save_hotkeys.array[i] != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(buffer->save_hotkeys.array[i]);
	}

	da_free(buffer->save_hotkeys);

	if (buffer->toggle_hotkey != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(buffer->toggle_hotkey);
		buffer->toggle_hotkey = OBS_INVALID_HOTKEY_ID;
	}
}

/* Reads the keys the buffer's hotkeys are bound to back into its
 * configuration, so they are saved with it. Expects the lock to be held. */
static void warp_buffer_capture_hotkeys(struct warp_buffer *buffer)
{
	int lengths[WARP_BUFFER_MAX_LENGTHS];
	size_t count = warp_buffer_lengths(buffer->config, lengths, WARP_BUFFER_MAX_LENGTHS);

	for (size_t i = 0; i < buffer->save_hotkeys.num && i < count; i++) {
		if (buffer->save_hotkeys.array[i] == OBS_INVALID_HOTKEY_ID)
			continue;

		obs_data_array_t *keys = obs_hotkey_save(buffer->save_hotkeys.array[i]);
		struct dstr key = {0};

		warp_buffer_hotkey_key(&key, lengths[i]);
		obs_data_set_array(buffer->config, key.array, keys);

		dstr_free(&key);
		obs_data_array_release(keys);
	}

	if (buffer->toggle_hotkey != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *keys = obs_hotkey_save(buffer->toggle_hotkey);

		obs_data_set_array(buffer->config, "hotkey_toggle", keys);
		obs_data_array_release(keys);
	}
}

/* expects the lock to be held */
static void warp_buffer_register_hotkeys(struct warp_buffer *buffer)
{
	const char *id = obs_data_get_string(buffer->config, WARP_BUFFER_ID);
	const char *name = obs_data_get_string(buffer->config, WARP_BUFFER_NAME);
	int lengths[WARP_BUFFER_MAX_LENGTHS];
	size_t count = warp_buffer_lengths(buffer->config, lengths, WARP_BUFFER_MAX_LENGTHS);

	for (size_t i = 0; i < count; i++) {
		struct dstr hotkey_name = {0};
		struct dstr hotkey_desc = {0};
		struct dstr key = {0};

		dstr_printf(&hotkey_name, "Warp.Buffer.%s.Save.%d", id, lengths[i]);
		dstr_printf(&hotkey_desc, obs_module_text("Warp.Buffer.Hotkey.Save"), lengths[i], name);

		obs_hotkey_id hotkey =
			obs_hotkey_register_frontend(hotkey_name.array, hotkey_desc.array, warp_buffer_hotkey_cb, NULL);

		warp_buffer_hotkey_key(&key, lengths[i]);

		obs_data_array_t *keys = obs_data_get_array(buffer->config, key.array);

		if (keys) {
			obs_hotkey_load(hotkey, keys);
			obs_data_array_release(keys);
		}

		da_push_back(buffer->save_hotkeys, &hotkey);

		dstr_free(&key);
		dstr_free(&hotkey_desc);
		dstr_free(&hotkey_name);
	}

	struct dstr toggle_name = {0};
	struct dstr toggle_desc = {0};

	dstr_printf(&toggle_name, "Warp.Buffer.%s.Toggle", id);
	dstr_printf(&toggle_desc, obs_module_text("Warp.Buffer.Hotkey.Toggle"), name);

	buffer->toggle_hotkey =
		obs_hotkey_register_frontend(toggle_name.array, toggle_desc.array, warp_buffer_hotkey_cb, NULL);

	obs_data_array_t *toggle_keys = obs_data_get_array(buffer->config, "hotkey_toggle");

	if (toggle_keys) {
		obs_hotkey_load(buffer->toggle_hotkey, toggle_keys);
		obs_data_array_release(toggle_keys);
	}

	dstr_free(&toggle_desc);
	dstr_free(&toggle_name);
}

/* expects the lock to be held */
static void warp_buffer_rename_hotkeys(struct warp_buffer *buffer)
{
	const char *name = obs_data_get_string(buffer->config, WARP_BUFFER_NAME);
	int lengths[WARP_BUFFER_MAX_LENGTHS];
	size_t count = warp_buffer_lengths(buffer->config, lengths, WARP_BUFFER_MAX_LENGTHS);

	for (size_t i = 0; i < buffer->save_hotkeys.num && i < count; i++) {
		struct dstr desc = {0};

		dstr_printf(&desc, obs_module_text("Warp.Buffer.Hotkey.Save"), lengths[i], name);
		obs_hotkey_set_description(buffer->save_hotkeys.array[i], desc.array);
		dstr_free(&desc);
	}

	if (buffer->toggle_hotkey != OBS_INVALID_HOTKEY_ID) {
		struct dstr desc = {0};

		dstr_printf(&desc, obs_module_text("Warp.Buffer.Hotkey.Toggle"), name);
		obs_hotkey_set_description(buffer->toggle_hotkey, desc.array);
		dstr_free(&desc);
	}
}

/* ------------------------------------------------------------------------- */
/* the buffer list */

static void warp_buffer_set_defaults(obs_data_t *config)
{
	obs_data_set_default_bool(config, WARP_BUFFER_FOLLOW_OBS, false);
	obs_data_set_default_bool(config, WARP_BUFFER_ACTIVE, false);
	obs_data_set_default_int(config, WARP_BUFFER_MAX_SIZE_MB, 512);
}

/* A buffer's configuration for the outside, defaults and all: applying one
 * data object to another copies what was set on it, not what it falls back to,
 * so a bare copy reads as a buffer that was never configured. */
static obs_data_t *warp_buffer_config_copy(obs_data_t *config)
{
	obs_data_t *copy = warp_buffer_data_copy(config);

	if (copy)
		warp_buffer_set_defaults(copy);

	return copy;
}

/* expects the lock to be held */
static void warp_buffer_destroy(struct warp_buffer *buffer, struct warp_buffer_teardown *teardown)
{
	warp_buffer_detach_locked(buffer, teardown);
	warp_buffer_unregister_hotkeys(buffer);

	for (size_t i = 0; i < buffer->angles.num; i++)
		warp_buffer_angle_free(&buffer->angles.array[i]);

	da_free(buffer->angles);
	da_free(buffer->outs);
	da_free(buffer->pending);

	obs_data_release(buffer->config);
	bfree(buffer);
}

/* expects the lock to be held */
static void warp_buffer_clear(struct warp_buffer_teardown *teardown)
{
	for (size_t i = 0; i < warp_buffers.num; i++)
		warp_buffer_destroy(warp_buffers.array[i], teardown);

	da_free(warp_buffers);
}

/* expects the lock to be held; takes ownership of nothing */
static struct warp_buffer *warp_buffer_add_locked(obs_data_t *config)
{
	struct warp_buffer *buffer = bzalloc(sizeof(struct warp_buffer));

	buffer->config = warp_buffer_data_copy(config);
	buffer->toggle_hotkey = OBS_INVALID_HOTKEY_ID;

	warp_buffer_set_defaults(buffer->config);

	const char *id = obs_data_get_string(buffer->config, WARP_BUFFER_ID);

	if (!id || !*id) {
		char *made = warp_buffer_make_id("buf");

		obs_data_set_string(buffer->config, WARP_BUFFER_ID, made);
		bfree(made);
	}

	warp_buffer_register_hotkeys(buffer);

	da_push_back(warp_buffers, &buffer);

	return buffer;
}

char *warp_buffer_add(obs_data_t *config)
{
	char *id;

	if (!config || !warp_buffer_ready)
		return NULL;

	pthread_mutex_lock(&warp_buffer_mutex);

	struct warp_buffer *buffer = warp_buffer_add_locked(config);

	id = bstrdup(obs_data_get_string(buffer->config, WARP_BUFFER_ID));

	pthread_mutex_unlock(&warp_buffer_mutex);

	warp_buffer_notify_changed();

	return id;
}

bool warp_buffer_update(const char *id, obs_data_t *config)
{
	struct warp_buffer_teardown teardown;
	bool restart = false;
	bool rehotkey = false;

	if (!config || !warp_buffer_ready)
		return false;

	warp_buffer_teardown_init(&teardown);

	pthread_mutex_lock(&warp_buffer_mutex);

	struct warp_buffer *buffer = warp_buffer_find(id);

	if (!buffer) {
		pthread_mutex_unlock(&warp_buffer_mutex);
		warp_buffer_teardown_release(&teardown);
		return false;
	}

	/* What a buffer holds is what it is made of, so a change to its angles
	 * or its lengths is carried out by taking it down and putting it back
	 * up. Its name is only a label, and is changed underneath it. */
	obs_data_array_t *angles = obs_data_get_array(config, WARP_BUFFER_ANGLES);
	obs_data_array_t *lengths = obs_data_get_array(config, WARP_BUFFER_LENGTHS);

	restart = buffer->running && (angles || lengths);
	/* the hotkeys are named after the lengths, so they are given back and
	 * registered again whenever the lengths could have moved */
	rehotkey = lengths != NULL;

	obs_data_array_release(angles);
	obs_data_array_release(lengths);

	if (restart)
		warp_buffer_detach_locked(buffer, &teardown);

	if (rehotkey)
		warp_buffer_capture_hotkeys(buffer);

	obs_data_apply(buffer->config, config);
	warp_buffer_set_defaults(buffer->config);

	if (rehotkey) {
		warp_buffer_unregister_hotkeys(buffer);
		warp_buffer_register_hotkeys(buffer);
	} else {
		warp_buffer_rename_hotkeys(buffer);
	}

	pthread_mutex_unlock(&warp_buffer_mutex);

	/* the old encoders and outputs go back before the new ones are asked
	 * for, so a buffer being reconfigured never holds two sets at once */
	warp_buffer_teardown_release(&teardown);

	if (restart)
		warp_buffer_start(id);

	warp_buffer_notify_changed();

	return true;
}

bool warp_buffer_remove(const char *id)
{
	struct warp_buffer_teardown teardown;
	bool removed = false;

	if (!warp_buffer_ready)
		return false;

	warp_buffer_teardown_init(&teardown);

	pthread_mutex_lock(&warp_buffer_mutex);

	struct warp_buffer *buffer = warp_buffer_find(id);

	if (buffer) {
		size_t at = 0;

		for (size_t i = 0; i < warp_buffers.num; i++) {
			if (warp_buffers.array[i] == buffer) {
				at = i;
				break;
			}
		}

		warp_buffer_destroy(buffer, &teardown);
		da_erase(warp_buffers, at);
		removed = true;
	}

	pthread_mutex_unlock(&warp_buffer_mutex);
	warp_buffer_teardown_release(&teardown);

	if (removed)
		warp_buffer_notify_changed();

	return removed;
}

obs_data_array_t *warp_buffer_list(void)
{
	obs_data_array_t *array = obs_data_array_create();

	if (!warp_buffer_ready)
		return array;

	pthread_mutex_lock(&warp_buffer_mutex);

	for (size_t i = 0; i < warp_buffers.num; i++) {
		obs_data_t *item = warp_buffer_config_copy(warp_buffers.array[i]->config);

		/* whether it is holding right now is not configuration, so it
		 * is reported rather than saved */
		obs_data_set_bool(item, "running", warp_buffers.array[i]->running);

		obs_data_array_push_back(array, item);
		obs_data_release(item);
	}

	pthread_mutex_unlock(&warp_buffer_mutex);

	return array;
}

obs_data_t *warp_buffer_get(const char *id)
{
	obs_data_t *config = NULL;

	if (!warp_buffer_ready)
		return NULL;

	pthread_mutex_lock(&warp_buffer_mutex);

	struct warp_buffer *buffer = warp_buffer_find(id);

	if (buffer) {
		config = warp_buffer_config_copy(buffer->config);
		obs_data_set_bool(config, "running", buffer->running);
	}

	pthread_mutex_unlock(&warp_buffer_mutex);

	return config;
}

obs_data_t *warp_buffer_get_by_name(const char *name)
{
	obs_data_t *config = NULL;

	if (!warp_buffer_ready || !name || !*name)
		return NULL;

	pthread_mutex_lock(&warp_buffer_mutex);

	for (size_t i = 0; i < warp_buffers.num; i++) {
		struct warp_buffer *buffer = warp_buffers.array[i];

		if (warp_buffer_str_eq(obs_data_get_string(buffer->config, WARP_BUFFER_NAME), name)) {
			config = warp_buffer_config_copy(buffer->config);
			obs_data_set_bool(config, "running", buffer->running);
			break;
		}
	}

	pthread_mutex_unlock(&warp_buffer_mutex);

	return config;
}

/* ------------------------------------------------------------------------- */

bool warp_buffer_start(const char *id)
{
	struct warp_buffer_teardown teardown;
	bool started;

	if (!warp_buffer_ready)
		return false;

	warp_buffer_teardown_init(&teardown);

	pthread_mutex_lock(&warp_buffer_mutex);

	struct warp_buffer *buffer = warp_buffer_find(id);

	started = buffer && warp_buffer_start_locked(buffer, &teardown);

	if (buffer)
		obs_data_set_bool(buffer->config, WARP_BUFFER_ACTIVE, started);

	pthread_mutex_unlock(&warp_buffer_mutex);

	/* whatever a start that did not come off left behind */
	warp_buffer_teardown_release(&teardown);

	warp_buffer_notify_changed();

	return started;
}

void warp_buffer_stop(const char *id)
{
	struct warp_buffer_teardown teardown;
	bool stopped = false;

	if (!warp_buffer_ready)
		return;

	warp_buffer_teardown_init(&teardown);

	pthread_mutex_lock(&warp_buffer_mutex);

	struct warp_buffer *buffer = warp_buffer_find(id);

	if (buffer) {
		stopped = buffer->running;

		warp_buffer_detach_locked(buffer, &teardown);
		obs_data_set_bool(buffer->config, WARP_BUFFER_ACTIVE, false);

		if (stopped)
			WARP_BUFFER_LOG(LOG_INFO, "'%s' stopped",
					obs_data_get_string(buffer->config, WARP_BUFFER_NAME));
	}

	pthread_mutex_unlock(&warp_buffer_mutex);
	warp_buffer_teardown_release(&teardown);

	warp_buffer_notify_changed();
}

bool warp_buffer_running(const char *id)
{
	bool running = false;

	if (!warp_buffer_ready)
		return false;

	pthread_mutex_lock(&warp_buffer_mutex);

	struct warp_buffer *buffer = warp_buffer_find(id);

	running = buffer && buffer->running;

	pthread_mutex_unlock(&warp_buffer_mutex);

	return running;
}

bool warp_buffer_save(const char *id, int seconds, const char *claim_flow_id)
{
	if (!warp_buffer_ready)
		return false;

	pthread_mutex_lock(&warp_buffer_mutex);

	struct warp_buffer *buffer = warp_buffer_find(id);

	if (!buffer || !buffer->running) {
		pthread_mutex_unlock(&warp_buffer_mutex);
		WARP_BUFFER_LOG(LOG_WARNING, "nothing to save: buffer '%s' is not running", id ? id : "");
		return false;
	}

	const char *name = obs_data_get_string(buffer->config, WARP_BUFFER_NAME);
	int lengths[WARP_BUFFER_MAX_LENGTHS];
	size_t count = warp_buffer_lengths(buffer->config, lengths, WARP_BUFFER_MAX_LENGTHS);

	/* nothing said means the first length the buffer offers, which is the
	 * one its properties put at the top */
	if (seconds <= 0 && count)
		seconds = lengths[0];

	bool offered = false;

	for (size_t i = 0; i < count; i++)
		offered |= lengths[i] == seconds;

	if (!offered) {
		pthread_mutex_unlock(&warp_buffer_mutex);
		WARP_BUFFER_LOG(LOG_WARNING, "'%s' does not hold %d seconds", name, seconds);
		return false;
	}

	warp_buffer_sweep_locked();

	struct warp_buffer_pending pending = {0};

	pending.seconds = seconds;
	pending.expected = buffer->angles.num;
	pending.started_ns = os_gettime_ns();
	pending.claim_flow_id = bstrdup(claim_flow_id);

	da_push_back(buffer->pending, &pending);
	os_atomic_inc_long(&warp_buffer_pending_count);

	/* Every angle is told to write in the same pass, so the clips end on
	 * the same moment: the muxers cut at the instant they are asked, and
	 * it is their ends that the angle register lines up on. */
	size_t asked = 0;

	for (size_t i = 0; i < buffer->outs.num; i++) {
		struct warp_buffer_out *out = &buffer->outs.array[i];

		if (out->seconds != seconds)
			continue;

		proc_handler_call(obs_output_get_proc_handler(out->output), "save", NULL);
		asked++;
	}

	pthread_mutex_unlock(&warp_buffer_mutex);

	WARP_BUFFER_LOG(LOG_INFO, "'%s': saving %d seconds from %d angle%s", name, seconds, (int)asked,
			asked == 1 ? "" : "s");

	return asked > 0;
}

void warp_buffer_set_clip_handler(warp_buffer_clip_t handler, void *param)
{
	warp_buffer_clip_handler = handler;
	warp_buffer_clip_param = param;
}

void warp_buffer_set_changed_handler(warp_buffer_changed_t handler, void *param)
{
	warp_buffer_changed_handler = handler;
	warp_buffer_changed_param = param;
}

int warp_buffer_memory_of(int angles, int seconds)
{
	if (angles <= 0 || seconds <= 0)
		return 0;

	/* kilobits a second, times seconds, times angles, into megabytes */
	return (int)((double)warp_buffer_kbps() * seconds * angles / 8192.0);
}

int warp_buffer_memory_estimate(const char *id)
{
	int total = 0;

	if (!warp_buffer_ready)
		return 0;

	pthread_mutex_lock(&warp_buffer_mutex);

	struct warp_buffer *buffer = warp_buffer_find(id);

	if (buffer)
		total = warp_buffer_memory_locked(buffer);

	pthread_mutex_unlock(&warp_buffer_mutex);

	return total;
}

/* ------------------------------------------------------------------------- */
/* saved with the scene collection */

obs_data_array_t *warp_buffer_save_all(void)
{
	obs_data_array_t *array = obs_data_array_create();

	if (!warp_buffer_ready)
		return array;

	pthread_mutex_lock(&warp_buffer_mutex);

	for (size_t i = 0; i < warp_buffers.num; i++) {
		struct warp_buffer *buffer = warp_buffers.array[i];

		warp_buffer_capture_hotkeys(buffer);
		obs_data_set_bool(buffer->config, WARP_BUFFER_ACTIVE, buffer->running);

		obs_data_t *copy = warp_buffer_data_copy(buffer->config);

		obs_data_array_push_back(array, copy);
		obs_data_release(copy);
	}

	pthread_mutex_unlock(&warp_buffer_mutex);

	return array;
}

void warp_buffer_load_all(obs_data_array_t *array)
{
	struct warp_buffer_teardown teardown;

	if (!warp_buffer_ready)
		return;

	warp_buffer_teardown_init(&teardown);

	pthread_mutex_lock(&warp_buffer_mutex);
	warp_buffer_clear(&teardown);

	size_t count = array ? obs_data_array_count(array) : 0;

	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(array, i);

		warp_buffer_add_locked(item);
		obs_data_release(item);
	}

	pthread_mutex_unlock(&warp_buffer_mutex);
	warp_buffer_teardown_release(&teardown);

	if (count)
		WARP_BUFFER_LOG(LOG_INFO, "loaded %d buffer%s from the scene collection", (int)count,
				count == 1 ? "" : "s");

	warp_buffer_notify_changed();
}

/* ------------------------------------------------------------------------- */
/* the frontend */

/* Starts the buffers that were running when the collection was written, once
 * there is a frontend to hold them. Buffers that follow OBS's own replay
 * buffer are left to it. */
static void warp_buffer_restore_active(void)
{
	DARRAY(char *) start;

	da_init(start);

	pthread_mutex_lock(&warp_buffer_mutex);

	for (size_t i = 0; i < warp_buffers.num; i++) {
		struct warp_buffer *buffer = warp_buffers.array[i];

		if (buffer->running || obs_data_get_bool(buffer->config, WARP_BUFFER_FOLLOW_OBS))
			continue;

		if (obs_data_get_bool(buffer->config, WARP_BUFFER_ACTIVE)) {
			char *id = bstrdup(obs_data_get_string(buffer->config, WARP_BUFFER_ID));

			da_push_back(start, &id);
		}
	}

	pthread_mutex_unlock(&warp_buffer_mutex);

	for (size_t i = 0; i < start.num; i++) {
		warp_buffer_start(start.array[i]);
		bfree(start.array[i]);
	}

	da_free(start);
}

/* Starts or stops every buffer set to follow OBS's own replay buffer. */
static void warp_buffer_follow_obs(bool start)
{
	DARRAY(char *) ids;

	da_init(ids);

	pthread_mutex_lock(&warp_buffer_mutex);

	for (size_t i = 0; i < warp_buffers.num; i++) {
		struct warp_buffer *buffer = warp_buffers.array[i];

		if (!obs_data_get_bool(buffer->config, WARP_BUFFER_FOLLOW_OBS))
			continue;

		if (buffer->running == start)
			continue;

		char *id = bstrdup(obs_data_get_string(buffer->config, WARP_BUFFER_ID));

		da_push_back(ids, &id);
	}

	pthread_mutex_unlock(&warp_buffer_mutex);

	for (size_t i = 0; i < ids.num; i++) {
		if (start)
			warp_buffer_start(ids.array[i]);
		else
			warp_buffer_stop(ids.array[i]);

		bfree(ids.array[i]);
	}

	da_free(ids);
}

/* Puts every running buffer back up, which is how a change to what the profile
 * records at reaches buffers that are already holding. */
static void warp_buffer_restart_running(void)
{
	struct warp_buffer_teardown teardown;
	DARRAY(char *) ids;

	warp_buffer_teardown_init(&teardown);
	da_init(ids);

	pthread_mutex_lock(&warp_buffer_mutex);

	for (size_t i = 0; i < warp_buffers.num; i++) {
		struct warp_buffer *buffer = warp_buffers.array[i];

		if (!buffer->running)
			continue;

		char *id = bstrdup(obs_data_get_string(buffer->config, WARP_BUFFER_ID));

		da_push_back(ids, &id);
		warp_buffer_detach_locked(buffer, &teardown);
	}

	pthread_mutex_unlock(&warp_buffer_mutex);
	warp_buffer_teardown_release(&teardown);

	for (size_t i = 0; i < ids.num; i++) {
		warp_buffer_start(ids.array[i]);
		bfree(ids.array[i]);
	}

	da_free(ids);

	warp_buffer_notify_changed();
}

static void warp_buffer_frontend_event(enum obs_frontend_event event, void *private_data)
{
	UNUSED_PARAMETER(private_data);

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		warp_buffer_restore_active();
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED:
		warp_buffer_follow_obs(true);
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED:
		warp_buffer_follow_obs(false);
		break;
	/* what the profile records at is what a buffer holds, so a buffer that
	 * is already holding is put back up against the new settings */
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		warp_buffer_restart_running();
		break;
	/* the buffers of the collection being left go with it; the one being
	 * loaded brings its own */
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_EXIT: {
		struct warp_buffer_teardown teardown;

		warp_buffer_teardown_init(&teardown);

		pthread_mutex_lock(&warp_buffer_mutex);
		warp_buffer_clear(&teardown);
		pthread_mutex_unlock(&warp_buffer_mutex);

		warp_buffer_teardown_release(&teardown);
		warp_buffer_notify_changed();
		break;
	}
	default:
		break;
	}
}

void warp_buffer_init(void)
{
	if (warp_buffer_ready)
		return;

	pthread_mutex_init(&warp_buffer_mutex, NULL);
	da_init(warp_buffers);

	warp_buffer_ready = true;

	obs_frontend_add_event_callback(warp_buffer_frontend_event, NULL);
	obs_add_tick_callback(warp_buffer_tick, NULL);
}

void warp_buffer_shutdown(void)
{
	struct warp_buffer_teardown teardown;

	if (!warp_buffer_ready)
		return;

	warp_buffer_ready = false;
	warp_buffer_clip_handler = NULL;
	warp_buffer_changed_handler = NULL;

	warp_buffer_teardown_init(&teardown);

	obs_remove_tick_callback(warp_buffer_tick, NULL);
	obs_frontend_remove_event_callback(warp_buffer_frontend_event, NULL);

	pthread_mutex_lock(&warp_buffer_mutex);
	warp_buffer_clear(&teardown);
	pthread_mutex_unlock(&warp_buffer_mutex);

	warp_buffer_teardown_release(&teardown);

	pthread_mutex_destroy(&warp_buffer_mutex);
}
