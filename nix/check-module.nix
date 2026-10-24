{
  pkgs,
  nixosSystem,
  module,
}:
let
  inherit (pkgs) lib;
  testPackage = pkgs.writeShellScriptBin "turing-screen-qt" "exit 0";
  evaluate =
    settings:
    nixosSystem {
      system = pkgs.stdenv.hostPlatform.system;
      modules = [
        module
        {
          system.stateVersion = "26.05";
          services.turing-screen = {
            package = testPackage;
          }
          // settings;
        }
      ];
    };
  disabled = (evaluate { }).config;
  enabled = (evaluate { enable = true; }).config;
  custom =
    (evaluate {
      enable = true;
      extraArgs = [
        "--theme"
        "/tmp/theme 100% $example.json"
      ];
      systemd.target = "turing-screen-test-session.target";
    }).config;
  service = enabled.systemd.user.services.turing-screen;
  customService = custom.systemd.user.services.turing-screen;
  unit = enabled.systemd.user.units."turing-screen.service";
  customUnit = custom.systemd.user.units."turing-screen.service";
in
assert !(disabled.systemd.user.services ? turing-screen);
assert builtins.elem testPackage enabled.environment.systemPackages;
assert service.wantedBy == [ "graphical-session.target" ];
assert service.partOf == [ "graphical-session.target" ];
assert service.serviceConfig.Type == "exec";
assert service.serviceConfig.Restart == "on-failure";
assert customService.wantedBy == [ "turing-screen-test-session.target" ];
assert builtins.elem "graphical-session.target" customService.partOf;
assert builtins.elem "turing-screen-test-session.target" customService.partOf;
assert lib.hasInfix ''"/tmp/theme 100%% $$example.json"'' customService.serviceConfig.ExecStart;
pkgs.runCommand "turing-screen-nixos-module-check" { nativeBuildInputs = [ pkgs.systemd ]; } ''
  mkdir -p "$out/default" "$out/custom"
  cp ${unit.unit}/turing-screen.service "$out/default/"
  cp ${customUnit.unit}/turing-screen.service "$out/custom/"
  export XDG_RUNTIME_DIR="$TMPDIR/runtime"
  mkdir -m 700 "$XDG_RUNTIME_DIR"
  export SYSTEMD_LOG_TARGET=console
  # Verify both session graphs, including the generated wants links, so target
  # ordering cycles are caught without contacting the host user manager.
  for scenario in default custom; do
    graph="$out/$scenario"
    for target in basic shutdown; do
      printf '[Unit]\nDescription=Test target\nDefaultDependencies=no\n' > "$graph/$target.target"
    done
    for target in graphical-session turing-screen-test-session; do
      printf '[Unit]\nDescription=Test session\nRequires=basic.target\n' > "$graph/$target.target"
    done
    printf '[Unit]\nDescription=Test session setup\nBefore=graphical-session.target\n' > "$graph/graphical-session-pre.target"
    printf '[Unit]\nDescription=Test application slice\n[Slice]\n' > "$graph/app.slice"
    printf '[Unit]\nDescription=Test root slice\n[Slice]\n' > "$graph/-.slice"
    target=graphical-session.target
    if [ "$scenario" = custom ]; then target=turing-screen-test-session.target; fi
    mkdir "$graph/$target.wants"
    ln -s ../turing-screen.service "$graph/$target.wants/turing-screen.service"
    export SYSTEMD_UNIT_PATH="$graph"
    systemd-analyze --user --man=no --generators=no verify "$graph/$target" "$graph/turing-screen.service"
  done
''
