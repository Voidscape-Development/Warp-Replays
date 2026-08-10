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

#ifdef __cplusplus
extern "C" {
#endif

/* A flow takes the clips the OBS replay buffer writes and hands them to a Warp
 * source, so a feed builds itself as an event goes on. What it does with a clip
 * is all it is: which source it feeds, how that source is left, and which other
 * flows are fed the same clip.
 *
 * Flows are configured entirely through obs_data_t objects, which are also
 * what they are saved as: the keys below are both the fields the UI fills in
 * and the fields written to the scene collection. */

#define WARP_FLOW_ID "id"
#define WARP_FLOW_NAME "name"
#define WARP_FLOW_KIND "kind"
#define WARP_FLOW_TRIGGER "trigger"
#define WARP_FLOW_ORDER "order"
#define WARP_FLOW_TARGET_UUID "target_uuid"
#define WARP_FLOW_TARGET_NAME "target_name"
#define WARP_FLOW_MAX_CLIPS "max_clips"
#define WARP_FLOW_ENABLED "enabled"
#define WARP_FLOW_LINKS "links"
#define WARP_FLOW_CLIPS_ADDED "clips_added"
/* instant replay flows only: what playback does with a clip that lands, one of
 * the WARP_MEDIA_LOAD_* values of warp-events.h, and the speed to play it at in
 * percent, with zero leaving the source's own speed alone */
#define WARP_FLOW_PLAYBACK "playback"
#define WARP_FLOW_SPEED "speed"

/* The range the speed above takes, which is the range a Warp source plays at;
 * warp-flow.c checks the two against each other. Zero, underneath the range,
 * is the flow leaving the source's own speed alone. */
#define WARP_FLOW_SPEED_MIN 10
#define WARP_FLOW_SPEED_MAX 400

/* items of the WARP_FLOW_LINKS array carry the id of the flow they link to */
#define WARP_FLOW_LINK_ID "id"

/* What the flow is for.
 *
 *   replay, highlight - a list in a Warp Playlist source. Both work the same
 *                       way; the kind is what the flow is called in the UI, and
 *                       what a Combo list pairs up.
 *   instant           - a Warp Media source holding one clip: the one that was
 *                       saved last, loaded the moment it lands, so it can be
 *                       brought on screen over what is happening now. There is
 *                       no list, so order and limit mean nothing to it, and
 *                       what it feeds is a Warp Media source rather than a
 *                       Warp Playlist one. */
#define WARP_FLOW_KIND_REPLAY "replay"
#define WARP_FLOW_KIND_HIGHLIGHT "highlight"
#define WARP_FLOW_KIND_INSTANT "instant"

/* Where clips come from:
 *
 *   hotkey - the flow's own Save Replay hotkey saves the replay buffer and
 *            keeps the clip for itself. Saves made any other way go past it.
 *   listen - the flow also takes every replay buffer save nobody claimed:
 *            OBS's own Save Replay hotkey, obs-websocket, a script.
 *
 * A flow's Save Replay hotkey works either way. What the setting decides is
 * whether saves the flow did not ask for land in it as well. */
#define WARP_FLOW_TRIGGER_HOTKEY "hotkey"
#define WARP_FLOW_TRIGGER_LISTEN "listen"

/* Which end of the playlist a clip is added to: oldest first appends, so the
 * list plays in the order the clips were saved; newest first inserts at the
 * top, so the clip that was just saved is the next one up. Means nothing to an
 * instant replay flow, which holds one clip rather than a list. */
#define WARP_FLOW_ORDER_OLDEST_FIRST "oldest_first"
#define WARP_FLOW_ORDER_NEWEST_FIRST "newest_first"

/* how far a chain of linked flows is followed before it is written off as a
 * loop */
#define WARP_FLOW_MAX_LINK_DEPTH 8

void warp_flow_init(void);
void warp_flow_shutdown(void);

/* Every call below is safe from any thread. The ones that change something are
 * meant for the UI thread, which is where the frontend hands the plugin its
 * events and where the Warp window runs. */

/* every flow, in the order they were made; the caller releases the array */
obs_data_array_t *warp_flow_list(void);
/* one flow's configuration, or NULL; the caller releases it */
obs_data_t *warp_flow_get(const char *id);
/* the flow with that name, or NULL; the caller releases it */
obs_data_t *warp_flow_get_by_name(const char *name);

/* Adds a flow configured by 'config' and answers with its id, which the caller
 * frees with bfree(). An id already in 'config' is kept, so a flow can be put
 * back the way it was; anything else gets one of its own. */
char *warp_flow_add(obs_data_t *config);
/* applies the fields of 'config' to the flow, leaving the rest alone */
bool warp_flow_update(const char *id, obs_data_t *config);
bool warp_flow_remove(const char *id);

/* Saves the replay buffer and keeps the clip for this flow. Answers false when
 * the flow is gone or the replay buffer is not running. */
bool warp_flow_save_replay(const char *id);
/* puts the clip the replay buffer saved last in this flow, without saving a
 * new one */
bool warp_flow_promote_last(const char *id);

/* Told when a flow's Save Replay hotkey is pressed and there is no replay
 * buffer running for it to save. The UI sets this so the press says so rather
 * than doing nothing, and can offer to start the buffer there and then; with
 * nothing set the press is only logged. Called on the UI thread. */
typedef void (*warp_flow_buffer_prompt_t)(const char *flow_name);
void warp_flow_set_buffer_prompt(warp_flow_buffer_prompt_t prompt);

bool warp_flow_replay_buffer_active(void);
/* the clip the replay buffer saved last, or NULL; the caller frees it */
char *warp_flow_last_clip(void);
/* How many clips the flow's source is holding, or -1 when it is gone. An
 * instant replay flow holds one clip at a time, so it answers 0 or 1. */
int warp_flow_clip_count(const char *id);

#ifdef __cplusplus
}
#endif
