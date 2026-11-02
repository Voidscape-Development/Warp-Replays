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

The conversion and drawing below follow libobs' own handling of async video
(libobs/obs-source.c, OBS Studio 32.2), so that a Warp Media source looks the
same drawn by itself as it did drawn by libobs. That code is Copyright (C)
Lain Bailey and the OBS Studio contributors, under the GNU GPL v2 or later.
*/

#include "warp-video.h"

#include <math.h>

#include <obs-module.h>
#include <util/bmem.h>

/* frames kept to copy the next ones into, at most */
#define WARP_VIDEO_POOL 12

/* ------------------------------------------------------------------------- */
/* what a frame is drawn as */

enum warp_convert {
	WARP_CONVERT_NONE,
	WARP_CONVERT_NV12,
	WARP_CONVERT_420,
	WARP_CONVERT_420_A,
	WARP_CONVERT_422,
	WARP_CONVERT_422P10LE,
	WARP_CONVERT_422_A,
	WARP_CONVERT_422_PACK,
	WARP_CONVERT_444,
	WARP_CONVERT_444P12LE,
	WARP_CONVERT_444_A,
	WARP_CONVERT_444P12LE_A,
	WARP_CONVERT_RGB_LIMITED,
	WARP_CONVERT_I010,
	WARP_CONVERT_P010,
};

static enum warp_convert convert_type(enum video_format format, bool full_range)
{
	switch (format) {
	case VIDEO_FORMAT_I420:
		return WARP_CONVERT_420;
	case VIDEO_FORMAT_NV12:
		return WARP_CONVERT_NV12;
	case VIDEO_FORMAT_I444:
		return WARP_CONVERT_444;
	case VIDEO_FORMAT_I412:
		return WARP_CONVERT_444P12LE;
	case VIDEO_FORMAT_I422:
		return WARP_CONVERT_422;
	case VIDEO_FORMAT_I210:
		return WARP_CONVERT_422P10LE;
	case VIDEO_FORMAT_YVYU:
	case VIDEO_FORMAT_YUY2:
	case VIDEO_FORMAT_UYVY:
		return WARP_CONVERT_422_PACK;
	case VIDEO_FORMAT_RGBA:
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
		return full_range ? WARP_CONVERT_NONE : WARP_CONVERT_RGB_LIMITED;
	case VIDEO_FORMAT_I40A:
		return WARP_CONVERT_420_A;
	case VIDEO_FORMAT_I42A:
		return WARP_CONVERT_422_A;
	case VIDEO_FORMAT_YUVA:
		return WARP_CONVERT_444_A;
	case VIDEO_FORMAT_YA2L:
		return WARP_CONVERT_444P12LE_A;
	case VIDEO_FORMAT_I010:
		return WARP_CONVERT_I010;
	case VIDEO_FORMAT_P010:
		return WARP_CONVERT_P010;
	default:
		return WARP_CONVERT_NONE;
	}
}

/* what the converted picture is held in */
static enum gs_color_format output_format(enum video_format format, enum video_trc trc)
{
	switch (trc) {
	case VIDEO_TRC_PQ:
	case VIDEO_TRC_HLG:
		return GS_RGBA16F;
	default:
		switch (format) {
		case VIDEO_FORMAT_RGBA:
			return GS_RGBA;
		case VIDEO_FORMAT_BGRA:
		case VIDEO_FORMAT_I40A:
		case VIDEO_FORMAT_I42A:
		case VIDEO_FORMAT_YUVA:
			return GS_BGRA;
		case VIDEO_FORMAT_I010:
		case VIDEO_FORMAT_P010:
		case VIDEO_FORMAT_I210:
		case VIDEO_FORMAT_I412:
		case VIDEO_FORMAT_YA2L:
			return GS_RGBA16F;
		default:
			return GS_BGRX;
		}
	}
}

static enum gs_color_space video_space(enum video_format format, enum video_trc trc)
{
	if (output_format(format, trc) != GS_RGBA16F)
		return GS_CS_SRGB;

	switch (trc) {
	case VIDEO_TRC_PQ:
	case VIDEO_TRC_HLG:
		return GS_CS_709_EXTENDED;
	default:
		return GS_CS_SRGB_16F;
	}
}

static const char *conversion_technique(enum video_format format, bool full_range, enum video_trc trc)
{
	switch (format) {
	case VIDEO_FORMAT_UYVY:
		return "UYVY_Reverse";
	case VIDEO_FORMAT_YUY2:
		return trc == VIDEO_TRC_PQ    ? "YUY2_PQ_Reverse"
		       : trc == VIDEO_TRC_HLG ? "YUY2_HLG_Reverse"
					      : "YUY2_Reverse";
	case VIDEO_FORMAT_YVYU:
		return "YVYU_Reverse";
	case VIDEO_FORMAT_I420:
		return trc == VIDEO_TRC_PQ    ? "I420_PQ_Reverse"
		       : trc == VIDEO_TRC_HLG ? "I420_HLG_Reverse"
					      : "I420_Reverse";
	case VIDEO_FORMAT_NV12:
		return trc == VIDEO_TRC_PQ    ? "NV12_PQ_Reverse"
		       : trc == VIDEO_TRC_HLG ? "NV12_HLG_Reverse"
					      : "NV12_Reverse";
	case VIDEO_FORMAT_I444:
		return "I444_Reverse";
	case VIDEO_FORMAT_I412:
		return trc == VIDEO_TRC_PQ    ? "I412_PQ_Reverse"
		       : trc == VIDEO_TRC_HLG ? "I412_HLG_Reverse"
					      : "I412_Reverse";
	case VIDEO_FORMAT_I422:
		return "I422_Reverse";
	case VIDEO_FORMAT_I210:
		return trc == VIDEO_TRC_PQ    ? "I210_PQ_Reverse"
		       : trc == VIDEO_TRC_HLG ? "I210_HLG_Reverse"
					      : "I210_Reverse";
	case VIDEO_FORMAT_I40A:
		return "I40A_Reverse";
	case VIDEO_FORMAT_I42A:
		return "I42A_Reverse";
	case VIDEO_FORMAT_YUVA:
		return "YUVA_Reverse";
	case VIDEO_FORMAT_YA2L:
		return "YA2L_Reverse";
	case VIDEO_FORMAT_I010:
		return trc == VIDEO_TRC_PQ    ? "I010_PQ_2020_709_Reverse"
		       : trc == VIDEO_TRC_HLG ? "I010_HLG_2020_709_Reverse"
					      : "I010_SRGB_Reverse";
	case VIDEO_FORMAT_P010:
		return trc == VIDEO_TRC_PQ    ? "P010_PQ_2020_709_Reverse"
		       : trc == VIDEO_TRC_HLG ? "P010_HLG_2020_709_Reverse"
					      : "P010_SRGB_Reverse";
	case VIDEO_FORMAT_RGBA:
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
		return full_range ? NULL : "RGB_Limited";
	default:
		return NULL;
	}
}

static bool needs_linear_output(enum video_format format)
{
	return format == VIDEO_FORMAT_I010 || format == VIDEO_FORMAT_P010 || format == VIDEO_FORMAT_I210 ||
	       format == VIDEO_FORMAT_I412 || format == VIDEO_FORMAT_YA2L;
}

/* the planes a frame of 'format' is uploaded as */
struct plane_layout {
	int count;
	uint32_t width[4];
	uint32_t height[4];
	enum gs_color_format format[4];
};

static void set_planes(struct plane_layout *p, int count, uint32_t w0, uint32_t h0, uint32_t wc, uint32_t hc,
		       enum gs_color_format f, enum gs_color_format fc, bool alpha)
{
	p->count = count;
	for (int i = 0; i < count; i++) {
		bool chroma = i == 1 || i == 2;
		p->width[i] = chroma ? wc : w0;
		p->height[i] = chroma ? hc : h0;
		p->format[i] = chroma ? fc : f;
	}
	UNUSED_PARAMETER(alpha);
}

static bool plane_layout(const struct obs_source_frame *frame, struct plane_layout *p)
{
	const uint32_t w = frame->width;
	const uint32_t h = frame->height;
	const uint32_t hw = (w + 1) / 2;
	const uint32_t hh = (h + 1) / 2;

	memset(p, 0, sizeof(*p));

	switch (convert_type(frame->format, frame->full_range)) {
	case WARP_CONVERT_422_PACK:
		set_planes(p, 1, hw, h, 0, 0, GS_BGRA, GS_BGRA, false);
		return true;
	case WARP_CONVERT_420:
		set_planes(p, 3, w, h, hw, hh, GS_R8, GS_R8, false);
		return true;
	case WARP_CONVERT_420_A:
		set_planes(p, 4, w, h, hw, hh, GS_R8, GS_R8, true);
		return true;
	case WARP_CONVERT_422:
		set_planes(p, 3, w, h, hw, h, GS_R8, GS_R8, false);
		return true;
	case WARP_CONVERT_422_A:
		set_planes(p, 4, w, h, hw, h, GS_R8, GS_R8, true);
		return true;
	case WARP_CONVERT_422P10LE:
		set_planes(p, 3, w, h, hw, h, GS_R16, GS_R16, false);
		return true;
	case WARP_CONVERT_444:
		set_planes(p, 3, w, h, w, h, GS_R8, GS_R8, false);
		return true;
	case WARP_CONVERT_444_A:
		set_planes(p, 4, w, h, w, h, GS_R8, GS_R8, true);
		return true;
	case WARP_CONVERT_444P12LE:
		set_planes(p, 3, w, h, w, h, GS_R16, GS_R16, false);
		return true;
	case WARP_CONVERT_444P12LE_A:
		set_planes(p, 4, w, h, w, h, GS_R16, GS_R16, true);
		return true;
	case WARP_CONVERT_NV12:
		p->count = 2;
		p->width[0] = w;
		p->height[0] = h;
		p->format[0] = GS_R8;
		p->width[1] = hw;
		p->height[1] = hh;
		p->format[1] = GS_R8G8;
		return true;
	case WARP_CONVERT_P010:
		p->count = 2;
		p->width[0] = w;
		p->height[0] = h;
		p->format[0] = GS_R16;
		p->width[1] = hw;
		p->height[1] = hh;
		p->format[1] = GS_RG16;
		return true;
	case WARP_CONVERT_I010:
		set_planes(p, 3, w, h, hw, hh, GS_R16, GS_R16, false);
		return true;
	case WARP_CONVERT_RGB_LIMITED:
		set_planes(p, 1, w, h, 0, 0, output_format(frame->format, frame->trc),
			   output_format(frame->format, frame->trc), false);
		return true;
	case WARP_CONVERT_NONE:
		break;
	}

	return false;
}

/* ------------------------------------------------------------------------- */
/* frames */

void warp_video_init(struct warp_video *v)
{
	memset(v, 0, sizeof(*v));
	pthread_mutex_init(&v->mutex, NULL);
	da_init(v->pool);
}

static void destroy_frames(struct warp_video *v)
{
	for (size_t i = 0; i < v->queue_num; i++)
		obs_source_frame_destroy(v->queue[i]);
	v->queue_num = 0;

	obs_source_frame_destroy(v->pending);
	obs_source_frame_destroy(v->preload);
	v->pending = NULL;
	v->preload = NULL;

	for (size_t i = 0; i < v->pool.num; i++)
		obs_source_frame_destroy(v->pool.array[i]);
	da_free(v->pool);
}

void warp_video_free(struct warp_video *v)
{
	pthread_mutex_lock(&v->mutex);
	destroy_frames(v);
	pthread_mutex_unlock(&v->mutex);

	obs_enter_graphics();
	for (size_t c = 0; c < MAX_AV_PLANES; c++)
		gs_texture_destroy(v->textures[c]);
	gs_texrender_destroy(v->texrender);
	gs_effect_destroy(v->conversion);
	obs_leave_graphics();

	pthread_mutex_destroy(&v->mutex);
}

/* expects v->mutex to be held */
static void give_back(struct warp_video *v, struct obs_source_frame *frame)
{
	if (!frame)
		return;

	if (v->pool.num < WARP_VIDEO_POOL)
		da_push_back(v->pool, &frame);
	else
		obs_source_frame_destroy(frame);
}

/* a copy of 'src', in a frame out of the pool when one of the same shape is
 * there; expects v->mutex to be held */
static struct obs_source_frame *copy_in(struct warp_video *v, const struct obs_source_frame *src)
{
	struct obs_source_frame *dst = NULL;

	for (size_t i = v->pool.num; i > 0; i--) {
		struct obs_source_frame *f = v->pool.array[i - 1];

		if (f->format == src->format && f->width == src->width && f->height == src->height) {
			dst = f;
			da_erase(v->pool, i - 1);
			break;
		}
	}

	if (!dst) {
		/* the pool is full of frames of another shape: a new file */
		if (v->pool.num >= WARP_VIDEO_POOL) {
			obs_source_frame_destroy(v->pool.array[0]);
			da_erase(v->pool, 0);
		}

		dst = obs_source_frame_create(src->format, src->width, src->height);
		if (!dst)
			return NULL;
	}

	obs_source_frame_copy(dst, src);

	/* RGB has no limited range to expand: libobs treats an RGB frame as
	 * full range whatever it says, and so does this */
	if (!format_is_yuv(dst->format))
		dst->full_range = true;

	return dst;
}

/* expects v->mutex to be held */
static void set_pending(struct warp_video *v, struct obs_source_frame *frame)
{
	give_back(v, v->pending);
	v->pending = frame;
	v->cleared = false;

	os_atomic_set_long(&v->width, (long)frame->width);
	os_atomic_set_long(&v->height, (long)frame->height);
}

void warp_video_push(struct warp_video *v, const struct obs_source_frame *frame)
{
	pthread_mutex_lock(&v->mutex);

	struct obs_source_frame *copy = copy_in(v, frame);

	if (copy) {
		/* a source that is not being ticked is not taking its frames
		 * off the queue: the oldest is overdue, so it is the one to
		 * put up next rather than one to keep waiting */
		if (v->queue_num == WARP_VIDEO_QUEUE) {
			set_pending(v, v->queue[0]);
			memmove(v->queue, v->queue + 1, sizeof(v->queue[0]) * (WARP_VIDEO_QUEUE - 1));
			v->queue_num--;
		}

		v->queue[v->queue_num++] = copy;

		if (!v->pending && v->queue_num == 1) {
			os_atomic_set_long(&v->width, (long)frame->width);
			os_atomic_set_long(&v->height, (long)frame->height);
		}
	}

	pthread_mutex_unlock(&v->mutex);
}

/* expects v->mutex to be held */
static void drop_queue(struct warp_video *v)
{
	for (size_t i = 0; i < v->queue_num; i++)
		give_back(v, v->queue[i]);
	v->queue_num = 0;
}

void warp_video_set(struct warp_video *v, const struct obs_source_frame *frame)
{
	pthread_mutex_lock(&v->mutex);

	struct obs_source_frame *copy = copy_in(v, frame);

	if (copy) {
		drop_queue(v);
		set_pending(v, copy);
	}

	pthread_mutex_unlock(&v->mutex);
}

void warp_video_preload(struct warp_video *v, const struct obs_source_frame *frame)
{
	pthread_mutex_lock(&v->mutex);

	struct obs_source_frame *copy = frame ? copy_in(v, frame) : NULL;

	if (copy || !frame) {
		give_back(v, v->preload);
		v->preload = copy;
	}

	pthread_mutex_unlock(&v->mutex);
}

bool warp_video_show_preload(struct warp_video *v)
{
	bool shown = false;

	pthread_mutex_lock(&v->mutex);

	if (v->preload) {
		struct obs_source_frame *copy = copy_in(v, v->preload);

		if (copy) {
			drop_queue(v);
			set_pending(v, copy);
			shown = true;
		}
	}

	pthread_mutex_unlock(&v->mutex);

	return shown;
}

void warp_video_clear(struct warp_video *v)
{
	pthread_mutex_lock(&v->mutex);

	drop_queue(v);
	give_back(v, v->pending);
	v->pending = NULL;
	v->cleared = true;

	pthread_mutex_unlock(&v->mutex);
}

void warp_video_tick(struct warp_video *v, uint64_t time)
{
	pthread_mutex_lock(&v->mutex);

	/* the latest frame due by the time of the video frame being made;
	 * anything before it has had its turn */
	size_t due = 0;

	while (due < v->queue_num && v->queue[due]->timestamp <= time)
		due++;

	if (due) {
		for (size_t i = 0; i + 1 < due; i++)
			give_back(v, v->queue[i]);

		set_pending(v, v->queue[due - 1]);

		memmove(v->queue, v->queue + due, sizeof(v->queue[0]) * (v->queue_num - due));
		v->queue_num -= due;
	}

	pthread_mutex_unlock(&v->mutex);
}

uint32_t warp_video_width(struct warp_video *v)
{
	return (uint32_t)os_atomic_load_long(&v->width);
}

uint32_t warp_video_height(struct warp_video *v)
{
	return (uint32_t)os_atomic_load_long(&v->height);
}

/* ------------------------------------------------------------------------- */
/* uploading */

/* (re)makes the textures for frames shaped like 'frame'; graphics thread */
static bool prepare_textures(struct warp_video *v, const struct obs_source_frame *frame)
{
	if (v->textures[0] && v->tex_width == frame->width && v->tex_height == frame->height &&
	    v->tex_format == frame->format && v->tex_full_range == frame->full_range && v->tex_trc == frame->trc)
		return true;

	for (size_t c = 0; c < MAX_AV_PLANES; c++) {
		gs_texture_destroy(v->textures[c]);
		v->textures[c] = NULL;
	}
	gs_texrender_destroy(v->texrender);
	v->texrender = NULL;

	v->tex_width = frame->width;
	v->tex_height = frame->height;
	v->tex_format = frame->format;
	v->tex_full_range = frame->full_range;
	v->tex_trc = frame->trc;

	const enum gs_color_format format = output_format(frame->format, frame->trc);
	struct plane_layout planes;

	v->gpu_conversion = convert_type(frame->format, frame->full_range) != WARP_CONVERT_NONE &&
			    plane_layout(frame, &planes);

	if (v->gpu_conversion) {
		v->texrender = gs_texrender_create(format, GS_ZS_NONE);

		for (int c = 0; c < planes.count; c++)
			v->textures[c] = gs_texture_create(planes.width[c], planes.height[c], planes.format[c], 1, NULL,
							   GS_DYNAMIC);
	} else {
		v->textures[0] = gs_texture_create(frame->width, frame->height, format, 1, NULL, GS_DYNAMIC);
	}

	return v->textures[0] != NULL;
}

static bool load_conversion(struct warp_video *v)
{
	if (v->conversion)
		return true;
	if (v->conversion_failed)
		return false;

	char *path = obs_module_file("shaders/format_conversion.effect");
	char *errors = NULL;

	v->conversion = path ? gs_effect_create_from_file(path, &errors) : NULL;

	if (!v->conversion) {
		blog(LOG_WARNING, "[Warp]: could not load the video conversion shader: %s",
		     errors ? errors : "(file not found)");
		v->conversion_failed = true;
	}

	bfree(errors);
	bfree(path);
	return v->conversion != NULL;
}

static inline void set_float(gs_effect_t *effect, const char *name, float val)
{
	gs_effect_set_float(gs_effect_get_param_by_name(effect, name), val);
}

/* converts the planes just uploaded into RGB in the texrender, the way libobs
 * does an async source's; graphics thread */
static bool convert(struct warp_video *v, const struct obs_source_frame *frame)
{
	const char *tech_name = conversion_technique(frame->format, frame->full_range, frame->trc);

	if (!tech_name || !load_conversion(v))
		return false;

	gs_effect_t *conv = v->conversion;
	gs_technique_t *tech = gs_effect_get_technique(conv, tech_name);
	const uint32_t cx = frame->width;
	const uint32_t cy = frame->height;

	if (!tech)
		return false;

	gs_texrender_reset(v->texrender);

	if (!gs_texrender_begin(v->texrender, cx, cy))
		return false;

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(needs_linear_output(frame->format));
	gs_enable_blending(false);

	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);

	static const char *images[4] = {"image", "image1", "image2", "image3"};
	for (size_t c = 0; c < 4; c++) {
		if (v->textures[c])
			gs_effect_set_texture(gs_effect_get_param_by_name(conv, images[c]), v->textures[c]);
	}

	set_float(conv, "width", (float)cx);
	set_float(conv, "height", (float)cy);
	set_float(conv, "width_d2", (float)cx * 0.5f);
	set_float(conv, "height_d2", (float)cy * 0.5f);
	set_float(conv, "width_x2_i", 0.5f / (float)cx);
	set_float(conv, "height_x2_i", 0.5f / (float)cy);

	/* BT.2408 says higher than 1000 isn't comfortable */
	float hlg_peak_level = obs_get_video_hdr_nominal_peak_level();
	if (hlg_peak_level > 1000.f)
		hlg_peak_level = 1000.f;

	const float maximum_nits = frame->trc == VIDEO_TRC_HLG ? hlg_peak_level : 10000.f;
	set_float(conv, "maximum_over_sdr_white_nits", maximum_nits / obs_get_video_sdr_white_level());
	set_float(conv, "hlg_exponent", 0.2f + (0.42f * log10f(hlg_peak_level / 1000.f)));
	set_float(conv, "hdr_lw", (float)frame->max_luminance);
	set_float(conv, "hdr_lmax", obs_get_video_hdr_nominal_peak_level());

	struct vec4 vec0, vec1, vec2;
	vec4_set(&vec0, frame->color_matrix[0], frame->color_matrix[1], frame->color_matrix[2], frame->color_matrix[3]);
	vec4_set(&vec1, frame->color_matrix[4], frame->color_matrix[5], frame->color_matrix[6], frame->color_matrix[7]);
	vec4_set(&vec2, frame->color_matrix[8], frame->color_matrix[9], frame->color_matrix[10],
		 frame->color_matrix[11]);
	gs_effect_set_vec4(gs_effect_get_param_by_name(conv, "color_vec0"), &vec0);
	gs_effect_set_vec4(gs_effect_get_param_by_name(conv, "color_vec1"), &vec1);
	gs_effect_set_vec4(gs_effect_get_param_by_name(conv, "color_vec2"), &vec2);

	if (!frame->full_range) {
		gs_effect_set_val(gs_effect_get_param_by_name(conv, "color_range_min"), frame->color_range_min,
				  sizeof(float) * 3);
		gs_effect_set_val(gs_effect_get_param_by_name(conv, "color_range_max"), frame->color_range_max,
				  sizeof(float) * 3);
	}

	gs_draw(GS_TRIS, 0, 3);

	gs_technique_end_pass(tech);
	gs_technique_end(tech);

	gs_enable_blending(true);
	gs_enable_framebuffer_srgb(previous);

	gs_texrender_end(v->texrender);
	return true;
}

/* puts 'frame' in the textures; graphics thread */
static bool upload(struct warp_video *v, const struct obs_source_frame *frame)
{
	if (!prepare_textures(v, frame))
		return false;

	v->flip = frame->flip;
	v->linear_alpha = (frame->flags & OBS_SOURCE_FRAME_LINEAR_ALPHA) != 0;
	v->format = frame->format;
	v->trc = frame->trc;

	if (!v->gpu_conversion) {
		gs_texture_set_image(v->textures[0], frame->data[0], frame->linesize[0], false);
		return true;
	}

	for (size_t c = 0; c < MAX_AV_PLANES; c++) {
		if (v->textures[c])
			gs_texture_set_image(v->textures[c], frame->data[c], frame->linesize[c], false);
	}

	return convert(v, frame);
}

/* ------------------------------------------------------------------------- */
/* drawing */

enum gs_color_space warp_video_color_space(struct warp_video *v, size_t count, const enum gs_color_space *preferred)
{
	const enum gs_color_space space = v->have_picture ? video_space(v->format, v->trc) : GS_CS_SRGB;
	enum gs_color_space chosen = space;

	for (size_t i = 0; i < count; ++i) {
		chosen = preferred[i];
		if (chosen == space)
			break;
	}

	return chosen;
}

static void draw_texture(struct warp_video *v, gs_effect_t *effect)
{
	gs_texture_t *tex = v->gpu_conversion ? gs_texrender_get_texture(v->texrender) : v->textures[0];

	if (!tex)
		return;

	gs_eparam_t *param = gs_effect_get_param_by_name(effect, "image");
	const bool linear_srgb = gs_get_linear_srgb();
	const bool previous = gs_framebuffer_srgb_enabled();

	gs_enable_framebuffer_srgb(linear_srgb);

	if (linear_srgb)
		gs_effect_set_texture_srgb(param, tex);
	else
		gs_effect_set_texture(param, tex);

	gs_draw_sprite(tex, v->flip ? GS_FLIP_V : 0, 0, 0);

	gs_enable_framebuffer_srgb(previous);
}

void warp_video_render(struct warp_video *v)
{
	struct obs_source_frame *frame;
	bool cleared;

	pthread_mutex_lock(&v->mutex);
	frame = v->pending;
	v->pending = NULL;
	cleared = v->cleared;
	pthread_mutex_unlock(&v->mutex);

	if (frame) {
		v->have_picture = upload(v, frame);

		pthread_mutex_lock(&v->mutex);
		give_back(v, frame);
		pthread_mutex_unlock(&v->mutex);
	} else if (cleared) {
		v->have_picture = false;
	}

	if (!v->have_picture)
		return;

	/* drawn the way libobs draws an async source, for the colour space
	 * the picture is in and the one it is being drawn into */
	const enum gs_color_space source_space = video_space(v->format, v->trc);
	gs_effect_t *const effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	const char *tech_name = "Draw";
	float multiplier = 1.0f;
	const enum gs_color_space current_space = gs_get_color_space();
	bool linear_srgb = gs_get_linear_srgb();
	bool nonlinear_alpha = false;

	switch (source_space) {
	case GS_CS_SRGB:
		linear_srgb = linear_srgb || (current_space != GS_CS_SRGB);
		nonlinear_alpha = linear_srgb && !v->linear_alpha;
		switch (current_space) {
		case GS_CS_SRGB:
		case GS_CS_SRGB_16F:
		case GS_CS_709_EXTENDED:
			if (nonlinear_alpha)
				tech_name = "DrawNonlinearAlpha";
			break;
		case GS_CS_709_SCRGB:
			tech_name = nonlinear_alpha ? "DrawNonlinearAlphaMultiply" : "DrawMultiply";
			multiplier = obs_get_video_sdr_white_level() / 80.0f;
		}
		break;
	case GS_CS_SRGB_16F:
		if (current_space == GS_CS_709_SCRGB) {
			tech_name = "DrawMultiply";
			multiplier = obs_get_video_sdr_white_level() / 80.0f;
		}
		break;
	case GS_CS_709_EXTENDED:
		switch (current_space) {
		case GS_CS_SRGB:
		case GS_CS_SRGB_16F:
			tech_name = "DrawTonemap";
			linear_srgb = true;
			break;
		case GS_CS_709_SCRGB:
			tech_name = "DrawMultiply";
			multiplier = obs_get_video_sdr_white_level() / 80.0f;
			break;
		case GS_CS_709_EXTENDED:
			break;
		}
		break;
	case GS_CS_709_SCRGB:
		switch (current_space) {
		case GS_CS_SRGB:
		case GS_CS_SRGB_16F:
			tech_name = "DrawMultiplyTonemap";
			multiplier = 80.0f / obs_get_video_sdr_white_level();
			linear_srgb = true;
			break;
		case GS_CS_709_EXTENDED:
			tech_name = "DrawMultiply";
			multiplier = 80.0f / obs_get_video_sdr_white_level();
			break;
		case GS_CS_709_SCRGB:
			break;
		}
	}

	const bool previous = gs_set_linear_srgb(linear_srgb);

	gs_technique_t *const tech = gs_effect_get_technique(effect, tech_name);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "multiplier"), multiplier);
	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);

	if (nonlinear_alpha) {
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	}

	draw_texture(v, effect);

	if (nonlinear_alpha)
		gs_blend_state_pop();

	gs_technique_end_pass(tech);
	gs_technique_end(tech);

	gs_set_linear_srgb(previous);
}
