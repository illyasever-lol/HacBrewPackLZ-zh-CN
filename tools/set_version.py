"""Sets the version everywhere it's written: the PC programs, Android and the Switch app.

    python tools/set_version.py 1.5.0           a release
    python tools/set_version.py 1.5.0-test.1    a test version of it

A test version is the release's number with -test.N after it, N from 1 to 98. Every version's update check counts it as coming after the test
versions before it and before the release with its number, so testers get each new test version and then the release. Test versions are
published in the same project, dated just before its newest release (see publish_release.py), so only a test version's copy looks at them.
Windows wants plain numbers in the GUI's manifest, so that has the release's number whatever the version is.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main():
    if len(sys.argv) != 2 or not re.fullmatch(r'\d+\.\d+\.\d+(-test\.([1-9]|[1-8][0-9]|9[0-8]))?', sys.argv[1]):
        print(__doc__)
        return 2
    version = sys.argv[1]
    release = version.split('-')[0]
    places = [
        ('src/Ui.cs', r'(public const string Version = ")[^"]*(";)', version),
        ('hacbrewpacklz.csproj', r'(<Version>)[^<]*(</Version>)', version),
        ('gui/hacbrewpacklz-gui.csproj', r'(<Version>)[^<]*(</Version>)', version),
        ('gui/app.manifest', r'(<assemblyIdentity version=")[^"]*(")', release + '.0'),
        ('android/hacbrewpacklz-android.csproj', r'(<Version>)[^<]*(</Version>)', version),
        ('switch/app/app.h', r'(constexpr const char\* AppVersion = ")[^"]*(";)', version),
        ('switch/Makefile', r'(APP_VERSION\t:=\t)\S+()', version),
        # What a compressed copy says made it, which both engines write the same, so their copies come out byte for byte the same.
        ('switch/core/title.h', r'(constexpr const char\* EngineVersion = ")[^"]*(";)', version),
    ]
    for path, pattern, value in places:
        full = ROOT / path
        # As it is, line endings and all.
        with open(full, encoding='utf-8', newline='') as f:
            text = f.read()
        text, count = re.subn(pattern, lambda m: m.group(1) + value + m.group(2), text, count=1)
        if count != 1:
            print(f"{path}: couldn't find where the version goes")
            return 1
        with open(full, 'w', encoding='utf-8', newline='') as f:
            f.write(text)
        print(f'{path}: {value}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
