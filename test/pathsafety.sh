#!/bin/sh
# Path-safety regression test.
#
# Builds a synthetic CArchive whose member names are all classic traversal
# attempts, extracts it, and asserts that nothing was written outside the
# output directory.
#
# Usage: ./pathsafety.sh [path-to-pyinstxtractor]

set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
bin=${1:-${PYINST_BIN:-$root/pyinstxtractor}}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cd "$tmp"

python3 "$here/mk_evil_archive.py" evil.bin >/dev/null

# Sentinel files: if any of these appear, a traversal succeeded.
sentinel_a="$tmp/escape_a"
sentinel_b="$(dirname "$tmp")/escape_b"

"$bin" -o out evil.bin >/dev/null

fail=0
if [ -e "$sentinel_a" ] || [ -e "$sentinel_b" ]; then
    echo "pathsafety: FAIL - archive escaped the output directory" >&2
    fail=1
fi

# Every extracted file must live under out/.
outside=$(find . -type f ! -name evil.bin ! -path './out/*' | head -5)
if [ -n "$outside" ]; then
    echo "pathsafety: FAIL - files written outside out/:" >&2
    echo "$outside" >&2
    fail=1
fi

count=$(find out -type f | wc -l)
if [ "$count" -eq 0 ]; then
    echo "pathsafety: FAIL - nothing was extracted" >&2
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "    path safety OK ($count files, all contained)"
fi
exit "$fail"
