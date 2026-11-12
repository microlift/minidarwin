# SDK check: building against the SDK is building against the system.
#
# C, C++ and library-using programs are compiled against minidarwinSdk (headers
# only, no dylib anywhere), then each is linked twice: once with the SDK as
# the sysroot -- the .tbd stubs -- and once with the rootfs itself, whose real
# dylibs the stubs were made from. The two links must be byte-identical: same
# load commands, same imports, same signature. The SDK link is also checked
# on its own terms (what it loads, that every load resolves inside the rootfs),
# so a pass cannot come from both links being wrong the same way.
{ lib
, stdenvNoCC
, writeText
, mkToolchain
, clangResourceDir
, minidarwinSdk
, rootfs
, buildSupport
}:

let
  sdkToolchain = mkToolchain {
    name = "minidarwin-toolchain-sdk";
    sysroot = minidarwinSdk;
    resourceDir = clangResourceDir;
  };
  # Only for linking: the rootfs has the dylibs but none of the C++ headers.
  rootfsToolchain = mkToolchain {
    name = "minidarwin-toolchain-rootfs";
    sysroot = rootfs;
    resourceDir = clangResourceDir;
  };

  c = writeText "sdk-probe.c" ''
    #include <dispatch/dispatch.h>
    #include <pthread.h>
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>
    #include <copyfile.h>

    static void *worker(void *arg) { return (void *)(strlen(arg) + 1); }

    int main(int argc, char **argv) {
      pthread_t t;
      void *r;
      char *s = strdup(argc > 1 ? argv[1] : "minidarwin");
      pthread_create(&t, NULL, worker, s);
      pthread_join(t, &r);
      dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0);
      copyfile_state_t st = copyfile_state_alloc();
      copyfile_state_free(st);
      printf("%p %ld\n", (void *)q, (long)r);
      free(s);
      return 0;
    }
  '';

  cxx = writeText "sdk-probe.cpp" ''
    #include <iostream>
    #include <map>
    #include <memory>
    #include <stdexcept>
    #include <string>

    struct Base { virtual ~Base() = default; };
    struct Derived : Base { int v = 3; };

    int main() {
      std::map<std::string, int> m{{"a", 1}};
      std::unique_ptr<Base> b = std::make_unique<Derived>();
      int n = dynamic_cast<Derived *>(b.get())->v + m["a"];
      try { throw std::runtime_error("sdk"); }
      catch (const std::exception &e) { n += static_cast<int>(std::string(e.what()).size()); }
      std::cout << n << std::endl;
      return 0;
    }
  '';

  libs = writeText "sdk-probe-libs.c" ''
    #include <stdio.h>
    #include <zlib.h>
    #include <bzlib.h>
    #include <sqlite3.h>
    #include <histedit.h>
    #include <curses.h>
    #include <libxml/parser.h>
    #include <unicode/uversion.h>

    int main(void) {
      UVersionInfo icu;
      u_getVersion(icu);
      EditLine *el = el_init("probe", stdin, stdout, stderr);
      el_end(el);
      xmlCheckVersion(LIBXML_VERSION);
      printf("%s %s %s %s %d\n", zlibVersion(), BZ2_bzlibVersion(),
             sqlite3_libversion(), curses_version(), icu[0]);
      return 0;
    }
  '';
in

stdenvNoCC.mkDerivation {
  name = "minidarwin-sdk-link-test";
  dontUnpack = true;
  dontFixup = true;
  nativeBuildInputs = [ sdkToolchain ];

  SOURCE_DATE_EPOCH = "1";
  ZERO_AR_DATE = "1";
  TZ = "UTC";
  LC_ALL = "C";

  buildPhase = ''
    runHook preBuild
    source ${buildSupport}
    sdkcc=${sdkToolchain}/bin/cc
    sdkcxx=${sdkToolchain}/bin/c++
    fscc=${rootfsToolchain}/bin/cc
    fscxx=${rootfsToolchain}/bin/c++

    # Compiled against the SDK only. No -I for libraries: their headers are
    # where the SDK says, the way a consumer finds them.
    $sdkcc  -O2 -c ${c}    -o c.o
    $sdkcxx -O2 -std=c++17 -c ${cxx} -o cxx.o
    $sdkcc  -O2 -iwithsysroot /usr/include/libxml2 -c ${libs} -o libs.o

    # Same output name in both: the ad-hoc signature's identifier is the file
    # name, and lld's UUID is a hash over the signed output.
    mkdir -p sdk rootfs
    link() { # name c|cxx objects/flags...
      local name=$1 kind=$2; shift 2
      local sdk=$sdkcc fs=$fscc
      if [ "$kind" = cxx ]; then sdk=$sdkcxx; fs=$fscxx; fi
      $sdk "$@" -o sdk/$name
      $fs  "$@" -o rootfs/$name
      if ! cmp sdk/$name rootfs/$name; then
        echo "sdk-link-test: $name links differently against the SDK and the rootfs" >&2
        $OTOOL -l sdk/$name > sdk.l; $OTOOL -l rootfs/$name > rootfs.l
        diff sdk.l rootfs.l >&2 || true
        exit 1
      fi
      md_verify_pure "sdk/$name"
    }
    link c    c   c.o -lcopyfile
    # As the tree's own C++ tools link: lld otherwise adds a load command for
    # libc++abi.dylib, which libc++ already reexports (ld64 does not).
    link cxx  cxx cxx.o -Wl,-no_implicit_dylibs
    link libs c   libs.o -lz -lbz2 -lsqlite3 -ledit -lcurses -lxml2 -licuuc

    # What each loads, judged on its own: libSystem, plus exactly what was asked.
    loads() { $OTOOL -L "$1" | tail -n +2 | sed -E 's/^[[:space:]]*//; s/ \(.*//' | sort | tr '\n' ' '; }
    want() { # binary expected...
      local b=$1; shift
      local got exp
      got=$(loads $b)
      exp=$(printf '%s\n' "$@" | sort | tr '\n' ' ')
      [ "$got" = "$exp" ] || { echo "sdk-link-test: $b loads [$got], expected [$exp]" >&2; exit 1; }
      for d in "$@"; do
        [ -e "${rootfs}$d" ] || { echo "sdk-link-test: $b loads $d, which the rootfs lacks" >&2; exit 1; }
      done
    }
    want sdk/c    /usr/lib/libSystem.B.dylib /usr/lib/libcopyfile.dylib
    want sdk/cxx  /usr/lib/libSystem.B.dylib /usr/lib/libc++.1.dylib
    want sdk/libs /usr/lib/libSystem.B.dylib /usr/lib/libz.1.dylib /usr/lib/libbz2.1.0.dylib \
      /usr/lib/libsqlite3.dylib /usr/lib/libedit.3.dylib /usr/lib/libncurses.5.4.dylib \
      /usr/lib/libxml2.2.dylib /usr/lib/libicuuc.76.dylib

    # No compile or link above may have reached for anything outside the SDK.
    echo "== nothing from the build machine"
    for b in sdk/*; do
      $OTOOL -L $b | grep -q /nix/store && { echo "sdk-link-test: $b names the store" >&2; exit 1; }
    done

    runHook postBuild
  '';

  installPhase = ''
    mkdir -p $out
    cp sdk/* $out/
  '';

  meta.description = "minidarwin SDK check: linking against its stubs equals linking against the rootfs";
}
