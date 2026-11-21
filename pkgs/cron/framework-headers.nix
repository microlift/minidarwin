# Only headers, private to cron's build, for do_command.c's battery check
# (`@AppleNotOnBattery` entries: IOPMCopyBatteryInfo over CF). The
# framework implementations are absent, so every CF/IOKit call cron makes is a
# declared hole (cron.nix). The SDK already has xnu's IOKit headers
# (IOTypes.h, IOReturn.h, pwr_mgt/IOPM.h, IOPMLibDefs.h); this adds the
# IOKitUser half and CoreFoundation.
{ runCommand, sources }:

runCommand "cron-framework-headers" { } ''
  mkdir -p $out/CoreFoundation $out/IOKit/pwr_mgt
  cp ${sources.CF}/*.h $out/CoreFoundation/
  chmod u+w $out/CoreFoundation/CoreFoundation.h
  # As for top: the full umbrella reaches CFStream/CFBundle and unreleased SDK
  # headers. These are what do_command.c and IOKit's headers use.
  cat > $out/CoreFoundation/CoreFoundation.h <<'EOF'
  #include <CoreFoundation/CFBase.h>
  #include <CoreFoundation/CFArray.h>
  #include <CoreFoundation/CFData.h>
  #include <CoreFoundation/CFDictionary.h>
  #include <CoreFoundation/CFNumber.h>
  #include <CoreFoundation/CFString.h>
  EOF
  cp ${sources.IOKitUser}/IOKitLib.h $out/IOKit/
  cp ${sources.IOKitUser}/pwr_mgt.subproj/IOPMLib.h \
     ${sources.IOKitUser}/pwr_mgt.subproj/IOPMKeys.h $out/IOKit/pwr_mgt/
''
