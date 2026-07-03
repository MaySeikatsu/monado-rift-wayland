// Copyright 2013, Fredrik Hultin.
// Copyright 2013, Jakob Bornecrantz.
// Copyright 2016, Philipp Zabel.
// Copyright 2019-2022, Jan Schmidt.
// Copyright 2020-2023, Collabora, Ltd.
// Copyright 2025, monado-rift-wayland contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Oculus Rift CV1 driver - Touch controllers.
 *
 * Radio message handling ported from OpenHMD's rift.c; the xrt_device
 * input layout mirrors Monado's rift_s driver so the full Oculus Touch
 * interaction profile is exposed to OpenXR apps and the SteamVR plugin.
 *
 * @ingroup drv_rift
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "math/m_api.h"
#include "math/m_mathinclude.h"
#include "math/m_space.h"

#include "os/os_time.h"

#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "util/u_var.h"

#include "rift_driver.h"

DEBUG_GET_ONCE_FLOAT_OPTION(rift_eye_height, "RIFT_EYE_HEIGHT", 1.6f)

#define OHMD_GRAVITY_EARTH 9.80665 // m/s²

/* Normalized capacitance at which we consider a control "touched".
 * 1.0 corresponds to the factory calibration touch point. */
#define RIFT_TOUCH_CAP_THRESHOLD 0.75f

/* Minimum haptic pulse duration. */
#define RIFT_MIN_HAPTIC_DURATION_NS (10 * U_TIME_1MS_IN_NS)

static inline struct rift_touch *
rift_touch(struct xrt_device *xdev)
{
	return (struct rift_touch *)xdev;
}

/*
 *
 * Bindings.
 *
 */

static struct xrt_binding_input_pair simple_inputs_rift[4] = {
    {XRT_INPUT_SIMPLE_SELECT_CLICK, XRT_INPUT_TOUCH_TRIGGER_VALUE},
    {XRT_INPUT_SIMPLE_MENU_CLICK, XRT_INPUT_TOUCH_MENU_CLICK},
    {XRT_INPUT_SIMPLE_GRIP_POSE, XRT_INPUT_TOUCH_GRIP_POSE},
    {XRT_INPUT_SIMPLE_AIM_POSE, XRT_INPUT_TOUCH_AIM_POSE},
};

static struct xrt_binding_output_pair simple_outputs_rift[1] = {
    {XRT_OUTPUT_NAME_SIMPLE_VIBRATION, XRT_OUTPUT_NAME_TOUCH_HAPTIC},
};

static struct xrt_binding_profile binding_profiles_rift[1] = {
    {
        .name = XRT_DEVICE_SIMPLE_CONTROLLER,
        .inputs = simple_inputs_rift,
        .input_count = ARRAY_SIZE(simple_inputs_rift),
        .outputs = simple_outputs_rift,
        .output_count = ARRAY_SIZE(simple_outputs_rift),
    },
};

enum touch_controller_input_index
{
	/* Left controller */
	OCULUS_TOUCH_X_CLICK = 0,
	OCULUS_TOUCH_X_TOUCH,
	OCULUS_TOUCH_Y_CLICK,
	OCULUS_TOUCH_Y_TOUCH,
	OCULUS_TOUCH_MENU_CLICK,

	/* Right controller */
	OCULUS_TOUCH_A_CLICK = 0,
	OCULUS_TOUCH_A_TOUCH,
	OCULUS_TOUCH_B_CLICK,
	OCULUS_TOUCH_B_TOUCH,
	OCULUS_TOUCH_SYSTEM_CLICK,

	/* Common */
	OCULUS_TOUCH_SQUEEZE_VALUE,
	OCULUS_TOUCH_TRIGGER_TOUCH,
	OCULUS_TOUCH_TRIGGER_VALUE,
	OCULUS_TOUCH_THUMBSTICK_CLICK,
	OCULUS_TOUCH_THUMBSTICK_TOUCH,
	OCULUS_TOUCH_THUMBSTICK,
	OCULUS_TOUCH_THUMBREST_TOUCH,
	OCULUS_TOUCH_GRIP_POSE,
	OCULUS_TOUCH_AIM_POSE,

	INPUT_INDICES_LAST
};

#define SET_TOUCH_INPUT(d, NAME) ((d)->base.inputs[OCULUS_TOUCH_##NAME].name = XRT_INPUT_TOUCH_##NAME)

/*
 *
 * Radio message handling (io thread, dev_mutex held).
 *
 */

static void
rift_touch_load_calibration(struct rift_touch *touch)
{
	struct rift_system *sys = touch->sys;

	/* We need the calibration data (from controller flash, over the
	 * radio) before we can do anything more. This can fail while the
	 * controller is still waking up - we retry on the next message. */
	if (rift_touch_get_calibration(NULL, &sys->radio, touch->device_num, &touch->calibration) < 0) {
		return;
	}

	rift_touch_calibration *c = &touch->calibration;

	if (sys->tracker != NULL) {
		quatf imu_orient = {{0.0, 0.0, 0.0, 1.0}};
		posef imu_pose;
		oposef_init(&imu_pose, &c->imu_position, &imu_orient);

		quatf model_orient = {{0.0, 0.0, 0.0, 1.0}};
		vec3f model_pos = {{0.0, 0.0, 0.0}};
		posef model_pose;
		oposef_init(&model_pose, &model_pos, &model_orient);

		rift_tracked_device_imu_calibration imu_calibration;
		imu_calibration.accel_offset = c->accel_offset;
		imu_calibration.gyro_offset = c->gyro_offset;
		for (int i = 0; i < 9; i++) {
			imu_calibration.accel_matrix[i] = c->accel_matrix[i / 3][i % 3];
			imu_calibration.gyro_matrix[i] = c->gyro_matrix[i / 3][i % 3];
		}

		touch->tracked_dev = rift_tracker_add_device(sys->tracker, touch->tracker_id, &imu_pose, &model_pose,
		                                             &c->leds, &imu_calibration);
	}

	touch->have_calibration = true;

	RIFT_INFO("%s Touch controller connected and calibrated",
	          touch->base.device_type == XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER ? "Left" : "Right");
}

void
rift_touch_handle_message(struct rift_touch *touch, uint64_t local_ts_ns, pkt_rift_radio_message *msg)
{
	/* The top bits carry something unknown - ignore them */
	uint8_t buttons = msg->touch.buttons & 0xf;
	if (touch->buttons != buttons) {
		RIFT_TRACE("Touch controller %d buttons now %x", touch->device_num, buttons);
	}
	touch->buttons = buttons;

	/* An all-zero IMU block means the controller is connected but
	 * asleep - nothing more to do. */
	if (!(msg->touch.timestamp || msg->touch.accel[0] || msg->touch.accel[1] || msg->touch.accel[2] ||
	      msg->touch.gyro[0] || msg->touch.gyro[1] || msg->touch.gyro[2])) {
		return;
	}

	if (!touch->have_calibration) {
		rift_touch_load_calibration(touch);
		if (!touch->have_calibration) {
			return;
		}
	}

	rift_touch_calibration *c = &touch->calibration;

	/* Timestamps are in microseconds, 500Hz updates = 2000µs spacing */
	int32_t dt;
	if (touch->time_valid) {
		dt = msg->touch.timestamp - touch->last_timestamp;
	} else {
		dt = 2000;
	}

	uint32_t device_ts = msg->touch.timestamp - dt;
	uint64_t sample_local_ts = local_ts_ns - (uint64_t)dt * 1000; // µs -> ns

	const double dt_s = 1e-6 * dt;
	vec3f raw_accel = {{
	    OHMD_GRAVITY_EARTH / 2048 * msg->touch.accel[0],
	    OHMD_GRAVITY_EARTH / 2048 * msg->touch.accel[1],
	    OHMD_GRAVITY_EARTH / 2048 * msg->touch.accel[2],
	}};

	/* Gyro is an MPU 6500, configured for 2000°/s. The datasheet says
	 * 16.4 LSB/°/s but 32768 / 2000 = 16.384 actually yields a 2000°/s
	 * full range; converted to radians for the fusion. */
	vec3f raw_gyro = {{
	    msg->touch.gyro[0] / (16.384 * 180.0) * M_PI,
	    msg->touch.gyro[1] / (16.384 * 180.0) * M_PI,
	    msg->touch.gyro[2] / (16.384 * 180.0) * M_PI,
	}};
	vec3f mag = {{0.0f, 0.0f, 0.0f}};
	vec3f gyro;
	vec3f accel;

	/* For controllers, apply the rotation matrix first, then add the
	 * factory offsets */
	ovec3f_multiply_mat3x3(&raw_gyro, c->gyro_matrix, &gyro);
	ovec3f_add(&gyro, &c->gyro_offset, &gyro);

	ovec3f_multiply_mat3x3(&raw_accel, c->accel_matrix, &accel);
	ovec3f_add(&accel, &c->accel_offset, &accel);

	if (touch->tracked_dev != NULL) {
		rift_tracked_device_imu_update(touch->tracked_dev, sample_local_ts, device_ts, dt_s, &gyro, &accel,
		                               &mag);
	}

	struct xrt_vec3 a = {accel.x, accel.y, accel.z};
	struct xrt_vec3 g = {gyro.x, gyro.y, gyro.z};
	m_imu_3dof_update(&touch->fusion, sample_local_ts, &a, &g);

	touch->last_timestamp = msg->touch.timestamp;
	touch->time_valid = true;
	touch->last_msg_local_ts_ns = local_ts_ns;

	/* Trigger, grip and thumbstick, using the factory range calibration */
	float t;
	if (msg->touch.trigger < c->trigger_mid_range) {
		t = 1.0f - ((float)msg->touch.trigger - c->trigger_min_range) /
		               (c->trigger_mid_range - c->trigger_min_range) * 0.5f;
	} else {
		t = 0.5f - ((float)msg->touch.trigger - c->trigger_mid_range) /
		               (c->trigger_max_range - c->trigger_mid_range) * 0.5f;
	}
	touch->trigger = OHMD_CLAMP(t, 0.0f, 1.0f);

	float gr;
	if (msg->touch.grip < c->middle_mid_range) {
		gr = 1.0f - ((float)msg->touch.grip - c->middle_min_range) /
		                (c->middle_mid_range - c->middle_min_range) * 0.5f;
	} else {
		gr = 0.5f - ((float)msg->touch.grip - c->middle_mid_range) /
		                (c->middle_max_range - c->middle_mid_range) * 0.5f;
	}
	touch->grip = OHMD_CLAMP(gr, 0.0f, 1.0f);

	float joy[2];
	if (msg->touch.stick[0] >= c->joy_x_dead_min && msg->touch.stick[0] <= c->joy_x_dead_max &&
	    msg->touch.stick[1] >= c->joy_y_dead_min && msg->touch.stick[1] <= c->joy_y_dead_max) {
		joy[0] = 0.0f;
		joy[1] = 0.0f;
	} else {
		joy[0] = ((float)msg->touch.stick[0] - c->joy_x_range_min) /
		             (c->joy_x_range_max - c->joy_x_range_min) * 2.0f -
		         1.0f;
		joy[1] = ((float)msg->touch.stick[1] - c->joy_y_range_min) /
		             (c->joy_y_range_max - c->joy_y_range_min) * 2.0f -
		         1.0f;
	}
	touch->stick[0] = OHMD_CLAMP(joy[0], -1.0f, 1.0f);
	touch->stick[1] = OHMD_CLAMP(joy[1], -1.0f, 1.0f);

	/* Capacitive sensing channels arrive round-robin */
	switch (msg->touch.adc_channel) {
	case RIFT_TOUCH_CONTROLLER_HAPTIC_COUNTER:
		/* Read pointer into the 256-byte haptics ringbuffer,
		 * incremented 320 times per second. Unused. */
		break;
	case RIFT_TOUCH_CONTROLLER_ADC_STICK:
		touch->cap_stick = ((float)msg->touch.adc_value - c->cap_sense_min[0]) /
		                   (c->cap_sense_touch[0] - c->cap_sense_min[0]);
		break;
	case RIFT_TOUCH_CONTROLLER_ADC_B_Y:
		touch->cap_b_y = ((float)msg->touch.adc_value - c->cap_sense_min[1]) /
		                 (c->cap_sense_touch[1] - c->cap_sense_min[1]);
		break;
	case RIFT_TOUCH_CONTROLLER_ADC_TRIGGER:
		touch->cap_trigger = ((float)msg->touch.adc_value - c->cap_sense_min[2]) /
		                     (c->cap_sense_touch[2] - c->cap_sense_min[2]);
		break;
	case RIFT_TOUCH_CONTROLLER_ADC_A_X:
		touch->cap_a_x = ((float)msg->touch.adc_value - c->cap_sense_min[3]) /
		                 (c->cap_sense_touch[3] - c->cap_sense_min[3]);
		break;
	case RIFT_TOUCH_CONTROLLER_ADC_REST:
		touch->cap_rest = ((float)msg->touch.adc_value - c->cap_sense_min[7]) /
		                  (c->cap_sense_touch[7] - c->cap_sense_min[7]);
		break;
	default: break;
	}

	/* Publish the pose for this controller */
	rift_system_push_device_pose(touch->sys, touch->tracked_dev, &touch->fusion, touch->rh, sample_local_ts);
}

/*
 *
 * Haptics (io thread, dev_mutex held).
 *
 */

void
rift_touch_update_haptics(struct rift_touch *touch, uint64_t now_ns)
{
	struct rift_touch_haptics *h = &touch->haptics;
	struct rift_system *sys = touch->sys;

	/* Check if we need to clear the active haptic event */
	if (h->haptics_on && h->end_time_ns < now_ns) {
		h->haptics_on = false;
		h->dirty = true;
	}

	/* Check if we're trying / need to send the haptic state */
	if (!h->dirty && !h->in_progress) {
		return;
	}

	uint8_t amplitude = h->haptics_on ? h->amplitude : 0;

	if (h->dirty && h->in_progress) {
		rift_touch_cancel_in_progress(&sys->radio, touch->device_num);
	}
	h->dirty = false;

	int ret = rift_touch_send_haptics(&sys->radio, touch->device_num, h->low_freq, amplitude);
	if (ret == 0) {
		RIFT_DEBUG("Haptics sent, dev %d %s freq amplitude %u", touch->device_num,
		           h->low_freq ? "low" : "high", amplitude);
		h->in_progress = false;
		if (h->haptics_on && h->end_time_ns == UINT64_MAX) {
			h->end_time_ns = now_ns + h->duration_ns;
		}
	} else if (ret == -EINPROGRESS || ret == -EBUSY) {
		h->in_progress = true;
	} else {
		/* For any other errors, cancel any on-going transmission */
		rift_touch_cancel_in_progress(&sys->radio, touch->device_num);
		h->in_progress = false;
	}
}

static void
rift_touch_set_output(struct xrt_device *xdev, enum xrt_output_name name, const union xrt_output_value *value)
{
	struct rift_touch *touch = rift_touch(xdev);
	struct rift_system *sys = touch->sys;

	if (name != XRT_OUTPUT_NAME_TOUCH_HAPTIC) {
		RIFT_ERROR("Unknown output name requested");
		return;
	}

	float frequency = value->vibration.frequency;
	float amplitude = value->vibration.amplitude;
	int64_t duration_ns = value->vibration.duration_ns;

	os_mutex_lock(&sys->dev_mutex);

	struct rift_touch_haptics *h = &touch->haptics;

	if (amplitude > 0.0f) {
		h->haptics_on = true;
		h->dirty = true;
		/* The Touch haptic actuator supports 160Hz and 320Hz pulses.
		 * Steer unspecified (0) to low frequency, like the official
		 * runtime's default buzz. */
		h->low_freq = frequency <= 200.0f;
		h->amplitude = (uint8_t)roundf(255.0f * OHMD_CLAMP(amplitude, 0.0f, 1.0f));
		if (duration_ns <= 0) {
			h->duration_ns = RIFT_MIN_HAPTIC_DURATION_NS;
		} else {
			h->duration_ns = (uint64_t)duration_ns;
		}
		h->end_time_ns = UINT64_MAX; /* computed when actually sent */
	} else if (h->haptics_on) {
		h->haptics_on = false;
		h->dirty = true;
	}

	os_mutex_unlock(&sys->dev_mutex);
}

/*
 *
 * xrt_device members.
 *
 */

static void
set_input_bool(struct rift_touch *touch, int index, uint64_t when_ns, bool value)
{
	touch->base.inputs[index].timestamp = (int64_t)when_ns;
	touch->base.inputs[index].value.boolean = value;
}

static void
set_input_f32(struct rift_touch *touch, int index, uint64_t when_ns, float value)
{
	touch->base.inputs[index].timestamp = (int64_t)when_ns;
	touch->base.inputs[index].value.vec1.x = value;
}

static void
rift_touch_update_inputs(struct xrt_device *xdev)
{
	struct rift_touch *touch = rift_touch(xdev);
	struct rift_system *sys = touch->sys;

	os_mutex_lock(&sys->dev_mutex);

	uint64_t when_ns = touch->last_msg_local_ts_ns;
	if (when_ns == 0) {
		when_ns = os_monotonic_get_ns();
	}

	/* Buttons: bit 0 = A/X, bit 1 = B/Y, bit 2 = system/menu,
	 * bit 3 = thumbstick click. */
	if (touch->base.device_type == XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER) {
		set_input_bool(touch, OCULUS_TOUCH_X_CLICK, when_ns,
		               (touch->buttons & RIFT_TOUCH_CONTROLLER_BUTTON_X) != 0);
		set_input_bool(touch, OCULUS_TOUCH_Y_CLICK, when_ns,
		               (touch->buttons & RIFT_TOUCH_CONTROLLER_BUTTON_Y) != 0);
		set_input_bool(touch, OCULUS_TOUCH_MENU_CLICK, when_ns,
		               (touch->buttons & RIFT_TOUCH_CONTROLLER_BUTTON_MENU) != 0);
		set_input_bool(touch, OCULUS_TOUCH_X_TOUCH, when_ns, touch->cap_a_x >= RIFT_TOUCH_CAP_THRESHOLD);
		set_input_bool(touch, OCULUS_TOUCH_Y_TOUCH, when_ns, touch->cap_b_y >= RIFT_TOUCH_CAP_THRESHOLD);
	} else {
		set_input_bool(touch, OCULUS_TOUCH_A_CLICK, when_ns,
		               (touch->buttons & RIFT_TOUCH_CONTROLLER_BUTTON_A) != 0);
		set_input_bool(touch, OCULUS_TOUCH_B_CLICK, when_ns,
		               (touch->buttons & RIFT_TOUCH_CONTROLLER_BUTTON_B) != 0);
		set_input_bool(touch, OCULUS_TOUCH_SYSTEM_CLICK, when_ns,
		               (touch->buttons & RIFT_TOUCH_CONTROLLER_BUTTON_OCULUS) != 0);
		set_input_bool(touch, OCULUS_TOUCH_A_TOUCH, when_ns, touch->cap_a_x >= RIFT_TOUCH_CAP_THRESHOLD);
		set_input_bool(touch, OCULUS_TOUCH_B_TOUCH, when_ns, touch->cap_b_y >= RIFT_TOUCH_CAP_THRESHOLD);
	}

	set_input_bool(touch, OCULUS_TOUCH_THUMBSTICK_CLICK, when_ns,
	               (touch->buttons & RIFT_TOUCH_CONTROLLER_BUTTON_STICK) != 0);
	set_input_bool(touch, OCULUS_TOUCH_THUMBSTICK_TOUCH, when_ns, touch->cap_stick >= RIFT_TOUCH_CAP_THRESHOLD);
	set_input_bool(touch, OCULUS_TOUCH_TRIGGER_TOUCH, when_ns, touch->cap_trigger >= RIFT_TOUCH_CAP_THRESHOLD);
	set_input_bool(touch, OCULUS_TOUCH_THUMBREST_TOUCH, when_ns, touch->cap_rest >= RIFT_TOUCH_CAP_THRESHOLD);

	set_input_f32(touch, OCULUS_TOUCH_TRIGGER_VALUE, when_ns, touch->trigger);
	set_input_f32(touch, OCULUS_TOUCH_SQUEEZE_VALUE, when_ns, touch->grip);

	touch->base.inputs[OCULUS_TOUCH_THUMBSTICK].timestamp = (int64_t)when_ns;
	touch->base.inputs[OCULUS_TOUCH_THUMBSTICK].value.vec2.x = touch->stick[0];
	touch->base.inputs[OCULUS_TOUCH_THUMBSTICK].value.vec2.y = touch->stick[1];

	os_mutex_unlock(&sys->dev_mutex);
}

static void
rift_touch_get_tracked_pose(struct xrt_device *xdev,
                            enum xrt_input_name name,
                            uint64_t at_timestamp_ns,
                            struct xrt_space_relation *out_relation)
{
	struct rift_touch *touch = rift_touch(xdev);

	if (name != XRT_INPUT_TOUCH_AIM_POSE && name != XRT_INPUT_TOUCH_GRIP_POSE) {
		RIFT_ERROR("Unknown pose name requested");
		U_ZERO(out_relation);
		return;
	}

	struct xrt_relation_chain xrc = {0};

	/* Rotate the grip/aim pose up by 40 degrees around the X axis, to
	 * match how the controller is typically held (same correction as
	 * the rift_s driver's Touch controllers). */
	struct xrt_pose pose_correction = {0};
	struct xrt_vec3 axis = {1.0, 0, 0};
	math_quat_from_angle_vector(DEG_TO_RAD(40), &axis, &pose_correction.orientation);
	m_relation_chain_push_pose(&xrc, &pose_correction);

	struct xrt_space_relation relation;
	U_ZERO(&relation);
	m_relation_history_get(touch->rh, at_timestamp_ns, &relation);

	if ((relation.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT) == 0 &&
	    (relation.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) != 0) {
		/* 3DoF fallback: park the controller at a plausible spot in
		 * front of and below the head position. */
		bool is_left = touch->base.device_type == XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER;
		relation.pose.position.x = is_left ? -0.2f : 0.2f;
		relation.pose.position.y = debug_get_float_option_rift_eye_height() - 0.35f;
		relation.pose.position.z = -0.35f;
		relation.relation_flags |= XRT_SPACE_RELATION_POSITION_VALID_BIT;
	}

	struct xrt_space_relation *rel = m_relation_chain_reserve(&xrc);
	*rel = relation;

	m_relation_chain_resolve(&xrc, out_relation);
}

static void
rift_touch_destroy(struct xrt_device *xdev)
{
	struct rift_touch *touch = rift_touch(xdev);

	/* Detach from the system FIRST: the io thread feeds our fusion,
	 * relation history and calibration through sys->touch[] under
	 * dev_mutex, so it must not be able to reach us once we start
	 * freeing those below. */
	os_mutex_lock(&touch->sys->dev_mutex);
	if (touch->sys->touch[0] == touch) {
		touch->sys->touch[0] = NULL;
	} else if (touch->sys->touch[1] == touch) {
		touch->sys->touch[1] = NULL;
	}
	os_mutex_unlock(&touch->sys->dev_mutex);

	u_var_remove_root(touch);

	m_imu_3dof_close(&touch->fusion);
	m_relation_history_destroy(&touch->rh);

	rift_touch_clear_calibration(&touch->calibration);

	/* The system outlives us via refcounting. */
	rift_system_reference(&touch->sys, NULL);

	u_device_free(&touch->base);
}

/*
 *
 * Creation.
 *
 */

struct rift_touch *
rift_touch_create(struct rift_system *sys, enum xrt_device_type device_type)
{
	enum u_device_alloc_flags flags = (enum u_device_alloc_flags)(U_DEVICE_ALLOC_TRACKING_NONE);

	struct rift_touch *touch = U_DEVICE_ALLOCATE(struct rift_touch, flags, INPUT_INDICES_LAST, 1);
	if (touch == NULL) {
		return NULL;
	}

	/* Store a ref to the parent system, released in destroy */
	rift_system_reference(&touch->sys, sys);

	touch->base.update_inputs = rift_touch_update_inputs;
	touch->base.set_output = rift_touch_set_output;
	touch->base.get_tracked_pose = rift_touch_get_tracked_pose;
	touch->base.get_view_poses = u_device_get_view_poses;
	touch->base.destroy = rift_touch_destroy;
	touch->base.name = XRT_DEVICE_TOUCH_CONTROLLER;
	touch->base.device_type = device_type;
	touch->base.tracking_origin = &sys->base;

	touch->base.orientation_tracking_supported = true;
	touch->base.position_tracking_supported = true;

	if (device_type == XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER) {
		touch->device_num = RIFT_TOUCH_CONTROLLER_LEFT;
		touch->tracker_id = 2;
		snprintf(touch->base.str, XRT_DEVICE_NAME_LEN, "Oculus Rift CV1 Left Touch Controller");
		snprintf(touch->base.serial, XRT_DEVICE_NAME_LEN, "Left Controller");
		SET_TOUCH_INPUT(touch, X_CLICK);
		SET_TOUCH_INPUT(touch, X_TOUCH);
		SET_TOUCH_INPUT(touch, Y_CLICK);
		SET_TOUCH_INPUT(touch, Y_TOUCH);
		SET_TOUCH_INPUT(touch, MENU_CLICK);
	} else {
		touch->device_num = RIFT_TOUCH_CONTROLLER_RIGHT;
		touch->tracker_id = 1;
		snprintf(touch->base.str, XRT_DEVICE_NAME_LEN, "Oculus Rift CV1 Right Touch Controller");
		snprintf(touch->base.serial, XRT_DEVICE_NAME_LEN, "Right Controller");
		SET_TOUCH_INPUT(touch, A_CLICK);
		SET_TOUCH_INPUT(touch, A_TOUCH);
		SET_TOUCH_INPUT(touch, B_CLICK);
		SET_TOUCH_INPUT(touch, B_TOUCH);
		SET_TOUCH_INPUT(touch, SYSTEM_CLICK);
	}

	SET_TOUCH_INPUT(touch, SQUEEZE_VALUE);
	SET_TOUCH_INPUT(touch, TRIGGER_TOUCH);
	SET_TOUCH_INPUT(touch, TRIGGER_VALUE);
	SET_TOUCH_INPUT(touch, THUMBSTICK_CLICK);
	SET_TOUCH_INPUT(touch, THUMBSTICK_TOUCH);
	SET_TOUCH_INPUT(touch, THUMBSTICK);
	SET_TOUCH_INPUT(touch, THUMBREST_TOUCH);
	SET_TOUCH_INPUT(touch, GRIP_POSE);
	SET_TOUCH_INPUT(touch, AIM_POSE);

	touch->base.outputs[0].name = XRT_OUTPUT_NAME_TOUCH_HAPTIC;

	touch->base.binding_profiles = binding_profiles_rift;
	touch->base.binding_profile_count = ARRAY_SIZE(binding_profiles_rift);

	m_imu_3dof_init(&touch->fusion, M_IMU_3DOF_USE_GRAVITY_DUR_20MS);
	m_relation_history_create(&touch->rh);

	u_var_add_root(touch, touch->base.str, true);
	u_var_add_gui_header(touch, NULL, "Inputs");
	u_var_add_f32(touch, &touch->trigger, "Trigger");
	u_var_add_f32(touch, &touch->grip, "Grip");
	u_var_add_f32(touch, &touch->stick[0], "Thumbstick X");
	u_var_add_f32(touch, &touch->stick[1], "Thumbstick Y");
	u_var_add_gui_header(touch, NULL, "3DoF fusion");
	m_imu_3dof_add_vars(&touch->fusion, touch, "");

	return touch;
}
