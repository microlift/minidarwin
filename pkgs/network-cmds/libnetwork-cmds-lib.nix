# Stage 6: libnetwork_cmds_lib.a -- network_cmds.xcodeproj target
# `network_cmds_lib` (Release), the static helpers (in_cksum, dump_hex,
# proc_name, clean_non_printable) ping, traceroute and friends link. Never
# installed: the target's products are consumed in-project, so this is a
# build input only (passthru.headers holds network_cmds_lib.h). Its test
# program, network_cmds_lib_test.c, is not part of the library.
{ lib
, mkDarwinPackage
, sources
, toolchain
, runCommand
}:

let
  # Target network_cmds_lib: network_cmds_lib.c and gmt2local.c.
  sources' = import ./libnetwork-cmds-lib-sources.nix;
in

mkDarwinPackage {
  pname = "libnetwork_cmds_lib";
  version = lib.removePrefix "network_cmds-" sources.network_cmds.rev;

  src = sources.network_cmds;
  inherit toolchain;

  passthru.headers = runCommand "network_cmds_lib-headers-${sources.network_cmds.rev}" { } ''
    install -Dm644 ${sources.network_cmds}/network_cmds_lib/network_cmds_lib.h \
      $out/usr/include/network_cmds_lib.h
  '';

  buildPhase = ''
    runHook preBuild

    export MD_SRCROOT=$PWD
    mkdir -p obj

    md_compile $PWD/obj "$CC" -Os -Wall -Wformat-nonliteral \
      -- ${lib.concatMapStringsSep " " (f: "$PWD/${f}") sources'}
    md_archive libnetwork_cmds_lib.a obj

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm644 libnetwork_cmds_lib.a $out/usr/lib/libnetwork_cmds_lib.a
    runHook postInstall
  '';

  meta.description = "libnetwork_cmds_lib.a (static helpers for network_cmds)";
}
