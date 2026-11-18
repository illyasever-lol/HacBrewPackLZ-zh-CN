"""Sends reports from the command line to a stand-in report service, and checks what arrives and what the person is told: sent, a known
problem, or saved to a file when the service can't be reached or turns it away. Nothing leaves this PC.

    python tests\\report_test.py <hacbrewpacklz.exe> <folder to work in>
"""
import http.server
import json
import os
import re
import shutil
import subprocess
import sys
import threading

exe, work = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
failures = 0
received = []
reply = {'code': 200, 'body': {'status': 'received', 'id': 7}}


def report(name, ok, detail=''):
    global failures
    failures += 0 if ok else 1
    print(f"{'PASS' if ok else 'FAIL'}  {name}{'' if ok else '  ' + detail}", flush=True)


class Service(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('content-length', 0)))
        received.append((self.path, json.loads(body)))
        data = json.dumps(reply['body']).encode()
        self.send_response(reply['code'])
        self.send_header('content-type', 'application/json')
        self.send_header('content-length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


shutil.rmtree(work, ignore_errors=True)
os.makedirs(work)
server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Service)
threading.Thread(target=server.serve_forever, daemon=True).start()
site = f'http://127.0.0.1:{server.server_address[1]}'
env = dict(os.environ, HACBREWPACKLZ_SETTINGS=os.path.join(work, 'settings.txt'), HACBREWPACKLZ_REPORT_FOLDER=work, HACBREWPACKLZ_REPORT_SITE=site)


def send(said, where=None):
    run_env = dict(env, HACBREWPACKLZ_REPORT_SITE=where) if where else env
    p = subprocess.run([exe, 'report', said], env=run_env, stdin=subprocess.DEVNULL, capture_output=True, text=True, errors='replace', timeout=120)
    return p.returncode, (p.stdout + p.stderr).strip()


rc, out = send('It froze while compressing')
report('a report is sent, and the person is told its number', rc == 0 and 'Sent as report 7' in out, out)
path, sent = received[-1] if received else ('', {})
report('it goes to /report', path == '/report', path)
report('with what the person wrote, and what it is', sent.get('said') == 'It froze while compressing' and sent.get('kind') == 'manual' and sent.get('app') == 'hacBrewPackLZ'
       and sent.get('version') and sent.get('platform') == 'Windows', json.dumps({k: v for k, v in sent.items() if k != 'report'}))
report('and the settings, as the report has them', '== Settings' in sent.get('report', ''), sent.get('report', '')[:200])
report('from a device name made up for it', re.fullmatch(r'[0-9a-f]{32}', sent.get('device', '')) is not None, sent.get('device', ''))
home = os.path.expanduser('~')
report("with the person's own name taken out of paths", home.lower() not in json.dumps(sent).lower().replace('\\\\', '\\'), '')
rc, out = send('Again')
report('the same device name each time', received[-1][1].get('device') == sent.get('device'), '')

reply['body'] = {'status': 'fixed', 'fixedIn': '1.5.1', 'issue': 42, 'id': 8}
rc, out = send('Still broken')
report('a known problem says what fixes it', rc == 0 and 'fixed in version 1.5.1' in out, out)

reply.update(code=429, body={'error': 'too many reports for now, try again later'})
rc, out = send('One more')
saved = [f for f in os.listdir(work) if f.endswith('.txt') and 'report' in f.lower()]
report('turned away, it says why and saves it instead', rc == 0 and 'too many reports' in out and 'saved to' in out and saved, out)

rc, out = send('With nowhere to send it', 'none')
report("with nowhere to send it, it's saved, and that's all it says", rc == 0 and 'Saved to' in out and "couldn't" not in out, out)

server.shutdown()
rc, out = send('With nothing there', 'http://127.0.0.1:9')
report("with the service out of reach, it's saved and says so", rc == 0 and "couldn't be reached" in out and 'saved to' in out, out)
with open(os.path.join(work, saved[0]), encoding='utf-8') as f:
    text = f.read()
report('the saved report starts with what the person wrote', text.startswith('What I was doing: '), text[:80])

print('all passed' if failures == 0 else f'{failures} failed')
sys.exit(1 if failures else 0)
