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

/* Warp Status Icon: what a Warp Media or Warp Playlist source is doing, as an
 * icon to sit over the replay on air - play, pause, a frame step, a skip, slow
 * motion. What the source is doing all along (playing, paused, stopped, at the
 * end, at another speed) stays up; what it has just been asked to do (step,
 * skip, change speed) is flashed up for a moment over it and then gives way
 * back to it. */

#include <stdio.h>
#include <string.h>

#include <obs-module.h>
#include <graphics/image-file.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>
#include <util/threading.h>

#include "warp-overlay.h"

/* The glyphs, numbered the way the overlay effect draws them. */
enum warp_icon_glyph {
	WARP_ICON_NONE,
	WARP_ICON_PLAY,
	WARP_ICON_PAUSE,
	WARP_ICON_STOP,
	WARP_ICON_ENDED,
	WARP_ICON_STEP_FORWARD,
	WARP_ICON_STEP_BACKWARD,
	WARP_ICON_NEXT,
	WARP_ICON_PREVIOUS,
	WARP_ICON_RESTART,
	WARP_ICON_FAST,
	WARP_ICON_SLOW,
	WARP_ICON_COUNT,
};

/* the setting each glyph's own picture is kept under, and its label */
static const char *warp_icon_image_keys[WARP_ICON_COUNT] = {
	NULL,
	"image_play",
	"image_pause",
	"image_stop",
	"image_ended",
	"image_step_forward",
	"image_step_backward",
	"image_next",
	"image_previous",
	"image_restart",
	"image_fast",
	"image_slow",
};

static const char *warp_icon_image_text[WARP_ICON_COUNT] = {
	NULL,
	"Warp.Icon.Image.Play",
	"Warp.Icon.Image.Pause",
	"Warp.Icon.Image.Stop",
	"Warp.Icon.Image.Ended",
	"Warp.Icon.Image.StepForward",
	"Warp.Icon.Image.StepBackward",
	"Warp.Icon.Image.Next",
	"Warp.Icon.Image.Previous",
	"Warp.Icon.Image.Restart",
	"Warp.Icon.Image.Fast",
	"Warp.Icon.Image.Slow",
};

#define S_SIZE "size"
#define S_PLATE "plate"
#define S_GLYPH_SCALE "glyph_scale"
#define S_GLYPH_COLOR "glyph_color"
#define S_PLATE_COLOR "plate_color"
#define S_SHADOW "shadow"
#define S_SHADOW_COLOR "shadow_color"
#define S_SHADOW_DISTANCE "shadow_distance"
#define S_SHADOW_BLUR "shadow_blur"
#define S_VALUE_TEXT "value_text"
#define S_TEXT_POSITION "text_position"
#define S_TEXT_FONT "text_font"
#define S_TEXT_COLOR "text_color"
#define S_TEXT_OUTLINE "text_outline"
#define S_TEXT_SHADOW "text_shadow"
#define S_SHOW_STEPS "show_steps"
#define S_SHOW_SPEED "show_speed"
#define S_SHOW_SKIPS "show_skips"
#define S_FLASH_MS "flash_ms"
#define S_FADE_MS "fade_ms"
#define S_HIDE_NORMAL_PLAY "hide_normal_play"
#define S_COMBINE_SPEED "combine_speed"
#define S_HIDE_OFF_AIR "hide_off_air"

#define PLATE_NONE "none"
#define PLATE_CIRCLE "circle"
#define PLATE_ROUNDED "rounded"

#define POS_RIGHT "right"
#define POS_BELOW "below"

/* the speed badge, as a share of the icon, and how far it sits out over the
 * icon's corner */
#define BADGE_SCALE 0.45f
/* the gap between the icon and its value, as a share of the icon */
#define TEXT_GAP 0.12f

/* what the icon is showing: a glyph, a badge in its corner, and the value
 * beside it */
struct warp_icon_look {
	enum warp_icon_glyph glyph;
	enum warp_icon_glyph badge;
	char text[32];
};

struct warp_icon_image {
	char *path;
	gs_image_file_t image;
	bool loaded;
};

struct warp_icon_source {
	obs_source_t *source;
	struct warp_overlay_target target;
	struct warp_overlay_text text;

	/* settings; the update runs on the graphics thread for a video
	 * source, which is where they are read */
	uint32_t size;
	float plate_shape;
	float glyph_scale;
	uint32_t glyph_color;
	uint32_t plate_color;
	bool shadow;
	uint32_t shadow_color;
	float shadow_distance;
	float shadow_blur;
	bool value_text;
	bool text_below;
	obs_data_t *text_font;
	uint32_t text_color;
	bool text_outline;
	bool text_shadow;
	bool show_steps;
	bool show_speed;
	bool show_skips;
	float flash_seconds;
	float fade_seconds;
	bool hide_normal_play;
	bool combine_speed;
	bool hide_off_air;

	/* the operator's own pictures, loaded as they are set and drawn while
	 * the render holds the lock */
	pthread_mutex_t image_mutex;
	struct warp_icon_image images[WARP_ICON_COUNT];

	/* what is on screen; graphics thread */
	struct warp_overlay_status status;
	struct warp_icon_look current;
	struct warp_icon_look previous;
	/* how far the change from 'previous' to 'current' has faded, 0 to 1 */
	float fade;
	/* the whole icon, faded out while its target is off air */
	float alpha;

	/* what was flashed up, and for how much longer */
	struct warp_icon_look flash;
	float flash_left;
	/* frames stepped in a row the same way while the step is flashed, so a
	 * held key reads +7 rather than +1 seven times */
	int step_run;

	/* the room kept for the value, so the source does not change size
	 * every time the number in it does */
	uint32_t text_reserve_w;
	uint32_t text_reserve_h;
};

/* ------------------------------------------------------------------------- */

static const char *warp_icon_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Warp.Icon.Name");
}

static void warp_icon_load_image(struct warp_icon_image *img, const char *path)
{
	if (!path)
		path = "";
	if ((img->path && strcmp(img->path, path) == 0) || (!img->path && !*path))
		return;

	if (img->loaded) {
		obs_enter_graphics();
		gs_image_file_free(&img->image);
		obs_leave_graphics();
		img->loaded = false;
	}

	bfree(img->path);
	img->path = *path ? bstrdup(path) : NULL;

	if (!img->path)
		return;

	gs_image_file_init(&img->image, img->path);

	obs_enter_graphics();
	gs_image_file_init_texture(&img->image);
	obs_leave_graphics();

	img->loaded = img->image.texture != NULL;

	if (!img->loaded) {
		blog(LOG_WARNING, "[Warp Status Icon]: could not load '%s'", img->path);
		obs_enter_graphics();
		gs_image_file_free(&img->image);
		obs_leave_graphics();
	}
}

static void warp_icon_update(void *data, obs_data_t *settings)
{
	struct warp_icon_source *s = data;
	const char *plate = obs_data_get_string(settings, S_PLATE);

	warp_overlay_target_set(&s->target, obs_data_get_string(settings, WARP_OVERLAY_TARGET));

	s->size = (uint32_t)obs_data_get_int(settings, S_SIZE);
	s->plate_shape = strcmp(plate, PLATE_CIRCLE) == 0 ? 1.0f : strcmp(plate, PLATE_ROUNDED) == 0 ? 2.0f : 0.0f;
	s->glyph_scale = (float)obs_data_get_int(settings, S_GLYPH_SCALE) / 100.0f;
	s->glyph_color = (uint32_t)obs_data_get_int(settings, S_GLYPH_COLOR);
	s->plate_color = (uint32_t)obs_data_get_int(settings, S_PLATE_COLOR);
	s->shadow = obs_data_get_bool(settings, S_SHADOW);
	s->shadow_color = (uint32_t)obs_data_get_int(settings, S_SHADOW_COLOR);
	s->shadow_distance = (float)obs_data_get_int(settings, S_SHADOW_DISTANCE);
	s->shadow_blur = (float)obs_data_get_int(settings, S_SHADOW_BLUR);
	s->value_text = obs_data_get_bool(settings, S_VALUE_TEXT);
	s->text_below = strcmp(obs_data_get_string(settings, S_TEXT_POSITION), POS_BELOW) == 0;
	obs_data_release(s->text_font);
	s->text_font = obs_data_get_obj(settings, S_TEXT_FONT);
	s->text_color = (uint32_t)obs_data_get_int(settings, S_TEXT_COLOR);
	s->text_outline = obs_data_get_bool(settings, S_TEXT_OUTLINE);
	s->text_shadow = obs_data_get_bool(settings, S_TEXT_SHADOW);
	s->show_steps = obs_data_get_bool(settings, S_SHOW_STEPS);
	s->show_speed = obs_data_get_bool(settings, S_SHOW_SPEED);
	s->show_skips = obs_data_get_bool(settings, S_SHOW_SKIPS);
	s->flash_seconds = (float)obs_data_get_int(settings, S_FLASH_MS) / 1000.0f;
	s->fade_seconds = (float)obs_data_get_int(settings, S_FADE_MS) / 1000.0f;
	s->hide_normal_play = obs_data_get_bool(settings, S_HIDE_NORMAL_PLAY);
	s->combine_speed = obs_data_get_bool(settings, S_COMBINE_SPEED);
	s->hide_off_air = obs_data_get_bool(settings, S_HIDE_OFF_AIR);

	if (s->glyph_scale <= 0.0f)
		s->glyph_scale = 0.5f;

	s->text_reserve_w = 0;
	s->text_reserve_h = 0;

	pthread_mutex_lock(&s->image_mutex);
	for (size_t i = 1; i < WARP_ICON_COUNT; i++)
		warp_icon_load_image(&s->images[i], obs_data_get_string(settings, warp_icon_image_keys[i]));
	pthread_mutex_unlock(&s->image_mutex);
}

static void warp_icon_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, WARP_OVERLAY_TARGET, "");
	obs_data_set_default_int(settings, S_SIZE, 128);
	obs_data_set_default_string(settings, S_PLATE, PLATE_CIRCLE);
	obs_data_set_default_int(settings, S_GLYPH_SCALE, 50);
	/* colors are 0xAABBGGRR */
	obs_data_set_default_int(settings, S_GLYPH_COLOR, 0xFFFFFFFF);
	obs_data_set_default_int(settings, S_PLATE_COLOR, 0x99000000);
	obs_data_set_default_bool(settings, S_SHADOW, false);
	obs_data_set_default_int(settings, S_SHADOW_COLOR, 0xB0000000);
	obs_data_set_default_int(settings, S_SHADOW_DISTANCE, 3);
	obs_data_set_default_int(settings, S_SHADOW_BLUR, 4);
	obs_data_set_default_bool(settings, S_VALUE_TEXT, true);
	obs_data_set_default_string(settings, S_TEXT_POSITION, POS_RIGHT);
	warp_overlay_default_font(settings, S_TEXT_FONT, 48);
	obs_data_set_default_int(settings, S_TEXT_COLOR, 0xFFFFFFFF);
	obs_data_set_default_bool(settings, S_TEXT_OUTLINE, false);
	obs_data_set_default_bool(settings, S_TEXT_SHADOW, true);
	obs_data_set_default_bool(settings, S_SHOW_STEPS, true);
	obs_data_set_default_bool(settings, S_SHOW_SPEED, true);
	obs_data_set_default_bool(settings, S_SHOW_SKIPS, true);
	obs_data_set_default_int(settings, S_FLASH_MS, 1000);
	obs_data_set_default_int(settings, S_FADE_MS, 150);
	obs_data_set_default_bool(settings, S_HIDE_NORMAL_PLAY, true);
	obs_data_set_default_bool(settings, S_COMBINE_SPEED, true);
	obs_data_set_default_bool(settings, S_HIDE_OFF_AIR, false);
}

/* ------------------------------------------------------------------------- */
/* properties */

static bool warp_icon_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);

	bool plate = strcmp(obs_data_get_string(settings, S_PLATE), PLATE_NONE) != 0;
	bool shadow = obs_data_get_bool(settings, S_SHADOW);
	bool text = obs_data_get_bool(settings, S_VALUE_TEXT);

	obs_property_set_visible(obs_properties_get(props, S_PLATE_COLOR), plate);
	obs_property_set_visible(obs_properties_get(props, S_SHADOW_COLOR), shadow);
	obs_property_set_visible(obs_properties_get(props, S_SHADOW_DISTANCE), shadow);
	obs_property_set_visible(obs_properties_get(props, S_SHADOW_BLUR), shadow);
	obs_property_set_visible(obs_properties_get(props, S_TEXT_POSITION), text);
	obs_property_set_visible(obs_properties_get(props, S_TEXT_FONT), text);
	obs_property_set_visible(obs_properties_get(props, S_TEXT_COLOR), text);
	obs_property_set_visible(obs_properties_get(props, S_TEXT_OUTLINE), text);
	obs_property_set_visible(obs_properties_get(props, S_TEXT_SHADOW), text);

	return true;
}

static obs_properties_t *warp_icon_properties(void *data)
{
	struct warp_icon_source *s = data;
	obs_properties_t *props = obs_properties_create();
	obs_data_t *settings = s ? obs_source_get_settings(s->source) : NULL;
	obs_properties_t *group;
	obs_property_t *p;

	warp_overlay_add_target_property(props, settings);
	obs_data_release(settings);

	/* what it shows */
	group = obs_properties_create();

	obs_properties_add_bool(group, S_SHOW_STEPS, obs_module_text("Warp.Icon.ShowSteps"));
	obs_properties_add_bool(group, S_SHOW_SPEED, obs_module_text("Warp.Icon.ShowSpeed"));
	obs_properties_add_bool(group, S_SHOW_SKIPS, obs_module_text("Warp.Icon.ShowSkips"));
	p = obs_properties_add_bool(group, S_HIDE_NORMAL_PLAY, obs_module_text("Warp.Icon.HideNormalPlay"));
	obs_property_set_long_description(p, obs_module_text("Warp.Icon.HideNormalPlay.Desc"));
	p = obs_properties_add_bool(group, S_COMBINE_SPEED, obs_module_text("Warp.Icon.CombineSpeed"));
	obs_property_set_long_description(p, obs_module_text("Warp.Icon.CombineSpeed.Desc"));
	p = obs_properties_add_bool(group, S_HIDE_OFF_AIR, obs_module_text("Warp.Overlay.HideOffAir"));
	obs_property_set_long_description(p, obs_module_text("Warp.Overlay.HideOffAir.Desc"));
	p = obs_properties_add_int(group, S_FLASH_MS, obs_module_text("Warp.Icon.Flash"), 100, 10000, 50);
	obs_property_int_set_suffix(p, " ms");
	obs_property_set_long_description(p, obs_module_text("Warp.Icon.Flash.Desc"));
	p = obs_properties_add_int(group, S_FADE_MS, obs_module_text("Warp.Icon.Fade"), 0, 2000, 10);
	obs_property_int_set_suffix(p, " ms");

	obs_properties_add_group(props, "show_group", obs_module_text("Warp.Icon.Group.Show"), OBS_GROUP_NORMAL, group);

	/* how it looks */
	group = obs_properties_create();

	p = obs_properties_add_int(group, S_SIZE, obs_module_text("Warp.Icon.Size"), 8, 4096, 1);
	obs_property_int_set_suffix(p, " px");
	p = obs_properties_add_list(group, S_PLATE, obs_module_text("Warp.Icon.Plate"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.Plate.None"), PLATE_NONE);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.Plate.Circle"), PLATE_CIRCLE);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.Plate.Rounded"), PLATE_ROUNDED);
	obs_property_set_modified_callback(p, warp_icon_modified);
	obs_properties_add_color_alpha(group, S_PLATE_COLOR, obs_module_text("Warp.Icon.PlateColor"));
	p = obs_properties_add_int_slider(group, S_GLYPH_SCALE, obs_module_text("Warp.Icon.GlyphScale"), 10, 100, 1);
	obs_property_int_set_suffix(p, "%");
	obs_properties_add_color_alpha(group, S_GLYPH_COLOR, obs_module_text("Warp.Icon.GlyphColor"));
	p = obs_properties_add_bool(group, S_SHADOW, obs_module_text("Warp.Icon.Shadow"));
	obs_property_set_modified_callback(p, warp_icon_modified);
	obs_properties_add_color_alpha(group, S_SHADOW_COLOR, obs_module_text("Warp.Icon.ShadowColor"));
	p = obs_properties_add_int(group, S_SHADOW_DISTANCE, obs_module_text("Warp.Icon.ShadowDistance"), 0, 256, 1);
	obs_property_int_set_suffix(p, " px");
	p = obs_properties_add_int(group, S_SHADOW_BLUR, obs_module_text("Warp.Icon.ShadowBlur"), 0, 256, 1);
	obs_property_int_set_suffix(p, " px");

	obs_properties_add_group(props, "look_group", obs_module_text("Warp.Icon.Group.Look"), OBS_GROUP_NORMAL, group);

	/* the value beside it */
	group = obs_properties_create();

	p = obs_properties_add_bool(group, S_VALUE_TEXT, obs_module_text("Warp.Icon.ValueText"));
	obs_property_set_long_description(p, obs_module_text("Warp.Icon.ValueText.Desc"));
	obs_property_set_modified_callback(p, warp_icon_modified);
	p = obs_properties_add_list(group, S_TEXT_POSITION, obs_module_text("Warp.Icon.TextPosition"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.TextPosition.Right"), POS_RIGHT);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.TextPosition.Below"), POS_BELOW);
	obs_properties_add_font(group, S_TEXT_FONT, obs_module_text("Warp.Overlay.Font"));
	obs_properties_add_color_alpha(group, S_TEXT_COLOR, obs_module_text("Warp.Overlay.TextColor"));
	obs_properties_add_bool(group, S_TEXT_OUTLINE, obs_module_text("Warp.Overlay.TextOutline"));
	obs_properties_add_bool(group, S_TEXT_SHADOW, obs_module_text("Warp.Overlay.TextShadow"));

	obs_properties_add_group(props, "text_group", obs_module_text("Warp.Icon.Group.Text"), OBS_GROUP_NORMAL, group);

	/* the operator's own pictures */
	group = obs_properties_create();

	for (size_t i = 1; i < WARP_ICON_COUNT; i++)
		obs_properties_add_path(group, warp_icon_image_keys[i], obs_module_text(warp_icon_image_text[i]),
					OBS_PATH_FILE, "Image Files (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.tga)",
					NULL);

	p = obs_properties_add_group(props, "image_group", obs_module_text("Warp.Icon.Group.Images"), OBS_GROUP_NORMAL,
				     group);
	obs_property_set_long_description(p, obs_module_text("Warp.Icon.Group.Images.Desc"));

	return props;
}

/* ------------------------------------------------------------------------- */
/* what to show */

static enum warp_icon_glyph warp_icon_speed_glyph(int speed)
{
	return speed < 100 ? WARP_ICON_SLOW : speed > 100 ? WARP_ICON_FAST : WARP_ICON_PLAY;
}

/* flashes 'glyph' up over what is showing, with 'text' beside it */
static void warp_icon_flash(struct warp_icon_source *s, enum warp_icon_glyph glyph, const char *text)
{
	s->flash.glyph = glyph;
	s->flash.badge = WARP_ICON_NONE;
	snprintf(s->flash.text, sizeof(s->flash.text), "%s", text ? text : "");
	s->flash_left = s->flash_seconds;
}

static void warp_icon_take_event(struct warp_icon_source *s, const struct warp_overlay_event *ev)
{
	char text[32];

	/* a step run is only a run for as long as the steps are flashed */
	if (ev->type != WARP_OVERLAY_EV_STEP)
		s->step_run = 0;

	switch (ev->type) {
	case WARP_OVERLAY_EV_SPEED:
		if (!s->show_speed)
			break;
		snprintf(text, sizeof(text), "%d%%", ev->value);
		warp_icon_flash(s, warp_icon_speed_glyph(ev->value), text);
		break;
	case WARP_OVERLAY_EV_STEP:
		if (!s->show_steps || !ev->value)
			break;
		if (s->flash_left > 0.0f && s->step_run && (s->step_run > 0) == (ev->value > 0))
			s->step_run += ev->value;
		else
			s->step_run = ev->value;
		snprintf(text, sizeof(text), "%+d", s->step_run);
		warp_icon_flash(s, ev->value > 0 ? WARP_ICON_STEP_FORWARD : WARP_ICON_STEP_BACKWARD, text);
		break;
	case WARP_OVERLAY_EV_NEXT:
		if (s->show_skips)
			warp_icon_flash(s, WARP_ICON_NEXT, NULL);
		break;
	case WARP_OVERLAY_EV_PREVIOUS:
		if (s->show_skips)
			warp_icon_flash(s, WARP_ICON_PREVIOUS, NULL);
		break;
	case WARP_OVERLAY_EV_FIRST:
	case WARP_OVERLAY_EV_RESTART:
		if (s->show_skips)
			warp_icon_flash(s, WARP_ICON_RESTART, NULL);
		break;
	case WARP_OVERLAY_EV_PLAY:
		/* worth a moment on screen even when playing is not, so going
		 * back into play is seen to happen */
		warp_icon_flash(s, WARP_ICON_PLAY, NULL);
		break;
	case WARP_OVERLAY_EV_PAUSE:
		/* paused shows for as long as it lasts; anything flashed before
		 * it is over */
		s->flash_left = 0.0f;
		break;
	default:
		break;
	}
}

/* what the target's state shows when nothing is being flashed */
static void warp_icon_state_look(const struct warp_icon_source *s, struct warp_icon_look *look)
{
	const struct warp_overlay_status *st = &s->status;

	memset(look, 0, sizeof(*look));

	if (!st->has_target)
		return;

	switch (st->state) {
	case OBS_MEDIA_STATE_PLAYING:
		look->glyph = WARP_ICON_PLAY;
		break;
	case OBS_MEDIA_STATE_PAUSED:
		look->glyph = WARP_ICON_PAUSE;
		break;
	case OBS_MEDIA_STATE_STOPPED:
		look->glyph = WARP_ICON_STOP;
		break;
	case OBS_MEDIA_STATE_ENDED:
		look->glyph = WARP_ICON_ENDED;
		break;
	default:
		return;
	}

	bool moving = st->state == OBS_MEDIA_STATE_PLAYING || st->state == OBS_MEDIA_STATE_PAUSED;

	if (s->show_speed && moving && st->speed != 100) {
		snprintf(look->text, sizeof(look->text), "%d%%", st->speed);

		if (s->combine_speed)
			look->badge = warp_icon_speed_glyph(st->speed);
		else if (st->state == OBS_MEDIA_STATE_PLAYING)
			look->glyph = warp_icon_speed_glyph(st->speed);
	}

	/* playing as it should is nothing worth an icon */
	if (s->hide_normal_play && look->glyph == WARP_ICON_PLAY && look->badge == WARP_ICON_NONE)
		memset(look, 0, sizeof(*look));
}

static void warp_icon_tick(void *data, float seconds)
{
	struct warp_icon_source *s = data;
	struct warp_overlay_event events[WARP_OVERLAY_MAX_EVENTS];
	size_t count = warp_overlay_target_tick(&s->target, seconds, &s->status, events);
	struct warp_icon_look want;

	for (size_t i = 0; i < count; i++)
		warp_icon_take_event(s, &events[i]);

	if (s->flash_left > 0.0f) {
		s->flash_left -= seconds;
		want = s->flash;
	} else {
		s->step_run = 0;
		warp_icon_state_look(s, &want);
	}

	if (!s->status.has_target)
		s->flash_left = 0.0f;

	/* a new glyph fades in over the old one; a new value beside the same
	 * glyph just changes */
	if (want.glyph != s->current.glyph || want.badge != s->current.badge) {
		s->previous = s->current;
		s->fade = s->fade_seconds > 0.0f ? 0.0f : 1.0f;
	}

	s->current = want;

	if (s->fade < 1.0f)
		s->fade = warp_overlay_approach(s->fade, 1.0f, 1.0f / s->fade_seconds, seconds);

	bool visible = !(s->hide_off_air && !s->status.on_program);

	s->alpha = warp_overlay_approach(s->alpha, visible ? 1.0f : 0.0f,
					 s->fade_seconds > 0.0f ? 1.0f / s->fade_seconds : 1000.0f, seconds);

	/* the value beside it: the one coming in, or the one going out as it
	 * fades away with nothing in its place */
	if (s->value_text) {
		const char *text = *s->current.text ? s->current.text : s->fade < 1.0f ? s->previous.text : "";

		warp_overlay_text_set(&s->text, text, s->text_font, s->text_color, s->text_outline, s->text_shadow);

		uint32_t tw = warp_overlay_text_width(&s->text);
		uint32_t th = warp_overlay_text_height(&s->text);

		if (tw > s->text_reserve_w)
			s->text_reserve_w = tw;
		if (th > s->text_reserve_h)
			s->text_reserve_h = th;
	}
}

/* ------------------------------------------------------------------------- */
/* size */

static uint32_t warp_icon_text_extent(const struct warp_icon_source *s, bool width)
{
	if (!s->value_text)
		return 0;

	uint32_t font = (uint32_t)warp_overlay_font_size(s->text_font);

	if (width)
		/* room for "-20" or "400%" before anything has been shown */
		return s->text_reserve_w > font * 2 ? s->text_reserve_w : font * 2;

	return s->text_reserve_h > font ? s->text_reserve_h : font;
}

static uint32_t warp_icon_width(void *data)
{
	struct warp_icon_source *s = data;
	uint32_t text = warp_icon_text_extent(s, true);

	if (!text)
		return s->size;
	if (s->text_below)
		return text > s->size ? text : s->size;

	return s->size + (uint32_t)(s->size * TEXT_GAP) + text;
}

static uint32_t warp_icon_height(void *data)
{
	struct warp_icon_source *s = data;
	uint32_t text = warp_icon_text_extent(s, false);

	if (text && s->text_below)
		return s->size + (uint32_t)(s->size * TEXT_GAP) + text;

	return s->size;
}

/* ------------------------------------------------------------------------- */
/* drawing */

static void warp_icon_set_color(gs_effect_t *effect, const char *name, uint32_t color)
{
	struct vec4 v;

	warp_overlay_color(&v, color);
	gs_effect_set_vec4(gs_effect_get_param_by_name(effect, name), &v);
}

static void warp_icon_draw_tech(gs_effect_t *effect, const char *name, gs_texture_t *tex, uint32_t cx, uint32_t cy)
{
	gs_technique_t *tech = gs_effect_get_technique(effect, name);
	size_t passes = gs_technique_begin(tech);

	for (size_t i = 0; i < passes; i++) {
		if (gs_technique_begin_pass(tech, i)) {
			gs_draw_sprite(tex, 0, cx, cy);
			gs_technique_end_pass(tech);
		}
	}

	gs_technique_end(tech);
}

/* Draws one glyph, on its plate, in a square 'size' wide at (x, y). A glyph
 * with a picture of the operator's own is drawn as that picture, fitted inside
 * the room the glyph would have taken. Called with the image lock held. */
static void warp_icon_draw_glyph(struct warp_icon_source *s, gs_effect_t *effect, enum warp_icon_glyph glyph, float x,
				 float y, float size, float opacity)
{
	if (glyph == WARP_ICON_NONE || opacity <= 0.0f || size < 1.0f)
		return;

	struct warp_icon_image *img = &s->images[glyph];
	struct vec2 sz;
	struct vec2 shadow_offset;
	float units = 2.0f / size;

	vec2_set(&sz, size, size);
	vec2_set(&shadow_offset, s->shadow_distance * units, s->shadow_distance * units);

	gs_effect_set_vec2(gs_effect_get_param_by_name(effect, "size"), &sz);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "opacity"), opacity);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "glyph"), img->loaded ? 0.0f : (float)glyph);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "plate_shape"), s->plate_shape);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "glyph_scale"), s->glyph_scale);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "shadow"), s->shadow ? 1.0f : 0.0f);
	gs_effect_set_vec2(gs_effect_get_param_by_name(effect, "shadow_offset"), &shadow_offset);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "shadow_soft"), (s->shadow_blur + 1.0f) * units);
	warp_icon_set_color(effect, "glyph_color", s->glyph_color);
	warp_icon_set_color(effect, "plate_color", s->plate_color);
	warp_icon_set_color(effect, "shadow_color", s->shadow_color);

	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	warp_icon_draw_tech(effect, "Icon", NULL, (uint32_t)size, (uint32_t)size);
	gs_matrix_pop();

	if (!img->loaded)
		return;

	/* the picture, fitted inside the glyph's room and kept to its shape */
	float room = size * s->glyph_scale;
	float iw = (float)img->image.cx;
	float ih = (float)img->image.cy;

	if (iw <= 0.0f || ih <= 0.0f)
		return;

	float fit = room / (iw > ih ? iw : ih);
	float w = iw * fit;
	float h = ih * fit;

	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), img->image.texture);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "opacity"), opacity);

	gs_matrix_push();
	gs_matrix_translate3f(x + (size - w) * 0.5f, y + (size - h) * 0.5f, 0.0f);
	gs_matrix_scale3f(fit, fit, 1.0f);
	warp_icon_draw_tech(effect, "Image", img->image.texture, img->image.cx, img->image.cy);
	gs_matrix_pop();
}

/* the icon and its badge for one look */
static void warp_icon_draw_look(struct warp_icon_source *s, gs_effect_t *effect, const struct warp_icon_look *look,
				float opacity)
{
	float size = (float)s->size;
	float x = 0.0f;

	/* below, the icon is centred over its value */
	if (s->value_text && s->text_below) {
		float w = (float)warp_icon_width(s);
		x = (w - size) * 0.5f;
	}

	warp_icon_draw_glyph(s, effect, look->glyph, x, 0.0f, size, opacity);

	if (look->badge != WARP_ICON_NONE) {
		float badge = size * BADGE_SCALE;

		warp_icon_draw_glyph(s, effect, look->badge, x + size - badge, size - badge, badge, opacity);
	}
}

static void warp_icon_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);

	struct warp_icon_source *s = data;
	gs_effect_t *effect = warp_overlay_effect();

	if (!effect || s->alpha <= 0.0f || !s->size)
		return;

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(gs_get_linear_srgb());

	pthread_mutex_lock(&s->image_mutex);

	if (s->fade < 1.0f)
		warp_icon_draw_look(s, effect, &s->previous, s->alpha * (1.0f - s->fade));
	warp_icon_draw_look(s, effect, &s->current, s->alpha * s->fade);

	pthread_mutex_unlock(&s->image_mutex);

	gs_enable_framebuffer_srgb(previous);

	/* the value, beside the icon or under it */
	uint32_t tw = warp_overlay_text_width(&s->text);
	uint32_t th = warp_overlay_text_height(&s->text);

	if (!s->value_text || !tw || !th)
		return;

	float text_alpha = *s->current.text ? s->fade : 1.0f - s->fade;
	float size = (float)s->size;
	float gap = size * TEXT_GAP;
	float x, y;

	if (s->text_below) {
		x = ((float)warp_icon_width(s) - (float)tw) * 0.5f;
		y = size + gap;
	} else {
		x = size + gap;
		y = (size - (float)th) * 0.5f;
	}

	warp_overlay_text_render(&s->text, x, y, 1.0f, s->alpha * text_alpha);
}

/* ------------------------------------------------------------------------- */

static void *warp_icon_create(obs_data_t *settings, obs_source_t *source)
{
	struct warp_icon_source *s = bzalloc(sizeof(*s));

	s->source = source;
	s->fade = 1.0f;

	if (!warp_overlay_target_init(&s->target, source)) {
		bfree(s);
		return NULL;
	}

	if (pthread_mutex_init(&s->image_mutex, NULL) != 0) {
		warp_overlay_target_free(&s->target);
		bfree(s);
		return NULL;
	}

	warp_icon_update(s, settings);
	return s;
}

static void warp_icon_destroy(void *data)
{
	struct warp_icon_source *s = data;

	warp_overlay_target_free(&s->target);
	warp_overlay_text_free(&s->text);
	obs_data_release(s->text_font);

	for (size_t i = 1; i < WARP_ICON_COUNT; i++)
		warp_icon_load_image(&s->images[i], NULL);

	pthread_mutex_destroy(&s->image_mutex);
	bfree(s);
}

struct obs_source_info warp_status_icon_source_info = {
	.id = WARP_STATUS_ICON_SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB,
	.icon_type = OBS_ICON_TYPE_MEDIA,
	.get_name = warp_icon_getname,
	.create = warp_icon_create,
	.destroy = warp_icon_destroy,
	.update = warp_icon_update,
	.get_defaults = warp_icon_defaults,
	.get_properties = warp_icon_properties,
	.get_width = warp_icon_width,
	.get_height = warp_icon_height,
	.video_tick = warp_icon_tick,
	.video_render = warp_icon_render,
};
