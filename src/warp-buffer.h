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

/* A buffer is a moment held back, ready to be written out at whichever length
 * is asked for.
 *
 * OBS has one replay buffer, of one length, set up in its own settings. A Warp
 * buffer is the same idea with the two things a replay operator keeps reaching
 * for: several lengths on the same feed, so the last five seconds and the last
 * twenty are both a keypress away, and several angles of the same moment, so
 * the play can be watched again from the camera that saw it best.
 *
 * What one is made of:
 *
 *   angles  - the feeds it holds. Each is either the program feed or one
 *             source, and each is encoded on its own. The first angle is the
 *             primary one: it is the clip a flow is handed, and the one an
 *             operator starts watching before reaching for another camera.
 *   lengths - the seconds it can be asked for. Each length registers a hotkey
 *             of its own and keeps a buffer of its own behind every angle, so
 *             asking for five seconds writes five seconds rather than twenty
 *             trimmed down.
 *
 * A save takes every angle at once and hands the set to the angle register of
 * warp-angles.h, which is how the clips find each other again afterwards.
 *
 * Picture and sound come from the OBS profile's own recording settings -
 * encoder, quality, container and folder - so a buffer is set up by naming it
 * and saying what it holds, rather than by configuring a second recorder.
 *
 * Buffers are configured entirely through obs_data_t objects, which are also
 * what they are saved as: the keys below are both the fields the UI fills in
 * and the fields written to the scene collection. */

#define WARP_BUFFER_ID "id"
#define WARP_BUFFER_NAME "name"
#define WARP_BUFFER_ANGLES "angles"
#define WARP_BUFFER_LENGTHS "lengths"
/* start and stop with OBS's own replay buffer rather than on its own */
#define WARP_BUFFER_FOLLOW_OBS "follow_obs"
/* whether it was running when the scene collection was last written, so a
 * collection comes back up the way it was left */
#define WARP_BUFFER_ACTIVE "active"
/* the cap on what one length of one angle holds, in megabytes, which is the
 * same cap OBS puts on its own replay buffer; zero for no cap */
#define WARP_BUFFER_MAX_SIZE_MB "max_size_mb"

/* items of the WARP_BUFFER_LENGTHS array */
#define WARP_BUFFER_LENGTH_SECONDS "seconds"

/* items of the WARP_BUFFER_ANGLES array */
#define WARP_BUFFER_ANGLE_ID "id"
#define WARP_BUFFER_ANGLE_NAME "name"
#define WARP_BUFFER_ANGLE_FEED "feed"
#define WARP_BUFFER_ANGLE_SOURCE_UUID "source_uuid"
#define WARP_BUFFER_ANGLE_SOURCE_NAME "source_name"

/* What an angle holds:
 *
 *   program - what goes out of OBS, which is what the replay buffer has always
 *             held: the programme as the audience saw it.
 *   source  - one source, on its own, whatever scene it is in and whether or
 *             not it is on screen. This is the one that gives a replay its
 *             camera angles: a buffer with a player cam on each angle catches
 *             every one of them looking at the same moment. */
#define WARP_BUFFER_FEED_PROGRAM "program"
#define WARP_BUFFER_FEED_SOURCE "source"

/* The lengths a buffer will take, in seconds. The floor is a second because
 * anything shorter is not a replay; the ceiling is twenty minutes, well past
 * what a buffer is for, and there to stop a typo eating the machine's memory.
 *
 * Every length costs memory of its own behind every angle, since each keeps
 * its own seconds of encoded picture - warp_buffer_memory_estimate() works out
 * how much so the UI can say so before a buffer is started. */
#define WARP_BUFFER_LENGTH_MIN 1
#define WARP_BUFFER_LENGTH_MAX 1200
#define WARP_BUFFER_MAX_LENGTHS 8

/* the most angles one buffer holds, which is the most one save can write */
#define WARP_BUFFER_MAX_ANGLES 8

void warp_buffer_init(void);
void warp_buffer_shutdown(void);

/* Every call below is safe from any thread. The ones that change something are
 * meant for the UI thread, which is where the frontend hands the plugin its
 * events and where the Warp window runs. */

/* every buffer, in the order they were made; the caller releases the array */
obs_data_array_t *warp_buffer_list(void);
/* one buffer's configuration, or NULL; the caller releases it */
obs_data_t *warp_buffer_get(const char *id);
/* the buffer with that name, or NULL; the caller releases it */
obs_data_t *warp_buffer_get_by_name(const char *name);

/* Adds a buffer configured by 'config' and answers with its id, which the
 * caller frees with bfree(). An id already in 'config' is kept, so a buffer
 * can be put back the way it was; anything else gets one of its own. */
char *warp_buffer_add(obs_data_t *config);
/* Applies the fields of 'config' to the buffer, leaving the rest alone. A
 * buffer that is running is stopped and started again when what it holds
 * changes, since its angles and lengths are what it is made of. */
bool warp_buffer_update(const char *id, obs_data_t *config);
bool warp_buffer_remove(const char *id);

/* Starts a buffer: builds an encoder for every angle and a buffer for every
 * length behind each of them, and sets them running. Answers false when the
 * buffer is gone, has nothing to hold, or could not be set up - the reason is
 * written to the OBS log either way. Starting one that is already running does
 * nothing and answers true. */
bool warp_buffer_start(const char *id);
void warp_buffer_stop(const char *id);
bool warp_buffer_running(const char *id);

/* Writes 'seconds' out of every angle of the buffer at once. A length the
 * buffer does not hold is refused rather than rounded to one it does, and zero
 * asks for the first length it offers. 'claim_flow_id' is the flow the clip
 * belongs to, or NULL for a save nobody claimed; it is handed back to the clip
 * handler below when the files have been written. */
bool warp_buffer_save(const char *id, int seconds, const char *claim_flow_id);

/* Told when a save has finished writing every angle it was going to. 'path' is
 * the primary angle - the clip a flow takes - and the rest of the set is in
 * the angle register by then. Called on the UI thread. */
typedef void (*warp_buffer_clip_t)(const char *buffer_id, const char *buffer_name, int seconds, const char *path,
				   const char *claim_flow_id, void *param);
void warp_buffer_set_clip_handler(warp_buffer_clip_t handler, void *param);

/* Told when a buffer is added, changed, removed, started or stopped, so the
 * Warp window can follow along rather than polling. Called on the UI thread. */
typedef void (*warp_buffer_changed_t)(void *param);
void warp_buffer_set_changed_handler(warp_buffer_changed_t handler, void *param);

/* Roughly how much memory a buffer takes while it is running, in megabytes:
 * every length of every angle holds its own seconds of encoded picture and
 * sound. Worked out from the bitrate the OBS profile records at, so it is an
 * estimate rather than a measurement, and it is what the UI says before a
 * buffer is started. Answers 0 for a buffer that is gone or empty. */
int warp_buffer_memory_estimate(const char *id);

/* The same sum over a buffer that does not exist yet: 'angles' feeds holding
 * 'seconds' in total between all their lengths. This is what the buffer dialog
 * says the cost of another angle is before there is a buffer to ask. */
int warp_buffer_memory_of(int angles, int seconds);

/* The buffers as they are saved with the scene collection, and put back from
 * them. warp-flow.c owns the plugin's corner of the collection, so it is what
 * calls these. The caller releases the array. */
obs_data_array_t *warp_buffer_save_all(void);
void warp_buffer_load_all(obs_data_array_t *array);

#ifdef __cplusplus
}
#endif
