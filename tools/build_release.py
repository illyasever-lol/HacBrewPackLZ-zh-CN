"""Builds the seven programs of a release from committed code, and proves each one is that version. Step 4 of a release.

    python tools/build_release.py <version> <folder> [--tree <folder>] [--replace]
    python tools/build_release.py --verify <folder> [--public <public key.pem>] [--tree <folder>]

The first builds, into <folder>, under the names publish_release.py uploads:
    hacbrewpacklz.exe, hacbrewpacklz-gui.exe     Windows, native
    hacbrewpacklz-linux-x64, hacbrewpacklz-macos-x64, hacbrewpacklz-macos-arm64    with .NET inside
    hacBrewPackLZ.nro                             the Switch app (its .elf is kept in the round folder, for crash addresses)
    hacbrewpacklz-android.apk                     signed with the project's Android key, from local.json
It refuses code with changes not committed, or whose version isn't <version> (python tools/set_version.py sets it), and a folder that
already holds any of the seven, unless --replace. Then each file has to say <version>, and the Android file has to be signed with the
project's own certificate, the one every release so far has had. Last, the Linux program is run in WSL (the distribution local.json's
linux_wsl names) on the patch test's base game, and its NSP has to be the Windows program's, byte for byte. With no linux_wsl, or a
WSL that can't start it, that check says skipped, never passed.

The two update files, hacbrewpacklz-update.txt and .sig, aren't made here: publish_release.py signs the seven with the update key (step 5).

--verify checks a folder's hacbrewpacklz-update.sig against its hacbrewpacklz-update.txt, and that text's SHA-256 of each file against the
files there. It uses the public key built into the programs (src/Updater.cs) unless --public names another one, as for a practice
signature made with a throwaway key. It never prints a hash or a key.

Builds and logs go in <round>/release-<version>, beside the round's other folders. Key files are passed to apksigner by path, never opened.
"""
import argparse
import filecmp
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import this_pc  # noqa: E402
from this_pc import safe  # noqa: E402

# The certificate every hacBrewPackLZ APK has been signed with (SHA-256 of it, as apksigner prints it). It's public: anyone can read it
# from an APK. A different one means the keystore isn't the project's, and phones would refuse the APK as an update.
ANDROID_CERT_SHA256 = '4401439d5cdef70382c40b8681c626f6c9ad7406d0456068e316c0b9a9394c85'

FILES = ['hacbrewpacklz.exe', 'hacbrewpacklz-gui.exe', 'hacbrewpacklz-linux-x64', 'hacbrewpacklz-macos-x64', 'hacbrewpacklz-macos-arm64',
         'hacBrewPackLZ.nro', 'hacbrewpacklz-android.apk']
OTHERS = [('linux-x64', 'hacbrewpacklz-linux-x64'), ('osx-x64', 'hacbrewpacklz-macos-x64'), ('osx-arm64', 'hacbrewpacklz-macos-arm64')]


def say(line=''):
    print(safe(line), flush=True)


def git(tree, *args):
    return subprocess.run(['git', '-c', 'core.fsmonitor=false', '-C', tree, *args], capture_output=True, text=True).stdout


def msys(path):
    path = os.path.abspath(path).replace('\\', '/')
    return '/' + path[0].lower() + path[2:] if re.match(r'^[A-Za-z]:', path) else path


def wsl(path):
    return '/mnt' + msys(path)


def tree_version(tree):
    with open(os.path.join(tree, 'src', 'Ui.cs'), encoding='utf-8') as source:
        found = re.search(r'public const string Version = "([^"]*)"', source.read())
    return found.group(1) if found else None


def contains(path, text, utf16=True):
    with open(path, 'rb') as source:
        data = source.read()
    return text.encode('ascii') in data or (utf16 and text.encode('utf-16-le') in data)


class Builder:
    def __init__(self, args, local):
        self.args = args
        self.local = local
        self.tree = os.path.normpath(os.path.abspath(args.tree or this_pc.TREE))
        self.version = args.version
        self.dest = os.path.normpath(os.path.abspath(args.folder))
        self.work = os.path.join(os.path.normpath(local['round']), f'release-{self.version}')
        self.logs = os.path.join(self.work, 'logs')
        self.env = dict(os.environ)
        self.env['PATH'] = local['vswhere'] + os.pathsep + self.env.get('PATH', '')
        self.env['JAVA_HOME'] = local['jdk']
        self.rows = []
        self.linux = 'skipped: not built'
        self.ok = True

    def run(self, name, cmd, env=None, cwd=None):
        os.makedirs(self.logs, exist_ok=True)
        log = os.path.join(self.logs, re.sub(r'[^A-Za-z0-9]+', '_', name).strip('_').lower() + '.log')
        start = time.time()
        with open(log, 'wb') as out:
            code = subprocess.run(cmd, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, cwd=cwd or self.tree,
                                  env=env or self.env).returncode
        say(f"{'built' if code == 0 else 'FAILED'}  {name} ({int(time.time() - start)} s)" + ('' if code == 0 else f', log: {log}'))
        if code != 0:
            self.ok = False
            with open(log, encoding='utf-8', errors='replace') as source:
                lines = [l.strip() for l in source if re.search(r'\berror\b|error:', l, re.I) and not re.search(r'\b0 error', l, re.I)]
            for line in list(dict.fromkeys(lines))[:3]:
                say('        ' + line[:200])
        return code == 0

    def path(self, *parts):
        return os.path.join(self.work, *parts)

    def refuse_if_running(self):
        prefix = (self.work.rstrip('\\') + '\\').replace("'", "''")
        script = (f"Get-Process | Where-Object {{ $_.Path -and $_.Path.StartsWith('{prefix}', 'OrdinalIgnoreCase') }} | "
                  "ForEach-Object { $_.Path }")
        out = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command', script], capture_output=True, text=True).stdout
        running = [l.strip() for l in out.splitlines() if l.strip()]
        if running:
            sys.exit('These are running from the build folder, so it\'s left alone: ' + ', '.join(running))

    def note(self, name, said_version, how, extra=''):
        if not said_version:
            self.ok = False
        self.rows.append((name, said_version, how, extra))

    def build(self):
        tools = os.path.join(self.local['android_sdk'], 'build-tools', '35.0.0')
        os.makedirs(self.dest, exist_ok=True)

        # The Switch app.
        env = dict(self.env)
        env['MSYSTEM'] = 'MSYS'
        env.pop('MSYS_NO_PATHCONV', None)
        if self.run('Switch app', [self.local['devkitpro_bash'], '-lc', f"cd '{msys(self.tree)}/switch' && make -j4"], env=env):
            nro = os.path.join(self.tree, 'switch', 'hacBrewPackLZ.nro')
            shutil.copy2(nro, os.path.join(self.dest, 'hacBrewPackLZ.nro'))
            elf = os.path.join(self.tree, 'switch', 'hacBrewPackLZ.elf')
            if os.path.exists(elf):
                shutil.copy2(elf, self.path(f'hacBrewPackLZ-{self.version}.elf'))
            self.note('hacBrewPackLZ.nro', contains(nro, self.version, utf16=False), 'in the file')

        # Windows, native.
        win = self.path('win')
        if self.run('Windows app', ['dotnet', 'publish', os.path.join('gui', 'hacbrewpacklz-gui.csproj'), '-c', 'Release', '-r', 'win-x64',
                                    '-o', win, '--artifacts-path', self.path('artifacts_gui')]):
            gui = os.path.join(win, 'hacbrewpacklz-gui.exe')
            shutil.copy2(gui, self.dest)
            self.note('hacbrewpacklz-gui.exe', contains(gui, self.version), 'in the file')
        if self.run('command line and home screen, Windows', ['dotnet', 'publish', 'hacbrewpacklz.csproj', '-c', 'Release', '-r', 'win-x64',
                                                              '-o', win, '--artifacts-path', self.path('artifacts_pc')]):
            exe = os.path.join(win, 'hacbrewpacklz.exe')
            shutil.copy2(exe, self.dest)
            said = subprocess.run([exe, '--version'], capture_output=True, text=True, stdin=subprocess.DEVNULL).stdout.strip().splitlines()
            self.note('hacbrewpacklz.exe', bool(said) and self.version in said[0], 'its --version')

        # Linux and Mac, with .NET inside. Their program is compressed in the bundle, so the version is looked for in it before bundling.
        for rid, name in OTHERS:
            out = self.path(f'rel_{rid}')
            artifacts = self.path(f'artifacts_{rid}')
            if self.run(f'command line, {rid}', ['dotnet', 'publish', 'hacbrewpacklz.csproj', '-c', 'Release', '-r', rid, '-o', out,
                                                  '--artifacts-path', artifacts, '-p:PublishAot=false', '-p:SelfContained=true',
                                                  '-p:PublishSingleFile=true', '-p:PublishTrimmed=true',
                                                  '-p:EnableCompressionInSingleFile=true']):
                shutil.copy2(os.path.join(out, 'hacbrewpacklz'), os.path.join(self.dest, name))
                inside = sorted(glob.glob(os.path.join(artifacts, 'obj', '**', 'linked', 'hacbrewpacklz.dll'), recursive=True),
                                key=os.path.getmtime)
                self.note(name, bool(inside) and contains(inside[-1], self.version), 'in its program, before bundling')

        # Android, signed with the project's key.
        artifacts = self.path('artifacts_android')
        if self.run('Android', ['dotnet', 'build', os.path.join('android', 'hacbrewpacklz-android.csproj'), '-c', 'Release',
                                f"-p:AndroidSdkDirectory={self.local['android_sdk']}", f"-p:JavaSdkDirectory={self.local['jdk']}",
                                '--artifacts-path', artifacts]):
            built = glob.glob(os.path.join(artifacts, 'bin', '**', '*.hacbrewpacklz.apk'), recursive=True)
            built = [p for p in built if not p.endswith('-Signed.apk')]
            aligned, signed = self.path('android_aligned.apk'), self.path('hacbrewpacklz-android.apk')
            apksigner = os.path.join(tools, 'apksigner.bat')
            password = 'file:' + self.local['android_keystore_pass']
            if built and self.run('Android: aligned', [os.path.join(tools, 'zipalign.exe'), '-f', '-p', '4', built[0], aligned]) and \
                    self.run('Android: signed', ['cmd', '/c', apksigner, 'sign', '--ks', self.local['android_keystore'],
                                                 '--ks-key-alias', self.local['android_key_alias'], '--ks-pass', password,
                                                 '--key-pass', password, '--out', signed, aligned]):
                os.remove(aligned)
                shutil.copy2(signed, self.dest)
                certs = subprocess.run(['cmd', '/c', apksigner, 'verify', '--print-certs', signed], capture_output=True, text=True).stdout
                found = re.findall(r'certificate SHA-256 digest: ([0-9a-f]+)', certs)
                same = found == [ANDROID_CERT_SHA256]
                if not same:
                    self.ok = False
                badging = subprocess.run([os.path.join(tools, 'aapt2.exe'), 'dump', 'badging', signed], capture_output=True, text=True).stdout
                name = re.search(r"versionName='([^']*)'", badging)
                self.note('hacbrewpacklz-android.apk', bool(name) and name.group(1) == self.version, 'its versionName',
                          'the project\'s certificate' if same else 'NOT the project\'s certificate')
            elif not built:
                self.ok = False
                say('FAILED  Android: no APK came out of the build')

        self.linux = self.check_linux()

    def check_linux(self):
        """Runs the Linux program in WSL on the patch test's base game: its NSP has to be the Windows program's, byte for byte."""
        distro = self.local.get('linux_wsl')
        exe, linux = os.path.join(self.dest, 'hacbrewpacklz.exe'), os.path.join(self.dest, 'hacbrewpacklz-linux-x64')
        game, keys = (self.local.get('checker_inputs') or [None])[0], self.local.get('keys')
        if not (distro and game and keys):
            return 'skipped: local.json names no linux_wsl, checker_inputs or keys'
        if not (os.path.exists(exe) and os.path.exists(linux)):
            return "skipped: the Windows or Linux program wasn't built"
        if subprocess.run(['wsl.exe', '-d', distro, '--', 'true'], capture_output=True, stdin=subprocess.DEVNULL).returncode != 0:
            return f"skipped: WSL couldn't start {distro}"
        keys = os.path.join(keys, 'prod.keys')
        os.makedirs(self.logs, exist_ok=True)
        outs = []
        for side, start, path in (('windows', [exe], os.path.abspath), ('linux', ['wsl.exe', '-d', distro, '--', wsl(linux)], wsl)):
            out = self.path('linux_check', side)
            shutil.rmtree(out, ignore_errors=True)
            os.makedirs(out)
            # Its output names NCAs in hex, so it goes to a log, not the screen.
            with open(os.path.join(self.logs, f'linux_check_{side}.log'), 'wb') as log:
                code = subprocess.run([*start, 'convert', '-k', path(keys), path(game), '--nspdir', path(out), '--nosignncasig2'],
                                      stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL).returncode
            if code != 0:
                self.ok = False
                return f'FAILED: the {side} program stopped with {code}, log in {self.logs}'
            outs.append(out)
        made = sorted(os.listdir(outs[0]))
        same = made == sorted(os.listdir(outs[1])) and any(n.endswith('.nsp') for n in made) and \
            all(filecmp.cmp(os.path.join(outs[0], n), os.path.join(outs[1], n), shallow=False) for n in made)
        self.ok = self.ok and same
        return f"{'the same' if same else 'NOT the same'} NSP as the Windows program, byte for byte (WSL {distro})"

    def report(self):
        say()
        width = max(len(f) for f in FILES)
        for name in FILES:
            row = next((r for r in self.rows if r[0] == name), None)
            path = os.path.join(self.dest, name)
            size = f'{os.path.getsize(path) / 1e6:6.1f} MB' if os.path.exists(path) and row else '  missing'
            if row:
                said = f"says {self.version}" if row[1] else f"DOESN'T say {self.version}"
                say(f"{name.ljust(width)}  {size}  {said} ({row[2]}){', ' + row[3] if row[3] else ''}")
            else:
                self.ok = False
                say(f'{name.ljust(width)}  {size}')
        say(f'Linux program run in WSL: {self.linux}')
        say()
        if self.ok:
            say(f'All seven are in {self.dest}. Next (step 5): python tools/publish_release.py <update key> {self.version} '
                f'{self.dest} <whats-new.md>, which signs them and writes the notes.')
        else:
            say(f'Not all of it worked: see above, and the logs in {self.logs}.')
        return 0 if self.ok else 1


def verify(folder, public, tree=None):
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding, rsa

    if public:
        with open(public, 'rb') as source:
            key = serialization.load_pem_public_key(source.read())
        whose = os.path.basename(public)
    else:
        with open(os.path.join(tree or this_pc.TREE, 'src', 'Updater.cs'), encoding='utf-8') as source:
            modulus = re.search(r'PublicModulus = Convert\.FromHexString\("([0-9a-fA-F]+)"\)', source.read())
        if not modulus:
            sys.exit("Couldn't find the public key in src/Updater.cs.")
        key = rsa.RSAPublicNumbers(65537, int(modulus.group(1), 16)).public_key()
        whose = 'the key built into the programs'
    text_path = os.path.join(folder, 'hacbrewpacklz-update.txt')
    sig_path = os.path.join(folder, 'hacbrewpacklz-update.sig')
    for path in (text_path, sig_path):
        if not os.path.exists(path):
            sys.exit(f'{path} is missing.')
    with open(text_path, 'rb') as source:
        text = source.read()
    with open(sig_path, 'rb') as source:
        signature = source.read()
    ok = True
    try:
        key.verify(signature, text, padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32), hashes.SHA256())
        say(f'The signature checks out against {whose}.')
    except InvalidSignature:
        say(f"The signature DOESN'T check out against {whose}.")
        ok = False
    lines = text.decode().splitlines()
    say(f'It\'s for {lines[0]}.' if lines and lines[0].startswith('version ') else 'Its text doesn\'t start with the version.')
    listed = {}
    for line in lines[1:]:
        digest, _, name = line.partition('  ')
        listed[name] = digest
    for name, digest in listed.items():
        path = os.path.join(folder, name)
        if not os.path.exists(path):
            say(f'  {name}: listed, but not in the folder')
            ok = False
            continue
        with open(path, 'rb') as source:
            same = hashlib.sha256(source.read()).hexdigest() == digest
        say(f'  {name}: ' + ('matches' if same else "DOESN'T match"))
        ok = ok and same
    unlisted = [f for f in FILES if f not in listed and os.path.exists(os.path.join(folder, f))]
    for name in unlisted:
        say(f'  {name}: in the folder, but not listed')
        ok = False
    say(f"{len(listed)} files listed. {'All good.' if ok else 'Something is wrong: see above.'}")
    return 0 if ok else 1


def main():
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__.split('\n\n', 1)[1])
    parser.add_argument('version', nargs='?')
    parser.add_argument('folder', nargs='?')
    parser.add_argument('--tree')
    parser.add_argument('--replace', action='store_true')
    parser.add_argument('--verify', metavar='FOLDER')
    parser.add_argument('--public', metavar='PEM')
    args = parser.parse_args()
    if args.verify:
        return verify(args.verify, args.public, args.tree)
    if not args.version or not args.folder:
        parser.error('give a version and a folder, or --verify <folder>')

    local = this_pc.load('round', 'vswhere', 'devkitpro_bash', 'android_sdk', 'jdk', 'android_keystore', 'android_key_alias',
                         'android_keystore_pass')
    builder = Builder(args, local)
    changed = git(builder.tree, 'status', '--porcelain').splitlines()
    if changed:
        sys.exit(f'{builder.tree} has {len(changed)} change(s) not committed. A release is built from committed code only.')
    found = tree_version(builder.tree)
    if found != args.version:
        sys.exit(f'The code says version {found}, not {args.version}: python tools/set_version.py {args.version}, and commit it.')
    signed = ['hacbrewpacklz-update.txt', 'hacbrewpacklz-update.sig']
    there = [f for f in FILES + signed if os.path.exists(os.path.join(builder.dest, f))]
    if there and not args.replace:
        sys.exit(f"{builder.dest} already holds {', '.join(there)}. Choose another folder, or add --replace.")
    for name in signed:
        # A signature of the builds being replaced would describe files that are no longer there.
        if name in there:
            os.remove(os.path.join(builder.dest, name))
            say(f'Removed {name}, which was for the builds being replaced: step 5 signs the new ones.')
    builder.refuse_if_running()
    head = git(builder.tree, 'rev-parse', '--short', 'HEAD').strip()
    say(f'{args.version} from {head} in {builder.tree}, into {builder.dest}')
    say(f'Builds and logs in {builder.work}')
    say()
    builder.build()
    return builder.report()


if __name__ == '__main__':
    sys.exit(main())
