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

#include <math.h>
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

/* the word each glyph is labelled with */
static const char *warp_icon_label_text[WARP_ICON_COUNT] = {
	NULL,
	"Warp.Icon.Label.Play",
	"Warp.Icon.Label.Pause",
	"Warp.Icon.Label.Stop",
	"Warp.Icon.Label.Ended",
	"Warp.Icon.Label.Step",
	"Warp.Icon.Label.Step",
	"Warp.Icon.Label.Next",
	"Warp.Icon.Label.Previous",
	"Warp.Icon.Label.Restart",
	"Warp.Icon.Label.Fast",
	"Warp.Icon.Label.Slow",
};

#define S_STYLE "style"
/* the style the settings were last filled in from, so picking one fills them
 * in once rather than every time the properties are opened */
#define S_STYLE_APPLIED "style_applied"
#define S_LAYOUT "layout"
#define S_SIZE "size"
#define S_PLATE "plate"
#define S_GLYPH_SCALE "glyph_scale"
#define S_GLYPH_COLOR "glyph_color"
#define S_PLATE_COLOR "plate_color"
#define S_ACCENT_COLOR "accent_color"
#define S_ACCENT_WIDTH "accent_width"
#define S_SHOW_PROGRESS "show_progress"
#define S_ROUNDED "rounded_glyphs"
#define S_CHIP_SHAPE "chip_shape"
#define S_GLYPH_SET "glyph_set"
#define S_GLYPH_COLOR2 "glyph_color2"
/* a duotone's supporting color is a shade of the icon color unless this is
 * set, by 'duotone_shade' percent: darker above zero, lighter below it */
#define S_GLYPH_COLOR2_OWN "glyph_color2_own"
#define S_DUOTONE_SHADE "duotone_shade"
#define S_ACCENT_BLOCK "accent_block"
#define S_POP "pop"
#define S_SHADOW "shadow"
#define S_SHADOW_COLOR "shadow_color"
#define S_SHADOW_DISTANCE "shadow_distance"
#define S_SHADOW_BLUR "shadow_blur"
#define S_VALUE_TEXT "value_text"
#define S_TEXT_CONTENT "text_content"
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

#define STYLE_CHIP "chip"
#define STYLE_BADGE "badge"
#define STYLE_MINIMAL "minimal"
#define STYLE_FLAT "flat"
#define STYLE_NEON "neon"
#define STYLE_GLASS "glass"
#define STYLE_TAG "tag"
#define STYLE_SPORT "sport"
#define STYLE_OUTLINE "outline"
#define STYLE_DUOTONE "duotone"
#define STYLE_AURORA "aurora"

#define SET_SOLID "solid"
#define SET_DUOTONE "duotone"
#define SET_OUTLINE "outline"
#define SET_GRADIENT "gradient"

#define SHAPE_PILL "pill"
#define SHAPE_ROUNDED "rounded"
#define SHAPE_SQUARE "square"
#define SHAPE_SLANTED "slanted"

#define LAYOUT_CHIP "chip"
#define LAYOUT_BESIDE "beside"

#define PLATE_NONE "none"
#define PLATE_CIRCLE "circle"
#define PLATE_ROUNDED "rounded"

#define CONTENT_VALUE "value"
#define CONTENT_LABEL "label"
#define CONTENT_BOTH "both"

#define POS_RIGHT "right"
#define POS_BELOW "below"

/* the speed badge, as a share of the icon */
#define BADGE_SCALE 0.45f
/* the gap between the icon and its value, as a share of the icon */
#define TEXT_GAP 0.12f
/* where the text starts in a chip, and the room after it, as shares of the
 * chip's height; and the tallest the text is drawn in it */
#define CHIP_TEXT_X 0.86f
#define CHIP_TEXT_END 0.42f
#define CHIP_TEXT_H 0.44f
/* how long a new glyph takes to pop in */
#define POP_SECONDS 0.24f
/* how far a glyph's corners are rounded, in glyph units */
#define GLYPH_ROUND 0.09f

enum warp_icon_chip_shape {
	WARP_ICON_CHIP_PILL,
	WARP_ICON_CHIP_ROUNDED,
	WARP_ICON_CHIP_SQUARE,
	WARP_ICON_CHIP_SLANTED,
};

enum warp_icon_content {
	WARP_ICON_CONTENT_VALUE,
	WARP_ICON_CONTENT_LABEL,
	WARP_ICON_CONTENT_BOTH,
};

/* what the icon is showing: a glyph, a badge in its corner, the value that
 * goes with them (a speed, the frames stepped), and the text that is shown */
struct warp_icon_look {
	enum warp_icon_glyph glyph;
	enum warp_icon_glyph badge;
	char value[32];
	char text[96];
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
	bool chip;
	enum warp_icon_chip_shape chip_shape;
	/* the glyph sits on a block of the accent color at the chip's start */
	bool accent_block;
	uint32_t size;
	float plate_shape;
	float glyph_scale;
	uint32_t glyph_color;
	/* how the glyphs are drawn, as the effect numbers it: solid, duotone,
	 * outline or gradient; and the second color the last two use */
	float glyph_mode;
	uint32_t glyph_color2;
	uint32_t plate_color;
	uint32_t accent_color;
	uint32_t accent_width;
	bool show_progress;
	bool rounded;
	bool pop;
	bool shadow;
	uint32_t shadow_color;
	float shadow_distance;
	float shadow_blur;
	bool value_text;
	enum warp_icon_content content;
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
	/* how far the glyph that came in last has popped in, 0 to 1 */
	float pop_t;
	/* the whole icon, faded out while its target is off air */
	float alpha;
	/* how far through its replay the target is */
	float fraction;
	/* the chip's width as it is drawn, easing toward the text in it */
	float chip_w;

	/* what was flashed up, and for how much longer */
	struct warp_icon_look flash;
	float flash_left;
	/* frames stepped in a row the same way while the step is flashed, so a
	 * held key reads +7 rather than +1 seven times */
	int step_run;

	/* the room kept for the text, so the source does not change size every
	 * time the number in it does */
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

/* 'color' (0xAABBGGRR) taken 'percent' of the way to black, or to white when
 * 'percent' is below zero, keeping its alpha: the supporting shade of a
 * duotone icon */
static uint32_t warp_icon_shade(uint32_t color, int percent)
{
	uint32_t out = color & 0xFF000000u;
	int amount = percent < -100 ? -100 : percent > 100 ? 100 : percent;

	for (int shift = 0; shift < 24; shift += 8) {
		int c = (int)((color >> shift) & 0xFF);

		if (amount >= 0)
			c = c * (100 - amount) / 100;
		else
			c = c + (255 - c) * -amount / 100;

		out |= (uint32_t)c << shift;
	}

	return out;
}

static void warp_icon_update(void *data, obs_data_t *settings)
{
	struct warp_icon_source *s = data;
	const char *plate = obs_data_get_string(settings, S_PLATE);
	const char *content = obs_data_get_string(settings, S_TEXT_CONTENT);

	warp_overlay_target_set(&s->target, obs_data_get_string(settings, WARP_OVERLAY_TARGET));

	const char *chip_shape = obs_data_get_string(settings, S_CHIP_SHAPE);

	s->chip = strcmp(obs_data_get_string(settings, S_LAYOUT), LAYOUT_CHIP) == 0;
	s->chip_shape = strcmp(chip_shape, SHAPE_ROUNDED) == 0   ? WARP_ICON_CHIP_ROUNDED
			: strcmp(chip_shape, SHAPE_SQUARE) == 0  ? WARP_ICON_CHIP_SQUARE
			: strcmp(chip_shape, SHAPE_SLANTED) == 0 ? WARP_ICON_CHIP_SLANTED
								 : WARP_ICON_CHIP_PILL;
	s->accent_block = obs_data_get_bool(settings, S_ACCENT_BLOCK);
	s->size = (uint32_t)obs_data_get_int(settings, S_SIZE);
	s->plate_shape = strcmp(plate, PLATE_CIRCLE) == 0 ? 1.0f : strcmp(plate, PLATE_ROUNDED) == 0 ? 2.0f : 0.0f;
	s->glyph_scale = (float)obs_data_get_int(settings, S_GLYPH_SCALE) / 100.0f;
	s->glyph_color = (uint32_t)obs_data_get_int(settings, S_GLYPH_COLOR);
	s->glyph_color2 = (uint32_t)obs_data_get_int(settings, S_GLYPH_COLOR2);

	const char *set = obs_data_get_string(settings, S_GLYPH_SET);

	s->glyph_mode = strcmp(set, SET_DUOTONE) == 0    ? 1.0f
			: strcmp(set, SET_OUTLINE) == 0  ? 2.0f
			: strcmp(set, SET_GRADIENT) == 0 ? 3.0f
							 : 0.0f;

	/* a duotone is one color in two shades unless told otherwise */
	if (s->glyph_mode == 1.0f && !obs_data_get_bool(settings, S_GLYPH_COLOR2_OWN))
		s->glyph_color2 = warp_icon_shade(s->glyph_color, (int)obs_data_get_int(settings, S_DUOTONE_SHADE));
	s->plate_color = (uint32_t)obs_data_get_int(settings, S_PLATE_COLOR);
	s->accent_color = (uint32_t)obs_data_get_int(settings, S_ACCENT_COLOR);
	s->accent_width = (uint32_t)obs_data_get_int(settings, S_ACCENT_WIDTH);
	s->show_progress = obs_data_get_bool(settings, S_SHOW_PROGRESS);
	s->rounded = obs_data_get_bool(settings, S_ROUNDED);
	s->pop = obs_data_get_bool(settings, S_POP);
	s->shadow = obs_data_get_bool(settings, S_SHADOW);
	s->shadow_color = (uint32_t)obs_data_get_int(settings, S_SHADOW_COLOR);
	s->shadow_distance = (float)obs_data_get_int(settings, S_SHADOW_DISTANCE);
	s->shadow_blur = (float)obs_data_get_int(settings, S_SHADOW_BLUR);
	s->value_text = obs_data_get_bool(settings, S_VALUE_TEXT);
	s->content = strcmp(content, CONTENT_LABEL) == 0  ? WARP_ICON_CONTENT_LABEL
		     : strcmp(content, CONTENT_BOTH) == 0 ? WARP_ICON_CONTENT_BOTH
							  : WARP_ICON_CONTENT_VALUE;
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

	/* the text is put together again with the new settings */
	s->current.glyph = WARP_ICON_NONE;
	s->current.badge = WARP_ICON_NONE;

	pthread_mutex_lock(&s->image_mutex);
	for (size_t i = 1; i < WARP_ICON_COUNT; i++)
		warp_icon_load_image(&s->images[i], obs_data_get_string(settings, warp_icon_image_keys[i]));
	pthread_mutex_unlock(&s->image_mutex);
}

/* ------------------------------------------------------------------------- */
/* styles */

/* colors as 0xAARRGGBB, the way they are written everywhere else, turned
 * into the 0xAABBGGRR OBS keeps them as */
#define ARGB(c) \
	((long long)(((uint32_t)(c) & 0xFF00FF00u) | (((uint32_t)(c) >> 16) & 0xFFu) | (((uint32_t)(c) & 0xFFu) << 16)))

struct warp_icon_setter {
	obs_data_t *data;
	bool as_default;
};

static void set_int(struct warp_icon_setter *w, const char *key, long long value)
{
	if (w->as_default)
		obs_data_set_default_int(w->data, key, value);
	else
		obs_data_set_int(w->data, key, value);
}

static void set_bool(struct warp_icon_setter *w, const char *key, bool value)
{
	if (w->as_default)
		obs_data_set_default_bool(w->data, key, value);
	else
		obs_data_set_bool(w->data, key, value);
}

static void set_string(struct warp_icon_setter *w, const char *key, const char *value)
{
	if (w->as_default)
		obs_data_set_default_string(w->data, key, value);
	else
		obs_data_set_string(w->data, key, value);
}

/* the font a style draws its text at: the face that is set, at its size */
static void set_font_size(struct warp_icon_setter *w, int size)
{
	if (w->as_default) {
		warp_overlay_default_font(w->data, S_TEXT_FONT, size);
		return;
	}

	obs_data_t *font = obs_data_get_obj(w->data, S_TEXT_FONT);
	obs_data_t *copy = obs_data_create();

	obs_data_set_string(copy, "face", obs_data_get_string(font, "face"));
	obs_data_set_string(copy, "style", obs_data_get_string(font, "style"));
	obs_data_set_int(copy, "flags", obs_data_get_int(font, "flags"));
	obs_data_set_int(copy, "size", size);
	obs_data_set_obj(w->data, S_TEXT_FONT, copy);

	obs_data_release(copy);
	obs_data_release(font);
}

/* The styles: the look of the icon, filled in as a starting point to change
 * from. They set how it looks, never what it shows or when. */
static void warp_icon_apply_style(obs_data_t *settings, const char *style, bool as_default)
{
	struct warp_icon_setter w = {settings, as_default};

	/* what every style starts from */
	set_int(&w, S_GLYPH_COLOR, ARGB(0xFFFFFFFF));
	set_int(&w, S_PLATE_COLOR, ARGB(0xC8101014));
	set_int(&w, S_ACCENT_COLOR, ARGB(0xFFFFB400));
	set_int(&w, S_ACCENT_WIDTH, 0);
	set_bool(&w, S_SHOW_PROGRESS, false);
	set_bool(&w, S_ROUNDED, true);
	set_bool(&w, S_POP, true);
	set_bool(&w, S_SHADOW, false);
	set_int(&w, S_SHADOW_COLOR, ARGB(0x99000000));
	set_int(&w, S_SHADOW_DISTANCE, 2);
	set_int(&w, S_SHADOW_BLUR, 6);
	set_int(&w, S_TEXT_COLOR, ARGB(0xFFFFFFFF));
	set_bool(&w, S_TEXT_OUTLINE, false);
	set_bool(&w, S_TEXT_SHADOW, true);
	set_int(&w, S_GLYPH_SCALE, 46);
	set_string(&w, S_TEXT_POSITION, POS_RIGHT);
	set_string(&w, S_CHIP_SHAPE, SHAPE_PILL);
	set_bool(&w, S_ACCENT_BLOCK, false);
	set_string(&w, S_GLYPH_SET, SET_SOLID);
	set_int(&w, S_GLYPH_COLOR2, ARGB(0x66FFFFFF));
	set_bool(&w, S_GLYPH_COLOR2_OWN, false);
	set_int(&w, S_DUOTONE_SHADE, 35);

	if (strcmp(style, STYLE_BADGE) == 0) {
		/* a disc with the replay's progress running round its rim */
		set_string(&w, S_LAYOUT, LAYOUT_BESIDE);
		set_string(&w, S_PLATE, PLATE_CIRCLE);
		set_int(&w, S_SIZE, 112);
		set_int(&w, S_ACCENT_WIDTH, 5);
		set_bool(&w, S_SHOW_PROGRESS, true);
		set_string(&w, S_TEXT_CONTENT, CONTENT_VALUE);
		set_font_size(&w, 40);
	} else if (strcmp(style, STYLE_MINIMAL) == 0) {
		/* the glyph alone, lifted off the picture by its shadow */
		set_string(&w, S_LAYOUT, LAYOUT_BESIDE);
		set_string(&w, S_PLATE, PLATE_NONE);
		set_int(&w, S_SIZE, 96);
		set_int(&w, S_GLYPH_SCALE, 62);
		set_bool(&w, S_SHADOW, true);
		set_string(&w, S_TEXT_CONTENT, CONTENT_VALUE);
		set_font_size(&w, 40);
	} else if (strcmp(style, STYLE_NEON) == 0) {
		/* the glyph alone in cyan, its shadow turned into a glow */
		set_string(&w, S_LAYOUT, LAYOUT_BESIDE);
		set_string(&w, S_PLATE, PLATE_NONE);
		set_int(&w, S_PLATE_COLOR, ARGB(0xCC0A0A14));
		set_int(&w, S_SIZE, 96);
		set_int(&w, S_GLYPH_SCALE, 60);
		set_int(&w, S_GLYPH_COLOR, ARGB(0xFFB8F7FF));
		set_bool(&w, S_SHADOW, true);
		set_int(&w, S_SHADOW_COLOR, ARGB(0xB321E6FF));
		set_int(&w, S_SHADOW_DISTANCE, 0);
		set_int(&w, S_SHADOW_BLUR, 18);
		set_int(&w, S_ACCENT_COLOR, ARGB(0xFFFF2BD6));
		set_int(&w, S_TEXT_COLOR, ARGB(0xFF8FF3FF));
		set_string(&w, S_TEXT_CONTENT, CONTENT_VALUE);
		set_font_size(&w, 40);
	} else if (strcmp(style, STYLE_GLASS) == 0) {
		/* a frosted white pill with dark type, for dark pictures */
		set_string(&w, S_LAYOUT, LAYOUT_CHIP);
		set_int(&w, S_SIZE, 68);
		set_int(&w, S_PLATE_COLOR, ARGB(0xE0F4F5F7));
		set_int(&w, S_GLYPH_COLOR, ARGB(0xFF141418));
		set_int(&w, S_TEXT_COLOR, ARGB(0xFF141418));
		set_int(&w, S_ACCENT_COLOR, ARGB(0x66141418));
		set_int(&w, S_ACCENT_WIDTH, 1);
		set_bool(&w, S_SHOW_PROGRESS, true);
		set_bool(&w, S_TEXT_SHADOW, false);
		set_string(&w, S_TEXT_CONTENT, CONTENT_BOTH);
		set_font_size(&w, 26);
	} else if (strcmp(style, STYLE_TAG) == 0) {
		/* a square sports tag: the glyph dark on an amber block, the
		 * words white on black */
		set_string(&w, S_LAYOUT, LAYOUT_CHIP);
		set_string(&w, S_CHIP_SHAPE, SHAPE_SQUARE);
		set_bool(&w, S_ACCENT_BLOCK, true);
		set_int(&w, S_SIZE, 64);
		set_int(&w, S_PLATE_COLOR, ARGB(0xEB101014));
		set_int(&w, S_GLYPH_COLOR, ARGB(0xFF101014));
		set_bool(&w, S_SHOW_PROGRESS, true);
		set_bool(&w, S_TEXT_SHADOW, false);
		set_string(&w, S_TEXT_CONTENT, CONTENT_BOTH);
		set_font_size(&w, 26);
	} else if (strcmp(style, STYLE_SPORT) == 0) {
		/* the tag leaning forward, in the sport bar's cyan and navy */
		set_string(&w, S_LAYOUT, LAYOUT_CHIP);
		set_string(&w, S_CHIP_SHAPE, SHAPE_SLANTED);
		set_bool(&w, S_ACCENT_BLOCK, true);
		set_int(&w, S_SIZE, 64);
		set_int(&w, S_PLATE_COLOR, ARGB(0xEB0B1020));
		set_int(&w, S_ACCENT_COLOR, ARGB(0xFF19E3FF));
		set_int(&w, S_GLYPH_COLOR, ARGB(0xFF0B1020));
		set_bool(&w, S_SHOW_PROGRESS, true);
		set_bool(&w, S_TEXT_SHADOW, false);
		set_string(&w, S_TEXT_CONTENT, CONTENT_BOTH);
		set_font_size(&w, 26);
	} else if (strcmp(style, STYLE_DUOTONE) == 0) {
		/* amber glyphs in two shades of amber, on a dark tile */
		set_string(&w, S_LAYOUT, LAYOUT_BESIDE);
		set_string(&w, S_PLATE, PLATE_ROUNDED);
		set_int(&w, S_PLATE_COLOR, ARGB(0xE6101014));
		set_int(&w, S_SIZE, 104);
		set_int(&w, S_GLYPH_SCALE, 54);
		set_string(&w, S_GLYPH_SET, SET_DUOTONE);
		set_int(&w, S_GLYPH_COLOR, ARGB(0xFFFFC233));
		set_int(&w, S_DUOTONE_SHADE, 40);
		set_string(&w, S_TEXT_CONTENT, CONTENT_VALUE);
		set_font_size(&w, 40);
	} else if (strcmp(style, STYLE_AURORA) == 0) {
		/* glyphs shaded cyan into violet, glowing violet */
		set_string(&w, S_LAYOUT, LAYOUT_BESIDE);
		set_string(&w, S_PLATE, PLATE_NONE);
		set_int(&w, S_PLATE_COLOR, ARGB(0xCC120C24));
		set_int(&w, S_SIZE, 96);
		set_int(&w, S_GLYPH_SCALE, 62);
		set_string(&w, S_GLYPH_SET, SET_GRADIENT);
		set_int(&w, S_GLYPH_COLOR, ARGB(0xFF5CF2FF));
		set_int(&w, S_GLYPH_COLOR2, ARGB(0xFFA46BFF));
		set_bool(&w, S_SHADOW, true);
		set_int(&w, S_SHADOW_COLOR, ARGB(0x998A5CFF));
		set_int(&w, S_SHADOW_DISTANCE, 0);
		set_int(&w, S_SHADOW_BLUR, 18);
		set_int(&w, S_TEXT_COLOR, ARGB(0xFFD9CCFF));
		set_string(&w, S_TEXT_CONTENT, CONTENT_VALUE);
		set_font_size(&w, 40);
	} else if (strcmp(style, STYLE_OUTLINE) == 0) {
		/* line art: an empty ring that is the progress, and the glyph
		 * drawn as a line to match */
		set_string(&w, S_GLYPH_SET, SET_OUTLINE);
		set_string(&w, S_LAYOUT, LAYOUT_BESIDE);
		set_string(&w, S_PLATE, PLATE_CIRCLE);
		set_int(&w, S_PLATE_COLOR, ARGB(0x00000000));
		set_int(&w, S_SIZE, 104);
		set_int(&w, S_ACCENT_COLOR, ARGB(0xFFFFFFFF));
		set_int(&w, S_ACCENT_WIDTH, 4);
		set_bool(&w, S_SHOW_PROGRESS, true);
		set_bool(&w, S_SHADOW, true);
		set_int(&w, S_SHADOW_COLOR, ARGB(0x80000000));
		set_int(&w, S_SHADOW_DISTANCE, 1);
		set_int(&w, S_SHADOW_BLUR, 4);
		set_string(&w, S_TEXT_CONTENT, CONTENT_VALUE);
		set_font_size(&w, 40);
	} else if (strcmp(style, STYLE_FLAT) == 0) {
		/* plain shapes, no light or motion */
		set_string(&w, S_LAYOUT, LAYOUT_BESIDE);
		set_string(&w, S_PLATE, PLATE_CIRCLE);
		set_int(&w, S_PLATE_COLOR, ARGB(0x99000000));
		set_int(&w, S_SIZE, 128);
		set_int(&w, S_GLYPH_SCALE, 50);
		set_bool(&w, S_ROUNDED, false);
		set_bool(&w, S_POP, false);
		set_string(&w, S_TEXT_CONTENT, CONTENT_VALUE);
		set_font_size(&w, 48);
	} else {
		/* chip: the glyph and what it means in one pill, with an accent
		 * rim, filling with the replay's progress */
		set_string(&w, S_LAYOUT, LAYOUT_CHIP);
		set_string(&w, S_PLATE, PLATE_CIRCLE);
		set_int(&w, S_SIZE, 72);
		set_int(&w, S_ACCENT_WIDTH, 2);
		set_bool(&w, S_SHOW_PROGRESS, true);
		set_bool(&w, S_TEXT_SHADOW, false);
		set_string(&w, S_TEXT_CONTENT, CONTENT_BOTH);
		set_font_size(&w, 28);
	}
}

static void warp_icon_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, WARP_OVERLAY_TARGET, "");
	obs_data_set_default_string(settings, S_STYLE, STYLE_CHIP);
	obs_data_set_default_string(settings, S_STYLE_APPLIED, STYLE_CHIP);
	warp_icon_apply_style(settings, STYLE_CHIP, true);
	obs_data_set_default_bool(settings, S_VALUE_TEXT, true);
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

	bool chip = strcmp(obs_data_get_string(settings, S_LAYOUT), LAYOUT_CHIP) == 0;
	bool plate = chip || strcmp(obs_data_get_string(settings, S_PLATE), PLATE_NONE) != 0;
	bool shadow = obs_data_get_bool(settings, S_SHADOW);
	bool text = obs_data_get_bool(settings, S_VALUE_TEXT);

	const char *set = obs_data_get_string(settings, S_GLYPH_SET);
	bool duotone = strcmp(set, SET_DUOTONE) == 0;
	bool own = obs_data_get_bool(settings, S_GLYPH_COLOR2_OWN);

	/* a duotone's second color is a shade of the first unless one is
	 * picked; a gradient always has both ends picked */
	obs_property_set_visible(obs_properties_get(props, S_GLYPH_COLOR2_OWN), duotone);
	obs_property_set_visible(obs_properties_get(props, S_DUOTONE_SHADE), duotone && !own);
	obs_property_set_visible(obs_properties_get(props, S_GLYPH_COLOR2),
				 strcmp(set, SET_GRADIENT) == 0 || (duotone && own));
	obs_property_set_description(obs_properties_get(props, S_GLYPH_COLOR2),
				     obs_module_text(strcmp(set, SET_GRADIENT) == 0 ? "Warp.Icon.GlyphColor2.Gradient"
										    : "Warp.Icon.GlyphColor2"));
	obs_property_set_description(obs_properties_get(props, S_GLYPH_COLOR),
				     obs_module_text(strcmp(set, SET_GRADIENT) == 0  ? "Warp.Icon.GlyphColor.Gradient"
						     : strcmp(set, SET_DUOTONE) == 0 ? "Warp.Icon.GlyphColor.Duotone"
										     : "Warp.Icon.GlyphColor"));
	obs_property_set_visible(obs_properties_get(props, S_PLATE), !chip);
	obs_property_set_visible(obs_properties_get(props, S_CHIP_SHAPE), chip);
	obs_property_set_visible(obs_properties_get(props, S_ACCENT_BLOCK), chip);
	obs_property_set_visible(obs_properties_get(props, S_PLATE_COLOR), plate);
	obs_property_set_visible(obs_properties_get(props, S_ACCENT_COLOR), plate);
	obs_property_set_visible(obs_properties_get(props, S_ACCENT_WIDTH), plate);
	obs_property_set_visible(obs_properties_get(props, S_SHOW_PROGRESS), plate);
	obs_property_set_visible(obs_properties_get(props, S_SHADOW_COLOR), shadow);
	obs_property_set_visible(obs_properties_get(props, S_SHADOW_DISTANCE), shadow);
	obs_property_set_visible(obs_properties_get(props, S_SHADOW_BLUR), shadow);
	obs_property_set_visible(obs_properties_get(props, S_TEXT_CONTENT), text);
	obs_property_set_visible(obs_properties_get(props, S_TEXT_POSITION), text && !chip);
	obs_property_set_visible(obs_properties_get(props, S_TEXT_FONT), text);
	obs_property_set_visible(obs_properties_get(props, S_TEXT_COLOR), text);
	obs_property_set_visible(obs_properties_get(props, S_TEXT_OUTLINE), text);
	obs_property_set_visible(obs_properties_get(props, S_TEXT_SHADOW), text);

	return true;
}

/* Picking a style fills in the settings it covers, once: the properties
 * call this every time they are opened as well, when it changes nothing. */
static bool warp_icon_style_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	const char *style = obs_data_get_string(settings, S_STYLE);

	if (strcmp(style, obs_data_get_string(settings, S_STYLE_APPLIED)) != 0) {
		warp_icon_apply_style(settings, style, false);
		obs_data_set_string(settings, S_STYLE_APPLIED, style);
	}

	return warp_icon_modified(props, prop, settings);
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

	p = obs_properties_add_list(props, S_STYLE, obs_module_text("Warp.Overlay.Style"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.Style.Chip"), STYLE_CHIP);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.Style.Badge"), STYLE_BADGE);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.Style.Minimal"), STYLE_MINIMAL);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.Style.Tag"), STYLE_TAG);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.Style.Sport"), STYLE_SPORT);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.Style.Glass"), STYLE_GLASS);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.Style.Neon"), STYLE_NEON);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.Style.Duotone"), STYLE_DUOTONE);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.Style.Aurora"), STYLE_AURORA);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.Style.Outline"), STYLE_OUTLINE);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.Style.Flat"), STYLE_FLAT);
	obs_property_set_long_description(p, obs_module_text("Warp.Overlay.Style.Desc"));
	obs_property_set_modified_callback(p, warp_icon_style_modified);

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
	obs_properties_add_bool(group, S_POP, obs_module_text("Warp.Icon.Pop"));

	obs_properties_add_group(props, "show_group", obs_module_text("Warp.Icon.Group.Show"), OBS_GROUP_NORMAL, group);

	/* how it looks */
	group = obs_properties_create();

	p = obs_properties_add_list(group, S_LAYOUT, obs_module_text("Warp.Icon.Layout"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.Layout.Chip"), LAYOUT_CHIP);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.Layout.Beside"), LAYOUT_BESIDE);
	obs_property_set_long_description(p, obs_module_text("Warp.Icon.Layout.Desc"));
	obs_property_set_modified_callback(p, warp_icon_modified);
	p = obs_properties_add_list(group, S_CHIP_SHAPE, obs_module_text("Warp.Icon.ChipShape"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.ChipShape.Pill"), SHAPE_PILL);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.ChipShape.Rounded"), SHAPE_ROUNDED);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.ChipShape.Square"), SHAPE_SQUARE);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.ChipShape.Slanted"), SHAPE_SLANTED);
	p = obs_properties_add_bool(group, S_ACCENT_BLOCK, obs_module_text("Warp.Icon.AccentBlock"));
	obs_property_set_long_description(p, obs_module_text("Warp.Icon.AccentBlock.Desc"));
	p = obs_properties_add_int(group, S_SIZE, obs_module_text("Warp.Icon.Size"), 8, 4096, 1);
	obs_property_int_set_suffix(p, " px");
	p = obs_properties_add_list(group, S_PLATE, obs_module_text("Warp.Icon.Plate"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.Plate.None"), PLATE_NONE);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.Plate.Circle"), PLATE_CIRCLE);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.Plate.Rounded"), PLATE_ROUNDED);
	obs_property_set_modified_callback(p, warp_icon_modified);
	obs_properties_add_color_alpha(group, S_PLATE_COLOR, obs_module_text("Warp.Icon.PlateColor"));
	obs_properties_add_color_alpha(group, S_ACCENT_COLOR, obs_module_text("Warp.Icon.AccentColor"));
	p = obs_properties_add_int(group, S_ACCENT_WIDTH, obs_module_text("Warp.Icon.AccentWidth"), 0, 64, 1);
	obs_property_int_set_suffix(p, " px");
	p = obs_properties_add_bool(group, S_SHOW_PROGRESS, obs_module_text("Warp.Icon.ShowProgress"));
	obs_property_set_long_description(p, obs_module_text("Warp.Icon.ShowProgress.Desc"));
	p = obs_properties_add_int_slider(group, S_GLYPH_SCALE, obs_module_text("Warp.Icon.GlyphScale"), 10, 100, 1);
	obs_property_int_set_suffix(p, "%");
	p = obs_properties_add_list(group, S_GLYPH_SET, obs_module_text("Warp.Icon.GlyphSet"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.GlyphSet.Solid"), SET_SOLID);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.GlyphSet.Duotone"), SET_DUOTONE);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.GlyphSet.Outline"), SET_OUTLINE);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.GlyphSet.Gradient"), SET_GRADIENT);
	obs_property_set_long_description(p, obs_module_text("Warp.Icon.GlyphSet.Desc"));
	obs_property_set_modified_callback(p, warp_icon_modified);
	obs_properties_add_color_alpha(group, S_GLYPH_COLOR, obs_module_text("Warp.Icon.GlyphColor"));
	p = obs_properties_add_int_slider(group, S_DUOTONE_SHADE, obs_module_text("Warp.Icon.DuotoneShade"), -80, 80,
					  1);
	obs_property_int_set_suffix(p, "%");
	obs_property_set_long_description(p, obs_module_text("Warp.Icon.DuotoneShade.Desc"));
	p = obs_properties_add_bool(group, S_GLYPH_COLOR2_OWN, obs_module_text("Warp.Icon.GlyphColor2Own"));
	obs_property_set_long_description(p, obs_module_text("Warp.Icon.GlyphColor2Own.Desc"));
	obs_property_set_modified_callback(p, warp_icon_modified);
	obs_properties_add_color_alpha(group, S_GLYPH_COLOR2, obs_module_text("Warp.Icon.GlyphColor2"));
	obs_properties_add_bool(group, S_ROUNDED, obs_module_text("Warp.Icon.Rounded"));
	p = obs_properties_add_bool(group, S_SHADOW, obs_module_text("Warp.Icon.Shadow"));
	obs_property_set_modified_callback(p, warp_icon_modified);
	obs_properties_add_color_alpha(group, S_SHADOW_COLOR, obs_module_text("Warp.Icon.ShadowColor"));
	p = obs_properties_add_int(group, S_SHADOW_DISTANCE, obs_module_text("Warp.Icon.ShadowDistance"), 0, 256, 1);
	obs_property_int_set_suffix(p, " px");
	p = obs_properties_add_int(group, S_SHADOW_BLUR, obs_module_text("Warp.Icon.ShadowBlur"), 0, 256, 1);
	obs_property_int_set_suffix(p, " px");

	obs_properties_add_group(props, "look_group", obs_module_text("Warp.Icon.Group.Look"), OBS_GROUP_NORMAL, group);

	/* the text with it */
	group = obs_properties_create();

	p = obs_properties_add_bool(group, S_VALUE_TEXT, obs_module_text("Warp.Icon.ValueText"));
	obs_property_set_long_description(p, obs_module_text("Warp.Icon.ValueText.Desc"));
	obs_property_set_modified_callback(p, warp_icon_modified);
	p = obs_properties_add_list(group, S_TEXT_CONTENT, obs_module_text("Warp.Icon.TextContent"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.TextContent.Both"), CONTENT_BOTH);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.TextContent.Value"), CONTENT_VALUE);
	obs_property_list_add_string(p, obs_module_text("Warp.Icon.TextContent.Label"), CONTENT_LABEL);
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

/* flashes 'glyph' up over what is showing, with 'value' to go with it */
static void warp_icon_flash(struct warp_icon_source *s, enum warp_icon_glyph glyph, const char *value)
{
	memset(&s->flash, 0, sizeof(s->flash));
	s->flash.glyph = glyph;
	snprintf(s->flash.value, sizeof(s->flash.value), "%s", value ? value : "");
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
		snprintf(look->value, sizeof(look->value), "%d%%", st->speed);

		if (s->combine_speed)
			look->badge = warp_icon_speed_glyph(st->speed);
		else if (st->state == OBS_MEDIA_STATE_PLAYING)
			look->glyph = warp_icon_speed_glyph(st->speed);
	}

	/* playing as it should is nothing worth an icon */
	if (s->hide_normal_play && look->glyph == WARP_ICON_PLAY && look->badge == WARP_ICON_NONE)
		memset(look, 0, sizeof(*look));
}

/* Puts the text of a look together: its value, the word for what it shows,
 * or both. The word is the badge's when there is one, since that is the thing
 * out of the ordinary - paused in slow motion reads SLOW MO 50% beside the
 * pause glyph. */
static void warp_icon_compose(const struct warp_icon_source *s, struct warp_icon_look *look)
{
	enum warp_icon_glyph named = look->badge != WARP_ICON_NONE ? look->badge : look->glyph;
	const char *label = named != WARP_ICON_NONE ? obs_module_text(warp_icon_label_text[named]) : "";

	look->text[0] = 0;

	if (!s->value_text || look->glyph == WARP_ICON_NONE)
		return;

	switch (s->content) {
	case WARP_ICON_CONTENT_LABEL:
		snprintf(look->text, sizeof(look->text), "%s", label);
		break;
	case WARP_ICON_CONTENT_BOTH:
		if (*look->value)
			snprintf(look->text, sizeof(look->text), "%s %s", label, look->value);
		else
			snprintf(look->text, sizeof(look->text), "%s", label);
		break;
	default:
		snprintf(look->text, sizeof(look->text), "%s", look->value);
		break;
	}
}

/* how big the text is drawn in a chip */
static float warp_icon_chip_text_scale(const struct warp_icon_source *s, uint32_t th)
{
	float room = (float)s->size * CHIP_TEXT_H;

	return th && (float)th > room ? room / (float)th : 1.0f;
}

/* How the chip is laid out, all worked out from its height. A slanted chip
 * leans by 'lean' over its height, and everything in it moves along by half
 * that so it sits in the middle of the lean. */
struct warp_icon_chip {
	float lean;
	float radius;
	/* the glyph's cell: the square the glyph is centred in, the block of
	 * the accent behind it when there is one */
	float cell;
	float text_x;
	float end;
};

static void warp_icon_chip_layout(const struct warp_icon_source *s, struct warp_icon_chip *c)
{
	float size = (float)s->size;
	bool rounded = s->chip_shape == WARP_ICON_CHIP_PILL || s->chip_shape == WARP_ICON_CHIP_ROUNDED;

	c->lean = s->chip_shape == WARP_ICON_CHIP_SLANTED ? roundf(size * 0.3f) : 0.0f;
	c->radius = s->chip_shape == WARP_ICON_CHIP_PILL      ? size * 0.5f
		    : s->chip_shape == WARP_ICON_CHIP_ROUNDED ? roundf(size * 0.22f)
							      : 0.0f;
	c->cell = size + c->lean;
	c->text_x = s->accent_block ? c->cell + roundf(size * 0.24f) : roundf(size * CHIP_TEXT_X + c->lean * 0.5f);
	c->end = roundf(size * (rounded ? CHIP_TEXT_END : 0.3f) + c->lean * 0.5f);
}

/* how wide the chip is with 'tw' x 'th' of text in it */
static float warp_icon_chip_width(const struct warp_icon_source *s, uint32_t tw, uint32_t th)
{
	struct warp_icon_chip c;

	warp_icon_chip_layout(s, &c);

	if (!tw)
		return c.cell;

	return roundf(c.text_x + (float)tw * warp_icon_chip_text_scale(s, th) + c.end);
}

static float ease_out_back(float t)
{
	const float c1 = 1.70158f;
	const float c3 = c1 + 1.0f;
	float u = t - 1.0f;

	return 1.0f + c3 * u * u * u + c1 * u * u;
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

	warp_icon_compose(s, &want);

	/* a new glyph fades and pops in over the old one; new text beside the
	 * same glyph just changes, and the chip eases to its new width */
	if (want.glyph != s->current.glyph || want.badge != s->current.badge) {
		s->previous = s->current;
		s->fade = s->fade_seconds > 0.0f ? 0.0f : 1.0f;
		s->pop_t = s->pop && want.glyph != WARP_ICON_NONE ? 0.0f : 1.0f;
	}

	s->current = want;

	if (s->fade < 1.0f)
		s->fade = warp_overlay_approach(s->fade, 1.0f, 1.0f / s->fade_seconds, seconds);
	if (s->pop_t < 1.0f)
		s->pop_t = warp_overlay_approach(s->pop_t, 1.0f, 1.0f / POP_SECONDS, seconds);

	bool visible = !(s->hide_off_air && !s->status.on_program);

	s->alpha = warp_overlay_approach(s->alpha, visible ? 1.0f : 0.0f,
					 s->fade_seconds > 0.0f ? 1.0f / s->fade_seconds : 1000.0f, seconds);

	/* how far through the replay it is, for the chip's fill or the ring */
	const struct warp_overlay_status *st = &s->status;
	float fraction = st->duration_ms > 0 ? (float)st->time_ms / (float)st->duration_ms : 0.0f;

	if (st->state == OBS_MEDIA_STATE_ENDED)
		fraction = 1.0f;
	fraction = fraction < 0.0f ? 0.0f : fraction > 1.0f ? 1.0f : fraction;
	s->fraction += (fraction - s->fraction) * fminf(1.0f, seconds * 12.0f);

	/* the text: the one coming in, or the one going out as it fades away
	 * with nothing in its place */
	const char *text = *s->current.text ? s->current.text : s->fade < 1.0f ? s->previous.text : "";

	if (s->value_text) {
		warp_overlay_text_set(&s->text, text, s->text_font, s->text_color, s->text_outline, s->text_shadow);

		uint32_t tw = warp_overlay_text_width(&s->text);
		uint32_t th = warp_overlay_text_height(&s->text);

		if (tw > s->text_reserve_w)
			s->text_reserve_w = tw;
		if (th > s->text_reserve_h)
			s->text_reserve_h = th;
	}

	/* the chip closes up round the glyph as text leaves it, and opens out
	 * as it comes in */
	uint32_t tw = s->value_text && *s->current.text ? warp_overlay_text_width(&s->text) : 0;
	float target_w = warp_icon_chip_width(s, tw, warp_overlay_text_height(&s->text));

	if (s->chip_w <= 0.0f || s->current.glyph == WARP_ICON_NONE)
		s->chip_w = s->current.glyph == WARP_ICON_NONE && s->chip_w > 0.0f ? s->chip_w : target_w;
	else
		s->chip_w += (target_w - s->chip_w) * fminf(1.0f, seconds * 14.0f);
}

/* ------------------------------------------------------------------------- */
/* size */

static uint32_t warp_icon_text_extent(const struct warp_icon_source *s, bool width)
{
	if (!s->value_text)
		return 0;

	uint32_t font = (uint32_t)warp_overlay_font_size(s->text_font);

	if (width)
		/* room for a short value before anything has been shown */
		return s->text_reserve_w > font * 2 ? s->text_reserve_w : font * 2;

	return s->text_reserve_h > font ? s->text_reserve_h : font;
}

static uint32_t warp_icon_width(void *data)
{
	struct warp_icon_source *s = data;
	uint32_t text = warp_icon_text_extent(s, true);

	if (s->chip)
		return (uint32_t)warp_icon_chip_width(s, text, warp_icon_text_extent(s, false));
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

	if (!s->chip && text && s->text_below)
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

/* Draws a plate of 'shape' (as the effect numbers them) and, on it, a glyph,
 * in a square 'size' wide at (x, y). The glyph is scaled by 'pop' about the
 * middle. A glyph with a picture of the operator's own is drawn as that
 * picture, fitted inside the room the glyph would have taken. Called with the
 * image lock held. */
static void warp_icon_draw_glyph(struct warp_icon_source *s, gs_effect_t *effect, enum warp_icon_glyph glyph,
				 float shape, uint32_t plate_color, uint32_t glyph_color, float mode, float x, float y,
				 float size, float pop, float opacity)
{
	if (opacity <= 0.0f || size < 1.0f)
		return;

	struct warp_icon_image *img = &s->images[glyph];
	struct vec2 sz;
	struct vec2 shadow_offset;
	float units = 2.0f / size;
	bool own = glyph != WARP_ICON_NONE && img->loaded;

	vec2_set(&sz, size, size);
	vec2_set(&shadow_offset, s->shadow_distance * units, s->shadow_distance * units);

	gs_effect_set_vec2(gs_effect_get_param_by_name(effect, "size"), &sz);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "opacity"), opacity);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "glyph_round"), s->rounded ? GLYPH_ROUND : 0.0f);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "glyph_scale"), s->glyph_scale);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "shadow"), s->shadow ? 1.0f : 0.0f);
	gs_effect_set_vec2(gs_effect_get_param_by_name(effect, "shadow_offset"), &shadow_offset);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "shadow_soft"), (s->shadow_blur + 1.0f) * units);
	warp_icon_set_color(effect, "glyph_color", glyph_color);
	warp_icon_set_color(effect, "glyph_color2", s->glyph_color2);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "glyph_mode"), mode);
	warp_icon_set_color(effect, "plate_color", plate_color);
	warp_icon_set_color(effect, "shadow_color", s->shadow_color);

	/* the plate on its own, then the glyph, so the glyph can pop in on a
	 * plate that stays put */
	if (shape > 0.5f) {
		gs_effect_set_float(gs_effect_get_param_by_name(effect, "plate_shape"), shape);
		gs_effect_set_float(gs_effect_get_param_by_name(effect, "glyph"), 0.0f);

		gs_matrix_push();
		gs_matrix_translate3f(x, y, 0.0f);
		warp_icon_draw_tech(effect, "Icon", NULL, (uint32_t)size, (uint32_t)size);
		gs_matrix_pop();
	}

	if (glyph == WARP_ICON_NONE)
		return;

	float gsize = size * pop;
	float gx = x + (size - gsize) * 0.5f;
	float gy = y + (size - gsize) * 0.5f;

	if (!own) {
		vec2_set(&sz, gsize, gsize);
		vec2_set(&shadow_offset, s->shadow_distance * 2.0f / gsize, s->shadow_distance * 2.0f / gsize);
		gs_effect_set_vec2(gs_effect_get_param_by_name(effect, "size"), &sz);
		gs_effect_set_vec2(gs_effect_get_param_by_name(effect, "shadow_offset"), &shadow_offset);
		gs_effect_set_float(gs_effect_get_param_by_name(effect, "plate_shape"), 0.0f);
		gs_effect_set_float(gs_effect_get_param_by_name(effect, "glyph"), (float)glyph);

		gs_matrix_push();
		gs_matrix_translate3f(gx, gy, 0.0f);
		warp_icon_draw_tech(effect, "Icon", NULL, (uint32_t)gsize, (uint32_t)gsize);
		gs_matrix_pop();
		return;
	}

	/* the picture, fitted inside the glyph's room and kept to its shape */
	float room = gsize * s->glyph_scale;
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
	gs_matrix_translate3f(gx + (gsize - w) * 0.5f, gy + (gsize - h) * 0.5f, 0.0f);
	gs_matrix_scale3f(fit, fit, 1.0f);
	warp_icon_draw_tech(effect, "Image", img->image.texture, img->image.cx, img->image.cy);
	gs_matrix_pop();
}

static float warp_icon_pop(const struct warp_icon_source *s)
{
	if (s->pop_t >= 1.0f)
		return 1.0f;

	return 0.7f + 0.3f * ease_out_back(s->pop_t);
}

/* Draws the accent and the replay's progress round a plate in the beside
 * layout: a ring round a circle that fills as the replay plays, or a rim round
 * a rounded square. */
static void warp_icon_draw_rim(struct warp_icon_source *s, gs_effect_t *effect, float x, float size, float opacity)
{
	if (!s->accent_width || s->plate_shape < 0.5f)
		return;

	struct warp_overlay_bar rim = {0};

	rim.w = size;
	rim.h = size;
	rim.opacity = opacity;
	warp_overlay_rgba(&rim.fill, s->accent_color);
	rim.fill2 = rim.fill;

	if (s->plate_shape < 1.5f) {
		rim.mode = 4.0f;
		rim.ring_thickness = (float)s->accent_width;
		rim.radius = 1.0f;
		rim.progress = s->show_progress ? s->fraction : 1.0f;
		/* the part still to play, faint */
		vec4_set(&rim.track, 1.0f, 1.0f, 1.0f, s->show_progress ? 0.18f : 0.0f);
	} else {
		rim.radius = size * 0.15f;
		rim.border = (float)s->accent_width;
		rim.border_color = rim.fill;
	}

	warp_overlay_draw_bar(effect, &rim, x, 0.0f);
}

/* the beside layout: a glyph on its plate, a badge in its corner */
static void warp_icon_draw_beside(struct warp_icon_source *s, gs_effect_t *effect, const struct warp_icon_look *look,
				  float plate_alpha, float glyph_alpha)
{
	float size = (float)s->size;
	float x = 0.0f;

	/* below, the icon is centred over its text */
	if (s->value_text && s->text_below)
		x = ((float)warp_icon_width(s) - size) * 0.5f;

	warp_icon_draw_glyph(s, effect, WARP_ICON_NONE, s->plate_shape, s->plate_color, s->glyph_color, 0.0f, x, 0.0f,
			     size, 1.0f, plate_alpha);
	warp_icon_draw_rim(s, effect, x, size, plate_alpha);
	warp_icon_draw_glyph(s, effect, look->glyph, 0.0f, s->plate_color, s->glyph_color, s->glyph_mode, x, 0.0f, size,
			     warp_icon_pop(s), glyph_alpha);

	if (look->badge != WARP_ICON_NONE) {
		/* With an accent, the badge is a pip in it with the glyph cut
		 * out in the plate's color, the way a notification dot sits on
		 * an icon; without one it is a small plate like the big one. */
		float badge = size * BADGE_SCALE;
		float shape = s->plate_shape > 0.5f ? s->plate_shape : 1.0f;
		bool pip = s->accent_width > 0;
		uint32_t plate = pip ? s->accent_color : s->plate_color;
		uint32_t mark = pip ? (s->plate_color | 0xFF000000u) : s->glyph_color;

		/* kept solid, so it reads at the size it is */
		warp_icon_draw_glyph(s, effect, look->badge, shape, plate, mark, 0.0f, x + size - badge * 0.92f,
				     size - badge * 0.92f, badge, 1.0f, glyph_alpha);
	}
}

/* how strongly the plate shows: steady while one glyph gives way to another,
 * fading only when there is nothing on one side of the change */
static float warp_icon_plate_alpha(const struct warp_icon_source *s)
{
	bool now = s->current.glyph != WARP_ICON_NONE;
	bool before = s->previous.glyph != WARP_ICON_NONE && s->fade < 1.0f;

	if (now && before)
		return s->alpha;
	if (now)
		return s->alpha * s->fade;
	if (before)
		return s->alpha * (1.0f - s->fade);
	return 0.0f;
}

static void warp_icon_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);

	struct warp_icon_source *s = data;
	gs_effect_t *effect = warp_overlay_effect();

	if (!effect || s->alpha <= 0.0f || !s->size)
		return;

	float size = (float)s->size;
	float plate_alpha = warp_icon_plate_alpha(s);

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(gs_get_linear_srgb());

	pthread_mutex_lock(&s->image_mutex);

	if (s->chip) {
		/* the chip, rimmed in the accent, in its shape */
		struct warp_icon_chip c;
		struct warp_overlay_bar pill = {0};

		warp_icon_chip_layout(s, &c);

		pill.w = fmaxf(s->chip_w, c.cell);
		pill.h = size;
		pill.radius = c.radius;
		pill.slant = c.lean;
		pill.opacity = plate_alpha;
		pill.border = (float)s->accent_width;
		warp_overlay_rgba(&pill.track, s->plate_color);
		warp_overlay_rgba(&pill.border_color, s->accent_color);
		warp_overlay_draw_bar(effect, &pill, 0.0f, 0.0f);

		/* the glyph's block of accent, cut at the chip's angle */
		if (s->accent_block) {
			struct warp_overlay_bar block = {0};

			block.w = c.cell;
			block.h = size;
			block.radius = c.radius;
			block.slant = c.lean;
			block.opacity = plate_alpha;
			warp_overlay_rgba(&block.track, s->accent_color);
			warp_overlay_draw_bar(effect, &block, 0.0f, 0.0f);
		}

		/* the replay's progress as a fine line under the text, set to
		 * its measure like an underline */
		float line_x = c.text_x;
		float line_w = pill.w - c.end - line_x;

		if (s->show_progress && line_w > 2.0f) {
			struct warp_overlay_bar line = {0};

			line.w = line_w;
			line.h = fmaxf(2.0f, roundf(size * 0.045f));
			line.radius = line.h * 0.5f;
			line.progress = s->fraction;
			line.opacity = plate_alpha;
			warp_overlay_rgba(&line.fill, s->accent_color);
			line.fill.w = fmaxf(line.fill.w, 0.85f);
			line.fill2 = line.fill;
			/* the part still to play, a faint trace of the same */
			line.track = line.fill;
			line.track.w *= 0.25f;
			warp_overlay_draw_bar(effect, &line, line_x, roundf(size * 0.80f - line.h * 0.5f));
		}

		float gx = roundf(c.lean * 0.5f);

		if (s->fade < 1.0f)
			warp_icon_draw_glyph(s, effect, s->previous.glyph, 0.0f, s->plate_color, s->glyph_color,
					     s->glyph_mode, gx, 0.0f, size, 1.0f, s->alpha * (1.0f - s->fade));
		warp_icon_draw_glyph(s, effect, s->current.glyph, 0.0f, s->plate_color, s->glyph_color, s->glyph_mode,
				     gx, 0.0f, size, warp_icon_pop(s), s->alpha * s->fade);
	} else {
		if (s->fade < 1.0f)
			warp_icon_draw_beside(s, effect, &s->previous, 0.0f, s->alpha * (1.0f - s->fade));
		warp_icon_draw_beside(s, effect, &s->current, plate_alpha, s->alpha * s->fade);
	}

	pthread_mutex_unlock(&s->image_mutex);

	gs_enable_framebuffer_srgb(previous);

	/* the text: in the chip, or beside the icon or under it */
	uint32_t tw = warp_overlay_text_width(&s->text);
	uint32_t th = warp_overlay_text_height(&s->text);

	if (!s->value_text || !tw || !th)
		return;

	float text_alpha = *s->current.text ? s->fade : 1.0f - s->fade;
	float x, y, scale = 1.0f;

	if (s->chip) {
		struct warp_icon_chip c;

		warp_icon_chip_layout(s, &c);
		scale = warp_icon_chip_text_scale(s, th);
		x = c.text_x;
		/* lifted a little when the progress line runs under it, so the
		 * two read as a caption over its meter rather than underlined */
		y = (size * (s->show_progress ? 0.9f : 1.0f) - (float)th * scale) * 0.5f;

		/* the text is only shown once the chip has opened out to take
		 * it, so it never spills over the end */
		float room = s->chip_w - c.end - x;
		float fits = room / fmaxf((float)tw * scale, 1.0f);

		text_alpha *= fits >= 1.0f ? 1.0f : fmaxf(0.0f, (fits - 0.6f) / 0.4f);
	} else if (s->text_below) {
		float gap = size * TEXT_GAP;

		x = ((float)warp_icon_width(s) - (float)tw) * 0.5f;
		y = size + gap;
	} else {
		x = size + size * TEXT_GAP;
		y = (size - (float)th) * 0.5f;
	}

	warp_overlay_text_render(&s->text, roundf(x), roundf(y), scale, s->alpha * text_alpha);
}

/* ------------------------------------------------------------------------- */

static void *warp_icon_create(obs_data_t *settings, obs_source_t *source)
{
	struct warp_icon_source *s = bzalloc(sizeof(*s));

	s->source = source;
	s->fade = 1.0f;
	s->pop_t = 1.0f;

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
