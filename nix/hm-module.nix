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

    openvr = {
      enable = lib.mkOption {
        type = lib.types.bool;
        default = true;
        description = ''
          Register xrizer (an OpenVR implementation on top of OpenXR) as
          the active OpenVR runtime via
          {file}`~/.config/openvr/openvrpaths.vrpath`. This is what lets
          OpenVR games - both native Linux and Windows-via-Proton (the
          Proton vrclient bridge resolves the host runtime through this
          file) - run against Monado without SteamVR. The file is a
          read-only store symlink, which also stops Steam from putting
          SteamVR back as the first runtime on every start.
        '';
      };

      xrizerPackage = lib.mkOption {
        type = lib.types.package;
        default = pkgs.xrizer;
        defaultText = lib.literalExpression "pkgs.xrizer";
        description = "The xrizer package providing lib/xrizer/bin/linux64/vrclient.so.";
      };

      steamPath = lib.mkOption {
        type = lib.types.str;
        default = "${config.home.homeDirectory}/.local/share/Steam";
        defaultText = lib.literalExpression "\"\${config.home.homeDirectory}/.local/share/Steam\"";
        description = "Steam root used for the config/log entries of openvrpaths.vrpath.";
      };
    };

    steamWrapper.enable = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = ''
        Install the Steam launch-option wrapper at
        {file}`~/.local/bin/monado-vr-wrap`. Use it as the launch option
        `~/.local/bin/monado-vr-wrap %command%` for every VR game: it
        shares the Monado IPC socket with Steam's pressure-vessel
        container and scrubs environment variables that break Proton.
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    home.packages = [ cfg.package ];

    # Written as inline text (a regular file in the home-manager store dir,
    # one symlink hop from ~/.config) rather than source = "${pkg}/...".
    # source = would create a two-hop symlink chain, which Steam's
    # pressure-vessel OpenXR runtime import cannot follow - Proton games
    # then fail with XR_ERROR_RUNTIME_UNAVAILABLE.
    xdg.configFile."openxr/1/active_runtime.json" = lib.mkIf cfg.defaultRuntime {
      text = builtins.toJSON {
        file_format_version = "1.0.0";
        runtime = {
          name = "Monado";
          library_path = "${cfg.package}/lib/libopenxr_monado.so";
        };
      };
    };

    # OpenVR runtime registration. xrizer only: listing SteamVR here (even
    # second) is pointless because Steam rewrites this file at startup when
    # it is writable, putting SteamVR first and silently breaking the whole
    # OpenVR->Monado chain. The read-only store symlink prevents that.
    xdg.configFile."openvr/openvrpaths.vrpath" = lib.mkIf cfg.openvr.enable {
      text = builtins.toJSON {
        config = [ "${cfg.openvr.steamPath}/config" ];
        external_drivers = null;
        jsonid = "vrpathreg";
        log = [ "${cfg.openvr.steamPath}/logs" ];
        runtime = [ "${cfg.openvr.xrizerPackage}/lib/xrizer" ];
        version = 1;
      };
    };

    home.file.".local/bin/monado-vr-wrap" = lib.mkIf cfg.steamWrapper.enable {
      source = ../scripts/monado-vr-wrap.sh;
      executable = true;
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
