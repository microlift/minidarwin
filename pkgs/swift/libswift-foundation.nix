# Stage 6: /usr/lib/swift/libswiftFoundation.dylib -- Foundation's Swift half,
# from swift-foundation at the runtime's release.
#
# Apple builds swift-foundation into Foundation.framework itself, with
# FOUNDATION_FRAMEWORK defined, and that configuration needs the framework's
# Objective-C half and private headers. This is swift-foundation's other
# configuration, FoundationEssentials as CMake lists it, compiled under the
# framework's module name so its symbols are the ones a macOS binary imports
# from Foundation ($s10Foundation4DataV...). The Objective-C half is not part
# of a Darwin base: whatever runs a binary provides it (Rosemary re-exports this
# image from its own Foundation).
#
# Where the two configurations differ, a binary built against Apple's SDK sees
# the framework's: what swift-foundation takes from Objective-C there (for one,
# Data.ReadingOptions is NSData.ReadingOptions) is a type of its own here, and
# an import of the framework's spelling is not defined.
#
# Not built: FoundationInternationalization (it needs swift-foundation-icu),
# FoundationMacros (#Predicate's expansion: a compiler plugin, run by whatever
# compiles against this), and NotificationCenter's Swift class and messages,
# which need _Concurrency and Synchronization and which the framework's ABI
# spells as NSNotificationCenter's.
#
# minidarwin's own, under foundation/: the Logger the Darwin build reaches
# through the closed os overlay (Logger.swift), the NSString-backed formatting
# Apple's framework layers over swift-foundation (String+Format.swift, over
# CoreFoundation, whose functions are declared in include/), and the module's
# own name where swift-foundation qualifies one with FoundationEssentials
# (module-name.patch).
{ lib
, mkSwiftLibrary
, sources
, copyfile
, removefile
, libswiftCore
, libswiftDarwin
, libswiftStringProcessing
}:

let
  inherit (libswiftCore) version;
  srcs = import ./swift-foundation-sources.nix;
  foundation = sources.swiftFoundation;
  collectionsSrc = sources.swiftCollections;

  # swift-foundation's CMakeLists.txt: FoundationPreview availability for
  # each release, and FoundationEssentials' language features.
  availability = lib.concatMap
    (v: [
      "-Xfrontend"
      "-enable-experimental-feature"
      "-Xfrontend"
      "AvailabilityMacro=FoundationPreview ${v}:macOS 15, iOS 18, tvOS 18, watchOS 11"
    ])
    [ "6.0.2" "6.1" "6.2" "6.3" ];
  features = lib.concatMap (f: [ "-enable-experimental-feature" f ]) [
    "VariadicGenerics"
    "LifetimeDependence"
    "AddressableTypes"
    "BuiltinModule"
    "AccessLevelOnImport"
    "StrictConcurrency"
    # String+Format.swift's
    "Extern"
  ] ++ lib.concatMap (f: [ "-enable-upcoming-feature" f ]) [
    "InferSendableFromCaptures"
    "MemberImportVisibility"
  ];

  # The single-module swift-collections the toolchain build links into
  # FoundationEssentials (COLLECTIONS_FOUNDATION_TOOLCHAIN_MODULE): an archive.
  collections = mkSwiftLibrary {
    pname = "swift-foundation-collections";
    version = "1.1.6";
    moduleName = "_FoundationCollections";
    linkName = "_FoundationCollections";
    static = true;
    libraryEvolution = false;
    listSources = "printf '%s\\n' ${lib.concatMapStringsSep " " (f: lib.escapeShellArg "${collectionsSrc}/${f}") srcs.collections}";
    swiftFlags = [
      "-D"
      "COLLECTIONS_SINGLE_MODULE"
      "-Xfrontend"
      "-allowable-client"
      "-Xfrontend"
      "Foundation"
    ];
  };

  essentials = map (f: "${foundation}/${f}") srcs.essentials;
  patched = "Sources/FoundationEssentials/Calendar/Date+FormatStyle.swift";
in

mkSwiftLibrary {
  pname = "libswiftFoundation";
  inherit version;
  moduleName = "Foundation";
  swiftDependencies = [ libswiftDarwin libswiftStringProcessing collections ];
  implicitStringProcessing = true;

  prepareSources = ''
    mkdir -p foundation/$(dirname ${patched})
    cp ${foundation}/${patched} foundation/${patched}
    chmod u+w foundation/${patched}
    patch -d foundation -p1 < ${./foundation/module-name.patch}
  '';
  listSources = ''
    printf '%s\n' ${lib.concatMapStringsSep " " lib.escapeShellArg essentials} \
      ${./foundation/Logger.swift} ${./foundation/String+Format.swift} |
      sed "s|^${foundation}/${patched}$|$NIX_BUILD_TOP/foundation/${patched}|"
  '';

  swiftFlags = availability ++ features ++ [
    "-package-name"
    "SwiftFoundation"
    "-I"
    "${foundation}/Sources/_FoundationCShims/include"
    "-I"
    "${./foundation/include}"
  ];
  # FileManager's copies and removals; libSystem's members on a Mac.
  linkLibraries = [
    { pkg = copyfile; l = "copyfile"; }
    { pkg = removefile; l = "removefile"; }
  ];
  cSources = map (f: "${foundation}/${f}") srcs.cShims;
  cFlags = [ "-I${foundation}/Sources/_FoundationCShims/include" ];

  allowUndefined = {
    "_CFStringCreateWithFormatAndArguments" = "CoreFoundation";
    "__NSGetExecutablePath" = "dyld";
    "_fmod" = "system_m";
    "_remainder" = "system_m";
    "_getgrgid_r" = "system_info";
    "_getgrnam_r" = "system_info";
    "_getpwnam_r" = "system_info";
    "_getpwuid_r" = "system_info";
    "_sysdir_start_search_path_enumeration" = "system_coreservices";
    "_sysdir_get_next_search_path_enumeration" = "system_coreservices";
  } // lib.genAttrs [
    "__objc_empty_cache"
    "_objc_autorelease"
    "_objc_getAssociatedObject"
    "_objc_opt_self"
    "_objc_setAssociatedObject"
    "_objc_sync_enter"
    "_objc_sync_exit"
  ]
    (_: "objc");

  # What a macOS binary imports from Foundation for Data, URL, JSONEncoder and
  # String(format:), spelled as the framework spells them.
  verifySymbols = [
    "_$s10Foundation4DataVN"
    "_$s10Foundation4DataV15_RepresentationON"
    "_$s10Foundation3URLV15fileURLWithPathACSSh_tcfC"
    "_$s10Foundation11JSONEncoderCMa"
    "_$sSS10FoundationE6format_S2Sh_s7CVarArg_pdtcfC"
    "_$sSSs7CVarArg10FoundationMc"
  ];

  passthru = { inherit collections; };
}
