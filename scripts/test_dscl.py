#!/usr/bin/env python3
"""Tests for dscl and dseditgroup, run against the host build.

DSCL and DSEDITGROUP name the binaries. Every test points DSCL_TEST_NODE at a
temporary node directory; the system's own account database is only read."""

import os
import plistlib
import subprocess
import tempfile
import unittest
from pathlib import Path


class DirectoryTools(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.node = Path(self.tmp.name) / "Default"
        self.env = {**os.environ, "DSCL_TEST_NODE": str(self.node)}

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, tool, *args, ok=True):
        result = subprocess.run([os.environ[tool], *args], capture_output=True,
                                text=True, env=self.env, timeout=15)
        self.assertEqual(result.returncode == 0, ok, result.stdout + result.stderr)
        return result

    def dscl(self, *args, ok=True):
        return self.run_tool("DSCL", *args, ok=ok)

    def dseditgroup(self, *args, ok=True):
        return self.run_tool("DSEDITGROUP", *args, ok=ok)

    def record(self, kind, name):
        return plistlib.loads((self.node / kind / f"{name}.plist").read_bytes())

    def test_a_user_is_built_attribute_by_attribute(self):
        self.dscl(".", "-create", "/Users/macports", "UniqueID", "502")
        self.dscl(".", "-create", "/Users/macports", "RealName", "MacPorts")
        self.dscl(".", "-create", "/Users/macports", "Password", "*")
        self.dscl(".", "-create", "/Users/macports", "NFSHomeDirectory", "/opt/local/var/macports/home")
        self.dscl(".", "-create", "/Users/macports", "UserShell", "/usr/bin/false")
        record = self.record("users", "macports")
        self.assertEqual(record["name"], ["macports"])
        self.assertEqual(record["uid"], ["502"])
        self.assertEqual(record["home"], ["/opt/local/var/macports/home"])
        self.assertEqual(len(record["generateduid"][0]), 36)
        out = self.dscl("-q", ".", "-read", "/Users/macports", "NFSHomeDirectory").stdout
        self.assertEqual(out, "NFSHomeDirectory: /opt/local/var/macports/home\n")
        out = self.dscl(".", "-read", "/Users/macports", "UserShell").stdout
        self.assertEqual(out.split()[1], "/usr/bin/false")

    def test_a_value_with_a_space_is_printed_on_its_own_line(self):
        self.dscl(".", "-create", "/Users/someone", "RealName", "Some One")
        out = self.dscl(".", "-read", "/Users/someone", "RealName").stdout
        self.assertEqual(out, "RealName:\n Some One\n")

    def test_missing_records_and_keys_fail(self):
        result = self.dscl(".", "-read", "/Groups/nonexistent-group", ok=False)
        self.assertIn("eDSRecordNotFound", result.stderr)
        self.dscl(".", "-list", "/Users/nonexistent-user", ok=False)
        self.dscl(".", "-create", "/Users/someone", "UniqueID", "600")
        result = self.dscl(".", "-read", "/Users/someone", "UserShell", ok=False)
        self.assertIn("No such key: UserShell", result.stderr)

    def test_search_finds_the_nodes_records_and_the_systems(self):
        self.dscl(".", "-create", "/Users/someone", "UniqueID", "4242")
        out = self.dscl("/Search", "-search", "/Users", "UniqueID", "4242").stdout
        self.assertEqual(out, "someone\t\tUniqueID = (\n    4242\n)\n")
        out = self.dscl("/Search", "-search", "/Users", "UniqueID", "0").stdout
        self.assertTrue(out.startswith("root\t\t"), out)
        self.assertEqual(self.dscl("/Search", "-search", "/Users", "UniqueID", "4243").stdout, "")

    def test_deleting_attributes_and_records(self):
        self.dscl(".", "-create", "/Users/someone", "UniqueID", "600")
        self.dscl(".", "-delete", "/Users/someone", "AuthenticationAuthority")
        self.dscl(".", "-delete", "/Users/someone", "UniqueID")
        self.assertNotIn("uid", self.record("users", "someone"))
        self.dscl(".", "-delete", "/Users/someone")
        self.assertFalse((self.node / "users/someone.plist").exists())
        self.dscl(".", "-delete", "/Users/root", ok=False)

    def test_native_attributes_keep_their_own_names(self):
        self.dscl(".", "-create", "/Users/someone", "dsAttrTypeNative:KerberosKeys", "k")
        self.assertEqual(self.record("users", "someone")["KerberosKeys"], ["k"])
        out = self.dscl(".", "-read", "/Users/someone").stdout
        self.assertTrue(out.startswith("dsAttrTypeNative:KerberosKeys: k\n"), out)

    def test_changing_a_system_record_copies_it_into_the_node(self):
        self.dscl(".", "-create", "/Users/root", "Comment", "changed")
        record = self.record("users", "root")
        self.assertEqual((record["uid"], record["comment"]), (["0"], ["changed"]))

    def test_a_record_name_cannot_leave_the_node(self):
        self.dscl(".", "-create", "/Users/../escape", "UniqueID", "1", ok=False)
        self.assertFalse((Path(self.tmp.name) / "escape.plist").exists())

    def test_dseditgroup_creates_a_group_with_a_free_id(self):
        self.dseditgroup("-q", "-o", "create", "macports")
        group = self.record("groups", "macports")
        gid = int(group["gid"][0])
        self.assertGreaterEqual(gid, 501)
        out = self.dscl("-q", ".", "-read", "/Groups/macports", "PrimaryGroupID").stdout
        self.assertEqual(out, f"PrimaryGroupID: {gid}\n")
        self.dseditgroup("-o", "create", "macports", ok=False)
        self.dseditgroup("-o", "create", "-i", str(gid), "other", ok=False)

    def test_dseditgroup_edits_and_deletes_membership(self):
        self.dseditgroup("-o", "create", "-i", "4300", "-r", "Some Group", "some")
        self.dseditgroup("-o", "edit", "-a", "alice", "-t", "user", "some")
        self.dseditgroup("-o", "edit", "-a", "alice", "-t", "user", "some")
        self.dseditgroup("-o", "edit", "-a", "bob", "-t", "user", "some")
        self.dseditgroup("-o", "edit", "-d", "alice", "-t", "user", "some")
        group = self.record("groups", "some")
        self.assertEqual((group["users"], group["realname"]), (["bob"], ["Some Group"]))
        self.dseditgroup("-o", "delete", "some")
        self.assertFalse((self.node / "groups/some.plist").exists())

    def test_writing_needs_root(self):
        if os.geteuid() == 0:
            self.skipTest("running as root")
        env = dict(self.env)
        del env["DSCL_TEST_NODE"]
        result = subprocess.run([os.environ["DSCL"], ".", "-create", "/Users/x", "UniqueID", "1"],
                                capture_output=True, text=True, env=env, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("eDSPermissionError", result.stderr)


if __name__ == "__main__":
    unittest.main()
