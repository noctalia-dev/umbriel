{
  config,
  pkgs,
  lib,
  ...
}:
let
  tomlFormat = pkgs.formats.toml { };

  cfg = config.rum.desktops.umbriel;

  configFile = tomlFormat.generate "umbriel-config.toml" cfg.config;
in
{
  options.rum.desktops.umbriel = {
    enable = lib.mkEnableOption "umbriel Wayland compositor";

    package = lib.mkPackageOption pkgs "umbriel" { };

    config = lib.mkOption {
      type = tomlFormat.type;

      default = {};
    };
  };

  config = lib.mkIf cfg.enable {
    packages = [
      cfg.package
    ];

    xdg.config.files."umbriel/config.toml".source = configFile;
  };
}
