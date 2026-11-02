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

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct AVFrame;
struct AVFilterGraph;
struct AVFilterContext;

/* Plays sound faster or slower without changing its pitch.
 *
 * Played at another speed the plain way - handed to libobs as though it had
 * been recorded at a higher or lower sample rate - sound goes up or down in
 * pitch with the speed: a slow-motion replay growls, a sped-up one squeaks.
 * This runs it through FFmpeg's atempo filter instead, which stretches or
 * squeezes it in time and leaves the pitch where it was. atempo takes 0.5 to
 * 2.0 at a time, so speeds outside that are a chain of them.
 *
 * Sound comes out of the filter a little after it goes in, and in pieces of its
 * own size, so the stretcher keeps its own timeline for what comes out: the
 * first piece is stamped with the moment the sound that went in first was due,
 * and every piece after it follows on from the one before. A gap in what goes
 * in - a pause, a seek, the clip going round - starts it afresh. */
struct mp_stretch {
	struct AVFilterGraph *graph;
	struct AVFilterContext *src;
	struct AVFilterContext *sink;
	struct AVFrame *out;

	/* what the graph was built for */
	int speed;
	int rate;
	int format;
	int channels;

	/* sample count of what has gone in, as the filter's timestamps */
	int64_t in_pts;
	/* when the next piece in is expected to be due, to tell a gap from
	 * sound carrying on */
	uint64_t expect_due;
	/* when the next piece out is due */
	uint64_t next_wall;
	bool started;
};

typedef void (*mp_stretch_out_cb)(void *param, struct AVFrame *frame, uint64_t timestamp);

/* Hands 'in', due at 'due', to the stretcher at 'speed' percent, and calls
 * 'out' with whatever comes out of it, stamped. False when the sound cannot be
 * stretched - a sample format or layout the filter does not take, or FFmpeg
 * without atempo - in which case nothing was taken and the caller plays it the
 * plain way. */
bool mp_stretch_push(struct mp_stretch *st, struct AVFrame *in, int speed, uint64_t due, mp_stretch_out_cb out,
		     void *param);

/* Drops whatever sound the stretcher is still holding, so the next piece in
 * starts afresh: after a seek, a stop, or a new file. */
void mp_stretch_reset(struct mp_stretch *st);

void mp_stretch_free(struct mp_stretch *st);

#ifdef __cplusplus
}
#endif
