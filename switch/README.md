# hacBrewPackLZ for Switch

Makes your games smaller, right on the console. It lists what's installed and the games that aren't, as files or extracted folders on the SD card, USB drives or a PC, says how much each would save, and compresses the ones you tick.

**This is a beta.** Keep copies of anything you'd mind losing, and start with one small game.

## What you need

- **Firmware 12.0.0 or newer**
- **Sigpatches.** The app checks this before touching anything.
- **Your console's keys** at `sdmc:/switch/prod.keys`, which is where Lockpick_RCM puts them

## Installing it

1. Download [hacBrewPackLZ.nro](https://gitlab.com/ismaellozanoperezserrano/hacbrewpacklz/-/releases/permalink/latest/downloads/hacBrewPackLZ.nro).
2. Copy it into the `switch` folder on your SD card, so it's at `switch/hacBrewPackLZ.nro`.
3. On the Switch, hold R while you start any game. The homebrew menu opens instead: choose hacBrewPackLZ there.

It keeps itself up to date from then on.

## The first time

1. Open it and let it analyze. It only reads at this point, so nothing on the console changes.
2. Tick one small game, untick the rest, and press X.
3. Start that game from the home menu and play it for a bit.
4. If it works, go on with the rest.

## Buttons

| Button | What it does |
|---|---|
| L, R | Go between the overview, Installed games and Game files |
| A | Tick or untick a game |
| X | Compress what's ticked, or on Game files, install it, compressed as it goes in |
| Y | Tick everything, nothing, or only what's worth it |
| ←, → | Show what's to do, what's done, or everything |
| + | Menu: sorting, this game, the ticked games, the list, and Settings |
| B | Back, or quit |

## Installing from a PC

Run **Send to Switch** in the PC version or the Android app. Its games come up on **Game files** on the Switch, over Wi-Fi when both are on the same network, or over a USB cable from a PC. Tick them and press X, which installs them, compressed as they go in. **Install games to**, in the settings, says whether they go on the SD card or the console.

## Good to know

- **Installed games are replaced** with the compressed copy, but only once it's been written and checked. Turn that off in the settings to save an NSP instead.
- **An official update installed over a compressed or repacked game stops it starting** until it's made part of it: the game is on Installed games, to do, and X does that, compressed. Or put the update's file on the SD card: it's on Game files as an update for the installed game, and X makes it part of it the same way. See [Updating a game](../docs/help.md#updating-a-game).
- **The game you hold R on to open the app can't be compressed** while the app runs through it. To do that one, open the app from another game.
- **Games with thousands of small files are slow** to extract or delete, because the SD card itself takes time for each file.
- **It opens on the overview**, which says how much space you can free, and where: each place games go has a bar showing how full it is now and what compressing would free. Below that, each list has a card with what there is to do and a button that does it, such as **Compress 12 games**, beside **Choose games**, which opens the list. X does that button from anywhere on the card. The games that save the most are under it, and A opens the list on one.
- **Analysis goes on by itself** while the app is open: after a quick first look at each game, it reads every game in full, writing nothing, the ones a first look isn't sure of first, so every figure ends up exact: to the byte for a game under 1 GB, and within a fraction of a percent for a bigger one. The overview says how far it's got and about how long that takes, at the speed your console reads, and it gives way to anything you start. Compressing does only the ticked games, and the ones not sure yet are ticked once they're measured to be worth it.
- **Compressed games say what they freed**, and one compressed by a version before 1.5 stays under Done until compressing it again, smaller, as repeats are now stored once, is sure to be worth it. A game its developer compressed is measured too, and says what compressing it again saves, but it's only done if you tick it, as an update then needs its original game file.
- **Each list starts on what's still to do**, and after that opens on the view you left it on. To do is what's worth compressing, and what isn't known yet. Installed games go once they're compressed, game files once they're installed, and a game measured to save less than the minimum, or with nothing to gain, is settled under Done, saying why. Left and right on the D-pad show what's done, or everything, and keep your place in the list.

## If something goes wrong

The original game stays until the new copy has been checked, even if the power goes. **+ → Help** has what to do about the usual problems, and **Report a problem** there sends a report of what happened straight to the developer, with what you type about it and none of your keys. With no internet, it's saved as `sdmc:/switch/hacBrewPackLZ/report.txt` instead: post that, with what you were doing, as a [new issue on GitLab](https://gitlab.com/ismaellozanoperezserrano/hacbrewpacklz/-/issues/new). If the app won't open at all, send `opening.txt` from the same folder. [Help](../docs/help.md) has more.

## More

[Everything in detail](DETAILS.md): every setting, how it works, and how to build and test it.
