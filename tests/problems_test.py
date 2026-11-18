"""Problems the app finds itself, sent by itself when the person has agreed to it, against a stand-in report service on this PC: a game a job
couldn't do, sent once a version and only counted after that; the app stopping on something it didn't expect, and not answering until it
was closed, both sent the next time it opens; and nothing at all without agreeing to it, or once it's turned off. Nothing leaves this PC.

    python tests\\problems_test.py <hacbrewpacklz.exe> <hacbrewpacklz-gui.exe> <folder with prod.keys> <small.nsp> <folder to work in>

The GUI and the home screen open in windows of their own for a moment, behind whatever's in use. Only what this starts is closed.
"""
import http.server
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time

exe, gui, keys, nsp, work = (os.path.abspath(a) for a in sys.argv[1:6])
failures = 0
received = []


def report(name, ok, detail=''):
    global failures
    failures += 0 if ok else 1
    print(f"{'PASS' if ok else 'FAIL'}  {name}{'' if ok else '  ' + detail}", flush=True)


class Service(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get('content-length', 0))))
        received.append((self.path, body))
        data = json.dumps({'status': 'received', 'id': len(received)} if self.path == '/report' else {'status': 'counted'}).encode()
        self.send_response(200)
        self.send_header('content-type', 'application/json')
        self.send_header('content-length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


def key_values():
    """The keys' values, to look for in what's sent. Never printed."""
    values = []
    for line in open(os.path.join(keys, 'prod.keys'), encoding='utf-8', errors='replace'):
        value = ''.join(c for c in line.partition('=')[2] if c in '0123456789abcdefABCDEF').lower()
        if len(value) >= 16:
            values.append(value)
    return values


def settings(*lines):
    with open(os.path.join(work, 'settings.txt'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(['welcomed=1', 'problemsasked=1', '--noautoupdate=1', *lines]) + '\n')


def waiting():
    folder = os.path.join(work, 'reports waiting')
    return sorted(os.listdir(folder)) if os.path.isdir(folder) else []


def wait_until(test, seconds):
    deadline = time.time() + seconds
    while time.time() < deadline:
        if test():
            return True
        time.sleep(0.2)
    return test()


def stop(process):
    """Only what this started, and whatever it started."""
    subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], capture_output=True, stdin=subprocess.DEVNULL)
    try:
        process.wait(15)
    except subprocess.TimeoutExpired:
        pass


def open_gui(**more):
    return subprocess.Popen([gui], cwd=work, env=dict(env, **more))


def open_home_screen(**more):
    # In a window of its own, as it only opens with one, behind whatever's in use.
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 4  # SW_SHOWNOACTIVATE
    return subprocess.Popen(['conhost.exe', exe], cwd=work, env=dict(env, **more), startupinfo=startup)


def cli(*args, **more):
    p = subprocess.run([exe, *args], cwd=work, env=dict(env, **more), stdin=subprocess.DEVNULL, capture_output=True, text=True, errors='replace', timeout=600)
    return p.returncode, (p.stdout + p.stderr)


shutil.rmtree(work, ignore_errors=True)
os.makedirs(work)
server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Service)
threading.Thread(target=server.serve_forever, daemon=True).start()
site = f'http://127.0.0.1:{server.server_address[1]}'
env = dict(os.environ, HACBREWPACKLZ_SETTINGS=os.path.join(work, 'settings.txt'), HACBREWPACKLZ_REPORT_FOLDER=work, HACBREWPACKLZ_REPORT_SITE=site)
env.pop('HACBREWPACKLZ_TEST_PROBLEM', None)
secrets = key_values()
key_file = os.path.join(keys, 'prod.keys')
out = os.path.join(work, 'out')
settings()

try:
    # Without agreeing to it, nothing goes, and nothing waits to.
    rc, said = cli('convert', '-k', key_file, f'--nspdir={out}', nsp, HACBREWPACKLZ_TEST_PROBLEM='job')
    report('a game that can\'t be done is said to have failed', rc != 0 and 'FAILED' in said, said[-300:])
    report('without --sendproblems, nothing is sent, nor kept to send', not received and not waiting(), f'{received} {waiting()}')

    # With it, it goes before the command line is done.
    rc, said = cli('convert', '-k', key_file, f'--nspdir={out}', '--sendproblems', '--level=9', '--keyareakey=00112233445566778899aabbccddeeff', nsp,
                   HACBREWPACKLZ_TEST_PROBLEM='job')
    sent = [b for p, b in received if p == '/report']
    first = sent[0] if sent else {}
    report('with --sendproblems, the game that couldn\'t be done goes to the developer', len(sent) == 1 and first.get('kind') == 'failed-job', json.dumps(received)[:400])
    report('known by what went wrong, without the name it was about, but with the error code',
           first.get('title') == "Couldn't write '...' (2002-0002), for the tests" and re.fullmatch(r'failed-job\|[0-9a-f]{12}', first.get('fingerprint', '')) is not None,
           f"{first.get('title')} / {first.get('fingerprint')}")
    report('from this version and system, as a device of its own', first.get('version') and first.get('platform') == 'Windows' and len(first.get('device', '')) == 32,
           json.dumps({k: v for k, v in first.items() if k != 'report'}))
    report('with no key in it', bool(first) and not any(v in json.dumps(first).lower() for v in secrets), 'a key value was in what was sent')
    options = first.get('report', '')
    report("its options are named, with only a number's value: no folder, and no key given on the command line",
           '--nspdir' in options and out.lower() not in options.lower() and '--keyareakey' in options and '00112233445566778899' not in options and '--level=9' in options,
           options[-400:])
    report('and nothing left waiting', not waiting(), str(waiting()))

    # The same problem again, in the same version, is only counted, and the count goes with the run after.
    before = len(received)
    cli('convert', '-k', key_file, f'--nspdir={out}', '--sendproblems', nsp, HACBREWPACKLZ_TEST_PROBLEM='job')
    again = received[before:]
    report('the same problem again isn\'t sent again, but counted', [p for p, _ in again] == ['/seen'] and again[0][1].get('count') == 1
           and again[0][1].get('fingerprint') == first.get('fingerprint'), json.dumps(again)[:300])

    # The GUI stopping on something it didn't expect: kept, and sent the next time it opens.
    settings('--sendproblems=1')
    before = len(received)
    process = open_gui(HACBREWPACKLZ_TEST_PROBLEM='crash')
    kept = wait_until(lambda: any(f.startswith('crash-') for f in waiting()), 30)
    stop(process)
    report('the GUI stopping unexpectedly keeps a report to send', kept and len(received) == before, f'{waiting()}, {len(received) - before} sent')
    process = open_gui()
    wait_until(lambda: len(received) > before, 30)
    stop(process)
    crash = next((b for p, b in received[before:] if p == '/report'), {})
    report('which goes the next time it opens', crash.get('kind') == 'crash' and 'InvalidOperationException' in crash.get('title', '')
           and 'GuiProgram.Main' in crash.get('title', '') and not waiting(), f"{crash.get('title')} / {waiting()}")
    report('with what it went through, and no key', 'InvalidOperationException' in crash.get('report', '') and not any(v in json.dumps(crash).lower() for v in secrets),
           crash.get('report', '')[:200])

    # Not answering until it was closed: a note while it's stuck, which is sent the next time it opens.
    before = len(received)
    process = open_gui(HACBREWPACKLZ_TEST_PROBLEM='freeze')
    noted = wait_until(lambda: any(f.startswith('not answering ') for f in os.listdir(work)), 40)
    stop(process)
    report('the GUI not answering leaves a note saying so', noted, str(os.listdir(work)))
    process = open_gui()
    wait_until(lambda: len(received) > before, 30)
    stop(process)
    freeze = next((b for p, b in received[before:] if p == '/report'), {})
    report('which goes as a freeze the next time it opens', freeze.get('kind') == 'freeze' and freeze.get('title', '').startswith('Stopped answering while ')
           and not any(f.startswith('not answering ') for f in os.listdir(work)), f"{freeze.get('title')} / {os.listdir(work)}")

    # The home screen stopping on something it didn't expect.
    before = len(received)
    host = open_home_screen(HACBREWPACKLZ_TEST_PROBLEM='crash')
    kept = wait_until(lambda: len(waiting()) > 0, 30)
    stop(host)
    report('the home screen stopping unexpectedly keeps a report to send', kept, str(waiting()))
    host = open_home_screen()
    wait_until(lambda: len(received) > before, 30)
    stop(host)
    home = next((b for p, b in received[before:] if p == '/report'), {})
    report('which goes the next time it opens', home.get('kind') == 'crash' and 'Tui.' in home.get('title', ''), str(home.get('title')))

    # Turned off in Options: what's waiting is dropped, and nothing goes. A crash that's new again, as the same one is only counted now.
    before = len(received)
    os.remove(os.path.join(work, 'problems sent.txt'))
    process = open_gui(HACBREWPACKLZ_TEST_PROBLEM='crash')
    wait_until(lambda: len(waiting()) > 0, 30)
    stop(process)
    settings('--sendproblems=0')
    process = open_gui()
    dropped = wait_until(lambda: not waiting(), 20)
    time.sleep(2)
    stop(process)
    report('turned off, what was waiting is dropped, and nothing goes', dropped and len(received) == before, f'{waiting()}, {len(received) - before} sent')
finally:
    server.shutdown()

print('all passed' if failures == 0 else f'{failures} failed')
sys.exit(1 if failures else 0)
