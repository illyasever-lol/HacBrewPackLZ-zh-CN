"""Updating a compressed game, with a real game that has two updates.

    python tests\\update_chain.py <hacbrewpacklz.exe> <prod.keys> <base> <older update> <newer update> <work folder> [hbptool.exe]

1. The base with the older update makes a compressed copy, which keeps a map of the original in its control data.
2. That copy with the newer update alone makes the updated game.
3. The base with the newer update, made directly, is what 2 has to come to: the same game data, read back by LibHac (the code, for a game with no game data).
With hbptool, the Switch engine has to make files byte for byte the same as the PC's in 1 and 2.

Games whose makers compressed them, such as Hollow Knight: Silksong, need the original game for 2, which is checked to be said.
Needs about three times the games' size free in the work folder.
"""
import glob
import os
import shutil
import subprocess
import sys

# Nothing a test does may reach the real report service.
os.environ['HACBREWPACKLZ_REPORT_SITE'] = 'none'
os.environ['HBP_TEST_REPORT_SITE'] = 'none'
S = os.path.dirname(os.path.abspath(__file__))
LIBHAC = os.path.join(S, 'libhac_check', 'bin', 'Release', 'net10.0', 'libhac_check.exe')
failures = 0


def report(name, ok, extra=''):
    global failures
    if not ok:
        failures += 1
    print(('PASS' if ok else 'FAIL') + ': ' + name + (' - ' + extra if extra and not ok else ''))


def convert(tool, keys, inputs, out):
    os.makedirs(out, exist_ok=True)
    result = subprocess.run([tool, 'convert', '--nosignncasig2', '-k', keys, *inputs, '--nspdir', out], capture_output=True, text=True, errors='replace')
    made = glob.glob(os.path.join(out, '*.nsp'))
    return (made[0] if made else None), result.stdout + result.stderr


def game_data(keys, nsp, image):
    subprocess.run([LIBHAC, keys, nsp, '--dump-image', image], capture_output=True)
    return image if os.path.exists(image) else None


def same_file(a, b):
    with open(a, 'rb') as x, open(b, 'rb') as y:
        while True:
            p, q = x.read(1 << 24), y.read(1 << 24)
            if p != q:
                return False
            if not p:
                return True


def main():
    tool, keys, base, older, newer, work = [os.path.abspath(a) for a in sys.argv[1:7]]
    hbptool = os.path.abspath(sys.argv[7]) if len(sys.argv) > 7 else None
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    d = lambda name: os.path.join(work, name)

    first, said = convert(tool, keys, [base, older], d('1'))
    report('the base with the older update is compressed', first is not None, said[-500:])
    second, said = convert(tool, keys, [first, newer], d('2'))
    direct, _ = convert(tool, keys, [base, newer], d('3'))
    makers_compressed = second is None and 'needs parts of the original game' in said
    if makers_compressed:
        report("a game its makers compressed says the original is needed", 'original game' in said, said[-500:])
    else:
        report('the copy with the newer update alone makes the updated game', second is not None, said[-500:])
        a = game_data(keys, second, d('2.img'))
        b = game_data(keys, direct, d('3.img'))
        report('with the same game data as the original with the newer update', a and b and same_file(a, b))
        for image in (a, b):
            if image:
                os.remove(image)

    if hbptool:
        out = d('1 switch')
        os.makedirs(out)
        subprocess.run([hbptool, 'convert', '-k', keys, base, older, '--nspdir', out, '--nosignncasig2'], capture_output=True)
        made = glob.glob(os.path.join(out, '*.nsp'))
        report('the Switch engine makes the same copy, byte for byte', made and same_file(made[0], first))
        if second:
            out = d('2 switch')
            os.makedirs(out)
            subprocess.run([hbptool, 'convert', '-k', keys, first, newer, '--nspdir', out, '--nosignncasig2'], capture_output=True)
            made = glob.glob(os.path.join(out, '*.nsp'))
            report('and the same updated game', made and same_file(made[0], second))

    print(f'{failures} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
