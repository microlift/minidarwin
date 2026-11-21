# Stage 6: crontabs.xcodeproj (see ../cmds/mk-cmds.nix) -- newsyslog, the
# periodic(8) script and the configuration the `files` aggregate installs.
#
# Not built: the periodic target's compiled half, /usr/libexec/periodic-wrapper.
# It is an XPC activity client and nothing else (xpc_activity_register,
# xpc_track_activity, vproc_swap_integer, asl_log, CoreAnalytics) -- libxpc
# is closed source, and so are its headers. The three
# com.apple.periodic-{daily,weekly,monthly} launchd jobs name only it, so
# they are left out too, as is its man page. periodic(8) itself is a script
# and is installed; run it from cron or by hand.
#
# Installed but not yet run by minidarwin's launchd: com.apple.newsyslog.plist
# uses StartCalendarInterval, LowPriorityIO and Nice, which it does not
# support; it logs and skips the job.
#
# Not reproduced: the OpenSourceVersions/OpenSourceLicenses files (none).
{ mkCmds
, sources
, toolchain
}:

mkCmds {
  pname = "crontabs";
  src = sources.crontabs;
  inherit toolchain;
  sourceLists = import ./crontabs-sources.nix;

  tools = {
    newsyslog = {
      installDir = "/usr/sbin";
      cflags = [ "-Wall" "-W" "-Werror" ]; # WARNING_CFLAGS
      man = {
        "newsyslog/newsyslog.8" = "/usr/share/man/man8/newsyslog.8";
        "newsyslog/newsyslog.conf.5" = "/usr/share/man/man5/newsyslog.conf.5";
      };
      allowUndefined = {
        "_getpwnam" = "system_info";
        "_getgrnam" = "system_info";
        "_environ" = "dyld"; # posix_spawn of the compressor; see shell_cmds' env
      };
    };
  };

  # Project-level Release settings (crontabs.xcodeproj): no language
  # standard or definitions.
  cflags = [ "-Os" ]; # GCC_OPTIMIZATION_LEVEL (Release default)
  defines = [ ];
  ldflags = [ "-Wl,-dead_strip" ]; # DEAD_CODE_STRIPPING

  extraInstall = ''
    # periodic: "Install periodic.sh" (install_executable.sh) and its man page.
    install -Dm755 periodic/periodic.sh $out/usr/sbin/periodic
    install -Dm644 periodic/periodic.8 $out/usr/share/man/man8/periodic.8

    # files: its copy phases, then "Fix Permissions" (chmod +x on the
    # periodic scripts).
    install -Dm644 files/newsyslog.conf $out/private/etc/newsyslog.conf
    mkdir -p $out/private/etc/newsyslog.d
    install -Dm644 files/periodic.conf $out/private/etc/defaults/periodic.conf
    install -Dm644 files/periodic.conf.5 $out/usr/share/man/man5/periodic.conf.5
    install -Dm644 files/LaunchDaemons/com.apple.newsyslog.plist \
      $out/System/Library/LaunchDaemons/com.apple.newsyslog.plist
    for p in daily weekly monthly; do
      install -Dm755 files/$p/999.local $out/private/etc/periodic/$p/999.local
    done
  '';
}
