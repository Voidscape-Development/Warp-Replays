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

#include <obs-module.h>
#include <plugin-support.h>

#include "warp-websocket.h"

#ifdef WARP_HAVE_FRONTEND_API
#include "warp-flow.h"
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

extern struct obs_source_info warp_media_source_info;
extern struct obs_source_info warp_playlist_source_info;
extern struct obs_source_info warp_detection_filter_info;

#ifdef WARP_HAVE_FRONTEND
extern void warp_register_tools_menu(void);
#endif

bool obs_module_load(void)
{
	obs_register_source(&warp_media_source_info);
	obs_register_source(&warp_playlist_source_info);
	obs_register_source(&warp_detection_filter_info);

#ifdef WARP_HAVE_FRONTEND
	warp_register_tools_menu();
#endif

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

/* obs-websocket hands out its API once every module has loaded, so the vendor
 * requests are registered from here rather than from obs_module_load() */
void obs_module_post_load(void)
{
#ifdef WARP_HAVE_FRONTEND_API
	/* The flows listen to the frontend, which is ready by the time every
	 * module has loaded, and are read out of the scene collection, which
	 * the frontend only loads once that has happened. */
	warp_flow_init();
#endif

	warp_websocket_register();
}

void obs_module_unload(void)
{
#ifdef WARP_HAVE_FRONTEND_API
	warp_flow_shutdown();
#endif

	obs_log(LOG_INFO, "plugin unloaded");
}
