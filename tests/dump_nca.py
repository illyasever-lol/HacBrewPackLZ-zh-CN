#!/usr/bin/env python3
"""Dump decrypted NCA header fields. usage: dump_nca.py keys file.nca ... (prints no key material)"""
import struct, sys
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

keys = {}
for line in open(sys.argv[1], encoding='ascii', errors='ignore'):
    if '=' in line:
        n, v = line.split('=', 1)
        keys[n.strip().lower()] = v.strip()
hk = bytes.fromhex(keys['header_key'])

def xts(data):
    out = bytearray()
    for i in range(0, len(data), 0x200):
        d = Cipher(algorithms.AES(hk), modes.XTS((i // 0x200).to_bytes(16, 'big'))).decryptor()
        out += d.update(data[i:i + 0x200]) + d.finalize()
    return bytes(out)

for path in sys.argv[2:]:
    h = xts(open(path, 'rb').read(0xC00))
    print('==', path)
    print(' sig1 nonzero', any(h[0:0x100]), ' sig2 nonzero', any(h[0x100:0x200]))
    print(' magic', h[0x200:0x204], 'dist', h[0x204], 'type', h[0x205], 'kg_old', h[0x206], 'kaek_idx', h[0x207])
    print(' size 0x%x tid %016x ctidx %d sdk %08x kg %d sigkg %d' % (*struct.unpack_from('<QQII', h, 0x208), h[0x220], h[0x221]))
    print(' reserved222', h[0x222:0x230].hex(), 'rights', h[0x230:0x240].hex())
    for i in range(4):
        st, en, a, b = struct.unpack_from('<IIII', h, 0x240 + i * 0x10)
        if st == 0 and en == 0:
            continue
        fh = h[0x400 + i * 0x200:0x600 + i * 0x200]
        ver, ft, ht, et, mt = struct.unpack_from('<HBBBB', fh, 0)
        print(f' sec{i}: media {st:#x}..{en:#x} ({a:#x},{b:#x}) file 0x{st*0x200:x}..0x{en*0x200:x} ver={ver} fs={ft} hash={ht} enc={et} meta={mt}')
        print('   upper_iv', fh[0x140:0x148].hex())
        if ht == 2:
            bs, lc = struct.unpack_from('<II', fh, 0x28)
            ho, hs, po, ps = struct.unpack_from('<QQQQ', fh, 0x30)
            print(f'   sha256: block=0x{bs:x} layers={lc} hash@0x{ho:x}+0x{hs:x} pfs0@0x{po:x}+0x{ps:x}')
            print('   tail nonzero', any(fh[0x50:0x100]))
        elif ht == 3:
            magic, ver2, msz, nl = struct.unpack_from('<4sIII', fh, 0x8)
            print(f'   ivfc {magic} ver=0x{ver2:x} master_size=0x{msz:x} levels={nl}')
            for l in range(6):
                o, s, bo, r = struct.unpack_from('<QQII', fh, 0x18 + l * 0x18)
                print(f'     L{l}: ofs=0x{o:x} size=0x{s:x} block_order={bo} res={r:#x}')
            print('   salt nonzero', any(fh[0xA8:0xC8]), 'master', fh[0xC8:0xE8].hex()[:16], 'tail nonzero', any(fh[0xE8:0x100]))
        print('   compress', fh[0x178:0x1A0].hex())
