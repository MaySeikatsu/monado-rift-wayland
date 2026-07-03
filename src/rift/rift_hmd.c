// Copyright 2013, Fredrik Hultin.
// Copyright 2013, Jakob Bornecrantz.
// Copyright 2016, Philipp Zabel.
// Copyright 2019-2021, Jan Schmidt.
// Copyright 2020-2023, Collabora, Ltd.
// Copyright 2025, monado-rift-wayland contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Oculus Rift CV1 driver - HMD device.
 *
 * Display, optics and head pose. The distortion model and the
 * hand-calibrated CV1/DK2 coefficients come from OpenHMD's universal
 * distortion shader; the compute function matches Monado's OpenHMD
 * wrapper (oh_device.c) so rendering is identical to the proven path.
 *
 * @ingroup drv_rift
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "math/m_api.h"
#include "math/m_mathinclude.h"
#include "math/m_vec2.h"

#include "os/os_time.h"

#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_distortion_mesh.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "util/u_var.h"

#include "rift_driver.h"

DEBUG_GET_ONCE_FLOAT_OPTION(rift_eye_height, "RIFT_EYE_HEIGHT", 1.6f)

/* CV1 lens separation. The CV1 reports the IPD slider value, but not the
 * optical lens centers; OpenHMD's hand-calibration measured 0.054m. */
#define RIFT_CV1_LENS_SEPARATION 0.054f

/* Fallback display parameters, used if the firmware reports zeros. */
#define RIFT_CV1_H_RESOLUTION 2160
#define RIFT_CV1_V_RESOLUTION 1200
#define RIFT_CV1_H_SCREEN_SIZE 0.14976f
#define RIFT_CV1_V_SCREEN_SIZE 0.0936f
#define RIFT_CV1_EYE_TO_SCREEN 0.041f

static inline struct rift_hmd *
rift_hmd(struct xrt_device *xdev)
{
	return (struct rift_hmd *)xdev;
}

/*
 *
 * Distortion (OpenHMD "universal" model).
 *
 */

static bool
rift_hmd_compute_distortion(struct xrt_device *xdev, uint32_t view, float u, float v, struct xrt_uv_triplet *result)
{
	struct rift_hmd *hmd = rift_hmd(xdev);
	const struct rift_distortion_values *val = &hmd->distortion_vals[view];

	struct xrt_vec2 r = {u, v};
	r = m_vec2_mul(r, val->viewport_scale);
	r = m_vec2_sub(r, val->lens_center);
	r = m_vec2_div_scalar(r, val->warp_scale);

	float r_mag = m_vec2_len(r);
	r_mag = val->warp_k[3] +                     // r^1
	        val->warp_k[2] * r_mag +             // r^2
	        val->warp_k[1] * r_mag * r_mag +     // r^3
	        val->warp_k[0] * r_mag * r_mag * r_mag; // r^4

	struct xrt_vec2 r_dist = m_vec2_mul_scalar(r, r_mag);
	r_dist = m_vec2_mul_scalar(r_dist, val->warp_scale);

	struct xrt_vec2 r_uv = m_vec2_mul_scalar(r_dist, val->aberration_k[0]);
	r_uv = m_vec2_add(r_uv, val->lens_center);
	r_uv = m_vec2_div(r_uv, val->viewport_scale);

	struct xrt_vec2 g_uv = m_vec2_mul_scalar(r_dist, val->aberration_k[1]);
	g_uv = m_vec2_add(g_uv, val->lens_center);
	g_uv = m_vec2_div(g_uv, val->viewport_scale);

	struct xrt_vec2 b_uv = m_vec2_mul_scalar(r_dist, val->aberration_k[2]);
	b_uv = m_vec2_add(b_uv, val->lens_center);
	b_uv = m_vec2_div(b_uv, val->viewport_scale);

	result->r = r_uv;
	result->g = g_uv;
	result->b = b_uv;
	return true;
}

/*
 *
 * xrt_device members.
 *
 */

static void
rift_hmd_update_inputs(struct xrt_device *xdev)
{
	(void)xdev;
}

static void
rift_hmd_get_tracked_pose(struct xrt_device *xdev,
                          enum xrt_input_name name,
                          uint64_t at_timestamp_ns,
                          struct xrt_space_relation *out_relation)
{
	struct rift_hmd *hmd = rift_hmd(xdev);

	if (name != XRT_INPUT_GENERIC_HEAD_POSE) {
		RIFT_ERROR("Unknown input name requested");
		U_ZERO(out_relation);
		return;
	}

	struct xrt_space_relation relation;
	U_ZERO(&relation);

	m_relation_history_get(hmd->rh, at_timestamp_ns, &relation);

	if ((relation.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT) == 0 &&
	    (relation.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) != 0) {
		/* 3DoF mode: put the view at a fixed standing eye height so
		 * apps that don't handle missing position still work. */
		relation.pose.position.x = 0.0f;
		relation.pose.position.y = hmd->default_eye_height;
		relation.pose.position.z = 0.0f;
		relation.relation_flags |= XRT_SPACE_RELATION_POSITION_VALID_BIT;
	}

	hmd->last_pose = relation.pose;

	*out_relation = relation;
}

static void
rift_hmd_destroy(struct xrt_device *xdev)
{
	struct rift_hmd *hmd = rift_hmd(xdev);

	/* Detach from the system FIRST: the io thread feeds our fusion and
	 * relation history through sys->hmd under dev_mutex, so it must not
	 * be able to reach us once we start freeing those below. */
	os_mutex_lock(&hmd->sys->dev_mutex);
	hmd->sys->hmd = NULL;
	os_mutex_unlock(&hmd->sys->dev_mutex);

	u_var_remove_root(hmd);

	m_imu_3dof_close(&hmd->fusion);
	m_relation_history_destroy(&hmd->rh);

	/* The system outlives us via refcounting. */
	rift_system_reference(&hmd->sys, NULL);

	u_device_free(&hmd->base);
}

/*
 *
 * Setup helpers.
 *
 */

static void
rift_hmd_setup_optics(struct rift_hmd *hmd)
{
	struct rift_system *sys = hmd->sys;
	pkt_sensor_display_info *info = &sys->display_info;

	uint16_t h_res = info->h_resolution;
	uint16_t v_res = info->v_resolution;
	float hss = info->h_screen_size;
	float vss = info->v_screen_size;
	float lens_sep = info->lens_separation;
	float v_center = info->v_center;
	float ets[2] = {info->eye_to_screen_distance[0], info->eye_to_screen_distance[1]};

	/* Guard against firmware reporting zeros. */
	if (h_res == 0 || v_res == 0) {
		h_res = RIFT_CV1_H_RESOLUTION;
		v_res = RIFT_CV1_V_RESOLUTION;
	}
	if (hss <= 0.0f || vss <= 0.0f) {
		hss = RIFT_CV1_H_SCREEN_SIZE;
		vss = RIFT_CV1_V_SCREEN_SIZE;
	}
	if (ets[0] <= 0.0f) {
		ets[0] = RIFT_CV1_EYE_TO_SCREEN;
	}
	if (ets[1] <= 0.0f) {
		ets[1] = ets[0];
	}

	if (sys->variant == RIFT_VARIANT_CV1) {
		/* The CV1 reports the IPD slider value in lens_separation but
		 * not the lens optical centers; use the measured constant for
		 * the optics (see OpenHMD rift.c). */
		lens_sep = RIFT_CV1_LENS_SEPARATION;
	}
	if (lens_sep <= 0.0f) {
		lens_sep = 0.063f;
	}
	if (v_center <= 0.0f || v_center >= vss) {
		v_center = vss / 2.0f;
	}

	/* Screen and view setup. */
	struct xrt_hmd_parts *hp = hmd->base.hmd;

	float refresh_hz = sys->variant == RIFT_VARIANT_CV1 ? 90.0f : 75.0f;
	hp->screens[0].nominal_frame_interval_ns = (uint64_t)(time_s_to_ns(1.0f / refresh_hz));
	hp->screens[0].w_pixels = h_res;
	hp->screens[0].h_pixels = v_res;

	for (int eye = 0; eye < 2; eye++) {
		hp->views[eye].display.w_pixels = h_res / 2;
		hp->views[eye].display.h_pixels = v_res;
		hp->views[eye].viewport.x_pixels = eye == 0 ? 0 : h_res / 2;
		hp->views[eye].viewport.y_pixels = 0;
		hp->views[eye].viewport.w_pixels = h_res / 2;
		hp->views[eye].viewport.h_pixels = v_res;
		hp->views[eye].rot = u_device_rotation_ident;
	}

	/*
	 * Field of view, from the per-eye screen bounds relative to the lens
	 * center (same math as OpenHMD rift.c, expressed as half-angles).
	 *
	 * For the left eye, the outer (left) screen edge is at
	 * -(hss/2 - lens_sep/2) from the lens center and the inner edge at
	 * +lens_sep/2. The right eye mirrors this.
	 */
	float outer = hss / 2.0f - lens_sep / 2.0f;
	float inner = lens_sep / 2.0f;
	float top = vss - v_center;
	float bottom = v_center;

	// Left eye
	hp->distortion.fov[0].angle_left = -atan2f(outer, ets[0]);
	hp->distortion.fov[0].angle_right = atan2f(inner, ets[0]);
	hp->distortion.fov[0].angle_up = atan2f(top, ets[0]);
	hp->distortion.fov[0].angle_down = -atan2f(bottom, ets[0]);

	// Right eye
	hp->distortion.fov[1].angle_left = -atan2f(inner, ets[1]);
	hp->distortion.fov[1].angle_right = atan2f(outer, ets[1]);
	hp->distortion.fov[1].angle_up = atan2f(top, ets[1]);
	hp->distortion.fov[1].angle_down = -atan2f(bottom, ets[1]);

	/*
	 * Distortion parameters (OpenHMD universal distortion model with
	 * hand-calibrated coefficients from OpenHMD rift.c).
	 */
	float warp_k[4];
	float aberration_k[3];

	switch (sys->variant) {
	case RIFT_VARIANT_DK2:
		warp_k[0] = 0.247f;
		warp_k[1] = -0.145f;
		warp_k[2] = 0.103f;
		warp_k[3] = 0.795f;
		aberration_k[0] = 0.985f;
		aberration_k[1] = 1.000f;
		aberration_k[2] = 1.015f;
		break;
	case RIFT_VARIANT_CV1:
	default:
		warp_k[0] = 0.269f;
		warp_k[1] = -0.25f;
		warp_k[2] = 0.178f;
		warp_k[3] = 0.803f;
		aberration_k[0] = 0.9992107f;
		aberration_k[1] = 1.0f;
		aberration_k[2] = 1.0120361f;
		break;
	}

	float view_w_meters = hss / 2.0f;
	float view_h_meters = vss;

	/* Lens centers, measured from each view's origin. Calibration was
	 * for the lens view to whichever screen edge is further away. */
	float lens_center_x[2] = {
	    view_w_meters - lens_sep / 2.0f, // left eye
	    lens_sep / 2.0f,                 // right eye
	};
	float warp_scale = lens_center_x[0] > lens_center_x[1] ? lens_center_x[0] : lens_center_x[1];

	for (int eye = 0; eye < 2; eye++) {
		struct rift_distortion_values *val = &hmd->distortion_vals[eye];
		for (int i = 0; i < 4; i++) {
			val->warp_k[i] = warp_k[i];
		}
		for (int i = 0; i < 3; i++) {
			val->aberration_k[i] = aberration_k[i];
		}
		val->lens_center.x = lens_center_x[eye];
		val->lens_center.y = v_center;
		val->viewport_scale.x = view_w_meters;
		val->viewport_scale.y = view_h_meters;
		val->warp_scale = warp_scale;
	}

	hp->distortion.models = XRT_DISTORTION_MODEL_COMPUTE;
	hp->distortion.preferred = XRT_DISTORTION_MODEL_COMPUTE;
	hmd->base.compute_distortion = rift_hmd_compute_distortion;
	u_distortion_mesh_fill_in_compute(&hmd->base);

	hp->blend_modes[0] = XRT_BLEND_MODE_OPAQUE;
	hp->blend_mode_count = 1;
}

/*
 *
 * io thread callbacks.
 *
 */

void
rift_hmd_handle_imu_sample(struct rift_hmd *hmd, uint64_t local_ts_ns, const vec3f *accel, const vec3f *gyro)
{
	struct xrt_vec3 a = {accel->x, accel->y, accel->z};
	struct xrt_vec3 g = {gyro->x, gyro->y, gyro->z};

	m_imu_3dof_update(&hmd->fusion, local_ts_ns, &a, &g);

	if (hmd->sys->tracker != NULL) {
		hmd->sensor_count = rift_tracker_get_sensor_count(hmd->sys->tracker);
	}

	rift_system_push_device_pose(hmd->sys, hmd->sys->hmd_tracked_dev, &hmd->fusion, hmd->rh, local_ts_ns);
}

/*
 *
 * Creation.
 *
 */

struct rift_hmd *
rift_hmd_create(struct rift_system *sys)
{
	enum u_device_alloc_flags flags = (enum u_device_alloc_flags)(U_DEVICE_ALLOC_HMD);

	struct rift_hmd *hmd = U_DEVICE_ALLOCATE(struct rift_hmd, flags, 1, 0);
	if (hmd == NULL) {
		return NULL;
	}

	/* Take a reference to the system, released on destroy. */
	rift_system_reference(&hmd->sys, sys);

	hmd->base.update_inputs = rift_hmd_update_inputs;
	hmd->base.get_tracked_pose = rift_hmd_get_tracked_pose;
	hmd->base.get_view_poses = u_device_get_view_poses;
	hmd->base.destroy = rift_hmd_destroy;
	hmd->base.name = XRT_DEVICE_GENERIC_HMD;
	hmd->base.device_type = XRT_DEVICE_TYPE_HMD;
	hmd->base.tracking_origin = &sys->base;

	hmd->base.inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;

	hmd->base.orientation_tracking_supported = true;
	hmd->base.position_tracking_supported = true;

	if (sys->variant == RIFT_VARIANT_CV1) {
		snprintf(hmd->base.str, XRT_DEVICE_NAME_LEN, "Oculus Rift CV1");
	} else {
		snprintf(hmd->base.str, XRT_DEVICE_NAME_LEN, "Oculus Rift DK2");
	}
	snprintf(hmd->base.serial, XRT_DEVICE_NAME_LEN, "%s", hmd->base.str);

	hmd->default_eye_height = debug_get_float_option_rift_eye_height();

	m_imu_3dof_init(&hmd->fusion, M_IMU_3DOF_USE_GRAVITY_DUR_20MS);
	m_relation_history_create(&hmd->rh);

	rift_hmd_setup_optics(hmd);

	u_var_add_root(hmd, "Oculus Rift CV1 HMD", true);
	u_var_add_gui_header(hmd, NULL, "Tracking");
	u_var_add_pose(hmd, &hmd->last_pose, "Tracked pose");
	u_var_add_i32(hmd, &hmd->sensor_count, "Camera sensors");
	u_var_add_f32(hmd, &hmd->default_eye_height, "3DoF eye height (m)");
	u_var_add_gui_header(hmd, NULL, "3DoF fusion");
	m_imu_3dof_add_vars(&hmd->fusion, hmd, "");

	return hmd;
}
