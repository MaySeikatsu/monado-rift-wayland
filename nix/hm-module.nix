# Copyright 2025, monado-rift-wayland contributors.
# SPDX-License-Identifier: BSL-1.0
#
# home-manager module: installs the patched Monado, makes it the active
# OpenXR runtime for the user and (optionally) runs monado-service as a
# systemd user service.
#
#   imports = [ monado-rift-wayland.homeManagerModules.default ];
#   programs.monado-rift = {
#     enable = true;
#     defaultRuntime = true;          # write ~/.config/openxr/1/active_runtime.json
#     service.enable = true;          # systemd --user service (start on demand)
#     environment.RIFT_LOG = "info";
#   };
#
# NOTE: udev rules are system-level; on NixOS pair this with the flake's
# nixosModules.default (hardware.oculus-rift-cv1.enable = true), on other
# distros run scripts/install-udev-rules.sh once.
self:
{
  config,
  lib,
  pkgs,
  ...
}:
let
  cfg = config.programs.monado-rift;
  system = pkgs.stdenv.hostPlatform.system;
in
{
  options.programs.monado-rift = {
    enable = lib.mkEnableOption "Monado with the Oculus Rift CV1 driver";

    package = lib.mkOption {
      type = lib.types.package;
      default = self.packages.${system}.monado-rift;
      defaultText = lib.literalExpression "monado-rift-wayland.packages.\${system}.monado-rift";
      description = "The patched Monado package.";
    };

    defaultRuntime = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = ''
        Make Monado the active OpenXR runtime by linking
        {file}`~/.config/openxr/1/active_runtime.json` to this package's
        runtime manifest.
      '';
    };

    service = {
      enable = lib.mkEnableOption "a systemd user service for monado-service";

      autoStart = lib.mkOption {
        type = lib.types.bool;
        default = false;
        description = ''
          Start monado-service with the graphical session. When false the
          unit is installed but only started on demand
          (`systemctl --user start monado-rift`).
        '';
      };
    };

    environment = lib.mkOption {
      type = lib.types.attrsOf lib.types.str;
      default = {
        RIFT_LOG = "info";
      };
      description = "Environment variables for the monado-service unit (RIFT_LOG, RIFT_DISABLE_TRACKER, XRT_COMPOSITOR_* ...).";
    };
  };

  config = lib.mkIf cfg.enable {
    home.packages = [ cfg.package ];

    xdg.configFile."openxr/1/active_runtime.json" = lib.mkIf cfg.defaultRuntime {
      source = "${cfg.package}/share/openxr/1/openxr_monado.json";
    };

    systemd.user.services.monado-rift = lib.mkIf cfg.service.enable {
      Unit = {
        Description = "Monado OpenXR runtime (Oculus Rift CV1)";
        Documentation = "https://github.com/MaySeikatsu/monado-rift-wayland";
        After = [ "graphical-session.target" ];
        PartOf = [ "graphical-session.target" ];
      };
      Service = {
        ExecStart = "${cfg.package}/bin/monado-service";
        Environment = lib.mapAttrsToList (n: v: "${n}=${v}") cfg.environment;
        Restart = "no";
      };
      Install = lib.mkIf cfg.service.autoStart {
        WantedBy = [ "graphical-session.target" ];
      };
    };
  };
}
