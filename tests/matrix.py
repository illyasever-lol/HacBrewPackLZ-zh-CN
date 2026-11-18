#!/usr/bin/env python3
"""End-to-end test matrix. Prints no key material. See README.md. usage: matrix.py <section> [...]
Sections: prepare batch analyze selection titles copies names extracting folders formats zstd engine diff options edges matrix l2. "prepare" must have run once before the others.
The engine section also needs HBPTOOL, the PC build of the Switch version's engine: see switch/README.md."""
import hashlib, os, re, shutil, signal, struct, subprocess, sys, time
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from cryptography.hazmat.primitives import hashes

# Nothing a test does may reach the real report service.
os.environ['HACBREWPACKLZ_REPORT_SITE'] = 'none'
os.environ['HBP_TEST_REPORT_SITE'] = 'none'
S = os.path.dirname(os.path.abspath(__file__))
OUT = os.environ.get('TESTDIR', os.path.join(S, 'out'))
T1 = os.path.join(OUT, 't1')
WORK = os.path.join(OUT, 'work')
TOOL = os.environ.get('TOOL', os.path.join(S, '..', '..', '_tools', 'hacbrewpacklz', 'hacbrewpacklz.exe'))
HBP = os.environ.get('HACBREWPACK', os.path.join(S, '..', '..', '_tools', 'hacbrewpack', 'hacbrewpack.exe'))
KEYS = os.environ.get('KEYS', os.path.join(os.path.dirname(HBP), 'prod.keys'))
LIBHAC = ['dotnet', os.path.join(S, 'libhac_check', 'bin', 'Release', 'net10.0', 'libhac_check.dll')]
REF_NSP = os.path.join(T1, 'hbp_nosig', 'hacbrewpack_nsp', '0100abcd12340000.nsp')
results = []


def report(name, ok, detail=''):
    results.append((name, ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}  {detail}", flush=True)


def run(cmd, cwd=None, env=None):
    p = subprocess.run(cmd, cwd=cwd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace',
                       env=None if env is None else {**os.environ, **env})
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

    # A game with copies of its own files, which the estimate sees from the file table, and files it mustn't take for copies. Without the
    # copies it would come to over 7 MB more than it does, and with the language versions or the unrelated files taken for copies, megabytes
    # less: each engine against what it makes itself.
    d = fresh('analyze_copies')
    rc, out = run([sys.executable, '-I', os.path.join(S, 'make_patch_test.py'), KEYS, TOOL, T1, d, 'copies'])
    if rc != 0:
        report('copies: test title generated', False, out[-300:])
    else:
        base, update = os.path.join(d, 'base.nsp'), os.path.join(d, 'update.nsp')
        engine = os.environ.get('HBPTOOL')
        for who, tool in [('this tool', TOOL)] + ([('the Switch engine', engine)] if engine and os.path.exists(engine) else []):
            for name, inputs in (('the game alone', [base]), ('with its update', [base, update])):
                rc, out = run([tool, 'analyze', '-k', KEYS, *inputs])
                target = os.path.join(d, 'out_' + str(abs(hash((who, name)))))
                shutil.rmtree(target, ignore_errors=True)
                rc2, out2 = run([tool, 'convert', '-k', KEYS, *inputs, '--nspdir', target])
                made = os.listdir(target) if rc2 == 0 else []
                if rc != 0 or len(made) != 1:
                    report(f'copies, {who}, {name}: analyze and convert', False, (out + out2)[-300:])
                    continue
                check_estimate(f'copies estimate, {who}: {name}', out, os.path.getsize(os.path.join(target, made[0])), False)

        # Measured, a game under 1 GB is measured exactly, every block compressed: each engine comes to what it makes, with no margin, and
        # both to the same figure.
        target = os.path.join(d, 'out_measured')
        shutil.rmtree(target, ignore_errors=True)
        rc, out = run([TOOL, 'convert', '-k', KEYS, base, update, '--nspdir', target])
        made = os.listdir(target) if rc == 0 else []
        figures = {}
        for who, cmd in [('this tool', [TOOL, 'analyze', '-k', KEYS, base, update, '--measure', '--minsaving', '99'])] + \
                        ([('the Switch engine', [engine, 'measure', '-k', KEYS, base, update])] if engine and os.path.exists(engine) else []):
            rc, out = run(cmd)
            figures[who] = estimated(out)
            if len(made) == 1:
                check_estimate(f'measured exactly, {who}', out, os.path.getsize(os.path.join(target, made[0])), True)
        report('measured exactly: both engines come to the same figure', len(set(figures.values())) == 1, str(figures))

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

    # What a first look can't vouch for is left out, saying --measure finds out, and with it, what's measured to save too little is left out
    # saying what it saves.
    rc, out, made = outputs('--minsaving', '20')
    report('a minimum leaves out what saves too little or isn\'t sure to', rc == 0 and made == ['0100AAAA00000000'], f'{made} {out[-200:]}')
    report('and says so, with what --measure does and a count', 'not sure yet, may save more: --measure reads it in full' in out and '2 left out as not worth it' in out
           and '1 of 1 succeeded' in out, out[-300:])
    rc, out, made = outputs('--minsaving', '20', '--measure')
    report('with --measure, what\'s measured to save too little is left out too', rc == 0 and made == ['0100AAAA00000000'], f'{made} {out[-200:]}')
    # Measured, what grows by less than compressing ever makes anything grow is as small as it gets, not "would grow by 0%".
    report('and says what it saves', "nothing to gain: it's already as small as it gets" in out and 'would grow by 0%' not in out and 'Measured' in out
           and '2 left out as not worth it' in out, out[-300:])

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
    # What would grow a hair by a first look isn't left out on its say: --measure reads it in full.
    hint = [line.strip() for line in out.splitlines() if 'convert --only' in line]
    # A game with no figure says "not sure" where its figure would be; one with a figure says it after its name.
    report('and gives the command for only the ones sure to be worth it', hint == ["To compress only what's sure to be worth it: convert --only 0100AAAA00000000"]
           and any('incompressible [0100BBBB00000000]' in line and 'not sure' in line for line in ranking.splitlines()) and 'add --measure' in out, ranking[-400:])
    rc, out = run([TOOL, 'analyze', '-k', KEYS, *games, '--minsaving', '20', '--measure'])
    ranking = out[out.find('Most worthwhile first'):]
    report('analyze --measure reads what isn\'t sure in full and says what it saves', rc == 0 and 'Measured' in out and 'not sure' not in ranking, ranking[-400:])

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
    # Built uncompressed, as a game from the shop is, or converting it would leave it out as already compressed.
    rc, out = run([TOOL, '-k', KEYS, '--nspdir', target, '--nocompress'] + dirs)
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

    def compare(d, name, inputs, *args, files=None):
        a, b = os.path.join(d, 'cs'), os.path.join(d, 'cpp')
        shutil.rmtree(a, ignore_errors=True)
        shutil.rmtree(b, ignore_errors=True)
        rc1, out1 = run([TOOL, 'convert', '-k', KEYS, *inputs, '--nspdir', a, '--nosignncasig2', *args])
        rc2, out2 = run([engine, 'convert', '-k', KEYS, *inputs, '--nspdir', b, '--nosignncasig2', *args])
        names = (sorted(os.listdir(a)) if rc1 == 0 and os.path.isdir(a) else None, sorted(os.listdir(b)) if rc2 == 0 and os.path.isdir(b) else None)
        same = rc1 == 0 and rc2 == 0 and names[0] == names[1] and nsp_hashes(a) == nsp_hashes(b)
        ok = same and len(names[0]) == (files or len(names[0])) > 0
        report(f'{name}: the same bytes', ok, '' if ok else f'{rc1} {rc2} {names} {out2[-300:]}')

    # The last is a game that holds some of its own data again, which both have to find the same way.
    went_back_somewhere = set()
    for variant in ('plain', 'compressed_update', 'compressed_both', 'repeats'):
        d = fresh('engine_' + variant)
        rc, out = run([sys.executable, '-I', os.path.join(S, 'make_patch_test.py'), KEYS, TOOL, T1, d, variant])
        if rc != 0:
            report(f'{variant}: test title generated', False, out[-300:])
            continue
        base, update, dlc, romfs_new = [os.path.join(d, n) for n in ('base.nsp', 'update.nsp', 'dlc.nsp', 'romfs_new')]

        compare(d, f'{variant}, NSPs', [base, update, dlc])
        if variant == 'repeats':
            compare(d, f'{variant}, the base game alone', [base])
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

        # A solid game and update read out of order, kept aside each way the console keeps them, come to the same bytes, and leave no copy
        # behind. With no section kept as it goes past, the update goes back as a big update does. Read again every time, nothing is kept;
        # as it comes, a copy is made only once it has gone back more than three times, which a test game may or may not do; kept from the
        # first time back, it goes in memory, in a file, or both. A game read in order has nothing to keep.
        for kind in ('solid', 'xcz-solid'):
            if kind not in forms:
                continue
            ways = (('read again', {'HBP_TEST_REWINDS_BEFORE_KEEPING': '1000000'}, ('temporary copy', 'kept in memory'), ()),
                    ('as it comes', {}, ('kept in memory',), ()),
                    ('in memory', {'HBP_TEST_SPARE_MEMORY': str(1 << 30)}, ('temporary copy',), ('kept in memory',)),
                    ('its start in memory, the rest in a file', {'HBP_TEST_SPARE_MEMORY': str(1 << 20), 'HBP_TEST_REWINDS_BEFORE_KEEPING': '0'}, (),
                     ('MB in memory, and a temporary copy of the rest',)),
                    ('in a file', {'HBP_TEST_REWINDS_BEFORE_KEEPING': '0'}, ('kept in memory',), ('a temporary copy of up to',)))
            made = {}
            went_back = True
            for name, env, never, said in ways:
                target, temp = os.path.join(d, 'keep_out'), os.path.join(d, 'keep_temp')
                shutil.rmtree(target, ignore_errors=True)
                shutil.rmtree(temp, ignore_errors=True)
                os.makedirs(temp)
                rc, out = run([engine, 'convert', '-k', KEYS, *forms[kind], '--nspdir', target, '--tempdir', temp, '--nosignncasig2'],
                              env={'HBP_TEST_MOST_KEPT_SECTION': '1', **env})
                made[name] = nsp_hashes(target) if rc == 0 else None
                left = os.listdir(temp)
                # Nothing is kept of a stream that never had to go back.
                if name == 'read again':
                    went_back = 'decoded from its start again' in out
                    print(f'      {variant}, {kind}: ' + ('goes back' if went_back else 'read in order, so there is nothing to keep'))
                    if went_back:
                        went_back_somewhere.add(kind)
                if not went_back:
                    never, said = ('temporary copy', 'kept in memory'), ()
                report(f'{variant}, {kind}, kept {name}: made, saying so, and no copy left',
                       rc == 0 and made[name] and not left and all(s in out for s in said) and not any(n in out for n in never), f'{rc} {left} {out[-300:]}')
            report(f'{variant}, {kind}: the same bytes however it is kept', len(set(map(str, made.values()))) == 1 and made['read again'], str(made)[:300])

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
    for kind in ('solid', 'xcz-solid'):
        report(f'{kind}: some test game goes back, so each way of keeping it is tried', kind in went_back_somewhere, str(sorted(went_back_somewhere)))

    # A game and its DLC in one file, with the smaller DLC given first, so both have to put them in title ID order.
    d = fresh('engine_dlcs')
    rc, out = run([sys.executable, '-I', os.path.join(S, 'make_patch_test.py'), KEYS, TOOL, T1, d, 'dlcs'])
    if rc != 0:
        report('dlcs: test titles generated', False, out[-300:])
    inputs = [os.path.join(d, n + '.nsp') for n in ('dlc_small', 'base', 'update', 'dlc')]
    compare(d, 'dlcs, a game and its DLC in one file', inputs, files=1)
    compare(d, 'dlcs, each DLC in a file of its own, none left out', inputs, '--separatedlc', '--minsaving', '5', files=3)
    compare(d, 'dlcs, a merged file', [os.path.join(d, 'merged.nsp')], files=1)
    compare(d, 'dlcs, DLC without their game in one file', inputs[:1] + inputs[3:], files=1)
    compare(d, 'dlcs, and with --separatedlc in two', inputs[:1] + inputs[3:], '--separatedlc', files=2)

    # A copy made again, here with a DLC its file doesn't hold. It keeps the map of what its update kept of the original, so a newer update can
    # still be made part of it.
    copy = os.path.join(d, 'copy')
    rc, out = run([TOOL, 'convert', '-k', KEYS, inputs[1], inputs[2], '--nspdir', copy, '--nosignncasig2'])
    copies = [os.path.join(copy, f) for f in os.listdir(copy)] if rc == 0 else []
    if len(copies) == 1:
        compare(d, 'dlcs, a copy made again with a DLC', [copies[0], inputs[0]], files=1)
        made = [os.path.join(d, 'cs', f) for f in os.listdir(os.path.join(d, 'cs'))] if os.path.isdir(os.path.join(d, 'cs')) else []
        x = os.path.join(d, 'x_again')
        rc, out = run([TOOL, 'extract', '-k', KEYS, *made[:1], '--outdir', x])
        kept = [os.path.join(r, f) for r, _, fs in os.walk(x) for f in fs if f == 'hacbrewpacklz_update_map.txt']
        report('dlcs: a copy made again keeps its update map', len(made) == 1 and len(kept) == 1, f'{made} {kept} {out[-200:]}')
        rc, out = run([TOOL, 'convert', '-k', KEYS, *made[:1], os.path.join(d, 'update_v2.nsp'), '--nspdir', os.path.join(d, 'v2'), '--nosignncasig2'])
        report('dlcs: and takes a newer update', rc == 0 and 'made part of it' in out and len(os.listdir(os.path.join(d, 'v2'))) == 1, out[-300:])
    else:
        report('dlcs: a copy of the game and its update is made', False, out[-300:])
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
    check = os.path.join(S, 'zstd_check', 'bin', 'Release', 'net10.0', 'zstd_check.dll')
    rc, out = run(['dotnet', check, corpus])
    report('every frame decodes to exactly what went in', rc == 0 and 'ZSTD: ALL OK' in out, out.strip().splitlines()[-1][:120] if rc == 0 else out[-500:])
    rc, out = run(['dotnet', check, corpus, '--fuzz', '40'])
    report('damaged copies are refused or decoded, with no crash', rc == 0 and 'ZSTD: ALL OK' in out, '' if rc == 0 else f'exit {rc} ' + out[-400:])
    shutil.rmtree(corpus, ignore_errors=True)


def nsp_hashes(folder):
    return sorted(sha(os.path.join(folder, f)) for f in os.listdir(folder) if f.endswith('.nsp'))


def section_formats():
    """Whatever form a game arrives in, the same NSP has to come out, and the same files."""
    # Each DLC in a file of its own, so each file can be compared with the estimate for its title.
    fixed = ['--nosignncasig2', '--separatedlc']
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
            # An NSZ or XCZ is counted at what it takes installed, which installing unpacks it to, while what's made of it installs as it is.
            packed = 'NSZ' in name or 'XCZ' in name
            for mode, args in (('compressed', fixed), ('plain', fixed + ['--nocompress'])):
                target = os.path.join(d, 'out')
                shutil.rmtree(target, ignore_errors=True)
                rc, out = run([TOOL, 'convert', '-k', KEYS, *inputs, '--nspdir', target, '--verify'] + args)
                same = rc == 0 and nsp_hashes(target) == reference[mode]
                report(f'{variant}: {name} gives the same NSPs, {mode}', same and out.count('OK: all checks passed') == 2, out[-400:] if not same else '')
                if mode == 'compressed':
                    said = 'installed as it is' in out
                    report(f'{variant}: {name} sums up against {"what it takes installed" if packed else "the file"}', rc == 0 and said == packed,
                           '\n'.join(line for line in out.splitlines() if line.startswith('Size:')))

            rc, out = run([TOOL, 'analyze', '-k', KEYS, *inputs, '--nocompress'])
            target_sizes = sorted(int(m) for m in re.findall(r'In bytes:\s+(\d+) \+/- 0', out))
            plain_target = os.path.join(d, 'plain_sizes')
            shutil.rmtree(plain_target, ignore_errors=True)
            run([TOOL, 'convert', '-k', KEYS, *inputs, '--nspdir', plain_target, '--nocompress'] + fixed)
            real_sizes = sorted(os.path.getsize(os.path.join(plain_target, f)) for f in os.listdir(plain_target))
            report(f'{variant}: {name} estimate is exact without compression', rc == 0 and target_sizes[:2] == real_sizes, f'{target_sizes} {real_sizes}')

            rc, out = run([TOOL, 'analyze', '-k', KEYS, *inputs])
            report(f'{variant}: {name} can be analyzed', rc == 0 and 'In bytes:' in out, out[-300:] if rc else '')
            report(f'{variant}: {name} analyzed {"says what its files come to, unpacked when installed" if packed else "has nothing to say about packing"}',
                   rc == 0 and ('As a file:' in out) == packed, '\n'.join(line for line in out.splitlines() if 'Now:' in line or 'As a file:' in line))

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
        report(f'{variant}: a cut down base game gets analyzed with its update', rc == 0 and 'with its update' in out and 'section' not in out, out[-300:])
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
    report('a folder is searched and grouped: base + update + DLC give one NSP', rc == 0 and len(made) == 1 and '2 of 2 succeeded' in out, str(made) + (out[-300:] if rc else ''))
    rc, out = run([TOOL, 'convert', '-k', KEYS, folder])
    made2 = sorted(n for _, _, ns in os.walk(folder) for n in ns if '[LZ4]' in n)
    report('running it again ignores its own output', rc == 0 and made == made2 and 'already compressed' in out, out[-300:] if rc else '')

    rc, out = run([TOOL, base, update, '-k', KEYS, '--nspdir', os.path.join(d, 'drop')])
    made = os.listdir(os.path.join(d, 'drop')) if rc == 0 else []
    # These are the last variant's files, where the base is a compressed copy: known by what's in it, whatever its name, it's said to be
    # made one title with its update rather than repacked as one.
    report('files with no command convert when nobody can answer the prompt', rc == 0 and len(made) == 1 and 'with its update' in out,
           out[-300:])

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

    # A game's DLC goes into its new file, or each into a file of its own with --separatedlc, and once the game's file is made none is left
    # out. The smaller DLC is given first, so the order in the file has to come from the title IDs.
    d = fresh('batch_dlcs')
    rc, out = run([sys.executable, '-I', os.path.join(S, 'make_patch_test.py'), KEYS, TOOL, T1, d, 'dlcs'])
    report('dlcs: test titles generated', rc == 0, out[-600:] if rc else '')
    base, update, dlc, small, merged, small2 = [os.path.join(d, n + '.nsp') for n in ('base', 'update', 'dlc', 'dlc_small', 'merged', 'dlc_small2')]

    def made(name, *args, inputs=(small, base, update, dlc), env=None):
        target = os.path.join(d, name)
        rc, out = run([TOOL, 'convert', '-k', KEYS, *inputs, '--nspdir', target, '--nosignncasig2', *args], env=env)
        return rc, out, sorted(os.path.join(target, f) for f in os.listdir(target)) if os.path.isdir(target) else []

    def whole(nsp, dlcs):
        """check reads all of it back, and details counts its DLC."""
        rc1, out1 = run([TOOL, 'check', '-k', KEYS, nsp])
        rc2, out2 = run([TOOL, 'details', '-k', KEYS, nsp])
        said = re.search(r'^\s*DLC\s+(\S+)', out2, re.M)
        return rc1 == 0 and 'every game read back as it should' in out1 and rc2 == 0 and said is not None and said.group(1) == str(dlcs), out1[-200:] + out2[-200:]

    rc, out, together = made('together')
    ok, detail = whole(together[0], 2) if rc == 0 and len(together) == 1 else (False, out[-400:])
    report("dlcs: a game's DLC goes into its file, which reads back whole", ok and 'In the game\'s file' in out, f'{together} {detail}')
    rc, out, apart = made('apart', '--separatedlc', '--minsaving', '5')
    names = [os.path.basename(p) for p in apart]
    report('dlcs: with --separatedlc each DLC gets its own file, even one that saves nothing', rc == 0 and len(apart) == 3 and any(n.startswith('dlc_small') for n in names),
           f'{names} {out[-300:]}')
    if len(together) == 1 and len(apart) == 3:
        game = next(p for p in apart if not os.path.basename(p).startswith('dlc'))
        each = [game, next(p for p in apart if os.path.basename(p).startswith('dlc [')), next(p for p in apart if os.path.basename(p).startswith('dlc_small'))]
        same = nsp_names(together[0]) == [n for p in each for n in nsp_names(p)]
        report("dlcs: the game's file holds each title as its own file does, then its DLC in title ID order", same,
               '' if same else f'{nsp_names(together[0])} {[nsp_names(p) for p in each]}')

    rc, out, one = made('merged', inputs=(merged,))
    ok, detail = whole(one[0], 2) if rc == 0 and len(one) == 1 else (False, out[-400:])
    report('dlcs: a merged file comes out as one file with all its DLC', ok, f'{one} {detail}')
    rc, out, three = made('merged_apart', '--separatedlc', inputs=(merged,))
    report('dlcs: and with --separatedlc as three', rc == 0 and len(three) == 3, f'{three} {out[-300:]}')

    # DLC without their game go together into one file, named as the one with the lowest title ID would be on its own. The smaller is given
    # first, so the order comes from the title IDs. With --separatedlc, each gets a file.
    def metas(nsp):
        return sum(n.endswith('.cnmt.nca') for n in nsp_names(nsp))

    def checked(nsp):
        rc, out = run([TOOL, 'check', '-k', KEYS, nsp])
        return rc == 0 and 'every game read back as it should' in out

    rc, out, alone = made('alone', inputs=(small, dlc))
    names = [os.path.basename(p) for p in alone]
    report('dlcs: DLC without their game go into one file, named as the first DLC, which reads back whole',
           rc == 0 and names == ['dlc [LZ4].nsp'] and metas(alone[0]) == 2 and checked(alone[0]) and 'In one file with the other DLC' in out, f'{names} {out[-300:]}')
    rc, out, apart = made('alone_apart', '--separatedlc', inputs=(small, dlc))
    report('dlcs: and with --separatedlc as two', rc == 0 and len(apart) == 2, f'{apart} {out[-300:]}')
    if len(alone) == 1:
        folder = os.path.join(d, 'alone_folder')
        os.makedirs(folder)
        for each in (small, dlc, alone[0]):
            shutil.copy(each, folder)
        rc, out = run([TOOL, 'convert', '-k', KEYS, folder, '--nosignncasig2'])
        report('dlcs: and that file given with them again makes nothing new', rc == 0 and len(os.listdir(folder)) == 3, f'{os.listdir(folder)} {out[-300:]}')

    # Copies this tool made. One of the game and its update made without DLC, given with merged.nsp, which holds the original and both DLC: the
    # original is used, and the new file holds the game and both DLC.
    rc, out, copies = made('copy', inputs=(base, update))
    copy = copies[0] if rc == 0 and len(copies) == 1 else None
    report('dlcs: a copy of the game and its update is made', copy is not None, out[-300:])
    if copy:
        rc, out, one = made('merged_copy', inputs=(merged, copy))
        ok, detail = whole(one[0], 2) if rc == 0 and len(one) == 1 else (False, out[-400:])
        report('dlcs: a copy without the DLC its original holds gives way to the original, and the new file holds them all', ok, f'{one} {detail}')
        # The same copy with a DLC and no original: it's made again with the DLC in its file, and a second time nothing new is made.
        folder = os.path.join(d, 'copy_folder')
        os.makedirs(folder)
        for each in (copy, small):
            shutil.copy(each, folder)
        rc1, out1 = run([TOOL, 'convert', '-k', KEYS, folder, '--nosignncasig2'])
        first = sorted(os.listdir(folder))
        new = [os.path.join(folder, n) for n in first if n not in (os.path.basename(copy), 'dlc_small.nsp')]
        ok, detail = whole(new[0], 1) if rc1 == 0 and len(new) == 1 else (False, out1[-400:])
        report('dlcs: a copy given with a DLC not in its file is made again with it', ok and "Its DLC isn't in its file" in out1, f'{first} {detail}')
        rc2, out2 = run([TOOL, 'convert', '-k', KEYS, folder, '--nosignncasig2'])
        report('dlcs: and converting that folder again makes nothing new', rc1 == 0 and rc2 == 0 and sorted(os.listdir(folder)) == first,
               f'{sorted(os.listdir(folder))} {out2[-300:]}')
        # Given with the original base game, which is older than it, and a DLC: the copy is newer, so it's made again with the DLC.
        rc, out, one = made('copy_base', inputs=(copy, base, small))
        ok, detail = whole(one[0], 1) if rc == 0 and len(one) == 1 else (False, out[-400:])
        report('dlcs: a copy newer than the original given beside it is made again with its DLC', ok and "Its DLC isn't in its file" in out, f'{one} {detail}')
        # Given with its original and each DLC in a file of its own, as 1.5.0 left them: the copy is kept and made again from itself with them,
        # rather than the original compressed again.
        rc, out, one = made('copy_original_loose', inputs=(copy, base, update, dlc, small))
        ok, detail = whole(one[0], 2) if rc == 0 and len(one) == 1 else (False, out[-400:])
        report('dlcs: a copy given with its original and its DLC in files of their own is kept, and made again from itself with them',
               ok and "Its DLC isn't in its file" in out and "Compressed from 'base.nsp' and 'update.nsp'" in out, f'{one} {detail}')
        # With --separatedlc, a DLC with nothing to compress whose only copy is inside the merged original gets a file of its own beside the
        # copy, with a minimum saving or without, and given that file, a second run makes nothing new.
        for minimum in ('off', '5'):
            folder = os.path.join(d, f'copy_merged_apart_{minimum}')
            os.makedirs(folder)
            shutil.copy(copy, folder)
            again = [TOOL, 'convert', '-k', KEYS, merged, folder, '--nspdir', folder, '--nosignncasig2', '--separatedlc', '--minsaving', minimum]
            rc1, out1 = run(again)
            first = sorted(os.listdir(folder))
            rc2, out2 = run(again)
            report(f'dlcs: with --separatedlc and minimum {minimum}, a DLC only inside the merged original gets a file of its own beside the copy',
                   rc1 == 0 and len(first) == 3 and any('0100ABCD12341002' in n for n in first), f'{first} {out1[-400:]}')
            report(f'dlcs: and with minimum {minimum}, a second run makes nothing new', rc2 == 0 and sorted(os.listdir(folder)) == first,
                   f'{sorted(os.listdir(folder))} {out2[-300:]}')

    # The same twice for things that would otherwise be made again on every run: DLC without their game that have nothing to compress, which
    # go into one file as they are, and a DLC that can't be made, as its file lacks an NCA its content meta lists.
    def twice(name, files, wanted):
        folder = os.path.join(d, name)
        os.makedirs(folder)
        for each in files:
            if isinstance(each, tuple):
                open(os.path.join(folder, each[0]), 'wb').write(each[1])
            else:
                shutil.copy(each, folder)
        rc1, out1 = run([TOOL, 'convert', '-k', KEYS, folder, '--nosignncasig2'])
        first = sorted(os.listdir(folder))
        rc2, out2 = run([TOOL, 'convert', '-k', KEYS, folder, '--nosignncasig2'])
        return len(first) == wanted and sorted(os.listdir(folder)) == first, f'{first} {sorted(os.listdir(folder))} {out1[-200:]} {out2[-300:]}', out1

    ok, detail, out = twice('small_folder', (small, small2), 3)
    report('dlcs: DLC without their game and with nothing to compress go into one file, and a second run makes nothing new', ok, detail)
    ok, detail, out = twice('broken_folder', (base, update, dlc, ('dlc_small.nsp', without_tickets(small, keep=lambda n: n.endswith('.cnmt.nca')))), 5)
    report("dlcs: a DLC that can't be made is said to be, and a second run makes nothing new", ok and 'is missing' in out, detail)

    rc, out, older = made('older', inputs=(dlc,))
    if rc == 0 and len(older) == 1:
        rc, out, one = made('with_older', inputs=(base, update, older[0]))
        ok, detail = whole(one[0], 1) if rc == 0 and len(one) == 1 else (False, out[-400:])
        report('dlcs: a DLC compressed before goes into the game\'s file, which reads back whole', ok, f'{one} {detail}')
    else:
        report('dlcs: a DLC compressed before goes into the game\'s file, which reads back whole', False, out[-300:])

    # A disk with room for the game but not its DLC as well. Without compressing, the size is known exactly, and 64 MB is kept free on top.
    def needs(*inputs):
        rc, out = run([TOOL, 'analyze', '-k', KEYS, *inputs, '--nocompress', '--nosignncasig2'])
        return sum(int(n) + int(m) for n, m in re.findall(r'In bytes:\s+(\d+) \+/- (\d+)', out)[:len(inputs)])
    free = needs(base, update) + (64 << 20) + needs(dlc, small) // 2
    rc, out, left = made('room', '--nocompress', env={'HACBREWPACKLZ_TEST_FREE_SPACE': str(free)})
    report("dlcs: with no room for the game and its DLC, it says so before anything is written, and what to do",
           rc != 0 and 'It needs about' in out and "Turning off Game and DLC in one file (--separatedlc) makes it smaller." in out, out[-300:])
    report("dlcs: and its DLC aren't made on their own in the same run",
           not left and out.count("Left out: its game's file couldn't be made") == 2, f'{left} {out[-300:]}')
    rc, out, alone = made('room_alone', '--nocompress', inputs=(base, update), env={'HACBREWPACKLZ_TEST_FREE_SPACE': str(free)})
    report('dlcs: and the game alone fits', rc == 0 and len(alone) == 1, out[-300:])

    # A damaged DLC stops the game's file too, and the error under the game names the DLC.
    bad = os.path.join(d, 'damaged', 'dlc.nsp')
    os.makedirs(os.path.dirname(bad))
    data = bytearray(open(dlc, 'rb').read())
    data[len(data) - 30000] ^= 0xFF
    open(bad, 'wb').write(data)
    rc, out, left = made('damaged_out', inputs=(base, update, bad))
    first = re.sub(r'[0-9A-Fa-f]{32}', '<id>', next((line for line in out.splitlines() if line.startswith('FAILED')), ''))
    report("dlcs: a damaged DLC is named as the DLC, not the game", rc != 0 and "Its DLC 0100ABCD12341001 in 'dlc.nsp'" in first and not left, f'{first[:200]} {left}')
    failed = [line for line in out.splitlines() if line.startswith('FAILED')]
    report("dlcs: and counts as one failure, as it isn't tried again on its own", len(failed) == 1 and "Left out: its game's file couldn't be made" in out,
           f'{len(failed)} failed {out[-300:]}')

    # The same folder again makes nothing new: the game is this tool's copy, the DLC that compressed is in it, and the one with nothing to
    # compress is left out.
    folder = os.path.join(d, 'folder')
    os.makedirs(folder)
    for each in (base, update, dlc, small):
        shutil.copy(each, folder)
    rc1, out1 = run([TOOL, 'convert', '-k', KEYS, folder, '--nosignncasig2'])
    first = sorted(os.listdir(folder))
    rc2, out2 = run([TOOL, 'convert', '-k', KEYS, folder, '--nosignncasig2'])
    again = sorted(os.listdir(folder))
    report('dlcs: converting the same folder again makes nothing new', rc1 == 0 and rc2 == 0 and len(first) == 5 and again == first and 'nothing to gain' in out2,
           f'{first} {again} {out2[-300:]}')

    # Slow settings, so the DLC is still being written when the stop comes, just after the game's content meta.
    stopped = os.path.join(d, 'stopped')
    os.makedirs(stopped)
    p = subprocess.Popen([TOOL, 'convert', '-k', KEYS, base, update, dlc, '--level', '12', '--blocksize', '4K', '--nspdir', stopped], stdin=subprocess.DEVNULL,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace', creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    seen = False
    for line in p.stdout:
        if 'Writing Meta NCA: done' in line:
            seen = True
            p.send_signal(signal.CTRL_BREAK_EVENT)
            break
    p.communicate(timeout=60)
    time.sleep(0.5)
    left = os.listdir(stopped)
    report('dlcs: stopping while the DLC is written leaves nothing, not even the game', seen and p.returncode != 0 and not left, f'seen={seen} rc={p.returncode} left={left}')
    shutil.rmtree(d, ignore_errors=True)


def nsp_names(path):
    """The files in an NSP, in the order its header lists them."""
    with open(path, 'rb') as f:
        count, table = struct.unpack('<II', f.read(0x10)[4:12])
        header = f.read(count * 0x18 + table)
    names = header[count * 0x18:]
    return [names[at:names.index(b'\0', at)].decode() for at in (struct.unpack_from('<I', header, i * 0x18 + 0x10)[0] for i in range(count))]


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


def without_tickets(path, keep=lambda name: not name.endswith(('.tik', '.cert'))):
    """An NSP's files less its tickets and certificates, as an NSP: an update that can't be read for want of its title key. keep says which
    files stay, otherwise."""
    with open(path, 'rb') as f:
        data = f.read()
    count, size = struct.unpack_from('<II', data, 4)
    strings = data[0x10 + count * 0x18:0x10 + count * 0x18 + size]
    start = 0x10 + count * 0x18 + size
    files = []
    for i in range(count):
        offset, length, name_at, _ = struct.unpack_from('<QQII', data, 0x10 + i * 0x18)
        name = strings[name_at:strings.index(b'\0', name_at)].decode()
        if keep(name):
            files.append((name, data[start + offset:start + offset + length]))
    names = bytearray()
    places = []
    for name, _ in files:
        places.append(len(names))
        names += name.encode() + b'\0'
    names += b'\0' * (-len(names) % 0x20)
    out = bytearray(b'PFS0' + struct.pack('<III', len(files), len(names), 0))
    position = 0
    for (name, body), at in zip(files, places):
        out += struct.pack('<QQII', position, len(body), at, 0)
        position += len(body)
    out += names
    for _, body in files:
        out += body
    return bytes(out)


def section_names():
    """What's saved never goes over a file that's there: converted where something has the name it would get, such as an original game named
    the way this names what it makes, a game comes out as "... (2).nsp", then "... (3).nsp", and what was there stays as it was."""
    d = fresh('names')
    rc, out = run([sys.executable, '-I', os.path.join(S, 'make_patch_test.py'), KEYS, TOOL, T1, d, 'plain'])
    if rc != 0:
        report('names: test title generated', False, out[-300:])
        return
    base = os.path.join(d, 'base.nsp')
    target = os.path.join(d, 'out')

    def convert():
        rc, out = run([TOOL, 'convert', '-k', KEYS, base, '--nspdir', target, '--nosignncasig2'])
        return rc, out, sorted(f for f in os.listdir(target) if f.endswith('.nsp')) if os.path.isdir(target) else []

    rc, out, first = convert()
    report('names: converted once', rc == 0 and len(first) == 1, out[-300:])
    if len(first) != 1:
        return
    # Something else of that name where it goes. What it's made from is in another folder, so nothing there is one of its inputs.
    taken = os.path.join(target, first[0])
    write(taken, b"somebody else's file")
    stem = first[0][:-4]
    rc, out, second = convert()
    report("names: converted beside a file of that name, it's saved as '... (2).nsp'", rc == 0 and second == sorted([first[0], stem + ' (2).nsp']), str(second))
    rc, out, third = convert()
    report("names: and again, as '... (3).nsp'", rc == 0 and third == sorted([first[0], stem + ' (2).nsp', stem + ' (3).nsp']), str(third))
    with open(taken, 'rb') as f:
        report('names: and the file that was there is as it was', f.read() == b"somebody else's file")
    shutil.rmtree(d, ignore_errors=True)


def section_extracting():
    """Extracting never leaves half a game that looks like a whole one: a folder an extraction is going on in says so in a note until all of
    it's out, one that fails or is stopped is deleted, one that's extracted already isn't done over, one that didn't finish is cleared and done
    afresh, and a folder of somebody else's of that name is left alone."""
    d = fresh('extracting')
    rc, out = run([sys.executable, '-I', os.path.join(S, 'make_patch_test.py'), KEYS, TOOL, T1, d, 'plain'])
    if rc != 0:
        report('extracting: test title generated', False, out[-300:])
        return
    base = os.path.join(d, 'base.nsp')
    target = os.path.join(d, 'out')
    note = 'Not finished extracting.txt'

    def extract(nsp):
        rc, out = run([TOOL, 'extract', '-k', KEYS, nsp, '--outdir', target])
        return rc, out, sorted(os.listdir(target)) if os.path.isdir(target) else []

    rc, out, folders = extract(base)
    game = os.path.join(target, folders[0]) if len(folders) == 1 else None
    report('extracting: a game is extracted, with no note left in it', rc == 0 and game is not None and os.path.isdir(os.path.join(game, 'exefs')) and
           not os.path.exists(os.path.join(game, note)), out[-300:])
    if game is None:
        return
    files = tree_hashes(game)
    rc, out, _ = extract(base)
    report("extracting: again over it, it says it's extracted already, and leaves it as it was", rc != 0 and 'extracted already' in out and tree_hashes(game) == files, out[-300:])

    # What an extraction that didn't finish leaves: cleared away, and done afresh.
    write(os.path.join(game, note), b'not finished')
    write(os.path.join(game, 'romfs', 'left over.bin'), b'from before')
    rc, out, _ = extract(base)
    report("extracting: a folder one didn't finish in is cleared and done afresh", rc == 0 and tree_hashes(game) == files, out[-300:])

    # Somebody else's folder of that name, with things in it, is left alone.
    shutil.rmtree(game)
    write(os.path.join(game, 'notes.txt'), b'mine')
    rc, out, _ = extract(base)
    report("extracting: a folder of that name with other things in it is left alone", rc != 0 and 'other things in it' in out and
           sorted(os.listdir(game)) == ['notes.txt'], out[-300:])
    shutil.rmtree(game)

    # A link back to a folder it's in, in a game folder's romfs: building it says so, rather than going round until the program falls over.
    rc, out, _ = extract(base)
    loop = os.path.join(game, 'romfs', 'loop')
    subprocess.run(['cmd', '/c', 'mklink', '/J', loop, os.path.join(game, 'romfs')], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if rc == 0 and os.path.isdir(loop):
        args = [TOOL, '-k', KEYS, '--exefsdir', os.path.join(game, 'exefs'), '--romfsdir', os.path.join(game, 'romfs'), '--controldir', os.path.join(game, 'control'),
                '--nspdir', os.path.join(d, 'built'), '--nosignncasig2']
        args += ['--logodir', os.path.join(game, 'logo')] if os.path.isdir(os.path.join(game, 'logo')) else ['--nologo']
        rc, out = run(args)
        report("extracting: a link back to a folder it's in, in a game folder's romfs, is said, rather than going round for ever", rc != 0 and 'link back' in out, out[-300:])
        os.rmdir(loop)
    else:
        report('extracting: a link to a folder could be made for the test', False, out[-300:])
    shutil.rmtree(game, ignore_errors=True)

    # A game whose data is damaged part of the way through: whatever came out before goes.
    damaged = os.path.join(d, 'damaged.nsp')
    data = bytearray(open(base, 'rb').read())
    for at in range(len(data) * 3 // 5, len(data) * 3 // 5 + 0x4000):
        data[at] ^= 0xA5
    write(damaged, bytes(data))
    rc, out, folders = extract(damaged)
    report('extracting: a game that fails part of the way through leaves nothing behind', rc != 0 and folders == [], f'{folders} {out[-300:]}')
    shutil.rmtree(d, ignore_errors=True)


def section_folders():
    """The command line takes game folders as the screens do: one given, or found in a folder given, is listed, analyzed and built, named as
    the screens name it, and what's in it isn't taken for games of their own."""
    d = fresh('folders')
    rc, out = run([sys.executable, '-I', os.path.join(S, 'make_patch_test.py'), KEYS, TOOL, T1, d, 'plain'])
    if rc != 0:
        report('folders: test title generated', False, out[-300:])
        return
    base = os.path.join(d, 'base.nsp')
    games = os.path.join(d, 'games')
    rc, out = run([TOOL, 'extract', '-k', KEYS, base, '--outdir', games])
    names = os.listdir(games) if rc == 0 and os.path.isdir(games) else []
    folder = os.path.join(games, names[0]) if len(names) == 1 else None
    report('folders: a game extracted to a folder', folder is not None, out[-300:])
    if folder is None:
        return
    rc, out = run([TOOL, 'analyze', '-k', KEYS, games])
    report('folders: analyzing the folder it is in lists it as a game folder, with what it comes to', rc == 0 and 'Found 0 title(s) and 1 game folder(s)' in out and
           'game folder, v0' in out, out[-400:])
    built = os.path.join(d, 'built')
    rc, out = run([TOOL, 'convert', '-k', KEYS, games, '--nspdir', built, '--nosignncasig2'])
    made = sorted(f for f in os.listdir(built) if f.endswith('.nsp')) if os.path.isdir(built) else []
    report('folders: converting the folder it is in builds it, named as the screens name it', rc == 0 and len(made) == 1 and made[0].endswith('[0100ABCD12340000][v0][LZ4].nsp'),
           f'{made} {out[-300:]}')
    if len(made) == 1:
        x = os.path.join(d, 'x')
        rc, _ = run([TOOL, 'extract', '-k', KEYS, os.path.join(built, made[0]), '--outdir', x])
        again = os.path.join(x, os.listdir(x)[0]) if rc == 0 and os.path.isdir(x) and len(os.listdir(x)) == 1 else None
        report("folders: and it holds all of the folder's game data", again is not None and tree_hashes(os.path.join(again, 'romfs')) == tree_hashes(os.path.join(folder, 'romfs')))
    rc, out = run([TOOL, 'convert', '-k', KEYS, folder, '--nspdir', built, '--nosignncasig2'])
    made = sorted(f for f in os.listdir(built) if f.endswith('.nsp')) if os.path.isdir(built) else []
    report('folders: the game folder given itself is built too, beside the first under a free name', rc == 0 and len(made) == 2 and any(f.endswith('[LZ4] (2).nsp') for f in made),
           str(made))
    rc, out = run([TOOL, 'extract', '-k', KEYS, games, '--outdir', os.path.join(d, 'x2')])
    report("folders: extracting leaves a game folder out, as it's extracted already", 'extracted already' in out and not os.path.isdir(os.path.join(d, 'x2')), out[-300:])
    shutil.rmtree(d, ignore_errors=True)


def section_copies():
    """A compressed copy given with a newer update: made one title with it when the update fits, and otherwise listed by itself, with the
    update saying why, which is only that the copy is missing parts of the original game when that's so. Each other update given with a copy
    is noted once. A game repacked
    with compression off takes a newer update too, from the map it was made with."""
    d = fresh('copies_with_updates')
    rc, out = run([sys.executable, '-I', os.path.join(S, 'make_patch_test.py'), KEYS, TOOL, T1, d, 'plain'])
    if rc != 0:
        report('copies: test title generated', False, out[-300:])
        return
    base, update, same = [os.path.join(d, n) for n in ('base.nsp', 'update.nsp', 'update_same.nsp')]

    def made(name, inputs, *args):
        target = os.path.join(d, name)
        rc, out = run([TOOL, 'convert', '-k', KEYS, *inputs, '--nspdir', target, '--nosignncasig2', *args])
        files = [os.path.join(target, f) for f in os.listdir(target) if f.endswith('.nsp')] if os.path.isdir(target) else []
        return (files[0] if rc == 0 and len(files) == 1 else None), out

    def game_data(nsp, name):
        """The files of its game data, extracted, by their hashes."""
        target = os.path.join(d, 'x_' + name)
        shutil.rmtree(target, ignore_errors=True)
        rc, _ = run([TOOL, 'extract', '-k', KEYS, nsp, '--outdir', target])
        folders = [os.path.join(target, f) for f in os.listdir(target)] if rc == 0 and os.path.isdir(target) else []
        return tree_hashes(os.path.join(folders[0], 'romfs')) if len(folders) == 1 else None

    copy, out = made('copy', [base])
    report('copies: a compressed copy of the game alone', copy is not None, out[-300:])
    merged, out = made('merged', [base, update])
    report('copies: and one with its update made part of it', merged is not None, out[-300:])
    if not copy or not merged:
        return

    _, out = made('together', [copy, update])
    report('copies: the copy and a newer update are made one title', 'is used in place of the original game' in out, out[-500:])

    # The same update without its ticket can't be read for want of its title key, which is what it says.
    noticket = os.path.join(d, 'update_noticket.nsp')
    write(noticket, without_tickets(update))
    _, out = made('noticket', [copy, noticket])
    report("copies: an update that can't be read says why, rather than that the copy is missing parts of the original game",
           "can't be made part of the compressed game" in out and 'title key' in out and 'needs parts of the original game' not in out, out[-600:])
    report('copies: and the copy is still listed by itself', 'already compressed' in out, out[-600:])

    # A copy with the update in it already, given with that update and another of the same version: each noted once.
    _, out = made('notes', [merged, update, same])
    report('copies: each update given with a copy is noted once', out.count("'update.nsp'") == 1 and out.count("'update_same.nsp'") == 1, out[-700:])

    # A copy with its update in it takes a newer one through the map it was made with. One that reads parts of the original the earlier update
    # replaced says the copy doesn't have them, and to give the original game: the earlier update's file has nothing the map hasn't.
    _, out = made('merged_newer', [merged, os.path.join(d, 'update_v2.nsp')])
    report('copies: a copy with its update in it and a newer update are made one title', 'is used in place of the original game' in out, out[-500:])
    _, out = made('merged_all', [merged, os.path.join(d, 'update_same_v2.nsp')])
    report('copies: a newer update needing parts the copy lacks says so, and to give the original game',
           'needs parts of the original game that the compressed game' in out and 'Give the original game along with it' in out
           and "that update's file" not in out, out[-500:])

    # A game repacked with compression off, with its update in it, and a newer one, as on the Switch: the newer update reads the parts of the
    # original the earlier one kept where the game has them, from the map it was made with, and the game data comes out as from the original.
    # One that reads parts of the original the earlier update replaced can't be made from it, and says it needs the original game.
    repacked, out = made('repacked', [base, update], '--nocompress')
    report('copies: a game repacked with its update, compression off', repacked is not None, out[-300:])
    if repacked:
        newer = os.path.join(d, 'update_v2.nsp')
        together, out = made('repacked_newer', [repacked, newer])
        report('copies: the repacked game and a newer update are made one title', together is not None, out[-500:])
        direct, out2 = made('original_newer', [base, newer])
        if together and direct:
            mine, theirs = game_data(together, 'repacked'), game_data(direct, 'original')
            report('copies: with the same game data as the original game with that update', mine is not None and mine == theirs,
                   f'{len(mine or {})} and {len(theirs or {})} files')
        else:
            report('copies: the original game with the newer update, to compare with', direct is not None, out2[-300:])
        _, out = made('repacked_all', [repacked, os.path.join(d, 'update_same_v2.nsp')])
        report('copies: a newer update reading what the earlier one replaced says the repacked game lacks part of the original game',
               'a part of the original game that the repacked game' in out and 'damaged' not in out, out[-500:])

    # The Switch's engine makes the same files from each of them, byte for byte, as it does from an original game and its update.
    engine = os.environ.get('HBPTOOL')
    if engine and os.path.exists(engine):
        newer = os.path.join(d, 'update_v2.nsp')
        cases = [('a compressed copy and a newer update', [copy, update]), ('a copy with its update in it and a newer one', [merged, newer])]
        if repacked:
            cases.append(('a repacked game and a newer update', [repacked, newer]))
        for i, (name, inputs) in enumerate(cases):
            a, b = os.path.join(d, f'pc_{i}'), os.path.join(d, f'switch_{i}')
            rc1, _ = run([TOOL, 'convert', '-k', KEYS, *inputs, '--nspdir', a, '--nosignncasig2'])
            rc2, out2 = run([engine, 'convert', '-k', KEYS, *inputs, '--nspdir', b, '--nosignncasig2'])
            made_here = sorted(os.listdir(a)) if rc1 == 0 and os.path.isdir(a) else []
            same = rc2 == 0 and len(made_here) == 1 and made_here == sorted(os.listdir(b)) and nsp_hashes(a) == nsp_hashes(b)
            report(f'copies: the Switch engine makes the same from {name}', same, out2[-300:])
    else:
        report('HBPTOOL points at the engine', False, str(engine))
    shutil.rmtree(d, ignore_errors=True)


for sec in sys.argv[1:]:
    globals()['section_' + sec]()
bad = [n for n, ok in results if not ok]
print(f'\n{len(results) - len(bad)}/{len(results)} passed' + (': FAILED ' + '; '.join(bad) if bad else ''))
sys.exit(1 if bad else 0)
