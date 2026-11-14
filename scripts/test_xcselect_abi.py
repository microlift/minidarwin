#!/usr/bin/env python3
"""Differential test of libxcselect against macOS's own, run by hand on macOS.

Builds pkgs/xcselect/tests/abi-probe.c, runs it against the host's
/usr/lib/libxcselect.dylib and against the library named by --library (the
host test build, `nix build .#xcselectBootstrap`), with the same environment
and arguments, and compares everything they print.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent.parent / "pkgs/xcselect/tests"
PATHS = ["/Applications/Xcode.app", "/Applications/Xcode.app/", "/Applications",
         "/Applications/Xcode.app/Contents/Developer", "/", "relative/path",
         "/nonexistent", "/Library/Developer/CommandLineTools", "com.apple.dt.Xcode",
         "com.apple.FileMerge", "com.apple.dt.Xcodex", "com.apple."]
ENVIRONMENTS = [{}, {"DEVELOPER_DIR": "/Applications/Xcode.app"},
                {"DEVELOPER_DIR": "/Applications/Xcode.app/Contents/Developer/"},
                {"DEVELOPER_DIR": "/nonexistent"}, {"DEVELOPER_DIR": "/"},
                {"DEVELOPER_DIR": ""}]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        probe = Path(tmp) / "probe"
        subprocess.run(["cc", "-o", probe, HERE / "abi-probe.c"], check=True)
        failures = 0
        for extra in ENVIRONMENTS:
            env = {k: v for k, v in os.environ.items()
                   if k not in ("DEVELOPER_DIR", "XCSELECT_TEST_ROOT")} | extra
            outputs = [subprocess.run([probe, lib, *PATHS], env=env, cwd="/",
                                      capture_output=True, text=True, check=True).stdout
                       for lib in ("/usr/lib/libxcselect.dylib", args.library)]
            if outputs[0] != outputs[1]:
                failures += 1
                print(f"DIFFERENT with {extra}:")
                for a, b in zip(outputs[0].splitlines(), outputs[1].splitlines()):
                    if a != b:
                        print(f"  macOS: {a}\n  ours:  {b}")
            else:
                print(f"same with {extra}")
        sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
