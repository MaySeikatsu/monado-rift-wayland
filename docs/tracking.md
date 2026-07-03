# Tracking: sensors, 6DoF and 3DoF

## TL;DR setup

- Plug the CV1 sensor cameras into USB 3 ports (USB 2 works at reduced
  frame rate on some hubs; prefer separate controllers for 2+ sensors).
- Place 1–3 sensors 1–2 m from your play space, angled to see the headset;
  two sensors in front corners give solid coverage, a third behind
  enables reliable 360° play.
- Start `monado-service`; sensors are found by USB hotplug at any time.
- 0 sensors → automatic 3DoF (orientation only) fallback.

## How the 6DoF tracking works

This driver vendors the constellation tracking stack Jan Schmidt built for
OpenHMD (`rift-kalman-filter` branch) and runs it unchanged inside Monado:

1. **LED control** — the driver configures the headset to strobe its IR
   LEDs in known 10-bit blink patterns, synchronised to the sensor
   exposures (the radio ID read from the HMD lets the cameras lock to the
   phase).
2. **Camera capture** — each Rift sensor (ESP770U-based, 1280×960 IR) is
   claimed over libusb and its frames are decoded (MJPEG → gray).
3. **Blob detection** (`rift-sensor-blobwatch.c`) — LED spots are
   extracted per frame and tracked across frames so their blink pattern
   (= LED identity) can be recovered.
4. **Correspondence search** (`correspondence_search.c`, Lambda-Twist P3P)
   — identified LEDs are matched against the device's 3D LED model (HMD
   model read from headset flash, controller models from the Touch
   calibration blobs) to produce a camera-space pose hypothesis, refined
   with OpenCV when available.
5. **Fusion** (`rift-kalman-6dof.c`, an Unscented Kalman Filter) — 1000 Hz
   headset IMU / 500 Hz controller IMU updates are fused with the camera
   observations, handling exposure latency, per-device time domains and
   bias estimation.

The Monado driver feeds each fused pose into a relation history, so
`get_tracked_pose` serves properly interpolated/extrapolated poses for any
timestamp the compositor or app asks about.

Sensor poses are estimated automatically at startup by observing the
headset; there is no manual room calibration step. (Persisting refined
room calibration across runs is a planned improvement — upstream OpenHMD
has an experimental `rift-room-config` branch for this.)

## 3DoF fallback

Without cameras (or with `RIFT_DISABLE_TRACKER=1`):

- The HMD uses gravity-aligned IMU orientation fusion at a fixed eye
  height (`RIFT_EYE_HEIGHT`, default 1.6 m).
- Controllers are orientation-only, parked at plausible positions in
  front of the user.

This mode is good for seated/media use and for verifying the setup.

## Performance notes

- Long-range/initial pose search runs on a separate thread; steady-state
  tracking is cheap (blob tracking + UKF).
- OpenCV (`libopencv-dev`) enables RANSAC PnP pose refinement — install it
  for best accuracy. The build works without it, with reduced long-range
  search quality.
- Each active sensor costs USB bandwidth (~54 fps × 1280×960). If a sensor
  fails to start, try another USB controller.

## Debugging

```sh
RIFT_LOG=debug monado-service          # driver + tracker logging
RIFT_LOG=trace monado-service          # very verbose, per-packet
```

Watch for these lines at startup:

- `LED model: HMD LEDs` — LED model read from the headset (~40 entries).
- `Found Rift Sensor ...` — each camera claimed and started.
- `Sensor ... TIMESYNC` — cameras locked to the LED blink phase.
- `... Touch controller connected and calibrated` — controller flash
  calibration (JSON) fetched over the radio; cached under
  `~/.cache/monado-rift/` so later startups are instant.
