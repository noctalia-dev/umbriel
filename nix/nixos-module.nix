{
  config,
  pkgs,
  lib,
  xdg-desktop-portal-umbriel ? pkgs.xdg-desktop-portal-umbriel or null,
  ...
}:
let
  tomlFormat = pkgs.formats.toml { };

  cfg = config.programs.umbriel;

  configFile = tomlFormat.generate "umbriel-config.toml" cfg.config;

  portals =
    lib.optionals (xdg-desktop-portal-umbriel != null) [
      xdg-desktop-portal-umbriel
    ]
    ++ [ pkgs.xdg-desktop-portal-gtk ];
in
{
  options.programs.umbriel = {
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
    assertions = [
      {
        assertion = cfg.package != null;
        message = "programs.umbriel.package cannot be null when programs.umbriel.enable is true";
      }
    ];
    
    environment.systemPackages = [
      cfg.package
      pkgs.tomlplusplus
    ];

    services.displayManager.sessionPackages = [
      cfg.package
    ];

    xdg.portal.extraPortals = portals;

    xdg.portal.configPackages = [
      cfg.package
    ];

    environment.etc."xdg/umbriel/config.toml".source =
      if cfg.configFile != null
      then cfg.configFile
      else configFile;
  };
}
