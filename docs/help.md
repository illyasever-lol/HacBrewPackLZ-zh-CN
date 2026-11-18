# Help

What people most often need to know. The same topics are under **Help** in every version of hacBrewPackLZ: in the **...** menu in Windows, under **H** on the home screen, in the menu at the top on Android, and in the **+** menu on the Switch.

## A game won't start

Compressed games need **sigpatches**, and **system version 12.0.0 or newer** on the Switch. Without them, the console says the game is damaged, or asks for a system update.

An official update installed over a compressed game stops it starting, as an update only fits the original game it was made for. On the Switch, that game is on **Installed games**, to do, and compressing it makes the update part of it. Without the Switch app, remove the update with the tool you install games with, and give its file to hacBrewPackLZ with the game (see [Updating a game](#updating-a-game)).

A game that version 1.4.3 or older made from one that's only whole with its update, as many newer eShop games are, can ask for an update and not start, though nothing is missing from it. On the Switch, the overview says how many games won't start as they are, and **Fix** there puts that right in a moment, changing nothing but the console's record of each game. Or compress the original game and its update again with this version, and install that.

A copy the Switch app compressed in version 1.5.0 test 2 or older can ask for a system update, when the game had been installed with an installer such as DBI or sphaira that lets games start on an older system than they ask for. Making the copy took the version from the game's files again. The overview counts these with the games that won't start as they are, and **Fix** lowers what each one asks for to the console's own version, again changing nothing but the console's record.

If it's neither, [report the problem](#reporting-a-problem), with the game's name and the error code the console shows.

## Updating a game

An update has to be made part of a compressed or repacked game, which hacBrewPackLZ does, compressed. Installed over it by itself, it stops the game starting until then.

- **On a PC or phone:** add the compressed or repacked game and the new update together, and press **Compress**. Then install the new compressed game.
- **On the Switch:** install the update the usual way. The game is then on **Installed games**, to do, and X makes the update part of it. Or put the update's file on the SD card, or share it from a PC: it's on **Game files** as an update for the installed game, and X makes it part of it, in place of what's installed.

The original game isn't needed, with two exceptions, and hacBrewPackLZ says when it is:

- **A game compressed with an update in it, by a version before 1.5:** give the update that's in it as well, or the original game. Some games still need the original.
- **A game its developer compressed**, such as many newer ones: give the original game. These are about as small as they get already.

Your save data stays. To stop the console downloading updates by itself, turn off **Auto-Update Software** in the Switch's **System Settings**, under **System**.

## Missing keys

"The key area key for generation ... is not in your keys" (or "in the keyset") means your keys are older than the game: newer games need keys from a newer system version.

1. Update the console's system version.
2. Run Lockpick_RCM again.
3. Use the new `prod.keys`: beside the program on a PC, chosen again on Android, and at `sdmc:/switch/prod.keys` on the Switch, where Lockpick_RCM puts it.

On the Switch, a game that wanted keys is tried again each time the app opens.

## Missing tickets

"Needs a title key" or "personalized ticket" means the game's ticket isn't there. Most games need one to be read.

- Use a copy of the game that has its ticket. Most dumping tools have an option to include it.
- On the Switch, a game you bought has its ticket on the system it was bought on: sysMMC or emuMMC. Open the app there.

## Games that barely shrink

Many newer games are already packed tightly, or keep most of their data where this kind of compression doesn't reach. **Analyze** works out each game's saving before anything's done, from a sample of it, and then, with **Thorough analysis** on in the options, as it is to begin with, goes on to read every game in full, writing nothing: a game under 1 GB to the byte, and a bigger one to within a fraction of a percent. The list can be used meanwhile, and compressing or anything else stops it. A sample can't see everything compressing stores once, such as what a game's level packs have in common, so it can say a game saves enough but not that it doesn't. A game it puts under the **minimum saving** in the options is **not sure yet** until it's been read in full: it may save more, so it isn't left out, but it isn't ticked. **Thorough analysis** reads those first and ticks the ones worth it, and compressing only does what's ticked. A game you tick yourself is compressed whatever it saves.

**To do** has what's worth compressing and what isn't known yet. A game read in full that saves less than the minimum, or one with nothing to gain, is settled: it's under **Done**, saying why, and you can still tick it there. A game that's sure to save nothing says **Nothing** in place of a figure. On the Switch, a game its developer compressed is read in full too, and says what compressing it again would save, but it's only compressed if you tick it: an update then needs its original game file.

## Sending games to the Switch

1. On the PC or phone, press **Send to Switch**.
2. On the Switch, open hacBrewPackLZ, on the same Wi-Fi. The games come up on **Game files**.
3. Tick them and press X. They're compressed as they're installed, so there's no need to compress them first.

A USB cable works too:

- From Windows, with the WinUSB driver for the Switch, which [Zadig](https://zadig.akeo.ie) installs.
- From Linux, with libusb, which nearly every Linux has, and this rule, once, so your user can use the Switch:

  ```
  echo 'SUBSYSTEM=="usb", ATTRS{idVendor}=="057e", ATTRS{idProduct}=="3000", MODE="0666"' | sudo tee /etc/udev/rules.d/70-hacbrewpacklz.rules
  sudo udevadm control --reload-rules && sudo udevadm trigger
  ```

- From a Mac, with libusb, which [Homebrew](https://brew.sh) installs: `brew install libusb`.

From a phone, use Wi-Fi.

## Where things are

- **Compressed games** go beside the originals, ending in `[LZ4].nsp`, unless the options say somewhere else. On the Switch, saved NSPs go in `sdmc:/hacBrewPackLZ`.
- **Files from games**: what Look inside saves goes in a `Files from games` folder, never where extracted games go, so it can't pass for a game: beside the game on a PC, or beside where extracting puts games when the options say where, and in `sdmc:/hacBrewPackLZ` on the Switch. On Android, Save to… puts it in the folder you choose.
- **Settings** are in `%APPDATA%\hacBrewPackLZ` on Windows, inside the app on Android, and in `sdmc:/switch/hacBrewPackLZ` on the Switch.
- **The Switch app's log** is `sdmc:/switch/hacBrewPackLZ/log.txt`, or `log-other.txt` beside it while the console won't let that name be made.
- **Game files on the Switch** are looked for everywhere on the SD card but in the folders of the system and of custom firmware, `config`, `SaltySD`, hidden folders, and apps' own things more than one folder deep in `switch`. **Where to look**, in the + menu, says so under What's left out, and **Only look in one folder** looks through a folder you choose all the way down.

## Reporting a problem

Use **Report a problem**, under **Help**. Say what you were doing, such as which game and what you pressed, read through the report, and **Send** it. It goes straight to the developer, with no account needed: the version, the system, the settings, the games on the list and what the app said. None of your keys are in it, whatever happened. You're told what came of it, such as a problem that's already known, and the version that fixes it.

On the Switch, the report also has the newest few of Atmosphère's own crash reports from the last month, for any game or app, without the memory they copy out, which could hold anything. On the command line, it's `hacbrewpacklz report "what you were doing"`.

If it can't be sent, such as with no internet, it's saved instead:

- **Windows, Mac and Linux:** on your desktop, as `hacBrewPackLZ report.txt`.
- **Android:** it's shared with whatever you choose, such as mail.
- **Switch:** as `sdmc:/switch/hacBrewPackLZ/report.txt`.

Then post it, with what you were doing, as a [new issue on GitLab](https://gitlab.com/ismaellozanoperezserrano/hacbrewpacklz/-/issues/new). That needs a GitLab account. Screenshots help too. Never post your `prod.keys` or any key, only the report.

### Sending problems by themselves

**Send problem reports**, in the welcome and the options, is off unless you turn it on. If you had the welcome before it was there, you're asked about it once, the first time this version opens. When it's on, a report goes by itself when a game can't be done, when the app stops on something it didn't expect, or when it stops answering until it's closed. Those last two go the next time it opens. Each problem goes once for each version, and after that it's only counted. Turning it off drops whatever was waiting to go. On the command line, `--sendproblems` sends a game that couldn't be done.

On the Switch, when Atmosphère says a compressed game stopped with an error since the app was last open, or the console's play history has one closing within 20 seconds of starting, the app asks whether it started. If it didn't, it says what usually helps, and sends what happened, or, with the setting off, offers to send it in one press.
