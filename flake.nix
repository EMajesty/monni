{
  description = "Arduino Mega 2560 logic monitor dev shell";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
      in {
        devShells.default = pkgs.mkShell {
          packages = [
            pkgs.arduino-cli
            pkgs.pkgsCross.avr.buildPackages.gcc
            pkgs.pkgsCross.avr.buildPackages.binutils
            pkgs.avrdude
            pkgs.python3
            pkgs.gnumake
          ];
        };
      });
}
