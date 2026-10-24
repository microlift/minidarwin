# Stage 6 check: a Swift program compiles against this stdlib's own module and
# links with nothing but -lswiftCore. Not run (cross builds never execute the
# target), so it checks what can be checked statically: every runtime entry
# point and stdlib symbol the probe needs is exported by libswiftCore.dylib,
# and the executable loads it from /usr/lib/swift as a macOS Swift binary does.
{ stdenvNoCC
, writeText
, toolchain
, libswiftCore
, buildSupport
}:

let
  inherit (libswiftCore) swift swiftObject target;

  # Classes (SwiftObject, ObjC metadata), generics over a protocol, a throw
  # (swift_allocError), a dynamic cast, String/Array/Dictionary, print.
  probe = writeText "main.swift" ''
    protocol Shape { var area: Double { get } }
    final class Square: Shape {
      let side: Double
      init(side: Double) { self.side = side }
      var area: Double { side * side }
    }
    struct Circle: Shape { var r: Double; var area: Double { 3.0 * r * r } }

    enum ProbeError: Error { case negative(Int) }

    func total<S: Sequence>(_ shapes: S) -> Double where S.Element == any Shape {
      shapes.reduce(0) { $0 + $1.area }
    }

    func check(_ n: Int) throws -> Int {
      guard n >= 0 else { throw ProbeError.negative(n) }
      return n
    }

    let shapes: [any Shape] = [Square(side: 2), Circle(r: 1)]
    var counts: [String: Int] = [:]
    for s in shapes { counts["\(type(of: s))", default: 0] += 1 }

    let anything: Any = shapes[0]
    let isSquare = (anything as? Square) != nil

    do { _ = try check(-1) } catch { print("caught \(error)") }
    print("total \(total(shapes)), \(counts.count) kinds, square: \(isSquare)")
  '';
in

stdenvNoCC.mkDerivation {
  pname = "minidarwin-swift-link-test";
  version = libswiftCore.version;

  dontUnpack = true;
  dontFixup = true;

  nativeBuildInputs = [ toolchain swift ];

  buildPhase = ''
    runHook preBuild
    source ${buildSupport}

    echo "== compiling the probe against this stdlib (-resource-dir), not the compiler's"
    cp ${probe} main.swift
    # No implicit _Concurrency/_StringProcessing: neither is built, and the
    # failed lookup falls through to Clang, which cannot parse the SDK's
    # top-level module.modulemap (Libinfo's rpc map, not unifdef'd).
    swiftc -c -O -target ${target} -sdk ${toolchain.sysroot} \
      -resource-dir ${swiftObject}/resource-dir \
      -Xfrontend -disable-implicit-concurrency-module-import \
      -Xfrontend -disable-implicit-string-processing-module-import \
      -module-cache-path $NIX_BUILD_TOP/module-cache \
      -module-name probe main.swift -o probe.o

    # A Swift class's metadata points at libobjc's empty method cache and its
    # accessor calls objc_opt_self: a macOS Swift binary links libobjc.A.dylib
    # itself. libobjc is not built (see libswift-core.nix), so those two are
    # the probe's only holes.
    echo "== -lswiftCore alone must resolve everything else"
    $CC -o probe probe.o -L${libswiftCore}/usr/lib/swift -lswiftCore \
      -Wl,-U,__objc_empty_cache -Wl,-U,_objc_opt_self

    md_verify_pure   probe
    md_verify_signed probe

    $OTOOL -L probe
    deps=$($OTOOL -L probe | tail -n +2 | awk '{ print $1 }' | sort -u)
    want=$(printf '%s\n' /usr/lib/libSystem.B.dylib ${libswiftCore.installName} | sort)
    if [ "$deps" != "$want" ]; then
      echo "probe links other than libSystem and libswiftCore:" >&2
      echo "$deps" >&2
      exit 1
    fi

    echo "== the runtime entry points are imports, defined by libswiftCore"
    $NM -u probe | awk '{ print $NF }' | sort -u > undefined.txt
    for s in _swift_allocObject _swift_allocError _swift_dynamicCast \
             '_$ss5print_9separator10terminatoryypd_S2StF'; do
      grep -qx -- "$s" undefined.txt || {
        echo "probe does not import $s -- probe no longer exercises it" >&2
        exit 1; }
    done

    runHook postBuild
  '';

  installPhase = ''
    mkdir -p $out
    cp probe undefined.txt $out/
    echo "Swift link ok" > $out/result
  '';

  meta.description = "Links a Swift program against minidarwin's libswiftCore with -lswiftCore only";
}
