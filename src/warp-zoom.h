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

#ifndef __cplusplus
#include <util/darray.h>
#include <util/threading.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The zoom a Warp source, or the Warp Zoom filter, is framed with, and the
 * presets it can be moved between.
 *
 * A view is a window on the picture: how far in it is zoomed, and where the
 * middle of what is shown sits in the source. The source keeps the size it
 * always had - only its contents are magnified - so nothing downstream of it,
 * from the playlist's transitions to the scene item it sits in, has to know
 * that a zoom is on at all.
 *
 * The window is always kept inside the picture, so the edge of the video stays
 * at the edge of the frame however far it is panned, and zooming out past the
 * whole frame is not offered: 100% is the picture as it is. */

#define WARP_ZOOM_FILTER_ID "warp_zoom_filter"

/* How far the picture may be magnified, and what one press of the zoom hotkeys
 * multiplies by. The step is a multiplier rather than a number of points so
 * that it feels the same at 600% as it does at 150%. */
#define WARP_ZOOM_MIN 1.0f
#define WARP_ZOOM_MAX 8.0f
#define WARP_ZOOM_STEP 1.25f

/* how far one press of the pan hotkeys moves, as a fraction of what is on
 * screen: panning at 400% moves a quarter as far across the file as panning at
 * 100% would, so the picture moves by the same amount either way */
#define WARP_ZOOM_PAN_STEP 0.05f

/* The directions the pan hotkeys and the dock's pad move in, in the order
 * their bindings are numbered: the four straight ones, then the corners. A
 * corner moves a full step on both axes rather than a scaled one, so pushing
 * into it covers more ground than a straight press, the way a joystick held
 * into its corner does. */
#define WARP_ZOOM_NUM_PANS 8

/* How long a move takes, in milliseconds. Recalling a preset eases over the
 * first; the hotkeys and the dock nudge with the second, short enough that
 * held-down presses run into one continuous move rather than stepping. */
#define WARP_ZOOM_GLIDE_MS 400
#define WARP_ZOOM_NUDGE_MS 120
#define WARP_ZOOM_GLIDE_MAX 5000

/* how many presets are reachable through the numbered recall hotkeys, which
 * are registered once and fire whatever preset is in that position */
#define WARP_ZOOM_SLOTS 8

/* Presets are given a hotkey each on top of the numbered ones, so the list
 * cannot be allowed to grow without bound. */
#define WARP_ZOOM_MAX_PRESETS 64

/* How much of a preset's name is carried with a change, which is what the zoom
 * signals say the shot came from. A name is kept whole in the preset itself and
 * cut to this only on its way out, so the signals have a length to be sized
 * for. */
#define WARP_ZOOM_PRESET_NAME_MAX 128

/* Settings the zoom of a source is saved with. The revision counts the edits
 * the presets have had: presets are made in the dock rather than in the
 * properties, so a properties window that was opened before a preset was made
 * would otherwise put the settings back the way they were when it hands them
 * over. Settings carrying an older revision than the source has are taken as
 * the stale copy they are. */
#define WARP_ZOOM_S_PRESETS "zoom_presets"
#define WARP_ZOOM_S_PRESETS_REV "zoom_presets_rev"
#define WARP_ZOOM_S_GLIDE "zoom_glide_ms"
#define WARP_ZOOM_S_NUDGE "zoom_nudge_ms"

/* Fields of a preset in that array. 'fixed' is only ever read back out: it
 * marks the reset position, which the UI offers no way to change. */
#define WARP_ZOOM_P_ID "id"
#define WARP_ZOOM_P_NAME "name"
#define WARP_ZOOM_P_ZOOM "zoom"
#define WARP_ZOOM_P_X "x"
#define WARP_ZOOM_P_Y "y"
#define WARP_ZOOM_P_GLIDE "glide_ms"
#define WARP_ZOOM_P_PATH "path"
#define WARP_ZOOM_P_FIXED "fixed"

/* The route the picture takes on the way to a preset.
 *
 * The window is always kept inside the picture, so how far the middle of a shot
 * may sit from the centre of it depends on how far in it is zoomed: at 100%
 * there is nowhere to go at all, and the room opens up as the zoom comes in.
 *
 * Arc walks the middle of the shot across the picture at a flat rate, which
 * takes no account of that. Early in a move that zooms in, while the picture is
 * still wide, a small step across it is most of the travel the frame allows, so
 * the shot is thrown out to the side of the frame in the first tenth of the
 * move and creeps back to where it belongs as the zoom catches up - and the
 * same in reverse on the way out, where it hangs at the side and snaps back at
 * the last moment. That swing is the motion Warp had before there was a choice.
 *
 * Direct works in the frame instead: the shot holds its place in the frame the
 * whole way across, so it travels evenly and lands without a swing. Presets
 * written before there was a choice are Direct. */
#define WARP_ZOOM_PATH_DIRECT 0
#define WARP_ZOOM_PATH_ARC 1

/* the view a source that keeps one is saved with */
#define WARP_ZOOM_S_ZOOM "zoom"
#define WARP_ZOOM_S_X "zoom_x"
#define WARP_ZOOM_S_Y "zoom_y"

/* Whether changes are staged rather than going straight to air. With it on,
 * anything that would reframe the source - the dock, the hotkeys, the
 * websocket - lines the shot up instead, and it only moves when something
 * takes it. Saved with the source, so a hotkey behaves the way the dock says
 * it will. */
#define WARP_ZOOM_S_CONFIRM "zoom_confirm"

/* Marks a Warp Zoom filter that is driven by the source that made it rather
 * than by an operator: it renders the view it is handed and keeps no presets
 * or hotkeys of its own. The playlist puts one on every file it plays, and the
 * Warp Media source puts one on itself. */
#define WARP_ZOOM_S_DRIVEN "warp_driven"

/* The reset position, which every video starts at. It is a preset like any
 * other as far as recalling it goes, except that it is always there, always
 * first, and cannot be renamed, moved, overwritten or removed: whatever else
 * is set up, there is always one way back to the whole picture. */
#define WARP_ZOOM_DEFAULT_ID "default"

/* Emitted whenever the view a source is framed with changes, so a Warp
 * Detection filter (and any script listening on the source) can react to a
 * zoom the way it reacts to a speed change:
 *
 *   warp_zoom_changed(ptr source, float zoom, float x, float y,
 *                     string change, string preset)
 *
 * 'change' says how the view got there, and 'preset' is the name of the preset
 * that was recalled, empty for the changes that are not one. The view a video
 * starts at is not a change: a playlist moving to its next file resets the zoom
 * without emitting anything, the same way it resets the speed. */
#define WARP_SIGNAL_ZOOM_CHANGED "warp_zoom_changed"
#define WARP_SIGNAL_DECL_ZOOM_CHANGED \
	"void warp_zoom_changed(ptr source, float zoom, float x, float y, string change, string preset)"

/* values of the 'change' field: nudged with the hotkeys or the dock, moved to
 * a preset, put back to the whole picture, or set to a view outright */
#define WARP_ZOOM_CHANGE_MANUAL "manual"
#define WARP_ZOOM_CHANGE_PRESET "preset"
#define WARP_ZOOM_CHANGE_RESET "reset"
#define WARP_ZOOM_CHANGE_SET "set"

/* Emitted as a shot is lined up in confirm mode, and again as it is taken or
 * dropped:
 *
 *   warp_zoom_staged(ptr source, bool staged, float zoom, float x, float y,
 *                    string change, string preset)
 *
 * 'staged' is false when the shot has gone - taken to air, or dropped - and
 * the view is then the one the source is actually framed with. Nothing has
 * moved on screen when this is emitted: a shot going to air is a
 * warp_zoom_changed like any other, which is what a Warp Detection filter
 * reacts to. */
#define WARP_SIGNAL_ZOOM_STAGED "warp_zoom_staged"
#define WARP_SIGNAL_DECL_ZOOM_STAGED \
	"void warp_zoom_staged(ptr source, bool staged, float zoom, float x, float y, string change, string preset)"

/* The procs every zoom-capable source carries, which is how everything outside
 * the source itself drives it: the zoom dock, the obs-websocket requests, a
 * Warp Detection filter recalling a preset, and any script that can reach the
 * source. A Warp Zoom filter carries the same ones, so a camera with the filter
 * on it is driven exactly like a Warp source. */
#define WARP_ZOOM_PROC_SET "warp_zoom_set"
#define WARP_ZOOM_PROC_GET "warp_zoom_get"
#define WARP_ZOOM_PROC_ADJUST "warp_zoom_adjust"
#define WARP_ZOOM_PROC_PAN "warp_zoom_pan"
#define WARP_ZOOM_PROC_RESET "warp_zoom_reset"
#define WARP_ZOOM_PROC_RECALL "warp_zoom_recall"
#define WARP_ZOOM_PROC_SAVE_PRESET "warp_zoom_save_preset"
#define WARP_ZOOM_PROC_UPDATE_PRESET "warp_zoom_update_preset"
#define WARP_ZOOM_PROC_REMOVE_PRESET "warp_zoom_remove_preset"
#define WARP_ZOOM_PROC_MOVE_PRESET "warp_zoom_move_preset"
#define WARP_ZOOM_PROC_PRESETS "warp_zoom_presets"
#define WARP_ZOOM_PROC_CONFIRM "warp_zoom_confirm"
#define WARP_ZOOM_PROC_TAKE "warp_zoom_take"
#define WARP_ZOOM_PROC_DROP "warp_zoom_drop"

/* How far in the picture is zoomed, and where the middle of what is shown sits
 * in it, from 0 to 1 across the whole file. */
struct warp_zoom_view {
	float zoom;
	float x;
	float y;
};

/* A framing worth coming back to. The id is made when the preset is and never
 * changes, so the hotkey a preset is given follows it through a rename. */
struct warp_zoom_preset {
	char *id;
	char *name;
	struct warp_zoom_view view;
	/* how long moving to this preset takes; zero uses the source's own */
	uint32_t glide_ms;
	/* the route the move takes, one of WARP_ZOOM_PATH_* */
	int path;
	obs_hotkey_id hotkey;
};

/* ------------------------------------------------------------------------- */
/* the view itself */

static inline struct warp_zoom_view warp_zoom_default_view(void)
{
	struct warp_zoom_view view = {1.0f, 0.5f, 0.5f};

	return view;
}

/* Puts a view inside what is allowed: a zoom in range, and a window that stays
 * within the picture, which is what keeps the edge of the video at the edge of
 * the frame instead of letting the pan run off into nothing. */
static inline void warp_zoom_clamp(struct warp_zoom_view *view)
{
	float half;

	if (!(view->zoom >= WARP_ZOOM_MIN))
		view->zoom = WARP_ZOOM_MIN;
	else if (view->zoom > WARP_ZOOM_MAX)
		view->zoom = WARP_ZOOM_MAX;

	half = 0.5f / view->zoom;

	if (!(view->x >= half))
		view->x = half;
	else if (view->x > 1.0f - half)
		view->x = 1.0f - half;

	if (!(view->y >= half))
		view->y = half;
	else if (view->y > 1.0f - half)
		view->y = 1.0f - half;
}

static inline bool warp_zoom_view_equal(const struct warp_zoom_view *a, const struct warp_zoom_view *b)
{
	return a->zoom == b->zoom && a->x == b->x && a->y == b->y;
}

static inline bool warp_zoom_view_is_default(const struct warp_zoom_view *view)
{
	return view->zoom <= WARP_ZOOM_MIN;
}

struct warp_zoom_control;

/* The control below is embedded in the sources that own one, so its definition
 * is only of use to them. The dock is C++ and drives sources through the procs
 * like anything else outside them, so it is left out of that half of the
 * header rather than having libobs' C containers dragged into C++. */
#ifndef __cplusplus

/* per-hotkey context for the hotkeys that differ only in what they move */
struct warp_zoom_binding {
	struct warp_zoom_control *ctl;
	int value;
};

/* What a source is framed with: where it is, where it is going, the presets it
 * can be sent to, and the hotkeys that send it there. It is embedded in the
 * source that owns it, and guards itself: everything below is taken under its
 * own mutex, and nothing that reaches into libobs' source graph is done while
 * that is held. */
struct warp_zoom_control {
	obs_source_t *source;
	/* what the hotkeys this control registers are named after, and whether
	 * it registers any: a driven filter is framed by the source that made
	 * it, so it has no hotkeys of its own */
	const char *prefix;
	bool hotkeys;
	/* Whether the view is saved with the source. A filter keeps the framing
	 * it was left with, the way any other filter keeps its settings; the
	 * Warp sources do not, because their framing belongs to the video that
	 * is playing rather than to the source. */
	bool persist_view;

	pthread_mutex_t mutex;

	/* where the view is now, and the move it is partway through */
	struct warp_zoom_view view;
	struct warp_zoom_view from;
	struct warp_zoom_view target;
	float glide_elapsed;
	float glide_len;
	/* the route that move is taking; only a preset asks for anything but
	 * the direct one */
	int glide_path;

	/* Confirm mode, and the shot lined up behind it. Nothing about the
	 * staged shot is on screen: it waits here until it is taken, and is
	 * dropped by anything that means the video it was lined up on has gone. */
	bool confirm;
	bool staged;
	struct warp_zoom_view stage;
	int stage_glide;
	int stage_path;
	const char *stage_change;
	char stage_preset[WARP_ZOOM_PRESET_NAME_MAX];

	uint32_t glide_ms;
	uint32_t nudge_ms;

	DARRAY(struct warp_zoom_preset) presets;

	/* Bumped whenever the view or the presets change, so the dock and the
	 * Warp window can tell there is something new to show without holding
	 * anything of the source's. */
	uint64_t revision;
	/* how many times the presets have been edited, written out with them */
	uint64_t presets_rev;

	obs_hotkey_id in_hotkey;
	obs_hotkey_id out_hotkey;
	obs_hotkey_id reset_hotkey;
	obs_hotkey_id take_hotkey;
	obs_hotkey_id drop_hotkey;
	obs_hotkey_id pan_hotkeys[WARP_ZOOM_NUM_PANS];
	obs_hotkey_id slot_hotkeys[WARP_ZOOM_SLOTS];

	struct warp_zoom_binding pan_bindings[WARP_ZOOM_NUM_PANS];
	struct warp_zoom_binding slot_bindings[WARP_ZOOM_SLOTS];

	/* Hotkeys of presets that have been removed. Registering and dropping a
	 * hotkey takes libobs' hotkey lock, which the hotkey thread already
	 * holds when it calls into this control, so none of it may be done from
	 * under the mutex: it is left here for the sync pass to carry out once
	 * the lock has been dropped. */
	DARRAY(obs_hotkey_id) orphans;

	/* What to say about the change once the lock has been dropped, and
	 * which of the two signals says it: a shot being lined up is not a
	 * change to the picture, so it is reported as a staging of its own. */
	bool signal_armed;
	bool signal_is_stage;
	bool signal_staged;
	struct warp_zoom_view signal_view;
	const char *signal_change;
	char signal_preset[WARP_ZOOM_PRESET_NAME_MAX];
};

/* ------------------------------------------------------------------------- */
/* the control */

void warp_zoom_control_init(struct warp_zoom_control *ctl, obs_source_t *source, const char *prefix, bool hotkeys,
			    bool persist_view);
void warp_zoom_control_free(struct warp_zoom_control *ctl);

void warp_zoom_control_defaults(obs_data_t *settings, bool with_view);
void warp_zoom_control_update(struct warp_zoom_control *ctl, obs_data_t *settings);
void warp_zoom_control_save(struct warp_zoom_control *ctl, obs_data_t *settings);
/* The zoom section of a source's properties, as a group of its own. 'with_view'
 * adds the framing itself, which is only worth showing for a source that keeps
 * it: on a playlist it would be a setting that undoes itself at the next file. */
void warp_zoom_control_properties(obs_properties_t *props, bool with_view);

void warp_zoom_control_register_procs(struct warp_zoom_control *ctl, obs_source_t *source);

/* Moves the view along by 'seconds' and answers with where it stands, whether
 * or not it moved. The owner applies what comes back: the filter renders it,
 * and the sources hand it to the filter of the file that is playing. */
bool warp_zoom_control_tick(struct warp_zoom_control *ctl, float seconds, struct warp_zoom_view *out);

/* where the view stands now, and where it is heading; either may be NULL */
void warp_zoom_control_get(struct warp_zoom_control *ctl, struct warp_zoom_view *view, struct warp_zoom_view *target);
uint64_t warp_zoom_control_revision(struct warp_zoom_control *ctl);

/* Frames the source. 'glide_ms' is how long the move takes, with -1 for the
 * source's own preset glide and 0 for landing on it outright. */
void warp_zoom_control_set(struct warp_zoom_control *ctl, const struct warp_zoom_view *view, int glide_ms,
			   const char *change, const char *preset);
/* multiplies the zoom, keeping the middle of the picture where it is */
void warp_zoom_control_adjust(struct warp_zoom_control *ctl, float factor, int glide_ms);
/* moves the view by a fraction of what is on screen */
void warp_zoom_control_pan(struct warp_zoom_control *ctl, float dx, float dy, int glide_ms);
void warp_zoom_control_reset(struct warp_zoom_control *ctl, int glide_ms);

/* Whether changes are lined up rather than going to air. Turning it off drops
 * whatever was waiting: switching out of confirm mode must not put a shot up
 * that nobody took. */
void warp_zoom_control_set_confirm(struct warp_zoom_control *ctl, bool confirm);
bool warp_zoom_control_confirm(struct warp_zoom_control *ctl);
/* the shot that is lined up, if there is one */
bool warp_zoom_control_staged(struct warp_zoom_control *ctl, struct warp_zoom_view *out);
/* puts the staged shot on screen, easing into it the way a recall does */
bool warp_zoom_control_take(struct warp_zoom_control *ctl);
bool warp_zoom_control_drop(struct warp_zoom_control *ctl);
/* Puts the view back to the whole picture without a word: this is a video
 * starting, not a change someone asked for. */
void warp_zoom_control_restore_default(struct warp_zoom_control *ctl);

bool warp_zoom_control_recall_slot(struct warp_zoom_control *ctl, size_t slot);
/* recalls the preset with that id, falling back to matching it by name */
bool warp_zoom_control_recall(struct warp_zoom_control *ctl, const char *id_or_name);

/* Keeps the view as a preset and answers with its id, which the caller frees.
 * 'view' is NULL to keep the view the source is framed with right now. */
char *warp_zoom_control_add_preset(struct warp_zoom_control *ctl, const char *name, const struct warp_zoom_view *view,
				   int glide_ms);
/* Applies whichever of the four a caller passes to a preset that is already
 * there: 'name' to rename it, 'view' to reframe it, 'glide_ms' from zero up to
 * change how long the move to it takes, 'path' from zero up to change the route
 * it takes, -1 for either to leave it alone. */
bool warp_zoom_control_update_preset(struct warp_zoom_control *ctl, const char *id, const char *name,
				     const struct warp_zoom_view *view, int glide_ms, int path);
bool warp_zoom_control_remove_preset(struct warp_zoom_control *ctl, const char *id);
/* moves a preset up or down the list, which is what the numbered recall
 * hotkeys go by */
bool warp_zoom_control_move_preset(struct warp_zoom_control *ctl, const char *id, int delta);
/* every preset, the default one first; the caller releases the array */
obs_data_array_t *warp_zoom_control_preset_array(struct warp_zoom_control *ctl);

#endif /* __cplusplus */

/* ------------------------------------------------------------------------- */
/* driving a source from outside it
 *
 * Everything that is not the source itself - the dock, the websocket requests,
 * a Warp Detection filter - goes through the procs above rather than through
 * the source's own type, so a Warp Playlist, a Warp Media source and a camera
 * with a Warp Zoom filter on it are all driven the same way. */

/* whether the source (or filter) can be zoomed at all */
bool warp_zoom_source_capable(obs_source_t *source);
/* how the source is framed right now; false when it cannot be zoomed */
bool warp_zoom_source_get(obs_source_t *source, struct warp_zoom_view *view, struct warp_zoom_view *target);
void warp_zoom_source_set(obs_source_t *source, const struct warp_zoom_view *view, int glide_ms);
void warp_zoom_source_adjust(obs_source_t *source, float factor, int glide_ms);
void warp_zoom_source_pan(obs_source_t *source, float dx, float dy, int glide_ms);
void warp_zoom_source_reset(obs_source_t *source, int glide_ms);
/* how the source is framed, what is lined up behind it, and whether it stages
 * changes at all; every out parameter may be NULL */
bool warp_zoom_source_stage(obs_source_t *source, struct warp_zoom_view *staged, bool *is_staged, bool *confirm);
void warp_zoom_source_set_confirm(obs_source_t *source, bool confirm);
bool warp_zoom_source_take(obs_source_t *source);
bool warp_zoom_source_drop(obs_source_t *source);
bool warp_zoom_source_recall(obs_source_t *source, const char *id_or_name);
bool warp_zoom_source_recall_slot(obs_source_t *source, int slot);
/* the presets of a source, as the array they are saved as, or NULL */
obs_data_array_t *warp_zoom_source_presets(obs_source_t *source);
char *warp_zoom_source_save_preset(obs_source_t *source, const char *name);
bool warp_zoom_source_update_preset(obs_source_t *source, const char *id, const char *name,
				    const struct warp_zoom_view *view, int glide_ms, int path);
bool warp_zoom_source_remove_preset(obs_source_t *source, const char *id);
bool warp_zoom_source_move_preset(obs_source_t *source, const char *id, int delta);

/* ------------------------------------------------------------------------- */
/* the filter
 *
 * The playlist and the Warp Media source do not draw their own picture - one
 * hands its files to a transition, the other is an async source - so the zoom
 * is drawn by a Warp Zoom filter they put on what they are showing and drive
 * themselves. It is the same filter that is offered in the OBS filter list, so
 * a camera or a scene can be framed with it too. */

/* Frames a driven filter. Called from the tick of the source that owns it, so
 * the view is applied on the frame it was asked for. */
void warp_zoom_filter_apply(obs_source_t *filter, const struct warp_zoom_view *view);
/* Makes a driven filter for a source to frame what it is showing with. The
 * caller owns the reference, and adds it to whatever it is framing. */
obs_source_t *warp_zoom_filter_create_driven(const char *name);
/* the driven filter on 'source', with a reference the caller releases, or NULL */
obs_source_t *warp_zoom_filter_find(obs_source_t *source);
/* The Warp Zoom filter an operator put on 'source' themselves, which is the one
 * with presets and hotkeys of its own. The dock and the websocket requests
 * reach for this, so a camera with the filter on it is framed like a Warp
 * source. The caller releases the reference. */
obs_source_t *warp_zoom_filter_find_operable(obs_source_t *source);

#ifdef __cplusplus
}
#endif
