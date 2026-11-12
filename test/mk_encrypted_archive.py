"""Build a synthetic .exe whose PYZ contains one plain and one 'encrypted'
module, to exercise the extractor's .encrypted fallback path."""

import marshal
import struct
import zlib

MAGIC = b'MEI\014\013\012\013\016'
COOKIE_SIZE = 88
PYC_MAGIC = b'\x6f\x0d\x0d\x0a'

plain_payload = zlib.compress(b'PLAIN-CODE-OBJECT-BYTES')
enc_payload = b'\xde\xad\xbe\xef' + bytes(range(32))   # not a zlib stream

# PYZ TOC positions are absolute file offsets (header is 12 bytes), which is
# what real PyInstaller archives use and what the extractor seeks with.
HEADER_LEN = 12
pos = HEADER_LEN
entries = []
data = b''
for name, payload, ispkg in [('plainmod', plain_payload, 0),
                             ('secretmod', enc_payload, 0),
                             ('plainpkg', plain_payload, 1)]:
    entries.append((name, (ispkg, pos, len(payload))))
    data += payload
    pos += len(payload)

toc_bytes = marshal.dumps(entries)
toc_pos = HEADER_LEN + len(data)

pyz = b'PYZ\0' + PYC_MAGIC + struct.pack('!i', toc_pos) + data + toc_bytes

# wrap the PYZ inside a CArchive
name = b'PYZ.pyz\0'
comp = zlib.compress(pyz)
entry_payload = struct.pack('!IIIBc', 0, len(comp), len(pyz), 1, b'z') + name
toc = struct.pack('!i', len(entry_payload) + 4) + entry_payload
blob = comp
overlay = blob + toc
cookie = struct.pack('!8sIIii64s', MAGIC, len(overlay) + COOKIE_SIZE,
                     len(blob), len(toc), 313, b'python3.13\0'.ljust(64, b'\0'))

with open('enc_test.bin', 'wb') as fh:
    fh.write(b'MZ' + b'\0' * 100 + overlay + cookie)

with open('enc_expected.bin', 'wb') as fh:
    fh.write(enc_payload)

print('built enc_test.bin (pyz %d bytes, %d entries)' % (len(pyz), len(entries)))
