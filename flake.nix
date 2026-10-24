{
  description = "Turing Screen — native USB smart display editor and system monitor";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forSystems = nixpkgs.lib.genAttrs systems;
    in
    {
      nixosModules = {
        default = import ./nix/nixos-module.nix;
        turing-screen = self.nixosModules.default;
      };

      packages = forSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
          package = pkgs.callPackage ./nix/package.nix { };
        in
        {
          default = package;
          turing-screen = package;
        }
      );

      apps = forSystems (system: {
        default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/turing-screen-qt";
          meta.description = "Turing Screen smart display editor";
        };
      });

      checks = forSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = self.packages.${system}.default;
          nixos-module = import ./nix/check-module.nix {
            inherit pkgs;
            inherit (nixpkgs.lib) nixosSystem;
            module = self.nixosModules.default;
          };
        }
      );

      devShells = forSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.default ];
            packages = [
              pkgs.nixfmt
              pkgs.python3Packages.fonttools
            ];
            shellHook = ''
              export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}:${pkgs.qt6.qtwayland}/${pkgs.qt6.qtbase.qtPluginPrefix}''${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
            '';
          };
        }
      );
    };
}
