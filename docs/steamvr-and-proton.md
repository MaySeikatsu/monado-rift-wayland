# Steam and Proton VR games with the Rift CV1

The tested, recommended way to run Steam VR titles on this driver does
**not** involve SteamVR at all:

```
OpenXR games (e.g. Beat Saber):    game ──► wineopenxr (GE-Proton) ──► Monado
OpenVR games (e.g. Alyx, VRChat):  game ──► openvr_api.dll ──► Proton vrclient bridge
                                     ──► host openvrpaths.vrpath ──► xrizer ──► OpenXR ──► Monado
```

[xrizer](https://github.com/Supreeeme/xrizer) is a reimplementation of the
OpenVR runtime on top of OpenXR. Both native Linux OpenVR games and
Windows-via-Proton OpenVR games resolve their runtime through the host's
`~/.config/openvr/openvrpaths.vrpath` (Proton's vrclient bridge forwards to
it), so with xrizer registered there they talk straight to Monado. Games see
a normal "Oculus Touch" setup: thumbsticks, A/B/X/Y with capacitive touch,
triggers, grips and haptics.

## TL;DR — the tested recipe

1. **Monado running** (socket-activated systemd unit, or `monado-service`).
2. **xrizer registered** as the only OpenVR runtime (see
   [Registering xrizer](#registering-xrizer) — the file must be read-only!).
3. **Every VR game** gets the launch option
   `/home/you/.local/bin/monado-vr-wrap %command%`
   (wrapper shipped as `scripts/monado-vr-wrap.sh`, installed by the
   home-manager module).
4. **Proton version**: GE-Proton for everything — except Source 2 games
   (Half-Life Alyx), which need **Valve Proton 8.0**. Details below.
5. One VR game at a time.

## Tested games (real CV1 hardware, 2026-07)

| Game | App ID | VR API | Proton | Notes |
| --- | --- | --- | --- | --- |
| Beat Saber | 620980 | OpenXR (Unity) | GE-Proton10-33 | Renders in-headset; the reference OpenXR title |
| Half-Life Alyx | 546560 | OpenVR (Source 2) | **Valve Proton 8.0** | Proton 9/10 crash at map load (IVRMailbox, see below) |
| VRChat | 438100 | OpenVR (Unity) | GE-Proton10-33 | Anti-cheat needs a healthy Wine prefix (see troubleshooting) |

All three launch, connect to headset + sensors + controllers and reach
`XR_SESSION_STATE_FOCUSED`. Touch-controller 6DoF position lock is still
being verified on real hardware — see
[troubleshooting.md](troubleshooting.md#controllers-dont-show-up--dont-move).

## Choosing a Proton version (and why not just "latest")

Two independent, deliberate incompatibilities in Valve's Proton force the
version choice:

1. **Valve Proton gates OpenXR on SteamVR.** Its `wineopenxr` refuses to
   initialize unless a registry key (`HKCU\Software\Wine\VR` `state`) that
   *only SteamVR* sets is present; with any other runtime it returns error
   `-6` and the game reports `XR_ERROR_RUNTIME_UNAVAILABLE`.
   [GE-Proton](https://github.com/GloriousEggroll/proton-ge-custom) removes
   that gate → **all OpenXR games need GE-Proton**.
2. **Proton 9+ crashes Source 2 games on non-SteamVR runtimes.** Source 2
   requests the undocumented `IVRMailbox` OpenVR interface; Proton 9/10's
   rewritten vrclient thunk exec-faults on it (Proton log signature:
   `_wassert` ... `winIVRMailbox.c:51`, crash in `vrclient_x64.so`) with any
   runtime that isn't real SteamVR. Proton 8's older winelib vrclient
   handles it → **Source 2 games (Alyx) need Valve Proton 8.0**.
   Unity/Unreal OpenVR games never touch IVRMailbox, so GE is fine there.

Decision tree for a game not listed above:

1. Game uses OpenXR → **GE-Proton**, done.
2. Game uses OpenVR and is Unity/Unreal (most of them) → **GE-Proton** +
   xrizer.
3. Game uses OpenVR and is Source 2 / a Valve title → **Valve Proton 8.0** +
   xrizer.
4. Not sure? Start with GE-Proton. If it crashes at load with the
   `winIVRMailbox.c:51` signature in `PROTON_LOG=1` output → switch to
   Proton 8.0. If it reports the headset missing → check the
   [xrizer registration](#registering-xrizer).

Set the version per game under *Properties → Compatibility*.

## Registering xrizer

With the home-manager module this is `programs.monado-rift.openvr.enable`
(default on) — it writes `~/.config/openvr/openvrpaths.vrpath` as a
read-only store symlink. Manually:

```json
{
  "config": ["/home/you/.local/share/Steam/config"],
  "external_drivers": null,
  "jsonid": "vrpathreg",
  "log": ["/home/you/.local/share/Steam/logs"],
  "runtime": ["/path/to/xrizer/lib/xrizer"],
  "version": 1
}
```

then **make it read-only** (`chmod 444`): Steam rewrites this file on every
start, re-inserting SteamVR as the first runtime, which silently breaks the
whole chain. This was the single most confusing failure during bring-up —
games just say "headset not found" while everything else is fine.

## The launch wrapper (required for every Proton VR game)

pressure-vessel (Steam's container) imports the OpenXR runtime manifest
automatically, but does **not** share the Monado IPC socket, and Steam
strips `PRESSURE_VESSEL_*` variables from env-prefix launch options — so a
wrapper script must set it. The repo ships it
(`scripts/monado-vr-wrap.sh`, installed to `~/.local/bin/monado-vr-wrap` by
the home-manager module):

- exports `PRESSURE_VESSEL_FILESYSTEMS_RW=$XDG_RUNTIME_DIR/monado_comp_ipc`
- unsets `SDL_VIDEODRIVER` (a session-wide `wayland` value leaks into
  wine's Windows SDL, which has no wayland/x11 backends, and breaks window
  and D3D11 creation — Proton 8 is affected)

Launch option for every VR game:

```
/home/you/.local/bin/monado-vr-wrap %command%
```

> `PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1` is **not** needed and on
> NixOS is counterproductive — the manifest is already visible, and the
> importer's host-side probing fails on NixOS' library layout.

## Troubleshooting Steam games

- **Black screen in-game, no crash** — a previous VR game's process is
  still connected and holds Monado's primary session (the new game never
  leaves `READY`). Check for leftovers
  (`ps -eo args | grep -iE "hlvr|vrchat|beat"`), kill them, or restart
  Monado. One VR game at a time.
- **"headset not found" in an OpenVR game** — Steam rewrote
  `openvrpaths.vrpath`; verify xrizer is `runtime[0]` and the file is
  read-only.
- **`XR_ERROR_RUNTIME_UNAVAILABLE` / wineopenxr error -6** — the game runs
  under Valve Proton but is an OpenXR title → GE-Proton.
- **Crash at map/level load, `winIVRMailbox.c:51` in the Proton log** —
  Source 2 + Proton 9/10 → Valve Proton 8.0.
- **"Could not create SDL window: wayland,x11 not available" / no D3D11
  device** — `SDL_VIDEODRIVER` leaked into the prefix; use the wrapper.
- **VRChat: "Cannot open Service Control Manager" / RPC unavailable** —
  the anti-cheat's service check hit a broken Wine prefix. Move
  `compatdata/438100` aside, let Steam create a fresh prefix, log in again.
- **Game takes 10–20 min to appear after a Proton/config change** — Steam
  runs shader precompilation (`iscriptevaluator`, fossilize) first; on a
  HDD this is slow. Don't diagnose a hang before it finishes.
- **World faces the wrong way / you spawn off-center** — recenter: hold the
  right Touch **Oculus button** ~1 s (buzz confirms), or
  `touch $XDG_RUNTIME_DIR/monado-rift-recenter`.

## SteamVR itself (Valve's runtime): status

The build ships Monado's SteamVR driver plugin
(`share/steamvr-monado`, register with
`./scripts/register-steamvr-plugin.sh`), which would let real SteamVR read
the CV1's tracking from Monado. **This path is untested with the CV1** and
has a hard structural problem: SteamVR's `vrcompositor` can only drive the
headset display in direct mode via X11 (or XWayland DRM leasing), which
typical Wayland setups — e.g. niri/Hyprland with xwayland-satellite,
especially on NVIDIA — do not provide, and Monado itself holds the CV1's
DRM lease while running. On the reference machine this is a verified dead
end.

In practice you don't need it: everything games require from SteamVR (the
OpenVR runtime API) is provided by xrizer. If you run a full X11 session
and get the plugin working, please open an issue with your setup — reports
welcome.

[OpenComposite](https://gitlab.com/znixian/OpenOVR), xrizer's predecessor,
may still work better for individual OpenVR titles; it is untested with
this driver.
