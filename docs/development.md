# Development notes

## Architecture

The driver lives in `src/rift/` and is copied verbatim into
`monado/src/xrt/drivers/rift/` by `build.sh` (the builder file moves to
`src/xrt/targets/common/target_builder_rift.c`). A ~20-line patch
(`patches/0001-monado-add-rift-cv1-driver.patch`) wires it into Monado's
CMake and builder list; everything else is additive.

```
target_builder_rift.c    xrt_builder: probes 2833:0031, opens HID ifaces 0+1
rift_system.c            system container; io thread (IMU, radio, keepalive,
                         haptics flush); headset bring-up; tracker lifecycle
rift_hmd.c               xrt_device HMD: optics/distortion/FOV, pose serving
rift_touch.c             xrt_device Touch controllers: full input profile,
                         haptics, flash calibration, tracker registration
rift_driver.h            shared structs (rift_system / rift_hmd / rift_touch)
drv_rift.cmake           the CMake target, included from drivers/CMakeLists

ohmd/                    OpenHMD compatibility layer + vendored tracking core
ohmd/openhmdi.h          shim: logging→u_logging, alloc, time, config cache
ohmd/platform.h          shim: mutex/cond/thread decls (pthreads impl)
ohmd/ohmd_shim.c         shim implementation
ohmd/drv_oculus_rift/    vendored from thaytan/OpenHMD rift-kalman-filter
```

### Vendored code provenance

- Source: https://github.com/thaytan/OpenHMD branch `rift-kalman-filter`,
  commit recorded in `src/rift/ohmd/VENDORED_COMMIT`.
- License: BSL-1.0, same as Monado and this repo.
- Local modifications (kept deliberately minimal so future re-syncs stay
  easy):
  1. `rift-hmd-radio.[ch]`: hidapi calls replaced with Monado `os_hid`
     (same feature-report semantics).
  2. `rift-tracker.[ch]`: added `rift_tracker_get_sensor_count()`.
  3. `rift-leds.c`: new file with the three `rift_leds_*` helpers
     extracted from OpenHMD's `rift.c` (which is not vendored — the
     Monado driver replaces OpenHMD's device layer).
  4. `rift-debug-draw.c` is present but not compiled (it needs the
     OpenHMD pipewire debug streams; the header stubs the calls out).
- Everything else compiles unmodified against the shim.

## Monado version pinning

The driver is compile-verified against the Monado revision pinned in
`build.sh` (`MONADO_REF`, main from 2024-01, plus the GitHub mirror
fallback). Monado's `xrt_device` API moves over time — notably
`update_inputs` gained a return value later in 2024 — so forward-porting
to current `main` is expected to need small signature updates in
`rift_hmd.c` / `rift_touch.c` and possibly builder API tweaks. Override
with `MONADO_REF=... MONADO_URL=... ./build.sh` when attempting it.

## Building for hacking

```sh
./build.sh                          # one-shot superbuild
cd build/monado/build
ninja                               # incremental rebuilds
./src/xrt/targets/cli/monado-cli probe
RIFT_LOG=debug ./src/xrt/targets/service/monado-service
```

After editing files in `src/rift/`, re-run `./build.sh` (it re-copies the
sources) or edit the copies inside `build/monado/src/xrt/drivers/rift/`
directly and copy back when done.

## Testing without hardware

- `monado-cli probe` exercises the builder estimate path.
- `RIFT_DISABLE_TRACKER=1` + the simulated driver are useful for pure
  runtime work.
- The vendored tracker has an offline recording simulator in upstream
  OpenHMD (`rift-recording-simulator` branch) that can replay captured
  sensor streams — porting that harness is on the wishlist.

## Wishlist / roadmap

- Forward-port to current Monado `main` and start the conversation about
  upstreaming (Jan Schmidt planned a Monado port of this tracker; this
  integration should be useful groundwork).
- Room/sensor pose persistence between runs.
- Expose the Oculus Remote as an input device.
- Proximity sensor (needs protocol discovery).
- Touch controller pairing without the official software.
- Hook the tracker's debug video/overlay into Monado's debug GUI
  (upstream used pipewire streams).
- DK2 validation (code paths exist, untested).
