#!/usr/bin/env bash
# Copyright 2025, monado-rift-wayland contributors.
# SPDX-License-Identifier: BSL-1.0
# Installs the udev rules that grant the seated user access to the Rift
# HMD, radio and tracking sensors. Run with sudo.

set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ $EUID -ne 0 ]]; then
	echo "This script needs root, re-running with sudo..."
	exec sudo "$0" "$@"
fi

install -m 0644 "$HERE/../udev/70-oculus-rift-cv1.rules" /etc/udev/rules.d/70-oculus-rift-cv1.rules
udevadm control --reload
udevadm trigger

echo "udev rules installed. Re-plug the headset (or reboot) if it was connected."
