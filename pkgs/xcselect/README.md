# xcode-select, xcrun and libxcselect

MiniDarwin's developer-directory plumbing, written to behave as macOS 27.2's
(`xcode-select version 2418`). The design, the behaviour it copies and how
that was established are in [docs/xcselect.md](../../docs/xcselect.md).

| Path | |
| --- | --- |
| `/usr/lib/libxcselect.dylib` | The 12 exports of macOS's library, linking only libSystem. |
| `/usr/include/xcselect.h` | The public declaration, `xcselect_host_sdk_path`. |
| `/usr/bin/xcode-select` | Prints or changes the active developer directory. |
| `/usr/bin/xcrun` | Runs `xcrun` from the active developer directory. |
| `/usr/bin/cc`, `make`, `git`, ... | Symlinks to `/usr/libexec/xcselect/tool-shim`. |
| `/usr/bin/xcodebuild`, `xed`, ... | Symlinks to `/usr/libexec/xcselect/xtool-shim`, which needs Xcode. |
| `/usr/libexec/xcselect-install` | Installs Apple's Command Line Tools. |
| `/private/var/select`, `/private/var/db` | Where `xcode-select --switch` writes its links. |

## Installing the Command Line Tools

```
sudo xcode-select --install
```

asks an installer agent on `/private/var/run/org.minidarwin.xcselect.install.sock`
first (macOS asks its GUI agent the same way); with none, it runs

```
xcselect-install [-list] [-release VERSION] [-pkg-dir DIRECTORY] [-accept-license]
```

which reads Apple's Software Update catalog for the system's macOS
(`/System/Library/CoreServices/SystemVersion.plist`), picks the newest
Command Line Tools whose Distribution allows that version, shows Apple's
licence and requires `agree` (or `-accept-license`), downloads the packages
with `/usr/bin/curl`, checks each against the catalog's size and digest (the
SHA-1 of the package's compressed XAR table of contents, which Apple's
signature signs), and installs them with

```
mdpkg -dist PRODUCT.dist -pkg-dir DIRECTORY -target / -verify-signature apple-software-update
```

so every package must also carry Apple's Software Update signature, chained to
Apple Root CA. Only `https://swscan.apple.com`, `swdist.apple.com` and
`swcdn.apple.com` are contacted. `-pkg-dir` keeps the downloads, and reuses
any already there that still match the catalog. `-list` prints the releases the
catalog offers and installs nothing.

Section 2B of Apple's licence permits installation only on Apple-branded
computers.

## Tests

`nix build .#checks.<system>.xcselectTest` runs `scripts/test_xcselect.py`
against the host build (`bootstrap.nix`, built with `XCSELECT_TESTING`, which
puts every system path under `$XCSELECT_TEST_ROOT`). On macOS,

```
python3 scripts/test_xcselect_abi.py --library "$(nix build .#xcselectBootstrap --print-out-paths --no-link)/lib/libxcselect.dylib"
```

compares every export with the host's own `/usr/lib/libxcselect.dylib`.
