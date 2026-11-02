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

#include "stretch.h"

#include <stdio.h>
#include <string.h>

#include <util/base.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#endif

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

/* The channel layout API this needs arrived in FFmpeg 5.1; anything older
 * plays sound the plain way. */
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
#define MP_STRETCH_SUPPORTED 1
#else
#define MP_STRETCH_SUPPORTED 0
#endif

/* A gap this long between where the sound going in was expected to be due and
 * where it actually is due is a pause, a seek or a loop rather than jitter. */
#define MP_STRETCH_GAP_NS 50000000ULL

static void free_graph(struct mp_stretch *st)
{
	avfilter_graph_free(&st->graph);
	st->src = NULL;
	st->sink = NULL;
	st->started = false;
	st->in_pts = 0;
}

void mp_stretch_reset(struct mp_stretch *st)
{
	free_graph(st);
}

void mp_stretch_free(struct mp_stretch *st)
{
	free_graph(st);
	av_frame_free(&st->out);
}

#if MP_STRETCH_SUPPORTED

static bool build_graph(struct mp_stretch *st, const AVFrame *in, int speed)
{
	const AVFilter *abuffer = avfilter_get_by_name("abuffer");
	const AVFilter *atempo = avfilter_get_by_name("atempo");
	const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
	char layout[128];
	char args[256];

	if (!abuffer || !atempo || !abuffersink)
		return false;

	AVChannelLayout ch;
	if (in->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
		av_channel_layout_default(&ch, in->ch_layout.nb_channels);
	else if (av_channel_layout_copy(&ch, &in->ch_layout) < 0)
		return false;

	int ret = av_channel_layout_describe(&ch, layout, sizeof(layout));
	av_channel_layout_uninit(&ch);
	if (ret < 0)
		return false;

	const char *fmt = av_get_sample_fmt_name(in->format);
	if (!fmt)
		return false;

	free_graph(st);

	st->graph = avfilter_graph_alloc();
	if (!st->graph)
		return false;

	snprintf(args, sizeof(args), "time_base=1/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s", in->sample_rate,
		 in->sample_rate, fmt, layout);

	if (avfilter_graph_create_filter(&st->src, abuffer, "in", args, NULL, st->graph) < 0 ||
	    avfilter_graph_create_filter(&st->sink, abuffersink, "out", NULL, NULL, st->graph) < 0)
		goto fail;

	/* atempo takes 0.5 to 2.0, so a speed outside that is several of them,
	 * each as close to the edge as it can go and the last one the rest */
	AVFilterContext *prev = st->src;
	double left = speed / 100.0;
	int n = 0;

	while (left > 0.0) {
		double tempo = left < 0.5 ? 0.5 : left > 2.0 ? 2.0 : left;
		char name[32];
		char tempo_args[32];
		AVFilterContext *f = NULL;

		snprintf(name, sizeof(name), "tempo%d", n++);
		snprintf(tempo_args, sizeof(tempo_args), "tempo=%.6f", tempo);

		if (avfilter_graph_create_filter(&f, atempo, name, tempo_args, NULL, st->graph) < 0 ||
		    avfilter_link(prev, 0, f, 0) < 0)
			goto fail;

		prev = f;
		left /= tempo;

		if (left > 0.999 && left < 1.001)
			break;
	}

	if (avfilter_link(prev, 0, st->sink, 0) < 0 || avfilter_graph_config(st->graph, NULL) < 0)
		goto fail;

	st->speed = speed;
	st->rate = in->sample_rate;
	st->format = in->format;
	st->channels = in->ch_layout.nb_channels;
	st->in_pts = 0;
	st->started = false;
	return true;

fail:
	free_graph(st);
	return false;
}

bool mp_stretch_push(struct mp_stretch *st, AVFrame *in, int speed, uint64_t due, mp_stretch_out_cb out, void *param)
{
	if (!in || in->sample_rate <= 0 || in->nb_samples <= 0 || speed <= 0)
		return false;

	if (!st->out) {
		st->out = av_frame_alloc();
		if (!st->out)
			return false;
	}

	bool same = st->graph && st->speed == speed && st->rate == in->sample_rate && st->format == in->format &&
		    st->channels == in->ch_layout.nb_channels;

	/* sound that does not carry on from what went in last starts afresh,
	 * rather than being stretched onto the end of it */
	if (same && st->started) {
		uint64_t gap = due > st->expect_due ? due - st->expect_due : st->expect_due - due;
		if (gap > MP_STRETCH_GAP_NS)
			same = false;
	}

	if (!same && !build_graph(st, in, speed))
		return false;

	if (!st->started) {
		st->next_wall = due;
		st->started = true;
	}

	uint64_t in_wall = (uint64_t)in->nb_samples * 1000000000ULL * 100 / ((uint64_t)in->sample_rate * speed);
	st->expect_due = due + in_wall;

	int64_t pts = in->pts;
	in->pts = st->in_pts;
	st->in_pts += in->nb_samples;

	int ret = av_buffersrc_add_frame_flags(st->src, in, AV_BUFFERSRC_FLAG_KEEP_REF);
	in->pts = pts;

	if (ret < 0) {
		free_graph(st);
		return false;
	}

	while (av_buffersink_get_frame(st->sink, st->out) >= 0) {
		uint64_t wall = st->next_wall;

		st->next_wall += (uint64_t)st->out->nb_samples * 1000000000ULL / (uint64_t)st->out->sample_rate;
		out(param, st->out, wall);
		av_frame_unref(st->out);
	}

	return true;
}

#else

bool mp_stretch_push(struct mp_stretch *st, AVFrame *in, int speed, uint64_t due, mp_stretch_out_cb out, void *param)
{
	UNUSED_PARAMETER(st);
	UNUSED_PARAMETER(in);
	UNUSED_PARAMETER(speed);
	UNUSED_PARAMETER(due);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(param);
	return false;
}

#endif
