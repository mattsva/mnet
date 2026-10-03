{
  description = "mnet development environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.11";
  };

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
    in
    {
      packages.${system}.default = pkgs.stdenv.mkDerivation {
        pname = "mnet";
        version = "0.2.0";
        src = ./.;
        nativeBuildInputs = [ pkgs.cmake ];
        meta = with pkgs.lib; {
          description = "A simple, ergonomic web framework for C";
          license = licenses.mit;
          platforms = platforms.all;
        };
      };

      devShells.${system}.default = pkgs.mkShell {
        packages = with pkgs; [
          gcc
          gnumake
          cmake
          meson
          ninja
          valgrind
          pkg-config
          git
          gh
        ];

        shellHook = ''
          echo "mnet development environment"
          echo "Available: gcc, make, cmake, meson, ninja, valgrind, pkg-config, git, gh"
          echo ""
          echo "Quick start:"
          echo "  make test                    # Build and run tests"
          echo "  mkdir build && cd build && cmake .. && make && ctest  # CMake"
          echo "  meson setup build && meson compile -C build && meson test -C build  # Meson"
          echo ""
          echo "After building, set PKG_CONFIG_PATH to use pkg-config:"
          echo "  export PKG_CONFIG_PATH=$PWD/build:$PKG_CONFIG_PATH"
        '';
      };
    };
}
