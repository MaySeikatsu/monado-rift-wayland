# SteamVR and Proton games with the Rift CV1

There are two complementary ways to run Steam VR titles, and you can have
both set up at the same time:

1. **SteamVR with the Monado plugin** — real SteamVR runs, but gets the
   headset, tracking and Touch controllers from Monado. Best compatibility
   with games that hard-require SteamVR.
2. **OpenComposite / xrizer (no SteamVR at all)** — OpenVR games talk to a
   translation layer that maps OpenVR onto OpenXR/Monado directly. Lower
   overhead, fewer moving parts, but a few titles misbehave.

Native OpenXR games need no translation at all — just make Monado the
active runtime (`./scripts/set-openxr-runtime.sh`).

## Option 1: SteamVR with the Monado driver plugin

The build produces SteamVR driver plugin (`driver_monado`). Register it:

```sh
./scripts/register-steamvr-plugin.sh
```

Then:

1. Start `monado-service` (or the systemd unit) *before* SteamVR.
2. Launch SteamVR. It should show the CV1 as a "Monado" headset, with both
   Touch controllers bound to the Oculus Touch interaction profile.
3. If SteamVR's built-in device handling fights over the hardware, disable
   unneeded add-ons under *SteamVR settings → Startup/Shutdown → Manage
   Add-Ons*.

Proton/Windows games launched from SteamVR work as usual — SteamVR is the
OpenVR runtime, Monado is invisible to the game.

## Option 2: OpenComposite or xrizer (OpenVR → OpenXR)

[xrizer](https://github.com/Supreeeme/xrizer) is the actively developed
reimplementation of OpenVR on top of OpenXR;
[OpenComposite](https://gitlab.com/znixian/OpenOVR) is its predecessor and
still works better for a handful of titles.

```sh
# xrizer
cargo build --release   # in the xrizer checkout
# point Steam's OpenVR at it via ~/.config/openvr/openvrpaths.vrpath
# (see the xrizer README for the exact runtime entry)
```

### Proton (Windows) titles

For Windows VR titles running under Proton you need:

- **Proton 9 or newer** (OpenXR support in pressure-vessel).
- The launch option below, because the Steam Linux Runtime container hides
  the host's OpenXR runtime by default:

```
PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 %command%
```

Set it per game under *Properties → Launch Options*.

With xrizer/OpenComposite handling OpenVR titles and Monado exposing the
full Oculus Touch profile, both native Linux VR games and
Proton-compatible Windows VR games see a normal "Oculus Touch" setup:
thumbsticks, A/B/X/Y with capacitive touch, triggers, grips and haptics.

## Which one should I use?

| | SteamVR + plugin | OpenComposite / xrizer |
| --- | --- | --- |
| Game compatibility | Highest | Good and improving |
| Overhead / latency | Higher (extra compositor hop) | Lower |
| SteamVR overlay/dashboard | Yes | No |
| Setup complexity | Medium | Medium |

Start with xrizer for the games you play; fall back to SteamVR-with-plugin
for anything that misbehaves.
