#!/bin/sh
# der-to-header.sh CERT.der NAME: CERT as a C array named NAME.
set -eu
printf '/* Generated from %s. */\nstatic const unsigned char %s[] = {\n' "$(basename "$1")" "$2"
od -An -v -tx1 "$1" | sed -e 's/ *\([0-9a-f][0-9a-f]\)/0x\1, /g' -e 's/, $/,/'
printf '};\n'
