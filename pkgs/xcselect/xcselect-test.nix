# Behavioural tests against the host build (target binaries are never run
# in a build). Depending on `xcselect` makes `nix flake check` build the
# target too, with its export, load-command and purity checks.
{ runCommand, python3, openssl, curl, xcselect, xcselectBootstrap, installerBootstrap }:

runCommand "xcselect-test" { nativeBuildInputs = [ python3 openssl ]; } ''
  export XCSELECT=${xcselectBootstrap}
  export MDPKG=${installerBootstrap}/bin/mdpkg
  export XCSELECT_TEST_CURL=${curl}/bin/curl
  cd ${../../scripts}
  python3 test_xcselect.py -v
  mkdir -p $out
  ln -s ${xcselect} $out/target
''
