// Copyright 2019-2023, Collabora, Ltd.
// Copyright 2025, monado-rift-wayland contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Oculus Rift CV1 prober/builder code.
 * @ingroup drv_rift
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "os/os_hid.h"

#include "xrt/xrt_config_drivers.h"
#include "xrt/xrt_prober.h"

#include "util/u_builders.h"
#include "util/u_debug.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_system_helpers.h"
#include "util/u_trace_marker.h"

#include "rift/rift_interface.h"
#include "rift/rift_driver.h"

/* Log level for the driver and the vendored tracking core, shared via
 * the openhmdi.h shim. */
enum u_logging_level rift_log_level;

DEBUG_GET_ONCE_LOG_OPTION(rift_log, "RIFT_LOG", U_LOGGING_WARN)
DEBUG_GET_ONCE_BOOL_OPTION(rift_enable_dk2, "RIFT_ENABLE_DK2", false)

static const char *driver_list[] = {
    "rift",
};

/*
 *
 * Helpers.
 *
 */

static struct xrt_prober_device *
rift_find_device(struct xrt_prober_device **xpdevs, size_t xpdev_count, enum rift_variant *out_variant)
{
	struct xrt_prober_device *dev =
	    u_builder_find_prober_device(xpdevs, xpdev_count, OCULUS_VR_INC_VID, OCULUS_RIFT_CV1_PID, XRT_BUS_TYPE_USB);
	if (dev != NULL) {
		*out_variant = RIFT_VARIANT_CV1;
		return dev;
	}

	if (debug_get_bool_option_rift_enable_dk2()) {
		dev = u_builder_find_prober_device(xpdevs, xpdev_count, OCULUS_VR_INC_VID, OCULUS_RIFT_DK2_PID,
		                                   XRT_BUS_TYPE_USB);
		if (dev == NULL) {
			dev = u_builder_find_prober_device(xpdevs, xpdev_count, OCULUS_VR_INC_VID,
			                                   OCULUS_RIFT_DK2_V2_PID, XRT_BUS_TYPE_USB);
		}
		if (dev != NULL) {
			*out_variant = RIFT_VARIANT_DK2;
			return dev;
		}
	}

	return NULL;
}

/*
 *
 * Member functions.
 *
 */

static xrt_result_t
rift_estimate_system(struct xrt_builder *xb, cJSON *config, struct xrt_prober *xp, struct xrt_builder_estimate *estimate)
{
	struct xrt_prober_device **xpdevs = NULL;
	size_t xpdev_count = 0;
	xrt_result_t xret = XRT_SUCCESS;
	enum rift_variant variant;

	U_ZERO(estimate);

	xret = xrt_prober_lock_list(xp, &xpdevs, &xpdev_count);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	struct xrt_prober_device *dev = rift_find_device(xpdevs, xpdev_count, &variant);
	if (dev != NULL) {
		estimate->certain.head = true;
		if (variant == RIFT_VARIANT_CV1) {
			estimate->certain.left = true;
			estimate->certain.right = true;
		}
	}

	xret = xrt_prober_unlock_list(xp, &xpdevs);
	assert(xret == XRT_SUCCESS);

	return XRT_SUCCESS;
}

static xrt_result_t
rift_open_system_impl(struct xrt_builder *xb,
                      cJSON *config,
                      struct xrt_prober *xp,
                      struct xrt_tracking_origin *origin,
                      struct xrt_system_devices *xsysd,
                      struct xrt_frame_context *xfctx,
                      struct u_builder_roles_helper *ubrh)
{
	struct xrt_prober_device **xpdevs = NULL;
	size_t xpdev_count = 0;
	xrt_result_t xret = XRT_SUCCESS;
	enum rift_variant variant;

	DRV_TRACE_MARKER();

	rift_log_level = debug_get_log_option_rift_log();

	xret = xrt_prober_lock_list(xp, &xpdevs, &xpdev_count);
	if (xret != XRT_SUCCESS) {
		goto unlock_and_fail;
	}

	struct xrt_prober_device *dev_hmd = rift_find_device(xpdevs, xpdev_count, &variant);
	if (dev_hmd == NULL) {
		goto unlock_and_fail;
	}

	struct os_hid_device *hid_hmd = NULL;
	int result = xrt_prober_open_hid_interface(xp, dev_hmd, RIFT_CV1_HMD_INTERFACE, &hid_hmd);
	if (result != 0) {
		RIFT_ERROR("Failed to open Rift HMD HID interface");
		goto unlock_and_fail;
	}

	/* The CV1 has the Touch controller radio link on interface 1. */
	struct os_hid_device *hid_radio = NULL;
	if (variant == RIFT_VARIANT_CV1) {
		result = xrt_prober_open_hid_interface(xp, dev_hmd, RIFT_CV1_RADIO_INTERFACE, &hid_radio);
		if (result != 0) {
			os_hid_destroy(hid_hmd);
			RIFT_ERROR("Failed to open Rift radio HID interface");
			goto unlock_and_fail;
		}
	}

	xret = xrt_prober_unlock_list(xp, &xpdevs);
	if (xret != XRT_SUCCESS) {
		goto fail;
	}

	struct rift_system *sys = rift_system_create(xp, variant, hid_hmd, hid_radio);
	if (sys == NULL) {
		RIFT_ERROR("Failed to initialise the Oculus Rift driver");
		goto fail;
	}

	// Create and add to list.
	struct xrt_device *hmd_xdev = rift_system_get_hmd(sys);
	xsysd->xdevs[xsysd->xdev_count++] = hmd_xdev;

	struct xrt_device *left_xdev = rift_system_get_controller(sys, 0);
	if (left_xdev != NULL) {
		xsysd->xdevs[xsysd->xdev_count++] = left_xdev;
	}
	struct xrt_device *right_xdev = rift_system_get_controller(sys, 1);
	if (right_xdev != NULL) {
		xsysd->xdevs[xsysd->xdev_count++] = right_xdev;
	}

	/* The devices hold references; the builder doesn't need its own. */
	rift_system_reference(&sys, NULL);

	// Assign to role(s).
	ubrh->head = hmd_xdev;
	ubrh->left = left_xdev;
	ubrh->right = right_xdev;

	return XRT_SUCCESS;


unlock_and_fail:
	xret = xrt_prober_unlock_list(xp, &xpdevs);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	/* Fallthrough */
fail:
	return XRT_ERROR_DEVICE_CREATION_FAILED;
}

static void
rift_destroy(struct xrt_builder *xb)
{
	free(xb);
}

/*
 *
 * 'Exported' functions.
 *
 */

struct xrt_builder *
rift_builder_create(void)
{
	struct u_builder *ub = U_TYPED_CALLOC(struct u_builder);

	// xrt_builder fields.
	ub->base.estimate_system = rift_estimate_system;
	ub->base.open_system = u_builder_open_system_static_roles;
	ub->base.destroy = rift_destroy;
	ub->base.identifier = "rift";
	ub->base.name = "Oculus Rift CV1";
	ub->base.driver_identifiers = driver_list;
	ub->base.driver_identifier_count = ARRAY_SIZE(driver_list);

	// u_builder fields.
	ub->open_system_static_roles = rift_open_system_impl;

	return &ub->base;
}
