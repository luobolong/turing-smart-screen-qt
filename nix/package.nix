{
  lib,
  stdenv,
  cmake,
  ninja,
  pkg-config,
  qt6,
  fontconfig,
  libpulseaudio,
  copyDesktopItems,
  makeDesktopItem,
}:

stdenv.mkDerivation {
  pname = "turing-screen";
  version = "0.2.1";

  src = lib.cleanSourceWith {
    src = ../.;
    filter =
      path: type:
      lib.cleanSourceFilter path type
      && !(builtins.elem (baseNameOf path) [
        "build"
        "dist"
        "artifacts"
        ".agents"
        ".codex"
        "result"
      ]);
  };

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    qt6.wrapQtAppsHook
    copyDesktopItems
  ];
  buildInputs = [
    qt6.qtbase
    qt6.qtsvg
    qt6.qtwayland
    libpulseaudio
  ];

  cmakeFlags = [
    "-DBUILD_TESTING=ON"
    "-DSCREEN_INSTALL_BINDIR=bin"
    "-DSCREEN_INSTALL_DATADIR=share/turing-screen"
    # Avoid keeping the build source in the runtime closure.
    "-DSCREEN_ASSET_DIR=${placeholder "out"}/share/turing-screen/assets"
  ];

  doCheck = true;
  preCheck = ''
    export QT_QPA_PLATFORM=offscreen
    # Tests edit copies of bundled themes; use the writable unpacked source.
    export TURING_QT_ASSETS="$PWD/../assets"
    export XDG_CONFIG_HOME="$TMPDIR/test-config"
    export XDG_CACHE_HOME="$TMPDIR/test-cache"
    export XDG_RUNTIME_DIR="$TMPDIR/test-runtime"
    mkdir -p "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME"
    mkdir -m 700 "$XDG_RUNTIME_DIR"
    export FONTCONFIG_FILE="${fontconfig.out}/etc/fonts/fonts.conf"
  '';

  desktopItems = [
    (makeDesktopItem {
      name = "turing-screen";
      desktopName = "Turing Screen";
      comment = "System monitor and theme editor for USB smart displays";
      exec = "turing-screen-qt";
      icon = "turing-screen";
      categories = [
        "System"
        "Monitor"
      ];
      keywords = [
        "CPU"
        "GPU"
        "RAM"
        "USB"
      ];
    })
  ];

  postInstall = ''
    install -Dm644 "$src/assets/turing-screen.svg" "$out/share/icons/hicolor/scalable/apps/turing-screen.svg"
  '';

  meta = {
    description = "Native theme editor and system monitor for USB smart displays";
    license = lib.licenses.gpl3Plus;
    platforms = lib.platforms.linux;
    mainProgram = "turing-screen-qt";
  };
}
