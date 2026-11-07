# Tests

These check the tool against independent implementations instead of against itself. They need Python with the `cryptography` and `zstandard` packages, the .NET SDK, `hacbrewpack.exe`, a keyset and a copy of Ryujinx.

## Setup

LibHac is the independent NCA reader. It comes out of a Ryujinx release, so nothing else needs downloading:

```
python tests\bundle_extract.py <path to Ryujinx.exe> tests\libhac LibHac
dotnet build tests\libhac_check\libhac_check.csproj -c Release
dotnet build tests\zstd_check\zstd_check.csproj -c Release
python tests\matrix.py prepare
```

By default the tests expect `..\_tools\hacbrewpacklz\hacbrewpacklz.exe`, `..\_tools\hacbrewpack\hacbrewpack.exe` and `prod.keys` beside the latter. Set `TOOL`, `HACBREWPACK`, `KEYS` and `TESTDIR` to use other paths. `TESTDIR` defaults to `tests\out`.

## Sections

```
python tests\matrix.py batch analyze selection titles formats zstd engine diff options edges matrix l2
```

| Section | What it proves |
|---|---|
| `batch` | Converting, merging and extracting. `make_patch_test.py` generates a base game, update and DLC in the retail layout (ticket, AES-CTR-Ex regions, indirect table), with plain and compressed variants, and no game files are needed. It also makes the two kinds of file that used to get refused: a base game cut down to what its update still reads from it, and an update with no data of its own and an empty AES-CTR-Ex table. Merged files must match both the new source files and LibHac's own patching. Also covers updates on the wrong base, updates alone, folders, and stopping a build part way |
| `analyze` | The size estimate against what really gets written: exact to the byte wherever nothing needs guessing, including without compression, and otherwise within its own margin. It also writes nothing |
| `selection` | The minimum saving, `--hidesmall`, `--only` and `--skip`: what gets left out, what gets said about it, and the ranking from `analyze` |
| `titles` | Names stored compressed in a `control.nacp`, as they can be since firmware 21, on both engines: every kind of Deflate block, languages the old format had no room for, damaged streams and formats nobody knows, and all the way through from folders to an NSP to the name it's listed under |
| `formats` | `make_game_files.py` turns the generated titles into solid and block NSZ, XCI with and without a key area, and solid and block XCZ, using the real Zstandard library. Whatever form a game arrives in, the same NSP must come out byte for byte, the same files must be extracted, and damaged or cut short files must be refused |
| `zstd` | The Zstandard decoder against files from the real library, covering every level, checksums, long distance matching, threads, several frames and many small blocks, whole and as a stream. Then damaged copies, which must be refused or decoded without crashing. Build `zstd_check` first |
| `engine` | The C++ engine of the Switch version against this tool, on every format, level and block size: the same input has to give the same bytes, the same exact estimates and the same extracted files, and the same NSP has to come out of building from those files. Set `HBPTOOL` to its PC build, see `switch\README.md` |
| `diff` | With `--nocompress`, every NCA is byte-identical to hacBrewPack's across its options. Input files stay untouched. The patched NPDM and NACP match hacBrewPack's, and NCA signature 2 verifies against the patched ACID key |
| `options` | Crypto, key generation and title options work with compression on, and bad input is rejected cleanly |
| `edges` | Empty RomFS, tiny files, all zeros, raw runs over the 64 MiB cap, Unicode names, 6000 files |
| `matrix` | Every level and block size, read back through LibHac and compared with the uncompressed hacBrewPack RomFS |
| `l2` | A 5.9 GB sparse file at 4K blocks, which is what it takes to need the second level of the BKTR tree |

Every compressed build is checked twice, by `hacbrewpacklz verify` and by LibHac.

## Limits of these tests

LibHac 0.19's `LibHac.FsSystem.CompressedStorage`, the port of Nintendo's class, is an unimplemented stub. What actually reads the data there is a simpler reader of its own, on top of a real port of `BucketTree`. So the tests prove the format, the table and the contents, but not the firmware's own cache and read-splitting code. `selftest\` exists to cover that on a console.

`host_selftest\switch.h` lets `selftest\source\main.c` run on a PC to check the test program's own logic. Dump a decompressed image with `libhac_check --dump-image`, then build with clang, passing this folder with `-I`.

`dump_nca.py` prints the decrypted header fields of an NCA for debugging.

## The console screen and the GUI

These open real windows for a while, so they aren't part of the matrix.

```
python tests\console_screen.py <hacbrewpacklz.exe> <folder with prod.keys> <small.nsp> [folder for screenshots]
python tests\gui_shots.py <hacbrewpacklz-gui.exe> <folder with prod.keys> <small.nsp> <large.nsp> <work folder> <folder for screenshots> [paths to list]...
python tests\gui_drop.py <hacbrewpacklz-gui.exe> <folder with prod.keys> <small game file> [folder for screenshots]
```

`console_screen.py` opens the home screen in a real console window, drops a path on it, analyzes and compresses, and resizes the window from large to tiny, checking every time that nothing wrapped or reached the edge. `gui_shots.py` walks through every window of the GUI with clicks sent directly to it, including a real job, Stop, and closing the window part way through, and saves a screenshot of each state to look over. The program must end with exit code 0 every time. `gui_drop.py` drags a file onto the GUI with a real OLE drag, which briefly moves the mouse pointer. They use temporary copies and their own settings, so they don't touch your files or your options.

The Switch version has tests of its own, including one that pulls the plug at every step of replacing a game: see `switch\README.md`.

## Updating

```
python tests\update_test.py <hacbrewpacklz.exe> <private key.pem> <work folder>
```

Serves a release from this PC, signed with the real key, to a copy of the tool, which has to replace itself with it and still run. Then a bad signature, a changed list of what's in the release, a damaged download and an older version, none of which may change anything.

A few environment variables help with this kind of testing: `HACBREWPACKLZ_SETTINGS` points at another settings file, `HACBREWPACKLZ_UPDATE_SITE` at another place to get releases from, and for the GUI, `HACBREWPACKLZ_GUI_THEME=light` or `dark` and `HACBREWPACKLZ_GUI_DPI=144` force a look without changing Windows' settings.
