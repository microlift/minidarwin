# /usr/bin/dscl and /usr/sbin/dseditgroup for the rootfs: the local node only,
# kept as dslocal property lists. Links only MiniDarwin's own libxml2.
{ lib, mkDarwinPackage, toolchain, libxml2 }:

let
  # The system's account lookups are Libinfo's, which only the runtime tree
  # has (libsystem/absent-members.nix), as for su.
  allowUndefined = lib.genAttrs [ "_getpwnam" "_getpwuid" "_getgrnam" "_getgrgid" ] (_: "system_info");
in
mkDarwinPackage {
  pname = "minidarwin-dscl";
  version = "0.1.0";
  src = ./.;
  inherit toolchain;
  passthru = { inherit allowUndefined; };

  buildPhase = ''
    runHook preBuild
    make CPPFLAGS="-I${libxml2}/usr/include/libxml2" LDFLAGS="-L${libxml2}/usr/lib ${lib.concatMapStringsSep " " (s: "-Wl,-U,${s}") (lib.attrNames allowUndefined)}"
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 dscl $out/usr/bin/dscl
    install -Dm755 dseditgroup $out/usr/sbin/dseditgroup
    install -Dm644 README.md $out/usr/share/doc/dscl/README.md
    for binary in $out/usr/bin/dscl $out/usr/sbin/dseditgroup; do
      md_verify_pure "$binary"
      md_verify_signed "$binary"
      deps=$($OTOOL -L "$binary" | tail -n +2 | awk '{ print $1 }' | sort)
      expected=$(printf '%s\n' /usr/lib/libSystem.B.dylib \
        "$($OTOOL -D ${libxml2}/usr/lib/libxml2.dylib | tail -n 1)" | sort)
      if [ "$deps" != "$expected" ]; then
        echo "dscl: unexpected load commands in $binary: $deps" >&2
        exit 1
      fi
    done
    runHook postInstall
  '';

  meta.description = "dscl and dseditgroup over the local dslocal node";
}
