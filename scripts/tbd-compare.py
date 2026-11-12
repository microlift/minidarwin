#!/usr/bin/env python3
"""Check that a .tbd stub describes its dylib: `llvm-readtapi -compare`, less
the two attributes a TBD v4 stub cannot carry.

usage: tbd-compare.py LLVM_READTAPI DYLIB TBD

`llvm-readtapi -compare` reports every difference between the two inputs. For
a v4 stub made by `llvm-readtapi -stubify`, two kinds are expected and
harmless to a linker, so they are allowed, and only for a symbol whose name
both sides export:

  * Text vs Data. TBD v4 has no text section, so every export reads back as
    Data. (A dylib reader marks some exports Text, e.g. in libstdc++.)
  * Weak-Referenced on an export. No TBD version records it (it describes an
    import), so a weak-defined, weak-referenced reexport loses it.

Anything else -- a symbol on one side only, or another attribute, or any
difference outside the symbol list (install name, versions, reexports, flags,
targets, ...) -- fails, with llvm-readtapi's report.
"""

import re
import subprocess
import sys

SYMBOL = re.compile(r"^\t\t([<>]) (\S+) - (.*?)\s*$")


def normal(kind):
    words = [w for w in kind.split() if w not in ("Text", "Data", "Weak-Referenced")]
    return " ".join(words)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    readtapi, dylib, tbd = sys.argv[1:]
    run = subprocess.run([readtapi, "-compare", dylib, tbd],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    report = run.stdout
    if run.returncode == 0 and not report.strip():
        return 0
    # Exit status 1 is "they differ"; anything else is llvm-readtapi failing.
    if run.returncode not in (0, 1):
        sys.exit(f"tbd-compare: llvm-readtapi failed ({run.returncode}):\n{report}")

    lines = report.splitlines()
    # Header: the two input names, then a blank line.
    body = lines[3:] if len(lines) >= 3 and lines[0].startswith("< ") and lines[1].startswith("> ") else None
    if body is None:
        sys.exit(f"tbd-compare: unexpected report:\n{report}")

    left, right, other = {}, {}, []
    section = None
    for line in body:
        if not line.strip():
            continue
        if not line.startswith("\t"):
            section = line
            continue
        m = SYMBOL.match(line)
        if section == "Symbols" and m:
            (left if m.group(1) == "<" else right)[m.group(2)] = m.group(3)
        elif section == "Symbols" and line.startswith("\t") and not line.startswith("\t\t"):
            continue  # the target heading, e.g. "\tx86_64-apple-macos26"
        else:
            other.append(f"{section}: {line.strip()}")

    if set(left) != set(right):
        other += [f"only in the dylib: {s}" for s in sorted(set(left) - set(right))]
        other += [f"only in the stub: {s}" for s in sorted(set(right) - set(left))]
    for name in sorted(set(left) & set(right)):
        if normal(left[name]) != normal(right[name]):
            other.append(f"{name}: '{left[name]}' in the dylib, '{right[name]}' in the stub")

    if other:
        print("\n".join(other[:50]), file=sys.stderr)
        sys.exit(f"tbd-compare: {tbd} does not describe {dylib} ({len(other)} differences)")
    print(f"tbd-compare: {len(left)} exports differ only in what TBD v4 cannot record")
    return 0


if __name__ == "__main__":
    sys.exit(main())
