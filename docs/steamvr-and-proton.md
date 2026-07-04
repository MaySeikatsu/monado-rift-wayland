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

Getting a Proton VR game (native OpenXR, e.g. Beat Saber; or OpenVR via
xrizer/OpenComposite) talking to Monado through the Steam Linux Runtime
container has two hard requirements that took real debugging to pin down:

**1. Use GE-Proton, not Valve's Proton Experimental / Proton 9-11.**

Valve's `wineopenxr` deliberately gates VR initialization on a registry
key (`HKCU\Software\Wine\VR` `state`) that *only SteamVR* sets. With any
other OpenXR runtime its `xrNegotiateLoaderRuntimeInterface` returns
error `-6` and the game fails with `XR_ERROR_RUNTIME_UNAVAILABLE` — even
though the runtime, manifest and IPC socket are all perfectly reachable
inside the container. [GE-Proton](https://github.com/GloriousEggroll/proton-ge-custom)
carries a patched `wineopenxr` without that SteamVR gate.

Set it under *Properties → Compatibility → Force GE-Proton*.

**2. Share the Monado IPC socket into the container.**

pressure-vessel imports the OpenXR runtime manifest automatically (it
reads `~/.config/openxr/1/active_runtime.json`), but it does *not* share
`$XDG_RUNTIME_DIR/monado_comp_ipc`, so the game can load the runtime but
never reach the running service. You'd normally pass
`PRESSURE_VESSEL_FILESYSTEMS_RW=$XDG_RUNTIME_DIR/monado_comp_ipc`, but
**Steam strips `PRESSURE_VESSEL_*` from env-prefix launch options**, so it
has to be set by a tiny wrapper that Steam execs. Ship this as
`~/.local/bin/monado-vr-wrap`:

```sh
#!/bin/sh
export PRESSURE_VESSEL_FILESYSTEMS_RW="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/monado_comp_ipc"
exec "$@"
```

and set the launch option to:

```
/home/you/.local/bin/monado-vr-wrap %command%
```

> Note: `PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1` is **not** needed and
> on NixOS is counterproductive — the manifest is already visible, and the
> importer's host-side probing fails on NixOS' library layout.

Verified working end-to-end: Beat Saber (native Unity OpenXR) reaches
`XR_SESSION_STATE_FOCUSED` and Monado logs `BEGIN_SESSION` on the Rift CV1
via GE-Proton + the wrapper.

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
