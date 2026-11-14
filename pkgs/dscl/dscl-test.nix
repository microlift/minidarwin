# Behavioural tests against the host build. Depending on `dscl` makes
# `nix flake check` build the target binaries too, with their load-command
# and purity checks.
{ runCommand, python3, dscl, dsclBootstrap }:

runCommand "dscl-test" { nativeBuildInputs = [ python3 ]; } ''
  export DSCL=${dsclBootstrap}/bin/dscl DSEDITGROUP=${dsclBootstrap}/bin/dseditgroup
  python3 ${../../scripts/test_dscl.py} -v
  mkdir -p $out
  ln -s ${dscl} $out/target
''
