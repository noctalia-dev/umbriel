{ xdg-desktop-portal-umbriel }:
{
  config,
  pkgs,
  lib,
  modulesPath,
  ...
}:
let
  cfg = config.programs.umbriel;
in
{
  # Disable the nixpkgs module to avoid conflicts
  disabledModules = [ "programs/wayland/umbriel.nix" ];

  options.programs.umbriel = {
    enable = lib.mkEnableOption "Umbriel, a Wayland compositor built on wlroots and SceneFX.";

    package = lib.mkOption {
      type = lib.types.nullOr lib.types.package;
      default = null;
      description = "The umbriel package to install.";
    };

    portalPackage = lib.mkOption {
      type = lib.types.nullOr lib.types.package;
      default = xdg-desktop-portal-umbriel.packages.${pkgs.stdenv.hostPlatform.system}.default;
      defaultText = lib.literalExpression "the xdg-desktop-portal-umbriel flake's package";
      description = ''
        The xdg-desktop-portal-umbriel package to install.
      '';
    };
  };

  config = lib.mkIf cfg.enable (
    lib.mkMerge [
      {
        hardware.graphics.enable = lib.mkDefault true;

        assertions = [
          {
            assertion = cfg.package != null;
            message = "programs.umbriel.package cannot be null when programs.umbriel.enable is true";
          }
        ];
      }

      (lib.mkIf (cfg.package != null) {
        environment.systemPackages = [
          cfg.package
          # So `just debug` / meson outside `nix develop` can find headers via pkg-config.
          pkgs.tomlplusplus
        ];

        # Required for greetd / noctalia-greeter to discover the session (Name=Umbriel).
        # Plain systemPackages .desktop files are not enough; NixOS aggregates via this.
        services.displayManager.sessionPackages = [ cfg.package ];

        systemd.packages = [ cfg.package ];
        systemd.user.services.umbriel = {
          restartIfChanged = false;
          enableDefaultPath = false;
        };
      })

      (lib.mkIf (cfg.portalPackage != null) {
        xdg.portal = {
          enable = lib.mkDefault true;
          extraPortals = [ cfg.portalPackage ];
          config.umbriel.default = lib.mkDefault [
            "umbriel"
            "gtk"
          ];
        };
      })

      (import "${modulesPath}/programs/wayland/wayland-session.nix" {
        inherit lib pkgs;
        enableXWayland = false;
        enableWlrPortal = false;
      })
    ]
  );
}
