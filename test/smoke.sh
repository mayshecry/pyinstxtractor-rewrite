#!/bin/sh
# Smoke test for pyinstxtractor.
#
# Usage: ./smoke.sh [path-to-pyinstaller-executable]
#
# If no executable is given, a small one is built with PyInstaller (requires
# `pyinstaller` on PATH). The test extracts it and verifies that every emitted
# .pyc file is a valid marshal stream.

set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
bin="${PYINST_BIN:-$root/pyinstxtractor}"

exe="$1"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

if [ -z "$exe" ]; then
    command -v pyinstaller >/dev/null 2>&1 || {
        echo "smoke: pyinstaller not found; pass an executable path instead" >&2
        exit 77
    }
    echo "==> building test executable with PyInstaller"
    ( cd "$tmp" && cp "$here/hello.py" . && \
      pyinstaller --onefile --name hello --distpath dist --workpath build \
                  --specpath . hello.py >build.log 2>&1 ) || {
        echo "smoke: PyInstaller build failed (see $tmp/build.log)" >&2
        exit 1
    }
    exe="$tmp/dist/hello"
fi

echo "==> extracting $exe"
"$bin" -o "$tmp/out" "$exe"

echo "==> verifying output"
test -d "$tmp/out" || { echo "smoke: no output directory" >&2; exit 1; }
test -f "$tmp/out/PYZ.pyz" || echo "smoke: note: archive has no PYZ.pyz"

python3 - "$tmp/out" <<'PY'
import marshal, os, sys
root = sys.argv[1]
ok = bad = 0
bad_files = []
for dirpath, _dirs, files in os.walk(root):
    for name in files:
        if not name.endswith('.pyc'):
            continue
        path = os.path.join(dirpath, name)
        with open(path, 'rb') as fh:
            fh.read(16)          # 4-byte magic + 12-byte PEP 552 header
            try:
                marshal.loads(fh.read())
                ok += 1
            except Exception as exc:
                bad += 1
                bad_files.append((path, exc))
print("    valid pyc: %d   invalid: %d" % (ok, bad))
for path, exc in bad_files[:10]:
    print("    BAD %s: %s" % (path, exc))
if ok == 0:
    sys.exit("smoke: no .pyc files were produced")
sys.exit(1 if bad else 0)
PY

echo "==> OK"
