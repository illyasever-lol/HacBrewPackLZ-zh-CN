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

## Running them together

`tools\check.py` builds what the tests need and runs them, then prints one table of what passed. It reads this PC's paths (keys, test games, the folder for output) from `tools\local.json`, which `tools\local.example.json` describes.

```
python tools\check.py quick --only LOG --seeds 1 --session 1 --matrix engine
python tools\check.py full
python tools\check.py setup
```

- `quick` runs only what it's given: app_test real with some `HBP_TEST_ONLY_` switches, app_test check seeds, session_check seeds, matrix sections, the window tests. It builds only what changed since last time. With `--real` it runs `replace_test --once` on a game with two programs as well.
- `full` builds the Windows programs as they ship, the Switch app and its PC build. Then it runs:
  - app_test demo, real and check seeds 1 to 4;
  - `replace_test --once` on a game with two programs;
  - session_check plain, versions and seeds 1 to 4;
  - every matrix section;
  - `look_check.py`;
  - last, the two window tests.
- `setup` does the setup above, copying LibHac from another working copy. The other two run it by themselves when the matrix needs it.

`replace_test` and the DLC checks read files from `make_patch_test.py`'s `programs` and `dlcs` variants (see `batch` below). `check.py` makes any that are missing, in local.json's `dlc_inputs` or the round folder, and a full check fails if it can't, `--tree` on an older copy included.

`python tools\check.py --help` lists every option.

## Sections

```
python tests\matrix.py batch analyze selection titles formats zstd engine diff options edges matrix l2
```

| Section | What it proves |
|---|---|
| `batch` | Converting, merging and extracting. `make_patch_test.py` generates a base game, update and DLC in the retail layout (ticket, AES-CTR-Ex regions, indirect table), with plain and compressed variants, and no game files are needed. It also makes the two kinds of file that used to get refused: a base game cut down to what its update still reads from it, and an update with no data of its own and an empty AES-CTR-Ex table. Merged files must match both the new source files and LibHac's own patching. Also covers updates on the wrong base, updates alone, folders, and stopping a build part way. Its `dlcs` variant adds two DLC too small to compress and a merged file of everything. A game's DLC has to go into its file, and DLC without their game into one file, or with `--separatedlc` into files of their own, with none left out. That holds for a merged file, for a DLC from an older compressed copy, and for a compressed copy without its DLC, which is made again with it. Given with its original and its DLC in files of their own, that copy is kept. With `--separatedlc`, a DLC only inside the merged original gets a file of its own beside the copy. The room check counts all of it, and stopping part way leaves nothing. The `programs` variant writes only `programs.nsp`, a collection with two programs in it that both hold `data/same.bin`, for `replace_test --once` (see `switch\DETAILS.md`) |
| `analyze` | The size estimate against what really gets written: exact to the byte wherever nothing needs guessing, including without compression, and otherwise within its own margin. It also writes nothing |
| `selection` | The minimum saving, `--hidesmall`, `--only` and `--skip`: what gets left out, what gets said about it, and the ranking from `analyze` |
| `titles` | Names stored compressed in a `control.nacp`, as they can be since firmware 21, on both engines: every kind of Deflate block, languages the old format had no room for, damaged streams and formats nobody knows, and all the way through from folders to an NSP to the name it's listed under |
| `formats` | `make_game_files.py` turns the generated titles into solid and block NSZ, XCI with and without a key area, and solid and block XCZ, using the real Zstandard library. Whatever form a game arrives in, the same NSP must come out byte for byte, the same files must be extracted, and damaged or cut short files must be refused |
| `zstd` | The Zstandard decoder against files from the real library, covering every level, checksums, long distance matching, threads, several frames and many small blocks, whole and as a stream. Then damaged copies, which must be refused or decoded without crashing. Build `zstd_check` first |
| `engine` | The C++ engine of the Switch version against this tool, on every format, level and block size: the same input has to give the same bytes, the same exact estimates and the same extracted files, and the same NSP has to come out of building from those files. Set `HBPTOOL` to its PC build, see `switch\DETAILS.md` |
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

## The list checked against itself

```
dotnet build tests\session_check -c Release -o <folder>
dotnet <folder>\session_check.dll <prod.keys> <work folder> <seed> <steps> <game files>...
```

The list the GUI, the home screen and Android share is driven at random from the seed: compression and the minimum saving changed, compressed games shown and hidden, games ticked, analyzed and compressed. After every step, a fresh session with the same files and settings has to show the same list, every row has to come to what converting it really comes to, and games asked about as gaining nothing have to be the same size. A row whose estimate read all of its game data, so has no margin, has to be as close as `matrix.py` holds small titles to, or smaller by what compressing found held again, which estimates don't look for. Give it a game with an update and DLC, one that compresses well, and the compressed copy of that one, a game whose update holds part of it more than once, from `python tests\make_patch_test.py <prod.keys> <hacbrewpacklz.exe> <title folder> <out folder> shared`, small enough to be analyzed whole, the same with `repeats`, which also holds some of its own data again, and a game compressed by Nintendo with its update, such as ANIMAL WELL (the two files, not a folder with a compressed copy beside them, which would take their place). It writes only in the work folder, with its own settings. The Switch app has the same check, `app_test ... check`, described in `switch\DETAILS.md`, which takes the same files after its first four. Before its random steps, session_check checks Game and DLC in one file, from the `dlcs` files in the folder `HBP_TEST_DLC_INPUTS` names (`check.py` sets it, and makes the files first if they aren't there). A game, its update and a DLC with nothing to compress come out as one file, or as two with the setting off, and that file opened again from its folder is one compressed row under Done. A DLC this tool compressed before goes into the game's file with the setting on, and is left as it is with it off. DLC without their game come out as one file. A copy made without its DLC, given with one, is to do and says "1 DLC not in its file", and doing it makes one file holding the game and the DLC. Given with its original as well, it's kept, and analyzed it's under Done. With the setting off, a DLC only inside a merged original keeps the copy's row to do until doing it makes that DLC a file. Without the variable, it says that check was skipped.

## The console screen and the GUI

These open real windows for a while, so they aren't part of the matrix.

```
python tests\console_screen.py <hacbrewpacklz.exe> <folder with prod.keys> <small.nsp> [folder for screenshots] [--large=<big.nsp>]
python tests\gui_shots.py <hacbrewpacklz-gui.exe> <folder with prod.keys> <small.nsp> <large.nsp> <work folder> <folder for screenshots> [paths to list]...
python tests\gui_drop.py <hacbrewpacklz-gui.exe> <folder with prod.keys> <small game file> [folder for screenshots]
```

`console_screen.py` opens the home screen in a real console window, drops a path on it, analyzes and compresses, and resizes the window from large to tiny, checking every time that nothing wrapped or reached the edge. With a big NSP, one that takes a while to compress, it stops a job part way through too. Every frame the program draws goes in a log (`HACBREWPACKLZ_SCREEN_LOG`), and no row may be cleared before it's written, as the row of a game being compressed flickered when it was. `gui_shots.py` walks through every window of the GUI with clicks sent directly to it, including a real job, Stop, and closing the window part way through, and saves a screenshot of each state to look over. The program must end with exit code 0 every time. `gui_drop.py` drags a file onto the GUI with a real OLE drag, which briefly moves the mouse pointer. They use temporary copies and their own settings, so they don't touch your files or your options.

The Switch version has tests of its own, including one that pulls the plug at every step of replacing a game: see `switch\DETAILS.md`.

## Reporting a problem

```
python tests\report_test.py <hacbrewpacklz.exe> <work folder>
python tests\problems_test.py <hacbrewpacklz.exe> <hacbrewpacklz-gui.exe> <folder with prod.keys> <small.nsp> <work folder>
python report-service\test\run_tests.py
```

`report_test.py` sends reports from the command line to a stand-in report service on this PC, and checks what arrives and what the person is told: sent, a known problem with the version that fixes it, and saved to a file when the service turns it away, can't be reached, or there's nowhere to send it. `problems_test.py` does the same for problems the app finds itself: a game that can't be done, sent once a version and only counted after that; the GUI and the home screen stopping on purpose, and the GUI not answering until it's closed, each sent the next time it opens; and nothing at all without agreeing to it, or once it's turned off. The GUI and the home screen open for a moment, behind whatever's in use. `run_tests.py` runs the report service's own tests in Edge, against an inbox kept in memory. The tests set `HACBREWPACKLZ_REPORT_SITE` (`HBP_TEST_REPORT_SITE` on the Switch) to a stand-in or to `none`, so nothing reaches the real service.

## Updating a compressed game

```
python tests\update_chain.py <hacbrewpacklz.exe> <prod.keys> <base> <older update> <newer update> <work folder> [hbptool.exe]
```

Needs a real game with two updates. The base with the older update makes a compressed copy, and that copy with the newer update alone has to come to the same game data as the base with the newer update, read back by LibHac (the code, for a game with no game data). With `hbptool`, the Switch engine has to make the same files byte for byte. For a game its makers compressed, it has to say the original game is needed instead.

## Updating

```
python tests\update_test.py <hacbrewpacklz.exe> <private key.pem> <work folder>
```

Serves a release from this PC, signed with the real key, to a copy of the tool, which has to replace itself with it and still run. Then a bad signature, a changed list of what's in the release, a damaged download and an older version, none of which may change anything.

A few environment variables help with this kind of testing: `HACBREWPACKLZ_SETTINGS` points at another settings file, `HACBREWPACKLZ_UPDATE_SITE` at another place to get releases from, and for the GUI, `HACBREWPACKLZ_GUI_THEME=light` or `dark` and `HACBREWPACKLZ_GUI_DPI=144` force a look without changing Windows' settings. `HACBREWPACKLZ_TEST_SLOW_OPEN=8000` makes opening a game for Details or Look inside take 8 s longer, as a busy disk does. `HACBREWPACKLZ_TEST_FREE_SPACE=<bytes>` makes the room check before a game is written see only that much free space.
