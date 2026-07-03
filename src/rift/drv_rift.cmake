# Copyright 2025, monado-rift-wayland contributors.
# SPDX-License-Identifier: BSL-1.0
#
# Defines the drv_rift static library. Included from
# src/xrt/drivers/CMakeLists.txt (see patches/) with
# CMAKE_CURRENT_SOURCE_DIR == src/xrt/drivers.

add_library(
	drv_rift STATIC
	# Monado driver glue
	rift/rift_interface.h
	rift/rift_driver.h
	rift/rift_system.c
	rift/rift_hmd.c
	rift/rift_touch.c
	# OpenHMD compatibility shim
	rift/ohmd/openhmdi.h
	rift/ohmd/platform.h
	rift/ohmd/ohmd_shim.c
	# Vendored OpenHMD support code (see rift/ohmd/VENDORED_COMMIT)
	rift/ohmd/omath.c
	rift/ohmd/matrices.c
	rift/ohmd/ukf.c
	rift/ohmd/unscented.c
	rift/ohmd/exponential-filter.c
	rift/ohmd/ext_deps/nxjson.c
	# Vendored Rift protocol + constellation tracking core
	rift/ohmd/drv_oculus_rift/packet.c
	rift/ohmd/drv_oculus_rift/rift-leds.c
	rift/ohmd/drv_oculus_rift/rift-hmd-radio.c
	rift/ohmd/drv_oculus_rift/rift-tracker.c
	rift/ohmd/drv_oculus_rift/rift-kalman-6dof.c
	rift/ohmd/drv_oculus_rift/rift-sensor.c
	rift/ohmd/drv_oculus_rift/rift-sensor-usb.c
	rift/ohmd/drv_oculus_rift/rift-sensor-blobwatch.c
	rift/ohmd/drv_oculus_rift/rift-sensor-pose-helper.c
	rift/ohmd/drv_oculus_rift/rift-sensor-pose-search.c
	rift/ohmd/drv_oculus_rift/rift-sensor-maths.c
	rift/ohmd/drv_oculus_rift/rift-sensor-flicker.c
	rift/ohmd/drv_oculus_rift/correspondence_search.c
	rift/ohmd/drv_oculus_rift/led_search.c
	rift/ohmd/drv_oculus_rift/ohmd-video.c
	rift/ohmd/drv_oculus_rift/ohmd-jpeg.c
	rift/ohmd/drv_oculus_rift/sensor/uvc.c
	rift/ohmd/drv_oculus_rift/sensor/esp770u.c
	rift/ohmd/drv_oculus_rift/sensor/esp570.c
	rift/ohmd/drv_oculus_rift/sensor/ar0134.c
	rift/ohmd/drv_oculus_rift/sensor/mt9v034.c
	)

target_link_libraries(
	drv_rift
	PRIVATE
		xrt-interfaces
		aux_util
		aux_math
		aux_os
		Threads::Threads
		${LIBUSB1_LIBRARIES}
		${JPEG_LIBRARIES}
	)

target_include_directories(
	drv_rift SYSTEM PRIVATE ${LIBUSB1_INCLUDE_DIRS} ${JPEG_INCLUDE_DIRS}
	)
target_include_directories(drv_rift PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/rift)

if(XRT_HAVE_OPENCV)
	target_sources(drv_rift PRIVATE rift/ohmd/drv_oculus_rift/rift-sensor-opencv.cpp)
	target_include_directories(drv_rift SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
	target_link_libraries(drv_rift PRIVATE ${OpenCV_LIBRARIES})
	target_compile_definitions(drv_rift PRIVATE HAVE_OPENCV=1)
else()
	target_compile_definitions(drv_rift PRIVATE HAVE_OPENCV=0)
endif()

list(APPEND ENABLED_HEADSET_DRIVERS rift)
