/*
 * Copyright (c) 2023 Lain Bailey <lain@obsproject.com>
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

#pragma once

#include <obs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The Warp playback engine.
 *
 * One engine is made per source and lives as long as the source does. It owns a
 * media thread, and everything it does is carried out there, in the order it
 * was asked for: every call below only queues a command and returns. Files are
 * loaded into the engine rather than the engine being made for a file, so the
 * thread, the hardware device and - for a file encoded the same way as the last
 * one, which is every clip a replay buffer writes - the decoder itself carry
 * over from one clip to the next.
 *
 * Time. Decoded frames keep the timestamps the file gave them. Speed, pausing,
 * seeking and looping are all the same thing to the engine: the playback clock
 * is re-anchored, so that a given point in the file falls due at a given moment
 * on the system clock, and it runs at the speed asked for from there. Frames
 * are handed out stamped with the moment they fall due, which is the timeline
 * libobs keeps for an async source, so there is never a jump in it for libobs
 * to wait out.
 *
 * Clips. A load can say where in the file the clip starts and ends, or that it
 * is the last few seconds of the file. Times reported for a clip - its position
 * and duration - are measured from where the clip starts, not the file. */

typedef struct media_playback media_playback_t;

typedef void (*mp_video_cb)(void *opaque, struct obs_source_frame *frame);

struct AVFrame;

/* What a frame handed out is for. */
enum mp_frame_use {
	/* to be shown at its timestamp, during playback */
	MP_FRAME_TIMED,
	/* to be put on screen now: a seek, a frame step, a held clip */
	MP_FRAME_STILL,
	/* the first frame of a cued clip, to be shown when it is started */
	MP_FRAME_PRELOAD,
};

/* A decoded frame that is still on the GPU, handed out instead of a copy in
 * memory when a load asks for GPU direct decoding (mp_load_info). 'frame' is a
 * new reference to the decoder's surface, which the receiver owns and frees
 * with av_frame_free(); 'info' describes the picture in it - its size, its
 * colour, its timestamp, and its format, which is the layout of the surface's
 * planes (VIDEO_FORMAT_NV12 or VIDEO_FORMAT_P010) - and has no data. Surfaces
 * come from a fixed pool the decoder draws on, so they are held only as long as
 * needed. */
struct mp_gpu_frame {
	struct AVFrame *frame;
	struct obs_source_frame info;
};

typedef void (*mp_gpu_video_cb)(void *opaque, struct mp_gpu_frame *frame, enum mp_frame_use use);
typedef void (*mp_audio_cb)(void *opaque, struct obs_source_audio *audio);

#define MP_SPEED_MIN 10
#define MP_SPEED_MAX 400

enum mp_state {
	/* no file */
	MP_STATE_IDLE,
	/* a file is being opened */
	MP_STATE_OPENING,
	/* the file is open and sitting at the start of the clip, not playing */
	MP_STATE_STOPPED,
	MP_STATE_PLAYING,
	MP_STATE_PAUSED,
	/* the clip played to its end and was not looped; it is sitting at the
	 * start again, with the last frame still on screen */
	MP_STATE_ENDED,
	/* the file could not be opened, or could not be read any further */
	MP_STATE_ERROR,
};

enum mp_event {
	/* The load numbered 'seq' is being started. Sent before anything of
	 * the new file is handed out, so everything handed out after it belongs
	 * to that load. What was on screen stays there until the new file has a
	 * frame to replace it with. */
	MP_EVENT_LOADING,
	/* the load numbered 'seq' is open and its first frame decoded */
	MP_EVENT_LOADED,
	/* the load numbered 'seq' could not be opened */
	MP_EVENT_LOAD_FAILED,
	/* the clip played to its end and was not looped */
	MP_EVENT_ENDED,
	/* the file could not be read any further, part way through */
	MP_EVENT_ERROR,
};

typedef void (*mp_event_cb)(void *opaque, enum mp_event event, uint64_t seq);

/* What playback does with a file once it has loaded. */
enum mp_start {
	/* sit at the start of the clip without playing, with its first frame
	 * handed to v_preload_cb so it can go up the moment it is started */
	MP_START_CUE,
	/* play it from the start of the clip */
	MP_START_PLAY,
	/* put its first frame on screen and sit there paused */
	MP_START_HOLD,
};

struct mp_media_info {
	void *opaque;

	/* a frame to be shown at its timestamp, during playback */
	mp_video_cb v_cb;
	/* the first frame of a clip that has been cued, to be shown when it is
	 * started rather than now */
	mp_video_cb v_preload_cb;
	/* a frame to be put on screen now: a seek, a frame step, a held clip */
	mp_video_cb v_still_cb;
	mp_audio_cb a_cb;
	/* frames on the GPU, for any of the three uses above; see mp_gpu_frame */
	mp_gpu_video_cb v_gpu_cb;
	/* called on the media thread, with nothing locked */
	mp_event_cb event_cb;
};

struct mp_load_info {
	const char *path;
	/* input format to force, for network input; NULL to let FFmpeg probe */
	const char *format;
	const char *ffmpeg_options;
	/* network buffering, in bytes */
	int buffering;
	/* percent; network input always plays at 100 */
	int speed;

	bool is_local_file;
	bool looping;
	bool hardware_decoding;
	/* Hardware-decoded frames are handed to v_gpu_cb on the GPU rather than
	 * copied into memory, where the platform's decoder can do that - D3D11
	 * on Windows, VAAPI on Linux. Only means anything with
	 * hardware_decoding, and falls back to copies wherever it cannot be
	 * done. */
	bool gpu_direct;
	/* the load is a reconnect attempt: a failure to open is not worth a
	 * warning every few seconds */
	bool reconnecting;

	enum video_range_type force_range;
	bool is_linear_alpha;
	/* Keep the sound at its own pitch when playing at another speed,
	 * rather than letting it go up and down with the speed. */
	bool preserve_pitch;

	/* Where the clip starts and ends, in milliseconds from the start of the
	 * file; out_ms 0 is the end of the file. tail_ms, when it is not 0, makes
	 * the clip the last that many milliseconds of the file - or of what is
	 * before out_ms - and in_ms is not used. All three are only honoured for
	 * local files. */
	int64_t in_ms;
	int64_t out_ms;
	int64_t tail_ms;

	enum mp_start start;
};

/* A snapshot of the engine, as it stood when the media thread last finished
 * something it was asked to do or handed a frame out. */
struct mp_status {
	/* the number of the last load asked for, and of the last one that went
	 * through to the end, opened or not */
	uint64_t load_seq;
	uint64_t loaded_seq;

	enum mp_state state;

	/* the loaded_seq file is open, and has these */
	bool open;
	bool has_video;
	bool has_audio;

	int speed;
	bool looping;

	/* of the clip, in milliseconds, from where it starts */
	int64_t time_ms;
	int64_t duration_ms;
	/* estimated, when the file does not say */
	int64_t frames;
};

extern media_playback_t *media_playback_create(const struct mp_media_info *info);
extern void media_playback_destroy(media_playback_t *mp);

/* Loads a file, replacing whatever was loaded. Returns the number the load is
 * reported back under, by the LOADED or LOAD_FAILED event and in the status;
 * a load that is overtaken by another before the media thread reaches it is
 * dropped without being reported. */
extern uint64_t media_playback_load(media_playback_t *mp, const struct mp_load_info *info);
/* closes the file */
extern void media_playback_close(media_playback_t *mp);

/* plays the clip from its start */
extern void media_playback_restart(media_playback_t *mp);
/* Pausing a clip that is not playing puts its first frame up and holds it
 * there; unpausing one that is not paused plays it from its start. */
extern void media_playback_pause(media_playback_t *mp, bool pause);
/* stops playback and goes back to the start of the clip */
extern void media_playback_stop(media_playback_t *mp);
/* Frame-accurate. A clip that is not playing is left paused on the frame it
 * was moved to. */
extern void media_playback_seek(media_playback_t *mp, int64_t ms);
/* steps a signed number of frames, pausing first if the clip is playing */
extern void media_playback_step_frames(media_playback_t *mp, int frames);
extern void media_playback_set_speed(media_playback_t *mp, int speed);
extern void media_playback_set_looping(media_playback_t *mp, bool looping);
extern void media_playback_set_is_linear_alpha(media_playback_t *mp, bool is_linear_alpha);
extern void media_playback_set_preserve_pitch(media_playback_t *mp, bool preserve_pitch);
/* stops handing frames out on the GPU, for whoever is drawing them to fall back
 * on when it cannot import them */
extern void media_playback_disable_gpu_direct(media_playback_t *mp);

extern void media_playback_get_status(media_playback_t *mp, struct mp_status *status);

#ifdef __cplusplus
}
#endif
