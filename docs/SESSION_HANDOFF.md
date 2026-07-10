# Session handoff (2026-07-10)

claude --resume 4f5519ac-4f11-4773-bd1b-67e44e03ac68

State of the Rift CV1 + Monado work on Maike's NixOS/niri/GTX1060 machine.

## Architecture (read this first)

```
OpenXR games (Beat Saber):  game → wineopenxr (GE-Proton) → Monado
OpenVR games (Alyx/VRChat): game → openvr_api.dll → Proton vrclient bridge
                              → host openvrpaths.vrpath → xrizer → OpenXR → Monado
```

- **xrizer** is a Linux OpenVR-runtime reimplementation on top of OpenXR. It is what
  replaces SteamVR: OpenVR games (native AND Proton) resolve their runtime through
  `~/.config/openvr/openvrpaths.vrpath`, and with xrizer first they talk straight to
  Monado. No SteamVR process ever runs.
- **Proton versions**: GE-Proton10-33 everywhere, EXCEPT Alyx on **Valve Proton 8.0**.
  Two independent reasons: (1) Valve Proton's wineopenxr refuses non-SteamVR OpenXR
  runtimes (registry gate, error -6) → OpenXR games need GE. (2) Proton 9/10's
  rewritten vrclient crashes on Source 2's undocumented IVRMailbox interface with any
  non-SteamVR runtime (exec fault + `winIVRMailbox.c:51` assert) → Alyx needs Proton
  8's old winelib bridge. Unity OpenVR games (VRChat) don't touch IVRMailbox → GE ok.
- **SteamVR itself: not viable on this machine, by design.** vrcompositor only does
  direct mode via X11/XWayland DRM leasing; niri+xwayland-satellite doesn't provide
  that path, NVIDIA leases are flaky, and Monado already owns the CV1's display lease
  and USB. Everything SteamVR would provide (OpenVR runtime for games) xrizer already
  does. Refs: NVIDIA forum "SteamVR DRM display leasing on Wayland", Arch wiki/forums
  Error 498 reports, lvra wiki.

## Verified working (headless; in-headset re-test pending)

- Beat Saber (620980, GE-Proton10-33): FOCUSED. Black screen from 07-05 test explained:
  a zombie game client kept the primary Monado session (2 clients never disconnected);
  fresh monado start + one-game-at-a-time fixed it. If it recurs: check
  `ps -eo args | grep -iE "hlvr|vrchat|beat"` for leftovers and kill them.
- Half-Life Alyx (546560, proton_8): FOCUSED, frames submitted (verified 07-05).
- VRChat (438100, GE-Proton10-33): FOCUSED (07-05; prefix was rebuilt → re-login).
- **Recenter** (new, in driver): hold RIGHT Touch Oculus button ~1s (buzz confirms),
  or `touch $XDG_RUNTIME_DIR/monado-rift-recenter`, or `monado-ctl -c` (LOCAL only).
  Verified via headless probe: yaw→0.1°, STAGE origin at head XZ, floor kept.

## Driver changes 2026-07-06 (committed 2026-07-10)

`src/rift/rift_system.c|rift_touch.c|rift_driver.h|target_builder_rift.c`,
`ohmd/drv_oculus_rift/rift-tracker.{c,h}`:

1. **world_from_tracker pose** replaces the hardcoded 180°Y output flip (07-05 fix for
   "world faces backwards"): initialized to 180°Y, recomputed on recenter so current
   HMD yaw/XZ = forward/origin. Applied to pose/velocity of HMD + controllers.
2. **Camera-lock gating**: poses are only reported as tracked after the device's first
   real camera observation (`rift_tracked_device_has_position_lock`, new vendored
   getter). Before that → 3DoF fallback (HMD at eye height, controllers parked in
   front). Previously devices without lock were reported POSITION_TRACKED at the UKF
   init pose = "controller frozen at origin" (the Alyx symptom).
3. **Recenter triggers**: right-controller Oculus button hold (io thread) + runtime
   file check. Confirmation buzz via right Touch haptics.
4. **Diagnostics**: default RIFT_LOG now info; INFO on first radio message per
   controller (awake/asleep); WARN every 5s while controller calibration-over-radio
   is still failing.

## Open: Touch controllers (main suspect list)

User report (07-05, Alyx): controllers stuck at default position, vibrating. Facts:
no "connected and calibrated" ever logged BUT log level hid INFO; no radio-RX errors;
haptics (TX) work. With new logging, the next controller wake will show either:

- no "radio link up" line at all → radio RX path broken (os_hid_read on interface 1)
- "radio link up" but repeating calibration WARNs → flash-read protocol issue
  (rift-hmd-radio.c, os_hid feature-report semantics)
- calibrated but never position-locked → LED/exposure sync issue (cameras never match
  controller LED patterns; check sensor exposure sync + `Error pose candidate` spam).
  Controllers now at least appear at a parked 3DoF pose instead of frozen at origin.
  Also watch: `iso_transfer_cb` USB errors (one burst seen 07-05) — sensor stream health.

## 2026-07-09 rebuild incident (lesson)

User ran nixos-rebuild on 07-09: it deployed the OLD driver (85aa8bc) because the
system flake tracks **github:MaySeikatsu/monado-rift-wayland** — local uncommitted
work is invisible to it. The reboot also killed the manual fixed instance, and the
units were still masked → nothing served VR at all ("can't interact with the VR
interface"). NOT an env-var problem: /etc/systemd/user/monado.service (from
services.monado + systemd.user.services.monado.environment in configuration.nix)
carries everything — WAYLAND_DIRECT, ORIGIN_OFFSET_Y=0.9, pacing, IPC_EXIT, and
ExecStart=/run/wrappers/bin/monado-service (CAP_SYS_NICE). Deploy path is always:
commit → push → `nix flake update monado-rift-wayland` in ~/.config/nixos →
nixos-rebuild switch.

## Current runtime state

- Fixed monado runs as MANUAL process from session scratchpad (`monado-fixed2`,
  restarted 07-10, log `monado-test3.log`). systemd monado.service AND
  monado.socket are **masked**: Monado's client lib auto-starts monado.socket via
  systemd on connect hiccups, and the socket unit then steals the socket path while
  activating the OLD driver build (bit us twice on 07-06 + effectively on 07-09).
- Deployment (after push + flake update + nixos-rebuild switch):
  `pkill -x monado-service`, then `systemctl --user unmask monado.socket
  monado.service && systemctl --user daemon-reload && systemctl --user start
  monado.socket`. Manual instance lacks CAP_SYS_NICE → "Frame late by ~11ms"
  pacing spam; the packaged unit has the wrapper.
- `~/.local/bin/monado-vr-wrap` still exports PROTON_LOG=1 (keep until controller
  debugging done; repo copy `scripts/monado-vr-wrap.sh` is the clean version).
- `~/.config/openvr/openvrpaths.vrpath`: xrizer first, chmod 444 (Steam rewrites it
  otherwise!). Will be store-managed after rebuild (see below).
- hm-module now has `programs.monado-rift.openvr.*` (xrizer + read-only vrpath) and
  `steamWrapper.enable` (installs wrapper) — declarative from next home-manager
  switch. NOTE: hm will refuse to overwrite the existing hand-made wrapper/vrpath
  files; remove them before switching.
- Proton 7.0 installed by mistake (appid mix-up), can be uninstalled. Old prefixes
  kept: `compatdata/546560.bak-ge10`, `compatdata/438100.bak-broken-rpc`.

## Approach notes

- Headless pose probe: scratchpad `xr_pose_probe.c` (XR_MND_headless); build with
  nixpkgs openxr-loader(-dev) + gcc. Manual monado-service needs stdin from a pipe
  (`tail -f /dev/null |`) or init_epoll(stdin) kills it at startup.
- pgrep/pkill -f self-match the harness shell (kills own shell, exit 144): filter
  `grep -vE "zsh|bash"`, kill by pid.
- Steam must be started with DISPLAY in env; session `SDL_VIDEODRIVER=wayland` must
  not reach Proton 8 games (wrapper unsets it).
- First launch after Proton/config changes: iscriptevaluator + fossilize can take
  10-20 min on the HDD before hlvr/game even starts. Be patient before diagnosing.
- Memory files: `maike-setup`, `monado-rift-state`, `steam-vr-chain`.
