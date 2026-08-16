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

/* libobs' own headers come first: plugin-support.h declares blogva() as a
 * plain extern, and util/base.h declares it exported, which MSVC reads as a
 * redefinition with different linkage when it meets the plain one first. */
#include <obs-module.h>
#include <plugin-support.h>

#include <graphics/vec2.h>

#include "warp-zoom.h"

/* The filter that does the zooming.
 *
 * It draws the picture it is given with its texture coordinates scaled and
 * shifted, which magnifies what is inside the window without changing the size
 * the source reports: nothing downstream - a playlist's transitions, the scene
 * item it sits in, the canvas - has to know a zoom is on.
 *
 * It works two ways round. Dropped on a source from the OBS filter list it is
 * an operator's filter like any other, with its own presets, hotkeys and
 * properties. Made by a Warp source, it is driven: the source that made it
 * frames it from its own tick, which is what lets a playlist keep the zoom with
 * the video that is playing rather than with the playlist itself. */

struct warp_zoom_filter {
	obs_source_t *source;

	/* Set for a filter a Warp source made for itself. A driven filter keeps
	 * no presets and registers no hotkeys or procs of its own: it renders
	 * what it is handed, and everything an operator reaches for lives on
	 * the source that made it. */
	bool driven;
	struct warp_zoom_control ctl;

	/* What is drawn. Both the tick that eases the control along and the
	 * warp_zoom_filter_apply() a driven filter is framed through run on the
	 * graphics thread, as does the render that reads it, so this needs no
	 * lock of its own. */
	struct warp_zoom_view view;

	gs_effect_t *effect;
	gs_eparam_t *mul_param;
	gs_eparam_t *add_param;
};

static const char *warp_zoom_filter_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Warp.ZoomFilter.Name");
}

static void warp_zoom_filter_defaults(obs_data_t *settings)
{
	warp_zoom_control_defaults(settings, true);
}

static obs_properties_t *warp_zoom_filter_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	UNUSED_PARAMETER(data);

	warp_zoom_control_properties(props, true);

	return props;
}

static void warp_zoom_filter_update(void *data, obs_data_t *settings)
{
	struct warp_zoom_filter *f = data;

	if (f->driven)
		return;

	warp_zoom_control_update(&f->ctl, settings);
}

static void warp_zoom_filter_save(void *data, obs_data_t *settings)
{
	struct warp_zoom_filter *f = data;

	if (f->driven)
		return;

	warp_zoom_control_save(&f->ctl, settings);
}

static void *warp_zoom_filter_create(obs_data_t *settings, obs_source_t *source)
{
	static const char *signals[] = {WARP_SIGNAL_DECL_ZOOM_CHANGED, NULL};

	struct warp_zoom_filter *f = bzalloc(sizeof(struct warp_zoom_filter));
	char *path = obs_module_file("shaders/warp-zoom.effect");

	f->source = source;
	f->driven = obs_data_get_bool(settings, WARP_ZOOM_S_DRIVEN);
	f->view = warp_zoom_default_view();

	obs_enter_graphics();
	f->effect = path ? gs_effect_create_from_file(path, NULL) : NULL;

	if (f->effect) {
		f->mul_param = gs_effect_get_param_by_name(f->effect, "mul_val");
		f->add_param = gs_effect_get_param_by_name(f->effect, "add_val");
	}
	obs_leave_graphics();

	bfree(path);

	if (!f->effect) {
		/* without the effect there is nothing to zoom with, so the
		 * filter passes the picture through untouched rather than
		 * taking it off the screen */
		obs_log(LOG_ERROR, "could not load the zoom effect; zoom will do nothing");
	}

	if (f->driven)
		return f;

	signal_handler_add_array(obs_source_get_signal_handler(source), signals);

	warp_zoom_control_init(&f->ctl, source, "WarpZoom", true, true);
	warp_zoom_control_register_procs(&f->ctl, source);
	warp_zoom_control_update(&f->ctl, settings);

	return f;
}

static void warp_zoom_filter_destroy(void *data)
{
	struct warp_zoom_filter *f = data;

	if (!f->driven)
		warp_zoom_control_free(&f->ctl);

	if (f->effect) {
		obs_enter_graphics();
		gs_effect_destroy(f->effect);
		obs_leave_graphics();
	}

	bfree(f);
}

static void warp_zoom_filter_tick(void *data, float seconds)
{
	struct warp_zoom_filter *f = data;

	/* a driven filter is moved along by the source that made it, on that
	 * source's own tick, so the view it renders lands on the frame it was
	 * asked for */
	if (f->driven)
		return;

	warp_zoom_control_tick(&f->ctl, seconds, &f->view);
}

static void warp_zoom_filter_render(void *data, gs_effect_t *effect)
{
	struct warp_zoom_filter *f = data;
	obs_source_t *target;
	struct vec2 mul;
	struct vec2 add;
	float window;
	uint32_t cx;
	uint32_t cy;

	UNUSED_PARAMETER(effect);

	/* the whole picture is what the source would draw anyway, so an
	 * unzoomed filter costs nothing but the call that steps out of the way */
	if (!f->effect || warp_zoom_view_is_default(&f->view)) {
		obs_source_skip_video_filter(f->source);
		return;
	}

	target = obs_filter_get_target(f->source);
	cx = obs_source_get_base_width(target);
	cy = obs_source_get_base_height(target);

	if (!cx || !cy) {
		obs_source_skip_video_filter(f->source);
		return;
	}

	if (!obs_source_process_filter_begin(f->source, GS_RGBA, OBS_NO_DIRECT_RENDERING))
		return;

	/* the window the view describes, in texture coordinates: what is drawn
	 * runs from one corner of it to the other */
	window = 1.0f / f->view.zoom;

	vec2_set(&mul, window, window);
	vec2_set(&add, f->view.x - window * 0.5f, f->view.y - window * 0.5f);

	gs_effect_set_vec2(f->mul_param, &mul);
	gs_effect_set_vec2(f->add_param, &add);

	obs_source_process_filter_end(f->source, f->effect, cx, cy);
}

/* ------------------------------------------------------------------------- */
/* driven filters */

void warp_zoom_filter_apply(obs_source_t *filter, const struct warp_zoom_view *view)
{
	struct warp_zoom_filter *f;
	const char *id;

	if (!filter || !view)
		return;

	id = obs_source_get_id(filter);

	if (!id || strcmp(id, WARP_ZOOM_FILTER_ID) != 0)
		return;

	f = obs_obj_get_data(filter);

	if (f && f->driven)
		f->view = *view;
}

obs_source_t *warp_zoom_filter_create_driven(const char *name)
{
	obs_data_t *settings = obs_data_create();
	obs_source_t *filter;

	obs_data_set_bool(settings, WARP_ZOOM_S_DRIVEN, true);

	filter = obs_source_create_private(WARP_ZOOM_FILTER_ID, name, settings);
	obs_data_release(settings);

	return filter;
}

/* Looks for one kind of zoom filter or the other: a driven one belongs to the
 * source that made it, an operator's one is the filter they dropped on the
 * source themselves, and the two are reached for in different places. */
struct warp_zoom_filter_search {
	obs_source_t *found;
	bool want_driven;
};

static void warp_zoom_filter_search_cb(obs_source_t *parent, obs_source_t *child, void *param)
{
	struct warp_zoom_filter_search *search = param;
	const char *id = obs_source_get_id(child);
	struct warp_zoom_filter *f;

	UNUSED_PARAMETER(parent);

	if (search->found || !id || strcmp(id, WARP_ZOOM_FILTER_ID) != 0)
		return;

	f = obs_obj_get_data(child);

	if (!f || f->driven != search->want_driven)
		return;

	search->found = obs_source_get_ref(child);
}

obs_source_t *warp_zoom_filter_find(obs_source_t *source)
{
	struct warp_zoom_filter_search search = {NULL, true};

	if (!source)
		return NULL;

	obs_source_enum_filters(source, warp_zoom_filter_search_cb, &search);

	return search.found;
}

obs_source_t *warp_zoom_filter_find_operable(obs_source_t *source)
{
	struct warp_zoom_filter_search search = {NULL, false};

	if (!source)
		return NULL;

	obs_source_enum_filters(source, warp_zoom_filter_search_cb, &search);

	return search.found;
}

struct obs_source_info warp_zoom_filter_info = {
	.id = WARP_ZOOM_FILTER_ID,
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = warp_zoom_filter_getname,
	.create = warp_zoom_filter_create,
	.destroy = warp_zoom_filter_destroy,
	.get_defaults = warp_zoom_filter_defaults,
	.get_properties = warp_zoom_filter_properties,
	.update = warp_zoom_filter_update,
	.save = warp_zoom_filter_save,
	.video_tick = warp_zoom_filter_tick,
	.video_render = warp_zoom_filter_render,
};
