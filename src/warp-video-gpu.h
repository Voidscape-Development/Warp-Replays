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

#include <obs.h>

#ifdef __cplusplus
extern "C" {
#endif

struct AVFrame;

/* Draws a hardware-decoded frame straight off the GPU: the decoder's surface is
 * made into the two textures - luma, and chroma interleaved - that the
 * conversion shader reads, without the picture passing through memory.
 *
 *   Windows: the decoder's D3D11 texture is copied, on the GPU, into an NV12 or
 *            P010 texture shared with OBS's device, guarded by its keyed mutex.
 *   Linux:   the decoder's VAAPI surface is exported as dma-bufs and each plane
 *            imported into OBS's EGL context.
 *
 * Anything else, or anything that goes wrong on the way, is reported as a
 * failure and the caller draws the frame from a copy instead. Graphics thread
 * only. */
struct warp_gpu {
	void *impl;
	/* the path has been said in the log */
	bool announced;
};

/* Makes 'planes' (up to four, the rest NULL) from 'frame', described by 'info'.
 * The textures are valid until warp_gpu_release(). */
bool warp_gpu_import(struct warp_gpu *g, struct AVFrame *frame, const struct obs_source_frame *info,
		     gs_texture_t *planes[4]);

/* Lets go of what warp_gpu_import() made, once it has been drawn from. */
void warp_gpu_release(struct warp_gpu *g);

/* Frees everything; expects the graphics context to be entered. */
void warp_gpu_free(struct warp_gpu *g);

/* what the platform's path is called, for the log */
const char *warp_gpu_path_name(void);

#ifdef __cplusplus
}
#endif
