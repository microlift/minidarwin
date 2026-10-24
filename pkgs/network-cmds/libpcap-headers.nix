# <pcap/pcap.h> and its companions from libpcap, for traceroute and
# traceroute6. Only headers, and not into the SDK: libpcap is not built, so
# those tools declare the pcap_* functions they call in their allowUndefined,
# labelled `libpcap`. Nothing is compiled from that tarball.
{ runCommand
, sources
}:

runCommand "libpcap-headers-${sources.libpcap.rev}" { } ''
  install -Dm644 ${sources.libpcap}/libpcap/pcap.h $out/usr/include/pcap.h
  for h in ${sources.libpcap}/libpcap/pcap/*.h; do
    install -Dm644 $h $out/usr/include/pcap/$(basename $h)
  done
''
