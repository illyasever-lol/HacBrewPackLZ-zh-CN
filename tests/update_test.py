#!/usr/bin/env python3
"""Updates a copy of the desktop tool from a pretend download site, signed with the real key, and tries to fool it.
usage: update_test.py <hacbrewpacklz.exe> <private key.pem> <folder to work in>"""
import hashlib
import http.server
import os
import shutil
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
exe, key, work = sys.argv[1:]
if not os.path.isfile(key) or os.path.isfile(work):
    sys.exit(__doc__)
name = os.path.basename(exe)
failures = 0


def report(what, ok, detail=''):
    global failures
    failures += 0 if ok else 1
    print(f"{'PASS' if ok else 'FAIL'}  {what}  {detail}", flush=True)


def sha(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


shutil.rmtree(work, ignore_errors=True)
site = os.path.join(work, 'site')
release = os.path.join(site, 'v9.9.9', 'downloads')
os.makedirs(release)
install = os.path.join(work, 'install')
os.makedirs(install)

# Something that still runs, and isn't the same bytes as what's there.
with open(exe, 'rb') as f:
    newer = f.read() + b'\0' * 64
with open(os.path.join(release, name), 'wb') as f:
    f.write(newer)
subprocess.run([sys.executable, os.path.join(HERE, '..', 'tools', 'sign_release.py'), key, '9.9.9', release, os.path.join(release, name)], check=True, stdout=subprocess.DEVNULL)
with open(os.path.join(site, 'releases'), 'w') as f:
    f.write('[{"name":"hacBrewPackLZ 9.9.9","tag_name":"v9.9.9","description":"whatever"}]')


class Quiet(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=site, **kwargs)

    def log_message(self, *args):
        pass


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Quiet)
threading.Thread(target=server.serve_forever, daemon=True).start()
env = dict(os.environ, HACBREWPACKLZ_UPDATE_SITE=f'http://127.0.0.1:{server.server_address[1]}')
copy = os.path.join(install, name)


def fresh():
    for entry in os.listdir(install):
        os.remove(os.path.join(install, entry))
    shutil.copy(exe, copy)


def update():
    p = subprocess.run([copy, 'update'], env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    return p.returncode, p.stdout.strip().splitlines()[-1]


def tamper(path, change):
    with open(path, 'rb') as f:
        original = f.read()
    with open(path, 'wb') as f:
        f.write(change(original))
    return original


original_hash = sha(exe)
fresh()
rc, out = update()
report('a signed release gets installed', rc == 0 and sha(copy) == hashlib.sha256(newer).hexdigest(), out)
report('and says so', 'Updated to version 9.9.9' in out, out)
p = subprocess.run([copy, '--help'], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
report('and runs', p.returncode == 0 and 'Usage' in p.stdout, p.stdout[:100])
report('and clears up after itself the next time it starts', sorted(os.listdir(install)) == [name], str(os.listdir(install)))

for what, file, change, word in (
    ('a bad signature', 'hacbrewpacklz-update.sig', lambda b: b[:9] + bytes([b[9] ^ 1]) + b[10:], 'signature'),
    ('a changed list of what is in the release', 'hacbrewpacklz-update.txt', lambda b: b[:14] + bytes([b[14] ^ 1]) + b[15:], 'signature'),
    ('a damaged download', name, lambda b: b[:4096] + bytes([b[4096] ^ 1]) + b[4097:], "isn't what the release says"),
):
    fresh()
    path = os.path.join(release, file)
    original = tamper(path, change)
    rc, out = update()
    report(f'{what} keeps the old program', rc != 0 and sha(copy) == original_hash and word in out, out)
    report(f'{what} leaves nothing behind', sorted(os.listdir(install)) == [name], str(os.listdir(install)))
    with open(path, 'wb') as f:
        f.write(original)

fresh()
with open(os.path.join(site, 'releases'), 'w') as f:
    f.write('[{"tag_name":"v1.0.0"}]')
rc, out = update()
report('an older release gets left alone', rc == 0 and sha(copy) == original_hash and 'latest' in out, out)

server.shutdown()
print('all passed' if failures == 0 else f'{failures} failed')
sys.exit(1 if failures else 0)
