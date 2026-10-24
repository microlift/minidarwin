# Stage 6: network_cmds targets (see ../cmds/mk-cmds.nix): ping, ping6,
# traceroute, traceroute6, ifconfig. The project's other tools (arp, ndp,
# netstat, route, rarpd, rtadvd, rtsol, spray, kdumpd, dnctl, ...) are not
# built yet.
#
# Not reproduced: set-id bits (traceroute and traceroute6 are 4555 as Apple
# installs them; the rootfs format has none, so 0755 and they cannot open a
# raw socket as an ordinary user) and the code-signing entitlements
# (network-management-entitlements.plist; an ad-hoc signature carries none).
# apple-generic versioning's generated <tool>_vers.c, as for shell_cmds.
{ lib
, mkCmds
, sources
, toolchain
, libnetworkCmdsLib
, libpcapHeaders
, systemFrameworkHeaders
}:

let
  lib' = { pkg = libnetworkCmdsLib; l = "network_cmds_lib"; };

  # traceroute and traceroute6 share traceroute.tproj's as/findsaddr/ifaddrlist
  # and look each other's headers up through Xcode's header map.
  tracerouteIncludes = [ "traceroute.tproj" ];

  # libpcap is not built; its headers come from the libpcap tarball.
  pcap = syms: lib.genAttrs (map (s: "_pcap_${s}") syms) (_: "libpcap");
  pcapUsed = pcap [
    "activate"
    "close"
    "compile"
    "create"
    "datalink"
    "setnonblock"
    "get_selectable_fd"
    "geterr"
    "next_ex"
    "set_buffer_size"
    "set_immediate_mode"
    "set_snaplen"
    "setfilter"
  ];

  # Libinfo (getaddrinfo, getifaddrs, ether_*, ...) is absent from the member
  # tree (absent-members.nix).
  info = syms: lib.genAttrs syms (_: "system_info");

  # Per target, what differs from the project's Release settings; the
  # attributes are described in mk-cmds.nix.
  tools = {
    ping = {
      installDir = "/sbin";
      archives = [ lib' ];
      man = { "ping.tproj/ping.8" = "/usr/share/man/man8/ping.8"; };
      allowUndefined = info [
        "_freeifaddrs" "_getifaddrs" "_gethostbyaddr" "_gethostbyname2"
        "_h_errno" "_hstrerror" "_if_nametoindex"
      ];
    };
    ping6 = {
      installDir = "/sbin";
      archives = [ lib' ];
      man = { "ping6.tproj/ping6.8" = "/usr/share/man/man8/ping6.8"; };
      allowUndefined = info [
        "_freeaddrinfo" "_gai_strerror" "_getaddrinfo" "_getnameinfo" "_if_nametoindex"
        "_inet6_opt_get_val" "_inet6_opt_next" "_inet6_rth_add" "_inet6_rth_getaddr"
        "_inet6_rth_init" "_inet6_rth_segments" "_inet6_rth_space"
      ];
    };
    traceroute = {
      installDir = "/usr/sbin";
      defines = projectDefines ++ [ "HAVE_SOCKADDR_SA_LEN" ];
      includes = tracerouteIncludes;
      archives = [ lib' ];
      man = { "traceroute.tproj/traceroute.8" = "/usr/share/man/man8/traceroute.8"; };
      allowUndefined = pcapUsed // info [
        "_gethostbyaddr" "_gethostbyname" "_getprotobyname" "_getservbyname"
        "_h_errno" "_hstrerror" "_if_indextoname"
      ];
    };
    traceroute6 = {
      installDir = "/usr/sbin";
      defines = projectDefines ++ [ "INET6" "IPSEC" ];
      includes = tracerouteIncludes;
      archives = [ lib' ];
      man = { "traceroute6.tproj/traceroute6.8" = "/usr/share/man/man8/traceroute6.8"; };
      # The ipsec_* calls are libipsec's (the `ipsec` project), which is not
      # built; the target's frameworks phase links it.
      allowUndefined = removeAttrs pcapUsed [ "_pcap_close" ] // info [
        "_freeaddrinfo" "_freeifaddrs" "_gai_strerror" "_getaddrinfo" "_gethostbyname"
        "_gethostbyname2" "_getifaddrs" "_getnameinfo" "_getservbyname" "_h_errno"
        "_hstrerror" "_inet6_rth_add" "_inet6_rth_init"
      ] // lib.genAttrs [ "_ipsec_get_policylen" "_ipsec_set_policy" "_ipsec_strerror" ] (_: "libipsec");
    };
    ifconfig = {
      installDir = "/sbin";
      # $(inherited), then USE_VLANS and USE_BONDS for macosx.
      defines = projectDefines ++ [ "USE_IF_MEDIA" "NO_IPX" "INET6" "USE_VLANS" "USE_BONDS" ];
      man = { "ifconfig.tproj/ifconfig.8" = "/usr/share/man/man8/ifconfig.8"; };
      allowUndefined = info [
        "_ether_aton" "_ether_ntoa" "_freeaddrinfo" "_freeifaddrs" "_gai_strerror"
        "_getaddrinfo" "_gethostbyname" "_getifaddrs" "_getnameinfo" "_getnetbyname"
        "_if_indextoname" "_if_nametoindex" "_in6addr_any"
      ];
    };
  };

  projectDefines = [
    "USE_RFC2292BIS=1"
    "__APPLE_USE_RFC_3542=1"
    "__APPLE_API_OBSOLETE=1"
  ];
in

mkCmds {
  pname = "network_cmds";
  src = sources.network_cmds;
  inherit toolchain tools;
  sourceLists = import ./network-cmds-sources.nix;

  # Project-level Release settings (network_cmds.xcodeproj).
  cflags = [
    "-Os" # GCC_OPTIMIZATION_LEVEL (Release default)
    "-Wall"
    "-Wformat-nonliteral" # WARNING_CFLAGS
    "-Inetwork_cmds_lib" # HEADER_SEARCH_PATHS
    "-I${libpcapHeaders}/usr/include"
    # The tools' HEADER_SEARCH_PATHS: <net/if_var.h> and friends carry their
    # PRIVATE companions only in this rendering (CLAUDE.md, "Two renderings of xnu").
    "-iwithsysroot"
    systemFrameworkHeaders
  ];
  defines = projectDefines;
  ldflags = [ "-Wl,-dead_strip" ]; # DEAD_CODE_STRIPPING

  # ifconfig's `model iod|fpd` for netem: xnu's released if_netem_model_t has
  # IF_NETEM_MODEL_NLC and nothing else, so IOD and FPD (in-order and fast
  # packet delivery) are declared nowhere we have. Their values are the
  # kernel's to say, not ours to invent, so the two model names are dropped
  # and ifconfig accepts only `nlc`.
  postPatch = ''
    sed -i ifconfig.tproj/ifconfig.c \
      -e '/strcmp(\*argv, "iod") == 0/,+3d' \
      -e '/case IF_NETEM_MODEL_IOD:/,+2d' \
      -e '/case IF_NETEM_MODEL_FPD:/,+2d'
    if grep -q 'NETEM_MODEL_\(IOD\|FPD\)' ifconfig.tproj/ifconfig.c; then
      echo "ifconfig.c: the netem model patch no longer applies" >&2; exit 1; fi
  '';
}
