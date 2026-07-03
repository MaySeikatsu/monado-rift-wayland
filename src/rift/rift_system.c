// Copyright 2013, Fredrik Hultin.
// Copyright 2013, Jakob Bornecrantz.
// Copyright 2016, Philipp Zabel.
// Copyright 2019-2021, Jan Schmidt.
// Copyright 2025, monado-rift-wayland contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Oculus Rift CV1 driver - system container and USB io thread.
 *
 * Ports the HID device handling of OpenHMD's drv_oculus_rift/rift.c onto
 * Monado: headset bring-up (feature report configuration, LED info,
 * radio pairing info), the IMU read loop, Touch controller radio
 * handling, keepalive and haptics transmission.
 *
 * @ingroup drv_rift
 */

#include <errno.h>
#include <libusb.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "os/os_time.h"

#include "util/u_debug.h"
#include "util/u_misc.h"

#include "rift_driver.h"
#include "rift_interface.h"

DEBUG_GET_ONCE_BOOL_OPTION(rift_disable_tracker, "RIFT_DISABLE_TRACKER", false)

#define OHMD_GRAVITY_EARTH 9.80665 // m/s²

#define TICK_LEN_US (1000000 / 1000) // 1000 Hz ticks, in µS
#define TICK_US_TO_NS(t) ((uint64_t)(t)*1000)
#define TICK_US_TO_SEC(t) ((float)(t) / 1000000.0f)
#define TICK_DIFF(a, b) ((int32_t)((a) - (b)))

#define SETFLAG(_s, _flag, _val) (_s) = ((_s) & ~(_flag)) | ((_val) ? (_flag) : 0)

static void
rift_system_destroy(struct rift_system *sys);

/*
 *
 * Feature report helpers.
 *
 */

static int
get_feature_report(struct rift_system *sys, rift_sensor_feature_cmd cmd, unsigned char *buf)
{
	memset(buf, 0, FEATURE_BUFFER_SIZE);
	return os_hid_get_feature(sys->hid_hmd, (uint8_t)cmd, buf, FEATURE_BUFFER_SIZE);
}

static int
send_feature_report(struct rift_system *sys, const unsigned char *data, size_t length)
{
	return os_hid_set_feature(sys->hid_hmd, data, length);
}

/*
 *
 * Headset configuration (ported from rift.c).
 *
 */

static void
set_coordinate_frame(struct rift_system *sys, rift_coordinate_frame coordframe)
{
	unsigned char buf[FEATURE_BUFFER_SIZE];

	sys->coordinate_frame = coordframe;

	SETFLAG(sys->sensor_config.flags, RIFT_SCF_SENSOR_COORDINATES, coordframe == RIFT_CF_SENSOR);

	int size = encode_sensor_config(buf, &sys->sensor_config);
	if (send_feature_report(sys, buf, size) < 0) {
		RIFT_ERROR("send_feature_report failed in set_coordinate_frame");
		return;
	}

	/* Read the state back to check whether the setting stuck. */
	size = get_feature_report(sys, RIFT_CMD_SENSOR_CONFIG, buf);
	if (size <= 0) {
		RIFT_WARN("Could not set coordinate frame");
		sys->hw_coordinate_frame = RIFT_CF_HMD;
		return;
	}

	decode_sensor_config(&sys->sensor_config, buf, size);
	sys->hw_coordinate_frame =
	    (sys->sensor_config.flags & RIFT_SCF_SENSOR_COORDINATES) ? RIFT_CF_SENSOR : RIFT_CF_HMD;

	if (sys->hw_coordinate_frame != coordframe) {
		RIFT_WARN("Coordinate frame didn't stick");
	}
}

static int
rift_send_tracking_config(struct rift_system *sys, bool blink, uint16_t exposure_us, uint16_t period_us)
{
	pkt_tracking_config tracking_config = {0};
	unsigned char buf[FEATURE_BUFFER_SIZE];
	int size;

	tracking_config.vsync_offset = RIFT_TRACKING_VSYNC_OFFSET;
	tracking_config.duty_cycle = RIFT_TRACKING_DUTY_CYCLE;
	tracking_config.exposure_us = exposure_us;
	tracking_config.period_us = period_us;

	if (blink) {
		tracking_config.pattern = 0;
		tracking_config.flags = RIFT_TRACKING_ENABLE | RIFT_TRACKING_USE_CARRIER | RIFT_TRACKING_AUTO_INCREMENT;
	} else {
		tracking_config.pattern = 0xff;
		tracking_config.flags = RIFT_TRACKING_ENABLE | RIFT_TRACKING_USE_CARRIER;
	}

	size = encode_tracking_config(buf, &tracking_config);
	if (send_feature_report(sys, buf, size) < 0) {
		RIFT_ERROR("Error sending LED tracking config");
		return -1;
	}

	return 0;
}

static void
rift_send_keepalive(struct rift_system *sys)
{
	unsigned char buffer[FEATURE_BUFFER_SIZE];

	pkt_keep_alive keep_alive = {0, sys->sensor_config.keep_alive_interval};
	int ka_size;
	if (sys->variant == RIFT_VARIANT_DK2 || sys->variant == RIFT_VARIANT_CV1) {
		ka_size = encode_dk2_keep_alive(buffer, &keep_alive);
	} else {
		ka_size = encode_dk1_keep_alive(buffer, &keep_alive);
	}
	if (send_feature_report(sys, buffer, ka_size) < 0) {
		RIFT_WARN("Error sending keepalive");
	}
}

/*
 * Obtains the positions and blinking patterns of the IR LEDs from the
 * Rift, plus the IMU position within the headset.
 */
static int
rift_get_led_info(struct rift_system *sys)
{
	int first_index = -1;
	unsigned char buf[FEATURE_BUFFER_SIZE];
	int size;
	int num_leds = 0;

	// Get LED positions
	while (true) {
		pkt_position_info pos;

		size = get_feature_report(sys, RIFT_CMD_POSITION_INFO, buf);
		if (size <= 0 || !decode_position_info(&pos, buf, size) || first_index == pos.index) {
			break;
		}

		if (first_index < 0) {
			first_index = pos.index;
			rift_leds_init(&sys->leds, pos.num);
		}

		if (pos.flags == 1) { // IMU position record
			sys->imu_pos.x = (float)pos.pos_x;
			sys->imu_pos.y = (float)pos.pos_y;
			sys->imu_pos.z = (float)pos.pos_z;
			RIFT_TRACE("IMU index %d pos x/y/z %d/%d/%d", pos.index, pos.pos_x, pos.pos_y, pos.pos_z);
			ovec3f_multiply_scalar(&sys->imu_pos, 1.0 / 1000000.0, &sys->imu_pos); /* to metres */
		} else if (pos.flags == 2) {
			rift_led *led = &sys->leds.points[pos.index];
			led->id = pos.index;
			led->pos.x = (float)pos.pos_x;
			led->pos.y = (float)pos.pos_y;
			led->pos.z = (float)pos.pos_z;
			led->dir.x = (float)pos.dir_x;
			led->dir.y = (float)pos.dir_y;
			led->dir.z = (float)pos.dir_z;
			led->pattern = 0xff;
			ovec3f_multiply_scalar(&led->pos, 1.0 / 1000000.0, &led->pos); /* to metres */
			ovec3f_normalize_me(&led->dir);
			if (pos.index >= num_leds) {
				num_leds = pos.index + 1;
			}
			RIFT_TRACE("LED index %d pos x/y/z %d/%d/%d", pos.index, pos.pos_x, pos.pos_y, pos.pos_z);
		}
	}
	sys->leds.num_points = num_leds;

	// Get LED patterns
	first_index = -1;
	while (true) {
		pkt_led_pattern_report pkt;
		int8_t pattern_length;
		int32_t pattern;

		size = get_feature_report(sys, RIFT_CMD_PATTERN_INFO, buf);
		if (size <= 0 || !decode_led_pattern_info(&pkt, buf, size) || first_index == pkt.index) {
			break;
		}

		if (first_index < 0) {
			first_index = pkt.index;
			if (sys->leds.num_points != pkt.num) {
				RIFT_ERROR("LED positions count doesn't match pattern count - got %d patterns for %d "
				           "LEDs",
				           pkt.num, sys->leds.num_points);
				return -1;
			}
		}
		if (pkt.index >= sys->leds.num_points) {
			RIFT_ERROR("Invalid LED pattern index %d (%d LEDs)", pkt.index, sys->leds.num_points);
			return -1;
		}

		pattern_length = pkt.pattern_length;
		pattern = pkt.pattern;

		/* pattern_length should be 10 */
		if (pattern_length != 10) {
			RIFT_ERROR("Unexpected LED pattern length: %d", pattern_length);
			return -1;
		}

		/*
		 * pattern should consist of 10 2-bit values that are either
		 * 1 (dark) or 3 (bright).
		 */
		if ((pattern & ~0xaaaaa) != 0x55555) {
			RIFT_ERROR("Unexpected LED pattern: 0x%x", pattern);
			return -1;
		}

		/* Convert into 10 single-bit values 1 -> 0, 3 -> 1 */
		pattern &= 0xaaaaa;
		pattern |= pattern >> 1;
		pattern &= 0x66666;
		pattern |= pattern >> 2;
		pattern &= 0xe1e1e;
		pattern |= pattern >> 4;
		pattern &= 0xe01fe;
		pattern |= pattern >> 8;
		pattern = (pattern >> 1) & 0x3ff;

		sys->leds.points[pkt.index].pattern = pattern;
	}

	/* Filter out the LEDs on the back of the headset strap, until the
	 * positional tracking copes with the device articulation. A camera
	 * that sees the back LEDs would extract the wrong position.
	 * Headset LEDs have a Z < -100mm */
	{
		int in_index, out_index = 0;
		for (in_index = 0; in_index < sys->leds.num_points; in_index++) {
			rift_led *led = &sys->leds.points[in_index];
			if (led->pos.z < -0.1) {
				RIFT_DEBUG("Dropping headband LED { .pos = {%f,%f,%f} }", led->pos.x, led->pos.y,
				           led->pos.z);
				continue;
			}
			if (in_index != out_index) {
				sys->leds.points[out_index] = *led;
			}
			out_index++;
		}
		sys->leds.num_points = out_index;
	}

	rift_leds_dump(&sys->leds, "HMD LEDs");

	return 0;
}

/*
 *
 * IMU handling (ported from rift.c handle_tracker_sensor_msg).
 *
 */

static void
handle_tracker_sensor_msg(struct rift_system *sys, uint64_t local_ts, unsigned char *buffer, int size)
{
	if (buffer[0] == RIFT_IRQ_SENSORS_DK1 && !decode_tracker_sensor_msg_dk1(&sys->sensor, buffer, size)) {
		RIFT_ERROR("Couldn't decode tracker sensor message");
		return;
	}
	if (buffer[0] == RIFT_IRQ_SENSORS_DK2 /* DK2 and CV1 variant */
	    && !decode_tracker_sensor_msg_dk2(&sys->sensor, buffer, size)) {
		RIFT_ERROR("Couldn't decode tracker sensor message");
		return;
	}

	pkt_tracker_sensor *s = &sys->sensor;
	vec3f raw_mag;

	int32_t mag32[] = {s->mag[0], s->mag[1], s->mag[2]};
	vec3f_from_rift_vec(mag32, &raw_mag);

	uint32_t dt = TICK_LEN_US;

	/* If we have a gap since the last sample handled, treat the
	 * first sample here as having the full dt */
	if (sys->have_imu_timestamp) {
		dt = (s->timestamp - sys->last_imu_timestamp);
		dt -= (s->num_samples - 1) * TICK_LEN_US;
	}

	/* Compute the starting timestamps, in local system time and device time */
	uint32_t total_dt = (s->num_samples - 1) * TICK_LEN_US + dt;
	uint32_t device_ts = s->timestamp - total_dt;
	bool sent_exposure_update = false;

	local_ts -= TICK_US_TO_NS(total_dt);

	for (int i = 0; i < s->num_samples; i++) {
		vec3f raw_gyro, gyro;
		vec3f raw_accel, accel;

		/* if the exposure timestamp is earlier than this sample, report it now */
		if (sys->tracker != NULL && !sent_exposure_update &&
		    TICK_DIFF(device_ts, s->exposure_timestamp) >= 0) {
			rift_tracker_on_new_exposure(sys->tracker, device_ts, s->exposure_count, s->exposure_timestamp,
			                             s->led_pattern_phase);
			sent_exposure_update = true;
		}

		vec3f_from_rift_vec(s->samples[i].accel, &raw_accel);
		vec3f_from_rift_vec(s->samples[i].gyro, &raw_gyro);

		/* If the rift isn't applying calibration, we should */
		if (!(sys->sensor_config.flags & RIFT_SCF_USE_CALIBRATION)) {
			/* Apply the rotation matrix first, and then add the provided factory offsets */
			ovec3f_multiply_mat3x3(&raw_gyro, sys->imu_calibration.gyro_matrix, &gyro);
			ovec3f_add(&gyro, &sys->imu_calibration.gyro_offset, &gyro);

			ovec3f_multiply_mat3x3(&raw_accel, sys->imu_calibration.accel_matrix, &accel);
			ovec3f_add(&accel, &sys->imu_calibration.accel_offset, &accel);
		} else {
			gyro = raw_gyro;
			accel = raw_accel;
		}

		if (sys->tracker != NULL && sys->hmd_tracked_dev != NULL) {
			rift_tracked_device_imu_update(sys->hmd_tracked_dev, local_ts, device_ts, TICK_US_TO_SEC(dt),
			                               &gyro, &accel, &raw_mag);
		}

		if (sys->hmd != NULL) {
			rift_hmd_handle_imu_sample(sys->hmd, local_ts, &accel, &gyro);
		}

		device_ts += dt;
		local_ts += TICK_US_TO_NS(dt);
		dt = TICK_LEN_US;
	}
	sys->last_imu_timestamp = s->timestamp;
	sys->have_imu_timestamp = true;

	if (sys->tracker != NULL && !sent_exposure_update) {
		rift_tracker_on_new_exposure(sys->tracker, s->timestamp, s->exposure_count, s->exposure_timestamp,
		                             s->led_pattern_phase);
	}
}

/*
 *
 * Radio (Touch controller / remote) handling.
 *
 */

static void
handle_rift_radio_message(struct rift_system *sys, uint64_t ts, pkt_rift_radio_message *msg)
{
	switch (msg->device_type) {
	case RIFT_REMOTE:
		if (sys->remote_buttons != msg->remote.buttons) {
			RIFT_TRACE("Remote buttons state 0x%02x", msg->remote.buttons);
		}
		sys->remote_buttons = msg->remote.buttons;
		break;
	case RIFT_TOUCH_CONTROLLER_LEFT:
		if (sys->touch[0] != NULL) {
			rift_touch_handle_message(sys->touch[0], ts, msg);
		}
		break;
	case RIFT_TOUCH_CONTROLLER_RIGHT:
		if (sys->touch[1] != NULL) {
			rift_touch_handle_message(sys->touch[1], ts, msg);
		}
		break;
	default: break;
	}
}

static void
handle_rift_radio_report(struct rift_system *sys, uint64_t ts, unsigned char *buffer, int size)
{
	pkt_rift_radio_report r;

	if (!decode_rift_radio_report(&r, buffer, size)) {
		return;
	}

	if (r.message[0].valid) {
		handle_rift_radio_message(sys, ts, &r.message[0]);
	}
	if (r.message[1].valid) {
		handle_rift_radio_message(sys, ts, &r.message[1]);
	}
}

/*
 *
 * Pose publishing.
 *
 */

void
rift_system_push_device_pose(struct rift_system *sys,
                             rift_tracked_device *tracked_dev,
                             struct m_imu_3dof *fallback_fusion,
                             struct m_relation_history *rh,
                             uint64_t local_ts_ns)
{
	struct xrt_space_relation relation;
	U_ZERO(&relation);

	int sensor_count = sys->tracker != NULL ? rift_tracker_get_sensor_count(sys->tracker) : 0;

	if (sys->tracker != NULL && tracked_dev != NULL) {
		posef pose;
		vec3f vel, accel, ang_vel;
		U_ZERO(&pose);
		rift_tracked_device_get_view_pose(tracked_dev, &pose, &vel, &accel, &ang_vel);

		relation.pose.orientation.x = pose.orient.x;
		relation.pose.orientation.y = pose.orient.y;
		relation.pose.orientation.z = pose.orient.z;
		relation.pose.orientation.w = pose.orient.w;
		relation.pose.position.x = pose.pos.x;
		relation.pose.position.y = pose.pos.y;
		relation.pose.position.z = pose.pos.z;
		relation.linear_velocity.x = vel.x;
		relation.linear_velocity.y = vel.y;
		relation.linear_velocity.z = vel.z;
		relation.angular_velocity.x = ang_vel.x;
		relation.angular_velocity.y = ang_vel.y;
		relation.angular_velocity.z = ang_vel.z;

		relation.relation_flags = XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
		                          XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
		                          XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT;

		if (sensor_count > 0) {
			/* Camera sensors present: the Kalman filter provides
			 * real positional tracking. */
			relation.relation_flags |= XRT_SPACE_RELATION_POSITION_VALID_BIT |
			                           XRT_SPACE_RELATION_POSITION_TRACKED_BIT |
			                           XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT;
		}
	} else if (fallback_fusion != NULL) {
		relation.pose.orientation = fallback_fusion->rot;
		relation.relation_flags =
		    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT;
	} else {
		return;
	}

	m_relation_history_push(rh, &relation, local_ts_ns);
}

/*
 *
 * io thread.
 *
 */

static void
check_keepalive(struct rift_system *sys, uint64_t now_ns)
{
	uint64_t interval_ns = (uint64_t)sys->sensor_config.keep_alive_interval * U_TIME_1MS_IN_NS;
	if (interval_ns < 300 * U_TIME_1MS_IN_NS) {
		interval_ns = 300 * U_TIME_1MS_IN_NS;
	}

	/* Renew 200ms before the device would time out. */
	if (now_ns - sys->last_keep_alive_ns >= interval_ns - 200 * U_TIME_1MS_IN_NS) {
		rift_send_keepalive(sys);
		sys->last_keep_alive_ns = now_ns;
	}
}

static void *
rift_system_run_thread(void *ptr)
{
	struct rift_system *sys = (struct rift_system *)ptr;
	unsigned char buffer[FEATURE_BUFFER_SIZE];

	os_thread_helper_lock(&sys->oth);
	while (os_thread_helper_is_running_locked(&sys->oth)) {
		os_thread_helper_unlock(&sys->oth);

		uint64_t now_ns = os_monotonic_get_ns();

		check_keepalive(sys, now_ns);

		os_mutex_lock(&sys->dev_mutex);
		if (sys->hid_radio != NULL) {
			for (int i = 0; i < 2; i++) {
				if (sys->touch[i] != NULL) {
					rift_touch_update_haptics(sys->touch[i], now_ns);
				}
			}
		}
		os_mutex_unlock(&sys->dev_mutex);

		/* Blocking read with a short timeout paces this loop. */
		int size = os_hid_read(sys->hid_hmd, buffer, sizeof(buffer), 2);
		if (size < 0) {
			RIFT_ERROR("Error reading from the HMD - stopping io thread");
			break;
		}
		if (size > 0) {
			if (buffer[0] == RIFT_IRQ_SENSORS_DK1 || buffer[0] == RIFT_IRQ_SENSORS_DK2) {
				os_mutex_lock(&sys->dev_mutex);
				handle_tracker_sensor_msg(sys, os_monotonic_get_ns(), buffer, size);
				os_mutex_unlock(&sys->dev_mutex);
			} else {
				RIFT_TRACE("Unknown HMD message type: %u", buffer[0]);
			}
		}

		/* Drain all pending controller radio messages. */
		if (sys->hid_radio != NULL) {
			while (true) {
				size = os_hid_read(sys->hid_radio, buffer, sizeof(buffer), 0);
				if (size < 0) {
					RIFT_ERROR("Error reading from the controller radio");
					break;
				}
				if (size == 0) {
					break;
				}
				if (buffer[0] == RIFT_RADIO_REPORT_ID) {
					os_mutex_lock(&sys->dev_mutex);
					handle_rift_radio_report(sys, os_monotonic_get_ns(), buffer, size);
					os_mutex_unlock(&sys->dev_mutex);
				}
			}
		}

		os_thread_helper_lock(&sys->oth);
	}
	os_thread_helper_unlock(&sys->oth);

	RIFT_DEBUG("Exiting io thread");

	return NULL;
}

/*
 *
 * System lifecycle.
 *
 */

struct rift_system *
rift_system_create(struct xrt_prober *xp,
                   enum rift_variant variant,
                   struct os_hid_device *hid_hmd,
                   struct os_hid_device *hid_radio)
{
	(void)xp;

	struct rift_system *sys = U_TYPED_CALLOC(struct rift_system);
	unsigned char buf[FEATURE_BUFFER_SIZE];
	int size;

	sys->variant = variant;
	sys->hid_hmd = hid_hmd;
	sys->hid_radio = hid_radio;
	xrt_reference_inc(&sys->ref);

	sys->base.type = XRT_TRACKING_TYPE_LIGHTHOUSE; // Outside-in IR constellation tracking
	snprintf(sys->base.name, XRT_TRACKING_NAME_LEN, "%s", "Oculus Rift CV1 Constellation Tracking");
	sys->base.offset.orientation.w = 1.0f;

	os_mutex_init(&sys->dev_mutex);
	os_thread_helper_init(&sys->oth);

	/* Send a keepalive first, to wake up the headset */
	rift_send_keepalive(sys);
	sys->last_keep_alive_ns = os_monotonic_get_ns();

	if (hid_radio != NULL) {
		rift_hmd_radio_init(&sys->radio, hid_radio);
	}

	// Read and decode the sensor range
	size = get_feature_report(sys, RIFT_CMD_RANGE, buf);
	decode_sensor_range(&sys->sensor_range, buf, size);
	dump_packet_sensor_range(&sys->sensor_range);

	size = get_feature_report(sys, RIFT_CMD_IMU_CALIBRATION, buf);
	decode_imu_calibration(&sys->imu_calibration, buf, size);
	dump_packet_imu_calibration(&sys->imu_calibration);

	// Read and decode display information
	size = get_feature_report(sys, RIFT_CMD_DISPLAY_INFO, buf);
	decode_sensor_display_info(&sys->display_info, buf, size);
	dump_packet_sensor_display_info(&sys->display_info);

	// Read and decode the sensor config
	size = get_feature_report(sys, RIFT_CMD_SENSOR_CONFIG, buf);
	decode_sensor_config(&sys->sensor_config, buf, size);
	dump_packet_sensor_config(&sys->sensor_config);

	// if the sensor has display info data, use HMD coordinate frame
	sys->coordinate_frame = sys->display_info.distortion_type != RIFT_DT_NONE ? RIFT_CF_HMD : RIFT_CF_SENSOR;

	// enable calibration, but these don't seem to stick. We check later
	// whether we need to apply it manually.
	SETFLAG(sys->sensor_config.flags, RIFT_SCF_USE_CALIBRATION, 1);
	SETFLAG(sys->sensor_config.flags, RIFT_SCF_AUTO_CALIBRATION, 1);

	// apply sensor config
	set_coordinate_frame(sys, sys->coordinate_frame);

	// Turn the screens on and set up IR LED tracking
	if (variant == RIFT_VARIANT_CV1) {
		size = encode_enable_components(buf, true, true, true);
		if (send_feature_report(sys, buf, size) < 0) {
			RIFT_ERROR("Error turning the screens on");
		}

		rift_send_tracking_config(sys, false, RIFT_TRACKING_EXPOSURE_US_CV1, RIFT_TRACKING_PERIOD_US_CV1);

		/* Read the radio ID for CV1 to enable camera sensor sync.
		 * Radio control commands go over the HMD HID interface. */
		if (!rift_hmd_radio_get_address(sys->hid_hmd, sys->radio_address)) {
			RIFT_WARN("Failed to read Touch controller radio address");
		}
	} else if (variant == RIFT_VARIANT_DK2) {
		rift_send_tracking_config(sys, false, RIFT_TRACKING_EXPOSURE_US_DK2, RIFT_TRACKING_PERIOD_US_DK2);
	}

	if (rift_get_led_info(sys) < 0) {
		RIFT_ERROR("Failed to read LED info from the device");
		goto cleanup;
	}

	// Re-read sensor settings, since the keep alive value will have been
	// ignored in favour of the default 1000 ms one.
	size = get_feature_report(sys, RIFT_CMD_SENSOR_CONFIG, buf);
	decode_sensor_config(&sys->sensor_config, buf, size);
	dump_packet_sensor_config(&sys->sensor_config);

	/* Start the constellation tracker (finds camera sensors via libusb
	 * hotplug and runs the Kalman filter fusion). */
	if (!debug_get_bool_option_rift_disable_tracker()) {
		sys->tracker = rift_tracker_new(NULL, sys->radio_address);
		if (sys->tracker == NULL) {
			RIFT_WARN("Failed to start constellation tracker, falling back to 3DoF");
		}
	} else {
		RIFT_INFO("Constellation tracker disabled by RIFT_DISABLE_TRACKER");
	}

	if (sys->tracker != NULL) {
		/* Register the HMD with the tracker. The HMD LED model is
		 * rotated 180 degrees around Y. */
		quatf imu_orient = {{0.0, 0.0, 0.0, 1.0}};
		posef imu_pose;
		oposef_init(&imu_pose, &sys->imu_pos, &imu_orient);

		quatf model_orient = {{0.0, 1.0, 0.0, 0.0}};
		vec3f model_pos = {{0.0, 0.0, 0.0}};
		posef model_pose;
		oposef_init(&model_pose, &model_pos, &model_orient);

		rift_tracked_device_imu_calibration imu_calibration;
		imu_calibration.accel_offset = sys->imu_calibration.accel_offset;
		imu_calibration.gyro_offset = sys->imu_calibration.gyro_offset;
		for (int i = 0; i < 9; i++) {
			imu_calibration.accel_matrix[i] = sys->imu_calibration.accel_matrix[i / 3][i % 3];
			imu_calibration.gyro_matrix[i] = sys->imu_calibration.gyro_matrix[i / 3][i % 3];
		}

		sys->hmd_tracked_dev =
		    rift_tracker_add_device(sys->tracker, 0, &imu_pose, &model_pose, &sys->leds, &imu_calibration);
	}

	/* Create the devices */
	sys->hmd = rift_hmd_create(sys);
	if (sys->hmd == NULL) {
		RIFT_ERROR("Failed to create HMD device");
		goto cleanup;
	}

	if (variant == RIFT_VARIANT_CV1) {
		sys->touch[0] = rift_touch_create(sys, XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER);
		sys->touch[1] = rift_touch_create(sys, XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER);
		if (sys->touch[0] == NULL || sys->touch[1] == NULL) {
			RIFT_ERROR("Failed to create Touch controller devices");
			goto cleanup;
		}
	}

	/* Start the io thread */
	if (os_thread_helper_start(&sys->oth, rift_system_run_thread, sys) != 0) {
		RIFT_ERROR("Failed to start Rift io thread");
		goto cleanup;
	}

	RIFT_INFO("Oculus Rift CV1 initialised%s",
	          sys->tracker != NULL ? " (constellation tracking enabled)" : " (3DoF mode)");

	return sys;

cleanup:
	/* Destroy any devices that were created; they hold references. */
	for (int i = 0; i < 2; i++) {
		if (sys->touch[i] != NULL) {
			struct xrt_device *xdev = &sys->touch[i]->base;
			xrt_device_destroy(&xdev);
		}
	}
	if (sys->hmd != NULL) {
		struct xrt_device *xdev = &sys->hmd->base;
		xrt_device_destroy(&xdev);
	}
	/* Drop the creator reference; frees the system. */
	rift_system_reference(&sys, NULL);
	return NULL;
}

static void
rift_system_destroy(struct rift_system *sys)
{
	RIFT_DEBUG("Destroying Rift system");

	// Stop the io thread first
	os_thread_helper_destroy(&sys->oth);

	if (sys->tracker != NULL) {
		rift_tracker_free(sys->tracker);
		sys->tracker = NULL;
	}

	if (sys->hid_radio != NULL) {
		rift_hmd_radio_clear(&sys->radio);
		os_hid_destroy(sys->hid_radio);
		sys->hid_radio = NULL;
	}

	if (sys->hid_hmd != NULL) {
		os_hid_destroy(sys->hid_hmd);
		sys->hid_hmd = NULL;
	}

	rift_leds_clear(&sys->leds);

	os_mutex_destroy(&sys->dev_mutex);

	free(sys);
}

void
rift_system_reference(struct rift_system **dst, struct rift_system *src)
{
	struct rift_system *old_dst = *dst;

	if (old_dst == src) {
		return;
	}

	if (src != NULL) {
		xrt_reference_inc(&src->ref);
	}

	*dst = src;

	if (old_dst != NULL) {
		if (xrt_reference_dec_and_is_zero(&old_dst->ref)) {
			rift_system_destroy(old_dst);
		}
	}
}

struct xrt_device *
rift_system_get_hmd(struct rift_system *sys)
{
	if (sys->hmd == NULL) {
		return NULL;
	}
	return &sys->hmd->base;
}

struct xrt_device *
rift_system_get_controller(struct rift_system *sys, int index)
{
	if (index < 0 || index > 1 || sys->touch[index] == NULL) {
		return NULL;
	}
	return &sys->touch[index]->base;
}

/*
 *
 * USB fixup helper.
 *
 */

bool
rift_usb_reattach_kernel_driver(uint16_t vid, uint16_t pid)
{
	libusb_context *ctx = NULL;
	libusb_device **devs = NULL;
	bool reattached = false;

	if (libusb_init(&ctx) != 0) {
		return false;
	}

	ssize_t count = libusb_get_device_list(ctx, &devs);
	for (ssize_t i = 0; i < count; i++) {
		struct libusb_device_descriptor desc;
		if (libusb_get_device_descriptor(devs[i], &desc) != 0) {
			continue;
		}
		if (desc.idVendor != vid || desc.idProduct != pid) {
			continue;
		}

		libusb_device_handle *handle = NULL;
		if (libusb_open(devs[i], &handle) != 0) {
			continue;
		}

		int n_ifaces = 2;
		struct libusb_config_descriptor *config = NULL;
		if (libusb_get_active_config_descriptor(devs[i], &config) == 0) {
			n_ifaces = config->bNumInterfaces;
			libusb_free_config_descriptor(config);
		}

		for (int iface = 0; iface < n_ifaces; iface++) {
			/* 0 = no kernel driver bound. 1 covers both usbhid and
			 * a live usbfs claim by another process; in the latter
			 * case attaching would fail anyway, so leave it be. */
			if (libusb_kernel_driver_active(handle, iface) != 0) {
				continue;
			}
			if (libusb_attach_kernel_driver(handle, iface) == 0) {
				RIFT_INFO("Reattached kernel HID driver to %04x:%04x interface %d "
				          "(a previous runtime left it detached)",
				          vid, pid, iface);
				reattached = true;
			}
		}
		libusb_close(handle);
	}

	if (devs != NULL) {
		libusb_free_device_list(devs, 1);
	}
	libusb_exit(ctx);
	return reattached;
}
