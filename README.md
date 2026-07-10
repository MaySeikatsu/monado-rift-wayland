# monado-rift-wayland

**A native [Monado](https://monado.freedesktop.org) driver for the Oculus Rift CV1 on Linux/Wayland — full 6DoF constellation tracking, Touch controllers with haptics, and Steam/Proton games via OpenXR + xrizer (no SteamVR needed).**

This project packages a complete, self-contained OpenXR stack for the original
consumer Oculus Rift (CV1):

- **`drv_rift`** — a new native Monado driver (this repo, `src/rift/`) for the
  CV1 HMD, the Touch controllers and the Oculus constellation tracking
  sensors (the USB cameras).
- **Positional tracking** — the proven LED-constellation + Kalman-filter
  tracking core from [Jan Schmidt's OpenHMD work](https://github.com/thaytan/OpenHMD/tree/rift-kalman-filter)
  (`rift-kalman-filter` branch), vendored and adapted to run inside Monado.
  Both projects are Boost-1.0 licensed.
- **Superbuild** — `build.sh` fetches Monado at a pinned, compile-verified
  revision, drops the driver in, applies a ~20-line integration patch and
  builds `monado-service`, `monado-cli` and the SteamVR driver plugin.
- **Setup tooling** — udev rules, OpenXR runtime activation, SteamVR plugin
  registration, a systemd user service and documentation for Hyprland, Niri
  and other Wayland compositors.

## Feature matrix

| Feature | Status |
| --- | --- |
| CV1 HMD display bring-up (screen on, 90 Hz, distortion + chromatic aberration correction) | ✅ implemented |
| Rotational tracking (1000 Hz IMU, gravity-aligned fusion) | ✅ implemented |
| **Positional (6DoF) tracking** via constellation sensors, 1–3 cameras, hotplug | ✅ implemented (vendored OpenHMD core) |
| Touch controllers: all buttons, thumbsticks, triggers, grips | ✅ implemented |
| Touch capacitive touch sensing (A/B/X/Y, trigger, thumbstick, thumbrest) | ✅ implemented |
| Touch haptics (160/320 Hz vibration) | ✅ implemented |
| Touch 6DoF tracking (LED models read from controller flash calibration) | 🔶 implemented, position-lock verification on real hardware in progress |
| Playspace recenter at runtime (hold right Touch **Oculus button** ~1 s, or `touch $XDG_RUNTIME_DIR/monado-rift-recenter`) | ✅ implemented |
| Oculus Remote buttons | 🔶 decoded, not yet exposed as an input device |
| DK2 (3DoF + camera) | 🔶 experimental, behind `RIFT_ENABLE_DK2=1` |
| Wayland direct mode (DRM lease) on Hyprland, Niri, KDE, wlroots | ✅ via Monado's `wayland-direct` backend |
| Steam VR titles, native + Proton/Windows (Beat Saber, Half-Life Alyx, VRChat tested) | ✅ OpenXR games via GE-Proton, OpenVR games via xrizer — see [docs/steamvr-and-proton.md](docs/steamvr-and-proton.md) |
| SteamVR (Valve's runtime) with the Monado driver plugin | 🔶 plugin is built and shipped, but **untested** — vrcompositor needs X11/DRM-lease direct mode that most Wayland setups can't provide; xrizer replaces it |
| Headset audio/mic (USB audio class) | ✅ handled by the kernel/PipeWire, nothing to do |
| Proximity sensor | ❌ not in the known protocol documentation yet |

> **Hardware testing status:** verified on real hardware on one reference
> machine (CV1 + 2 sensors; NixOS, niri + xwayland-satellite, NVIDIA
> GTX 1060): display bring-up at 90 Hz, HMD 6DoF constellation tracking,
> controller radio + haptics, and Steam/Proton games (Beat Saber,
> Half-Life Alyx, VRChat) running against the driver. Touch controller
> 6DoF position lock is still being verified — see
> [docs/troubleshooting.md](docs/troubleshooting.md). Test reports
> (`RIFT_LOG=info`) are very welcome — please open issues!

## Quick start

```sh
git clone https://github.com/MaySeikatsu/monado-rift-wayland.git
cd monado-rift-wayland

# 1. Build (checks and lists any missing dependencies first)
./build.sh

# 2. Install runtime + tools (default PREFIX=/usr/local)
./build.sh --install

# 3. Device permissions (HMD, radio, tracking cameras)
sudo ./scripts/install-udev-rules.sh     # then re-plug the headset

# 4. Make Monado your active OpenXR runtime
./scripts/set-openxr-runtime.sh

# 5. Run it (from a Wayland session on Hyprland/Niri/KDE/...)
monado-service
```

Connect the CV1's HDMI and USB, plug in at least one sensor camera for 6DoF
(0 cameras = 3DoF orientation-only mode), and start an OpenXR app.

Once in VR, **hold the right Touch controller's Oculus button for ~1 second**
(a short buzz confirms) to recenter: the direction you're facing becomes
forward and where you stand becomes the origin. Without controllers:
`touch $XDG_RUNTIME_DIR/monado-rift-recenter`.

### Nix / NixOS / home-manager

The repo is also a flake with a package, NixOS + home-manager modules and
a dev shell:

```sh
nix run github:MaySeikatsu/monado-rift-wayland#monado-cli -- probe
nix develop     # hacking environment
```

```nix
# NixOS: udev rules + runtime
imports = [ monado-rift-wayland.nixosModules.default ];
hardware.oculus-rift-cv1.enable = true;

# home-manager: active runtime + user service
imports = [ monado-rift-wayland.homeManagerModules.default ];
programs.monado-rift.enable = true;
```

See [docs/nix.md](docs/nix.md) for the full guide.

For **Steam VR games** (native and Proton) — tested titles, recommended
Proton versions, per-game recipes — see
[docs/steamvr-and-proton.md](docs/steamvr-and-proton.md).
For compositor specifics (Hyprland, Niri), see
[docs/wayland-compositors.md](docs/wayland-compositors.md).

## How it fits together

```
OpenXR app ──► monado-service
                 ├─ drv_rift (this repo)
                 │    ├─ HID io thread: IMU @1kHz, radio @500Hz, keepalive, haptics
                 │    ├─ rift_hmd / rift_touch xrt_devices (full Touch profile)
                 │    └─ vendored OpenHMD tracking core
                 │         ├─ libusb sensor manager (hotplug, ESP770U/uvc cameras)
                 │         ├─ blob detection ► correspondence search ► P3P pose
                 │         └─ UKF (Kalman) fusion: IMU + camera observations
                 └─ compositor: Wayland DRM-lease direct mode @ 90Hz

OpenXR game (native or GE-Proton) ─────────────────────────► monado-service
OpenVR game (native or Proton) ──► xrizer (OpenVR→OpenXR) ──► monado-service
```

The driver registers as a Monado *builder* (`rift`), probing USB VID
`2833` PID `0031`. HID interface 0 is the headset (IMU, feature reports,
LED patterns); interface 1 is the wireless radio link to the Touch
controllers. Camera sensors are claimed directly over libusb and
synchronised to the headset's LED blink phase via the radio ID.

## Runtime options

| Environment variable | Default | Meaning |
| --- | --- | --- |
| `RIFT_LOG` | `info` | Driver + tracker log level (`trace`/`debug`/`info`/`warn`/`error`) |
| `RIFT_DISABLE_TRACKER` | `false` | Force 3DoF, don't touch the cameras |
| `RIFT_EYE_HEIGHT` | `1.6` | Eye height (m) used for the fixed position in 3DoF mode |
| `RIFT_ENABLE_DK2` | `false` | Also probe for a DK2 (experimental) |
| `XRT_COMPOSITOR_FORCE_WAYLAND_DIRECT` | `false` | Force the Wayland DRM-lease direct-mode backend (recommended on Wayland + NVIDIA) |
| `XRT_TRACKING_ORIGIN_OFFSET_Y` | `0` | Monado core: lift the tracked world (meters) — floor-height calibration |
| `XRT_COMPOSITOR_LOG` | `info` | Compositor logging, useful for display debugging |

## Repository layout

```
src/rift/                 The Monado driver (copied into monado/src/xrt/drivers/rift)
src/rift/ohmd/            OpenHMD shim + vendored tracking core (see VENDORED_COMMIT)
src/rift/target_builder_rift.c   Builder (goes to monado/src/xrt/targets/common)
patches/                  ~20-line Monado integration patch (CMake + builder list)
scripts/, udev/, systemd/ Setup tooling
docs/                     SteamVR/Proton, Wayland, tracking, troubleshooting, development
build.sh                  Superbuild (fetch Monado, integrate, build)
```

## Documentation

- [docs/nix.md](docs/nix.md) — Nix flake, NixOS + home-manager modules, dev shell
- [docs/steamvr-and-proton.md](docs/steamvr-and-proton.md) — Steam & Proton games: xrizer, recommended Proton versions, tested titles, troubleshooting
- [docs/wayland-compositors.md](docs/wayland-compositors.md) — Hyprland, Niri, DRM lease, fallbacks
- [docs/tracking.md](docs/tracking.md) — sensor setup, 6DoF vs 3DoF, tracking internals
- [docs/troubleshooting.md](docs/troubleshooting.md) — permissions, display, controllers, orientation/recenter, Steam games
- [docs/development.md](docs/development.md) — architecture, vendored code provenance, forward-porting

## Credits & license

- **Jan Schmidt** ([@thaytan](https://github.com/thaytan)) — the CV1
  constellation tracking, Kalman filter and radio protocol work this driver
  stands on, plus the Monado `rift_s` driver used as the structural template.
- **Philipp Zabel, Fredrik Hultin, Jakob Bornecrantz** and OpenHMD
  contributors — Rift protocol reverse engineering.
- **Collabora and the Monado community** — the OpenXR runtime itself.

Everything here, like Monado and OpenHMD, is licensed
[BSL-1.0](https://opensource.org/licenses/BSL-1.0). See `LICENSE`.
Vendored code provenance is pinned in `src/rift/ohmd/VENDORED_COMMIT`.
