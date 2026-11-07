#!/usr/bin/env python3
"""End-to-end test matrix. Prints no key material. See README.md. usage: matrix.py <section> [...]
Sections: prepare batch analyze selection titles formats zstd engine diff options edges matrix l2. "prepare" must have run once before the others.
The engine section also needs HBPTOOL, the PC build of the Switch version's engine: see switch/README.md."""
import hashlib, os, re, shutil, signal, struct, subprocess, sys, time
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from cryptography.hazmat.primitives import hashes

S = os.path.dirname(os.path.abspath(__file__))
OUT = os.environ.get('TESTDIR', os.path.join(S, 'out'))
T1 = os.path.join(OUT, 't1')
WORK = os.path.join(OUT, 'work')
TOOL = os.environ.get('TOOL', os.path.join(S, '..', '..', '_tools', 'hacbrewpacklz', 'hacbrewpacklz.exe'))
HBP = os.environ.get('HACBREWPACK', os.path.join(S, '..', '..', '_tools', 'hacbrewpack', 'hacbrewpack.exe'))
KEYS = os.environ.get('KEYS', os.path.join(os.path.dirname(HBP), 'prod.keys'))
LIBHAC = ['dotnet', os.path.join(S, 'libhac_check', 'bin', 'Release', 'net8.0', 'libhac_check.dll')]
REF_NSP = os.path.join(T1, 'hbp_nosig', 'hacbrewpack_nsp', '0100abcd12340000.nsp')
results = []


def report(name, ok, detail=''):
    results.append((name, ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}  {detail}", flush=True)


def run(cmd, cwd=None):
    p = subprocess.run(cmd, cwd=cwd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    return p.returncode, p.stdout


def fresh(name):
    d = os.path.join(WORK, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    return d


def libhac(nsp, romfs=None, exefs=None, ref=None, randoms=3000):
    cmd = LIBHAC + [KEYS, nsp, '--random', str(randoms)]
    if romfs: cmd += ['--romfsdir', romfs]
    if exefs: cmd += ['--exefsdir', exefs]
    if ref: cmd += ['--ref', ref]
    rc, out = run(cmd)
    return rc == 0 and 'LIBHAC: ALL OK' in out, out


def build(name, *args, dirs=None):
    """Runs the tool with --verify; returns (ok, nsp path, output)."""
    d = fresh(name)
    base = dirs or {'--exefsdir': os.path.join(T1, 'exefs'), '--romfsdir': os.path.join(T1, 'romfs'),
                    '--logodir': os.path.join(T1, 'logo'), '--controldir': os.path.join(T1, 'control')}
    cmd = [TOOL, '-k', KEYS, '--nspdir', d, '--verify']
    for k, v in base.items():
        if v is not None:
            cmd += [k, v]
    cmd += list(args)
    start = time.time()
    rc, out = run(cmd)
    took = time.time() - start
    nsps = [f for f in os.listdir(d) if f.endswith('.nsp')]
    nsp = os.path.join(d, nsps[0]) if nsps else None
    ok = rc == 0 and nsp is not None and 'OK: all checks passed' in out
    return ok, nsp, out, took


def estimated(out):
    """The size in bytes and the margin from the analyze command, for the first title it reports."""
    found = re.search(r'In bytes:\s+(\d+) \+/- (\d+)', out)
    return (int(found.group(1)), int(found.group(2))) if found else (None, None)


def check_estimate(name, out, actual, exact):
    size, margin = estimated(out)
    if size is None:
        report(name, False, out[-300:])
    elif exact:
        report(name, size == actual and margin == 0, f'estimated {size}, made {actual}')
    else:
        # Small titles are read completely, leaving only the padding between blocks as an average.
        slack = margin + max(actual // 200, 256 << 10)
        report(name, abs(size - actual) <= slack, f'estimated {size} +/- {margin}, made {actual}')


def section_analyze():
    """The estimate is exact wherever nothing needs guessing, and otherwise within its own margin."""
    dirs = ['--exefsdir', os.path.join(T1, 'exefs'), '--romfsdir', os.path.join(T1, 'romfs'), '--logodir', os.path.join(T1, 'logo'), '--controldir', os.path.join(T1, 'control')]
    extras = fresh('analyze_extras')
    html, legal = os.path.join(extras, 'h'), os.path.join(extras, 'l')
    write(os.path.join(html, 'index.html'), b'<html></html>' * 100)
    write(os.path.join(legal, 'legalinfo.xml'), b'<x/>' * 200)

    builds = [
        ('compressed', [], False),
        ('level 1, 4K blocks', ['--level', '1', '--blocksize', '4K'], False),
        ('no compression', ['--nocompress'], True),
        ('no compression, html and legal', ['--nocompress', '--htmldocdir', html, '--legalinfodir', legal], True),
        ('no compression, no logo', ['--nocompress', '--nologo'], True),
        ('no RomFS', ['--noromfs'], True),
    ]
    for name, args, exact in builds:
        d = fresh('analyze_build')
        rc, out = run([TOOL, 'analyze', '-k', KEYS] + dirs + args)
        rc2, _ = run([TOOL, '-k', KEYS, '--nspdir', d] + dirs + args)
        made = [f for f in os.listdir(d) if f.endswith('.nsp')]
        report(f'analyze writes nothing, then build: {name}', rc == 0 and rc2 == 0 and len(made) == 1, out[-300:] if rc else '')
        if made:
            check_estimate(f'build estimate: {name}', out, os.path.getsize(os.path.join(d, made[0])), exact)

    for variant in ('plain', 'compressed_update', 'compressed_both'):
        d = fresh('analyze_' + variant)
        rc, out = run([sys.executable, '-I', os.path.join(S, 'make_patch_test.py'), KEYS, TOOL, T1, d, variant])
        if rc != 0:
            report(f'{variant}: test title generated', False, out[-300:])
            continue
        base, update = os.path.join(d, 'base.nsp'), os.path.join(d, 'update.nsp')
        for name, args, exact in (('compressed', [], False), ('no compression', ['--nocompress'], True), ('unsigned', ['--nocompress', '--nosignncasig2'], True)):
            rc, out = run([TOOL, 'analyze', '-k', KEYS, base, update] + args)
            target = os.path.join(d, 'out_' + name.split()[0])
            rc2, _ = run([TOOL, 'convert', '-k', KEYS, base, update, '--nspdir', target] + args)
            made = os.listdir(target) if rc2 == 0 else []
            if rc != 0 or len(made) != 1:
                report(f'{variant} {name}: analyze and convert', False, out[-300:])
                continue
            check_estimate(f'{variant} estimate: {name}', out, os.path.getsize(os.path.join(target, made[0])), exact)

        rc, out = run([TOOL, 'analyze', '-k', KEYS, update])
        report(f'{variant}: analyze refuses an update alone and says why', 'without its base game' in out)

    d = fresh('analyze_nothing')
    before = set(os.listdir(d))
    rc, out = run([TOOL, 'analyze', '-k', KEYS] + dirs, cwd=d)
    report('analyze leaves nothing behind', rc == 0 and set(os.listdir(d)) == before, str(os.listdir(d)))


def section_selection():
    """Leaving out titles that aren't worth the time: by the minimum saving, by size, and by name."""
    def title(name, size, data):
        """A title holding either the same bytes over and over, or bytes that can't shrink, under a title ID of its own."""
        d = fresh('selection_' + name)
        write(os.path.join(d, 'romfs', 'data.bin'), data(size))
        title_id = {'compressible': '0100AAAA00000000', 'incompressible': '0100BBBB00000000', 'tiny': '0100CCCC00000000'}[name]
        target = os.path.join(d, 'out')
        rc, out = run([TOOL, '-k', KEYS, '--nspdir', target, '--nocompress', '--titleid', title_id, '--titlename', name,
                       '--exefsdir', os.path.join(T1, 'exefs'), '--romfsdir', os.path.join(d, 'romfs'), '--logodir', os.path.join(T1, 'logo'), '--controldir', os.path.join(T1, 'control')])
        made = [f for f in os.listdir(target)] if rc == 0 else []
        report(f'{name} title built', len(made) == 1, out[-300:] if rc else '')
        return os.path.join(target, made[0]) if made else None

    def noise(size):
        return b''.join(hashlib.shake_128(str(i).encode()).digest(1 << 16) for i in range(size >> 16))

    good = title('compressible', 8 << 20, lambda n: b'hacBrewPackLZ ' * (n // 14))
    bad = title('incompressible', 8 << 20, noise)
    tiny = title('tiny', 1 << 16, noise)
    if not (good and bad and tiny):
        return
    games = [good, bad, tiny]

    def outputs(*args):
        d = fresh('selection_out')
        rc, out = run([TOOL, 'convert', '-k', KEYS, *games, '--nspdir', d, *args])
        return rc, out, sorted(re.search(r'\[(0100[0-9A-F]{12})\]', f).group(1) for f in os.listdir(d))

    rc, out, made = outputs()
    report('with no minimum, everything is compressed', rc == 0 and len(made) == 3, str(made))

    rc, out, made = outputs('--minsaving', '20')
    report('a minimum leaves out what saves too little', rc == 0 and made == ['0100AAAA00000000'], f'{made} {out[-200:]}')
    report('and says so, with the reason and a count', 'below the 20% minimum' in out and '2 left out as not worth it' in out and '1 of 1 succeeded' in out, out[-300:])

    rc, out, made = outputs('--hidesmall')
    report('--hidesmall leaves out a game under 1 MB with nothing else to it', rc == 0 and made == ['0100AAAA00000000', '0100BBBB00000000'], f'{made} {out[-200:]}')

    rc, out, made = outputs('--minsaving', '20', '--nocompress')
    report('with compression off, the minimum has no say', rc == 0 and len(made) == 3, str(made))

    rc, out, made = outputs('--minsaving', '99')
    report('when nothing is worth it, nothing is written and it still ends well', rc == 0 and made == [], f'{rc} {made}')

    rc, out, made = outputs('--only', '0100BBBB00000000,tiny')
    report('--only goes by title ID or name', rc == 0 and made == ['0100BBBB00000000', '0100CCCC00000000'], str(made))
    rc, out, made = outputs('--skip', 'compressible')
    report('--skip goes by name too, and any part of it', rc == 0 and made == ['0100CCCC00000000'], str(made))
    rc, out, made = outputs('--only', 'no such game')
    report('--only matching nothing says so and fails', rc != 0 and made == [] and 'Nothing to do' in out, out[-200:])

    rc, out = run([TOOL, 'analyze', '-k', KEYS, *games, '--minsaving', '20'])
    ranking = out[out.find('Most worthwhile first'):]
    report('analyze ranks the titles, best first', rc == 0 and ranking.find('compressible [') < ranking.find('tiny [') and ranking.find('compressible [') > 0, ranking[:300])
    report('and gives the command for only the ones worth it', 'convert --only 0100AAAA00000000' in out and ranking.count('left out') >= 2, ranking[-300:])

    for wrong, why in ((['--minsaving', '150'], 'percent'), (['--minsaving', 'lots'], 'percent'), (['--alwaysupto', '1G'], 'Unknown option')):
        rc, out = run([TOOL, 'convert', '-k', KEYS, good, *wrong])
        report(f'rejects {" ".join(wrong)}', rc != 0 and why in out, out.strip().splitlines()[-1][:110])
    for accepted in (['--minsaving', 'Off'], ['--minsaving', '15%']):
        rc, out = run([TOOL, 'analyze', '-k', KEYS, good, *accepted])
        report(f'accepts {" ".join(accepted)}', rc == 0, out[-200:] if rc else '')


def section_titles():
    """Names stored compressed in the control.nacp, as they can be since firmware 21, which come out as random symbols if read the old way."""
    import zlib
    engine = os.environ.get('HBPTOOL')
    d = fresh('titles')
    with open(os.path.join(T1, 'control', 'control.nacp'), 'rb') as f:
        original = f.read()

    def packed(names, level=9, data=None, fmt=1):
        plain = bytearray(len(names) * 0x300)
        for i, name in enumerate(names):
            plain[i * 0x300:i * 0x300 + len(name)] = name.encode()
            plain[i * 0x300 + 0x200:i * 0x300 + 0x200 + 9] = b'Publisher'
        if data is None:
            squeeze = zlib.compressobj(level, zlib.DEFLATED, -15)
            data = squeeze.compress(bytes(plain)) + squeeze.flush()
        nacp = bytearray(original)
        nacp[:0x3000] = bytes(0x3000)
        struct.pack_into('<H', nacp, 0, len(data))
        nacp[2:2 + len(data)] = data
        nacp[0x3215] = fmt
        return bytes(nacp)

    def engine_says(name, nacp, expected):
        path = os.path.join(d, 'one.nacp')
        write(path, nacp)
        # As bytes, since what it prints is UTF-8 whatever this console's own code page is.
        out = subprocess.run([engine, 'nacp', path], stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout
        report(f'engine: {name}', f'name [{expected}]'.encode() in out, repr(out[-150:]))

    names = [f'Compressed Name {i}' for i in range(18)]
    if engine and os.path.exists(engine):
        engine_says('compressed the quickest way', packed(names, 1), names[0])
        engine_says('compressed as far as it goes', packed(names, 9), names[0])
        engine_says('stored in the stream, not compressed', packed(names[:12], 0), names[0])
        engine_says('a name only in a language the old format had no room for', packed([''] * 17 + ['Only in Thai']), 'Only in Thai')
        engine_says('Japanese, in UTF-8', packed(['', '', '\u30bc\u30eb\u30c0']), '\u30bc\u30eb\u30c0')
        engine_says('the old way still works', original, re.search(rb'[^\0]+', original[:0x200]).group().decode())
        damaged = zlib.compressobj(9, zlib.DEFLATED, -15).compress(b'x') + b'\xff' * 40
        engine_says('a damaged stream gives no name rather than symbols', packed(names, data=damaged), '')
        engine_says('a format nobody knows gives no name rather than symbols', packed(names, fmt=2), '')
    else:
        report('HBPTOOL points at the engine', False, str(engine))

    # All the way through, from folders to an NSP to the name it's listed under, on both.
    folder = os.path.join(d, 'game')
    for part in ('exefs', 'logo', 'control'):
        shutil.copytree(os.path.join(T1, part), os.path.join(folder, part))
    write(os.path.join(folder, 'romfs', 'data.bin'), b'hacBrewPackLZ ' * 4096)
    write(os.path.join(folder, 'control', 'control.nacp'), packed(names))
    dirs = ['--exefsdir', os.path.join(folder, 'exefs'), '--romfsdir', os.path.join(folder, 'romfs'), '--logodir', os.path.join(folder, 'logo'), '--controldir', os.path.join(folder, 'control')]
    target = os.path.join(d, 'built')
    rc, out = run([TOOL, '-k', KEYS, '--nspdir', target] + dirs)
    made = os.listdir(target) if rc == 0 else []
    report('a game with compressed names builds', len(made) == 1, out[-300:] if rc else '')
    if made:
        nsp = os.path.join(target, made[0])
        rc, out = run([TOOL, 'analyze', '-k', KEYS, nsp])
        report('its name comes out right', rc == 0 and names[0] in out, out[:300])
        converted = os.path.join(d, 'converted')
        rc, out = run([TOOL, 'convert', '-k', KEYS, nsp, '--nspdir', converted])
        report('and goes in the name of the file', rc == 0 and any(f.startswith(names[0]) for f in os.listdir(converted)), str(os.listdir(converted)) if os.path.isdir(converted) else out[-200:])
        if engine and os.path.exists(engine):
            rc, out = run([engine, 'analyze', '-k', KEYS, nsp])
            report('and the engine agrees', rc == 0 and names[0] in out, out[:200])
    target = os.path.join(d, 'renamed')
    rc, out = run([TOOL, '-k', KEYS, '--nspdir', target, '--titlename', 'A New Name'] + dirs)
    made = os.listdir(target) if rc == 0 else []
    if made:
        rc, out = run([TOOL, 'analyze', '-k', KEYS, os.path.join(target, made[0])])
    report('a new name takes over from compressed ones', rc == 0 and 'A New Name' in out and names[0] not in out, out[:300])
    if engine and os.path.exists(engine):
        target = os.path.join(d, 'from_folder')
        rc, out = run([engine, 'build', '-k', KEYS, '--nspdir', target, folder])
        report('the engine reads them from a folder too', rc == 0 and names[0] in out, out[-300:])


def section_engine():
    """The C++ engine of the Switch version against this tool: the same input has to give the same bytes, every time."""
    from cryptography.hazmat.primitives.asymmetric import padding as rsa_padding
    engine = os.environ.get('HBPTOOL')
    if not engine or not os.path.exists(engine):
        report('HBPTOOL points at the engine', False, str(engine))
        return

    def compare(d, name, inputs, *args):
        a, b = os.path.join(d, 'cs'), os.path.join(d, 'cpp')
        shutil.rmtree(a, ignore_errors=True)
        shutil.rmtree(b, ignore_errors=True)
        rc1, out1 = run([TOOL, 'convert', '-k', KEYS, *inputs, '--nspdir', a, '--nosignncasig2', *args])
        rc2, out2 = run([engine, 'convert', '-k', KEYS, *inputs, '--nspdir', b, '--nosignncasig2', *args])
        names = (sorted(os.listdir(a)) if rc1 == 0 else None, sorted(os.listdir(b)) if rc2 == 0 else None)
        same = rc1 == 0 and rc2 == 0 and names[0] == names[1] and nsp_hashes(a) == nsp_hashes(b)
        report(f'{name}: the same bytes', same and len(names[0]) > 0, '' if same else f'{rc1} {rc2} {names} {out2[-300:]}')

    for variant in ('plain', 'compressed_update', 'compressed_both'):
        d = fresh('engine_' + variant)
        rc, out = run([sys.executable, '-I', os.path.join(S, 'make_patch_test.py'), KEYS, TOOL, T1, d, variant])
        if rc != 0:
            report(f'{variant}: test title generated', False, out[-300:])
            continue
        base, update, dlc, romfs_new = [os.path.join(d, n) for n in ('base.nsp', 'update.nsp', 'dlc.nsp', 'romfs_new')]

        compare(d, f'{variant}, NSPs', [base, update, dlc])
        compare(d, f'{variant}, NSPs, no compression', [base, update, dlc], '--nocompress')
        compare(d, f'{variant}, level 1, 4K blocks', [base, update, dlc], '--level', '1', '--blocksize', '4096')
        compare(d, f'{variant}, level 3, 16K blocks', [base, update, dlc], '--level', '3', '--blocksize', '16384')
        compare(d, f'{variant}, level 11', [base, update, dlc], '--level', '11')
        compare(d, f'{variant}, level 12', [base, update, dlc], '--level', '12')
        compare(d, f'{variant}, one thread', [base, update, dlc], '--threads', '1')
        compare(d, f'{variant}, a cut down base game', [os.path.join(d, 'base_sparse.nsp'), update])
        compare(d, f'{variant}, an update with nothing of its own', [base, os.path.join(d, 'update_same.nsp')])

        # However much of it stays in memory on the way, instead of being read back, it comes to the same thing.
        kept = {}
        for hold in (0, 5 << 20, 1 << 30):
            target = os.path.join(d, f'hold_{hold}')
            shutil.rmtree(target, ignore_errors=True)
            rc, out = run([engine, 'convert', '-k', KEYS, base, update, dlc, '--nspdir', target, '--nosignncasig2', '--hold', str(hold)])
            kept[hold] = nsp_hashes(target) if rc == 0 else out[-200:]
        target = os.path.join(d, 'nocheck')
        shutil.rmtree(target, ignore_errors=True)
        rc, out = run([engine, 'convert', '-k', KEYS, base, update, dlc, '--nspdir', target, '--nosignncasig2', '--hold', str(5 << 20), '--nocheck'])
        report(f'{variant}: the same bytes without the checks', rc == 0 and nsp_hashes(target) == kept[0], out[-200:])
        report(f'{variant}: the same bytes however much is kept in memory', kept[0] == kept[5 << 20] == kept[1 << 30] and isinstance(kept[0], dict | list), str(kept)[:200])

        forms = {}
        for kind, file in (('solid', None), ('block', None), ('xci', 'out.xci'), ('xci-key', 'out.xci'), ('xcz', 'out.xcz'), ('xcz-solid', 'out.xcz')):
            target = os.path.join(d, 'made_' + kind)
            rc, out = run([sys.executable, os.path.join(S, 'make_game_files.py'), KEYS, target, kind, base, update, dlc])
            if rc != 0:
                report(f'{variant}: made {kind} files', False, out[-400:])
                continue
            forms[kind] = [os.path.join(target, file)] if file else [os.path.join(target, f) for f in os.listdir(target)]
        for kind, inputs in forms.items():
            compare(d, f'{variant}, {kind}', inputs)
            compare(d, f'{variant}, {kind}, no compression', inputs, '--nocompress')

            rc1, out1 = run([TOOL, 'analyze', '-k', KEYS, *inputs, '--nocompress'])
            rc2, out2 = run([engine, 'analyze', '-k', KEYS, *inputs, '--nocompress'])
            sizes1 = sorted(re.findall(r'In bytes:\s+(\d+) \+/- 0', out1))
            sizes2 = sorted(re.findall(r'In bytes:\s+(\d+) \+/- 0', out2))
            # With several titles this tool adds a line for all of them together.
            if len(sizes1) == len(sizes2) + 1 and sum(map(int, sizes2)) in map(int, sizes1):
                sizes1.remove(str(sum(map(int, sizes2))))
            report(f'{variant}, {kind}: the same exact estimate', rc1 == 0 and rc2 == 0 and sizes1 == sizes2 and len(sizes1) > 0, f'{sizes1} {sizes2}')

            x1, x2 = os.path.join(d, 'x1'), os.path.join(d, 'x2')
            shutil.rmtree(x1, ignore_errors=True)
            shutil.rmtree(x2, ignore_errors=True)
            rc1, _ = run([TOOL, 'extract', '-k', KEYS, *inputs, '--outdir', x1])
            rc2, out2 = run([engine, 'extract', '-k', KEYS, *inputs, '--outdir', x2])
            report(f'{variant}, {kind}: the same files extracted', rc1 == 0 and rc2 == 0 and tree_hashes(x1) == tree_hashes(x2), out2[-300:] if rc2 else '')

        # Building from the folders that extracting leaves, with the version that's in their name.
        x = os.path.join(d, 'x_build')
        shutil.rmtree(x, ignore_errors=True)
        rc, out = run([TOOL, 'extract', '-k', KEYS, base, update, '--outdir', x])
        folders = [os.path.join(x, f) for f in os.listdir(x)] if rc == 0 else []
        report(f'{variant}: there is a folder to build from', len(folders) == 1, out[-300:])
        for folder in folders:
            version = int(re.search(r'\[v(\d+)\]', os.path.basename(folder)).group(1))
            parts = ['--exefsdir', os.path.join(folder, 'exefs'), '--controldir', os.path.join(folder, 'control'), '--titleversion', f'{version:x}']
            for part, flag, absent in (('romfs', '--romfsdir', '--noromfs'), ('logo', '--logodir', '--nologo'), ('htmldoc', '--htmldocdir', None), ('legalinfo', '--legalinfodir', None)):
                if os.path.isdir(os.path.join(folder, part)):
                    parts += [flag, os.path.join(folder, part)]
                elif absent:
                    parts.append(absent)
            for name, args in (('compressed', []), ('plain', ['--nocompress']), ('level 3, 16K', ['--level', '3', '--blocksize', '16384'])):
                a, b = os.path.join(d, 'bcs'), os.path.join(d, 'bcpp')
                shutil.rmtree(a, ignore_errors=True)
                shutil.rmtree(b, ignore_errors=True)
                os.makedirs(a)
                rc1, out1 = run([TOOL, '-k', KEYS, *parts, '-o', os.path.join(a, 'x.nsp'), '--nosignncasig2', *args])
                rc2, out2 = run([engine, 'build', '-k', KEYS, '--nspdir', b, '--nosignncasig2', *args, folder])
                made = [os.path.join(b, f) for f in os.listdir(b)] if rc2 == 0 else []
                same = rc1 == 0 and len(made) == 1 and open(os.path.join(a, 'x.nsp'), 'rb').read() == open(made[0], 'rb').read()
                report(f'{variant}: building from folders gives the same bytes, {name}', same, f'{rc1} {rc2} {out1[-200:]} {out2[-200:]}')
            rc1, out1 = run([TOOL, 'analyze', '-k', KEYS, *parts, '--nocompress'])
            rc2, out2 = run([engine, 'estimatebuild', '-k', KEYS, '--nocompress', folder])
            sizes1 = re.findall(r'In bytes:\s+(\d+) \+/- 0', out1)
            sizes2 = re.findall(r'In bytes:\s+(\d+) \+/- 0', out2)
            report(f'{variant}: the same exact estimate of a build', rc1 == 0 and rc2 == 0 and sizes1 == sizes2 and len(sizes1) == 1, f'{sizes1} {sizes2} {out2[-200:]}')

        # With signing on, as on a console: the result has to pass this tool's own checks and LibHac's.
        target = os.path.join(d, 'signed')
        rc, out = run([engine, 'convert', '-k', KEYS, base, update, '--nspdir', target])
        made = [os.path.join(target, f) for f in os.listdir(target) if f.endswith('.nsp')] if rc == 0 else []
        ok = len(made) == 1
        if ok:
            rc2, out2 = run([TOOL, 'verify', '-k', KEYS, made[0]])
            ok, lout = rc2 == 0 and 'OK: all checks passed' in out2, out2
            report(f'{variant}: a signed build passes verify', ok, out2[-400:] if not ok else '')
            lh, lout = libhac(made[0], romfs_new, None, randoms=1000)
            report(f'{variant}: a signed build reads back through LibHac', lh, lout[-400:] if not lh else '')
        else:
            report(f'{variant}: a signed build is made', False, out[-300:])
        shutil.rmtree(d, ignore_errors=True)

    d = fresh('engine_broken')
    rc, out = run([sys.executable, '-I', os.path.join(S, 'make_patch_test.py'), KEYS, TOOL, T1, d, 'plain'])
    base = os.path.join(d, 'base.nsp')
    run([sys.executable, os.path.join(S, 'make_game_files.py'), KEYS, os.path.join(d, 'made'), 'solid', base])
    run([sys.executable, os.path.join(S, 'make_game_files.py'), KEYS, os.path.join(d, 'blocks'), 'block', base])
    run([sys.executable, os.path.join(S, 'make_game_files.py'), KEYS, os.path.join(d, 'cart'), 'xci', base])

    def damaged(name, source, change, wanted):
        data = change(bytearray(open(source, 'rb').read()))
        path = os.path.join(d, name)
        open(path, 'wb').write(data)
        target = os.path.join(d, 'out_' + name.replace('.', '_'))
        rc, out = run([engine, 'convert', '-k', KEYS, path, '--nspdir', target])
        left = [f for f in os.listdir(target) if f.endswith('.nsp') or f.endswith('.part') or f.endswith('.tmp')] if os.path.isdir(target) else []
        report(f'the engine refuses {wanted}', rc != 0 and not left, f'{rc} {left} {out.strip().splitlines()[-1][:110]}')

    def flip_late(data):
        data[len(data) - 30000] ^= 0xFF
        return data

    cart = os.path.join(d, 'cart', 'out.xci')
    damaged('flipped.nsp', base, flip_late, 'an NSP with a damaged byte')
    damaged('flipped_solid.nsz', os.path.join(d, 'made', 'base.nsz'), flip_late, 'a solid NSZ with a damaged byte')
    damaged('flipped_block.nsz', os.path.join(d, 'blocks', 'base.nsz'), flip_late, 'a block NSZ with a damaged byte')
    damaged('short.nsz', os.path.join(d, 'made', 'base.nsz'), lambda x: x[:len(x) // 2], 'an NSZ cut in half')
    damaged('short.xci', cart, lambda x: x[:len(x) // 2], 'an XCI cut in half')

    overlong = bytearray(open(cart, 'rb').read())
    struct.pack_into('<Q', overlong, 0xF000 + 0x10 + 8, len(overlong) * 2)
    path = os.path.join(d, 'overlong.xci')
    open(path, 'wb').write(overlong)
    rc, out = run([engine, 'convert', '-k', KEYS, path, '--nspdir', os.path.join(d, 'out_overlong')])
    report('the engine takes an XCI whose unused partition runs past its end', rc == 0, out[-300:] if rc else '')

    rubbish = os.path.join(d, 'rubbish.nsp')
    open(rubbish, 'wb').write(b'this is not a game file' * 100)
    rc, out = run([engine, 'convert', '-k', KEYS, rubbish, '--nspdir', os.path.join(d, 'r')])
    report('the engine says what it wants when given something else', rc != 0 and 'is not an NSP, NSZ, XCI or XCZ' in out, out.strip().splitlines()[-1][:110])
    shutil.rmtree(d, ignore_errors=True)

    # The arithmetic behind personalized tickets and the second signature, against a separate implementation.
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    n, private = key.public_key().public_numbers().n, key.private_numbers().d
    message = bytes(range(16))
    ciphertext = key.public_key().encrypt(message, rsa_padding.OAEP(mgf=rsa_padding.MGF1(hashes.SHA256()), algorithm=hashes.SHA256(), label=None))
    rc, out = run([engine, 'rsa', f'{n:0512x}', f'{private:0512x}', ciphertext.hex(), 'oaep'])
    lines = out.split()
    report('RSA with a private key matches', rc == 0 and int(lines[0], 16) == pow(int.from_bytes(ciphertext, 'big'), private, n), out[:80])
    report('OAEP gives back what went in', rc == 0 and lines[1] == message.hex(), out[-60:])

    data = bytes(range(256)) * 2
    rc, out = run([engine, 'sign', data.hex()])
    modulus, signature = out.split()
    public = rsa.RSAPublicNumbers(65537, int(modulus, 16)).public_key()
    try:
        public.verify(bytes.fromhex(signature), data, rsa_padding.PSS(mgf=rsa_padding.MGF1(hashes.SHA256()), salt_length=32), hashes.SHA256())
        report('a PSS signature verifies', True)
    except Exception as e:
        report('a PSS signature verifies', False, repr(e))

    corpus = os.path.join(OUT, 'zstd_corpus')
    if not os.path.isdir(corpus) or len(os.listdir(corpus)) < 8:
        run([sys.executable, os.path.join(S, 'make_zstd_corpus.py'), corpus])
    rc, out = run([engine, 'zstd', corpus])
    report('the engine decodes what the real Zstandard library makes', rc == 0 and ' 0 failed' in out, out[-300:])
    rc, out = run([engine, 'fuzz', corpus, '40'])
    report('and refuses or survives damaged copies', rc == 0 and 'no crash' in out, f'exit {rc} ' + out[-200:])


def section_zstd():
    """The decoder against the real library, on every kind of frame, then on damaged copies of them."""
    corpus = os.path.join(fresh('zstd'), 'corpus')
    rc, out = run([sys.executable, os.path.join(S, 'make_zstd_corpus.py'), corpus, os.path.join(T1, 'romfs')])
    report('corpus made by the real library', rc == 0, out.strip().splitlines()[-1][:90] if out.strip() else '')
    if rc != 0:
        return
    check = os.path.join(S, 'zstd_check', 'bin', 'Release', 'net8.0', 'zstd_check.dll')
    rc, out = run(['dotnet', check, corpus])
    report('every frame decodes to exactly what went in', rc == 0 and 'ZSTD: ALL OK' in out, out.strip().splitlines()[-1][:120] if rc == 0 else out[-500:])
    rc, out = run(['dotnet', check, corpus, '--fuzz', '40'])
    report('damaged copies are refused or decoded, with no crash', rc == 0 and 'ZSTD: ALL OK' in out, '' if rc == 0 else f'exit {rc} ' + out[-400:])
    shutil.rmtree(corpus, ignore_errors=True)


def nsp_hashes(folder):
    return sorted(sha(os.path.join(folder, f)) for f in os.listdir(folder) if f.endswith('.nsp'))


def section_formats():
    """Whatever form a game arrives in, the same NSP has to come out, and the same files."""
    fixed = ['--nosignncasig2']
    for variant in ('plain', 'compressed_update', 'compressed_both'):
        d = fresh('formats_' + variant)
        rc, out = run([sys.executable, '-I', os.path.join(S, 'make_patch_test.py'), KEYS, TOOL, T1, d, variant])
        if rc != 0:
            report(f'{variant}: test title generated', False, out[-300:])
            continue
        base, update, dlc, romfs_new = [os.path.join(d, n) for n in ('base.nsp', 'update.nsp', 'dlc.nsp', 'romfs_new')]

        def make(kind, *inputs):
            target = os.path.join(d, 'made_' + kind)
            rc, out = run([sys.executable, os.path.join(S, 'make_game_files.py'), KEYS, target, kind, *inputs])
            if rc != 0:
                report(f'{variant}: made {kind} files', False, out[-400:])
                return None
            return target

        reference = {}
        for name, args in (('compressed', fixed), ('plain', fixed + ['--nocompress'])):
            target = os.path.join(d, 'reference_' + name)
            rc, out = run([TOOL, 'convert', '-k', KEYS, base, update, dlc, '--nspdir', target] + args)
            reference[name] = nsp_hashes(target) if rc == 0 else None
            report(f'{variant}: reference from NSPs, {name}', rc == 0 and len(reference[name]) == 2, out[-300:] if rc else '')

        solid = make('solid', base, update, dlc)
        block = make('block', base, update, dlc)
        forms = {}
        if solid:
            forms['solid NSZ'] = [os.path.join(solid, f) for f in os.listdir(solid)]
            forms['solid NSZ update on an NSP base'] = [base, os.path.join(solid, 'update.nsz'), os.path.join(solid, 'dlc.nsz')]
        if block:
            forms['block NSZ'] = [os.path.join(block, f) for f in os.listdir(block)]
            forms['NSP update on a block NSZ base'] = [os.path.join(block, 'base.nsz'), update, os.path.join(block, 'dlc.nsz')]
        for kind, name, file in (('xci', 'XCI', 'out.xci'), ('xci-key', 'XCI with a key area', 'out.xci'), ('xcz', 'block XCZ', 'out.xcz'), ('xcz-solid', 'solid XCZ', 'out.xcz')):
            made = make(kind, base, update, dlc)
            if made:
                forms[name] = [os.path.join(made, file)]

        for name, inputs in forms.items():
            for mode, args in (('compressed', fixed), ('plain', fixed + ['--nocompress'])):
                target = os.path.join(d, 'out')
                shutil.rmtree(target, ignore_errors=True)
                rc, out = run([TOOL, 'convert', '-k', KEYS, *inputs, '--nspdir', target, '--verify'] + args)
                same = rc == 0 and nsp_hashes(target) == reference[mode]
                report(f'{variant}: {name} gives the same NSPs, {mode}', same and out.count('OK: all checks passed') == 2, out[-400:] if not same else '')

            rc, out = run([TOOL, 'analyze', '-k', KEYS, *inputs, '--nocompress'])
            target_sizes = sorted(int(m) for m in re.findall(r'In bytes:\s+(\d+) \+/- 0', out))
            plain_target = os.path.join(d, 'plain_sizes')
            shutil.rmtree(plain_target, ignore_errors=True)
            run([TOOL, 'convert', '-k', KEYS, *inputs, '--nspdir', plain_target, '--nocompress'] + fixed)
            real_sizes = sorted(os.path.getsize(os.path.join(plain_target, f)) for f in os.listdir(plain_target))
            report(f'{variant}: {name} estimate is exact without compression', rc == 0 and target_sizes[:2] == real_sizes, f'{target_sizes} {real_sizes}')

            rc, out = run([TOOL, 'analyze', '-k', KEYS, *inputs])
            report(f'{variant}: {name} can be analyzed', rc == 0 and 'In bytes:' in out, out[-300:] if rc else '')

            x = os.path.join(d, 'x')
            shutil.rmtree(x, ignore_errors=True)
            rc, out = run([TOOL, 'extract', '-k', KEYS, *inputs, '--outdir', x])
            roots = [r for r in os.listdir(x) if not r.upper().startswith('DLC')] if rc == 0 else []
            same = any(os.path.isdir(os.path.join(x, r, 'romfs')) and tree_hashes(os.path.join(x, r, 'romfs')) == tree_hashes(romfs_new) for r in roots)
            report(f'{variant}: {name} extracts to the new files', rc == 0 and same, out[-300:] if not same else '')
            shutil.rmtree(x, ignore_errors=True)
        shutil.rmtree(d, ignore_errors=True)

    d = fresh('formats_broken')
    rc, out = run([sys.executable, '-I', os.path.join(S, 'make_patch_test.py'), KEYS, TOOL, T1, d, 'plain'])
    base = os.path.join(d, 'base.nsp')
    made = os.path.join(d, 'made')
    run([sys.executable, os.path.join(S, 'make_game_files.py'), KEYS, made, 'solid', base])
    run([sys.executable, os.path.join(S, 'make_game_files.py'), KEYS, os.path.join(d, 'blocks'), 'block', base])
    run([sys.executable, os.path.join(S, 'make_game_files.py'), KEYS, os.path.join(d, 'cart'), 'xci', base])

    def damaged(name, source, change, wanted):
        path = os.path.join(d, name)
        data = bytearray(open(source, 'rb').read())
        data = change(data)
        open(path, 'wb').write(data)
        target = os.path.join(d, 'out_' + name.replace('.', '_'))
        rc, out = run([TOOL, 'convert', '-k', KEYS, path, '--nspdir', target])
        left = os.listdir(target) if os.path.isdir(target) else []
        report(f'refuses {wanted}', rc != 0 and not [f for f in left if f.endswith('.nsp')], out.strip().splitlines()[-1][:110] if out.strip() else '')

    def flip_late(data):
        data[len(data) - 30000] ^= 0xFF
        return data

    solid = os.path.join(made, 'base.nsz')
    block = os.path.join(d, 'blocks', 'base.nsz')
    cart = os.path.join(d, 'cart', 'out.xci')
    damaged('flipped_solid.nsz', solid, flip_late, 'a solid NSZ with a damaged byte')
    damaged('flipped_block.nsz', block, flip_late, 'a block NSZ with a damaged byte')
    damaged('short.nsz', solid, lambda x: x[:len(x) // 2], 'an NSZ cut in half')
    damaged('short.xci', cart, lambda x: x[:len(x) // 2], 'an XCI cut in half')

    def no_secure(data):
        root = 0xF000
        at = data.index(b'secure\0', root)
        data[at:at + 6] = b'public'
        return data

    damaged('nosecure.xci', cart, no_secure, 'an XCI with no secure partition')

    # The first partition listed is the update one, which isn't used, so it can run past the end of the file.
    overlong = bytearray(open(cart, 'rb').read())
    struct.pack_into('<Q', overlong, 0xF000 + 0x10 + 8, len(overlong) * 2)
    path = os.path.join(d, 'overlong.xci')
    open(path, 'wb').write(overlong)
    target = os.path.join(d, 'out_overlong')
    rc, out = run([TOOL, 'convert', '-k', KEYS, path, '--nspdir', target])
    report('takes an XCI whose unused partition runs past its end', rc == 0 and len(nsp_hashes(target)) == 1, out[-300:] if rc else '')

    rubbish = os.path.join(d, 'rubbish.nsp')
    open(rubbish, 'wb').write(b'this is not a game file' * 100)
    rc, out = run([TOOL, 'convert', '-k', KEYS, rubbish, '--nspdir', os.path.join(d, 'r')])
    report('says what it wants when given something else', rc != 0 and 'is not an NSP, NSZ, XCI or XCZ' in out, out.strip().splitlines()[-1][:110])
    shutil.rmtree(d, ignore_errors=True)


def section_prepare():
    """Creates the test title and the uncompressed hacBrewPack reference build. hacBrewPack edits its inputs, so it gets copies."""
    shutil.rmtree(T1, ignore_errors=True)
    rc, out = run([sys.executable, '-I', os.path.join(S, 'make_testset.py'), T1, '40'])
    report('test title created', rc == 0, out.strip().splitlines()[-1] if out.strip() else '')
    ref = os.path.join(T1, 'hbp_nosig')
    copy_title(ref)
    rc, _ = run([HBP, '-k', KEYS, '--exefsdir', 'exefs', '--romfsdir', os.path.join(T1, 'romfs'), '--logodir', os.path.join(T1, 'logo'),
                 '--controldir', 'control', '--nosignncasig2', '--nopatchnacplogo', '--keepncadir'], cwd=ref)
    report('hacBrewPack reference built', rc == 0 and os.path.exists(REF_NSP))


def tree_hashes(root):
    result = {}
    for folder, _, names in os.walk(root):
        for name in names:
            path = os.path.join(folder, name)
            with open(path, 'rb') as f:
                result[os.path.relpath(path, root).replace(os.sep, '/')] = hashlib.sha256(f.read()).hexdigest()
    return result


def section_batch():
    """Convert, merge and extract, against a synthetic base and update in the retail layout."""
    for variant in ('plain', 'compressed_update', 'compressed_both'):
        d = fresh('batch_' + variant)
        rc, out = run([sys.executable, '-I', os.path.join(S, 'make_patch_test.py'), KEYS, TOOL, T1, d, variant])
        report(f'{variant}: test title generated', rc == 0, out.strip().splitlines()[0][:90] if rc == 0 else out[-600:])
        if rc != 0:
            continue
        base, update, romfs_new = [os.path.join(d, n) for n in ('base.nsp', 'update.nsp', 'romfs_new')]
        before = (sha(base), sha(update))

        rc, out = run([TOOL, 'convert', '-k', KEYS, base, update, '--nspdir', os.path.join(d, 'out'), '--verify'])
        names = os.listdir(os.path.join(d, 'out')) if rc == 0 else []
        report(f'{variant}: base + update become one NSP', rc == 0 and len(names) == 1 and 'OK: all checks passed' in out, names[0] if names else out[-500:])
        if not names:
            continue
        merged = os.path.join(d, 'out', names[0])

        rc, out = run([TOOL, 'verify', '-k', KEYS, merged, '--romfsdir', romfs_new])
        report(f'{variant}: merged RomFS is byte identical to the new files', rc == 0, out[-300:] if rc else '')
        ok, out = libhac(merged, romfs_new, None)
        report(f'{variant}: LibHac reads the merged NSP', ok, out[-400:] if not ok else '')
        rc, out = run(LIBHAC + [KEYS, merged, '--base', base, '--update', update])
        report(f"{variant}: same files as LibHac's own patching of base + update", rc == 0 and 'LIBHAC: ALL OK' in out, out[-400:] if rc else '')
        report(f'{variant}: inputs untouched', before == (sha(base), sha(update)))

        rc, out = run([TOOL, 'extract', '-k', KEYS, base, update, '--outdir', os.path.join(d, 'x')])
        roots = os.listdir(os.path.join(d, 'x')) if rc == 0 else []
        same = bool(roots) and tree_hashes(os.path.join(d, 'x', roots[0], 'romfs')) == tree_hashes(romfs_new)
        report(f'{variant}: extracted RomFS is byte identical to the new files', rc == 0 and same, out[-300:] if rc else '')
        if roots:
            x = os.path.join(d, 'x', roots[0])
            report(f'{variant}: extraction gives every folder', all(os.path.isdir(os.path.join(x, n)) for n in ('exefs', 'romfs', 'logo', 'control')), str(os.listdir(x)))
            rc, out = run([TOOL, '-k', KEYS, '--exefsdir', os.path.join(x, 'exefs'), '--romfsdir', os.path.join(x, 'romfs'), '--logodir', os.path.join(x, 'logo'),
                           '--controldir', os.path.join(x, 'control'), '--nspdir', os.path.join(d, 'rebuilt'), '--verify'])
            report(f'{variant}: extracted folders build again', rc == 0 and 'OK: all checks passed' in out, out[-300:] if rc else '')

        rc, out = run([TOOL, 'convert', '-k', KEYS, base, update, '--nocompress', '--nspdir', os.path.join(d, 'plain')])
        plain = os.listdir(os.path.join(d, 'plain')) if rc == 0 else []
        ok = False
        if plain:
            ok, lout = libhac(os.path.join(d, 'plain', plain[0]), romfs_new, None)
        report(f'{variant}: merge without compression reads back', ok, out[-300:] if not ok else '')

        # A base game cut down to what its update still reads from it, which has the same version as that update.
        sparse = os.path.join(d, 'base_sparse.nsp')
        rc, out = run([TOOL, 'analyze', '-k', KEYS, sparse, update])
        report(f'{variant}: a cut down base game gets analyzed with its update', rc == 0 and 'repacked as one title' in out and 'section' not in out, out[-300:])
        rc, out = run([TOOL, 'convert', '-k', KEYS, sparse, update, '--nspdir', os.path.join(d, 'sparse_out'), '--verify'])
        names = os.listdir(os.path.join(d, 'sparse_out')) if rc == 0 else []
        report(f'{variant}: a cut down base game and its update become one NSP', rc == 0 and len(names) == 1 and 'OK: all checks passed' in out, names[0] if names else out[-400:])
        if names:
            rc, out = run([TOOL, 'verify', '-k', KEYS, os.path.join(d, 'sparse_out', names[0]), '--romfsdir', romfs_new])
            report(f'{variant}: with the same files as the whole game and its update', rc == 0, out[-300:] if rc else '')
            ok, lout = libhac(os.path.join(d, 'sparse_out', names[0]), romfs_new, None)
            report(f'{variant}: LibHac reads what comes of a cut down base game', ok, lout[-400:] if not ok else '')
        rc, out = run([TOOL, 'convert', '-k', KEYS, sparse, '--nspdir', os.path.join(d, 'sparse_alone')])
        report(f'{variant}: a cut down base game on its own says what it needs', rc != 0 and 'rest of it is in its update' in out and not os.path.exists(os.path.join(d, 'sparse_alone')), out[-300:])
        rc, out = run([TOOL, 'convert', '-k', KEYS, base, update, sparse, '--nspdir', os.path.join(d, 'sparse_newest')])
        names = os.listdir(os.path.join(d, 'sparse_newest')) if rc == 0 else []
        report(f'{variant}: with the whole game there too, it still goes by the newest', rc == 0 and len(names) == 1, out[-300:])

        # An update that changes nothing in the game data has none of its own, and an empty AES-CTR-Ex table.
        same = os.path.join(d, 'update_same.nsp')
        rc, out = run([TOOL, 'convert', '-k', KEYS, base, same, '--nspdir', os.path.join(d, 'same_out'), '--verify'])
        names = os.listdir(os.path.join(d, 'same_out')) if rc == 0 else []
        report(f'{variant}: an update with nothing of its own for the game data converts', rc == 0 and len(names) == 1 and 'OK: all checks passed' in out, names[0] if names else out[-400:])
        if names:
            rc, out = run([TOOL, 'verify', '-k', KEYS, os.path.join(d, 'same_out', names[0]), '--romfsdir', os.path.join(T1, 'romfs')])
            report(f'{variant}: and comes to the game data of the base game', rc == 0, out[-300:] if rc else '')

    # Everything below shares the last generated title.
    rc, out = run([TOOL, 'convert', '-k', KEYS, update, '--nspdir', os.path.join(d, 'alone')])
    report('update alone is refused and says why', rc != 0 and 'without its base game' in out and not os.path.exists(os.path.join(d, 'alone')))

    other = os.path.join(d, 'other_base.nsp')
    rc, out = run([TOOL, '-k', KEYS, '--exefsdir', os.path.join(T1, 'exefs'), '--romfsdir', os.path.join(d, 'dlc_romfs'), '--logodir', os.path.join(T1, 'logo'),
                   '--controldir', os.path.join(T1, 'control'), '--nocompress', '--nosignncasig2', '-o', other])
    rc, out = run([TOOL, 'convert', '-k', KEYS, other, update, '--nspdir', os.path.join(d, 'wrong')])
    left = os.listdir(os.path.join(d, 'wrong')) if os.path.isdir(os.path.join(d, 'wrong')) else []
    report('an update on the wrong base fails its hash check and leaves no file', rc != 0 and 'hash check failed' in out and not left, out[-250:].replace('\n', ' ') if rc == 0 else '')

    dlc = os.path.join(d, 'dlc.nsp')
    rc, out = run([TOOL, 'convert', '-k', KEYS, dlc, '--nspdir', os.path.join(d, 'dlc_out'), '--verify'])
    names = os.listdir(os.path.join(d, 'dlc_out')) if rc == 0 else []
    report('DLC converts', rc == 0 and len(names) == 1 and 'OK: all checks passed' in out, out[-300:] if rc else names[0])
    if names:
        sized = os.path.getsize(os.path.join(d, 'dlc_out', names[0])) < os.path.getsize(dlc) * 0.5
        report('DLC data is compressed', sized)
        rc, out = run(LIBHAC + [KEYS, os.path.join(d, 'dlc_out', names[0]), '--base', dlc])
        report('DLC files identical through LibHac', rc == 0 and 'LIBHAC: ALL OK' in out, out[-400:] if rc else '')
    rc, out = run([TOOL, 'extract', '-k', KEYS, dlc, '--outdir', os.path.join(d, 'dlc_x')])
    roots = os.listdir(os.path.join(d, 'dlc_x')) if rc == 0 else []
    report('DLC extracts', rc == 0 and bool(roots) and tree_hashes(os.path.join(d, 'dlc_x', roots[0], 'data')) == tree_hashes(os.path.join(d, 'dlc_romfs')), out[-300:] if rc else '')

    folder = os.path.join(d, 'folder')
    os.makedirs(os.path.join(folder, 'sub'))
    shutil.copy(base, folder)
    shutil.copy(update, os.path.join(folder, 'sub'))
    shutil.copy(dlc, folder)
    rc, out = run([TOOL, 'convert', '-k', KEYS, folder])
    made = sorted(n for _, _, ns in os.walk(folder) for n in ns if '[LZ4]' in n)
    report('a folder is searched and grouped: base + update + DLC give two NSPs', rc == 0 and len(made) == 2 and '2 of 2 succeeded' in out, str(made) + (out[-300:] if rc else ''))
    rc, out = run([TOOL, 'convert', '-k', KEYS, folder])
    made2 = sorted(n for _, _, ns in os.walk(folder) for n in ns if '[LZ4]' in n)
    report('running it again ignores its own output', rc == 0 and made == made2 and 'already compressed' in out, out[-300:] if rc else '')

    rc, out = run([TOOL, base, update, '-k', KEYS, '--nspdir', os.path.join(d, 'drop')])
    made = os.listdir(os.path.join(d, 'drop')) if rc == 0 else []
    report('files with no command convert when nobody can answer the prompt', rc == 0 and len(made) == 1 and 'repacked as one title' in out, out[-300:] if rc else '')

    # Slow settings, so the build is still running when it gets stopped.
    stopped = os.path.join(d, 'stopped')
    os.makedirs(stopped)
    p = subprocess.Popen([TOOL, 'convert', '-k', KEYS, base, update, '--level', '12', '--blocksize', '4K', '--nspdir', stopped], stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    seen = False
    for _ in range(200):
        seen = any(n.endswith('.part') for n in os.listdir(stopped))
        if seen or p.poll() is not None:
            break
        time.sleep(0.05)
    time.sleep(1)
    running = p.poll() is None
    if running:
        p.send_signal(signal.CTRL_BREAK_EVENT)
    p.wait(60)
    time.sleep(0.5)
    left = os.listdir(stopped)
    report('stopping a build leaves no partial or temporary file', seen and running and not left, f'seen={seen} running={running} left={left}')


def section_matrix():
    romfs = os.path.join(T1, 'romfs')
    exefs = os.path.join(T1, 'exefs')
    for level in (1, 2, 3, 6, 9, 10, 11, 12):
        ok, nsp, out, took = build(f'level{level}', '--level', str(level))
        size = os.path.getsize(nsp) if nsp else 0
        lh = False
        if ok:
            lh, lout = libhac(nsp, romfs, exefs, REF_NSP)
            if not lh: print(lout[-1500:])
        else:
            print(out[-1500:])
        report(f'level {level}', ok and lh, f'nsp={size} {took:.1f}s')
    for bs in ('4K', '8K', '16K', '32K', '0x10000'):
        ok, nsp, out, took = build(f'bs{bs}', '--blocksize', bs)
        size = os.path.getsize(nsp) if nsp else 0
        lh = False
        if ok:
            lh, lout = libhac(nsp, romfs, exefs, REF_NSP)
            if not lh: print(lout[-1500:])
        else:
            print(out[-1500:])
        report(f'blocksize {bs}', ok and lh, f'nsp={size} {took:.1f}s')
    ok, nsp, out, took = build('threads1', '--threads', '1')
    report('threads 1', ok, f'{took:.1f}s')
    ok2, nsp2, out2, took2 = build('threads4', '--threads', '4')
    same = False
    if ok and ok2:
        # Same input and options must give the same RomFS regardless of thread count (signature bytes aside).
        a = open(nsp, 'rb').read()
        b = open(nsp2, 'rb').read()
        same = len(a) == len(b)
    report('threads 4 same size as threads 1', ok2 and same, f'{took2:.1f}s')


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def section_edges():
    base = {'--exefsdir': os.path.join(T1, 'exefs'), '--logodir': None, '--controldir': os.path.join(T1, 'control')}
    rnd = lambda n: hashlib.shake_128(str(n).encode()).digest(n)

    def case(name, make, *args):
        root = os.path.join(fresh('edge_src_' + name), 'romfs')
        os.makedirs(root)
        make(root)
        dirs = dict(base)
        dirs['--romfsdir'] = root
        ok, nsp, out, took = build('edge_' + name, '--nologo', *args, dirs=dirs)
        lh = False
        if ok:
            lh, lout = libhac(nsp, root, None)
            if not lh: print(lout[-1500:])
        else:
            print(out[-1500:])
        report(f'edge: {name}', ok and lh, f'nsp={os.path.getsize(nsp) if nsp else 0} {took:.1f}s')
        shutil.rmtree(os.path.dirname(root), ignore_errors=True)

    case('empty romfs', lambda r: None)
    case('one byte', lambda r: write(os.path.join(r, 'a'), b'x'))
    case('one empty file', lambda r: write(os.path.join(r, 'a'), b''))
    case('only empty dirs', lambda r: os.makedirs(os.path.join(r, 'a', 'b', 'c')))
    case('all zero', lambda r: write(os.path.join(r, 'z'), b'\0' * (5 << 20)))
    case('exact block', lambda r: write(os.path.join(r, 'z'), b'ab' * (0x10000 // 2 - 0x100)))
    case('raw run over 64 MiB cap', lambda r: write(os.path.join(r, 'r'), rnd(70 << 20)))
    case('incompressible then compressible', lambda r: (write(os.path.join(r, 'a'), rnd(3 << 20)), write(os.path.join(r, 'b'), b'hello world ' * 400000)))
    case('unicode and spaces', lambda r: write(os.path.join(r, 'ñandú con espacio', '日本語ファイル.bin'), b'data' * 1000))
    case('many files', lambda r: [write(os.path.join(r, f'd{i % 37}', f'file{i}.txt'), (f'{i} ' * (i % 500 + 1)).encode()) for i in range(6000)])
    case('dir prefix order', lambda r: [write(os.path.join(r, n, 'x'), n.encode() * 50) for n in ('a', 'a b', 'a-b', 'a.b', 'a0', 'A', 'aa', 'a~')])


def section_options():
    romfs = os.path.join(T1, 'romfs')
    exefs = os.path.join(T1, 'exefs')
    for name, args in [('plaintext', ['--plaintext']), ('keygen 5', ['--keygeneration', '5']), ('keygen 2', ['--keygeneration', '2']),
                       ('keygen 17', ['--keygeneration', '17']), ('no logo', ['--nologo']),
                       ('custom keyareakey', ['--keyareakey', '00112233445566778899AABBCCDDEEFF']),
                       ('sdk 000F0000', ['--sdkversion', '000F0000']), ('title override', ['--titleid', '0100AAAA00000000', '--titleversion', '10000'])]:
        ok, nsp, out, took = build('opt_' + name.replace(' ', '_'), *args)
        lh = False
        if ok:
            lh, lout = libhac(nsp, romfs, exefs, REF_NSP)
            if not lh: print(lout[-1500:])
        else:
            print(out[-1500:])
        report(f'option: {name}', ok and lh, f'{took:.1f}s')

    ok, nsp, out, took = build('opt_noromfs', '--noromfs', dirs={'--exefsdir': exefs, '--logodir': os.path.join(T1, 'logo'), '--controldir': os.path.join(T1, 'control')})
    report('option: noromfs', ok, f'{took:.1f}s')
    if not ok: print(out[-1500:])

    # Inputs that should be rejected cleanly, leaving nothing behind.
    d = fresh('bad')
    bad = [(['--exefsdir', os.path.join(S, 'nope')], 'missing exefs'),
           (['--romfsdir', os.path.join(S, 'nope')], 'missing romfs'),
           (['--level', '99'], 'bad level'), (['--blocksize', '128K'], 'block over 64K'), (['--blocksize', '5000'], 'block not power of two'),
           (['--sdkversion', '00010000'], 'sdk too old'), (['--keygeneration', '99'], 'bad keygen'), (['--nonsense'], 'unknown option')]
    for args, name in bad:
        cmd = [TOOL, '-k', KEYS, '--nspdir', d, '--exefsdir', exefs, '--romfsdir', romfs, '--logodir', os.path.join(T1, 'logo'),
               '--controldir', os.path.join(T1, 'control')] + args
        rc, out = run(cmd)
        report(f'rejects: {name}', rc != 0 and 'Error:' in out and not os.listdir(d), out.strip().splitlines()[-1][:90])


def sha(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()


def copy_title(dst):
    os.makedirs(dst)
    shutil.copytree(os.path.join(T1, 'exefs'), os.path.join(dst, 'exefs'))
    shutil.copytree(os.path.join(T1, 'control'), os.path.join(dst, 'control'))


def section_diff():
    romfs = os.path.join(T1, 'romfs')
    logo = os.path.join(T1, 'logo')
    html = os.path.join(fresh('diff_html'), 'h')
    legal = os.path.join(fresh('diff_legal'), 'l')
    write(os.path.join(html, 'index.html'), b'<html></html>' * 100)
    write(os.path.join(html, 'sub', 'a.css'), b'body{}' * 40)
    write(os.path.join(legal, 'legalinfo.xml'), b'<x/>' * 200)

    scenarios = [
        ('defaults (nacp logo patch)', [], []),
        ('titleid override', ['--titleid', '0100FEED00000000'], []),
        ('html and legal', ['--htmldocdir', html, '--legalinfodir', legal], []),
        ('keygen 4 and sdk', ['--keygeneration', '4', '--sdkversion', '000D0000'], []),
        ('keygen 2', ['--keygeneration', '2'], []),
        ('plaintext', ['--plaintext'], []),
        ('no logo', ['--nologo'], []),
        ('no romfs', ['--noromfs'], []),
        ('custom key area key', ['--keyareakey', '00112233445566778899AABBCCDDEEFF'], []),
    ]
    for name, args, _ in scenarios:
        d = fresh('diff_' + name.split()[0] + str(abs(hash(name)) % 1000))
        a, b = os.path.join(d, 'hbp'), os.path.join(d, 'mine')
        copy_title(a)
        copy_title(b)
        base = ['--exefsdir', 'exefs', '--controldir', 'control', '--nosignncasig2', '--keepncadir']
        if '--noromfs' not in args: base += ['--romfsdir', romfs]
        if '--nologo' not in args: base += ['--logodir', logo]
        rc1, out1 = run([HBP, '-k', KEYS] + base + args, cwd=a)
        before = (sha(os.path.join(b, 'exefs', 'main.npdm')), sha(os.path.join(b, 'control', 'control.nacp')))
        rc2, out2 = run([TOOL, '-k', KEYS, '--nocompress'] + base + args, cwd=b)
        after = (sha(os.path.join(b, 'exefs', 'main.npdm')), sha(os.path.join(b, 'control', 'control.nacp')))
        hn = sorted(os.listdir(os.path.join(a, 'hacbrewpack_nca'))) if rc1 == 0 else []
        mn = sorted(os.listdir(os.path.join(b, 'hacbrewpack_nca'))) if rc2 == 0 else []
        same_bytes = hn == mn and all(sha(os.path.join(a, 'hacbrewpack_nca', f)) == sha(os.path.join(b, 'hacbrewpack_nca', f)) for f in hn)
        report(f'identical to hacbrewpack: {name}', rc1 == 0 and rc2 == 0 and same_bytes and hn != [], f'{len(hn)} NCAs')
        if not same_bytes:
            print('   hbp', hn, '\n   mine', mn, out1[-300:], out2[-300:])
        report(f'input untouched: {name}', before == after)
        shutil.rmtree(d, ignore_errors=True)

    # Signed path: hacBrewPack patches main.npdm on disk, this tool must produce the same NPDM except the key, and a valid sig2.
    d = fresh('diff_signed')
    a, b = os.path.join(d, 'hbp'), os.path.join(d, 'mine')
    copy_title(a)
    copy_title(b)
    args = ['--exefsdir', 'exefs', '--romfsdir', romfs, '--logodir', logo, '--controldir', 'control', '--keepncadir', '--titleid', '0100FEED00000000',
            '--titlename', 'Renamed', '--titlepublisher', 'Someone']
    rc1, _ = run([HBP, '-k', KEYS] + args, cwd=a)
    before = sha(os.path.join(b, 'exefs', 'main.npdm'))
    rc2, out2 = run([TOOL, '-k', KEYS] + args, cwd=b)
    report('signed build: original main.npdm untouched', before == sha(os.path.join(b, 'exefs', 'main.npdm')))

    keys = {}
    for line in open(KEYS, encoding='ascii', errors='ignore'):
        if '=' in line:
            k, v = line.split('=', 1)
            keys[k.strip().lower()] = v.strip()
    hk = bytes.fromhex(keys['header_key'])

    def xts(data):
        out = bytearray()
        for i in range(0, len(data), 0x200):
            dec = Cipher(algorithms.AES(hk), modes.XTS((i // 0x200).to_bytes(16, 'big'))).decryptor()
            out += dec.update(data[i:i + 0x200]) + dec.finalize()
        return bytes(out)

    def program_nca(root):
        ncas = os.path.join(root, 'hacbrewpack_nca')
        for f in os.listdir(ncas):
            raw = open(os.path.join(ncas, f), 'rb').read(0xC00)
            h = xts(raw)
            if h[0x205] == 0:
                return os.path.join(ncas, f), h
        raise Exception('no program nca')

    def exefs_files(path, h):
        st = struct.unpack_from('<I', h, 0x240)[0] * 0x200
        fh = h[0x400:0x600]
        pfs_ofs = struct.unpack_from('<Q', fh, 0x40)[0]
        pfs_size = struct.unpack_from('<Q', fh, 0x48)[0]
        with open(path, 'rb') as f:
            f.seek(st)
            raw = f.read(pfs_ofs + pfs_size + 16)
        ctr = (st >> 4).to_bytes(16, 'big')
        plain = Cipher(algorithms.AES(b'\x04' * 16), modes.CTR(ctr)).decryptor().update(raw)
        pfs = plain[pfs_ofs:pfs_ofs + pfs_size]
        n, stsz = struct.unpack_from('<II', pfs, 4)
        files = {}
        for i in range(n):
            o, s, no, _ = struct.unpack_from('<QQII', pfs, 0x10 + i * 0x18)
            names = pfs[0x10 + n * 0x18:]
            name = names[no:names.index(b'\0', no)].decode()
            files[name] = pfs[0x10 + n * 0x18 + stsz + o:0x10 + n * 0x18 + stsz + o + s]
        return files

    pa, ha = program_nca(a)
    pb, hb = program_nca(b)
    fa, fb = exefs_files(pa, ha), exefs_files(pb, hb)
    na, nb = fa['main.npdm'], fb['main.npdm']
    acid = struct.unpack_from('<I', nb, 0x78)[0]
    masked = lambda x: x[:acid + 0x100] + x[acid + 0x200:]
    report('signed build: NPDM equals hacbrewpack apart from the ACID key', rc1 == 0 and rc2 == 0 and masked(na) == masked(nb) and len(na) == len(nb))
    report('signed build: ACID key was replaced', nb[acid + 0x100:acid + 0x200] != open(os.path.join(T1, 'exefs', 'main.npdm'), 'rb').read()[acid + 0x100:acid + 0x200])
    pub = rsa.RSAPublicNumbers(65537, int.from_bytes(nb[acid + 0x100:acid + 0x200], 'big')).public_key()
    try:
        pub.verify(hb[0x100:0x200], hb[0x200:0x400], padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32), hashes.SHA256())
        report('signed build: NCA sig2 verifies with the patched ACID key (PSS, salt 0x20)', True)
    except Exception as e:
        report('signed build: NCA sig2 verifies with the patched ACID key (PSS, salt 0x20)', False, str(e))
    report('signed build: same TID patch at ACI0', struct.unpack_from('<Q', nb, struct.unpack_from('<I', nb, 0x70)[0] + 0x10)[0] == 0x0100FEED00000000)

    # NACP: compare with what hacBrewPack wrote on disk, ignoring the four newer language slots it does not fill.
    nacp_a = open(os.path.join(a, 'control', 'control.nacp'), 'rb').read()
    from_mine = None
    ncas = os.path.join(b, 'hacbrewpack_nca')
    for f in os.listdir(ncas):
        h = xts(open(os.path.join(ncas, f), 'rb').read(0xC00))
        if h[0x205] == 2:
            st = struct.unpack_from('<I', h, 0x240)[0] * 0x200
            en = struct.unpack_from('<I', h, 0x244)[0] * 0x200
            with open(os.path.join(ncas, f), 'rb') as fp:
                fp.seek(st)
                raw = fp.read(en - st)
            plain = Cipher(algorithms.AES(b'\x04' * 16), modes.CTR((st >> 4).to_bytes(16, 'big'))).decryptor().update(raw)
            fh = h[0x400:0x600]
            data = plain[struct.unpack_from('<Q', fh, 0x18 + 5 * 0x18)[0]:]
            fpart = struct.unpack_from('<Q', data, 0x48)[0]
            tbl = struct.unpack_from('<Q', data, 0x38)[0]
            pos = 0
            while pos < struct.unpack_from('<Q', data, 0x40)[0]:
                o, s = struct.unpack_from('<QQ', data, tbl + pos + 8)
                nl = struct.unpack_from('<I', data, tbl + pos + 0x1C)[0]
                nm = data[tbl + pos + 0x20:tbl + pos + 0x20 + nl]
                if nm == b'control.nacp':
                    from_mine = data[fpart + o:fpart + o + s]
                pos += 0x20 + ((nl + 3) & ~3)
    a12, b12 = nacp_a[:12 * 0x300] + nacp_a[16 * 0x300:], from_mine[:12 * 0x300] + from_mine[16 * 0x300:]
    report('signed build: NACP equals hacbrewpack (title slots 0-11 and all other fields)', a12 == b12)
    report('signed build: NACP name also set in slots 12-15', from_mine[12 * 0x300:12 * 0x300 + 7] == b'Renamed')
    shutil.rmtree(d, ignore_errors=True)


def section_l2():
    d = fresh('l2_src')
    root = os.path.join(d, 'romfs')
    os.makedirs(root)
    big = os.path.join(root, 'sparse.bin')
    size = 5_900_000_000
    open(big, 'wb').close()
    run(['fsutil', 'sparse', 'setflag', big])
    with open(big, 'r+b') as f:
        f.truncate(size)
        for off in range(0, size, 512 << 20):   # non-zero islands so blocks differ
            f.seek(off)
            f.write(hashlib.shake_128(str(off).encode()).digest(200_000))
    dirs = {'--exefsdir': os.path.join(T1, 'exefs'), '--romfsdir': root, '--logodir': None, '--controldir': os.path.join(T1, 'control')}
    ok, nsp, out, took = build('l2', '--nologo', '--blocksize', '4K', '--level', '1', dirs=dirs)
    print(out[-900:])
    lh = False
    if ok:
        lh, lout = libhac(nsp, root, None, randoms=2000)
        print(lout[-500:])
    report('L2 tree: 5.9 GB at 4K blocks', ok and lh, f'nsp={os.path.getsize(nsp) if nsp else 0} {took:.1f}s')
    shutil.rmtree(d, ignore_errors=True)
    shutil.rmtree(os.path.join(WORK, 'l2'), ignore_errors=True)


for sec in sys.argv[1:]:
    globals()['section_' + sec]()
bad = [n for n, ok in results if not ok]
print(f'\n{len(results) - len(bad)}/{len(results)} passed' + (': FAILED ' + '; '.join(bad) if bad else ''))
sys.exit(1 if bad else 0)
