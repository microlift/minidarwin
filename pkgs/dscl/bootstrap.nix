# The same sources built in the host world, for the behavioural tests:
# target binaries are never run during a build.
{ stdenv, libxml2 }:

stdenv.mkDerivation {
  pname = "dscl-bootstrap";
  version = "0.1.0";
  src = ./.;
  buildInputs = [ libxml2 ];
  makeFlags = [ "CPPFLAGS=-I${libxml2.dev}/include/libxml2" ];

  installPhase = ''
    runHook preInstall
    install -Dm755 dscl $out/bin/dscl
    install -Dm755 dseditgroup $out/bin/dseditgroup
    runHook postInstall
  '';

  meta.description = "Host build of dscl and dseditgroup, for tests";
}
