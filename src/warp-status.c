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

#include <stdarg.h>

#include <obs-module.h>
#include <util/dstr.h>

#include "warp-status.h"

/* Set and cleared on the UI thread, and called only there. It is read from
 * other threads too, but only to skip queueing a message that would go
 * nowhere: whether it is still set is asked again on the UI thread, where the
 * message is shown. */
static warp_status_sink_t warp_status_sink = NULL;

struct warp_status_message {
	char *text;
	int timeout_ms;
};

static void warp_status_show_task(void *param)
{
	struct warp_status_message *msg = param;

	if (warp_status_sink)
		warp_status_sink(msg->text, msg->timeout_ms);

	bfree(msg->text);
	bfree(msg);
}

void warp_status_set_sink(warp_status_sink_t sink)
{
	warp_status_sink = sink;
}

void warp_status_post(int timeout_ms, const char *format, ...)
{
	struct warp_status_message *msg;
	struct dstr text = {0};
	va_list args;

	if (!warp_status_sink || !format)
		return;

	va_start(args, format);
	dstr_vprintf(&text, format, args);
	va_end(args);

	if (dstr_is_empty(&text)) {
		dstr_free(&text);
		return;
	}

	msg = bmalloc(sizeof(struct warp_status_message));
	msg->text = text.array;
	msg->timeout_ms = timeout_ms;

	/* run straight away when posted from the UI thread, queued behind
	 * whatever it is doing otherwise */
	obs_queue_task(OBS_TASK_UI, warp_status_show_task, msg, false);
}
