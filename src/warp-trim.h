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

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The replay buffer writes everything it is holding, so a clip is as long as
 * the buffer is. A flow that wants a shorter one gets it from here: the last
 * few seconds of the file are copied into a file of their own beside it, and
 * that is what the flow is fed.
 *
 * The copy is a remux - the picture and sound are moved across as they were
 * encoded, not encoded again - so it is quick and nothing is lost. What it
 * cannot do is cut between keyframes: the clip starts at the keyframe at or
 * before the mark, so it can run a little longer than it was asked for, by up
 * to the recording's keyframe interval.
 *
 * The file the replay buffer wrote is never touched.
 *
 * Safe from any thread, and meant for one that is not the UI's: it is disk
 * work. */

/* Writes the last 'seconds' of 'path' to a file beside it and answers that
 * file's path, which the caller frees with bfree().
 *
 * Answers NULL when there is nothing to write - the clip is already that long
 * or shorter - and when the trim could not be made, so the caller carries on
 * with the file it already has. */
char *warp_trim_tail(const char *path, int seconds);

#ifdef __cplusplus
}
#endif
