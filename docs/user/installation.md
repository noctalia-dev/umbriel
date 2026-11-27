# Installing Umbriel

Umbriel is packaged for several Linux distributions. Prefer a distribution package when one is available; it provides the simplest installation and upgrade path. Otherwise, [build it from source](building.md). Once installed, see [Getting started](getting-started.md).

> [!NOTE]
> Distribution packages and third-party repositories are maintained by their respective maintainers. Review a repository before installing from it.

## Arch Linux

[`umbriel-git`](https://aur.archlinux.org/packages/umbriel-git) is available in the AUR:

```sh
yay -S umbriel-git
```

For **Artix Linux**, you will need to edit the `PKGBUILD` to pass the `systemd-libs` flag. To do this, you'll need to add `-Dlocaled=disabled` to the meson setup arguments. If using an AUR Helper, use git commit so it builds with your change and git merges it into future upgrades.

## Fedora

[Terra](https://wiki.fyralabs.com/Terra) provides nightly builds:

```sh
sudo dnf install umbriel-nightly
```

## openSUSE

The [Noctalia OBS repository](https://build.opensuse.org/project/show/home\:neifua\:Noctalia) provides [`umbriel-git`](https://build.opensuse.org/package/show/home\:neifua\:Noctalia/umbriel-git).

#### Tumbleweed

```sh
sudo zypper addrepo --refresh --name Noctalia https://download.opensuse.org/repositories/home:neifua:Noctalia/openSUSE_Tumbleweed/home:neifua:Noctalia.repo
sudo zypper refresh && sudo zypper install umbriel-git
```

#### Slowroll

```sh
sudo zypper addrepo --refresh --name Noctalia https://download.opensuse.org/repositories/home:neifua:Noctalia/openSUSE_Slowroll/home:neifua:Noctalia.repo
sudo zypper refresh && sudo zypper install umbriel-git
```

## Debian and Ubuntu

The NickH APT repository provides Umbriel for Debian-based distributions.

### Install the repository signing key

```sh
wget https://pkg.noctalia.dev/deb/nickh-archive-keyring.deb
sudo dpkg -i nickh-archive-keyring.deb
```

### Add the repository

Choose the source matching your distribution:

```sh
# Debian Trixie
sudo wget -O /etc/apt/sources.list.d/noctalia-trixie.sources \
  https://pkg.noctalia.dev/deb/noctalia-trixie.sources

# Debian Sid
sudo wget -O /etc/apt/sources.list.d/noctalia-unstable.sources \
  https://pkg.noctalia.dev/deb/noctalia-unstable.sources

# Ubuntu 26.04
sudo wget -O /etc/apt/sources.list.d/noctalia-resolute.sources \
  https://pkg.noctalia.dev/deb/noctalia-resolute.sources

# Ubuntu 26.10
sudo wget -O /etc/apt/sources.list.d/noctalia-stonking.sources \
  https://pkg.noctalia.dev/deb/noctalia-stonking.sources
```

### Install Umbriel

Choose the command matching your distribution:

```sh
sudo apt update

# Debian Trixie
sudo apt install -t trixie-backports umbriel

# Debian Sid and Ubuntu 26.04 and 26.10
sudo apt install umbriel
```

The repository provides `amd64` and `arm64` packages only.

## GNU Guix

Umbriel and its XDG portal are available through the third-party [`midnight`](https://codeberg.org/stampede/midnight) Guix channel. Add the channel to `~/.config/guix/channels.scm`:

```scheme
(channel
  (name 'midnight)
  (url "https://codeberg.org/stampede/midnight.git")
  (branch "main")
  (introduction
    (make-channel-introduction
      "d97d1568954cfcbf543c9fcdfd5771e2b730ae19"
      (openpgp-fingerprint
        "640A 2C3C E948 22D3 394B 40C3 CAFA EECA 00FF 9B1E"))))
```

Run `guix pull`, then install `umbriel` and `xdg-desktop-portal-umbriel`. Adding them to the system configuration makes the Umbriel session available to display managers.

## NixOS

Add Umbriel to your flake inputs:

```nix
{
  inputs.umbriel.url = "github:noctalia-dev/umbriel";
}
```

Import the NixOS module and enable Umbriel in your system configuration:

```nix
{ inputs, ... }:
{
  imports = [ inputs.umbriel.nixosModules.default ];

  programs.umbriel.enable = true;
}
```

The module installs Umbriel, registers its display-manager session, enables the required graphics support, and installs and configures `xdg-desktop-portal-umbriel` for screen capture and sharing. Set `programs.umbriel.portalPackage = null` only when another portal setup should provide those services.

Rebuild the system, then select Umbriel in the display manager or start it from a TTY with `start-umbriel`.

### Home Manager and hjem

The Home Manager module (`inputs.umbriel.homeModules.default`) and the hjem module (`inputs.umbriel.hjemModules.default`) write the user configuration from Nix. `settings` is an attrset serialized to TOML; Home Manager also accepts a raw TOML string or a path:

```nix
{ inputs, ... }:
{
  imports = [ inputs.umbriel.homeModules.default ];

  programs.umbriel = {
    enable = true;
    settings = {
      general.autostart = [ "noctalia" ];
      layout.gap = 5;
      input.keyboard.layout = "de";
      keybinds = {
        "Mod+Return" = "spawn kitty";
        "Mod+Q" = "window-close";
        "Mod" = "spawn noctalia msg panel-toggle launcher";
      };
    };
  };
}
```

When `settings` is omitted, both modules leave the user configuration path untouched, so Umbriel loads its packaged configuration.

### Binary cache

Pre-built `x86_64-linux` and `aarch64-linux` packages for Umbriel and its portal are available from [Cachix](https://app.cachix.org/cache/umbriel). Add the cache to the NixOS configuration to avoid compiling them locally:

```nix
{
  nix.settings = {
    extra-substituters = [ "https://noctalia.cachix.org" ];
    extra-trusted-public-keys = [ "noctalia.cachix.org-1:pCOR47nnMEo5thcxNDtzWpOxNFQsBRglJzxWPp3dkU4=" ];
  };
}
```

Umbriel must use the locked `nixpkgs` input it was built against for its store paths to match the cache. Do not set `inputs.umbriel.inputs.nixpkgs.follows` when using the binary cache.

The [`cachix`](https://github.com/noctalia-dev/umbriel/tree/cachix) branch points to the latest commit that CI cached successfully. Track it instead of `main` to avoid selecting a commit whose binaries are not available yet:

```nix
{
  inputs.umbriel.url = "github:noctalia-dev/umbriel/cachix";
}
```

### Using bundled effects

The module links the bundled effect presets into the system profile, so they are available at a path that stays the same across rebuilds and updates:

```
/run/current-system/sw/share/umbriel/effects
```

Use it in place of `/usr/share/umbriel/effects` when including a preset:

```toml
[include]
files = [
  "/run/current-system/sw/share/umbriel/effects/border/pulse/effect.toml",
]
```

Only the `effects` directory is linked. The example `config.toml` is not, since it is of little use for a declarative setup; it is still available in the package under `share/umbriel`.

If you generate the config from Nix, you can reference the package directly instead, which doesn't depend on the module's link:

```nix
"${config.programs.umbriel.package}/share/umbriel/effects/border/pulse/effect.toml"
```

## Void Linux

Umbriel is provided by the third-party repository [`UniversalRepository`](https://universalrepo.pages.dev).

### Install repository

```bash
echo 'repository=https://universalrepo.pages.dev/void' | sudo tee /etc/xbps.d/10-universalrepository.conf
```

### Sync and import the signing key

```bash
sudo xbps-install -S
```

### Install Umbriel

```bash
sudo xbps-install -S umbriel
```
