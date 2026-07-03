# Nix / NixOS / home-manager

The repo is a flake. It pins both nixpkgs and the exact Monado revision
the driver is verified against — hashes land in `flake.lock` on first
use, nothing to fill in manually.

```
github:MaySeikatsu/monado-rift-wayland
├── packages.{x86_64,aarch64}-linux
│   ├── monado-rift   Monado + drv_rift + SteamVR plugin + Wayland direct
│   └── udev-rules    udev rules package (for services.udev.packages)
├── nixosModules.default          hardware.oculus-rift-cv1.*
├── homeManagerModules.default    programs.monado-rift.*
├── overlays.default              pkgs.monado-rift / pkgs.monado-rift-udev-rules
├── apps                          nix run .#monado-service / .#monado-cli
└── devShells.default             full build environment for hacking
```

## Try it without installing anything

```sh
nix run github:MaySeikatsu/monado-rift-wayland#monado-cli -- probe
RIFT_LOG=debug nix run github:MaySeikatsu/monado-rift-wayland   # monado-service
```

(udev rules still need to be installed once — see below — otherwise the
devices aren't accessible from your user.)

## NixOS

```nix
# flake.nix of your system config
{
  inputs.monado-rift-wayland.url = "github:MaySeikatsu/monado-rift-wayland";

  outputs = { nixpkgs, monado-rift-wayland, ... }: {
    nixosConfigurations.myhost = nixpkgs.lib.nixosSystem {
      modules = [
        monado-rift-wayland.nixosModules.default
        {
          # udev rules + monado-service/monado-cli in systemPackages
          hardware.oculus-rift-cv1.enable = true;
        }
      ];
    };
  };
}
```

Optionally run it as a user service via nixpkgs' own Monado module,
pointed at this package:

```nix
services.monado = {
  enable = true;
  package = config.hardware.oculus-rift-cv1.package;
  defaultRuntime = true;   # registers the OpenXR runtime system-wide
};
# then e.g.:  systemctl --user start monado.service
```

## home-manager (any distro)

```nix
{
  inputs.monado-rift-wayland.url = "github:MaySeikatsu/monado-rift-wayland";

  # in your home config:
  imports = [ monado-rift-wayland.homeManagerModules.default ];

  programs.monado-rift = {
    enable = true;
    defaultRuntime = true;          # ~/.config/openxr/1/active_runtime.json
    service.enable = true;          # systemd --user unit "monado-rift"
    service.autoStart = false;      # start on demand
    environment = {
      RIFT_LOG = "info";
      # RIFT_DISABLE_TRACKER = "1";  # force 3DoF
    };
  };
}
```

Then `systemctl --user start monado-rift` (or just run `monado-service`).

On **non-NixOS** hosts, udev rules are the one system-level piece
home-manager can't manage — install them once:

```sh
sudo ./scripts/install-udev-rules.sh
# or by hand: copy udev/70-oculus-rift-cv1.rules to /etc/udev/rules.d/
```

## SteamVR plugin under Nix

The plugin is built into the package at
`$out/share/steamvr-monado`. Register it with SteamVR using the helper
(SteamVR's registry is stateful in `~/.steam`, so this is a one-off
imperative step, re-run after the store path changes):

```sh
PREFIX=$(nix build github:MaySeikatsu/monado-rift-wayland#monado-rift --print-out-paths) \
  ./scripts/register-steamvr-plugin.sh
```

## Dev environment

```sh
nix develop            # compilers, cmake/ninja, all libs, clangd, gdb, shellcheck
./build.sh             # classic superbuild works inside the shell
# or the pure build:
nix build .#monado-rift
```

## Notes

- The Monado source input fetches from gitlab.freedesktop.org. If that's
  unreachable, swap to the GitHub mirror without touching the flake:

  ```sh
  nix flake lock --override-input monado-src \
    github:Pylgos/monado/0d662607e43aa3f4207fef2833badffcd0e6eae4
  ```

- The package intentionally builds our pinned Monado rather than
  overriding `pkgs.monado` (current nixpkgs carries a newer Monado whose
  driver API has moved; see docs/development.md on forward-porting).
