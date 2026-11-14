# mdpkg

`mdpkg` installs flat `.pkg` files into a directory tree: the running root (`/`),
which is how Rosetta 3 uses it on its MiniDarwin base system, or an offline
tree, typically a writable copy of the MiniDarwin rootfs. It uses libxml2,
zlib, liblzma, LibreSSL/OpenSSL and an embedded QuickJS engine, not
CoreFoundation or Apple's Installer.

Options are spelled as in macOS's `installer`, with one dash:

```
mdpkg -pkg FILE -target DIRECTORY [-script-runner EXECUTABLE]
mdpkg -dist FILE -pkg-dir DIRECTORY -target DIRECTORY [...]
mdpkg -pkginfo -pkg FILE [-target DIRECTORY]
mdpkg -vers
```

`-verify-signature apple-software-update` (any form) refuses a package that
is not signed as Apple's Software Update packages are (see "Signatures").

Two further options exist for packages written for a macOS that is not the
host: `-system-version X.Y` and `-skip-scripts` (see "MacPorts" below).

`-root` is a synonym of `-target` (`-target` is the macOS name). The older
`mdpkg install ...` and `mdpkg inspect ...` forms take the same options.

There are two builds of the same sources:

- `nix build .#installer` is `/usr/bin/mdpkg` in the rootfs, built with the
  MiniDarwin toolchain against MiniDarwin's own libraries.
- `nix build .#installerBootstrap` (or `nix run .#mdpkg`) is a host build that
  also provides `libexec/mdpkg-sandbox-runner`. MiniDarwin has no dyld yet,
  so this is the build that can actually populate a root today.

## What is supported

- **Container:** XAR version 1, members stored raw or zlib-compressed. The
  TOC checksum and the archived and extracted checksums (SHA-1, MD5, SHA-256
  or SHA-512) of every member used are verified. The file is mapped and a
  member is decoded when first read; stored members are used in place.
  Checksums detect corruption; only a signature policy (below) establishes
  who made the package.
- **Packages:** a bare component package, or a product archive whose
  Distribution selects embedded components (`#name.pkg`
  references, percent-encoded or not). `hostArchitectures` is shown by
  `inspect` and not enforced: the root's architecture is not the host's.
- **Payloads:** raw, gzip or pbzx CPIO in odc, newc or crc format, holding
  regular files, directories and symlinks. A payload is read as a stream and
  its file contents are spooled to an unlinked file in `$TMPDIR` (else
  `/private/var/tmp`, `/tmp`), so it never has to fit in memory. pbzx, the
  Software Update format, is chunks that are each an .xz stream, or stored
  when the chunk's two lengths are equal.
- **Software Update products:** `-dist FILE -pkg-dir DIRECTORY` installs a
  standalone Distribution whose `pkg-ref`s name component packages in
  `DIRECTORY` by plain file name, as Apple's Software Update products do.
- **Scripts:** `preflight`, `preinstall`, `postinstall` and `postflight`.
  In the running root they are run directly; in an offline root, through a
  script runner (below).
- **Installer JS:** inline Distribution scripts, installation/volume checks,
  choice expressions and package activation, using the subset described below.

Refused, before the root is touched: other compressions (bzip2, lzfse),
external Distribution scripts and package references, hardlinks, device nodes,
FIFOs, set-id bits, AppleDouble files, DTDs in package XML, and unknown
PackageInfo or Distribution elements. XML plist reads have the separate,
restricted DOCTYPE handling described below.

`PackageInfo`'s `<groups>` (package groups, as `pkgutil --groups` lists them)
is recorded in the mdpkg inventory as `PackageGroups`; Apple's receipt plist
has no key for it, so the receipt is left as Apple writes it.

`PackageInfo`'s `numberOfFiles` must count either every payload entry (as
`pkgbuild` writes it) or only its regular files (as Apple's Software Update
packages do).

## Signatures

`-verify-signature POLICY` checks every package mdpkg opens. The TOC must
carry one RSA signature of the TOC checksum (PKCS#1 v1.5, with the
checksum's digest), and the signing certificate, the first in the TOC, must
chain through the TOC's other certificates to the policy's anchor. Only the
anchor is trusted, and the TOC's own copy of it counts for nothing.

- `apple-software-update`: anchor Apple Root CA (compiled in from
  `security_certificates`), and the signing certificate must have the Apple
  Software Update Signing usage, `1.2.840.113635.100.4.1`. Apple's
  Command Line Tools packages verify under it.

`-pkginfo` prints the verified signer. The host build (`bootstrap.nix`, built
with `MDPKG_TESTING`) also takes `MDPKG_TEST_SIGNATURE_ANCHOR`, a DER
certificate that replaces the anchor, for the tests' own chain; the rootfs
build has no such override.

## MacPorts

The MacPorts 2.12.6 package for macOS 15 (`sources.macportsPkg`, a product
archive with a gzip CPIO payload of 1,693 entries, a `postinstall` and a
Distribution `installation-check`) installs its payload with:

```
mdpkg -pkg MacPorts-2.12.6-15-Sequoia.pkg -target ROOT \
      -system-version 15.6 -skip-scripts
```

Two things in that package are about macOS 15, not about the format, and mdpkg
does not guess at either:

- **`-system-version X.Y`.** The package's check refuses any
  `system.version.ProductVersion` outside 15.x, and `system.version` is the
  host's `SystemVersion.plist` (a MiniDarwin root has none, a macOS 26 host
  says 26). The option answers `system.version.ProductVersion` with the given
  value, and nothing else: `my.target.systemVersion` is still read from the
  target, and `system.sysctl` from the host. The package's
  `<allowed-os-versions>` is parsed and reported by `-pkginfo` but, like
  `hostArchitectures`, not enforced: the check script is what enforces it.
- **`-skip-scripts`.** The `postinstall` creates the `macports` user with
  `/usr/bin/dscl` and `dseditgroup`, edits the installing user's shell profile
  through `su`, and then runs the just-installed `tclsh8.6` and `port
  selfupdate` (network). In an offline root the script's absolute
  `/opt/local` and `/usr/bin` paths name the host's.
  With the sandbox runner it fails at `dscl` and the install rolls back, which
  is the right outcome and the reason the option exists. `-skip-scripts`
  installs payloads and receipts only; each skipped script is named on stderr.
  The configuration files the script would copy from `*.conf.default`, the
  `macports` user and the shell profile are left to the administrator.

Checks (`installation-check`, `volume-check`) are evaluated as scripts, so a
statement such as `script="InstallationCheck();"` works; a check written as a
bare expression still does. `Warn` results, such as MacPorts' missing
`/usr/bin/xcodebuild`, are printed and the install continues.

The payload is only installed, not run: MacPorts' binaries need a working dyld
and, for its Tcl, libraries MiniDarwin has not built.

### In the running root, under Rosemary

In a Rosemary prefix built from this rootfs, the package installs into the
running root with its `postinstall`, as root:

```
ROSEMARY_DARWIN_RELEASE=25.0.0 rosemary run /usr/bin/su root -c \
  "/usr/bin/mdpkg -pkg /tmp/MacPorts-2.12.7-26-Tahoe.pkg -target /"
```

(`25.0.0` makes the prefix report macOS 26, which the Tahoe package
requires.) The script creates the `macports` group and user with
[`dseditgroup` and `dscl`](../dscl/README.md), copies the configuration
files, adds `/opt/local/bin` to the installing user's shell profile, and runs
`port selfupdate`, which syncs the ports tree with `/usr/bin/rsync`
([openrsync](../rsync/rsync.nix), as on macOS).

## Installer JavaScript

Both builds embed the same pinned [QuickJS](../quickjs/README.md) engine.
The object model follows [Apple's Installer JS documentation](https://developer.apple.com/documentation/installer_js)
within this supported subset:

- `system.localizedString(key)` looks the key up in the Distribution's
  English `<localization><strings>` table (Apple's `.strings` syntax). A
  missing key returns itself, as Cocoa's lookups do; Installer's own answer
  is undocumented.
- `system.log(text)` prints `JS:` messages to stderr. `propertiesOf(object)`
  returns its own enumerable string property names. `compareVersions(a, b)`
  compares numeric dotted versions, ignoring leading zeros and treating omitted
  trailing components as zero; other version syntax is an error.
- `system.version` reads the host's
  `/System/Library/CoreServices/SystemVersion.plist`. `system.sysctl(name)`
  supports `hw.machine`, `hw.model`, `hw.ncpu`, `hw.memsize`, `hw.optional.*`,
  `kern.osrelease` and `kern.osversion`, returning strings or numbers as appropriate.
  Unsupported selectors and unavailable queries are errors.
- `my.target.mountpoint`, `availableKilobytes`, `systemVersion` and
  `receiptForIdentifier(id)` describe the target tree. Available space is in
  kilobytes (1024 bytes); OS version and receipts are read from that tree,
  without falling back to the host. Missing plists/receipts return `null`.
- `system.files.fileExistsAtPath(path)`, `plistAtPath(path)` and
  `bundleAtPath(path)` read absolute host paths. Bundle lookup reads
  `Contents/Info.plist`, then `Info.plist`. Paths entering the target tree,
  including through aliases of its parent directories, follow the target's
  symlink-confinement rules. File existence returns `false` for missing paths;
  missing plists/bundles return `null`.
- XML plists support dictionaries, arrays, strings, signed integers, finite
  real numbers, booleans, UTC dates (`Date`) and base64 data (`Uint8Array`).
  The canonical Apple plist DOCTYPE is accepted without loading it. Binary
  plists, entity declarations, malformed values and duplicate keys are errors.

A `volume-check` holding `<allowed-os-versions>` is enforced: the target's
`SystemVersion.plist` `ProductVersion` must be in one of its `[min, before)`
ranges, and a target without one is refused. Such a check needs no `script`.
An `<allowed-os-versions>` elsewhere is reported by `-pkginfo` and not enforced.
Software Update's `tags`, `platforms` and `localization` elements are
accepted; of several `choices-outline`s, the one without a `ui` attribute
(Installer's) is followed, and `ui` values may not repeat.

The inline `<script>` (escaped XML text or CDATA) executes once. Its globals
are shared by subsequent checks and expressions. Installation checks run
before volume checks; `my.result.type`, `title` and `message` reset between
them. A false result stops installation unless the type is `Warn`, in which
case mdpkg prints the warning and continues. Fatal errors include the check
and its title/message. Evaluation happens before staging or installation
writes; offline locking and interrupted-transaction recovery still happen first.

`choices[id]` exposes `selected`, `enabled`, `visible`, `title`, `description`
and `packages` (identifier/version metadata). Literal `start_*` flags initialize
state. Supplied choice expressions evaluate with `my` as the current choice,
also carrying `my.target` and `my.result`. Choices reevaluate in outline order
until stable, with a maximum of 64 passes. Disabled or hidden choices can still
be selected. Package-reference attributes merge across global and choice-local
nodes; conflicting definitions are refused. A false `active` expression skips
the component before decoding its payload. Static Distributions retain their
existing `start_selected && selected` selection behavior.

`-pkginfo` / `inspect` with `-target` evaluates checks and selection read-only,
without locking, recovery or installation. Without a target, static packages
are inspected as before; JS packages report candidate metadata and unresolved
selection. Scripts and expressions are syntax-checked but never executed in
target-free inspection.

The runtime has a 64 MiB engine memory limit, a 1 MiB JS stack limit and a shared
five-second monotonic evaluation deadline. Evaluation is synchronous; queued
asynchronous jobs are refused. The embedder provides no QuickJS `std`/`os`
modules, module loader, process execution, network or file-writing APIs.
`system.run`/`runOnce`, other localization calls, applications, IORegistry, defaults,
desktop-session queries, Gestalt and `choice.packageUpgradeAction` fail with
explicit unsupported-API diagnostics when accessed. Relocation searches and
other unsupported Distribution elements remain refused. JS support does not
require a shell-script runner; payload shell hooks still do.

Bundle upgrade and relocation data (`bundle-version`, `relocate`, ...) is
accepted and ignored: it only matters when the bundle is already installed,
and mdpkg refuses reinstalls. There is no uninstall or upgrade.

## Paths

Payload paths are relative to the root plus the component's
`install-location`. They follow symlinks already in the root, as Apple's
installer does: on MiniDarwin, `/etc` is a link to `private/etc`, so a
package's `/etc/x` is written to `/private/etc/x`. A link that would lead out
of the root (by `..`, an absolute target or a chain of links) is an error,
whether it is in the root or in the payload.

Existing directories are kept as they are. Any other existing file at a
payload path is a collision and fails the install, as does a package whose
receipt is already present. Components of one install may share a path only
when every entry for it is a directory, or when the entries are identical
(type, mode, owner, time and contents), as the placeholder file in each of
Apple's SDK removal packages is: the file is written once and each
component's inventory lists it. `/private/var/db/receipts`, `/private/var/db/mdpkg`
and `/.mdpkg-scripts` are reserved.

Files and new directories get the packaged mode and modification time. Run as
root, mdpkg also applies the packaged owner and group; otherwise files belong
to the installing user and the requested owner is only recorded.

## Transactions

## The running root

`-target /` installs into the running root, in place: it cannot be copied
and swapped, so there is no transaction. Run as root (otherwise refused), it

1. validates the package and refuses collisions exactly as below, before
   anything is written; absolute symlinks already in the root are followed
   *within* it, since `/` is the root;
2. holds an exclusive lock on `/private/var/db/mdpkg/.lock`;
3. unpacks scripts into a private directory under `$TMPDIR`,
   `/private/var/tmp` or `/tmp`, never into the root;
4. writes payloads and receipts directly, journalling each directory, file
   and link it creates (nothing existing is ever replaced).

If anything fails, what the journal holds is removed. Not covered: what a
script itself changed (a failed script's effects stay, and the inventory
records no script changes in this mode, since hashing the whole running
system is not feasible), and a killed or crashed mdpkg, which can leave a
partial install with no receipt.

Scripts are executed themselves, with `PATH=/usr/bin:/bin:/usr/sbin:/sbin`,
`COMMAND_LINE_INSTALL`, `PACKAGE_PATH`, `DSTVOLUME` (`/`), `DSTROOT`,
`INSTALLER_TEMP` and `TMPDIR`, the installing user's `USER` and `HOME` when
mdpkg has them, and arguments `PACKAGE TARGET VOLUME`; they
need their exec bit and a working interpreter in the system. MiniDarwin has
no `/bin/sh` (its `sh` installs as `/usr/local/bin/ash`), so a `#!/bin/sh`
script does not run there until one is provided. A `-script-runner` given
with `-target /` is used instead of direct execution.

The tests cannot install into the real `/`; they set
`MDPKG_TEST_LIVE_ROOT=DIRECTORY` to make mdpkg treat that directory as the
running root. It is not for any other use.

## Offline roots

`-target` must be a real directory (not a symlink, not `/`, not in the Nix
store) whose parent is writable. mdpkg

1. takes an exclusive lock on `ROOT.mdpkg-lock` (the file is left behind);
2. validates the whole package and checks for collisions;
3. copies the root into `ROOT.mdpkg-transaction/new` -- APFS clones where
   possible, never hard links -- refusing entries with file flags or
   extended attributes that the copy would lose;
4. runs scripts, extracts payloads and writes receipts in the copy, and puts
   base directories' modes and times back;
5. swaps the copy in with two renames, then deletes the original.

Any failure before step 5 leaves the root untouched. A run interrupted inside
step 5 is resolved by the next `mdpkg install` on that root: it rolls back if
only the first rename happened, and completes otherwise. Between the renames
the root does not exist, so nothing may be using it: this is for offline
roots, not live systems.

## Script runners

Packages with scripts need `-script-runner` for an offline root; scripts are
never run directly against one, since their absolute paths would hit the host. A runner is invoked as

```
RUNNER SCRIPT WORKDIR PACKAGE TARGET STAGED_ROOT
```

in `WORKDIR` (inside the staged root), with `COMMAND_LINE_INSTALL=1`,
`PACKAGE_PATH`, `DSTVOLUME` (= `STAGED_ROOT`), `DSTROOT` (= `TARGET`),
`INSTALLER_TEMP` and `TMPDIR` set. It must run the script as
`SCRIPT PACKAGE TARGET STAGED_ROOT`, the arguments Apple's installer passes
when installing to another volume, and exit non-zero if the script does. A
runner is trusted: it is responsible for isolation.

`mdpkg-sandbox-runner` (bootstrap build, macOS only) runs the script with a
fixed bash and a `PATH` of coreutils, sed, grep and findutils, under
`sandbox-exec`: writes only beneath the staged root and to `/dev/null`, no
network, and no executables but that shell and those tools. Scripts must be
`sh` or `bash` scripts. The host stays readable, so this confines what a
script can change, not what it can see, and it is not a MiniDarwin runtime:
MiniDarwin binaries cannot run there. `sandbox-exec` cannot nest inside Nix's
build sandbox, so script-bearing packages are installed outside of Nix builds.

## Receipts

Per installed component:

- `/private/var/db/receipts/ID.plist`: `PackageIdentifier`, `PackageVersion`,
  `PackageFileName`, `InstallPrefixPath`, `InstallProcessName` (`mdpkg`) and
  `InstallDate` (`SOURCE_DATE_EPOCH` if set), as Apple writes them;
  `pkgutil --volume ROOT` reads them.
- `/private/var/db/receipts/ID.bom`: the package's BOM, unchanged.
- `/private/var/db/mdpkg/ID.inventory.plist`: every payload entry as
  installed (path, packaged path where a symlink redirected it, type, mode,
  requested uid/gid, symlink target, SHA-256), and every entry the package's
  scripts created, modified or removed.

These are extra files in a writable copy of the rootfs; they do not change the
release manifest (see `docs/rootfs-spec.md`).

## Tests

`nix flake check` runs `scripts/test_mdpkg.py` against the bootstrap build:
fixture packages built from the format descriptions (not with Apple's tools)
covering each supported and refused feature, hostile paths and links, limits,
checksums, collisions, locking, rollback and recovery. JS fixtures cover checks,
selection convergence, host/target metadata, read confinement, plist values,
unsupported APIs, read-only inspection and runtime limits. The target build is
checked for its load commands and purity but not run.

`scripts/test_mdpkg_mc.py` is the end-to-end proof, run by hand on macOS since
it needs `sandbox-exec`. It extracts the x86_64 release bundle, installs the
pinned Midnight Commander 4.8.7 package (Rudix, with a postinstall script), and
checks all 392 payload entries against the CPIO and BOM, the configuration
files the unmodified postinstall creates, the receipts (with `lsbom` and
`pkgutil` as read-only oracles), that no base file changed and the root
still passes `mdrootfs verify --tree --allow-extra`, and that a
script writing outside the root is stopped by the sandbox.

```bash
python3 scripts/test_mdpkg_mc.py \
  --release "$(nix build .#cross.x86_64.rootfsRelease --print-out-paths --no-link)" \
  --installer "$(nix build .#installerBootstrap --print-out-paths --no-link)/bin/mdpkg" \
  --runner "$(nix build .#installerBootstrap --print-out-paths --no-link)/libexec/mdpkg-sandbox-runner" \
  --pkg "$(nix build .#sources.midnightCommanderPkg --print-out-paths --no-link)"
```

It prints where it left the installed root and `report.json`. MC is installed,
not run: it is an x86_64 binary that needs dyld, libiconv, CoreFoundation and
CoreServices, none of which MiniDarwin has yet.
