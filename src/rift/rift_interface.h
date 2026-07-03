// Copyright 2025, monado-rift-wayland contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Interface to the Oculus Rift CV1 driver.
 * @ingroup drv_rift
 */

#pragma once

#include "xrt/xrt_prober.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @defgroup drv_rift Oculus Rift CV1 driver
 * @ingroup drv
 *
 * @brief Driver for the Oculus Rift CV1 HMD, Touch controllers and
 *        constellation tracking sensors.
 */

#define OCULUS_VR_INC_VID 0x2833
#define OCULUS_RIFT_CV1_PID 0x0031
#define OCULUS_RIFT_DK2_PID 0x0021
#define OCULUS_RIFT_DK2_V2_PID 0x2021

/* USB HID interfaces on the CV1 headset */
#define RIFT_CV1_HMD_INTERFACE 0
#define RIFT_CV1_RADIO_INTERFACE 1

/*!
 * Builder for the Oculus Rift CV1 system.
 *
 * @ingroup drv_rift
 */
struct xrt_builder *
rift_builder_create(void);

/*!
 * @dir drivers/rift
 *
 * @brief @ref drv_rift files.
 */

#ifdef __cplusplus
}
#endif
