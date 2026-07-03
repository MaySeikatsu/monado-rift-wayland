# Copyright 2025, monado-rift-wayland contributors.
# SPDX-License-Identifier: BSL-1.0
{ runCommand, driverSrc }:

runCommand "monado-rift-udev-rules" { } ''
  mkdir -p $out/lib/udev/rules.d
  cp ${driverSrc}/udev/70-oculus-rift-cv1.rules $out/lib/udev/rules.d/
''
