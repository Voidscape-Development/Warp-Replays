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

/* Warp Progress Bar: how far through its replay a Warp Media or Warp Playlist
 * source is, drawn as a bar or a ring to sit over the replay on air. For a
 * playlist it is the file that is playing, from its start to its end - or the
 * clip of it, when the file is played as one - the same position the media
 * controls show. */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <obs-module.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include "warp-overlay.h"

#define S_STYLE "style"
/* the style the settings were last filled in from, so picking one fills them
 * in once rather than every time the properties are opened */
#define S_STYLE_APPLIED "style_applied"
#define S_SHAPE "shape"
#define S_SLANT "slant"
#define S_SEGMENTS "segments"
#define S_SEGMENT_GAP "segment_gap"
#define S_SHEEN "sheen"
#define S_GLOW "glow"
#define S_SHADOW "shadow"
#define S_SHADOW_COLOR "shadow_color"
#define S_SHADOW_BLUR "shadow_blur"
#define S_TEXT_CHIP "text_chip"
#define S_CHIP_COLOR "chip_color"
#define S_REVERSE "reverse"
#define S_WIDTH "width"
#define S_HEIGHT "height"
#define S_RING_SIZE "ring_size"
#define S_RING_THICKNESS "ring_thickness"
#define S_RADIUS "corner_radius"
#define S_ROUNDED_ENDS "rounded_ends"
#define S_FILL_COLOR "fill_color"
#define S_TRACK_COLOR "track_color"
#define S_GRADIENT "gradient"
#define S_FILL_COLOR2 "fill_color2"
#define S_BORDER_WIDTH "border_width"
#define S_BORDER_COLOR "border_color"
#define S_PLAYHEAD "playhead"
#define S_PLAYHEAD_WIDTH "playhead_width"
#define S_PLAYHEAD_COLOR "playhead_color"
#define S_COLOR_BY_STATE "color_by_state"
#define S_PAUSED_COLOR "paused_color"
#define S_SLOW_COLOR "slow_color"
#define S_FAST_COLOR "fast_color"
#define S_ENDED_COLOR "ended_color"
#define S_TEXT "text"
#define S_TEXT_POSITION "text_position"
#define S_TEXT_FONT "text_font"
#define S_TEXT_COLOR "text_color"
#define S_TEXT_OUTLINE "text_outline"
#define S_TEXT_SHADOW "text_shadow"
#define S_REAL_TIME "real_time"
#define S_SMOOTH "smooth"
#define S_HIDE_OFF_AIR "hide_off_air"
#define S_IDLE_STOPPED "idle_stopped"
#define S_IDLE_ENDED "idle_ended"
#define S_IDLE_NONE "idle_none"

#define STYLE_BROADCAST "broadcast"
#define STYLE_SEGMENTED "segmented"
#define STYLE_SPORT "sport"
#define STYLE_MINIMAL "minimal"
#define STYLE_FLAT "flat"

#define PLAYHEAD_NONE "none"
#define PLAYHEAD_LINE "line"
#define PLAYHEAD_KNOB "knob"

#define SHAPE_HORIZONTAL "horizontal"
#define SHAPE_VERTICAL "vertical"
#define SHAPE_RING "ring"

#define TEXT_NONE "none"
#define TEXT_ELAPSED "elapsed"
#define TEXT_REMAINING "remaining"
#define TEXT_ELAPSED_TOTAL "elapsed_total"
#define TEXT_REMAINING_TOTAL "remaining_total"
#define TEXT_PERCENT "percent"

#define POS_INSIDE "inside"
#define POS_ABOVE "above"
#define POS_BELOW "below"
#define POS_FOLLOW "follow"

#define IDLE_EMPTY "empty"
#define IDLE_FULL "full"
#define IDLE_LAST "last"
#define IDLE_HIDE "hide"

/* how long the bar takes to fade in or out, and to change color */
#define FADE_SECONDS 0.25f
#define COLOR_SECONDS 0.2f
/* a smoothed position further than this from the source's own is put straight
 * back on it, rather than drifting there: a seek, a step, a new file */
#define SMOOTH_SNAP_MS 300.0
/* how quickly the smoothed position settles onto the source's own, per second */
#define SMOOTH_PULL 8.0

enum warp_progress_shape {
	WARP_PROGRESS_HORIZONTAL,
	WARP_PROGRESS_VERTICAL,
	WARP_PROGRESS_RING,
};

enum warp_progress_text {
	WARP_PROGRESS_TEXT_NONE,
	WARP_PROGRESS_TEXT_ELAPSED,
	WARP_PROGRESS_TEXT_REMAINING,
	WARP_PROGRESS_TEXT_ELAPSED_TOTAL,
	WARP_PROGRESS_TEXT_REMAINING_TOTAL,
	WARP_PROGRESS_TEXT_PERCENT,
};

enum warp_progress_text_pos {
	WARP_PROGRESS_POS_INSIDE,
	WARP_PROGRESS_POS_ABOVE,
	WARP_PROGRESS_POS_BELOW,
	/* above the bar, riding along with the position */
	WARP_PROGRESS_POS_FOLLOW,
};

enum warp_progress_idle {
	WARP_PROGRESS_IDLE_EMPTY,
	WARP_PROGRESS_IDLE_FULL,
	WARP_PROGRESS_IDLE_LAST,
	WARP_PROGRESS_IDLE_HIDE,
};

struct warp_progress_source {
	obs_source_t *source;
	struct warp_overlay_target target;
	struct warp_overlay_text text;

	/* settings; written by the update, read on the graphics thread, which
	 * is where libobs applies a video source's update */
	enum warp_progress_shape shape;
	bool reverse;
	uint32_t width;
	uint32_t height;
	uint32_t ring_size;
	uint32_t ring_thickness;
	uint32_t radius;
	bool rounded_ends;
	uint32_t fill_color;
	uint32_t fill_color2;
	uint32_t track_color;
	bool gradient;
	uint32_t border_width;
	uint32_t border_color;
	/* 0 none, 1 line, 2 knob, as the effect takes it */
	float playhead;
	uint32_t playhead_width;
	uint32_t slant;
	uint32_t segments;
	uint32_t segment_gap;
	float sheen;
	uint32_t glow;
	bool shadow;
	uint32_t shadow_color;
	uint32_t shadow_blur;
	bool text_chip;
	uint32_t chip_color;
	uint32_t playhead_color;
	bool color_by_state;
	uint32_t paused_color;
	uint32_t slow_color;
	uint32_t fast_color;
	uint32_t ended_color;
	enum warp_progress_text text_mode;
	enum warp_progress_text_pos text_pos;
	obs_data_t *text_font;
	uint32_t text_color;
	bool text_outline;
	bool text_shadow;
	bool real_time;
	bool smooth;
	bool hide_off_air;
	enum warp_progress_idle idle_stopped;
	enum warp_progress_idle idle_ended;
	enum warp_progress_idle idle_none;

	/* what is on screen; graphics thread */
	struct warp_overlay_status status;
	/* the position shown, in the source's own milliseconds */
	double shown_ms;
	/* the share of the bar filled */
	float fraction;
	float alpha;
	/* the colors the fill is at, moving toward the ones its state calls for */
	struct vec4 fill;
	struct vec4 fill2;
	bool colors_set;

	/* the fill easing back from where the outgoing file left it to the
	 * incoming one as a playlist's transition plays */
	bool animating;
	float anim_from;
	float anim_elapsed;
	float anim_seconds;
};

/* ------------------------------------------------------------------------- */

static const char *warp_progress_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Warp.Progress.Name");
}

static enum warp_progress_idle warp_progress_idle_from(const char *value)
{
	if (strcmp(value, IDLE_FULL) == 0)
		return WARP_PROGRESS_IDLE_FULL;
	if (strcmp(value, IDLE_LAST) == 0)
		return WARP_PROGRESS_IDLE_LAST;
	if (strcmp(value, IDLE_HIDE) == 0)
		return WARP_PROGRESS_IDLE_HIDE;
	return WARP_PROGRESS_IDLE_EMPTY;
}

static void warp_progress_update(void *data, obs_data_t *settings)
{
	struct warp_progress_source *s = data;
	const char *shape = obs_data_get_string(settings, S_SHAPE);
	const char *text = obs_data_get_string(settings, S_TEXT);
	const char *pos = obs_data_get_string(settings, S_TEXT_POSITION);

	warp_overlay_target_set(&s->target, obs_data_get_string(settings, WARP_OVERLAY_TARGET));

	s->shape = strcmp(shape, SHAPE_VERTICAL) == 0 ? WARP_PROGRESS_VERTICAL
		   : strcmp(shape, SHAPE_RING) == 0   ? WARP_PROGRESS_RING
						      : WARP_PROGRESS_HORIZONTAL;
	s->reverse = obs_data_get_bool(settings, S_REVERSE);
	s->width = (uint32_t)obs_data_get_int(settings, S_WIDTH);
	s->height = (uint32_t)obs_data_get_int(settings, S_HEIGHT);
	s->ring_size = (uint32_t)obs_data_get_int(settings, S_RING_SIZE);
	s->ring_thickness = (uint32_t)obs_data_get_int(settings, S_RING_THICKNESS);
	s->radius = (uint32_t)obs_data_get_int(settings, S_RADIUS);
	s->rounded_ends = obs_data_get_bool(settings, S_ROUNDED_ENDS);
	s->fill_color = (uint32_t)obs_data_get_int(settings, S_FILL_COLOR);
	s->fill_color2 = (uint32_t)obs_data_get_int(settings, S_FILL_COLOR2);
	s->track_color = (uint32_t)obs_data_get_int(settings, S_TRACK_COLOR);
	s->gradient = obs_data_get_bool(settings, S_GRADIENT);
	s->border_width = (uint32_t)obs_data_get_int(settings, S_BORDER_WIDTH);
	s->border_color = (uint32_t)obs_data_get_int(settings, S_BORDER_COLOR);
	const char *playhead = obs_data_get_string(settings, S_PLAYHEAD);

	s->playhead = strcmp(playhead, PLAYHEAD_KNOB) == 0 ? 2.0f : strcmp(playhead, PLAYHEAD_LINE) == 0 ? 1.0f : 0.0f;
	s->slant = (uint32_t)obs_data_get_int(settings, S_SLANT);
	s->segments = (uint32_t)obs_data_get_int(settings, S_SEGMENTS);
	s->segment_gap = (uint32_t)obs_data_get_int(settings, S_SEGMENT_GAP);
	s->sheen = (float)obs_data_get_int(settings, S_SHEEN) / 100.0f;
	s->glow = (uint32_t)obs_data_get_int(settings, S_GLOW);
	s->shadow = obs_data_get_bool(settings, S_SHADOW);
	s->shadow_color = (uint32_t)obs_data_get_int(settings, S_SHADOW_COLOR);
	s->shadow_blur = (uint32_t)obs_data_get_int(settings, S_SHADOW_BLUR);
	s->text_chip = obs_data_get_bool(settings, S_TEXT_CHIP);
	s->chip_color = (uint32_t)obs_data_get_int(settings, S_CHIP_COLOR);
	s->playhead_width = (uint32_t)obs_data_get_int(settings, S_PLAYHEAD_WIDTH);
	s->playhead_color = (uint32_t)obs_data_get_int(settings, S_PLAYHEAD_COLOR);
	s->color_by_state = obs_data_get_bool(settings, S_COLOR_BY_STATE);
	s->paused_color = (uint32_t)obs_data_get_int(settings, S_PAUSED_COLOR);
	s->slow_color = (uint32_t)obs_data_get_int(settings, S_SLOW_COLOR);
	s->fast_color = (uint32_t)obs_data_get_int(settings, S_FAST_COLOR);
	s->ended_color = (uint32_t)obs_data_get_int(settings, S_ENDED_COLOR);

	s->text_mode = strcmp(text, TEXT_ELAPSED) == 0           ? WARP_PROGRESS_TEXT_ELAPSED
		       : strcmp(text, TEXT_REMAINING) == 0       ? WARP_PROGRESS_TEXT_REMAINING
		       : strcmp(text, TEXT_ELAPSED_TOTAL) == 0   ? WARP_PROGRESS_TEXT_ELAPSED_TOTAL
		       : strcmp(text, TEXT_REMAINING_TOTAL) == 0 ? WARP_PROGRESS_TEXT_REMAINING_TOTAL
		       : strcmp(text, TEXT_PERCENT) == 0         ? WARP_PROGRESS_TEXT_PERCENT
								 : WARP_PROGRESS_TEXT_NONE;
	s->text_pos = strcmp(pos, POS_ABOVE) == 0    ? WARP_PROGRESS_POS_ABOVE
		      : strcmp(pos, POS_BELOW) == 0  ? WARP_PROGRESS_POS_BELOW
		      : strcmp(pos, POS_FOLLOW) == 0 ? WARP_PROGRESS_POS_FOLLOW
						     : WARP_PROGRESS_POS_INSIDE;

	/* riding along with the position only makes sense along a bar that
	 * runs across; up a vertical one the time sits above it, and in a
	 * ring, in the middle */
	if (s->text_pos == WARP_PROGRESS_POS_FOLLOW && s->shape == WARP_PROGRESS_VERTICAL)
		s->text_pos = WARP_PROGRESS_POS_ABOVE;
	if (s->text_pos == WARP_PROGRESS_POS_FOLLOW && s->shape == WARP_PROGRESS_RING)
		s->text_pos = WARP_PROGRESS_POS_INSIDE;

	obs_data_release(s->text_font);
	s->text_font = obs_data_get_obj(settings, S_TEXT_FONT);
	s->text_color = (uint32_t)obs_data_get_int(settings, S_TEXT_COLOR);
	s->text_outline = obs_data_get_bool(settings, S_TEXT_OUTLINE);
	s->text_shadow = obs_data_get_bool(settings, S_TEXT_SHADOW);
	s->real_time = obs_data_get_bool(settings, S_REAL_TIME);
	s->smooth = obs_data_get_bool(settings, S_SMOOTH);
	s->hide_off_air = obs_data_get_bool(settings, S_HIDE_OFF_AIR);
	s->idle_stopped = warp_progress_idle_from(obs_data_get_string(settings, S_IDLE_STOPPED));
	s->idle_ended = warp_progress_idle_from(obs_data_get_string(settings, S_IDLE_ENDED));
	s->idle_none = warp_progress_idle_from(obs_data_get_string(settings, S_IDLE_NONE));

	/* the colors are taken straight to the new settings rather than
	 * faded there, so what is set is what is seen */
	s->colors_set = false;
}

/* colors as 0xAARRGGBB, the way they are written everywhere else, turned
 * into the 0xAABBGGRR OBS keeps them as */
#define ARGB(c) \
	((long long)(((uint32_t)(c) & 0xFF00FF00u) | (((uint32_t)(c) >> 16) & 0xFFu) | (((uint32_t)(c) & 0xFFu) << 16)))

struct warp_progress_setter {
	obs_data_t *data;
	bool as_default;
};

static void set_int(struct warp_progress_setter *w, const char *key, long long value)
{
	if (w->as_default)
		obs_data_set_default_int(w->data, key, value);
	else
		obs_data_set_int(w->data, key, value);
}

static void set_bool(struct warp_progress_setter *w, const char *key, bool value)
{
	if (w->as_default)
		obs_data_set_default_bool(w->data, key, value);
	else
		obs_data_set_bool(w->data, key, value);
}

static void set_string(struct warp_progress_setter *w, const char *key, const char *value)
{
	if (w->as_default)
		obs_data_set_default_string(w->data, key, value);
	else
		obs_data_set_string(w->data, key, value);
}

/* The styles: the look of the bar, filled in as a starting point to change
 * from. They set how it looks, never what it measures, where it sits or how
 * big it is along its length. */
static void warp_progress_apply_style(obs_data_t *settings, const char *style, bool as_default)
{
	struct warp_progress_setter w = {settings, as_default};

	/* what every style starts from */
	set_bool(&w, S_GRADIENT, false);
	set_int(&w, S_FILL_COLOR2, ARGB(0xFFFFFFFF));
	set_int(&w, S_BORDER_WIDTH, 0);
	set_int(&w, S_BORDER_COLOR, ARGB(0xFF000000));
	set_int(&w, S_SLANT, 0);
	set_int(&w, S_SEGMENTS, 0);
	set_int(&w, S_SEGMENT_GAP, 3);
	set_int(&w, S_SHEEN, 0);
	set_int(&w, S_GLOW, 0);
	set_bool(&w, S_SHADOW, true);
	set_int(&w, S_SHADOW_COLOR, ARGB(0x8C000000));
	set_int(&w, S_SHADOW_BLUR, 10);
	set_string(&w, S_PLAYHEAD, PLAYHEAD_NONE);
	set_int(&w, S_PLAYHEAD_COLOR, ARGB(0xFFFFFFFF));
	set_int(&w, S_PLAYHEAD_WIDTH, 16);
	set_bool(&w, S_ROUNDED_ENDS, true);
	set_bool(&w, S_TEXT_CHIP, false);
	set_int(&w, S_CHIP_COLOR, ARGB(0xC8101014));
	set_int(&w, S_TEXT_COLOR, ARGB(0xFFFFFFFF));
	set_bool(&w, S_TEXT_SHADOW, false);
	set_bool(&w, S_TEXT_OUTLINE, false);

	if (strcmp(style, STYLE_SEGMENTED) == 0) {
		/* a strip of tape, counted off */
		set_int(&w, S_HEIGHT, 10);
		set_int(&w, S_RING_THICKNESS, 14);
		set_int(&w, S_RADIUS, 2);
		set_int(&w, S_FILL_COLOR, ARGB(0xFFFFFFFF));
		set_int(&w, S_TRACK_COLOR, ARGB(0x2EFFFFFF));
		set_int(&w, S_SEGMENTS, 24);
		set_int(&w, S_SEGMENT_GAP, 3);
		set_int(&w, S_GLOW, 8);
		set_bool(&w, S_ROUNDED_ENDS, false);
		set_string(&w, S_TEXT_POSITION, POS_ABOVE);
		set_bool(&w, S_TEXT_SHADOW, true);
	} else if (strcmp(style, STYLE_SPORT) == 0) {
		/* leaning forward, in a cool gradient, with a line for a head */
		set_int(&w, S_HEIGHT, 16);
		set_int(&w, S_RING_THICKNESS, 16);
		set_int(&w, S_RADIUS, 0);
		set_int(&w, S_SLANT, 12);
		set_bool(&w, S_GRADIENT, true);
		set_int(&w, S_FILL_COLOR, ARGB(0xFF19E3FF));
		set_int(&w, S_FILL_COLOR2, ARGB(0xFF2F6BFF));
		set_int(&w, S_TRACK_COLOR, ARGB(0x99000000));
		set_int(&w, S_SHEEN, 50);
		set_int(&w, S_GLOW, 10);
		set_string(&w, S_PLAYHEAD, PLAYHEAD_LINE);
		set_int(&w, S_PLAYHEAD_WIDTH, 3);
		set_bool(&w, S_ROUNDED_ENDS, false);
		set_string(&w, S_TEXT_POSITION, POS_FOLLOW);
		set_bool(&w, S_TEXT_CHIP, true);
		set_int(&w, S_CHIP_COLOR, ARGB(0xE62F6BFF));
	} else if (strcmp(style, STYLE_MINIMAL) == 0) {
		/* a hairline with a dot on it */
		set_int(&w, S_HEIGHT, 4);
		set_int(&w, S_RING_THICKNESS, 6);
		set_int(&w, S_RADIUS, 2);
		set_int(&w, S_FILL_COLOR, ARGB(0xFFFF3B30));
		set_int(&w, S_TRACK_COLOR, ARGB(0x47FFFFFF));
		set_int(&w, S_SHADOW_COLOR, ARGB(0x59000000));
		set_int(&w, S_SHADOW_BLUR, 4);
		set_string(&w, S_PLAYHEAD, PLAYHEAD_KNOB);
		set_int(&w, S_PLAYHEAD_COLOR, ARGB(0xFFFF3B30));
		set_int(&w, S_PLAYHEAD_WIDTH, 12);
		set_string(&w, S_TEXT_POSITION, POS_BELOW);
		set_bool(&w, S_TEXT_SHADOW, true);
	} else if (strcmp(style, STYLE_FLAT) == 0) {
		/* plain shapes, no light or shadow */
		set_int(&w, S_HEIGHT, 12);
		set_int(&w, S_RING_THICKNESS, 16);
		set_int(&w, S_RADIUS, 6);
		set_int(&w, S_FILL_COLOR, ARGB(0xFFFFFFFF));
		set_int(&w, S_TRACK_COLOR, ARGB(0x80000000));
		set_bool(&w, S_SHADOW, false);
		set_string(&w, S_TEXT_POSITION, POS_ABOVE);
		set_bool(&w, S_TEXT_SHADOW, true);
	} else {
		/* broadcast: a warm fill lit from above with a glowing head and a
		 * knob, over a pale track, the time in a chip riding with it */
		set_int(&w, S_HEIGHT, 8);
		set_int(&w, S_RING_THICKNESS, 12);
		set_int(&w, S_RADIUS, 4);
		set_bool(&w, S_GRADIENT, true);
		set_int(&w, S_FILL_COLOR, ARGB(0xFFFFD34D));
		set_int(&w, S_FILL_COLOR2, ARGB(0xFFFF8A1F));
		set_int(&w, S_TRACK_COLOR, ARGB(0x38FFFFFF));
		set_int(&w, S_SHEEN, 60);
		set_int(&w, S_GLOW, 12);
		set_string(&w, S_PLAYHEAD, PLAYHEAD_KNOB);
		set_int(&w, S_PLAYHEAD_WIDTH, 18);
		set_string(&w, S_TEXT_POSITION, POS_FOLLOW);
		set_bool(&w, S_TEXT_CHIP, true);
	}
}

static void warp_progress_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, WARP_OVERLAY_TARGET, "");
	obs_data_set_default_string(settings, S_STYLE, STYLE_BROADCAST);
	obs_data_set_default_string(settings, S_STYLE_APPLIED, STYLE_BROADCAST);
	obs_data_set_default_string(settings, S_SHAPE, SHAPE_HORIZONTAL);
	obs_data_set_default_bool(settings, S_REVERSE, false);
	obs_data_set_default_int(settings, S_WIDTH, 960);
	obs_data_set_default_int(settings, S_RING_SIZE, 160);
	warp_progress_apply_style(settings, STYLE_BROADCAST, true);
	obs_data_set_default_bool(settings, S_COLOR_BY_STATE, false);
	obs_data_set_default_int(settings, S_PAUSED_COLOR, ARGB(0xFFFFC000));
	obs_data_set_default_int(settings, S_SLOW_COLOR, ARGB(0xFF40C0FF));
	obs_data_set_default_int(settings, S_FAST_COLOR, ARGB(0xFFFF4040));
	obs_data_set_default_int(settings, S_ENDED_COLOR, ARGB(0xFF808080));
	obs_data_set_default_string(settings, S_TEXT, TEXT_NONE);
	warp_overlay_default_font(settings, S_TEXT_FONT, 22);
	obs_data_set_default_bool(settings, S_REAL_TIME, false);
	obs_data_set_default_bool(settings, S_SMOOTH, true);
	obs_data_set_default_bool(settings, S_HIDE_OFF_AIR, false);
	obs_data_set_default_string(settings, S_IDLE_STOPPED, IDLE_HIDE);
	obs_data_set_default_string(settings, S_IDLE_ENDED, IDLE_FULL);
	obs_data_set_default_string(settings, S_IDLE_NONE, IDLE_HIDE);
}

/* ------------------------------------------------------------------------- */
/* properties */

static bool warp_progress_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);

	const char *shape = obs_data_get_string(settings, S_SHAPE);
	bool ring = strcmp(shape, SHAPE_RING) == 0;
	bool text = strcmp(obs_data_get_string(settings, S_TEXT), TEXT_NONE) != 0;
	bool by_state = obs_data_get_bool(settings, S_COLOR_BY_STATE);

	obs_property_set_visible(obs_properties_get(props, S_WIDTH), !ring);
	obs_property_set_visible(obs_properties_get(props, S_HEIGHT), !ring);
	obs_property_set_visible(obs_properties_get(props, S_RADIUS), !ring);
	obs_property_set_visible(obs_properties_get(props, S_REVERSE), !ring);
	obs_property_set_visible(obs_properties_get(props, S_RING_SIZE), ring);
	obs_property_set_visible(obs_properties_get(props, S_RING_THICKNESS), ring);
	obs_property_set_visible(obs_properties_get(props, S_ROUNDED_ENDS), ring);

	obs_property_set_description(obs_properties_get(props, S_REVERSE),
				     obs_module_text(strcmp(shape, SHAPE_VERTICAL) == 0
							     ? "Warp.Progress.Reverse.Vertical"
							     : "Warp.Progress.Reverse"));

	bool horizontal = strcmp(shape, SHAPE_HORIZONTAL) == 0;
	const char *playhead = obs_data_get_string(settings, S_PLAYHEAD);
	bool has_playhead = strcmp(playhead, PLAYHEAD_NONE) != 0;

	obs_property_set_visible(obs_properties_get(props, S_SLANT), horizontal);
	obs_property_set_visible(obs_properties_get(props, S_SEGMENT_GAP), obs_data_get_int(settings, S_SEGMENTS) > 1);
	obs_property_set_visible(obs_properties_get(props, S_SHADOW_COLOR), obs_data_get_bool(settings, S_SHADOW));
	obs_property_set_visible(obs_properties_get(props, S_SHADOW_BLUR), obs_data_get_bool(settings, S_SHADOW));
	obs_property_set_visible(obs_properties_get(props, S_FILL_COLOR2), obs_data_get_bool(settings, S_GRADIENT));
	obs_property_set_visible(obs_properties_get(props, S_BORDER_COLOR),
				 obs_data_get_int(settings, S_BORDER_WIDTH) > 0);
	obs_property_set_visible(obs_properties_get(props, S_PLAYHEAD_WIDTH), has_playhead);
	obs_property_set_visible(obs_properties_get(props, S_PLAYHEAD_COLOR), has_playhead);
	obs_property_set_description(obs_properties_get(props, S_PLAYHEAD_WIDTH),
				     obs_module_text(strcmp(playhead, PLAYHEAD_KNOB) == 0
							     ? "Warp.Progress.KnobSize"
							     : "Warp.Progress.PlayheadWidth"));

	obs_property_set_visible(obs_properties_get(props, S_PAUSED_COLOR), by_state);
	obs_property_set_visible(obs_properties_get(props, S_SLOW_COLOR), by_state);
	obs_property_set_visible(obs_properties_get(props, S_FAST_COLOR), by_state);
	obs_property_set_visible(obs_properties_get(props, S_ENDED_COLOR), by_state);

	obs_property_set_visible(obs_properties_get(props, S_TEXT_POSITION), text);
	obs_property_set_visible(obs_properties_get(props, S_TEXT_CHIP), text);
	obs_property_set_visible(obs_properties_get(props, S_CHIP_COLOR),
				 text && obs_data_get_bool(settings, S_TEXT_CHIP));
	obs_property_set_visible(obs_properties_get(props, S_TEXT_FONT), text);
	obs_property_set_visible(obs_properties_get(props, S_TEXT_COLOR), text);
	obs_property_set_visible(obs_properties_get(props, S_TEXT_OUTLINE), text);
	obs_property_set_visible(obs_properties_get(props, S_TEXT_SHADOW), text);
	obs_property_set_visible(obs_properties_get(props, S_REAL_TIME), text);

	return true;
}

/* Picking a style fills in the settings it covers, once: the properties
 * call this every time they are opened as well, when it changes nothing. */
static bool warp_progress_style_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	const char *style = obs_data_get_string(settings, S_STYLE);

	if (strcmp(style, obs_data_get_string(settings, S_STYLE_APPLIED)) != 0) {
		warp_progress_apply_style(settings, style, false);
		obs_data_set_string(settings, S_STYLE_APPLIED, style);
	}

	return warp_progress_modified(props, prop, settings);
}

static void warp_progress_add_idle(obs_properties_t *props, const char *key, const char *text, bool can_be_full)
{
	obs_property_t *list = obs_properties_add_list(props, key, obs_module_text(text), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);

	obs_property_list_add_string(list, obs_module_text("Warp.Progress.Idle.Empty"), IDLE_EMPTY);
	if (can_be_full) {
		obs_property_list_add_string(list, obs_module_text("Warp.Progress.Idle.Full"), IDLE_FULL);
		obs_property_list_add_string(list, obs_module_text("Warp.Progress.Idle.Last"), IDLE_LAST);
	}
	obs_property_list_add_string(list, obs_module_text("Warp.Progress.Idle.Hide"), IDLE_HIDE);
}

static obs_properties_t *warp_progress_properties(void *data)
{
	struct warp_progress_source *s = data;
	obs_properties_t *props = obs_properties_create();
	obs_data_t *settings = s ? obs_source_get_settings(s->source) : NULL;
	obs_properties_t *group;
	obs_property_t *p;

	warp_overlay_add_target_property(props, settings);
	obs_data_release(settings);

	p = obs_properties_add_list(props, S_STYLE, obs_module_text("Warp.Overlay.Style"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Warp.Progress.Style.Broadcast"), STYLE_BROADCAST);
	obs_property_list_add_string(p, obs_module_text("Warp.Progress.Style.Segmented"), STYLE_SEGMENTED);
	obs_property_list_add_string(p, obs_module_text("Warp.Progress.Style.Sport"), STYLE_SPORT);
	obs_property_list_add_string(p, obs_module_text("Warp.Progress.Style.Minimal"), STYLE_MINIMAL);
	obs_property_list_add_string(p, obs_module_text("Warp.Progress.Style.Flat"), STYLE_FLAT);
	obs_property_set_long_description(p, obs_module_text("Warp.Overlay.Style.Desc"));
	obs_property_set_modified_callback(p, warp_progress_style_modified);

	/* shape */
	group = obs_properties_create();

	p = obs_properties_add_list(group, S_SHAPE, obs_module_text("Warp.Progress.Shape"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Warp.Progress.Shape.Horizontal"), SHAPE_HORIZONTAL);
	obs_property_list_add_string(p, obs_module_text("Warp.Progress.Shape.Vertical"), SHAPE_VERTICAL);
	obs_property_list_add_string(p, obs_module_text("Warp.Progress.Shape.Ring"), SHAPE_RING);
	obs_property_set_modified_callback(p, warp_progress_modified);

	obs_properties_add_bool(group, S_REVERSE, obs_module_text("Warp.Progress.Reverse"));

	p = obs_properties_add_int(group, S_WIDTH, obs_module_text("Warp.Progress.Width"), 1, 8192, 1);
	obs_property_int_set_suffix(p, " px");
	p = obs_properties_add_int(group, S_HEIGHT, obs_module_text("Warp.Progress.Height"), 1, 8192, 1);
	obs_property_int_set_suffix(p, " px");
	p = obs_properties_add_int(group, S_RING_SIZE, obs_module_text("Warp.Progress.RingSize"), 8, 4096, 1);
	obs_property_int_set_suffix(p, " px");
	p = obs_properties_add_int(group, S_RING_THICKNESS, obs_module_text("Warp.Progress.RingThickness"), 1, 2048, 1);
	obs_property_int_set_suffix(p, " px");
	p = obs_properties_add_int(group, S_RADIUS, obs_module_text("Warp.Progress.Radius"), 0, 4096, 1);
	obs_property_int_set_suffix(p, " px");
	obs_properties_add_bool(group, S_ROUNDED_ENDS, obs_module_text("Warp.Progress.RoundedEnds"));
	p = obs_properties_add_int(group, S_SLANT, obs_module_text("Warp.Progress.Slant"), 0, 512, 1);
	obs_property_int_set_suffix(p, " px");
	p = obs_properties_add_int(group, S_SEGMENTS, obs_module_text("Warp.Progress.Segments"), 0, 200, 1);
	obs_property_set_long_description(p, obs_module_text("Warp.Progress.Segments.Desc"));
	obs_property_set_modified_callback(p, warp_progress_modified);
	p = obs_properties_add_int(group, S_SEGMENT_GAP, obs_module_text("Warp.Progress.SegmentGap"), 1, 64, 1);
	obs_property_int_set_suffix(p, " px");

	obs_properties_add_group(props, "shape_group", obs_module_text("Warp.Progress.Group.Shape"), OBS_GROUP_NORMAL,
				 group);

	/* colors */
	group = obs_properties_create();

	obs_properties_add_color_alpha(group, S_FILL_COLOR, obs_module_text("Warp.Progress.FillColor"));
	p = obs_properties_add_bool(group, S_GRADIENT, obs_module_text("Warp.Progress.Gradient"));
	obs_property_set_modified_callback(p, warp_progress_modified);
	obs_properties_add_color_alpha(group, S_FILL_COLOR2, obs_module_text("Warp.Progress.FillColor2"));
	obs_properties_add_color_alpha(group, S_TRACK_COLOR, obs_module_text("Warp.Progress.TrackColor"));

	p = obs_properties_add_int(group, S_BORDER_WIDTH, obs_module_text("Warp.Progress.BorderWidth"), 0, 256, 1);
	obs_property_int_set_suffix(p, " px");
	obs_property_set_modified_callback(p, warp_progress_modified);
	obs_properties_add_color_alpha(group, S_BORDER_COLOR, obs_module_text("Warp.Progress.BorderColor"));

	p = obs_properties_add_int_slider(group, S_SHEEN, obs_module_text("Warp.Progress.Sheen"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	p = obs_properties_add_int(group, S_GLOW, obs_module_text("Warp.Progress.Glow"), 0, 128, 1);
	obs_property_int_set_suffix(p, " px");
	obs_property_set_long_description(p, obs_module_text("Warp.Progress.Glow.Desc"));
	p = obs_properties_add_bool(group, S_SHADOW, obs_module_text("Warp.Progress.Shadow"));
	obs_property_set_modified_callback(p, warp_progress_modified);
	obs_properties_add_color_alpha(group, S_SHADOW_COLOR, obs_module_text("Warp.Progress.ShadowColor"));
	p = obs_properties_add_int(group, S_SHADOW_BLUR, obs_module_text("Warp.Progress.ShadowBlur"), 0, 128, 1);
	obs_property_int_set_suffix(p, " px");

	p = obs_properties_add_list(group, S_PLAYHEAD, obs_module_text("Warp.Progress.Playhead"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Warp.Progress.Playhead.None"), PLAYHEAD_NONE);
	obs_property_list_add_string(p, obs_module_text("Warp.Progress.Playhead.Knob"), PLAYHEAD_KNOB);
	obs_property_list_add_string(p, obs_module_text("Warp.Progress.Playhead.Line"), PLAYHEAD_LINE);
	obs_property_set_modified_callback(p, warp_progress_modified);
	p = obs_properties_add_int(group, S_PLAYHEAD_WIDTH, obs_module_text("Warp.Progress.PlayheadWidth"), 1, 256, 1);
	obs_property_int_set_suffix(p, " px");
	obs_properties_add_color_alpha(group, S_PLAYHEAD_COLOR, obs_module_text("Warp.Progress.PlayheadColor"));

	p = obs_properties_add_bool(group, S_COLOR_BY_STATE, obs_module_text("Warp.Progress.ColorByState"));
	obs_property_set_long_description(p, obs_module_text("Warp.Progress.ColorByState.Desc"));
	obs_property_set_modified_callback(p, warp_progress_modified);
	obs_properties_add_color_alpha(group, S_PAUSED_COLOR, obs_module_text("Warp.Progress.PausedColor"));
	obs_properties_add_color_alpha(group, S_SLOW_COLOR, obs_module_text("Warp.Progress.SlowColor"));
	obs_properties_add_color_alpha(group, S_FAST_COLOR, obs_module_text("Warp.Progress.FastColor"));
	obs_properties_add_color_alpha(group, S_ENDED_COLOR, obs_module_text("Warp.Progress.EndedColor"));

	obs_properties_add_group(props, "color_group", obs_module_text("Warp.Progress.Group.Colors"), OBS_GROUP_NORMAL,
				 group);

	/* time */
	group = obs_properties_create();

	p = obs_properties_add_list(group, S_TEXT, obs_module_text("Warp.Progress.Text"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Warp.Progress.Text.None"), TEXT_NONE);
	obs_property_list_add_string(p, obs_module_text("Warp.Progress.Text.Elapsed"), TEXT_ELAPSED);
	obs_property_list_add_string(p, obs_module_text("Warp.Progress.Text.Remaining"), TEXT_REMAINING);
	obs_property_list_add_string(p, obs_module_text("Warp.Progress.Text.ElapsedTotal"), TEXT_ELAPSED_TOTAL);
	obs_property_list_add_string(p, obs_module_text("Warp.Progress.Text.RemainingTotal"), TEXT_REMAINING_TOTAL);
	obs_property_list_add_string(p, obs_module_text("Warp.Progress.Text.Percent"), TEXT_PERCENT);
	obs_property_set_modified_callback(p, warp_progress_modified);

	p = obs_properties_add_list(group, S_TEXT_POSITION, obs_module_text("Warp.Progress.TextPosition"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Warp.Progress.TextPosition.Follow"), POS_FOLLOW);
	obs_property_list_add_string(p, obs_module_text("Warp.Progress.TextPosition.Above"), POS_ABOVE);
	obs_property_list_add_string(p, obs_module_text("Warp.Progress.TextPosition.Inside"), POS_INSIDE);
	obs_property_list_add_string(p, obs_module_text("Warp.Progress.TextPosition.Below"), POS_BELOW);

	p = obs_properties_add_bool(group, S_TEXT_CHIP, obs_module_text("Warp.Progress.TextChip"));
	obs_property_set_modified_callback(p, warp_progress_modified);
	obs_properties_add_color_alpha(group, S_CHIP_COLOR, obs_module_text("Warp.Progress.ChipColor"));
	obs_properties_add_font(group, S_TEXT_FONT, obs_module_text("Warp.Overlay.Font"));
	obs_properties_add_color_alpha(group, S_TEXT_COLOR, obs_module_text("Warp.Overlay.TextColor"));
	obs_properties_add_bool(group, S_TEXT_OUTLINE, obs_module_text("Warp.Overlay.TextOutline"));
	obs_properties_add_bool(group, S_TEXT_SHADOW, obs_module_text("Warp.Overlay.TextShadow"));
	p = obs_properties_add_bool(group, S_REAL_TIME, obs_module_text("Warp.Progress.RealTime"));
	obs_property_set_long_description(p, obs_module_text("Warp.Progress.RealTime.Desc"));

	obs_properties_add_group(props, "text_group", obs_module_text("Warp.Progress.Group.Text"), OBS_GROUP_NORMAL,
				 group);

	/* behavior */
	group = obs_properties_create();

	p = obs_properties_add_bool(group, S_SMOOTH, obs_module_text("Warp.Progress.Smooth"));
	obs_property_set_long_description(p, obs_module_text("Warp.Progress.Smooth.Desc"));
	p = obs_properties_add_bool(group, S_HIDE_OFF_AIR, obs_module_text("Warp.Overlay.HideOffAir"));
	obs_property_set_long_description(p, obs_module_text("Warp.Overlay.HideOffAir.Desc"));
	warp_progress_add_idle(group, S_IDLE_STOPPED, "Warp.Progress.IdleStopped", true);
	warp_progress_add_idle(group, S_IDLE_ENDED, "Warp.Progress.IdleEnded", true);
	warp_progress_add_idle(group, S_IDLE_NONE, "Warp.Progress.IdleNone", false);

	obs_properties_add_group(props, "behavior_group", obs_module_text("Warp.Progress.Group.Behavior"),
				 OBS_GROUP_NORMAL, group);

	return props;
}

/* ------------------------------------------------------------------------- */
/* following the target */

static bool warp_progress_has_text(const struct warp_progress_source *s)
{
	return s->text_mode != WARP_PROGRESS_TEXT_NONE;
}

/* the size of the bar or ring itself, leaving the text out */
static void warp_progress_bar_size(const struct warp_progress_source *s, uint32_t *cx, uint32_t *cy)
{
	if (s->shape == WARP_PROGRESS_RING) {
		*cx = *cy = s->ring_size;
	} else {
		*cx = s->width;
		*cy = s->height;
	}
}

/* The room kept all round the bar for what spills off it: its shadow, its
 * glow, and a knob bigger than the bar is thick. It is part of the source,
 * so nothing is cut off when the source is drawn into a texture. */
static uint32_t warp_progress_pad(const struct warp_progress_source *s)
{
	uint32_t cx, cy;
	float pad = 0.0f;

	warp_progress_bar_size(s, &cx, &cy);

	float thickness = s->shape == WARP_PROGRESS_RING ? (float)s->ring_thickness : (float)cy;

	if (s->glow)
		pad = fmaxf(pad, (float)s->glow * 1.6f + 2.0f);
	if (s->shadow)
		pad = fmaxf(pad, (float)s->shadow_blur + (float)s->shadow_blur * 0.25f + 2.0f);
	if (s->playhead > 1.5f)
		pad = fmaxf(pad, (float)s->playhead_width * 0.5f - thickness * 0.5f + 5.0f);

	return (uint32_t)ceilf(pad);
}

/* how the time is laid out: its height, and the chip round it */
struct warp_progress_text_box {
	float tw;
	float th;
	float pad_x;
	float pad_y;
	/* how far a chip leans, to match a slanted bar */
	float lean;
	/* the whole box, chip and all */
	float w;
	float h;
};

static void warp_progress_text_box(const struct warp_progress_source *s, struct warp_progress_text_box *box)
{
	float font = (float)warp_overlay_font_size(s->text_font);
	float th = (float)warp_overlay_text_height(&s->text);

	box->tw = (float)warp_overlay_text_width(&s->text);
	box->th = th > 0.0f ? th : font * 1.2f;
	box->pad_x = s->text_chip ? roundf(box->th * 0.5f) : 0.0f;
	box->pad_y = s->text_chip ? roundf(box->th * 0.14f) : 0.0f;

	/* a chip leans as far as the bar does over its own height, so the two
	 * are cut at the same angle */
	float bar_h = (float)(s->height ? s->height : 1);
	float h = box->th + box->pad_y * 2.0f;

	box->lean = s->text_chip && s->shape == WARP_PROGRESS_HORIZONTAL && s->slant
			    ? roundf(fminf((float)s->slant / bar_h * h, h * 0.5f))
			    : 0.0f;
	box->w = box->tw + box->pad_x * 2.0f + box->lean;
	box->h = box->th + box->pad_y * 2.0f;
}

/* Where everything goes in the source: the bar, with room round it for what
 * spills off it, and the time above or below it - set close to the bar, the
 * way a caption sits on what it captions, leaving just enough room for a knob
 * standing proud of the bar. */
struct warp_progress_layout {
	float pad;
	float bar_x;
	float bar_y;
	float text_y;
	float w;
	float h;
	struct warp_progress_text_box box;
};

static void warp_progress_layout(const struct warp_progress_source *s, struct warp_progress_layout *l)
{
	uint32_t cx, cy;

	warp_progress_bar_size(s, &cx, &cy);
	warp_progress_text_box(s, &l->box);

	float pad = (float)warp_progress_pad(s);
	bool band = warp_progress_has_text(s) && s->text_pos != WARP_PROGRESS_POS_INSIDE;
	float thickness = s->shape == WARP_PROGRESS_RING ? (float)s->ring_thickness : (float)cy;
	float overhang = s->playhead > 1.5f ? fmaxf(0.0f, (float)s->playhead_width * 0.5f - thickness * 0.5f) : 0.0f;
	float gap = roundf(fmaxf(6.0f, l->box.th * 0.3f)) + overhang;

	l->pad = pad;
	l->bar_x = pad;
	l->bar_y = pad;
	l->text_y = 0.0f;
	l->w = (float)cx + pad * 2.0f;
	l->h = (float)cy + pad * 2.0f;

	if (!band)
		return;

	if (s->text_pos == WARP_PROGRESS_POS_BELOW) {
		l->text_y = pad + (float)cy + gap;
		l->h = fmaxf(l->h, l->text_y + l->box.h);
	} else {
		l->bar_y = fmaxf(pad, l->box.h + gap);
		l->h = l->bar_y + (float)cy + pad;
	}

	l->bar_y = roundf(l->bar_y);
	l->text_y = roundf(l->text_y);
}

static uint32_t warp_progress_width(void *data)
{
	struct warp_progress_layout l;

	warp_progress_layout(data, &l);
	return (uint32_t)ceilf(l.w);
}

static uint32_t warp_progress_height(void *data)
{
	struct warp_progress_layout l;

	warp_progress_layout(data, &l);
	return (uint32_t)ceilf(l.h);
}

static void warp_progress_format_time(char *out, size_t size, double ms, bool negative)
{
	long long total = (long long)(ms / 1000.0 + (negative ? 0.999 : 0.0));
	long long h, m, sec;

	if (total < 0)
		total = 0;

	h = total / 3600;
	m = (total / 60) % 60;
	sec = total % 60;

	if (h)
		snprintf(out, size, "%s%lld:%02lld:%02lld", negative ? "-" : "", h, m, sec);
	else
		snprintf(out, size, "%s%lld:%02lld", negative ? "-" : "", m, sec);
}

static void warp_progress_make_text(struct warp_progress_source *s, char *out, size_t size)
{
	const struct warp_overlay_status *st = &s->status;
	double duration = st->duration_ms > 0 ? (double)st->duration_ms : 0.0;
	double elapsed = s->fraction * duration;
	double scale = 1.0;
	char a[32], b[32];

	/* the time it takes on the clock, at the speed the replay is going */
	if (s->real_time && st->speed > 0)
		scale = 100.0 / st->speed;

	if (s->text_mode == WARP_PROGRESS_TEXT_PERCENT) {
		snprintf(out, size, "%d%%", (int)floorf(s->fraction * 100.0f + 0.5f));
		return;
	}

	if (duration <= 0.0) {
		*out = 0;
		return;
	}

	switch (s->text_mode) {
	case WARP_PROGRESS_TEXT_ELAPSED:
		warp_progress_format_time(out, size, elapsed * scale, false);
		break;
	case WARP_PROGRESS_TEXT_REMAINING:
		warp_progress_format_time(out, size, (duration - elapsed) * scale, true);
		break;
	case WARP_PROGRESS_TEXT_ELAPSED_TOTAL:
		warp_progress_format_time(a, sizeof(a), elapsed * scale, false);
		warp_progress_format_time(b, sizeof(b), duration * scale, false);
		snprintf(out, size, "%s / %s", a, b);
		break;
	case WARP_PROGRESS_TEXT_REMAINING_TOTAL:
		warp_progress_format_time(a, sizeof(a), (duration - elapsed) * scale, true);
		warp_progress_format_time(b, sizeof(b), duration * scale, false);
		snprintf(out, size, "%s / %s", a, b);
		break;
	default:
		*out = 0;
		break;
	}
}

/* the fill color the target's state calls for */
static void warp_progress_state_colors(const struct warp_progress_source *s, uint32_t *fill, uint32_t *fill2)
{
	const struct warp_overlay_status *st = &s->status;

	*fill = s->fill_color;
	*fill2 = s->gradient ? s->fill_color2 : s->fill_color;

	if (!s->color_by_state || !st->has_target)
		return;

	uint32_t c = 0;
	bool by_state = true;

	if (st->state == OBS_MEDIA_STATE_ENDED)
		c = s->ended_color;
	else if (st->state == OBS_MEDIA_STATE_PAUSED)
		c = s->paused_color;
	else if (st->speed < 100)
		c = s->slow_color;
	else if (st->speed > 100)
		c = s->fast_color;
	else
		by_state = false;

	if (by_state)
		*fill = *fill2 = c;
}

static void warp_progress_approach_color(struct vec4 *v, uint32_t color, float seconds, bool snap)
{
	struct vec4 target;

	/* faded in sRGB, as set; made linear only as it is drawn */
	vec4_from_rgba(&target, color);

	if (snap) {
		*v = target;
		return;
	}

	float rate = 1.0f / COLOR_SECONDS;

	v->x = warp_overlay_approach(v->x, target.x, rate, seconds);
	v->y = warp_overlay_approach(v->y, target.y, rate, seconds);
	v->z = warp_overlay_approach(v->z, target.z, rate, seconds);
	v->w = warp_overlay_approach(v->w, target.w, rate, seconds);
}

static float smoothstepf(float t)
{
	t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	return t * t * (3.0f - 2.0f * t);
}

static void warp_progress_tick(void *data, float seconds)
{
	struct warp_progress_source *s = data;
	struct warp_overlay_event events[WARP_OVERLAY_MAX_EVENTS];
	size_t count = warp_overlay_target_tick(&s->target, seconds, &s->status, events);
	const struct warp_overlay_status *st = &s->status;
	bool snap = false;

	for (size_t i = 0; i < count; i++) {
		switch (events[i].type) {
		case WARP_OVERLAY_EV_NEXT:
		case WARP_OVERLAY_EV_PREVIOUS:
		case WARP_OVERLAY_EV_FIRST:
		case WARP_OVERLAY_EV_SWITCH:
			/* a new file: the fill goes back from where the old
			 * one left it over the transition, or at once when
			 * there is none */
			snap = true;
			if (events[i].transition_ms > 0) {
				s->animating = true;
				s->anim_from = s->fraction;
				s->anim_elapsed = 0.0f;
				s->anim_seconds = (float)events[i].transition_ms / 1000.0f;
			} else {
				s->animating = false;
			}
			break;
		case WARP_OVERLAY_EV_LOADED:
		case WARP_OVERLAY_EV_RESTART:
		case WARP_OVERLAY_EV_STEP:
			snap = true;
			break;
		default:
			break;
		}
	}

	/* where the source is, smoothed between the frames it reports */
	double reported = st->time_ms > 0 ? (double)st->time_ms : 0.0;
	double duration = st->duration_ms > 0 ? (double)st->duration_ms : 0.0;

	if (!s->smooth || snap || st->state != OBS_MEDIA_STATE_PLAYING) {
		s->shown_ms = reported;
	} else {
		double predicted = s->shown_ms + (double)seconds * 1000.0 * st->speed / 100.0;
		double pull = SMOOTH_PULL * seconds;

		if (fabs(reported - predicted) > SMOOTH_SNAP_MS * st->speed / 100.0 + SMOOTH_SNAP_MS)
			s->shown_ms = reported;
		else
			s->shown_ms = predicted + (reported - predicted) * (pull < 1.0 ? pull : 1.0);
	}

	if (duration > 0.0 && s->shown_ms > duration)
		s->shown_ms = duration;
	if (s->shown_ms < 0.0)
		s->shown_ms = 0.0;

	float live = duration > 0.0 ? (float)(s->shown_ms / duration) : 0.0f;

	/* what an idle source shows */
	enum warp_progress_idle idle = WARP_PROGRESS_IDLE_LAST;
	bool is_idle = true;

	if (!st->has_target || st->state == OBS_MEDIA_STATE_NONE || st->state == OBS_MEDIA_STATE_ERROR)
		idle = s->idle_none;
	else if (st->state == OBS_MEDIA_STATE_STOPPED)
		idle = s->idle_stopped;
	else if (st->state == OBS_MEDIA_STATE_ENDED)
		idle = s->idle_ended;
	else
		is_idle = false;

	float fraction = live;

	if (is_idle) {
		s->animating = false;

		if (idle == WARP_PROGRESS_IDLE_EMPTY)
			fraction = 0.0f;
		else if (idle == WARP_PROGRESS_IDLE_FULL)
			fraction = 1.0f;
		else
			fraction = s->fraction;
	} else if (s->animating) {
		s->anim_elapsed += seconds;

		float t = s->anim_seconds > 0.0f ? s->anim_elapsed / s->anim_seconds : 1.0f;

		if (t >= 1.0f)
			s->animating = false;
		else
			fraction = s->anim_from + (live - s->anim_from) * smoothstepf(t);
	}

	s->fraction = fraction < 0.0f ? 0.0f : fraction > 1.0f ? 1.0f : fraction;

	/* shown, or faded away */
	bool visible = !(is_idle && idle == WARP_PROGRESS_IDLE_HIDE);

	if (s->hide_off_air && !st->on_program)
		visible = false;

	s->alpha = warp_overlay_approach(s->alpha, visible ? 1.0f : 0.0f, 1.0f / FADE_SECONDS, seconds);

	/* the fill's colors */
	uint32_t fill, fill2;

	warp_progress_state_colors(s, &fill, &fill2);
	warp_progress_approach_color(&s->fill, fill, seconds, !s->colors_set);
	warp_progress_approach_color(&s->fill2, fill2, seconds, !s->colors_set);
	s->colors_set = true;

	/* and the time beside it */
	if (warp_progress_has_text(s)) {
		char text[96];

		warp_progress_make_text(s, text, sizeof(text));
		warp_overlay_text_set(&s->text, text, s->text_font, s->text_color, s->text_outline, s->text_shadow);
	} else if (s->text.text && *s->text.text) {
		warp_overlay_text_set(&s->text, "", s->text_font, s->text_color, s->text_outline, s->text_shadow);
	}
}

/* ------------------------------------------------------------------------- */
/* drawing */

static void warp_progress_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);

	struct warp_progress_source *s = data;
	gs_effect_t *effect = warp_overlay_effect();

	if (!effect || s->alpha <= 0.0f)
		return;

	uint32_t cx, cy;

	warp_progress_bar_size(s, &cx, &cy);

	if (!cx || !cy)
		return;

	struct warp_progress_layout l;

	warp_progress_layout(s, &l);

	float pad = l.pad;
	float bar_x = l.bar_x;
	float bar_y = l.bar_y;
	bool ring = s->shape == WARP_PROGRESS_RING;

	struct warp_overlay_bar bar = {0};

	bar.w = (float)cx;
	bar.h = (float)cy;
	bar.pad = pad;
	bar.progress = s->fraction;
	bar.border = (float)s->border_width;
	bar.ring_thickness = (float)s->ring_thickness;
	bar.slant = s->shape == WARP_PROGRESS_HORIZONTAL ? (float)s->slant * (s->reverse ? -1.0f : 1.0f) : 0.0f;
	bar.segments = (float)s->segments;
	bar.segment_gap = (float)s->segment_gap;
	bar.sheen = s->sheen;
	bar.glow = (float)s->glow;
	bar.shadow = s->shadow;
	bar.shadow_blur = (float)s->shadow_blur;
	bar.shadow_y = roundf((float)s->shadow_blur * 0.25f);
	bar.knob = s->playhead;
	bar.head_width = (float)s->playhead_width;
	bar.fill = s->fill;
	bar.fill2 = s->fill2;
	bar.opacity = s->alpha;
	warp_overlay_rgba(&bar.track, s->track_color);
	warp_overlay_rgba(&bar.border_color, s->border_color);
	warp_overlay_rgba(&bar.head, s->playhead_color);
	warp_overlay_rgba(&bar.shadow_color, s->shadow_color);

	if (ring) {
		bar.mode = 4.0f;
		bar.radius = s->rounded_ends ? 1.0f : 0.0f;
	} else if (s->shape == WARP_PROGRESS_VERTICAL) {
		bar.mode = s->reverse ? 3.0f : 2.0f;
		bar.radius = (float)s->radius;
	} else {
		bar.mode = s->reverse ? 1.0f : 0.0f;
		bar.radius = (float)s->radius;
	}

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(gs_get_linear_srgb());

	warp_overlay_draw_bar(effect, &bar, bar_x, bar_y);

	/* the time, in its chip when it has one */
	struct warp_progress_text_box box = l.box;

	if (!warp_progress_has_text(s) || box.tw <= 0.0f) {
		gs_enable_framebuffer_srgb(previous);
		return;
	}

	float total_w = l.w;
	float scale = 1.0f;
	float avail_w = s->text_pos == WARP_PROGRESS_POS_INSIDE ? (float)cx : total_w;
	float avail_h = s->text_pos == WARP_PROGRESS_POS_INSIDE ? (float)cy : box.h;

	if (s->text_pos == WARP_PROGRESS_POS_INSIDE && ring)
		avail_w = (float)cx - 2.0f * (float)s->ring_thickness;
	if (box.w > avail_w && avail_w > 0.0f)
		scale = avail_w / box.w;
	if (box.h * scale > avail_h && avail_h > 0.0f)
		scale = avail_h / box.h;

	float w = box.w * scale;
	float h = box.h * scale;
	float x, y;

	if (s->text_pos == WARP_PROGRESS_POS_FOLLOW) {
		/* centred over the leading edge, kept inside the source */
		float lean = (float)s->slant;
		float run = (float)cx - lean;
		float head = s->reverse ? (float)cx - lean * 0.5f - s->fraction * run : lean * 0.5f + s->fraction * run;

		x = bar_x + head - w * 0.5f;
		x = fminf(fmaxf(x, 0.0f), total_w - w);
		y = l.text_y;
	} else if (s->text_pos == WARP_PROGRESS_POS_INSIDE) {
		x = (total_w - w) * 0.5f;
		y = bar_y + ((float)cy - h) * 0.5f;
	} else {
		x = (total_w - w) * 0.5f;
		y = l.text_y;
	}

	x = roundf(x);
	y = roundf(y);

	if (s->text_chip) {
		struct warp_overlay_bar chip = {0};

		chip.w = w;
		chip.h = h;
		/* rounded like the bar is: a pill on a rounded bar, square on a
		 * square one */
		chip.radius = s->radius > 0 || s->shape != WARP_PROGRESS_HORIZONTAL ? h * 0.5f : 0.0f;
		chip.slant = box.lean * scale * (s->reverse ? -1.0f : 1.0f);
		chip.opacity = s->alpha;
		warp_overlay_rgba(&chip.track, s->chip_color);
		chip.shadow = s->shadow;
		chip.shadow_blur = 6.0f;
		chip.shadow_y = 2.0f;
		chip.pad = chip.shadow ? 8.0f : 0.0f;
		warp_overlay_rgba(&chip.shadow_color, s->shadow_color);
		warp_overlay_draw_bar(effect, &chip, x, y);
	}

	gs_enable_framebuffer_srgb(previous);

	warp_overlay_text_render(&s->text, x + (box.pad_x + box.lean * 0.5f) * scale, y + box.pad_y * scale, scale,
				 s->alpha);
}

/* ------------------------------------------------------------------------- */

static void *warp_progress_create(obs_data_t *settings, obs_source_t *source)
{
	struct warp_progress_source *s = bzalloc(sizeof(*s));

	s->source = source;

	if (!warp_overlay_target_init(&s->target, source)) {
		bfree(s);
		return NULL;
	}

	warp_progress_update(s, settings);
	return s;
}

static void warp_progress_destroy(void *data)
{
	struct warp_progress_source *s = data;

	warp_overlay_target_free(&s->target);
	warp_overlay_text_free(&s->text);
	obs_data_release(s->text_font);
	bfree(s);
}

struct obs_source_info warp_progress_source_info = {
	.id = WARP_PROGRESS_SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB,
	.icon_type = OBS_ICON_TYPE_MEDIA,
	.get_name = warp_progress_getname,
	.create = warp_progress_create,
	.destroy = warp_progress_destroy,
	.update = warp_progress_update,
	.get_defaults = warp_progress_defaults,
	.get_properties = warp_progress_properties,
	.get_width = warp_progress_width,
	.get_height = warp_progress_height,
	.video_tick = warp_progress_tick,
	.video_render = warp_progress_render,
};
