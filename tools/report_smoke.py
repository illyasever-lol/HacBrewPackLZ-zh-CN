"""Checks the report service once it's set up: sends a report, finds it in the inbox with the read key, reads it whole, and marks it dismissed.

    python tools\\report_smoke.py

It reads the service's address from HBPLZ_REPORTS_URL and the read key from HBPLZ_REPORTS_KEY, and never prints the key.
"""
import json
import os
import sys
import urllib.error
import urllib.request
import uuid


def call(method, url, body=None, key=None):
    data = json.dumps(body).encode() if body is not None else None
    request = urllib.request.Request(url, data=data, method=method, headers={'content-type': 'application/json', 'user-agent': 'hacBrewPackLZ smoke test'})
    if key:
        request.add_header('Authorization', f'Bearer {key}')
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return response.status, json.loads(response.read() or b'null')
    except urllib.error.HTTPError as e:
        return e.code, None


def main():
    base = os.environ.get('HBPLZ_REPORTS_URL', '').rstrip('/')
    key = os.environ.get('HBPLZ_REPORTS_KEY', '')
    if not base or not key:
        print('Set HBPLZ_REPORTS_URL and HBPLZ_REPORTS_KEY first, as report-service\\README.md says.')
        return 2
    mark = f'smoke|{uuid.uuid4()}'
    failures = 0

    def report(name, ok, detail=''):
        nonlocal failures
        failures += 0 if ok else 1
        print(f"{'PASS' if ok else 'FAIL'}  {name}{'' if ok else '  ' + detail}")

    code, sent = call('POST', f'{base}/report', {'app': 'hacBrewPackLZ', 'version': '0.0.0-test.1', 'platform': 'smoke test', 'kind': 'failed-job',
                                                 'fingerprint': mark, 'title': 'A smoke test', 'report': 'Sent by tools/report_smoke.py', 'device': 'smoke'})
    report('a report is taken', code == 200 and sent and sent.get('status') == 'received', f'{code} {sent}')
    report('the inbox needs the key', call('GET', f'{base}/inbox')[0] == 401)
    code, inbox = call('GET', f'{base}/inbox?status=new&limit=500', key=key)
    found = next((p for p in (inbox or {}).get('problems', []) if p.get('key') == f'hacBrewPackLZ|{mark}'), None)
    report('it is in the inbox, with the key', code == 200 and found is not None and found.get('reports') == 1, f'{code}')
    if sent and sent.get('id'):
        code, whole = call('GET', f"{base}/inbox/report/{sent['id']}", key=key)
        report('and can be read whole', code == 200 and whole and whole.get('body') == 'Sent by tools/report_smoke.py', f'{code}')
    # Seen again, as an app says of a problem it sent before: only counted, with no report of its own.
    code, counted = call('POST', f'{base}/seen', {'app': 'hacBrewPackLZ', 'version': '0.0.0-test.1', 'fingerprint': mark, 'device': 'smoke', 'count': 2})
    report('seen again, it is only counted', code == 200 and counted and counted.get('status') == 'counted', f'{code} {counted}')
    code, inbox = call('GET', f'{base}/inbox?status=new&limit=500', key=key)
    found = next((p for p in (inbox or {}).get('problems', []) if p.get('key') == f'hacBrewPackLZ|{mark}'), None)
    report('as two more times, with no report of its own', found is not None and found.get('reports') == 1 and found.get('occurrences') == 3,
           f"{found and {k: found.get(k) for k in ('reports', 'occurrences')}}")
    code, _ = call('POST', f'{base}/problem/{urllib.request.quote(f"hacBrewPackLZ|{mark}", safe="")}', {'status': 'dismissed'}, key=key)
    report('it can be marked dismissed', code == 200, f'{code}')
    print('all passed' if failures == 0 else f'{failures} failed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
