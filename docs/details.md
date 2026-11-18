# hacBrewPackLZ in detail

Everything about the PC versions that the [README](../README.md) leaves out.

## Ways to use it

**The GUI** is `hacbrewpacklz-gui.exe`, for Windows. The first time it opens, in place of the list until **Start**, it says what it's for and asks the few things most people want a say in: where NSPs go, whether to check each one, the minimum saving, automatic updates, and sending problem reports, which is off unless you turn it on, all of which are under **Options** too. Whoever had the welcome before problem reports were on it is asked about them once. The home screen does the same. Drop game files or folders on it, or use **Add** at the top to pick files or a folder, or to search a folder or drive and tick the ones you want. A game folder, with exefs, romfs and control in it, is built into an NSP like any other game. Along the top of the list, **To do**, **Done** and **Everything**, with how many games each has, choose what it shows, as on the Switch: Done is what's compressed, whether it already was when it was added, such as the copy this tool made beside its original, or it was compressed here, and what's settled as not worth it, read in full and saving less than the minimum or with nothing to gain, each saying why. With compression off nothing is done, so there's no Done. Each saving has a bar beside it showing how much smaller it comes out. The bar below the list says how it's going and has what to do next: **Analyze**, to see what each would save, and **Compress**, which becomes **Send to Switch** once everything ticked is compressed, with **Send to Switch** beside it at any time. Compressing shows only the games it's doing, how each is going and all of it together, and then what came of each, those that couldn't be done first with why. **Back to the list** goes back, where what was compressed is under Done and what wasn't is still to do. A game that couldn't be done says what went wrong in a few plain words under its name, such as "Damaged: dump or download it again.", and the error itself is in its **Details**, behind the info button at the end of its row: everything about the game, with **Look inside**, **Extract** and **Check for damage**. Right clicking a game has those a click sooner. The arrow beside **Compress** extracts the ticked games, checks them for damage, analyzes them again or removes them from the list. **Options** has the settings, and the **...** menu has clearing the list, **Help**, **Report a problem**, checking for a new version and About. Help has a few lines on each of the usual problems, as in [Help](help.md), and **Report a problem**, which sends a report of what happened straight to the developer, with no keys in it, or saves it on the desktop when it can't be sent. Each of these, like searching a location, opens in the window in place of the list, with an arrow at the top, or Esc, to go back to it. It follows Windows' light or dark setting and shows a long job on its taskbar button. Being drawn directly on the Windows API, it isn't visible to screen readers.

**The home screen** is what you get by double clicking `hacbrewpacklz.exe` or dropping files on it. It takes over the console window and stays open until you press Q. It draws itself to fit, so you can resize the window to anything without the text wrapping. Drop files or folders on it, or press **P** to type a path. The top of the list says **To do**, **Done** and **Everything**, with how many games each has, and left and right go between them, keeping the pointer on the same game, or starting at the top when that view doesn't have it. Under the list, two lines say what the row of the game the pointer's on has no room for, such as why it couldn't be done, in a few plain words and then the error itself, or other copies of it that were left aside: the same two for every game, so the list stays put, and **I** shows all of it. Above the keys along the bottom is what comes next and what it comes to, such as **C** to compress the ticked games, and then the other keys, such as **A** to analyze, **Enter** or **I** for the Details of the game the pointer's on, everything about it, and **O** for the options. **L** looks inside that game, **E** extracts it and **V** checks it for damage. Compressing shows only the games it's doing, how each is going and all of it together, and the window's title says how far it is, so it can be seen from the taskbar. Then it shows what came of each, those that couldn't be done first, and **Enter** goes back to the list, where what was compressed is under Done. **D** shows the log of the last job. Everything else is under **M**, More, in groups: this game; the ticked games, to extract, check for damage or analyze again; the list, to send to a Switch (**S**) or clear (**X**); and help (**H**), reporting a problem (**R**), checking for a new version and About. A key beside one also works by itself. On Linux and Mac, running it with nothing after it opens it, as double clicking it on a Mac does, and so does `hacbrewpacklz menu`.

**The Android app** is `hacbrewpacklz-android.apk`, for Android 8 or newer on a 64-bit phone or tablet. It has the same welcome, list and options as the GUI. The first time, it asks for your `prod.keys`, which it keeps inside the app, and for access to your files, so it can read games where they are and save the compressed copies beside them. **+** at the top adds files or a folder. **To do**, **Done** and **Everything** above the list choose what it shows, with how many games each has. Tapping a game opens its **Details**, everything about it, with **Look inside**, and extracting it or checking it for damage in its menu. A long press has those a tap sooner. Compressing shows only the games it's doing and then what came of each, and **Back to the list**, or Back, goes back, where what was compressed is under Done. The arrow beside **Compress** has **Send to Switch**, and extracting the ticked games, checking them for damage, analyzing them again or removing them from the list. The menu at the top has choosing keys, clearing the list, Help, Report a problem, which sends a report straight to the developer, or shares it through whatever the phone has when it can't be sent, checking for a new version, and About. A job keeps going with the screen off, with its progress in a notification. Sending to a Switch is over Wi-Fi.

**The command line** works as before, for scripts and for typing commands yourself. Its output stays in the scrollback, and is plain text when sent to a file or another program.

What you choose in the GUI or on the home screen is remembered in `%APPDATA%\hacBrewPackLZ\settings.txt`, and on Android inside the app. The command line only goes by what you give it.

## Keeping it up to date

`hacbrewpacklz update` installs the latest version, if it's newer than the one you're running. In the GUI, on the home screen and on Android it's **Check for a new version**, at the end of the options. **Keep this app up to date**, in the options, does it each time one of them opens, and is on to begin with. The new version takes over the next time it's opened. On Android, the new version is downloaded and checked, and Android asks before installing it.

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
  Saves:      at least 3.02 GB (72%)
  Game data:  4.16 GB compresses to 1.07 GB (26%)
```

Everything but the compressed data is worked out exactly from the headers, so without compression the figure is right to the byte. For the compressed data, a spread of regions from across the game is really compressed, with the same encoder, level and block size the real run would use. The margin is measured from how much those regions differ, and should hold 19 times out of 20. What a game saves is shown as at least the estimate less its margin, as what a sample can't see only ever makes a game save more. The minimum saving goes by that figure, and the size after it on the lists goes with it, so the two add up. Part of the original game that an update holds more than once is only stored once, so the regions leave it out wherever it's held again, and how well the rest compresses is worked out from the rest alone, as what's held again can compress better or worse. Copies of a game's own files are seen from its file table, which has every file's name, size and place. A file the same size as an earlier one, with the same 4 KB from its middle, start and end, is a copy of the whole file. A file with the same name as an earlier one and about the same size, whose end or start holds much the same, is a near copy, as when a game keeps a pack for each area with what the areas share, and 95% of it is taken as held before. Groups of three or more with the same name whose sizes spread further are versions of one file, such as one for each language, and aren't copies. Copies go the same way as what an update holds again, and looking for them takes at most 128 small reads and a quarter of the time the sample has, the copies that could hold the most first. What else a game holds again by itself, such as what level packs have in common, is only found while compressing (see below), as seeing it would mean reading most of the game, so a game can still come out smaller than its estimate: Metroid Dread with its 2.1.0 update by about 8% (15% before copies were looked for), Outlast with its update by about 16% (22%) and Cave Story+ by about 1% (3%).

That's why a first look can say a game is worth compressing but never that it isn't. A game whose first look comes in under the minimum saving, or says it has nothing to gain, isn't left out on that: it's **not sure yet**, and it isn't ticked, as only reading all of it can say, which takes a while and usually finds too little. Reading all of each game does that, writing nothing, with the same planner that stores what's held again once, so all of that is counted, and every 16th of the blocks stored where they are is compressed, the same blocks every time, to measure the rest by. A game under 1 GB has every block compressed, which takes about twice as long, so its figure is exact to the byte: for a small game, a margin would be most of what it saves. The game's own hashes are checked as it's read. The games most likely to be worth it go first, and what each comes to is kept, so stopping part way loses nothing. A game that saves at least the minimum is then ticked; one that doesn't is settled, under Done, saying it's not worth it, with its measured figure. A game sure to save nothing has no figure, rather than 0 B, or a hair over its size from a margin that could go either way. A game that would grow by more than 1% isn't not sure: compressing never makes game data more than a fraction of a percent bigger, so that's a game packed with something stronger already, such as an NSZ's Zstandard, and reading all of it wouldn't change that. Measured, Outlast 2 came to 8,742,404,880 bytes and compressed to 8,742,257,424: its first look had said it would save 1%, and it saves 16%. Splatoon 3 with its update, Overcooked! All You Can Eat and Minecraft with its update were each within 0.2%. Reading all of it takes as long as the disk takes to read it, and both versions measure a game to the same byte. A game ticked by hand is compressed whatever it saves, without measuring. In the GUI, the home screen and the Android app, **Thorough analysis**, under Which titles are worth it in the options and on to begin with, has Analyze go on to read every game in full straight after the first looks, the ones not sure yet first, so every figure is exact. That goes on behind the list, which can be used meanwhile, and compressing, sending or anything else stops it, keeping what it measured; Analyze goes on with the rest. Compress only does what's ticked: a game not sure yet is ticked once it's measured to be worth it, and with Thorough analysis off, it stays unticked unless you tick it. On the command line, `convert` leaves out what a first look can't vouch for, and `--measure` has `analyze` read those in full and say what they save, and `convert` read them first and convert those worth it.

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

Three settings, under **Which games are worth it** in the options, do this for you, to get the most space in the least time:

| Option | Meaning |
|---|---|
| `--minsaving 15` | Leave out titles that would save less than this percentage. Estimating takes seconds, so nothing much is lost finding out. The GUI and the home screen start at 5%, and the command line at none |
| `--hidesmall` | Leave out games under 1 MB with no update or DLC, like shortcuts to homebrew apps (forwarders) |
| `--measure` | **Thorough analysis**, on to begin with in the GUI, the home screen and the Android app: after the first looks, read every title in full, writing nothing, the ones a first look can't vouch for first, so every figure is exact. On the command line, `analyze` reads the titles a first look can't vouch for and says what they save, and `convert` compresses those worth it; without it, `convert` leaves them out. With a lot of games that can take a long time |

A game this tool compressed is known by what's in it, not by its name: its game data is stored compressed under the oldest key generation, where a game its developer compressed, which needs firmware 12 or later, never is. That holds for copies made by every version, including 1.0 to 1.3, which put that data after the levels that check it. One made by 1.5 or later also says what the game came to before, and for one made earlier that's worked out from what its contents say they come to unpacked. So each compressed game says what it saved, that its developer compressed it, or, with nothing in it compressed at all, such as a game with all its data in its program, that there's nothing to gain. On the Switch, an installed game its developer compressed is worked out like any other, and measured with the rest, and says what compressing it again saves, but it's only ever ticked by hand: an update then needs its original game file (see Updating a compressed game).

A title you tick by hand gets done whatever the minimum says. With compression off, a title only comes out any smaller by having an update made part of it, so one without one starts unticked, under Done, unless it was picked on its own. If any without one are ticked, the GUI and the home screen ask whether to untick them, repack them anyway, or leave things as they are. On the command line they work with `convert`, and `analyze` shows what they'd leave out and ranks what's left, with the command to compress just those:

```
hacbrewpacklz analyze -k prod.keys "D:\My Games" --minsaving 15 --hidesmall
hacbrewpacklz convert -k prod.keys "D:\My Games" --only 0100000000010000,0100000000020000
```

`--only` and `--skip` take a list of title IDs or parts of names.

## Converting and extracting

The easiest way is the GUI or the home screen, above. From a command line:

```
hacbrewpacklz compress -k prod.keys "Game [BASE].nsp" "Game [UPD].nsp" --verify
hacbrewpacklz compress -k prod.keys "D:\My Games" --nspdir "D:\Compressed"
hacbrewpacklz extract -k prod.keys "Game [BASE].nsp" "Game [UPD].nsp" --outdir "D:\Extracted"
hacbrewpacklz check -k prod.keys "D:\My Games"
hacbrewpacklz details -k prod.keys "Game [BASE].nsp" "Game [UPD].nsp"
hacbrewpacklz look -k prod.keys "Game [BASE].nsp" "Game data" --save
```

- **The same verbs as the screens:** `compress` is what `convert` always was, and `convert` keeps working. `check` reads all of each game back through its hashes, writing nothing, as the Switch checks a game. `details` says what a game's Details say on the screens, and `look` goes through what's inside a game a folder at a time, as Look inside does: `--all` lists every file under a folder, and `--save` saves a file or folder to `Files from games` beside the game, or `--to` a folder of your own.

- **What goes in:** NSP, NSZ, XCI and XCZ files, in any mix. The kind is worked out from the file itself, not its name. A base game can be an XCI and its update an NSZ, for example.
- **Batch:** folders are searched for those files, including subfolders, and for games extracted to folders (with `exefs` and `control` in them), which `convert` builds into NSPs and `analyze` sizes up, as the screens do; a game folder can be given by itself too. One failed title doesn't stop the others, and files this tool already compressed are skipped.
- **Base game and update:** given together, in any order or location, they become one title. The newest update wins. The tool works out which update belongs to which game from the files themselves, not their names.
- **What a game holds more than once:** an update often reads the same part of the original from more than one place, so the game it makes holds that part twice or more, and games repeat their own data too. Each such piece is stored once, and the other places it's held point at it, which the console's own reader allows. What an update holds again is found through its own table. The rest is found while compressing reads the game: any 64 KB that's the same as a block stored before it, wherever it is. Every piece is compared with what it would point at first, through hashes of all of it. Metroid Dread with its 2.1.0 update holds 1.40 GB again and comes to 3.25 GB, against 4.27 GB for the base game and update as they were (3.91 GB with only what the update holds again, about 4.45 GB with nothing stored once). Hollow Knight with its v589824 update holds 77.3 MB again and comes out 14 MB smaller for it, Minecraft with 1.26.60 55.6 MB, 25 MB smaller, and Minecraft with 1.26.45 246 MB, 176 MB smaller than with only places a multiple of 16 bytes in.
- **DLC** goes into its game's file, each piece compressed where that helps and kept as it is otherwise, so emulators find it without installing it. DLC given without their game go together into one file. In the GUI, the home screen and on Android the setting is **Game and DLC in one file**, under Output in the options, and it starts on. `--separatedlc` turns it off, and each DLC gets an NSP of its own, even one that can't be made smaller. Beside this tool's copy of its game, a DLC that has no file of its own gets one too, whatever it saves. A DLC has a file of its own when a copy of it is alone in a file or in a file this tool made. The lists leave a DLC given without its game that has nothing to compress as it is, with the setting off or when it's the only DLC of its game. Once a game's new file is made, none of its DLC is left out, in either mode. When there's room for the game alone but not with its DLC, the message ends "Turning off Game and DLC in one file (--separatedlc) makes it smaller."
- **A compressed copy without all its DLC**, given with DLC it doesn't hold, is made again with them while the setting is on. It's made from the copy, unless a DLC of the game has no file of its own, like one only inside a merged original: then it's made from the original, when that's given too and the copy is no newer. Of several copies of a game, the one that holds its DLC is used. On the lists the copy says "1 DLC not in its file". It's to do when making it saves enough, like any game; otherwise it's under Done, and ticking it makes it. The command line makes it whatever it saves.
- **Cartridge images:** everything in an XCI's secure partition is used, so a cartridge holding a game, its update and DLC gives one NSP for the game with the update applied and its DLC in it. With `--separatedlc`, each DLC gets an NSP of its own instead. A rebuilt NCA is marked as a download rather than a cartridge, so the result installs like any other NSP. Its system update partition and padding are left out, which accounts for much of the saving: a 1.86 GB dump came to 247 MB, where the same game uncompressed is 305 MB.
- **NSZ and XCZ** need no other tool and no extra steps. Both kinds of `nsz` compression work: block, and solid, which has to be read in order. They're counted at what they take installed, as installing one unpacks it, while the NSP made of it goes on the console compressed as it is: a 510 MB NSZ that unpacks to 656 MB comes out as a 598 MB NSP, bigger as a file but 58 MB less on the console. The list says how big the file itself is, and the line after the NSP is made says so too. A solid file needs memory for its window, up to about 256 MB as `nsz` normally writes them, or much more for one made with extreme settings. A game and its update can need each other out of order, and then the decompressed data goes to temporary files as well, in `--tempdir`, or the output folder by default: a 14.5 GB game with a 2 GB update used another 16.5 GB while it ran. Solid files are slower too: that pair took 32 minutes as solid NSZ against 23 as NSP, on a slow laptop with one USB hard drive.
- **Output** goes beside the input unless you use `--nspdir`, as `Title [TitleID][vVersion][LZ4].nsp`. The originals are never changed, and nothing that's there is written over: where a file has that name already, such as the original game named the same way, it's saved as `… (2).nsp`, then `(3)` and on. Only a file named exactly with `-o`, or one the build from folders makes in its own folder as hacBrewPack does, is written over. In the GUI, the home screen and on Android, a copy made there is found again with its original the next time, so the game is under Done rather than to do again.
- **Extracting** writes `exefs`, `romfs`, `logo`, `control`, `htmldoc` and `legalinfo` folders, the same ones the build mode takes. With an update, you get the updated game's files. While it goes, the game's folder holds `Not finished extracting.txt`, which goes once all of it is out: a folder with it isn't listed as a game. An extraction that fails or is stopped deletes what it got out; extracting again over a whole one says it's there already, and one that didn't finish is started afresh.
- **Checks:** every block read from a source NSP is checked against its hash tree, and an update on the wrong base game stops with an error instead of producing a broken NSP.

What changes in a converted NSP:

- It needs no ticket: every NCA is re-encrypted with a key area, like hacBrewPack's output. It therefore needs sigpatches even where the original didn't.
- The content meta keeps the source's version and required firmware, raised to 12.0.0 when compressing. It loses the mark a base game cut down to go with its update has, as what's made always holds all of the game: with that mark, DBI warned that a game needed its update, and it didn't start.
- Game files are unchanged. `main.npdm` gets the same ACID key patch hacBrewPack applies, unless you use `--nosignncasig2`.

**Installing a repacked game:** uninstall the game's old update first, and don't install official updates over it afterwards. An official update is a patch against the original base game: it says which bytes to take from where in it, so it can't apply to a repacked one, and the game won't open with one installed. Compressing a game and installing its original update beside it doesn't work for that reason, which LibHac confirms with a hash error, and an update can't be compressed on its own. Give the two together, and they come out as one title. On the Switch, the app does that with a newer update installed over a game it compressed or repacked, from the copy as it's installed.

**Updating a compressed or repacked game:** give the game and the new update together, and they come out as one title, just as from the original. A compressed copy holds the same game data as the original, which is rebuilt from it for the update to read. When an update was made part of a game, compressed or repacked with compression off, that update kept some of the original as it was, and a newer update reads those same parts, as every update is made against the game as it first came out. So since version 1.5 the game keeps a small map of where they went, `hacbrewpacklz_update_map.txt` in its control data, beside its name and icons. One made before that can be given with the update that's in it, which has the same map in its own tables. Each part is checked against the new update's own hashes as it's read. What can't be followed like that takes the original game: a game its developer compressed, whose updates read the original's compressed bytes, and a newer update that reads parts of the original the earlier one had replaced.

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
| `--level N` | 1 and 2 are LZ4, the fastest; 3 to 12 are LZ4 HC, smaller and slower to pack, 10 to 12 the strongest. Default 9 |
| `--blocksize N` | Compression block: 4K, 8K, 16K, 32K or 64K. Default 64K |
| `--threads N` | Compression threads. Default all logical processors |
| `--verify` | Check the finished NSP against the source folders |
| `--separatedlc` | Save each DLC as its own file, not in the game's file |
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
- Data held more than once is stored once: an entry for a place that holds it again points back at the very same LZ4 block, or at the raw bytes, stored before. Those places can be anywhere, and the blocks after one go on from where it ends, so most entries can start at places that aren't a multiple of 16.
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

How to open the Linux and Mac builds is in the [README](../README.md#download). The Mac builds are unsigned, and macOS won't run an unsigned program on Apple silicon, which is why they need signing on the Mac the first time.

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

The Android app needs the .NET SDK's `android` workload, the Android SDK and a JDK, and is signed with the project's own key, so that a new version installs over the old one:

```
dotnet publish android\hacbrewpacklz-android.csproj -c Release -p:AndroidSdkDirectory=<Android SDK> -p:AndroidKeyStore=true -p:AndroidSigningKeyStore=<keystore> -p:AndroidSigningKeyAlias=<alias> -p:AndroidSigningStorePass=file:<password file> -p:AndroidSigningKeyPass=file:<password file>
```

The Switch version is built with devkitPro, by running `make` in `switch`. What it takes is on [its details page](../switch/DETAILS.md#building).

A release is put out with `tools\publish_release.py`, which signs it with `tools\sign_release.py`, writing the two files the updaters look for, writes short release notes with the download links first, and uploads every file with a name that says who it's for.

The format details come from reading [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)'s `fssystem` sources, [Switchbrew](https://switchbrew.org/wiki/NCA) and hacBrewPack. RomFS and PFS0 layouts deliberately match hacBrewPack, whose output is known to work on hardware.
