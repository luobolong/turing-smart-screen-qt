{
  config,
  lib,
  pkgs,
  utils,
  ...
}:
let
  cfg = config.services.turing-screen;
in
{
  options.services.turing-screen = {
    enable = lib.mkEnableOption "Turing Screen autostart in graphical user sessions";

    package = lib.mkOption {
      type = lib.types.package;
      default = pkgs.callPackage ./package.nix { };
      defaultText = lib.literalExpression "pkgs.callPackage ./package.nix { }";
      description = "Turing Screen package to run.";
    };

    extraArgs = lib.mkOption {
      type = lib.types.listOf lib.types.str;
      default = [ ];
      example = [
        "--theme"
        "/home/user/themes/nightfall.json"
      ];
      description = "Additional command-line arguments, passed without shell expansion.";
    };

    systemd.target = lib.mkOption {
      type = lib.types.str;
      default = "graphical-session.target";
      example = "hyprland-session.target";
      description = "User session target that starts Turing Screen.";
    };
  };

  config = lib.mkIf cfg.enable {
    environment.systemPackages = [ cfg.package ];

    systemd.user.services.turing-screen = {
      description = "Turing Screen smart display monitor";
      wantedBy = [ cfg.systemd.target ];
      after = [ "graphical-session-pre.target" ];
      partOf = lib.unique [
        "graphical-session.target"
        cfg.systemd.target
      ];
      serviceConfig = {
        Type = "exec";
        ExecStart = utils.escapeSystemdExecArgs (
          [ (lib.getExe' cfg.package "turing-screen-qt") ] ++ cfg.extraArgs
        );
        Restart = "on-failure";
        RestartSec = "5s";
        TimeoutStopSec = "10s";
        Slice = "app.slice";
      };
    };
  };
}
