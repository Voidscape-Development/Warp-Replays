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

#ifdef __cplusplus
extern "C" {
#endif

#include <util/deque.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#pragma warning(disable : 4204)
#endif

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <util/threading.h>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#if LIBAVCODEC_VERSION_MAJOR < 60
#define CODEC_CAP_TRUNC AV_CODEC_CAP_TRUNCATED
#define CODEC_FLAG_TRUNC AV_CODEC_FLAG_TRUNCATED
#endif

struct media_playback;

/* One stream of the file that is open, and the decoder for it.
 *
 * Timestamps here are media time: nanoseconds on the file's own timeline,
 * whatever speed it is being played at. Speed is applied by the playback clock
 * when a frame is handed out, never to the timestamps themselves, so nothing
 * decoded ever has to be rescaled when the speed changes.
 *
 * The decoder outlives the file. A new file whose stream has the same codec
 * parameters as the last one - which is every clip a replay buffer writes - is
 * decoded by the same decoder, flushed, rather than by a new one. */
struct mp_decode {
	struct media_playback *m;
	AVStream *stream;
	bool audio;

	AVCodecContext *decoder;
	/* what the decoder was opened with, to tell whether the next file's
	 * stream can be handed to it as it is */
	AVCodecParameters *params;
	bool params_hw;
	bool params_gpu_direct;
	const AVCodec *codec;

	int64_t last_duration;
	int64_t frame_pts;
	int64_t next_pts;

	AVFrame *in_frame;
	AVFrame *sw_frame;
	AVFrame *hw_frame;
	AVFrame *frame;
	enum AVPixelFormat hw_format;
	bool got_first_keyframe;
	bool frame_ready;
	bool eof;
	bool hw;

	uint16_t max_luminance;

	AVPacket *orig_pkt;
	AVPacket *pkt;
	bool packet_pending;
	struct deque packets;
};

/* how many surfaces the decoder's pool is given on top of what it needs, for the
 * frames held while they wait to be drawn off the GPU */
#define MP_GPU_EXTRA_SURFACES 12

/* the decoder whose frames can be drawn off the GPU on this platform, or
 * AV_HWDEVICE_TYPE_NONE */
extern enum AVHWDeviceType mp_decode_gpu_direct_type(void);

/* Gets 'd' ready to decode 'stream' of the file that has just been opened,
 * reusing the decoder it already has when the stream's parameters match. */
extern bool mp_decode_open(struct mp_decode *d, struct media_playback *m, AVStream *stream, bool audio, bool hw);
extern void mp_decode_free(struct mp_decode *decode);

extern void mp_decode_clear_packets(struct mp_decode *decode);

extern void mp_decode_push_packet(struct mp_decode *decode, AVPacket *pkt);
extern bool mp_decode_next(struct mp_decode *decode);
extern void mp_decode_flush(struct mp_decode *decode);

#ifdef __cplusplus
}
#endif
