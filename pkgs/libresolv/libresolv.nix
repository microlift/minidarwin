# Stage 6: /usr/lib/libresolv.9.dylib -- libresolv.xcodeproj target
# `libresolv` (Release, xcodescripts/libresolv.xcconfig): the BIND resolver
# (res_9_*, ns_*, dn_*, b64_*) and Apple's dns_* API over it. Installed as the
# target's phases install it: the dylib, links.sh's libresolv.dylib symlink,
# and manpages.sh's resolver.3/resolver.5 with their `.so` aliases.
#
# Its public headers (resolv.h, nameser.h, arpa/nameser.h, dns.h, dns_util.h,
# resolv_mt.h) are already in the SDK (sdk-headers.nix); the Private one
# (dns_private.h, to the internal SDK's /usr/local/include) is not installed.
#
# Two includes come from outside the project, as from the internal SDK:
#   * <md5.h> (hmac_link.c's HMAC-MD5 TSIG) is libmd's, whose MD5Init & co.
#     are #defines for CommonCrypto's CC_MD5_*, declared holes (libmd.nix).
#   * <dnsinfo.h> (dns.c, res_init.c: the resolver configuration configd
#     publishes) is configd's, copied to a local include dir, never the SDK.
#     Its dns_configuration_* functions are libsystem_info's on macOS.
#   * <dns_sd.h> (res_query.c) is mDNSResponder's public header, likewise;
#     DNSService* is libsystem_dnssd's, closed source.
#
# res_state.c gates per-thread resolver state on secondary threads on
# dyld_2024_SU_E_os_versions, a version set the released AvailabilityVersions
# (157.2, whose DSL ends at 2023_SU_D) does not define. Its value is not ours
# to invent, so the gate is taken as passed (as libxml2.nix does): every
# program built for this tree has minOS 26, past any 2024 set. A program with
# an older minOS gets the per-thread state too, not the shared _res.
# Not reproduced: apple-generic versioning's generated libresolv_vers.c (its
# symbols are VERSION_INFO_PREFIX'd hidden), and the OpenSourceVersions plist.
{ lib
, mkDarwinPackage
, sources
, toolchain
, libmd
, commonCryptoHeaders
}:

let
  srcs = import ./libresolv-sources.nix;

  # libresolv.xcconfig, with the target's WARNING_CFLAGS replacing its -Wall
  # and the project's HEADER_SEARCH_PATHS = $(SRCROOT).
  cflags = [
    "-std=gnu99" # GCC_C_LANGUAGE_STANDARD
    "-Os"
    "-fno-common" # OTHER_CFLAGS / GCC_NO_COMMON_BLOCKS
    "-Wall"
    "-Werror=format-nonliteral"
    "-Werror=format"
  ];

  # Imports nothing in the tree defines yet, by the libSystem member that
  # exports each on macOS 26 (Apple's libresolv.9.dylib imports the same,
  # flat, from libSystem).
  allowUndefined =
    # res_state.c's main-thread _res and res_data.c's h_errno; dns_async.c's
    # si_* (installapi.h's dns_async_*, over Libinfo's search modules); the
    # netdb/ifaddrs calls of res_debug.c, res_mkupdate.c and res_send.c.
    lib.genAttrs
      (map (s: "_${s}") [
        "_res"
        "h_errno"
        "si_async_call"
        "si_async_cancel"
        "si_async_handle_reply"
        "si_item_release"
        "si_module_release"
        "si_module_with_name"
        "endprotoent"
        "endservent"
        "freeaddrinfo"
        "freeifaddrs"
        "getaddrinfo"
        "getifaddrs"
        "getnameinfo"
        "getprotobynumber"
        "getprotoent"
        "getservbyport"
        "getservent"
        "if_indextoname"
        "if_nametoindex"
        "in6addr_any"
        "setprotoent"
        "setservent"
      ])
      (_: "system_info")
    # <dnsinfo.h>: configd's resolver configuration (libsystem_configuration).
    // lib.genAttrs [ "_dns_configuration_copy" "_dns_configuration_free" "_dns_configuration_notify_key" ]
      (_: "system_configuration")
    # dns.c/res_send.c watch the configuration and exit requests through notifyd.
    // lib.genAttrs
      (map (s: "_notify_${s}") [
        "cancel"
        "check"
        "get_state"
        "monitor_file"
        "register_check"
        "register_plain"
      ])
      (_: "system_notify")
    # res_query.c's mDNSResponder path.
    // lib.genAttrs
      (map (s: "_DNSService${s}") [
        "ProcessResult"
        "QueryRecord"
        "RefDeallocate"
        "RefSockFD"
      ])
      (_: "system_dnssd")
    # hmac_link.c's HMAC-MD5, through libmd's <md5.h> macros.
    // lib.genAttrs (map (f: "_CC_MD5_${f}") [ "Init" "Update" "Final" ]) (_: "commonCrypto");

  # manpages.sh: each an `.so man3/resolver.3` page.
  manAliases = [
    "dn_comp.3"
    "dn_expand.3"
    "dn_skipname.3"
    "ns_get16.3"
    "ns_get32.3"
    "ns_put16.3"
    "ns_put32.3"
    "res_init.3"
    "res_mkquery.3"
    "res_query.3"
    "res_search.3"
    "res_send.3"
  ];
in

mkDarwinPackage {
  pname = "libresolv";
  version = lib.removePrefix "libresolv-" sources.libresolv.rev;

  src = sources.libresolv;
  inherit toolchain;

  passthru.allowUndefined = allowUndefined; # for rootfs closure check
  passthru.installName = "/usr/lib/libresolv.9.dylib";

  postPatch = ''
    substituteInPlace res_state.c \
      --replace-fail '!dyld_program_minos_at_least(dyld_2024_SU_E_os_versions)' '0'
  '';

  buildPhase = ''
    runHook preBuild

    export MD_SRCROOT=$PWD
    mkdir -p obj

    install -Dm644 ${sources.configd}/dnsinfo/dnsinfo.h md-include/dnsinfo.h
    install -Dm644 ${sources.mDNSResponder}/mDNSShared/dns_sd.h md-include/dns_sd.h

    md_compile $PWD/obj "$CC" ${lib.escapeShellArgs cflags} \
      -I$PWD \
      -I$PWD/md-include \
      -I${libmd.headers}/usr/include \
      -I${commonCryptoHeaders}/usr/include \
      -- ${lib.concatMapStringsSep " " (f: "$PWD/${f}") srcs}

    # No EXPORTED_SYMBOLS_FILE: every extern the sources define is exported.
    md_dylib libresolv.9.dylib /usr/lib/libresolv.9.dylib obj \
      ${lib.escapeShellArgs (map (s: "-Wl,-U,${s}") (lib.attrNames allowUndefined))} \
      -lSystem

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall

    f=$out/usr/lib/libresolv.9.dylib
    install -Dm755 libresolv.9.dylib $f
    ln -s libresolv.9.dylib $out/usr/lib/libresolv.dylib

    install -Dm644 resolver.3 $out/usr/share/man/man3/resolver.3
    install -Dm644 resolver.5 $out/usr/share/man/man5/resolver.5
    for m in ${lib.escapeShellArgs manAliases}; do
      echo ".so man3/resolver.3" > $out/usr/share/man/man3/$m
      chmod 644 $out/usr/share/man/man3/$m
    done

    md_verify_pure   $f
    md_verify_signed $f
    # What the tree's consumers import (w, rsync), and installapi.h's
    # dns_async_* (declared by Libinfo's headers, defined here).
    md_verify_symbols $f _res_9_init ___res_9_state _res_9_b64_ntop _res_9_b64_pton \
      _res_9_query _res_9_search _res_9_dn_expand _dns_open _dns_search \
      _dns_async_start _dns_async_send _dns_async_receive _dns_async_handle_reply _dns_async_cancel

    deps=$($OTOOL -L $f | tail -n +2 | awk '{ print $1 }' | grep -vx /usr/lib/libresolv.9.dylib | sort -u)
    if [ "$deps" != /usr/lib/libSystem.B.dylib ]; then
      echo "libresolv: links other than libSystem:" >&2
      echo "$deps" >&2
      exit 1
    fi

    # A declared hole the dylib no longer imports is stale.
    $NM -u $f | awk '{ print $NF }' | sort -u > imports
    for s in ${lib.escapeShellArgs (lib.attrNames allowUndefined)}; do
      grep -qx -- "$s" imports || {
        echo "libresolv: declares $s absent but does not import it" >&2
        exit 1; }
    done

    runHook postInstall
  '';

  meta.description = "libresolv.9.dylib (BIND resolver and dns_* API), linked against minidarwin's libSystem";
}
