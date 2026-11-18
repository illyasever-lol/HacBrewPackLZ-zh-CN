# Contributing to hacBrewPackLZ

## Reporting a problem

Use **Report a problem**, under **Help** in any version of the app. It collects the version, your system, your settings, the games on the list and what the app said, and none of your keys. [Reporting a problem](docs/help.md#reporting-a-problem) says where it goes and where the file is saved when it can't be sent.

To post it yourself, open a [new issue](https://gitlab.com/ismaellozanoperezserrano/hacbrewpacklz/-/issues/new) with the **Problem** template and attach the report. Never paste your `prod.keys`, `title.keys` or any key, in an issue or anywhere else.

A security problem goes in a confidential issue instead: see [SECURITY.md](SECURITY.md).

## Asking for something new

Open an issue with the **Idea** template. Say what you'd like to do, and on which version: the Windows app, the home screen and command line, Android or the Switch.

## Building and testing

- The PC programs and Android: [Building](docs/details.md#building).
- The Switch app: [Building](switch/DETAILS.md#building).
- The tests: [tests/README.md](tests/README.md). Most of them need your own `prod.keys` and some game files, which stay on your computer.

## Making a change

hacBrewPackLZ comes as a Windows app, a home screen and command line for Windows, Mac and Linux, an Android app and a Switch app. The PC programs and Android share the engine in `src/`. The Switch has its own, in C++, in `switch/core/`.

- **A change in one version goes into the others,** adapted to each: right-click and double-click on Windows, keys on the home screen, tap and long press on Android, the + menu and buttons on the Switch. The words stay the same everywhere.
- **Both engines make the same files, byte for byte.** A change to what's written goes into both, and `tests/matrix.py engine` compares them.
- **Never commit keys, game files** (`.nsp`, `.nsz`, `.xci`, `.xcz`, `.nca`, `.ncz`) **or console dumps.** `.gitignore` keeps most of them out.
- **Plain words** on screen and in the docs. An error on a list is a few words with what to do next, and the full error goes behind Details or in the log.
- **Commit messages** say what changed for people.

`main` is the newest release. Work for the next version goes on its `release-x.y` branch, so a merge request goes there.

## License

hacBrewPackLZ is under the GPL, version 2, and anything you contribute is under it too. See [LICENSE](LICENSE).
