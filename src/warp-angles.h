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

/* Angle sets.
 *
 * A Warp buffer saving a moment writes one clip per angle, all of them the
 * same seconds of the same instant seen from different cameras. What ties them
 * together afterwards is this register: the clips of one save are kept as a
 * set, and any of their paths finds the rest.
 *
 * Only the first angle - the primary one - is handed to a flow, so a playlist
 * holds one entry per moment rather than one per camera, and a source playing
 * that clip reaches the other angles by looking its path up here. A playlist's
 * file list is therefore exactly what it always was, and a scene collection
 * written by a Warp that knows nothing about angles still loads.
 *
 * The set carries each clip's duration, which is what makes switching angles
 * land on the same moment. The clips of one save do not start together: each
 * is written from the oldest keyframe its buffer still holds, so they begin up
 * to a keyframe apart. They *end* together, because the buffers are all told
 * to save at the same instant, so the sets are lined up on their ends and a
 * position in one angle is turned into a position in another by the difference
 * between their durations.
 *
 * Every call below is safe from any thread. */

/* How many sets are kept. A set is small - a handful of paths - and the clips
 * it names outlive OBS, so this is generous; it is only here so that a machine
 * left running for a season does not grow a register without end. The oldest
 * set is dropped as the limit is passed. */
#define WARP_ANGLES_MAX_SETS 512

/* the most angles one save can hold, which is the most a buffer can carry */
#define WARP_ANGLES_MAX 8

void warp_angles_init(void);
void warp_angles_shutdown(void);

/* Records the clips one save wrote. 'names', 'paths' and 'durations' are
 * parallel arrays of 'count' entries, and entry 0 is the primary angle - the
 * one a flow is handed. A count of one is recorded like any other, so a
 * single-angle buffer reads back the same way; a clip that is already in a set
 * replaces it, since the path was written again. */
void warp_angles_add(const char *buffer_name, int seconds, size_t count, const char *const *names,
		     const char *const *paths, const int64_t *durations);

/* How many angles the clip at 'path' was saved with, and, when 'index' is
 * given, which of them that clip is. Answers 0 for a clip that is not part of
 * a set, which is every clip that did not come from a Warp buffer. */
size_t warp_angles_count(const char *path, size_t *index);

/* One angle of the set 'path' belongs to. The caller frees whichever of
 * 'out_path' and 'out_name' it asked for. Answers false when there is no such
 * set, or no angle that far along it. */
bool warp_angles_get(const char *path, size_t index, char **out_path, char **out_name, int64_t *out_duration);

/* Where playback has to land in angle 'index' to be at the same moment as
 * 'cursor' is in the clip at 'path', in milliseconds, clamped to the angle
 * being moved to. Answers false when the clip is in no set, or the set is not
 * that long. */
bool warp_angles_map_cursor(const char *path, size_t index, int64_t cursor, int64_t *out_cursor);

/* The set the clip at 'path' belongs to, written out as JSON, or NULL when it
 * is in none; the caller frees it. This is what the websocket answers with,
 * since a calldata cannot carry an array. */
char *warp_angles_describe(const char *path);

/* The register as it is saved with the scene collection, and put back from it.
 * A set names clips on disk, so it is worth keeping across a restart: the
 * files are still there and are still angles of each other. The caller
 * releases the array. */
obs_data_array_t *warp_angles_save(void);
void warp_angles_load(obs_data_array_t *array);
void warp_angles_clear(void);

#ifdef __cplusplus
}
#endif
