"""Runs the report service's tests in Edge, with nothing to install: serves the folder, has Edge open test.html headless, and prints what it says.

    python report-service\\test\\run_tests.py
"""
import functools
import html
import http.server
import os
import re
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
SERVICE = os.path.dirname(HERE)
EDGE = next((p for p in [r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe', r'C:\Program Files\Microsoft\Edge\Application\msedge.exe']
             if os.path.exists(p)), None)


class Quiet(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, '.js': 'text/javascript'}

    def log_message(self, *args):
        pass


def main():
    if not EDGE:
        print('Edge is needed to run these')
        return 2
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), functools.partial(Quiet, directory=SERVICE))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f'http://127.0.0.1:{server.server_address[1]}/test/test.html'
    try:
        dom = subprocess.run([EDGE, '--headless=new', '--disable-gpu', '--no-first-run', '--virtual-time-budget=20000', '--dump-dom', url],
                             capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=120).stdout
    finally:
        server.shutdown()
    found = re.search(r'<pre id="out">(.*?)</pre>', dom, re.S)
    out = html.unescape(found.group(1)) if found else 'the page never said how it went'
    print(out)
    return 0 if out.rstrip().endswith('all passed') else 1


if __name__ == '__main__':
    sys.exit(main())
