#!/usr/bin/env python3
"""
Makes NSZ, XCI and XCZ files from NSPs, the way nsz and cartridge dumps lay them out, with the real Zstandard library.
Decrypted sections get compressed as NCZ for Program and PublicData NCAs, and everything else is copied. Prints no keys.

usage: make_game_files.py keys out_dir kind in.nsp [in.nsp...]
Kinds:
  solid    <stem>.nsz of each input, as one Zstandard stream per NCA, written as a stream of unknown size
  block    <stem>.nsz of each input, as separately compressed blocks
  xci      out.xci, a cartridge holding all the inputs in its secure partition
  xci-key  out.xci again, with a key area in front of the header, as some dumps have
  xcz      out.xcz, as xci but with block compressed NCZ
  xcz-solid  out.xcz, as xcz but solid
"""
import hashlib
import os
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
import zstandard

NCZ_HEADER = 0x4000
BLOCK_EXPONENT = 16
CART_PARTITION_OFFSET = 0xF000
KEY_AREA = 0x1000


def load_keys(path):
    keys = {}
    for line in open(path, encoding='ascii', errors='ignore'):
        if '=' in line:
            name, value = line.split('=', 1)
            keys[name.strip().lower()] = value.strip()
    return keys


def xts_decrypt(key, data):
    out = bytearray()
    for i in range(0, len(data), 0x200):
        op = Cipher(algorithms.AES(key), modes.XTS((i // 0x200).to_bytes(16, 'big'))).decryptor()
        out += op.update(data[i:i + 0x200]) + op.finalize()
    return bytes(out)


def ecb_decrypt(key, data):
    op = Cipher(algorithms.AES(key), modes.ECB()).decryptor()
    return op.update(data) + op.finalize()


def ctr(key, data, file_offset, upper):
    counter = upper.to_bytes(8, 'big') + (file_offset >> 4).to_bytes(8, 'big')
    return Cipher(algorithms.AES(key), modes.CTR(counter)).decryptor().update(data)


def read_pfs0(path):
    with open(path, 'rb') as f:
        data = f.read()
    count, table = struct.unpack_from('<II', data, 4)
    names = data[0x10 + count * 0x18:0x10 + count * 0x18 + table]
    start = 0x10 + count * 0x18 + table
    files = []
    for i in range(count):
        offset, size, name_offset, _ = struct.unpack_from('<QQII', data, 0x10 + i * 0x18)
        files.append((names[name_offset:names.index(b'\0', name_offset)].decode(), data[start + offset:start + offset + size]))
    return files


def write_pfs0(path, files):
    strings = bytearray()
    offsets = []
    for name, _ in files:
        offsets.append(len(strings))
        strings += name.encode() + b'\0'
    strings += b'\0' * (-len(strings) % 0x20)
    with open(path, 'wb') as f:
        f.write(b'PFS0' + struct.pack('<III', len(files), len(strings), 0))
        position = 0
        for (_, body), offset in zip(files, offsets):
            f.write(struct.pack('<QQII', position, len(body), offset, 0))
            position += len(body)
        f.write(strings)
        for _, body in files:
            f.write(body)


def hfs0(files):
    """files: [(name, bytes)]. Each entry's hash covers its first block, as on a real cartridge."""
    strings = bytearray()
    offsets = []
    for name, _ in files:
        offsets.append(len(strings))
        strings += name.encode() + b'\0'
    strings += b'\0' * (-(0x10 + len(files) * 0x40 + len(strings)) % 0x200)
    out = bytearray(b'HFS0' + struct.pack('<III', len(files), len(strings), 0))
    position = 0
    for (name, body), offset in zip(files, offsets):
        hashed = min(len(body), 0x200)
        out += struct.pack('<QQIIQ', position, len(body), offset, hashed, 0) + hashlib.sha256(body[:hashed]).digest()
        position += len(body)
    out += strings
    for _, body in files:
        out += body
    return bytes(out)


def bktr_entries(table, entry_size):
    """Every entry of a BKTR table, in order."""
    count = struct.unpack_from('<i', table, 4)[0]
    nodes = 1
    out = []
    for i in range(count):
        node = (nodes + i) * 0x4000
        _, entries, _ = struct.unpack_from('<iiq', table, node)
        for j in range(entries):
            out.append(table[node + 0x10 + j * entry_size:node + 0x10 + (j + 1) * entry_size])
    return out


class Converter:
    def __init__(self, keys):
        self.header_key = bytes.fromhex(keys['header_key'])
        self.keys = keys
        self.title_keys = {}

    def add_tickets(self, files):
        for name, body in files:
            if name.endswith('.tik'):
                rights = body[0x2A0:0x2B0]
                self.title_keys[rights] = body[0x180:0x190]

    def content_key(self, header):
        generation = max(max(header[0x206], header[0x220]) - 1, 0)
        rights = bytes(header[0x230:0x240])
        if any(rights):
            kek = bytes.fromhex(self.keys[f'titlekek_{generation:02x}'])
            return ecb_decrypt(kek, self.title_keys[rights])
        kaek = bytes.fromhex(self.keys[f'key_area_key_application_{generation:02x}'])
        return ecb_decrypt(kaek, bytes(header[0x300:0x340]))[0x20:0x30]

    def sections_of(self, nca):
        """(offset, size, crypto type, key, counter, plain bytes) for all the encrypted regions, in file order."""
        header = xts_decrypt(self.header_key, nca[:0xC00])
        key = None
        regions = []
        for i in range(4):
            start, end = struct.unpack_from('<II', header, 0x240 + i * 0x10)
            if not start and not end:
                continue
            offset, size = start * 0x200, (end - start) * 0x200
            fs = header[0x400 + i * 0x200:0x600 + i * 0x200]
            upper = struct.unpack_from('<Q', fs, 0x140)[0]
            raw = nca[offset:offset + size]
            encryption = fs[4]
            if encryption == 1:
                regions.append((offset, size, 1, b'\0' * 16, b'\0' * 16, raw))
                continue
            key = key or self.content_key(header)
            if encryption == 3:
                regions.append((offset, size, 3, key, upper.to_bytes(8, 'big') + bytes(8), ctr(key, raw, offset, upper)))
                continue
            if encryption != 4:
                raise ValueError(f'encryption type {encryption}')

            # An update: its own data in regions with their own counters, then the tables under the section's counter.
            table_offset, table_size = struct.unpack_from('<qq', fs, 0x120)
            secure = struct.unpack_from('<I', fs, 0x144)[0]
            table = ctr(key, raw[table_offset:table_offset + table_size], offset + table_offset, upper)
            entries = [struct.unpack_from('<qBBBBi', e) for e in bktr_entries(table, 0x10)]
            for j, entry in enumerate(entries):
                begin = entry[0]
                stop = entries[j + 1][0] if j + 1 < len(entries) else table_offset
                # The indirect table comes straight after the data, so the data stops at the first table.
                stop = min(stop, struct.unpack_from('<q', fs, 0x100)[0])
                if stop <= begin:
                    continue
                counter = (secure << 32) | entry[5]
                regions.append((offset + begin, stop - begin, 3, key, counter.to_bytes(8, 'big') + bytes(8),
                                ctr(key, raw[begin:stop], offset + begin, counter)))
            tables_start = struct.unpack_from('<q', fs, 0x100)[0]
            regions.append((offset + tables_start, size - tables_start, 3, key, upper.to_bytes(8, 'big') + bytes(8),
                            ctr(key, raw[tables_start:], offset + tables_start, upper)))
        return sorted(regions, key=lambda r: r[0])

    def ncz(self, nca, solid):
        regions = self.sections_of(nca)
        table = b'NCZSECTN' + struct.pack('<Q', len(regions))
        stream = bytearray()

        # As nsz does: a gap before the first section goes first in the stream, but isn't listed.
        first = regions[0][0]
        if first > NCZ_HEADER:
            stream += nca[NCZ_HEADER:first]
        for offset, size, crypto, key, counter, plain in regions:
            table += struct.pack('<QQQQ', offset, size, crypto, 0) + key + counter
            stream += plain

        out = bytearray(nca[:NCZ_HEADER]) + table
        if solid:
            compressor = zstandard.ZstdCompressor(level=5, threads=2).compressobj()
            out += compressor.compress(bytes(stream)) + compressor.flush()
            return bytes(out)

        block = 1 << BLOCK_EXPONENT
        blocks = []
        compressor = zstandard.ZstdCompressor(level=5)
        for start in range(0, len(stream), block):
            chunk = bytes(stream[start:start + block])
            packed = compressor.compress(chunk)
            blocks.append(packed if len(packed) < len(chunk) else chunk)
        out += b'NCZBLOCK' + bytes([2, 1, 0, BLOCK_EXPONENT]) + struct.pack('<IQ', len(blocks), len(stream))
        for packed in blocks:
            out += struct.pack('<I', len(packed))
        for packed in blocks:
            out += packed
        return bytes(out)

    def compress_files(self, files, solid):
        self.add_tickets(files)
        out = []
        for name, body in files:
            is_game_data = False
            if name.endswith('.nca') and not name.endswith('.cnmt.nca'):
                is_game_data = xts_decrypt(self.header_key, body[:0xC00])[0x205] in (0, 5)
            if is_game_data and len(body) > NCZ_HEADER:
                out.append((name[:-1] + 'z', self.ncz(body, solid)))
            else:
                out.append((name, body))
        return out


def write_cartridge(path, files, key_area=False):
    root = hfs0([('update', hfs0([])), ('normal', hfs0([])), ('secure', hfs0(files))])
    header = bytearray(0x200)
    header[0x100:0x104] = b'HEAD'
    root_header_size = 0x10 + 3 * 0x40 + struct.unpack_from('<I', root, 8)[0]
    struct.pack_into('<QQ', header, 0x130, CART_PARTITION_OFFSET, root_header_size)
    header[0x140:0x160] = hashlib.sha256(root[:root_header_size]).digest()
    with open(path, 'wb') as f:
        if key_area:
            f.write(bytes(KEY_AREA))
        f.write(bytes(header).ljust(CART_PARTITION_OFFSET, b'\0'))
        f.write(root)


def main():
    keys_path, out_dir, kind, *inputs = sys.argv[1:]
    os.makedirs(out_dir, exist_ok=True)
    converter = Converter(load_keys(keys_path))
    files_per_input = [read_pfs0(path) for path in inputs]
    for files in files_per_input:
        converter.add_tickets(files)

    if kind in ('solid', 'block'):
        for path, files in zip(inputs, files_per_input):
            stem = os.path.splitext(os.path.basename(path))[0]
            write_pfs0(os.path.join(out_dir, stem + '.nsz'), converter.compress_files(files, kind == 'solid'))
    else:
        merged = [item for files in files_per_input for item in files]
        if kind == 'xci':
            write_cartridge(os.path.join(out_dir, 'out.xci'), merged)
        elif kind == 'xci-key':
            write_cartridge(os.path.join(out_dir, 'out.xci'), merged, key_area=True)
        elif kind in ('xcz', 'xcz-solid'):
            write_cartridge(os.path.join(out_dir, 'out.xcz'), converter.compress_files(merged, kind == 'xcz-solid'))
        else:
            raise SystemExit(f'unknown kind {kind}')


if __name__ == '__main__':
    main()
