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

## Display stays black / no direct mode

- Confirm the HDMI is connected and the compositor sees a non-desktop
  output (`wlr-randr`, `hyprctl monitors all`, or kernel log).
- Run `XRT_COMPOSITOR_LOG=debug monado-service` — you should see the
  Wayland direct backend leasing the output. See
  [wayland-compositors.md](wayland-compositors.md) for per-compositor
  notes.
- The driver sends the "enable components" command to power the panels;
  if the headset was asleep for a long time, re-plugging USB helps.

## Controllers don't show up

- Wake them (press any button); they connect over the headset's built-in
  radio, no pairing dongle needed.
- First connection reads the calibration block over the radio — this can
  take a few seconds; watch for "Touch controller connected and
  calibrated" with `RIFT_LOG=info`.
- If they were paired to another headset, pair them again using the
  official app (pairing writes are not implemented yet).

## Tracking is jittery or jumps

- Make sure the sensors can actually see the headset LEDs (IR — beware
  sunlight and reflective surfaces).
- One sensor: expect occasional occlusion recoveries; two+ placed at
  different angles is much more robust.
- Check USB bandwidth: each sensor needs a stable ~200 Mbps. Distribute
  sensors across USB controllers.
- `RIFT_LOG=debug` prints per-sensor pose lock/loss events.

## SteamVR shows "compositor not available" / no headset

- Start `monado-service` *before* SteamVR.
- Re-run `./scripts/register-steamvr-plugin.sh` after SteamVR updates
  (updates can reset the driver registration).
- Check `~/.steam/steam/logs/vrserver.txt` for `driver_monado` lines.

## Proton game doesn't detect VR

- Proton 9+ only.
- Launch options: `PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 %command%`
- For OpenVR titles you also need xrizer/OpenComposite *or* SteamVR
  running with the plugin — see
  [steamvr-and-proton.md](steamvr-and-proton.md).

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
- Room calibration is re-estimated at each start (no persistence yet).
- Controller pairing must be done with the official software once.
