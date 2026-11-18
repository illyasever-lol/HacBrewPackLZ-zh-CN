"""
Drives the console home screen in a real console window, the way someone double clicking the program would use it:
it has to stay open, take dropped paths, and redraw at any window size with nothing wrapped or left behind.

    python tests\\console_screen.py <tool.exe> <keys folder> <small.nsp> [folder for screenshots] [--large=<big.nsp>] [--more=<a.nsp>;<b.nsp>...]

With a big NSP, one that takes a while to compress, it also stops a job part way through. With other games, it fills a list longer than
the window, one game of it with more notes than fit below it. What it writes goes in its own folder.
A window opens for a moment while this runs. Output goes to a temporary folder beside the copied input.
Every frame the program draws goes in a log (HACBREWPACKLZ_SCREEN_LOG), and no row may be cleared before it's written, as that flickers.
"""
import ctypes
import ctypes.wintypes as wt
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import winshot

kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
user32 = ctypes.WinDLL('user32', use_last_error=True)

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 1
FILE_SHARE_WRITE = 2
OPEN_EXISTING = 3
SWP_NOMOVE = 2
SWP_NOZORDER = 4
# Without it, resizing brings the window to the front, and whatever someone is typing in another window comes here as keys.
SWP_NOACTIVATE = 0x10
SW_SHOWNOACTIVATE = 4
KEY_EVENT = 1
VK = {'esc': 0x1B, 'enter': 0x0D, 'down': 0x28, 'up': 0x26, 'left': 0x25, 'right': 0x27, 'delete': 0x2E, 'home': 0x24, 'end': 0x23}


class COORD(ctypes.Structure):
    _fields_ = [('X', ctypes.c_short), ('Y', ctypes.c_short)]


class SMALL_RECT(ctypes.Structure):
    _fields_ = [('Left', ctypes.c_short), ('Top', ctypes.c_short), ('Right', ctypes.c_short), ('Bottom', ctypes.c_short)]


class CSBI(ctypes.Structure):
    _fields_ = [('dwSize', COORD), ('dwCursorPosition', COORD), ('wAttributes', wt.WORD), ('srWindow', SMALL_RECT), ('dwMaximumWindowSize', COORD)]


class KEY_EVENT_RECORD(ctypes.Structure):
    _fields_ = [('bKeyDown', wt.BOOL), ('wRepeatCount', wt.WORD), ('wVirtualKeyCode', wt.WORD), ('wVirtualScanCode', wt.WORD), ('uChar', wt.WCHAR), ('dwControlKeyState', wt.DWORD)]


class INPUT_RECORD(ctypes.Structure):
    _fields_ = [('EventType', wt.WORD), ('Key', KEY_EVENT_RECORD)]


kernel32.CreateFileW.restype = wt.HANDLE
kernel32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.c_void_p, wt.DWORD, wt.DWORD, wt.HANDLE]
kernel32.GetConsoleScreenBufferInfo.argtypes = [wt.HANDLE, ctypes.POINTER(CSBI)]
kernel32.ReadConsoleOutputCharacterW.argtypes = [wt.HANDLE, wt.LPWSTR, wt.DWORD, COORD, ctypes.POINTER(wt.DWORD)]
kernel32.WriteConsoleInputW.argtypes = [wt.HANDLE, ctypes.POINTER(INPUT_RECORD), wt.DWORD, ctypes.POINTER(wt.DWORD)]
kernel32.GetConsoleWindow.restype = wt.HWND
kernel32.GetConsoleTitleW.argtypes = [wt.LPWSTR, wt.DWORD]
kernel32.CloseHandle.argtypes = [wt.HANDLE]
user32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wt.UINT]

failures = 0


def report(name, ok, extra=''):
    global failures
    failures += 0 if ok else 1
    print(f"{'PASS' if ok else 'FAIL'}: {name}{' - ' + extra if extra and not ok else ''}", flush=True)


def running_pids(image):
    out = subprocess.run(['tasklist', '/FI', f'IMAGENAME eq {image}', '/FO', 'CSV', '/NH'], capture_output=True, text=True).stdout
    return {int(line.split('","')[1]) for line in out.splitlines() if line.startswith('"')}


class Console:
    def __init__(self, tool, cwd, args):
        image = os.path.basename(tool)
        before = running_pids(image)
        # It opens behind whatever's in use, so nothing typed there reaches it. Its keys come from WriteConsoleInput, and pictures of it
        # from PrintWindow, neither of which needs it in front.
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = SW_SHOWNOACTIVATE
        self.host = subprocess.Popen(['conhost.exe', tool, *args], cwd=cwd, startupinfo=startup)
        deadline = time.time() + 15
        new = set()
        while not new and time.time() < deadline:
            time.sleep(0.2)
            new = running_pids(image) - before
        if not new:
            raise RuntimeError('the program did not start')
        self.pid = new.pop()
        # Attaching makes this a second process on its console, so it has to have decided the window is its own first: a busy machine
        # can take a while to get it that far.
        time.sleep(4)
        kernel32.FreeConsole()
        if not kernel32.AttachConsole(self.pid):
            raise ctypes.WinError(ctypes.get_last_error())
        self.output = kernel32.CreateFileW('CONOUT$', GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, None, OPEN_EXISTING, 0, None)
        self.input = kernel32.CreateFileW('CONIN$', GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, None, OPEN_EXISTING, 0, None)
        self.window = kernel32.GetConsoleWindow()

    def alive(self):
        return self.pid in running_pids(f'{os.path.basename(TOOL)}')

    def size(self):
        info = CSBI()
        kernel32.GetConsoleScreenBufferInfo(self.output, ctypes.byref(info))
        w = info.srWindow
        return w.Right - w.Left + 1, w.Bottom - w.Top + 1, info.dwSize.X, info.dwSize.Y

    def rows(self):
        width, height, buffer_width, buffer_height = self.size()
        result = []
        for y in range(buffer_height):
            text = ctypes.create_unicode_buffer(buffer_width + 1)
            read = wt.DWORD()
            kernel32.ReadConsoleOutputCharacterW(self.output, text, buffer_width, COORD(0, y), ctypes.byref(read))
            result.append(text.value[:read.value].rstrip())
        return result

    def type(self, text):
        records = (INPUT_RECORD * (len(text) * 2))()
        for i, c in enumerate(text):
            for j, down in enumerate((True, False)):
                record = records[i * 2 + j]
                record.EventType = KEY_EVENT
                record.Key.bKeyDown = down
                record.Key.wRepeatCount = 1
                record.Key.uChar = c
                record.Key.wVirtualKeyCode = ord(c.upper()) if c.isascii() and (c.isalnum() or c == ' ') else 0
        written = wt.DWORD()
        kernel32.WriteConsoleInputW(self.input, records, len(records), ctypes.byref(written))

    def burst(self, parts):
        """Keys and text in one write, as the console hands them over together when a key is pressed while a path is dropped, or when they
        pile up while the screen falls behind. Each part is ('key', name) or ('text', what's typed)."""
        events = []
        for kind, value in parts:
            if kind == 'key':
                events.append((VK[value], '\r' if value == 'enter' else '\x1b' if value == 'esc' else '\0'))
            else:
                events.extend((ord(c.upper()) if c.isascii() and (c.isalnum() or c == ' ') else 0, c) for c in value)
        records = (INPUT_RECORD * (len(events) * 2))()
        for i, (code, char) in enumerate(events):
            for j, down in enumerate((True, False)):
                record = records[i * 2 + j]
                record.EventType = KEY_EVENT
                record.Key.bKeyDown = down
                record.Key.wRepeatCount = 1
                record.Key.wVirtualKeyCode = code
                record.Key.uChar = char
        written = wt.DWORD()
        kernel32.WriteConsoleInputW(self.input, records, len(records), ctypes.byref(written))

    def press(self, name):
        records = (INPUT_RECORD * 2)()
        for j, down in enumerate((True, False)):
            records[j].EventType = KEY_EVENT
            records[j].Key.bKeyDown = down
            records[j].Key.wRepeatCount = 1
            records[j].Key.wVirtualKeyCode = VK[name]
            records[j].Key.uChar = '\r' if name == 'enter' else '\x1b' if name == 'esc' else '\0'
        written = wt.DWORD()
        kernel32.WriteConsoleInputW(self.input, records, 2, ctypes.byref(written))

    def hold(self, name, repeats):
        """A key held down: Windows repeats the key down, and only lets go once."""
        records = (INPUT_RECORD * (repeats + 1))()
        for j in range(repeats + 1):
            records[j].EventType = KEY_EVENT
            records[j].Key.bKeyDown = j < repeats
            records[j].Key.wRepeatCount = 1
            records[j].Key.wVirtualKeyCode = VK[name]
        written = wt.DWORD()
        kernel32.WriteConsoleInputW(self.input, records, len(records), ctypes.byref(written))

    def pointed(self):
        return next((row for row in self.rows() if '►' in row), '')

    def resize(self, width, height):
        user32.SetWindowPos(self.window, None, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)

    def wait_for(self, text, seconds=20):
        deadline = time.time() + seconds
        while time.time() < deadline:
            if any(text in row for row in self.rows()):
                return True
            time.sleep(0.1)
        return False

    def shot(self, name):
        if SHOTS:
            winshot.capture(self.window, os.path.join(SHOTS, name + '.png'))


def check_layout(console, name, needed):
    """Nothing reaches the last column, the buffer is exactly the window, and what has to be on screen is there once. It's looked at once
    the screen has stopped changing, as a whole new screen after a resize can take longer to come up than a fixed wait allows for."""
    time.sleep(0.3)
    rows = console.rows()
    deadline = time.time() + 3
    while time.time() < deadline:
        time.sleep(0.15)
        again = console.rows()
        if again == rows:
            break
        rows = again
    width, height, buffer_width, buffer_height = console.size()
    problems = []
    if (buffer_width, buffer_height) != (width, height):
        problems.append(f'buffer {buffer_width}x{buffer_height} but window {width}x{height}')
    for y, row in enumerate(rows):
        if len(row) >= width:
            problems.append(f'row {y} touches the edge: {row!r}')
    for text in needed:
        count = sum(text in row for row in rows)
        if count != 1:
            problems.append(f'{text!r} shows {count} times')
    report(f'{name} at {width}x{height}', not problems, '; '.join(problems) + ' | ' + ' / '.join(r for r in rows if r)[:400])
    return rows


def chip(key, label):
    """A key cap and what it does, as the screen shows them: the key with a space either side, then a space and the label."""
    return f'{key}  {label}'


def foot(console):
    """What's below the last rule: the line that says what's next or going on, and the keys."""
    rows = console.rows()
    last = max((i for i, row in enumerate(rows) if row.strip().startswith('─' * 10)), default=len(rows))
    return [row for row in rows[last + 1:] if row.strip()]


def none_cut(console, name, rows=None):
    """What a screen says to read, as opposed to a game's name in its row, is never cut short with an ellipsis: it wraps."""
    rows = rows if rows is not None else foot(console)
    cut = [row for row in rows if row.rstrip().endswith('…')]
    report(f'{name}: nothing is cut short', not cut, ' / '.join(cut))


def wait_until(test, seconds=20):
    deadline = time.time() + seconds
    while time.time() < deadline:
        if test():
            return True
        time.sleep(0.1)
    return False


def read_frames(path):
    """What HACBREWPACKLZ_SCREEN_LOG has the program write: a line for each frame, after the size it was drawn for."""
    frames = []
    with open(path, encoding='utf-8', newline='\n') as f:
        for line in f.read().split('\n'):
            sized = re.match(r'(\d+)x(\d+)\t', line)
            if sized:
                frames.append(((int(sized.group(1)), int(sized.group(2))), line[sized.end():]))
            elif frames:
                frames[-1] = (frames[-1][0], frames[-1][1] + '\n' + line)
    return frames


# What's behind a key in a box: the next step's accent colour, and the grey every key had before they became coloured letters.
KEY_BOXES = ('48;5;238', '48;5;80')


def boxes_touching(screen_log):
    """Where a key's grey box sits right under another's on the screen as last drawn, which makes them one grey bar: the row and column, or None."""
    rows = {}
    for _, frame in read_frames(screen_log):
        if '\x1b[2J' in frame:
            rows = {}
        for number, text in re.findall(r'\x1b\[(\d+);1H(.*?)\x1b\[K', frame):
            boxed, cells = False, []
            for token in re.findall(r'\x1b\[[0-9;?]*[A-Za-z]|.', text):
                if token.startswith('\x1b['):
                    if token.endswith('m'):
                        boxed = False if token in ('\x1b[0m', '\x1b[m') else boxed or any(box in token for box in KEY_BOXES)
                    continue
                cells.append(boxed)
            rows[int(number)] = cells
    for y in sorted(rows):
        below = rows.get(y + 1, [])
        for x, boxed in enumerate(rows[y]):
            if boxed and x < len(below) and below[x]:
                return y, x
    return None


# A row is written over what was there, and only then is what's left of it cleared. Cleared first, it shows blank for a moment, which on the
# row of the game being compressed, changing several times a second, flickered. An empty row is only cleared, whichever way round, so a
# clear straight after moving to a row is only wrong if the row has something in it.
CLEARED_FIRST = re.compile(r'\x1b\[(\d+);1H\x1b\[[012]?K(?!$|\x1b\[\d+;\d+H|\x1b\[\?25[lh])')


def cleared_before_written(frames):
    """Where a frame clears a row before writing it, or clears the whole screen when the size hasn't changed."""
    problems = []
    size = None
    for number, (drawn_for, frame) in enumerate(frames):
        if '\x1b[2J' in frame and drawn_for == size:
            problems.append(f'frame {number} clears the whole screen, at the same size')
        size = drawn_for
        for match in CLEARED_FIRST.finditer(frame):
            row = re.split(r'\x1b\[\d+;\d+H', frame[match.end():], maxsplit=1)[0]
            text = re.sub(r'\x1b\[[0-9;?]*[A-Za-z]', '', row).strip()
            problems.append(f'frame {number} clears row {match.group(1)} before writing {text[:60]!r}')
    return problems


def check_asked_once(keys, work, settings):
    """Whoever had the welcome before sending problems to the developer was on it, as on 1.4.3, is asked the once, and the answer is kept."""
    with open(settings, 'w', encoding='utf-8') as f:
        f.write(f'--noautoupdate=1\n--nspdir={work}\nwelcomed=1\n')
    console = Console(TOOL, keys, [])
    try:
        console.resize(1000, 560)
        report('someone who had the welcome before is asked about sending problems', console.wait_for('Send problem reports?', 10)
               and console.wait_for(chip('Y', 'Send them'), 3), ' / '.join(r for r in console.rows() if r))
        none_cut(console, 'the question about sending problems')
        console.shot('problems_question')
        console.type('n')
        gone = wait_until(lambda: not any('Send problem reports?' in r for r in console.rows()), 5)
        wait_until(lambda: 'problemsasked=1' in open(settings, encoding='utf-8').read(), 3)
        saved = open(settings, encoding='utf-8').read()
        report('N leaves it off, and that is kept', gone and 'problemsasked=1' in saved and '--sendproblems=1' not in saved, saved)
        console.type('q')
        deadline = time.time() + 10
        while console.alive() and time.time() < deadline:
            time.sleep(0.2)
    finally:
        kernel32.CloseHandle(console.output)
        kernel32.CloseHandle(console.input)
        kernel32.FreeConsole()
        subprocess.run(['taskkill', '/PID', str(console.pid), '/F'], capture_output=True, stdin=subprocess.DEVNULL)
        try:
            console.host.wait(10)
        except subprocess.TimeoutExpired:
            subprocess.run(['taskkill', '/PID', str(console.host.pid), '/F'], capture_output=True, stdin=subprocess.DEVNULL)


def check_notes_room(keys, work, settings, source, more):
    """Under the list, the game the pointer's on gets two lines for what its row has no room for, whichever game that is: the list neither
    jumps about nor gives up more than those for a game with a lot to say. Eight copies of a game give it seven notes, more than fit."""
    # NSPs go to a folder of their own, as the copy compressed earlier is found where NSPs go and would take its game's place.
    notes_out = os.path.join(work, 'notes_out')
    os.makedirs(notes_out, exist_ok=True)
    with open(settings, 'w', encoding='utf-8') as f:
        f.write(f'--noautoupdate=1\n--nspdir={notes_out}\nwelcomed=1\nproblemsasked=1\n')
    folder = os.path.join(work, 'notes')
    os.makedirs(folder, exist_ok=True)
    for i in range(8):
        shutil.copyfile(source, os.path.join(folder, f'A Test Game copy {i + 1} [0123456789ABCDEF].nsp'))
    for path in more:
        shutil.copyfile(path, os.path.join(folder, os.path.basename(path)))
    console = Console(TOOL, keys, [])

    def layout():
        rows = console.rows()
        games = [i for i, row in enumerate(rows) if '[x]' in row or '[ ]' in row]
        rule = next((i for i, row in enumerate(rows) if i > (games[-1] if games else 0) and row.strip().startswith('─' * 10)), len(rows))
        return rows, games, rule

    # The row the pointer's on, among the games: the views above them end in arrows too.
    def pointer():
        return next(((i, row) for i, row in enumerate(console.rows()) if '►' in row and ('[x]' in row or '[ ]' in row)), (-1, ''))

    try:
        console.resize(760, 420)
        console.wait_for('Drop game files or folders', 10)
        time.sleep(0.5)
        # Dropped with an arrow pressed just before and after it, which the console hands over all together: the path still goes in as a
        # path, rather than its letters being taken as keys, which would compress, report, open the options and quit.
        console.burst([('key', 'down'), ('text', f'"{folder}"'), ('key', 'up')])
        listed = wait_until(lambda: any('To do' in r for r in console.rows()) and any('[x]' in r or '[ ]' in r for r in console.rows()), 60)
        report('a path dropped together with arrow keys still goes in as a path', listed and console.alive(), ' / '.join(r for r in console.rows() if r))
        # Every file read, which the screen stops changing for.
        last, stable, deadline = None, 0, time.time() + 60
        while stable < 4 and time.time() < deadline:
            time.sleep(0.5)
            now = console.rows()
            stable = stable + 1 if now == last else 0
            last = now
        rows, games, rule = layout()
        views = next((r for r in rows if 'To do' in r), '')
        to_do = int(re.search(r'To do (\d+)', views).group(1)) if re.search(r'To do (\d+)', views) else -1
        report('a game with seven notes leaves room for every game on the list', listed and to_do >= 3 and len(games) == to_do, f'{len(games)} of {to_do} / ' + ' / '.join(r for r in rows if r))
        # The pointer can be anywhere on the list to start with, so it goes whichever way the game is. Each move waits for the screen to show
        # it, or a slow redraw would have it go past.
        target = next((i for i in games if 'LZ Test' in rows[i]), -1)
        for _ in range(to_do):
            at, was = pointer()
            if 'LZ Test' in was:
                break
            console.press('down' if at < target else 'up')
            wait_until(lambda: pointer()[1] != was, 3)
        time.sleep(0.5)
        rows, games, rule = layout()
        said = [row for row in rows[games[-1] + 1:rule] if row.strip()] if games else []
        report('on that game, two lines under the list say the start of it, and where the rest is', len(said) == 2 and said[0].strip().startswith('Another copy of this')
               and said[1].rstrip().endswith('I shows all of it'), ' / '.join(said) or ' / '.join(r for r in rows if r))
        console.shot('notes_room_noted')
        before = games
        console.press('up' if 'LZ Test' not in rows[games[0]] else 'down')
        wait_until(lambda: 'LZ Test' not in pointer()[1], 3)
        time.sleep(0.3)
        rows, games, rule = layout()
        said = [row for row in rows[games[-1] + 1:rule] if row.strip()] if games else []
        report('on a game with nothing to say, those lines are empty, and the list stays where it was', not said and games == before, f'{before} then {games}: ' + ' / '.join(said))
        console.shot('notes_room_other')
        # Shorter than the list, it fills all of the window but those lines.
        console.resize(760, 340)
        time.sleep(1)
        rows, games, rule = layout()
        report('a list longer than the window has all of it but the two lines and the gap above them', bool(games) and len(games) < to_do and rule - games[-1] == 4,
               f'{len(games)} of {to_do}, rule {rule - (games[-1] if games else 0)} rows after the last game / ' + ' / '.join(r for r in rows if r))
        console.shot('notes_room_short')
        console.type('q')
        deadline = time.time() + 10
        while console.alive() and time.time() < deadline:
            time.sleep(0.2)
    finally:
        kernel32.CloseHandle(console.output)
        kernel32.CloseHandle(console.input)
        kernel32.FreeConsole()
        subprocess.run(['taskkill', '/PID', str(console.pid), '/F'], capture_output=True, stdin=subprocess.DEVNULL)
        try:
            console.host.wait(10)
        except subprocess.TimeoutExpired:
            subprocess.run(['taskkill', '/PID', str(console.host.pid), '/F'], capture_output=True, stdin=subprocess.DEVNULL)


def check_nsp_folder(keys, work, settings, source):
    """A game compressed into the NSP folder, rather than beside its original, is found with it the next time the app opens: under Done,
    rather than to do again."""
    originals = os.path.join(work, 'originals')
    elsewhere = os.path.join(work, 'nsp folder')
    os.makedirs(originals, exist_ok=True)
    os.makedirs(elsewhere, exist_ok=True)
    game = os.path.join(originals, 'A Test Game [0123456789ABCDEF].nsp')
    shutil.copyfile(source, game)
    with open(settings, 'w', encoding='utf-8') as f:
        f.write(f'--noautoupdate=1\n--nspdir={elsewhere}\nwelcomed=1\nproblemsasked=1\n')
    for turn in ('compress', 'open again'):
        console = Console(TOOL, keys, [])
        try:
            console.resize(900, 500)
            console.wait_for('Drop game files or folders', 10)
            time.sleep(0.5)
            console.type(f'"{game}"')
            if turn == 'compress':
                listed = console.wait_for('Compress 1 game', 30)
                console.type('c')
                done = listed and console.wait_for('Compressed 1 game', 180)
                made = [f for f in os.listdir(elsewhere) if f.endswith('[LZ4].nsp')]
                report('a game is compressed into the NSP folder, away from its original', done and len(made) == 1 and len(os.listdir(originals)) == 1,
                       f'{made} / {os.listdir(originals)} / ' + ' / '.join(r for r in console.rows() if r))
                console.press('enter')
                time.sleep(0.5)
            else:
                listed = wait_until(lambda: any('To do' in r for r in console.rows()), 30)
                time.sleep(1.5)
                views = next((r for r in console.rows() if 'To do' in r), '')
                report('opened again, its original is under Done, as its copy in the NSP folder is found with it', listed and 'To do 0' in views and 'Done 1' in views,
                       ' / '.join(r for r in console.rows() if r))
                console.shot('nsp_folder_found')
            console.type('q')
            deadline = time.time() + 10
            while console.alive() and time.time() < deadline:
                time.sleep(0.2)
        finally:
            kernel32.CloseHandle(console.output)
            kernel32.CloseHandle(console.input)
            kernel32.FreeConsole()
            subprocess.run(['taskkill', '/PID', str(console.pid), '/F'], capture_output=True, stdin=subprocess.DEVNULL)
            try:
                console.host.wait(10)
            except subprocess.TimeoutExpired:
                subprocess.run(['taskkill', '/PID', str(console.host.pid), '/F'], capture_output=True, stdin=subprocess.DEVNULL)


def main():
    global TOOL, SHOTS
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    TOOL = os.path.abspath(sys.argv[1])
    keys = os.path.abspath(sys.argv[2])
    source = os.path.abspath(sys.argv[3])
    SHOTS = os.path.abspath(sys.argv[4]) if len(sys.argv) > 4 and not sys.argv[4].startswith('--') else None
    large = next((a[len('--large='):] for a in sys.argv if a.startswith('--large=')), None)
    # Other games, for a list longer than the window: separated by semicolons.
    more = next((a[len('--more='):].split(';') for a in sys.argv if a.startswith('--more=')), [])
    if SHOTS:
        os.makedirs(SHOTS, exist_ok=True)

    work = tempfile.mkdtemp(prefix='hbplz_screen_', dir=os.path.dirname(source) if '--beside' in sys.argv else None)
    nsp = os.path.join(work, 'A Test Game [0123456789ABCDEF].nsp')
    shutil.copyfile(source, nsp)
    # Settings of its own, starting from nothing as on a first run, so the welcome comes up and whoever runs this keeps theirs.
    # The program inherits it through the console host.
    settings = os.path.join(work, 'settings.txt')
    with open(settings, 'w', encoding='utf-8') as f:
        f.write(f'--noautoupdate=1\n--nspdir={work}\n')
    os.environ['HACBREWPACKLZ_SETTINGS'] = settings
    os.environ['HACBREWPACKLZ_REPORT_FOLDER'] = work
    # Nowhere to send reports, so a test never reaches the real report service.
    os.environ['HACBREWPACKLZ_REPORT_SITE'] = 'none'
    screen_log = os.path.join(work, 'screen.log')
    os.environ['HACBREWPACKLZ_SCREEN_LOG'] = screen_log

    # The check has to tell the two ways round apart before it's any use: a row with a band of colour cleared first, and written first.
    band = '\x1b[7m  Compressing  45%\x1b[0m'
    old_way = [((100, 30), f'\x1b[2J\x1b[1;1H\x1b[K{band}\x1b[2;1H\x1b[K\x1b[?25l'), ((100, 30), f'\x1b[1;1H\x1b[K{band}\x1b[?25l')]
    new_way = [((100, 30), f'\x1b[2J\x1b[1;1H{band}\x1b[K\x1b[2;1H\x1b[K\x1b[?25l'), ((100, 30), f'\x1b[1;1H{band}\x1b[K\x1b[5;9H\x1b[?25h')]
    report('the flicker check finds a row cleared before it is written, and not one written first', len(cleared_before_written(old_way)) == 2 and not cleared_before_written(new_way),
           f'{cleared_before_written(old_way)} / {cleared_before_written(new_way)}')

    # A slow disk can take seconds over the first start of a program that's fallen out of the cache.
    subprocess.run([TOOL, '-h'], capture_output=True, stdin=subprocess.DEVNULL)
    console = Console(TOOL, keys, [])
    try:
        console.resize(1000, 560)
        report('the first run opens on the welcome', console.wait_for('Welcome to hacBrewPackLZ'), ' / '.join(console.rows()))
        check_layout(console, 'welcome', ['Welcome to hacBrewPackLZ', 'sigpatches', 'NSP folder', 'Minimum saving', 'To begin with'])
        console.shot('welcome')
        console.press('down')
        console.press('enter')
        time.sleep(0.4)
        report('a choice on the welcome is kept', '--verify=1' in open(settings, encoding='utf-8').read(), open(settings, encoding='utf-8').read())
        report('it waits for start before counting as seen', 'welcomed' not in open(settings, encoding='utf-8').read())
        console.press('enter')
        time.sleep(0.4)
        console.type('s')
        report('start leaves the welcome for the home screen', console.wait_for('Drop game files or folders'), ' / '.join(console.rows()))
        time.sleep(0.4)
        report('having been welcomed is kept', 'welcomed=1' in open(settings, encoding='utf-8').read())
        # It can be had again, from the end of the options.
        console.type('o')
        time.sleep(0.4)
        console.hold('down', 40)
        time.sleep(0.4)
        console.press('enter')
        report('the options show the welcome again', console.wait_for('Welcome to hacBrewPackLZ', 5), ' / '.join(console.rows()))
        console.type('s')
        report('and start goes back to the list', console.wait_for('Drop game files or folders'), ' / '.join(console.rows()))
        time.sleep(2)
        report('stays open by itself', console.alive())
        check_layout(console, 'empty home screen', ['hacBrewPackLZ', 'Drop game files or folders', chip('O', 'Options'), chip('M', 'More')])
        console.shot('home_empty')

        console.type(f'"{nsp}"')
        report('a dropped path is picked up', console.wait_for(chip('A', 'Analyze')), ' / '.join(console.rows()))
        report('the next step is compressing it, said with its size', console.wait_for('Compress 1 game', 5), ' / '.join(console.rows()))
        time.sleep(0.5)
        rows = console.rows()
        title_row = next((r for r in rows if 'version ' in r or 'DLC v' in r), '')
        report('the title is listed, ticked, with the pointer on it', '[x]' in title_row and '►' in title_row, ' / '.join(rows))
        report('before Analyze, its figure says Not yet rather than being blank', 'Not yet' in title_row, title_row)
        report('the views are along the top, with how many each has', any('To do 1' in r and 'Done 0' in r and 'Everything 1' in r for r in rows), ' / '.join(rows))
        console.shot('home_title')

        console.type('a')
        report('analyze finishes, and the next step says what it comes to', wait_until(lambda: any('Compress 1 game' in r and ' to ' in r and '%' in r for r in console.rows()), 60),
               ' / '.join(console.rows()))
        console.shot('home_analyzed')
        none_cut(console, 'analyzed')

        for width, height in [(1100, 700), (760, 420), (560, 340), (440, 300), (900, 500)]:
            console.resize(width, height)
            rows = check_layout(console, 'titles', ['hacBrewPackLZ', 'Compress 1 game', chip('Q', 'Quit')])
            none_cut(console, f'titles at {width}x{height}')
            console.shot(f'home_{width}x{height}')
            touching = boxes_touching(screen_log)
            report(f"at {width}x{height}, no key's box sits right under another's", touching is None, f'row and column {touching}')

        console.resize(300, 180)
        time.sleep(0.4)
        report('a tiny window asks to be larger', console.wait_for('make this window larger', 3) or console.wait_for('hacBrewPackLZ', 1), ' / '.join(console.rows()))
        console.shot('home_tiny')
        console.resize(900, 500)
        time.sleep(0.4)

        # Details: all there is to know about the game the pointer's on, in the same places as on the Switch, with what can be done to it.
        console.press('enter')
        report('Enter opens the Details of the game', console.wait_for("What's in it", 10) and console.wait_for('The file', 1)
               and console.wait_for('A Test Game [0123456789ABCDEF].nsp', 1) and console.wait_for('What it needs', 1), ' / '.join(console.rows()))
        report('with what compressing it would make of it first', console.wait_for('Compressed, it would take', 3), ' / '.join(console.rows()))
        # Details opens before the game is open, and Look inside comes once it is.
        report('and what can be done to it along the bottom',
               wait_until(lambda: any(chip('L', 'Look inside') in r and chip('V', 'Check for damage') in r for r in console.rows()), 10),
               ' / '.join(console.rows()))
        none_cut(console, 'details', console.rows())
        console.shot('details_game')
        # Look inside: the game's parts as folders, with what's under the pointer beside them.
        console.type('l')
        report('L looks inside it, with its parts as folders', console.wait_for('Game data/', 10) and console.wait_for('Program/', 1), ' / '.join(console.rows()))
        report('what the folder under the pointer holds is beside the list', console.wait_for(' items', 10) and console.wait_for('Folder', 5), ' / '.join(console.rows()))
        console.shot('inside_top')
        console.press('enter')
        report('Enter opens a folder, which says where it is', console.wait_for('a.bin', 5) and console.wait_for('Game data', 1), ' / '.join(console.rows()))
        console.press('end')
        report('a file of data is shown as its bytes', console.wait_for('·  As bytes', 10) or console.wait_for(' lines', 1), ' / '.join(console.rows()))
        console.shot('inside_folder')
        console.type('b')
        report('B shows any file as bytes', wait_until(lambda: any(chip('B', 'As it is') in r for r in console.rows()), 10) and console.wait_for('·  As bytes', 10),
               ' / '.join(console.rows()))
        console.type('b')
        wait_until(lambda: any(chip('B', 'As bytes') in r for r in console.rows()), 10)
        console.type('s')
        saved = os.path.join(work, 'Files from games')
        report('S saves it to Files from games, beside the game, and says where', console.wait_for('Saved to', 20) and os.path.isdir(saved), ' / '.join(console.rows()))
        console.press('left')
        report('left goes back up to the top', console.wait_for('Program/', 3), ' / '.join(console.rows()))
        console.press('esc')
        report('Esc goes back to its Details', console.wait_for("What's in it", 3), ' / '.join(console.rows()))
        console.press('esc')
        report('and Esc again to the list', console.wait_for(chip('M', 'More'), 3), ' / '.join(console.rows()))

        console.type('p')
        time.sleep(0.3)
        console.type('x')
        # While a frame is being written the cursor goes through each row it changes, so it's looked at once it's settled.
        typed, rows = '', []
        deadline = time.time() + 3
        while time.time() < deadline and '> x' not in typed:
            time.sleep(0.2)
            info = CSBI()
            kernel32.GetConsoleScreenBufferInfo(console.output, ctypes.byref(info))
            rows = console.rows()
            typed = rows[info.dwCursorPosition.Y] if info.dwCursorPosition.Y < len(rows) else ''
        report('typing a path shows the cursor on its row', '> x' in typed, f'cursor on {typed!r} | ' + ' / '.join(r for r in rows if r)[-300:])
        console.press('esc')
        time.sleep(0.3)

        console.type('m')
        report("M opens the menu, with the pages of the Switch's + menu side by side", console.wait_for('This game', 3) and console.wait_for('The list', 1)
               and console.wait_for('Check it for damage', 1) and console.wait_for('Check for a new version', 1), ' / '.join(console.rows()))
        report('taking a game off the list is in the words the Windows app and Android use', console.wait_for('Remove it from the list', 1),
               ' / '.join(console.rows()))
        rows = check_layout(console, 'menu', ['hacBrewPackLZ', 'Check it for damage'])
        labels = ['Details', 'Look inside', 'Extract it', 'Check it for damage', 'Remove it from the list', 'Extract them', 'Check them for damage', 'Add a path',
                  'Send to Switch', 'Clear the list', 'Log', 'Report a problem', 'Check for a new version']
        report('the menu uses the width of the window', any(sum(label in r for label in labels) > 1 for r in rows), ' / '.join(r for r in rows if r))
        report('the options are not under More', not any('Options' in r for r in rows if 'Quit' not in r), ' / '.join(r for r in rows if r))
        console.shot('menu')
        touching = boxes_touching(screen_log)
        report("on the menu, no key's box sits right under another's", touching is None, f'row and column {touching}')
        console.press('esc')
        report('Esc closes the menu', console.wait_for(chip('Space', 'Tick'), 3), ' / '.join(console.rows()))

        # Its letters are keys on the menu, so they mustn't be taken as such.
        console.type('m')
        time.sleep(0.3)
        console.type(f'"{work}"')
        time.sleep(2)
        rows = console.rows()
        report('a folder dropped on the open menu is added, and the menu closes', console.wait_for(chip('Space', 'Tick'), 3) and not any('Compression level' in r for r in rows),
               ' / '.join(r for r in rows if r))
        report("what Look inside saved isn't taken for a game: there's still the one", console.wait_for('Everything 1', 5), ' / '.join(console.rows()))

        # Help is on the menu's last page, by its letter as on the list.
        console.type('m')
        time.sleep(0.3)
        console.type('h')
        report('help opens from the menu, with how to use this screen first, then the usual problems', console.wait_for('Using the home screen') and console.wait_for("A game won't start", 1),
               ' / '.join(console.rows()))
        for width, height in [(900, 500), (480, 320)]:
            console.resize(width, height)
            check_layout(console, 'help', ['Using the home screen'])
            console.shot(f'help_{width}x{height}')
        console.press('esc')
        console.resize(900, 500)
        report('help closes', console.wait_for(chip('M', 'More')), ' / '.join(console.rows()))

        console.type('o')
        report('options open', console.wait_for('Compression level'), ' / '.join(console.rows()))
        for key, last in [('down', 'Welcome screen'), ('up', 'Compress game data')]:
            console.hold(key, 40)
            time.sleep(1)
            rows = console.rows()
            report(f'holding {key} goes through every option to {last}', last in console.pointed() and not any("Couldn't find" in r for r in rows),
                   ' / '.join(r for r in rows if r))
            if key == 'down':
                report('the welcome screen is the last option, shown with Enter', any(chip('Enter', 'Show it') in r for r in rows), ' / '.join(r for r in rows if r))
                console.shot('options_last')
        for width, height in [(900, 600), (560, 340)]:
            console.resize(width, height)
            rows = check_layout(console, 'options', ['hacBrewPackLZ'])
            report(f'the whole description shows at {width}x{height}', ' '.join(r.strip() for r in rows if r.strip()).find('only comes out smaller if it had an update.') >= 0, ' / '.join(r for r in rows if r))
            console.shot(f'options_{width}x{height}')
        console.press('esc')
        console.resize(900, 500)
        report('options close', console.wait_for(chip('M', 'More')), ' / '.join(console.rows()))

        console.type(' ')
        report('space unticks the title', console.wait_for('[ ]', 3), ' / '.join(console.rows()))
        report('with nothing ticked, the line above the keys says so', console.wait_for('Nothing is ticked', 3), ' / '.join(console.rows()))
        console.type('c')
        report('and compress says so too', console.wait_for('Press Space to tick a game', 3), ' / '.join(console.rows()))
        console.type('t')
        report('T ticks everything', console.wait_for('[x]', 3), ' / '.join(console.rows()))

        # The job gets a view of its own, with only its games, which is then what came of them.
        console.type('c')
        seen_running = console.wait_for('All together', 15)
        if seen_running:
            report('while compressing, it says how far it all is', console.wait_for('Compressing 1 game', 1), ' / '.join(console.rows()))
            console.shot('job_running')
        report('compressing finishes, with what was saved', console.wait_for('Compressed 1 game', 180) and console.wait_for(' saved', 5), ' / '.join(console.rows()))
        console.shot('job_finished')
        rows = check_layout(console, 'finished job', ['hacBrewPackLZ', 'Compressed 1 game', chip('Enter', 'Back to the list')])
        report('it says where the new files are', any('The new files are in ' in r for r in rows), ' / '.join(r for r in rows if r))
        report('it offers to send them and to show them', any(chip('S', 'Send to Switch') in r and chip('F', 'Show in folder') in r for r in rows), ' / '.join(r for r in rows if r))
        none_cut(console, 'finished job', [r for r in rows if 'The new files' in r or 'official update' in r] + foot(console))
        title = ctypes.create_unicode_buffer(512)
        kernel32.GetConsoleTitleW(title, 512)
        report('the window title says it finished, and what it saved', title.value.startswith('Finished, ') and 'saved' in title.value, title.value)
        for width, height in [(560, 340), (1100, 700)]:
            console.resize(width, height)
            check_layout(console, 'finished job', ['hacBrewPackLZ'])
            console.shot(f'job_finished_{width}x{height}')
            # What's said about the job is all there, or ends in where the rest is: never cut off without a word.
            said = ' '.join(' '.join(console.rows()).split())
            report(f'at {width}x{height}, what the job says is all there or says where the rest is',
                   'they come out as one title' in said or 'D shows all of it' in said, ' / '.join(console.rows()))
        console.resize(900, 500)
        made = [f for f in os.listdir(work) if '[LZ4]' in f and f.endswith('.nsp')]
        report('the compressed NSP is beside the input', len(made) == 1, str(os.listdir(work)))
        report('no partial files are left', not [f for f in os.listdir(work) if f.endswith('.part') or f.endswith('.tmp')], str(os.listdir(work)))
        report('the window is still open after the job', console.alive())

        console.type('i')
        report('I on what came of it opens its Details, with what compressing made of it', console.wait_for('made it smaller by', 10), ' / '.join(console.rows()))
        console.hold('down', 15)
        report('and, further down if need be, the new file', console.wait_for('[LZ4].nsp', 3), ' / '.join(console.rows()))
        console.shot('details_after')
        console.press('esc')
        console.type('d')
        report('D shows the log of the job', console.wait_for('Finished', 3), ' / '.join(console.rows()))
        console.shot('log')
        console.press('esc')
        report('and Esc comes back to what came of it', console.wait_for(chip('Enter', 'Back to the list'), 3), ' / '.join(console.rows()))

        console.press('enter')
        report('Enter goes back to the list, where the compressed game is done', console.wait_for('Everything here is done', 5) and console.wait_for('Done 1', 1),
               ' / '.join(console.rows()))
        title = ctypes.create_unicode_buffer(512)
        kernel32.GetConsoleTitleW(title, 512)
        report('and the window title is back to the name', title.value == 'hacBrewPackLZ', title.value)
        console.shot('home_done')
        console.press('right')
        report('right shows it under Done, compressed, with what it saved', wait_until(lambda: any('[x]' in r and 'compressed, ' in r and ' saved' in r for r in console.rows()), 3),
               ' / '.join(console.rows()))
        console.shot('done_view')
        report('Done says what it was, what it takes now and what was freed', any('Was' in r and 'Now' in r and 'Freed' in r for r in console.rows()),
               ' / '.join(console.rows()))
        # With nothing ticked, the line below says what compressing came to; ticked again for what follows.
        console.type(' ')
        report('and with nothing ticked, how many were compressed and what that freed', wait_until(lambda: any('1 game compressed, ' in r and 'freed in all' in r for r in console.rows()), 3),
               ' / '.join(console.rows()))
        console.type(' ')
        wait_until(lambda: any('[x]' in r for r in console.rows()), 3)

        # V checks the game the pointer's on for damage, reading all of it back, which changes nothing about it.
        console.type('v')
        report('V checks the game for damage, reading all of it back', console.wait_for('Checked 1 game', 120) and console.wait_for('whole', 3)
               and console.wait_for('read back as it should', 3), ' / '.join(console.rows()))
        console.shot('checked')
        console.press('enter')
        report('back on the list, it is as it was before: under Done', console.wait_for(chip('M', 'More'), 5) and console.wait_for('Done 1', 3), ' / '.join(console.rows()))

        # A report goes in the test's own folder rather than on the desktop of whoever runs this, with nothing of the keys in it.
        report_path = os.path.join(work, 'hacBrewPackLZ report.txt')
        console.type('r')
        report('R opens Report a problem, with the report to read', console.wait_for('Report a problem', 5) and console.wait_for('What you were doing', 1)
               and console.wait_for('== The list', 3), ' / '.join(console.rows()))
        check_layout(console, 'report a problem', ['Report a problem', chip('W', 'Write what you were doing')])
        console.shot('report_page')
        console.type('w')
        time.sleep(0.3)
        console.type('It froze')
        time.sleep(0.3)
        console.press('enter')
        report('W writes what you were doing', wait_until(lambda: any('It froze' in r for r in console.rows()) and not any('> It froze' in r for r in console.rows()), 5),
               ' / '.join(console.rows()))
        # With nowhere to send it, as there's no report service in a test, saving it is all there is, so it's the one key for it.
        report('with nowhere to send it, S saves it, and there is no second key for that', any(chip('S', 'Save it') in r for r in console.rows())
               and not any(chip('F', 'Save instead') in r for r in console.rows()), ' / '.join(r for r in console.rows() if r))
        console.type('s')
        report('S saves it, and says so', console.wait_for('Report saved', 5), ' / '.join(console.rows()))
        none_cut(console, 'report saved')
        console.shot('report_saved')
        saved = open(report_path, encoding='utf-8').read() if os.path.exists(report_path) else ''
        secrets = []
        key_file = os.path.join(keys, 'prod.keys') if os.path.isdir(keys) else keys
        for line in open(key_file, encoding='utf-8', errors='replace'):
            value = ''.join(c for c in line.partition('=')[2] if c in '0123456789abcdefABCDEF').lower()
            if len(value) >= 16:
                secrets.append(value)
        report('the report has the list and what was said', 'hacBrewPackLZ' in saved and '== The list' in saved and 'A Test Game' in saved)
        report('and what you were doing, first', saved.startswith('What I was doing: It froze'), saved[:80])
        report('and no key in it', saved != '' and not any(s in saved.lower() for s in secrets))
        console.type('h')
        report('Help has the usual problems', console.wait_for("A game won't start", 3), ' / '.join(console.rows()))
        console.shot('help')
        console.press('esc')
        report('back to the list after help', console.wait_for(chip('M', 'More'), 3), ' / '.join(console.rows()))

        if large:
            # Stopping part way: Esc asks first, Y stops, and what came of it says so. Back on the list, the game is still to do.
            console.press('left')
            # Left on its own, before the path comes in as typing, as a dropped file does: arriving together, they'd be read as keys.
            console.wait_for('Everything here is done', 5)
            time.sleep(0.5)
            console.type(f'"{large}"')
            report('a big game is added, to be compressed', console.wait_for('Compress 1 game', 60), ' / '.join(console.rows()))
            console.type('c')
            report('while it compresses, it says how far it all is', console.wait_for('All together', 30), ' / '.join(console.rows()))
            time.sleep(3)
            check_layout(console, 'a job running', ['All together', chip('Esc', 'Stop')])
            console.shot('job_running_large')
            console.press('esc')
            report('Esc asks before stopping, with Y and N', console.wait_for('Stop this job?', 3) and console.wait_for(chip('Y', 'Stop'), 1) and console.wait_for(chip('N', 'Keep going'), 1),
                   ' / '.join(console.rows()))
            console.shot('stop_question')
            console.type('n')
            report('N keeps it going', console.wait_for(chip('Esc', 'Stop'), 3) and not any('Stop this job?' in r for r in console.rows()), ' / '.join(console.rows()))
            console.press('esc')
            console.wait_for('Stop this job?', 3)
            console.type('y')
            report('Y stops it, and it says so', console.wait_for('Stopped', 20) and console.wait_for('not done', 3), ' / '.join(console.rows()))
            console.shot('stopped')
            report('nothing is left half written', not [f for f in os.listdir(work) if f.endswith('.part') or f.endswith('.tmp')], str(os.listdir(work)))
            console.press('enter')
            report('back on the list, the game is still to do', console.wait_for('To do 1', 5) and console.wait_for('Compress 1 game', 3), ' / '.join(console.rows()))

        console.type('x')
        report('X clears the list', console.wait_for('Drop game files or folders', 3), ' / '.join(console.rows()))
        console.type('q')
        deadline = time.time() + 10
        while console.alive() and time.time() < deadline:
            time.sleep(0.2)
        report('Q closes it', not console.alive())

        frames = read_frames(screen_log) if os.path.exists(screen_log) else []
        changing = sum('Compressing' in frame for _, frame in frames)
        report('every frame is in the screen log, compressing included', len(frames) > 50 and changing > 0, f'{len(frames)} frames, {changing} while compressing')
        problems = cleared_before_written(frames)
        report('no row is cleared before it is written, which flickers', bool(frames) and not problems, f'{len(problems)} times, such as: ' + '; '.join(problems[:5]))
    finally:
        # These handles count as being connected to the console, and would keep its window open.
        kernel32.CloseHandle(console.output)
        kernel32.CloseHandle(console.input)
        kernel32.FreeConsole()
        subprocess.run(['taskkill', '/PID', str(console.pid), '/F'], capture_output=True, stdin=subprocess.DEVNULL)
        started = time.time()
        try:
            console.host.wait(5)
            print(f'window closed {time.time() - started:.1f} s after the program ended')
        except subprocess.TimeoutExpired:
            report('the window closes when the program ends', False)
            result = subprocess.run(['taskkill', '/PID', str(console.host.pid), '/F'], capture_output=True, text=True, errors='replace', stdin=subprocess.DEVNULL)
            print('taskkill:', result.stdout.strip(), result.stderr.strip())
            console.host.wait(30)
            print(f'window closed {time.time() - started:.1f} s after the program ended')
        check_asked_once(keys, work, settings)
        check_nsp_folder(keys, work, settings, source)
        if more:
            check_notes_room(keys, work, settings, source, more)
        if failures and SHOTS and os.path.exists(screen_log):
            # What it drew, frame by frame, to work out what went wrong.
            shutil.copyfile(screen_log, os.path.join(SHOTS, 'screen.log'))
        shutil.rmtree(work, ignore_errors=True)

    print(f'{failures} failure(s)')
    return 1 if failures else 0


TOOL = ''
SHOTS = None
if __name__ == '__main__':
    sys.exit(main())
