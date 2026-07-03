#!/usr/bin/env bash
# Copyright 2025, monado-rift-wayland contributors.
# SPDX-License-Identifier: BSL-1.0
# Registers Monado's SteamVR driver plugin with SteamVR, so SteamVR
# sessions use the Rift CV1 (and its Touch controllers) through Monado.
#
# Usage: ./register-steamvr-plugin.sh [--remove]

set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PREFIX="${PREFIX:-/usr/local}"

CANDIDATES=(
	"$PREFIX/share/steamvr-monado"
	"/usr/share/steamvr-monado"
	"$HERE/../build/monado/build/steamvr-monado" # uninstalled dev build
)

PLUGIN_DIR=""
for c in "${CANDIDATES[@]}"; do
	if [[ -f "$c/driver.vrdrivermanifest" ]]; then
		PLUGIN_DIR="$c"
		break
	fi
done

if [[ -z "$PLUGIN_DIR" ]]; then
	echo "ERROR: steamvr-monado plugin not found. Build with XRT_FEATURE_STEAMVR_PLUGIN=ON first (./build.sh)." >&2
	exit 1
fi

# Find vrpathreg from the SteamVR install.
STEAM_ROOTS=(
	"${STEAM_ROOT:-}"
	"$HOME/.steam/steam"
	"$HOME/.local/share/Steam"
	"$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam" # flatpak
)

VRPATHREG=""
STEAMVR_DIR=""
for root in "${STEAM_ROOTS[@]}"; do
	[[ -n "$root" ]] || continue
	if [[ -x "$root/steamapps/common/SteamVR/bin/vrpathreg.sh" ]]; then
		STEAMVR_DIR="$root/steamapps/common/SteamVR"
		VRPATHREG="$STEAMVR_DIR/bin/vrpathreg.sh"
		break
	fi
done

if [[ -z "$VRPATHREG" ]]; then
	echo "ERROR: SteamVR installation not found (is SteamVR installed via Steam?)." >&2
	echo "You can register manually by adding this path to the 'external_drivers' list in" >&2
	echo "~/.steam/steam/config/openvrpaths.vrpath :" >&2
	echo "    $PLUGIN_DIR" >&2
	exit 1
fi

if [[ "${1:-}" == "--remove" ]]; then
	"$VRPATHREG" removedriver "$PLUGIN_DIR"
	echo "Removed Monado driver from SteamVR."
	exit 0
fi

"$VRPATHREG" adddriver "$PLUGIN_DIR"
echo "Registered Monado SteamVR driver: $PLUGIN_DIR"
echo
echo "Notes:"
echo " - Start monado-service before launching SteamVR."
echo " - To make SteamVR prefer Monado's HMD, disable its own Oculus support if it"
echo "   fights over the device (SteamVR settings -> Startup -> Manage Add-Ons)."
echo " - Undo with: $0 --remove"
