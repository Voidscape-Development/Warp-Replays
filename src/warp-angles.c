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

#include <string.h>

#include <obs-module.h>
#include <util/darray.h>
#include <util/threading.h>

#include <plugin-support.h>

#include "warp-angles.h"

#define WARP_ANGLES_LOG(level, format, ...) blog(level, "[Warp Angles]: " format, ##__VA_ARGS__)

/* the keys a set is saved under, which are also the field names the websocket
 * answers with */
#define WARP_ANGLES_KEY_BUFFER "buffer"
#define WARP_ANGLES_KEY_SECONDS "seconds"
#define WARP_ANGLES_KEY_ANGLES "angles"
#define WARP_ANGLES_KEY_NAME "name"
#define WARP_ANGLES_KEY_PATH "path"
#define WARP_ANGLES_KEY_DURATION "duration"

struct warp_angle {
	char *name;
	char *path;
	/* how long that clip is, in milliseconds; zero when it could not be
	 * read, which leaves the angle reachable but not lined up */
	int64_t duration;
};

struct warp_angle_set {
	char *buffer;
	int seconds;
	DARRAY(struct warp_angle) angles;
};

static pthread_mutex_t warp_angles_mutex;
static bool warp_angles_ready = false;
static DARRAY(struct warp_angle_set) warp_angle_sets;

/* ------------------------------------------------------------------------- */
/* small helpers */

static void warp_angle_set_free(struct warp_angle_set *set)
{
	for (size_t i = 0; i < set->angles.num; i++) {
		bfree(set->angles.array[i].name);
		bfree(set->angles.array[i].path);
	}

	da_free(set->angles);
	bfree(set->buffer);
}

/* expects the lock to be held */
static void warp_angles_clear_locked(void)
{
	for (size_t i = 0; i < warp_angle_sets.num; i++)
		warp_angle_set_free(&warp_angle_sets.array[i]);

	da_free(warp_angle_sets);
}

/* expects the lock to be held */
static struct warp_angle_set *warp_angles_find(const char *path, size_t *index)
{
	if (!path || !*path)
		return NULL;

	for (size_t i = 0; i < warp_angle_sets.num; i++) {
		struct warp_angle_set *set = &warp_angle_sets.array[i];

		for (size_t j = 0; j < set->angles.num; j++) {
			if (strcmp(set->angles.array[j].path, path) == 0) {
				if (index)
					*index = j;

				return set;
			}
		}
	}

	return NULL;
}

/* Drops the set the clip at 'path' is in, so a path written a second time does
 * not turn up in two sets at once. Expects the lock to be held. */
static void warp_angles_forget(const char *path)
{
	size_t index;
	struct warp_angle_set *set = warp_angles_find(path, &index);

	if (!set)
		return;

	size_t at = (size_t)(set - warp_angle_sets.array);

	warp_angle_set_free(set);
	da_erase(warp_angle_sets, at);
}

/* ------------------------------------------------------------------------- */

void warp_angles_init(void)
{
	if (warp_angles_ready)
		return;

	pthread_mutex_init(&warp_angles_mutex, NULL);
	da_init(warp_angle_sets);

	warp_angles_ready = true;
}

void warp_angles_shutdown(void)
{
	if (!warp_angles_ready)
		return;

	warp_angles_ready = false;

	pthread_mutex_lock(&warp_angles_mutex);
	warp_angles_clear_locked();
	pthread_mutex_unlock(&warp_angles_mutex);

	pthread_mutex_destroy(&warp_angles_mutex);
}

void warp_angles_add(const char *buffer_name, int seconds, size_t count, const char *const *names,
		     const char *const *paths, const int64_t *durations)
{
	if (!warp_angles_ready || !count || !names || !paths)
		return;

	if (count > WARP_ANGLES_MAX)
		count = WARP_ANGLES_MAX;

	struct warp_angle_set set = {0};

	da_init(set.angles);
	set.buffer = bstrdup(buffer_name);
	set.seconds = seconds;

	for (size_t i = 0; i < count; i++) {
		if (!paths[i] || !*paths[i])
			continue;

		struct warp_angle angle = {0};

		angle.name = bstrdup(names[i] ? names[i] : "");
		angle.path = bstrdup(paths[i]);
		angle.duration = durations ? durations[i] : 0;

		da_push_back(set.angles, &angle);
	}

	if (!set.angles.num) {
		warp_angle_set_free(&set);
		return;
	}

	pthread_mutex_lock(&warp_angles_mutex);

	for (size_t i = 0; i < set.angles.num; i++)
		warp_angles_forget(set.angles.array[i].path);

	while (warp_angle_sets.num >= WARP_ANGLES_MAX_SETS) {
		warp_angle_set_free(&warp_angle_sets.array[0]);
		da_erase(warp_angle_sets, 0);
	}

	da_push_back(warp_angle_sets, &set);

	size_t num = set.angles.num;

	pthread_mutex_unlock(&warp_angles_mutex);

	if (num > 1)
		WARP_ANGLES_LOG(LOG_INFO, "'%s' saved %d seconds from %d angles", buffer_name ? buffer_name : "",
				seconds, (int)num);
}

size_t warp_angles_count(const char *path, size_t *index)
{
	size_t count = 0;

	if (!warp_angles_ready)
		return 0;

	pthread_mutex_lock(&warp_angles_mutex);

	struct warp_angle_set *set = warp_angles_find(path, index);

	if (set)
		count = set->angles.num;

	pthread_mutex_unlock(&warp_angles_mutex);

	return count;
}

bool warp_angles_get(const char *path, size_t index, char **out_path, char **out_name, int64_t *out_duration)
{
	bool found = false;

	if (!warp_angles_ready)
		return false;

	pthread_mutex_lock(&warp_angles_mutex);

	struct warp_angle_set *set = warp_angles_find(path, NULL);

	if (set && index < set->angles.num) {
		struct warp_angle *angle = &set->angles.array[index];

		if (out_path)
			*out_path = bstrdup(angle->path);
		if (out_name)
			*out_name = bstrdup(angle->name);
		if (out_duration)
			*out_duration = angle->duration;

		found = true;
	}

	pthread_mutex_unlock(&warp_angles_mutex);

	return found;
}

bool warp_angles_map_cursor(const char *path, size_t index, int64_t cursor, int64_t *out_cursor)
{
	bool mapped = false;

	if (!warp_angles_ready || !out_cursor)
		return false;

	pthread_mutex_lock(&warp_angles_mutex);

	size_t from = 0;
	struct warp_angle_set *set = warp_angles_find(path, &from);

	if (set && index < set->angles.num) {
		const struct warp_angle *a = &set->angles.array[from];
		const struct warp_angle *b = &set->angles.array[index];

		/* The clips end together, so a position measured from the start
		 * of one is the same moment as that position plus the
		 * difference in their lengths measured from the start of the
		 * other. With a duration missing there is nothing to line up
		 * with, so the position is carried across as it stands. */
		int64_t at = (a->duration && b->duration) ? cursor + a->duration - b->duration : cursor;

		if (at < 0)
			at = 0;
		if (b->duration && at > b->duration)
			at = b->duration;

		*out_cursor = at;
		mapped = true;
	}

	pthread_mutex_unlock(&warp_angles_mutex);

	return mapped;
}

char *warp_angles_describe(const char *path)
{
	char *json = NULL;

	if (!warp_angles_ready)
		return NULL;

	pthread_mutex_lock(&warp_angles_mutex);

	size_t index = 0;
	struct warp_angle_set *set = warp_angles_find(path, &index);

	if (set) {
		obs_data_t *data = obs_data_create();
		obs_data_array_t *angles = obs_data_array_create();

		obs_data_set_string(data, WARP_ANGLES_KEY_BUFFER, set->buffer ? set->buffer : "");
		obs_data_set_int(data, WARP_ANGLES_KEY_SECONDS, set->seconds);
		obs_data_set_int(data, "index", (long long)index);

		for (size_t i = 0; i < set->angles.num; i++) {
			obs_data_t *item = obs_data_create();

			obs_data_set_string(item, WARP_ANGLES_KEY_NAME, set->angles.array[i].name);
			obs_data_set_string(item, WARP_ANGLES_KEY_PATH, set->angles.array[i].path);
			obs_data_set_int(item, WARP_ANGLES_KEY_DURATION, set->angles.array[i].duration);

			obs_data_array_push_back(angles, item);
			obs_data_release(item);
		}

		obs_data_set_array(data, WARP_ANGLES_KEY_ANGLES, angles);
		json = bstrdup(obs_data_get_json(data));

		obs_data_array_release(angles);
		obs_data_release(data);
	}

	pthread_mutex_unlock(&warp_angles_mutex);

	return json;
}

obs_data_array_t *warp_angles_save(void)
{
	obs_data_array_t *array = obs_data_array_create();

	if (!warp_angles_ready)
		return array;

	pthread_mutex_lock(&warp_angles_mutex);

	for (size_t i = 0; i < warp_angle_sets.num; i++) {
		struct warp_angle_set *set = &warp_angle_sets.array[i];
		obs_data_t *item = obs_data_create();
		obs_data_array_t *angles = obs_data_array_create();

		obs_data_set_string(item, WARP_ANGLES_KEY_BUFFER, set->buffer ? set->buffer : "");
		obs_data_set_int(item, WARP_ANGLES_KEY_SECONDS, set->seconds);

		for (size_t j = 0; j < set->angles.num; j++) {
			obs_data_t *angle = obs_data_create();

			obs_data_set_string(angle, WARP_ANGLES_KEY_NAME, set->angles.array[j].name);
			obs_data_set_string(angle, WARP_ANGLES_KEY_PATH, set->angles.array[j].path);
			obs_data_set_int(angle, WARP_ANGLES_KEY_DURATION, set->angles.array[j].duration);

			obs_data_array_push_back(angles, angle);
			obs_data_release(angle);
		}

		obs_data_set_array(item, WARP_ANGLES_KEY_ANGLES, angles);
		obs_data_array_push_back(array, item);

		obs_data_array_release(angles);
		obs_data_release(item);
	}

	pthread_mutex_unlock(&warp_angles_mutex);

	return array;
}

void warp_angles_load(obs_data_array_t *array)
{
	if (!warp_angles_ready)
		return;

	warp_angles_clear();

	if (!array)
		return;

	size_t count = obs_data_array_count(array);

	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(array, i);
		obs_data_array_t *angles = obs_data_get_array(item, WARP_ANGLES_KEY_ANGLES);

		if (angles) {
			size_t num = obs_data_array_count(angles);

			if (num > WARP_ANGLES_MAX)
				num = WARP_ANGLES_MAX;

			const char *names[WARP_ANGLES_MAX];
			const char *paths[WARP_ANGLES_MAX];
			int64_t durations[WARP_ANGLES_MAX];
			obs_data_t *held[WARP_ANGLES_MAX];

			for (size_t j = 0; j < num; j++) {
				held[j] = obs_data_array_item(angles, j);
				names[j] = obs_data_get_string(held[j], WARP_ANGLES_KEY_NAME);
				paths[j] = obs_data_get_string(held[j], WARP_ANGLES_KEY_PATH);
				durations[j] = obs_data_get_int(held[j], WARP_ANGLES_KEY_DURATION);
			}

			if (num)
				warp_angles_add(obs_data_get_string(item, WARP_ANGLES_KEY_BUFFER),
						(int)obs_data_get_int(item, WARP_ANGLES_KEY_SECONDS), num, names, paths,
						durations);

			for (size_t j = 0; j < num; j++)
				obs_data_release(held[j]);

			obs_data_array_release(angles);
		}

		obs_data_release(item);
	}
}

void warp_angles_clear(void)
{
	if (!warp_angles_ready)
		return;

	pthread_mutex_lock(&warp_angles_mutex);
	warp_angles_clear_locked();
	pthread_mutex_unlock(&warp_angles_mutex);
}
