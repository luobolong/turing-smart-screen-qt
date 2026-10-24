{
  lib,
  stdenv,
  cmake,
  ninja,
  pkg-config,
  qt6,
  libpulseaudio,
  copyDesktopItems,
  makeDesktopItem,
}:

stdenv.mkDerivation {
  pname = "turing-screen";
  version = "0.2.0";

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
    export TURING_QT_ASSETS="$src/assets"
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
