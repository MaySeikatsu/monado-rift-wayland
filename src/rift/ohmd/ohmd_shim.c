// Copyright 2025, monado-rift-wayland contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Implementation of the OpenHMD compatibility shim on POSIX.
 * @ingroup drv_rift
 */

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "openhmdi.h"

struct ohmd_mutex
{
	pthread_mutex_t mutex;
};

struct ohmd_cond
{
	pthread_cond_t cond;
};

struct ohmd_thread
{
	pthread_t thread;
	unsigned int (*routine)(void *arg);
	void *arg;
};

void *
ohmd_alloc(ohmd_context *ctx, size_t size)
{
	(void)ctx;
	return calloc(1, size);
}

void
ohmd_set_error(ohmd_context *ctx, const char *fmt, ...)
{
	(void)ctx;
	char buf[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	LOGE("%s", buf);
}

uint64_t
ohmd_monotonic_get(ohmd_context *ctx)
{
	(void)ctx;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t
ohmd_monotonic_per_sec(ohmd_context *ctx)
{
	(void)ctx;
	return 1000000000ULL;
}

static int
config_cache_path(const char *key, char *path, size_t path_size, bool create_dir)
{
	char dir[512];
	const char *cache = getenv("XDG_CACHE_HOME");
	if (cache != NULL && cache[0] != '\0') {
		snprintf(dir, sizeof(dir), "%s/monado-rift", cache);
	} else {
		const char *home = getenv("HOME");
		if (home == NULL || home[0] == '\0') {
			return -1;
		}
		snprintf(dir, sizeof(dir), "%s/.cache/monado-rift", home);
	}

	if (create_dir) {
		mkdir(dir, 0700); // Best effort; open() below reports real errors.
	}

	/* Keys are hash-derived filenames from the vendored code, but refuse
	 * anything that could escape the cache directory. */
	if (strchr(key, '/') != NULL || strstr(key, "..") != NULL) {
		return -1;
	}

	int n = snprintf(path, path_size, "%s/%s", dir, key);
	if (n < 0 || (size_t)n >= path_size) {
		return -1;
	}
	return 0;
}

int
ohmd_get_config(ohmd_context *ctx, const char *key, char **out_buf, unsigned long *out_len)
{
	(void)ctx;
	char path[768];
	if (config_cache_path(key, path, sizeof(path), false) < 0) {
		return -1;
	}

	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return -1;
	}

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size < 0 || size > 16 * 1024 * 1024) {
		fclose(f);
		return -1;
	}

	char *buf = malloc((size_t)size + 1);
	if (buf == NULL || fread(buf, 1, (size_t)size, f) != (size_t)size) {
		free(buf);
		fclose(f);
		return -1;
	}
	fclose(f);
	buf[size] = '\0';

	*out_buf = buf;
	*out_len = (unsigned long)size;
	return 0;
}

int
ohmd_set_config(ohmd_context *ctx, const char *key, char *buf, unsigned long len)
{
	(void)ctx;
	char path[768];
	if (config_cache_path(key, path, sizeof(path), true) < 0) {
		return -1;
	}

	FILE *f = fopen(path, "wb");
	if (f == NULL) {
		return -1;
	}

	size_t written = fwrite(buf, 1, len, f);
	fclose(f);
	return written == len ? 0 : -1;
}

double
ohmd_get_tick(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void
ohmd_sleep(double seconds)
{
	struct timespec ts;
	ts.tv_sec = (time_t)seconds;
	ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
	while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
	}
}

ohmd_mutex *
ohmd_create_mutex(ohmd_context *ctx)
{
	ohmd_mutex *mutex = ohmd_alloc(ctx, sizeof(ohmd_mutex));
	if (mutex != NULL && pthread_mutex_init(&mutex->mutex, NULL) != 0) {
		free(mutex);
		return NULL;
	}
	return mutex;
}

void
ohmd_destroy_mutex(ohmd_mutex *mutex)
{
	if (mutex == NULL) {
		return;
	}
	pthread_mutex_destroy(&mutex->mutex);
	free(mutex);
}

void
ohmd_lock_mutex(ohmd_mutex *mutex)
{
	if (mutex != NULL) {
		pthread_mutex_lock(&mutex->mutex);
	}
}

void
ohmd_unlock_mutex(ohmd_mutex *mutex)
{
	if (mutex != NULL) {
		pthread_mutex_unlock(&mutex->mutex);
	}
}

ohmd_cond *
ohmd_create_cond(ohmd_context *ctx)
{
	ohmd_cond *cond = ohmd_alloc(ctx, sizeof(ohmd_cond));
	if (cond != NULL && pthread_cond_init(&cond->cond, NULL) != 0) {
		free(cond);
		return NULL;
	}
	return cond;
}

void
ohmd_destroy_cond(ohmd_cond *cond)
{
	if (cond == NULL) {
		return;
	}
	pthread_cond_destroy(&cond->cond);
	free(cond);
}

void
ohmd_cond_wait(ohmd_cond *cond, ohmd_mutex *mutex)
{
	if (cond != NULL && mutex != NULL) {
		pthread_cond_wait(&cond->cond, &mutex->mutex);
	}
}

void
ohmd_cond_signal(ohmd_cond *cond)
{
	if (cond != NULL) {
		pthread_cond_signal(&cond->cond);
	}
}

void
ohmd_cond_broadcast(ohmd_cond *cond)
{
	if (cond != NULL) {
		pthread_cond_broadcast(&cond->cond);
	}
}

static void *
ohmd_thread_wrapper(void *arg)
{
	ohmd_thread *thread = arg;
	thread->routine(thread->arg);
	return NULL;
}

ohmd_thread *
ohmd_create_thread(ohmd_context *ctx, unsigned int (*routine)(void *arg), void *arg)
{
	ohmd_thread *thread = ohmd_alloc(ctx, sizeof(ohmd_thread));
	if (thread == NULL) {
		return NULL;
	}

	thread->routine = routine;
	thread->arg = arg;

	if (pthread_create(&thread->thread, NULL, ohmd_thread_wrapper, thread) != 0) {
		free(thread);
		return NULL;
	}
	return thread;
}

void
ohmd_destroy_thread(ohmd_thread *thread)
{
	if (thread == NULL) {
		return;
	}
	pthread_join(thread->thread, NULL);
	free(thread);
}
