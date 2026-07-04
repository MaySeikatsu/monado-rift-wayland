#!/bin/sh
# VR launch wrapper: makes the Monado IPC socket reachable inside Steam's
# pressure-vessel container. Use as a Steam launch option:
#     /home/maike/.local/bin/monado-vr-wrap %command%
#
# Requirements for Proton VR on Monado (Rift CV1):
#  - Use a GE-Proton build. Valve Proton Experimental gates wineopenxr on a
#    SteamVR-only registry key (Software\Wine\VR "state") and returns error
#    -6 for any other OpenXR runtime, so its VR init fails with Monado.
#  - The OpenXR manifest at ~/.config/openxr/1/active_runtime.json must be a
#    direct symlink to the Monado runtime (handled by the programs.monado-rift
#    home-manager module).
#
# Steam strips PRESSURE_VESSEL_* from env-prefix launch options, so this must
# be a wrapper that sets the var and then execs the game command.
export PRESSURE_VESSEL_FILESYSTEMS_RW="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/monado_comp_ipc"
exec "$@"
