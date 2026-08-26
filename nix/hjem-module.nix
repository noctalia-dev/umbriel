{
  config,
  pkgs,
  lib,
  ...
}:
let
  inherit (lib.modules) mkIf;
  inherit (lib.options) mkEnableOption mkOption;
  inherit (lib.lists) optional;

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
in
{
  options.programs.umbriel = {
    enable = mkEnableOption "Umbriel, a Wayland compositor built on wlroots and SceneFX.";

    package = mkOption {
      type = lib.types.nullOr lib.types.package;
      default = null;
      description = "The umbriel package to install.";
    };
    validateConfig = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Validate the configuration file at build time.";
    };
    settings = mkOption {
      type =
        with lib.types;
        nullOr (oneOf [
          tomlFormat.type
          str
          path
        ]);

      default = null;
      description = ''
        Configuration written to {file}`$XDG_CONFIG_HOME/umbriel/config.toml`.
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
  };

  config = mkIf cfg.enable {
    packages = optional (cfg.package != null) cfg.package;

    xdg.config.files = mkIf (cfg.settings != null) {
      "umbriel/config.toml".source =
        let
          rawConfig = generateToml "umbriel-config.toml" cfg.settings;
        in
        if cfg.validateConfig && cfg.package != null then
          pkgs.runCommand "umbriel-config" { } ''
            ${lib.getExe cfg.package} validate -c ${rawConfig}
            cp ${rawConfig} $out
          ''
        else
          rawConfig;
    };
  };

  _class = "hjem";
}
