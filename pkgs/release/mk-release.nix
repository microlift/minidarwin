# A release: a tree packed for distribution, with the manifest that describes
# it and the spec that pins both. Format: docs/rootfs-spec.md. Used for the
# rootfs (rootfs-release.nix) and the SDK (sdk-release.nix); the two differ
# only in the tree and in `identity`, which is what keeps one kind's spec from
# ever verifying the other's artifact.
#
# Host world (nothing here is compiled for the target), and reproducible like
# everything else: every file in $out is what a published release carries,
# byte for byte, except that the published spec is *stamped* with the tag,
# commit and download URLs -- which changes none of the hashes it holds.
{ lib
, stdenvNoCC
, python3
, gnutar
, gzip
, sources
, llvmVersion
, targetArch
, minOS
, release
, mdrootfsScript
}:

{ kind
  # The tree to release, at the paths it is installed at.
, tree
  # Added to the common identity; must not redefine any of it.
, extraIdentity ? { }
  # Identity values only known at build time: KEY=COMMAND, run in the build,
  # whose stdout (one line) is the value.
, identityFromBuild ? { }
, description
}:

let
  name = "minidarwin-${kind}-${targetArch}-r${toString release.sequence}";

  baseIdentity = {
    project = "minidarwin";
    arch = targetArch;
    min_os = minOS;
    libsystem = sources.Libsystem.rev;
    xnu = sources.xnu.rev;
  };

  identity =
    assert lib.assertMsg
      (lib.intersectLists (lib.attrNames baseIdentity)
        (lib.attrNames extraIdentity ++ lib.attrNames identityFromBuild) == [ ])
      "mk-release: ${kind} redefines a common identity key";
    baseIdentity // extraIdentity;

  # Every pinned Apple source, with the hash that pins it: the whole OS train,
  # not only what this tree happens to use so far.
  appleSources = lib.mapAttrsToList (n: s: "${n}=${s.rev}=${s.outputHash}")
    (lib.filterAttrs (_: s: s ? rev) sources);
in

stdenvNoCC.mkDerivation {
  pname = "minidarwin-${kind}-release";
  version = "r${toString release.sequence}";

  dontUnpack = true;
  dontFixup = true;

  nativeBuildInputs = [ python3 gnutar gzip ];

  SOURCE_DATE_EPOCH = "1";
  TZ = "UTC";
  LC_ALL = "C";

  buildPhase = ''
    runHook preBuild
    mkdir -p $out
    md() { python3 ${mdrootfsScript} "$@"; }
    buildIdentity=()

    # The tree is already reproducible; the tarball must add nothing of its
    # own. Sorted names, no owner, the epoch the tree was built at, and the
    # canonical permissions (the store's 0555/0444 made owner-writable, which
    # is 0755/0644). --hard-dereference because `nix store optimise` may have
    # hardlinked two identical files, and whether it has is not the tree's
    # business. GNU format: nothing in it depends on atime or ctime.
    tar --sort=name --format=gnu \
        --numeric-owner --owner=0 --group=0 \
        --mtime=@1 --mode=u+w,go-w \
        --hard-dereference \
        -C ${tree} -cf - . | gzip -9n > $out/${name}.tar.gz

    ${lib.concatStrings (lib.mapAttrsToList (k: cmd: ''
    v=$(${cmd})
    [ -n "$v" ] || { echo "mk-release: identity ${k} is empty" >&2; exit 1; }
    buildIdentity+=(--identity "${k}=$v")
    '') identityFromBuild)}
    md manifest ${tree} -o $out/${name}.manifest.yaml \
      ${lib.concatMapStringsSep " " (k: "--identity ${lib.escapeShellArg "${k}=${identity.${k}}"}") (lib.attrNames identity)} \
      ''${buildIdentity[@]+"''${buildIdentity[@]}"} \
      --llvm ${lib.escapeShellArg llvmVersion} \
      ${lib.concatMapStringsSep " " (s: "--apple-source ${lib.escapeShellArg s}") appleSources}

    md spec --sequence ${toString release.sequence} \
      --manifest $out/${name}.manifest.yaml \
      --artifact $out/${name}.tar.gz \
      -o $out/${name}.spec.yaml

    md bundle -o $out/${name}.bundle.zip \
      --spec $out/${name}.spec.yaml \
      --manifest $out/${name}.manifest.yaml \
      --artifact $out/${name}.tar.gz

    # The manifest was made from the store path and the tarball by tar; this
    # is where the two are checked against each other, through the same
    # hardened extraction a consumer uses.
    md verify --spec $out/${name}.spec.yaml \
      --manifest $out/${name}.manifest.yaml \
      --artifact $out/${name}.tar.gz
    md verify --bundle $out/${name}.bundle.zip --spec $out/${name}.spec.yaml

    runHook postBuild
  '';

  dontInstall = true;

  passthru = {
    inherit identity kind tree;
    releaseName = name;
    spec = "${name}.spec.yaml";
    manifest = "${name}.manifest.yaml";
    artifact = "${name}.tar.gz";
    bundle = "${name}.bundle.zip";
  };

  meta.description = description;
}
