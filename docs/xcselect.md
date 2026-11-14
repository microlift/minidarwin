# xcode-select, xcrun and libxcselect

MiniDarwin's developer-directory plumbing (`pkgs/xcselect/`):
`/usr/lib/libxcselect.dylib`, `/usr/bin/xcode-select`, `/usr/bin/xcrun`, the
`/usr/bin` tool shims (`cc`, `make`, `git`, ...), and an installer for Apple's
Command Line Tools (CLT) built on [mdpkg](../pkgs/installer/README.md).

The behaviour copied is macOS 27.2's (`xcode-select version 2418`). It was
established from the shipped binaries' imports, strings and disassembly, the
SDK's `xcselect.h` and `libxcselect.tbd`, and Apple's Software Update catalog,
and is checked against the host library by `scripts/test_xcselect_abi.py`.
No Apple code is used.

## Compared with Darling's

Darling's [`src/xcselect`](https://github.com/darlinghq/darling/tree/master/src/xcselect)
was the starting point. It differs from macOS, and from this, in these ways:

- **Installer.** Darling's `--install` runs a Python 2.7 script through
  `system()`, which runs `sudo installer`. Here it is a C program run with
  `posix_spawn`. It downloads with MiniDarwin's `curl` and installs with mdpkg.
- **Paths.** Darling's script uses `/Volumes/SystemRoot/home/$USER` and
  `~/.cache/darling`. Nothing here uses a path that is not Darwin's own.
- **Trust.** Darling downloads fixed CDN URLs with no verification, and falls
  back to a third-party list. Here the catalog and Distribution come over HTTPS
  from Apple's three Software Update hosts and nowhere else. Each package must
  match the catalog's size and TOC digest, and must carry Apple's Software
  Update signature, chained to Apple Root CA (see "Trust").
- **ABI.** Darling declares `xcselect_get_developer_dir_path(char *, size_t,
  bool *)`. The shipped function takes `(char *buf, int len, bool *from_env,
  bool *is_cltools, bool *is_default)`, so callers built for macOS write
  through two pointers Darling never reads. Darling's
  `xcselect_find_developer_contents_from_path` has its last two arguments in
  the wrong order. It exports 7 of the 12 symbols and links CoreFoundation;
  macOS's library links only libSystem.
- **Lookup.** Darling skips `/var/select/developer_dir`, and falls through to
  the next source when `DEVELOPER_DIR` is invalid. `-s` does not need root.

## Files

| Path | What |
| --- | --- |
| `/usr/lib/libxcselect.dylib` | Install name as shown, versions `1.0.0`/`1.0.0`. Links only libSystem. Exports exactly `libxcselect.tbd`'s 12 symbols. |
| `/usr/include/xcselect.h` | The one public declaration, `xcselect_host_sdk_path`. Apple's header takes `CF_ENUM` from CoreFoundation; this one uses a `uint32_t` enum with the same values. |
| `/usr/bin/xcode-select`, `/usr/bin/xcrun` | |
| `/usr/libexec/xcselect/tool-shim`, `xtool-shim` | One shim for public tools and one for tools that need Xcode. Every name in `shims.nix` is a symlink in `/usr/bin` to one of them, because the rootfs format has no hardlinks. The names are the 78 + 16 hardlinks of macOS 27.2's `tool-shim-public` and `xtool-shim-public`. |
| `/usr/libexec/xcselect-install` | The CLT installer. |
| `/private/var/select`, `/private/var/db` | Present on macOS. `xcode-select -s` writes its links there and does not create the directories. |

The shims and `xcrun` carry a 4-byte `__DATA,__xcrun_shim` section, as
macOS's do. libxcselect will not run a developer directory whose
`usr/bin/xcrun` has that section ("malformed developer path"), so a shim
cannot recurse into itself.

## libxcselect

The prototypes are in `pkgs/xcselect/xcselect-private.h`. Points that differ
from what one might guess:

- **Resolution** (`xcselect_get_developer_dir_path`):
  1. `DEVELOPER_DIR`, if non-empty. A path that resolves through
     `find_developer_contents_from_path` replaces the variable in the process
     environment. One that does not resolve is returned as it is; resolution
     stops either way.
  2. The symlinks `/var/select/developer_dir`, `/var/db/xcode_select_link` and
     `/usr/share/xcode-select/xcode_dir_link`, then the file
     `/usr/share/xcode-select/xcode_dir_path` (one trailing newline dropped).
     A missing source, or one that does not name a directory, moves on to the
     next. An unreadable one is reported and ends resolution with failure.
  3. The defaults (`*is_default`): Xcode.app, then
     `/Library/Developer/CommandLineTools`, then Xcode-beta.app. The first
     whose `usr/lib/libxcrun.dylib` exists is used.

  `*is_cltools` is set when the last path component is `CommandLineTools`.
- **find_developer_contents_from_path**: makes the path absolute and strips
  trailing slashes. Then, in this order:
  - `/`, or a directory containing `mach_kernel`: its
    `Library/Developer/CommandLineTools`.
  - A path ending in `CommandLineTools`: itself. In both of these cases the
    result must contain `usr/lib/libxcrun.dylib` or `usr/bin/xcrun`.
  - Otherwise `PATH/Contents/Developer` if it is a directory, else `PATH`
    itself if it contains xcrun, else `PATH/Xcode.app/Contents/Developer` if
    that is a directory.
- **Running a tool** (`xcselect_invoke_xcrun`):
  - For a shim (a non-NULL tool), `xcrun_log` is unset first.
  - `XCSELECT_FLAG_REQUIRE_XCODE` with a CLT directory is an error.
  - If `DEV/usr/lib/libxcrun.dylib` exists, it is `dlopen`ed and
    `xcrun_main(tool, argc, argv, dev)` is called. When the program is `xcrun`
    itself, an unknown-utility handler is installed first, which requests an
    install.
  - Otherwise `DEV/usr/bin/xcrun` is `execv`ed, with macOS's errors when that
    fails.
  - With no developer directory, a named tool is looked for in
    `/usr/libexec/DeveloperTools`, then `/usr/local/bin`, and run from there.
    If neither has it, an install is requested.
- **xcselect_host_sdk_path** reproduces a quirk of macOS's library. The host
  "major" version is `strtol` of `kern.osproductversion` from its fourth
  character, which is the minor version of a 10.x release. Only
  `MacOSX10.N.sdk` names carry a version (N); every other SDK counts as 0.
  "Latest" is therefore the first SDK in `readdir` order unless a 10.x SDK is
  present, and on macOS 11 and later `MATCHING_ONLY` is ENOENT. The SDKs
  searched are the chosen developer directory's, but only when it is neither a
  CLT directory nor a default, and then `/Library/Developer/CommandLineTools/SDKs`.
  The ABI test checks this against the host.
- **xcselect_get_manpaths** returns only libxcrun's paths when libxcrun loads.
  When there is no libxcrun, it returns the three standard paths: `usr/share/man`,
  `usr/llvm-gcc-4.2/share/man` (Darling misspells this one as `4,2`) and
  `Toolchains/XcodeDefault.xctoolchain/usr/share/man`.
- **Not reproduced**, and marked `FIXME (ACCURACY)`:
  - macOS refuses App Sandbox callers; MiniDarwin has no App Sandbox.
  - The shim takes its name from the file system's spelling of the
    executable's path, which matters only on case-insensitive volumes.
  - `xcode-select -s` marks the chosen Xcode as used in Spotlight.

### Install requests

On macOS, `xcselect_trigger_install_request` sends `{tool-name}` to the
`com.apple.dt.CommandLineTools.installondemand` CFMessagePort, where a GUI
agent shows a dialog. MiniDarwin has no CoreFoundation, no GUI and no agent.
As [`open`](open-protocol.md) does, libxcselect sends one request to whatever
serves a socket, and leaves every decision to that agent:

- `AF_UNIX`, `/private/var/run/org.minidarwin.xcselect.install.sock`, or
  `$MINIDARWIN_XCSELECT_INSTALL_SOCKET` when it is set.
- One frame each way: a big-endian `uint32` length, then an XML property list.
  The request is `{Version: 1, ToolName: string}`, with `ToolName` omitted for
  NULL. The reply is `{Version: 1, OK: bool}`. `OK` means someone will ask the
  user; it does not mean anything was installed.
- No socket, no reply within 10 seconds, or `OK` false all mean false.

MiniDarwin installs no agent. Rosemary may serve the socket from the host and
show the licence in a dialog of its own (see "Rosemary").

## xcode-select

The options, usage text, messages and exit statuses are macOS's. On this
host, every case `test_xcselect.py` and a side-by-side run cover is
byte-identical. `-s PATH` validates the path first, then requires root, and
replaces `/var/select/developer_dir` and `/var/db/xcode_select_link`. `-r`
requires root and removes those links and the two `/usr/share/xcode-select`
files.

`--install` is the one deliberate difference. Like macOS, it first prints the
"already installed" note if `/Library/Developer/CommandLineTools/usr/lib/libxcrun.dylib`
exists, and then sends an install request. macOS ends there if no install
could be requested. MiniDarwin then runs `/usr/libexec/xcselect-install` in the
foreground, because nothing else on a bare root can show the licence.

## xcselect-install

```
xcselect-install [-list] [-release VERSION] [-pkg-dir DIRECTORY] [-accept-license]
```

1. It requires root, since it installs into the running root.
2. It reads the target's `/System/Library/CoreServices/SystemVersion.plist`
   for `ProductVersion`.
3. It fetches the Software Update catalog for that major version. The catalog
   names are known for 12, 13, 14, 15, 26 and 27; any other major is refused
   by name.
4. Products with a `CLTools_Executables*` package are candidates. Their English
   Distributions are fetched. A Distribution counts only if it is tagged
   `com.apple.dt.commandlinetools` and has an `SU_VERS`. The newest whose
   `volume-check` `allowed-os-versions` includes the target is chosen, or the
   one `-release` names.
5. The licence (the Distribution's RTF, shown as text) must be answered with
   `agree` on the terminal, or `-accept-license` must be given. No
   environment variable accepts it.
6. Each package is downloaded with `/usr/bin/curl`, using
   `--proto =https --proto-redir =https` and a size limit. Each must match the
   catalog's `Size` and `Digest`. The `Digest` is not a hash of the file: it is
   the package's XAR TOC checksum, the SHA-1 of the compressed table of
   contents, which is the value Apple's signature signs. `-pkg-dir` keeps the
   files, and reuses those that still match.
7. It creates `/tmp/.com.apple.dt.CommandLineTools.installondemand.in-progress`,
   which is what makes the Distribution's choice visible on a fresh install.
   Then it runs
   `mdpkg -dist PRODUCT.dist -pkg-dir DIR -target / -verify-signature apple-software-update`,
   so the choice of components is Apple's own Distribution JavaScript as mdpkg
   evaluates it. The marker and any temporary directory are removed afterwards.

Against Apple's live catalog with a macOS 26.6 target, `-list` offers 26.5
and 26.6 and refuses the eleven older releases, as their version ranges say.

### Trust

- **TLS.** The catalog and Distributions come over HTTPS, validated against
  MiniDarwin's `/etc/ssl/cert.pem`, from Apple's Software Update hosts only.
- **Packages.** mdpkg's `apple-software-update` policy (`pkgs/installer/signature.c`)
  requires three things of each package:
  - its XAR signature verifies, as an RSA PKCS#1 v1.5 signature of the TOC
    checksum;
  - the signing certificate chains, through the certificates in the TOC, to
    Apple Root CA, which is compiled in from `security_certificates` and is
    the policy's only anchor;
  - the signing certificate carries the Apple Software Update Signing usage,
    `1.2.840.113635.100.4.1`.

  The TOC lists every member's checksum, so the signature covers the whole
  package.

The real CLT 26.6 packages verify this way: Software Update, then Apple
Software Update Certification Authority, then Apple Root CA.

### Licence

Section 2B of the licence permits installation only on Apple-branded
computers. Rosemary on Linux, and MiniDarwin on other hardware, do not meet
that condition. The installer shows the licence and requires explicit
agreement, and the decision is the user's.

## mdpkg changes this needed

Each has fixtures in `scripts/test_mdpkg.py`:

- **pbzx payloads.** These are xz chunks, or stored chunks whose two lengths
  are equal. They use the new `/usr/lib/liblzma.5.dylib` (`pkgs/xz/`):
  XZ Utils 5.4.7 with the CVE-2025-31115 fix, using macOS's install name,
  versions and export set. libarchive now links it too.
- **Streaming.** The package file is mapped and members are decoded only when
  read. Payload contents are spooled to an unlinked temporary file instead of
  memory, which the 3.3 GiB CLT payload needs.
- **Stored XAR members** are used in place, without being copied.
- **`-dist FILE -pkg-dir DIR`.** A standalone Distribution whose `pkg-ref`s
  name package files in `DIR`. Only plain file names are accepted.
- **Signature policy.** `-verify-signature apple-software-update`, described
  under "Trust".
- **`system.localizedString`.** It reads the Distribution's English
  `<localization>` table. A missing key returns itself, as Cocoa's lookups do;
  this is marked `FIXME (ACCURACY)`.
- **`volume-check` with `allowed-os-versions`** is enforced against the
  target's `SystemVersion.plist`. A top-level list stays informational, as it
  was.
- **Software Update elements** (`tags`, `platforms`, `localization`) are
  accepted. Of several `choices-outline`s, the one without `ui` is followed.
- **`numberOfFiles`** may count every payload entry (pkgbuild's convention)
  or only regular files (Apple's Software Update packages').
- **`<groups>`** in `PackageInfo` (package groups, as in `pkgutil --groups`) is
  accepted and recorded in mdpkg's inventory. Apple's receipt plist has no key
  for it.
- **Shared files.** Components may share a path when their entries are
  identical. All four of Apple's SDK removal packages install the same
  placeholder file in `/private/tmp`.
- **Entry limit.** The bound is now one million entries, up from 100,000. Each
  entry is only metadata, and the two macOS SDKs alone have about 96,000.

## Rosemary

Nothing in Rosemary is required. Two optional pieces would help:

- **Install agent.** Rosemary could serve the install socket with a host-side
  licence dialog, then run `xcselect-install -accept-license` in the prefix as
  root.
- **Package cache.** It could keep downloaded CLT packages on the host and
  pass them in with `-pkg-dir`. They are still checked against the catalog
  and their signatures.

`/var/select`, `/var/db/xcode_select_link` and `/Library/Developer` hold user
state in a prefix: they are not in the MiniDarwin manifest's files, so prefix
repair keeps them.

## Tests

- `xcselectTest` (`nix flake check`) runs `scripts/test_xcselect.py` against
  the host build. That build is compiled with `XCSELECT_TESTING`, which roots
  every system path at `$XCSELECT_TEST_ROOT` and allows `file:` URLs. The
  tests cover:
  - the resolution order and errors;
  - `-s`, `-r` and the command line;
  - the shims and their refusals;
  - install requests to a fake agent;
  - the library's exports;
  - `xcselect-install` end to end, with a fake catalog and a package signed by
    a test chain.
- `scripts/test_xcselect_abi.py` (macOS, by hand) runs the same probe against
  the host's `/usr/lib/libxcselect.dylib` and ours under six `DEVELOPER_DIR`
  settings, and requires identical output.
- Installing the real CLT needs Apple's licence to be accepted and about 950
  MB to be downloaded. It is not automated.
