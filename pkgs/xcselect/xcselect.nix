# /usr/lib/libxcselect.dylib, xcode-select, xcrun, the /usr/bin tool shims
# and xcselect-install (docs/xcselect.md). libxcselect links only
# libSystem, as macOS's does; the installer links libxml2, zlib and LibreSSL.
{ lib, mkDarwinPackage, toolchain, libxml2, zlib, libressl }:

let
  shims = import ./shims.nix;
  # Loading libxcrun from the developer directory is libdyld's.
  libraryHoles = lib.genAttrs [ "_dlopen" "_dlsym" "_dlerror" "_dlclose" ] (_: "dyld");
  # The installer's posix_spawn passes it on (libdyld defines it).
  installerHoles = { "_environ" = "dyld"; };
  allowUndefined = libraryHoles // installerHoles;
  holes = attrs: lib.concatMapStringsSep " " (s: "-Wl,-U,${s}") (lib.attrNames attrs);
  exports = [
    "_xcselect_bundle_is_developer_tool" "_xcselect_developer_dir_matches_path"
    "_xcselect_find_developer_contents_from_path" "_xcselect_get_developer_dir_path"
    "_xcselect_get_manpaths" "_xcselect_get_version" "_xcselect_host_sdk_path"
    "_xcselect_invoke_xcrun" "_xcselect_manpaths_free"
    "_xcselect_manpaths_get_num_paths" "_xcselect_manpaths_get_path"
    "_xcselect_trigger_install_request"
  ];
  cflags = "-std=gnu11 -O2 -fblocks -fvisibility=hidden -Wall -Wextra -Werror";
in

mkDarwinPackage {
  pname = "xcselect";
  version = "2418";
  src = ./.;
  inherit toolchain;
  passthru = { inherit allowUndefined shims; };

  buildPhase = ''
    runHook preBuild
    $CC ${cflags} -dynamiclib -install_name /usr/lib/libxcselect.dylib \
      -compatibility_version 1.0.0 -current_version 1.0.0 \
      libxcselect.c -o libxcselect.dylib ${holes libraryHoles}
    $CC ${cflags} xcode-select.c -o xcode-select -L. -lxcselect
    $CC ${cflags} -DXCRUN shim.c -o xcrun -L. -lxcselect
    $CC ${cflags} -DTOOL_FLAGS=0 shim.c -o tool-shim -L. -lxcselect
    $CC ${cflags} -DTOOL_FLAGS=XCSELECT_FLAG_REQUIRE_XCODE shim.c -o xtool-shim -L. -lxcselect
    $CC ${cflags} -I${libxml2}/usr/include/libxml2 -I${zlib}/usr/include \
      -I${libressl}/usr/local/libressl/include xcselect-install.c -o xcselect-install \
      -L${libxml2}/usr/lib -L${zlib}/usr/lib -L${libressl}/usr/lib \
      -lxml2 -lz -lcrypto ${holes installerHoles}
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 libxcselect.dylib $out/usr/lib/libxcselect.dylib
    install -Dm644 xcselect.h $out/usr/include/xcselect.h
    install -Dm755 xcode-select $out/usr/bin/xcode-select
    install -Dm755 xcrun $out/usr/bin/xcrun
    install -Dm755 tool-shim $out/usr/libexec/xcselect/tool-shim
    install -Dm755 xtool-shim $out/usr/libexec/xcselect/xtool-shim
    install -Dm755 xcselect-install $out/usr/libexec/xcselect-install
    ${lib.concatMapStrings (n: "ln -s ../libexec/xcselect/tool-shim $out/usr/bin/${lib.escapeShellArg n}\n") shims.public}
    ${lib.concatMapStrings (n: "ln -s ../libexec/xcselect/xtool-shim $out/usr/bin/${lib.escapeShellArg n}\n") shims.requireXcode}
    install -Dm644 xcode-select.1 $out/usr/share/man/man1/xcode-select.1
    install -Dm644 README.md $out/usr/share/doc/xcselect/README.md
    install -Dm644 ${../../LICENSE} $out/usr/share/licenses/xcselect/LICENSE
    # Where xcode-select --switch writes its links, as on macOS.
    mkdir -p $out/private/var/select $out/private/var/db

    exported=$($NM -gU $out/usr/lib/libxcselect.dylib | awk '{ print $3 }' | sort | tr '\n' ' ')
    expected=$(printf '%s\n' ${lib.concatStringsSep " " exports} | sort | tr '\n' ' ')
    if [ "$exported" != "$expected" ]; then
      echo "xcselect: libxcselect exports $exported, not $expected" >&2
      exit 1
    fi
    check() {
      binary=$1; shift
      md_verify_pure "$binary"
      md_verify_signed "$binary"
      deps=$($OTOOL -L "$binary" | tail -n +2 | awk '{ print $1 }' | grep -v "^$($OTOOL -D "$binary" | tail -n +2)$" | sort | tr '\n' ' ')
      want=$(printf '%s\n' /usr/lib/libSystem.B.dylib "$@" | sort | tr '\n' ' ')
      if [ "$deps" != "$want" ]; then
        echo "xcselect: $binary loads $deps, not $want" >&2
        exit 1
      fi
    }
    check $out/usr/lib/libxcselect.dylib
    for tool in usr/bin/xcode-select usr/bin/xcrun usr/libexec/xcselect/tool-shim usr/libexec/xcselect/xtool-shim; do
      check $out/$tool /usr/lib/libxcselect.dylib
      $OTOOL -l $out/$tool | grep -q 'sectname __xcrun_shim' || [ $tool = usr/bin/xcode-select ] || {
        echo "xcselect: $tool has no __DATA,__xcrun_shim" >&2; exit 1; }
    done
    check $out/usr/libexec/xcselect-install \
      "$($OTOOL -D ${libxml2}/usr/lib/libxml2.dylib | tail -n 1)" \
      "$($OTOOL -D ${zlib}/usr/lib/libz.dylib | tail -n 1)" \
      "$($OTOOL -D ${libressl}/usr/lib/libcrypto.dylib | tail -n 1)"
    runHook postInstall
  '';

  meta.description = "xcode-select, xcrun, libxcselect and the developer tool shims";
}
