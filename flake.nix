{
  description = "Monado OpenXR runtime with a native Oculus Rift CV1 driver (6DoF constellation tracking, Touch controllers, Wayland direct mode)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    # Monado at the pinned revision this driver is compile-verified against
    # (main from 2024-01-06). The hash lands in flake.lock on first build -
    # no manual hashes needed. If gitlab.freedesktop.org is unreachable:
    #   nix flake lock --override-input monado-src \
    #     github:Pylgos/monado/0d662607e43aa3f4207fef2833badffcd0e6eae4
    monado-src = {
      url = "gitlab:monado/monado/0d662607e43aa3f4207fef2833badffcd0e6eae4?host=gitlab.freedesktop.org";
      flake = false;
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      monado-src,
    }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAllSystems (pkgs: rec {
        monado-rift = pkgs.callPackage ./nix/package.nix {
          inherit monado-src;
          driverSrc = self;
        };
        udev-rules = pkgs.callPackage ./nix/udev-rules.nix { driverSrc = self; };
        default = monado-rift;
      });

      overlays.default = final: prev: {
        monado-rift = final.callPackage ./nix/package.nix {
          inherit monado-src;
          driverSrc = self;
        };
        monado-rift-udev-rules = final.callPackage ./nix/udev-rules.nix { driverSrc = self; };
      };

      # NixOS: udev rules + system-wide install.
      #   imports = [ monado-rift-wayland.nixosModules.default ];
      #   hardware.oculus-rift-cv1.enable = true;
      nixosModules.default = import ./nix/nixos-module.nix self;

      # home-manager: package, OpenXR active runtime, user service.
      #   imports = [ monado-rift-wayland.homeManagerModules.default ];
      #   programs.monado-rift.enable = true;
      homeManagerModules.default = import ./nix/hm-module.nix self;

      apps = forAllSystems (pkgs: rec {
        monado-service = {
          type = "app";
          program = "${self.packages.${pkgs.stdenv.hostPlatform.system}.monado-rift}/bin/monado-service";
        };
        monado-cli = {
          type = "app";
          program = "${self.packages.${pkgs.stdenv.hostPlatform.system}.monado-rift}/bin/monado-cli";
        };
        default = monado-service;
      });

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.monado-rift ];
          packages = with pkgs; [
            clang-tools # clangd / clang-format
            gdb
            shellcheck
            nixfmt-rfc-style
          ];
          shellHook = ''
            echo "monado-rift-wayland dev shell"
            echo "  ./build.sh              classic superbuild (build/ directory)"
            echo "  nix build .#monado-rift pure nix build (result/ symlink)"
            echo "  RIFT_LOG=debug result/bin/monado-service"
          '';
        };
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt-rfc-style);
    };
}
