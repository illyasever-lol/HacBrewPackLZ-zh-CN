"""Puts a file on the Switch's SD card over FTP and reads it back to compare, or gets a file or a folder's list from it.

Ask the console's owner before every run: sending anything to their Switch is theirs to approve.

    python tools/switch_ftp.py put <file on this PC> <path on the SD card>
    python tools/switch_ftp.py get <path on the SD card> <file on this PC>
    python tools/switch_ftp.py list <folder on the SD card>

SD card paths are written from the card's root, like switch/hacBrewPackLZ/log.txt (sdmc:/ in front is fine too). In Git Bash, don't start
one with /, which Git Bash turns into a Windows path, or set MSYS_NO_PATHCONV=1.

The console's address and FTP port come from tools/local.json (switch_ip, switch_ftp_port); --ip and --port change them for one run. The
address can change, as the console gets it from the router: if it doesn't answer, ask the user for the new one.

put never writes SaltySD/exceptions.txt (the console owner's own) or switch/hacBrewPackLZ-fixes.nro. It writes the file once, under
<name>.part, reads it back and compares, and only then puts it in place of the old one, so a cut-short upload never replaces anything.
"""
import argparse
import ftplib
import hashlib
import os
import posixpath
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import this_pc  # noqa: E402

NEVER_WRITE = {'saltysd/exceptions.txt', 'switch/hacbrewpacklz-fixes.nro'}


def card_path(path):
    """A path from the SD card's root, without sdmc:/ or a leading slash; stops at .. or a drive letter."""
    path = path.replace('\\', '/')
    if path.lower().startswith('sdmc:'):
        path = path[5:]
    path = posixpath.normpath('/' + path.lstrip('/')).lstrip('/')
    if path.startswith('..') or ':' in path:
        sys.exit(f'{path}: not a path on the SD card')
    return path


def connect(args):
    local = this_pc.load() if not (args.ip and args.port) else {}
    ip = args.ip or local.get('switch_ip')
    port = args.port or local.get('switch_ftp_port') or 5000
    if not ip:
        sys.exit('No address for the Switch: set switch_ip in tools/local.json, or give --ip.')
    ftp = ftplib.FTP()
    try:
        ftp.connect(ip, int(port), timeout=30)
        ftp.login()
    except (OSError, ftplib.Error) as e:
        sys.exit(f"The Switch at {ip}:{port} didn't answer ({e}). Is its FTP server on, and is the address still the same?")
    return ftp


def sha256_of_remote(ftp, path):
    digest = hashlib.sha256()
    size = [0]

    def take(block):
        digest.update(block)
        size[0] += len(block)
    ftp.retrbinary(f'RETR /{path}', take)
    return digest.digest(), size[0]


def put(ftp, source, target):
    if target.lower() in NEVER_WRITE:
        sys.exit(f'{target} is never written: it belongs to the console owner.')
    if not os.path.isfile(source):
        sys.exit(f'{source} is not a file.')
    with open(source, 'rb') as f:
        data = f.read()
    folder = posixpath.dirname(target)
    part = target + '.part'
    if folder:
        try:
            ftp.cwd('/' + folder)
        except ftplib.error_perm:
            sys.exit(f'The folder {folder} is not on the SD card. Nothing was written.')
        ftp.cwd('/')
    with open(source, 'rb') as f:
        ftp.storbinary(f'STOR /{part}', f)
    back, size = sha256_of_remote(ftp, part)
    if back != hashlib.sha256(data).digest() or size != len(data):
        try:
            ftp.delete('/' + part)
        except ftplib.Error:
            pass
        sys.exit(f'What was read back from {part} differs from {source} ({size} of {len(data)} bytes), so it was deleted and '
                 f'{target} was left as it was.')
    # The old file steps aside under <name>.old until the new one is in place, and comes back if that fails.
    old = target + '.old'
    had_old = True
    try:
        ftp.rename('/' + target, '/' + old)
    except ftplib.error_perm:
        had_old = False
    try:
        ftp.rename('/' + part, '/' + target)
    except (OSError, ftplib.Error) as e:
        if had_old:
            ftp.rename('/' + old, '/' + target)
        sys.exit(f'The new file is at sdmc:/{part}, but it could not take the old one\'s place ({e}); the old one is as it was.')
    if had_old:
        ftp.delete('/' + old)
    print(f'Put {os.path.basename(source)} ({len(data):,} bytes) at sdmc:/{target}, and what was read back is the same.')


def get(ftp, source, target):
    if os.path.isdir(target):
        target = os.path.join(target, posixpath.basename(source))
    with open(target, 'wb') as f:
        try:
            ftp.retrbinary(f'RETR /{source}', f.write)
        except ftplib.error_perm as e:
            f.close()
            os.remove(target)
            sys.exit(f'sdmc:/{source} could not be read: {e}')
    print(f'Got sdmc:/{source} ({os.path.getsize(target):,} bytes) as {target}.')


def listing(ftp, folder):
    try:
        names = ftp.nlst('/' + folder)
    except ftplib.error_perm as e:
        sys.exit(f'sdmc:/{folder} could not be listed: {e}')
    for name in sorted(names):
        print(posixpath.basename(name.rstrip('/')))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__.split('\n\n', 1)[1])
    parser.add_argument('action', choices=['put', 'get', 'list'])
    parser.add_argument('paths', nargs='+')
    parser.add_argument('--ip')
    parser.add_argument('--port', type=int)
    args = parser.parse_args()
    expected = {'put': 2, 'get': 2, 'list': 1}[args.action]
    if len(args.paths) != expected:
        parser.error(f'{args.action} takes {expected} path(s)')
    if args.action == 'put':
        target = card_path(args.paths[1])
        if target.lower() in NEVER_WRITE:
            sys.exit(f'{target} is never written: it belongs to the console owner.')
    ftp = connect(args)
    try:
        if args.action == 'put':
            put(ftp, args.paths[0], card_path(args.paths[1]))
        elif args.action == 'get':
            get(ftp, card_path(args.paths[0]), args.paths[1])
        else:
            listing(ftp, card_path(args.paths[0]))
    finally:
        try:
            ftp.quit()
        except (OSError, ftplib.Error):
            ftp.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
