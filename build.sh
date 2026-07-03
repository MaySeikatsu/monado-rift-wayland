#!/usr/bin/env bash
# Copyright 2025, monado-rift-wayland contributors.
# SPDX-License-Identifier: BSL-1.0
#
# Superbuild: fetches Monado at a pinned, known-good revision, drops the
# Rift CV1 driver in, applies the small integration patch and builds the
# whole runtime (monado-service, monado-cli, SteamVR driver plugin).
#
# Usage:
#   ./build.sh                 # fetch + build
#   ./build.sh --install       # ... and install to PREFIX (default /usr/local, uses sudo if needed)
#   MONADO_URL=... MONADO_REF=... ./build.sh   # override the Monado source
#
# The driver is developed and compile-verified against MONADO_REF below.
# Newer Monado revisions may have xrt_device API changes; override at
# your own risk (patches welcome for forward ports).

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

MONADO_URL="${MONADO_URL:-https://gitlab.freedesktop.org/monado/monado.git}"
# Monado main from 2024-01-06, the revision this driver is verified against.
MONADO_REF="${MONADO_REF:-0d662607e43aa3f4207fef2833badffcd0e6eae4}"
# GitHub fallback mirror containing the same commit, for hosts where
# gitlab.freedesktop.org is unreachable.
MONADO_MIRROR_URL="${MONADO_MIRROR_URL:-https://github.com/Pylgos/monado.git}"

BUILD_DIR="${BUILD_DIR:-$HERE/build}"
MONADO_DIR="$BUILD_DIR/monado"
PREFIX="${PREFIX:-/usr/local}"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}"
NPROC="$(nproc 2>/dev/null || echo 4)"

DO_INSTALL=0
for arg in "$@"; do
	case "$arg" in
	--install) DO_INSTALL=1 ;;
	--help | -h)
		sed -n '5,18p' "$0"
		exit 0
		;;
	*)
		echo "Unknown argument: $arg" >&2
		exit 1
		;;
	esac
done

msg() { echo -e "\033[1;32m==>\033[0m $*"; }
err() { echo -e "\033[1;31mERROR:\033[0m $*" >&2; }

check_deps() {
	local missing=()
	for tool in git cmake ninja pkg-config cc c++; do
		command -v "$tool" > /dev/null || missing+=("$tool")
	done
	for pc in libusb-1.0 libjpeg eigen3 libudev vulkan wayland-client; do
		pkg-config --exists "$pc" 2> /dev/null || missing+=("$pc (dev package)")
	done
	if ((${#missing[@]} > 0)); then
		err "Missing build dependencies: ${missing[*]}"
		cat >&2 <<-'EOF'

			On Debian/Ubuntu:
			  sudo apt install git cmake ninja-build pkg-config build-essential \
			    libusb-1.0-0-dev libjpeg-dev libeigen3-dev libudev-dev \
			    libvulkan-dev glslang-tools libgl1-mesa-dev \
			    libx11-xcb-dev libxcb-randr0-dev libxrandr-dev \
			    libwayland-dev wayland-protocols libdrm-dev \
			    libhidapi-dev libbluetooth-dev libopencv-dev

			On Arch:
			  sudo pacman -S --needed git cmake ninja pkgconf base-devel \
			    libusb libjpeg-turbo eigen systemd-libs vulkan-headers \
			    vulkan-icd-loader glslang mesa libx11 libxcb libxrandr \
			    wayland wayland-protocols libdrm hidapi bluez-libs opencv

			On Fedora:
			  sudo dnf install git cmake ninja-build pkgconf gcc gcc-c++ \
			    libusb1-devel libjpeg-turbo-devel eigen3-devel systemd-devel \
			    vulkan-loader-devel vulkan-headers glslang mesa-libGL-devel \
			    libX11-devel libxcb-devel libXrandr-devel wayland-devel \
			    wayland-protocols-devel libdrm-devel hidapi-devel bluez-libs-devel \
			    opencv-devel
		EOF
		exit 1
	fi
	if ! pkg-config --exists opencv4 2> /dev/null && ! pkg-config --exists opencv 2> /dev/null; then
		msg "NOTE: OpenCV not found. The driver still builds, but installing OpenCV"
		msg "      is recommended for the best 6DoF pose refinement (libopencv-dev)."
	fi
}

fetch_monado() {
	if [[ -d "$MONADO_DIR/.git" ]]; then
		msg "Monado checkout already present: $MONADO_DIR"
		return
	fi

	mkdir -p "$MONADO_DIR"
	git -C "$MONADO_DIR" init -q
	msg "Fetching Monado @ $MONADO_REF"
	if ! git -C "$MONADO_DIR" fetch -q --depth 1 "$MONADO_URL" "$MONADO_REF" 2> /dev/null; then
		msg "Fetch from $MONADO_URL failed, trying mirror $MONADO_MIRROR_URL"
		if ! git -C "$MONADO_DIR" fetch -q --depth 1 "$MONADO_MIRROR_URL" "$MONADO_REF"; then
			# Shallow fetch of an arbitrary commit needs
			# uploadpack.allowReachableSHA1InWant; fall back to a
			# full-ish fetch of the default branch history.
			msg "Shallow commit fetch refused, fetching branch history (slower)"
			git -C "$MONADO_DIR" fetch -q "$MONADO_MIRROR_URL" main
		fi
	fi
	git -C "$MONADO_DIR" checkout -q FETCH_HEAD 2> /dev/null || git -C "$MONADO_DIR" checkout -q "$MONADO_REF"
}

install_driver_sources() {
	msg "Installing drv_rift sources into the Monado tree"
	rm -rf "$MONADO_DIR/src/xrt/drivers/rift"
	cp -r "$HERE/src/rift" "$MONADO_DIR/src/xrt/drivers/rift"
	# The builder lives with the other target builders.
	mv "$MONADO_DIR/src/xrt/drivers/rift/target_builder_rift.c" \
		"$MONADO_DIR/src/xrt/targets/common/target_builder_rift.c"

	local patch="$HERE/patches/0001-monado-add-rift-cv1-driver.patch"
	if git -C "$MONADO_DIR" apply --reverse --check "$patch" 2> /dev/null; then
		msg "Integration patch already applied"
	else
		msg "Applying integration patch"
		git -C "$MONADO_DIR" apply "$patch"
	fi
}

build_monado() {
	msg "Configuring Monado (prefix: $PREFIX)"
	cmake -S "$MONADO_DIR" -B "$MONADO_DIR/build" -G Ninja \
		-DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" \
		-DXRT_BUILD_DRIVER_RIFT=ON \
		-DXRT_FEATURE_STEAMVR_PLUGIN=ON

	if ! grep -q "XRT_BUILD_DRIVER_RIFT:BOOL=ON" "$MONADO_DIR/build/CMakeCache.txt"; then
		err "XRT_BUILD_DRIVER_RIFT did not enable - missing libusb or libjpeg dev packages?"
		exit 1
	fi

	msg "Building (this takes a few minutes)"
	ninja -C "$MONADO_DIR/build" -j "$NPROC"

	msg "Build complete:"
	echo "    monado-service : $MONADO_DIR/build/src/xrt/targets/service/monado-service"
	echo "    monado-cli     : $MONADO_DIR/build/src/xrt/targets/cli/monado-cli"
	echo "    SteamVR plugin : $MONADO_DIR/build/steamvr-monado"
}

install_monado() {
	msg "Installing to $PREFIX"
	if [[ -w "$PREFIX" ]]; then
		ninja -C "$MONADO_DIR/build" install
	else
		sudo ninja -C "$MONADO_DIR/build" install
	fi
	msg "Installed. Next steps:"
	echo "    sudo ./scripts/install-udev-rules.sh   # device permissions"
	echo "    ./scripts/set-openxr-runtime.sh        # make Monado the active OpenXR runtime"
	echo "    ./scripts/register-steamvr-plugin.sh   # let SteamVR use the Rift through Monado"
}

check_deps
fetch_monado
install_driver_sources
build_monado
if ((DO_INSTALL)); then
	install_monado
fi
