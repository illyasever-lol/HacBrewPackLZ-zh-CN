# hacBrewPackLZ in detail

Everything about the PC versions that the [README](../README.md) leaves out.

## Ways to use it

**The GUI** is `hacbrewpacklz-gui.exe`, for Windows. Drop game files or folders on it, pick them with **Add files** or **Add folder**, or use **Search a location** to look through a folder or drive and tick the ones you want. A game folder, with exefs, romfs and control in it, is built into an NSP like any other game. Then choose **Analyze**, to see what each would save, or **Compress**. The arrow on **Compress** has **Send to Switch** and **Extract to folders**. **Options** has the settings, and the **...** menu has checking an NSP. It follows Windows' light or dark setting and shows a long job on its taskbar button. Being drawn directly on the Windows API, it isn't visible to screen readers.

**The home screen** is what you get by double clicking `hacbrewpacklz.exe` or dropping files on it. It takes over the console window and stays open until you press Q. It draws itself to fit, so you can resize the window to anything without the text wrapping. Drop files or folders on it, or press **P** to type a path. Along the bottom are the keys for what comes next, such as **A** to analyze and **C** to compress, and **O** for the options. Everything else is under **M**, More: sending to a Switch, extracting, checking an NSP and help, each with a key that also works by itself. On Linux and Mac, `hacbrewpacklz menu` opens it.

**The command line** works as before, for scripts and for typing commands yourself. Its output stays in the scrollback, and is plain text when sent to a file or another program.

What you choose in the GUI or on the home screen is remembered in `%APPDATA%\hacBrewPackLZ\settings.txt`. The command line only goes by what you give it.

## Keeping it up to date

`hacbrewpacklz update` installs the latest version, if it's newer than the one you're running. In the GUI and on the home screen it's **Check for updates**, at the end of the options. **Automatic updates**, in the options, does it each time the GUI or the home screen opens, and is on to begin with. The new version takes over the next time it's opened.

Every release is signed, and the program won't install anything without the right signature, whatever the network or the download site does.

## Knowing the size beforehand

Analyze tells you how large the finished NSP will be, without writing anything:

```
hacbrewpacklz analyze -k prod.keys "D:\My Games"
```

```
Example Game [0100000000000000]: base v0
  Now:        4.19 GB
  After:      about 1.10 GB (give or take 74.1 MB)
  In bytes:   1181116006 +/- 77698662
  Saved:      3.09 GB (74%)
  Game data:  4.16 GB compresses to 1.07 GB (26%)
```

Everything but the compressed data is worked out exactly from the headers, so without compression the figure is right to the byte. For the compressed data, a spread of regions from across the game is really compressed, with the same encoder, level and block size the real run would use. The margin is measured from how much those regions differ, and should hold 19 times out of 20.

Every processor core keeps going between reading a region and compressing it, so neither ever waits for the other. Regions are read in order, so a hard drive sweeps across rather than jumping about, and one at a time, unless timing the first few shows that reading several at once gets more read, which is up to what it's stored on and not to what it says it is.

On a USB hard drive this takes about 2 seconds a title, for at least 128 regions. Anything faster gets a larger sample in about a second, which narrows the margin, and stops sooner if the answer's good to 2%. Five retail games against what really came out:

| Before | On a USB hard drive | From something faster | Actual |
|---|---|---|---|
| 73.3 MB | 36.8 MB | 36.8 MB | 36.8 MB |
| 2.81 GB | 2.17 GB, give or take 186 MB | 2.09 GB, give or take 58 MB | 2.14 GB |
| 3.88 GB | 1.95 GB, give or take 188 MB | 1.97 GB, give or take 59 MB | 1.98 GB |
| 4.19 GB | 1.12 GB, give or take 184 MB | 1.05 GB, give or take 44 MB | 1.07 GB |
| 5.75 GB | 5.54 GB, give or take 103 MB | 5.54 GB, give or take 78 MB | 5.56 GB |

Hashes aren't checked while analyzing, since that would mean reading everything, so a damaged file or an update on the wrong base game only shows up when you really convert it.

A solid NSZ can only be read from front to back, so analyzing one means going through the whole file once, which takes minutes rather than seconds: 4.5 minutes for a 16 GB game and its update, which estimated 12.17 GB, give or take 567 MB, and really came to 11.94 GB. An NSZ or XCZ will often come out as "larger", too: Zstandard compresses harder than LZ4, but LZ4 is what the console's firmware reads by itself, with no installer unpacking it first.

## Choosing what's worth compressing

Compressing takes time, and some games barely shrink. After analyzing, the titles that gain the most come first, so you can tick the ones you want and leave out the rest. In the GUI, click the circle beside a title, or the one at the top for all of them. On the home screen, Space ticks a title and T ticks or unticks them all. Removing a title from the list works too.

Two settings, under **Which titles are worth it** in the options, do this for you, to get the most space in the least time:

| Option | Meaning |
|---|---|
| `--minsaving 15` | Leave out titles that would save less than this percentage. Estimating takes seconds, so nothing much is lost finding out. The GUI and the home screen start at 5%, and the command line at none |
| `--hidesmall` | Leave out games under 1 MB with no update or DLC, which is what forwarders are |

A title you tick by hand gets done whatever the minimum says. With compression off, a title only comes out any smaller by having an update made part of it, so if any without one are ticked, the GUI and the home screen ask whether to untick them, repack them anyway, or leave things as they are. On the command line they work with `convert`, and `analyze` shows what they'd leave out and ranks what's left, with the command to compress just those:

```
hacbrewpacklz analyze -k prod.keys "D:\My Games" --minsaving 15 --hidesmall
hacbrewpacklz convert -k prod.keys "D:\My Games" --only 0100000000010000,0100000000020000
```

`--only` and `--skip` take a list of title IDs or parts of names.

## Converting and extracting

The easiest way is the GUI or the home screen, above. From a command line:

```
hacbrewpacklz convert -k prod.keys "Game [BASE].nsp" "Game [UPD].nsp" --verify
hacbrewpacklz convert -k prod.keys "D:\My Games" --nspdir "D:\Compressed"
hacbrewpacklz extract -k prod.keys "Game [BASE].nsp" "Game [UPD].nsp" --outdir "D:\Extracted"
```

- **What goes in:** NSP, NSZ, XCI and XCZ files, in any mix. The kind is worked out from the file itself, not its name. A base game can be an XCI and its update an NSZ, for example.
- **Batch:** folders are searched for those files, including subfolders. One failed title doesn't stop the others, and files this tool already compressed are skipped.
- **Base game and update:** given together, in any order or location, they become one title. The newest update wins. The tool works out which update belongs to which game from the files themselves, not their names.
- **DLC** is converted to its own NSP.
- **Cartridge images:** everything in an XCI's secure partition is used, so a cartridge holding a game, its update and DLC gives one NSP for the game with the update applied, and one for each DLC. A rebuilt NCA is marked as a download rather than a cartridge, so the result installs like any other NSP. Its system update partition and padding are left out, which accounts for much of the saving: a 1.86 GB dump came to 247 MB, where the same game uncompressed is 305 MB.
- **NSZ and XCZ** need no other tool and no extra steps. Both kinds of `nsz` compression work: block, and solid, which has to be read in order. A solid file needs memory for its window, up to about 256 MB as `nsz` normally writes them, or much more for one made with extreme settings. A game and its update can need each other out of order, and then the decompressed data goes to temporary files as well, in `--tempdir`, or the output folder by default: a 14.5 GB game with a 2 GB update used another 16.5 GB while it ran. Solid files are slower too: that pair took 32 minutes as solid NSZ against 23 as NSP, on a slow laptop with one USB hard drive.
- **Output** goes beside the input unless you use `--nspdir`, as `Title [TitleID][vVersion][LZ4].nsp`. The originals are never changed.
- **Extracting** writes `exefs`, `romfs`, `logo`, `control`, `htmldoc` and `legalinfo` folders, the same ones the build mode takes. With an update, you get the updated game's files.
- **Checks:** every block read from a source NSP is checked against its hash tree, and an update on the wrong base game stops with an error instead of producing a broken NSP.

What changes in a converted NSP:

- It needs no ticket: every NCA is re-encrypted with a key area, like hacBrewPack's output. It therefore needs sigpatches even where the original didn't.
- The content meta keeps the source's version and required firmware, raised to 12.0.0 when compressing.
- Game files are unchanged. `main.npdm` gets the same ACID key patch hacBrewPack applies, unless you use `--nosignncasig2`.

**Installing a repacked game:** uninstall the game's old update first, and don't install official updates over it afterwards. An official update is a patch against the original base game: it says which bytes to take from where in it, so it can't apply to a repacked one, and the game won't open with one installed. Compressing a game and installing its original update beside it doesn't work for that reason, which LibHac confirms with a hash error, and an update can't be compressed on its own. Give the two together, and they come out as one title.

Not supported: an update without its base game, a base game cut down to what its update reads from it without that update, and personalized tickets (add the key to a `title.keys` file beside your keyset instead).

## Usage

To build from folders, the options are the same as hacBrewPack's, so it works as a drop-in:

```
hacbrewpacklz -k prod.keys --exefsdir exefs --romfsdir romfs --logodir logo --controldir control --nspdir out --verify
```

That writes `out\<titleid>.nsp`, then re-reads it and compares every RomFS file with your `romfs` folder.

Run `hacbrewpacklz --help` for all options. The ones added to hacBrewPack's set:

| Option | Meaning |
|---|---|
| `--level N` | LZ4 level, 1 (fastest) to 12 (smallest). Default 9 |
| `--blocksize N` | Compression block: 4K, 8K, 16K, 32K or 64K. Default 64K |
| `--threads N` | Compression threads. Default all logical processors |
| `--verify` | Check the finished NSP against the source folders |
| `--nocompress` | Plain RomFS, which makes it a repack. Output NCAs are byte-identical to hacBrewPack's |
| `-o`, `--output` | Exact output path, instead of `--nspdir\<titleid>.nsp` |
| `--tempdir` | Where a solid NSZ or XCZ goes if it has to be gone back to. Nothing else needs it. Default the output folder |
| `--titleversion` | Title version in the content meta, in hex |
| `--requiredsystemversion` | Required firmware in the content meta, in hex. Default `30000000` (12.0.0) when compressing, otherwise 0 |

To check any NSP later, including comparing it with a folder:

```
hacbrewpacklz verify -k prod.keys game.nsp --romfsdir romfs --exefsdir exefs
```

### Requirements on the console

- **Firmware 12.0.0 or newer.** Older firmware has no compression layer, so the content meta asks for 12.0.0 by default.
- **Sigpatches**, exactly as with hacBrewPack: the NCA and ACID signatures cannot be valid without Nintendo's keys.

### Disk space and memory

You need room for the finished NSP and nothing else: compressed data goes straight into it as it's made, without a temporary file. It says so before it starts if there isn't enough, or if the NSP wouldn't fit on a FAT32 disk, rather than running out part way through. A killed build can leave a `<titleid>.nsp.part` file, which the next build overwrites. Files are streamed, so memory use stays low whatever the size of the game. A solid NSZ or XCZ can need more of both, as described under [Converting and extracting](#converting-and-extracting).

The disk gets read and written as little as it can be. A title is read once, its compressed data written once, and it's all read back once at the end, which is what its hash takes, as that covers the header, and the header has to say how the data came out. That read also checks what was written against a hash taken just before it went to the disk.

## How it works

On a read, the firmware's layers run in this order: AES-CTR, then the IVFC hash tree, then decompression. So the section is built from the top down:

```
NCA section (AES-CTR)
└─ IVFC data level
│  ├─ compressed blocks          LZ4 or raw, each aligned to 0x10
│  └─ BKTR table                 16 KiB node tree over 0x18 byte entries
│                                virtual offset → physical offset, type, size
└─ IVFC hash levels 0-4          hashes of the data level
```

The FS header's `CompressionInfo` (offset 0x178) points at the table, and the virtual size is the size of the ordinary RomFS image. The hash levels come after the data, where retail games have them before it, as their size depends on how far the data compressed. The header says where each level is, and that's what readers go by, so the data can go straight to where it'll stay as it's made. Limits this tool keeps to, from `fssystem::CompressedStorage`:

- A compressed block is at most 64 KiB on disk, which is why blocks are capped at 64 KiB.
- Compressed blocks start on a 0x10 boundary.
- Blocks that do not shrink are stored raw. Neighbouring raw blocks share one entry, because raw entries are random access and have no size limit.
- The all-zero entry type is not used. LZ4 handles zeros nearly as well, and it avoids an odd firmware path that requires such entries to have a non-zero physical size.

Every compressed block is decoded again and compared with its input before it is accepted, so a compressor fault cannot alter game data.

## Choosing settings

The defaults are the best trade-off in testing. Results for a 660 MB RomFS from a real game build, on a 2 core laptop:

| Settings | NSP size | Time |
|---|---|---|
| `--nocompress` (same as hacBrewPack) | 649.4 MB | 18 s |
| `--level 1` | 561.7 MB | 21 s |
| `--level 9` (default) | 549.2 MB | 25 s |
| `--level 12` | 548.7 MB | 245 s |
| `--level 9 --blocksize 32K` | 553.4 MB | 22 s |
| `--level 9 --blocksize 16K` | 558.4 MB | 21 s |

How much you save depends on the game. Assets that are already compressed (video, audio, many engine archives) cannot shrink further and are stored raw; that particular game saved 15%. Executable code came down to about 55% in testing, on a par with the reference LZ4HC encoder (within 0.2%).

Smaller blocks cost some size but mean less decoding for small reads, which could help a game that makes many tiny reads. Level 12 is rarely worth the time. Level 1 is for quick test builds.

## Other tools

`hactool` and `nstool` do not understand the compression layer and report the RomFS section as corrupt. This also happens with retail NCAs that use compression, and it does not mean the NSP is bad. LibHac based tools (`hactoolnet`, Ryujinx) read these NCAs, and so does `hacbrewpacklz verify`.

Only Application NSPs are built, as with hacBrewPack. Update and DLC NSPs are not supported.

## Testing on a console

`selftest\` holds a small title that tests the compression layer on real hardware. Building it needs devkitA64 and Python:

```
selftest\build.ps1 -Keyset <prod.keys> -Tool <path to hacbrewpacklz.exe>
```

That writes `selftest\hacbrewpacklz-selftest.nsp`. Install it like any other NSP and launch "hacBrewPackLZ self-test". It reads its own compressed RomFS through the console's real FS driver, using sequential, unaligned, very large and random reads, and checks every byte against a known pattern. The screen ends with `RESULT: PASS` or `RESULT: FAIL`, and the same text is saved to `sdmc:/hacbrewpacklz_selftest.log`. A failed read shows the firmware's error code (for example `2002-xxxx`).

## Building

On Linux, run `chmod +x hacbrewpacklz-linux-x64` first. The Mac builds are unsigned, and macOS won't run an unsigned program on Apple silicon. Sign your copy once, in the folder that holds it, using the file name you downloaded:

```
xattr -d com.apple.quarantine hacbrewpacklz-macos-arm64; chmod +x hacbrewpacklz-macos-arm64; codesign -s - hacbrewpacklz-macos-arm64
```


```
dotnet publish hacbrewpacklz.csproj -c Release -r win-x64 -o <output folder>
```

This gives one native `.exe` with no .NET dependency. It needs the .NET SDK and the Visual Studio C++ linker, and the folder holding `vswhere.exe` (`C:\Program Files (x86)\Microsoft Visual Studio\Installer`) must be on `PATH`. `dotnet build -c Release` is enough for testing and needs only the SDK.

The GUI is built the same way, for Windows only:

```
dotnet publish gui\hacbrewpacklz-gui.csproj -c Release -r win-x64 -o <output folder>
```

Native compilation can't target Linux or Mac from Windows, so those builds bundle the .NET runtime instead, which needs only the SDK. Use `-r linux-x64`, `-r osx-x64` or `-r osx-arm64`:

```
dotnet publish hacbrewpacklz.csproj -c Release -r linux-x64 -o <output folder> -p:PublishAot=false -p:SelfContained=true -p:PublishSingleFile=true -p:PublishTrimmed=true -p:EnableCompressionInSingleFile=true
```

The Switch version is built with devkitPro, by running `make` in `switch`. What it takes is on [its details page](../switch/DETAILS.md#building).

A release gets signed with `tools\sign_release.py`, which writes the two files the updaters look for, to go with it.

The format details come from reading [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)'s `fssystem` sources, [Switchbrew](https://switchbrew.org/wiki/NCA) and hacBrewPack. RomFS and PFS0 layouts deliberately match hacBrewPack, whose output is known to work on hardware.
