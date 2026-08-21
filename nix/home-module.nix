{
  config,
  pkgs,
  lib,
  xdg-desktop-portal-umbriel ? pkgs.xdg-desktop-portal-umbriel or null,
  ...
}:
let
  tomlFormat = pkgs.formats.toml { };

  cfg = config.wayland.windowManager.umbriel;

  configFile = tomlFormat.generate "umbriel-config.toml" cfg.config;

  portals =
    lib.optionals (xdg-desktop-portal-umbriel != null) [
      xdg-desktop-portal-umbriel
    ]
    ++ [ pkgs.xdg-desktop-portal-gtk ];
in
{
  options.wayland.windowManager.umbriel = {
    enable = lib.mkEnableOption "Umbriel, a Wayland compositor built on wlroots and SceneFX.";

    package = lib.mkOption {
      type = lib.types.nullOr lib.types.package;
      default = null;
      description = "The umbriel package to install.";
    };

    config = lib.mkOption {
      type = tomlFormat.type;

      default = {};
    };

    configFile = lib.mkOption {
      type = lib.types.nullOr lib.types.path;

      default = null;
    };

  };

  config = lib.mkIf cfg.enable {
    home.packages = [
      cfg.package
    ];

    xdg.portal = {
      enable = true;
      config = {
        common = {
          default = "*";
        };
        
        umbriel = {
          default = [ "gtk" ];
        };
      };
      
      extraPortals = portals;

      configPackages = [
        cfg.package
      ];
    };

    xdg.configFile."umbriel/config.toml".source =
      if cfg.configFile != null
      then cfg.configFile
      else configFile;
  };
}
