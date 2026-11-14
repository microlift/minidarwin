# XZ Utils' liblzma as /usr/lib/liblzma.5.dylib, with macOS's install name and
# versions. Only the library is built: macOS ships no xz tools.
{ lib, mkDarwinPackage, sources, toolchain, gnumake }:

mkDarwinPackage {
  pname = "liblzma";
  version = "5.4.7";
  src = sources.xz;
  inherit toolchain;
  nativeBuildInputs = [ gnumake ];
  patches = [ sources.xzCve202531115 ];
  passthru.installName = "/usr/lib/liblzma.5.dylib";

  configurePhase = ''
    runHook preConfigure
    export PKG_CONFIG=false
    # configure defaults to -g, and shipped binaries carry no debug info.
    export CFLAGS=-O2
    ./configure \
      --build=x86_64-unknown-linux-gnu \
      --host=${toolchain.targetArch}-apple-darwin \
      --prefix=/usr \
      --disable-shared \
      --enable-static \
      --enable-threads=posix \
      --disable-xz \
      --disable-xzdec \
      --disable-lzmadec \
      --disable-lzmainfo \
      --disable-lzma-links \
      --disable-scripts \
      --disable-doc \
      --disable-nls
    runHook postConfigure
  '';

  buildPhase = ''
    runHook preBuild
    make -C src/liblzma -j''${NIX_BUILD_CORES:-1} liblzma.la
    # The export list is upstream's symbol map, which is exactly the set
    # macOS's liblzma.5.dylib exports.
    grep -o 'lzma_[a-z0-9_]*' src/liblzma/liblzma_generic.map | sort -u \
      | sed 's/^/_/' > liblzma.exports
    "$CC" -dynamiclib \
      -install_name /usr/lib/liblzma.5.dylib \
      -compatibility_version 6.0.0 -current_version 6.3.0 \
      -o liblzma.5.dylib \
      -Wl,-force_load,src/liblzma/.libs/liblzma.a \
      -Wl,-exported_symbols_list,liblzma.exports -lSystem
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 liblzma.5.dylib "$out/usr/lib/liblzma.5.dylib"
    ln -s liblzma.5.dylib "$out/usr/lib/liblzma.dylib"
    install -Dm644 src/liblzma/api/lzma.h "$out/usr/include/lzma.h"
    for h in src/liblzma/api/lzma/*.h; do
      install -Dm644 "$h" "$out/usr/include/lzma/$(basename "$h")"
    done
    install -Dm644 COPYING "$out/usr/share/licenses/liblzma/COPYING"

    exported=$($NM -gU "$out/usr/lib/liblzma.5.dylib" | awk '{ print $3 }' | sort)
    if [ "$exported" != "$(cat liblzma.exports)" ]; then
      echo "liblzma: exports differ from liblzma_generic.map" >&2
      diff <(echo "$exported") liblzma.exports >&2 || true
      exit 1
    fi
    md_verify_pure "$out/usr/lib/liblzma.5.dylib"
    md_verify_signed "$out/usr/lib/liblzma.5.dylib"
    runHook postInstall
  '';

  meta.description = "XZ Utils' liblzma";
}
