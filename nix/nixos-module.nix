{ xdg-desktop-portal-umbriel }:
{
  config,
  lib,
  pkgs,
  ...
}:
let
  cfg = config.programs.umbriel;
in
{
  # Disable the nixpkgs module to avoid conflicts
  disabledModules = [ "programs/wayland/umbriel.nix" ];

  options.programs.umbriel = {
    enable = lib.mkEnableOption "Umbriel, a Wayland compositor built on wlroots.";

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
        programs.dconf.enable = lib.mkDefault true;
        services.graphical-desktop.enable = lib.mkDefault true;
        security.polkit.enable = lib.mkDefault true;

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
        environment.pathsToLink = [ "/share/umbriel/effects" ];

        # Required for greetd / noctalia-greeter to discover the session (Name=Umbriel).
        # Plain systemPackages .desktop files are not enough; NixOS aggregates via this.
        services.displayManager.sessionPackages = [ cfg.package ];

        systemd.packages = [ cfg.package ];
        systemd.user.services.umbriel = {
          restartIfChanged = false;
          enableDefaultPath = false;
        };
      })

      {
        xdg.portal = {
          enable = lib.mkDefault (cfg.portalPackage != null);
          configPackages = lib.mkIf (cfg.portalPackage != null) [ cfg.portalPackage ];
          extraPortals = [
            pkgs.xdg-desktop-portal-gtk
          ]
          ++ lib.optional (cfg.portalPackage != null) cfg.portalPackage;
        };
      }
    ]
  );
}
