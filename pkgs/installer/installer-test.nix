# Behavioural tests run against the host build (target binaries are never
# executed in a build). Depending on `installer` makes `nix flake check`
# build the target binary too, with its load-command and purity checks.
{ runCommand, python3, openssl, installer, installerBootstrap, sources }:

# openssl makes the signature tests' certificate chain.
runCommand "mdpkg-test" { nativeBuildInputs = [ python3 openssl ]; } ''
  export MDPKG=${installerBootstrap}/bin/mdpkg
  export MC_PKG=${sources.midnightCommanderPkg}
  python3 ${../../scripts/test_mdpkg.py} -v
  test -s ${installer}/usr/share/licenses/mdpkg/QuickJS-LICENSE
  test -s ${installerBootstrap}/share/licenses/mdpkg/QuickJS-LICENSE
  mkdir -p $out
  ln -s ${installer} $out/target
''
