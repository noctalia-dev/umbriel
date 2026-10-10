{
  lib,
  stdenv,
  meson,
  ninja,
  pkg-config,
  wayland-scanner,
  wayland,
  wayland-protocols,
  wlroots_0_20,
  libxkbcommon,
  libinput,
  systemd,
  pixman,
  cairo,
  pango,
  libGL,
  libdrm,
  libdisplay-info,
  libgbm,
  libxcb,
  libxcb-wm,
  lcms2,
  jemalloc,
  pipewire,
  tomlplusplus,
  nlohmann_json,
  xwayland,
  makeBinaryWrapper,
  rev ? "unknown",
}:
let
  version = lib.trim (builtins.readFile ../VERSION);
in
stdenv.mkDerivation {
  pname = "umbriel";
  inherit version;

  src = ../.;

  nativeBuildInputs = [
    makeBinaryWrapper
    meson
    ninja
    pkg-config
    wayland-scanner
  ];

  buildInputs = [
    wayland
    wayland-protocols
    wlroots_0_20
    libxkbcommon
    libinput
    # Supplies libudev for the optional native DRM policy support.
    systemd
    pixman
    tomlplusplus
    libGL
    nlohmann_json
    libdrm
    libdisplay-info
    libgbm
    libxcb
    libxcb-wm
    lcms2
    jemalloc
    pipewire
    cairo
    pango
  ];

  mesonBuildType = "release";

  mesonFlags = [
    (lib.mesonEnable "tests" false)
    (lib.mesonEnable "audio_helper" true)
  ];

  postPatch = ''
    substituteInPlace meson.build \
      --replace-fail "umbriel_git_revision_config.set('VCS_TAG', 'unknown')" "umbriel_git_revision_config.set('VCS_TAG', '${rev}')"
  '';

  postInstall = ''
    if [ -f "$out/share/wayland-sessions/umbriel.desktop" ]; then
      substituteInPlace "$out/share/wayland-sessions/umbriel.desktop" \
        --replace-fail 'Exec=start-umbriel' "Exec=$out/bin/start-umbriel"
    fi
    wrapProgram $out/bin/umbriel \
      --prefix PATH : ${lib.makeBinPath [ xwayland ]} \
  '';

  passthru.providedSessions = [ "umbriel" ];

  meta = with lib; {
    description = "A Wayland compositor built on wlroots";
    homepage = "https://github.com/noctalia-dev/umbriel";
    license = licenses.mit;
    platforms = platforms.linux;
    mainProgram = "umbriel";
  };
}
