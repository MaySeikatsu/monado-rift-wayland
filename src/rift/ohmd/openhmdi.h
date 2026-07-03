// Copyright 2025, monado-rift-wayland contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenHMD internal-API compatibility shim for the vendored Rift
 *         tracking core, mapped onto Monado utilities.
 *
 * The files under drv_oculus_rift/ are vendored from Jan Schmidt's OpenHMD
 * rift-kalman-filter branch and include "../openhmdi.h". This header stands
 * in for OpenHMD's internal header and provides just the subset of the API
 * that the vendored code uses.
 *
 * @ingroup drv_rift
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/u_logging.h"

#include "omath.h"
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OHMD_STR_SIZE 256

#define OHMD_MAX(_a, _b) ((_a) > (_b) ? (_a) : (_b))
#define OHMD_MIN(_a, _b) ((_a) < (_b) ? (_a) : (_b))
#define OHMD_CLAMP(_a, minval, maxval) (OHMD_MAX(OHMD_MIN((_a), (maxval)), (minval)))

/* Opaque context. The vendored code only passes it through to the
 * functions below, all of which ignore it. NULL is always valid. */
typedef struct ohmd_context ohmd_context;

/* Stub for the OpenHMD device base struct embedded in rift.h's
 * rift_device_priv. The Monado driver replaces OpenHMD's device layer
 * entirely, so this only needs to exist, not to work. */
typedef struct ohmd_device
{
	int unused;
} ohmd_device;

/* Log level shared with the Monado rift driver (set from RIFT_LOG). */
extern enum u_logging_level rift_log_level;

#define LOGV(...) U_LOG_IFL_T(rift_log_level, __VA_ARGS__)
#define LOGD(...) U_LOG_IFL_D(rift_log_level, __VA_ARGS__)
#define LOGI(...) U_LOG_IFL_I(rift_log_level, __VA_ARGS__)
#define LOGW(...) U_LOG_IFL_W(rift_log_level, __VA_ARGS__)
#define LOGE(...) U_LOG_IFL_E(rift_log_level, __VA_ARGS__)

void *
ohmd_alloc(ohmd_context *ctx, size_t size);

void
ohmd_set_error(ohmd_context *ctx, const char *fmt, ...);

/* Monotonic clock in nanoseconds (OpenHMD ticks == ns on POSIX). */
uint64_t
ohmd_monotonic_get(ohmd_context *ctx);

uint64_t
ohmd_monotonic_per_sec(ohmd_context *ctx);

/* Small key/value blob store used to cache Touch controller calibration
 * data. Backed by files in $XDG_CACHE_HOME/monado-rift (or
 * ~/.cache/monado-rift). ohmd_get_config returns 0 and a malloc'd,
 * NUL-terminated buffer on success. */
int
ohmd_get_config(ohmd_context *ctx, const char *key, char **out_buf, unsigned long *out_len);

int
ohmd_set_config(ohmd_context *ctx, const char *key, char *buf, unsigned long len);

#ifdef __cplusplus
}
#endif
