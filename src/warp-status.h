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

#ifdef __cplusplus
extern "C" {
#endif

/* A line in the OBS status bar, along the bottom of the main window, saying
 * what Warp is doing: a clip being cut, a clip landing in a flow, a flow that
 * had nowhere to put one. It is where OBS itself says a replay was saved, so it
 * is where an operator is already looking.
 *
 * Messages are posted from wherever the work is - the hotkey thread, the
 * thread clips are cut on, obs-websocket's - and shown from the UI thread. With
 * no frontend to show them, as when the plugin is built without one, they go
 * nowhere; the log is still the record of what happened. */

/* how long a message stays up: about as long as OBS leaves its own, and longer
 * for one saying something went wrong, which is worth the second look */
#define WARP_STATUS_SHORT_MS 10000
#define WARP_STATUS_LONG_MS 20000

/* Shows the message, replacing whatever was up, for 'timeout_ms'. Shown by the
 * UI thread only, and only ever called there. */
typedef void (*warp_status_sink_t)(const char *message, int timeout_ms);

/* Set by the UI once it is there to show messages, and set back to NULL as it
 * goes; called on the UI thread. */
void warp_status_set_sink(warp_status_sink_t sink);

/* Formats a message and has the UI thread show it. Safe from any thread. */
#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
void warp_status_post(int timeout_ms, const char *format, ...);

#ifdef __cplusplus
}
#endif
