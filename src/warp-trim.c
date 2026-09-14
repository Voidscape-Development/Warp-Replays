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

#include <string.h>

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#pragma warning(disable : 4204)
#endif

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "warp-trim.h"

#define WARP_TRIM_LOG(level, format, ...) blog(level, "[Warp Trim]: " format, ##__VA_ARGS__)

/* What a trimmed clip is called: the name of the clip it was cut from with the
 * length it was cut to on the end, so the two sit next to each other in the
 * folder and a clip cut twice to the same length is the same file both times.
 *
 * 'tail' goes on before the extension, which is how the file is written under a
 * name of its own and only takes the real one once it is whole: the extension
 * has to stay where it is for the muxer to be picked from it. */
static char *warp_trim_name(const char *path, int seconds, const char *tail)
{
	const char *dot = strrchr(path, '.');
	const char *slash = strrchr(path, '/');
	struct dstr name = {0};

#ifdef _WIN32
	const char *back = strrchr(path, '\\');

	if (back > slash)
		slash = back;
#endif

	/* a dot in a folder's name is not an extension */
	if (dot && slash && dot < slash)
		dot = NULL;

	if (dot)
		dstr_ncat(&name, path, (size_t)(dot - path));
	else
		dstr_copy(&name, path);

	dstr_catf(&name, "-%ds", seconds);

	if (tail)
		dstr_cat(&name, tail);
	if (dot)
		dstr_cat(&name, dot);

	return name.array;
}

/* the streams worth carrying over: the picture, the sound and anything written
 * over them, but not the cover art and data streams a player would ignore */
static bool warp_trim_wanted(enum AVMediaType type)
{
	return type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO || type == AVMEDIA_TYPE_SUBTITLE;
}

/* Sets the output up with the same streams as the input, and says which of the
 * input's streams each one came from - -1 for the ones left behind. 'video' is
 * left holding the input's first video stream, or -1 when it has none. */
static bool warp_trim_map_streams(AVFormatContext *ic, AVFormatContext *oc, int *map, int *video)
{
	*video = -1;

	for (unsigned int i = 0; i < ic->nb_streams; i++) {
		AVStream *in = ic->streams[i];
		AVStream *out;

		map[i] = -1;

		if (!warp_trim_wanted(in->codecpar->codec_type))
			continue;

		out = avformat_new_stream(oc, NULL);

		if (!out)
			return false;

		if (avcodec_parameters_copy(out->codecpar, in->codecpar) < 0)
			return false;

		/* the tag belongs to the format that was read, not to the one
		 * being written */
		out->codecpar->codec_tag = 0;
		out->time_base = in->time_base;
		out->disposition = in->disposition;
		av_dict_copy(&out->metadata, in->metadata, 0);

		map[i] = out->index;

		if (*video < 0 && in->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
			*video = (int)i;
	}

	return true;
}

/* Copies what is left of the file, from wherever the demuxer has been seeked
 * to, into the output, with the timestamps brought back to zero.
 *
 * Where zero is, is the first video keyframe: a clip that began anywhere else
 * would open on a frame that cannot be decoded on its own. Everything that came
 * before it goes - a fraction of a second of sound at the front, at most - and
 * so does anything that still lands before it once the packets are moved back,
 * which is the interleaving running a little ahead of the picture. */
static bool warp_trim_copy_packets(AVFormatContext *ic, AVFormatContext *oc, const int *map, int video)
{
	AVPacket *pkt = av_packet_alloc();
	bool started = false;
	bool written = false;
	int64_t zero = 0;
	int ret;

	if (!pkt)
		return false;

	while ((ret = av_read_frame(ic, pkt)) >= 0) {
		AVStream *in = ic->streams[pkt->stream_index];
		const int out_index = map[pkt->stream_index];
		int64_t ts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
		int64_t offset;

		if (out_index < 0 || ts == AV_NOPTS_VALUE) {
			av_packet_unref(pkt);
			continue;
		}

		if (!started) {
			/* with no picture in the file there is no keyframe to
			 * wait for: the first packet is the start */
			if (video >= 0 && (pkt->stream_index != video || !(pkt->flags & AV_PKT_FLAG_KEY))) {
				av_packet_unref(pkt);
				continue;
			}

			zero = av_rescale_q(ts, in->time_base, AV_TIME_BASE_Q);
			started = true;
		}

		offset = av_rescale_q(zero, AV_TIME_BASE_Q, in->time_base);

		if (pkt->pts != AV_NOPTS_VALUE)
			pkt->pts -= offset;
		if (pkt->dts != AV_NOPTS_VALUE)
			pkt->dts -= offset;

		if ((pkt->dts != AV_NOPTS_VALUE && pkt->dts < 0) || (pkt->pts != AV_NOPTS_VALUE && pkt->pts < 0)) {
			av_packet_unref(pkt);
			continue;
		}

		av_packet_rescale_ts(pkt, in->time_base, oc->streams[out_index]->time_base);
		pkt->stream_index = out_index;
		pkt->pos = -1;

		ret = av_interleaved_write_frame(oc, pkt);

		/* the packet belongs to the muxer from here, whether it took it
		 * or not */
		av_packet_unref(pkt);

		if (ret < 0) {
			WARP_TRIM_LOG(LOG_WARNING, "could not write a packet: %s", av_err2str(ret));
			av_packet_free(&pkt);
			return false;
		}

		written = true;
	}

	av_packet_free(&pkt);

	if (ret != AVERROR_EOF && ret < 0)
		WARP_TRIM_LOG(LOG_WARNING, "stopped reading early: %s", av_err2str(ret));

	return written;
}

char *warp_trim_tail(const char *path, int seconds)
{
	AVFormatContext *ic = NULL;
	AVFormatContext *oc = NULL;
	char *out_path = NULL;
	char *part = NULL;
	int *map = NULL;
	int64_t want;
	int64_t start;
	int64_t mark;
	int64_t seek_ts;
	int video = -1;
	int ret;
	bool ok = false;

	if (!path || !*path || seconds <= 0)
		return NULL;

	ret = avformat_open_input(&ic, path, NULL, NULL);

	if (ret < 0) {
		WARP_TRIM_LOG(LOG_WARNING, "could not open '%s': %s", path, av_err2str(ret));
		return NULL;
	}

	ret = avformat_find_stream_info(ic, NULL);

	if (ret < 0) {
		WARP_TRIM_LOG(LOG_WARNING, "could not read '%s': %s", path, av_err2str(ret));
		goto done;
	}

	if (ic->duration == AV_NOPTS_VALUE || ic->duration <= 0) {
		WARP_TRIM_LOG(LOG_WARNING, "'%s' does not say how long it is, leaving it whole", path);
		goto done;
	}

	want = (int64_t)seconds * AV_TIME_BASE;

	/* A clip that is already that short is the clip: the replay buffer was
	 * not holding any more than this. */
	if (ic->duration <= want) {
		WARP_TRIM_LOG(LOG_INFO, "'%s' is %.1fs, already inside the %ds asked for", path,
			      (double)ic->duration / AV_TIME_BASE, seconds);
		goto done;
	}

	if (!ic->nb_streams)
		goto done;

	out_path = warp_trim_name(path, seconds, NULL);

	/* The name says which clip this is and how long it was cut to, so a
	 * file already under it is the same few seconds of the same recording -
	 * two flows asking for the same length, or the same clip added again
	 * after the fact. It is written under a name of its own until it is
	 * whole, so what is there is never half a clip. */
	if (os_file_exists(out_path) && os_get_file_size(out_path) > 0) {
		WARP_TRIM_LOG(LOG_INFO, "the last %ds of '%s' is already cut, using '%s'", seconds, path, out_path);
		ok = true;
		goto done;
	}

	part = warp_trim_name(path, seconds, ".part");

	ret = avformat_alloc_output_context2(&oc, NULL, NULL, part);

	if (ret < 0 || !oc) {
		WARP_TRIM_LOG(LOG_WARNING, "nothing can write '%s': %s", part, av_err2str(ret));
		goto done;
	}

	map = bmalloc(sizeof(int) * ic->nb_streams);

	if (!warp_trim_map_streams(ic, oc, map, &video)) {
		WARP_TRIM_LOG(LOG_WARNING, "could not lay '%s' out to be cut", path);
		goto done;
	}

	if (!oc->nb_streams) {
		WARP_TRIM_LOG(LOG_WARNING, "'%s' has nothing in it to cut", path);
		goto done;
	}

	start = ic->start_time == AV_NOPTS_VALUE ? 0 : ic->start_time;
	mark = start + ic->duration - want;
	seek_ts = video >= 0 ? av_rescale_q(mark, AV_TIME_BASE_Q, ic->streams[video]->time_base) : mark;

	/* back to the keyframe at or before the mark: what a clip opens on has
	 * to be a frame that stands on its own */
	ret = av_seek_frame(ic, video, seek_ts, AVSEEK_FLAG_BACKWARD);

	if (ret < 0) {
		WARP_TRIM_LOG(LOG_WARNING, "could not seek '%s': %s", path, av_err2str(ret));
		goto done;
	}

	if (!(oc->oformat->flags & AVFMT_NOFILE)) {
		ret = avio_open(&oc->pb, part, AVIO_FLAG_WRITE);

		if (ret < 0) {
			WARP_TRIM_LOG(LOG_WARNING, "could not write '%s': %s", part, av_err2str(ret));
			goto done;
		}
	}

	ret = avformat_write_header(oc, NULL);

	if (ret < 0) {
		WARP_TRIM_LOG(LOG_WARNING, "could not start '%s': %s", part, av_err2str(ret));
		goto done;
	}

	if (!warp_trim_copy_packets(ic, oc, map, video)) {
		WARP_TRIM_LOG(LOG_WARNING, "nothing came out of the last %ds of '%s'", seconds, path);
		goto done;
	}

	/* the trailer is where the index goes, so a clip that is not given one
	 * is a clip nothing can seek */
	ret = av_write_trailer(oc);

	if (ret < 0) {
		WARP_TRIM_LOG(LOG_WARNING, "could not finish '%s': %s", part, av_err2str(ret));
		goto done;
	}

	if (oc->pb)
		avio_closep(&oc->pb);

	avformat_free_context(oc);
	oc = NULL;

	if (os_rename(part, out_path) != 0) {
		WARP_TRIM_LOG(LOG_WARNING, "could not put '%s' in place as '%s'", part, out_path);
		goto done;
	}

	WARP_TRIM_LOG(LOG_INFO, "cut the last %ds of '%s' into '%s'", seconds, path, out_path);
	ok = true;

done:
	if (oc) {
		if (oc->pb)
			avio_closep(&oc->pb);

		avformat_free_context(oc);
	}

	if (ic)
		avformat_close_input(&ic);

	bfree(map);

	if (!ok) {
		/* half a clip is worse than none: the flow is fed the whole one
		 * instead */
		if (part)
			os_unlink(part);

		bfree(out_path);
		out_path = NULL;
	}

	bfree(part);

	return out_path;
}
