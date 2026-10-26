{
  pkgs,
  src,
}:

# Cross-compiles the Rekindle crash-restore natives with MinGW (imports only system DLLs).
pkgs.pkgsCross.mingwW64.stdenv.mkDerivation {
  name = "rekindle";
  inherit src;
  dontConfigure = true;
  buildPhase = ''
    make CC=$CC
  '';
  installPhase = ''
    mkdir -p $out
    cp rekindle.dll $out/
  '';
}
