#!/usr/bin/env python3
"""Writes the self-test RomFS. The content recipe must match wordAt() in source/main.c. usage: make_romfs.py outdir"""
import os
import struct
import sys

MASK = (1 << 64) - 1
PATTERNS = [
    0x0A2F3A7365636E65, 0x6D61726620676E69, 0x2072657475706D6F, 0x7465787475726520,
    0x68736568205F7870, 0x0000000000000000, 0x0101010101010101, 0x3F800000BF800000,
    0x00FF00FF00FF00FF, 0x2020202020202020, 0x4D4F44454C5F4C4F, 0x4144303030313233,
    0xFFFFFFFFFFFFFFFF, 0x1122334455667788, 0x3C2F6E6F64653E0A, 0x72656E64657220AB,
]


def splitmix64(x):
    x = (x + 0x9E3779B97F4A7C15) & MASK
    x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & MASK
    x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & MASK
    return x ^ (x >> 31)


def word_at(kind, seed, index):
    if kind == 0:
        return splitmix64(seed + index)
    if kind == 1:
        return PATTERNS[splitmix64(seed + (index >> 2)) & 15]
    if kind == 2:
        return 0
    if kind == 3:
        return splitmix64(seed + index) if (index & 7) < 4 else PATTERNS[(index >> 3) & 15]
    return word_at((index >> 14) % 4, seed, index)


def write_file(root, relative, kind, seed, size):
    path = os.path.join(root, relative)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        written = 0
        index = 0
        while written < size:
            n = min(0x4000, (size - written + 7) // 8)
            chunk = struct.pack('<%dQ' % n, *(word_at(kind, seed, index + i) for i in range(n)))
            take = min(len(chunk), size - written)
            f.write(chunk[:take])
            written += take
            index += n


def name(kind, seed, label):
    return f'k{kind}_s{seed}_{label}.bin'


def main():
    root = sys.argv[1]
    MiB = 1 << 20
    write_file(root, name(1, 9001, 'stdio'), 1, 9001, 0x30000)
    big = [(0, 24 * MiB + 12345, 'random'), (1, 40 * MiB + 7, 'compressible'), (2, 8 * MiB, 'zeros'),
           (3, 24 * MiB + 1, 'half'), (4, 32 * MiB + 3, 'mixed')]
    for i, (kind, size, label) in enumerate(big):
        write_file(root, 'data/' + name(kind, 100 + i, label), kind, 100 + i, size)

    # Sizes around every boundary in the format: LZ4 minimums, 0x10 alignment, IVFC and compression blocks.
    edges = [1, 12, 13, 15, 16, 17, 255, 256, 0x3FFF, 0x4000, 0x4001, 0xFFFF, 0x10000, 0x10001, 0x1FFFF, 0x20000]
    seed = 200
    for size in edges:
        for kind in (0, 1, 3):
            write_file(root, f'edge/{size:x}/' + name(kind, seed, 'edge'), kind, seed, size)
            seed += 1
    write_file(root, 'edge/' + name(0, seed, 'empty'), 0, seed, 0)
    os.makedirs(os.path.join(root, 'edge', 'emptydir', 'child'), exist_ok=True)

    # Many small files, which pack several file starts and ends into each compression block.
    for i in range(400):
        kind = i % 5
        size = 300 + (i * 7919) % 40000
        write_file(root, f'many/{i % 16:02d}/' + name(kind, 1000 + i, 'small'), kind, 1000 + i, size)


if __name__ == '__main__':
    main()
