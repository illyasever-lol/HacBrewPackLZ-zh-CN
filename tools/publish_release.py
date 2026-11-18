"""Signs a release, writes its notes, and puts it on GitLab with every file named for who it's for.

    python tools\\publish_release.py <private key.pem> <version> <folder> <what's new.md> [--publish]

<folder> holds the files to release under their usual names (see FILES below). <what's new.md> is a short list of
what changed, in Markdown. This signs them (see sign_release.py), and writes release-notes.md into <folder>, with the
download links first, what's new, and the rest folded away. Read it over, then run it again with --publish, which
creates the release with glab. Push the commit and its tag before publishing (main for a release, a branch of its own for a test
version): glab otherwise tags GitLab's own main.
"""
import hashlib
import json
import os
import subprocess
import sys
import urllib.request
from datetime import datetime, timedelta, timezone

from sign_release import sign

PROJECT = 'ismaellozanoperezserrano/hacbrewpacklz'
# Test versions, such as 1.5.0-test.1, go here too, dated just before the newest release. Every copy but a test version's, and every copy before
# 1.5, only asks for the newest release by date, and so do the README's links to the latest release, so none of them ever sees a test version;
# a test version's copy looks through every release.
RELEASES_API = f"https://gitlab.com/api/v4/projects/{PROJECT.replace('/', '%2F')}/releases?per_page=100"


def is_test(version):
    return '-test.' in version


def dated_before_newest():
    """A second before the newest release that isn't a test version, as GitLab takes it."""
    request = urllib.request.Request(RELEASES_API, headers={'User-Agent': 'hacBrewPackLZ release tool'})
    with urllib.request.urlopen(request) as response:
        releases = json.load(response)
    dates = [datetime.fromisoformat(r['released_at'].replace('Z', '+00:00')) for r in releases if not is_test(r['tag_name'])]
    if not dates:
        sys.exit('There is no release to date a test version before.')
    return (max(dates) - timedelta(seconds=1)).astimezone(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


GLAB = os.path.expandvars(r'%LOCALAPPDATA%\Programs\glab\glab.exe')

# In the order they're listed, with the name the Releases page gives each. The updaters fetch files by these file
# names, so they mustn't change; the names shown can.
FILES = [
    ('hacbrewpacklz-gui.exe', 'Windows', 'package'),
    ('hacBrewPackLZ.nro', 'Switch (copy it into the switch folder on your SD card)', 'package'),
    ('hacbrewpacklz-android.apk', 'Android', 'package'),
    ('hacbrewpacklz-macos-arm64', 'Mac with an Apple chip (M1 or newer)', 'package'),
    ('hacbrewpacklz-macos-x64', 'Mac with an Intel processor', 'package'),
    ('hacbrewpacklz-linux-x64', 'Linux', 'package'),
    ('hacbrewpacklz.exe', 'Windows, for a console window and the command line', 'package'),
    # GitLab wants every link of a release to have a name of its own.
    ('hacbrewpacklz-update.txt', 'For the built-in updater: what is in this release (not needed)', 'other'),
    ('hacbrewpacklz-update.sig', 'For the built-in updater: its signature (not needed)', 'other'),
]


def notes(version, folder, whats_new):
    url = lambda name: f'https://gitlab.com/{PROJECT}/-/releases/v{version}/downloads/{name}'
    readme = f'https://gitlab.com/{PROJECT}#download'
    sums = []
    for name, _, _ in FILES:
        with open(os.path.join(folder, name), 'rb') as source:
            sums.append(f'{hashlib.sha256(source.read()).hexdigest()}  {name}')
    release = version.split('-test.')[0]
    test = f"""A test version of {release}, to try it before it's out. It's listed below the newest release, and only copies of a test version look for it: they update to each new test version, and then to {release} once it's out.

""" if is_test(version) else ''
    settled = 'From then on, it updates itself.' if is_test(version) else 'Already have it? It updates itself.'
    return f"""{test}**Download:** [Windows]({url('hacbrewpacklz-gui.exe')}) · [Switch]({url('hacBrewPackLZ.nro')}) · [Android]({url('hacbrewpacklz-android.apk')}) · Mac with an [Apple chip]({url('hacbrewpacklz-macos-arm64')}) or [Intel]({url('hacbrewpacklz-macos-x64')}) · [Linux]({url('hacbrewpacklz-linux-x64')})

What to do with it once it's downloaded is in the [README]({readme}). {settled}

## What's new

{whats_new.strip()}

<details>
<summary>Which file is which</summary>

| File | For |
|---|---|
| `hacbrewpacklz-gui.exe` | Windows. Put `prod.keys` beside it |
| `hacBrewPackLZ.nro` | Switch. Copy it into the `switch` folder on your SD card |
| `hacbrewpacklz-android.apk` | Android 8 or newer, 64-bit |
| `hacbrewpacklz-macos-arm64` | Mac with an Apple chip |
| `hacbrewpacklz-macos-x64` | Mac with an Intel processor |
| `hacbrewpacklz-linux-x64` | Linux |
| `hacbrewpacklz.exe` | Windows, for a console window and the command line |
| `hacbrewpacklz-update.txt`, `.sig` | The built-in updater. Not needed |

You need your own `prod.keys`, and on the Switch, firmware 12.0.0 or newer with sigpatches.

</details>

<details>
<summary>SHA-256</summary>

```
{chr(10).join(sums)}
```

</details>
"""


def main(args):
    publish = '--publish' in args
    args = [a for a in args if a != '--publish']
    if len(args) != 4:
        sys.exit(__doc__)
    key, version, folder, whats_new_path = args
    programs = [name for name, _, kind in FILES if kind == 'package']
    missing = [name for name in programs if not os.path.isfile(os.path.join(folder, name))]
    if missing:
        sys.exit(f"Not in {folder}: {', '.join(missing)}")
    sign(key, version, folder, [os.path.join(folder, name) for name in programs])
    with open(whats_new_path, encoding='utf-8') as source:
        text = notes(version, folder, source.read())
    notes_path = os.path.join(folder, 'release-notes.md')
    with open(notes_path, 'w', encoding='utf-8', newline='\n') as out:
        out.write(text)
    print(f'Notes are in {notes_path}')
    if not publish:
        print('Read them over, then run this again with --publish.')
        return
    tag = f'v{version}'
    remote = subprocess.run(['git', 'ls-remote', '--tags', f'https://gitlab.com/{PROJECT}.git', tag], capture_output=True, text=True)
    if tag not in remote.stdout:
        sys.exit(f'{tag} is not on GitLab yet. Push its commit and the tag first.')
    # The Releases page lists the newest link first, so they go up last to first.
    assets = [f'{os.path.join(folder, name)}#{label}#{kind}' for name, label, kind in reversed(FILES)]
    # A test version is dated a second before the newest release, so the newest by date stays that release.
    title = f"hacBrewPackLZ {version.replace('-test.', ' test ')}"
    dated = ['--released-at', dated_before_newest()] if is_test(version) else []
    subprocess.run([GLAB, 'release', 'create', tag, '-R', PROJECT, '--name', title, '-F', notes_path, *dated, *assets], check=True)


if __name__ == '__main__':
    main(sys.argv[1:])
