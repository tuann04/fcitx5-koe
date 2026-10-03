{
  description = "Hold-to-talk speech-to-text for fcitx5";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
  };

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forSystems = f: nixpkgs.lib.genAttrs systems (system: f (import nixpkgs { inherit system; }));
    in
    {
      packages = forSystems (pkgs: rec {
        default = fcitx5-koe;
        fcitx5-koe = pkgs.callPackage ./package.nix { };
      });

      devShells = forSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ (pkgs.callPackage ./package.nix { }) ];
          packages = with pkgs; [
            socat
            gdb
          ];
        };
      });

      nixosModules.koe = import ./nix/module.nix;
      nixosModules.default = self.nixosModules.koe;

      overlays.default = final: prev: {
        fcitx5-koe = final.callPackage ./package.nix { };
      };
    };
}
