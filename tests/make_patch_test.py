#!/usr/bin/env python3
"""Builds a base game, a matching update and a DLC in the retail layout, without any game files.

The update uses what retail ones do: a ticket and title key, AES-CTR-Ex regions with their own counters, and an indirect
table that takes unchanged 16 KiB blocks from the base. usage: make_patch_test.py keys tool title_dir out_dir variant
Variants: plain, compressed_update, compressed_both. Writes base.nsp, update.nsp, dlc.nsp, romfs_new/ and exefs_new/.

Two more kinds of file retail games come as: base_sparse.nsp, a base game cut down to what update.nsp still reads from it, with the same version
as that update, and update_same.nsp, an update that changes nothing in the game data, which has no data of its own and an empty AES-CTR-Ex table.
"""
import hashlib, os, shutil, struct, subprocess, sys
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

KEYS_PATH, TOOL, TITLE, OUT, VARIANT = sys.argv[1:6]
KEY_AREA_KEY = b'\x04' * 16
TITLE_KEY = hashlib.md5(b'hacbrewpacklz test title key').digest()
SECURE_VALUE = 0x5A17
NODE = 0x4000
CHUNK = 0x4000

keys = {}
for line in open(KEYS_PATH, encoding='ascii', errors='ignore'):
    if '=' in line:
        name, value = line.split('=', 1)
        keys[name.strip().lower()] = value.strip()
HEADER_KEY = bytes.fromhex(keys['header_key'])
KAEK = bytes.fromhex(keys['key_area_key_application_00'])
TITLEKEK = bytes.fromhex(keys['titlekek_00'])


def xts(data, encrypt):
    out = bytearray()
    for i in range(0, len(data), 0x200):
        cipher = Cipher(algorithms.AES(HEADER_KEY), modes.XTS((i // 0x200).to_bytes(16, 'big')))
        op = cipher.encryptor() if encrypt else cipher.decryptor()
        out += op.update(data[i:i + 0x200]) + op.finalize()
    return bytes(out)


def ecb(key, data, encrypt=True):
    cipher = Cipher(algorithms.AES(key), modes.ECB())
    op = cipher.encryptor() if encrypt else cipher.decryptor()
    return op.update(data) + op.finalize()


def ctr(key, data, file_offset, upper):
    counter = upper.to_bytes(8, 'big') + (file_offset >> 4).to_bytes(8, 'big')
    return Cipher(algorithms.AES(key), modes.CTR(counter)).encryptor().update(data)


def read_nsp(path):
    data = open(path, 'rb').read()
    count, table = struct.unpack_from('<II', data, 4)
    names = data[0x10 + count * 0x18:0x10 + count * 0x18 + table]
    start = 0x10 + count * 0x18 + table
    files = {}
    for i in range(count):
        offset, size, name_offset, _ = struct.unpack_from('<QQII', data, 0x10 + i * 0x18)
        files[names[name_offset:names.index(b'\0', name_offset)].decode()] = data[start + offset:start + offset + size]
    return files


def write_pfs0(files):
    strings = bytearray()
    offsets = []
    for name, _ in files:
        offsets.append(len(strings))
        strings += name.encode() + b'\0'
    strings += b'\0' * (-len(strings) % 0x20)
    out = bytearray(b'PFS0' + struct.pack('<III', len(files), len(strings), 0))
    position = 0
    for (name, body), offset in zip(files, offsets):
        out += struct.pack('<QQII', position, len(body), offset, 0)
        position += len(body)
    out += strings
    for _, body in files:
        out += body
    return bytes(out)


class Nca:
    def __init__(self, raw):
        self.header = bytearray(xts(raw[:0xC00], False))
        self.raw = raw
        self.sections = {}
        for i in range(4):
            start, end = struct.unpack_from('<II', self.header, 0x240 + i * 0x10)
            if start or end:
                fs = bytes(self.header[0x400 + i * 0x200:0x600 + i * 0x200])
                body = raw[start * 0x200:end * 0x200]
                if fs[4] == 3:
                    body = ctr(KEY_AREA_KEY, body, start * 0x200, struct.unpack_from('<Q', fs, 0x140)[0])
                self.sections[i] = (fs, body)

    @property
    def content_type(self):
        return self.header[0x205]


def make_nca(content_type, program_id, sections, rights_id=b'\0' * 16, content_index=0):
    """sections: {index: (fs_header, encrypted_bytes)}; bytes must already be encrypted for their final offset."""
    header = bytearray(0xC00)
    header[0x200:0x204] = b'NCA3'
    header[0x205] = content_type
    position = 0xC00
    body = bytearray()
    for index, (fs, data) in sorted(sections.items()):
        if len(data) % 0x200:
            raise ValueError('section not sector aligned')
        struct.pack_into('<IIII', header, 0x240 + index * 0x10, position // 0x200, (position + len(data)) // 0x200, 1, 0)
        header[0x280 + index * 0x20:0x2A0 + index * 0x20] = hashlib.sha256(fs).digest()
        header[0x400 + index * 0x200:0x600 + index * 0x200] = fs
        body += data
        position += len(data)
    struct.pack_into('<QQII', header, 0x208, position, program_id, content_index, 0x000C1100)
    header[0x230:0x240] = rights_id
    if not any(rights_id):
        header[0x300:0x340] = ecb(KAEK, b'\0' * 0x20 + KEY_AREA_KEY + b'\0' * 0x10)
    return xts(bytes(header), True) + bytes(body)


def bktr(entries, entry_size, end):
    per_set = (NODE - 0x10) // entry_size
    sets = [entries[i:i + per_set] for i in range(0, len(entries), per_set)]
    starts = [struct.unpack_from('<q', s[0])[0] for s in sets]
    table = (struct.pack('<iiq', 0, len(sets), end) + b''.join(struct.pack('<q', s) for s in starts)).ljust(NODE, b'\0')
    for i, entry_set in enumerate(sets):
        set_end = starts[i + 1] if i + 1 < len(sets) else end
        table += (struct.pack('<iiq', i, len(entry_set), set_end) + b''.join(entry_set)).ljust(NODE, b'\0')
    return table, struct.pack('<4sIii', b'BKTR', 1, len(entries), 0)


def patch_section(new, old, file_offset, new_fs):
    """Takes every 16 KiB block of the new section that the old one holds from there, the rest from the update's own data."""
    known = {}
    for offset in range(0, len(old) - CHUNK + 1, CHUNK):
        known.setdefault(old[offset:offset + CHUNK], offset)

    own = bytearray()
    runs = []
    reused = 0
    for virtual in range(0, len(new), CHUNK):
        chunk = new[virtual:virtual + CHUNK]
        source = known.get(chunk) if len(chunk) == CHUNK else None
        if source is None:
            kind, physical = 1, len(own)
            own += chunk
        else:
            kind, physical = 0, source
            reused += len(chunk)
        if runs and runs[-1][2] == kind and runs[-1][1] + (virtual - runs[-1][0]) == physical:
            continue
        runs.append((virtual, physical, kind))
    own += b'\0' * (-len(own) % NODE)
    print(f'{VARIANT}: {reused} bytes reused from the base, {len(own)} bytes of update data in {len(runs)} runs')

    last_runs.clear()
    last_runs.extend(runs)
    indirect, indirect_header = bktr([struct.pack('<qqi', *r) for r in runs], 0x14, len(new))

    regions = max(len(own) // 0x100000, 1)
    size = (len(own) // regions) & ~0xF
    ctr_entries = [(i * size, 7 + i) for i in range(regions)]
    ctr_table, ctr_header = bktr([struct.pack('<qBBBBi', o, 0, 0, 0, 0, g) for o, g in ctr_entries], 0x10, len(own))

    encrypted = bytearray()
    for i, (offset, generation) in enumerate(ctr_entries):
        end = ctr_entries[i + 1][0] if i + 1 < regions else len(own)
        encrypted += ctr(TITLE_KEY, bytes(own[offset:end]), file_offset + offset, (SECURE_VALUE << 32) | generation)

    table_upper = (SECURE_VALUE << 32) | 1
    tables = indirect + ctr_table
    encrypted += ctr(TITLE_KEY, tables, file_offset + len(own), table_upper)

    fs = bytearray(new_fs)
    fs[4] = 4
    fs[0x100:0x140] = struct.pack('<qq', len(own), len(indirect)) + indirect_header + struct.pack('<qq', len(own) + len(indirect), len(ctr_table)) + ctr_header
    fs[0x140:0x148] = struct.pack('<II', 1, SECURE_VALUE)
    return bytes(fs), bytes(encrypted)


def section_with_nothing_new(length, file_offset, old_fs):
    """All of it comes from the base, so there's nothing for the AES-CTR-Ex table to say."""
    indirect, indirect_header = bktr([struct.pack('<qqi', 0, 0, 0)], 0x14, length)
    data = ctr(TITLE_KEY, indirect, file_offset, (SECURE_VALUE << 32) | 1)
    fs = bytearray(old_fs)
    fs[4] = 4
    fs[0x100:0x140] = struct.pack('<qq', 0, len(indirect)) + indirect_header + struct.pack('<qq', len(indirect), 0) + struct.pack('<4sIii', b'BKTR', 1, 0, 0)
    fs[0x140:0x148] = struct.pack('<II', 1, SECURE_VALUE)
    return bytes(fs), data


def cut_down(program, runs):
    """Only what the update reads from the base game is still there, encrypted just as it was. The sections still say where they used to be."""
    start, end = struct.unpack_from('<II', program.header, 0x250)
    body = program.raw[start * 0x200:end * 0x200]
    wanted = set()
    for i, (virtual, physical, kind) in enumerate(runs):
        if kind == 0:
            stop = runs[i + 1][0] if i + 1 < len(runs) else len(body)
            wanted.update(range(physical // CHUNK, (physical + stop - virtual + CHUNK - 1) // CHUNK))

    kept = bytearray()
    entries = []
    position = 0
    for chunk in sorted(wanted):
        offset = chunk * CHUNK
        if offset > position:
            entries.append((position, 0, 1))
        if offset != position or not entries or entries[-1][2] != 0:
            entries.append((offset, len(kept), 0))
        kept += body[offset:offset + CHUNK]
        position = offset + CHUNK
    if position < len(body):
        entries.append((position, 0, 1))
    kept += b'\0' * (-len(kept) % NODE)
    table, tree_header = bktr([struct.pack('<qqi', *e) for e in entries], 0x14, len(body))

    header = bytearray(program.header)
    position = 0xC00
    rest = bytearray()
    if 2 in program.sections:
        logo_fs, logo_plain = program.sections[2]
        rest += ctr(KEY_AREA_KEY, logo_plain, position, struct.unpack_from('<Q', logo_fs, 0x140)[0]) if logo_fs[4] == 3 else logo_plain
        struct.pack_into('<II', header, 0x260, position // 0x200, (position + len(logo_plain)) // 0x200)
        position += len(logo_plain)

    romfs_fs = bytearray(program.sections[1][0])
    secure = struct.unpack_from('<I', romfs_fs, 0x144)[0]
    romfs_fs[0x148:0x178] = struct.pack('<qq', len(kept), len(table)) + tree_header + struct.pack('<qH6x', position, 1)
    exefs_fs = bytearray(program.sections[0][0])
    total = position + len(kept) + len(table)
    exefs_fs[0x148:0x178] = struct.pack('<qq', 0, 0) + struct.pack('<4sIii', b'BKTR', 1, 0, 0) + struct.pack('<qH6x', total, 1)
    for index, fs in ((0, exefs_fs), (1, romfs_fs)):
        header[0x400 + index * 0x200:0x600 + index * 0x200] = fs
        header[0x280 + index * 0x20:0x2A0 + index * 0x20] = hashlib.sha256(fs).digest()
    struct.pack_into('<Q', header, 0x208, total)
    encrypted_table = ctr(KEY_AREA_KEY, table, position + len(kept), (secure << 32) | (1 << 16))
    return xts(bytes(header), True) + bytes(rest) + bytes(kept) + encrypted_table


def build(work, name, romfs, control, compress):
    args = [TOOL, '-k', KEYS_PATH, '--exefsdir', os.path.join(TITLE, 'exefs'), '--romfsdir', romfs, '--logodir', os.path.join(TITLE, 'logo'),
            '--controldir', control, '--nosignncasig2', '--nopatchnacplogo', '--nspdir', os.path.join(work, name)]
    if not compress:
        args.append('--nocompress')
    subprocess.check_call(args, stdout=subprocess.DEVNULL)
    return os.path.join(work, name, os.listdir(os.path.join(work, name))[0])


def meta_nca(title_id, cnmt_name, cnmt):
    pfs = write_pfs0([(cnmt_name, cnmt)])
    table = b''.join(hashlib.sha256(pfs[i:i + 0x1000]).digest() for i in range(0, len(pfs), 0x1000))
    pfs_offset = len(table) + (0x200 - len(table) % 0x200)
    data = table.ljust(pfs_offset, b'\0') + pfs
    data += b'\0' * (-len(data) % 0x200)
    fs = bytearray(0x200)
    fs[0] = 2
    fs[2], fs[3], fs[4] = 1, 2, 3
    fs[8:0x28] = hashlib.sha256(table).digest()
    struct.pack_into('<IIQQQQ', fs, 0x28, 0x1000, 2, 0, len(table), pfs_offset, len(pfs))
    return make_nca(1, title_id, {0: (bytes(fs), ctr(KEY_AREA_KEY, data, 0xC00, 0))})


def record(nca, content_type):
    digest = hashlib.sha256(nca).digest()
    return digest + digest[:16] + len(nca).to_bytes(6, 'little') + bytes([content_type, 0])


def nca_name(nca, extension='.nca'):
    return hashlib.sha256(nca).hexdigest()[:32] + extension


def cnmt(title_id, version, meta_type, extended, records):
    return struct.pack('<QIBBHHHBBBBII', title_id, version, meta_type, 0, len(extended), len(records), 0, 0, 0, 0, 0, 0, 0) + extended + b''.join(records) + b'\0' * 0x20


last_runs = []
os.makedirs(OUT, exist_ok=True)
work = os.path.join(OUT, 'work')
shutil.rmtree(work, ignore_errors=True)
os.makedirs(work)

# The update changes some files, adds one and removes one: most of the RomFS stays the same.
romfs_old = os.path.join(TITLE, 'romfs')
romfs_new = os.path.join(OUT, 'romfs_new')
shutil.rmtree(romfs_new, ignore_errors=True)
shutil.copytree(romfs_old, romfs_new)
with open(os.path.join(romfs_new, 'data', 'text.bin'), 'r+b') as f:
    f.seek(2_000_000)
    f.write(b'UPDATED' * 1000)
with open(os.path.join(romfs_new, 'added_by_update.bin'), 'wb') as f:
    f.write(hashlib.shake_128(b'new file').digest(300_000))
os.remove(os.path.join(romfs_new, 'b', 'short'))

base_path = build(work, 'base', romfs_old, os.path.join(TITLE, 'control'), VARIANT == 'compressed_both')
new_path = build(work, 'new', romfs_new, os.path.join(TITLE, 'control'), VARIANT != 'plain')

base_files = read_nsp(base_path)
new_files = read_nsp(new_path)
base_program = next(Nca(v) for k, v in base_files.items() if k.endswith('.nca') and not k.endswith('.cnmt.nca') and Nca(v).content_type == 0)
new_ncas = [Nca(v) for k, v in new_files.items() if k.endswith('.nca') and not k.endswith('.cnmt.nca')]
new_program = next(n for n in new_ncas if n.content_type == 0)
new_control_raw = next(v for k, v in new_files.items() if k.endswith('.nca') and not k.endswith('.cnmt.nca') and Nca(v).content_type == 2)

title_id = struct.unpack_from('<Q', new_program.header, 0x210)[0]

# ExeFS comes along in full with the update, under the ticket's title key.
exefs_fs, exefs_plain = new_program.sections[0]
sections = {0: (exefs_fs, ctr(TITLE_KEY, exefs_plain, 0xC00, 0))}
romfs_fs, romfs_plain = new_program.sections[1]
sections[1] = patch_section(romfs_plain, base_program.sections[1][1], 0xC00 + len(exefs_plain), romfs_fs)

rights_id = title_id.to_bytes(8, 'big')[:6] + b'\x08\x00' + b'\0' * 8
patch_program = make_nca(0, title_id, sections, rights_id)

ticket = bytearray(0x2C0)
struct.pack_into('<I', ticket, 0, 0x10004)
ticket[0x180:0x190] = ecb(TITLEKEK, TITLE_KEY)
ticket[0x280] = 2
ticket[0x2A0:0x2B0] = rights_id

records = [record(patch_program, 1), record(new_control_raw, 3)]
extended = struct.pack('<QIII', title_id, 0, 0, 0)
patch_meta_body = cnmt(title_id + 0x800, 65536, 0x81, extended, records)
patch_meta = meta_nca(title_id + 0x800, f'Patch_{title_id + 0x800:016x}.cnmt', patch_meta_body)

update_nsp = write_pfs0([
    (nca_name(patch_program), patch_program),
    (nca_name(new_control_raw), new_control_raw),
    (nca_name(patch_meta, '.cnmt.nca'), patch_meta),
    (rights_id.hex() + '.tik', bytes(ticket)),
])
open(os.path.join(OUT, 'update.nsp'), 'wb').write(update_nsp)
shutil.copy(base_path, os.path.join(OUT, 'base.nsp'))

base_control_raw = next(v for k, v in base_files.items() if k.endswith('.nca') and not k.endswith('.cnmt.nca') and Nca(v).content_type == 2)
sparse_program = cut_down(base_program, last_runs)
sparse_meta = meta_nca(title_id, f'Application_{title_id:016x}.cnmt',
                       cnmt(title_id, 65536, 0x80, struct.pack('<QII', title_id + 0x800, 0, 0), [record(sparse_program, 1), record(base_control_raw, 3)]))
open(os.path.join(OUT, 'base_sparse.nsp'), 'wb').write(write_pfs0([
    (nca_name(sparse_program), sparse_program),
    (nca_name(base_control_raw), base_control_raw),
    (nca_name(sparse_meta, '.cnmt.nca'), sparse_meta),
]))

old_fs, old_plain = base_program.sections[1]
same_sections = {0: sections[0], 1: section_with_nothing_new(len(old_plain), 0xC00 + len(exefs_plain), old_fs)}
same_sections[1] = (same_sections[1][0], same_sections[1][1] + b'\0' * (-len(same_sections[1][1]) % 0x200))
same_program = make_nca(0, title_id, same_sections, rights_id)
same_meta = meta_nca(title_id + 0x800, f'Patch_{title_id + 0x800:016x}.cnmt',
                     cnmt(title_id + 0x800, 65536, 0x81, extended, [record(same_program, 1), record(new_control_raw, 3)]))
open(os.path.join(OUT, 'update_same.nsp'), 'wb').write(write_pfs0([
    (nca_name(same_program), same_program),
    (nca_name(new_control_raw), new_control_raw),
    (nca_name(same_meta, '.cnmt.nca'), same_meta),
    (rights_id.hex() + '.tik', bytes(ticket)),
]))

shutil.rmtree(os.path.join(OUT, 'exefs_new'), ignore_errors=True)
shutil.copytree(os.path.join(TITLE, 'exefs'), os.path.join(OUT, 'exefs_new'))

# DLC: a single Data NCA holding a RomFS, under its own title ID.
dlc_id = title_id + 0x1001
dlc_dir = os.path.join(work, 'dlc_romfs')
shutil.copytree(os.path.join(TITLE, 'control'), dlc_dir)
with open(os.path.join(dlc_dir, 'dlc_data.bin'), 'wb') as f:
    f.write(b'DLC content that compresses well. ' * 40000)
dlc_build = read_nsp(build(work, 'dlc', romfs_old, dlc_dir, False))
dlc_raw = next(v for k, v in dlc_build.items() if k.endswith('.nca') and not k.endswith('.cnmt.nca') and Nca(v).content_type == 2)
dlc_header = bytearray(xts(dlc_raw[:0xC00], False))
dlc_header[0x205] = 4
struct.pack_into('<Q', dlc_header, 0x210, dlc_id)
dlc_nca = xts(bytes(dlc_header), True) + dlc_raw[0xC00:]
dlc_meta = meta_nca(dlc_id, f'AddOnContent_{dlc_id:016x}.cnmt',
                    cnmt(dlc_id, 0, 0x82, struct.pack('<QII', title_id, 0, 0), [record(dlc_nca, 2)]))
open(os.path.join(OUT, 'dlc.nsp'), 'wb').write(write_pfs0([(nca_name(dlc_nca), dlc_nca), (nca_name(dlc_meta, '.cnmt.nca'), dlc_meta)]))
shutil.rmtree(os.path.join(OUT, 'dlc_romfs'), ignore_errors=True)
shutil.copytree(dlc_dir, os.path.join(OUT, 'dlc_romfs'))

shutil.rmtree(work, ignore_errors=True)
print('ready', OUT)
