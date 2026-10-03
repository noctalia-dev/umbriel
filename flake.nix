{
  description = "Umbriel, a Wayland compositor built on wlroots.";

  inputs = {
    nixpkgs.url = "https://channels.nixos.org/nixos-unstable/nixexprs.tar.xz";
    xdg-desktop-portal-umbriel = {
      url = "github:noctalia-dev/xdg-desktop-portal-umbriel";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      xdg-desktop-portal-umbriel,
      ...
    }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      rev = self.shortRev or self.dirtyShortRev or "local";

      forEachSystem =
        perSystem: nixpkgs.lib.genAttrs systems (system: perSystem nixpkgs.legacyPackages.${system});

      withDefaultPackage =
        module:
        { pkgs, lib, ... }:
        {
          imports = [ module ];
          programs.umbriel.package = lib.mkDefault self.packages.${pkgs.stdenv.hostPlatform.system}.default;
        };
    in
    {
      formatter = forEachSystem (pkgs: pkgs.nixfmt-tree);

      overlays.default = final: _: {
        umbriel = final.callPackage ./nix/package.nix { inherit rev; };
      };

      packages = forEachSystem (pkgs: {
        default = pkgs.callPackage ./nix/package.nix { inherit rev; };
        xdg-desktop-portal-umbriel =
          xdg-desktop-portal-umbriel.packages.${pkgs.stdenv.hostPlatform.system}.default;
      });

      devShells = forEachSystem (pkgs: {
        default = pkgs.callPackage ./nix/devshell.nix {
          umbriel = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
        };
      });

      homeModules.default = withDefaultPackage ./nix/home-module.nix;
      hjemModules.default = withDefaultPackage ./nix/hjem-module.nix;
      nixosModules.default = withDefaultPackage (
        import ./nix/nixos-module.nix { inherit xdg-desktop-portal-umbriel; }
      );
    };
}
