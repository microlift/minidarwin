# The SDK release: minidarwinSdk packed exactly as the rootfs is, by the same
# code (mk-release.nix), so a consumer verifies it with the same tool and the
# same trust chain. Format: docs/rootfs-spec.md, "SDK releases".
#
# Its identity names the rootfs it was made from by that rootfs's tree digest
# (`rootfs_tree`), read from the rootfs release's own manifest. A spec for one
# therefore pins the other: a build system that holds a trusted SDK spec knows
# which system its binaries were linked for, and can check a rootfs against it.
{ lib, mkRelease, minidarwinSdk, rootfsRelease, python3, mdrootfsScript }:

assert lib.assertMsg (minidarwinSdk.rootfs.outPath == rootfsRelease.tree.outPath)
  "sdk-release: the SDK was not made from the rootfs that is released";

mkRelease {
  kind = "sdk";
  tree = minidarwinSdk;
  extraIdentity.kind = "sdk";
  identityFromBuild.rootfs_tree = lib.concatStringsSep " " [
    "${python3}/bin/python3 -c"
    (lib.escapeShellArg ''
      import importlib.util, sys
      spec = importlib.util.spec_from_file_location("mdrootfs", sys.argv[1])
      md = importlib.util.module_from_spec(spec)
      spec.loader.exec_module(md)
      print(md.load_manifest(sys.argv[2]).tree)
    '')
    "${mdrootfsScript}"
    "${rootfsRelease}/${rootfsRelease.manifest}"
  ];
  description = "minidarwin SDK release: tarball, manifest, spec and bundle";
}
