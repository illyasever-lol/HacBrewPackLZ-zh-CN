"""
Opens the GUI and walks through its windows and states, saving a screenshot of each and checking that it behaves.
Clicks are sent to the windows themselves, so the mouse stays where it is. Windows open for a short while as it runs.
It uses its own settings file, and the file it compresses is a temporary copy, so nothing is written beside the original.

    python tests\\gui_shots.py <hacbrewpacklz-gui.exe> <keys folder> <small.nsp> <large.nsp> <work folder> <folder for screenshots> [more paths to list]...

The large NSP should take at least half a minute to compress. It's compressed only until the Stop button is tested,
into the work folder, which should have a few GB free.
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

user32 = ctypes.WinDLL('user32', use_last_error=True)
user32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
user32.FindWindowW.restype = wt.HWND
user32.FindWindowExW.argtypes = [wt.HWND, wt.HWND, wt.LPCWSTR, wt.LPCWSTR]
user32.FindWindowExW.restype = wt.HWND
user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.SendMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.SendMessageW.restype = ctypes.c_ssize_t
user32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wt.UINT]
user32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.IsWindow.argtypes = [wt.HWND]
user32.IsWindowVisible.argtypes = [wt.HWND]
user32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
SendText = ctypes.WinDLL('user32').SendMessageW
SendText.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPCWSTR]
SendText.restype = ctypes.c_ssize_t
GetText = ctypes.WinDLL('user32').SendMessageW
GetText.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPWSTR]
GetText.restype = ctypes.c_ssize_t

WM_SETTEXT = 0x000C
WM_GETTEXT = 0x000D
WM_CLOSE = 0x0010
WM_KEYDOWN = 0x0100
WM_COMMAND = 0x0111
WM_MOUSEMOVE = 0x0200
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
SWP_NOMOVE = 2
SWP_NOZORDER = 4
VK_ESCAPE = 0x1B
IDOK = 1
IDCANCEL = 2
IDYES = 6

WINDOW_CLASS = 'hacBrewPackLZ.Window'
POPUP_CLASS = 'hacBrewPackLZ.Popup'

failures = 0
shots = ''
settings = ''


def report(name, ok, extra=''):
    global failures
    failures += 0 if ok else 1
    print(f"{'PASS' if ok else 'FAIL'}: {name}{' - ' + extra if extra and not ok else ''}", flush=True)


def lparam(x, y):
    return (y << 16) | (x & 0xFFFF)


def click(hwnd, x, y, settle=0.25):
    user32.SendMessageW(hwnd, WM_MOUSEMOVE, 0, lparam(x, y))
    user32.PostMessageW(hwnd, WM_LBUTTONDOWN, 1, lparam(x, y))
    user32.PostMessageW(hwnd, WM_LBUTTONUP, 0, lparam(x, y))
    time.sleep(settle)


def hover(hwnd, x, y):
    user32.SendMessageW(hwnd, WM_MOUSEMOVE, 0, lparam(x, y))
    time.sleep(0.15)


def client_size(hwnd):
    rect = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rect))
    return rect.right, rect.bottom


def find(cls, title, seconds=10):
    deadline = time.time() + seconds
    while time.time() < deadline:
        hwnd = user32.FindWindowW(cls, title)
        if hwnd:
            return hwnd
        time.sleep(0.05)
    return None


def shot(hwnd, name):
    time.sleep(0.15)
    winshot.capture(hwnd, os.path.join(shots, name + '.png'))


def close(hwnd):
    user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
    deadline = time.time() + 5
    while user32.IsWindow(hwnd) and time.time() < deadline:
        time.sleep(0.05)
    return not user32.IsWindow(hwnd)


class Layout:
    """Where things are in the main window, measured from the right and bottom edges as its layout does."""

    def __init__(self, hwnd, scale=1.0):
        self.hwnd = hwnd
        self.scale = scale

    def s(self, value):
        return int(value * self.scale)

    @property
    def size(self):
        return client_size(self.hwnd)

    def options(self):
        return self.size[0] - self.s(130), self.s(33)

    def more(self):
        return self.size[0] - self.s(38), self.s(33)

    def bar(self, from_right):
        width, height = self.size
        return width - self.s(from_right), height - self.s(34)

    def compress(self):
        return self.bar(70)

    def compress_menu(self):
        return self.bar(24)

    def analyze(self):
        return self.bar(175)

    def details(self):
        # Where Analyze is, while there's nothing left to analyze.
        return self.bar(175)

    def stop(self):
        return self.bar(50)

    def empty_buttons(self):
        width, height = self.size
        middle_x = width // 2
        middle_y = self.s(62) + (height - self.s(68) - self.s(8) - self.s(62)) // 2 + self.s(22) + self.s(18)
        return middle_x, middle_y


def write_settings(*lines):
    with open(settings, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')


def open_gui(tool, cwd, inputs, **env):
    environment = dict(os.environ)
    environment['HACBREWPACKLZ_SETTINGS'] = settings
    environment.update(env)
    process = subprocess.Popen([tool, *inputs], cwd=cwd, env=environment)
    window = find(WINDOW_CLASS, 'hacBrewPackLZ')
    return process, window


def stop_process(process):
    # Along with the crash handler, if that's what is holding it open, so no dialog stays behind on the desktop.
    subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], capture_output=True, stdin=subprocess.DEVNULL)
    process.wait(8)


def finish(process, window):
    if window and user32.IsWindow(window):
        close(window)
    try:
        code = process.wait(15)
    except subprocess.TimeoutExpired:
        stop_process(process)
        report('the program ends when closed', False)
        return False
    if code != 0:
        report('the program ends cleanly', False, f'exit code {code & 0xFFFFFFFF:#x}')
    return code == 0


def walk_empty(tool, keys):
    process, window = open_gui(tool, keys, [])
    report('the main window opens', bool(window))
    if not window:
        return
    time.sleep(0.8)
    shot(window, 'empty')
    layout = Layout(window)

    click(window, *layout.more())
    popup = find(POPUP_CLASS, '', 3)
    report('the more menu opens', bool(popup))
    if popup:
        hover(popup, 40, 20)
        shot(popup, 'menu')
        user32.PostMessageW(popup, WM_KEYDOWN, VK_ESCAPE, 0)
        time.sleep(0.2)

    click(window, *layout.options())
    options = find(WINDOW_CLASS, 'Options', 3)
    report('options open', bool(options))
    if options:
        for page in range(6):
            click(options, 90, 58 + page * 38 + 17)
            shot(options, f'options_{page}')
        click(options, 90, 58 + 17)
        click(options, 600, 248)
        popup = find(POPUP_CLASS, '', 2)
        report('a choice opens its list', bool(popup))
        if popup:
            shot(popup, 'dropdown')
            user32.PostMessageW(popup, WM_KEYDOWN, VK_ESCAPE, 0)
            time.sleep(0.2)
        report('options close', close(options))

    click(window, *layout.more())
    popup = find(POPUP_CLASS, '', 3)
    if popup:
        click(popup, 60, 20)
    verify = find(WINDOW_CLASS, 'Check an NSP', 3)
    report('check an NSP opens', bool(verify))
    if verify:
        shot(verify, 'verify')
        close(verify)

    report('closes when asked', finish(process, window))


def walk_search(tool, keys, location):
    process, window = open_gui(tool, keys, [])
    if not window:
        return
    time.sleep(0.8)
    x, y = Layout(window).empty_buttons()
    click(window, x + 130, y)
    search = find(WINDOW_CLASS, 'Search a location', 3)
    report('search opens', bool(search))
    if not search:
        finish(process, window)
        return
    edit = user32.FindWindowExW(search, None, 'EDIT', None)
    SendText(edit, WM_SETTEXT, 0, location)
    width, _ = client_size(search)
    click(search, width - 50, 33)
    time.sleep(2)
    shot(search, 'search_results')

    second = user32.FindWindowExW(search, edit, 'EDIT', None)
    SendText(second, WM_SETTEXT, 0, 'knight')
    time.sleep(0.5)
    shot(search, 'search_filtered')

    # Choose the first result, then add what's chosen.
    width, height = client_size(search)
    click(search, width - 70, height - 32)
    time.sleep(4)
    report('search adds what was chosen', not user32.IsWindow(search))
    shot(window, 'after_search')
    finish(process, window)


def walk_titles(tool, keys, nsp, extra):
    process, window = open_gui(tool, keys, [nsp, *extra])
    if not window:
        report('the main window opens with files', False)
        return
    time.sleep(5)
    shot(window, 'titles')
    layout = Layout(window)
    width, height = layout.size

    hover(window, width // 2, 62 + 34 + 28)
    shot(window, 'titles_hover')

    click(window, *layout.analyze())
    time.sleep(2.5)
    shot(window, 'analyzing')
    time.sleep(60)
    shot(window, 'analyzed')

    click(window, *layout.compress_menu())
    popup = find(POPUP_CLASS, '', 3)
    report('the arrow on Compress opens what else can be done', bool(popup))
    if popup:
        shot(popup, 'compress_menu')
        user32.PostMessageW(popup, WM_KEYDOWN, VK_ESCAPE, 0)
        time.sleep(0.2)

    user32.SetWindowPos(window, None, 0, 0, 676, 499, SWP_NOMOVE | SWP_NOZORDER)
    time.sleep(0.3)
    shot(window, 'small')
    user32.SetWindowPos(window, None, 0, 0, 1200, 760, SWP_NOMOVE | SWP_NOZORDER)
    time.sleep(0.3)
    shot(window, 'large')
    report('closes with a list', finish(process, window))


def walk_compress(tool, keys, nsp):
    process, window = open_gui(tool, keys, [nsp])
    if not window:
        report('the main window opens for compressing', False)
        return
    time.sleep(3)
    layout = Layout(window)
    click(window, *layout.compress())

    folder = os.path.dirname(nsp)
    deadline = time.time() + 120
    made = []
    while time.time() < deadline:
        made = [f for f in os.listdir(folder) if '[LZ4]' in f and f.endswith('.nsp')]
        if made:
            break
        time.sleep(0.3)
    time.sleep(1)
    report('compressing makes an NSP beside the input', len(made) == 1, str(os.listdir(folder)))
    shot(window, 'compressed')

    # The log is a text box of its own lying over the list, so it has to really go when Details is clicked again.
    log = user32.FindWindowExW(window, None, 'EDIT', None)
    click(window, *layout.details())
    time.sleep(0.3)
    shot(window, 'details')
    report('Details opens the log', bool(log) and bool(user32.IsWindowVisible(log)))
    click(window, *layout.details())
    time.sleep(0.3)
    shot(window, 'details_closed')
    report('clicking Details again closes it', bool(log) and not user32.IsWindowVisible(log))
    click(window, *layout.details())
    time.sleep(0.3)
    report('and clicking again opens it', bool(log) and bool(user32.IsWindowVisible(log)))

    # Pressing Compress again must not disturb what's finished: there's nothing left for it to do.
    click(window, *layout.compress())
    time.sleep(0.8)
    shot(window, 'details_after_extra_clicks')
    report('closes after a job', finish(process, window))


# Where the first row's tick box is, from the top left as the window's layout has it.
FIRST_TICK = (46, 62 + 34 + 28)


def walk_minimum(tool, keys, nsp):
    """Unticking a title, and the minimum saving leaving one out until it's ticked by hand."""
    folder = os.path.dirname(nsp)

    def made():
        return [f for f in os.listdir(folder) if '[LZ4]' in f and f.endswith('.nsp')]

    for old in made():
        os.remove(os.path.join(folder, old))

    process, window = open_gui(tool, keys, [nsp])
    if not window:
        report('the main window opens for ticking', False)
        return
    time.sleep(3)
    layout = Layout(window)
    click(window, *FIRST_TICK)
    time.sleep(0.3)
    shot(window, 'unticked')
    click(window, *layout.compress())
    time.sleep(3)
    report('an unticked title is not compressed', not made(), str(made()))
    finish(process, window)

    # It should save far less than this, so the minimum leaves it out.
    write_settings('--minsaving=50%')
    process, window = open_gui(tool, keys, [nsp])
    if not window:
        report('the main window opens with a minimum', False)
        return
    time.sleep(3)
    layout = Layout(window)
    click(window, *layout.analyze())
    time.sleep(12)
    shot(window, 'below_minimum')
    click(window, *layout.compress())
    time.sleep(3)
    report('a title below the minimum is left out', not made(), str(made()))

    click(window, *FIRST_TICK)
    time.sleep(0.3)
    click(window, *layout.compress())
    deadline = time.time() + 120
    while not made() and time.time() < deadline:
        time.sleep(0.3)
    time.sleep(1)
    report('ticking it by hand compresses it anyway', len(made()) == 1, str(os.listdir(folder)))
    shot(window, 'ticked_by_hand')
    report('closes after ticking', finish(process, window))
    write_settings()
    for old in made():
        os.remove(os.path.join(folder, old))


def walk_stop(tool, keys, large, work):
    """A real job on a big file: its progress, Stop, and closing the window part way through."""
    folder = os.path.join(work, 'stop')
    os.makedirs(folder, exist_ok=True)
    write_settings(f'--nspdir={folder}')
    process, window = open_gui(tool, keys, [large])
    if not window:
        report('the main window opens for stopping', False)
        return
    time.sleep(5)
    layout = Layout(window)
    click(window, *layout.compress())
    time.sleep(15)
    shot(window, 'progress')
    left = os.listdir(folder)
    report('a job in progress is writing', any(f.endswith('.part') for f in left), str(left))

    click(window, *layout.stop())
    time.sleep(4)
    shot(window, 'stopped')
    left = os.listdir(folder)
    report('Stop leaves nothing behind', not left, str(left))

    # Now the same again, by closing the window and answering yes.
    click(window, *layout.compress())
    time.sleep(8)
    user32.PostMessageW(window, WM_CLOSE, 0, 0)
    question = find('#32770', 'hacBrewPackLZ', 5)
    report('closing during a job asks first', bool(question))
    if question:
        shot(question, 'close_question')
        user32.PostMessageW(question, WM_COMMAND, IDYES, 0)
    deadline = time.time() + 20
    while process.poll() is None and time.time() < deadline:
        time.sleep(0.2)
    report('then it stops and closes', process.poll() == 0, str(process.poll()))
    left = os.listdir(folder)
    report('and leaves nothing behind', not left, str(left))
    if process.poll() is None:
        stop_process(process)
    write_settings()


def type_name(edit, name):
    """
    The picker empties its name box once it has loaded, which can take a while on a busy PC, so this checks it stays.
    OK must wait for that too: pressed any earlier, Windows' picker hangs and even Cancel won't close it, in any program.
    """
    buffer = ctypes.create_unicode_buffer(1024)
    deadline = time.time() + 20
    while time.time() < deadline:
        SendText(edit, WM_SETTEXT, 0, name)
        time.sleep(1)
        GetText(edit, WM_GETTEXT, 1024, buffer)
        if buffer.value == name:
            return True
    return False


def walk_dialog(tool, keys, nsp):
    process, window = open_gui(tool, keys, [])
    if not window:
        return
    time.sleep(0.8)
    x, y = Layout(window).empty_buttons()
    click(window, x - 150, y)
    dialog = find('#32770', 'Add game files', 5)
    report('the add files picker opens', bool(dialog))
    if dialog:
        shot(dialog, 'picker')
        edit = dialog
        for cls in ('ComboBoxEx32', 'ComboBox', 'Edit'):
            edit = user32.FindWindowExW(edit, None, cls, None) if edit else None
        typed = bool(edit) and type_name(edit, nsp)
        report('its name box takes a path', typed)

        user32.PostMessageW(window, WM_CLOSE, 0, 0)
        time.sleep(0.5)
        report('the main window stays open while the picker is', bool(user32.IsWindow(window)))

        if typed:
            user32.PostMessageW(dialog, WM_COMMAND, IDOK, 0)
        deadline = time.time() + 15
        while typed and user32.IsWindow(dialog) and time.time() < deadline:
            time.sleep(0.05)
        report('the picker takes the file', typed and not user32.IsWindow(dialog))
        if user32.IsWindow(dialog):
            user32.PostMessageW(dialog, WM_COMMAND, IDCANCEL, 0)
        time.sleep(3)
        shot(window, 'picked')
    report('closes after the picker', finish(process, window))


def walk_no_keys(tool, nsp, work):
    write_settings('--keyset=' + os.path.join(work, 'no such file.keys'))
    process, window = open_gui(tool, work, [nsp])
    if window:
        time.sleep(2.5)
        shot(window, 'no_keys')
        finish(process, window)
    write_settings()


def walk_themes(tool, keys, nsp):
    for name, env in [('light', {'HACBREWPACKLZ_GUI_THEME': 'light'}), ('dpi150', {'HACBREWPACKLZ_GUI_DPI': '144'})]:
        process, window = open_gui(tool, keys, [nsp], **env)
        if not window:
            report(f'{name}: opens', False)
            continue
        time.sleep(3)
        shot(window, name)
        scale = 1.5 if name == 'dpi150' else 1.0
        layout = Layout(window, scale)
        click(window, *layout.options())
        options = find(WINDOW_CLASS, 'Options', 3)
        if options:
            shot(options, f'{name}_options')
            close(options)
        finish(process, window)


def main():
    global shots, settings
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    tool = os.path.abspath(sys.argv[1])
    keys = os.path.abspath(sys.argv[2])
    source = os.path.abspath(sys.argv[3])
    large = os.path.abspath(sys.argv[4])
    work_root = os.path.abspath(sys.argv[5])
    shots = os.path.abspath(sys.argv[6])
    extra = sys.argv[7:]

    os.makedirs(work_root, exist_ok=True)
    os.makedirs(shots, exist_ok=True)
    work = tempfile.mkdtemp(prefix='hbplz_gui_', dir=work_root)
    settings = os.path.join(work, 'settings.txt')
    write_settings()
    nsp = os.path.join(work, 'A Test Game [0123456789ABCDEF].nsp')
    shutil.copyfile(source, nsp)
    try:
        walk_empty(tool, keys)
        walk_search(tool, keys, os.path.dirname(os.path.dirname(large)))
        walk_dialog(tool, keys, nsp)
        walk_no_keys(tool, nsp, work)
        walk_titles(tool, keys, nsp, extra)
        walk_compress(tool, keys, nsp)
        walk_minimum(tool, keys, nsp)
        walk_themes(tool, keys, nsp)
        walk_stop(tool, keys, large, work)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print(f'{failures} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
