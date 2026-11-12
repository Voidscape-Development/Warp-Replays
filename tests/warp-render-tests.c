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

/* Render tests for the Warp Progress Bar and the Warp Status Icon.
 *
 * Runs libobs headless - an OpenGL context on an X display, which Xvfb
 * provides - loads the plugin the way OBS does, plays real files through real
 * Warp Media and Warp Playlist sources, and reads the overlays' pictures back
 * to check them pixel by pixel. Every picture checked is also written out as a
 * PNG-ready raw file, so a failure can be looked at; see tests/README.md.
 *
 * Usage:
 *   warp-render-tests --plugin Warp.so --data data/ --obs-plugins DIR
 *                     --obs-data DIR --media DIR --out DIR
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>

#include <obs.h>
#include <obs-module.h>
#include <obs-nix-platform.h>
#include <util/platform.h>
#include <util/threading.h>

#define CANVAS_W 1280
#define CANVAS_H 720

static const char *plugin_path;
static const char *data_path;
static const char *obs_plugins_dir;
static const char *obs_data_dir;
static const char *media_dir;
static const char *out_dir;
static bool verbose;

static int failures;
static int checks;

/* ------------------------------------------------------------------------- */
/* reporting */

static void check(bool ok, const char *format, ...)
{
	va_list args;

	checks++;
	printf("  %s ", ok ? "ok  " : "FAIL");
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
	printf("\n");

	if (!ok)
		failures++;
}

static void log_handler(int level, const char *format, va_list args, void *param)
{
	(void)param;

	/* the plugin's own lines, and anything that went wrong, are worth
	 * seeing; the rest of libobs' chatter is not */
	if (!verbose && level > LOG_WARNING && !strstr(format, "Warp") && !strstr(format, "warp"))
		return;

	char line[4096];

	vsnprintf(line, sizeof(line), format, args);

	if (!verbose && level > LOG_WARNING && !strstr(line, "Warp"))
		return;

	printf("    [obs] %s\n", line);
}

/* ------------------------------------------------------------------------- */
/* pictures */

struct picture {
	uint32_t w;
	uint32_t h;
	uint8_t *rgba;
};

static void picture_free(struct picture *p)
{
	bfree(p->rgba);
	p->rgba = NULL;
}

/* draws 'source' on its own, at its own size, and reads it back */
static bool render_source(obs_source_t *source, struct picture *out)
{
	uint32_t w = obs_source_get_width(source);
	uint32_t h = obs_source_get_height(source);
	bool ok = false;

	memset(out, 0, sizeof(*out));

	if (!w || !h)
		return false;

	obs_enter_graphics();

	gs_texrender_t *tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_stagesurf_t *stage = gs_stagesurface_create(w, h, GS_RGBA);

	if (gs_texrender_begin(tr, w, h)) {
		struct vec4 clear;

		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);

		gs_blend_state_push();
		gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		obs_source_video_render(source);
		gs_blend_state_pop();

		gs_texrender_end(tr);

		gs_stage_texture(stage, gs_texrender_get_texture(tr));

		uint8_t *data;
		uint32_t linesize;

		if (gs_stagesurface_map(stage, &data, &linesize)) {
			out->w = w;
			out->h = h;
			out->rgba = bmalloc((size_t)w * h * 4);

			for (uint32_t y = 0; y < h; y++)
				memcpy(out->rgba + (size_t)y * w * 4, data + (size_t)y * linesize, (size_t)w * 4);

			gs_stagesurface_unmap(stage);
			ok = true;
		}
	}

	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(tr);

	obs_leave_graphics();
	return ok;
}

static const uint8_t *px(const struct picture *p, uint32_t x, uint32_t y)
{
	if (x >= p->w)
		x = p->w - 1;
	if (y >= p->h)
		y = p->h - 1;

	return p->rgba + ((size_t)y * p->w + x) * 4;
}

static uint32_t count_opaque(const struct picture *p)
{
	uint32_t n = 0;

	for (size_t i = 0; i < (size_t)p->w * p->h; i++)
		if (p->rgba[i * 4 + 3] > 128)
			n++;

	return n;
}

/* Writes the picture as a PAM file (RGB_ALPHA), which any image tool reads;
 * tests/run-render-tests.sh turns them into PNGs. */
static void save(const struct picture *p, const char *name)
{
	char path[1024];

	snprintf(path, sizeof(path), "%s/%s.pam", out_dir, name);

	FILE *f = fopen(path, "wb");

	if (!f)
		return;

	fprintf(f, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", p->w, p->h);
	fwrite(p->rgba, 1, (size_t)p->w * p->h * 4, f);
	fclose(f);
}

static bool render_and_save(obs_source_t *source, struct picture *out, const char *name)
{
	bool ok = render_source(source, out);

	if (ok)
		save(out, name);

	return ok;
}

/* ------------------------------------------------------------------------- */
/* driving sources */

static void wait_ms(uint32_t ms)
{
	os_sleep_ms(ms);
}

/* waits for 'source' to be in 'state', for up to 'ms' */
static bool wait_state(obs_source_t *source, enum obs_media_state state, uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 10) {
		if (obs_source_media_get_state(source) == state)
			return true;
		wait_ms(10);
	}

	return obs_source_media_get_state(source) == state;
}

static void media_path(char *out, size_t size, const char *name)
{
	snprintf(out, size, "%s/%s", media_dir, name);
}

static void load_clip(obs_source_t *media, const char *name, const char *playback)
{
	char path[1024];
	calldata_t cd = {0};

	media_path(path, sizeof(path), name);

	calldata_init(&cd);
	calldata_set_string(&cd, "path", path);
	calldata_set_int(&cd, "speed", 0);
	calldata_set_string(&cd, "playback", playback);
	calldata_set_int(&cd, "in_ms", 0);
	calldata_set_int(&cd, "out_ms", 0);
	calldata_set_int(&cd, "tail_ms", 0);
	proc_handler_call(obs_source_get_proc_handler(media), "warp_media_load", &cd);
	calldata_free(&cd);
}

static void call_int(obs_source_t *source, const char *proc, const char *arg, long long value)
{
	calldata_t cd = {0};

	calldata_init(&cd);
	calldata_set_int(&cd, arg, value);
	proc_handler_call(obs_source_get_proc_handler(source), proc, &cd);
	calldata_free(&cd);
}

/* The plainest look of each overlay, which the pixel checks below are written
 * against: nothing spills off the bar, nothing pops or rounds on the icon. */
static const char *flat_bar = "{\"shadow\": false, \"glow\": 0, \"sheen\": 0, \"playhead\": \"none\","
			      " \"gradient\": false, \"fill_color\": 4294967295, \"text_chip\": false,"
			      " \"slant\": 0, \"segments\": 0}";
static const char *flat_icon = "{\"layout\": \"beside\", \"plate\": \"circle\", \"plate_color\": 2566914048,"
			       " \"size\": 128, \"glyph_scale\": 50, \"rounded_glyphs\": false, \"pop\": false,"
			       " \"accent_width\": 0, \"show_progress\": false, \"text_content\": \"value\"}";

static obs_source_t *make_overlay(const char *id, const char *name, obs_source_t *target, obs_data_t *extra)
{
	obs_data_t *settings =
		obs_data_create_from_json(strcmp(id, "warp_progress_source") == 0 ? flat_bar : flat_icon);

	obs_data_set_string(settings, "target", target ? obs_source_get_uuid(target) : "");
	if (extra)
		obs_data_apply(settings, extra);

	obs_source_t *source = obs_source_create(id, name, settings, NULL);

	obs_data_release(settings);
	return source;
}

static void set(obs_source_t *source, const char *json)
{
	obs_data_t *settings = obs_data_create_from_json(json);

	obs_source_update(source, settings);
	obs_data_release(settings);
}

/* the share of the bar's middle row that is lit, left to right */
static float lit_fraction(const struct picture *p, uint32_t y)
{
	uint32_t lit = 0;

	for (uint32_t x = 0; x < p->w; x++) {
		const uint8_t *c = px(p, x, y);

		if (c[3] > 200 && c[0] > 200 && c[1] > 200 && c[2] > 200)
			lit++;
	}

	return (float)lit / (float)p->w;
}

static float media_fraction(obs_source_t *source)
{
	int64_t d = obs_source_media_get_duration(source);
	int64_t t = obs_source_media_get_time(source);

	return d > 0 ? (float)t / (float)d : 0.0f;
}

/* ------------------------------------------------------------------------- */
/* switches the playlist reports */

static pthread_mutex_t switch_mutex = PTHREAD_MUTEX_INITIALIZER;
static char switch_reason[32];
static long long switch_ms = -1;
static int switch_count;

static void on_switched(void *param, calldata_t *cd)
{
	(void)param;

	pthread_mutex_lock(&switch_mutex);
	snprintf(switch_reason, sizeof(switch_reason), "%s", calldata_string(cd, "reason"));
	switch_ms = calldata_int(cd, "transition_ms");
	switch_count++;
	pthread_mutex_unlock(&switch_mutex);
}

static int switches(char *reason, size_t size, long long *ms)
{
	pthread_mutex_lock(&switch_mutex);
	int count = switch_count;
	if (reason)
		snprintf(reason, size, "%s", switch_reason);
	if (ms)
		*ms = switch_ms;
	pthread_mutex_unlock(&switch_mutex);

	return count;
}

/* the media actions a source reports */
static volatile long pause_actions;

static void on_media_action(void *param, calldata_t *cd)
{
	(void)param;

	if (strcmp(calldata_string(cd, "action"), "pause") == 0)
		os_atomic_inc_long(&pause_actions);
}

/* ------------------------------------------------------------------------- */
/* the tests */

static obs_scene_t *program;

static obs_sceneitem_t *put_on_program(obs_source_t *source)
{
	return obs_scene_add(program, source);
}

static void test_progress_bar(obs_source_t *media)
{
	struct picture pic;

	printf("Progress bar, horizontal, following a Warp Media source\n");

	obs_source_t *bar = make_overlay("warp_progress_source", "bar", media, NULL);

	set(bar, "{\"width\": 400, \"height\": 20, \"corner_radius\": 0, \"smooth\": false,"
		 " \"idle_none\": \"empty\", \"track_color\": 4278190080}");
	/* a video source's settings land on its next tick */
	wait_ms(100);

	check(obs_source_get_width(bar) == 400 && obs_source_get_height(bar) == 20, "sized as set (%ux%u)",
	      obs_source_get_width(bar), obs_source_get_height(bar));

	load_clip(media, "clip-4s.mp4", "play");
	check(wait_state(media, OBS_MEDIA_STATE_PLAYING, 3000), "the media plays");
	wait_ms(1500);

	float expected = media_fraction(media);

	render_and_save(bar, &pic, "bar-playing");
	float lit = lit_fraction(&pic, 10);

	check(fabsf(lit - expected) < 0.06f, "fill follows the position (lit %.2f, media at %.2f)", lit, expected);
	picture_free(&pic);

	/* reversed: the fill comes in from the right */
	set(bar, "{\"reverse\": true}");
	wait_ms(100);
	render_and_save(bar, &pic, "bar-reversed");
	{
		const uint8_t *right = px(&pic, pic.w - 3, 10);
		const uint8_t *left = px(&pic, 2, 10);

		check(right[0] > 200 && left[0] < 60, "reversed fills from the right");
	}
	picture_free(&pic);
	set(bar, "{\"reverse\": false}");

	/* vertical, filling from the bottom, with a gradient up it */
	set(bar, "{\"shape\": \"vertical\", \"width\": 20, \"height\": 300, \"gradient\": true,"
		 " \"fill_color\": 4294967295, \"fill_color2\": 4278190335}");
	obs_source_media_play_pause(media, true);
	wait_state(media, OBS_MEDIA_STATE_PAUSED, 1000);
	wait_ms(200);
	render_and_save(bar, &pic, "bar-vertical-gradient");
	{
		float f = media_fraction(media);
		const uint8_t *bottom = px(&pic, 10, 297);
		const uint8_t *top = px(&pic, 10, 3);
		const uint8_t *upper = px(&pic, 10, (uint32_t)(300.0f * (1.0f - f)) + 4);

		check(pic.w == 20 && pic.h == 300, "a vertical bar is as tall as set");
		check(bottom[0] > 200 && bottom[2] > 200, "fills from the bottom, starting at the first color");
		check(top[0] < 60 && top[3] > 200, "with the track showing at the top (at %.2f)", f);
		check(upper[0] > 200 && upper[2] < bottom[2], "and turns toward the second color going up (%d,%d,%d)",
		      upper[0], upper[1], upper[2]);
	}
	picture_free(&pic);
	set(bar, "{\"shape\": \"horizontal\", \"width\": 400, \"height\": 20, \"gradient\": false,"
		 " \"fill_color\": 4294967295}");
	obs_source_media_play_pause(media, false);
	wait_ms(100);

	/* paused, the fill takes the paused color */
	set(bar, "{\"color_by_state\": true, \"paused_color\": 4278239487}");
	obs_source_media_play_pause(media, true);
	wait_state(media, OBS_MEDIA_STATE_PAUSED, 1000);
	wait_ms(500);
	render_and_save(bar, &pic, "bar-paused-color");
	{
		/* 0xFF00C0FF is orange: red full, green 0xC0, no blue */
		const uint8_t *c = px(&pic, 3, 10);

		check(c[0] > 230 && c[1] > 150 && c[1] < 220 && c[2] < 40, "paused fill is the paused color (%d,%d,%d)",
		      c[0], c[1], c[2]);
	}
	picture_free(&pic);

	/* frame steps move the fill exactly, without smoothing */
	float before = media_fraction(media);

	call_int(media, "warp_step_frames", "frames", 30);
	wait_ms(400);
	float after = media_fraction(media);
	check(after > before, "stepping moved the media (%.3f -> %.3f)", before, after);
	set(bar, "{\"color_by_state\": false}");
	wait_ms(400);
	render_and_save(bar, &pic, "bar-after-step");
	lit = lit_fraction(&pic, 10);
	check(fabsf(lit - after) < 0.04f, "the fill lands on the stepped frame (lit %.2f, media at %.2f)", lit, after);
	picture_free(&pic);

	/* a playhead and a border, and the time over the bar */
	set(bar, "{\"playhead\": \"line\", \"playhead_width\": 6, \"playhead_color\": 4278255360,"
		 " \"border_width\": 2, \"border_color\": 4294901760,"
		 " \"text\": \"elapsed_total\", \"text_position\": \"above\"}");
	wait_ms(300);
	render_and_save(bar, &pic, "bar-playhead-text");
	check(pic.h > 20, "the text adds a band above the bar (height %u)", pic.h);
	{
		uint32_t bar_top = pic.h - 20;
		uint32_t head_x = (uint32_t)(after * 400.0f);
		const uint8_t *head = px(&pic, head_x, bar_top + 10);
		const uint8_t *edge = px(&pic, 200, bar_top);
		uint32_t text_pixels = 0;

		for (uint32_t y = 0; y < bar_top; y++)
			for (uint32_t x = 0; x < pic.w; x++)
				if (px(&pic, x, y)[3] > 128)
					text_pixels++;

		check(head[1] > 200 && head[0] < 80, "the playhead is drawn at the position (%d,%d,%d)", head[0],
		      head[1], head[2]);
		check(edge[2] > 200 && edge[0] < 80, "the border is drawn round the bar (%d,%d,%d)", edge[0], edge[1],
		      edge[2]);
		check(text_pixels > 50, "the time is drawn above it (%u pixels)", text_pixels);
	}
	picture_free(&pic);

	/* ended: full by default */
	set(bar, "{\"playhead\": \"none\", \"border_width\": 0, \"text\": \"none\", \"idle_ended\": \"full\"}");
	obs_source_media_play_pause(media, false);
	check(wait_state(media, OBS_MEDIA_STATE_ENDED, 6000), "the media runs to its end");
	wait_ms(300);
	render_and_save(bar, &pic, "bar-ended-full");
	lit = lit_fraction(&pic, 10);
	check(lit > 0.97f, "an ended source shows full (lit %.2f)", lit);
	picture_free(&pic);

	set(bar, "{\"idle_ended\": \"hide\"}");
	wait_ms(500);
	render_and_save(bar, &pic, "bar-ended-hidden");
	check(count_opaque(&pic) == 0, "or hides when set to");
	picture_free(&pic);

	/* hidden while the target is off air */
	set(bar, "{\"idle_ended\": \"full\", \"hide_off_air\": true}");
	wait_ms(500);
	render_and_save(bar, &pic, "bar-on-air");
	check(count_opaque(&pic) > 0, "shown while its target is on program");
	picture_free(&pic);

	obs_sceneitem_t *item = obs_scene_find_source(program, obs_source_get_name(media));

	obs_sceneitem_set_visible(item, false);
	wait_ms(600);
	render_and_save(bar, &pic, "bar-off-air");
	check(count_opaque(&pic) == 0, "hidden while its target is off program");
	picture_free(&pic);
	obs_sceneitem_set_visible(item, true);
	wait_ms(200);

	obs_source_release(bar);
}

static void test_ring(obs_source_t *media)
{
	struct picture pic;

	printf("Progress bar, ring\n");

	obs_source_t *ring = make_overlay("warp_progress_source", "ring", media, NULL);

	set(ring, "{\"shape\": \"ring\", \"ring_size\": 200, \"ring_thickness\": 24, \"smooth\": false,"
		  " \"track_color\": 4278190080, \"text\": \"remaining\", \"text_position\": \"inside\"}");

	load_clip(media, "clip-4s.mp4", "play");
	wait_state(media, OBS_MEDIA_STATE_PLAYING, 3000);
	wait_ms(1500);
	obs_source_media_play_pause(media, true);
	wait_state(media, OBS_MEDIA_STATE_PAUSED, 1000);
	wait_ms(200);

	float f = media_fraction(media);

	render_and_save(ring, &pic, "ring");
	check(pic.w == 200 && pic.h == 200, "the ring is as big as set");

	/* a point on the ring a little way round from the top is filled; one
	 * just short of all the way round is not */
	float mid = 100.0f - 12.0f;
	float a_in = f * 0.5f * 6.2831853f;
	float a_out = (f + (1.0f - f) * 0.5f) * 6.2831853f;
	const uint8_t *in = px(&pic, (uint32_t)(100 + sinf(a_in) * mid), (uint32_t)(100 - cosf(a_in) * mid));
	const uint8_t *out = px(&pic, (uint32_t)(100 + sinf(a_out) * mid), (uint32_t)(100 - cosf(a_out) * mid));

	check(in[0] > 200 && in[3] > 200, "the arc is filled behind the position (at %.2f)", f);
	check(out[0] < 60 && out[3] > 200, "and the track shows ahead of it");
	check(px(&pic, 100, 100)[3] < 255, "the middle is left open for the time");
	picture_free(&pic);

	obs_source_release(ring);
}

static void test_icon(obs_source_t *media)
{
	struct picture pic, other;

	printf("Status icon, following a Warp Media source\n");

	obs_source_t *icon = make_overlay("warp_status_icon_source", "icon", media, NULL);

	set(icon, "{\"size\": 128, \"fade_ms\": 0, \"flash_ms\": 500}");

	load_clip(media, "clip-4s.mp4", "play");
	wait_state(media, OBS_MEDIA_STATE_PLAYING, 3000);
	wait_ms(800);

	render_and_save(icon, &pic, "icon-playing");
	check(count_opaque(&pic) == 0, "nothing shows while playing at 100%%");
	picture_free(&pic);

	obs_source_media_play_pause(media, true);
	wait_state(media, OBS_MEDIA_STATE_PAUSED, 1000);
	wait_ms(200);
	render_and_save(icon, &pic, "icon-paused");
	{
		/* the pause glyph is two bars either side of the middle */
		const uint8_t *middle = px(&pic, 64, 64);
		const uint8_t *bar = px(&pic, 64 - 12, 64);

		check(pic.w > 128, "room is kept beside it for the value (width %u)", pic.w);
		check(bar[0] > 200 && bar[3] > 200, "pause shows its bars (%d,%d,%d,%d)", bar[0], bar[1], bar[2],
		      bar[3]);
		check(middle[0] < 80 && middle[3] > 100, "with the plate showing between them");
	}

	call_int(media, "warp_step_frames", "frames", 5);
	wait_ms(100);
	render_and_save(icon, &other, "icon-step");
	{
		uint32_t differ = 0;

		for (size_t i = 0; i < (size_t)128 * 128; i++)
			if (abs((int)pic.rgba[(i / 128) * pic.w * 4 + (i % 128) * 4] -
				(int)other.rgba[(i / 128) * other.w * 4 + (i % 128) * 4]) > 100)
				differ++;

		uint32_t text = 0;

		for (uint32_t y = 0; y < other.h; y++)
			for (uint32_t x = 140; x < other.w; x++)
				if (px(&other, x, y)[3] > 128)
					text++;

		check(differ > 300, "a step flashes its own glyph (%u pixels changed)", differ);
		check(text > 50, "with the frames stepped beside it (%u pixels)", text);
	}
	picture_free(&other);

	/* and gives way back to pause */
	wait_ms(800);
	render_and_save(icon, &other, "icon-after-flash");
	{
		uint32_t differ = 0;

		for (size_t i = 0; i < (size_t)128 * 128; i++)
			if (abs((int)pic.rgba[(i / 128) * pic.w * 4 + (i % 128) * 4] -
				(int)other.rgba[(i / 128) * other.w * 4 + (i % 128) * 4]) > 100)
				differ++;

		check(differ < 50, "then goes back to pause (%u pixels differ)", differ);
	}
	picture_free(&other);
	picture_free(&pic);

	/* slow motion while playing: play with a slow badge */
	call_int(media, "warp_set_speed", "speed", 50);
	obs_source_media_play_pause(media, false);
	wait_ms(900);
	render_and_save(icon, &pic, "icon-slow-badge");
	{
		const uint8_t *badge = px(&pic, 128 - 20, 128 - 20);

		check(count_opaque(&pic) > 0, "slow motion shows even though it is playing");
		check(badge[3] > 200, "with a badge in the corner");
	}
	picture_free(&pic);

	/* not combined: the slow glyph stands in for play */
	set(icon, "{\"combine_speed\": false}");
	wait_ms(100);
	render_and_save(icon, &pic, "icon-slow-alone");
	check(count_opaque(&pic) > 0, "the speed glyph shows on its own when not combined");
	picture_free(&pic);

	/* an image of the operator's own in place of the pause glyph, with the
	 * value below the icon */
	{
		char image[1024];

		media_path(image, sizeof(image), "red-square.png");

		obs_data_t *own = obs_data_create();

		obs_data_set_string(own, "image_pause", image);
		obs_data_set_string(own, "text_position", "below");
		obs_source_update(icon, own);
		obs_data_release(own);

		obs_source_media_play_pause(media, true);
		wait_state(media, OBS_MEDIA_STATE_PAUSED, 1000);
		wait_ms(200);
		render_and_save(icon, &pic, "icon-own-image");

		uint32_t cx = pic.w / 2;
		const uint8_t *c = px(&pic, cx, 64);

		check(pic.h > 128, "the value moves below the icon (%ux%u)", pic.w, pic.h);
		check(c[0] > 200 && c[1] < 60 && c[2] < 60, "an image of its own stands in for the glyph (%d,%d,%d)",
		      c[0], c[1], c[2]);
		picture_free(&pic);

		obs_data_t *back = obs_data_create();

		obs_data_set_string(back, "image_pause", "");
		obs_data_set_string(back, "text_position", "right");
		obs_source_update(icon, back);
		obs_data_release(back);
		obs_source_media_play_pause(media, false);
		wait_ms(100);
	}

	/* the other glyphs, for the eye */
	set(icon, "{\"combine_speed\": true, \"plate\": \"rounded\", \"shadow\": true}");
	call_int(media, "warp_set_speed", "speed", 200);
	wait_ms(100);
	render_and_save(icon, &pic, "icon-fast-flash");
	picture_free(&pic);

	/* a step taken while playing pauses first, but that pause is the
	 * step's own, not one an operator asked for */
	load_clip(media, "clip-4s.mp4", "play");
	wait_state(media, OBS_MEDIA_STATE_PLAYING, 3000);
	wait_ms(300);
	signal_handler_connect(obs_source_get_signal_handler(media), "warp_media_action", on_media_action, NULL);
	os_atomic_set_long(&pause_actions, 0);
	call_int(media, "warp_step_frames", "frames", -1);
	wait_ms(150);
	check(os_atomic_load_long(&pause_actions) == 0, "a step from playing is not reported as a pause");
	signal_handler_disconnect(obs_source_get_signal_handler(media), "warp_media_action", on_media_action, NULL);
	render_and_save(icon, &pic, "icon-step-back");
	{
		uint32_t text = 0;

		for (uint32_t y = 0; y < pic.h; y++)
			for (uint32_t x = 140; x < pic.w; x++)
				if (px(&pic, x, y)[3] > 128)
					text++;

		check(text > 50, "and its flash is not cut short by one (%u pixels of value)", text);
	}
	picture_free(&pic);
	obs_source_media_stop(media);
	wait_ms(700);
	render_and_save(icon, &pic, "icon-stopped");
	check(count_opaque(&pic) > 0, "stopped shows");
	picture_free(&pic);

	call_int(media, "warp_set_speed", "speed", 100);
	obs_source_release(icon);
}

static void test_playlist(void)
{
	struct picture pic;
	char path[1024];

	printf("Playlist: switches, and the bar easing back over the transition\n");

	obs_data_t *settings = obs_data_create();
	obs_data_array_t *files = obs_data_array_create();

	for (int i = 0; i < 3; i++) {
		obs_data_t *file = obs_data_create();

		media_path(path, sizeof(path), i == 1 ? "clip-4s-b.mp4" : "clip-4s.mp4");
		obs_data_set_string(file, "value", path);
		obs_data_array_push_back(files, file);
		obs_data_release(file);
	}

	obs_data_set_array(settings, "playlist", files);
	obs_data_set_int(settings, "transition_duration", 800);
	obs_data_array_release(files);

	obs_source_t *playlist = obs_source_create("warp_playlist_source", "playlist", settings, NULL);

	obs_data_release(settings);
	signal_handler_connect(obs_source_get_signal_handler(playlist), "warp_playlist_switched", on_switched, NULL);

	obs_sceneitem_t *item = put_on_program(playlist);

	obs_source_t *bar = make_overlay("warp_progress_source", "playlist-bar", playlist, NULL);
	obs_source_t *icon = make_overlay("warp_status_icon_source", "playlist-icon", playlist, NULL);

	set(bar, "{\"width\": 400, \"height\": 20, \"corner_radius\": 0, \"smooth\": true,"
		 " \"track_color\": 4278190080}");
	set(icon, "{\"size\": 128, \"fade_ms\": 0, \"flash_ms\": 600}");

	check(wait_state(playlist, OBS_MEDIA_STATE_PLAYING, 4000), "the playlist plays");
	wait_ms(300);

	char reason[32];
	long long ms;
	int count = switches(reason, sizeof(reason), &ms);

	check(count >= 1 && strcmp(reason, "start") == 0, "its first file going up is reported as a start (%s)",
	      reason);

	wait_ms(1700);
	render_and_save(bar, &pic, "playlist-before-next");
	float before = lit_fraction(&pic, 10);
	picture_free(&pic);

	obs_source_media_next(playlist);
	wait_ms(250);

	int after_count = switches(reason, sizeof(reason), &ms);

	check(after_count == count + 1 && strcmp(reason, "next") == 0, "Next is reported as next (%s)", reason);
	check(ms == 800, "with the transition's length (%lld ms)", ms);

	render_and_save(bar, &pic, "playlist-easing");
	float easing = lit_fraction(&pic, 10);
	picture_free(&pic);

	render_and_save(icon, &pic, "playlist-icon-next");
	check(count_opaque(&pic) > 0, "the icon flashes the skip");
	picture_free(&pic);

	check(easing < before - 0.02f && easing > 0.02f,
	      "part way through the transition the fill is easing back (%.2f, was %.2f)", easing, before);

	wait_ms(900);
	render_and_save(bar, &pic, "playlist-after-transition");
	float settled = lit_fraction(&pic, 10);
	picture_free(&pic);

	check(settled < 0.3f, "and has come back to the new file once it is over (%.2f, media at %.2f)", settled,
	      media_fraction(playlist));

	/* runs out by itself into the third file */
	count = switches(NULL, 0, NULL);
	for (int t = 0; t < 600 && switches(NULL, 0, NULL) == count; t++)
		wait_ms(10);

	switches(reason, sizeof(reason), &ms);
	check(strcmp(reason, "auto") == 0, "moving on by itself is reported as auto (%s)", reason);

	obs_source_media_previous(playlist);
	wait_ms(250);
	switches(reason, sizeof(reason), &ms);
	check(strcmp(reason, "previous") == 0, "Previous is reported as previous (%s)", reason);
	render_and_save(icon, &pic, "playlist-icon-previous");
	picture_free(&pic);

	signal_handler_disconnect(obs_source_get_signal_handler(playlist), "warp_playlist_switched", on_switched, NULL);
	obs_sceneitem_remove(item);
	obs_source_release(bar);
	obs_source_release(icon);
	obs_source_release(playlist);
}

/* picks a style the way the properties do, and applies what it filled in */
static void apply_style(obs_source_t *source, const char *style)
{
	obs_data_t *settings = obs_source_get_settings(source);
	obs_properties_t *props = obs_source_properties(source);

	obs_data_set_string(settings, "style", style);
	obs_property_modified(obs_properties_get(props, "style"), settings);
	obs_source_update(source, settings);

	obs_properties_destroy(props);
	obs_data_release(settings);
}

static uint32_t opaque_columns(const struct picture *p)
{
	uint32_t right = 0;

	for (uint32_t x = 0; x < p->w; x++)
		for (uint32_t y = 0; y < p->h; y++)
			if (px(p, x, y)[3] > 128)
				right = x + 1;

	return right;
}

static void test_styles(obs_source_t *media)
{
	static const char *bar_styles[] = {"broadcast", "segmented", "sport",   "scoreboard", "glass",
					   "neon",      "gold",      "minimal", "flat"};
	static const char *icon_styles[] = {"chip",    "badge",  "tag",     "sport",   "glass", "neon",
					    "duotone", "aurora", "outline", "minimal", "flat"};
	struct picture pic;
	char name[128];

	printf("Styles\n");

	load_clip(media, "clip-4s.mp4", "play");
	wait_state(media, OBS_MEDIA_STATE_PLAYING, 3000);
	wait_ms(1600);
	obs_source_media_play_pause(media, true);
	wait_state(media, OBS_MEDIA_STATE_PAUSED, 1000);
	call_int(media, "warp_set_speed", "speed", 50);

	for (size_t i = 0; i < sizeof(bar_styles) / sizeof(bar_styles[0]); i++) {
		obs_source_t *bar = obs_source_create("warp_progress_source", "style-bar", NULL, NULL);
		obs_data_t *t = obs_data_create();

		obs_data_set_string(t, "target", obs_source_get_uuid(media));
		obs_data_set_string(t, "text", "elapsed_total");
		obs_data_set_int(t, "width", 640);
		obs_source_update(bar, t);
		obs_data_release(t);
		apply_style(bar, bar_styles[i]);
		wait_ms(300);

		snprintf(name, sizeof(name), "style-bar-%s", bar_styles[i]);
		render_and_save(bar, &pic, name);
		check(count_opaque(&pic) > 640 * 3, "the %s bar draws (%ux%u)", bar_styles[i], pic.w, pic.h);

		if (strcmp(bar_styles[i], "broadcast") == 0) {
			check(pic.w > 640, "its shadow and glow are given room round the bar (%u wide)", pic.w);
		}

		if (strcmp(bar_styles[i], "scoreboard") == 0) {
			/* the time is printed in the bar, so the source is no
			 * taller than the bar and the room for its shadow */
			uint32_t light = 0;
			uint32_t bar_y = (pic.h - 30) / 2;

			for (uint32_t x = 0; x < pic.w; x++)
				for (uint32_t y = bar_y; y < bar_y + 30; y++) {
					const uint8_t *c = px(&pic, x, y);
					if (c[3] > 200 && c[0] > 200 && c[1] > 200 && c[2] > 200)
						light++;
				}

			check(pic.h <= 30 + 2 * 16, "the scoreboard keeps its time inside the bar (%u high)", pic.h);
			check(light > 30, "where it shows light on the dark track (%u pixels)", light);
		}
		picture_free(&pic);

		set(bar, "{\"shape\": \"ring\", \"ring_size\": 180, \"text\": \"remaining\","
			 " \"text_position\": \"inside\"}");
		wait_ms(200);
		snprintf(name, sizeof(name), "style-ring-%s", bar_styles[i]);
		render_and_save(bar, &pic, name);
		check(count_opaque(&pic) > 1000, "and as a ring");
		picture_free(&pic);

		obs_source_release(bar);
	}

	for (size_t i = 0; i < sizeof(icon_styles) / sizeof(icon_styles[0]); i++) {
		obs_source_t *icon = obs_source_create("warp_status_icon_source", "style-icon", NULL, NULL);
		obs_data_t *t = obs_data_create();
		bool chip = strcmp(icon_styles[i], "chip") == 0;

		obs_data_set_string(t, "target", obs_source_get_uuid(media));
		obs_source_update(icon, t);
		obs_data_release(t);
		apply_style(icon, icon_styles[i]);
		wait_ms(500);

		snprintf(name, sizeof(name), "style-icon-%s-paused-slow", icon_styles[i]);
		render_and_save(icon, &pic, name);
		check(count_opaque(&pic) > 500, "the %s icon shows paused in slow motion", icon_styles[i]);

		if (chip) {
			uint32_t paused_w = opaque_columns(&pic);
			uint32_t size = obs_source_get_height(icon);

			check(paused_w > size * 2, "the chip opens out to take its text (%u wide, %u high)", paused_w,
			      size);
			picture_free(&pic);

			call_int(media, "warp_step_frames", "frames", 5);
			wait_ms(400);
			render_and_save(icon, &pic, "style-icon-chip-step");
			check(count_opaque(&pic) > 500, "and flashes a step");
			picture_free(&pic);

			call_int(media, "warp_set_speed", "speed", 100);
			wait_ms(1300);
			render_and_save(icon, &pic, "style-icon-chip-paused");
			uint32_t plain_w = opaque_columns(&pic);
			check(plain_w < paused_w, "and closes up to fit shorter text (%u, was %u)", plain_w, paused_w);
			call_int(media, "warp_set_speed", "speed", 50);
		} else {
			call_int(media, "warp_step_frames", "frames", 5);
			wait_ms(400);
			snprintf(name, sizeof(name), "style-icon-%s-step", icon_styles[i]);
			picture_free(&pic);
			render_and_save(icon, &pic, name);
		}

		if (strcmp(icon_styles[i], "tag") == 0 || strcmp(icon_styles[i], "sport") == 0) {
			/* the corner of the glyph's cell is the accent */
			/* left of the glyph, halfway down, where the slant of a
			 * slanted block has come in */
			bool tag = strcmp(icon_styles[i], "tag") == 0;
			const uint8_t *c = px(&pic, tag ? 6 : 16, 32);

			check(tag ? (c[0] > 220 && c[1] > 150 && c[2] < 60) : (c[0] < 80 && c[1] > 180 && c[2] > 200),
			      "the %s icon sits on a block of its accent (%d,%d,%d)", icon_styles[i], c[0], c[1], c[2]);
		}

		if (strcmp(icon_styles[i], "badge") == 0) {
			/* a little way round the rim from the top is the accent */
			float f = media_fraction(media);
			float a = f * 0.4f * 6.2831853f;
			float r = 112.0f * 0.5f - 3.0f;
			const uint8_t *rim = px(&pic, (uint32_t)(56 + sinf(a) * r), (uint32_t)(56 - cosf(a) * r));

			check(rim[0] > 200 && rim[1] > 140 && rim[2] < 80,
			      "the badge's rim fills with the progress (%d,%d,%d)", rim[0], rim[1], rim[2]);
		}

		picture_free(&pic);
		obs_source_release(icon);
	}

	call_int(media, "warp_set_speed", "speed", 100);
}

/* Every glyph in every icon set, brought up by driving real sources through
 * each state and event, each held up long enough to read back. */
static void test_glyph_sets(obs_source_t *media)
{
	/* duotone-own is a duotone with its supporting color picked rather
	 * than shaded */
	static const char *sets[] = {"solid", "duotone", "duotone-own", "outline", "gradient"};
	struct picture pic;
	char name[128];
	char path[1024];

	printf("Icon sets\n");

	/* a playlist, for next and previous */
	obs_data_t *pl_settings = obs_data_create();
	obs_data_array_t *files = obs_data_array_create();

	for (int i = 0; i < 3; i++) {
		obs_data_t *file = obs_data_create();

		media_path(path, sizeof(path), "clip-4s.mp4");
		obs_data_set_string(file, "value", path);
		obs_data_array_push_back(files, file);
		obs_data_release(file);
	}

	obs_data_set_array(pl_settings, "playlist", files);
	obs_data_set_int(pl_settings, "transition_duration", 100);
	/* kept going for as long as the gallery takes */
	obs_data_set_bool(pl_settings, "loop_playlist", true);
	obs_data_array_release(files);

	obs_source_t *playlist = obs_source_create("warp_playlist_source", "glyph-playlist", pl_settings, NULL);
	obs_sceneitem_t *item = put_on_program(playlist);

	obs_data_release(pl_settings);
	wait_state(playlist, OBS_MEDIA_STATE_PLAYING, 4000);

	for (size_t i = 0; i < sizeof(sets) / sizeof(sets[0]); i++) {
		obs_data_t *look = obs_data_create();

		obs_data_set_string(look, "layout", "beside");
		obs_data_set_string(look, "plate", "rounded");
		obs_data_set_int(look, "plate_color", 0xE6141010);
		obs_data_set_int(look, "size", 96);
		obs_data_set_int(look, "glyph_scale", 56);
		obs_data_set_bool(look, "rounded_glyphs", true);
		bool duo = strncmp(sets[i], "duotone", 7) == 0;
		bool own = strcmp(sets[i], "duotone-own") == 0;

		obs_data_set_string(look, "glyph_set", duo ? "duotone" : sets[i]);
		obs_data_set_bool(look, "glyph_color2_own", own);
		obs_data_set_int(look, "duotone_shade", 35);
		/* a shaded duotone in amber (0xFFC233), everything else white */
		obs_data_set_int(look, "glyph_color", duo && !own ? 0xFF33C2FF : 0xFFFFFFFF);
		/* amber, as 0xAABBGGRR */
		obs_data_set_int(look, "glyph_color2", 0xFF00B4FF);
		obs_data_set_bool(look, "value_text", false);
		obs_data_set_bool(look, "combine_speed", false);
		obs_data_set_int(look, "flash_ms", 4000);

		obs_source_t *icon = make_overlay("warp_status_icon_source", "glyph-icon", media, look);
		obs_source_t *pl_icon = make_overlay("warp_status_icon_source", "glyph-pl-icon", playlist, look);

		obs_data_release(look);

#define SHOT(source, glyph_name)                                                   \
	do {                                                                        \
		wait_ms(250);                                                       \
		snprintf(name, sizeof(name), "glyph-%s-%s", sets[i], glyph_name); \
		render_and_save(source, &pic, name);                                \
	} while (0)

		load_clip(media, "clip-4s.mp4", "play");
		wait_state(media, OBS_MEDIA_STATE_PLAYING, 3000);
		wait_ms(200);
		obs_source_media_play_pause(media, true);
		SHOT(icon, "pause");

		if (strcmp(sets[i], "duotone") == 0) {
			/* the left bar leads in the icon color, the right one
			 * follows in a shade of it 35% darker */
			const uint8_t *left = px(&pic, 48 - 10, 48);
			const uint8_t *right = px(&pic, 48 + 10, 48);

			check(left[0] > 240 && left[1] > 180 && left[2] < 80,
			      "shaded duotone pause leads in amber (%d,%d,%d)", left[0], left[1], left[2]);
			check(right[0] > 145 && right[0] < 185 && right[1] > 105 && right[1] < 145 && right[2] < 60,
			      "and follows in a darker amber (%d,%d,%d)", right[0], right[1], right[2]);
		} else if (strcmp(sets[i], "duotone-own") == 0) {
			/* the left bar leads in the first color, the right one
			 * follows in the second */
			const uint8_t *left = px(&pic, 48 - 10, 48);
			const uint8_t *right = px(&pic, 48 + 10, 48);

			check(left[0] > 220 && left[1] > 220 && left[2] > 220,
			      "duotone pause leads in white (%d,%d,%d)", left[0], left[1], left[2]);
			check(right[0] > 220 && right[1] > 140 && right[1] < 210 && right[2] < 60,
			      "and follows in amber (%d,%d,%d)", right[0], right[1], right[2]);
		} else if (strcmp(sets[i], "outline") == 0) {
			/* an outlined bar is hollow down its middle */
			const uint8_t *edge = px(&pic, 48 - 15, 48);
			const uint8_t *hollow = px(&pic, 48 - 10, 48);

			check(edge[0] > 200 && hollow[0] < 120,
			      "outline pause is drawn as a line (%d at the edge, %d inside)", edge[0], hollow[0]);
		} else if (strcmp(sets[i], "gradient") == 0) {
			const uint8_t *top = px(&pic, 48 - 10, 48 - 16);
			const uint8_t *bottom = px(&pic, 48 - 10, 48 + 16);

			check(top[2] > bottom[2] + 60, "gradient pause runs from white into amber (%d to %d blue)",
			      top[2], bottom[2]);
		}
		picture_free(&pic);

		call_int(media, "warp_step_frames", "frames", 1);
		SHOT(icon, "step-forward");
		picture_free(&pic);
		call_int(media, "warp_step_frames", "frames", -1);
		SHOT(icon, "step-backward");
		picture_free(&pic);
		call_int(media, "warp_set_speed", "speed", 200);
		SHOT(icon, "fast");
		picture_free(&pic);
		call_int(media, "warp_set_speed", "speed", 50);
		SHOT(icon, "slow");
		picture_free(&pic);
		call_int(media, "warp_set_speed", "speed", 100);
		obs_source_media_play_pause(media, false);
		SHOT(icon, "play");
		picture_free(&pic);
		obs_source_media_restart(media);
		SHOT(icon, "restart");
		picture_free(&pic);
		obs_source_media_stop(media);
		wait_ms(4000);
		SHOT(icon, "stop");
		picture_free(&pic);
		load_clip(media, "clip-4s.mp4", "play");
		wait_state(media, OBS_MEDIA_STATE_ENDED, 6000);
		SHOT(icon, "ended");
		picture_free(&pic);

		obs_source_media_next(playlist);
		SHOT(pl_icon, "next");
		picture_free(&pic);
		obs_source_media_previous(playlist);
		SHOT(pl_icon, "previous");
		picture_free(&pic);

#undef SHOT

		obs_source_release(icon);
		obs_source_release(pl_icon);
	}

	obs_sceneitem_remove(item);
	obs_source_release(playlist);
}

static void test_target_by_uuid(obs_source_t *media)
{
	printf("Target kept by UUID\n");

	obs_source_t *bar = make_overlay("warp_progress_source", "rename-bar", media, NULL);
	obs_data_t *settings = obs_source_get_settings(bar);
	char *before = bstrdup(obs_data_get_string(settings, "target"));

	obs_data_release(settings);
	obs_source_set_name(media, "renamed media");

	settings = obs_source_get_settings(bar);
	check(strcmp(before, obs_data_get_string(settings, "target")) == 0 &&
		      strcmp(before, obs_source_get_uuid(media)) == 0,
	      "renaming the target leaves the setting pointing at it");
	obs_data_release(settings);

	obs_properties_t *props = obs_source_properties(bar);
	obs_property_t *list = obs_properties_get(props, "target");
	bool listed = false;

	for (size_t i = 0; i < obs_property_list_item_count(list); i++)
		if (strcmp(obs_property_list_item_string(list, i), before) == 0 &&
		    strcmp(obs_property_list_item_name(list, i), "renamed media") == 0)
			listed = true;

	check(listed, "the target list shows it under its new name");
	obs_properties_destroy(props);

	obs_source_set_name(media, "media");
	bfree(before);
	obs_source_release(bar);
}

/* ------------------------------------------------------------------------- */

static bool load_module(const char *bin, const char *data)
{
	obs_module_t *module = NULL;
	int ret = obs_open_module(&module, bin, data);

	if (ret != MODULE_SUCCESS) {
		printf("could not open %s (%d)\n", bin, ret);
		return false;
	}

	if (!obs_init_module(module)) {
		printf("could not initialise %s\n", bin);
		return false;
	}

	return true;
}

static bool load_obs_module(const char *name)
{
	char bin[1024], data[1024];

	snprintf(bin, sizeof(bin), "%s/%s.so", obs_plugins_dir, name);
	snprintf(data, sizeof(data), "%s/%s", obs_data_dir, name);

	return load_module(bin, data);
}

static bool start_obs(void)
{
	Display *display = XOpenDisplay(NULL);

	if (!display) {
		printf("no X display: run under Xvfb (xvfb-run)\n");
		return false;
	}

	obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);
	obs_set_nix_platform_display(display);

	if (!obs_startup("en-US", NULL, NULL))
		return false;

	struct obs_audio_info ai = {.samples_per_sec = 48000, .speakers = SPEAKERS_STEREO};

	if (!obs_reset_audio(&ai))
		return false;

	struct obs_video_info ovi = {
		.graphics_module = "libobs-opengl",
		.fps_num = 60,
		.fps_den = 1,
		.base_width = CANVAS_W,
		.base_height = CANVAS_H,
		.output_width = CANVAS_W,
		.output_height = CANVAS_H,
		.output_format = VIDEO_FORMAT_NV12,
		.adapter = 0,
		.gpu_conversion = true,
		.colorspace = VIDEO_CS_709,
		.range = VIDEO_RANGE_PARTIAL,
		.scale_type = OBS_SCALE_BICUBIC,
	};

	int ret = obs_reset_video(&ovi);

	if (ret != OBS_VIDEO_SUCCESS) {
		printf("could not start video (%d)\n", ret);
		return false;
	}

	/* the transitions a playlist runs through, and the text the overlays
	 * draw their numbers with */
	if (!load_obs_module("obs-transitions") || !load_obs_module("text-freetype2"))
		return false;
	if (!load_module(plugin_path, data_path))
		return false;

	obs_post_load_modules();
	return true;
}

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];
		const char *value = i + 1 < argc ? argv[i + 1] : NULL;

		if (strcmp(arg, "--verbose") == 0) {
			verbose = true;
			continue;
		}

		if (!value)
			break;

		if (strcmp(arg, "--plugin") == 0)
			plugin_path = value;
		else if (strcmp(arg, "--data") == 0)
			data_path = value;
		else if (strcmp(arg, "--obs-plugins") == 0)
			obs_plugins_dir = value;
		else if (strcmp(arg, "--obs-data") == 0)
			obs_data_dir = value;
		else if (strcmp(arg, "--media") == 0)
			media_dir = value;
		else if (strcmp(arg, "--out") == 0)
			out_dir = value;
		i++;
	}

	if (!plugin_path || !data_path || !obs_plugins_dir || !obs_data_dir || !media_dir || !out_dir) {
		printf("usage: %s --plugin Warp.so --data DIR --obs-plugins DIR --obs-data DIR --media DIR --out DIR\n",
		       argv[0]);
		return 2;
	}

	base_set_log_handler(log_handler, NULL);

	if (!start_obs()) {
		printf("could not start libobs\n");
		return 2;
	}

	obs_source_t *media = obs_source_create("warp_media_source", "media", NULL, NULL);

	program = obs_scene_create("program");
	put_on_program(media);
	obs_set_output_source(0, obs_scene_get_source(program));

	test_target_by_uuid(media);
	test_progress_bar(media);
	test_ring(media);
	test_icon(media);
	test_playlist();
	test_styles(media);
	test_glyph_sets(media);

	obs_set_output_source(0, NULL);
	obs_source_release(media);
	/* a scene belongs to the canvas it was made on until it is removed */
	obs_source_remove(obs_scene_get_source(program));
	obs_scene_release(program);

	/* sources are let go of on a thread of their own */
	obs_wait_for_destroy_queue();
	obs_shutdown();

	printf("\n%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
