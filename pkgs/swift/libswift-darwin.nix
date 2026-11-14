# Stage 6: the C library's Swift overlays, /usr/lib/swift/libswiftDarwin.dylib
# and libswift_Builtin_float.dylib (<float.h>'s, whose symbols carry the
# Darwin ABI name as Apple's do).
#
# swift-6.3.3 releases no Darwin overlay: Apple's is built from sources of its
# own and split over _DarwinFoundation1-3, all with the ABI name Darwin. This
# one is swift's shared stdlib/public/Platform (Platform.swift, with the Darwin
# arms its signal and semaphore sections lost restored by
# darwin/platform-darwin.patch; TiocConstants.swift; tgmath) and the few
# declarations of Apple's that minidarwin's overlays use (darwin/*.swift), as
# one module over the Swift SDK's `Darwin` Clang module.
{ lib
, mkSwiftLibrary
, sources
, python3
, libswiftCore
, targetArch
}:

let
  inherit (libswiftCore) version;
  platform = "${sources.swift}/stdlib/public/Platform";
  gyb = file: ''
    mkdir -p gyb
    python3 ${sources.swift}/utils/gyb -DCMAKE_SIZEOF_VOID_P=8 \
      -o gyb/${lib.removeSuffix ".gyb" (baseNameOf file)} ${file}
  '';
  shims = "${sources.swift}/stdlib/public/SwiftShims";

  # SWIFT_RUNTIME_SWIFT_COMPILE_FLAGS' feature and definition, as CMake passes
  # them to every overlay.
  overlayFlags = [ "-D" "SWIFT_ENABLE_REFLECTION" "-enable-experimental-feature" "Extern" ];

  builtinFloat = mkSwiftLibrary {
    pname = "libswift_Builtin_float";
    inherit version;
    moduleName = "_Builtin_float";
    nativeBuildInputs = [ python3 ];
    prepareSources = gyb "${sources.swift}/stdlib/public/ClangOverlays/float.swift.gyb";
    listSources = "echo $NIX_BUILD_TOP/gyb/float.swift";
    # The ClangOverlays CMake's BUILTIN_FLOAT_SWIFT_FLAGS for Darwin.
    swiftFlags = overlayFlags ++ [ "-Xfrontend" "-module-abi-name" "-Xfrontend" "Darwin" ];
    # ld's $ld$previous$ directives: these symbols were libswiftDarwin's
    # before macOS 15.
    cSources = [ "${sources.swift}/stdlib/public/ClangOverlays/linker-support/magic-symbols-for-install-name.c" ];
    cFlags = [ "-DSWIFT_STDLIB_SUPPORT_BACK_DEPLOYMENT" "-I${shims}" ];
    verifySymbols = [ "_$s6Darwin11DBL_EPSILONSdvg" ];
  };

  darwin = mkSwiftLibrary {
    pname = "libswiftDarwin";
    inherit version;
    moduleName = "Darwin";
    swiftDependencies = [ builtinFloat ];
    nativeBuildInputs = [ python3 ];
    prepareSources = ''
      mkdir -p platform/stdlib/public/Platform
      cp ${platform}/Platform.swift platform/stdlib/public/Platform/
      chmod u+w platform/stdlib/public/Platform/Platform.swift
      patch -d platform -p1 < ${./darwin/platform-darwin.patch}
    '' + gyb "${platform}/tgmath.swift.gyb";
    listSources = ''
      printf '%s\n' $NIX_BUILD_TOP/platform/stdlib/public/Platform/Platform.swift \
        ${platform}/TiocConstants.swift $NIX_BUILD_TOP/gyb/tgmath.swift \
        ${./darwin/POSIXErrorCode.swift} ${./darwin/Darwin.swift}
    '';
    # The module is the overlay of the Clang module of its own name.
    swiftFlags = overlayFlags ++ [ "-import-underlying-module" ];
    # tgmath's Float overloads and Platform.swift's Bessel functions: libm is
    # not built (libswift-core.nix declares its own the same way).
    allowUndefined = lib.genAttrs
      (map (s: "_${s}") [
        "acosf"
        "acoshf"
        "asinf"
        "asinhf"
        "atan2f"
        "atanf"
        "atanhf"
        "cbrtf"
        "cosf"
        "coshf"
        "erfcf"
        "erff"
        "exp2f"
        "expf"
        "expm1f"
        "fdimf"
        "hypotf"
        "jn"
        "lgamma_r"
        "lgammaf_r"
        "log10f"
        "log1pf"
        "log2f"
        "logbf"
        "logf"
        "nanf"
        "nextafterf"
        "powf"
        "remquo"
        "remquof"
        "sinf"
        "sinhf"
        "tanf"
        "tanhf"
        "tgammaf"
        "yn"
      ])
      (_: "system_m")
    # x86_64 only: tgmath's Float80 overloads (the `l` functions).
    // lib.optionalAttrs (targetArch == "x86_64") (lib.genAttrs
      (map (s: "_${s}") [
        "acoshl"
        "acosl"
        "asinhl"
        "asinl"
        "atan2l"
        "atanhl"
        "atanl"
        "cbrtl"
        "coshl"
        "cosl"
        "erfcl"
        "erfl"
        "exp2l"
        "expl"
        "expm1l"
        "fdiml"
        "fmaxl"
        "fminl"
        "hypotl"
        "lgammal_r"
        "log10l"
        "log1pl"
        "log2l"
        "logbl"
        "logl"
        "nanl"
        "nearbyintl"
        "nextafterl"
        "powl"
        "remquol"
        "rintl"
        "sinhl"
        "sinl"
        "tanhl"
        "tanl"
        "tgammal"
      ])
      (_: "system_m"));
    verifySymbols = [
      "_$s6Darwin5errnos5Int32Vvg"
      "_$s6Darwin14POSIXErrorCodeOMn"
      "_$s6Darwin10MAP_FAILEDSvSgvg"
    ];
    passthru = { inherit builtinFloat; };
  };
in
darwin
