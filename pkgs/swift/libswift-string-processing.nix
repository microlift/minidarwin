# Stage 6: /usr/lib/swift/libswift_StringProcessing.dylib (Regex and the
# string algorithms Swift imports implicitly) and libswift_RegexParser.dylib,
# from swift-experimental-string-processing at the runtime's release, as
# stdlib/public/StringProcessing and RegexParser's CMake build them: every
# Swift file of the module's directory (a GLOB_RECURSE there too), and
# _CUnicode's C linked into _StringProcessing.
{ mkSwiftLibrary
, sources
, libswiftCore
}:

let
  inherit (libswiftCore) version;
  src = "${sources.swiftStringProcessing}/Sources";
  # SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS' features these use.
  features = [ "-enable-experimental-feature" "Extern" "-Xfrontend" "-enable-experimental-concise-pound-file" ];
  glob = dir: "find ${src}/${dir} -name '*.swift' | LC_ALL=C sort";

  regexParser = mkSwiftLibrary {
    pname = "libswift_RegexParser";
    inherit version;
    moduleName = "_RegexParser";
    listSources = glob "_RegexParser";
    # RegexParser's CMake strips -enable-library-evolution: _StringProcessing
    # imports it as an implementation detail.
    libraryEvolution = false;
    swiftFlags = features;
    verifySymbols = [ "_$s12_RegexParser3ASTVMn" ];
  };

  stringProcessing = mkSwiftLibrary {
    pname = "libswift_StringProcessing";
    inherit version;
    moduleName = "_StringProcessing";
    swiftDependencies = [ regexParser ];
    listSources = glob "_StringProcessing";
    swiftFlags = features ++ [ "-D" "RESILIENT_LIBRARIES" ];
    cSources = [ "${src}/_CUnicode/UnicodeData.c" "${src}/_CUnicode/UnicodeScalarProps.c" ];
    cFlags = [ "-I${src}/_CUnicode/include" "-Dswift_StringProcessing_EXPORTS" ];
    # A Swift class's metadata: libobjc's, which is not built.
    allowUndefined = {
      "__objc_empty_cache" = "objc";
      "_objc_opt_self" = "objc";
    };
    verifySymbols = [ "_$s17_StringProcessing5RegexVMn" ];
    passthru = { inherit regexParser; };
  };
in
stringProcessing
