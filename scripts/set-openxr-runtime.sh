#!/usr/bin/env bash
# Copyright 2025, monado-rift-wayland contributors.
# SPDX-License-Identifier: BSL-1.0
# Makes Monado the active OpenXR runtime for the current user by linking
# active_runtime.json to the installed (or freshly built) manifest.

set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PREFIX="${PREFIX:-/usr/local}"
CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/openxr/1"

CANDIDATES=(
	"$PREFIX/share/openxr/1/openxr_monado.json"
	"/usr/share/openxr/1/openxr_monado.json"
	"$HERE/../build/monado/build/openxr_monado-dev.json" # uninstalled dev build
)

MANIFEST=""
for c in "${CANDIDATES[@]}"; do
	if [[ -f "$c" ]]; then
		MANIFEST="$c"
		break
	fi
done

if [[ -z "$MANIFEST" ]]; then
	echo "ERROR: no openxr_monado.json manifest found. Build and/or install first:" >&2
	echo "    ./build.sh --install" >&2
	exit 1
fi

mkdir -p "$CONFIG_DIR"
ln -sf "$MANIFEST" "$CONFIG_DIR/active_runtime.json"

echo "Active OpenXR runtime set for this user:"
echo "    $CONFIG_DIR/active_runtime.json -> $MANIFEST"
echo
echo "Undo with: rm $CONFIG_DIR/active_runtime.json"
