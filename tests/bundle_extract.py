#!/usr/bin/env python3
"""List / extract files from a .NET single-file bundle. usage: bundle_extract.py exe outdir [substr ...]"""
import struct, sys, zlib, os, mmap

SIG = bytes([0x8b, 0x12, 0x02, 0xb9, 0x6a, 0x61, 0x20, 0x38, 0x72, 0x7b, 0x93, 0x02, 0x14, 0xd7, 0xa0, 0x32,
             0x13, 0xf5, 0xb9, 0xe6, 0xef, 0xae, 0x33, 0x18, 0xee, 0x3b, 0x2d, 0xce, 0x24, 0xb3, 0x6a, 0xae])

exe, out = sys.argv[1], sys.argv[2]
wanted = sys.argv[3:]
f = open(exe, 'rb')
m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
pos = m.find(SIG)
if pos < 0:
    sys.exit('no bundle signature')
hdr = struct.unpack_from('<q', m, pos - 8)[0]
print('bundle header at', hex(hdr))

def rd_str(p):
    n = 0; shift = 0
    while True:
        b = m[p]; p += 1
        n |= (b & 0x7f) << shift
        if not b & 0x80:
            break
        shift += 7
    return m[p:p + n].decode('utf8'), p + n

major, minor, count = struct.unpack_from('<IIi', m, hdr)
p = hdr + 12
bid, p = rd_str(p)
print('version', major, minor, 'files', count, 'id', bid)
if major >= 2:
    p += 8 * 4 + 8  # depsjson ofs/size, runtimeconfig ofs/size, flags
for i in range(count):
    off, size = struct.unpack_from('<qq', m, p); p += 16
    csize = 0
    if major >= 6:
        csize = struct.unpack_from('<q', m, p)[0]; p += 8
    typ = m[p]; p += 1
    name, p = rd_str(p)
    if any(w.lower() in name.lower() for w in wanted):
        data = m[off:off + (csize if csize else size)]
        if csize:
            data = zlib.decompress(data, -15)
        os.makedirs(out, exist_ok=True)
        dest = os.path.join(out, os.path.basename(name))
        with open(dest, 'wb') as o:
            o.write(data)
        print('extracted', name, len(data))
