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

#include "decode.h"

#include "media-playback.h"
#include "media.h"
#include <libavutil/mastering_display_metadata.h>

enum AVHWDeviceType hw_priority[] = {
	AV_HWDEVICE_TYPE_CUDA,  AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_DXVA2,        AV_HWDEVICE_TYPE_VAAPI,
	AV_HWDEVICE_TYPE_VDPAU, AV_HWDEVICE_TYPE_QSV,     AV_HWDEVICE_TYPE_VIDEOTOOLBOX, AV_HWDEVICE_TYPE_NONE,
};

static bool has_hw_type(const AVCodec *c, enum AVHWDeviceType type, enum AVPixelFormat *hw_format)
{
	for (int i = 0;; i++) {
		const AVCodecHWConfig *config = avcodec_get_hw_config(c, i);
		if (!config) {
			break;
		}

		if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX && config->device_type == type) {
			*hw_format = config->pix_fmt;
			return true;
		}
	}

	return false;
}

/* The hardware device is the engine's rather than the decoder's: creating one
 * is the slowest part of opening a file, and a new clip is decoded on the same
 * device as the last one. */
static void init_hw_decoder(struct mp_decode *d, AVCodecContext *c)
{
	struct media_playback *m = d->m;
	enum AVHWDeviceType *priority = hw_priority;

	if (m->hw_device && has_hw_type(d->codec, m->hw_type, &d->hw_format)) {
		c->hw_device_ctx = av_buffer_ref(m->hw_device);
		c->opaque = d;
		d->hw = true;
		return;
	}

	while (*priority != AV_HWDEVICE_TYPE_NONE) {
		if (has_hw_type(d->codec, *priority, &d->hw_format)) {
			AVBufferRef *hw_ctx = NULL;
			int ret = av_hwdevice_ctx_create(&hw_ctx, *priority, NULL, NULL, 0);

			if (ret == 0) {
				av_buffer_unref(&m->hw_device);
				m->hw_device = hw_ctx;
				m->hw_type = *priority;

				c->hw_device_ctx = av_buffer_ref(hw_ctx);
				c->opaque = d;
				d->hw = true;
				return;
			}
		}

		priority++;
	}
}

static int mp_open_codec(struct mp_decode *d, bool hw)
{
	AVCodecContext *c;
	int ret;

	c = avcodec_alloc_context3(d->codec);
	if (!c) {
		blog(LOG_WARNING, "MP: Failed to allocate context");
		return -1;
	}

	ret = avcodec_parameters_to_context(c, d->stream->codecpar);
	if (ret < 0)
		goto fail;

	d->hw = false;

	if (hw)
		init_hw_decoder(d, c);

	if (c->thread_count == 1 && c->codec_id != AV_CODEC_ID_PNG && c->codec_id != AV_CODEC_ID_TIFF &&
	    c->codec_id != AV_CODEC_ID_JPEG2000 && c->codec_id != AV_CODEC_ID_MPEG4 && c->codec_id != AV_CODEC_ID_WEBP)
		c->thread_count = 0;

	ret = avcodec_open2(c, d->codec, NULL);
	if (ret < 0)
		goto fail;

	d->decoder = c;
	return ret;

fail:
	avcodec_free_context(&c);
	return ret;
}

static uint16_t get_max_luminance(const AVStream *stream)
{
	uint32_t max_luminance = 0;

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(60, 31, 102)
	for (int i = 0; i < stream->nb_side_data; i++) {
		const AVPacketSideData *const sd = &stream->side_data[i];
#else
	for (int i = 0; i < stream->codecpar->nb_coded_side_data; i++) {
		const AVPacketSideData *const sd = &stream->codecpar->coded_side_data[i];
#endif
		switch (sd->type) {
		case AV_PKT_DATA_MASTERING_DISPLAY_METADATA: {
			const AVMasteringDisplayMetadata *mastering = (AVMasteringDisplayMetadata *)sd->data;
			if (mastering->has_luminance) {
				max_luminance = (uint32_t)(av_q2d(mastering->max_luminance) + 0.5);
			}

			break;
		}
		case AV_PKT_DATA_CONTENT_LIGHT_LEVEL: {
			const AVContentLightMetadata *const md = (AVContentLightMetadata *)&sd->data;
			max_luminance = md->MaxCLL;
			break;
		}
		default:
			break;
		}
	}

	return max_luminance;
}

static int codecpar_channels(const AVCodecParameters *p)
{
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 24, 100)
	return p->ch_layout.nb_channels;
#else
	return p->channels;
#endif
}

/* whether a decoder opened with 'a' can be handed a stream described by 'b'
 * without being opened again */
static bool same_codec_params(const AVCodecParameters *a, const AVCodecParameters *b)
{
	if (a->codec_id != b->codec_id || a->format != b->format || a->width != b->width || a->height != b->height ||
	    a->sample_rate != b->sample_rate || codecpar_channels(a) != codecpar_channels(b) ||
	    a->extradata_size != b->extradata_size)
		return false;

	return !a->extradata_size || memcmp(a->extradata, b->extradata, (size_t)a->extradata_size) == 0;
}

static const AVCodec *find_decoder(const AVStream *stream)
{
	enum AVCodecID id = stream->codecpar->codec_id;

	if (id == AV_CODEC_ID_VP8 || id == AV_CODEC_ID_VP9) {
		AVDictionaryEntry *tag = av_dict_get(stream->metadata, "alpha_mode", NULL, AV_DICT_IGNORE_SUFFIX);

		if (tag && strcmp(tag->value, "1") == 0) {
			const AVCodec *codec =
				avcodec_find_decoder_by_name(id == AV_CODEC_ID_VP8 ? "libvpx" : "libvpx-vp9");

			if (codec)
				return codec;
		}
	}

	return avcodec_find_decoder(id);
}

/* lets go of the decoder itself, keeping the packet queue and the frames */
static void mp_decode_close_codec(struct mp_decode *d)
{
	if (d->decoder)
		avcodec_free_context(&d->decoder);
	if (d->params)
		avcodec_parameters_free(&d->params);
	d->codec = NULL;
	d->hw = false;
}

bool mp_decode_open(struct mp_decode *d, struct media_playback *m, AVStream *stream, bool audio, bool hw)
{
	const AVCodec *codec = find_decoder(stream);
	enum AVMediaType type = audio ? AVMEDIA_TYPE_AUDIO : AVMEDIA_TYPE_VIDEO;

	d->m = m;
	d->audio = audio;

	mp_decode_clear_packets(d);

	if (!codec) {
		blog(LOG_WARNING, "MP: Failed to find %s codec", av_get_media_type_string(type));
		mp_decode_close_codec(d);
		return false;
	}

	bool reuse = d->decoder && d->params && d->codec == codec && d->params_hw == hw &&
		     same_codec_params(d->params, stream->codecpar);

	d->stream = stream;

	if (reuse) {
		avcodec_flush_buffers(d->decoder);
	} else {
		mp_decode_close_codec(d);

		d->codec = codec;

		int ret = mp_open_codec(d, hw);
		if (ret < 0) {
			blog(LOG_WARNING, "MP: Failed to open %s decoder: %s", av_get_media_type_string(type),
			     av_err2str(ret));
			d->codec = NULL;
			return false;
		}

		d->params = avcodec_parameters_alloc();
		if (d->params)
			avcodec_parameters_copy(d->params, stream->codecpar);
		d->params_hw = hw;

#if LIBAVCODEC_VERSION_MAJOR < 60
		if (d->codec->capabilities & CODEC_CAP_TRUNC)
			d->decoder->flags |= CODEC_FLAG_TRUNC;
#endif
	}

	if (!audio)
		d->max_luminance = get_max_luminance(stream);

	if (!d->sw_frame)
		d->sw_frame = av_frame_alloc();
	if (d->hw && !d->hw_frame)
		d->hw_frame = av_frame_alloc();
	if (!d->orig_pkt)
		d->orig_pkt = av_packet_alloc();
	if (!d->pkt)
		d->pkt = av_packet_alloc();

	if (!d->sw_frame || (d->hw && !d->hw_frame) || !d->orig_pkt || !d->pkt) {
		blog(LOG_WARNING, "MP: Failed to allocate %s frame", av_get_media_type_string(type));
		return false;
	}

	d->in_frame = d->hw ? d->hw_frame : d->sw_frame;
	d->frame = NULL;
	d->frame_ready = false;
	d->eof = false;
	d->got_first_keyframe = false;
	d->frame_pts = 0;
	d->next_pts = 0;
	d->last_duration = 0;
	d->packet_pending = false;

	return true;
}

extern void mp_media_free_packet(struct media_playback *m, AVPacket *pkt);

void mp_decode_clear_packets(struct mp_decode *d)
{
	if (d->packet_pending) {
		av_packet_unref(d->orig_pkt);
		d->packet_pending = false;
	}

	while (d->packets.size) {
		AVPacket *pkt;
		deque_pop_front(&d->packets, &pkt, sizeof(pkt));
		mp_media_free_packet(d->m, pkt);
	}
}

void mp_decode_free(struct mp_decode *d)
{
	mp_decode_clear_packets(d);
	deque_free(&d->packets);

	av_packet_free(&d->pkt);
	av_packet_free(&d->orig_pkt);

	if (d->hw_frame) {
		av_frame_unref(d->hw_frame);
		av_free(d->hw_frame);
	}

	mp_decode_close_codec(d);

	if (d->sw_frame) {
		av_frame_unref(d->sw_frame);
		av_free(d->sw_frame);
	}

	memset(d, 0, sizeof(*d));
}

void mp_decode_push_packet(struct mp_decode *decode, AVPacket *packet)
{
	deque_push_back(&decode->packets, &packet, sizeof(packet));
}

static inline int64_t get_estimated_duration(struct mp_decode *d, int64_t last_pts)
{
	if (d->audio) {
		return av_rescale_q(d->in_frame->nb_samples, (AVRational){1, d->in_frame->sample_rate},
				    (AVRational){1, 1000000000});
	} else {
		if (last_pts)
			return d->frame_pts - last_pts;

		if (d->last_duration)
			return d->last_duration;

		return av_rescale_q(d->decoder->time_base.num, d->decoder->time_base, (AVRational){1, 1000000000});
	}
}

static int decode_packet(struct mp_decode *d, int *got_frame)
{
	int ret;
	*got_frame = 0;

	ret = avcodec_receive_frame(d->decoder, d->in_frame);
	if (ret != 0 && ret != AVERROR(EAGAIN)) {
		if (ret == AVERROR_EOF)
			ret = 0;
		return ret;
	}

	if (ret != 0) {
		ret = avcodec_send_packet(d->decoder, d->pkt);
		if (ret != 0 && ret != AVERROR(EAGAIN)) {
			if (ret == AVERROR_EOF)
				ret = 0;
			return ret;
		}

		ret = avcodec_receive_frame(d->decoder, d->in_frame);
		if (ret != 0 && ret != AVERROR(EAGAIN)) {
			if (ret == AVERROR_EOF)
				ret = 0;
			return ret;
		}

		*got_frame = (ret == 0);
		ret = d->pkt->size;
	} else {
		ret = 0;
		*got_frame = 1;
	}

	if (*got_frame && d->hw) {
		if (d->hw_frame->format != d->hw_format) {
			d->frame = d->hw_frame;
			return ret;
		}

		/* the frame history keeps references to sw_frame's buffers
		 * (see mp_media_hist_push), so the transfer below must get a
		 * fresh buffer every time instead of writing into the
		 * previous one in place */
		av_frame_unref(d->sw_frame);

		int err = av_hwframe_transfer_data(d->sw_frame, d->hw_frame, 0);
		if (err == 0) {
			err = av_frame_copy_props(d->sw_frame, d->hw_frame);
		}
		if (err) {
			ret = 0;
			*got_frame = false;
		}
	}

	d->frame = d->sw_frame;
	return ret;
}

bool mp_decode_next(struct mp_decode *d)
{
	bool eof = d->m->eof;
	int got_frame;
	int ret;

	d->frame_ready = false;

	if (!eof && !d->packets.size)
		return true;

	while (!d->frame_ready) {
		if (!d->packet_pending) {
			if (!d->packets.size) {
				if (eof) {
					d->pkt->data = NULL;
					d->pkt->size = 0;
				} else {
					return true;
				}
			} else {
				mp_media_free_packet(d->m, d->orig_pkt);
				deque_pop_front(&d->packets, &d->orig_pkt, sizeof(d->orig_pkt));
				av_packet_ref(d->pkt, d->orig_pkt);
				d->packet_pending = true;
			}
		}

		ret = decode_packet(d, &got_frame);

		if (!got_frame && ret == 0) {
			d->eof = true;
			return true;
		}
		if (ret < 0) {
#ifdef DETAILED_DEBUG_INFO
			blog(LOG_DEBUG, "MP: decode failed: %s", av_err2str(ret));
#endif

			if (d->packet_pending) {
				av_packet_unref(d->orig_pkt);
				av_packet_unref(d->pkt);
				d->packet_pending = false;
			}
			return true;
		}

		d->frame_ready = !!got_frame;

		if (d->packet_pending) {
			if (d->pkt->size) {
				d->pkt->data += ret;
				d->pkt->size -= ret;
			}

			if (d->pkt->size <= 0) {
				av_packet_unref(d->orig_pkt);
				av_packet_unref(d->pkt);
				d->packet_pending = false;
			}
		}
	}

	if (d->frame_ready) {
		int64_t last_pts = d->frame_pts;

		if (d->in_frame->best_effort_timestamp == AV_NOPTS_VALUE)
			d->frame_pts = d->next_pts;
		else
			d->frame_pts = av_rescale_q(d->in_frame->best_effort_timestamp, d->stream->time_base,
						    (AVRational){1, 1000000000});
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 30, 100)
		int64_t duration = d->in_frame->duration;
#else
		int64_t duration = d->in_frame->pkt_duration;
#endif
		if (!duration)
			duration = get_estimated_duration(d, last_pts);
		else
			duration = av_rescale_q(duration, d->stream->time_base, (AVRational){1, 1000000000});

		d->last_duration = duration;
		d->next_pts = d->frame_pts + duration;

		if (!d->audio && d->m->is_local_file)
			mp_media_hist_push(d->m, d);
	}

	return true;
}

void mp_decode_flush(struct mp_decode *d)
{
	avcodec_flush_buffers(d->decoder);
	mp_decode_clear_packets(d);
	d->eof = false;
	d->frame_pts = 0;
	d->frame_ready = false;
	d->next_pts = 0;
}
