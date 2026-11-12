"""Build a synthetic CArchive full of hostile member names.

Used by pathsafety.sh. The archive is structurally valid (real cookie, real
zlib-compressed members, real TOC) so it exercises the extractor's real code
path rather than an error branch.
"""

import struct
import sys
import zlib

MAGIC = b'MEI\014\013\012\013\016'
COOKIE_SIZE = 24 + 64

# Every one of these must end up confined to the output directory.
HOSTILE_NAMES = [
    "../escape_a",
    "../../escape_b",
    "....//escape_c",
    "/abs/escape_d",
    "C:\\windows\\escape_e",
    "\\..\\escape_f",
    "a/./b/./c.txt",
    "a//b///c.txt",
    "deep/" + "../" * 10 + "lvl.txt",
    "-",
    "..",
    ".",
    "name with spaces.txt",
    "normal.txt",
]


def build(path):
    blob = b''
    toc = b''
    pos = 0

    for name in HOSTILE_NAMES:
        data = b'DATA:' + name.encode('utf-8', 'surrogateescape')
        body = zlib.compress(data)
        payload = struct.pack('!IIIBc', pos, len(body), len(data), 1, b'b')
        payload += name.encode('utf-8', 'surrogateescape') + b'\0'
        toc += struct.pack('!i', len(payload) + 4) + payload
        blob += body
        pos += len(body)

    overlay = blob + toc
    cookie = struct.pack('!8sIIii64s', MAGIC, len(overlay) + COOKIE_SIZE,
                         len(blob), len(toc), 313,
                         b'python3.13\0'.ljust(64, b'\0'))

    with open(path, 'wb') as fh:
        fh.write(b'MZ' + b'\0' * 100 + overlay + cookie)


if __name__ == '__main__':
    build(sys.argv[1] if len(sys.argv) > 1 else 'evil.bin')
