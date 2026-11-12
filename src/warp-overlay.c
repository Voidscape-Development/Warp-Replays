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
#include <graphics/srgb.h>
#include <graphics/vec4.h>
#include <util/bmem.h>
#include <util/platform.h>

#include "warp-events.h"
#include "warp-overlay.h"

/* how often a target that is not there is looked for again */
#define WARP_OVERLAY_RESOLVE_SECONDS 0.5f

/* ------------------------------------------------------------------------- */
/* the target */

static void warp_overlay_push(struct warp_overlay_target *t, enum warp_overlay_event_type type, int value,
			      int transition_ms)
{
	pthread_mutex_lock(&t->mutex);

	/* A tick that never comes - the overlay not being drawn - must not let
	 * this grow: the oldest goes, since what is shown is what happened
	 * last. */
	if (t->num_events == WARP_OVERLAY_MAX_EVENTS) {
		memmove(t->events, t->events + 1, sizeof(t->events[0]) * (WARP_OVERLAY_MAX_EVENTS - 1));
		t->num_events--;
	}

	t->events[t->num_events++] = (struct warp_overlay_event){
		.type = type,
		.value = value,
		.transition_ms = transition_ms,
	};

	pthread_mutex_unlock(&t->mutex);
}

static void warp_overlay_speed_signal(void *param, calldata_t *cd)
{
	warp_overlay_push(param, WARP_OVERLAY_EV_SPEED, (int)calldata_int(cd, "speed"), 0);
}

static void warp_overlay_frames_signal(void *param, calldata_t *cd)
{
	warp_overlay_push(param, WARP_OVERLAY_EV_STEP, (int)calldata_int(cd, "frames"), 0);
}

static void warp_overlay_media_signal(void *param, calldata_t *cd)
{
	const char *action = calldata_string(cd, "action");

	if (!action)
		return;

	if (strcmp(action, WARP_MEDIA_ACTION_PLAY) == 0)
		warp_overlay_push(param, WARP_OVERLAY_EV_PLAY, 0, 0);
	else if (strcmp(action, WARP_MEDIA_ACTION_PAUSE) == 0)
		warp_overlay_push(param, WARP_OVERLAY_EV_PAUSE, 0, 0);
	else if (strcmp(action, WARP_MEDIA_ACTION_RESTART) == 0)
		warp_overlay_push(param, WARP_OVERLAY_EV_RESTART, 0, 0);
	else if (strcmp(action, WARP_MEDIA_ACTION_LOADED) == 0)
		warp_overlay_push(param, WARP_OVERLAY_EV_LOADED, 0, 0);
}

static void warp_overlay_switch_signal(void *param, calldata_t *cd)
{
	const char *reason = calldata_string(cd, "reason");
	int ms = (int)calldata_int(cd, "transition_ms");
	enum warp_overlay_event_type type = WARP_OVERLAY_EV_SWITCH;

	if (reason && strcmp(reason, WARP_SWITCH_NEXT) == 0)
		type = WARP_OVERLAY_EV_NEXT;
	else if (reason && strcmp(reason, WARP_SWITCH_PREVIOUS) == 0)
		type = WARP_OVERLAY_EV_PREVIOUS;
	else if (reason && strcmp(reason, WARP_SWITCH_FIRST) == 0)
		type = WARP_OVERLAY_EV_FIRST;

	warp_overlay_push(param, type, 0, ms);
}

static bool warp_overlay_is_warp_source(obs_source_t *source)
{
	const char *id = obs_source_get_id(source);

	return id && (strcmp(id, WARP_MEDIA_SOURCE_ID) == 0 || strcmp(id, WARP_PLAYLIST_SOURCE_ID) == 0);
}

static bool warp_overlay_is_playlist(obs_source_t *source)
{
	const char *id = obs_source_get_id(source);

	return id && strcmp(id, WARP_PLAYLIST_SOURCE_ID) == 0;
}

/* Connecting and disconnecting is done without the mutex held: disconnecting
 * waits for a signal that is being delivered, and delivering one wants it. */
static void warp_overlay_disconnect(struct warp_overlay_target *t)
{
	if (!t->listening_to)
		return;

	obs_source_t *source = obs_weak_source_get_source(t->listening_to);

	if (source) {
		signal_handler_t *handler = obs_source_get_signal_handler(source);

		signal_handler_disconnect(handler, WARP_SIGNAL_SPEED_CHANGED, warp_overlay_speed_signal, t);
		signal_handler_disconnect(handler, WARP_SIGNAL_FRAMES_STEPPED, warp_overlay_frames_signal, t);
		signal_handler_disconnect(handler, WARP_SIGNAL_MEDIA_ACTION, warp_overlay_media_signal, t);
		if (warp_overlay_is_playlist(source))
			signal_handler_disconnect(handler, WARP_SIGNAL_PLAYLIST_SWITCHED, warp_overlay_switch_signal,
						  t);
		obs_source_release(source);
	}

	obs_weak_source_release(t->listening_to);
	t->listening_to = NULL;

	/* what the last target said is nothing to do with the next one */
	pthread_mutex_lock(&t->mutex);
	t->num_events = 0;
	pthread_mutex_unlock(&t->mutex);
}

/* the source the settings name, with a reference, when it is there and is a
 * Warp source */
static obs_source_t *warp_overlay_wanted(struct warp_overlay_target *t)
{
	if (!t->wanted || !*t->wanted)
		return NULL;

	obs_source_t *source = obs_get_source_by_uuid(t->wanted);

	if (source && !warp_overlay_is_warp_source(source)) {
		obs_source_release(source);
		source = NULL;
	}

	return source;
}

/* follows the source the settings name, or stops following one that has gone;
 * returns the target with a reference, or NULL */
static obs_source_t *warp_overlay_resolve(struct warp_overlay_target *t, float seconds)
{
	obs_source_t *current = obs_weak_source_get_source(t->listening_to);

	t->resolve_delay -= seconds;

	/* the one being followed is kept until it goes or another is picked,
	 * without looking it up every frame */
	if (current && t->resolve_delay > 0.0f)
		return current;

	if (!current && t->listening_to)
		warp_overlay_disconnect(t);

	if (t->resolve_delay > 0.0f)
		return NULL;

	t->resolve_delay = WARP_OVERLAY_RESOLVE_SECONDS;

	obs_source_t *wanted = warp_overlay_wanted(t);

	if (wanted && current && wanted == current) {
		obs_source_release(wanted);
		return current;
	}

	obs_source_release(current);
	warp_overlay_disconnect(t);

	if (!wanted)
		return NULL;

	signal_handler_t *handler = obs_source_get_signal_handler(wanted);

	signal_handler_connect(handler, WARP_SIGNAL_SPEED_CHANGED, warp_overlay_speed_signal, t);
	signal_handler_connect(handler, WARP_SIGNAL_FRAMES_STEPPED, warp_overlay_frames_signal, t);
	signal_handler_connect(handler, WARP_SIGNAL_MEDIA_ACTION, warp_overlay_media_signal, t);
	/* only a playlist switches files */
	if (warp_overlay_is_playlist(wanted))
		signal_handler_connect(handler, WARP_SIGNAL_PLAYLIST_SWITCHED, warp_overlay_switch_signal, t);
	t->listening_to = obs_source_get_weak_source(wanted);

	return wanted;
}

bool warp_overlay_target_init(struct warp_overlay_target *t, obs_source_t *owner)
{
	memset(t, 0, sizeof(*t));
	t->owner = owner;

	return pthread_mutex_init(&t->mutex, NULL) == 0;
}

void warp_overlay_target_free(struct warp_overlay_target *t)
{
	warp_overlay_disconnect(t);
	bfree(t->wanted);
	t->wanted = NULL;
	pthread_mutex_destroy(&t->mutex);
}

void warp_overlay_target_set(struct warp_overlay_target *t, const char *uuid)
{
	if ((!uuid || !*uuid) && !t->wanted)
		return;
	if (uuid && t->wanted && strcmp(uuid, t->wanted) == 0)
		return;

	bfree(t->wanted);
	t->wanted = uuid && *uuid ? bstrdup(uuid) : NULL;

	/* looked up on the next tick rather than half a second on */
	t->resolve_delay = 0.0f;
}

static int warp_overlay_get_speed(obs_source_t *source)
{
	calldata_t cd = {0};
	int speed = 100;

	calldata_init(&cd);
	if (proc_handler_call(obs_source_get_proc_handler(source), "warp_get_speed", &cd))
		speed = (int)calldata_int(&cd, "speed");
	calldata_free(&cd);

	return speed > 0 ? speed : 100;
}

size_t warp_overlay_target_tick(struct warp_overlay_target *t, float seconds, struct warp_overlay_status *status,
				struct warp_overlay_event *events)
{
	obs_source_t *target = warp_overlay_resolve(t, seconds);
	size_t count;

	memset(status, 0, sizeof(*status));
	status->state = OBS_MEDIA_STATE_NONE;
	status->speed = 100;

	if (target) {
		status->has_target = true;
		status->is_playlist = warp_overlay_is_playlist(target);
		status->on_program = obs_source_active(target);
		status->state = obs_source_media_get_state(target);
		status->time_ms = obs_source_media_get_time(target);
		status->duration_ms = obs_source_media_get_duration(target);
		status->speed = warp_overlay_get_speed(target);
		obs_source_release(target);
	}

	pthread_mutex_lock(&t->mutex);
	count = t->num_events;
	memcpy(events, t->events, sizeof(t->events[0]) * count);
	t->num_events = 0;
	pthread_mutex_unlock(&t->mutex);

	return count;
}

struct warp_overlay_list {
	obs_property_t *list;
	const char *selected;
	bool found;
};

static bool warp_overlay_add_source(void *data, obs_source_t *source)
{
	struct warp_overlay_list *ctx = data;
	const char *name = obs_source_get_name(source);
	const char *uuid = obs_source_get_uuid(source);

	if (!warp_overlay_is_warp_source(source) || !name || !*name || !uuid)
		return true;

	obs_property_list_add_string(ctx->list, name, uuid);

	if (ctx->selected && strcmp(ctx->selected, uuid) == 0)
		ctx->found = true;

	return true;
}

obs_property_t *warp_overlay_add_target_property(obs_properties_t *props, obs_data_t *settings)
{
	obs_property_t *list = obs_properties_add_list(props, WARP_OVERLAY_TARGET,
						       obs_module_text("Warp.Overlay.Target"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	struct warp_overlay_list ctx = {
		.list = list,
		.selected = settings ? obs_data_get_string(settings, WARP_OVERLAY_TARGET) : NULL,
	};

	obs_property_set_long_description(list, obs_module_text("Warp.Overlay.Target.Desc"));
	obs_property_list_add_string(list, obs_module_text("Warp.Overlay.None"), "");
	obs_enum_sources(warp_overlay_add_source, &ctx);

	/* A target that is not there right now - a scene collection that is
	 * still loading - must not be dropped from the settings just because the
	 * properties were opened, so what is saved is always in the list. */
	if (ctx.selected && *ctx.selected && !ctx.found)
		obs_property_list_add_string(list, obs_module_text("Warp.Overlay.Missing"), ctx.selected);

	return list;
}

/* ------------------------------------------------------------------------- */
/* text */

static const char *warp_overlay_text_id(bool *is_gdi)
{
	/* newest first: the one OBS lists in its own Add menu */
	static const char *gdi_ids[] = {"text_gdiplus_v3", "text_gdiplus_v2", "text_gdiplus"};

	for (size_t i = 0; i < sizeof(gdi_ids) / sizeof(gdi_ids[0]); i++) {
		if (obs_source_get_display_name(gdi_ids[i])) {
			*is_gdi = true;
			return gdi_ids[i];
		}
	}

	*is_gdi = false;
	return obs_source_get_display_name("text_ft2_source_v2") ? "text_ft2_source_v2" : NULL;
}

/* The four values a font property is made of, copied out as values of their
 * own: a font that was never changed is all defaults, which obs_data_apply()
 * would leave behind. */
static obs_data_t *warp_overlay_font_copy(obs_data_t *font)
{
	obs_data_t *copy = obs_data_create();

	if (font) {
		obs_data_set_string(copy, "face", obs_data_get_string(font, "face"));
		obs_data_set_string(copy, "style", obs_data_get_string(font, "style"));
		obs_data_set_int(copy, "size", obs_data_get_int(font, "size"));
		obs_data_set_int(copy, "flags", obs_data_get_int(font, "flags"));
	}

	return copy;
}

static bool warp_overlay_font_equal(obs_data_t *a, obs_data_t *b)
{
	if (!a || !b)
		return a == b;

	return strcmp(obs_data_get_string(a, "face"), obs_data_get_string(b, "face")) == 0 &&
	       strcmp(obs_data_get_string(a, "style"), obs_data_get_string(b, "style")) == 0 &&
	       obs_data_get_int(a, "size") == obs_data_get_int(b, "size") &&
	       obs_data_get_int(a, "flags") == obs_data_get_int(b, "flags");
}

void warp_overlay_text_set(struct warp_overlay_text *t, const char *text, obs_data_t *font, uint32_t color,
			   bool outline, bool shadow)
{
	if (!text)
		text = "";

	bool same = t->source && t->text && strcmp(t->text, text) == 0 && t->color == color && t->outline == outline &&
		    t->shadow == shadow && warp_overlay_font_equal(t->font, font);

	if (same)
		return;

	bool is_gdi;
	const char *id = warp_overlay_text_id(&is_gdi);

	if (!id)
		return;

	obs_data_t *settings = obs_data_create();
	obs_data_t *font_copy = warp_overlay_font_copy(font);

	obs_data_set_obj(settings, "font", font_copy);
	obs_data_set_string(settings, "text", text);

	if (is_gdi) {
		/* GDI+ takes the color without its alpha, and the alpha as a
		 * percentage of its own */
		obs_data_set_int(settings, "color", color & 0xFFFFFF);
		obs_data_set_int(settings, "opacity", (long long)(((color >> 24) & 0xFF) * 100 / 255));
		obs_data_set_bool(settings, "outline", outline);
		obs_data_set_int(settings, "outline_color", 0x000000);
		obs_data_set_int(settings, "outline_size", 2);
		obs_data_set_int(settings, "outline_opacity", 100);
	} else {
		obs_data_set_int(settings, "color1", color);
		obs_data_set_int(settings, "color2", color);
		obs_data_set_bool(settings, "outline", outline);
		obs_data_set_bool(settings, "drop_shadow", shadow);
	}

	if (t->source)
		obs_source_update(t->source, settings);
	else
		t->source = obs_source_create_private(id, "warp-overlay-text", settings);

	obs_data_release(settings);

	bfree(t->text);
	t->text = bstrdup(text);
	t->color = color;
	t->outline = outline;
	t->shadow = shadow;
	obs_data_release(t->font);
	t->font = font_copy;
}

void warp_overlay_text_free(struct warp_overlay_text *t)
{
	obs_source_release(t->source);
	bfree(t->text);
	obs_data_release(t->font);

	if (t->texrender) {
		obs_enter_graphics();
		gs_texrender_destroy(t->texrender);
		obs_leave_graphics();
	}

	memset(t, 0, sizeof(*t));
}

uint32_t warp_overlay_text_width(const struct warp_overlay_text *t)
{
	return t->source && t->text && *t->text ? obs_source_get_width(t->source) : 0;
}

uint32_t warp_overlay_text_height(const struct warp_overlay_text *t)
{
	return t->source && t->text && *t->text ? obs_source_get_height(t->source) : 0;
}

void warp_overlay_text_render(struct warp_overlay_text *t, float x, float y, float scale, float opacity)
{
	uint32_t cx = warp_overlay_text_width(t);
	uint32_t cy = warp_overlay_text_height(t);
	gs_effect_t *effect = warp_overlay_effect();

	if (!cx || !cy || opacity <= 0.0f)
		return;

	/* At full strength the text source draws itself. Faded, it is drawn into
	 * a texture of its own first and that is laid down faded: fading each
	 * glyph's own drawing would show where an outline or a shadow overlaps
	 * the letters. */
	if (opacity >= 1.0f || !effect) {
		gs_matrix_push();
		gs_matrix_translate3f(x, y, 0.0f);
		gs_matrix_scale3f(scale, scale, 1.0f);
		obs_source_video_render(t->source);
		gs_matrix_pop();
		return;
	}

	if (!t->texrender)
		t->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

	gs_texrender_reset(t->texrender);

	if (gs_texrender_begin(t->texrender, cx, cy)) {
		struct vec4 clear;

		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

		/* kept premultiplied, so its edges are not darkened twice */
		gs_blend_state_push();
		gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		obs_source_video_render(t->source);
		gs_blend_state_pop();

		gs_texrender_end(t->texrender);
	}

	gs_texture_t *tex = gs_texrender_get_texture(t->texrender);

	if (!tex)
		return;

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), tex);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "opacity"), opacity);

	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	gs_matrix_scale3f(scale, scale, 1.0f);

	gs_technique_t *tech = gs_effect_get_technique(effect, "ImagePremultiplied");
	size_t passes = gs_technique_begin(tech);

	for (size_t i = 0; i < passes; i++) {
		if (gs_technique_begin_pass(tech, i)) {
			gs_draw_sprite(tex, 0, cx, cy);
			gs_technique_end_pass(tech);
		}
	}

	gs_technique_end(tech);
	gs_matrix_pop();
	gs_blend_state_pop();
}

int warp_overlay_font_size(obs_data_t *font)
{
	int size = font ? (int)obs_data_get_int(font, "size") : 0;

	return size > 0 ? size : 32;
}

void warp_overlay_default_font(obs_data_t *settings, const char *key, int size)
{
	obs_data_t *font = obs_data_create();

#ifdef _WIN32
	obs_data_set_default_string(font, "face", "Arial");
#elif defined(__APPLE__)
	obs_data_set_default_string(font, "face", "Helvetica");
#else
	obs_data_set_default_string(font, "face", "Sans Serif");
#endif
	obs_data_set_default_string(font, "style", "Bold");
	obs_data_set_default_int(font, "size", size);
	/* bold */
	obs_data_set_default_int(font, "flags", 1);
	obs_data_set_default_obj(settings, key, font);
	obs_data_release(font);
}

/* ------------------------------------------------------------------------- */
/* drawing */

static gs_effect_t *warp_overlay_shared_effect = NULL;
static bool warp_overlay_effect_failed = false;

gs_effect_t *warp_overlay_effect(void)
{
	if (warp_overlay_shared_effect || warp_overlay_effect_failed)
		return warp_overlay_shared_effect;

	char *path = obs_module_file("shaders/warp-overlay.effect");
	char *errors = NULL;

	warp_overlay_shared_effect = path ? gs_effect_create_from_file(path, &errors) : NULL;

	if (!warp_overlay_shared_effect) {
		blog(LOG_ERROR, "[Warp] could not load the overlay effect from '%s': %s", path ? path : "(not found)",
		     errors ? errors : "");
		warp_overlay_effect_failed = true;
	}

	bfree(errors);
	bfree(path);

	return warp_overlay_shared_effect;
}

void warp_overlay_effect_free(void)
{
	if (!warp_overlay_shared_effect)
		return;

	obs_enter_graphics();
	gs_effect_destroy(warp_overlay_shared_effect);
	obs_leave_graphics();

	warp_overlay_shared_effect = NULL;
}

void warp_overlay_color_for_render(struct vec4 *out, const struct vec4 *srgb)
{
	*out = *srgb;

	if (gs_get_linear_srgb()) {
		out->x = gs_srgb_nonlinear_to_linear(srgb->x);
		out->y = gs_srgb_nonlinear_to_linear(srgb->y);
		out->z = gs_srgb_nonlinear_to_linear(srgb->z);
	}
}

void warp_overlay_color(struct vec4 *out, uint32_t color)
{
	struct vec4 srgb;

	vec4_from_rgba(&srgb, color);
	warp_overlay_color_for_render(out, &srgb);
}

float warp_overlay_approach(float value, float target, float rate, float seconds)
{
	float step = rate * seconds;

	if (value < target)
		return value + step < target ? value + step : target;

	return value - step > target ? value - step : target;
}
