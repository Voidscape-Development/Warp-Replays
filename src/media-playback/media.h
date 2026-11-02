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

/* The engine's insides, shared by media.c and decode.c only. Everything the
 * sources use is in media-playback.h. */

#include <obs.h>
#include <util/darray.h>
#include "media-playback.h"
#include "decode.h"
#include "stretch.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#pragma warning(disable : 4204)
#endif

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <util/threading.h>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

/* Capacity of the decoded-frame history used to show backward frame steps
 * straight away. The largest step hotkey is 20 frames, and presses arriving
 * while the media thread is busy are added together into one larger step, so
 * keep room for a couple of them. The byte cap keeps the history from
 * ballooning on very large frames. */
#define MP_FRAME_HIST_SIZE 45
#define MP_FRAME_HIST_MAX_BYTES (256 * 1024 * 1024)

enum mp_cmd_type {
	MP_CMD_LOAD,
	MP_CMD_CLOSE,
	MP_CMD_RESTART,
	MP_CMD_PAUSE,
	MP_CMD_STOP,
	MP_CMD_SEEK,
	MP_CMD_STEP,
	MP_CMD_SPEED,
	MP_CMD_LOOPING,
	MP_CMD_PITCH,
	MP_CMD_KILL,
};

/* a load, copied out of the caller's mp_load_info and owned by the engine */
struct mp_load {
	uint64_t seq;
	char *path;
	char *format;
	char *ffmpeg_options;
	int buffering;
	int speed;
	bool is_local_file;
	bool looping;
	bool hw;
	bool reconnecting;
	enum video_range_type force_range;
	bool is_linear_alpha;
	bool preserve_pitch;
	int64_t in_ms;
	int64_t out_ms;
	int64_t tail_ms;
	enum mp_start start;
};

struct mp_cmd {
	enum mp_cmd_type type;
	int64_t value;
	struct mp_load *load;
};

struct media_playback {
	/* --- set when the engine is made, read-only after --------------- */
	void *opaque;
	mp_video_cb v_cb;
	mp_video_cb v_preload_cb;
	mp_video_cb v_still_cb;
	mp_audio_cb a_cb;
	mp_event_cb event_cb;

	pthread_t thread;
	bool thread_valid;

	/* --- shared with the callers, under 'mutex' ---------------------- */
	pthread_mutex_t mutex;
	/* signalled whenever a command is queued */
	os_event_t *wake;
	DARRAY(struct mp_cmd) cmds;
	uint64_t next_seq;
	struct mp_status status;
	/* Asks a read or an open that is blocked on the network to give up,
	 * because what it is for has been superseded. Set with a load or a
	 * kill, cleared by the media thread. */
	volatile bool interrupt;
	volatile bool is_linear_alpha;

	/* --- the media thread's alone ------------------------------------ */
	struct mp_load *cur;
	enum mp_state state;
	AVFormatContext *fmt;
	bool is_local_file;
	bool looping;
	int speed;
	enum video_range_type force_range;
	/* sound at another speed keeps its pitch, through 'stretch' */
	bool preserve_pitch;
	struct mp_stretch stretch;

	AVBufferRef *hw_device;
	enum AVHWDeviceType hw_type;

	DARRAY(AVPacket *) packet_pool;
	struct mp_decode v;
	struct mp_decode a;
	bool has_video;
	bool has_audio;
	/* the demuxer has nothing more to give */
	bool eof;

	/* Where the clip sits in the file, in media time. out_ns of 0 is the
	 * end of the file; file_end_ns is that, where the file says. */
	int64_t start_ns;
	int64_t in_ns;
	int64_t out_ns;
	int64_t file_end_ns;

	/* The playback clock: the media time anchor_media falls due at the
	 * system time anchor_wall, and from there media time runs at 'speed'
	 * percent. need_anchor puts the anchor on whatever is handed out next,
	 * as it is handed out. */
	uint64_t anchor_wall;
	int64_t anchor_media;
	bool need_anchor;
	/* just past the last thing handed out, in media time */
	int64_t played_end;

	/* The frame on screen, in media time, and whether it is the one that
	 * is sitting in the video decoder too - a still the playback has not
	 * moved on from - rather than one handed out and gone. */
	int64_t cur_pts;
	bool have_cur;
	/* where the last seek was aimed, so a still taken from the history when
	 * the file ran out first is the last frame before it */
	int64_t still_target;
	bool shown;

	enum AVPixelFormat scale_format;
	struct SwsContext *swscale;
	int scale_w;
	int scale_h;
	enum AVPixelFormat scale_src;
	int scale_linesizes[4];
	uint8_t *scale_pic[4];

	struct obs_source_frame obsframe;
	enum video_colorspace cur_space;
	enum video_range_type cur_range;

	uint64_t interrupt_poll_ts;

	/* Ring of references to recently decoded video frames, oldest at
	 * hist_start, in media time, so a backward step that lands inside it
	 * is put up without waiting for the keyframe to be decoded again. */
	AVFrame *hist_frames[MP_FRAME_HIST_SIZE];
	int64_t hist_pts[MP_FRAME_HIST_SIZE];
	size_t hist_start;
	size_t hist_num;
	size_t hist_bytes;
};

typedef struct media_playback mp_media_t;

/* media thread only: hands a packet back to the pool */
extern void mp_media_free_packet(mp_media_t *m, AVPacket *pkt);

/* media thread only: records the decoder's current video frame in the frame
 * history (called from mp_decode_next) */
extern void mp_media_hist_push(mp_media_t *m, struct mp_decode *d);

#ifdef __cplusplus
}
#endif
