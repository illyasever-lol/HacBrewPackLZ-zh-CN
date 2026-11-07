#!/usr/bin/env python3
"""
Writes <name>.zst and <name>.raw pairs for zstd_check, made by the real Zstandard library, the one nsz itself uses.
usage: make_zstd_corpus.py <output folder> [folder of real files to include]...
"""
import hashlib
import os
import random
import struct
import sys

import zstandard


def noise(size, seed):
    out = bytearray()
    counter = 0
    while len(out) < size:
        out += hashlib.shake_128(f'{seed}:{counter}'.encode()).digest(1 << 20)
        counter += 1
    return bytes(out[:size])


def text_like(size, seed):
    rng = random.Random(seed)
    words = [bytes(rng.choices(range(97, 123), k=rng.randint(2, 12))) for _ in range(4000)]
    out = bytearray()
    while len(out) < size:
        out += rng.choice(words) + b' '
    return bytes(out[:size])


def structured(size, seed):
    """Records with small numbers, repeating fields and runs: what game assets and executables look like to a compressor."""
    rng = random.Random(seed)
    out = bytearray()
    while len(out) < size:
        kind = rng.randint(0, 5)
        if kind == 0:
            out += struct.pack('<IIff', rng.randint(0, 255), len(out), rng.random(), 1.0)
        elif kind == 1:
            out += bytes([rng.randint(0, 3)]) * rng.randint(1, 4000)
        elif kind == 2 and len(out) > 100:
            start = rng.randint(0, len(out) - 1)
            out += out[start:start + rng.randint(3, 3000)]
        elif kind == 3:
            out += noise(rng.randint(1, 300), rng.random())
        elif kind == 4 and len(out) > 8:
            distance = rng.randint(1, 15)
            for _ in range(rng.randint(4, 200)):
                out.append(out[-distance])
        else:
            out += bytes(rng.getrandbits(8) & 0x1F for _ in range(rng.randint(1, 200)))
    return bytes(out[:size])


def main():
    folder = sys.argv[1]
    os.makedirs(folder, exist_ok=True)

    def emit(name, raw, compressed):
        with open(os.path.join(folder, name + '.raw'), 'wb') as f:
            f.write(raw)
        with open(os.path.join(folder, name + '.zst'), 'wb') as f:
            f.write(compressed)

    sources = {
        'empty': b'',
        'one': b'x',
        'tiny': b'hello zstandard',
        'zeros': bytes(3 << 20),
        'ones': b'\xff' * (1 << 20),
        'noise': noise(1 << 20, 'a'),
        'text': text_like(4 << 20, 1),
        'mixed': structured(6 << 20, 2),
        'small_mixed': structured(70000, 3),
        'noise_then_zeros': noise(300000, 'b') + bytes(300000) + noise(300000, 'c'),
    }
    for extra in sys.argv[2:]:
        for root, _, names in os.walk(extra):
            for name in sorted(names):
                path = os.path.join(root, name)
                if os.path.getsize(path) <= 32 << 20:
                    with open(path, 'rb') as f:
                        sources['real_' + hashlib.sha1(path.encode()).hexdigest()[:10]] = f.read()

    levels = [-5, 1, 3, 9, 12, 18, 19, 22]
    for name, raw in sources.items():
        for level in levels:
            compressor = zstandard.ZstdCompressor(level=level)
            emit(f'{name}_l{level}', raw, compressor.compress(raw))

        # The ways a solid NSZ is written: a stream of unknown size, with threads, a checksum, or long distance matching.
        with_checksum = zstandard.ZstdCompressor(level=3, write_checksum=True, write_content_size=False)
        emit(f'{name}_checksum', raw, with_checksum.compress(raw))
        threaded = zstandard.ZstdCompressor(level=5, threads=2)
        chunks = threaded.compressobj()
        emit(f'{name}_threads', raw, chunks.compress(raw) + chunks.flush())
        long_mode = zstandard.ZstdCompressor(compression_params=zstandard.ZstdCompressionParameters.from_level(9, window_log=27, enable_ldm=True))
        chunks = long_mode.compressobj()
        emit(f'{name}_long', raw, chunks.compress(raw) + chunks.flush())

    # Several frames in a row, with skippable frames between them, and many small blocks flushed in one frame.
    a, b = sources['text'][:300000], sources['mixed'][:300000]
    plain = zstandard.ZstdCompressor(level=3)
    skip = struct.pack('<II', 0x184D2A53, 5) + b'skip!'
    emit('frames', a + b + a, plain.compress(a) + skip + plain.compress(b) + plain.compress(a))
    emit('skippable_only', b'', skip)

    flushed = zstandard.ZstdCompressor(level=3).compressobj()
    out = b''
    raw = sources['mixed'][:800000]
    for start in range(0, len(raw), 7001):
        out += flushed.compress(raw[start:start + 7001])
        out += flushed.flush(zstandard.COMPRESSOBJ_FLUSH_BLOCK)
    emit('many_blocks', raw, out + flushed.flush())

    # Matches that reach back further than the block they're in, so a window larger than the decoder's slack.
    repeated = noise(1 << 20, 'd') * 40
    far = zstandard.ZstdCompressor(compression_params=zstandard.ZstdCompressionParameters.from_level(3, window_log=26))
    chunks = far.compressobj()
    emit('far_matches', repeated, chunks.compress(repeated) + chunks.flush())

    print(f'{len(os.listdir(folder)) // 2} pairs written')


if __name__ == '__main__':
    main()
