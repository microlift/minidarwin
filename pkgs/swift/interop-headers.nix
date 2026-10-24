# What libswiftCore's Objective-C interop includes and the SDK does not have:
# <objc/*.h> (objc4's Public headers), <CoreFoundation/*.h> (CF's Makefile
# PUBLIC_HEADERS), <asl.h> (syslog's libsystem_asl) and a Foundation.h of our
# own. Never into the SDK -- none of those libraries is built, so the runtime
# declares every symbol of theirs it imports as a hole (libswift-core.nix).
{ runCommand
, sources
}:

let
  # objc4's objc.xcodeproj, headers with ATTRIBUTES = (Public, ).
  objcHeaders = [
    "NSObjCRuntime.h"
    "NSObject.h"
    "message.h"
    "objc-api.h"
    "objc-auto.h"
    "objc-exception.h"
    "objc-sync.h"
    "objc.h"
    "runtime.h"
  ];
in

runCommand "swift-interop-headers" { } ''
  for h in ${toString objcHeaders}; do
    install -Dm644 ${sources.objc4}/runtime/$h $out/usr/include/objc/$h
  done

  # The Makefile's own list, so a header it does not install stays out.
  cf=$(sed -n 's/^PUBLIC_HEADERS=//p' ${sources.CF}/Makefile)
  [ -n "$cf" ] || { echo "swift-interop-headers: no PUBLIC_HEADERS in CF's Makefile" >&2; exit 1; }
  for h in $cf; do
    install -Dm644 ${sources.CF}/$h $out/usr/include/CoreFoundation/$h
  done

  install -Dm644 ${sources.syslog}/libsystem_asl.tproj/include/asl.h $out/usr/include/asl.h
  install -Dm644 ${./compat/Foundation/Foundation.h} $out/usr/include/Foundation/Foundation.h
''
