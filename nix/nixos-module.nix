{
  config,
  pkgs,
  lib,
  xdg-desktop-portal-umbriel ? pkgs.xdg-desktop-portal-umbriel or null,
  ...
}:
let
  cfg = config.programs.umbriel;
  tomlFormat = pkgs.formats.toml { };

  generateConfig =
    format: name: value:
    if lib.isString value then
      pkgs.writeText name value
    else if builtins.isPath value || lib.isStorePath value then
      value
    else
      format.generate name value;

  generateToml = generateConfig tomlFormat;

  portals =
    lib.optionals (xdg-desktop-portal-umbriel != null) [
      xdg-desktop-portal-umbriel
    ]
    ++ [ pkgs.xdg-desktop-portal-gtk ];

  configSource =
    if cfg.configFile != null then
      cfg.configFile
    else
      let
        rawConfig = generateToml "umbriel-config.toml" cfg.config;
      in
      if cfg.validateConfig && cfg.package != null then
        pkgs.runCommand "umbriel-config" { } ''
          ${lib.getExe' cfg.package "umbriel"} validate -c ${rawConfig}
          cp ${rawConfig} $out
        ''
      else
        rawConfig;
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
      type =
        with lib.types;
        nullOr (oneOf [
          tomlFormat.type
          str
          path
        ]);
      default = null;
      description = ''
        Configuration written to {file}`/etc/xdg/umbriel/config.toml`.
        Leave null to use the configuration packaged with Umbriel.

        Can be written as:
          - A Nix attrset (converted to TOML via nixpkgs' tomlFormat)
          - A raw TOML string
          - A path to a `.toml` file

        See {file}`examples/config.toml` in the Umbriel repository for every available option.
      '';
      example = lib.literalExpression ''
        general.autostart = [ "noctalia" ];

        layout.gap = 5;

        input.keyboard.layout = "de";

        keybinds = {
          "Mod+Return" = "spawn:kitty";
          "Mod+Q" = "window-close";
          "Mod+R" = "spawn:noctalia msg panel-toggle launcher";
        };
      '';
    };

    configFile = lib.mkOption {
      type = lib.types.nullOr lib.types.path;

      default = null;
    };

    validateConfig = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Validate the configuration file at build time.";
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

    environment.etc = lib.mkIf (cfg.configFile != null || cfg.config != null) {
      "xdg/umbriel/config.toml".source = configSource;
    };
  };
}
