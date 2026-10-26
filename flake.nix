{
  description = "Rekindle: crash recovery for Nightreign runs";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { nixpkgs, ... }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      rekindle = import ./package.nix {
        inherit pkgs;
        src = ./.;
      };
      toolchain = [
        pkgs.pkgsCross.mingwW64.buildPackages.gcc
        pkgs.gnumake
      ];
      # Builds rekindle.dll from the working tree in the current directory.
      build = pkgs.writeShellApplication {
        name = "rekindle-build";
        runtimeInputs = toolchain;
        text = ''
          if [ ! -f Makefile ] || [ ! -d snapshot ]; then
            echo "Run this from the Rekindle source folder." >&2
            exit 1
          fi
          # The bare MinGW wrapper lacks the mcfgthread link path that stdenv adds.
          make CC="x86_64-w64-mingw32-gcc -L${pkgs.pkgsCross.mingwW64.windows.mcfgthreads}/lib" "$@"
        '';
      };
    in
    {
      packages.${system} = {
        inherit rekindle;
        default = rekindle;
      };

      apps.${system}.default = {
        type = "app";
        program = "${build}/bin/rekindle-build";
      };

      # `make` inside builds rekindle.dll with the MinGW cross compiler.
      devShells.${system}.default = pkgs.mkShell { packages = toolchain; };
    };
}
