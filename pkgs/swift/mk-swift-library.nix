# A Swift library of /usr/lib/swift beside libswiftCore: one module, compiled
# whole-module against the Swift SDK (swift-sdk.nix) and this stdlib's own
# module, into `out` (the dylib) and `modules` (its .swiftmodule, and with
# library evolution the .swiftinterfaces, for what compiles against it).
#
# As libswiftCore's Swift half, the compile is the host's swift-frontend (the
# stdlib's release) and the link is the toolchain's. Neither _Concurrency nor
# (unless `implicitStringProcessing`) _StringProcessing is imported implicitly:
# a library imports what it is built against, and nothing else.
{ lib
, mkDarwinPackage
, buildSupport
, sources
, toolchain
, swiftSdk
, libswiftCore
}:

{ pname
, version
, moduleName
, linkName ? "swift${moduleName}"
, installName ? "/usr/lib/swift/lib${linkName}.dylib"
  # Shell that prints the Swift sources, one absolute path per line, in
  # compile order.
, listSources
  # Shell run before the compile, from the build top (gyb, patched copies).
, prepareSources ? ""
, swiftFlags ? [ ]
  # Libraries made by this function that the module imports.
, swiftDependencies ? [ ]
, libraryEvolution ? true
  # An archive for another library of this kind to link in, rather than a
  # dylib of /usr/lib/swift: `out` is lib/lib<linkName>.a.
, static ? false
, implicitStringProcessing ? false
  # C sources linked into the dylib, and their flags.
, cSources ? [ ]
, cFlags ? [ ]
  # Other built dylibs the library links, { pkg; l; } as mk-cmds.nix's.
, linkLibraries ? [ ]
  # Imports of libraries not built (libobjc, CoreFoundation, ...), as
  # libswift-core.nix's: symbol -> the library that defines it at run time.
, allowUndefined ? { }
, verifySymbols ? [ ]
, nativeBuildInputs ? [ ]
, passthru ? { }
, meta ? { }
}:

let
  inherit (libswiftCore) swift swiftObject target;
  arch = toolchain.machoArch;
  moduleDir = "modules/${moduleName}.swiftmodule";
  moduleBase = "${moduleDir}/${arch}-apple-macos";

  flags = [
    "-sdk"
    "${swiftSdk}"
    "-target"
    target
    "-resource-dir"
    "${swiftObject}/resource-dir"
    "-O"
    "-whole-module-optimization"
    "-parse-as-library"
    "-module-name"
    moduleName
    "-module-link-name"
    linkName
    "-runtime-compatibility-version"
    "none"
    "-Xfrontend"
    "-disable-implicit-concurrency-module-import"
    # Search paths stay out of the .swiftmodule: they are this build's.
    "-Xfrontend"
    "-no-serialize-debugging-options"
    # The ABI descriptor beside the module lists declarations in no stable
    # order; libswift-core.nix empties its own for the same reason.
    "-Xfrontend"
    "-empty-abi-descriptor"
  ]
  ++ lib.optionals (!implicitStringProcessing) [
    "-Xfrontend"
    "-disable-implicit-string-processing-module-import"
  ]
  ++ lib.optional libraryEvolution "-enable-library-evolution"
  ++ lib.concatMap (d: [ "-I" "${d.modules}" ]) swiftDependencies
  ++ swiftFlags;

  holeFlags = map (s: "-Wl,-U,${s}") (lib.attrNames allowUndefined);
  libraryDirs = [ "-L${libswiftCore}/usr/lib/swift" ]
    ++ map (d: "-L${d}/${if d.static or false then "lib" else "usr/lib/swift"}") swiftDependencies;
  linkFlags = lib.concatMap (l: [ "-L${l.pkg}/usr/lib" "-l${l.l}" ]) linkLibraries;
in

mkDarwinPackage {
  inherit pname version toolchain;
  outputs = [ "out" "modules" ];

  dontUnpack = true;
  # The multiple-output configure hook wants a doc output, and there is
  # nothing to configure: the build phase sources the build support that
  # mkDarwinPackage's preConfigure would have.
  dontConfigure = true;
  nativeBuildInputs = [ swift ] ++ nativeBuildInputs;

  passthru = {
    inherit allowUndefined installName moduleName static;
  } // passthru;

  buildPhase = ''
    source ${buildSupport}
    runHook preBuild
    cd $NIX_BUILD_TOP
    export MD_SRCROOT=$NIX_BUILD_TOP
    ${prepareSources}

    files=()
    while IFS= read -r f; do files+=( "$f" ); done < <(${listSources})
    [ "''${#files[@]}" -gt 0 ] || { echo "${pname}: no sources" >&2; exit 1; }

    # The stdlib's availability macros (SwiftStdlib x.y and its
    # StdlibDeploymentTarget twin), as libswift-core.nix defines them: CMake
    # passes them to every library of the stdlib's build.
    avail=()
    while IFS= read -r m; do
      avail+=( -Xfrontend -define-availability -Xfrontend "$m" )
      case $m in
        "SwiftStdlib "*)
          avail+=( -Xfrontend -define-availability -Xfrontend "StdlibDeploymentTarget ''${m#SwiftStdlib }" ) ;;
      esac
    done < <(grep -v '^#' ${sources.swift}/utils/availability-macros.def | grep .)
    [ "''${#avail[@]}" -gt 0 ] || { echo "${pname}: no availability macros" >&2; exit 1; }

    common=( ${lib.escapeShellArgs flags} "''${avail[@]}" -module-cache-path $NIX_BUILD_TOP/module-cache )
    mkdir -p ${moduleDir} obj
    # Two invocations, as libswift-core.nix's: the module (and its
    # interfaces), then the object.
    swiftc -emit-module -o ${moduleBase}.swiftmodule -avoid-emit-module-source-info \
      "''${common[@]}" \
      ${lib.optionalString libraryEvolution ''
        -emit-module-interface-path ${moduleBase}.swiftinterface \
        -emit-private-module-interface-path ${moduleBase}.private.swiftinterface \
      ''} \
      "''${files[@]}"
    swiftc -c -o obj/${moduleName}.o "''${common[@]}" "''${files[@]}"

    ${lib.optionalString (cSources != [ ]) ''
      md_compile $PWD/obj/c "$CC" -O2 ${lib.escapeShellArgs cFlags} -- ${lib.escapeShellArgs cSources}
    ''}

    ${if static then ''
      $AR rcs lib${linkName}.a $(find obj -name '*.o' | LC_ALL=C sort)
    '' else ''
      # The Swift objects autolink their libraries (swiftCore, the overlays
      # they import); every other import is a declared hole.
      md_dylib lib${linkName}.dylib ${installName} obj \
        ${lib.escapeShellArgs (libraryDirs ++ linkFlags ++ holeFlags)} \
        -Wl,-headerpad_max_install_names
    ''}
    runHook postBuild
  '';

  installPhase =
    if static then ''
      runHook preInstall
      install -Dm644 lib${linkName}.a $out/lib/lib${linkName}.a
      mkdir -p $modules
      cp -R ${moduleDir} $modules/
      runHook postInstall
    '' else ''
      runHook preInstall
      install -Dm755 lib${linkName}.dylib $out${installName}
      f=$out${installName}
      md_verify_pure   $f
      md_verify_signed $f
      ${lib.optionalString (verifySymbols != [ ]) ''
        md_verify_symbols $f ${lib.escapeShellArgs verifySymbols}
      ''}

      # A declared hole the dylib no longer imports is stale.
      $NM -u $f | awk '{ print $NF }' | sort -u > imports
      for s in ${lib.escapeShellArgs (lib.attrNames allowUndefined)}; do
        grep -qx -- "$s" imports || {
          echo "${pname}: declares $s absent but does not import it" >&2
          exit 1; }
      done

      mkdir -p $modules
      cp -R ${moduleDir} $modules/
      runHook postInstall
    '';

  meta = { description = "/usr/lib/swift/lib${linkName}.dylib (the ${moduleName} module)"; } // meta;
}
