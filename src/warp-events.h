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

/* ids of the sources that emit the events below */
#define WARP_MEDIA_SOURCE_ID "warp_media_source"
#define WARP_PLAYLIST_SOURCE_ID "warp_playlist_source"

/* number of percentage points each speed up/down hotkey press applies */
#define WARP_SPEED_STEP 10

/* The speeds the preset hotkeys set, and the frame counts the stepping hotkeys
 * step by. Both sources register their hotkeys from these, and the Warp
 * Detection filter builds one event per hotkey from them, so a hotkey added
 * here turns up in all three places. Kept in ascending order: the filter lists
 * its events in it. */
#define WARP_SPEED_PRESET_LIST 25, 50, 125, 150, 200
#define WARP_NUM_SPEED_PRESETS 5

#define WARP_STEP_COUNT_LIST 1, 5, 10, 20
#define WARP_NUM_STEP_COUNTS 4
#define WARP_NUM_STEP_HOTKEYS (WARP_NUM_STEP_COUNTS * 2)

/* Signals both Warp sources emit as playback is controlled, listened for by
 * the Warp Detection filter (and available to scripts through the source's
 * signal handler):
 *
 *   warp_speed_changed(ptr source, int speed, int prev_speed, string change)
 *   warp_frames_stepped(ptr source, int frames)
 *   warp_media_action(ptr source, string action)
 *
 * 'change' says how the speed got to its new value; 'frames' is the signed
 * frame count of a step, negative when stepping backward; 'action' is the
 * playback command that was carried out.
 *
 * Speed a file starts at is not a change: moving to the next file in a
 * playlist resets its speed without emitting anything. */
#define WARP_SIGNAL_SPEED_CHANGED "warp_speed_changed"
#define WARP_SIGNAL_FRAMES_STEPPED "warp_frames_stepped"
#define WARP_SIGNAL_MEDIA_ACTION "warp_media_action"

#define WARP_SIGNAL_DECL_SPEED_CHANGED \
	"void warp_speed_changed(ptr source, int speed, int prev_speed, string change)"
#define WARP_SIGNAL_DECL_FRAMES_STEPPED "void warp_frames_stepped(ptr source, int frames)"
#define WARP_SIGNAL_DECL_MEDIA_ACTION "void warp_media_action(ptr source, string action)"

/* values of the 'change' field: the speed was set to a value outright (a preset
 * hotkey, Reset Speed, or the Speed property), or stepped by WARP_SPEED_STEP */
#define WARP_SPEED_CHANGE_SET "set"
#define WARP_SPEED_CHANGE_INCREASED "increased"
#define WARP_SPEED_CHANGE_DECREASED "decreased"

/* values of the 'action' field: playback was started or resumed, paused, or
 * restarted from the top; or a file was put in the source from outside it,
 * which is what a Warp Instant Replay flow does when a clip lands */
#define WARP_MEDIA_ACTION_PLAY "play"
#define WARP_MEDIA_ACTION_PAUSE "pause"
#define WARP_MEDIA_ACTION_RESTART "restart"
#define WARP_MEDIA_ACTION_LOADED "loaded"

/* Puts a file in a Warp Media source, as a proc on the source itself:
 *
 *   warp_media_load(string path, int speed, string playback)
 *
 * The source is switched to that local file, played at 'speed' percent when
 * that is anything other than zero, and left doing whatever 'playback' says.
 * The loaded action above is emitted once the file is in, whichever way
 * playback was left, so a Warp Detection filter can bring the source on screen
 * the moment a clip arrives.
 *
 * This is what a Warp Instant Replay flow calls; it is a proc rather than a
 * settings change so that holding a clip on its first frame - which has to
 * wait for the file to open and decode - is done by the source, on its own
 * tick, rather than by whoever handed the clip over. */
#define WARP_MEDIA_LOAD_PROC "warp_media_load"

/* what playback does with a file that has just been loaded:
 *
 *   keep - nothing beyond loading it: the source's own settings decide, the
 *          same way they do when the file is changed from the properties. A
 *          source that is on screen plays the clip; one that is not waits, and
 *          plays it when it is brought on if it restarts on activate.
 *   play - starts the clip from the top there and then, on screen or not.
 *   hold - parks the clip on its first frame and leaves it there, for an
 *          operator to start when they are ready. A source set to restart when
 *          it becomes active still restarts as it is brought on screen, so a
 *          clip that is to stay held through the reveal wants that setting off. */
#define WARP_MEDIA_LOAD_KEEP "keep"
#define WARP_MEDIA_LOAD_PLAY "play"
#define WARP_MEDIA_LOAD_HOLD "hold"

static inline void warp_signal_speed_changed(obs_source_t *source, int speed, int prev_speed, const char *change)
{
	struct calldata cd;
	uint8_t stack[256];

	calldata_init_fixed(&cd, stack, sizeof(stack));
	calldata_set_ptr(&cd, "source", source);
	calldata_set_int(&cd, "speed", speed);
	calldata_set_int(&cd, "prev_speed", prev_speed);
	calldata_set_string(&cd, "change", change);

	signal_handler_signal(obs_source_get_signal_handler(source), WARP_SIGNAL_SPEED_CHANGED, &cd);
}

static inline void warp_signal_frames_stepped(obs_source_t *source, int frames)
{
	struct calldata cd;
	uint8_t stack[128];

	calldata_init_fixed(&cd, stack, sizeof(stack));
	calldata_set_ptr(&cd, "source", source);
	calldata_set_int(&cd, "frames", frames);

	signal_handler_signal(obs_source_get_signal_handler(source), WARP_SIGNAL_FRAMES_STEPPED, &cd);
}

static inline void warp_signal_media_action(obs_source_t *source, const char *action)
{
	struct calldata cd;
	uint8_t stack[128];

	calldata_init_fixed(&cd, stack, sizeof(stack));
	calldata_set_ptr(&cd, "source", source);
	calldata_set_string(&cd, "action", action);

	signal_handler_signal(obs_source_get_signal_handler(source), WARP_SIGNAL_MEDIA_ACTION, &cd);
}
