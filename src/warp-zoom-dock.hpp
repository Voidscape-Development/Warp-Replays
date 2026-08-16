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

class QWidget;

/* The Warp Zoom dock: the panel an operator frames a replay from, with the
 * picture it is framing, the presets it can be sent to, and the speed it is
 * played at. Added to OBS as a dock of its own when the plugin loads. */
extern "C" void warp_register_zoom_dock(void);

/* Opens the window the zoom presets of a source are set up in, which is what
 * the Warp window's Zoom Presets button opens. Presets belong to the source
 * they are made on, so the window picks the source first. */
void warp_open_zoom_presets(QWidget *parent);
