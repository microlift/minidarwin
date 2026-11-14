# Stage 6: rsync.xcodeproj target `openrsync` -- openrsync, installed as
# /usr/bin/rsync as on macOS 26 (see ../cmds/mk-cmds.nix). The project's
# samba rsync 2.6.9 target (`rsync`, product rsync.samba) installs only to
# /AppleInternal/Tests, as do the test helpers and the install-test-files.sh
# script phase, so none of them is built. Not reproduced: the OpenSourceVersions
# and OpenSourceLicenses copy phases, and rsyncd-entitlements.plist (an ad-hoc
# signature carries none; the target sets it only for non-macOS SDKs anyway).
#
# Per-file COMPILER_FLAGS are -Wno-* only and the target turns the project's one
# -Werror off, so they change nothing and are not passed.
{ lib
, mkCmds
, sources
, toolchain
, copyfile
, libutil
, libsbuf
, libmd
, commonCryptoHeaders
}:

let
  # main.c includes <os/variant_private.h> for os_variant_has_internal_content,
  # which libsystem_darwin defines (absent). The header is Libc's and is
  # self-contained; it is laid in a directory of its own, not the SDK.
  variantHeader = "md-include/os/variant_private.h";
in

mkCmds {
  pname = "rsync";
  src = sources.rsync;
  inherit toolchain;
  sourceLists = import ./rsync-sources.nix;

  postPatch = ''
    install -Dm644 ${sources.Libc}/os/variant_private.h ${variantHeader}
  '';

  tools.openrsync = {
    product = "rsync";
    # Frameworks phase: libresolv.9, libutil, libresolv, libsbuf. libresolv is
    # not built (its b64_ntop is a hole, below). copyfile(3) is a libSystem
    # member on macOS; here it is its own dylib, as file_cmds links it.
    libraries = [
      { pkg = copyfile; l = "copyfile"; }
      { pkg = libutil; l = "util"; }
      { pkg = libsbuf; l = "sbuf"; }
    ];
    cflags = [
      # extern.h reaches gettimeofday (client.c) and OSSwapHostToLittleInt32
      # (log.c) through <os/log.h>: on macOS that is libsystem_trace's, which
      # includes <mach-o/loader.h> (-> <libkern/OSByteOrder.h>) and, through
      # <os/trace_base.h>, <sys/time.h>. libsystem_trace was never released and
      # the SDK's <os/log.h> is xnu's, which includes neither -- so they are
      # included here, for what Apple's header brings in, not added to the source.
      "-include"
      "mach-o/loader.h"
      "-include"
      "sys/time.h"
      "-Imd-include"
      # md4.h maps MD4_* onto libmd's <md4.h> macros, which name CommonDigest's
      # CC_MD4_*; Apple's build does not link libmd for it (md4.h says so).
      "-I${libmd.headers}/usr/include"
      "-I${commonCryptoHeaders}/usr/include"
    ];
    man = {
      "openrsync/rsync.1" = "/usr/share/man/man1/rsync.1";
      "openrsync/openrsync.1" = "/usr/share/man/man1/openrsync.1";
      "openrsync/rsync.5" = "/usr/share/man/man5/rsync.5";
      "openrsync/rsyncd.5" = "/usr/share/man/man5/rsyncd.5";
      "openrsync/rsyncd.conf.5" = "/usr/share/man/man5/rsyncd.conf.5";
    };
    allowUndefined =
      # getpwnam/getgrgid (--usermap, daemon uid), getaddrinfo & co. (remote
      # shells' hosts, rsync://, --daemon).
      lib.genAttrs (map (s: "_${s}") [
        "freeaddrinfo" "gai_strerror" "getaddrinfo" "getgrgid" "getgrnam"
        "getnameinfo" "getpwnam" "getpwuid" "getservbyname" "in6addr_any"
      ]) (_: "system_info")
      # --log-file=syslog and the daemon's logging.
      // lib.genAttrs [ "_openlog" "_closelog" "_vsyslog" ] (_: "system_asl")
      # The trace log main.c opens and do_os_log writes to.
      // lib.genAttrs [ "_os_log_create" "__os_log_internal" ] (_: "system_trace")
      // {
        "_os_variant_has_internal_content" = "system_darwin"; # see variantHeader
        # <resolv.h>'s b64_ntop: daemon password challenges. libresolv's.
        "_res_9_b64_ntop" = "resolv";
      }
      # The block and file checksums: md4.h's MD4_* through libmd's macros.
      // lib.genAttrs (map (f: "_CC_MD4_${f}") [ "Init" "Update" "Final" ]) (_: "commonCrypto");
  };

  # Project-level Release settings (rsync.xcodeproj), with the target's
  # WARNING_CFLAGS replacing the project's -Werror=format-nonliteral.
  cflags = [
    "-std=gnu99" # GCC_C_LANGUAGE_STANDARD
    "-Os" # GCC_OPTIMIZATION_LEVEL (Release default)
    "-fno-common" # GCC_NO_COMMON_BLOCKS
    "-Wno-error=format-nonliteral" # WARNING_CFLAGS (target)
    "-Irsync" # HEADER_SEARCH_PATHS (project; the samba tree's)
    "-Irsync/popt"
  ];
  defines = [ "HAVE_CONFIG_H=1" ]; # GCC_PREPROCESSOR_DEFINITIONS
  ldflags = [ "-Wl,-dead_strip" ]; # DEAD_CODE_STRIPPING
}
