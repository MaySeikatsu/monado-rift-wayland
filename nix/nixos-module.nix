# Copyright 2025, monado-rift-wayland contributors.
# SPDX-License-Identifier: BSL-1.0
#
# NixOS module: udev permissions for the Rift CV1 (HMD, Touch radio,
# constellation camera sensors) and optional system-wide install of the
# patched Monado runtime.
#
#   imports = [ monado-rift-wayland.nixosModules.default ];
#   hardware.oculus-rift-cv1.enable = true;
#
# You can combine this with nixpkgs' `services.monado` module to run the
# runtime as a systemd user service - just point it at our package:
#
#   services.monado = {
#     enable = true;
#     package = config.hardware.oculus-rift-cv1.package;
#     defaultRuntime = true;
#   };
self:
{
  config,
  options,
  lib,
  pkgs,
  ...
}:
let
  cfg = config.hardware.oculus-rift-cv1;
  system = pkgs.stdenv.hostPlatform.system;
in
{
  options.hardware.oculus-rift-cv1 = {
    enable = lib.mkEnableOption "Oculus Rift CV1 support (udev rules + Monado with the Rift driver)";

    package = lib.mkOption {
      type = lib.types.package;
      default = self.packages.${system}.monado-rift;
      defaultText = lib.literalExpression "monado-rift-wayland.packages.\${system}.monado-rift";
      description = "The patched Monado package to install.";
    };

    installPackage = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Add the patched Monado (monado-service, monado-cli) to environment.systemPackages.";
    };
  };

  config = lib.mkIf cfg.enable {
    services.udev.packages = [ self.packages.${system}.udev-rules ];
    environment.systemPackages = lib.mkIf cfg.installPackage [ cfg.package ];

    # The stock nixpkgs Monado ships the OpenHMD wrapper driver, which
    # claims the CV1's USB interfaces over libusb (detaching the kernel
    # HID driver) and blocks this driver. Two runtimes must not fight
    # over the headset.
    warnings =
      lib.optional
        (
          options.services ? monado
          && config.services.monado.enable
          && config.services.monado.package != cfg.package
        )
        ''
          hardware.oculus-rift-cv1: services.monado is enabled with a different
          Monado package. The stock Monado (via its OpenHMD driver) will claim the
          Rift CV1 over libusb and block the Rift driver. Set:
            services.monado.package = config.hardware.oculus-rift-cv1.package;
        '';
  };
}
