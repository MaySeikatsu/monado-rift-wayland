// Copyright 2013, Fredrik Hultin.
// Copyright 2013, Jakob Bornecrantz.
// Copyright 2016, Philipp Zabel.
// Copyright 2019-2021, Jan Schmidt.
// Copyright 2025, monado-rift-wayland contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Oculus Rift CV1 driver - internal interface.
 *
 * Native Monado driver for the Oculus Rift CV1 HMD and Touch controllers,
 * including optional 6DoF constellation (LED + camera) tracking using the
 * tracking core from Jan Schmidt's OpenHMD work (vendored under ohmd/).
 *
 * @ingroup drv_rift
 */

#pragma once

#include "os/os_hid.h"
#include "os/os_threading.h"

#include "math/m_imu_3dof.h"
#include "math/m_relation_history.h"

#include "util/u_logging.h"
#include "util/u_var.h"

#include "xrt/xrt_defines.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_prober.h"
#include "xrt/xrt_tracking.h"

#include "ohmd/drv_oculus_rift/rift.h"
#include "ohmd/drv_oculus_rift/rift-hmd-radio.h"
#include "ohmd/drv_oculus_rift/rift-tracker.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Defined in target_builder_rift.c (or the test target). Shared with the
 * vendored tracking core via the openhmdi.h shim. */
extern enum u_logging_level rift_log_level;

#define RIFT_TRACE(...) U_LOG_IFL_T(rift_log_level, __VA_ARGS__)
#define RIFT_DEBUG(...) U_LOG_IFL_D(rift_log_level, __VA_ARGS__)
#define RIFT_INFO(...) U_LOG_IFL_I(rift_log_level, __VA_ARGS__)
#define RIFT_WARN(...) U_LOG_IFL_W(rift_log_level, __VA_ARGS__)
#define RIFT_ERROR(...) U_LOG_IFL_E(rift_log_level, __VA_ARGS__)

enum rift_variant
{
	RIFT_VARIANT_CV1,
	RIFT_VARIANT_DK2,
};

struct rift_system;

/*!
 * Distortion parameters for one eye, for the OpenHMD "universal"
 * distortion model (Catmull-Rom style polynomial + chromatic aberration).
 */
struct rift_distortion_values
{
	float warp_k[4];
	float aberration_k[3];
	struct xrt_vec2 lens_center;
	struct xrt_vec2 viewport_scale;
	float warp_scale;
};

/*!
 * The Oculus Rift HMD device.
 *
 * @implements xrt_device
 */
struct rift_hmd
{
	struct xrt_device base;

	struct rift_system *sys;

	//! Fallback orientation fusion, used when no camera sensors exist.
	struct m_imu_3dof fusion;

	//! Pose history fed by the IMU/tracker thread, read by get_tracked_pose.
	struct m_relation_history *rh;

	struct rift_distortion_values distortion_vals[2];

	//! Eye height used for the fixed position in 3DoF mode (meters).
	float default_eye_height;

	//! Debug-UI copy of the most recent tracked pose.
	struct xrt_pose last_pose;

	//! Number of camera sensors seen by the tracker (updated per IMU batch).
	int sensor_count;
};

/*!
 * State for the haptic feedback of one Touch controller.
 * Mirrors OpenHMD's rift_haptic_state handling.
 */
struct rift_touch_haptics
{
	bool dirty;       //!< State needs (re)sending to the controller
	bool in_progress; //!< A radio command is pending completion

	bool haptics_on;
	bool low_freq;     //!< true for 160Hz, false for 320Hz
	uint8_t amplitude; //!< 0..255

	uint64_t duration_ns; //!< Requested pulse duration
	uint64_t end_time_ns; //!< Monotonic deadline for the active pulse
};

/*!
 * One Oculus Touch controller (CV1 generation).
 *
 * @implements xrt_device
 */
struct rift_touch
{
	struct xrt_device base;

	struct rift_system *sys;

	//! Radio device type: RIFT_TOUCH_CONTROLLER_LEFT or _RIGHT.
	int device_num;
	//! Device id in the constellation tracker (right = 1, left = 2).
	int tracker_id;

	rift_tracked_device *tracked_dev;

	bool have_calibration;
	rift_touch_calibration calibration;

	//! First radio message from this controller was logged.
	bool seen_radio_message;
	//! Rate limit for the "calibration still pending" warning.
	uint64_t last_calib_warn_ns;

	bool time_valid;
	uint32_t last_timestamp;
	uint64_t last_msg_local_ts_ns;

	/* Raw input state, written by the system io thread under dev_mutex. */
	uint8_t buttons;
	float trigger;
	float grip;
	float stick[2];
	float cap_a_x;
	float cap_b_y;
	float cap_rest;
	float cap_stick;
	float cap_trigger;

	struct rift_touch_haptics haptics;

	//! Fallback orientation fusion, used when no camera sensors exist.
	struct m_imu_3dof fusion;

	struct m_relation_history *rh;
};

/*!
 * Container for the single USB device that is an Oculus Rift CV1: the HMD
 * itself (HID interface 0), the wireless radio link to the Touch
 * controllers (HID interface 1) and the constellation tracker consuming
 * the USB camera sensors.
 */
struct rift_system
{
	struct xrt_tracking_origin base;
	struct xrt_reference ref;

	enum rift_variant variant;

	struct os_hid_device *hid_hmd;
	struct os_hid_device *hid_radio;

	//! io thread: HID reads, keepalive, haptics, IMU->tracker feeding.
	struct os_thread_helper oth;

	//! Protects device state, radio state and haptics.
	struct os_mutex dev_mutex;

	/* Configuration read from the headset at startup. */
	pkt_sensor_range sensor_range;
	pkt_imu_calibration imu_calibration;
	pkt_sensor_display_info display_info;
	pkt_sensor_config sensor_config;

	/* IMU decode state. */
	pkt_tracker_sensor sensor;
	bool have_imu_timestamp;
	uint32_t last_imu_timestamp;

	uint64_t last_keep_alive_ns;

	rift_coordinate_frame coordinate_frame, hw_coordinate_frame;

	//! Position of the IMU within the HMD (from the LED position report).
	vec3f imu_pos;
	//! HMD LED constellation model.
	rift_leds leds;

	uint8_t radio_address[5];
	rift_hmd_radio_state radio;

	//! Constellation tracker context, NULL when disabled/unavailable.
	rift_tracker_ctx *tracker;
	rift_tracked_device *hmd_tracked_dev;

	/* Recenter support (all io-thread state, guarded by dev_mutex).
	 * world_from_tracker is applied to every pose the tracker reports:
	 * it starts as a 180° Y rotation (the tracker world has the wearer
	 * facing +Z at init) and is recomputed on demand so that the HMD's
	 * current yaw/XZ position become forward/origin. */
	struct xrt_pose world_from_tracker;
	uint64_t recenter_hold_start_ns;
	bool recenter_hold_done;
	uint64_t last_recenter_file_check_ns;

	//! Buttons of the simple Oculus remote that shipped with the CV1.
	uint16_t remote_buttons;

	struct rift_hmd *hmd;
	//! 0 = left, 1 = right.
	struct rift_touch *touch[2];
};

/*
 *
 * rift_system.c
 *
 */

struct rift_system *
rift_system_create(struct xrt_prober *xp,
                   enum rift_variant variant,
                   struct os_hid_device *hid_hmd,
                   struct os_hid_device *hid_radio);

struct xrt_device *
rift_system_get_hmd(struct rift_system *sys);

struct xrt_device *
rift_system_get_controller(struct rift_system *sys, int index);

void
rift_system_reference(struct rift_system **dst, struct rift_system *src);

/*!
 * If a previous runtime (OpenHMD via hidapi-libusb, another Monado, a
 * crashed process) detached the kernel HID driver from the headset's USB
 * interfaces and never reattached it, no hidraw nodes exist and probing
 * fails. This asks the kernel to take unbound interfaces back.
 *
 * @return true if at least one interface was reattached.
 */
bool
rift_usb_reattach_kernel_driver(uint16_t vid, uint16_t pid);

/*!
 * Push the current tracker (or fusion fallback) pose of a tracked device
 * into the given relation history. Called from the io thread.
 */
void
rift_system_push_device_pose(struct rift_system *sys,
                             rift_tracked_device *tracked_dev,
                             struct m_imu_3dof *fallback_fusion,
                             struct m_relation_history *rh,
                             uint64_t local_ts_ns);

/*!
 * Re-yaw and re-origin the reported playspace so the HMD's current
 * facing direction becomes forward and its XZ position the origin.
 * Called from the io thread with dev_mutex held.
 */
void
rift_system_recenter(struct rift_system *sys);

/*!
 * Track a hold of the right controller's Oculus button; a ~1s hold
 * triggers a recenter. Called from the io thread with dev_mutex held.
 */
void
rift_system_handle_recenter_button(struct rift_system *sys, uint64_t now_ns, bool pressed);

/*
 *
 * rift_hmd.c
 *
 */

struct rift_hmd *
rift_hmd_create(struct rift_system *sys);

/*!
 * Handle one decoded HMD IMU sample. Called from the io thread with
 * dev_mutex held.
 */
void
rift_hmd_handle_imu_sample(struct rift_hmd *hmd, uint64_t local_ts_ns, const vec3f *accel, const vec3f *gyro);

/*
 *
 * rift_touch.c
 *
 */

struct rift_touch *
rift_touch_create(struct rift_system *sys, enum xrt_device_type device_type);

/*!
 * Handle one radio message for this controller. Called from the io
 * thread with dev_mutex held.
 */
void
rift_touch_handle_message(struct rift_touch *touch, uint64_t local_ts_ns, pkt_rift_radio_message *msg);

/*!
 * Send any pending haptics state over the radio. Called from the io
 * thread with dev_mutex held.
 */
void
rift_touch_update_haptics(struct rift_touch *touch, uint64_t now_ns);

#ifdef __cplusplus
}
#endif
