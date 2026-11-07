"""
Drives the console home screen in a real console window, the way someone double clicking the program would use it:
it has to stay open, take dropped paths, and redraw at any window size with nothing wrapped or left behind.

    python tests\\console_screen.py <tool.exe> <keys folder> <small.nsp> [folder for screenshots]

A window opens for a moment while this runs. Output goes to a temporary folder beside the copied input.
"""
import ctypes
import ctypes.wintypes as wt
import os
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
KEY_EVENT = 1
VK = {'esc': 0x1B, 'enter': 0x0D, 'down': 0x28, 'up': 0x26, 'delete': 0x2E}


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
        self.host = subprocess.Popen(['conhost.exe', tool, *args], cwd=cwd)
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
        user32.SetWindowPos(self.window, None, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER)

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
    """Nothing reaches the last column, the buffer is exactly the window, and what has to be on screen is there once."""
    time.sleep(0.3)
    width, height, buffer_width, buffer_height = console.size()
    rows = console.rows()
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


def main():
    global TOOL, SHOTS
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    TOOL = os.path.abspath(sys.argv[1])
    keys = os.path.abspath(sys.argv[2])
    source = os.path.abspath(sys.argv[3])
    SHOTS = os.path.abspath(sys.argv[4]) if len(sys.argv) > 4 else None
    if SHOTS:
        os.makedirs(SHOTS, exist_ok=True)

    work = tempfile.mkdtemp(prefix='hbplz_screen_', dir=os.path.dirname(source) if '--beside' in sys.argv else None)
    nsp = os.path.join(work, 'A Test Game [0123456789ABCDEF].nsp')
    shutil.copyfile(source, nsp)

    # A slow disk can take seconds over the first start of a program that's fallen out of the cache.
    subprocess.run([TOOL, '-h'], capture_output=True, stdin=subprocess.DEVNULL)
    console = Console(TOOL, keys, [])
    try:
        console.resize(1000, 560)
        report('home screen opens', console.wait_for('Drop game files or folders'), ' / '.join(console.rows()))
        time.sleep(2)
        report('stays open by itself', console.alive())
        check_layout(console, 'empty home screen', ['hacBrewPackLZ', 'Drop game files or folders', 'Options', 'More'])
        console.shot('home_empty')

        console.type(f'"{nsp}"')
        report('a dropped path is picked up', console.wait_for('Analyze'), ' / '.join(console.rows()))
        time.sleep(0.5)
        rows = console.rows()
        title_row = next((r for r in rows if 'base v' in r or 'DLC v' in r), '')
        report('the title is listed', bool(title_row), ' / '.join(rows))
        console.shot('home_title')

        console.type('a')
        report('analyze finishes and shows a size', console.wait_for('Nothing has been written', 60), ' / '.join(console.rows()))
        console.shot('home_analyzed')

        for width, height in [(1100, 700), (760, 420), (560, 340), (440, 300), (900, 500)]:
            console.resize(width, height)
            check_layout(console, 'titles', ['hacBrewPackLZ', 'Compress', 'More'])
            console.shot(f'home_{width}x{height}')

        console.resize(300, 180)
        time.sleep(0.4)
        report('a tiny window asks to be larger', console.wait_for('make this window larger', 3) or console.wait_for('hacBrewPackLZ', 1), ' / '.join(console.rows()))
        console.shot('home_tiny')
        console.resize(900, 500)
        time.sleep(0.4)

        console.type('p')
        time.sleep(0.3)
        console.type('x')
        time.sleep(0.3)
        info = CSBI()
        kernel32.GetConsoleScreenBufferInfo(console.output, ctypes.byref(info))
        rows = console.rows()
        typed = rows[info.dwCursorPosition.Y] if info.dwCursorPosition.Y < len(rows) else ''
        report('typing a path shows the cursor on its row', '> x' in typed, f'cursor on {typed!r} | ' + ' / '.join(r for r in rows if r)[-300:])
        console.press('esc')
        time.sleep(0.3)

        console.type('m')
        report('M opens the menu', console.wait_for('Send to Switch', 3) and console.wait_for('Check an NSP', 1), ' / '.join(console.rows()))
        rows = check_layout(console, 'menu', ['hacBrewPackLZ', 'Send to Switch'])
        labels = ['Extract to folders', 'Send to Switch', 'Untick all', 'Take this one off the list', 'Add a path', 'Check an NSP', 'Clear the list', 'Help']
        report('the menu uses the width of the window', any(sum(label in r for label in labels) > 1 for r in rows), ' / '.join(r for r in rows if r))
        report('options and updates are not under More', not any('Check for updates' in r or 'O  Options' in r for r in rows), ' / '.join(r for r in rows if r))
        console.shot('menu')
        console.press('esc')
        report('Esc closes the menu', console.wait_for('Space Tick', 3), ' / '.join(console.rows()))

        # Its letters are keys on the menu, so they mustn't be taken as such.
        console.type('m')
        time.sleep(0.3)
        console.type(f'"{work}"')
        time.sleep(2)
        rows = console.rows()
        report('a folder dropped on the open menu is added, and the menu closes', console.wait_for('Space Tick', 3) and not any('Compression level' in r for r in rows),
               ' / '.join(r for r in rows if r))

        # Help is last on the menu.
        console.type('m')
        time.sleep(0.3)
        console.hold('down', 20)
        time.sleep(0.3)
        console.press('enter')
        report('help opens from the menu, with how to use this screen first', console.wait_for('Using the home screen') and console.wait_for('Usage', 1),
               ' / '.join(console.rows()))
        for width, height in [(900, 500), (480, 320)]:
            console.resize(width, height)
            check_layout(console, 'help', ['Using the home screen'])
            console.shot(f'help_{width}x{height}')
        console.press('esc')
        console.resize(900, 500)
        report('help closes', console.wait_for('M More'), ' / '.join(console.rows()))

        console.type('o')
        report('options open', console.wait_for('Compression level'), ' / '.join(console.rows()))
        for key, last in [('down', 'Check for updates'), ('up', 'Compress game data')]:
            console.hold(key, 40)
            time.sleep(1)
            rows = console.rows()
            report(f'holding {key} goes through every option to {last}', last in console.pointed() and not any("Couldn't find" in r for r in rows),
                   ' / '.join(r for r in rows if r))
            if key == 'down':
                # Not pressed: it would ask the real site.
                report('checking for updates is the last option, done with Enter', any('Enter Check now' in r for r in rows), ' / '.join(r for r in rows if r))
                console.shot('options_check_for_updates')
        for width, height in [(900, 600), (560, 340)]:
            console.resize(width, height)
            rows = check_layout(console, 'options', ['hacBrewPackLZ'])
            report(f'the whole description shows at {width}x{height}', any(r.endswith('like hacBrewPack.') for r in rows), ' / '.join(r for r in rows if r))
            console.shot(f'options_{width}x{height}')
        console.press('esc')
        console.resize(900, 500)
        report('options close', console.wait_for('M More'), ' / '.join(console.rows()))

        console.type(' ')
        report('space unticks the title', console.wait_for('[ ]', 3), ' / '.join(console.rows()))
        console.type('c')
        report('with nothing ticked, compress says so', console.wait_for('Nothing is ticked', 3), ' / '.join(console.rows()))
        console.type('t')
        report('T ticks everything', console.wait_for('[x]', 3), ' / '.join(console.rows()))

        console.type('c')
        report('compressing shows its screen, then finishes', console.wait_for('succeeded', 180), ' / '.join(console.rows()))
        console.shot('job_finished')
        check_layout(console, 'finished job', ['hacBrewPackLZ'])
        for width, height in [(560, 340), (1100, 700)]:
            console.resize(width, height)
            check_layout(console, 'finished job', ['hacBrewPackLZ'])
        made = [f for f in os.listdir(work) if '[LZ4]' in f and f.endswith('.nsp')]
        report('the compressed NSP is beside the input', len(made) == 1, str(os.listdir(work)))
        report('no partial files are left', not [f for f in os.listdir(work) if f.endswith('.part') or f.endswith('.tmp')], str(os.listdir(work)))
        report('the window is still open after the job', console.alive())
        report('it offers to show what was written', console.wait_for('Show in folder', 3), ' / '.join(console.rows()))

        console.press('enter')
        report('back to the list', console.wait_for('M More'), ' / '.join(console.rows()))
        console.shot('home_done')

        console.type('m')
        time.sleep(0.3)
        console.type('v')
        report('V on the menu asks for an NSP to check', console.wait_for('NSP to check', 3), ' / '.join(console.rows()))
        if made:
            console.type(f'"{os.path.join(work, made[0])}"')
            time.sleep(0.5)
            for _ in range(3):
                console.press('enter')
                time.sleep(0.3)
            report('it checks the NSP it made', console.wait_for('All checks passed', 120), ' / '.join(console.rows()))
            console.shot('checked')
            console.press('enter')
        report('back to the list after checking', console.wait_for('M More'), ' / '.join(console.rows()))

        console.type('x')
        report('X clears the list', console.wait_for('Drop game files or folders', 3), ' / '.join(console.rows()))
        console.type('q')
        deadline = time.time() + 10
        while console.alive() and time.time() < deadline:
            time.sleep(0.2)
        report('Q closes it', not console.alive())
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
        shutil.rmtree(work, ignore_errors=True)

    print(f'{failures} failure(s)')
    return 1 if failures else 0


TOOL = ''
SHOTS = None
if __name__ == '__main__':
    sys.exit(main())
