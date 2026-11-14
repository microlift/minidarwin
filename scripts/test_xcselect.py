#!/usr/bin/env python3
"""Behavioural tests for libxcselect, xcode-select, the shims and
xcselect-install, against the host build (pkgs/xcselect/bootstrap.nix).

That build puts every system path under $XCSELECT_TEST_ROOT, so each test
gets a fresh fake root. XCSELECT names the build's prefix (bin/, lib/).
MDPKG, when set, is mdpkg's host build, for the installer's end-to-end test.
"""
import ctypes
import gzip
import hashlib
import os
from pathlib import Path
import plistlib
import socket
import stat
import struct
import subprocess
import tempfile
import threading
import unittest

import test_mdpkg

BUILD = Path(os.environ["XCSELECT"])
XCODE = "Applications/Xcode.app/Contents/Developer"
CLT = "Library/Developer/CommandLineTools"
FAKE_XCRUN = "#!/bin/sh\necho \"xcrun $DEVELOPER_DIR|$*\"\n"


class Root:
    def __init__(self, path):
        self.path = Path(path)

    def __truediv__(self, rel):
        return self.path / rel

    def developer(self, rel, library=False):
        """A developer directory with a usr/bin/xcrun that echoes its arguments."""
        d = self / rel
        (d / "usr/bin").mkdir(parents=True, exist_ok=True)
        xcrun = d / "usr/bin/xcrun"
        xcrun.write_text(FAKE_XCRUN)
        xcrun.chmod(0o755)
        if library:
            (d / "usr/lib").mkdir(parents=True, exist_ok=True)
            (d / "usr/lib/libxcrun.dylib").write_bytes(b"")
        return str(d)

    def link(self, rel, target):
        p = self / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.symlink_to(target)


class XcselectTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="xcselect-tests-")
        self.root = Root(Path(self.tmp.name) / "root")
        self.root.path.mkdir()
        self.bin = Path(self.tmp.name) / "bin"
        self.bin.mkdir()
        for name, shim in [("cc", "tool-shim"), ("llvm-gcc", "tool-shim"),
                           ("xcodebuild", "xtool-shim"), ("xcrun", "xcrun"),
                           ("xcode-select", "xcode-select")]:
            (self.bin / name).symlink_to(BUILD / "bin" / shim)
        self.env = {k: v for k, v in os.environ.items() if k != "DEVELOPER_DIR"}
        self.env["XCSELECT_TEST_ROOT"] = str(self.root.path)
        self.env["MINIDARWIN_XCSELECT_INSTALL_SOCKET"] = str(Path(self.tmp.name) / "none.sock")

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, name, *args, env=None, status=0):
        result = subprocess.run([self.bin / name, *args], capture_output=True, text=True,
                                env={**self.env, **(env or {})}, timeout=30)
        self.assertEqual(result.returncode, status, result.stdout + result.stderr)
        return result

    def print_path(self, **kwargs):
        return self.run_tool("xcode-select", "-p", **kwargs).stdout.strip()

    def test_resolution_order(self):
        xcode = self.root.developer(XCODE, library=True)
        self.assertEqual(self.print_path(), xcode)
        clt = self.root.developer(CLT, library=True)
        self.assertEqual(self.print_path(), xcode)
        (self.root / XCODE / "usr/lib/libxcrun.dylib").unlink()
        self.assertEqual(self.print_path(), clt)
        others = [self.root.developer(f"Developer{i}") for i in range(4)]
        (self.root / "usr/share/xcode-select").mkdir(parents=True)
        (self.root / "usr/share/xcode-select/xcode_dir_path").write_text(others[0] + "\n")
        self.assertEqual(self.print_path(), others[0])
        self.root.link("usr/share/xcode-select/xcode_dir_link", others[1])
        self.assertEqual(self.print_path(), others[1])
        self.root.link("var/db/xcode_select_link", others[2])
        self.assertEqual(self.print_path(), others[2])
        self.root.link("var/select/developer_dir", others[3])
        self.assertEqual(self.print_path(), others[3])
        self.assertEqual(self.print_path(env={"DEVELOPER_DIR": xcode}), xcode)
        # An unusable DEVELOPER_DIR is the answer anyway.
        self.assertEqual(self.print_path(env={"DEVELOPER_DIR": "/nonexistent"}), "/nonexistent")

    def test_dangling_links_fall_through_and_bad_links_fail(self):
        clt = self.root.developer(CLT, library=True)
        self.root.link("var/select/developer_dir", "/nonexistent")
        (self.root / "usr/share/xcode-select").mkdir(parents=True)
        (self.root / "usr/share/xcode-select/xcode_dir_path").write_text("/nonexistent\n")
        self.assertEqual(self.print_path(), clt)
        (self.root / "var/db").mkdir(parents=True)
        (self.root / "var/db/xcode_select_link").write_text("not a link")
        result = self.run_tool("xcode-select", "-p", status=2)
        self.assertIn("expected symbolic link", result.stderr)

    def test_switch_and_reset(self):
        xcode = self.root.developer(XCODE)
        app = str(self.root / "Applications/Xcode.app")
        self.run_tool("xcode-select", "-s", app, status=1)
        for d in ["var/select", "var/db"]:
            (self.root / d).mkdir(parents=True)
        self.run_tool("xcode-select", "--switch", app, env={"XCSELECT_TEST_UID": "0"})
        for link in ["var/select/developer_dir", "var/db/xcode_select_link"]:
            self.assertEqual(os.readlink(self.root / link), xcode)
        self.assertEqual(self.print_path(), xcode)
        result = self.run_tool("xcode-select", "-s", "/nonexistent", status=1,
                               env={"XCSELECT_TEST_UID": "0"})
        self.assertIn("invalid developer directory '/nonexistent'", result.stderr)
        self.run_tool("xcode-select", "-r", status=1)
        self.run_tool("xcode-select", "-r", env={"XCSELECT_TEST_UID": "0"})
        self.assertFalse(os.path.lexists(self.root / "var/select/developer_dir"))
        self.run_tool("xcode-select", "-p", status=2)

    def test_command_line(self):
        self.assertIn("Usage: xcode-select [options]", self.run_tool("xcode-select", "-h").stderr)
        self.assertEqual(self.run_tool("xcode-select", "-v").stdout, "xcode-select version 2418.\n")
        for args, message in [((), "no command option given"), (("-x",), "invalid argument '-x'"),
                              (("-p", "-v"), "cannot execute multiple actions"),
                              (("-s",), "missing argument to '-s'")]:
            self.assertIn(message, self.run_tool("xcode-select", *args, status=1).stderr)
        # No libxcrun, so the standard paths (an unloadable one is an error).
        self.root.link("var/select/developer_dir", self.root.developer(XCODE))
        self.run_tool("xcode-select", "-p", "-p")
        manpaths = self.run_tool("xcode-select", "--show-manpaths").stdout.splitlines()
        self.assertEqual([Path(p).relative_to(self.root / XCODE).as_posix() for p in manpaths],
                         ["usr/share/man", "usr/llvm-gcc-4.2/share/man",
                          "Toolchains/XcodeDefault.xctoolchain/usr/share/man"])

    def test_shims_run_xcrun_with_the_tool_name(self):
        xcode = self.root.developer(XCODE)
        self.root.link("var/select/developer_dir", xcode)
        self.assertEqual(self.run_tool("cc", "-v", "x").stdout, f"xcrun |cc -v x\n")
        self.assertEqual(self.run_tool("llvm-gcc").stdout, "xcrun |clang\n")
        self.assertEqual(self.run_tool("xcrun", "--find", "cc").stdout, "xcrun |--find cc\n")
        self.assertEqual(self.run_tool("xcodebuild").stdout, "xcrun |xcodebuild\n")
        # DEVELOPER_DIR is passed on as resolved.
        out = self.run_tool("cc", env={"DEVELOPER_DIR": str(self.root / "Applications/Xcode.app")}).stdout
        self.assertEqual(out, f"xcrun {xcode}|cc\n")

    def test_shim_refusals(self):
        clt = self.root.developer(CLT)
        result = self.run_tool("xcodebuild", env={"DEVELOPER_DIR": clt}, status=1)
        self.assertIn("requires Xcode, but active developer directory", result.stderr)
        (self.root / CLT / "usr/bin/xcrun").unlink()
        (self.root / CLT / "usr/bin/xcrun").symlink_to(BUILD / "bin/tool-shim")
        result = self.run_tool("cc", env={"DEVELOPER_DIR": clt}, status=1)
        self.assertIn("malformed developer path", result.stderr)
        empty = self.root / "Empty"
        empty.mkdir()
        result = self.run_tool("cc", env={"DEVELOPER_DIR": str(empty)}, status=1)
        self.assertIn("invalid DEVELOPER_DIR path", result.stderr)
        result = self.run_tool("cc", env={"DEVELOPER_DIR": "/nonexistent"}, status=1)
        self.assertIn("missing DEVELOPER_DIR path: /nonexistent", result.stderr)

    def agent(self, ok):
        """An install agent on a socket; returns (socket path, requests)."""
        path = Path(self.tmp.name) / "agent.sock"
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(str(path))
        server.listen()
        requests = []

        def serve():
            while True:
                try:
                    conn, _ = server.accept()
                except OSError:
                    return
                with conn:
                    length = struct.unpack(">I", conn.recv(4))[0]
                    data = b""
                    while len(data) < length:
                        data += conn.recv(length - len(data))
                    requests.append(plistlib.loads(data))
                    reply = plistlib.dumps({"Version": 1, "OK": ok})
                    conn.sendall(struct.pack(">I", len(reply)) + reply)

        threading.Thread(target=serve, daemon=True).start()
        self.addCleanup(server.close)
        return str(path), requests

    def test_no_developer_directory(self):
        tools = self.root / "usr/libexec/DeveloperTools"
        tools.mkdir(parents=True)
        (tools / "cc").write_text("#!/bin/sh\necho fallback \"$@\"\n")
        (tools / "cc").chmod(0o755)
        self.assertEqual(self.run_tool("cc", "a").stdout, "fallback a\n")
        result = self.run_tool("xcodebuild", status=1)
        self.assertIn("no install could be requested", result.stderr)
        sock, requests = self.agent(True)
        result = self.run_tool("xcodebuild", status=1, env={"MINIDARWIN_XCSELECT_INSTALL_SOCKET": sock})
        self.assertIn("requesting install", result.stderr)
        result = self.run_tool("xcrun", status=1, env={"MINIDARWIN_XCSELECT_INSTALL_SOCKET": sock})
        self.assertEqual([r["ToolName"] for r in requests], ["xcodebuild", "xcrun"])

    def test_install_option(self):
        sock, requests = self.agent(True)
        result = self.run_tool("xcode-select", "--install", env={"MINIDARWIN_XCSELECT_INSTALL_SOCKET": sock})
        self.assertIn("install requested", result.stderr)
        self.assertEqual(requests, [{"Version": 1, "ToolName": "xcode-select"}])
        installer = self.root / "usr/libexec/xcselect-install"
        installer.parent.mkdir(parents=True)
        installer.write_text("#!/bin/sh\necho local installer\n")
        installer.chmod(0o755)
        self.assertEqual(self.run_tool("xcode-select", "--install").stdout, "local installer\n")
        self.root.developer(CLT, library=True)
        result = self.run_tool("xcode-select", "--install", status=1)
        self.assertIn("already installed", result.stderr)


class LibraryTests(unittest.TestCase):
    """The exports, called directly."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="xcselect-lib-")
        self.root = Root(self.tmp.name)
        os.environ["XCSELECT_TEST_ROOT"] = self.tmp.name
        os.environ.pop("DEVELOPER_DIR", None)
        lib = ctypes.CDLL(str(BUILD / "lib/libxcselect.dylib"))
        self.lib = lib
        lib.xcselect_get_version.restype = ctypes.c_char_p
        lib.xcselect_find_developer_contents_from_path.argtypes = [
            ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(ctypes.c_bool)]
        lib.xcselect_find_developer_contents_from_path.restype = ctypes.c_bool
        lib.xcselect_developer_dir_matches_path.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        lib.xcselect_developer_dir_matches_path.restype = ctypes.c_bool
        lib.xcselect_bundle_is_developer_tool.argtypes = [ctypes.c_char_p]
        lib.xcselect_bundle_is_developer_tool.restype = ctypes.c_bool
        lib.xcselect_host_sdk_path.argtypes = [ctypes.c_uint32, ctypes.POINTER(ctypes.c_char_p)]

    def tearDown(self):
        os.environ.pop("XCSELECT_TEST_ROOT", None)
        self.tmp.cleanup()

    def find(self, path):
        buf, clt = ctypes.create_string_buffer(1024), ctypes.c_bool(True)
        ok = self.lib.xcselect_find_developer_contents_from_path(str(path).encode(), buf, 1024,
                                                                ctypes.byref(clt))
        return (buf.value.decode(), clt.value) if ok else None

    def test_find_developer_contents(self):
        xcode = self.root.developer("A/Xcode.app/Contents/Developer")
        self.assertEqual(self.find(self.root / "A/Xcode.app"), (xcode, False))
        self.assertEqual(self.find(self.root / "A/Xcode.app/"), (xcode, False))
        self.assertEqual(self.find(self.root / "A"), (xcode, False))
        clt = self.root.developer("B/CommandLineTools")
        self.assertEqual(self.find(clt), (clt, True))
        volume = self.root.developer("C/Library/Developer/CommandLineTools")
        (self.root / "C/mach_kernel").write_bytes(b"")
        self.assertEqual(self.find(self.root / "C"), (volume, True))
        plain = self.root.developer("D")
        self.assertEqual(self.find(plain), (plain, False))
        self.assertIsNone(self.find(self.root / "E"))

    def test_matches_path_and_bundles(self):
        m = self.lib.xcselect_developer_dir_matches_path
        self.assertTrue(m(b"/X/Y.app/Contents/Developer/", b"/X/Y.app/Contents/MacOS/Y"))
        self.assertTrue(m(b"/X/Y.app/Contents/Developer", b"/X/Y.apple"))
        self.assertFalse(m(b"/X/Y.app/Contents/Developer", b"/X"))
        self.assertFalse(m(b"/X/Y.app", None))
        b = self.lib.xcselect_bundle_is_developer_tool
        self.assertTrue(b(b"com.apple.dt.Xcode"))
        self.assertTrue(b(b"com.apple.RealityComposerPro"))
        self.assertFalse(b(b"com.apple.dt.Xcodex"))
        self.assertFalse(b(b"org.apple.dt.Xcode"))
        self.assertFalse(b(None))
        self.assertEqual(self.lib.xcselect_get_version(), b"2418")

    def test_host_sdk_path(self):
        sdks = self.root / CLT / "SDKs"
        for name in ["MacOSX26.sdk", "MacOSX10.15.sdk", "MacOSX10.9.sdk", "NotAnSDK"]:
            (sdks / name / "usr/include/sys").mkdir(parents=True)
            if name != "NotAnSDK":
                (sdks / name / "usr/include/sys/cdefs.h").write_text("")
        path = ctypes.c_char_p()
        self.assertEqual(self.lib.xcselect_host_sdk_path(3, ctypes.byref(path)), 0)
        self.assertEqual(path.value.decode(), str(sdks / "MacOSX10.15.sdk"))
        self.assertEqual(self.lib.xcselect_host_sdk_path(0, ctypes.byref(path)), 22)
        self.assertEqual(self.lib.xcselect_host_sdk_path(4, ctypes.byref(path)), 22)
        self.assertEqual(self.lib.xcselect_host_sdk_path(1, None), 22)


def rtf_license():
    return ("{\\rtf1\\ansi{\\fonttbl\\f0\\fnil HelveticaNeue;}{\\colortbl;\\red255;}"
            "\\f0\\b TEST AGREEMENT\\\n\\b0 Clause \\'93one\\'94.\\par "
            "{\\field{\\*\\fldinst{HYPERLINK \"http://x\"}}{\\fldrslt http://x}}}")


class InstallerTests(unittest.TestCase):
    """xcselect-install against a fake catalog, with file: URLs (test build
    only), a package signed by a test chain and mdpkg's host build."""

    def setUp(self):
        if not os.environ.get("MDPKG"):
            self.skipTest("MDPKG not set")
        self.tmp = tempfile.TemporaryDirectory(prefix="xcselect-install-")
        self.base = Path(self.tmp.name)
        self.root = self.base / "root"
        (self.root / "System/Library/CoreServices").mkdir(parents=True)
        (self.root / "tmp").mkdir()
        self.set_version("26.6")
        self.cdn = self.base / "cdn"
        self.cdn.mkdir()
        pki = test_mdpkg.TestPKI(self.base / "pki")
        self.anchor = str(pki.dir / "root.der")
        entries = [("./Library", stat.S_IFDIR | 0o755, b""),
                   ("./Library/Developer", stat.S_IFDIR | 0o755, b""),
                   ("./Library/Developer/CommandLineTools", stat.S_IFDIR | 0o755, b""),
                   ("./Library/Developer/CommandLineTools/usr", stat.S_IFDIR | 0o755, b""),
                   ("./Library/Developer/CommandLineTools/usr/bin", stat.S_IFDIR | 0o755, b""),
                   ("./Library/Developer/CommandLineTools/usr/bin/xcrun", stat.S_IFREG | 0o755,
                    FAKE_XCRUN.encode())]
        files = test_mdpkg.component_files(entries, identifier="com.apple.pkg.CLTools_Executables")
        self.package = test_mdpkg.xar(files, sign=pki.signer("leaf"))
        (self.cdn / "CLTools_Executables_Test.pkg").write_bytes(self.package)
        # The catalog's Digest is the TOC checksum: SHA-1 of the packed TOC.
        _, header, _, packed = struct.unpack(">4sHHQ", self.package[:16])
        self.digest = hashlib.sha1(self.package[header:header + packed]).hexdigest()

    def tearDown(self):
        self.tmp.cleanup()

    def set_version(self, version):
        (self.root / "System/Library/CoreServices/SystemVersion.plist").write_bytes(
            plistlib.dumps({"ProductVersion": version}))

    def distribution(self, version="26.6", minimum="26.2", before="27.0"):
        rtf = rtf_license().replace("&", "&amp;").replace("<", "&lt;")
        return f"""<installer-gui-script minSpecVersion="1">
  <volume-check><allowed-os-versions><os-version before="{before}" min="{minimum}"/></allowed-os-versions></volume-check>
  <license mime-type="text/rtf" language="English">{rtf}</license>
  <tags><tag>DTCommandLineTools</tag><tag>com.apple.dt.commandlinetools</tag></tags>
  <choices-outline ui="SoftwareUpdate"><line choice="su"/></choices-outline>
  <choices-outline><line choice="su"/></choices-outline>
  <choice id="su" start_selected="true" title="SU_TITLE" visible="Or0(my.target.mountpoint)">
    <pkg-ref id="com.apple.pkg.CLTools_Executables" auth="Root">CLTools_Executables_Test.pkg</pkg-ref>
  </choice>
  <script>function Or0(prefix) {{
    return system.files.fileExistsAtPath(prefix + '/tmp/.com.apple.dt.CommandLineTools.installondemand.in-progress') == 1;
  }}</script>
  <localization><strings language="English">"SU_TITLE" = "Command Line Tools for Xcode {version}";
"SU_VERS" = "{version}";
</strings></localization>
  <pkg-ref id="com.apple.pkg.CLTools_Executables" version="{version}.0.0.1"/>
</installer-gui-script>"""

    def catalog(self, products):
        """PRODUCTS: (id, distribution, digest)."""
        entries = {}
        for pid, dist, digest in products:
            (self.cdn / f"{pid}.English.dist").write_text(dist)
            entries[pid] = {
                "Packages": [{"URL": (self.cdn / "CLTools_Executables_Test.pkg").as_uri(),
                              "Size": len(self.package), "Digest": digest}],
                "Distributions": {"English": (self.cdn / f"{pid}.English.dist").as_uri()}}
        entries["000-unrelated"] = {"Packages": [{"URL": "https://swcdn.apple.com/x/Other.pkg",
                                                  "Size": 1}]}
        path = self.cdn / "catalog.sucatalog.gz"
        path.write_bytes(gzip.compress(plistlib.dumps({"Products": entries})))
        return path.as_uri()

    def install(self, *args, status=0):
        env = {**os.environ, "XCSELECT_TEST_ROOT": str(self.root),
               "XCSELECT_TEST_MDPKG": os.environ["MDPKG"],
               "MDPKG_TEST_LIVE_ROOT": str(self.root),
               "MDPKG_TEST_SIGNATURE_ANCHOR": self.anchor}
        result = subprocess.run([BUILD / "bin/xcselect-install", *args], capture_output=True,
                                text=True, env=env, timeout=60, stdin=subprocess.DEVNULL,
                                start_new_session=True)
        self.assertEqual(result.returncode, status, result.stdout + result.stderr)
        return result

    def test_lists_and_installs_the_newest_allowed_release(self):
        url = self.catalog([("001-old", self.distribution("26.1"), self.digest),
                            ("002-new", self.distribution("26.6"), self.digest),
                            ("003-other", self.distribution("27.1", "27.0", "28.0"), self.digest)])
        listing = self.install("-catalog", url, "-list").stdout
        self.assertIn("26.6 002-new, for macOS [26.2, 27.0)\n", listing)
        self.assertIn("27.1 003-other, for macOS [27.0, 28.0) (not this target)", listing)
        keep = self.base / "keep"
        result = self.install("-catalog", url, "-accept-license", "-pkg-dir", str(keep))
        self.assertIn("Command Line Tools for Xcode 26.6 (002-new)", result.stdout)
        self.assertTrue((self.root / CLT / "usr/bin/xcrun").exists())
        self.assertFalse((self.root / "tmp/.com.apple.dt.CommandLineTools.installondemand.in-progress").exists())
        receipt = plistlib.loads((self.root / "private/var/db/receipts/com.apple.pkg.CLTools_Executables.plist").read_bytes())
        self.assertEqual(receipt["PackageFileName"], "CLTools_Executables_Test.pkg")
        self.assertTrue((keep / "CLTools_Executables_Test.pkg").exists())

    def test_release_choice_and_refusals(self):
        url = self.catalog([("001-old", self.distribution("26.1"), self.digest)])
        self.assertIn("are not offered", self.install("-catalog", url, "-release", "26.6",
                                                      "-accept-license", status=1).stderr)
        self.assertIn("licence must be accepted", self.install("-catalog", url, status=1).stderr)
        self.assertFalse((self.root / CLT).exists())
        self.set_version("25.0")
        self.assertIn("no Command Line Tools are offered", self.install(
            "-catalog", url, "-accept-license", status=1).stderr)
        self.set_version("26.6")
        bad = self.catalog([("001-old", self.distribution("26.1"), "0" * 40)])
        self.assertIn("does not match the catalog", self.install(
            "-catalog", bad, "-accept-license", status=1).stderr)
        self.assertFalse((self.root / CLT).exists())

    def test_licence_text(self):
        url = self.catalog([("001-old", self.distribution("26.1"), self.digest)])
        # Without a terminal the licence is still printed before refusing.
        out = self.install("-catalog", url, status=1).stdout
        self.assertIn("TEST AGREEMENT\nClause “one”.\nhttp://x", out)
        self.assertNotIn("HelveticaNeue", out)


if __name__ == "__main__":
    unittest.main()
