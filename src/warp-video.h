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

#include <obs.h>
#include <util/darray.h>
#include <util/threading.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The picture of a Warp Media source, drawn by the source itself.
 *
 * libobs can draw a source's video for it - an "async" source hands it frames
 * and libobs times, uploads and converts them - but on its own terms: a frame
 * is only uploaded while the source is being rendered, so one that is not on
 * screen holds no picture however much it has decoded; frames are held back or
 * thrown away by libobs' own reading of their timestamps; and a frame can only
 * reach the GPU by being copied there from memory. This draws it instead.
 *
 * Frames come in from the media thread stamped with the moment they are due.
 * On every video frame the one due by then is picked out, and when the source is
 * drawn that one frame is uploaded and converted to RGB with the same shaders,
 * and the same handling of colour range, colour space and HDR, that libobs uses
 * for an async source - a copy of them ships with the plugin. A source that is
 * not being drawn keeps hold of its latest frame and uploads it the moment it
 * is, so it has picture from its first frame on screen.
 *
 * Everything that hands a frame over is safe from any thread. The tick, the
 * drawing and the size and colour space queries belong to the graphics
 * thread. */

#define WARP_VIDEO_QUEUE 8

struct warp_video {
	/* --- shared, under 'mutex' -------------------------------------- */
	pthread_mutex_t mutex;
	/* frames waiting to fall due, oldest first */
	struct obs_source_frame *queue[WARP_VIDEO_QUEUE];
	size_t queue_num;
	/* the frame to put up when next drawn */
	struct obs_source_frame *pending;
	/* a frame put aside to be put up when playback starts */
	struct obs_source_frame *preload;
	/* frames done with, kept to copy the next ones into */
	DARRAY(struct obs_source_frame *) pool;
	/* nothing is to be drawn until another frame comes */
	bool cleared;

	/* the size of the picture, for any thread to read */
	volatile long width;
	volatile long height;

	/* --- the graphics thread's alone --------------------------------- */
	gs_effect_t *conversion;
	bool conversion_failed;
	gs_texture_t *textures[MAX_AV_PLANES];
	gs_texrender_t *texrender;
	bool gpu_conversion;
	bool have_picture;

	/* what the textures were made for */
	uint32_t tex_width;
	uint32_t tex_height;
	enum video_format tex_format;
	bool tex_full_range;
	enum video_trc tex_trc;

	/* about the frame in the textures */
	bool flip;
	bool linear_alpha;
	enum video_format format;
	enum video_trc trc;
};

void warp_video_init(struct warp_video *v);
/* expects to be called with no graphics context entered */
void warp_video_free(struct warp_video *v);

/* a frame to put up when it falls due, at frame->timestamp */
void warp_video_push(struct warp_video *v, const struct obs_source_frame *frame);
/* a frame to put up now, in place of anything waiting: a seek, a step, a
 * held clip */
void warp_video_set(struct warp_video *v, const struct obs_source_frame *frame);
/* a frame to put aside, to be put up by warp_video_show_preload(); NULL
 * forgets the one that was */
void warp_video_preload(struct warp_video *v, const struct obs_source_frame *frame);
/* puts up the frame put aside; false when there is none */
bool warp_video_show_preload(struct warp_video *v);
/* draws nothing until the next frame */
void warp_video_clear(struct warp_video *v);

/* graphics thread: picks out the frame due by 'time', the time of the video
 * frame being made */
void warp_video_tick(struct warp_video *v, uint64_t time);
/* graphics thread */
void warp_video_render(struct warp_video *v);
enum gs_color_space warp_video_color_space(struct warp_video *v, size_t count, const enum gs_color_space *preferred);

uint32_t warp_video_width(struct warp_video *v);
uint32_t warp_video_height(struct warp_video *v);

#ifdef __cplusplus
}
#endif
