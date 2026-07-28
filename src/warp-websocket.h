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

/* Registers the Warp obs-websocket vendor and its requests, which put every
 * action of the Warp Media and Warp Playlist sources on the websocket.
 *
 * obs-websocket only hands out its API once every module has loaded, so this
 * has to be called from obs_module_post_load(). It does nothing at all when
 * obs-websocket is not installed. */
void warp_websocket_register(void);
