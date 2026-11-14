# Host build of libxcselect and its tools for the tests (Makefile): system
# paths go under $XCSELECT_TEST_ROOT. Not for installing anything.
{ stdenv, pkg-config, libxml2, zlib, openssl }:

stdenv.mkDerivation {
  pname = "xcselect-test-build";
  version = "2418";
  src = ./.;
  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ libxml2 zlib openssl ];

  installPhase = ''
    runHook preInstall
    install -Dm755 libxcselect.dylib $out/lib/libxcselect.dylib
    for tool in xcode-select xcrun tool-shim xtool-shim xcselect-install; do
      install -Dm755 $tool $out/bin/$tool
    done
    runHook postInstall
  '';

  meta.description = "Host build of libxcselect and its tools, for tests";
}
