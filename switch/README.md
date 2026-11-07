# hacBrewPackLZ for Switch

Makes your games smaller, right on the console. It lists what's installed and the games that aren't, as files or extracted folders on the SD card, USB drives or a PC, says how much each would save, and compresses the ones you tick.

**This is a beta.** Keep copies of anything you'd mind losing, and start with one small game.

## What you need

- **Firmware 12.0.0 or newer**
- **Sigpatches.** The app checks this before touching anything.
- **Your console's keys** at `sdmc:/switch/prod.keys`, which is where Lockpick_RCM puts them

Copy `hacBrewPackLZ.nro` to `sdmc:/switch/` and open it from the homebrew menu. For the most memory, open the homebrew menu by holding R while starting a game.

## The first time

1. Open it and let it analyze. This only reads, so nothing can go wrong.
2. Tick one small game, untick the rest, and press X.
3. Start that game from the home menu and play it for a bit.
4. If it works, go on with the rest.

## Buttons

| Button | What it does |
|---|---|
| L, R | Switch between Installed games, Game files and Settings |
| A | Tick or untick a game |
| X | Compress what's ticked |
| Y | Tick everything, nothing, or only what's worth it |
| + | Menu: sorting, this game, the ticked games, the list |
| B | Back, or quit |

## Installing from a PC

Run **Send to Switch** in the PC version. Its games come up on **Game files** on the Switch, over Wi-Fi when both are on the same network, or over a USB cable. Tick them and press X, which installs them, compressed as they go in. **Install games to**, in the settings, says whether they go on the SD card or the console.

## Good to know

- **Installed games are replaced** with the compressed copy, but only once it's been written and checked. Turn that off in the settings to save an NSP instead.
- **Don't install official updates over a compressed game.** An update only works with the original game. Reinstall the original first.
- **Games with thousands of small files are slow** to extract or delete, because the SD card itself takes time for each file.
- **Games that are done are hidden** from the lists by default: installed games once they're compressed, and game files once they're installed. The + menu shows them again.

## If something goes wrong

The original game stays until the new copy has been checked, even if the power goes. Turn on **Keep a log** under More settings, try again, and send `sdmc:/switch/hacBrewPackLZ/log.txt`. If the app won't open at all, send `opening.txt` from the same folder.

## More

[Everything in detail](DETAILS.md): every setting, how it works, and how to build and test it.
