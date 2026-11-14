# The sysroot Swift modules compile against: the toolchain's SDK with the
# Clang module maps Swift's importer needs to `import Darwin`, `os`, `MachO`
# and `ObjectiveC` (sdk/module.modulemap), <sysdir.h>, the clock-id apinotes,
# and objc4's public headers. A sysroot of its own rather than an SDK change:
# nothing compiled as C needs any of it, and the SDK's bytes are every
# member's input.
{ runCommand
, toolchain
, swiftInteropHeaders
}:

runCommand "minidarwin-swift-sdk" { } ''
  cp -R ${toolchain.sysroot}/. $out
  chmod -R u+w $out
  inc=$out/usr/include

  # Libinfo's rpc map, copied raw into usr/include: `umbrella "."` there would
  # claim every header beside it for `rpc`, and its #ifdef is not module-map
  # syntax, so no module under usr/include could be built at all.
  grep -q '^module rpc ' $inc/module.modulemap || {
    echo "swift-sdk: usr/include/module.modulemap is no longer Libinfo's rpc map" >&2
    exit 1; }
  rm $inc/module.modulemap

  for f in module.modulemap Darwin.apinotes _DarwinUmbrella.h sysdir.h; do
    [ ! -e $inc/$f ] || { echo "swift-sdk: $f would overwrite the SDK's" >&2; exit 1; }
    install -m444 ${./sdk}/$f $inc/$f
  done
  [ ! -e $inc/objc ] || { echo "swift-sdk: the SDK has <objc/*.h> now" >&2; exit 1; }
  cp -R ${swiftInteropHeaders}/usr/include/objc $inc/objc
''
