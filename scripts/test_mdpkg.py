#!/usr/bin/env python3
"""Independent flat-package fixtures and behavioral tests for mdpkg.

The fixtures are built here from the format descriptions, not with Apple's
tools, and mdpkg is only ever run against temporary roots. MDPKG names the
binary; MC_PKG optionally names the pinned Midnight Commander package.
"""
import base64
import ctypes
import datetime
import gzip
import hashlib
import json
import lzma
import os
from pathlib import Path
import plistlib
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zlib


def cpio(entries, newc=False):
    out = bytearray()
    for ino, (name, mode, data) in enumerate([*entries, ("TRAILER!!!", 0, b"")], 1):
        name = name.encode() + b"\0"
        if newc:
            fields = [ino, mode, 0, 0, 1, 123, len(data), 0, 0, 0, 0, len(name), 0]
            out.extend(b"070701" + b"".join(f"{x:08x}".encode() for x in fields))
            out.extend(name)
            out.extend(b"\0" * (-len(out) % 4))
            out.extend(data)
            out.extend(b"\0" * (-len(out) % 4))
        else:
            fields = [(0, 6), (ino, 6), (mode, 6), (0, 6), (0, 6), (1, 6), (0, 6),
                      (123, 11), (len(name), 6), (len(data), 11)]
            out.extend(b"070707" + b"".join(f"{x:0{n}o}".encode() for x, n in fields))
            out.extend(name)
            out.extend(data)
    out.extend(b"\0" * (-len(out) % 512))
    return bytes(out)


def pbzx(data, chunk=4096, stored=()):
    """DATA as pbzx: CHUNK-sized pieces, each an .xz stream unless its index
    is in STORED. As in Apple's encoder, a piece that xz does not shrink is
    stored: equal lengths are what mark a chunk as stored."""
    out = bytearray(b"pbzx" + struct.pack(">Q", chunk))
    for i in range(0, max(len(data), 1), chunk):
        piece = data[i:i + chunk]
        packed = lzma.compress(piece, format=lzma.FORMAT_XZ)
        if i // chunk in stored or len(packed) >= len(piece):
            packed = piece
        out += struct.pack(">QQ", len(piece), len(packed)) + packed
    return bytes(out)


SIGNATURE_SIZE = 256


def xar(files, algorithm="sha1", named_header=True, stored=(), sign=None):
    """A XAR with zlib members, or stored ones for the names in STORED.
    Algorithms other than sha1/md5 are header type 3 ("other"), named in a
    64-byte header unless NAMED_HEADER is off. SIGN is (certificates,
    signer): DER certificates, leaf first, and a function from the TOC
    checksum to an RSA signature."""
    size = hashlib.new(algorithm).digest_size
    doc = ET.Element("xar")
    toc = ET.SubElement(doc, "toc")
    check = ET.SubElement(toc, "checksum", style=algorithm)
    ET.SubElement(check, "offset").text = "0"
    ET.SubElement(check, "size").text = str(size)
    heap = bytearray(b"\0" * size)
    if sign:
        signature = ET.SubElement(toc, "signature", style="RSA")
        ET.SubElement(signature, "offset").text = str(size)
        ET.SubElement(signature, "size").text = str(SIGNATURE_SIZE)
        data = ET.SubElement(ET.SubElement(signature, "KeyInfo"), "X509Data")
        for der in sign[0]:
            ET.SubElement(data, "X509Certificate").text = base64.encodebytes(der).decode()
        heap.extend(b"\0" * SIGNATURE_SIZE)
    directories = {"": toc}
    for path, value in files.items():
        parent = ""
        bits = path.split("/")
        for bit in bits[:-1]:
            current = parent + "/" + bit if parent else bit
            if current not in directories:
                node = ET.SubElement(directories[parent], "file")
                ET.SubElement(node, "name").text = bit
                ET.SubElement(node, "type").text = "directory"
                directories[current] = node
            parent = current
        node = ET.SubElement(directories[parent], "file")
        ET.SubElement(node, "name").text = bits[-1]
        ET.SubElement(node, "type").text = "file"
        data = ET.SubElement(node, "data")
        packed = value if path in stored else zlib.compress(value)
        for name, v in [("offset", len(heap)), ("length", len(packed)), ("size", len(value))]:
            ET.SubElement(data, name).text = str(v)
        ET.SubElement(data, "encoding", style="application/octet-stream" if path in stored
                      else "application/x-gzip")
        ET.SubElement(data, "archived-checksum", style=algorithm).text = hashlib.new(algorithm, packed).hexdigest()
        ET.SubElement(data, "extracted-checksum", style=algorithm).text = hashlib.new(algorithm, value).hexdigest()
        heap.extend(packed)
    xml = ET.tostring(doc)
    compressed = zlib.compress(xml)
    heap[:size] = hashlib.new(algorithm, compressed).digest()
    if sign:
        heap[size:size + SIGNATURE_SIZE] = sign[1](bytes(heap[:size]))
    kind = {"sha1": 1, "md5": 2}.get(algorithm, 3)
    name = algorithm.encode().ljust(36, b"\0") if kind == 3 and named_header else b""
    header = struct.pack(">4sHHQQI", b"xar!", 28 + len(name), 1, len(compressed), len(xml), kind) + name
    return header + compressed + heap


def component_files(entries, *, script=None, location="/", newc=False, payload=None,
                    identifier="org.minidarwin.test", groups=None):
    info = ET.Element("pkg-info", {"format-version": "2", "identifier": identifier,
                                   "version": "1.0", "install-location": location, "relocatable": "false"})
    ET.SubElement(info, "payload", numberOfFiles=str(len(entries)))
    if groups is not None:
        node = ET.SubElement(info, "groups")
        for tag, text in groups:
            ET.SubElement(node, tag).text = text
    files = {"Payload": gzip.compress(cpio(entries, newc)) if payload is None else payload,
             "Bom": b"BOMStore" + b"\0" * 24}
    if script is not None:
        ET.SubElement(ET.SubElement(info, "scripts"), "postinstall", file="./postinstall")
        files["Scripts"] = gzip.compress(cpio([("./postinstall", stat.S_IFREG | 0o755, script)]))
    files["PackageInfo"] = ET.tostring(info)
    return files


def distribution(*components, extra=""):
    """A static Distribution selecting each (identifier, member-dir) pair."""
    lines = "".join(f'<line choice="c{i}"/>' for i in range(len(components)))
    choices = "".join(f'<choice id="c{i}"><pkg-ref id="{ident}"/></choice>'
                      f'<pkg-ref id="{ident}">#{ref}</pkg-ref>'
                      for i, (ident, ref) in enumerate(components))
    return (f"<installer-gui-script minSpecVersion='1'>{extra}"
            f"<choices-outline>{lines}</choices-outline>{choices}</installer-gui-script>")


def set_xattr(path, name, value):
    """os.setxattr is Linux-only; call Darwin's setxattr(2) directly."""
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.setxattr(os.fsencode(path), name.encode(), value, len(value), 0, 0x0001):
        raise OSError(ctypes.get_errno(), "setxattr")


def get_xattr(path, name):
    libc = ctypes.CDLL(None, use_errno=True)
    buf = ctypes.create_string_buffer(256)
    n = libc.getxattr(os.fsencode(path), name.encode(), buf, 256, 0, 0x0001)
    if n < 0:
        raise OSError(ctypes.get_errno(), "getxattr")
    return buf.raw[:n]


def read_xar(path):
    """Test oracle; does not use the C installer or Apple Installer."""
    data = Path(path).read_bytes()
    magic, size, version, packed_size, unpacked_size, algorithm = struct.unpack(">4sHHQQI", data[:28])
    assert magic == b"xar!" and version == 1
    raw = zlib.decompress(data[size:size + packed_size])
    assert len(raw) == unpacked_size
    toc = ET.fromstring(raw).find("toc")
    heap = size + packed_size
    result = {}

    def walk(node, prefix=""):
        for item in node.findall("file"):
            name = prefix + item.findtext("name")
            content = item.find("data")
            if content is not None:
                offset, length = int(content.findtext("offset")), int(content.findtext("length"))
                value = data[heap + offset:heap + offset + length]
                if content.find("encoding").get("style") == "application/x-gzip":
                    value = zlib.decompress(value)
                assert len(value) == int(content.findtext("size"))
                result[name] = value
            walk(item, name + "/")
    walk(toc)
    return result


def read_odc(data):
    if data.startswith(b"\x1f\x8b"):
        data = gzip.decompress(data)
    position = 0
    records = []
    while position < len(data):
        header = data[position:position + 76]
        assert header[:6] == b"070707"
        mode = int(header[18:24], 8)
        uid, gid = int(header[24:30], 8), int(header[30:36], 8)
        namesize, size = int(header[59:65], 8), int(header[65:76], 8)
        name = data[position + 76:position + 76 + namesize - 1].decode()
        start = position + 76 + namesize
        value = data[start:start + size]
        assert len(value) == size
        position = start + size
        if name == "TRAILER!!!":
            break
        records.append((name.removeprefix("./") if name != "." else "", mode, uid, gid, value))
    return records


SCRIPTS_RESERVED = ".mdpkg-scripts"


PKI_CONFIG = """[req]
distinguished_name = dn
prompt = no
[dn]
CN = Test {name}
[ca]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
[leaf]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = critical,1.2.840.113635.100.4.1
[plain]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
"""


class TestPKI:
    """A root, an intermediate and leaves made with the openssl CLI, shaped
    like Apple's Software Update chain."""

    def __init__(self, directory):
        self.dir = Path(directory)
        self.dir.mkdir()
        self.make("root", None, "ca")
        self.make("inter", "root", "ca")
        self.make("leaf", "inter", "leaf")
        self.make("plain", "inter", "plain")
        self.make("stranger", None, "ca")

    def openssl(self, *args, data=None):
        return subprocess.run(["openssl", *args], cwd=self.dir, input=data,
                              capture_output=True, check=True).stdout

    def make(self, name, issuer, extensions):
        # LibreSSL ignores -subj when the config sets prompt = no.
        (self.dir / "pki.cnf").write_text(PKI_CONFIG.format(name=name))
        if issuer is None:
            self.openssl("req", "-new", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout",
                         f"{name}.key", "-out", f"{name}.pem",
                         "-days", "2", "-config", "pki.cnf", "-extensions", extensions)
        else:
            self.openssl("req", "-new", "-newkey", "rsa:2048", "-nodes", "-keyout", f"{name}.key",
                         "-out", f"{name}.csr", "-config", "pki.cnf")
            self.openssl("x509", "-req", "-in", f"{name}.csr", "-CA", f"{issuer}.pem", "-CAkey",
                         f"{issuer}.key", "-set_serial", str(len(list(self.dir.iterdir()))),
                         "-days", "2", "-extfile", "pki.cnf", "-extensions", extensions,
                         "-out", f"{name}.pem")
        self.openssl("x509", "-in", f"{name}.pem", "-outform", "DER", "-out", f"{name}.der")

    def der(self, name):
        return (self.dir / f"{name}.der").read_bytes()

    def signer(self, leaf):
        """(certificates, sign) for xar(): LEAF's chain, and its signature."""
        def sign(checksum):
            return self.openssl("pkeyutl", "-sign", "-inkey", f"{leaf}.key",
                                "-pkeyopt", "digest:sha1", data=checksum)
        return [self.der(leaf), self.der("inter")], sign


SU_DISTRIBUTION = """<installer-gui-script minSpecVersion="1">
  <options hostArchitectures="x86_64,arm64" customize="never"/>
  <title>SU_TITLE</title>
  <volume-check><allowed-os-versions><os-version before="27.0" min="26.2"/></allowed-os-versions></volume-check>
  <tags><tag>DTCommandLineTools</tag></tags>
  <platforms><client arch="i386"/><server arch="i386"/></platforms>
  <choices-outline ui="SoftwareUpdate"><line choice="su"/></choices-outline>
  <choices-outline><line choice="su"/></choices-outline>
  <choice id="su" start_selected="true" title="SU_TITLE" visible="Or0(my.target.mountpoint)">
    <pkg-ref id="org.minidarwin.test" auth="Root" packageIdentifier="org.minidarwin.test">REF</pkg-ref>
  </choice>
  <script>function Or0(prefix) {
    if (system.localizedString('SU_TITLE') !== 'Tools 1.0' ||
        system.localizedString('SU_DESCRIPTION') !== '&lt;p&gt;"quoted"&lt;/p&gt;' ||
        system.localizedString('MISSING') !== 'MISSING')
      throw new Error('localization');
    return true;
  }</script>
  <localization><strings language="English">/* comment */ "SU_TITLE" = "Tools 1.0";
"SU_DESCRIPTION" = '&lt;p&gt;"quoted"&lt;/p&gt;';
</strings></localization>
  <pkg-ref id="org.minidarwin.test" installKBytes="1" version="1.0"/>
</installer-gui-script>"""


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="mdpkg-tests-")
        self.base = Path(self.tmp.name)
        self.root = self.base / "root"
        self.root.mkdir()
        (self.root / "base-file").write_bytes(b"unchanged")
        self.pkg = self.base / "test.pkg"
        self.binary = os.environ["MDPKG"]
        self.entries = [("./usr", stat.S_IFDIR | 0o755, b""),
                        ("./usr/bin", stat.S_IFDIR | 0o755, b""),
                        ("./usr/bin/tool", stat.S_IFREG | 0o755, b"hello\n"),
                        ("./usr/bin/alias", stat.S_IFLNK | 0o777, b"tool")]

    def tearDown(self):
        self.tmp.cleanup()

    def package(self, entries=None, **kwargs):
        self.pkg.write_bytes(xar(component_files(self.entries if entries is None else entries, **kwargs)))

    def run_cli(self, *args, ok=True, env=None):
        result = subprocess.run([self.binary, *args], capture_output=True, text=True,
                                env=None if env is None else {**os.environ, **env}, timeout=15)
        self.assertEqual(result.returncode == 0, ok, result.stdout + result.stderr)
        return result

    def install(self, ok=True, *extra):
        return self.run_cli("install", "-pkg", str(self.pkg), "-root", str(self.root), *extra, ok=ok)

    def original_intact(self):
        self.assertEqual(sorted(p.name for p in self.root.iterdir()), ["base-file"])
        self.assertEqual((self.root / "base-file").read_bytes(), b"unchanged")

    def test_pbzx_payload_in_a_stored_member(self):
        blob = os.urandom(5000) + b"x" * 20000
        entries = [("./data", stat.S_IFDIR | 0o755, b""), ("./data/blob", stat.S_IFREG | 0o644, blob),
                   ("./data/link", stat.S_IFLNK | 0o777, b"blob")]
        files = component_files(entries, payload=pbzx(cpio(entries), chunk=4096, stored={1}))
        self.pkg.write_bytes(xar(files, stored={"Payload"}))
        self.install()
        self.assertEqual((self.root / "data/blob").read_bytes(), blob)
        self.assertEqual(os.readlink(self.root / "data/link"), "blob")

    def test_corrupt_pbzx(self):
        good = pbzx(cpio(self.entries), chunk=256)
        longer = bytearray(good)
        longer[19] += 1  # the first chunk claims one more decoded byte
        stray = bytearray(good)
        stray[-1] ^= 0xff
        for payload in [bytes(longer), good[:-5], good + b"\0\0\0", bytes(stray),
                        b"pbzx" + struct.pack(">Q", 1 << 40)]:
            with self.subTest(payload=payload[:24]):
                self.pkg.write_bytes(xar(component_files(self.entries, payload=payload),
                                         stored={"Payload"}))
                self.install(ok=False)
                self.original_intact()

    def test_signature_policy(self):
        pki = TestPKI(self.base / "pki")
        files = component_files(self.entries)
        anchor = {"MDPKG_TEST_SIGNATURE_ANCHOR": str(pki.dir / "root.der")}
        policy = ("-verify-signature", "apple-software-update")

        self.pkg.write_bytes(xar(files, sign=pki.signer("leaf")))
        inspect = self.run_cli("-pkginfo", "-pkg", str(self.pkg), *policy, env=anchor)
        self.assertIn("signed by /CN=Test leaf", inspect.stdout)
        stranger = {"MDPKG_TEST_SIGNATURE_ANCHOR": str(pki.dir / "stranger.der")}
        self.assertIn("does not verify", self.run_cli(
            "-pkginfo", "-pkg", str(self.pkg), *policy, env=stranger, ok=False).stderr)
        # The shipped anchor is Apple's, which signed none of these.
        self.run_cli("-pkginfo", "-pkg", str(self.pkg), *policy, ok=False)
        self.run_cli("-pkginfo", "-pkg", str(self.pkg), "-verify-signature", "nobody",
                     env=anchor, ok=False)

        signed = bytearray(self.pkg.read_bytes())
        header, packed = struct.unpack(">H", signed[4:6])[0], struct.unpack(">Q", signed[8:16])[0]
        signed[header + packed + 20 + 7] ^= 1
        self.pkg.write_bytes(bytes(signed))
        self.assertIn("does not match", self.run_cli(
            "-pkginfo", "-pkg", str(self.pkg), *policy, env=anchor, ok=False).stderr)

        self.pkg.write_bytes(xar(files, sign=pki.signer("plain")))
        self.assertIn("usage", self.run_cli(
            "-pkginfo", "-pkg", str(self.pkg), *policy, env=anchor, ok=False).stderr)

        self.pkg.write_bytes(xar(files))
        self.assertIn("not signed", self.run_cli(
            "-pkginfo", "-pkg", str(self.pkg), *policy, env=anchor, ok=False).stderr)
        self.run_cli("-pkginfo", "-pkg", str(self.pkg))

        self.pkg.write_bytes(xar(files, sign=pki.signer("leaf")))
        self.run_cli("install", "-pkg", str(self.pkg), "-root", str(self.root), *policy, env=anchor)
        self.assertTrue((self.root / "usr/bin/tool").exists())

    def su_product(self, ref="component.pkg", version="26.6", distribution=SU_DISTRIBUTION):
        product = self.base / "product"
        product.mkdir(exist_ok=True)
        (product / "component.pkg").write_bytes(xar(component_files(self.entries)))
        dist = product / "product.dist"
        dist.write_text(distribution.replace("REF", ref))
        plist = self.root / "System/Library/CoreServices/SystemVersion.plist"
        if version:
            plist.parent.mkdir(parents=True, exist_ok=True)
            plist.write_bytes(plistlib.dumps({"ProductVersion": version}))
        return dist, product

    def install_dist(self, dist, product, *extra, ok=True):
        return self.run_cli("-dist", str(dist), "-pkg-dir", str(product), "-target",
                            str(self.root), *extra, ok=ok)

    def test_software_update_distribution(self):
        dist, product = self.su_product()
        self.install_dist(dist, product)
        self.assertEqual((self.root / "usr/bin/tool").read_bytes(), b"hello\n")
        receipt = plistlib.loads((self.root / "private/var/db/receipts/org.minidarwin.test.plist").read_bytes())
        self.assertEqual(receipt["PackageFileName"], "component.pkg")

    def test_software_update_distribution_refusals(self):
        cases = [
            ({"version": "25.0"}, "allowed-os-versions"),
            ({"version": "27.0"}, "allowed-os-versions"),
            ({"version": None}, "macOS version"),
            ({"ref": "#component.pkg"}, "package file name"),
            ({"ref": "sub/component.pkg"}, "package file name"),
            ({"ref": "https://example.invalid/component.pkg"}, "package file name"),
            ({"distribution": SU_DISTRIBUTION.replace(' ui="SoftwareUpdate"', "")}, "duplicate"),
            ({"distribution": SU_DISTRIBUTION.replace("'Tools 1.0'", "'other'")}, "localization"),
            ({"distribution": SU_DISTRIBUTION.replace('"Tools 1.0";', '"Tools 1.0"')}, "expected ';'"),
        ]
        for kwargs, diagnostic in cases:
            with self.subTest(**{k: str(v)[:40] for k, v in kwargs.items()}):
                shutil.rmtree(self.root / "System", ignore_errors=True)
                dist, product = self.su_product(**kwargs)
                self.assertIn(diagnostic, self.install_dist(dist, product, ok=False).stderr)
                self.assertFalse((self.root / "usr").exists())
        dist, product = self.su_product()
        self.run_cli("-dist", str(dist), "-target", str(self.root), ok=False)
        (product / "component.pkg").write_bytes(self.pkg.read_bytes() if self.pkg.exists() else b"")
        files = {"Distribution": distribution(("org.minidarwin.test", "inner.pkg")).encode()}
        files.update({"inner.pkg/" + k: v for k, v in component_files(self.entries).items()})
        (product / "component.pkg").write_bytes(xar(files))
        self.assertIn("component package", self.install_dist(dist, product, ok=False).stderr)

    def test_components_may_share_identical_files(self):
        marker = [("./private", stat.S_IFDIR | 0o755, b""),
                  ("./private/tmp", stat.S_IFDIR | 0o755, b""),
                  ("./private/tmp/.marker", stat.S_IFREG | 0o644, b"same\n")]
        def product(second):
            files = {}
            for ident, directory, entries in [("org.minidarwin.a", "a.pkg", marker),
                                              ("org.minidarwin.b", "b.pkg", second)]:
                files.update({f"{directory}/{k}": v for k, v in
                              component_files(entries, identifier=ident).items()})
            files["Distribution"] = distribution(("org.minidarwin.a", "a.pkg"),
                                                 ("org.minidarwin.b", "b.pkg")).encode()
            self.pkg.write_bytes(xar(files, stored={"a.pkg/Payload", "b.pkg/Payload"}))
        product([*marker[:2], ("./private/tmp/.marker", stat.S_IFREG | 0o644, b"other\n")])
        self.assertIn("duplicate path", self.install(ok=False).stderr)
        self.original_intact()
        product([*marker[:2], ("./private/tmp/.marker", stat.S_IFREG | 0o600, b"same\n")])
        self.install(ok=False)
        product(marker)
        self.install()
        self.assertEqual((self.root / "private/tmp/.marker").read_bytes(), b"same\n")
        for ident in ["a", "b"]:
            inventory = plistlib.loads((self.root / f"private/var/db/mdpkg/org.minidarwin.{ident}.inventory.plist").read_bytes())
            self.assertIn("private/tmp/.marker", [e["path"] for e in inventory["Entries"]])

    def test_package_groups(self):
        self.package(groups=[("group", "com.apple.FindSystemFiles.pkg-group")])
        self.install()
        inventory = plistlib.loads((self.root / "private/var/db/mdpkg/org.minidarwin.test.inventory.plist").read_bytes())
        self.assertEqual(inventory["PackageGroups"], ["com.apple.FindSystemFiles.pkg-group"])
        receipt = plistlib.loads((self.root / "private/var/db/receipts/org.minidarwin.test.plist").read_bytes())
        self.assertNotIn("PackageGroups", receipt)
        for groups in [[("other", "x")], [("group", "bad/id")]]:
            shutil.rmtree(self.root / "private", ignore_errors=True)
            shutil.rmtree(self.root / "usr", ignore_errors=True)
            self.package(groups=groups)
            self.install(ok=False)

    def test_install_modes_links_receipts_and_inventory(self):
        self.package()
        self.install()
        tool = self.root / "usr/bin/tool"
        self.assertEqual(tool.read_bytes(), b"hello\n")
        self.assertEqual(stat.S_IMODE(tool.stat().st_mode), 0o755)
        self.assertEqual(tool.stat().st_mtime, 123)
        self.assertEqual(os.readlink(self.root / "usr/bin/alias"), "tool")
        receipt = plistlib.loads((self.root / "private/var/db/receipts/org.minidarwin.test.plist").read_bytes())
        self.assertEqual(receipt["PackageIdentifier"], "org.minidarwin.test")
        self.assertEqual(receipt["PackageVersion"], "1.0")
        self.assertEqual(receipt["InstallPrefixPath"], "/")
        self.assertEqual(receipt["PackageFileName"], "test.pkg")
        self.assertIsInstance(receipt["InstallDate"], datetime.datetime)
        inventory = plistlib.loads((self.root / "private/var/db/mdpkg/org.minidarwin.test.inventory.plist").read_bytes())
        self.assertEqual(len(inventory["Entries"]), 4)
        self.assertEqual((self.root / "base-file").read_bytes(), b"unchanged")
        self.install(ok=False)
        self.assertEqual(tool.read_bytes(), b"hello\n")

    def test_newc_and_nonroot_location(self):
        self.package(newc=True, location="/opt")
        self.install()
        self.assertEqual((self.root / "opt/usr/bin/tool").read_bytes(), b"hello\n")

    def test_raw_cpio(self):
        self.package(payload=cpio(self.entries))
        self.install()

    def test_traversal_absolute_escape_and_duplicates(self):
        for entries in [
            [("../../outside", stat.S_IFREG | 0o644, b"bad")],
            [("/absolute", stat.S_IFREG | 0o644, b"bad")],
            [("link", stat.S_IFLNK | 0o777, b"../outside")],
            [("same", stat.S_IFREG | 0o644, b"a"), ("same", stat.S_IFREG | 0o644, b"b")],
            [("a/b", stat.S_IFREG | 0o644, b"a"), ("a", stat.S_IFLNK | 0o777, b"base-file")],
            [("private/var/db/receipts/evil", stat.S_IFREG | 0o644, b"bad")],
        ]:
            with self.subTest(entries=entries):
                self.package(entries)
                self.install(ok=False)
                self.original_intact()

    def test_unsupported_types_and_hardlinks(self):
        for mode in [stat.S_IFIFO | 0o644, stat.S_IFCHR | 0o644, stat.S_IFREG | 0o4755]:
            self.package([("bad", mode, b"")])
            self.install(ok=False)
            self.original_intact()

    def test_collision(self):
        self.package([("base-file", stat.S_IFREG | 0o644, b"changed")])
        self.install(ok=False)
        self.original_intact()

    def test_existing_symlink_parent(self):
        outside = self.base / "outside"
        outside.mkdir()
        (self.root / "usr").symlink_to(outside, target_is_directory=True)
        self.package()
        self.install(ok=False)
        self.assertEqual(list(outside.iterdir()), [])

    def test_root_refusals(self):
        self.package()
        link = self.base / "root-link"
        link.symlink_to(self.root, target_is_directory=True)
        self.run_cli("install", "-pkg", str(self.pkg), "-root", str(link), ok=False)
        self.run_cli("install", "-pkg", str(self.pkg), "-root", "/nix/store/fake-root", ok=False)
        self.original_intact()

    def live(self, *args, ok=True):
        """The running root is "/", which a test must never install into:
        MDPKG_TEST_LIVE_ROOT makes mdpkg treat the temporary root that way."""
        return self.run_cli(*args, ok=ok, env={"MDPKG_TEST_LIVE_ROOT": str(self.root)})

    def test_live_install_in_place_with_direct_scripts(self):
        files = component_files(self.entries, script=b"#!/bin/sh\nexit 0\n")
        files["Scripts"] = gzip.compress(cpio([
            ("./postinstall", stat.S_IFREG | 0o755,
             b'#!/bin/sh\n[ -n "$COMMAND_LINE_INSTALL" ] || exit 3\n'
             b'[ "$3" = "$DSTVOLUME" ] && [ "$2" = "$DSTROOT" ] || exit 4\n'
             b'[ "$1" = "$PACKAGE_PATH" ] || exit 5\n'
             b'echo "$2" > "$DSTVOLUME/script-ran"\n'),
        ]))
        self.pkg.write_bytes(xar(files))
        before = (self.root / "base-file").stat()
        self.live("-pkg", str(self.pkg), "-target", str(self.root))
        self.assertEqual((self.root / "usr/bin/tool").read_bytes(), b"hello\n")
        self.assertEqual(os.readlink(self.root / "usr/bin/alias"), "tool")
        self.assertEqual((self.root / "script-ran").read_text().strip(), str(self.root.resolve()))
        self.assertEqual((self.root / "base-file").stat().st_ino, before.st_ino)
        self.assertTrue((self.root / "private/var/db/receipts/org.minidarwin.test.plist").exists())
        self.assertFalse(Path(str(self.root) + ".mdpkg-transaction").exists())
        self.assertFalse((self.root / SCRIPTS_RESERVED).exists())
        self.live("-pkg", str(self.pkg), "-target", str(self.root), ok=False)

    def test_live_scripts_see_the_installing_user(self):
        files = component_files(self.entries, script=b"#!/bin/sh\nexit 0\n")
        files["Scripts"] = gzip.compress(cpio([
            ("./postinstall", stat.S_IFREG | 0o755,
             b'#!/bin/sh\necho "$USER:$HOME" > "$DSTVOLUME/script-user"\n'),
        ]))
        self.pkg.write_bytes(xar(files))
        self.run_cli("-pkg", str(self.pkg), "-target", str(self.root),
                     env={"MDPKG_TEST_LIVE_ROOT": str(self.root),
                          "USER": "installer", "HOME": "/var/installer"})
        self.assertEqual((self.root / "script-user").read_text().strip(),
                         "installer:/var/installer")

    def test_live_failure_removes_what_was_created(self):
        self.package(script=b"#!/bin/sh\nexit 9\n")
        self.live("-pkg", str(self.pkg), "-target", str(self.root), ok=False)
        self.assertFalse((self.root / "usr").exists())
        self.assertFalse((self.root / "private/var/db/receipts").exists())
        self.assertEqual((self.root / "base-file").read_bytes(), b"unchanged")

    def test_live_collision_changes_nothing(self):
        (self.root / "usr/bin").mkdir(parents=True)
        (self.root / "usr/bin/tool").write_bytes(b"mine")
        self.package()
        self.live("-pkg", str(self.pkg), "-target", str(self.root), ok=False)
        self.assertEqual((self.root / "usr/bin/tool").read_bytes(), b"mine")
        self.assertFalse((self.root / "usr/bin/alias").exists())

    def test_live_follows_absolute_links_inside_the_root_only(self):
        (self.root / "private/etc").mkdir(parents=True)
        (self.root / "etc").symlink_to("/private/etc")
        self.package([("./etc", stat.S_IFDIR | 0o755, b""), ("./etc/conf", stat.S_IFREG | 0o644, b"x")])
        self.live("-pkg", str(self.pkg), "-target", str(self.root))
        self.assertEqual((self.root / "private/etc/conf").read_bytes(), b"x")
        # Offline, the same link is a way out of the root and is refused.
        offline = self.base / "offline"
        offline.mkdir()
        (offline / "private").mkdir()
        (offline / "private/etc").mkdir()
        (offline / "etc").symlink_to("/private/etc")
        self.run_cli("-pkg", str(self.pkg), "-target", str(offline), ok=False)

    def test_live_requires_root_for_the_real_one(self):
        self.package()
        if os.geteuid() != 0:
            self.run_cli("-pkg", str(self.pkg), "-target", "/", ok=False)

    def test_macos_style_options(self):
        self.package()
        self.assertIn("org.minidarwin.test", self.run_cli("-pkginfo", "-pkg", str(self.pkg)).stdout)
        self.run_cli("-pkg", str(self.pkg), "-target", str(self.root))
        self.assertTrue((self.root / "usr/bin/tool").exists())
        self.run_cli("--pkg", str(self.pkg), "--root", str(self.root), ok=False)
        self.run_cli("-pkg", str(self.pkg), "-target", str(self.root), "-root", str(self.root), ok=False)

    def test_scripts_require_runner_and_failure_rolls_back(self):
        self.package(script=b"#!/bin/sh\nexit 0\n")
        self.install(ok=False)
        self.original_intact()
        runner = self.base / "failing-runner"
        runner.write_text("#!/bin/sh\nexit 7\n")
        runner.chmod(0o755)
        self.install(False, "-script-runner", str(runner))
        self.original_intact()
        self.assertFalse(Path(str(self.root) + ".mdpkg-transaction").exists())

    def test_preinstall_and_postinstall_inventory(self):
        files = component_files(self.entries, script=b"#!/bin/sh\nexit 0\n")
        info = ET.fromstring(files["PackageInfo"])
        ET.SubElement(info.find("scripts"), "preinstall", file="./preinstall")
        files["PackageInfo"] = ET.tostring(info)
        files["Scripts"] = gzip.compress(cpio([
            ("./preinstall", stat.S_IFREG | 0o755, b"#!/bin/sh\nexit 0\n"),
            ("./postinstall", stat.S_IFREG | 0o755, b"#!/bin/sh\nexit 0\n"),
        ]))
        self.pkg.write_bytes(xar(files))
        runner = self.base / "test-runner"
        runner.write_text(f"#!{sys.executable}\n" + """import os, sys
from pathlib import Path
script, workdir, package, location, root = sys.argv[1:]
assert os.getcwd() == workdir
assert os.environ['DSTVOLUME'] == root
assert os.environ['DSTROOT'] == location
assert os.environ['PACKAGE_PATH'] == package
assert location == root
if Path(script).name == 'preinstall':
    Path(root, 'pre-created').write_text('pre')
else:
    Path(root, 'usr/bin/tool').write_text('post')
""")
        runner.chmod(0o755)
        self.install(True, "-script-runner", str(runner))
        inventory = plistlib.loads((self.root / "private/var/db/mdpkg/org.minidarwin.test.inventory.plist").read_bytes())
        changes = {(entry["path"], entry["change"]) for entry in inventory["Entries"] if entry["origin"] == "script"}
        self.assertIn(("pre-created", "created"), changes)
        self.assertIn(("usr/bin/tool", "modified"), changes)

    def test_existing_metadata_rejected_without_loss(self):
        path = self.root / "base-file"
        try:
            set_xattr(path, "com.minidarwin.test", b"preserve")
        except OSError:
            self.skipTest("extended attributes not supported")
        self.package()
        self.install(ok=False)
        self.original_intact()
        self.assertEqual(get_xattr(path, "com.minidarwin.test"), b"preserve")

    def test_existing_nanosecond_timestamp_preserved(self):
        path = self.root / "base-file"
        os.utime(path, ns=(1234567890123456789, 1234567890123456789))
        self.package()
        self.install()
        self.assertEqual(path.stat().st_mtime_ns, 1234567890123456789)

    def test_checksum_and_truncation(self):
        self.package()
        good = self.pkg.read_bytes()
        for bad in [good[:20], good[:-10], good[:60] + bytes([good[60] ^ 1]) + good[61:]]:
            self.pkg.write_bytes(bad)
            self.install(ok=False)
            self.original_intact()

    def test_decompression_limit(self):
        self.package()
        data = bytearray(self.pkg.read_bytes())
        data[16:24] = struct.pack(">Q", 512 * 1024 * 1024)
        self.pkg.write_bytes(data)
        self.install(ok=False)
        self.original_intact()

    def test_invalid_cpio(self):
        for payload in [b"pbzxunsupported", cpio(self.entries)[:150], cpio(self.entries) + b"nonzero"]:
            self.package(payload=payload)
            self.install(ok=False)
            self.original_intact()

    def test_distribution(self):
        files = {"component.pkg/" + name: data for name, data in component_files(self.entries).items()}
        distribution = '''<installer-script><choices-outline><line choice="c"/></choices-outline>
<choice id="c"><pkg-ref id="org.minidarwin.test"/></choice>
<pkg-ref id="org.minidarwin.test">#component.pkg</pkg-ref></installer-script>'''
        files["Distribution"] = distribution.encode()
        self.pkg.write_bytes(xar(files))
        self.install()
        self.assertTrue((self.root / "usr/bin/tool").exists())
        for bad in [distribution.replace('<choice id="c">', '<choice id="c" selected="system.version()">'),
                    distribution.replace("</installer-script>", "<installation-check script='check()'/></installer-script>")]:
            files["Distribution"] = bad.encode()
            self.pkg.write_bytes(xar(files))
            self.assertIn("unresolved", self.run_cli("inspect", "-pkg", str(self.pkg)).stdout)
            self.run_cli("inspect", "-pkg", str(self.pkg), "-target", str(self.root), ok=False)
        files["Distribution"] = distribution.replace("#component.pkg", "https://example.invalid/pkg").encode()
        self.pkg.write_bytes(xar(files))
        self.run_cli("inspect", "-pkg", str(self.pkg), ok=False)

    def js_package(self, *, script="", check="true", volume="true", choice="",
                   active="true", extra="", components=None):
        files = {}
        components = components or [("org.minidarwin.test", "component.pkg", self.entries)]
        for ident, directory, entries in components:
            files.update({directory + "/" + name: data
                          for name, data in component_files(entries, identifier=ident).items()})
        dist = ET.Element("installer-gui-script")
        ET.SubElement(dist, "script").text = script
        ET.SubElement(dist, "installation-check", script=check)
        ET.SubElement(dist, "volume-check", script=volume)
        outline = ET.SubElement(dist, "choices-outline")
        for i, (ident, directory, _) in enumerate(components):
            ET.SubElement(outline, "line", choice=f"c{i}")
            node = ET.SubElement(dist, "choice", id=f"c{i}", title=f"Choice {i}",
                                 description="Description")
            if choice:
                node.set("selected", choice)
            ET.SubElement(node, "pkg-ref", id=ident)
            ET.SubElement(dist, "pkg-ref", id=ident, version="1.0", active=active).text = "#" + directory
        if extra:
            dist.append(ET.fromstring(extra))
        files["Distribution"] = ET.tostring(dist)
        self.pkg.write_bytes(xar(files))
        return files, dist

    def js_inspect(self, ok=True):
        return self.run_cli("-pkginfo", "-pkg", str(self.pkg), "-target", str(self.root), ok=ok)

    def root_state(self):
        return {str(p.relative_to(self.root)): (p.lstat().st_mode,
                os.readlink(p) if p.is_symlink() else p.read_bytes() if p.is_file() else None)
                for p in self.root.rglob("*")}

    def test_js_shared_script_checks_and_choice_context(self):
        self.js_package(script="let calls=0; function check(){ calls++; return calls === 1; }",
                        check="check()", volume="calls === 1",
                        choice="my.title === 'Choice 0' && my.description === 'Description' && "
                               "my.packages[0].identifier === 'org.minidarwin.test' && "
                               "my.packages[0].version === '1.0' && choices.c0 === my")
        self.install()
        self.assertTrue((self.root / "usr/bin/tool").exists())

    def test_js_cdata_and_escaped_script(self):
        files, dist = self.js_package(script="function yes(){ return 1 < 2 && true; }", check="yes()")
        self.assertIn(b"&lt;", files["Distribution"])
        self.js_inspect()
        script = dist.find("script")
        script.text = "PLACEHOLDER"
        files["Distribution"] = ET.tostring(dist).replace(b"PLACEHOLDER",
            b"<![CDATA[function yes(){ return 1 < 2 && true; }]]>")
        self.pkg.write_bytes(xar(files))
        self.js_inspect()
        self.original_intact()

    def test_js_checks_reject_before_writes_and_report_result(self):
        for name in ["installation", "volume"]:
            with self.subTest(check=name):
                kwargs = {"check" if name == "installation" else "volume": "reject()"}
                self.js_package(script="function reject(){my.result.type='Fatal'; "
                    "my.result.title='No install'; my.result.message='Wrong volume'; return false;}", **kwargs)
                result = self.install(ok=False)
                self.assertIn(name + "-check", result.stderr)
                self.assertIn("No install: Wrong volume", result.stderr)
                self.original_intact()
                self.assertFalse(Path(str(self.root) + ".mdpkg-transaction").exists())
                self.js_inspect(ok=False)

    def test_js_warning_continues_and_result_is_reset(self):
        self.js_package(script="function warn(){my.result.type='Warn'; my.result.title='Notice';"
            "my.result.message='Continue'; return false;} function volume(){"
            "return my.result.type === '' && my.result.title === '' && my.result.message === '';}",
            check="warn()", volume="volume()")
        result = self.install()
        self.assertIn("warning: Notice: Continue", result.stderr)
        self.assertTrue((self.root / "usr/bin/tool").exists())

    def test_js_check_may_be_a_statement(self):
        # MacPorts' Distribution says script="InstallationCheck();".
        self.js_package(script="function ok(){return true;} function no(){return false;}",
                        check="ok();", volume="ok();")
        self.install()
        self.assertTrue((self.root / "usr/bin/tool").exists())
        self.js_package(script="function no(){my.result.type='Fatal'; return false;}", check="no();")
        self.install(ok=False)

    def test_system_version_override(self):
        self.js_package(script="function v(){return system.version.ProductVersion === '15.6';}", check="v();")
        self.install(ok=False)
        self.original_intact()
        self.js_inspect(ok=False)
        self.assertIn("payload entries", self.run_cli("-pkginfo", "-pkg", str(self.pkg), "-target",
                                                       str(self.root), "-system-version", "15.6").stdout)
        self.install(True, "-system-version", "15.6")
        self.assertTrue((self.root / "usr/bin/tool").exists())
        for bad in ["", "15.", ".15", "15..6", "15.x", "15 "]:
            self.run_cli("-pkginfo", "-pkg", str(self.pkg), "-system-version", bad, ok=False)

    def test_allowed_os_versions_are_reported_and_validated(self):
        self.js_package(extra='<allowed-os-versions><os-version min="15" before="16.0"/>'
                              '<os-version min="14.4"/></allowed-os-versions>')
        out = self.run_cli("-pkginfo", "-pkg", str(self.pkg), "-target", str(self.root)).stdout
        self.assertIn("allowed OS versions (not enforced): >= 15 and < 16.0, >= 14.4", out)
        for bad in ['<allowed-os-versions><os-version min="15" only="1"/></allowed-os-versions>',
                    '<allowed-os-versions><os-version min="x"/></allowed-os-versions>',
                    '<allowed-os-versions><os-version/></allowed-os-versions>']:
            with self.subTest(bad=bad):
                self.js_package(extra=bad)
                self.run_cli("-pkginfo", "-pkg", str(self.pkg), "-target", str(self.root), ok=False)

    def test_skip_scripts_installs_payload_without_running_them(self):
        marker = self.base / "ran"
        self.package(script=("#!/bin/sh\ntouch %s\nexit 1\n" % marker).encode())
        self.install(ok=False)
        result = self.install(True, "-skip-scripts")
        self.assertIn("skipping org.minidarwin.test postinstall", result.stderr)
        self.assertFalse(marker.exists())
        self.assertTrue((self.root / "usr/bin/tool").exists())
        self.assertTrue((self.root / "private/var/db/receipts/org.minidarwin.test.plist").exists())

    def test_js_inspection_is_read_only_and_target_free_does_not_execute(self):
        self.js_package(script="throw new Error('top level executed');")
        result = self.run_cli("inspect", "-pkg", str(self.pkg))
        self.assertIn("selection unresolved", result.stdout)
        self.assertIn("org.minidarwin.test 1.0", result.stdout)
        self.assertNotIn("payload entries", result.stdout)
        self.original_intact()
        self.assertFalse(Path(str(self.root) + ".mdpkg-lock").exists())
        self.assertIn("top level executed", self.js_inspect(ok=False).stderr)
        self.js_package(check="system.files.fileExistsAtPath(my.target.mountpoint + '/base-file')")
        before = self.root_state()
        self.assertIn("payload entries", self.js_inspect().stdout)
        self.assertEqual(before, self.root_state())
        self.assertFalse(Path(str(self.root) + ".mdpkg-lock").exists())
        self.assertFalse(Path(str(self.root) + ".mdpkg-transaction").exists())

    def test_js_syntax_errors_are_caught_without_target(self):
        for kwargs in [{"script": "function broken( {"}, {"choice": "true &&"},
                       {"check": "check("}, {"active": "true &&"}]:
            with self.subTest(kwargs=kwargs):
                self.js_package(**kwargs)
                self.assertIn("SyntaxError", self.run_cli("inspect", "-pkg", str(self.pkg), ok=False).stderr)
                self.original_intact()

    def test_js_fixed_point_and_selected_receipts(self):
        entries = lambda name: [(f"./{name}", stat.S_IFREG | 0o644, name.encode())]
        components = [(f"org.minidarwin.{name}", name + ".pkg", entries(name)) for name in ["one", "two", "three"]]
        files, dist = self.js_package(components=components)
        nodes = dist.findall("choice")
        nodes[0].set("selected", "choices.c1.selected")
        nodes[1].set("selected", "choices.c2.selected")
        nodes[2].set("selected", "false")
        # A fourth, selected package proves a nonempty stable result.
        ET.SubElement(dist.find("choices-outline"), "line", choice="c3")
        choice = ET.SubElement(dist, "choice", id="c3", enabled="false", visible="false")
        ET.SubElement(choice, "pkg-ref", id="org.minidarwin.keep")
        ET.SubElement(dist, "pkg-ref", id="org.minidarwin.keep").text = "#keep.pkg"
        files.update({"keep.pkg/" + name: data for name, data in component_files(entries("keep"),
                      identifier="org.minidarwin.keep").items()})
        files["Distribution"] = ET.tostring(dist)
        self.pkg.write_bytes(xar(files))
        self.install()
        self.assertTrue((self.root / "keep").exists())
        for name in ["one", "two", "three"]:
            self.assertFalse((self.root / name).exists())
        receipts = self.root / "private/var/db/receipts"
        self.assertEqual(sorted(p.name for p in receipts.glob("*.plist")), ["org.minidarwin.keep.plist"])

    def test_js_initial_flags_and_explicit_selection(self):
        files, dist = self.js_package(choice="!my.enabled && !my.visible")
        node = dist.find("choice")
        for key in ["start_selected", "start_enabled", "start_visible"]:
            node.set(key, "false")
        files["Distribution"] = ET.tostring(dist)
        self.pkg.write_bytes(xar(files))
        self.install()
        self.assertTrue((self.root / "usr/bin/tool").exists())

    def test_js_unstable_selection(self):
        self.js_package(choice="!my.selected")
        self.assertIn("64 passes", self.install(ok=False).stderr)
        self.original_intact()

    def test_js_merged_inactive_reference(self):
        components = [("org.minidarwin.one", "one.pkg", [("./one", stat.S_IFREG | 0o644, b"one")]),
                      ("org.minidarwin.two", "two.pkg", [("./two", stat.S_IFREG | 0o644, b"two")])]
        files, dist = self.js_package(components=components)
        first = dist.findall("pkg-ref")[0]
        del first.attrib["active"]
        dist.findall("choice")[0].find("pkg-ref").set("active", "system.compareVersions('1.0', '2') > 0")
        # An inactive component's payload must not be decoded.
        files["one.pkg/Payload"] = b"not cpio"
        files["Distribution"] = ET.tostring(dist)
        self.pkg.write_bytes(xar(files))
        self.install()
        self.assertFalse((self.root / "one").exists())
        self.assertTrue((self.root / "two").exists())

    def test_static_inactive_reference_and_initial_selection(self):
        files, dist = self.js_package(components=[
            ("org.minidarwin.one", "one.pkg", [("./one", stat.S_IFREG | 0o644, b"one")]),
            ("org.minidarwin.two", "two.pkg", [("./two", stat.S_IFREG | 0o644, b"two")])])
        for node in [*dist.findall("script"), *dist.findall("installation-check"), *dist.findall("volume-check")]:
            dist.remove(node)
        dist.findall("pkg-ref")[0].set("active", "false")
        files["Distribution"] = ET.tostring(dist)
        self.pkg.write_bytes(xar(files))
        self.install()
        self.assertFalse((self.root / "one").exists())
        self.assertTrue((self.root / "two").exists())

    def test_js_helpers_versions_and_sysctl(self):
        self.js_package(check="helpers()", script="""
            function helpers() {
              system.log('helper fixture');
              let versions = [['1','1.0.0',0], ['1.02','1.2',0], ['10.3.1','10.4',-1],
                ['1.10','1.9',1], ['999999999999999999999','2',1], ['0.0','0',0]];
              if (!versions.every(v => system.compareVersions(v[0],v[1]) === v[2])) return false;
              if (system.propertiesOf({a:1,b:2}).sort().join(',') !== 'a,b') return false;
              return typeof system.sysctl('hw.machine') === 'string' &&
                typeof system.sysctl('hw.model') === 'string' && system.sysctl('hw.ncpu') > 0 &&
                system.sysctl('hw.memsize') > 0 && typeof system.sysctl('kern.osrelease') === 'string' &&
                typeof system.sysctl('kern.osversion') === 'string';
            }
        """)
        self.assertIn("JS: helper fixture", self.js_inspect().stderr)
        self.original_intact()

    def test_js_host_and_target_metadata_and_receipts(self):
        target_version = self.root / "System/Library/CoreServices/SystemVersion.plist"
        target_version.parent.mkdir(parents=True)
        target_version.write_bytes(plistlib.dumps({"ProductVersion": "999.1", "ProductBuildVersion": "test"}))
        receipt = self.root / "private/var/db/receipts/org.example.existing.plist"
        receipt.parent.mkdir(parents=True)
        receipt.write_bytes(plistlib.dumps({"PackageIdentifier": "org.example.existing", "PackageVersion": "2.1"}))
        self.js_package(check="metadata()", script="""
            function metadata() {
              let host = system.version;
              return host.ProductVersion !== '999.1' && typeof host.ProductVersion === 'string' &&
                my.target.systemVersion === '999.1' && my.target.availableKilobytes > 0 &&
                my.target.receiptForIdentifier('org.example.existing').PackageVersion === '2.1' &&
                my.target.receiptForIdentifier('org.example.missing') === null;
            }
        """)
        # Nix's sandbox may not expose host SystemVersion.plist; assert the
        # distinction directly when outside it and missing-host semantics inside it.
        if not Path("/System/Library/CoreServices/SystemVersion.plist").exists():
            files, dist = self.js_package(check="system.version === null && my.target.systemVersion === '999.1' && "
                "my.target.availableKilobytes > 0 && my.target.receiptForIdentifier('org.example.existing').PackageVersion === '2.1'")
        before = self.root_state()
        self.js_inspect()
        self.assertEqual(before, self.root_state())

    def test_js_missing_target_version_does_not_use_host(self):
        self.js_package(check="my.target.systemVersion === null && "
            "my.target.receiptForIdentifier('org.example.missing') === null")
        self.js_inspect()
        self.original_intact()

    def test_js_xml_plist_types_and_bundle_reads(self):
        value = {"s": "text & < >", "i": -42, "r": 1.25, "b": True, "n": False,
                 "a": [1, "two"], "nested": {"__proto__": "safe"}, "data": b"\x00\xff\x01",
                 "empty": b"", "date": datetime.datetime(2020, 1, 2, 3, 4, 5)}
        (self.root / "values.plist").write_bytes(plistlib.dumps(value))
        bundle = self.root / "Example.app/Contents"
        bundle.mkdir(parents=True)
        (bundle / "Info.plist").write_bytes(plistlib.dumps({"CFBundleIdentifier": "org.example.app"}))
        self.js_package(check="plists()", script="""
            function plists() {
              let base = my.target.mountpoint;
              let p = system.files.plistAtPath(base + '/values.plist');
              return p.s === 'text & < >' && p.i === -42 && p.r === 1.25 && p.b && !p.n &&
                p.a[1] === 'two' && p.nested.__proto__ === 'safe' && p.data instanceof Uint8Array &&
                p.data.length === 3 && p.data[1] === 255 && p.empty.length === 0 &&
                p.date.toISOString() === '2020-01-02T03:04:05.000Z' &&
                system.files.bundleAtPath(base + '/Example.app').CFBundleIdentifier === 'org.example.app' &&
                system.files.bundleAtPath(base + '/Missing.app') === null &&
                system.files.plistAtPath(base + '/missing.plist') === null &&
                !system.files.fileExistsAtPath(base + '/missing') && system.files.fileExistsAtPath(base);
            }
        """)
        before = self.root_state()
        self.js_inspect()
        self.assertEqual(before, self.root_state())

    def test_js_binary_malformed_and_hostile_plists(self):
        values = [plistlib.dumps({"x": 1}, fmt=plistlib.FMT_BINARY), b"<plist><dict><key>x</key></dict></plist>",
            b'<!DOCTYPE plist [<!ENTITY x SYSTEM "file:///etc/passwd">]><plist><string>&x;</string></plist>',
            b'<plist><dict><key>x</key><true/><key>x</key><false/></dict></plist>',
            b'<plist><integer>1oops</integer></plist>', b'<plist><data>bad?</data></plist>',
            b'<plist><date>2020-02-31T03:04:05Z</date></plist>',
            b'<plist><dict><key>x</key>garbage<string>y</string></dict></plist>']
        for value in values:
            with self.subTest(value=value[:50]):
                (self.root / "bad.plist").write_bytes(value)
                self.js_package(check="system.files.plistAtPath(my.target.mountpoint + '/bad.plist') !== null")
                before = self.root_state()
                self.js_inspect(ok=False)
                self.assertEqual(before, self.root_state())

    def test_js_confined_target_reads_and_aliases(self):
        (self.root / "etc").symlink_to("private/etc")
        private = self.root / "private/etc"
        private.mkdir(parents=True)
        (private / "safe.plist").write_bytes(plistlib.dumps({"ok": True}))
        self.js_package(check="system.files.plistAtPath(my.target.mountpoint + '/etc/safe.plist').ok")
        self.js_inspect()
        outside = self.base / "outside.plist"
        outside.write_bytes(plistlib.dumps({"ok": True}))
        (self.root / "escape.plist").symlink_to("../outside.plist")
        (self.root / "absolute.plist").symlink_to(str(outside))
        for path in ["escape.plist", "absolute.plist"]:
            for prefix in [str(self.root), str(self.root.resolve())]:
                with self.subTest(path=path, prefix=prefix):
                    # JSON supplies exact JS quoting, including spaces.
                    self.js_package(check=f"system.files.plistAtPath({json.dumps(prefix + '/' + path)}).ok")
                    before = self.root_state()
                    self.assertRegex(self.js_inspect(ok=False).stderr, "root|absolute symlink")
                    self.assertEqual(before, self.root_state())

    def test_js_unsupported_apis_are_explicit(self):
        for expression in ["system.run('x')", "system.runOnce('x')", "system.localizedStringWithFormat('x')",
            "system.defaults.foo", "system.applications.foo", "system.ioregistry.foo", "system.users",
            "my.packageUpgradeAction", "system.sysctl('kern.unsupported')",
            "system.compareVersions('1beta','2')"]:
            with self.subTest(expression=expression):
                self.js_package(choice=expression)
                result = self.install(ok=False)
                self.assertRegex(result.stderr, "unsupported|supports numeric")
                self.original_intact()

    def test_js_limits_and_missing_module_access(self):
        cases = [("while(true){}", "interrupted"),
                 ("let a=[]; while(true){a.push('x'.repeat(1000000))}", "out of memory"),
                 ("function f(){return f()} f()", "stack overflow"),
                 ("std.open('x')", "std"), ("os.exec(['true'])", "os")]
        for script, diagnostic in cases:
            with self.subTest(script=script):
                self.js_package(script=script)
                result = self.install(ok=False)
                self.assertIn(diagnostic, result.stderr)
                self.original_intact()
                self.assertFalse(Path(str(self.root) + ".mdpkg-transaction").exists())

    def test_js_shared_deadline_and_async_jobs(self):
        self.js_package(script="function spin(){let end=Date.now()+3000;"
            "while(Date.now()<end){} return true;}", check="spin()", volume="spin()")
        self.assertRegex(self.install(ok=False).stderr, "interrupted|time limit")
        self.original_intact()
        for script in ["Promise.resolve().then(() => true)", "import('std')"]:
            with self.subTest(script=script):
                self.js_package(script=script)
                self.assertIn("unsupported", self.install(ok=False).stderr)
                self.original_intact()

    def test_js_live_checks_run_before_lock_and_confine_absolute_links(self):
        self.js_package(check="false")
        self.run_cli("install", "-pkg", str(self.pkg), "-target", str(self.root), ok=False,
                     env={"MDPKG_TEST_LIVE_ROOT": str(self.root)})
        self.original_intact()
        (self.root / "etc").symlink_to("/private/etc")
        (self.root / "private/etc").mkdir(parents=True)
        (self.root / "private/etc/value.plist").write_bytes(plistlib.dumps({"ok": True}))
        self.js_package(check="system.files.plistAtPath(my.target.mountpoint + '/etc/value.plist').ok")
        self.run_cli("install", "-pkg", str(self.pkg), "-target", str(self.root),
                     env={"MDPKG_TEST_LIVE_ROOT": str(self.root)})
        self.assertTrue((self.root / "usr/bin/tool").exists())

    def test_js_distribution_structure_rejections(self):
        for extra in ["<script src='outside.js'/>", "<installation-check script='true'/>",
                      "<choice id='c0'/>", "<locator/>"]:
            with self.subTest(extra=extra):
                self.js_package(extra=extra)
                self.run_cli("inspect", "-pkg", str(self.pkg), ok=False)
                self.original_intact()
        files, dist = self.js_package(choice="1 < 2", extra="<options require-scripts='false'/>")
        self.run_cli("inspect", "-pkg", str(self.pkg), ok=False)

    def test_checksum_algorithms(self):
        files = component_files(self.entries)
        for algorithm, named in [("sha1", True), ("md5", True), ("sha256", True),
                                 ("sha512", True), ("sha256", False)]:
            with self.subTest(algorithm=algorithm, named=named):
                self.pkg.write_bytes(xar(files, algorithm, named))
                self.run_cli("inspect", "-pkg", str(self.pkg))
        data = bytearray(xar(files, "sha256"))
        data[28:34] = b"sha512"  # header and TOC disagree
        self.pkg.write_bytes(data)
        self.run_cli("inspect", "-pkg", str(self.pkg), ok=False)

    def test_product_archive(self):
        files = {}
        for ident, ref, tool in [("org.minidarwin.one", "Tool One.pkg", "one"),
                                 ("org.minidarwin.two", "two.pkg", "two")]:
            component = component_files([("./usr", stat.S_IFDIR | 0o755, b""),
                                         (f"./usr/{tool}", stat.S_IFREG | 0o644, b"x")],
                                        identifier=ident)
            info = ET.fromstring(component["PackageInfo"])
            ET.SubElement(ET.SubElement(info, "bundle-version"), "bundle", id="org.example.app")
            component["PackageInfo"] = ET.tostring(info)
            files.update({f"{ref}/{name}": data for name, data in component.items()})
        files["Distribution"] = distribution(
            ("org.minidarwin.one", "Tool%20One.pkg"), ("org.minidarwin.two", "two.pkg"),
            extra='<options hostArchitectures="arm64,x86_64" customize="never"/>').encode()
        self.pkg.write_bytes(xar(files))
        result = self.run_cli("inspect", "-pkg", str(self.pkg))
        self.assertIn("org.minidarwin.one 1.0 /: 2 payload entries", result.stdout)
        self.assertIn("host architectures (not enforced): arm64,x86_64", result.stdout)
        self.install()
        self.assertTrue((self.root / "usr/one").exists() and (self.root / "usr/two").exists())
        for ident in ("org.minidarwin.one", "org.minidarwin.two"):
            self.assertTrue((self.root / f"private/var/db/receipts/{ident}.bom").exists())

    def test_components_may_not_overlap(self):
        files = {}
        other = [*self.entries[:2], ("./usr/bin/tool", stat.S_IFREG | 0o755, b"other\n"),
                 self.entries[3]]
        for ident, entries in (("org.minidarwin.one", self.entries), ("org.minidarwin.two", other)):
            files.update({f"{ident}.pkg/{name}": data for name, data in
                          component_files(entries, identifier=ident).items()})
        files["Distribution"] = distribution(("org.minidarwin.one", "org.minidarwin.one.pkg"),
                                             ("org.minidarwin.two", "org.minidarwin.two.pkg")).encode()
        self.pkg.write_bytes(xar(files))
        self.install(ok=False)
        self.original_intact()

    def test_symlink_chain_escape(self):
        # Each link is contained on its own; d/l -> d/s/../x is not.
        self.package([("./d", stat.S_IFDIR | 0o755, b""),
                      ("./d/s", stat.S_IFLNK | 0o777, b".."),
                      ("./d/l", stat.S_IFLNK | 0o777, b"s/../x")])
        self.install(ok=False)
        self.original_intact()

    def test_payload_follows_base_symlinks(self):
        (self.root / "private/etc").mkdir(parents=True)
        (self.root / "etc").symlink_to("private/etc")
        self.package([("./etc", stat.S_IFDIR | 0o755, b""),
                      ("./etc/tool.conf", stat.S_IFREG | 0o644, b"conf")])
        self.install()
        self.assertEqual(os.readlink(self.root / "etc"), "private/etc")
        self.assertEqual((self.root / "private/etc/tool.conf").read_bytes(), b"conf")
        inventory = plistlib.loads((self.root / "private/var/db/mdpkg/org.minidarwin.test.inventory.plist").read_bytes())
        placed = {e["path"]: e.get("package-path") for e in inventory["Entries"]}
        self.assertEqual(placed["private/etc/tool.conf"], "etc/tool.conf")

    def test_escaping_base_symlink(self):
        outside = self.base / "outside"
        outside.mkdir()
        (self.root / "etc").symlink_to("../outside")
        self.package([("./etc/tool.conf", stat.S_IFREG | 0o644, b"conf")])
        self.install(ok=False)
        self.assertEqual(list(outside.iterdir()), [])

    def test_lock_is_exclusive(self):
        import fcntl
        self.package()
        with open(str(self.root) + ".mdpkg-lock", "w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            self.install(ok=False)
        self.original_intact()
        self.install()

    def test_source_date_epoch(self):
        self.package()
        self.run_cli("install", "-pkg", str(self.pkg), "-root", str(self.root),
                     env={"SOURCE_DATE_EPOCH": "86400"})
        receipt = plistlib.loads((self.root / "private/var/db/receipts/org.minidarwin.test.plist").read_bytes())
        self.assertEqual(receipt["InstallDate"], datetime.datetime(1970, 1, 2))

    def test_cli(self):
        self.assertIn("mdpkg ", self.run_cli("--version").stdout)
        self.assertIn("usage:", self.run_cli("--help").stdout)
        self.package()
        self.run_cli(ok=False)
        self.run_cli("inspect", "-pkg", str(self.pkg), "-root", str(self.root))
        self.run_cli("inspect", "-pkg", str(self.pkg), "-script-runner", str(self.binary), ok=False)
        self.run_cli("install", "-pkg", str(self.pkg), ok=False)
        self.run_cli("install", "-pkg", str(self.pkg), "-pkg", str(self.pkg), ok=False)

    def test_dtd_rejected(self):
        files = component_files(self.entries)
        files["PackageInfo"] = b'<!DOCTYPE pkg-info [<!ENTITY x "bad">]>' + files["PackageInfo"]
        self.pkg.write_bytes(xar(files))
        self.install(ok=False)
        self.original_intact()

    def test_interrupted_publish_recovers_original(self):
        self.package()
        txn = Path(str(self.root) + ".mdpkg-transaction")
        txn.mkdir(mode=0o700)
        (txn / "target").write_text(str(self.root.resolve()))
        self.root.rename(txn / "original")
        (txn / "new").mkdir()
        (txn / "new/unfinished").write_text("bad")
        self.install()
        self.assertEqual((self.root / "base-file").read_bytes(), b"unchanged")
        self.assertFalse((self.root / "unfinished").exists())
        self.assertFalse(txn.exists())

    def test_interrupted_cleanup_completes_publication(self):
        # Both renames happened; only removing the original was left.
        self.package()
        txn = Path(str(self.root) + ".mdpkg-transaction")
        txn.mkdir(mode=0o700)
        (txn / "target").write_text(str(self.root.resolve()))
        (txn / "original").mkdir()
        (txn / "original/old").write_text("old")
        self.install()
        self.assertFalse(txn.exists())
        self.assertEqual((self.root / "base-file").read_bytes(), b"unchanged")

    @unittest.skipUnless(os.environ.get("MC_PKG"), "MC fixture unavailable")
    def test_actual_mc_metadata(self):
        self.pkg = Path(os.environ["MC_PKG"])
        data = self.pkg.read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(), "5b047f602247de2b9cd9f4ed6f4b95fb9083a18a6435477dacbfac5f36bfa030")
        result = self.run_cli("inspect", "-pkg", str(self.pkg))
        self.assertIn("org.rudix.pkg.mc 4.8.7-0 /: 392 payload entries; script postinstall", result.stdout)
        files = read_xar(self.pkg)
        self.assertEqual(len(read_odc(files["mcinstall.pkg/Payload"])), 392)
        self.install(ok=False)
        self.original_intact()


if __name__ == "__main__":
    unittest.main()
