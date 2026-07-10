# Copyright 2025, monado-rift-wayland contributors.
# SPDX-License-Identifier: BSL-1.0
#
# Monado built from the pinned source with the Rift CV1 driver integrated,
# the SteamVR plugin and the Wayland DRM-lease direct-mode backend.
{
  lib,
  stdenv,
  cmake,
  ninja,
  pkg-config,
  python3,
  glslang,
  # libraries
  eigen,
  libusb1,
  libjpeg,
  opencv4,
  vulkan-headers,
  vulkan-loader,
  libGL,
  udev,
  hidapi,
  bluez,
  wayland,
  wayland-protocols,
  wayland-scanner,
  libdrm,
  libxkbcommon,
  libx11,
  libxcb,
  libxrandr,
  zlib,
  SDL2,
  # sources
  monado-src,
  driverSrc,
}:

stdenv.mkDerivation {
  pname = "monado-rift";
  version = "24.0.0-rift-${driverSrc.shortRev or "dirty"}";

  src = monado-src;

  postPatch = ''
    # Drop the Rift CV1 driver into the tree.
    cp -r ${driverSrc}/src/rift src/xrt/drivers/rift
    chmod -R u+w src/xrt/drivers/rift
    mv src/xrt/drivers/rift/target_builder_rift.c src/xrt/targets/common/

    # Wire it into the build (CMake options, config header, builder list).
    patch -p1 < ${driverSrc}/patches/0001-monado-add-rift-cv1-driver.patch
  '';

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    python3
    glslang
    wayland-scanner
  ];

  buildInputs = [
    eigen
    libusb1
    libjpeg
    opencv4
    vulkan-headers
    vulkan-loader
    libGL
    udev
    hidapi
    bluez
    wayland
    wayland-protocols
    wayland-scanner
    libdrm
    libxkbcommon
    libx11
    libxcb
    libxrandr
    zlib
    SDL2
  ];

  cmakeFlags = [
    "-DXRT_BUILD_DRIVER_RIFT=ON"
    "-DXRT_FEATURE_STEAMVR_PLUGIN=ON"
    # Without this the OpenXR runtime manifest gets a broken relative
    # library_path under Nix (share/openxr/1/../../.. + absolute store
    # path), which no OpenXR loader (host or pressure-vessel) can resolve.
    "-DXRT_OPENXR_INSTALL_ABSOLUTE_RUNTIME_PATH=ON"
  ];

  postConfigure = ''
    if ! grep -q "XRT_BUILD_DRIVER_RIFT:BOOL=ON" CMakeCache.txt; then
      echo "XRT_BUILD_DRIVER_RIFT did not enable (missing libusb/libjpeg?)" >&2
      exit 1
    fi
  '';

  postInstall = ''
    # Monado's unit template concatenates the install prefix with the
    # (already absolute) bindir, yielding a doubled, broken ExecStart path.
    for unit in $out/lib/systemd/user/monado.service $out/lib/systemd/user/monado-dev.service; do
      if [ -f "$unit" ]; then
        sed -i "s|^ExecStart=.*monado-service.*$|ExecStart=$out/bin/monado-service|" "$unit"
      fi
    done

    # This Monado predates manifests advertising libmonado (the runtime
    # control API); overlay tools like WayVR need the path to manage
    # client z-order/visibility over games. Add the key upstream uses.
    manifest=$out/share/openxr/1/openxr_monado.json
    if [ -f "$manifest" ] && [ -f "$out/lib/libmonado.so" ]; then
      sed -i "s|\"library_path\": \(.*\)$|\"library_path\": \1,\n        \"MND_libmonado_path\": \"$out/lib/libmonado.so\"|" "$manifest"
    fi
  '';

  meta = with lib; {
    description = "Monado OpenXR runtime with a native Oculus Rift CV1 driver";
    homepage = "https://github.com/MaySeikatsu/monado-rift-wayland";
    license = licenses.boost;
    platforms = platforms.linux;
    mainProgram = "monado-service";
  };
}
