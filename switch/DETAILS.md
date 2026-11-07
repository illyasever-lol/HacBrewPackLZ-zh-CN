# hacBrewPackLZ for Switch in detail

Everything the [Switch page](README.md) leaves out.

## Using it

There are three lists and the settings, and L and R go between them, all the way round:

- **Installed games**: what's on the console.
- **Game files**: every NSP, NSZ, XCI and XCZ, and every game extracted to a folder, on the SD card, USB drives, and PCs running `hacbrewpacklz send`, or in one folder if you choose one. On the SD card, the folders of the system and of custom firmware, and `config`, where homebrew keeps its settings, are left out. A PC on the USB cable is only listed once `send` there has answered. A PC's games come up by themselves as soon as it's heard from, and go when it stops sharing. The PC also works out what's in its files and how small they'd get, which is a good deal faster than reading them across the network. **Where to look**, in the + menu, chooses which of those it looks in. Each game is one entry, with its update and DLC; other copies of it are noted on it, under + → This game → About this game. Of copies just as new, the one used is on the SD card, then a USB drive, then a PC on the USB cable, then one over the network. A PC on both the cable and the network is only looked in on the cable. A copy this has already compressed, as new as the original, takes its place, and says "Compressed, not installed yet" until it's installed.
- **Settings**, along with About and Quit.

The first time it's opened, a welcome screen says what it does and asks about the settings most people will want a say in. When compressing: whether to replace installed games or save an NSP of them, whether to install game files or save an NSP of them, where NSP files go, and whether to check what's written. Then the minimum saving, and automatic updates. It only comes up the once, and it can be brought back from the settings.

The first time you see a game it gets analyzed by itself, in the background, which takes about a second and only reads. The answers are remembered, so this only happens once, unless you ask for it again in the + menu. Then the best savings come first, and you can see for each game what it takes up now, what it would take up after, and what that saves. What's just been done goes to the end of the list, and what's next is at the top.

The lists come up straight away from what was there last time, and get brought up to date behind that. Only the first time, or for a game that's new or has changed, does anything have to be read for its name and icon. Whichever list is on the screen comes first, so another list that's still coming up stops and picks up again when it's that list's turn. Analyzing doesn't stop for any of that: it goes on beside a list coming up, and once the list on the screen has been analyzed, it goes on with the others that have come up. Whatever gets saved goes on the lists by itself.

| Button | What it does |
|---|---|
| A | Ticks or unticks a game |
| X | Compresses what's ticked, after asking. It says Repack with compression off |
| Y | Goes through ticking everything, nothing, or only what's worth it. The button's label says which is next |


| + | The menu, in the middle of the screen |
| B | Asks whether to quit, and A then does. Anywhere other than the lists and the settings, it goes back or up a folder |
| L, R | Switch lists |
| ZL, ZR | A page up or down |

In the + menu:

- **Sort by**, set with left and right: best saving, size or name. The top of the list says which it is.
- **Install the ticked games**, from Game files, whatever the settings say to do with them.
- **Extract** and **Check for damage**.
- **Stop analyzing**, or **Go on analyzing** if it's been stopped.
- **Analyze again**, for what's ticked, or everything if nothing is.
- **Refresh the list**, to look through it again.
- **Hide** a game, and bring hidden games back.

The touch screen works too: tap a game to tick it, drag to scroll, and tap any button shown at the bottom.

### Spending the time where it counts

Settings to get the most space in the least time, and a list worth looking at:

- **Minimum saving**, 5% to begin with: games that would save less than this come up unticked, and say why. You can still tick them yourself.
- **Hide games under 1 MB**, on by default: leaves out games that small with no update or DLC, which is what forwarders are. The top of the list says how many are hidden.
- **Hide games that are done**, on by default: games go off the list once there's nothing left to do with them, to keep it short. An installed game is done once it's compressed. A game file is done once its game is installed compressed, as new or newer, so a compressed file that isn't installed yet stays, ready to install. If game files are set to be saved as NSPs, a game file is done once it's compressed. The top of the list says how many are hidden, and the + menu brings them back, which is how to check one of them.

A game that's in a folder only comes up once, however many NSPs have been made from it. Once one has been compressed, the folder counts as compressed too.

A dump of one of Sphaira's forwarders is on the list like anything else, even though its content meta is cut short: Sphaira records the wrong size for it, so whatever dumped it only took the first 192 bytes. What was in it can be worked out from the program and control data, and whatever gets made from such a file has a whole one again.

A game that's already compressed, or that wouldn't get any smaller, comes up unticked whatever the settings say, except a compressed game file that's still to be installed. The first kind gets the same check as a game that's just been compressed, and the second says there's nothing to gain.

### What compressing does

For an **installed game** there's a choice, in the settings:

- **Replace them**, the default, which is what frees the space. Each step can be seen as it happens:
  1. The compressed game is written beside the original, as it's made. Nothing goes through a temporary file.
  2. Its hash gets worked out, from memory for whatever fits there, and otherwise by reading it back.
  3. Only then is the new copy swapped in and the original deleted.
- **Save an NSP**, which leaves the game as it is and frees nothing.

**Check what's written**, in the settings, is off to begin with. Turned on, every byte gets read back from the card in step 2 and checked against what was written, and game data read through the new copy gets compared with the original from across the game, which the firmware does itself where it can. That catches a failing card before the original goes, and takes longer.

The hash can't be worked out while it's being written, as it starts with the header, which says how everything else came out and so can't be known until all of it has been written. An NCA's name and what the game's records say about it are its hash, so there's no leaving that out. What can be saved is reading it back: with the memory opened through a game gives it, a little over 2 GB of each NCA stays in memory until then, and that much never has to be read back at all, and the rest gets read ahead of being hashed. How large a game can be only depends on memory for its hash trees and its table, about 800 MB for one of 160 GB, opened through a game, and it says at the start if there isn't enough.

Once each time it's opened, before the first game that needs it, the console is asked to read a small compressed test copy, so that a console that can't read these finds out straight away, before any time has gone on it. If it can't, nothing else gets done.

For a **game file**, it's installed, compressed as it goes in, or if the settings say so, a compressed NSP is saved in the NSP folder instead. The file itself isn't changed. What's already compressed goes across as it is when it's installed. If you'd rather not keep the file once it's in, **Once a file is installed** in the settings deletes it, but only after the game has been installed and checked, and only when everything in that file has been.

For an **extracted game**, which is a folder holding `exefs` and `control`, and also `romfs`, `logo`, `htmldoc` and `legalinfo` where it has them, an NSP is built and saved in the NSP folder, or it's installed. That's what extracting leaves behind, and also what you can change files in first. Its icon, name and size come from those files. The version comes from the `[v123]` in the folder's name, if there is one. The folder isn't changed.

A base game, its update and its DLC are one row, and all get done. The update becomes part of the game. Save data isn't touched. Some base games only hold what their update still reads from them, sometimes with the same version as the update. Those work like any other with their update there, and can't be done without it, which it says.

**Official updates can't be installed over a compressed game**, since an update is a patch against the original. To update one, reinstall the original first.

**With compression off**, in the settings, games are repacked just as they are, with an update made part of the game and no ticket needed. Everything says Repack rather than Compress. A game with an update can come out a good deal smaller that way, and it can take the place of what's installed like anything else. A game with no update comes out the same size, so if any are ticked it asks before going on, and they can be unticked, repacked anyway, or the whole thing cancelled with the ticks left as they were.

**Before anything starts**, when you press X, it works out what fits. For a game being replaced, that's room for the new copy beside the old one, and what that frees afterwards counts towards the next, so the order can change to make more of them fit. If some won't fit it says which, and how much more it would take. If none will, nothing starts.

**Checking for damage**, in the + menu, reads all of each ticked game through its hash trees, which takes about as long as copying it. **Extracting** writes a game's files out to the folder chosen for that.

### Folders and USB drives

In the settings, **NSP files go in** and **Extracted games go in** each open a file explorer. A goes into a folder, B back up, X uses the folder you're in, Y goes back to the usual place, which is `sdmc:/hacBrewPackLZ`, and `extracted` inside that, and the - button closes it. Folders holding an extracted game are marked as such.

USB drives work anywhere a folder can be chosen, and their game files are listed with the rest. FAT32, exFAT, NTFS and ext4 can all be used, with the drive plugged into the dock or through an adapter. FAT32 can't hold a file of 4 GB or more, which it says before it starts. If a drive gets taken out, whatever's being written to it fails and leaves a `.part` file, and the original stays as it was.

### How little it reads and writes

A game is read once, its compressed data written once, straight to where it'll stay, and read back once at the end. Compressed blocks only pass through memory on the way, and with the memory opened through a game gives it, a little over 2 GB of each NCA stays there too, so that much doesn't need reading back. A new copy of an installed game is read back in full if **Check what's written** is on, as that's what checks it. A game on the SD card needs room for its compressed size, and nothing more.

It works on the three of the processor's cores a game gets, and on the fourth if it's been opened in a way that allows it, such as with Sphaira's option for that. The fourth is the one the system keeps for itself and the home menu, which get it whenever they want it, as nothing here ever goes ahead of them, and it's left alone altogether while something else is on the screen. The log says how many it has, on its second line.

Going through folders and reading files is mostly done by the system's own file service, which only ever runs on that fourth core, whatever this does. So while something else is on the screen, like the home menu, everything here reads and writes a good deal more slowly, to keep that core free. It goes back to full speed when it's back on the screen.

### Keeping it up to date

**Automatic updates**, in the settings, looks for a newer version each time the app opens, if the console is online, and installs it. It's on to begin with. **Check for updates now** does it once. It only goes to GitLab, and everything it gets is checked against a signature only a real release has. The new version takes over the next time the app opens, or straight away if you choose to restart.

### If something goes wrong

Stopping, a failure of any kind, or the power going all leave the game as it was: the original stays until the new copy has been checked. The app writes down how far it has got, so if the power goes, the next start either undoes what was written or finishes the swap, and tells you it did. Closing it from the home menu stops at a safe point.

A game that's been compressed never asks for a newer version of itself: that's cleared in what gets made, and when it's installed from here, the console is told that the version now on it is the one that counts, which is what a required version reset in other tools does.

The + menu is split into pages, for this game, the ticked games and the list, and B goes back. It can uninstall the game that's selected, or on the other lists, delete its file or folder, or do that to everything that's ticked. It always asks first, naming what's about to go, with keeping it as the choice already selected. The settings tab starts with what most people change, and **More settings** at the bottom brings up the rest. Uninstalling leaves the game's save data alone. A game that's open can't be uninstalled.

The welcome screen comes up the first time, and whenever the app's own folder has gone, even though the settings themselves are kept in a second place. It can also be brought up from the settings.

**Keep a log**, under More settings, is off to begin with. When it's on, what it does is written to `sdmc:/switch/hacBrewPackLZ/log.txt`, of this time only, and `opening.txt` beside it says how far opening got, with the system's error codes, and without keys. That's what to look at, or send, if something doesn't work.

Settings and what's been worked out are saved in that folder and again in `sdmc:/config/hacBrewPackLZ`, one after the other, so that there's always a whole copy of them. If they can't be saved anywhere, it says so on the screen, with the reason. The log says which files were there when it opened and when it closed, and which copy it went by.

### How analyzing gets through it quickly

A game that's already stored compressed, with no update to make part of it, doesn't take any of this: looking at its header is enough to say there's nothing to gain. Otherwise, part of each game gets really compressed, from all across it. Every processor core keeps going between reading a piece and compressing it, so neither ever waits for the other. Pieces are read in order, so a hard drive on USB sweeps across rather than jumping about, and one at a time, unless timing the first few shows that reading several at once gets more read, which is up to what it's stored on and not to what it says it is. Installed games are read straight from the file system rather than through the system's content manager, if it lets it, as a game with an update is read from two files in turn. It stops as soon as the answer is good to about 2%, or after about a second, or a little longer if it's still reading what it takes to say anything at all.

### How the lists come up quickly

There's no reading the card's own tables from here the way Everything does on Windows, but the idea is the same: answer from what's already been noted down, instead of going to the disk. What's in each game file is noted down the first time it's seen, with its icon. After that, bringing the lists up to date only takes going through the folders, a level at a time with all the cores working on it, and asking for nothing but names, and only a file that's new or has changed in size gets opened. **Refresh the list** in the + menu looks at all of it again. Opening one takes a few small reads, several at a time. Its hash trees don't get read for it, as all it's after is a name and an icon. Nothing else about a game file gets opened until there's something to do with it. Installed games work the same way, and the names and icons of those it hasn't seen get read several at a time as well.

## What's been learned on a console

- With Atmosphere, everything the app reads and writes goes through its fs.mitm module, which has been seen to abort, taking the console with it, when several requests overlap. So the app sends one at a time. It only costs a little, as the file service runs on one core anyway.
- Creating and opening some files has come back as "out of handles" (2001-0106), at no particular time, and the app asks again. When it's always the same file, a disk check of the SD card is worth doing.
- Each file or folder created or deleted on an SD card takes several milliseconds, so a game with thousands of small files takes minutes to extract or delete, whatever the processor does.
- A crash or a hard reset can lose files that were open at the time, which may be why settings and logs went missing.

## What's where

| Folder | What's in it |
|---|---|
| `core` | The engine: the PC tool's engine ported to C++, giving the same bytes from the same input |
| `app` | The interface, drawn in software into plain memory, with its own font rasterizer and no libraries |
| `nx` | What's particular to the console: services, files, threads and the screen |
| `host` | The same engine and app on a PC, with a pretend console, for testing |
| `third_party` | libusbhsfs, which reads USB drives |

## Building

With devkitPro's `switch-dev`, `switch-curl`, `switch-ntfs-3g` and `switch-lwext4`:

```
make
```

That gives `hacBrewPackLZ.nro`. Unlike libnx's template, the Makefile leaves exceptions and RTTI on, as the engine uses them.

USB drives are read with [libusbhsfs](https://github.com/DarkMatterCore/libusbhsfs), [NTFS-3G](https://github.com/tuxera/ntfs-3g) and [lwext4](https://github.com/gkostka/lwext4), and the updater uses [curl](https://curl.se). libusbhsfs's source is in `third_party`, and the others come from [devkitPro's packages](https://github.com/devkitPro/pacman-packages). The first three are under the GPL, version 2 or later, which fits this tool's own GPL-2.0.

## Testing on a PC

`host\build.cmd <output folder>` builds these with Visual Studio's compiler:

| Program | What it's for |
|---|---|
| `hbptool` | The engine on its own, with commands like the PC tool's. `tests\matrix.py engine` compares its output with the PC tool's, byte for byte, on every format and setting. Point `HBPTOOL` at it |
| `replace_test` | Replaces games on the pretend console, and loses power at every step along the way, and again during the recovery. Whenever that happens, the game has to stay whole, nothing can be left behind, and it has to work when tried again |
| `app_test` | The whole app with a pretend person pressing the buttons, against the pretend console, with screenshots to look at |

```
replace_test -k prod.keys <folder> [--stride N] base.nsp update.nsp [dlc.nsp]
app_test -k prod.keys <folder> <screenshots> demo
app_test -k prod.keys <folder> <screenshots> real base.nsp update.nsp [other.nsp]
```

`demo` fills the screens with made-up games to look at. `real` goes through everything with real ones: analyzing and analyzing again, compressing, starting again with everything remembered, a firmware that refuses, no room or only some, stopping, a power cut, the minimum saving, hiding games, saving and installing game files, deleting them afterwards, extracting, extracted games and choosing their folder, installing from the menu, a USB drive coming and going, repacking in place or to a file, the checks turned off, settings kept or not being able to be saved, and the buttons. With `UPDATE_DIR` set to a folder holding what `tools\sign_release.py` makes for a `program.bin` of version 9.9.9, it tries updating too, with bad signatures and damaged downloads.

The pretend console behaves as the real services do as far as is known, but it isn't one. What can only be found out on a console is whether `nx_console.cpp` has understood those services right.
