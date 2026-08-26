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
          # this is required by finix to be able to run umbriel as session
          (lib.hiPrio (
            pkgs.writeTextDir "share/wayland-sessions/umbriel.desktop" ''
              [Desktop Entry]
              Encoding=UTF-8
              Name=Umbriel
              DesktopNames=umbriel;wlroots
              Comment=Umbrel, a Wayland compositor built on wlroots and SceneFX
              Exec=${lib.getExe' pkgs.dbus "dbus-run-session"} -- ${lib.getExe' cfg.package "start-umbriel"}
              Type=Application
            ''
          ))
        ];
      })

      (lib.mkIf (cfg.portalPackage != null) {
        xdg.portal = {
          enable = lib.mkDefault true;
          # finix follows config.xdg.portals = []; logic
          # for extraPortals from nixos
          portals = [ cfg.portalPackage ];
          config.umbriel.default = lib.mkDefault [
            "umbriel"
            "gtk"
          ];
        };
      })
    ]
  );
}
