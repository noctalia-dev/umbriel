#!/usr/bin/env bash
# Installs the completion scripts the binary generates into the directories their shells read. Meson runs this only
# for a native build, so a cross build installs none.
set -euo pipefail

binary=$1
datadir=$2

if [[ $datadir == /* ]]; then
  dest=${DESTDIR:-}$datadir
else
  dest=$MESON_INSTALL_DESTDIR_PREFIX/$datadir
fi

mkdir -p "$dest/bash-completion/completions" "$dest/zsh/site-functions" "$dest/fish/vendor_completions.d"
"$binary" completions bash >"$dest/bash-completion/completions/umbriel"
"$binary" completions zsh >"$dest/zsh/site-functions/_umbriel"
"$binary" completions fish >"$dest/fish/vendor_completions.d/umbriel.fish"
