#!/usr/bin/env python3
"""Print the sources a CMake target lists, as a Nix list of paths.

    swift-cmake-sources.py <source-root> <target> <CMakeLists.txt>...

Collects every add_library(<target> ...) and target_sources(<target> ...)
argument naming a .swift or .c file, relative to the CMakeLists.txt that names
it and printed relative to <source-root>, in the order CMake lists them. The
target may be written as a variable (`${module_name}`); every spelling given
with --alias counts as the target too. A source listed twice is kept once, as
CMake keeps it.
"""

import argparse
import os
import re
import sys

CALL = re.compile(r'\b(add_library|target_sources)\s*\(', re.I)
# A quoted argument, or an unquoted one, whose `\ ` escapes a space.
TOKEN = re.compile(r'"([^"]*)"|((?:\\.|[^\s()"\\])+)')
KEYWORDS = {'PRIVATE', 'PUBLIC', 'INTERFACE', 'STATIC', 'SHARED', 'OBJECT'}


def calls(text):
    """(command, [argument]) for each add_library/target_sources call."""
    for m in CALL.finditer(text):
        depth, i = 1, m.end()
        while depth:
            depth += {'(': 1, ')': -1}.get(text[i], 0)
            i += 1
        body = re.sub(r'#[^\n]*', '', text[m.end():i - 1])
        yield m.group(1).lower(), [a or re.sub(r'\\(.)', r'\1', b) for a, b in TOKEN.findall(body)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('root')
    parser.add_argument('target')
    parser.add_argument('cmakelists', nargs='+')
    parser.add_argument('--alias', action='append', default=[])
    args = parser.parse_args()
    names = {args.target, *args.alias}

    out = []
    for cmakelists in args.cmakelists:
        base = os.path.dirname(cmakelists)
        with open(cmakelists) as f:
            for _, arguments in calls(f.read()):
                if not arguments or arguments[0] not in names:
                    continue
                for a in arguments[1:]:
                    if a in KEYWORDS or not a.endswith(('.swift', '.c')):
                        continue
                    path = os.path.normpath(os.path.join(base, a))
                    if not os.path.isfile(path):
                        sys.exit(f'{cmakelists}: {a} does not exist')
                    rel = os.path.relpath(path, args.root)
                    if rel not in out:
                        out.append(rel)
    print('[')
    for p in out:
        print(f'    "{p}"')
    print('  ]')


if __name__ == '__main__':
    main()
