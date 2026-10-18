# Installing Umbriel

Umbriel is packaged for several Linux distributions. Prefer a distribution
package when one is available; it provides the simplest installation and
upgrade path.

> Distribution packages and third-party repositories are maintained by their
> respective maintainers. Review a repository before installing from it.

## Arch Linux

[`umbriel-git`](https://aur.archlinux.org/packages/umbriel-git) is available in
the AUR:

```sh
yay -S umbriel-git
```

## Fedora

[Terra](https://wiki.fyralabs.com/Terra) provides nightly builds:

```sh
sudo dnf install umbriel-nightly
```

## openSUSE

The [Noctalia OBS repository](https://build.opensuse.org/project/show/home:neifua:Noctalia)
provides
[`umbriel-git`](https://build.opensuse.org/package/show/home:neifua:Noctalia/umbriel-git).

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
```

### Install Umbriel

Choose the command matching your distribution:

```sh
sudo apt update

# Debian Trixie
sudo apt install -t trixie-backports umbriel

# Debian Sid and Ubuntu 26.04
sudo apt install umbriel
```

The repository provides `amd64` and `arm64` packages only.

## GNU Guix

Umbriel and its XDG portal are available through the third-party
[`midnight`](https://codeberg.org/stampede/midnight) Guix channel. Add the
channel to `~/.config/guix/channels.scm`:

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

Run `guix pull`, then install `umbriel` and
`xdg-desktop-portal-umbriel`. Adding them to the system configuration makes the
Umbriel session available to display managers.

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

The module installs Umbriel, registers its display-manager session, enables
the required graphics support, and installs and configures
`xdg-desktop-portal-umbriel` for screen capture and sharing. Set
`programs.umbriel.portalPackage = null` only when another portal setup should
provide those services.

Rebuild the system, then select Umbriel in the display manager or start it from
a TTY with `start-umbriel`.

### Binary cache

Pre-built `x86_64-linux` and `aarch64-linux` packages for Umbriel and its portal
are available from [Cachix](https://app.cachix.org/cache/umbriel). Add the cache
to the NixOS configuration to avoid compiling them locally:

```nix
{
  nix.settings = {
    extra-substituters = [ "https://umbriel.cachix.org" ];
    extra-trusted-public-keys = [
      "umbriel.cachix.org-1:JfNq/2yg2S6D6z4Z2dVSZrZlDPQTKtexB6GAVLD98nw="
    ];
  };
}
```

Umbriel must use the locked `nixpkgs` input it was built against for its store
paths to match the cache. Do not set `inputs.umbriel.inputs.nixpkgs.follows`
when using the binary cache.

The [`cachix`](https://github.com/noctalia-dev/umbriel/tree/cachix) branch points
to the latest commit that CI cached successfully. Track it instead of `main` to
avoid selecting a commit whose binaries are not available yet:

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

## Manual build

Manual installations have no automatic upgrade path. Prefer a distribution
package when one is available.

Install a C++23 compiler, Meson, Ninja, `just`, `pkg-config`,
`wayland-scanner`, and the development packages listed in
[`PACKAGING.md`](https://github.com/noctalia-dev/umbriel/blob/main/PACKAGING.md#dependencies).
Then clone, build, and
install Umbriel:

```sh
git clone https://github.com/noctalia-dev/umbriel.git
cd umbriel
just release
sudo just install
```

The default installation prefix is `/usr/local`. Set `prefix` when building to
install elsewhere:

```sh
just prefix="$HOME/.local" release
just install
```

## Starting Umbriel

Installed display-manager sessions start Umbriel through `start-umbriel`.
Select Umbriel from your display manager, or start it from a TTY:

```sh
start-umbriel
```

The launcher loads the login profile for supported shells such as bash, zsh,
and fish. Environment variables from that profile, including `PATH`, are
available to Umbriel and applications started in the session. Interactive
shell files such as `~/.zshrc` are not loaded. In a systemd-managed session,
the launcher imports the login environment into the user manager before
starting Umbriel. Login-profile values take precedence over values with the
same names from `environment.d`; variables found only in the user manager
remain available. The direct fallback inherits the same login environment
without importing it.

In a managed native session, Umbriel places startup, autostart, event, and
`spawn:` commands in scopes bound to the compositor service, so they are
cleaned up when the session ends. This requires systemd 254 or newer.

Run `umbriel` directly only for a nested development session or an explicitly
unmanaged launch.

## Logs

Umbriel writes its main log to `$XDG_CACHE_HOME/umbriel/umbriel.log`. If
`XDG_CACHE_HOME` is unset, the fallback path is
`~/.cache/umbriel/umbriel.log`. The previous file is retained as
`umbriel.log.1` when the current log reaches 1 MiB.

Messages from wlroots appear with the `[wlr]` tag at info level and above. To
include wlroots debug output when reporting a rendering or display problem,
start Umbriel with `UMBRIEL_WLR_DEBUG=1` set in its environment.

When standard output or standard error is connected to a TTY, raw writes from
Umbriel and its child processes are redirected to
`$XDG_CACHE_HOME/umbriel/umbriel-stderr.log`, or
`~/.cache/umbriel/umbriel-stderr.log` when `XDG_CACHE_HOME` is unset.
