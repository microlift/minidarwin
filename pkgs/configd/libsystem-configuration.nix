# /usr/lib/system/libsystem_configuration.dylib -- configd.xcodeproj target
# `libsystem_configuration`: the client half of what configd publishes, the
# DNS configuration (dns_configuration_*), the network state (nwi_*) and the
# configuration agents (config_agent_*), each fetched from configd over XPC.
#
# libSystem re-exports it on macOS. Here it is packaged standalone, as
# copyfile is (absent-members.nix): every call it makes ends in libxpc, which
# Apple never released, so in this tree it is only useful to whatever
# provides libxpc at run time.
#
# Two includes are dropped rather than satisfied, because nothing they
# declare is used:
#   * <vproc.h> and <vproc_priv.h> in libSystemConfiguration_client.c,
#     launchd's, which no released project here provides;
#   * <SystemConfiguration/SCPrivate.h> in dnsinfo_internal.h, which it takes
#     "for SC_log": dnsinfo_copy.c defines its own my_log before including it,
#     and the framework's private headers need Security and CoreFoundation
#     headers newer than any release.
#
# Quoted includes reach across the project's directories, as Xcode's header
# map (USE_HEADERMAP) lets them; hence the -I per directory below.
{ lib
, mkDarwinPackage
, sources
, toolchain
, commonCryptoHeaders
}:

let
  srcs = import ./libsystem-configuration-sources.nix;

  # The project's Release settings: OTHER_CFLAGS and GCC_NO_COMMON_BLOCKS.
  cflags = [
    "-Os"
    "-fno-common"
    "-fblocks"
    "-fconstant-cfstrings"
    "-fstack-protector-all"
  ];

  headerDirs = [ "dnsinfo" "libSystemConfiguration" "nwi" "config-agent-info" "Plugins/IPMonitor" ];

  # What the target's OTHER_LDFLAGS link that this tree does not build, by
  # provider (absent-members.nix).
  allowUndefined =
    lib.genAttrs
      (map (s: "_xpc_${s}") [
        "array_apply"
        "connection_create_mach_service"
        "connection_get_pid"
        "connection_resume"
        "connection_send_message"
        "connection_send_message_with_reply_sync"
        "connection_set_context"
        "connection_set_event_handler"
        "connection_set_finalizer_f"
        "copy_description"
        "create_from_plist"
        "data_get_bytes_ptr"
        "data_get_length"
        "dictionary_create"
        "dictionary_get_data"
        "dictionary_get_string"
        "dictionary_get_value"
        "dictionary_set_int64"
        "dictionary_set_string"
        "dictionary_set_uint64"
        "dictionary_set_uuid"
        "get_type"
        "release"
      ]
      ++ map (s: "__xpc_${s}") [
        "error_connection_interrupted"
        "error_connection_invalid"
        "error_key_description"
        "type_array"
        "type_data"
        "type_dictionary"
        "type_error"
      ])
      (_: "xpc")
    // lib.genAttrs [ "_notify_check" "_notify_register_check" ] (_: "system_notify")
    // lib.genAttrs [ "__os_log_default" "__os_log_internal" ] (_: "system_trace")
    // { "_syslog$DARWIN_EXTSN" = "system_asl"; };
in

mkDarwinPackage {
  pname = "libsystem_configuration";
  version = lib.removePrefix "configd-" sources.configd.rev;

  src = sources.configd;
  inherit toolchain;

  passthru.allowUndefined = allowUndefined;
  passthru.installName = "/usr/lib/system/libsystem_configuration.dylib";

  postPatch = ''
    substituteInPlace libSystemConfiguration/libSystemConfiguration_client.c \
      --replace-fail '#include <vproc.h>' '/* MiniDarwin: <vproc.h> unused. */' \
      --replace-fail '#include <vproc_priv.h>' '/* MiniDarwin: <vproc_priv.h> unused. */'
    substituteInPlace dnsinfo/dnsinfo_internal.h \
      --replace-fail '#include <SystemConfiguration/SCPrivate.h>' \
        '/* MiniDarwin: <SystemConfiguration/SCPrivate.h> unused; see libsystem-configuration.nix. */'
  '';

  buildPhase = ''
    runHook preBuild

    export MD_SRCROOT=$PWD
    md_compile $PWD/obj "$CC" ${lib.escapeShellArgs cflags} \
      ${lib.concatMapStringsSep " " (d: "-I$PWD/${d}") headerDirs} \
      -I${./compat} \
      -I${commonCryptoHeaders}/usr/include \
      -- ${lib.concatMapStringsSep " " (f: "$PWD/${f}") srcs}

    # No EXPORTED_SYMBOLS_FILE: the project exports what it does not hide.
    md_dylib libsystem_configuration.dylib /usr/lib/system/libsystem_configuration.dylib obj \
      -Wl,-umbrella,System \
      ${lib.escapeShellArgs (map (s: "-Wl,-U,${s}") (lib.attrNames allowUndefined))} \
      -lSystem

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall

    f=$out/usr/lib/system/libsystem_configuration.dylib
    install -Dm755 libsystem_configuration.dylib $f

    md_verify_pure   $f
    md_verify_signed $f
    # What libresolv imports, and the network state API beside it.
    md_verify_symbols $f _dns_configuration_copy _dns_configuration_free \
      _dns_configuration_notify_key _nwi_state_copy _nwi_state_release

    # A declared hole the dylib no longer imports is stale.
    $NM -u $f | awk '{ print $NF }' | sort -u > imports
    for s in ${lib.escapeShellArgs (lib.attrNames allowUndefined)}; do
      grep -qx -- "$s" imports || {
        echo "libsystem_configuration: declares $s absent but does not import it" >&2
        exit 1
      }
    done

    runHook postInstall
  '';

  meta.description = "configd's client library: DNS configuration and network state over XPC";
}
