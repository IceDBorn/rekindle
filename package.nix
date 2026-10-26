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
    $CC -O2 -Wall -shared -static -s -o rekindle.dll rekindle.c -lbcrypt -lws2_32
    $CC -O2 -Wall -shared -static -s -o rekindle_snapshot.dll rekindle_snapshot.c
  '';
  installPhase = ''
    mkdir -p $out
    cp rekindle.dll rekindle_snapshot.dll $out/
  '';
}
