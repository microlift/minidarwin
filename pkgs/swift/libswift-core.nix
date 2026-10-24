# Stage 6: /usr/lib/swift/libswiftCore.dylib -- the Swift runtime and
# standard library, swift-5.10.1-RELEASE, as stdlib's CMake builds target
# swiftCore for macosx (Release, Objective-C interop, library evolution).
#
# Two halves, as in CMake: the Swift module (every stdlib/public/core file,
# one whole-module -parse-stdlib compile into Swift.o) and the object
# libraries swiftCore incorporates (runtime, LLVMSupport, Demangling, stubs,
# Threading, CommandLineSupport), C++ and Objective-C++. The Swift half is
# compiled by the host's swift-frontend (nixpkgs' swift-unwrapped, the same
# release: -parse-stdlib code is written against that compiler's builtins),
# against our sysroot; the C/C++ half and the link are toolchainStage4's.
#
# The Objective-C interop is Apple's (SWIFT_OBJC_INTEROP=1): Swift classes are
# ObjC classes, so this is the dylib a macOS Swift binary expects. libobjc,
# Foundation, CoreFoundation and libsystem_asl are not built: their headers
# come from interop-headers.nix, and every symbol of theirs the runtime
# imports is a declared hole (`allowUndefined`), bound in a running process
# by whatever provides libobjc there. There is no LC_LOAD_DYLIB for them.
{ lib
, mkDarwinPackage
, sources
, toolchain
, targetArch
, minOS
, swift
, python3
, swiftInteropHeaders
}:

let
  version = "5.10.1";
  srcs = import ./swift-core-sources.nix;
  arch = toolchain.machoArch;
  target = "${arch}-apple-macosx${minOS}";
  isX86 = targetArch == "x86_64";

  # gyb's own -D's, as CMake's handle_gyb_source_single passes them.
  gybFlags = [
    "-DunicodeGraphemeBreakPropertyFile=${sources.swift}/utils/UnicodeData/GraphemeBreakProperty.txt"
    "-DunicodeGraphemeBreakTestFile=${sources.swift}/utils/UnicodeData/GraphemeBreakTest.txt"
    "-DCMAKE_SIZEOF_VOID_P=8"
  ];
  gyb = dir: files: ''
    mkdir -p $NIX_BUILD_TOP/gyb
    for f in ${lib.escapeShellArgs files}; do
      (cd ${sources.swift}/${dir} &&
        python3 ${sources.swift}/utils/gyb ${lib.escapeShellArgs gybFlags} \
          -o $NIX_BUILD_TOP/gyb/''${f%.gyb} $f)
    done
  '';

  # The compiler resource dir CMake assembles in lib/swift: the compiler's
  # own clang headers, and SwiftShims with the parent module map.
  resourceDir = ''
    mkdir -p rd/shims
    ln -s ${swift.lib}/lib/swift/clang rd/clang
    cp ${sources.swift}/stdlib/public/SwiftShims/swift/shims/*.h \
       ${sources.swift}/stdlib/public/SwiftShims/swift/shims/module.modulemap rd/shims/
    cp ${sources.swift}/stdlib/public/SwiftShims/swift/shims/parent.modulemap rd/module.modulemap
  '' + lib.optionalString isX86 ''
    # The class layouts of pre-5.0 runtimes (back-deployment), x86_64 only.
    mkdir -p rd/macosx
    cp ${sources.swift}/stdlib/toolchain/legacy_layouts/macosx/layouts-x86_64.yaml rd/macosx/
  '';

  # swiftCore's SWIFT_COMPILE_FLAGS for macosx, from CMake's command line.
  # Availability macros are added from utils/availability-macros.def.
  swiftFlags = [
    "-c"
    "-sdk"
    "${toolchain.sysroot}"
    "-target"
    target
    "-resource-dir"
    "rd"
    "-O"
    "-D"
    "SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY"
    "-D"
    "SWIFT_RUNTIME_OS_VERSIONING"
    "-D"
    "SWIFT_STDLIB_ENABLE_UNICODE_DATA"
    "-D"
    "SWIFT_STDLIB_ENABLE_VECTOR_TYPES"
    "-D"
    "SWIFT_STDLIB_HAS_COMMANDLINE"
    "-D"
    "SWIFT_STDLIB_HAS_STDIN"
    "-D"
    "SWIFT_STDLIB_HAS_ENVIRON"
    "-Xcc"
    "-DSWIFT_STDLIB_HAS_ENVIRON"
    "-D"
    "SWIFT_STDLIB_OVERRIDABLE_RETAIN_RELEASE"
    "-D"
    "SWIFT_THREADING_DARWIN"
    "-no-link-objc-runtime"
    "-enable-library-evolution"
    "-library-level"
    "api"
    "-Xfrontend"
    "-require-explicit-availability=ignore"
    "-Xfrontend"
    "-enforce-exclusivity=unchecked"
    "-D"
    "SWIFT_ENABLE_REFLECTION"
    "-nostdimport"
    "-parse-stdlib"
    "-module-name"
    "Swift"
    "-Xfrontend"
    "-group-info-path"
    "-Xfrontend"
    "${sources.swift}/stdlib/public/core/GroupInfo.json"
    "-swift-version"
    "5"
    "-runtime-compatibility-version"
    "none"
    "-disable-autolinking-runtime-compatibility-dynamic-replacements"
    "-Xfrontend"
    "-disable-autolinking-runtime-compatibility-concurrency"
    "-warn-swift3-objc-inference-complete"
    "-Xllvm"
    "-sil-inline-generics"
    "-Xllvm"
    "-sil-partial-specialization"
    "-Xfrontend"
    "-enable-experimental-concise-pound-file"
    "-enable-experimental-feature"
    "Macros"
    "-enable-experimental-feature"
    "FreestandingMacros"
    "-Xcc"
    "-DswiftCore_EXPORTS"
    "-warn-implicit-overrides"
    "-Xfrontend"
    "-enable-ossa-modules"
    "-Xfrontend"
    "-enable-lexical-lifetimes=false"
    "-Xfrontend"
    "-disable-implicit-concurrency-module-import"
    "-Xfrontend"
    "-disable-implicit-string-processing-module-import"
    "-Xfrontend"
    "-prespecialize-generic-metadata"
    "-Xfrontend"
    "-target-min-inlining-version"
    "-Xfrontend"
    "min"
    "-module-link-name"
    "swiftCore"
    "-whole-module-optimization"
    "-parse-as-library"
    "-I"
    "rd/macosx"
  ];

  # The Swift module: Swift.o, and Swift.swiftmodule for anything compiled
  # against this stdlib (swift-link-test.nix). Compile-only, so it is built
  # once whatever the link half does.
  swiftObject = mkDarwinPackage {
    pname = "libswiftCore-swift-object";
    inherit version toolchain;
    dontUnpack = true;
    nativeBuildInputs = [ swift python3 ];

    buildPhase = ''
      runHook preBuild
      cd $NIX_BUILD_TOP
      ${gyb "stdlib/public/core" srcs.swiftGyb}
      ${resourceDir}

      avail=()
      while IFS= read -r m; do
        avail+=( -Xfrontend -define-availability -Xfrontend "$m" )
      done < <(grep -v '^#' ${sources.swift}/utils/availability-macros.def | grep .)
      [ "''${#avail[@]}" -gt 0 ] || { echo "libswiftCore: no availability macros" >&2; exit 1; }

      files=( ${lib.concatMapStringsSep " " (f: "${sources.swift}/stdlib/public/core/${f}") srcs.swift}
              ${lib.concatMapStringsSep " " (f: "gyb/${lib.removeSuffix ".gyb" f}") srcs.swiftGyb} )

      mkdir -p Swift.swiftmodule
      swiftc ${lib.escapeShellArgs swiftFlags} "''${avail[@]}" \
        -module-cache-path $NIX_BUILD_TOP/module-cache \
        -emit-module -emit-module-path Swift.swiftmodule/${arch}-apple-macos.swiftmodule \
        -avoid-emit-module-source-info \
        -o Swift.o "''${files[@]}"
      runHook postBuild
    '';

    installPhase = ''
      runHook preInstall
      install -Dm644 Swift.o $out/Swift.o
      # A compiler resource dir laid out as CMake's lib/swift, module included.
      cp -R rd $out/resource-dir
      mkdir -p $out/resource-dir/macosx
      cp -R Swift.swiftmodule $out/resource-dir/macosx/
      runHook postInstall
    '';
  };

  # _add_target_variant_c_compile_flags for macosx, and the project-wide
  # flags CMake puts first. -O2 is what CMake's later -O2 leaves of its -O3.
  cFlags = [
    "-DLLVM_DISABLE_ABI_BREAKING_CHECKS_ENFORCING=1"
    "-DSWIFT_INLINE_NAMESPACE=__runtime"
    "-D__STDC_CONSTANT_MACROS"
    "-D__STDC_FORMAT_MACROS"
    "-D__STDC_LIMIT_MACROS"
    "-Werror=date-time"
    "-Werror=unguarded-availability-new"
    "-Wall"
    "-Wextra"
    "-Wno-unused-parameter"
    "-Wwrite-strings"
    "-Wcast-qual"
    "-Wmissing-field-initializers"
    "-Wimplicit-fallthrough"
    "-Wcovered-switch-default"
    "-Wstring-conversion"
    "-Wmisleading-indentation"
    "-Wctad-maybe-unsupported"
    "-fvisibility=hidden"
    "-Werror=gnu"
    "-fno-sanitize=all"
    "-DSWIFT_RUNTIME"
    "-DSWIFT_LIB_SUBDIR=\"macosx\""
    "-DSWIFT_ARCH=\"${arch}\""
    "-O2"
    "-g0"
    "-DNDEBUG"
    "-DSWIFT_LIBRARY_EVOLUTION=1"
    "-DSWIFT_STDLIB_SUPPORT_BACK_DEPLOYMENT"
    "-DSWIFT_ENABLE_REFLECTION"
    "-DSWIFT_STDLIB_HAS_DLADDR"
    "-DSWIFT_STDLIB_HAS_DLSYM=1"
    "-DSWIFT_STDLIB_HAS_FILESYSTEM"
    "-DSWIFT_STDLIB_HAS_DARWIN_LIBMALLOC=1"
    "-DSWIFT_STDLIB_HAS_ASL"
    "-DSWIFT_STDLIB_HAS_STDIN"
    "-DSWIFT_STDLIB_HAS_ENVIRON"
    "-DSWIFT_STDLIB_HAS_LOCALE"
    "-DSWIFT_THREADING_DARWIN"
    "-DSWIFT_RUNTIME_OS_VERSIONING"
    "-DSWIFT_STDLIB_SHORT_MANGLING_LOOKUPS"
    "-DSWIFT_STDLIB_ENABLE_VECTOR_TYPES"
    "-DSWIFT_STDLIB_HAS_TYPE_PRINTING"
    "-DSWIFT_STDLIB_SUPPORTS_BACKTRACE_REPORTING"
    "-DSWIFT_STDLIB_ENABLE_UNICODE_DATA"
    # Not SWIFT_STDLIB_TRACING or SWIFT_STDLIB_CONCURRENCY_TRACING, which
    # Darwin defaults on: they emit os_signpost events, and <os/signpost.h>
    # is libsystem_trace's, never released. The runtime traces nothing.
    "-DSWIFT_STDLIB_OVERRIDABLE_RETAIN_RELEASE"
    "-I${sources.swift}/stdlib/include"
    "-I${sources.swift}/stdlib/public/SwiftShims"
    "-Igen/include"
    "-I${sources.swift}/include"
    # System headers, as they are in Apple's SDK: CF's and objc4's predate
    # clang's -Welaborated-enum-base, which system headers are exempt from.
    "-isystem"
    "${swiftInteropHeaders}/usr/include"
  ] ++ lib.optional isX86 "-mcx16"; # the double-word atomics

  # What CMake adds for C++ (and Objective-C++) over C.
  cxxFlags = [
    "-std=c++17"
    "-fvisibility-inlines-hidden"
    "-fno-exceptions"
    "-funwind-tables"
    "-Wno-noexcept-type"
    "-Wnon-virtual-dtor"
    "-Wdelete-non-virtual-dtor"
    "-Wsuggest-override"
    "-Werror=switch"
    "-Wdocumentation"
    "-Wunreachable-code"
    "-Woverloaded-virtual"
    "-Werror=c++98-compat-extra-semi"
    "-DOBJC_OLD_DISPATCH_PROTOTYPES=0"
  ];

  # The runtime's (and stubs') own SWIFT_RUNTIME_CORE_CXX_FLAGS.
  runtimeWarnings = [
    "-Wformat-nonliteral"
    "-Werror=format-nonliteral"
    # Still reported, but not fatal: clang 21 counts Metadata.cpp's
    # deliberate __attribute__((constructor)) as one; 5.10's clang did not.
    "-Wglobal-constructors"
    "-Wexit-time-destructors"
    "-Werror=exit-time-destructors"
  ];

  # Per object library: its source dir first, as CMake's include order has it.
  groups = {
    swiftRuntime = {
      dir = "stdlib/public/runtime";
      flags = runtimeWarnings ++ [
        "-DSWIFT_HAVE_CRASHREPORTERCLIENT=1"
        # As Apple's presets build it (CMake's default is off): the crash
        # handler that execs swift-backtrace, and _swift_backtrace_*.
        "-DSWIFT_ENABLE_BACKTRACING"
        "-DswiftCore_EXPORTS"
        "-I${sources.swift}/stdlib/include/llvm/Support"
      ];
    };
    swiftLLVMSupport = { dir = "stdlib/public/LLVMSupport"; flags = [ ]; };
    swiftDemangling = {
      dir = "stdlib/public";
      flags = [ "-DSWIFT_SUPPORT_OLD_MANGLING=1" "-DswiftCore_EXPORTS" ];
    };
    swiftStdlibStubs = {
      dir = "stdlib/public/stubs";
      flags = [ "-I${sources.swift}/stdlib/public" ] ++ runtimeWarnings ++ [ "-DswiftCore_EXPORTS" ];
    };
    swiftThreading = { dir = "stdlib/public/Threading"; flags = [ ]; };
    swiftCommandLineSupport = { dir = "stdlib/public/CommandLineSupport"; flags = [ ]; };
  };

  linkFlags = [
    "-Wl,-headerpad_max_install_names"
    "-Wl,-dead_strip"
    "-Wl,-sectcreate,__TEXT,__info_plist,Info.plist"
    "-Wl,-application_extension"
    # Apple's links libc++.1.dylib, not the libc++abi it re-exports.
    "-Wl,-no_implicit_dylibs"
  ];

  # Symbols of libraries not built here, looked up by flat namespace at run
  # time (-U leaves no LC_LOAD_DYLIB): libobjc's, by whatever provides it in
  # the process; see the header comment.
  allowUndefined =
    lib.genAttrs [
      "_OBJC_CLASS_$_NSObject"
      "_OBJC_METACLASS_$_NSObject"
      "__objc_empty_cache"
      "__objc_realizeClassFromSwift"
      "__objc_rootAutorelease"
      "_class_conformsToProtocol"
      "_class_copyIvarList"
      "_class_createInstance"
      "_class_getInstanceSize"
      "_class_getMethodImplementation"
      "_class_getName"
      "_class_getSuperclass"
      "_class_isMetaClass"
      "_class_respondsToSelector"
      "_class_setSuperclass"
      "_ivar_getOffset"
      "_objc_addLoadImageFunc"
      "_objc_alloc_init"
      "_objc_autorelease"
      "_objc_autoreleaseReturnValue"
      "_objc_constructInstance"
      "_objc_copyWeak"
      "_objc_destroyWeak"
      "_objc_destructInstance"
      "_objc_getClass"
      "_objc_getProtocol"
      "_objc_initWeak"
      "_objc_loadWeakRetained"
      "_objc_lookUpClass"
      "_objc_moveWeak"
      "_objc_msgSend"
      "_objc_msgSendSuper2"
      "_objc_opt_self"
      "_objc_readClassPair"
      "_objc_release"
      "_objc_retain"
      "_objc_retainAutoreleasedReturnValue"
      "_objc_setHook_getClass"
      "_objc_setHook_getImageName"
      "_objc_setHook_lazyClassNamer"
      "_objc_storeWeak"
      "_object_dispose"
      "_object_getClass"
      "_object_setClass"
      "_protocol_conformsToProtocol"
      "_protocol_getName"
      "_sel_getName"
    ]
      (_: "objc")
    # libdyld's, and dyld_priv.h's protocol-conformance and shared-cache queries.
    // lib.genAttrs [
      "__dyld_find_foreign_type_protocol_conformance"
      "__dyld_find_foreign_type_protocol_conformance_on_disk"
      "__dyld_find_protocol_conformance"
      "__dyld_find_protocol_conformance_on_disk"
      "__dyld_get_shared_cache_range"
      "__dyld_has_preoptimized_swift_protocol_conformances"
      "__dyld_is_objc_constant"
      "__dyld_swift_optimizations_version"
      "_dladdr"
      "_dlsym"
      "_dyld_image_path_containing_address"
      "_dyld_program_sdk_at_least"
      "_dyld_shared_cache_some_image_overridden"
      "_environ"
    ]
      (_: "dyld")
    // lib.genAttrs
      (map (s: "_${s}") [
        "cos"
        "cosf"
        "exp"
        "exp2"
        "exp2f"
        "expf"
        "fmod"
        "fmodf"
        "log"
        "log10"
        "log10f"
        "log2"
        "log2f"
        "logf"
        "remainder"
        "remainderf"
        "sin"
        "sinf"
      ])
      (_: "system_m")
    # x86_64 only: Float80's math (the `l` functions), and fma/floor-style
    # calls arm64 has as instructions.
    // lib.optionalAttrs isX86 (lib.genAttrs
      (map (s: "_${s}") [
        "ceill"
        "cosl"
        "exp2l"
        "expl"
        "floorl"
        "fma"
        "fmaf"
        "fmal"
        "fmodl"
        "log10l"
        "log2l"
        "logl"
        "nearbyintl"
        "remainderl"
        "rintl"
        "roundl"
        "sinl"
        "truncl"
      ])
      (_: "system_m"))
    # x86_64 only: SwiftNativeNSXXXBaseARC.m's AUTORELEASEPOOL is i386/x86_64's.
    // lib.optionalAttrs isX86 (lib.genAttrs [
      "_objc_autoreleasePoolPop"
      "_objc_autoreleasePoolPush"
    ]
      (_: "objc"))
    // {
      "_asl_log" = "system_asl"; # fatal-error messages, besides stderr
      "_os_system_version_get_current_version" = "system_darwin"; # weak
    };
  holeFlags = map (s: "-Wl,-U,${s}") (lib.attrNames allowUndefined);

  installName = "/usr/lib/swift/libswiftCore.dylib";
in

mkDarwinPackage {
  pname = "libswiftCore";
  inherit version toolchain;

  src = sources.swift;
  nativeBuildInputs = [ python3 ];

  passthru = {
    inherit allowUndefined installName swiftObject swift target;
  };

  buildPhase = ''
    runHook preBuild
    # The build top, not the source root: md_compile names objects by their
    # path below it, and the gyb output (gyb/, beside swift-src/) must not
    # be named by the random build directory -- that reorders the link.
    export MD_SRCROOT=$NIX_BUILD_TOP

    ${gyb "stdlib/public/stubs" [ "SwiftNativeNSXXXBase.mm.gyb" ]}

    # include/swift/Runtime/CMakeConfig.h.in, as configure_file renders it.
    mkdir -p gen/include/swift/Runtime
    sed -e 's/^#cmakedefine01 \(.*\)/#define \1 0/' \
        -e 's/^#cmakedefine SWIFT_VERSION_MAJOR .*/#define SWIFT_VERSION_MAJOR "5"/' \
        -e 's/^#cmakedefine SWIFT_VERSION_MINOR .*/#define SWIFT_VERSION_MINOR "10"/' \
        include/swift/Runtime/CMakeConfig.h.in > gen/include/swift/Runtime/CMakeConfig.h
    ! grep -q cmakedefine gen/include/swift/Runtime/CMakeConfig.h

    # stdlib/Info.plist.in, for the __TEXT,__info_plist section.
    sed -e 's/@PLIST_INFO_UTI@/com.apple.dt.runtime.swiftCore/' \
        -e 's/@PLIST_INFO_NAME@/swiftCore/' \
        -e 's/@PLIST_INFO_VERSION@/${version}/' \
        -e 's/@PLIST_INFO_BUILD_VERSION@//' \
        stdlib/Info.plist.in > Info.plist
    ! grep -q @ Info.plist

    ${lib.concatStringsSep "\n" (lib.mapAttrsToList (name: g:
      let
        files = map (f: if lib.hasPrefix "$(GYB)/" f
                        then "$NIX_BUILD_TOP/gyb/${lib.removePrefix "$(GYB)/" f}"
                        else "$PWD/${f}") srcs.${name};
        # ARC files compile as Objective-C, with CMake's C flags (no C++ ones).
        arc = lib.filter (f: lib.hasSuffix ".m" f) files;
        rest = lib.filter (f: !lib.hasSuffix ".m" f) files;
        flags = [ "-I${sources.swift}/${g.dir}" ] ++ cFlags ++ g.flags
          ++ [ "-DSWIFT_TARGET_LIBRARY_NAME=${name}" ];
      in ''
        md_compile $PWD/obj/${name} "$CXX" ${lib.escapeShellArgs (flags ++ cxxFlags)} -- ${toString rest}
      '' + lib.optionalString (arc != [ ]) ''
        md_compile $PWD/obj/${name} "$CC" ${lib.escapeShellArgs flags} -fobjc-arc -- ${toString arc}
      '') groups)}

    md_compile $PWD/obj/swiftCore "$CC" -I${sources.swift}/stdlib/public/core \
      ${lib.escapeShellArgs cFlags} -DswiftCore_macosx_${arch}_EXPORTS \
      -DSWIFT_TARGET_LIBRARY_NAME=swiftCore \
      -- ${lib.concatMapStringsSep " " (f: "$PWD/${f}") srcs.swiftCoreC}
    cp ${swiftObject}/Swift.o obj/swiftCore/Swift.o

    CC="$CXX" MD_COMPAT_VERSION=1.0.0 MD_CURRENT_VERSION=0 \
      md_dylib libswiftCore.dylib ${installName} obj \
        ${lib.escapeShellArgs (linkFlags ++ holeFlags)}

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 libswiftCore.dylib $out${installName}
    f=$out${installName}

    md_verify_pure   $f
    md_verify_signed $f
    deps=$($OTOOL -L $f | tail -n +2 | awk '{ print $1 }' | grep -vx ${installName} | sort -u || true)
    want=$(printf '%s\n' /usr/lib/libSystem.B.dylib /usr/lib/libc++.1.dylib | sort)
    if [ "$deps" != "$want" ]; then
      echo "libswiftCore: links other than libSystem and libc++:" >&2
      echo "$deps" >&2
      exit 1
    fi
    md_verify_symbols $f \
      _swift_retain _swift_release _swift_allocObject _swift_getTypeByMangledNameInContext \
      _swift_conformsToProtocol _swift_dynamicCast '_$sSSN' '_$sSiN' '_$ss5print_9separator10terminatoryypd_S2StF' \
      __swift_backtrace_isThunkFunction

    # Foundation's classes are found at run time (see compat/Foundation);
    # NSObject is libobjc's.
    if $NM -u $f | grep '_OBJC_\(META\)\?CLASS_\$_NS' | grep -v '_\$_NSObject$'; then
      echo "libswiftCore: imports a Foundation class at link time (above)" >&2
      exit 1
    fi

    # A declared hole the dylib no longer imports is stale.
    $NM -u $f | awk '{ print $NF }' | sort -u > imports
    for s in ${lib.escapeShellArgs (lib.attrNames allowUndefined)}; do
      grep -qx -- "$s" imports || {
        echo "libswiftCore: declares $s absent but does not import it" >&2
        exit 1; }
    done

    runHook postInstall
  '';

  meta.description = "libswiftCore.dylib (Swift ${version} runtime and standard library), linked against minidarwin's libSystem and libc++";
}
