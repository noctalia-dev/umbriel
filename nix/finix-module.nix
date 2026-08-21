{
  config,
  pkgs,
  lib,
  ...
}:
let
  tomlFormat = pkgs.formats.toml { };

  cfg = config.programs.umbriel;

  configFile = tomlFormat.generate "umbriel-config.toml" cfg.config;
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

  # not copying nixos module - taking module from upstreaming finix
  # aka niri, labwc, sway, mango

  config = lib.mkIf cfg.enable {
    environment.systemPackages = [
      cfg.package
      pkgs.tomlplusplus

      (
        lib.hiPrio (
          pkgs.writeTextDir  "share/wayland-sessions/umbriel.desktop" ''
            [Desktop Entry]
            Encoding=UTF-8
            Name=Umbriel
            DesktopNames=umbriel;wlroots
            Comment=Umbrel, a Wayland compositor built on wlroots and SceneFX
            Exec=${lib.getExe' pkgs.dbus "dbus-run-session"} -- ${lib.getExe' cfg.package "start-umbriel"}
            Type=Application
          ''
        )    
      )
    ];

    environment.etc."xdg/umbriel/config.toml".source =
      if cfg.configFile != null
      then cfg.configFile
      else configFile;
  };
}  
