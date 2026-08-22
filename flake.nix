{
  description = "Umbriel, a Wayland compositor built on wlroots and SceneFX.";

  inputs = {
    nixpkgs.url = "https://channels.nixos.org/nixos-unstable/nixexprs.tar.xz";
    xdg-desktop-portal-umbriel.url = "git+https://github.com/noctalia-dev/xdg-desktop-portal-umbriel";
    scenefx = {
      url = "git+https://github.com/noctalia-dev/scenefx?ref=umbriel";
      flake = false;
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      scenefx,
      xdg-desktop-portal-umbriel,
    }:
    let
      inherit (nixpkgs.lib) genAttrs getExe;

      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      forEachSystem =
        perSystem:
        genAttrs systems (
          system:
          let
            pkgs = nixpkgs.legacyPackages.${system};
          in
          perSystem { inherit pkgs system; }
        );
    in
    {
      overlays.default = final: prev: {
        umbriel = final.callPackage ./nix/package.nix { inherit scenefx; };
        xdg-desktop-portal-umbriel =
          self.packages.${final.stdenv.hostPlatform.system}.xdg-desktop-portal-umbriel;
      };

      packages = forEachSystem (
        { pkgs, ... }:
        {
          default = pkgs.callPackage ./nix/package.nix { inherit scenefx; };
          xdg-desktop-portal-umbriel =
            xdg-desktop-portal-umbriel.packages.${pkgs.stdenv.hostPlatform.system}.default;
        }
      );

      devShells = forEachSystem (
        { pkgs, system, ... }:
        {
          default = pkgs.callPackage ./nix/devshell.nix {
            umbriel = self.packages.${system}.default;
          };
        }
      );

      formatter = forEachSystem ({ pkgs, ... }: pkgs.nixfmt-tree);

      apps = forEachSystem (
        { system, ... }:
        {
          default = {
            type = "app";
            program = getExe self.packages.${system}.default;
          };
        }
      );

      homeModules.default =
        { pkgs, lib, ... }@args:
        {
          imports = [
            (import ./nix/home-module.nix (
              args
              // {
                xdg-desktop-portal-umbriel =
                  self.packages.${pkgs.stdenv.hostPlatform.system}.xdg-desktop-portal-umbriel;
              }
            ))
          ];
          wayland.windowManager.umbriel.package =
            lib.mkDefault
              self.packages.${pkgs.stdenv.hostPlatform.system}.default;
        };

      hjemModules.default =
        { pkgs, lib, ... }:
        {
          imports = [ ./nix/hjem-module.nix ];
          rum.desktops.umbriel.package =
            lib.mkDefault
              self.packages.${pkgs.stdenv.hostPlatform.system}.default;
        };

      finixModules.default =
        { pkgs, lib, ... }@args:
        {
          imports = [
            (import ./nix/finix-module.nix (
              args
              // {
                xdg-desktop-portal-umbriel =
                  self.packages.${pkgs.stdenv.hostPlatform.system}.xdg-desktop-portal-umbriel;
              }
            ))
          ];
          programs.umbriel.package = lib.mkDefault self.packages.${pkgs.stdenv.hostPlatform.system}.default;
        };

      nixosModules.default =
        { pkgs, lib, ... }@args:
        {
          imports = [
            (import ./nix/nixos-module.nix (
              args
              // {
                xdg-desktop-portal-umbriel =
                  self.packages.${pkgs.stdenv.hostPlatform.system}.xdg-desktop-portal-umbriel;
              }
            ))
          ];
          programs.umbriel.package = lib.mkDefault self.packages.${pkgs.stdenv.hostPlatform.system}.default;
        };
    };
}
