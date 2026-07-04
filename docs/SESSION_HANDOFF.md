# Session handoff (2026-07-04)

State of the Rift CV1 + Monado work on Maike's NixOS/niri/GTX1060 machine.

## Works
- **Driver**: HMD 6DoF constellation tracking, both sensors, Touch controllers all init on hardware. `monado.service` (system, GE via `services.monado.package = config.hardware.oculus-rift-cv1.package`) runs, DRM-leases the headset. Needs `XRT_COMPOSITOR_FORCE_WAYLAND_DIRECT=1` (NVIDIA+Wayland).
- **Beat Saber (620980)**: reaches `FOCUSED` / `BEGIN_SESSION`. Recipe: **GE-Proton10-33** + launch option `~/.local/bin/monado-vr-wrap %command%` (wrapper exports `PRESSURE_VESSEL_FILESYSTEMS_RW=$XDG_RUNTIME_DIR/monado_comp_ipc`; committed as `scripts/monado-vr-wrap.sh`). Both required. Do NOT set `PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES` on NixOS.
- Repo commits through `ac66642` pushed to main. Key fixes: absolute OpenXR manifest path, systemd unit, teardown UAF, USB kernel-driver self-heal, inline HM manifest.

## Open issues (next chat)
1. **HMD orientation shifted the wrong way in-game** (Beat Saber). Likely a coordinate-frame/axis sign bug in `rift_hmd.c` pose output or the tracker→xrt quaternion mapping. Compare against how `drv_rift_s`/OpenHMD map the pose. Test 3DoF (`RIFT_DISABLE_TRACKER=1`) to isolate fusion vs constellation.
2. **VRChat (438100) & Half-Life Alyx (546560) don't find the headset** despite same GE-Proton+wrapper config (both use SteamLinuxRuntime_sniper, first-boot prefix rebuild + EAC for VRChat). Get game logs: VRChat `compatdata/438100/pfx/.../VRChat/output_log_*.txt`; Alyx add `PROTON_LOG=1` → `~/steam-546560.log`, grep `could not initialize openxr: -N`. Suspect OpenVR path (both are OpenVR-native, not OpenXR like Beat Saber) → needs xrizer/OpenComposite OR the SteamVR plugin route.
3. **SteamVR plugin route** (user wants this): register Monado's built SteamVR driver (`$pkg/share/steamvr-monado`) via `scripts/register-steamvr-plugin.sh`. Likely the real fix for OpenVR games (VRChat/Alyx).

## Approach notes
- Memory files exist: `maike-setup`, `monado-rift-state`, `steam-vr-chain` (full debug playbook there).
- Editing Steam vdfs needs Steam fully stopped (`pkill -9 -f steam.sh`). Backups: `*.pre-final`.
- nushell login shell → wrap shell cmds in `bash -c`.
- Verify a game: game `Player.log`/`output_log` for `XR_SESSION_STATE_*`; `journalctl --user -u monado.service` for `BEGIN_SESSION` (real) vs rapid connect/disconnect (wineopenxr probes).
