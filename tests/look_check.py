"""
Checks that Look inside reads a game as extracting it does, in both engines: everything Look inside saves of a game, part by part, is the same
files with the same bytes as the PC tool's extraction and the Switch engine's (hbptool), and look --all lists exactly what it saves.

    python tests\\look_check.py <hacbrewpacklz.exe> <hbptool.exe> <keys file> <work folder> <game file> [its update and DLC]...

Each game's files are given together, the way the list puts them together; the work folder gets a folder for each reader and is emptied first.
"""
import hashlib
import os
import shutil
import subprocess
import sys

# Look inside names each part for what it's for; extracting names them as hacBrewPack's folders.
PART_FOLDERS = {
    "Program": "exefs",
    "Game data": "romfs",
    "Start-up logo": "logo",
    "Icons and names": "control",
    "Manual": "htmldoc",
    "Legal notices": "legalinfo",
    "Data": "data",
}


def files_under(root):
    """Every file under a folder, by its path from it with forward slashes, with its SHA-256."""
    found = {}
    for folder, _, names in os.walk(root):
        for name in names:
            path = os.path.join(folder, name)
            relative = os.path.relpath(path, root).replace(os.sep, "/")
            digest = hashlib.sha256()
            with open(path, "rb") as f:
                for block in iter(lambda: f.read(1 << 20), b""):
                    digest.update(block)
            found[relative] = digest.hexdigest()
    return found


def extracted(root):
    """What an extraction wrote: each title's folder, then its parts' folders. A game's DLC is a title of its own, with its own folder."""
    titles = {}
    for title in sorted(os.listdir(root)):
        path = os.path.join(root, title)
        if os.path.isdir(path):
            titles[title] = files_under(path)
    return titles


def run(command):
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
    return result.returncode, result.stdout + result.stderr


def main():
    if len(sys.argv) < 6:
        print(__doc__)
        return 2
    tool, hbptool, keys, work = sys.argv[1:5]
    games = sys.argv[5:]
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    failures = 0

    def report(name, ok, said=""):
        nonlocal failures
        print(("PASS  " if ok else "FAIL  ") + name + ("" if ok or not said else "  " + said[:400]))
        failures += 0 if ok else 1

    # Look inside: all of the game saved, then what look --all lists.
    look_to = os.path.join(work, "look")
    code, said = run([tool, "look", "-k", keys, *games, "--save", "--to", look_to])
    report("look saves all of the game", code == 0, said)
    saved_titles = os.listdir(look_to) if os.path.isdir(look_to) else []
    report("into one folder named after it", len(saved_titles) == 1, str(saved_titles))
    if len(saved_titles) != 1:
        return 1
    saved = files_under(os.path.join(look_to, saved_titles[0]))
    code, said = run([tool, "look", "-k", keys, *games, "--all"])
    listed = {}
    for line in said.splitlines():
        parts = line.split("\t")
        if len(parts) == 3:
            listed[parts[0]] = int(parts[1])
    sizes = {path: os.path.getsize(os.path.join(look_to, saved_titles[0], *path.split("/"))) for path in saved}
    report("look --all lists what it saves, at the same sizes", listed == sizes,
           f"{len(listed)} listed, {len(sizes)} saved, differing: {sorted(set(listed.items()) ^ set(sizes.items()))[:5]}")

    # Look inside's parts as extraction folders: the game's own, then its DLC, which extracting puts in folders of their own.
    game_parts, dlc_parts, parts = {}, {}, set()
    for path, digest in saved.items():
        part, _, rest = path.partition("/")
        if part == "DLC" or part.startswith("DLC "):
            dlc_parts.setdefault(part, {})[rest] = digest
            continue
        parts.add(part)
        if part in PART_FOLDERS:
            game_parts[f"{PART_FOLDERS[part]}/{rest}"] = digest
    for part in sorted(parts):
        report(f"part '{part}' is one extracting knows", part in PART_FOLDERS)

    for engine, command in (("the PC tool", [tool, "extract", "-k", keys, "--outdir"]), ("the Switch engine", [hbptool, "extract", "-k", keys, "--outdir"])):
        to = os.path.join(work, "pc" if engine == "the PC tool" else "switch")
        code, said = run([*command, to, *games])
        report(f"{engine} extracts it", code == 0, said)
        titles = extracted(to) if os.path.isdir(to) else {}
        # The game is the title with its program; its DLC are the others, in the order Look inside numbers them.
        main_title = next((t for t, files in titles.items() if any(p.startswith("exefs/") for p in files)), None)
        if main_title is None and len(titles) == 1:
            main_title = next(iter(titles))
        report(f"{engine}: the game is found among what it extracted", main_title is not None, str(list(titles)))
        if main_title is None:
            continue
        theirs = titles.pop(main_title)
        same = theirs == game_parts
        detail = ""
        if not same:
            only_look = sorted(set(game_parts) - set(theirs))[:4]
            only_them = sorted(set(theirs) - set(game_parts))[:4]
            differ = sorted(p for p in set(game_parts) & set(theirs) if game_parts[p] != theirs[p])[:4]
            detail = f"only in Look inside {only_look}, only extracted {only_them}, different bytes {differ}"
        report(f"{engine}: Look inside's game is the same {len(game_parts)} files, byte for byte", same, detail)
        # Each DLC's data, against each DLC extracted, whichever way round they come.
        unmatched = [files for files in titles.values()]
        for part, files in sorted(dlc_parts.items()):
            as_extracted = {f"data/{p}": d for p, d in files.items()}
            match = next((i for i, t in enumerate(unmatched) if t == as_extracted), None)
            report(f"{engine}: '{part}' is one of its DLC, byte for byte", match is not None)
            if match is not None:
                unmatched.pop(match)

    print(f"{'All passed' if failures == 0 else f'{failures} failed'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
