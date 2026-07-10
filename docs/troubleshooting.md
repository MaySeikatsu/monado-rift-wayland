# Troubleshooting

## The headset isn't found

```sh
lsusb | grep 2833
```

- `2833:0031` should be listed (CV1). Sensors are `2833:0211`.
- No device: check the USB cable (the CV1's combined cable is notoriously
  finicky), try another port.
- Device present but `monado-cli probe` doesn't select the `rift` builder:
  permissions. Install the udev rules and re-plug:

  ```sh
  sudo ./scripts/install-udev-rules.sh
  ```

## "Could not find the requested hid interface (0) on the device!"

Another VR runtime is (or was) holding the headset's USB interfaces over
libusb, which detaches the kernel HID driver — so no `/dev/hidraw*` nodes
exist for the headset and this driver can't open it. Typical culprits:

- **A stock Monado instance** (e.g. NixOS `services.monado` with the
  default nixpkgs package): its OpenHMD wrapper driver opens the CV1 via
  hidapi-libusb. Stop it (`systemctl --user stop monado.service
  monado.socket`) and make sure only one Monado is installed/enabled —
  on NixOS point `services.monado.package` at this flake's package.
- OpenHMD-based software, SteamVR, or a crashed process that never
  reattached the kernel driver.

You can confirm with:

```sh
for i in /sys/bus/usb/devices/*:*; do
  v=$(cat $(dirname $i)/$(basename $i | cut -d: -f1)/idVendor 2>/dev/null)
  [ "$v" = 2833 ] && echo "$i -> $(basename $(readlink $i/driver 2>/dev/null))"
done
# "usbfs" = claimed by a running process; empty = left detached
```

The driver now self-heals the "left detached" case: it reattaches the
kernel driver and re-probes automatically at startup. If the interfaces
are actively claimed (`usbfs`) you must stop the owning process first;
worst case, re-plug the headset's USB cable.

## Display stays black / no direct mode

- Confirm the HDMI is connected and the compositor sees a non-desktop
  output (`wlr-randr`, `hyprctl monitors all`, or kernel log).
- Run `XRT_COMPOSITOR_LOG=debug monado-service` — you should see the
  Wayland direct backend leasing the output. See
  [wayland-compositors.md](wayland-compositors.md) for per-compositor
  notes.
- The driver sends the "enable components" command to power the panels;
  if the headset was asleep for a long time, re-plugging USB helps.

## Controllers don't show up / don't move

- Wake them (press any button); they connect over the headset's built-in
  radio, no pairing dongle needed.
- First connection reads the calibration block over the radio — this can
  take a few seconds; watch for "Touch controller connected and
  calibrated" with `RIFT_LOG=info` (the default level).
- **Controllers appear in-game but sit at a fixed "parked" pose** (or, in
  some games, vibrate constantly at that spot): they haven't gotten a
  camera position lock yet, so the driver reports orientation-only with a
  placeholder position. Move them inside the sensors' view. The log tells
  you which stage is failing:
  - no "radio link up" line at all → the radio receive path — check the
    controller batteries, wake them, re-plug the headset USB;
  - repeating calibration-read warnings → the flash read over radio keeps
    failing — re-plug and let them reconnect;
  - calibrated but never position-locked → the cameras can't match the
    controller LEDs — check sensor placement/IR interference (see
    "Tracking is jittery").
- If they were paired to another headset, pair them again using the
  official app (pairing writes are not implemented yet).

## World faces the wrong way / you spawn off-center

The tracker's world frame is anchored when tracking first locks, which
rarely matches the direction you want to play in. Recenter at runtime:

- **hold the right Touch controller's Oculus button ~1 s** — a short buzz
  confirms; the direction you face becomes forward, your position the
  origin (floor height is kept), or
- `touch $XDG_RUNTIME_DIR/monado-rift-recenter` from a terminal, or
- `monado-ctl -c` (recenters the LOCAL space only; the file/button
  recenter moves the whole world including STAGE).

## Game shows a black screen but no error

A previous VR game's process is still connected to Monado and holds the
primary session — the new game's session never gets displayed. Check for
leftover game processes (`ps -eo args | grep -iE "hlvr|vrchat|beat"`),
kill them or restart `monado-service`. Run one VR game at a time.

## Tracking is jittery or jumps

- Make sure the sensors can actually see the headset LEDs (IR — beware
  sunlight and reflective surfaces).
- One sensor: expect occasional occlusion recoveries; two+ placed at
  different angles is much more robust.
- Check USB bandwidth: each sensor needs a stable ~200 Mbps. Distribute
  sensors across USB controllers.
- `RIFT_LOG=debug` prints per-sensor pose lock/loss events.

## SteamVR shows "compositor not available" / no headset

Expected on most Wayland setups: SteamVR's `vrcompositor` needs X11 (or
XWayland DRM-leasing) direct mode, and Monado already holds the CV1's
display lease. **Don't run SteamVR — use xrizer for OpenVR games instead**
(see [steamvr-and-proton.md](steamvr-and-proton.md)). If you're on a real
X11 session and want to experiment with the untested plugin path:

- Start `monado-service` *before* SteamVR.
- Re-run `./scripts/register-steamvr-plugin.sh` after SteamVR updates
  (updates can reset the driver registration).
- Check `~/.steam/steam/logs/vrserver.txt` for `driver_monado` lines.

## Proton game doesn't detect VR

- **Wrong Proton version** is the most common cause: OpenXR games need
  GE-Proton (Valve's builds gate VR on SteamVR), Source 2 OpenVR games
  (Alyx) need Valve Proton 8.0. Full rationale and a decision tree in
  [steamvr-and-proton.md](steamvr-and-proton.md).
- Launch option must be the wrapper: `~/.local/bin/monado-vr-wrap %command%`
  (shares the Monado IPC socket into Steam's container).
- OpenVR titles additionally need xrizer registered in a **read-only**
  `~/.config/openvr/openvrpaths.vrpath` — Steam rewrites that file on
  every start if it's writable.

## Where are the logs / knobs?

- `RIFT_LOG=trace|debug|info|warn|error` — this driver.
- `XRT_COMPOSITOR_LOG=debug` — display/compositor path.
- `XRT_LOG=debug` — everything else in Monado.
- Monado's debug UI (`XRT_DEBUG_GUI=1` with SDL2 installed) exposes the
  driver's live pose, sensor count, input values and IMU fusion state.

## Known limitations

- Proximity sensor isn't wired up (protocol not documented in the sources
  this driver builds on).
- The Oculus Remote's buttons are decoded but not yet exposed as an input
  device.
- Room calibration is re-estimated at each start (no persistence yet) —
  recenter after putting the headset on (right Oculus button, ~1 s hold).
- Controller pairing must be done with the official software once.
