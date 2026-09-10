# Installing Umbriel

Umbriel is available for Arch Linux, Fedora, Debian, and Ubuntu. You can also
[build it manually](#manual-build) on another Linux distribution.

> Package ownership: the Umbriel team maintains the manual build instructions.
> Distribution packages are maintained by their distributions or package
> repository maintainers. Review third-party repositories before installing
> from them.

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

[home:neifua:Noctalia](https://build.opensuse.org/project/show/home:neifua:Noctalia) repo provides [umbriel-git](https://build.opensuse.org/package/show/home:neifua:Noctalia/umbriel-git) on OBS.

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

Installed display-manager sessions use `start-umbriel`. For supported account
shells listed in `/etc/shells`, including bash, zsh, and fish, the launcher
enters the configured shell as a noninteractive login shell before starting a
native session. Exports from its login profile, such as `~/.zprofile` for zsh,
are inherited by Umbriel and the session. Interactive startup files such as
`~/.zshrc` are not read. Starting `start-umbriel` from a TTY performs this step
even if the TTY login already loaded the profile.

When a systemd user manager is available, the launcher imports that environment
and runs Umbriel as a user service. This also includes variables from
`environment.d`. On other init systems it starts the compositor directly with
the login environment.

Run `umbriel` directly for nested development sessions or explicit unmanaged
startup.

From a TTY, start a normal installed session with:

```sh
start-umbriel
```

## Logs

Umbriel writes its main log to `$XDG_CACHE_HOME/umbriel/umbriel.log`. If
`XDG_CACHE_HOME` is unset, the fallback path is
`~/.cache/umbriel/umbriel.log`. The previous file is retained as
`umbriel.log.1` when the current log reaches 1 MiB.

When standard output or standard error is connected to a TTY, raw writes from
Umbriel and its child processes are redirected to
`$XDG_CACHE_HOME/umbriel/umbriel-stderr.log`, or
`~/.cache/umbriel/umbriel-stderr.log` when `XDG_CACHE_HOME` is unset.
