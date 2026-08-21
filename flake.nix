{
  description = "Umbriel, a Wayland compositor built on wlroots and SceneFX.";

  inputs = {
    nixpkgs.url = "https://channels.nixos.org/nixos-unstable/nixexprs.tar.xz";
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
      };

      packages = forEachSystem (
        { pkgs, ... }:
        {
          default = pkgs.callPackage ./nix/package.nix { inherit scenefx; };
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
        { pkgs, lib, ... }:
        {
          imports = [ ./nix/home-module.nix ];
          programs.umbriel.package = lib.mkDefault self.packages.${pkgs.stdenv.hostPlatform.system}.default;
        };

      hjemModules.default =
        { pkgs, lib, ... }:
        {
          imports = [ ./nix/hjem-module.nix ];
          programs.umbriel.package = lib.mkDefault self.packages.${pkgs.stdenv.hostPlatform.system}.default;
        };

      finixModules.default =
        { pkgs, lib, ... }:
        {
          imports = [ ./nix/finix-module.nix ];
          programs.umbriel.package = lib.mkDefault self.packages.${pkgs.stdenv.hostPlatform.system}.default;
        };

      nixosModules.default =
        { pkgs, lib, ... }:
        {
          imports = [ ./nix/nixos-module.nix ];
          programs.umbriel.package = lib.mkDefault self.packages.${pkgs.stdenv.hostPlatform.system}.default;
        };
    };
}
