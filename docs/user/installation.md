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

```sh
sudo apt update
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
and fish. Environment variables from that profile are available to Umbriel and
applications started in the session. Interactive shell files such as
`~/.zshrc` are not loaded. In a systemd-managed session, `PATH` remains the
value supplied by the user manager, including `environment.d`; the direct
fallback inherits `PATH` from the login profile like the other variables.

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

When standard output or standard error is connected to a TTY, raw writes from
Umbriel and its child processes are redirected to
`$XDG_CACHE_HOME/umbriel/umbriel-stderr.log`, or
`~/.cache/umbriel/umbriel-stderr.log` when `XDG_CACHE_HOME` is unset.
