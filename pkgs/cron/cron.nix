# Stage 6: cron.xcodeproj (see ../cmds/mk-cmds.nix) -- /usr/sbin/cron,
# /usr/bin/crontab, and what the cron target's "Install misc" phase
# (xcodescripts/install_misc.sh) and copy phases put beside them.
#
# libcron (lib/entry.c, env.c, misc.c) is a static library both tools link;
# here its sources are compiled into each tool instead, as top does with
# libtop. With -dead_strip the result is what linking the archive gives.
#
# cron.patch guards closed or unreleased dependencies with __has_include,
# and nothing else:
#   - <xpc/xpc.h> and CoreAnalytics: the job-count analytics event is not sent;
#   - <btm.h>/<os/feature_private.h> (BackgroundTaskManagement): jobs run
#     unconditionally, as with the cronBTMToggle feature flag off;
#   - launchd's <vproc_priv.h>/<bootstrap_priv.h> (last released with
#     launchd-842): a job keeps cron's Mach bootstrap rather than moving to
#     the user's -- minidarwin's launchd has no per-user bootstrap anyway.
# The `@AppleNotOnBattery` check is kept: it compiles against
# CoreFoundation's and IOKitUser's released headers (framework-headers.nix),
# and its calls are declared holes.
#
# Not reproduced: crontab's set-user-id bit (INSTALL_MODE_FLAG 04755; the
# rootfs format has none, so crontab can edit only what the caller may
# write), cron's entitlement (com.apple.private.backgroundtaskmanagement.manage,
# which an ad-hoc signature cannot carry), the 0700 modes and daemon owner
# of /private/var/at/{tabs,tmp} (the format's directories are 0755 root),
# the cronBTMToggle feature-flag plist (the BTM check is compiled out), and
# the OpenSourceVersions/OpenSourceLicenses files.
{ lib
, callPackage
, mkCmds
, sources
, toolchain
, libutil
, commonCryptoHeaders
}:

let
  frameworkHeaders = callPackage ./framework-headers.nix { };
  generated = import ./cron-sources.nix;

  # Imports both tools make into libraries the tree does not have yet.
  common = {
    "_getpwnam" = "system_info";
    "_endpwent" = "system_info";
    "_syslog$DARWIN_EXTSN" = "system_asl"; # log_it
    "_openlog" = "system_asl";
  };
in

mkCmds {
  pname = "cron";
  src = sources.cron;
  inherit toolchain;

  # libcron's sources into each tool that links it (both do).
  sourceLists = {
    cron = generated.cron ++ generated.libcron;
    crontab = generated.crontab ++ generated.libcron;
  };

  postPatch = ''
    patch -p1 < ${./cron.patch}
  '';

  tools = {
    cron = {
      installDir = "/usr/sbin";
      # Its sources live there; libcron's are found through it
      # (libcron's USER_HEADER_SEARCH_PATHS = $(SRCROOT)/cron).
      includes = [ "cron" ];
      cflags = [ "-I${frameworkHeaders}" ];
      # libutil.dylib from the frameworks phase (pidfile_*). CoreFoundation
      # and IOKit are its holes; BackgroundTaskManagement and CoreAnalytics
      # are not linked, their callers being compiled out.
      libraries = [{ pkg = libutil; l = "util"; }];
      man = { "cron/cron.8" = "/usr/share/man/man8/cron.8"; };
      allowUndefined = common // {
        "_getgrnam" = "system_info";
        "_initgroups" = "system_info"; # each job's groups
        "_closelog" = "system_asl";
        "_CFArrayGetValueAtIndex" = "CoreFoundation";
        "_CFDictionaryGetValue" = "CoreFoundation";
        "_CFNumberGetValue" = "CoreFoundation";
        "___CFConstantStringClassReference" = "CoreFoundation";
        "_IOPMCopyBatteryInfo" = "IOKit";
        "_kIOMainPortDefault" = "IOKit";
      };
    };
    crontab = {
      includes = [ "cron" ];
      # crontab.c computes MD5s of the crontab before and after editing.
      cflags = [ "-I${commonCryptoHeaders}/usr/include" ];
      man = {
        "crontab/crontab.1" = "/usr/share/man/man1/crontab.1";
        "crontab/crontab.5" = "/usr/share/man/man5/crontab.5";
      };
      allowUndefined = common // {
        "_getpwuid" = "system_info"; # the invoking user
        "_CC_MD5" = "commonCrypto";
      };
    };
  };

  # Project-level Release settings (cron.xcodeproj): no language standard,
  # definitions or warning flags that fail the build.
  cflags = [ "-Os" ]; # GCC_OPTIMIZATION_LEVEL (Release default)
  defines = [ ];
  ldflags = [ "-Wl,-dead_strip" ]; # DEAD_CODE_STRIPPING

  extraInstall = ''
    install -Dm644 com.vix.cron.plist $out/System/Library/LaunchDaemons/com.vix.cron.plist
    # install_misc.sh
    install -Dm644 cron.deny $out/private/var/at/cron.deny
    mkdir -p $out/private/var/at/tabs $out/private/var/at/tmp
    # CRONDIR (cron/pathnames.h). macOS ships this link but no released
    # project installs it; Apple's is ../../var/at, which needs the /var link
    # the rootfs does not have, so this one goes through private/.
    mkdir -p $out/usr/lib
    ln -s ../../private/var/at $out/usr/lib/cron
  '';
}
