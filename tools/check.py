"""Builds and tests hacBrewPackLZ at two levels, quick and full, and ends with one table of what passed.

    python tools/check.py full  [--tree <folder>] [--matrix <sections>|none] [--skip <parts>] [--stall <minutes>]
    python tools/check.py quick [--tree <folder>] <what to run>...
    python tools/check.py setup [--tree <folder>]

full is for before a build goes to the console or to anyone, and for the end of a batch of changes. It builds the Windows programs as they
ship (native), the Switch app and the PC build of the Switch app. Then it runs app_test demo, real and check seeds 1 to 4 with all 12 inputs,
replace_test --once on a game with two programs, session_check (plain, versions, seeds 1 to 4), every matrix section, look_check, and last the
two tests that open windows, console_screen and gui_shots. --skip takes any of: demo real check session matrix look home gui nro (real also
leaves out replace_test).

quick is for after each change. It runs only what it's given, and builds only what that needs, and only what changed since the last time:
    --only LOG,AGAIN     app_test real with just those HBP_TEST_ONLY_ switches
    --real, --demo       app_test real or demo, whole; --real runs replace_test --once too
    --seeds 1,2          app_test check seeds
    --session 1,2        session_check seeds (--session-modes adds plain and versions)
    --matrix a,b         matrix sections
    --look, --home, --gui    look_check, console_screen, gui_shots
    --nro                build the Switch app as well

setup gets a working copy ready for the matrix: LibHac (copied from local.json's libhac), libhac_check, zstd_check, and the matrix's
prepared title. full and quick do it by themselves when the matrix needs it.

--tree checks another working copy (this one by default). A test that copy doesn't have is listed as skipped, though in a full check the
test files below fail when it can't make them.

replace_test and the checkers' DLC checks read files make_patch_test.py makes: programs.nsp, kept in <round>/test_inputs, and the dlcs
files, in local.json's dlc_inputs or the same folder. Whatever isn't there is made first, from the matrix's prepared title. When that can't
be done, a full check fails and a quick one lists them as skipped.

Builds go in <round>/build-<copy>, kept from one check to the next so only what changed is built again. Each check's logs, test output and
summary.txt go in <round>/check-<date>-<time>. The output of a test that passed is deleted, except the screenshots of the window tests.

Rules it keeps by itself: no report reaches a report service (HACBREWPACKLZ_REPORT_SITE and HBP_TEST_REPORT_SITE are none); a line with 32 or
more hex characters is never printed; it doesn't build into a folder a running program was started from; a full check needs local.json's
min_free_gb free first; app_test real gets its folder with backslashes; temporary files go in the check's own folder, not on C:.
A test whose log hasn't grown for --stall minutes (20 by default, an hour for the matrix's l2) is stopped and marked stalled. Only processes
this script started are ever stopped.

Exit code 0 when everything passed, 1 when something didn't, 2 for a mistake in how it was called.
"""
import argparse
import datetime
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

MATRIX_GROUPS = [
    ['analyze', 'selection', 'titles', 'formats', 'zstd'],
    ['engine'],
    ['diff', 'options', 'edges', 'matrix', 'batch'],
    ['names', 'extracting', 'folders', 'copies'],
    ['l2'],
]
SKIPPABLE = ['demo', 'real', 'check', 'session', 'matrix', 'look', 'home', 'gui', 'nro']
# What tests read of make_patch_test.py's variants: programs for replace_test --once, and dlcs for the checkers' DLC checks.
TEST_FILES = {'programs': ['programs.nsp'], 'dlcs': ['base.nsp', 'update.nsp', 'dlc.nsp', 'dlc_small.nsp', 'dlc_small2.nsp', 'merged.nsp']}


def fwd(path):
    return path.replace('\\', '/')


def back(path):
    return path.replace('/', '\\')


def msys(path):
    """E:\\a\\b as MSYS2's bash names it, /e/a/b."""
    path = fwd(os.path.abspath(path))
    return '/' + path[0].lower() + path[2:] if re.match(r'^[A-Za-z]:', path) else path


def minutes(seconds):
    seconds = int(seconds)
    return f'{seconds // 60}:{seconds % 60:02d}'


class Check:
    def __init__(self, args, local, mode):
        self.args = args
        self.local = local
        self.mode = mode
        self.tree = os.path.normpath(os.path.abspath(args.tree or this_pc.TREE))
        self.round = os.path.normpath(local['round'])
        tag = os.path.basename(self.tree) + '-' + hashlib.sha1(self.tree.lower().encode()).hexdigest()[:6]
        self.builds = os.path.join(self.round, 'build-' + tag)
        self.run_dir = os.path.join(self.round, 'check-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
        self.logs = os.path.join(self.run_dir, 'logs')
        self.out = os.path.join(self.run_dir, 'out')
        self.tmp = os.path.join(self.run_dir, 'tmp')
        for folder in (self.logs, self.out, self.tmp, self.builds):
            os.makedirs(folder, exist_ok=True)
        self.keys_folder = os.path.normpath(local['keys'])
        self.prod_keys = os.path.join(self.keys_folder, 'prod.keys')
        self.inputs = [os.path.normpath(p) for p in local.get('checker_inputs', [])]
        self.results = []
        self.built = {}
        self.stall = args.stall
        self.env = dict(os.environ)
        # Python tests run with -I -u: -u so their output reaches the log as it's printed, which the stall watch goes by.
        self.env.update({'HACBREWPACKLZ_REPORT_SITE': 'none', 'HBP_TEST_REPORT_SITE': 'none', 'TEMP': self.tmp, 'TMP': self.tmp})
        if local.get('vswhere'):
            self.env['PATH'] = local['vswhere'] + os.pathsep + self.env.get('PATH', '')
        # make_patch_test.py's files that tests read, made when they aren't there (made()). They go in local.json's dlc_inputs or the round folder.
        self.test_inputs = {'programs': os.path.join(self.round, 'test_inputs'),
                            'dlcs': os.path.normpath(local.get('dlc_inputs') or os.path.join(self.round, 'test_inputs'))}
        self.made_inputs = {}
        self.started = time.time()

    # ---- what the copy has

    def has(self, relative):
        return os.path.exists(os.path.join(self.tree, relative))

    def matrix_sections(self):
        path = os.path.join(self.tree, 'tests', 'matrix.py')
        if not os.path.exists(path):
            return set()
        with open(path, encoding='utf-8', errors='replace') as source:
            return set(re.findall(r'^def section_(\w+)\(', source.read(), re.M))

    def tree_state(self):
        git = ['git', '-c', 'core.fsmonitor=false', '-C', self.tree]
        head = subprocess.run(git + ['rev-parse', '--short', 'HEAD'], capture_output=True, text=True).stdout.strip()
        branch = subprocess.run(git + ['rev-parse', '--abbrev-ref', 'HEAD'], capture_output=True, text=True).stdout.strip()
        changed = subprocess.run(git + ['status', '--porcelain'], capture_output=True, text=True).stdout.splitlines()
        return f'{self.tree}: {branch} at {head}, {len(changed)} file(s) changed and not committed'

    # ---- running things

    def say(self, line=''):
        print(safe(line), flush=True)

    def running_from(self, folder):
        """Paths of running programs started from inside `folder`."""
        prefix = (folder.rstrip('\\') + '\\').replace("'", "''")
        script = (f"Get-Process | Where-Object {{ $_.Path -and $_.Path.StartsWith('{prefix}', 'OrdinalIgnoreCase') }} | "
                  "ForEach-Object { $_.Path }")
        try:
            out = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command', script],
                                 capture_output=True, text=True, timeout=60).stdout
        except (OSError, subprocess.TimeoutExpired) as e:
            # Not knowing isn't the same as nothing running: leave the folder alone.
            return [f"(couldn't list running programs: {e})"]
        return [line.strip() for line in out.splitlines() if line.strip()]

    def execute(self, name, cmd, env=None, cwd=None, stall=None):
        """Runs `cmd` with its output in a log of its own, stopping it if the log stops growing. Returns (exit code, log, stalled)."""
        slug = re.sub(r'[^A-Za-z0-9]+', '_', name).strip('_').lower()
        log = os.path.join(self.logs, slug + '.log')
        limit = (stall or self.stall) * 60
        stalled = False
        with open(log, 'wb') as out:
            process = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, cwd=cwd or self.tree,
                                       env=env or self.env)
            size, changed = -1, time.time()
            while process.poll() is None:
                time.sleep(3)
                now = os.path.getsize(log)
                if now != size:
                    size, changed = now, time.time()
                elif time.time() - changed > limit:
                    # Only the process this script started, and what it started in turn.
                    subprocess.run(['taskkill', '/T', '/F', '/PID', str(process.pid)], capture_output=True)
                    process.wait()
                    stalled = True
        return process.returncode, log, stalled

    def record(self, name, status, passed=None, failed=None, seconds=0, detail=None, log=None):
        result = {'name': name, 'status': status, 'passed': passed, 'failed': failed, 'seconds': seconds, 'detail': detail or [], 'log': log}
        self.results.append(result)
        counts = '' if passed is None else f'  {passed} passed, {failed} failed'
        self.say(f'{status.upper():8} {name}{counts}  ({minutes(seconds)})')
        for line in result['detail'][:3]:
            self.say('         ' + line[:200])
        return status == 'passed'

    @staticmethod
    def read(log):
        with open(log, encoding='utf-8', errors='replace') as source:
            return source.read().splitlines()

    def test(self, name, cmd, needs=(), env=None, stall=None, clear=(), keep=(), kind='lines'):
        """Runs a test once the builds it needs are there, and records how it went."""
        missing = [need for need in needs if not self.built.get(need)]
        if missing:
            return self.record(name, 'skipped', detail=[f"{', '.join(missing)} didn't build"])
        for folder in clear:
            shutil.rmtree(folder, ignore_errors=True)
        start = time.time()
        code, log, stalled = self.execute(name, cmd, env=env, stall=stall)
        seconds = time.time() - start
        lines = self.read(log)
        if kind == 'matrix':
            tally = next((m for m in (re.match(r'^(\d+)/(\d+) passed', l) for l in reversed(lines)) if m), None)
            passed = int(tally.group(1)) if tally else 0
            failed = int(tally.group(2)) - passed if tally else 0
            fails = [l for l in lines if l.startswith('FAIL')]
        else:
            passed = sum(1 for l in lines if l.startswith('PASS'))
            fails = [l for l in lines if l.startswith('FAIL')]
            failed = len(fails)
        detail = fails[:3]
        if stalled:
            status = 'stalled'
            detail = [f'its log stopped growing for {(stall or self.stall)} min, so it was stopped'] + detail
        elif code == 0 and failed == 0 and passed > 0:
            status = 'passed'
        else:
            status = 'failed'
            if not detail:
                tail = [l for l in lines if l.strip()][-3:]
                detail = [f'exit code {code}' + ('' if passed else ', nothing passed')] + tail
        if status == 'passed':
            for folder in clear:
                if folder not in keep:
                    shutil.rmtree(folder, ignore_errors=True)
        return self.record(name, status, passed, failed, seconds, detail, log)

    # ---- builds

    def build(self, key):
        """Builds one thing, once a check. Returns whether it's there."""
        if key in self.built:
            return self.built[key]
        dotnet = ['dotnet']
        plans = {
            'native_pc': ('build: command line and home screen (native)', 'native',
                          dotnet + ['publish', 'hacbrewpacklz.csproj', '-c', 'Release', '-r', 'win-x64', '-o', self.path('native'),
                                    '--artifacts-path', self.path('artifacts_native_pc')], 'hacbrewpacklz.csproj'),
            'native_gui': ('build: Windows app (native)', 'native',
                           dotnet + ['publish', os.path.join('gui', 'hacbrewpacklz-gui.csproj'), '-c', 'Release', '-r', 'win-x64', '-o',
                                     self.path('native'), '--artifacts-path', self.path('artifacts_native_gui')], 'gui'),
            'dev_pc': ('build: command line and home screen', 'dev_pc',
                       dotnet + ['build', 'hacbrewpacklz.csproj', '-c', 'Release', '-o', self.path('dev_pc'),
                                 '--artifacts-path', self.path('artifacts_dev_pc')], 'hacbrewpacklz.csproj'),
            'dev_gui': ('build: Windows app', 'dev_gui',
                        dotnet + ['build', os.path.join('gui', 'hacbrewpacklz-gui.csproj'), '-c', 'Release', '-o', self.path('dev_gui'),
                                  '--artifacts-path', self.path('artifacts_dev_gui')], 'gui'),
            'session': ('build: session_check', 'session_check',
                        dotnet + ['build', os.path.join('tests', 'session_check', 'session_check.csproj'), '-c', 'Release', '-o',
                                  self.path('session_check'), '--artifacts-path', self.path('artifacts_sc')], os.path.join('tests', 'session_check')),
            'host': ('build: Switch app on the PC (host)', 'host',
                     ['cmd', '/c', os.path.join(self.tree, 'switch', 'host', 'build_fast.cmd'), self.path('host')],
                     os.path.join('switch', 'host', 'build_fast.cmd')),
            'nro': ('build: Switch app (NRO)', None,
                    [self.local.get('devkitpro_bash') or 'bash', '-lc', f"cd '{msys(self.tree)}/switch' && make -j4"], 'switch'),
            'libhac_check': ('build: libhac_check', None,
                             dotnet + ['build', os.path.join('tests', 'libhac_check', 'libhac_check.csproj'), '-c', 'Release'],
                             os.path.join('tests', 'libhac_check')),
            'zstd_check': ('build: zstd_check', None,
                           dotnet + ['build', os.path.join('tests', 'zstd_check', 'zstd_check.csproj'), '-c', 'Release'],
                           os.path.join('tests', 'zstd_check')),
        }
        name, folder, cmd, source = plans[key]
        if not self.has(source):
            self.built[key] = False
            self.record(name, 'skipped', detail=[f'this copy has no {source}'])
            return False
        if folder:
            running = self.running_from(self.path(folder))
            if running:
                self.built[key] = False
                self.record(name, 'failed', detail=[f'{p} is running, so its folder is left alone' for p in running])
                return False
        env = dict(self.env)
        if key == 'nro':
            # devkitPro's make loses its settings when it starts from Git Bash's environment.
            env['MSYSTEM'] = 'MSYS'
            env.pop('MSYS_NO_PATHCONV', None)
        start = time.time()
        code, log, stalled = self.execute(name, cmd, env=env)
        lines = self.read(log)
        errors = list(dict.fromkeys(l.strip() for l in lines if re.search(r'\berror\b|error:', l, re.I)
                                    and not re.search(r'warning|\b0 error', l, re.I)))
        ok = code == 0 and not stalled
        detail = [] if ok else (errors[:3] or [f'exit code {code}'] + [l for l in lines if l.strip()][-2:])
        if stalled:
            detail = ['its log stopped growing, so it was stopped'] + detail
        self.record(name, 'passed' if ok else ('stalled' if stalled else 'failed'), seconds=time.time() - start, detail=detail, log=log)
        self.built[key] = ok
        return ok

    def path(self, *parts):
        return os.path.join(self.builds, *parts)

    def tool(self):
        """The command line build the tests use: native in a full check, as it ships, and the quicker one otherwise."""
        return self.path('native', 'hacbrewpacklz.exe') if self.mode == 'full' else self.path('dev_pc', 'hacbrewpacklz.exe')

    def tool_build(self):
        return 'native_pc' if self.mode == 'full' else 'dev_pc'

    def gui(self):
        return self.path('native', 'hacbrewpacklz-gui.exe') if self.mode == 'full' else self.path('dev_gui', 'hacbrewpacklz-gui.exe')

    def gui_build(self):
        return 'native_gui' if self.mode == 'full' else 'dev_gui'

    # ---- setup for the matrix

    def setup(self):
        ok = True
        libhac = os.path.join(self.tree, 'tests', 'libhac')
        if not os.path.exists(os.path.join(libhac, 'LibHac.dll')):
            source = self.local.get('libhac')
            if source and os.path.exists(os.path.join(source, 'LibHac.dll')):
                shutil.copytree(source, libhac, dirs_exist_ok=True)
                self.record('setup: LibHac copied', 'passed', detail=[f'from {source}'])
            else:
                self.record('setup: LibHac', 'failed', detail=["local.json's libhac has no LibHac.dll; or run tests\\bundle_extract.py "
                                                               "<Ryujinx.exe> tests\\libhac LibHac (tests/README.md)"])
                ok = False
        ok = self.build('libhac_check') and ok
        ok = self.build('zstd_check') and ok
        try:
            import cryptography  # noqa: F401
            import zstandard  # noqa: F401
        except ImportError as e:
            self.record('setup: Python packages', 'failed', detail=[f'{e}: pip install cryptography zstandard'])
            ok = False
        work = self.local.get('matrix_work')
        reference = os.path.join(work or '', 't1', 'hbp_nosig', 'hacbrewpack_nsp', '0100abcd12340000.nsp')
        if work and not os.path.exists(reference):
            ok = self.test('setup: the matrix\'s prepared title', [sys.executable, '-I', '-u', os.path.join('tests', 'matrix.py'), 'prepare'],
                           env=self.matrix_env(), kind='matrix') and ok
        return ok

    def matrix_env(self):
        env = dict(self.env)
        env.update({'TOOL': self.tool(), 'HBPTOOL': self.path('host', 'hbptool.exe'), 'TESTDIR': self.local['matrix_work'],
                    'KEYS': self.prod_keys, 'HACBREWPACK': os.path.join(self.keys_folder, 'hacbrewpack.exe')})
        return env

    # ---- test files made here

    def made(self, variant):
        """The folder holding make_patch_test.py's `variant` files, made first if any isn't there, once a check. None if they can't be had."""
        if variant not in self.made_inputs:
            self.made_inputs[variant] = self.make(variant)
            if variant == 'dlcs' and self.made_inputs[variant]:
                self.env['HBP_TEST_DLC_INPUTS'] = self.made_inputs[variant]
        return self.made_inputs[variant]

    def make(self, variant):
        folder = self.test_inputs[variant]

        def lacking():
            return [n for n in TEST_FILES[variant] if not os.path.exists(os.path.join(folder, n))]

        if not lacking():
            return folder
        name = f'make: {variant} test files'
        title = os.path.join(self.local.get('matrix_work') or '', 't1')
        if not os.path.isdir(os.path.join(title, 'exefs')):
            why = "the matrix's prepared title isn't there (local.json's matrix_work, then check.py setup)"
        elif not self.build(self.tool_build()):
            why = f"{self.tool_build()} didn't build"
        else:
            # Made in this check's folder, and only what's missing copied over: tests may be reading the others, and it makes more besides.
            work = os.path.join(self.out, 'make_' + variant)
            start = time.time()
            code, log, _ = self.execute(name, [sys.executable, '-I', '-u', os.path.join('tests', 'make_patch_test.py'), self.prod_keys, self.tool(),
                                               title, work, variant])
            os.makedirs(folder, exist_ok=True)
            for n in lacking():
                if os.path.exists(os.path.join(work, n)):
                    shutil.copy2(os.path.join(work, n), os.path.join(folder, n))
            shutil.rmtree(work, ignore_errors=True)
            ok = code == 0 and not lacking()
            detail = [] if ok else [f'exit code {code}, {", ".join(lacking()) or "nothing"} missing'] + [l for l in self.read(log) if l.strip()][-2:]
            self.record(name, 'passed' if ok else 'failed', seconds=time.time() - start, detail=detail, log=log)
            return folder if ok else None
        self.record(name, 'failed' if self.mode == 'full' else 'skipped', detail=[f"{', '.join(lacking())} not in {folder}, and {why}"])
        return None

    # ---- the tests

    def app_test(self, mode, extra=(), env_extra=None, name=None):
        exe = self.path('host', 'app_test.exe')
        work = os.path.join(self.out, f'app_{mode}{extra[0] if mode == "check" else ""}')
        shots = work + '_shots'
        if mode == 'real':
            # app_test real hangs for good if its folder is written with forward slashes.
            work, shots = back(work), fwd(shots)
        else:
            work, shots = fwd(work), fwd(shots)
        env = dict(self.env)
        env.update(env_extra or {})
        return self.test(name or f'app_test {mode}', [exe, '-k', fwd(self.prod_keys), work, shots, mode, *extra], needs=['host'], env=env,
                         clear=[work, shots])

    def app_real(self, only=None):
        if len(self.inputs) < 8:
            return self.record('app_test real', 'skipped', detail=["local.json's checker_inputs has fewer than 8"])
        self.made('dlcs')
        env = {'HBP_TEST_SOLID_FILES': back(self.local.get('solid_files', ''))}
        name = 'app_test real'
        if only:
            env.update({f'HBP_TEST_ONLY_{switch.strip().upper()}': '1' for switch in only})
            name += f" (only {', '.join(s.strip().upper() for s in only)})"
        games = [self.inputs[6], self.inputs[7], self.inputs[0], self.inputs[1], self.inputs[2]]
        return self.app_test('real', [back(g) for g in games], env, name)

    def app_check(self, seed):
        if len(self.inputs) < 12:
            return self.record(f'app_test check seed {seed}', 'skipped', detail=["local.json's checker_inputs needs all 12"])
        return self.app_test('check', [str(seed), '12', *[fwd(p) for p in self.inputs]], name=f'app_test check seed {seed}')

    def replace_once(self):
        """replace_test --once on programs.nsp: one game with two programs in it, as collections have."""
        name = 'replace_test --once (two programs)'
        source = os.path.join(self.tree, 'switch', 'host', 'replace_test.cpp')
        text = ''
        if os.path.exists(source):
            with open(source, encoding='utf-8', errors='replace') as code:
                text = code.read()
        if '"--once"' not in text:
            return self.record(name, 'skipped', detail=["this copy's replace_test has no --once"])
        folder = self.made('programs')
        if not folder:
            return False
        work = os.path.join(self.out, 'replace_once')
        return self.test(name, [self.path('host', 'replace_test.exe'), '-k', fwd(self.prod_keys), fwd(work), '--once',
                                fwd(os.path.join(folder, 'programs.nsp'))], needs=['host'], clear=[work])

    def session(self, what):
        dll = self.path('session_check', 'session_check.dll')
        if what in ('plain', 'versions'):
            return self.test(f'session_check {what}', ['dotnet', dll, what], needs=['session'])
        self.made('dlcs')
        work = os.path.join(self.out, f'session{what}')
        files = [fwd(p) for p in self.inputs + list(self.local.get('session_extra', []))]
        return self.test(f'session_check seed {what}', ['dotnet', dll, fwd(self.prod_keys), fwd(work), str(what), '16', *files],
                         needs=['session'], clear=[work])

    def matrix(self, sections):
        have = self.matrix_sections()
        missing = [s for s in sections if s not in have]
        if missing:
            self.record(f"matrix {' '.join(missing)}", 'skipped', detail=["this copy's matrix.py has no such section"])
        sections = [s for s in sections if s in have]
        if not sections:
            return True
        result = self.test(f"matrix {' '.join(sections)}", [sys.executable, '-I', '-u', os.path.join('tests', 'matrix.py'), *sections],
                           needs=[self.tool_build(), 'host', 'libhac_check', 'zstd_check'], env=self.matrix_env(),
                           stall=60 if 'l2' in sections else None, kind='matrix')
        shutil.rmtree(os.path.join(self.local['matrix_work'], 'work'), ignore_errors=True)
        return result

    def lacks_inputs(self, name, count):
        """Records a skip when local.json has fewer than `count` checker inputs."""
        if len(self.inputs) >= count:
            return False
        self.record(name, 'skipped', detail=[f"local.json's checker_inputs needs at least {count}"])
        return True

    def look(self):
        if not self.has(os.path.join('tests', 'look_check.py')):
            return self.record('look_check', 'skipped', detail=['this copy has no tests/look_check.py'])
        if self.lacks_inputs('look_check', 3):
            return False
        work = os.path.join(self.out, 'look')
        return self.test('look_check', [sys.executable, '-I', '-u', os.path.join('tests', 'look_check.py'), self.tool(),
                                        self.path('host', 'hbptool.exe'), self.prod_keys, work, *self.inputs[:3]],
                         needs=[self.tool_build(), 'host'], clear=[work])

    def home(self):
        # console_screen finds its program by name, so it gets a copy under a name nothing else uses.
        if not self.built.get(self.tool_build()):
            return self.record('console_screen (home screen)', 'skipped', detail=[f"{self.tool_build()} didn't build"])
        if self.lacks_inputs('console_screen (home screen)', 1):
            return False
        copy = os.path.join(self.out, 'screen_bin')
        shutil.rmtree(copy, ignore_errors=True)
        source = os.path.dirname(self.tool())
        if self.mode == 'full':
            os.makedirs(copy)
            shutil.copy2(self.tool(), copy)
        else:
            shutil.copytree(source, copy)
        exe = os.path.join(copy, f'hbplzcheck{os.getpid()}.exe')
        os.replace(os.path.join(copy, 'hacbrewpacklz.exe'), exe)
        shots = os.path.join(self.out, 'console_shots')
        more = ';'.join(self.inputs[4:12])
        ok = self.test('console_screen (home screen)', [sys.executable, '-I', '-u', os.path.join('tests', 'console_screen.py'), exe,
                                                        self.keys_folder, self.inputs[0], shots, f'--more={more}'],
                       needs=[self.tool_build()], clear=[shots], keep=[shots])
        shutil.rmtree(copy, ignore_errors=True)
        return ok

    def gui_test(self):
        work, shots = os.path.join(self.out, 'gui_work'), os.path.join(self.out, 'gui_shots')
        large = self.local.get('gui_large')
        if not large or not os.path.exists(large):
            return self.record('gui_shots (Windows app)', 'skipped', detail=["local.json's gui_large isn't there"])
        if self.lacks_inputs('gui_shots (Windows app)', 1):
            return False
        return self.test('gui_shots (Windows app)', [sys.executable, '-I', '-u', os.path.join('tests', 'gui_shots.py'), self.gui(),
                                                     self.keys_folder, self.inputs[0], large, work, shots],
                         needs=[self.gui_build()], clear=[work, shots], keep=[shots])

    # ---- the end

    def summary(self):
        rows = [('Part', 'Result', 'Passed', 'Failed', 'Time')]
        for r in self.results:
            rows.append((r['name'], r['status'], '' if r['passed'] is None else str(r['passed']),
                         '' if r['failed'] is None else str(r['failed']), minutes(r['seconds'])))
        widths = [max(len(row[i]) for row in rows) for i in range(5)]
        lines = ['  '.join(cell.ljust(widths[i]) if i < 2 else cell.rjust(widths[i]) for i, cell in enumerate(row)).rstrip() for row in rows]
        lines.insert(1, '-' * len(lines[0]))
        bad = [r for r in self.results if r['status'] in ('failed', 'stalled')]
        skipped = [r for r in self.results if r['status'] == 'skipped']
        lines.append('')
        if bad:
            lines.append(f'{len(bad)} part(s) did not pass:')
            for r in bad:
                lines.append(f"  {r['name']}: {r['status']}" + (f" (log: {r['log']})" if r['log'] else ''))
                lines.extend('    ' + safe(d)[:200] for d in r['detail'][:3])
        else:
            lines.append('Everything that ran passed.')
        if skipped:
            lines.append(f'{len(skipped)} part(s) skipped, so not checked:')
            lines.extend(f"  {r['name']}: {r['detail'][0] if r['detail'] else ''}" for r in skipped)
        lines.append(f'Took {minutes(time.time() - self.started)} in all. Logs: {self.logs}')
        text = '\n'.join(safe(l) for l in lines)
        with open(os.path.join(self.run_dir, 'summary.txt'), 'w', encoding='utf-8') as out:
            out.write(self.header + '\n\n' + text + '\n')
        self.say()
        self.say(text)
        shutil.rmtree(self.tmp, ignore_errors=True)
        return 1 if bad else 0


def full(check):
    skip = set(check.args.skip or [])
    usage = shutil.disk_usage(check.round)
    need = float(check.local.get('min_free_gb') or 0)
    if usage.free / 1e9 < need:
        sys.exit(f'{check.round} has {usage.free / 1e9:.0f} GB free, and a full check wants {need:.0f} GB (local.json min_free_gb).')
    sections = MATRIX_GROUPS
    if check.args.matrix == 'none':
        sections = []
    elif check.args.matrix:
        sections = [check.args.matrix.split(',')]
    if 'matrix' in skip:
        sections = []

    for key in ['native_pc', 'native_gui', 'host', 'session']:
        check.build(key)
    if 'nro' not in skip:
        check.build('nro')
    if sections:
        check.setup()

    if 'demo' not in skip:
        check.app_test('demo')
    if 'real' not in skip:
        check.app_real()
        check.replace_once()
    if 'check' not in skip:
        for seed in (1, 2, 3, 4):
            check.app_check(seed)
    if 'session' not in skip:
        for what in ('plain', 'versions', 1, 2, 3, 4):
            check.session(what)
    for group in sections:
        check.matrix(group)
    if 'look' not in skip:
        check.look()
    # The two tests that open windows go last, on their own.
    if 'home' not in skip:
        check.home()
    if 'gui' not in skip:
        check.gui_test()


def quick(check):
    a = check.args
    wanted = any([a.only, a.real, a.demo, a.seeds, a.session, a.matrix, a.look, a.home, a.gui, a.nro])
    if not wanted:
        print('quick needs to be told what to run: see python tools/check.py --help', file=sys.stderr)
        return False
    if a.nro:
        check.build('nro')
    if a.only or a.real or a.demo or a.seeds or a.look or a.matrix:
        check.build('host')
    if a.matrix or a.look or a.home:
        check.build('dev_pc')
    if a.gui:
        check.build('dev_gui')
    if a.session:
        check.build('session')
    if a.matrix:
        check.setup()

    if a.demo:
        check.app_test('demo')
    if a.real:
        check.app_real()
        check.replace_once()
    if a.only:
        check.app_real(a.only.split(','))
    for seed in (a.seeds.split(',') if a.seeds else []):
        check.app_check(int(seed))
    if a.session:
        if a.session_modes:
            check.session('plain')
            check.session('versions')
        for seed in a.session.split(','):
            check.session(int(seed))
    if a.matrix:
        check.matrix(a.matrix.split(','))
    if a.look:
        check.look()
    if a.home:
        check.home()
    if a.gui:
        check.gui_test()
    return True


def main():
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__.split('\n\n', 1)[1])
    parser.add_argument('level', choices=['full', 'quick', 'setup'])
    parser.add_argument('--tree', help='the working copy to check (this one by default)')
    parser.add_argument('--stall', type=float, default=20, help='minutes without a word before a test is stopped (default 20)')
    parser.add_argument('--skip', type=lambda s: [p.strip() for p in s.split(',')], help='full: parts to leave out: ' + ' '.join(SKIPPABLE))
    parser.add_argument('--matrix', help='matrix sections, comma separated (full: none to leave the matrix out)')
    parser.add_argument('--only', help='quick: app_test real with just these HBP_TEST_ONLY_ switches, comma separated')
    parser.add_argument('--real', action='store_true')
    parser.add_argument('--demo', action='store_true')
    parser.add_argument('--seeds', help='quick: app_test check seeds, comma separated')
    parser.add_argument('--session', help='quick: session_check seeds, comma separated')
    parser.add_argument('--session-modes', action='store_true')
    parser.add_argument('--look', action='store_true')
    parser.add_argument('--home', action='store_true')
    parser.add_argument('--gui', action='store_true')
    parser.add_argument('--nro', action='store_true')
    args = parser.parse_args()
    if args.skip and any(p not in SKIPPABLE for p in args.skip):
        parser.error('--skip takes: ' + ' '.join(SKIPPABLE))

    needed = ['keys', 'round']
    if args.level == 'full':
        needed += ['checker_inputs', 'session_extra', 'solid_files', 'matrix_work', 'gui_large', 'vswhere', 'devkitpro_bash']
    elif args.level == 'setup':
        needed += ['matrix_work']
    local = this_pc.load(*needed)
    if args.level != 'setup' and args.matrix and args.matrix != 'none' and not local.get('matrix_work'):
        this_pc.load('matrix_work')

    check = Check(args, local, args.level)
    check.header = f'check.py {" ".join(sys.argv[1:])}\n{check.tree_state()}\nstarted {datetime.datetime.now():%Y-%m-%d %H:%M}'
    check.say(check.header)
    check.say(f'Builds in {check.builds}, this check in {check.run_dir}')
    check.say()
    if args.level == 'full':
        full(check)
    elif args.level == 'quick':
        if not quick(check):
            return 2
    else:
        check.setup()
    return check.summary()


if __name__ == '__main__':
    sys.exit(main())
