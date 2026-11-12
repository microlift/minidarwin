# Stage 7 release: the rootfs packed for distribution, with the manifest that
# describes it and the spec that pins both. Format: docs/rootfs-spec.md.
{ mkRelease, rootfs }:

mkRelease {
  kind = "rootfs";
  tree = rootfs;
  description = "minidarwin rootfs release: tarball, manifest, spec and bundle";
}
