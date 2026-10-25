{ pkgs ? import <nixpkgs> { } }:
pkgs.mkShell {
  # the headers and libraries darktable builds against, for compiling
  # lightroom.c into the harness and linking it to libdarktable
  inputsFrom = [ pkgs.darktable ];
  packages = [
    pkgs.libarchive
    pkgs.llvmPackages.clang
    pkgs.llvmPackages.compiler-rt
  ];
}
