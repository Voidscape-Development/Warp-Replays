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

#include "warp-video-gpu.h"

#include <util/bmem.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#endif

#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

/* how long either side waits for the other to finish with a shared texture
 * before giving the frame up to the copy path, in milliseconds */
#define WARP_GPU_SYNC_MS 100

#if defined(_WIN32)

/* ------------------------------------------------------------------------- */
/* Windows: D3D11
 *
 * The decoder runs on a device of its own (made on the adapter OBS draws with,
 * see mp_decode), and decodes into slices of a texture array OBS cannot see. So
 * an NV12 (or P010) texture is made on OBS's device as a shared, keyed-mutex
 * texture - the same kind OBS hands its own frames to hardware encoders in -
 * opened on the decoder's device, and each frame is copied into it there: a
 * copy between two textures on one GPU, nothing through memory. The keyed
 * mutex hands the texture from one device to the other and back: the decoder's
 * side writes under key 0 and hands over key 1, OBS draws under key 1 and hands
 * back key 0. */

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <libavutil/hwcontext_d3d11va.h>

struct warp_gpu_d3d11 {
	gs_texture_t *y;
	gs_texture_t *uv;
	uint32_t width;
	uint32_t height;
	enum video_format format;

	/* the decoder's device, and the shared texture as that device sees it */
	ID3D11Device *device;
	ID3D11Texture2D *shared;
	IDXGIKeyedMutex *mutex;

	/* OBS's side holds the texture, under key 1 */
	bool held;
};

static void d3d11_release_texture(struct warp_gpu_d3d11 *w)
{
	if (w->mutex)
		IDXGIKeyedMutex_Release(w->mutex);
	if (w->shared)
		ID3D11Texture2D_Release(w->shared);
	if (w->device)
		ID3D11Device_Release(w->device);
	w->mutex = NULL;
	w->shared = NULL;
	w->device = NULL;

	gs_texture_destroy(w->y);
	gs_texture_destroy(w->uv);
	w->y = NULL;
	w->uv = NULL;
}

static bool d3d11_make_texture(struct warp_gpu_d3d11 *w, ID3D11Device *device, uint32_t width, uint32_t height,
			       enum video_format format)
{
	bool made;

	d3d11_release_texture(w);

	if (format == VIDEO_FORMAT_P010)
		made = gs_p010_available() &&
		       gs_texture_create_p010(&w->y, &w->uv, width, height, GS_RENDER_TARGET | GS_SHARED_KM_TEX);
	else
		made = gs_nv12_available() &&
		       gs_texture_create_nv12(&w->y, &w->uv, width, height, GS_RENDER_TARGET | GS_SHARED_KM_TEX);

	if (!made)
		return false;

	uint32_t handle = gs_texture_get_shared_handle(w->y);
	if (handle == GS_INVALID_HANDLE) {
		d3d11_release_texture(w);
		return false;
	}

	HRESULT hr = ID3D11Device_OpenSharedResource(device, (HANDLE)(uintptr_t)handle, &IID_ID3D11Texture2D,
						     (void **)&w->shared);
	if (FAILED(hr)) {
		blog(LOG_WARNING, "[Warp]: the decoder's device could not open OBS's texture (0x%08lX)",
		     (unsigned long)hr);
		d3d11_release_texture(w);
		return false;
	}

	hr = ID3D11Texture2D_QueryInterface(w->shared, &IID_IDXGIKeyedMutex, (void **)&w->mutex);
	if (FAILED(hr)) {
		d3d11_release_texture(w);
		return false;
	}

	w->device = device;
	ID3D11Device_AddRef(device);
	w->width = width;
	w->height = height;
	w->format = format;
	return true;
}

bool warp_gpu_import(struct warp_gpu *g, struct AVFrame *frame, const struct obs_source_frame *info,
		     gs_texture_t *planes[4])
{
	if (!frame->hw_frames_ctx)
		return false;

	const AVHWFramesContext *fc = (const AVHWFramesContext *)frame->hw_frames_ctx->data;
	if (fc->device_ctx->type != AV_HWDEVICE_TYPE_D3D11VA)
		return false;

	AVD3D11VADeviceContext *dc = fc->device_ctx->hwctx;
	ID3D11Texture2D *src = (ID3D11Texture2D *)frame->data[0];
	UINT index = (UINT)(intptr_t)frame->data[1];

	/* NV12 and P010 copies have to cover whole chroma samples */
	uint32_t width = (info->width + 1) & ~1u;
	uint32_t height = (info->height + 1) & ~1u;

	if (!g->impl)
		g->impl = bzalloc(sizeof(struct warp_gpu_d3d11));

	struct warp_gpu_d3d11 *w = g->impl;

	if (!w->y || w->width != width || w->height != height || w->format != info->format || w->device != dc->device) {
		if (!d3d11_make_texture(w, dc->device, width, height, info->format))
			return false;
	}

	/* the decoder's device context is shared with the decoder itself, which
	 * guards it with the lock FFmpeg gives it */
	dc->lock(dc->lock_ctx);

	HRESULT hr = IDXGIKeyedMutex_AcquireSync(w->mutex, 0, WARP_GPU_SYNC_MS);
	if (hr != S_OK) {
		dc->unlock(dc->lock_ctx);
		return false;
	}

	D3D11_BOX box = {0, 0, 0, width, height, 1};
	ID3D11DeviceContext_CopySubresourceRegion(dc->device_context, (ID3D11Resource *)w->shared, 0, 0, 0, 0,
						  (ID3D11Resource *)src, index, &box);

	IDXGIKeyedMutex_ReleaseSync(w->mutex, 1);
	dc->unlock(dc->lock_ctx);

	if (gs_texture_acquire_sync(w->y, 1, WARP_GPU_SYNC_MS) != 0)
		return false;

	w->held = true;
	planes[0] = w->y;
	planes[1] = w->uv;
	return true;
}

void warp_gpu_release(struct warp_gpu *g)
{
	struct warp_gpu_d3d11 *w = g->impl;

	if (w && w->held) {
		gs_texture_release_sync(w->y, 0);
		w->held = false;
	}
}

void warp_gpu_free(struct warp_gpu *g)
{
	struct warp_gpu_d3d11 *w = g->impl;

	if (!w)
		return;

	warp_gpu_release(g);
	d3d11_release_texture(w);
	bfree(w);
	g->impl = NULL;
}

const char *warp_gpu_path_name(void)
{
	return "D3D11 shared texture";
}

#elif defined(__linux__)

/* ------------------------------------------------------------------------- */
/* Linux: VAAPI
 *
 * The decoder's surface is exported as dma-buf file descriptors - one per plane
 * or one shared by both - and each plane imported into OBS's EGL context as a
 * texture of its own: R8 for luma and GR88 for the interleaved chroma of NV12,
 * R16 and GR1616 for P010. The textures read the surface where it is. They are
 * made for each frame and let go of once it has been drawn, along with the
 * mapping that keeps the surface exported. */

#include <libavutil/hwcontext_drm.h>

#define WARP_FOURCC(a, b, c, d) \
	((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define WARP_DRM_R8 WARP_FOURCC('R', '8', ' ', ' ')
#define WARP_DRM_GR88 WARP_FOURCC('G', 'R', '8', '8')
#define WARP_DRM_R16 WARP_FOURCC('R', '1', '6', ' ')
#define WARP_DRM_GR1616 WARP_FOURCC('G', 'R', '3', '2')
#define WARP_DRM_MOD_INVALID ((1ULL << 56) - 1)

struct warp_gpu_vaapi {
	AVFrame *mapped;
	gs_texture_t *textures[2];
};

void warp_gpu_release(struct warp_gpu *g)
{
	struct warp_gpu_vaapi *v = g->impl;

	if (!v)
		return;

	for (size_t i = 0; i < 2; i++) {
		gs_texture_destroy(v->textures[i]);
		v->textures[i] = NULL;
	}

	av_frame_free(&v->mapped);
}

static gs_texture_t *import_plane(uint32_t width, uint32_t height, uint32_t drm_format,
				  enum gs_color_format color_format, int fd, uint32_t pitch, uint32_t offset,
				  uint64_t modifier)
{
	return gs_texture_create_from_dmabuf(width, height, drm_format, color_format, 1, &fd, &pitch, &offset,
					     modifier != WARP_DRM_MOD_INVALID ? &modifier : NULL);
}

bool warp_gpu_import(struct warp_gpu *g, struct AVFrame *frame, const struct obs_source_frame *info,
		     gs_texture_t *planes[4])
{
	if (!frame->hw_frames_ctx)
		return false;

	const AVHWFramesContext *fc = (const AVHWFramesContext *)frame->hw_frames_ctx->data;
	if (fc->device_ctx->type != AV_HWDEVICE_TYPE_VAAPI)
		return false;

	if (!g->impl)
		g->impl = bzalloc(sizeof(struct warp_gpu_vaapi));

	struct warp_gpu_vaapi *v = g->impl;

	warp_gpu_release(g);

	v->mapped = av_frame_alloc();
	if (!v->mapped)
		return false;

	v->mapped->format = AV_PIX_FMT_DRM_PRIME;

	if (av_hwframe_map(v->mapped, frame, AV_HWFRAME_MAP_READ) < 0) {
		av_frame_free(&v->mapped);
		return false;
	}

	const AVDRMFrameDescriptor *desc = (const AVDRMFrameDescriptor *)v->mapped->data[0];
	struct {
		int fd;
		uint32_t pitch;
		uint32_t offset;
		uint64_t modifier;
	} plane[2];
	int n = 0;

	/* the planes in order, whether they came as one layer of two planes or
	 * two layers of one */
	for (int l = 0; l < desc->nb_layers && n < 2; l++) {
		const AVDRMLayerDescriptor *layer = &desc->layers[l];

		for (int p = 0; p < layer->nb_planes && n < 2; p++) {
			const AVDRMObjectDescriptor *obj = &desc->objects[layer->planes[p].object_index];

			plane[n].fd = obj->fd;
			plane[n].pitch = (uint32_t)layer->planes[p].pitch;
			plane[n].offset = (uint32_t)layer->planes[p].offset;
			plane[n].modifier = obj->format_modifier;
			n++;
		}
	}

	if (n != 2) {
		warp_gpu_release(g);
		return false;
	}

	const bool p010 = info->format == VIDEO_FORMAT_P010;
	const uint32_t width = info->width;
	const uint32_t height = info->height;

	v->textures[0] = import_plane(width, height, p010 ? WARP_DRM_R16 : WARP_DRM_R8, p010 ? GS_R16 : GS_R8,
				      plane[0].fd, plane[0].pitch, plane[0].offset, plane[0].modifier);
	v->textures[1] = import_plane((width + 1) / 2, (height + 1) / 2, p010 ? WARP_DRM_GR1616 : WARP_DRM_GR88,
				      p010 ? GS_RG16 : GS_R8G8, plane[1].fd, plane[1].pitch, plane[1].offset,
				      plane[1].modifier);

	if (!v->textures[0] || !v->textures[1]) {
		warp_gpu_release(g);
		return false;
	}

	planes[0] = v->textures[0];
	planes[1] = v->textures[1];
	return true;
}

void warp_gpu_free(struct warp_gpu *g)
{
	if (!g->impl)
		return;

	warp_gpu_release(g);
	bfree(g->impl);
	g->impl = NULL;
}

const char *warp_gpu_path_name(void)
{
	return "VAAPI dma-buf";
}

#else

/* ------------------------------------------------------------------------- */
/* Elsewhere - macOS among them - there is no import, and frames are drawn from
 * copies. libobs takes an IOSurface only as a single plane, and decoders hand
 * out two. */

bool warp_gpu_import(struct warp_gpu *g, struct AVFrame *frame, const struct obs_source_frame *info,
		     gs_texture_t *planes[4])
{
	UNUSED_PARAMETER(g);
	UNUSED_PARAMETER(frame);
	UNUSED_PARAMETER(info);
	UNUSED_PARAMETER(planes);
	return false;
}

void warp_gpu_release(struct warp_gpu *g)
{
	UNUSED_PARAMETER(g);
}

void warp_gpu_free(struct warp_gpu *g)
{
	UNUSED_PARAMETER(g);
}

const char *warp_gpu_path_name(void)
{
	return "none";
}

#endif
