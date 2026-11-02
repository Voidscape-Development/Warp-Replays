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

/* The Warp playback engine. See media-playback.h for what it is; this is how.
 *
 * Everything below the "media thread" heading runs on the engine's own thread
 * and touches nothing the callers can see except through 'status', which is
 * published under the mutex. The callers only ever queue commands. The thread
 * sleeps until either a command arrives or the next frame falls due, carries
 * every queued command out in order, then hands out whatever has fallen due.
 *
 * There is one clock, and every timestamp handed out is read off it: a frame
 * at media time t is stamped anchor_wall + (t - anchor_media) * 100 / speed.
 * Pausing, resuming, seeking, looping and changing speed all just move the
 * anchor, so the stamps handed out never jump against the system clock -
 * which is the timeline libobs keeps for an async source - and nothing that
 * has been decoded ever has to be rescaled. */

#include <util/platform.h>
#include <util/bmem.h>

#include "media-playback.h"
#include "media.h"
#include "closest-format.h"

#include <libavdevice/avdevice.h>
#include <libavutil/imgutils.h>

#define NS_PER_MS 1000000LL
#define NS_PER_SEC 1000000000LL

/* A frame falling due further than this from where the clock says it should
 * is a discontinuity in the input rather than playback running late or early,
 * and the clock is put back on it. */
#define MAX_TS_VAR (2 * NS_PER_SEC)

/* how early a frame is handed out rather than slept for */
#define DUE_SLACK_NS (1 * NS_PER_MS)

/* A video frame this far behind the clock is not converted or handed out: the
 * engine is behind, and libobs would only drop it on arrival. */
#define LATE_DROP_NS (100 * NS_PER_MS)

/* ------------------------------------------------------------------------- */
/* format conversion */

static inline enum video_format convert_pixel_format(int f)
{
	switch (f) {
	case AV_PIX_FMT_NONE:
		return VIDEO_FORMAT_NONE;
	case AV_PIX_FMT_YUV420P:
		return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_YUYV422:
		return VIDEO_FORMAT_YUY2;
	case AV_PIX_FMT_YUV422P:
		return VIDEO_FORMAT_I422;
	case AV_PIX_FMT_YUV422P10LE:
		return VIDEO_FORMAT_I210;
	case AV_PIX_FMT_YUV444P:
		return VIDEO_FORMAT_I444;
	case AV_PIX_FMT_YUV444P12LE:
		return VIDEO_FORMAT_I412;
	case AV_PIX_FMT_UYVY422:
		return VIDEO_FORMAT_UYVY;
	case AV_PIX_FMT_YVYU422:
		return VIDEO_FORMAT_YVYU;
	case AV_PIX_FMT_NV12:
		return VIDEO_FORMAT_NV12;
	case AV_PIX_FMT_RGBA:
		return VIDEO_FORMAT_RGBA;
	case AV_PIX_FMT_BGRA:
		return VIDEO_FORMAT_BGRA;
	case AV_PIX_FMT_YUVA420P:
		return VIDEO_FORMAT_I40A;
	case AV_PIX_FMT_YUV420P10LE:
		return VIDEO_FORMAT_I010;
	case AV_PIX_FMT_YUVA422P:
		return VIDEO_FORMAT_I42A;
	case AV_PIX_FMT_YUVA444P:
		return VIDEO_FORMAT_YUVA;
#if LIBAVUTIL_BUILD >= AV_VERSION_INT(56, 31, 100)
	case AV_PIX_FMT_YUVA444P12LE:
		return VIDEO_FORMAT_YA2L;
#endif
	case AV_PIX_FMT_BGR0:
		return VIDEO_FORMAT_BGRX;
	case AV_PIX_FMT_P010LE:
		return VIDEO_FORMAT_P010;
	default:;
	}

	return VIDEO_FORMAT_NONE;
}

static inline enum audio_format convert_sample_format(int f)
{
	switch (f) {
	case AV_SAMPLE_FMT_U8:
		return AUDIO_FORMAT_U8BIT;
	case AV_SAMPLE_FMT_S16:
		return AUDIO_FORMAT_16BIT;
	case AV_SAMPLE_FMT_S32:
		return AUDIO_FORMAT_32BIT;
	case AV_SAMPLE_FMT_FLT:
		return AUDIO_FORMAT_FLOAT;
	case AV_SAMPLE_FMT_U8P:
		return AUDIO_FORMAT_U8BIT_PLANAR;
	case AV_SAMPLE_FMT_S16P:
		return AUDIO_FORMAT_16BIT_PLANAR;
	case AV_SAMPLE_FMT_S32P:
		return AUDIO_FORMAT_32BIT_PLANAR;
	case AV_SAMPLE_FMT_FLTP:
		return AUDIO_FORMAT_FLOAT_PLANAR;
	default:;
	}

	return AUDIO_FORMAT_UNKNOWN;
}

static inline enum speaker_layout convert_speaker_layout(uint8_t channels)
{
	switch (channels) {
	case 0:
		return SPEAKERS_UNKNOWN;
	case 1:
		return SPEAKERS_MONO;
	case 2:
		return SPEAKERS_STEREO;
	case 3:
		return SPEAKERS_2POINT1;
	case 4:
		return SPEAKERS_4POINT0;
	case 5:
		return SPEAKERS_4POINT1;
	case 6:
		return SPEAKERS_5POINT1;
	case 8:
		return SPEAKERS_7POINT1;
	default:
		return SPEAKERS_UNKNOWN;
	}
}

static inline enum video_colorspace convert_color_space(enum AVColorSpace s, enum AVColorTransferCharacteristic trc,
							enum AVColorPrimaries color_primaries)
{
	switch (s) {
	case AVCOL_SPC_BT709:
		return (trc == AVCOL_TRC_IEC61966_2_1) ? VIDEO_CS_SRGB : VIDEO_CS_709;
	case AVCOL_SPC_FCC:
	case AVCOL_SPC_BT470BG:
	case AVCOL_SPC_SMPTE170M:
	case AVCOL_SPC_SMPTE240M:
		return VIDEO_CS_601;
	case AVCOL_SPC_BT2020_NCL:
		return (trc == AVCOL_TRC_ARIB_STD_B67) ? VIDEO_CS_2100_HLG : VIDEO_CS_2100_PQ;
	default:
		return (color_primaries == AVCOL_PRI_BT2020)
			       ? ((trc == AVCOL_TRC_ARIB_STD_B67) ? VIDEO_CS_2100_HLG : VIDEO_CS_2100_PQ)
			       : VIDEO_CS_DEFAULT;
	}
}

static inline enum video_range_type convert_color_range(enum AVColorRange r)
{
	return r == AVCOL_RANGE_JPEG ? VIDEO_RANGE_FULL : VIDEO_RANGE_DEFAULT;
}

static inline int get_sws_colorspace(enum AVColorSpace cs)
{
	switch (cs) {
	case AVCOL_SPC_BT709:
		return SWS_CS_ITU709;
	case AVCOL_SPC_FCC:
		return SWS_CS_FCC;
	case AVCOL_SPC_BT470BG:
		return SWS_CS_ITU624;
	case AVCOL_SPC_SMPTE170M:
		return SWS_CS_SMPTE170M;
	case AVCOL_SPC_SMPTE240M:
		return SWS_CS_SMPTE240M;
	case AVCOL_SPC_BT2020_NCL:
		return SWS_CS_BT2020;
	default:
		break;
	}

	return SWS_CS_ITU709;
}

static inline int get_sws_range(enum AVColorRange r)
{
	return r == AVCOL_RANGE_JPEG ? 1 : 0;
}

#define FIXED_1_0 (1 << 16)

static void free_scaling(mp_media_t *m)
{
	sws_freeContext(m->swscale);
	m->swscale = NULL;
	av_freep(&m->scale_pic[0]);
	memset(m->scale_pic, 0, sizeof(m->scale_pic));
	m->scale_w = 0;
	m->scale_h = 0;
	m->scale_src = AV_PIX_FMT_NONE;
}

/* Works out what 'f' is handed to libobs as, and sets up a conversion when that
 * is not the format it was decoded in. The engine outlives the file, so this is
 * worked out again whenever the size or format of what is decoded changes. */
static bool prepare_scaling(mp_media_t *m, const AVFrame *f)
{
	if (m->scale_src == f->format && m->scale_w == f->width && m->scale_h == f->height)
		return true;

	free_scaling(m);

	m->scale_src = f->format;
	m->scale_w = f->width;
	m->scale_h = f->height;
	m->scale_format = closest_format(f->format);

	if (m->scale_format == f->format)
		return true;

	int space = get_sws_colorspace(f->colorspace);
	int range = get_sws_range(f->color_range);
	const int *coeff = sws_getCoefficients(space);

	m->swscale = sws_getCachedContext(NULL, f->width, f->height, f->format, f->width, f->height, m->scale_format,
					  SWS_POINT, NULL, NULL, NULL);
	if (!m->swscale) {
		blog(LOG_WARNING, "MP: Failed to initialize scaler");
		return false;
	}

	sws_setColorspaceDetails(m->swscale, coeff, range, coeff, range, 0, FIXED_1_0, FIXED_1_0);

	int ret = av_image_alloc(m->scale_pic, m->scale_linesizes, f->width, f->height, m->scale_format, 32);
	if (ret < 0) {
		blog(LOG_WARNING, "MP: Failed to create scale pic data");
		return false;
	}

	return true;
}

/* Fills in m->obsframe from 'f'. Returns NULL for a frame there is nothing to
 * show from. */
static struct obs_source_frame *convert_video(mp_media_t *m, AVFrame *f)
{
	struct obs_source_frame *frame = &m->obsframe;
	struct mp_decode *d = &m->v;

	if (!f || !prepare_scaling(m, f))
		return NULL;

	/* network input picks up wherever it is joined, so nothing is shown
	 * until a frame that can be decoded whole */
	if (!m->is_local_file && !d->got_first_keyframe) {
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58, 29, 100)
		if (!f->key_frame)
#else
		if (!(f->flags & AV_FRAME_FLAG_KEY))
#endif
			return NULL;

		d->got_first_keyframe = true;
	}

	bool flip = false;
	if (m->swscale) {
		int ret = sws_scale(m->swscale, (const uint8_t *const *)f->data, f->linesize, 0, f->height,
				    m->scale_pic, m->scale_linesizes);
		if (ret < 0)
			return NULL;

		flip = m->scale_linesizes[0] < 0 && m->scale_linesizes[1] == 0;
		for (size_t i = 0; i < 4; i++) {
			frame->data[i] = m->scale_pic[i];
			frame->linesize[i] = abs(m->scale_linesizes[i]);
		}
		for (size_t i = 4; i < MAX_AV_PLANES; i++) {
			frame->data[i] = NULL;
			frame->linesize[i] = 0;
		}
	} else {
		flip = f->linesize[0] < 0 && f->linesize[1] == 0;

		for (size_t i = 0; i < MAX_AV_PLANES; i++) {
			frame->data[i] = f->data[i];
			frame->linesize[i] = abs(f->linesize[i]);
		}
	}

	if (flip)
		frame->data[0] -= frame->linesize[0] * ((size_t)f->height - 1);

	enum video_format new_format = convert_pixel_format(m->scale_format);
	enum video_colorspace new_space = convert_color_space(f->colorspace, f->color_trc, f->color_primaries);
	enum video_range_type new_range = m->force_range == VIDEO_RANGE_DEFAULT ? convert_color_range(f->color_range)
										: m->force_range;

	if (new_format != frame->format || new_space != m->cur_space || new_range != m->cur_range) {
		frame->format = new_format;
		frame->full_range = new_range == VIDEO_RANGE_FULL;

		bool success = video_format_get_parameters_for_format(new_space, new_range, new_format,
								      frame->color_matrix, frame->color_range_min,
								      frame->color_range_max);

		m->cur_space = new_space;
		m->cur_range = new_range;

		if (!success) {
			frame->format = VIDEO_FORMAT_NONE;
			return NULL;
		}
	}

	if (frame->format == VIDEO_FORMAT_NONE)
		return NULL;

	frame->width = f->width;
	frame->height = f->height;
	frame->max_luminance = d->max_luminance;
	frame->flip = flip;
	frame->flags = os_atomic_load_bool(&m->is_linear_alpha) ? OBS_SOURCE_FRAME_LINEAR_ALPHA : 0;

	switch (f->color_trc) {
	case AVCOL_TRC_BT709:
	case AVCOL_TRC_GAMMA22:
	case AVCOL_TRC_GAMMA28:
	case AVCOL_TRC_SMPTE170M:
	case AVCOL_TRC_SMPTE240M:
	case AVCOL_TRC_IEC61966_2_1:
		frame->trc = VIDEO_TRC_SRGB;
		break;
	case AVCOL_TRC_SMPTE2084:
		frame->trc = VIDEO_TRC_PQ;
		break;
	case AVCOL_TRC_ARIB_STD_B67:
		frame->trc = VIDEO_TRC_HLG;
		break;
	default:
		frame->trc = VIDEO_TRC_DEFAULT;
	}

	return frame;
}

/* ------------------------------------------------------------------------- */
/* frame history */

static size_t hist_frame_bytes(const AVFrame *f)
{
	int size = av_image_get_buffer_size(f->format, f->width, f->height, 1);
	return size > 0 ? (size_t)size : 0;
}

static void hist_pop_oldest(mp_media_t *m)
{
	AVFrame **slot = &m->hist_frames[m->hist_start];
	m->hist_bytes -= hist_frame_bytes(*slot);
	av_frame_free(slot);
	m->hist_start = (m->hist_start + 1) % MP_FRAME_HIST_SIZE;
	m->hist_num--;
}

static void hist_clear(mp_media_t *m)
{
	while (m->hist_num)
		hist_pop_oldest(m);
	m->hist_start = 0;
}

void mp_media_hist_push(mp_media_t *m, struct mp_decode *d)
{
	/* GPU-surface frames are not kept: holding references to them would
	 * starve the decoder's fixed surface pool */
	if (!d->frame || d->frame->hw_frames_ctx)
		return;

	/* a frame already in the history - decoding forward again over ground
	 * just stepped back across - is not kept twice */
	for (size_t i = 0; i < m->hist_num; i++) {
		if (m->hist_pts[(m->hist_start + i) % MP_FRAME_HIST_SIZE] == d->frame_pts)
			return;
	}

	AVFrame *clone = av_frame_clone(d->frame);
	if (!clone)
		return;

	size_t bytes = hist_frame_bytes(clone);
	while (m->hist_num && (m->hist_num >= MP_FRAME_HIST_SIZE || m->hist_bytes + bytes > MP_FRAME_HIST_MAX_BYTES))
		hist_pop_oldest(m);

	size_t idx = (m->hist_start + m->hist_num) % MP_FRAME_HIST_SIZE;
	m->hist_frames[idx] = clone;
	m->hist_pts[idx] = d->frame_pts;
	m->hist_num++;
	m->hist_bytes += bytes;
}

static AVFrame *hist_find(mp_media_t *m, int64_t pts, int64_t tolerance, int64_t *found_pts)
{
	AVFrame *best = NULL;
	int64_t best_diff = tolerance;

	for (size_t i = 0; i < m->hist_num; i++) {
		size_t idx = (m->hist_start + i) % MP_FRAME_HIST_SIZE;
		int64_t diff = m->hist_pts[idx] - pts;
		if (diff < 0)
			diff = -diff;
		if (diff <= best_diff) {
			best_diff = diff;
			best = m->hist_frames[idx];
			*found_pts = m->hist_pts[idx];
		}
	}

	return best;
}

/* the latest frame in the history at or before 'pts' */
static AVFrame *hist_at_or_before(mp_media_t *m, int64_t pts, int64_t *found_pts)
{
	AVFrame *best = NULL;
	int64_t best_pts = INT64_MIN;

	for (size_t i = 0; i < m->hist_num; i++) {
		size_t idx = (m->hist_start + i) % MP_FRAME_HIST_SIZE;

		if (m->hist_pts[idx] <= pts && m->hist_pts[idx] > best_pts) {
			best_pts = m->hist_pts[idx];
			best = m->hist_frames[idx];
		}
	}

	if (best)
		*found_pts = best_pts;
	return best;
}

/* ------------------------------------------------------------------------- */
/* status and events */

static void emit_event(mp_media_t *m, enum mp_event event, uint64_t seq)
{
	if (m->event_cb)
		m->event_cb(m->opaque, event, seq);
}

static int64_t clip_end_ns(mp_media_t *m)
{
	if (m->out_ns)
		return m->out_ns;
	return m->file_end_ns;
}

static int64_t estimate_frames(mp_media_t *m, int64_t duration_ns)
{
	if (!m->has_video || duration_ns <= 0)
		return 0;

	AVStream *stream = m->v.stream;
	AVRational fr = stream->avg_frame_rate;

	if (fr.num > 0 && fr.den > 0)
		return av_rescale(duration_ns, fr.num, (int64_t)fr.den * NS_PER_SEC);

	return stream->nb_frames > 0 ? stream->nb_frames : 0;
}

/* publishes where the engine has got to; 'loaded' marks the end of a load */
static void publish(mp_media_t *m)
{
	struct mp_status st;
	bool open = m->fmt != NULL;
	int64_t end = clip_end_ns(m);
	int64_t duration = open && end > m->in_ns ? end - m->in_ns : 0;
	int64_t time = open && m->have_cur ? m->cur_pts - m->in_ns : 0;

	if (time < 0)
		time = 0;
	if (duration && time > duration)
		time = duration;

	pthread_mutex_lock(&m->mutex);
	st = m->status;
	st.state = m->state;
	st.open = open;
	st.has_video = open && m->has_video;
	st.has_audio = open && m->has_audio;
	st.speed = m->speed;
	st.looping = m->looping;
	st.time_ms = time / NS_PER_MS;
	st.duration_ms = duration / NS_PER_MS;
	st.frames = estimate_frames(m, duration);
	m->status = st;
	pthread_mutex_unlock(&m->mutex);
}

static void publish_loaded(mp_media_t *m, uint64_t seq)
{
	pthread_mutex_lock(&m->mutex);
	m->status.loaded_seq = seq;
	pthread_mutex_unlock(&m->mutex);

	publish(m);
}

/* ------------------------------------------------------------------------- */
/* the clock */

static inline uint64_t clock_wall_for(mp_media_t *m, int64_t media)
{
	int64_t delta = (media - m->anchor_media) * 100 / m->speed;
	return (uint64_t)((int64_t)m->anchor_wall + delta);
}

static inline int64_t clock_media_at(mp_media_t *m, uint64_t wall)
{
	int64_t delta = (int64_t)(wall - m->anchor_wall);
	return m->anchor_media + delta * m->speed / 100;
}

static inline void clock_anchor(mp_media_t *m, uint64_t wall, int64_t media)
{
	m->anchor_wall = wall;
	m->anchor_media = media;
	m->need_anchor = false;
}

/* ------------------------------------------------------------------------- */
/* reading and decoding */

void mp_media_free_packet(mp_media_t *m, AVPacket *pkt)
{
	av_packet_unref(pkt);
	da_push_back(m->packet_pool, &pkt);
}

static inline struct mp_decode *get_packet_decoder(mp_media_t *m, const AVPacket *pkt)
{
	if (m->has_audio && pkt->stream_index == m->a.stream->index)
		return &m->a;
	if (m->has_video && pkt->stream_index == m->v.stream->index)
		return &m->v;

	return NULL;
}

static int read_packet(mp_media_t *m)
{
	AVPacket *pkt;

	if (m->packet_pool.num) {
		pkt = m->packet_pool.array[m->packet_pool.num - 1];
		da_pop_back(m->packet_pool);
	} else {
		pkt = av_packet_alloc();
		if (!pkt)
			return AVERROR(ENOMEM);
	}

	int ret = av_read_frame(m->fmt, pkt);
	if (ret < 0) {
		if (ret != AVERROR_EOF && ret != AVERROR_EXIT)
			blog(LOG_WARNING, "MP: av_read_frame failed: %s (%d)", av_err2str(ret), ret);
		mp_media_free_packet(m, pkt);
		return ret;
	}

	struct mp_decode *d = get_packet_decoder(m, pkt);
	if (d && pkt->size)
		mp_decode_push_packet(d, pkt);
	else
		mp_media_free_packet(m, pkt);

	return ret;
}

static inline bool needs_frame(struct mp_decode *d)
{
	return !d->frame_ready && !d->eof;
}

/* Decodes until every stream has a frame ready or has run out. False when the
 * file could not be read: an error on network input, or the read being given
 * up on. A local file that fails to read part way through is treated as having
 * ended there. */
static bool prepare_frames(mp_media_t *m)
{
	while ((m->has_video && needs_frame(&m->v)) || (m->has_audio && needs_frame(&m->a))) {
		if (!m->eof) {
			int ret = read_packet(m);

			if (ret == AVERROR_EOF) {
				m->eof = true;
			} else if (ret == AVERROR_EXIT) {
				return false;
			} else if (ret < 0) {
				if (!m->is_local_file)
					return false;
				m->eof = true;
			}
		}

		if (m->has_video && needs_frame(&m->v) && !mp_decode_next(&m->v))
			return false;
		if (m->has_audio && needs_frame(&m->a) && !mp_decode_next(&m->a))
			return false;
	}

	return true;
}

/* whether 'd' is holding a frame that belongs to the clip */
static inline bool in_clip(mp_media_t *m, struct mp_decode *d)
{
	return d->frame_ready && (!m->out_ns || d->frame_pts < m->out_ns);
}

static int64_t frame_interval(mp_media_t *m)
{
	struct mp_decode *d = &m->v;

	if (d->last_duration > 0)
		return d->last_duration;

	AVRational fr = d->stream ? d->stream->avg_frame_rate : (AVRational){0, 1};
	return fr.num > 0 ? av_rescale(NS_PER_SEC, fr.den, fr.num) : 33333333LL;
}

/* drops audio that sits wholly before 'pts', so playback picks up in sync with
 * the video frame on screen */
static void drop_audio_before(mp_media_t *m, int64_t pts)
{
	while (m->has_audio && m->a.frame_ready && m->a.next_pts <= pts) {
		m->a.frame_ready = false;
		if (!prepare_frames(m))
			break;
	}
}

/* Moves to 'target', in media time, frame-accurately: the video decoder is left
 * holding the frame that is on screen at that moment, and the audio decoder the
 * sound from then on. Input that cannot seek is left where it is. */
static void seek_exact(mp_media_t *m, int64_t target)
{
	if (!m->fmt)
		return;

	int64_t end = clip_end_ns(m);
	if (end > m->in_ns && m->has_video && target > end - frame_interval(m))
		target = end - frame_interval(m);
	if (target < m->start_ns)
		target = m->start_ns;

	m->shown = false;
	m->still_target = target;

	if (!m->is_local_file)
		return;

	struct mp_decode *d = m->has_video ? &m->v : &m->a;
	int64_t ts = av_rescale_q(target, (AVRational){1, 1000000000}, d->stream->time_base);

	int ret = av_seek_frame(m->fmt, d->stream->index, ts, AVSEEK_FLAG_BACKWARD);
	if (ret < 0)
		blog(LOG_WARNING, "MP: Failed to seek: %s", av_err2str(ret));

	if (m->has_video)
		mp_decode_flush(&m->v);
	if (m->has_audio)
		mp_decode_flush(&m->a);
	m->eof = false;

	if (!prepare_frames(m))
		return;

	/* Decode up to the frame the target falls inside. The frame before it
	 * is the one shown if the file runs out first, so it is not let go of
	 * until there is one after it: it stays in the history. */
	while (m->has_video && m->v.frame_ready && m->v.next_pts <= target) {
		m->v.frame_ready = false;
		if (!prepare_frames(m) || !m->v.frame_ready)
			break;
	}

	drop_audio_before(m, target);
}

/* ------------------------------------------------------------------------- */
/* handing frames out */

/* Puts the frame the video decoder is holding on screen now, or the last one
 * decoded if it is holding none. */
static void show_still(mp_media_t *m)
{
	AVFrame *f = NULL;
	int64_t pts = 0;
	bool from_decoder = false;

	if (!m->has_video || !m->v_still_cb)
		return;

	if (m->v.frame_ready) {
		f = m->v.frame;
		pts = m->v.frame_pts;
		from_decoder = true;
	} else {
		f = hist_at_or_before(m, m->still_target, &pts);
	}

	struct obs_source_frame *frame = convert_video(m, f);
	if (!frame)
		return;

	frame->timestamp = os_gettime_ns();
	m->v_still_cb(m->opaque, frame);

	m->cur_pts = pts;
	m->have_cur = true;
	m->shown = from_decoder;
}

/* hands the first frame of a clip that is being cued over to be put up the
 * moment it is started */
static void cue_first_frame(mp_media_t *m)
{
	if (!m->has_video || !m->v.frame_ready || !m->v_preload_cb)
		return;

	struct obs_source_frame *frame = convert_video(m, m->v.frame);
	if (!frame)
		return;

	frame->timestamp = os_gettime_ns();
	m->v_preload_cb(m->opaque, frame);
}

static void emit_video(mp_media_t *m, uint64_t due, uint64_t now)
{
	struct mp_decode *d = &m->v;
	int64_t pts = d->frame_pts;

	d->frame_ready = false;
	m->cur_pts = pts;
	m->have_cur = true;
	m->shown = false;

	if (!m->v_cb || due + LATE_DROP_NS < now)
		return;

	struct obs_source_frame *frame = convert_video(m, d->frame);
	if (!frame)
		return;

	frame->timestamp = due;
	m->v_cb(m->opaque, frame);
}

static void emit_audio(mp_media_t *m, uint64_t due)
{
	struct mp_decode *d = &m->a;
	AVFrame *f = d->frame;
	struct obs_source_audio audio = {0};
	int channels;

	d->frame_ready = false;

	if (!m->has_video) {
		m->cur_pts = d->frame_pts;
		m->have_cur = true;
	}

	if (!m->a_cb)
		return;

#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(59, 19, 100)
	channels = f->channels;
#else
	channels = f->ch_layout.nb_channels;
#endif

	for (size_t i = 0; i < MAX_AV_PLANES; i++)
		audio.data[i] = f->data[i];

	/* Speed is applied to the sound by playing it at a rate that much
	 * faster or slower; libobs resamples it to the mix rate. */
	audio.samples_per_sec = (uint32_t)((int64_t)f->sample_rate * m->speed / 100);
	audio.speakers = convert_speaker_layout((uint8_t)channels);
	audio.format = convert_sample_format(f->format);
	audio.frames = f->nb_samples;
	audio.timestamp = due;

	if (audio.format == AUDIO_FORMAT_UNKNOWN || !audio.samples_per_sec)
		return;

	m->a_cb(m->opaque, &audio);
}

/* Whatever the clip holds next, out of both streams, or NULL when it has run
 * out. */
static struct mp_decode *next_up(mp_media_t *m)
{
	bool v = m->has_video && in_clip(m, &m->v);
	bool a = m->has_audio && in_clip(m, &m->a);

	if (v && a)
		return m->a.frame_pts < m->v.frame_pts ? &m->a : &m->v;
	if (v)
		return &m->v;
	if (a)
		return &m->a;
	return NULL;
}

static void cue_clip(mp_media_t *m)
{
	seek_exact(m, m->in_ns);
	cue_first_frame(m);
	m->have_cur = false;
}

static void start_playing(mp_media_t *m)
{
	m->state = MP_STATE_PLAYING;
	m->need_anchor = true;
}

/* the clip has run out: around again, or finished */
static void reached_end(mp_media_t *m)
{
	if (m->looping && m->is_local_file) {
		/* The clip goes round with no gap: its start falls due when
		 * its end would have. Both are the clip's own bounds rather
		 * than wherever the audio frames either side of them happen
		 * to start or stop, which would open a gap of up to a frame
		 * of sound at each end. */
		int64_t end = m->played_end;
		if (m->out_ns && end > m->out_ns)
			end = m->out_ns;

		uint64_t end_wall = clock_wall_for(m, end);
		uint64_t now = os_gettime_ns();

		seek_exact(m, m->in_ns);

		if (!next_up(m)) {
			m->state = MP_STATE_ENDED;
			publish(m);
			emit_event(m, MP_EVENT_ENDED, 0);
			return;
		}

		if (end_wall + MAX_TS_VAR < now)
			end_wall = now;
		clock_anchor(m, end_wall, m->in_ns);
		m->played_end = m->in_ns;
		return;
	}

	m->state = MP_STATE_ENDED;

	if (m->is_local_file)
		cue_clip(m);

	publish(m);
	emit_event(m, MP_EVENT_ENDED, 0);
}

/* Hands out everything that has fallen due. Returns how long until the next
 * thing does, or 0 if playback has stopped. */
static uint64_t play_due(mp_media_t *m)
{
	for (int handed = 0; handed < 16; handed++) {
		struct mp_decode *d = next_up(m);

		if (!d) {
			reached_end(m);
			if (m->state != MP_STATE_PLAYING)
				return 0;
			continue;
		}

		uint64_t now = os_gettime_ns();

		if (m->need_anchor)
			clock_anchor(m, now, d->frame_pts);

		uint64_t due = clock_wall_for(m, d->frame_pts);

		/* a gap or a jump in the input: carry on from here */
		if (due > now + MAX_TS_VAR || due + MAX_TS_VAR < now) {
			clock_anchor(m, now, d->frame_pts);
			due = now;
		}

		if (due > now + DUE_SLACK_NS)
			return due - now;

		if (d->next_pts > m->played_end)
			m->played_end = d->next_pts;

		if (d == &m->v)
			emit_video(m, due, now);
		else
			emit_audio(m, due);

		if (!prepare_frames(m)) {
			if (os_atomic_load_bool(&m->interrupt))
				return 1;

			if (m->is_local_file) {
				reached_end(m);
			} else {
				m->state = MP_STATE_ERROR;
				publish(m);
				emit_event(m, MP_EVENT_ERROR, 0);
			}
			return 0;
		}
	}

	/* let the commands in before carrying on */
	return 1;
}

/* ------------------------------------------------------------------------- */
/* opening files */

static int interrupt_callback(void *data)
{
	mp_media_t *m = data;
	return os_atomic_load_bool(&m->interrupt);
}

static void close_input(mp_media_t *m)
{
	if (m->has_video)
		mp_decode_clear_packets(&m->v);
	if (m->has_audio)
		mp_decode_clear_packets(&m->a);

	avformat_close_input(&m->fmt);
	hist_clear(m);

	m->has_video = false;
	m->has_audio = false;
	m->v.frame_ready = false;
	m->a.frame_ready = false;
	m->eof = false;
	m->have_cur = false;
	m->shown = false;
	m->start_ns = 0;
	m->in_ns = 0;
	m->out_ns = 0;
	m->file_end_ns = 0;
}

#define RIST_PROTO "rist"

static bool open_input(mp_media_t *m, const struct mp_load *load)
{
	const AVInputFormat *format = NULL;

	if (load->format && *load->format) {
		format = av_find_input_format(load->format);
		if (!format)
			blog(LOG_INFO, "MP: Unable to find input format for '%s'", load->path);
	}

	AVDictionary *opts = NULL;
	bool is_rist = strncmp(load->path, RIST_PROTO, strlen(RIST_PROTO)) == 0;
	if (load->buffering && !load->is_local_file && !is_rist)
		av_dict_set_int(&opts, "buffer_size", load->buffering, 0);

	if (load->ffmpeg_options && *load->ffmpeg_options) {
		int ret = av_dict_parse_string(&opts, load->ffmpeg_options, "=", " ", 0);
		if (ret)
			blog(LOG_WARNING, "Failed to parse FFmpeg options: %s\n%s", av_err2str(ret),
			     load->ffmpeg_options);
	}

	m->fmt = avformat_alloc_context();
	if (!m->fmt) {
		av_dict_free(&opts);
		return false;
	}

	if (load->buffering == 0 && !load->is_local_file)
		m->fmt->flags |= AVFMT_FLAG_NOBUFFER;
	if (!load->is_local_file)
		av_dict_set(&opts, "stimeout", "30000000", 0);

	m->fmt->interrupt_callback.callback = interrupt_callback;
	m->fmt->interrupt_callback.opaque = m;

	int ret = avformat_open_input(&m->fmt, load->path, format, opts ? &opts : NULL);
	av_dict_free(&opts);

	if (ret < 0) {
		if (!load->reconnecting && ret != AVERROR_EXIT)
			blog(LOG_WARNING, "MP: Failed to open media: '%s'", load->path);
		return false;
	}

	if (avformat_find_stream_info(m->fmt, NULL) < 0) {
		blog(LOG_WARNING, "MP: Failed to find stream info for '%s'", load->path);
		return false;
	}

	int vi = av_find_best_stream(m->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	int ai = av_find_best_stream(m->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);

	m->has_video = vi >= 0 && mp_decode_open(&m->v, m, m->fmt->streams[vi], false, load->hw);
	m->has_audio = ai >= 0 && mp_decode_open(&m->a, m, m->fmt->streams[ai], true, load->hw);

	if (!m->has_video && !m->has_audio) {
		blog(LOG_WARNING, "MP: Could not initialize audio or video: '%s'", load->path);
		return false;
	}

	return true;
}

/* where the clip sits in the file, worked out once the file is open */
static void place_clip(mp_media_t *m, const struct mp_load *load)
{
	int64_t start = m->fmt->start_time;
	int64_t duration = m->fmt->duration;

	m->start_ns = start == AV_NOPTS_VALUE ? 0 : av_rescale(start, NS_PER_SEC, AV_TIME_BASE);
	m->file_end_ns = duration == AV_NOPTS_VALUE || duration <= 0
				 ? 0
				 : m->start_ns + av_rescale(duration, NS_PER_SEC, AV_TIME_BASE);

	m->in_ns = m->start_ns;
	m->out_ns = 0;

	if (!load->is_local_file)
		return;

	if (load->out_ms > 0) {
		int64_t out = m->start_ns + load->out_ms * NS_PER_MS;

		if (!m->file_end_ns || out < m->file_end_ns)
			m->out_ns = out;
	}

	int64_t end = clip_end_ns(m);

	if (load->tail_ms > 0) {
		if (end) {
			int64_t in = end - load->tail_ms * NS_PER_MS;
			m->in_ns = in > m->start_ns ? in : m->start_ns;
		}
	} else if (load->in_ms > 0) {
		int64_t in = m->start_ns + load->in_ms * NS_PER_MS;

		if (!end || in < end)
			m->in_ns = in;
	}
}

static void do_load(mp_media_t *m, struct mp_load *load)
{
	emit_event(m, MP_EVENT_LOADING, load->seq);

	close_input(m);

	if (m->cur) {
		bfree(m->cur->path);
		bfree(m->cur->format);
		bfree(m->cur->ffmpeg_options);
		bfree(m->cur);
	}
	m->cur = load;

	m->is_local_file = load->is_local_file;
	m->looping = load->looping;
	m->force_range = load->force_range;
	os_atomic_set_bool(&m->is_linear_alpha, load->is_linear_alpha);
	m->speed = load->is_local_file && load->speed >= MP_SPEED_MIN && load->speed <= MP_SPEED_MAX ? load->speed
												     : 100;
	m->state = MP_STATE_OPENING;
	publish(m);

	if (!open_input(m, load)) {
		close_input(m);
		m->state = MP_STATE_ERROR;
		publish_loaded(m, load->seq);
		emit_event(m, MP_EVENT_LOAD_FAILED, load->seq);
		return;
	}

	place_clip(m, load);

	if (m->in_ns > m->start_ns) {
		seek_exact(m, m->in_ns);
	} else if (!prepare_frames(m)) {
		close_input(m);
		m->state = MP_STATE_ERROR;
		publish_loaded(m, load->seq);
		emit_event(m, MP_EVENT_LOAD_FAILED, load->seq);
		return;
	}

	m->played_end = m->in_ns;

	switch (load->start) {
	case MP_START_PLAY:
		start_playing(m);
		break;
	case MP_START_HOLD:
		show_still(m);
		m->state = MP_STATE_PAUSED;
		break;
	case MP_START_CUE:
	default:
		cue_first_frame(m);
		m->state = MP_STATE_STOPPED;
		break;
	}

	publish_loaded(m, load->seq);
	emit_event(m, MP_EVENT_LOADED, load->seq);
}

/* ------------------------------------------------------------------------- */
/* commands */

static void do_restart(mp_media_t *m)
{
	if (!m->fmt) {
		/* a file that could not be read is given another go */
		if (m->cur && m->state == MP_STATE_ERROR) {
			struct mp_load *again = bmemdup(m->cur, sizeof(*again));

			again->path = bstrdup(m->cur->path);
			again->format = m->cur->format ? bstrdup(m->cur->format) : NULL;
			again->ffmpeg_options = m->cur->ffmpeg_options ? bstrdup(m->cur->ffmpeg_options) : NULL;
			again->start = MP_START_PLAY;
			do_load(m, again);
		}
		return;
	}

	if (m->is_local_file)
		seek_exact(m, m->in_ns);

	m->played_end = m->in_ns;
	start_playing(m);
}

static void do_pause(mp_media_t *m, bool pause)
{
	if (!m->fmt)
		return;

	if (pause) {
		switch (m->state) {
		case MP_STATE_PLAYING:
			m->state = MP_STATE_PAUSED;
			break;
		case MP_STATE_STOPPED:
		case MP_STATE_ENDED:
			/* hold the start of the clip on screen */
			show_still(m);
			m->state = MP_STATE_PAUSED;
			break;
		default:
			break;
		}
		return;
	}

	switch (m->state) {
	case MP_STATE_PAUSED:
		/* the frame on screen has been seen; play on from the one
		 * after it, timed from now */
		if (m->shown && m->has_video) {
			m->v.frame_ready = false;
			prepare_frames(m);
		}
		if (m->have_cur)
			drop_audio_before(m, m->cur_pts);
		m->shown = false;
		start_playing(m);
		break;
	case MP_STATE_STOPPED:
	case MP_STATE_ENDED:
		do_restart(m);
		break;
	default:
		break;
	}
}

static void do_stop(mp_media_t *m)
{
	if (!m->fmt)
		return;

	if (m->is_local_file)
		cue_clip(m);
	m->state = MP_STATE_STOPPED;
}

static void do_seek(mp_media_t *m, int64_t ms)
{
	if (!m->fmt || !m->is_local_file)
		return;

	seek_exact(m, m->in_ns + ms * NS_PER_MS);
	m->played_end = m->in_ns + ms * NS_PER_MS;

	if (m->state == MP_STATE_PLAYING) {
		m->need_anchor = true;
	} else {
		show_still(m);
		m->state = MP_STATE_PAUSED;
	}
}

static void do_step(mp_media_t *m, int frames)
{
	struct mp_decode *d = &m->v;

	if (!m->fmt || !m->has_video || !m->is_local_file || !frames)
		return;

	bool was_still = m->state != MP_STATE_PLAYING && m->state != MP_STATE_PAUSED;
	m->state = MP_STATE_PAUSED;

	if (frames > 0) {
		int left = frames;

		/* a frame that is ready but has not been seen - paused part way
		 * through, or sitting at the start - is the first step */
		if (!m->shown && d->frame_ready)
			left--;

		while (left > 0) {
			d->frame_ready = false;
			if (!prepare_frames(m) || !in_clip(m, d)) {
				/* stepped off the end: stay on the last frame */
				seek_exact(m, clip_end_ns(m));
				break;
			}
			left--;
		}
	} else {
		int64_t interval = frame_interval(m);
		int64_t from = m->have_cur && !was_still ? m->cur_pts : m->in_ns;
		int64_t target = from + (int64_t)frames * interval;

		if (target < m->in_ns)
			target = m->in_ns;

		/* a frame still in the history goes up straight away; the
		 * decode below then only puts the decoder back in step with
		 * what is on screen */
		int64_t found_pts;
		AVFrame *cached = hist_find(m, target, interval / 2, &found_pts);

		if (cached && m->v_still_cb) {
			struct obs_source_frame *frame = convert_video(m, cached);

			if (frame) {
				frame->timestamp = os_gettime_ns();
				m->v_still_cb(m->opaque, frame);
			}
		}

		/* the middle of the frame, so rounding in the interval cannot
		 * land the seek on the frame next to it */
		seek_exact(m, target + interval / 2);
	}

	show_still(m);
	drop_audio_before(m, m->cur_pts);
}

static void do_speed(mp_media_t *m, int speed)
{
	if (speed < MP_SPEED_MIN)
		speed = MP_SPEED_MIN;
	else if (speed > MP_SPEED_MAX)
		speed = MP_SPEED_MAX;

	if (!m->is_local_file || speed == m->speed)
		return;

	/* the clock carries on from where it has got to, at the new rate: the
	 * next frame falls due as late or as early as it now should */
	if (m->state == MP_STATE_PLAYING && !m->need_anchor) {
		uint64_t now = os_gettime_ns();
		clock_anchor(m, now, clock_media_at(m, now));
	}

	m->speed = speed;
}

static void free_load(struct mp_load *load)
{
	if (!load)
		return;
	bfree(load->path);
	bfree(load->format);
	bfree(load->ffmpeg_options);
	bfree(load);
}

/* Takes the next command off the queue. Commands that a later one in the queue
 * makes pointless are dropped on the way: a load overtaken by another load, a
 * seek or a speed overtaken by another of the same, and steps next to each
 * other are added together. */
static bool next_command(mp_media_t *m, struct mp_cmd *out)
{
	pthread_mutex_lock(&m->mutex);

	while (m->cmds.num) {
		struct mp_cmd cmd = m->cmds.array[0];
		bool superseded = false;

		da_erase(m->cmds, 0);

		if (cmd.type == MP_CMD_LOAD || cmd.type == MP_CMD_SEEK || cmd.type == MP_CMD_SPEED) {
			for (size_t i = 0; i < m->cmds.num; i++) {
				if (m->cmds.array[i].type == cmd.type ||
				    (cmd.type != MP_CMD_LOAD && m->cmds.array[i].type == MP_CMD_LOAD)) {
					superseded = true;
					break;
				}
			}
		} else if (cmd.type == MP_CMD_STEP) {
			while (m->cmds.num && m->cmds.array[0].type == MP_CMD_STEP) {
				cmd.value += m->cmds.array[0].value;
				da_erase(m->cmds, 0);
			}
		}

		if (superseded) {
			free_load(cmd.load);
			continue;
		}

		if (cmd.type == MP_CMD_LOAD) {
			/* nothing still queued is asking the load to give up */
			bool pending = false;
			for (size_t i = 0; i < m->cmds.num; i++)
				pending |= m->cmds.array[i].type == MP_CMD_LOAD || m->cmds.array[i].type == MP_CMD_KILL;
			if (!pending)
				os_atomic_set_bool(&m->interrupt, false);
		}

		*out = cmd;
		pthread_mutex_unlock(&m->mutex);
		return true;
	}

	pthread_mutex_unlock(&m->mutex);
	return false;
}

/* Carries out every queued command. False once the engine is being shut. */
static bool run_commands(mp_media_t *m)
{
	struct mp_cmd cmd;
	bool any = false;

	while (next_command(m, &cmd)) {
		any = true;

		switch (cmd.type) {
		case MP_CMD_KILL:
			free_load(cmd.load);
			return false;
		case MP_CMD_LOAD:
			do_load(m, cmd.load);
			cmd.load = NULL;
			break;
		case MP_CMD_CLOSE:
			close_input(m);
			m->state = MP_STATE_IDLE;
			break;
		case MP_CMD_RESTART:
			do_restart(m);
			break;
		case MP_CMD_PAUSE:
			do_pause(m, cmd.value != 0);
			break;
		case MP_CMD_STOP:
			do_stop(m);
			break;
		case MP_CMD_SEEK:
			do_seek(m, cmd.value);
			break;
		case MP_CMD_STEP:
			do_step(m, (int)cmd.value);
			break;
		case MP_CMD_SPEED:
			do_speed(m, (int)cmd.value);
			break;
		case MP_CMD_LOOPING:
			m->looping = cmd.value != 0;
			break;
		}

		free_load(cmd.load);
	}

	if (any)
		publish(m);

	return true;
}

/* ------------------------------------------------------------------------- */
/* the media thread */

static void *media_thread(void *opaque)
{
	mp_media_t *m = opaque;
	uint64_t wait_ns = 0;
	uint64_t last_publish = 0;

	os_set_thread_name("mp_media_thread");

	for (;;) {
		if (m->state == MP_STATE_PLAYING && wait_ns) {
			unsigned long ms = (unsigned long)(wait_ns / NS_PER_MS);
			os_event_timedwait(m->wake, ms ? ms : 1);
		} else if (m->state != MP_STATE_PLAYING) {
			os_event_wait(m->wake);
		}

		if (!run_commands(m))
			break;

		wait_ns = 0;

		if (m->state == MP_STATE_PLAYING) {
			wait_ns = play_due(m);

			/* the position is published as it moves, but not on
			 * every frame */
			uint64_t now = os_gettime_ns();
			if (now - last_publish > 20 * NS_PER_MS) {
				publish(m);
				last_publish = now;
			}
		}
	}

	return NULL;
}

/* ------------------------------------------------------------------------- */
/* the callers' side */

static void post(media_playback_t *mp, enum mp_cmd_type type, int64_t value, struct mp_load *load)
{
	struct mp_cmd cmd = {.type = type, .value = value, .load = load};

	pthread_mutex_lock(&mp->mutex);
	da_push_back(mp->cmds, &cmd);
	if (type == MP_CMD_LOAD || type == MP_CMD_KILL)
		os_atomic_set_bool(&mp->interrupt, true);
	pthread_mutex_unlock(&mp->mutex);

	os_event_signal(mp->wake);
}

media_playback_t *media_playback_create(const struct mp_media_info *info)
{
	static bool initialized = false;

	if (!initialized) {
		avdevice_register_all();
		avformat_network_init();
		initialized = true;
	}

	media_playback_t *mp = bzalloc(sizeof(*mp));

	mp->opaque = info->opaque;
	mp->v_cb = info->v_cb;
	mp->v_preload_cb = info->v_preload_cb;
	mp->v_still_cb = info->v_still_cb;
	mp->a_cb = info->a_cb;
	mp->event_cb = info->event_cb;
	mp->speed = 100;
	mp->state = MP_STATE_IDLE;
	mp->scale_src = AV_PIX_FMT_NONE;
	mp->status.speed = 100;
	da_init(mp->cmds);
	da_init(mp->packet_pool);

	if (pthread_mutex_init(&mp->mutex, NULL) != 0) {
		blog(LOG_WARNING, "MP: Failed to init mutex");
		bfree(mp);
		return NULL;
	}

	if (os_event_init(&mp->wake, OS_EVENT_TYPE_AUTO) != 0) {
		blog(LOG_WARNING, "MP: Failed to init event");
		pthread_mutex_destroy(&mp->mutex);
		bfree(mp);
		return NULL;
	}

	if (pthread_create(&mp->thread, NULL, media_thread, mp) != 0) {
		blog(LOG_WARNING, "MP: Could not create media thread");
		os_event_destroy(mp->wake);
		pthread_mutex_destroy(&mp->mutex);
		bfree(mp);
		return NULL;
	}

	mp->thread_valid = true;
	return mp;
}

void media_playback_destroy(media_playback_t *mp)
{
	if (!mp)
		return;

	if (mp->thread_valid) {
		post(mp, MP_CMD_KILL, 0, NULL);
		pthread_join(mp->thread, NULL);
	}

	for (size_t i = 0; i < mp->cmds.num; i++)
		free_load(mp->cmds.array[i].load);
	da_free(mp->cmds);

	close_input(mp);
	free_load(mp->cur);
	mp_decode_free(&mp->v);
	mp_decode_free(&mp->a);
	av_buffer_unref(&mp->hw_device);

	for (size_t i = 0; i < mp->packet_pool.num; i++)
		av_packet_free(&mp->packet_pool.array[i]);
	da_free(mp->packet_pool);

	free_scaling(mp);
	os_event_destroy(mp->wake);
	pthread_mutex_destroy(&mp->mutex);
	bfree(mp);
}

uint64_t media_playback_load(media_playback_t *mp, const struct mp_load_info *info)
{
	if (!mp || !info || !info->path || !*info->path)
		return 0;

	struct mp_load *load = bzalloc(sizeof(*load));

	load->path = bstrdup(info->path);
	load->format = info->format && *info->format ? bstrdup(info->format) : NULL;
	load->ffmpeg_options = info->ffmpeg_options && *info->ffmpeg_options ? bstrdup(info->ffmpeg_options) : NULL;
	load->buffering = info->buffering;
	load->speed = info->speed;
	load->is_local_file = info->is_local_file;
	load->looping = info->looping;
	load->hw = info->hardware_decoding;
	load->reconnecting = info->reconnecting;
	load->force_range = info->force_range;
	load->is_linear_alpha = info->is_linear_alpha;
	load->in_ms = info->in_ms > 0 ? info->in_ms : 0;
	load->out_ms = info->out_ms > 0 ? info->out_ms : 0;
	load->tail_ms = info->tail_ms > 0 ? info->tail_ms : 0;
	load->start = info->start;

	pthread_mutex_lock(&mp->mutex);
	load->seq = ++mp->next_seq;
	mp->status.load_seq = load->seq;
	pthread_mutex_unlock(&mp->mutex);

	post(mp, MP_CMD_LOAD, 0, load);
	return load->seq;
}

void media_playback_close(media_playback_t *mp)
{
	if (mp)
		post(mp, MP_CMD_CLOSE, 0, NULL);
}

void media_playback_restart(media_playback_t *mp)
{
	if (mp)
		post(mp, MP_CMD_RESTART, 0, NULL);
}

void media_playback_pause(media_playback_t *mp, bool pause)
{
	if (mp)
		post(mp, MP_CMD_PAUSE, pause, NULL);
}

void media_playback_stop(media_playback_t *mp)
{
	if (mp)
		post(mp, MP_CMD_STOP, 0, NULL);
}

void media_playback_seek(media_playback_t *mp, int64_t ms)
{
	if (mp)
		post(mp, MP_CMD_SEEK, ms < 0 ? 0 : ms, NULL);
}

void media_playback_step_frames(media_playback_t *mp, int frames)
{
	if (mp && frames)
		post(mp, MP_CMD_STEP, frames, NULL);
}

void media_playback_set_speed(media_playback_t *mp, int speed)
{
	if (mp)
		post(mp, MP_CMD_SPEED, speed, NULL);
}

void media_playback_set_looping(media_playback_t *mp, bool looping)
{
	if (mp)
		post(mp, MP_CMD_LOOPING, looping, NULL);
}

void media_playback_set_is_linear_alpha(media_playback_t *mp, bool is_linear_alpha)
{
	if (mp)
		os_atomic_set_bool(&mp->is_linear_alpha, is_linear_alpha);
}

void media_playback_get_status(media_playback_t *mp, struct mp_status *status)
{
	if (!mp) {
		memset(status, 0, sizeof(*status));
		status->speed = 100;
		return;
	}

	pthread_mutex_lock(&mp->mutex);
	*status = mp->status;
	pthread_mutex_unlock(&mp->mutex);
}
