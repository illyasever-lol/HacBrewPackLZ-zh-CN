#!/usr/bin/env python3
"""Create a deterministic test title: exefs/ romfs/ logo/ control/. usage: make_testset.py outdir [romfs_mb]"""
import os, random, subprocess, sys, json, struct

out = sys.argv[1]
mb = int(sys.argv[2]) if len(sys.argv) > 2 else 40
rnd = random.Random(0x5A17)

def w(path, data):
    p = os.path.join(out, path)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'wb') as f:
        f.write(data)

TID = '0100ABCD12340000'
npdm_json = {
    "name": "lztest", "title_id": "0x" + TID, "title_id_range_min": "0x" + TID, "title_id_range_max": "0x" + TID,
    "main_thread_stack_size": "0x100000", "main_thread_priority": 44, "default_cpu_id": 0, "process_category": 0,
    "is_retail": True, "pool_partition": 0, "is_64_bit": True, "address_space_type": 1,
    "filesystem_access": {"permissions": "0xffffffffffffffff"},
    "service_access": ["*"], "service_host": ["*"],
    "kernel_capabilities": [
        {"type": "kernel_flags", "value": {"highest_thread_priority": 59, "lowest_thread_priority": 28,
                                           "lowest_cpu_id": 0, "highest_cpu_id": 2}},
        {"type": "syscalls", "value": {"svcUnknown": "0x00"}},
        {"type": "min_kernel_version", "value": "0x0030"},
    ],
}
os.makedirs(os.path.join(out, 'exefs'), exist_ok=True)
jp = os.path.join(out, 'npdm.json')
json.dump(npdm_json, open(jp, 'w'))
subprocess.check_call([r'C:\devkitPro\tools\bin\npdmtool.exe', jp, os.path.join(out, 'exefs', 'main.npdm')],
                      stdout=subprocess.DEVNULL)
w('exefs/main', b'NSO0' + rnd.randbytes(300_000))
w('exefs/rtld', b'NSO0' + rnd.randbytes(70_000))

os.makedirs(os.path.join(out, 'control'), exist_ok=True)
subprocess.check_call([r'C:\devkitPro\tools\bin\nacptool.exe', '--create', 'LZ Test', 'hacbrewpacklz', '1.0.0',
                       os.path.join(out, 'control', 'control.nacp')], stdout=subprocess.DEVNULL)
w('control/icon_AmericanEnglish.dat', rnd.randbytes(30_000))
w('logo/NintendoLogo.png', rnd.randbytes(5000))
w('logo/StartupMovie.gif', rnd.randbytes(9000))

WORDS = [rnd.randbytes(rnd.randint(4, 40)) for _ in range(400)]
TEXT = [('word%04d ' % i).encode() for i in range(2000)]

def compressible(n):
    b = bytearray()
    while len(b) < n:
        b += rnd.choice(WORDS)
    return bytes(b[:n])

def texty(n):
    b = bytearray()
    while len(b) < n:
        b += rnd.choice(TEXT)
    return bytes(b[:n])

# Names chosen to pin down sort order (separator vs digit/space/dash/upper, nested dirs, empty dirs).
w('romfs/a.bin', compressible(1000))
w('romfs/a b/x.txt', texty(5000))
w('romfs/a-b/x.txt', texty(5000))
w('romfs/a/x.txt', texty(70_000))
w('romfs/a/z/deep/deeper/file.dat', compressible(123_456))
w('romfs/a0/x.txt', b'')
w('romfs/A1/UPPER.TXT', texty(777))
w('romfs/b/zero.bin', b'\0' * 300_000)
w('romfs/b/short', b'x')
w('romfs/b/exact_block', compressible(0x10000))
w('romfs/b/exact_ivfc', compressible(0x4000))
os.makedirs(os.path.join(out, 'romfs', 'emptydir', 'emptychild'), exist_ok=True)
w('romfs/data/incompressible.bin', rnd.randbytes(3_000_000))
w('romfs/data/compressible.bin', compressible(4_000_000))
w('romfs/data/text.bin', texty(4_000_000))
w('romfs/data/mixed.bin', b''.join((rnd.randbytes(200_000) if i % 3 == 0 else compressible(200_000)) for i in range(15)))
remain = mb * 1000 * 1000 - 14_500_000
i = 0
while remain > 0:
    sz = min(remain, rnd.randint(50_000, 3_000_000))
    kind = i % 3
    data = rnd.randbytes(sz) if kind == 0 else (compressible(sz) if kind == 1 else texty(sz))
    w(f'romfs/many/{i % 7}/f{i:04d}.bin', data)
    remain -= sz
    i += 1
print('test set ready:', out)
