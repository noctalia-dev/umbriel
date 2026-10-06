{
  pkgs,
  umbriel,
}:
pkgs.mkShell {
  inputsFrom = [ umbriel ];

  # Keep the packaged helper optional while making its protocol/reconnect tests
  # available to developers through -Daudio_helper=enabled.
  buildInputs = [ pkgs.pipewire ];

  # glibc rejects _FORTIFY_SOURCE at the debug profile's -O0, and werror
  # promotes that diagnostic to a build failure.
  hardeningDisable = [
    "fortify"
    "fortify3"
  ];

  nativeBuildInputs = with pkgs; [
    just
    lefthook
    meson
    ninja
    pkg-config
    wayland-scanner
    llvmPackages_22.clang-tools
    llvmPackages_22.libclang
    gnugrep
    gnused
    findutils
    gdb
    grim
    jq
    foot
    python3
    imagemagick
    procps
    xwayland
  ];

  shellHook = ''
    echo " Umbriel dev-shell | 'just --list' to see available tasks"
  '';
}
