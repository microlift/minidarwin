# The MiniDarwin SDK: what a compiler on *another* machine needs to build
# binaries that run on a MiniDarwin system -- its headers, and a text stub
# (.tbd) for every dylib the rootfs ships -- with none of the system's code.
#
# It is a sysroot in the shape of an Xcode SDK (`-isysroot`/`--sysroot`, or
# SDKROOT for tools that read one): link against it and the load commands,
# install names and imports are exactly those of linking against the rootfs
# itself, because each stub is made from the rootfs's own dylib and checked
# against it. Released like the rootfs (docs/rootfs-spec.md, "SDK releases").
#
# What is in it, and from where:
#   * headers  -- sdkStage4's usr/include (the stage 1 SDK, libc++'s c++/v1,
#                 libunwind) and System.framework's PrivateHeaders, plus every
#                 file under an `include` directory of the rootfs (zlib,
#                 libedit, ncurses, ICU, libxml2, LibreSSL, the /compat
#                 OpenSSL, ...), and every rootfs member's passthru.headers
#                 (libedit, ncurses, libmd, ...: what Apple's projects install
#                 to /usr/include, which the rootfs itself does not carry).
#                 A path two inputs provide must be byte-identical.
#   * stubs    -- every MH_DYLIB in the rootfs, at its own path with .dylib
#                 replaced by .tbd (TAPI v4: what ld64 and lld both read,
#                 old ld64 included), each checked against its dylib by
#                 scripts/tbd-compare.py, and
#                 every *.dylib symlink as the matching *.tbd symlink, so
#                 -lc, -lcurses, libSystem's reexports, ... resolve as on the
#                 system. The rootfs's static archives and pkg-config files
#                 are copied as they are.
#   * SDKSettings.json, which clang and swift-driver read for the SDK version.
#
# Deliberately left out: sdkStage4's libc++.a/libc++abi.a/libunwind.a (the
# toolchain's, not the system's: nothing in the rootfs links them, and
# -lc++ must find the stub); usr/local/internal_hdr (xnu's build-internal
# headers, which no consumer of the system should see); executables, data and
# everything else in the rootfs. No Swift module is shipped yet: the stdlib's
# Swift.swiftmodule is not installed anywhere (see CLAUDE.md, "The Swift runtime").
#
# Host world: nothing is compiled, and llvm-readtapi only reads the Mach-Os.
{ lib
, stdenvNoCC
, llvmPackages
, python3
, sdkStage4
, rootfs
, targetArch
, minOS
}:

let
  llvm = llvmPackages.llvm;
  # Members whose headers are a derivation of their own (not a list of names).
  headerPkgs = map (m: m.headers)
    (lib.filter (m: m ? headers && lib.isDerivation m.headers) rootfs.members);

  machoArch = if targetArch == "aarch64" then "arm64" else targetArch;

  # The subset clang (DarwinSDKInfo) and swift-driver read, plus what tools
  # that look for an Xcode SDK use to tell which one it is. Written by
  # python's json module so it is canonical.
  sdkSettings = {
    CanonicalName = "macosx${minOS}";
    DisplayName = "MiniDarwin ${minOS}";
    Version = minOS;
    MaximumDeploymentTarget = "${minOS}.99";
    DefaultDeploymentTarget = minOS;
    IsBaseSDK = "YES";
    DefaultProperties = {
      PLATFORM_NAME = "macosx";
    };
    SupportedTargets = {
      macosx = {
        Archs = [ machoArch ];
        LLVMTargetTripleSys = "macos";
        LLVMTargetTripleVendor = "apple";
        DefaultDeploymentTarget = minOS;
        MinimumDeploymentTarget = minOS;
        MaximumDeploymentTarget = "${minOS}.99";
        PlatformFamilyName = "macOS";
      };
    };
  };
in

stdenvNoCC.mkDerivation {
  pname = "minidarwin-sdk";
  version = minOS;

  dontUnpack = true;
  dontFixup = true;

  nativeBuildInputs = [ llvm python3 ];

  SOURCE_DATE_EPOCH = "1";
  TZ = "UTC";
  LC_ALL = "C";

  passAsFile = [ "sdkSettingsJson" ];
  sdkSettingsJson = builtins.toJSON sdkSettings;

  installPhase = ''
    runHook preInstall
    set -eo pipefail
    mkdir -p $out

    die() { echo "minidarwin-sdk: $*" >&2; exit 1; }

    # Merge one file in; a path two inputs provide must be the same bytes.
    put() { # src rel
      if [ -e "$out/$2" ] || [ -L "$out/$2" ]; then
        cmp -s "$1" "$out/$2" || die "$2 differs between the SDK and the rootfs"
        return
      fi
      mkdir -p "$(dirname "$out/$2")"
      cp "$1" "$out/$2"
    }
    putlink() { # target rel
      if [ -L "$out/$2" ]; then
        [ "$(readlink "$out/$2")" = "$1" ] || die "$2: two inputs disagree on its target"
        return
      fi
      [ ! -e "$out/$2" ] || die "$2 is a link in one input and a file in another"
      mkdir -p "$(dirname "$out/$2")"
      ln -s "$1" "$out/$2"
    }
    is_dylib() { # MH_MAGIC_64, filetype MH_DYLIB
      [ "$(od -An -tx1 -N4 "$1" | tr -d ' \n')" = cffaedfe ] &&
        [ "$(od -An -tu4 -j12 -N4 "$1" | tr -d ' \n')" = 6 ]
    }

    # 1. The compiler-facing headers. Copied whole: c++/v1 is thousands of files.
    mkdir -p $out/usr $out/System/Library/Frameworks
    cp -R ${sdkStage4}/usr/include $out/usr/include
    cp -R ${sdkStage4}/System/Library/Frameworks/System.framework \
      $out/System/Library/Frameworks/
    chmod -R u+w $out
    ls ${sdkStage4} | grep -qvx -e System -e usr &&
      die "sdkStage4 grew a top-level directory this SDK does not know about"

    # 2. The members' own header packages.
    for h in ${lib.escapeShellArgs headerPkgs}; do
      cd "$h"
      while IFS= read -r -d "" p; do
        rel="''${p#./}"
        if [ -L "$p" ]; then putlink "$(readlink "$p")" "$rel"
        elif [ -f "$p" ]; then put "$p" "$rel"; fi
      done < <(find . -mindepth 1 -print0 | sort -z)
    done

    # 3. The rootfs: headers, stubs, archives, pkg-config.
    cd ${rootfs}
    ndylibs=0
    while IFS= read -r -d "" p; do
      rel="''${p#./}"
      [ -d "$p" ] && [ ! -L "$p" ] && continue
      case "$rel" in
        include/*|*/include/*)
          if [ -L "$p" ]; then putlink "$(readlink "$p")" "$rel"; else put "$p" "$rel"; fi
          continue ;;
      esac
      if [ -L "$p" ]; then
        case "$rel" in
          *.dylib)
            t=$(readlink "$p")
            case "$t" in *.dylib) ;; *) die "$rel -> $t: a dylib link to a non-dylib name" ;; esac
            putlink "''${t%.dylib}.tbd" "''${rel%.dylib}.tbd" ;;
        esac
        continue
      fi
      [ -f "$p" ] || continue
      if is_dylib "$p"; then
        case "$rel" in *.dylib) ;; *) die "$rel: a dylib not named *.dylib" ;; esac
        stub="$out/''${rel%.dylib}.tbd"
        mkdir -p "$(dirname "$stub")"
        llvm-readtapi -stubify --filetype=tbd-v4 "$p" -o "$stub"
        # The stub must describe the dylib: same install name, versions,
        # reexports, flags and exports -- all but what v4 cannot record.
        python3 ${../../scripts/tbd-compare.py} ${llvm}/bin/llvm-readtapi "$p" "$stub" >/dev/null ||
          die "$rel: the stub does not match the dylib"
        ndylibs=$((ndylibs + 1))
        continue
      fi
      case "$rel" in
        *.a|*/lib/pkgconfig/*.pc) put "$p" "$rel" ;;
      esac
    done < <(find . -mindepth 1 -print0 | sort -z)
    cd - >/dev/null

    # Every dylib link must land on a stub (a link to a link is fine).
    while IFS= read -r l; do
      [ -e "$l" ] || die "''${l#$out/} -> $(readlink "$l") resolves to nothing"
    done < <(find $out -type l -name '*.tbd')

    python3 -c 'import json, sys; json.dump(json.load(open(sys.argv[1])), open(sys.argv[2], "w"), indent=2, sort_keys=True); open(sys.argv[2], "a").write("\n")' \
      "$sdkSettingsJsonPath" $out/SDKSettings.json

    # The canonical modes the release format describes (nothing executable).
    find $out -type d -exec chmod 755 {} +
    find $out -type f -exec chmod 644 {} +

    for f in usr/include/stdio.h usr/include/c++/v1/vector usr/include/zlib.h \
      usr/include/histedit.h usr/include/curses.h \
      usr/lib/libSystem.B.tbd usr/lib/libSystem.tbd usr/lib/system/libsystem_c.tbd \
      usr/lib/libc++.tbd usr/lib/libc++abi.tbd usr/lib/swift/libswiftCore.tbd; do
      [ -e "$out/$f" ] || die "missing $f"
    done
    [ -z "$(find $out -name '*.dylib')" ] || die "a dylib leaked into the SDK"
    [ ! -e $out/usr/local/internal_hdr ] || die "xnu's internal headers leaked into the SDK"
    echo "[minidarwin] sdk: $ndylibs stubs, $(find $out -name '*.h' | wc -l | tr -d ' ') headers" >&2

    runHook postInstall
  '';

  passthru = {
    inherit rootfs headerPkgs;
    settings = sdkSettings;
  };

  meta.description = "minidarwin SDK: headers and .tbd stubs for building against a minidarwin system from elsewhere";
}
