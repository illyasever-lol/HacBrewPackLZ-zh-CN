# hacBrewPackLZ

Makes Switch games smaller. It rebuilds NSP, NSZ, XCI and XCZ files as NSPs whose game data is **LZ4 compressed**, using the compression the console's firmware reads by itself, so games play as normal with nothing to unpack. It can also merge a game with its update, extract games to folders, and tell you how much each one would save before you start.

Your original files are never changed.

There's also a **[version for the Switch itself](switch/README.md)** (beta), which needs no PC and can compress games that are already installed.

## Download

Get it from the [Releases page](https://gitlab.com/ismaellozanoperezserrano/hacbrewpacklz/-/releases). Each build is a single file. Keys aren't included: put your own `prod.keys` beside the program.

| File | For |
|---|---|
| `hacbrewpacklz-gui.exe` | Windows, with windows and buttons |
| `hacbrewpacklz.exe` | Windows, in a console window or on the command line |
| `hacBrewPackLZ.nro` | Nintendo Switch, **beta**. See [its page](switch/README.md) |
| `hacbrewpacklz-linux-x64` | Linux (run `chmod +x` on it first) |
| `hacbrewpacklz-macos-arm64`, `-macos-x64` | Mac. See [details](docs/details.md#building) to run it |

## How to use it

1. Open `hacbrewpacklz-gui.exe`, or double click `hacbrewpacklz.exe`.
2. Drop your game files or folders on it. Give a game and its update together and they become one game.
3. **Analyze** shows how much each would save. Only the ones worth it stay ticked.
4. **Compress**. The new files go beside the originals, ending in `[LZ4].nsp`.

From a command line:

```
hacbrewpacklz analyze -k prod.keys "D:\My Games"
hacbrewpacklz convert -k prod.keys "D:\My Games" --nspdir "D:\Compressed"
hacbrewpacklz extract -k prod.keys "Game.nsp" --outdir "D:\Extracted"
```

`hacbrewpacklz --help` lists every option.

## Installing on the Switch from the PC

1. In the GUI, add the games and choose **Send to Switch** from the arrow on the **Compress** button. It shares every game on the list, the hidden compressed ones too, whatever's ticked. On the home screen press **S**, or run `hacbrewpacklz send "D:\Compressed"`. It shares only the game files and folders it's given, so a file like prod.keys is never offered to the Switch.
2. On the Switch, open [hacBrewPackLZ](switch/README.md), and your PC's games come up on **Game files**.
3. Tick the games and press X.

Over Wi-Fi, the PC and the Switch have to be on the same network, and Windows may ask to let it through the firewall. Over a USB cable, Windows needs the WinUSB or libusbK driver for the Switch, which [Zadig](https://zadig.akeo.ie) installs, the same one DBI uses. The cable is Windows only for now.

## Before you install a compressed game

- **The console needs firmware 12.0.0 or newer, and sigpatches.**
- **Uninstall the game's old update first, and don't install official updates over a compressed game.** An update only works with the original game it was made for. To keep an update, compress the game and update together.
- Games that are already packed tightly can come out the same size or larger. Analyze tells you first.

## Updates

The program checks for new versions by itself, and only installs ones signed by this project. `hacbrewpacklz update` does it by hand.

## More

- [Everything in detail](docs/details.md): how the estimate works, every option, what changes in a converted game, how it's built and tested.
- [Tests](tests/README.md)

## License

GPL-2.0. See [LICENSE](LICENSE).
