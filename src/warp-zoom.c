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

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <util/dstr.h>
#include <util/platform.h>

#include "warp-events.h"
#include "warp-zoom.h"

/* ------------------------------------------------------------------------- */
/* the stacks the signals and the procs are carried on
 *
 * Every one of these is worked out from the parameters that go on it, the way
 * WARP_CD_PARAM in warp-events.h describes: a stack that is short does not
 * merely truncate, it drops parameters and says so in the log every time the
 * call is made. Anything carrying a string an operator typed - a preset name or
 * the presets themselves - has no length to size for at all, and is allocated
 * instead. */

/* The two zoom signals: the source, the view, how it got there, the name of the
 * preset it came from, and whether a shot is being lined up or has gone. */
#define WARP_ZOOM_CD_SIGNAL \
	WARP_CD_STACK(WARP_CD_PARAM("source", sizeof(void *)) + 3 * WARP_CD_PARAM("zoom", sizeof(double)) + \
		      WARP_CD_PARAM("change", sizeof(WARP_ZOOM_CHANGE_MANUAL)) + \
		      WARP_CD_PARAM("preset", WARP_ZOOM_PRESET_NAME_MAX) + \
		      WARP_CD_PARAM("staged", sizeof(bool)))

/* The reply to warp_zoom_get, which is the widest of the procs: eleven
 * parameters, none of them named longer than staged_zoom and none of them
 * carrying more than a float. */
#define WARP_ZOOM_CD_GET WARP_CD_STACK(11 * WARP_CD_PARAM("staged_zoom", sizeof(double)))

/* a view and how long the move to it takes */
#define WARP_ZOOM_CD_SET \
	WARP_CD_STACK(3 * WARP_CD_PARAM("zoom", sizeof(double)) + WARP_CD_PARAM("glide", sizeof(long long)))
/* a nudge: a factor, or a pair of deltas, and the glide */
#define WARP_ZOOM_CD_NUDGE \
	WARP_CD_STACK(2 * WARP_CD_PARAM("factor", sizeof(double)) + WARP_CD_PARAM("glide", sizeof(long long)))
/* the glide on its own */
#define WARP_ZOOM_CD_GLIDE WARP_CD_STACK(WARP_CD_PARAM("glide", sizeof(long long)))
/* a flag in and a flag back out, which is as much as the confirm, take and drop
 * procs carry */
#define WARP_ZOOM_CD_FLAGS WARP_CD_STACK(2 * WARP_CD_PARAM("dropped", sizeof(bool)))
/* recalling by slot, which names no preset */
#define WARP_ZOOM_CD_SLOT \
	WARP_CD_STACK(WARP_CD_PARAM("preset", sizeof("")) + WARP_CD_PARAM("slot", sizeof(long long)) + \
		      WARP_CD_PARAM("found", sizeof(bool)))

/* ------------------------------------------------------------------------- */
/* the view
 *
 * A move eases in and out rather than running at a flat rate, so recalling a
 * preset reads as a camera move instead of a slide. The zoom is interpolated
 * geometrically - each step multiplies rather than adds - because that is how
 * zoom is felt: halfway between 100% and 400% looks like 200%, not 250%. */

static inline float warp_zoom_ease(float t)
{
	if (t <= 0.0f)
		return 0.0f;
	if (t >= 1.0f)
		return 1.0f;

	return t * t * (3.0f - 2.0f * t);
}

static inline float warp_zoom_lerp(float from, float to, float t)
{
	return from + (to - from) * t;
}

static struct warp_zoom_view warp_zoom_between(const struct warp_zoom_view *from, const struct warp_zoom_view *to,
					       float t)
{
	struct warp_zoom_view view;

	view.zoom = from->zoom * expf(logf(to->zoom / from->zoom) * t);
	view.x = warp_zoom_lerp(from->x, to->x, t);
	view.y = warp_zoom_lerp(from->y, to->y, t);

	warp_zoom_clamp(&view);

	return view;
}

/* ------------------------------------------------------------------------- */
/* presets (all of these expect ctl->mutex to be held) */

static struct warp_zoom_preset *warp_zoom_find(struct warp_zoom_control *ctl, const char *id)
{
	if (!id || !*id)
		return NULL;

	for (size_t i = 0; i < ctl->presets.num; i++) {
		if (strcmp(ctl->presets.array[i].id, id) == 0)
			return &ctl->presets.array[i];
	}

	return NULL;
}

/* The preset a caller asked for by id, or, when nothing goes by that id, the
 * first one that goes by that name: the dock and the websocket work in ids, an
 * operator writing a request by hand works in names. */
static struct warp_zoom_preset *warp_zoom_find_loose(struct warp_zoom_control *ctl, const char *id_or_name)
{
	struct warp_zoom_preset *preset = warp_zoom_find(ctl, id_or_name);

	if (preset || !id_or_name || !*id_or_name)
		return preset;

	for (size_t i = 0; i < ctl->presets.num; i++) {
		if (astrcmpi(ctl->presets.array[i].name, id_or_name) == 0)
			return &ctl->presets.array[i];
	}

	return NULL;
}

static bool warp_zoom_name_taken(struct warp_zoom_control *ctl, const char *name, const char *except_id)
{
	for (size_t i = 0; i < ctl->presets.num; i++) {
		const struct warp_zoom_preset *preset = &ctl->presets.array[i];

		if (except_id && strcmp(preset->id, except_id) == 0)
			continue;

		if (astrcmpi(preset->name, name) == 0)
			return true;
	}

	return false;
}

/* A name nothing else in the list is using, so the presets stay tellable apart
 * in the dock and can be recalled by name from a script or the websocket. */
static char *warp_zoom_unique_name(struct warp_zoom_control *ctl, const char *wanted, const char *except_id)
{
	struct dstr name = {0};

	if (!wanted || !*wanted)
		wanted = obs_module_text("Warp.Zoom.Preset.Unnamed");

	dstr_copy(&name, wanted);

	for (int i = 2; warp_zoom_name_taken(ctl, name.array, except_id) && i < 1000; i++)
		dstr_printf(&name, "%s %d", wanted, i);

	return name.array;
}

/* ids are never shown, only matched, so anything that cannot collide will do */
static char *warp_zoom_make_id(void)
{
	static uint64_t counter;
	struct dstr id = {0};

	dstr_printf(&id, "zp_%llx_%llx", (unsigned long long)os_gettime_ns(), (unsigned long long)++counter);

	return id.array;
}

static void warp_zoom_preset_free(struct warp_zoom_preset *preset)
{
	bfree(preset->id);
	bfree(preset->name);
}

static bool warp_zoom_is_default(const struct warp_zoom_preset *preset)
{
	return strcmp(preset->id, WARP_ZOOM_DEFAULT_ID) == 0;
}

/* The reset position, which is always the first preset and is never taken from
 * the settings: it is what it is whatever a scene collection was saved with.
 * Expects ctl->mutex to be held. */
static void warp_zoom_add_default(struct warp_zoom_control *ctl)
{
	struct warp_zoom_preset preset = {0};

	preset.id = bstrdup(WARP_ZOOM_DEFAULT_ID);
	preset.name = bstrdup(obs_module_text("Warp.Zoom.Preset.Default"));
	preset.view = warp_zoom_default_view();
	preset.hotkey = OBS_INVALID_HOTKEY_ID;

	da_insert(ctl->presets, 0, &preset);
}

/* ------------------------------------------------------------------------- */
/* saying what changed
 *
 * Whatever reacts to a zoom is free to drive the source straight back, so the
 * signal is never emitted from under the lock: the change is left on the
 * control and handed out once it has been dropped. */

static void warp_zoom_arm_signal(struct warp_zoom_control *ctl, const struct warp_zoom_view *view, const char *change,
				 const char *preset)
{
	ctl->signal_armed = true;
	ctl->signal_is_stage = false;
	ctl->signal_view = *view;
	ctl->signal_change = change;

	snprintf(ctl->signal_preset, sizeof(ctl->signal_preset), "%s", preset ? preset : "");
}

/* A shot being lined up, taken or dropped. Nothing has moved on screen, so it
 * is said as a staging rather than as a change. Expects ctl->mutex to be
 * held. */
static void warp_zoom_arm_stage_signal(struct warp_zoom_control *ctl, const struct warp_zoom_view *view, bool staged,
				       const char *change, const char *preset)
{
	warp_zoom_arm_signal(ctl, view, change, preset);

	ctl->signal_is_stage = true;
	ctl->signal_staged = staged;
}

static void warp_zoom_unlock(struct warp_zoom_control *ctl)
{
	struct warp_zoom_view view = ctl->signal_view;
	const char *change = ctl->signal_change;
	bool armed = ctl->signal_armed;
	bool is_stage = ctl->signal_is_stage;
	bool staged = ctl->signal_staged;
	char preset[sizeof(ctl->signal_preset)];

	snprintf(preset, sizeof(preset), "%s", ctl->signal_preset);
	ctl->signal_armed = false;

	pthread_mutex_unlock(&ctl->mutex);

	if (!armed || !ctl->source)
		return;

	struct calldata cd;
	uint8_t stack[WARP_ZOOM_CD_SIGNAL];

	calldata_init_fixed(&cd, stack, sizeof(stack));
	calldata_set_ptr(&cd, "source", ctl->source);
	calldata_set_float(&cd, "zoom", view.zoom);
	calldata_set_float(&cd, "x", view.x);
	calldata_set_float(&cd, "y", view.y);
	calldata_set_string(&cd, "change", change ? change : WARP_ZOOM_CHANGE_SET);
	calldata_set_string(&cd, "preset", preset);

	if (is_stage)
		calldata_set_bool(&cd, "staged", staged);

	signal_handler_signal(obs_source_get_signal_handler(ctl->source),
			      is_stage ? WARP_SIGNAL_ZOOM_STAGED : WARP_SIGNAL_ZOOM_CHANGED, &cd);
}

/* ------------------------------------------------------------------------- */
/* moving the view */

static void warp_zoom_write_view(struct warp_zoom_control *ctl);

/* expects ctl->mutex to be held */
static void warp_zoom_aim(struct warp_zoom_control *ctl, const struct warp_zoom_view *view, int glide_ms)
{
	struct warp_zoom_view target = *view;

	warp_zoom_clamp(&target);

	if (glide_ms < 0)
		glide_ms = (int)ctl->glide_ms;
	else if (glide_ms > WARP_ZOOM_GLIDE_MAX)
		glide_ms = WARP_ZOOM_GLIDE_MAX;

	ctl->target = target;
	ctl->revision++;

	if (glide_ms <= 0) {
		ctl->view = target;
		ctl->glide_len = 0.0f;
		return;
	}

	/* a move that is asked for partway through another one starts from
	 * where the picture actually is, so the two run into one another */
	ctl->from = ctl->view;
	ctl->glide_elapsed = 0.0f;
	ctl->glide_len = (float)glide_ms / 1000.0f;
}

/* Where a nudge works from: the shot being lined up when there is one, so that
 * holding a key down walks the staged shot along rather than starting over from
 * what is on screen. Expects ctl->mutex to be held. */
static struct warp_zoom_view warp_zoom_base(struct warp_zoom_control *ctl)
{
	return ctl->staged ? ctl->stage : ctl->target;
}

/* Lines a shot up instead of putting it on screen, and answers whether it did.
 * Everything that reframes the source goes through here first, so the dock, the
 * hotkeys and the websocket all behave the way confirm mode says they will.
 * Expects ctl->mutex to be held. */
/* 'glide_ms' is how long the move will take when the shot is taken, which is
 * not the same as how long the move that lined it up would have taken: lining
 * a shot up moves nothing, and taking it should ease the way a recall does
 * however the shot was found. Only a preset carrying a glide of its own passes
 * anything but -1 here. */
static bool warp_zoom_stage(struct warp_zoom_control *ctl, const struct warp_zoom_view *view, int glide_ms,
			    const char *change, const char *preset)
{
	if (!ctl->confirm)
		return false;

	ctl->stage = *view;
	warp_zoom_clamp(&ctl->stage);

	ctl->staged = true;
	ctl->stage_glide = glide_ms;
	ctl->stage_change = change ? change : WARP_ZOOM_CHANGE_SET;
	ctl->revision++;

	snprintf(ctl->stage_preset, sizeof(ctl->stage_preset), "%s", preset ? preset : "");

	warp_zoom_arm_stage_signal(ctl, &ctl->stage, true, ctl->stage_change, ctl->stage_preset);

	return true;
}

void warp_zoom_control_set(struct warp_zoom_control *ctl, const struct warp_zoom_view *view, int glide_ms,
			   const char *change, const char *preset)
{
	bool staged;

	pthread_mutex_lock(&ctl->mutex);

	staged = warp_zoom_stage(ctl, view, -1, change ? change : WARP_ZOOM_CHANGE_SET, preset);

	if (!staged) {
		warp_zoom_aim(ctl, view, glide_ms);
		warp_zoom_arm_signal(ctl, &ctl->target, change ? change : WARP_ZOOM_CHANGE_SET, preset);
	}

	warp_zoom_unlock(ctl);

	if (!staged)
		warp_zoom_write_view(ctl);
}

void warp_zoom_control_adjust(struct warp_zoom_control *ctl, float factor, int glide_ms)
{
	struct warp_zoom_view view;
	bool staged;

	if (!(factor > 0.0f))
		return;

	pthread_mutex_lock(&ctl->mutex);

	/* Zooming works on where the view is heading rather than on where it
	 * has got to, so holding the key down walks steadily in instead of
	 * fighting the glide it just asked for. */
	view = warp_zoom_base(ctl);
	view.zoom *= factor;

	if (glide_ms < 0)
		glide_ms = (int)ctl->nudge_ms;

	staged = warp_zoom_stage(ctl, &view, -1, WARP_ZOOM_CHANGE_MANUAL, NULL);

	if (!staged) {
		warp_zoom_aim(ctl, &view, glide_ms);
		warp_zoom_arm_signal(ctl, &ctl->target, WARP_ZOOM_CHANGE_MANUAL, NULL);
	}

	warp_zoom_unlock(ctl);

	if (!staged)
		warp_zoom_write_view(ctl);
}

void warp_zoom_control_pan(struct warp_zoom_control *ctl, float dx, float dy, int glide_ms)
{
	struct warp_zoom_view view;
	bool staged;

	pthread_mutex_lock(&ctl->mutex);

	view = warp_zoom_base(ctl);

	/* the move is a fraction of what is on screen, so panning covers the
	 * same distance on the stream however far in the picture is zoomed */
	view.x += dx / view.zoom;
	view.y += dy / view.zoom;

	if (glide_ms < 0)
		glide_ms = (int)ctl->nudge_ms;

	staged = warp_zoom_stage(ctl, &view, -1, WARP_ZOOM_CHANGE_MANUAL, NULL);

	if (!staged) {
		warp_zoom_aim(ctl, &view, glide_ms);
		warp_zoom_arm_signal(ctl, &ctl->target, WARP_ZOOM_CHANGE_MANUAL, NULL);
	}

	warp_zoom_unlock(ctl);

	if (!staged)
		warp_zoom_write_view(ctl);
}

void warp_zoom_control_reset(struct warp_zoom_control *ctl, int glide_ms)
{
	struct warp_zoom_view view = warp_zoom_default_view();
	char name[sizeof(ctl->signal_preset)];
	bool staged;

	pthread_mutex_lock(&ctl->mutex);

	snprintf(name, sizeof(name), "%s", ctl->presets.num ? ctl->presets.array[0].name : "");

	staged = warp_zoom_stage(ctl, &view, -1, WARP_ZOOM_CHANGE_RESET, name);

	if (!staged) {
		warp_zoom_aim(ctl, &view, glide_ms);
		warp_zoom_arm_signal(ctl, &ctl->target, WARP_ZOOM_CHANGE_RESET, name);
	}

	warp_zoom_unlock(ctl);

	if (!staged)
		warp_zoom_write_view(ctl);
}

void warp_zoom_control_restore_default(struct warp_zoom_control *ctl)
{
	pthread_mutex_lock(&ctl->mutex);

	/* A video starting is not a change anyone asked for, so nothing is
	 * said about it and nothing is eased: the file goes up framed the way
	 * every file goes up. A shot lined up on the video that has just gone
	 * goes with it - taking it against the next one would frame a video it
	 * was never composed against. */
	ctl->staged = false;
	ctl->view = warp_zoom_default_view();
	ctl->target = ctl->view;
	ctl->from = ctl->view;
	ctl->glide_len = 0.0f;
	ctl->glide_elapsed = 0.0f;
	ctl->revision++;

	pthread_mutex_unlock(&ctl->mutex);
}

bool warp_zoom_control_recall(struct warp_zoom_control *ctl, const char *id_or_name)
{
	struct warp_zoom_preset *preset;
	struct warp_zoom_view view;
	char name[sizeof(ctl->signal_preset)];
	const char *change;
	int glide;
	bool reset;
	bool staged;

	pthread_mutex_lock(&ctl->mutex);

	preset = warp_zoom_find_loose(ctl, id_or_name);

	if (!preset) {
		pthread_mutex_unlock(&ctl->mutex);
		return false;
	}

	view = preset->view;
	glide = preset->glide_ms ? (int)preset->glide_ms : -1;
	reset = warp_zoom_is_default(preset);
	snprintf(name, sizeof(name), "%s", preset->name);

	change = reset ? WARP_ZOOM_CHANGE_RESET : WARP_ZOOM_CHANGE_PRESET;
	staged = warp_zoom_stage(ctl, &view, glide, change, name);

	if (!staged) {
		warp_zoom_aim(ctl, &view, glide);
		warp_zoom_arm_signal(ctl, &ctl->target, change, name);
	}

	warp_zoom_unlock(ctl);

	if (!staged)
		warp_zoom_write_view(ctl);

	return true;
}

void warp_zoom_control_set_confirm(struct warp_zoom_control *ctl, bool confirm)
{
	struct warp_zoom_view view;

	pthread_mutex_lock(&ctl->mutex);

	if (ctl->confirm != confirm) {
		ctl->confirm = confirm;
		ctl->revision++;
	}

	/* Coming out of confirm mode drops whatever was waiting rather than
	 * putting it up: a shot goes to air because someone took it, never
	 * because a setting was switched off. */
	if (!confirm && ctl->staged) {
		ctl->staged = false;
		view = ctl->target;

		warp_zoom_arm_stage_signal(ctl, &view, false, WARP_ZOOM_CHANGE_SET, NULL);
	}

	warp_zoom_unlock(ctl);
}

bool warp_zoom_control_confirm(struct warp_zoom_control *ctl)
{
	bool confirm;

	pthread_mutex_lock(&ctl->mutex);
	confirm = ctl->confirm;
	pthread_mutex_unlock(&ctl->mutex);

	return confirm;
}

bool warp_zoom_control_staged(struct warp_zoom_control *ctl, struct warp_zoom_view *out)
{
	bool staged;

	pthread_mutex_lock(&ctl->mutex);

	staged = ctl->staged;

	if (out)
		*out = staged ? ctl->stage : ctl->target;

	pthread_mutex_unlock(&ctl->mutex);

	return staged;
}

bool warp_zoom_control_take(struct warp_zoom_control *ctl)
{
	struct warp_zoom_view view;
	char preset[sizeof(ctl->stage_preset)];
	const char *change;
	int glide;

	pthread_mutex_lock(&ctl->mutex);

	if (!ctl->staged) {
		pthread_mutex_unlock(&ctl->mutex);
		return false;
	}

	view = ctl->stage;
	glide = ctl->stage_glide;
	change = ctl->stage_change;
	snprintf(preset, sizeof(preset), "%s", ctl->stage_preset);

	ctl->staged = false;

	/* it is going to air now, so this is a change to the picture like any
	 * other: the same signal fires, and a Warp Detection filter reacts to
	 * it here rather than when the shot was lined up */
	warp_zoom_aim(ctl, &view, glide);
	warp_zoom_arm_signal(ctl, &ctl->target, change, preset);

	warp_zoom_unlock(ctl);
	warp_zoom_write_view(ctl);

	return true;
}

bool warp_zoom_control_drop(struct warp_zoom_control *ctl)
{
	struct warp_zoom_view view;

	pthread_mutex_lock(&ctl->mutex);

	if (!ctl->staged) {
		pthread_mutex_unlock(&ctl->mutex);
		return false;
	}

	ctl->staged = false;
	ctl->revision++;
	view = ctl->target;

	warp_zoom_arm_stage_signal(ctl, &view, false, WARP_ZOOM_CHANGE_SET, NULL);

	warp_zoom_unlock(ctl);

	return true;
}

bool warp_zoom_control_recall_slot(struct warp_zoom_control *ctl, size_t slot)
{
	char id[128];

	pthread_mutex_lock(&ctl->mutex);

	/* the numbered slots count the presets that were made, so slot one is
	 * the first preset in the list rather than the reset position */
	if (slot >= ctl->presets.num) {
		pthread_mutex_unlock(&ctl->mutex);
		return false;
	}

	snprintf(id, sizeof(id), "%s", ctl->presets.array[slot].id);

	pthread_mutex_unlock(&ctl->mutex);

	return warp_zoom_control_recall(ctl, id);
}

bool warp_zoom_control_tick(struct warp_zoom_control *ctl, float seconds, struct warp_zoom_view *out)
{
	struct warp_zoom_view before;
	bool moved;

	pthread_mutex_lock(&ctl->mutex);

	before = ctl->view;

	if (ctl->glide_len > 0.0f) {
		ctl->glide_elapsed += seconds;

		if (ctl->glide_elapsed >= ctl->glide_len) {
			ctl->view = ctl->target;
			ctl->glide_len = 0.0f;
		} else {
			float t = warp_zoom_ease(ctl->glide_elapsed / ctl->glide_len);

			ctl->view = warp_zoom_between(&ctl->from, &ctl->target, t);
		}
	}

	moved = !warp_zoom_view_equal(&before, &ctl->view);

	if (out)
		*out = ctl->view;

	pthread_mutex_unlock(&ctl->mutex);

	return moved;
}

void warp_zoom_control_get(struct warp_zoom_control *ctl, struct warp_zoom_view *view, struct warp_zoom_view *target)
{
	pthread_mutex_lock(&ctl->mutex);

	if (view)
		*view = ctl->view;
	if (target)
		*target = ctl->target;

	pthread_mutex_unlock(&ctl->mutex);
}

uint64_t warp_zoom_control_revision(struct warp_zoom_control *ctl)
{
	uint64_t revision;

	pthread_mutex_lock(&ctl->mutex);
	revision = ctl->revision;
	pthread_mutex_unlock(&ctl->mutex);

	return revision;
}

/* ------------------------------------------------------------------------- */
/* hotkeys
 *
 * A press arrives on the hotkey thread with libobs' hotkey lock held, and that
 * same lock is taken to register or drop a hotkey. So the preset hotkeys are
 * brought up to date by a pass of its own, run with ctl->mutex dropped, rather
 * than from under the lock the presets are edited behind. */

/* Whether what this control frames is in front of the operator: the hotkeys
 * only apply to a source that is on screen, the way every other Warp hotkey
 * does. A filter is showing when the source it is on is, so it is the parent
 * that is asked about when there is one. */
static bool warp_zoom_showing(struct warp_zoom_control *ctl)
{
	obs_source_t *parent = obs_filter_get_parent(ctl->source);

	return obs_source_showing(parent ? parent : ctl->source);
}

static void warp_zoom_zoom_in_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	struct warp_zoom_control *ctl = data;

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed || !warp_zoom_showing(ctl))
		return;

	warp_zoom_control_adjust(ctl, WARP_ZOOM_STEP, -1);
}

static void warp_zoom_zoom_out_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	struct warp_zoom_control *ctl = data;

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed || !warp_zoom_showing(ctl))
		return;

	warp_zoom_control_adjust(ctl, 1.0f / WARP_ZOOM_STEP, -1);
}

static void warp_zoom_reset_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	struct warp_zoom_control *ctl = data;

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed || !warp_zoom_showing(ctl))
		return;

	warp_zoom_control_reset(ctl, -1);
}

/* the four pan hotkeys, told apart by the direction in their binding */
static void warp_zoom_pan_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	struct warp_zoom_binding *binding = data;
	float dx = 0.0f;
	float dy = 0.0f;

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed || !warp_zoom_showing(binding->ctl))
		return;

	/* the four straight directions, then the corners, which move a full
	 * step on both axes at once */
	switch (binding->value) {
	case 0:
		dx = -WARP_ZOOM_PAN_STEP;
		break;
	case 1:
		dx = WARP_ZOOM_PAN_STEP;
		break;
	case 2:
		dy = -WARP_ZOOM_PAN_STEP;
		break;
	case 3:
		dy = WARP_ZOOM_PAN_STEP;
		break;
	case 4:
		dx = -WARP_ZOOM_PAN_STEP;
		dy = -WARP_ZOOM_PAN_STEP;
		break;
	case 5:
		dx = WARP_ZOOM_PAN_STEP;
		dy = -WARP_ZOOM_PAN_STEP;
		break;
	case 6:
		dx = -WARP_ZOOM_PAN_STEP;
		dy = WARP_ZOOM_PAN_STEP;
		break;
	default:
		dx = WARP_ZOOM_PAN_STEP;
		dy = WARP_ZOOM_PAN_STEP;
		break;
	}

	warp_zoom_control_pan(binding->ctl, dx, dy, -1);
}

static void warp_zoom_take_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	struct warp_zoom_control *ctl = data;

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed || !warp_zoom_showing(ctl))
		return;

	warp_zoom_control_take(ctl);
}

static void warp_zoom_drop_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	struct warp_zoom_control *ctl = data;

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed || !warp_zoom_showing(ctl))
		return;

	warp_zoom_control_drop(ctl);
}

static void warp_zoom_slot_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	struct warp_zoom_binding *binding = data;

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed || !warp_zoom_showing(binding->ctl))
		return;

	warp_zoom_control_recall_slot(binding->ctl, (size_t)binding->value);
}

/* A preset's own hotkey. The preset it belongs to is looked up by the id of
 * the hotkey that fired rather than carried in the callback's data, so that a
 * preset being removed cannot leave a press holding freed memory. */
static void warp_zoom_preset_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	struct warp_zoom_control *ctl = data;
	char preset[128];
	bool found = false;

	UNUSED_PARAMETER(hotkey);

	if (!pressed || !warp_zoom_showing(ctl))
		return;

	pthread_mutex_lock(&ctl->mutex);

	for (size_t i = 0; i < ctl->presets.num; i++) {
		if (ctl->presets.array[i].hotkey != id)
			continue;

		snprintf(preset, sizeof(preset), "%s", ctl->presets.array[i].id);
		found = true;
		break;
	}

	pthread_mutex_unlock(&ctl->mutex);

	if (found)
		warp_zoom_control_recall(ctl, preset);
}

/* what a preset's hotkey is registered and listed as */
static void warp_zoom_preset_hotkey_names(struct warp_zoom_control *ctl, const char *id, const char *name,
					  struct dstr *hotkey_name, struct dstr *desc)
{
	dstr_printf(hotkey_name, "%s.ZoomPreset.%s", ctl->prefix, id);
	dstr_printf(desc, obs_module_text("Warp.Hotkey.Zoom.Preset"), name);
}

/* What the preset hotkeys have to become, read off the control so the pass
 * that carries it out does not have to hold the lock while it does. */
struct warp_zoom_key {
	char *id;
	char *name;
	obs_hotkey_id hotkey;
};

/* Registers a hotkey for every preset that has none, renames the ones that
 * have been renamed, and drops the ones whose preset is gone. Expects
 * ctl->mutex NOT to be held. */
static void warp_zoom_sync_hotkeys(struct warp_zoom_control *ctl)
{
	DARRAY(obs_hotkey_id) orphans;
	DARRAY(struct warp_zoom_key) keys;

	if (!ctl->hotkeys || !ctl->source)
		return;

	da_init(orphans);
	da_init(keys);

	pthread_mutex_lock(&ctl->mutex);

	orphans.da = ctl->orphans.da;
	da_init(ctl->orphans);

	/* the reset position is reached with the Reset Zoom hotkey, so it is
	 * not given one of its own on top of it */
	for (size_t i = 1; i < ctl->presets.num; i++) {
		struct warp_zoom_key key;

		key.id = bstrdup(ctl->presets.array[i].id);
		key.name = bstrdup(ctl->presets.array[i].name);
		key.hotkey = ctl->presets.array[i].hotkey;

		da_push_back(keys, &key);
	}

	pthread_mutex_unlock(&ctl->mutex);

	for (size_t i = 0; i < orphans.num; i++)
		obs_hotkey_unregister(orphans.array[i]);

	for (size_t i = 0; i < keys.num; i++) {
		struct warp_zoom_key *key = &keys.array[i];
		struct dstr hotkey_name = {0};
		struct dstr desc = {0};

		warp_zoom_preset_hotkey_names(ctl, key->id, key->name, &hotkey_name, &desc);

		/* A hotkey registered after the source was loaded is given the
		 * keys the source was saved with all the same: libobs keeps the
		 * source's hotkey data and applies it as each one is
		 * registered, which is what lets presets be made and removed
		 * without the rest of them losing their bindings. */
		if (key->hotkey == OBS_INVALID_HOTKEY_ID)
			key->hotkey = obs_hotkey_register_source(ctl->source, hotkey_name.array, desc.array,
								 warp_zoom_preset_hotkey, ctl);
		else
			obs_hotkey_set_description(key->hotkey, desc.array);

		dstr_free(&hotkey_name);
		dstr_free(&desc);
	}

	pthread_mutex_lock(&ctl->mutex);

	for (size_t i = 0; i < keys.num; i++) {
		struct warp_zoom_preset *preset = warp_zoom_find(ctl, keys.array[i].id);

		/* the preset may have been removed again while its hotkey was
		 * being registered, which the next pass picks up */
		if (preset && preset->hotkey == OBS_INVALID_HOTKEY_ID)
			preset->hotkey = keys.array[i].hotkey;
		else if (!preset && keys.array[i].hotkey != OBS_INVALID_HOTKEY_ID)
			da_push_back(ctl->orphans, &keys.array[i].hotkey);
	}

	pthread_mutex_unlock(&ctl->mutex);

	for (size_t i = 0; i < keys.num; i++) {
		bfree(keys.array[i].id);
		bfree(keys.array[i].name);
	}

	da_free(keys);
	da_free(orphans);
}

static void warp_zoom_register_hotkeys(struct warp_zoom_control *ctl)
{
	static const char *const pan_names[WARP_ZOOM_NUM_PANS] = {"ZoomPanLeft",     "ZoomPanRight",
								  "ZoomPanUp",       "ZoomPanDown",
								  "ZoomPanUpLeft",   "ZoomPanUpRight",
								  "ZoomPanDownLeft", "ZoomPanDownRight"};
	static const char *const pan_text[WARP_ZOOM_NUM_PANS] = {
		"Warp.Hotkey.Zoom.PanLeft",     "Warp.Hotkey.Zoom.PanRight",    "Warp.Hotkey.Zoom.PanUp",
		"Warp.Hotkey.Zoom.PanDown",     "Warp.Hotkey.Zoom.PanUpLeft",   "Warp.Hotkey.Zoom.PanUpRight",
		"Warp.Hotkey.Zoom.PanDownLeft", "Warp.Hotkey.Zoom.PanDownRight"};
	struct dstr name = {0};
	struct dstr desc = {0};

	if (!ctl->hotkeys || !ctl->source)
		return;

	dstr_printf(&name, "%s.ZoomIn", ctl->prefix);
	ctl->in_hotkey = obs_hotkey_register_source(ctl->source, name.array, obs_module_text("Warp.Hotkey.Zoom.In"),
						    warp_zoom_zoom_in_hotkey, ctl);

	dstr_printf(&name, "%s.ZoomOut", ctl->prefix);
	ctl->out_hotkey = obs_hotkey_register_source(ctl->source, name.array, obs_module_text("Warp.Hotkey.Zoom.Out"),
						     warp_zoom_zoom_out_hotkey, ctl);

	dstr_printf(&name, "%s.ZoomReset", ctl->prefix);
	ctl->reset_hotkey = obs_hotkey_register_source(
		ctl->source, name.array, obs_module_text("Warp.Hotkey.Zoom.Reset"), warp_zoom_reset_hotkey, ctl);

	for (size_t i = 0; i < WARP_ZOOM_NUM_PANS; i++) {
		ctl->pan_bindings[i].ctl = ctl;
		ctl->pan_bindings[i].value = (int)i;

		dstr_printf(&name, "%s.%s", ctl->prefix, pan_names[i]);
		ctl->pan_hotkeys[i] = obs_hotkey_register_source(ctl->source, name.array, obs_module_text(pan_text[i]),
								 warp_zoom_pan_hotkey, &ctl->pan_bindings[i]);
	}

	dstr_printf(&name, "%s.ZoomTake", ctl->prefix);
	ctl->take_hotkey = obs_hotkey_register_source(ctl->source, name.array, obs_module_text("Warp.Hotkey.Zoom.Take"),
						      warp_zoom_take_hotkey, ctl);

	dstr_printf(&name, "%s.ZoomDrop", ctl->prefix);
	ctl->drop_hotkey = obs_hotkey_register_source(ctl->source, name.array, obs_module_text("Warp.Hotkey.Zoom.Drop"),
						      warp_zoom_drop_hotkey, ctl);

	/* The numbered slots are registered once and stay put, whatever the
	 * presets do: a slot fires whichever preset is in that position, so a
	 * preset being renamed or removed mid-show cannot take a binding with
	 * it. Each preset has a hotkey of its own on top of these. */
	for (size_t i = 0; i < WARP_ZOOM_SLOTS; i++) {
		ctl->slot_bindings[i].ctl = ctl;
		ctl->slot_bindings[i].value = (int)(i + 1);

		dstr_printf(&name, "%s.ZoomSlot%d", ctl->prefix, (int)(i + 1));
		dstr_printf(&desc, obs_module_text("Warp.Hotkey.Zoom.Slot"), (int)(i + 1));

		ctl->slot_hotkeys[i] = obs_hotkey_register_source(ctl->source, name.array, desc.array,
								  warp_zoom_slot_hotkey, &ctl->slot_bindings[i]);
	}

	dstr_free(&name);
	dstr_free(&desc);
}

/* ------------------------------------------------------------------------- */
/* the control itself */

void warp_zoom_control_init(struct warp_zoom_control *ctl, obs_source_t *source, const char *prefix, bool hotkeys,
			    bool persist_view)
{
	ctl->source = source;
	ctl->prefix = prefix;
	ctl->hotkeys = hotkeys;
	ctl->persist_view = persist_view;

	ctl->view = warp_zoom_default_view();
	ctl->from = ctl->view;
	ctl->target = ctl->view;
	ctl->glide_ms = WARP_ZOOM_GLIDE_MS;
	ctl->nudge_ms = WARP_ZOOM_NUDGE_MS;

	ctl->in_hotkey = OBS_INVALID_HOTKEY_ID;
	ctl->out_hotkey = OBS_INVALID_HOTKEY_ID;
	ctl->reset_hotkey = OBS_INVALID_HOTKEY_ID;
	ctl->take_hotkey = OBS_INVALID_HOTKEY_ID;
	ctl->drop_hotkey = OBS_INVALID_HOTKEY_ID;

	for (size_t i = 0; i < WARP_ZOOM_NUM_PANS; i++)
		ctl->pan_hotkeys[i] = OBS_INVALID_HOTKEY_ID;
	for (size_t i = 0; i < WARP_ZOOM_SLOTS; i++)
		ctl->slot_hotkeys[i] = OBS_INVALID_HOTKEY_ID;

	pthread_mutex_init(&ctl->mutex, NULL);

	da_init(ctl->presets);
	da_init(ctl->orphans);

	warp_zoom_add_default(ctl);
	warp_zoom_register_hotkeys(ctl);
}

void warp_zoom_control_free(struct warp_zoom_control *ctl)
{
	for (size_t i = 0; i < ctl->presets.num; i++) {
		if (ctl->presets.array[i].hotkey != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(ctl->presets.array[i].hotkey);

		warp_zoom_preset_free(&ctl->presets.array[i]);
	}

	for (size_t i = 0; i < ctl->orphans.num; i++)
		obs_hotkey_unregister(ctl->orphans.array[i]);

	da_free(ctl->presets);
	da_free(ctl->orphans);

	pthread_mutex_destroy(&ctl->mutex);
}

void warp_zoom_control_defaults(obs_data_t *settings, bool with_view)
{
	obs_data_set_default_int(settings, WARP_ZOOM_S_GLIDE, WARP_ZOOM_GLIDE_MS);
	obs_data_set_default_int(settings, WARP_ZOOM_S_NUDGE, WARP_ZOOM_NUDGE_MS);
	obs_data_set_default_bool(settings, WARP_ZOOM_S_CONFIRM, false);

	if (!with_view)
		return;

	obs_data_set_default_double(settings, WARP_ZOOM_S_ZOOM, 100.0);
	obs_data_set_default_double(settings, WARP_ZOOM_S_X, 50.0);
	obs_data_set_default_double(settings, WARP_ZOOM_S_Y, 50.0);
}

/* one preset out of the array a source was saved with; expects ctl->mutex to
 * be held */
static void warp_zoom_load_preset(struct warp_zoom_control *ctl, obs_data_t *item)
{
	struct warp_zoom_preset preset = {0};
	const char *id = obs_data_get_string(item, WARP_ZOOM_P_ID);
	const char *name = obs_data_get_string(item, WARP_ZOOM_P_NAME);

	/* the reset position is not taken from the settings: it is added
	 * first, and is what it is whatever a collection was saved with */
	if (id && strcmp(id, WARP_ZOOM_DEFAULT_ID) == 0)
		return;

	if (ctl->presets.num >= WARP_ZOOM_MAX_PRESETS)
		return;

	preset.id = id && *id ? bstrdup(id) : warp_zoom_make_id();
	preset.name = warp_zoom_unique_name(ctl, name, preset.id);
	preset.view.zoom = (float)obs_data_get_double(item, WARP_ZOOM_P_ZOOM);
	preset.view.x = (float)obs_data_get_double(item, WARP_ZOOM_P_X);
	preset.view.y = (float)obs_data_get_double(item, WARP_ZOOM_P_Y);
	preset.glide_ms = (uint32_t)obs_data_get_int(item, WARP_ZOOM_P_GLIDE);
	preset.hotkey = OBS_INVALID_HOTKEY_ID;

	warp_zoom_clamp(&preset.view);

	if (preset.glide_ms > WARP_ZOOM_GLIDE_MAX)
		preset.glide_ms = WARP_ZOOM_GLIDE_MAX;

	da_push_back(ctl->presets, &preset);
}

void warp_zoom_control_update(struct warp_zoom_control *ctl, obs_data_t *settings)
{
	obs_data_array_t *presets = obs_data_get_array(settings, WARP_ZOOM_S_PRESETS);
	uint64_t rev = (uint64_t)obs_data_get_int(settings, WARP_ZOOM_S_PRESETS_REV);
	size_t count = presets ? obs_data_array_count(presets) : 0;
	bool confirm = obs_data_get_bool(settings, WARP_ZOOM_S_CONFIRM);
	int glide = (int)obs_data_get_int(settings, WARP_ZOOM_S_GLIDE);
	int nudge = (int)obs_data_get_int(settings, WARP_ZOOM_S_NUDGE);
	struct warp_zoom_view view = warp_zoom_default_view();

	if (glide < 0)
		glide = 0;
	else if (glide > WARP_ZOOM_GLIDE_MAX)
		glide = WARP_ZOOM_GLIDE_MAX;

	if (nudge < 0)
		nudge = 0;
	else if (nudge > WARP_ZOOM_GLIDE_MAX)
		nudge = WARP_ZOOM_GLIDE_MAX;

	if (ctl->persist_view) {
		view.zoom = (float)obs_data_get_double(settings, WARP_ZOOM_S_ZOOM) / 100.0f;
		view.x = (float)obs_data_get_double(settings, WARP_ZOOM_S_X) / 100.0f;
		view.y = (float)obs_data_get_double(settings, WARP_ZOOM_S_Y) / 100.0f;
		warp_zoom_clamp(&view);
	}

	pthread_mutex_lock(&ctl->mutex);

	ctl->glide_ms = (uint32_t)glide;
	ctl->nudge_ms = (uint32_t)nudge;

	/* The presets are rebuilt when the settings carry an array of them and
	 * that array is not one from before an edit the dock made: an update
	 * that leaves them out, or that hands back what the source held before
	 * a preset was added, keeps what is there. */
	/* Confirm mode is set from the dock as well as from the properties, so
	 * it is guarded by the same revision the presets are: settings from
	 * before the dock changed it are the stale copy they look like. */
	if (rev >= ctl->presets_rev && ctl->confirm != confirm) {
		ctl->confirm = confirm;
		ctl->revision++;

		/* switching confirm mode off drops what was waiting rather
		 * than putting it on screen */
		if (!confirm && ctl->staged) {
			ctl->staged = false;
			warp_zoom_arm_stage_signal(ctl, &ctl->target, false, WARP_ZOOM_CHANGE_SET, NULL);
		}
	}

	if (presets && rev >= ctl->presets_rev) {
		for (size_t i = 0; i < ctl->presets.num; i++) {
			if (ctl->presets.array[i].hotkey != OBS_INVALID_HOTKEY_ID)
				da_push_back(ctl->orphans, &ctl->presets.array[i].hotkey);

			warp_zoom_preset_free(&ctl->presets.array[i]);
		}

		da_resize(ctl->presets, 0);
		warp_zoom_add_default(ctl);

		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(presets, i);

			warp_zoom_load_preset(ctl, item);
			obs_data_release(item);
		}

		ctl->presets_rev = rev;
		ctl->revision++;
	}

	/* the framing is a setting like any other on a source that keeps it, so
	 * typing a zoom into the properties moves the picture */
	if (ctl->persist_view && !warp_zoom_view_equal(&view, &ctl->target))
		warp_zoom_aim(ctl, &view, 0);

	warp_zoom_unlock(ctl);

	obs_data_array_release(presets);

	warp_zoom_sync_hotkeys(ctl);
}

void warp_zoom_control_save(struct warp_zoom_control *ctl, obs_data_t *settings)
{
	obs_data_array_t *presets = warp_zoom_control_preset_array(ctl);
	struct warp_zoom_view view;

	pthread_mutex_lock(&ctl->mutex);
	obs_data_set_int(settings, WARP_ZOOM_S_PRESETS_REV, (long long)ctl->presets_rev);
	obs_data_set_bool(settings, WARP_ZOOM_S_CONFIRM, ctl->confirm);
	pthread_mutex_unlock(&ctl->mutex);

	obs_data_set_array(settings, WARP_ZOOM_S_PRESETS, presets);
	obs_data_array_release(presets);

	if (!ctl->persist_view)
		return;

	/* what it is heading for rather than where it has got to: a source
	 * saved partway through a move is saved framed the way it was asked to
	 * be */
	warp_zoom_control_get(ctl, NULL, &view);

	obs_data_set_double(settings, WARP_ZOOM_S_ZOOM, view.zoom * 100.0);
	obs_data_set_double(settings, WARP_ZOOM_S_X, view.x * 100.0);
	obs_data_set_double(settings, WARP_ZOOM_S_Y, view.y * 100.0);
}

void warp_zoom_control_properties(obs_properties_t *props, bool with_view)
{
	obs_properties_t *group = obs_properties_create();
	obs_property_t *prop;

	if (with_view) {
		prop = obs_properties_add_float_slider(group, WARP_ZOOM_S_ZOOM, obs_module_text("Warp.Zoom.Zoom"),
						       WARP_ZOOM_MIN * 100.0, WARP_ZOOM_MAX * 100.0, 1.0);
		obs_property_float_set_suffix(prop, "%");
		obs_property_set_long_description(prop, obs_module_text("Warp.Zoom.Zoom.Desc"));

		prop = obs_properties_add_float_slider(group, WARP_ZOOM_S_X, obs_module_text("Warp.Zoom.CentreX"), 0.0,
						       100.0, 0.1);
		obs_property_float_set_suffix(prop, "%");

		prop = obs_properties_add_float_slider(group, WARP_ZOOM_S_Y, obs_module_text("Warp.Zoom.CentreY"), 0.0,
						       100.0, 0.1);
		obs_property_float_set_suffix(prop, "%");
	}

	prop = obs_properties_add_int_slider(group, WARP_ZOOM_S_GLIDE, obs_module_text("Warp.Zoom.Glide"), 0,
					     WARP_ZOOM_GLIDE_MAX, 10);
	obs_property_int_set_suffix(prop, " ms");
	obs_property_set_long_description(prop, obs_module_text("Warp.Zoom.Glide.Desc"));

	prop = obs_properties_add_int_slider(group, WARP_ZOOM_S_NUDGE, obs_module_text("Warp.Zoom.Nudge"), 0,
					     WARP_ZOOM_GLIDE_MAX, 10);
	obs_property_int_set_suffix(prop, " ms");
	obs_property_set_long_description(prop, obs_module_text("Warp.Zoom.Nudge.Desc"));

	prop = obs_properties_add_bool(group, WARP_ZOOM_S_CONFIRM, obs_module_text("Warp.Zoom.Confirm"));
	obs_property_set_long_description(prop, obs_module_text("Warp.Zoom.Confirm.Desc"));

	prop = obs_properties_add_text(group, "zoom_presets_note", obs_module_text("Warp.Zoom.Presets.Note"),
				       OBS_TEXT_INFO);
	obs_property_set_long_description(prop, obs_module_text("Warp.Zoom.Presets.Note.Desc"));

	obs_properties_add_group(props, "zoom_group", obs_module_text("Warp.Zoom.Group"), OBS_GROUP_NORMAL, group);
}

/* ------------------------------------------------------------------------- */
/* editing the presets */

/* Writes the framing back into the source's own settings, for a source that
 * keeps one: the dock and the hotkeys move the picture without going through
 * the properties, and a filter is meant to be left the way it was found.
 * Expects ctl->mutex NOT to be held. */
static void warp_zoom_write_view(struct warp_zoom_control *ctl)
{
	struct warp_zoom_view view;
	obs_data_t *settings;

	if (!ctl->persist_view || !ctl->source)
		return;

	settings = obs_source_get_settings(ctl->source);

	if (!settings)
		return;

	/* where it is heading rather than where it has got to, so a source
	 * saved partway through a move is saved framed the way it was asked to
	 * be rather than halfway there */
	warp_zoom_control_get(ctl, NULL, &view);

	obs_data_set_double(settings, WARP_ZOOM_S_ZOOM, view.zoom * 100.0);
	obs_data_set_double(settings, WARP_ZOOM_S_X, view.x * 100.0);
	obs_data_set_double(settings, WARP_ZOOM_S_Y, view.y * 100.0);

	obs_data_release(settings);
}

/* Writes the presets back the same way, along with the count of the edits they
 * have had: a properties window that was opened before a preset was made hands
 * back the settings as they were, and the count is what tells that apart from
 * settings being loaded. Expects ctl->mutex NOT to be held. */
static void warp_zoom_write_back(struct warp_zoom_control *ctl)
{
	obs_data_array_t *presets;
	obs_data_t *settings;

	if (!ctl->source)
		return;

	settings = obs_source_get_settings(ctl->source);

	if (!settings)
		return;

	presets = warp_zoom_control_preset_array(ctl);

	pthread_mutex_lock(&ctl->mutex);
	obs_data_set_int(settings, WARP_ZOOM_S_PRESETS_REV, (long long)ctl->presets_rev);
	pthread_mutex_unlock(&ctl->mutex);

	obs_data_set_array(settings, WARP_ZOOM_S_PRESETS, presets);

	obs_data_array_release(presets);
	obs_data_release(settings);

	warp_zoom_write_view(ctl);
}

char *warp_zoom_control_add_preset(struct warp_zoom_control *ctl, const char *name, const struct warp_zoom_view *view,
				   int glide_ms)
{
	struct warp_zoom_preset preset = {0};
	char *id;

	pthread_mutex_lock(&ctl->mutex);

	if (ctl->presets.num >= WARP_ZOOM_MAX_PRESETS) {
		pthread_mutex_unlock(&ctl->mutex);
		return NULL;
	}

	preset.id = warp_zoom_make_id();
	preset.name = warp_zoom_unique_name(ctl, name, preset.id);
	/* Nothing handed over means the framing being looked at, which is how
	 * the dock keeps a shot that has just been found: the one lined up when
	 * there is one, and what the source is set to otherwise. */
	preset.view = view ? *view : warp_zoom_base(ctl);
	preset.glide_ms = glide_ms > 0 ? (uint32_t)glide_ms : 0;
	preset.hotkey = OBS_INVALID_HOTKEY_ID;

	warp_zoom_clamp(&preset.view);

	if (preset.glide_ms > WARP_ZOOM_GLIDE_MAX)
		preset.glide_ms = WARP_ZOOM_GLIDE_MAX;

	da_push_back(ctl->presets, &preset);

	id = bstrdup(preset.id);
	ctl->revision++;
	ctl->presets_rev++;

	pthread_mutex_unlock(&ctl->mutex);

	warp_zoom_sync_hotkeys(ctl);
	warp_zoom_write_back(ctl);

	return id;
}

bool warp_zoom_control_update_preset(struct warp_zoom_control *ctl, const char *id, const char *name,
				     const struct warp_zoom_view *view, int glide_ms)
{
	struct warp_zoom_preset *preset;
	bool renamed = false;

	pthread_mutex_lock(&ctl->mutex);

	preset = warp_zoom_find(ctl, id);

	/* the reset position is the one thing that is always there and always
	 * the whole picture; nothing may write over it */
	if (!preset || warp_zoom_is_default(preset)) {
		pthread_mutex_unlock(&ctl->mutex);
		return false;
	}

	if (name && *name && strcmp(preset->name, name) != 0) {
		char *unique = warp_zoom_unique_name(ctl, name, preset->id);

		bfree(preset->name);
		preset->name = unique;
		renamed = true;
	}

	if (view) {
		preset->view = *view;
		warp_zoom_clamp(&preset->view);
	}

	if (glide_ms >= 0)
		preset->glide_ms = (uint32_t)(glide_ms > WARP_ZOOM_GLIDE_MAX ? WARP_ZOOM_GLIDE_MAX : glide_ms);

	ctl->revision++;
	ctl->presets_rev++;

	pthread_mutex_unlock(&ctl->mutex);

	if (renamed)
		warp_zoom_sync_hotkeys(ctl);

	warp_zoom_write_back(ctl);

	return true;
}

bool warp_zoom_control_remove_preset(struct warp_zoom_control *ctl, const char *id)
{
	struct warp_zoom_preset *preset;
	size_t index;

	pthread_mutex_lock(&ctl->mutex);

	preset = warp_zoom_find(ctl, id);

	if (!preset || warp_zoom_is_default(preset)) {
		pthread_mutex_unlock(&ctl->mutex);
		return false;
	}

	index = (size_t)(preset - ctl->presets.array);

	if (preset->hotkey != OBS_INVALID_HOTKEY_ID)
		da_push_back(ctl->orphans, &preset->hotkey);

	warp_zoom_preset_free(preset);
	da_erase(ctl->presets, index);
	ctl->revision++;
	ctl->presets_rev++;

	pthread_mutex_unlock(&ctl->mutex);

	warp_zoom_sync_hotkeys(ctl);
	warp_zoom_write_back(ctl);

	return true;
}

bool warp_zoom_control_move_preset(struct warp_zoom_control *ctl, const char *id, int delta)
{
	struct warp_zoom_preset *preset;
	struct warp_zoom_preset moved;
	size_t index;
	size_t to;

	pthread_mutex_lock(&ctl->mutex);

	preset = warp_zoom_find(ctl, id);

	if (!preset || warp_zoom_is_default(preset) || !delta) {
		pthread_mutex_unlock(&ctl->mutex);
		return false;
	}

	index = (size_t)(preset - ctl->presets.array);

	/* the reset position stays at the top, so nothing moves above it */
	if (delta < 0 && index + (size_t)(-delta) <= 1) {
		to = 1;
	} else if (delta < 0) {
		to = index - (size_t)(-delta);
	} else {
		to = index + (size_t)delta;

		if (to >= ctl->presets.num)
			to = ctl->presets.num - 1;
	}

	if (to == index) {
		pthread_mutex_unlock(&ctl->mutex);
		return false;
	}

	moved = ctl->presets.array[index];
	da_erase(ctl->presets, index);
	da_insert(ctl->presets, to, &moved);
	ctl->revision++;
	ctl->presets_rev++;

	pthread_mutex_unlock(&ctl->mutex);

	warp_zoom_write_back(ctl);

	return true;
}

obs_data_array_t *warp_zoom_control_preset_array(struct warp_zoom_control *ctl)
{
	obs_data_array_t *array = obs_data_array_create();

	pthread_mutex_lock(&ctl->mutex);

	for (size_t i = 0; i < ctl->presets.num; i++) {
		const struct warp_zoom_preset *preset = &ctl->presets.array[i];
		obs_data_t *item = obs_data_create();

		obs_data_set_string(item, WARP_ZOOM_P_ID, preset->id);
		obs_data_set_string(item, WARP_ZOOM_P_NAME, preset->name);
		obs_data_set_double(item, WARP_ZOOM_P_ZOOM, preset->view.zoom);
		obs_data_set_double(item, WARP_ZOOM_P_X, preset->view.x);
		obs_data_set_double(item, WARP_ZOOM_P_Y, preset->view.y);
		obs_data_set_int(item, WARP_ZOOM_P_GLIDE, preset->glide_ms);

		if (i == 0)
			obs_data_set_bool(item, WARP_ZOOM_P_FIXED, true);

		obs_data_array_push_back(array, item);
		obs_data_release(item);
	}

	pthread_mutex_unlock(&ctl->mutex);

	return array;
}

/* ------------------------------------------------------------------------- */
/* procs
 *
 * What everything outside the source drives it through. They are the same on
 * every zoom-capable source, so the dock, the websocket requests and a Warp
 * Detection filter do not have to know which kind of source they are holding. */

static int warp_zoom_glide_field(calldata_t *cd)
{
	long long glide;

	/* nothing asked for means the source's own preset glide, which is what
	 * makes a move from a control surface look like a move from a hotkey */
	if (!calldata_get_int(cd, "glide", &glide))
		return -1;

	return (int)glide;
}

static void warp_zoom_set_proc(void *data, calldata_t *cd)
{
	struct warp_zoom_control *ctl = data;
	struct warp_zoom_view view;
	double zoom;
	double x;
	double y;

	warp_zoom_control_get(ctl, NULL, &view);

	if (calldata_get_float(cd, "zoom", &zoom))
		view.zoom = (float)zoom;
	if (calldata_get_float(cd, "x", &x))
		view.x = (float)x;
	if (calldata_get_float(cd, "y", &y))
		view.y = (float)y;

	warp_zoom_control_set(ctl, &view, warp_zoom_glide_field(cd), WARP_ZOOM_CHANGE_SET, NULL);
}

static void warp_zoom_get_proc(void *data, calldata_t *cd)
{
	struct warp_zoom_control *ctl = data;
	struct warp_zoom_view view;
	struct warp_zoom_view target;
	struct warp_zoom_view stage;
	bool staged;

	warp_zoom_control_get(ctl, &view, &target);
	staged = warp_zoom_control_staged(ctl, &stage);

	calldata_set_float(cd, "zoom", view.zoom);
	calldata_set_float(cd, "x", view.x);
	calldata_set_float(cd, "y", view.y);
	calldata_set_float(cd, "target_zoom", target.zoom);
	calldata_set_float(cd, "target_x", target.x);
	calldata_set_float(cd, "target_y", target.y);

	/* what is waiting behind it, so a control surface can show the shot
	 * that is lined up as well as the one that is on */
	calldata_set_bool(cd, "confirm", warp_zoom_control_confirm(ctl));
	calldata_set_bool(cd, "staged", staged);
	calldata_set_float(cd, "staged_zoom", stage.zoom);
	calldata_set_float(cd, "staged_x", stage.x);
	calldata_set_float(cd, "staged_y", stage.y);
}

static void warp_zoom_confirm_proc(void *data, calldata_t *cd)
{
	bool confirm;

	if (calldata_get_bool(cd, "confirm", &confirm))
		warp_zoom_control_set_confirm(data, confirm);

	calldata_set_bool(cd, "state", warp_zoom_control_confirm(data));
}

static void warp_zoom_take_proc(void *data, calldata_t *cd)
{
	calldata_set_bool(cd, "taken", warp_zoom_control_take(data));
}

static void warp_zoom_drop_proc(void *data, calldata_t *cd)
{
	calldata_set_bool(cd, "dropped", warp_zoom_control_drop(data));
}

static void warp_zoom_adjust_proc(void *data, calldata_t *cd)
{
	double factor;

	if (calldata_get_float(cd, "factor", &factor))
		warp_zoom_control_adjust(data, (float)factor, warp_zoom_glide_field(cd));
}

static void warp_zoom_pan_proc(void *data, calldata_t *cd)
{
	double dx = 0.0;
	double dy = 0.0;

	calldata_get_float(cd, "dx", &dx);
	calldata_get_float(cd, "dy", &dy);

	if (dx != 0.0 || dy != 0.0)
		warp_zoom_control_pan(data, (float)dx, (float)dy, warp_zoom_glide_field(cd));
}

static void warp_zoom_reset_proc(void *data, calldata_t *cd)
{
	warp_zoom_control_reset(data, warp_zoom_glide_field(cd));
}

static void warp_zoom_recall_proc(void *data, calldata_t *cd)
{
	struct warp_zoom_control *ctl = data;
	const char *preset = calldata_string(cd, "preset");
	long long slot;
	bool found;

	if (preset && *preset)
		found = warp_zoom_control_recall(ctl, preset);
	else if (calldata_get_int(cd, "slot", &slot) && slot > 0)
		found = warp_zoom_control_recall_slot(ctl, (size_t)slot);
	else
		found = false;

	calldata_set_bool(cd, "found", found);
}

static void warp_zoom_save_preset_proc(void *data, calldata_t *cd)
{
	char *id = warp_zoom_control_add_preset(data, calldata_string(cd, "name"), NULL, 0);

	calldata_set_string(cd, "id", id ? id : "");
	bfree(id);
}

static void warp_zoom_update_preset_proc(void *data, calldata_t *cd)
{
	struct warp_zoom_control *ctl = data;
	struct warp_zoom_view view;
	bool has_view = false;
	long long glide;
	double value;

	warp_zoom_control_get(ctl, NULL, &view);

	if (calldata_get_float(cd, "zoom", &value)) {
		view.zoom = (float)value;
		has_view = true;
	}
	if (calldata_get_float(cd, "x", &value)) {
		view.x = (float)value;
		has_view = true;
	}
	if (calldata_get_float(cd, "y", &value)) {
		view.y = (float)value;
		has_view = true;
	}

	calldata_set_bool(cd, "found",
			  warp_zoom_control_update_preset(ctl, calldata_string(cd, "id"), calldata_string(cd, "name"),
							  has_view ? &view : NULL,
							  calldata_get_int(cd, "glide", &glide) ? (int)glide : -1));
}

static void warp_zoom_remove_preset_proc(void *data, calldata_t *cd)
{
	calldata_set_bool(cd, "removed", warp_zoom_control_remove_preset(data, calldata_string(cd, "id")));
}

static void warp_zoom_move_preset_proc(void *data, calldata_t *cd)
{
	long long delta;

	calldata_set_bool(cd, "moved",
			  calldata_get_int(cd, "delta", &delta) &&
				  warp_zoom_control_move_preset(data, calldata_string(cd, "id"), (int)delta));
}

static void warp_zoom_presets_proc(void *data, calldata_t *cd)
{
	obs_data_array_t *array = warp_zoom_control_preset_array(data);
	obs_data_t *wrapper = obs_data_create();

	/* an array is not something a calldata can carry, so it goes over as
	 * the JSON the presets are saved as */
	obs_data_set_array(wrapper, WARP_ZOOM_S_PRESETS, array);
	calldata_set_string(cd, "presets", obs_data_get_json(wrapper));

	obs_data_release(wrapper);
	obs_data_array_release(array);
}

void warp_zoom_control_register_procs(struct warp_zoom_control *ctl, obs_source_t *source)
{
	proc_handler_t *ph = obs_source_get_proc_handler(source);

	proc_handler_add(ph, "void " WARP_ZOOM_PROC_SET "(float zoom, float x, float y, int glide)", warp_zoom_set_proc,
			 ctl);
	proc_handler_add(ph,
			 "void " WARP_ZOOM_PROC_GET "(out float zoom, out float x, out float y, out float target_zoom, "
			 "out float target_x, out float target_y, out bool confirm, out bool staged, "
			 "out float staged_zoom, out float staged_x, out float staged_y)",
			 warp_zoom_get_proc, ctl);
	proc_handler_add(ph, "void " WARP_ZOOM_PROC_ADJUST "(float factor, int glide)", warp_zoom_adjust_proc, ctl);
	proc_handler_add(ph, "void " WARP_ZOOM_PROC_PAN "(float dx, float dy, int glide)", warp_zoom_pan_proc, ctl);
	proc_handler_add(ph, "void " WARP_ZOOM_PROC_RESET "(int glide)", warp_zoom_reset_proc, ctl);
	proc_handler_add(ph, "void " WARP_ZOOM_PROC_RECALL "(string preset, int slot, out bool found)",
			 warp_zoom_recall_proc, ctl);
	proc_handler_add(ph, "void " WARP_ZOOM_PROC_SAVE_PRESET "(string name, out string id)",
			 warp_zoom_save_preset_proc, ctl);
	proc_handler_add(ph,
			 "void " WARP_ZOOM_PROC_UPDATE_PRESET
			 "(string id, string name, float zoom, float x, float y, int glide, out bool found)",
			 warp_zoom_update_preset_proc, ctl);
	proc_handler_add(ph, "void " WARP_ZOOM_PROC_REMOVE_PRESET "(string id, out bool removed)",
			 warp_zoom_remove_preset_proc, ctl);
	proc_handler_add(ph, "void " WARP_ZOOM_PROC_MOVE_PRESET "(string id, int delta, out bool moved)",
			 warp_zoom_move_preset_proc, ctl);
	proc_handler_add(ph, "void " WARP_ZOOM_PROC_PRESETS "(out string presets)", warp_zoom_presets_proc, ctl);
	proc_handler_add(ph, "void " WARP_ZOOM_PROC_CONFIRM "(bool confirm, out bool state)", warp_zoom_confirm_proc,
			 ctl);
	proc_handler_add(ph, "void " WARP_ZOOM_PROC_TAKE "(out bool taken)", warp_zoom_take_proc, ctl);
	proc_handler_add(ph, "void " WARP_ZOOM_PROC_DROP "(out bool dropped)", warp_zoom_drop_proc, ctl);
}

/* ------------------------------------------------------------------------- */
/* driving a source from outside it */

static bool warp_zoom_call(obs_source_t *source, const char *proc, calldata_t *cd)
{
	if (!source)
		return false;

	return proc_handler_call(obs_source_get_proc_handler(source), proc, cd);
}

bool warp_zoom_source_capable(obs_source_t *source)
{
	/* Whether the source answers warp_zoom_get at all, which is asked of
	 * every source in the collection while the dock's lists are looked
	 * over. The reply is thrown away, but it is still written, so it goes
	 * through the same call as any other read rather than on a stack of its
	 * own that would have to be kept the same size as this one. */
	return warp_zoom_source_get(source, NULL, NULL);
}

bool warp_zoom_source_get(obs_source_t *source, struct warp_zoom_view *view, struct warp_zoom_view *target)
{
	struct calldata cd;
	uint8_t stack[WARP_ZOOM_CD_GET];
	double value;

	calldata_init_fixed(&cd, stack, sizeof(stack));

	if (!warp_zoom_call(source, WARP_ZOOM_PROC_GET, &cd))
		return false;

	if (view) {
		*view = warp_zoom_default_view();

		if (calldata_get_float(&cd, "zoom", &value))
			view->zoom = (float)value;
		if (calldata_get_float(&cd, "x", &value))
			view->x = (float)value;
		if (calldata_get_float(&cd, "y", &value))
			view->y = (float)value;
	}

	if (target) {
		*target = warp_zoom_default_view();

		if (calldata_get_float(&cd, "target_zoom", &value))
			target->zoom = (float)value;
		if (calldata_get_float(&cd, "target_x", &value))
			target->x = (float)value;
		if (calldata_get_float(&cd, "target_y", &value))
			target->y = (float)value;
	}

	return true;
}

void warp_zoom_source_set(obs_source_t *source, const struct warp_zoom_view *view, int glide_ms)
{
	struct calldata cd;
	uint8_t stack[WARP_ZOOM_CD_SET];

	calldata_init_fixed(&cd, stack, sizeof(stack));
	calldata_set_float(&cd, "zoom", view->zoom);
	calldata_set_float(&cd, "x", view->x);
	calldata_set_float(&cd, "y", view->y);
	calldata_set_int(&cd, "glide", glide_ms);

	warp_zoom_call(source, WARP_ZOOM_PROC_SET, &cd);
}

void warp_zoom_source_adjust(obs_source_t *source, float factor, int glide_ms)
{
	struct calldata cd;
	uint8_t stack[WARP_ZOOM_CD_NUDGE];

	calldata_init_fixed(&cd, stack, sizeof(stack));
	calldata_set_float(&cd, "factor", factor);
	calldata_set_int(&cd, "glide", glide_ms);

	warp_zoom_call(source, WARP_ZOOM_PROC_ADJUST, &cd);
}

void warp_zoom_source_pan(obs_source_t *source, float dx, float dy, int glide_ms)
{
	struct calldata cd;
	uint8_t stack[WARP_ZOOM_CD_NUDGE];

	calldata_init_fixed(&cd, stack, sizeof(stack));
	calldata_set_float(&cd, "dx", dx);
	calldata_set_float(&cd, "dy", dy);
	calldata_set_int(&cd, "glide", glide_ms);

	warp_zoom_call(source, WARP_ZOOM_PROC_PAN, &cd);
}

void warp_zoom_source_reset(obs_source_t *source, int glide_ms)
{
	struct calldata cd;
	uint8_t stack[WARP_ZOOM_CD_GLIDE];

	calldata_init_fixed(&cd, stack, sizeof(stack));
	calldata_set_int(&cd, "glide", glide_ms);

	warp_zoom_call(source, WARP_ZOOM_PROC_RESET, &cd);
}

bool warp_zoom_source_recall(obs_source_t *source, const char *id_or_name)
{
	/* A preset is named by whoever recalls it, so there is no length to set
	 * a stack aside for: this one is allocated. Recalling a shot is
	 * something somebody does, not something that happens every frame, so
	 * the allocation costs nothing worth saving. */
	calldata_t cd;
	bool found = false;

	calldata_init(&cd);
	calldata_set_string(&cd, "preset", id_or_name ? id_or_name : "");

	if (warp_zoom_call(source, WARP_ZOOM_PROC_RECALL, &cd))
		calldata_get_bool(&cd, "found", &found);

	calldata_free(&cd);

	return found;
}

bool warp_zoom_source_recall_slot(obs_source_t *source, int slot)
{
	struct calldata cd;
	uint8_t stack[WARP_ZOOM_CD_SLOT];
	bool found = false;

	calldata_init_fixed(&cd, stack, sizeof(stack));
	calldata_set_string(&cd, "preset", "");
	calldata_set_int(&cd, "slot", slot);

	if (warp_zoom_call(source, WARP_ZOOM_PROC_RECALL, &cd))
		calldata_get_bool(&cd, "found", &found);

	return found;
}

bool warp_zoom_source_stage(obs_source_t *source, struct warp_zoom_view *staged, bool *is_staged, bool *confirm)
{
	struct calldata cd;
	uint8_t stack[WARP_ZOOM_CD_GET];
	double value;
	bool flag = false;

	calldata_init_fixed(&cd, stack, sizeof(stack));

	if (!warp_zoom_call(source, WARP_ZOOM_PROC_GET, &cd))
		return false;

	if (staged) {
		*staged = warp_zoom_default_view();

		if (calldata_get_float(&cd, "staged_zoom", &value))
			staged->zoom = (float)value;
		if (calldata_get_float(&cd, "staged_x", &value))
			staged->x = (float)value;
		if (calldata_get_float(&cd, "staged_y", &value))
			staged->y = (float)value;
	}

	if (is_staged) {
		calldata_get_bool(&cd, "staged", &flag);
		*is_staged = flag;
	}

	if (confirm) {
		flag = false;
		calldata_get_bool(&cd, "confirm", &flag);
		*confirm = flag;
	}

	return true;
}

void warp_zoom_source_set_confirm(obs_source_t *source, bool confirm)
{
	struct calldata cd;
	uint8_t stack[WARP_ZOOM_CD_FLAGS];

	calldata_init_fixed(&cd, stack, sizeof(stack));
	calldata_set_bool(&cd, "confirm", confirm);

	warp_zoom_call(source, WARP_ZOOM_PROC_CONFIRM, &cd);
}

bool warp_zoom_source_take(obs_source_t *source)
{
	struct calldata cd;
	uint8_t stack[WARP_ZOOM_CD_FLAGS];
	bool taken = false;

	calldata_init_fixed(&cd, stack, sizeof(stack));

	if (warp_zoom_call(source, WARP_ZOOM_PROC_TAKE, &cd))
		calldata_get_bool(&cd, "taken", &taken);

	return taken;
}

bool warp_zoom_source_drop(obs_source_t *source)
{
	struct calldata cd;
	uint8_t stack[WARP_ZOOM_CD_FLAGS];
	bool dropped = false;

	calldata_init_fixed(&cd, stack, sizeof(stack));

	if (warp_zoom_call(source, WARP_ZOOM_PROC_DROP, &cd))
		calldata_get_bool(&cd, "dropped", &dropped);

	return dropped;
}

obs_data_array_t *warp_zoom_source_presets(obs_source_t *source)
{
	/* The whole preset list comes back as JSON, which is as long as the
	 * presets are: no stack can be set aside for that, so this one is
	 * allocated. */
	calldata_t cd;
	obs_data_array_t *array = NULL;
	obs_data_t *wrapper;
	const char *json;

	calldata_init(&cd);

	if (!warp_zoom_call(source, WARP_ZOOM_PROC_PRESETS, &cd)) {
		calldata_free(&cd);
		return NULL;
	}

	json = calldata_string(&cd, "presets");

	if (json && *json) {
		wrapper = obs_data_create_from_json(json);

		if (wrapper) {
			array = obs_data_get_array(wrapper, WARP_ZOOM_S_PRESETS);
			obs_data_release(wrapper);
		}
	}

	calldata_free(&cd);

	return array;
}

char *warp_zoom_source_save_preset(obs_source_t *source, const char *name)
{
	/* the name is typed by an operator and the id is made from it, so both
	 * are as long as they are */
	calldata_t cd;
	char *saved = NULL;
	const char *id;

	calldata_init(&cd);
	calldata_set_string(&cd, "name", name ? name : "");

	if (warp_zoom_call(source, WARP_ZOOM_PROC_SAVE_PRESET, &cd)) {
		id = calldata_string(&cd, "id");

		if (id && *id)
			saved = bstrdup(id);
	}

	calldata_free(&cd);

	return saved;
}

bool warp_zoom_source_update_preset(obs_source_t *source, const char *id, const char *name,
				    const struct warp_zoom_view *view, int glide_ms)
{
	/* names and ids again, so this one is allocated too */
	calldata_t cd;
	bool found = false;

	calldata_init(&cd);
	calldata_set_string(&cd, "id", id ? id : "");
	calldata_set_string(&cd, "name", name ? name : "");

	if (view) {
		calldata_set_float(&cd, "zoom", view->zoom);
		calldata_set_float(&cd, "x", view->x);
		calldata_set_float(&cd, "y", view->y);
	}

	if (glide_ms >= 0)
		calldata_set_int(&cd, "glide", glide_ms);

	if (warp_zoom_call(source, WARP_ZOOM_PROC_UPDATE_PRESET, &cd))
		calldata_get_bool(&cd, "found", &found);

	calldata_free(&cd);

	return found;
}

bool warp_zoom_source_remove_preset(obs_source_t *source, const char *id)
{
	calldata_t cd;
	bool removed = false;

	calldata_init(&cd);
	calldata_set_string(&cd, "id", id ? id : "");

	if (warp_zoom_call(source, WARP_ZOOM_PROC_REMOVE_PRESET, &cd))
		calldata_get_bool(&cd, "removed", &removed);

	calldata_free(&cd);

	return removed;
}

bool warp_zoom_source_move_preset(obs_source_t *source, const char *id, int delta)
{
	calldata_t cd;
	bool moved = false;

	calldata_init(&cd);
	calldata_set_string(&cd, "id", id ? id : "");
	calldata_set_int(&cd, "delta", delta);

	if (warp_zoom_call(source, WARP_ZOOM_PROC_MOVE_PRESET, &cd))
		calldata_get_bool(&cd, "moved", &moved);

	calldata_free(&cd);

	return moved;
}
