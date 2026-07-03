// Copyright 2025, monado-rift-wayland contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenHMD platform-API compatibility shim (threads, mutexes,
 *         conditions, time) for the vendored Rift tracking core.
 *
 * Implemented in ohmd_shim.c with plain POSIX primitives.
 *
 * @ingroup drv_rift
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ohmd_context ohmd_context;

typedef struct ohmd_mutex ohmd_mutex;
typedef struct ohmd_cond ohmd_cond;
typedef struct ohmd_thread ohmd_thread;

/* Seconds since an arbitrary epoch, monotonic. */
double
ohmd_get_tick(void);

void
ohmd_sleep(double seconds);

ohmd_mutex *
ohmd_create_mutex(ohmd_context *ctx);
void
ohmd_destroy_mutex(ohmd_mutex *mutex);
void
ohmd_lock_mutex(ohmd_mutex *mutex);
void
ohmd_unlock_mutex(ohmd_mutex *mutex);

ohmd_cond *
ohmd_create_cond(ohmd_context *ctx);
void
ohmd_destroy_cond(ohmd_cond *cond);
void
ohmd_cond_wait(ohmd_cond *cond, ohmd_mutex *mutex);
void
ohmd_cond_signal(ohmd_cond *cond);
void
ohmd_cond_broadcast(ohmd_cond *cond);

ohmd_thread *
ohmd_create_thread(ohmd_context *ctx, unsigned int (*routine)(void *arg), void *arg);
void
ohmd_destroy_thread(ohmd_thread *thread);

#ifdef __cplusplus
}
#endif
