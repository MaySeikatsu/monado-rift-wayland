# Vendored OpenHMD Rift tracking core

The code in this directory (except the shim files listed below) is
vendored from Jan Schmidt's OpenHMD fork:

- Repository: https://github.com/thaytan/OpenHMD
- Branch: `rift-kalman-filter`
- Commit: see `VENDORED_COMMIT`
- License: BSL-1.0 (same as Monado and this repository)

## Shim files (ours, not vendored)

- `openhmdi.h` — replacement for OpenHMD's internal header, mapping
  logging, allocation, time and the config cache onto Monado utilities.
- `platform.h` / `ohmd_shim.c` — mutex/cond/thread/time shims (pthreads).

## Local modifications to vendored files

Kept minimal on purpose, to make re-syncing with upstream easy:

1. `drv_oculus_rift/rift-hmd-radio.c`, `rift-hmd-radio.h` — hidapi
   replaced with Monado `os_hid` (identical feature-report semantics).
2. `drv_oculus_rift/rift-tracker.c`, `rift-tracker.h` — added
   `rift_tracker_get_sensor_count()` (marked with a
   "monado-rift-wayland addition" comment).
3. `drv_oculus_rift/rift-leds.c` — new file containing the three
   `rift_leds_*` helpers extracted from upstream `rift.c` (upstream
   `rift.c` itself is *not* vendored; the Monado driver in `../`
   replaces OpenHMD's device layer).
4. `drv_oculus_rift/rift-sensor-pose-helper.c` - one noisy `printf`
   during pose search converted to `LOGV` (marked with a comment).
5. `drv_oculus_rift/rift-debug-draw.c` is present for reference but not
   compiled (depends on OpenHMD's pipewire debug streams; its header
   stubs all call sites when HAVE_PIPEWIRE is unset).

## Re-syncing with upstream

```sh
git clone -b rift-kalman-filter https://github.com/thaytan/OpenHMD /tmp/ohmd
# compare/update drv_oculus_rift/ + the support files at this level
# (omath, matrices, ukf, unscented, exponential-filter, ext_deps/nxjson)
# re-apply the modifications listed above, update VENDORED_COMMIT
```
