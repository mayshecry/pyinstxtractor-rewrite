#!/bin/sh
# Encrypted-PYZ regression test.
#
# Builds a synthetic archive containing one plain and one "encrypted" PYZ
# module, then checks that:
#   * the plain module is extracted as a valid .pyc
#   * the encrypted module is preserved byte-for-byte as .pyc.encrypted
#   * package entries land in pkg/__init__.pyc
#
# Usage: ./encrypted.sh [path-to-pyinstxtractor]

set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
bin=${1:-${PYINST_BIN:-$root/pyinstxtractor}}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cd "$tmp"

python3 "$here/mk_encrypted_archive.py" >/dev/null

"$bin" -o out enc_test.bin >/dev/null

fail=0

if [ ! -f out/PYZ.pyz_extracted/plainmod.pyc ]; then
    echo "encrypted: FAIL - plain module missing" >&2
    fail=1
fi
if [ ! -f out/PYZ.pyz_extracted/plainpkg/__init__.pyc ]; then
    echo "encrypted: FAIL - package __init__.pyc missing" >&2
    fail=1
fi
if [ ! -f out/PYZ.pyz_extracted/secretmod.pyc.encrypted ]; then
    echo "encrypted: FAIL - encrypted module was not preserved" >&2
    fail=1
fi

python3 - <<'PY' || fail=1
import sys
with open('out/PYZ.pyz_extracted/secretmod.pyc.encrypted', 'rb') as fh:
    got = fh.read()
with open('enc_expected.bin', 'rb') as fh:
    want = fh.read()
if got != want:
    sys.exit('encrypted: FAIL - ciphertext was not preserved byte-for-byte')
with open('out/PYZ.pyz_extracted/plainmod.pyc', 'rb') as fh:
    fh.read(16)
    body = fh.read()
if b'PLAIN-CODE-OBJECT-BYTES' not in body:
    sys.exit('encrypted: FAIL - plain module payload corrupted')
PY

if [ "$fail" -eq 0 ]; then
    echo "    encrypted PYZ handling OK"
fi
exit "$fail"
