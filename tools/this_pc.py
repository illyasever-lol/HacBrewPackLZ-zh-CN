"""This PC's paths for the check and release scripts.

They're read from tools/local.json, which stays out of git, as every PC has its own. tools/local.example.json names every entry. In a
working copy without a local.json, the one in the main checkout is used (the first folder `git worktree list` names).

Key files are only ever passed to other programs by their path: nothing here, in check.py or in build_release.py opens one.
"""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.dirname(HERE)

ENTRIES = {
    'keys': 'the folder holding prod.keys and hacbrewpack.exe',
    'round': 'the folder builds, logs and test output go in, off C:',
    'min_free_gb': 'how much room the round folder\'s drive needs before a full check',
    'vswhere': 'Visual Studio\'s Installer folder, which native builds need on PATH',
    'devkitpro_bash': 'devkitPro\'s MSYS2 bash.exe, which builds the Switch app',
    'android_sdk': 'the Android SDK folder',
    'jdk': 'the Java JDK folder',
    'android_keystore': 'the project\'s Android keystore',
    'android_key_alias': 'the key\'s alias in it',
    'android_keystore_pass': 'the file holding its password',
    'libhac': 'a tests\\libhac folder to copy LibHac from, for the matrix',
    'checker_inputs': 'the 12 checker inputs, in order',
    'session_extra': 'the compressed copies session_check also takes',
    'solid_files': 'the folder app_test real takes as HBP_TEST_SOLID_FILES',
    'matrix_work': 'the matrix\'s TESTDIR, which keeps its prepared title',
    'gui_large': 'an NSP that takes over 15 s to compress, for the Windows app\'s test',
    'linux_wsl': 'the WSL distribution build_release.py runs the Linux program in (left out: that check is skipped)',
    'switch_ip': 'the Switch\'s address on the network',
    'switch_ftp_port': 'the port of its FTP server',
}


def _main_checkout():
    try:
        out = subprocess.run(['git', '-C', TREE, 'worktree', 'list', '--porcelain'], capture_output=True, text=True).stdout
    except OSError:
        return None
    first = re.search(r'^worktree (.+)$', out, re.M)
    return os.path.normpath(first.group(1)) if first else None


def file_path():
    here = os.path.join(HERE, 'local.json')
    if os.path.exists(here):
        return here
    main = _main_checkout()
    there = os.path.join(main, 'tools', 'local.json') if main else None
    return there if there and os.path.exists(there) else here


def load(*needed):
    """This PC's settings, stopping with what's missing if any of `needed` isn't there."""
    path = file_path()
    if not os.path.exists(path):
        sys.exit(f"{path} is missing: copy tools/local.example.json to tools/local.json and put in this PC's paths.")
    with open(path, encoding='utf-8') as source:
        data = json.load(source)
    missing = [name for name in needed if data.get(name) in (None, '', [])]
    if missing:
        sys.exit(f"{path} needs " + '; '.join(f"{name} ({ENTRIES.get(name, '')})" for name in missing))
    return data


LONG_HEX = re.compile(r'[0-9a-fA-F]{32,}')


def safe(line):
    """A line fit to show: a run of 32 or more hex characters may be a key, so a line with one is hidden."""
    return '(a line with a long hex string, hidden)' if LONG_HEX.search(line) else line
