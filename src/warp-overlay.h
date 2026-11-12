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

#include <obs-module.h>
#include <graphics/vec4.h>
#include <util/threading.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* What the Warp Progress Bar and the Warp Status Icon have in common: they are
 * drawn over a show to say what a Warp Media or Warp Playlist source is doing,
 * so both follow one of them - the target - and read its playback off it every
 * frame, along with the things it says happened (a speed change, a frame step,
 * a file going up) as it says them.
 *
 * The target is picked in the properties and kept by its UUID, so renaming it
 * does not lose it. It is looked up again every so often rather than held, so
 * one that is deleted, or not there yet while a scene collection loads, is
 * picked up as soon as it is. */

/* the setting the target's UUID is kept under, in both sources */
#define WARP_OVERLAY_TARGET "target"

/* the ids the two sources are registered with */
#define WARP_PROGRESS_SOURCE_ID "warp_progress_source"
#define WARP_STATUS_ICON_SOURCE_ID "warp_status_icon_source"

/* something the target said happened */
enum warp_overlay_event_type {
	WARP_OVERLAY_EV_SPEED,
	WARP_OVERLAY_EV_STEP,
	WARP_OVERLAY_EV_PLAY,
	WARP_OVERLAY_EV_PAUSE,
	WARP_OVERLAY_EV_RESTART,
	WARP_OVERLAY_EV_LOADED,
	WARP_OVERLAY_EV_NEXT,
	WARP_OVERLAY_EV_PREVIOUS,
	WARP_OVERLAY_EV_FIRST,
	/* a playlist file went up by itself, or as the playlist started */
	WARP_OVERLAY_EV_SWITCH,
};

struct warp_overlay_event {
	enum warp_overlay_event_type type;
	/* the new speed, or the frames stepped (negative going back) */
	int value;
	/* for a switch: how long its transition runs, in milliseconds */
	int transition_ms;
};

#define WARP_OVERLAY_MAX_EVENTS 32

/* where the target stands, read off it on the graphics thread every tick */
struct warp_overlay_status {
	bool has_target;
	bool is_playlist;
	/* the target is in the program output */
	bool on_program;
	enum obs_media_state state;
	int64_t time_ms;
	int64_t duration_ms;
	/* percent */
	int speed;
};

struct warp_overlay_target {
	obs_source_t *owner;

	/* the UUID the settings name; graphics thread */
	char *wanted;
	obs_weak_source_t *listening_to;
	float resolve_delay;

	/* events as the signals bring them in, from whichever thread emits
	 * them, for the next tick to take */
	pthread_mutex_t mutex;
	struct warp_overlay_event events[WARP_OVERLAY_MAX_EVENTS];
	size_t num_events;
};

bool warp_overlay_target_init(struct warp_overlay_target *t, obs_source_t *owner);
void warp_overlay_target_free(struct warp_overlay_target *t);

/* Follows the source with this UUID from the next tick; NULL or "" for none.
 * Called from the source's update. */
void warp_overlay_target_set(struct warp_overlay_target *t, const char *uuid);

/* Reads where the target stands into 'status', and hands over what it has said
 * happened since the last call, in order, returning how many there were.
 * Graphics thread. */
size_t warp_overlay_target_tick(struct warp_overlay_target *t, float seconds, struct warp_overlay_status *status,
				struct warp_overlay_event *events);

/* The list of Warp Media and Warp Playlist sources to pick a target from, by
 * name, with their UUID as the value. */
obs_property_t *warp_overlay_add_target_property(obs_properties_t *props, obs_data_t *settings);

/* ------------------------------------------------------------------------- */
/* text
 *
 * The time on a bar and the speed beside an icon are drawn by one of OBS's own
 * text sources, made privately and drawn as part of ours, so they take every
 * font the system has. Which one depends on the platform: GDI+ on Windows,
 * FreeType everywhere else. */

struct warp_overlay_text {
	obs_source_t *source;
	char *text;
	uint32_t color;
	obs_data_t *font;
	bool outline;
	bool shadow;
	/* what it is drawn into first, so it can be faded as one */
	gs_texrender_t *texrender;
};

/* Updates the text and how it looks, making the text source the first time;
 * only touches it when something changed. Any thread that may update a
 * source. */
void warp_overlay_text_set(struct warp_overlay_text *t, const char *text, obs_data_t *font, uint32_t color,
			   bool outline, bool shadow);
void warp_overlay_text_free(struct warp_overlay_text *t);
uint32_t warp_overlay_text_width(const struct warp_overlay_text *t);
uint32_t warp_overlay_text_height(const struct warp_overlay_text *t);
/* Draws it with its top left at (x, y), scaled by 'scale' and faded to
 * 'opacity'. Graphics thread, inside a render. */
void warp_overlay_text_render(struct warp_overlay_text *t, float x, float y, float scale, float opacity);

/* the size of the font in a font property, in pixels */
int warp_overlay_font_size(obs_data_t *font);

/* the font a text setting starts out as */
void warp_overlay_default_font(obs_data_t *settings, const char *key, int size);

/* ------------------------------------------------------------------------- */
/* drawing */

/* the overlay effect, shared by both sources and made the first time either
 * draws; NULL if it failed to compile. Graphics thread. */
gs_effect_t *warp_overlay_effect(void);
/* lets go of it as the module unloads */
void warp_overlay_effect_free(void);

/* A color setting as the shader wants it: in linear light when the canvas is
 * drawn in it, the way OBS's own color source does. Graphics thread, inside a
 * render, since that is the only place to ask which it is. */
void warp_overlay_color(struct vec4 *out, uint32_t color);
/* the same for a color already taken out of a setting, as it is (sRGB);
 * for colors that are faded from one to another between renders */
void warp_overlay_color_for_render(struct vec4 *out, const struct vec4 *srgb);

/* moves 'value' toward 'target' at 'rate' per second */
float warp_overlay_approach(float value, float target, float rate, float seconds);

#ifdef __cplusplus
}
#endif
