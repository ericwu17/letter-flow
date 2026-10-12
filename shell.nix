{ pkgs ? import <nixpkgs> {} }:

let
  runtimeLibs = with pkgs; [
    libGL
    libglvnd
    xorg.libX11
    xorg.libXext
    xorg.libXcursor
    xorg.libXi
    xorg.libXrandr
    xorg.libXfixes
    libxkbcommon
    wayland
  ];
in
pkgs.mkShell {
  nativeBuildInputs = [ pkgs.pkg-config ];

  buildInputs = [ pkgs.SDL2 ] ++ runtimeLibs;

  # SDL dlopen()s these at runtime, so the linker's rpath doesn't cover them.
  shellHook = ''
    export LD_LIBRARY_PATH=${pkgs.lib.makeLibraryPath runtimeLibs}:$LD_LIBRARY_PATH
  '';
}
