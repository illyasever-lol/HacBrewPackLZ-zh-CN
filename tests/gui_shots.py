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
import http.server
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import winshot

user32 = ctypes.WinDLL('user32', use_last_error=True)
user32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
user32.FindWindowW.restype = wt.HWND
user32.FindWindowExW.argtypes = [wt.HWND, wt.HWND, wt.LPCWSTR, wt.LPCWSTR]
user32.FindWindowExW.restype = wt.HWND
user32.ChildWindowFromPointEx.argtypes = [wt.HWND, wt.POINT, wt.UINT]
user32.ChildWindowFromPointEx.restype = wt.HWND
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
WM_RBUTTONUP = 0x0205
SWP_NOMOVE = 2
SWP_NOZORDER = 4
# Resizing without it brings the window to the front, where whatever someone is typing elsewhere would go to it.
SWP_NOACTIVATE = 0x10
VK_ESCAPE = 0x1B
VK_END = 0x23
VK_RETURN = 0x0D
IDOK = 1
IDCANCEL = 2
IDYES = 6

WINDOW_CLASS = 'hacBrewPackLZ.Window'
POPUP_CLASS = 'hacBrewPackLZ.Popup'

failures = 0
shots = ''
settings = ''
layout_file = ''


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


def wait_for_name(layout, name, seconds=5):
    """Whether the main window shows the button or box of that name, in time."""
    deadline = time.time() + seconds
    while layout.text(name) is None and time.time() < deadline:
        time.sleep(0.1)
    return layout.text(name) is not None


def sections(layout):
    """The options' sections down the side, by their names in the layout, in order."""
    names = []
    while layout.text(f'section_{len(names)}') is not None:
        names.append(f'section_{len(names)}')
    return names


def section_titled(layout, title):
    return next((name for name in sections(layout) if layout.text(name) == title), None)


def edit_at(window, x, y):
    """The text box of the main window at that point, which is a window of its own."""
    point = wt.POINT(x, y)
    return user32.ChildWindowFromPointEx(window, point, 1)


def client_size(hwnd):
    rect = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rect))
    return rect.right, rect.bottom


# The program the test started. Only its windows are looked for, so a copy someone has open is never touched.
own_pid = None

EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
user32.EnumWindows.argtypes = [EnumWindowsProc, wt.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
user32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]


def find(cls, title, seconds=10):
    """A window of the program the test started with this class and title, or of any program for Windows' own dialogs."""
    deadline = time.time() + seconds
    while time.time() < deadline:
        found = []

        def each(hwnd, _):
            pid = wt.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if own_pid is not None and pid.value != own_pid:
                return True
            name = ctypes.create_unicode_buffer(256)
            user32.GetClassNameW(hwnd, name, 256)
            text = ctypes.create_unicode_buffer(256)
            user32.GetWindowTextW(hwnd, text, 256)
            if name.value == cls and text.value == title:
                found.append(hwnd)
                return False
            return True

        user32.EnumWindows(EnumWindowsProc(each), 0)
        if found:
            return found[0]
        time.sleep(0.05)
    return None


def dialog_text(hwnd):
    """What a message box says, from its text controls."""
    said, child = [], None
    while True:
        child = user32.FindWindowExW(hwnd, child, 'Static', None)
        if not child:
            return ' '.join(t for t in said if t)
        text = ctypes.create_unicode_buffer(512)
        user32.GetWindowTextW(child, text, 512)
        said.append(text.value)


def shot(hwnd, name):
    # What moves (a page fading in, a menu coming out) is in its place after 0.16 s.
    time.sleep(0.3)
    winshot.capture(hwnd, os.path.join(shots, name + '.png'))


def close(hwnd):
    user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
    deadline = time.time() + 5
    while user32.IsWindow(hwnd) and time.time() < deadline:
        time.sleep(0.05)
    return not user32.IsWindow(hwnd)


class Layout:
    """Where the main window's buttons are, from what it writes each time it lays itself out (HACBREWPACKLZ_GUI_LAYOUT)."""

    def __init__(self, hwnd, scale=1.0):
        self.hwnd = hwnd
        self.scale = scale

    def s(self, value):
        return int(value * self.scale)

    @property
    def size(self):
        return client_size(self.hwnd)

    def where(self, name):
        """The middle of a button, waiting a moment for it to be laid out where it's expected to be shown."""
        deadline = time.time() + 3
        while True:
            try:
                with open(layout_file, encoding='utf-8') as f:
                    for line in f:
                        parts = line.split()
                        if parts and parts[0] == name:
                            x, y, w, h = map(int, parts[1:5])
                            return x, y, w, h
            except OSError:
                pass
            if time.time() > deadline:
                raise AssertionError(f'{name} is not on the window')
            time.sleep(0.1)

    def text(self, name):
        """What a button says, as the window last laid it out, or None while it isn't shown."""
        try:
            with open(layout_file, encoding='utf-8') as f:
                for line in f:
                    parts = line.rstrip('\n').split(' ', 5)
                    if parts and parts[0] == name:
                        return parts[5] if len(parts) > 5 else ''
        except OSError:
            pass
        return None

    def middle(self, name):
        x, y, w, h = self.where(name)
        return x + w // 2, y + h // 2

    def options(self):
        return self.middle('options')

    def more(self):
        return self.middle('more')

    def add(self):
        return self.middle('add')

    def compress(self):
        return self.middle('compress')

    def analyze(self):
        return self.middle('analyze')

    def details(self):
        return self.middle('details')

    def stop(self):
        return self.middle('stop')

    def back_to_list(self):
        return self.middle('back_to_list')

    def heading(self):
        """What the list says along its top: a job's headline, or the views with how many each has, the one shown starred."""
        return self.text('heading') or ''

    def add_files(self):
        return self.middle('add_files')

    def search(self):
        return self.middle('search')


def write_settings(*lines, welcomed=True, asked=True):
    """Every run but the welcome's own is one where the welcome has been and gone, and with it the question about sending problems."""
    with open(settings, 'w', encoding='utf-8') as f:
        f.write('\n'.join([*lines, *(['welcomed=1'] if welcomed else []), *(['problemsasked=1'] if welcomed and asked else [])]) + '\n')


def open_gui(tool, cwd, inputs, **env):
    environment = dict(os.environ)
    environment['HACBREWPACKLZ_SETTINGS'] = settings
    environment['HACBREWPACKLZ_GUI_LAYOUT'] = layout_file
    # Nowhere to send reports, other than a stand-in a walk gives it, so a test never reaches the real report service.
    environment['HACBREWPACKLZ_REPORT_SITE'] = 'none'
    if os.path.exists(layout_file):
        os.remove(layout_file)
    environment.update(env)
    global own_pid
    process = subprocess.Popen([tool, *inputs], cwd=cwd, env=environment)
    own_pid = process.pid
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


def walk_welcome(tool, keys):
    """The first run opens on the welcome, in the main window where the list goes, until Start. It only comes the once."""
    write_settings('--noautoupdate=1', welcomed=False)
    process, window = open_gui(tool, keys, [])
    if not window:
        return
    layout = Layout(window)
    deadline = time.time() + 5
    while layout.text('start') is None and time.time() < deadline:
        time.sleep(0.1)
    report('the first run opens on the welcome', layout.text('start') == 'Start', str(layout.text('start')))
    report('with nothing of the list showing yet', layout.text('add_files') is None and layout.text('options') is None)
    report('and all of it fits without scrolling', layout.text('welcome_overflow') == '0', str(layout.text('welcome_overflow')))
    shot(window, 'welcome')
    with open(settings, encoding='utf-8') as f:
        report('it waits for Start before counting as seen', 'welcomed' not in f.read())
    if layout.text('start') is not None:
        click(window, *layout.middle('start'))
        deadline = time.time() + 5
        while layout.text('add_files') is None and time.time() < deadline:
            time.sleep(0.1)
        report('start goes on to the list', layout.text('add_files') is not None and layout.text('start') is None)
        shot(window, 'welcome_started')
    with open(settings, encoding='utf-8') as f:
        report('having been welcomed is kept', 'welcomed=1' in f.read())
    finish(process, window)

    process, window = open_gui(tool, keys, [])
    layout = Layout(window)
    deadline = time.time() + 5
    while layout.text('add_files') is None and time.time() < deadline:
        time.sleep(0.1)
    report('the welcome only comes the once', layout.text('start') is None and layout.text('add_files') is not None)

    # It can be had again, from the end of the options.
    click(window, *layout.options())
    if wait_for_name(layout, 'section_0') and section_titled(layout, 'This app'):
        click(window, *layout.middle(section_titled(layout, 'This app')))
        click(window, *layout.middle('show-welcome'))
        report('the options show the welcome again', wait_for_name(layout, 'start'))
        click(window, *layout.middle('start'))
        report('and start goes back to the list', wait_for_name(layout, 'add_files'))
    finish(process, window)
    write_settings()


def walk_problems_question(tool, keys):
    """Whoever had the welcome before sending problems to the developer was on it, as on 1.4.3, is asked the once, in the main window, and
    the answer is kept."""
    write_settings('--noautoupdate=1', asked=False)
    process, window = open_gui(tool, keys, [])
    if not window:
        report('the main window opens for the question', False)
        return
    time.sleep(0.8)
    layout = Layout(window)
    report('someone who had the welcome before is asked about sending problems', wait_for_name(layout, 'no_thanks') and layout.text('send_problems') == 'Send them',
           str(layout.text('send_problems')))
    shot(window, 'problems_question')
    click(window, *layout.middle('no_thanks'))
    report('no thanks goes on to the list', wait_for_name(layout, 'add_files'))
    with open(settings, encoding='utf-8') as f:
        saved = f.read()
    report('and that is kept, with sending left off', 'problemsasked=1' in saved and '--sendproblems=1' not in saved, saved)
    report('closes after the question', finish(process, window))
    process, window = open_gui(tool, keys, [])
    if window:
        time.sleep(0.8)
        layout = Layout(window)
        report('and it is not asked again', wait_for_name(layout, 'add_files') and layout.text('no_thanks') is None)
        finish(process, window)
    write_settings()


def walk_empty(tool, keys):
    # A report goes in a folder of the test's own, rather than on the desktop of whoever runs this.
    reports = tempfile.mkdtemp(prefix='hbplz_reports_')
    process, window = open_gui(tool, keys, [], HACBREWPACKLZ_REPORT_FOLDER=reports)
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
    report('options open, in the main window', wait_for_name(layout, 'done') and wait_for_name(layout, 'back'))
    if layout.text('done') is not None:
        # What's only for building NSPs by hand stays on the command line.
        report('the options are only what the GUI goes by', [layout.text(n) for n in sections(layout)] == ['Compression', 'Which games are worth it', 'Output', 'Keys', 'This app'],
               str([layout.text(n) for n in sections(layout)]))
        for page, name in enumerate(sections(layout)):
            click(window, *layout.middle(name))
            shot(window, f'options_{page}')
            for flag in ('keepncadir', 'ncadir', 'keygeneration', 'keyareakey', 'sdkversion', 'plaintext', 'nosignncasig2', 'titleversion', 'requiredsystemversion',
                         'blocksize', 'tempdir'):
                if layout.text(flag) is not None:
                    report(f'--{flag} is only on the command line', False)
        # What only goes with compressing goes while compression is off, as on the Switch and Android, and comes back with it.
        click(window, *layout.middle(section_titled(layout, 'Compression')))
        had = layout.text('level') is not None
        click(window, *layout.middle('nocompress'))
        time.sleep(0.4)
        gone = layout.text('level') is None and layout.text('nocompress') is not None
        click(window, *layout.middle(section_titled(layout, 'Which games are worth it')))
        gone = gone and layout.text('minsaving') is None and layout.text('measure') is None and layout.text('hidesmall') is not None
        shot(window, 'options_compression_off')
        click(window, *layout.middle(section_titled(layout, 'Compression')))
        click(window, *layout.middle('nocompress'))
        time.sleep(0.4)
        back = layout.text('level') is not None
        report('with compression off, its level, the minimum saving and thorough analysis go, and come back with it', had and gone and back,
               f'shown before {had}, gone {gone}, back {back}')
        click(window, *layout.middle(section_titled(layout, 'Which games are worth it')))
        click(window, *layout.middle('minsaving'))
        popup = find(POPUP_CLASS, '', 2)
        report('a choice opens its list', bool(popup))
        if popup:
            shot(popup, 'dropdown')
            user32.PostMessageW(popup, WM_KEYDOWN, VK_ESCAPE, 0)
            time.sleep(0.2)
        click(window, *layout.middle('done'))
        report('options close', wait_for_name(layout, 'add_files') and layout.text('done') is None)

    # Help is first on the menu of an empty list: it has the app's own things, as games are checked from the list now.
    click(window, *layout.more())
    popup = find(POPUP_CLASS, '', 3)
    if popup:
        click(popup, 60, 4 + 16)
    report('help opens', wait_for_name(layout, 'report'))
    if layout.text('report') is not None:
        shot(window, 'help')
        click(window, *layout.middle('topic_1'))
        time.sleep(0.3)
        shot(window, 'help_topic')
        click(window, *layout.middle('report'))
        # A page of its own, with the report to read through. With nowhere to send it, saving it is all there is, so that's the one button.
        report('report a problem opens its page, with saving it the one thing to do', wait_for_name(layout, 'send') and layout.text('send') == 'Save it'
               and layout.text('save') is None, f"{layout.text('send')} / {layout.text('save')}")
        shot(window, 'report_page')
        saved = os.path.join(reports, 'hacBrewPackLZ report.txt')
        SendText(edit_at(window, *layout.middle('said')), WM_SETTEXT, 0, 'It froze')
        click(window, *layout.middle('send'))
        deadline = time.time() + 5
        while not os.path.exists(saved) and time.time() < deadline:
            time.sleep(0.1)
        report('save it saves the report', os.path.exists(saved), str(os.listdir(reports)))
        text = open(saved, encoding='utf-8').read() if os.path.exists(saved) else ''
        report('with what was typed first', text.startswith('What I was doing: It froze'), text[:60])
        report('then the button is Done', wait_until(lambda: layout.text('send') == 'Done', 3), str(layout.text('send')))
        shot(window, 'report_saved')
        # A page takes the place of the one before, so the way back from one is to the list.
        click(window, *layout.middle('send'))
        report('done goes back to the list', wait_for_name(layout, 'add_files') and layout.text('send') is None)
    shutil.rmtree(reports, ignore_errors=True)

    # About is last: after Help, Report a problem and Check for a new version.
    click(window, *layout.more())
    popup = find(POPUP_CLASS, '', 3)
    if popup:
        click(popup, 60, 4 + 96 + 16)
    report('about opens', wait_for_name(layout, 'done'))
    if layout.text('done') is not None:
        shot(window, 'about')
        click(window, *layout.middle('back'))
        report('back goes back to the list', wait_for_name(layout, 'add_files'))

    report('closes when asked', finish(process, window))


def key_values(keys):
    """The keys' values, to look for in what's sent. Never printed."""
    values = []
    key_file = os.path.join(keys, 'prod.keys') if os.path.isdir(keys) else keys
    for line in open(key_file, encoding='utf-8', errors='replace'):
        value = ''.join(c for c in line.partition('=')[2] if c in '0123456789abcdefABCDEF').lower()
        if len(value) >= 16:
            values.append(value)
    return values


def walk_report_sent(tool, keys):
    """With a report service there, a stand-in on this PC, the report page sends the report with what was typed, and says what came of it."""
    received = []

    class Service(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            body = self.rfile.read(int(self.headers.get('content-length', 0)))
            received.append((self.path, json.loads(body)))
            data = b'{"status":"received","id":7}'
            self.send_response(200)
            self.send_header('content-type', 'application/json')
            self.send_header('content-length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *args):
            pass

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Service)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    reports = tempfile.mkdtemp(prefix='hbplz_reports_')
    process, window = open_gui(tool, keys, [], HACBREWPACKLZ_REPORT_FOLDER=reports, HACBREWPACKLZ_REPORT_SITE=f'http://127.0.0.1:{server.server_address[1]}')
    try:
        report('the main window opens with a report service', bool(window))
        if not window:
            return
        time.sleep(0.8)
        layout = Layout(window)
        click(window, *layout.more())
        popup = find(POPUP_CLASS, '', 3)
        if popup:
            click(popup, 60, 4 + 16)
        if not wait_for_name(layout, 'report'):
            report('help opens with a report service', False)
            return
        click(window, *layout.middle('report'))
        report('with a report service, the page sends it, or saves it instead', wait_for_name(layout, 'send') and layout.text('send') == 'Send'
               and layout.text('save') == 'Save instead', f"{layout.text('send')} / {layout.text('save')}")
        SendText(edit_at(window, *layout.middle('said')), WM_SETTEXT, 0, 'It froze while compressing')
        click(window, *layout.middle('send'))
        report('send sends it, then the button is Done', wait_until(lambda: layout.text('send') == 'Done', 40) and len(received) == 1, f"{layout.text('send')}, {len(received)} received")
        shot(window, 'report_sent')
        where, sent = received[-1] if received else ('', {})
        report('to the service, with what was typed, and what it is', where == '/report' and sent.get('said') == 'It froze while compressing'
               and sent.get('kind') == 'manual' and sent.get('platform') == 'Windows' and '== ' in sent.get('report', ''),
               json.dumps({k: v for k, v in sent.items() if k != 'report'}))
        values = key_values(keys)
        report('and no key in it', bool(sent) and not any(v in json.dumps(sent).lower() for v in values), 'a key value was in what was sent')
        report('nothing is saved, as it went', not os.listdir(reports), str(os.listdir(reports)))
        click(window, *layout.middle('send'))
        report('done goes back to the list after sending', wait_for_name(layout, 'add_files') and layout.text('send') is None)
        report('closes after sending a report', finish(process, window))
    finally:
        if process.poll() is None:
            stop_process(process)
        server.shutdown()
        shutil.rmtree(reports, ignore_errors=True)


def walk_search(tool, keys, location):
    process, window = open_gui(tool, keys, [])
    if not window:
        return
    time.sleep(0.8)
    layout = Layout(window)
    click(window, *layout.search())
    report('search opens, in the main window', wait_for_name(layout, 'add_chosen'))
    if layout.text('add_chosen') is None:
        finish(process, window)
        return
    # It starts on the Downloads folder by itself, so whatever that's still doing is stopped first, or the click below would only stop it.
    if layout.text('search_go') == 'Stop':
        click(window, *layout.middle('search_go'))
        time.sleep(0.5)
    SendText(edit_at(window, *layout.middle('location')), WM_SETTEXT, 0, location)
    click(window, *layout.middle('search_go'))
    time.sleep(2)
    shot(window, 'search_results')

    SendText(edit_at(window, *layout.middle('filter')), WM_SETTEXT, 0, 'knight')
    time.sleep(0.5)
    shot(window, 'search_filtered')

    # Add what's chosen, which goes back to the list with it.
    click(window, *layout.middle('add_chosen'))
    # However long adding takes on a busy PC: a fixed wait was sometimes too short.
    report('search adds what was chosen', wait_until(lambda: layout.text('add_chosen') is None and layout.text('add') is not None, 15))
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
    # Before Analyze, a game's figure says so in words, as a figure is never left blank.
    report('a game not analyzed yet says Not yet', (layout.text('result_0') or '').endswith('|Not yet'), str(layout.text('result_0')))
    width, height = layout.size

    hover(window, width // 2, 62 + 34 + 28)
    shot(window, 'titles_hover')

    click(window, *layout.analyze())
    time.sleep(2.5)
    shot(window, 'analyzing')
    time.sleep(60)
    shot(window, 'analyzed')

    click(window, *layout.more())
    popup = find(POPUP_CLASS, '', 3)
    report('More opens, with the app\'s own things', bool(popup))
    if popup:
        shot(popup, 'more_with_games')
        user32.PostMessageW(popup, WM_KEYDOWN, VK_ESCAPE, 0)
        time.sleep(0.2)

    # A right click on a game has what can be done to it, and the arrow on Compress what can be done to all the ticked ones.
    left, top, w, h = layout.where('info_0')
    user32.PostMessageW(window, WM_RBUTTONUP, 0, lparam(width // 3, top + h // 2))
    popup = find(POPUP_CLASS, '', 3)
    report('a right click on a game opens its menu', bool(popup))
    if popup:
        shot(popup, 'game_menu')
        user32.PostMessageW(popup, WM_KEYDOWN, VK_ESCAPE, 0)
        time.sleep(0.2)
    cl, ct, cw, ch = layout.where('compress')
    click(window, cl + cw - layout.s(14), ct + ch // 2)
    popup = find(POPUP_CLASS, '', 3)
    report('the arrow on Compress opens what can be done to the ticked games', bool(popup))
    if popup:
        shot(popup, 'ticked_menu')
        user32.PostMessageW(popup, WM_KEYDOWN, VK_ESCAPE, 0)
        time.sleep(0.2)

    # Details: everything about the game, in the same places as on the Switch, with what can be done to it.
    click(window, *layout.middle('info_0'))
    report('the info button opens the game\'s Details', wait_for_name(layout, 'compress_this') and wait_for_name(layout, 'look_inside'))
    report('which say what compressing it would make of it', wait_until(lambda: 'would take' in (layout.text('details_figure') or ''), 5), str(layout.text('details_figure')))
    report("and what's in it, once it's open", wait_until(lambda: 'Game ID' in (layout.text("details_What's_in_it") or ''), 15), str(layout.text("details_What's_in_it")))
    shot(window, 'details_page')
    # Look inside: its parts as folders, with what's under the pointer beside them.
    click(window, *layout.middle('look_inside'))
    report('Look inside opens on its parts', wait_until(lambda: (layout.text('inside') or '').startswith('top | '), 10), str(layout.text('inside')))
    report('with the first one shown beside the list', wait_until(lambda: 'Folder' in (layout.text('inside') or ''), 10), str(layout.text('inside')))
    shot(window, 'inside_top')
    user32.PostMessageW(window, WM_KEYDOWN, VK_RETURN, 0)
    report('Enter opens a folder', wait_until(lambda: not (layout.text('inside') or 'top').startswith('top'), 5), str(layout.text('inside')))
    user32.PostMessageW(window, WM_KEYDOWN, VK_END, 0)
    report('and a file in it is shown', wait_until(lambda: 'reading' not in (layout.text('inside') or 'reading') and 'Folder' not in (layout.text('inside') or 'Folder'), 10),
           str(layout.text('inside')))
    shot(window, 'inside_file')
    click(window, *layout.middle('save'))
    report('Save saves it to Files from games, and says where', wait_until(lambda: 'Saved to' in (layout.text('inside_status') or ''), 15)
           and os.path.isdir(os.path.join(os.path.dirname(nsp), 'Files from games')), str(layout.text('inside_status')))
    shot(window, 'inside_saved')
    click(window, *layout.middle('back'))
    report('back goes to its Details', wait_for_name(layout, 'look_inside'))
    click(window, *layout.middle('back'))
    report('and back again to the list', wait_for_name(layout, 'compress'))

    user32.SetWindowPos(window, None, 0, 0, 676, 499, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)
    time.sleep(0.3)
    shot(window, 'small')
    # In the smallest window, the views along the top keep clear of the Now heading, which gives way where they'd meet.
    views_right = sum(layout.where('view_everything')[0:3:2])
    report("the views don't run into the Now heading", layout.text('heading_now') is None or layout.where('heading_now')[0] >= views_right + layout.s(8),
           f"views end at {views_right}, Now at {layout.where('heading_now')[0] if layout.text('heading_now') is not None else '-'}")
    # And what's said along the bottom gets as many lines as it needs, rather than being cut short.
    said = layout.text('status_first')
    if said is None:
        report('what the bar says is all there in a small window', False, 'no status_first')
    else:
        _, _, needs, given = layout.where('status_first')
        report('what the bar says is all there in a small window', needs <= layout.where('status')[2] or given > layout.s(22),
               f'{said!r} needs {needs}, has {layout.where("status")[2]} by {given}')
        report('and keeps each figure with its unit where it wraps', not re.search(r'\d ', said), repr(said))
    user32.SetWindowPos(window, None, 0, 0, 1200, 760, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)
    time.sleep(0.3)
    shot(window, 'large')
    report('closes with a list', finish(process, window))

    # With compression off, Done is still there, and holds what a repack gains nothing from, as on the Switch: a game with no update, such as
    # this one, found in a folder rather than picked on its own.
    write_settings('--nocompress=1')
    alone = tempfile.mkdtemp(prefix='hbplz_alone_')
    shutil.copy(nsp, alone)
    process, window = open_gui(tool, keys, [alone])
    if window:
        layout = Layout(window)
        report('with compression off, the views are To do, Done and Everything', wait_until(lambda: layout.heading().startswith('*To do'), 8)
               and layout.text('view_done') is not None, layout.heading())
        report('and a game with no update is under Done, as a repack gains nothing', wait_until(lambda: 'Done 0' not in layout.heading(), 8), layout.heading())
        shot(window, 'no_compression')
        report('closes with compression off', finish(process, window))
    write_settings()
    shutil.rmtree(alone, ignore_errors=True)


def wait_until(test, seconds=10):
    deadline = time.time() + seconds
    while time.time() < deadline:
        if test():
            return True
        time.sleep(0.2)
    return False


def walk_compress(tool, keys, nsp):
    process, window = open_gui(tool, keys, [nsp])
    if not window:
        report('the main window opens for compressing', False)
        return
    time.sleep(3)
    layout = Layout(window)
    report('the list starts on To do, with the game in it', layout.heading().startswith('*To do 1') and 'Done 0' in layout.heading(), layout.heading())
    click(window, *layout.compress())
    report('compressing shows the job on its own', wait_until(lambda: layout.heading().startswith('Compressing 1 game'), 10) or layout.heading().startswith('Compressed'),
           layout.heading())

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
    # What came of it, with the way back to the list, and sending what it made as the main thing to do next.
    report('then it says what came of it', wait_until(lambda: layout.heading().startswith('Compressed 1 game'), 10), layout.heading())
    report('with a way back to the list', layout.text('back_to_list') == 'Back to the list', str(layout.text('back_to_list')))
    report('and sending what it made as the next thing', layout.text('compress') == 'Send to Switch', str(layout.text('compress')))
    shot(window, 'compressed')

    # The log is a text box of its own lying over the list, so it has to really go when Log is clicked again.
    log = user32.FindWindowExW(window, None, 'EDIT', None)
    report('the log is Log, as Details is a game\'s own page', layout.text('details') == 'Log', str(layout.text('details')))
    click(window, *layout.details())
    time.sleep(0.3)
    shot(window, 'log')
    report('Log opens the log', bool(log) and bool(user32.IsWindowVisible(log)))
    click(window, *layout.details())
    time.sleep(0.3)
    shot(window, 'log_closed')
    report('clicking Log again closes it', bool(log) and not user32.IsWindowVisible(log))
    click(window, *layout.details())
    time.sleep(0.3)
    report('and clicking again opens it', bool(log) and bool(user32.IsWindowVisible(log)))

    click(window, *layout.details())
    time.sleep(0.3)

    # Back on the list, the game is done: To do has nothing left, and Done has it, ready to send.
    click(window, *layout.back_to_list())
    report('back on the list, the compressed game is under Done', wait_until(lambda: layout.heading().startswith('*To do 0') and 'Done 1' in layout.heading(), 5),
           layout.heading())
    report('with nothing left to compress, and no way back left over', layout.text('compress') is None and layout.text('back_to_list') is None,
           f"{layout.text('compress')} / {layout.text('back_to_list')}")
    shot(window, 'back_to_list')
    click(window, *layout.middle('view_done'))
    report('Done shows it, with sending it to the Switch next', wait_until(lambda: layout.heading().startswith('To do 0 | *Done 1'), 5) and layout.text('compress') == 'Send to Switch',
           f"{layout.heading()} / {layout.text('compress')}")
    report('on Done, it says what compressing it freed', '|' in (layout.text('result_0') or '') and (layout.text('result_0') or '|').split('|')[1].endswith('B'),
           str(layout.text('result_0')))

    # Checking it for damage from its Details reads all of it back, and then it's as it was: under Done.
    click(window, *layout.middle('info_0'))
    if wait_for_name(layout, 'check_this'):
        time.sleep(1)
        click(window, *layout.middle('check_this'))
        report('checking a game shows what it found', wait_until(lambda: layout.heading().startswith('Checked 1 game'), 60), layout.heading())
        report('it read back whole', wait_until(lambda: (layout.text('result_0') or '').startswith('Whole'), 5), str(layout.text('result_0')))
        shot(window, 'checked')
        click(window, *layout.back_to_list())
        report('back on the list, it is still under Done', wait_until(lambda: 'Done 1' in layout.heading(), 5), layout.heading())
        click(window, *layout.middle('view_done'))
        wait_until(lambda: layout.heading().startswith('To do 0 | *Done 1'), 5)

    # The big button sends it to the Switch, and Stop in its place ends that.
    shot(window, 'send_next')
    click(window, *layout.compress())
    time.sleep(1.5)
    shot(window, 'sending')
    click(window, *layout.stop())
    time.sleep(1.5)
    shot(window, 'sending_stopped')
    report('Done says what it was, what it takes now and what was freed', layout.heading().startswith('To do 0 | *Done 1') and
           (layout.text('heading_now') is None or layout.text('heading_now') == 'Now'), layout.heading())
    # Unticked on Done, with nothing left to compress: no greyed-out Compress, and sending the list is the main thing to do.
    left, top, w, h = layout.where('info_0')
    click(window, layout.s(40), top + h // 2)
    report('on Done with nothing ticked, Compress is gone and Send to Switch is there',
           wait_until(lambda: layout.text('compress') is None and layout.text('send') == 'Send to Switch', 3),
           f"{layout.text('compress')} / {layout.text('send')}")
    shot(window, 'done_unticked')
    report('closes after a job', finish(process, window))


def walk_failed(tool, keys, source, work):
    """A game that couldn't be done says what went wrong in a few plain words under its name, with the error itself behind its info button."""
    folder = os.path.join(work, 'failing')
    os.makedirs(folder, exist_ok=True)

    def fails(nsp, sentence, what):
        process, window = open_gui(tool, keys, [nsp])
        if not window:
            report(f'the main window opens for {what}', False)
            return
        time.sleep(3)
        layout = Layout(window)
        # Reading the game for a first look can find it damaged already, which leaves nothing ticked to compress.
        if layout.text('said_0') != sentence:
            try:
                click(window, *layout.compress())
            except AssertionError:
                pass
            wait_until(lambda: layout.text('said_0') == sentence and (layout.text('back_to_list') == 'Back to the list' or layout.text('compress') is None), 90)
        report(f'{what} says so in plain words under its name', layout.text('said_0') == sentence, str(layout.text('said_0')))
        report('with the error itself behind its info button', layout.text('info_0') is not None)
        shot(window, 'failed_' + what.split()[-1])
        finish(process, window)

    # Bytes changed through the middle of it, where its game data is, which reading it in full finds.
    damaged = os.path.join(folder, 'A Damaged Game.nsp')
    shutil.copyfile(source, damaged)
    size = os.path.getsize(damaged)
    with open(damaged, 'r+b') as f:
        for tenth in range(4, 9):
            f.seek(size * tenth // 10)
            byte = f.read(1)
            f.seek(size * tenth // 10)
            f.write(bytes([byte[0] ^ 0xFF]))
    write_settings()
    fails(damaged, "Damaged: dump or download it again.", 'a game whose file is damaged')

    # No folder can be made inside a file, which has nothing plainer to say than where to read it.
    nsp = os.path.join(folder, 'A Failing Game.nsp')
    shutil.copyfile(source, nsp)
    write_settings('--nspdir=' + os.path.join(settings, 'a folder for NSPs that can never be made inside a file', 'out'))
    fails(nsp, 'Something went wrong. The info button says what.', 'a failure with nothing plainer to say')
    write_settings()
    shutil.rmtree(folder, ignore_errors=True)


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

    # It saves far less than this. Quick, a first look can't say so, so it isn't ticked, and compressing makes nothing; ticked by hand, it's
    # compressed whatever it saves.
    write_settings('--minsaving=50%', '--measure=0')
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
    report('quick, a title a first look puts under the minimum isn\'t ticked, so compressing makes nothing', not made(), str(made()))

    click(window, *FIRST_TICK)
    time.sleep(0.3)
    click(window, *layout.compress())
    deadline = time.time() + 120
    while not made() and time.time() < deadline:
        time.sleep(0.3)
    time.sleep(1)
    report('ticking it by hand compresses it anyway', len(made()) == 1, str(os.listdir(folder)))
    shot(window, 'ticked_by_hand')
    # Esc goes back too, as on the home screen.
    if wait_until(lambda: layout.text('back_to_list') is not None, 10):
        user32.PostMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0)
    report('Esc goes back to the list from what came of it', wait_until(lambda: layout.heading().startswith('*To do'), 5), layout.heading())
    report('closes after ticking', finish(process, window))
    for old in made():
        os.remove(os.path.join(folder, old))

    # Thorough, as it is to begin with, Analyze goes on to read every title in full, writing nothing, leaving nothing more to analyze, and the
    # title is left out with what it was measured to save.
    write_settings('--minsaving=50%')
    process, window = open_gui(tool, keys, [nsp])
    if not window:
        report('the main window opens with a thorough analysis', False)
        return
    time.sleep(3)
    layout = Layout(window)
    click(window, *layout.analyze())
    time.sleep(1)
    # Compress is there while it measures, as measuring goes on behind the list, so it's Stop going that says it's done.
    idle = wait_until(lambda: layout.text('stop') is None, 120)
    time.sleep(1)
    shot(window, 'thorough_analyzed')
    report('thorough, Analyze goes on to read it in full, leaving nothing more to analyze', idle and layout.text('analyze') is None,
           f"idle {idle}, analyze {layout.text('analyze')}")
    report('and nothing is written', not made(), str(made()))
    report('closes after a thorough analysis', finish(process, window))
    write_settings()


def walk_measuring(tool, keys, large, work):
    """Measuring goes on behind the list, so a game's Details still open while it does, as on the home screen and Android."""
    # An empty folder for its copies, so none made before has it on Done.
    folder = os.path.join(work, 'measuring')
    os.makedirs(folder, exist_ok=True)
    write_settings(f'--nspdir={folder}')
    # Opening a game takes 8 s here, as when measuring shares the disk, so Back comes while it's still being opened.
    process, window = open_gui(tool, keys, [large], HACBREWPACKLZ_TEST_SLOW_OPEN='8000')
    if not window:
        report('the main window opens for measuring', False)
        return
    time.sleep(5)
    layout = Layout(window)
    # Its first look may have been and gone by now, leaving Analyze to start measuring.
    if layout.text('analyze') is not None:
        click(window, *layout.analyze())
    report('analyzing a big game goes on to measuring it', wait_until(lambda: layout.text('measuring') is not None, 30))
    click(window, *layout.middle('view_everything'))
    report('a view can be chosen while it does', wait_until(lambda: '*Everything' in layout.heading(), 5), layout.heading())
    click(window, *layout.middle('view_todo'))
    wait_until(lambda: '*To do' in layout.heading(), 5)
    click(window, *layout.middle('info_0'))
    report("the info button opens the game's Details while it's measured", wait_for_name(layout, 'look_inside'))
    shot(window, 'details_while_measuring')
    if layout.text('look_inside') is not None:
        # Back doesn't wait for the opening to finish.
        opening = 'being opened' in (layout.text('details_What_it_needs') or '')
        click(window, *layout.middle('back'))
        report('and back goes to the list at once, while the game is still being opened', opening and wait_until(lambda: layout.text('compress') is not None, 2),
               'it had already opened' if not opening else 'still on Details after 2 s')
    if layout.text('stop') is not None:
        click(window, *layout.stop())
        wait_until(lambda: layout.text('stop') is None, 15)
    finish(process, window)


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
    report('during a job, Add, Options and the menu are hidden, not greyed out',
           layout.text('add') is None and layout.text('options') is None and layout.text('more') is None,
           f"{layout.text('add')} / {layout.text('options')} / {layout.text('more')}")
    said = layout.text('status') or ''
    report('what a job says below its name never parts a figure from its unit', bool(said) and not re.search(r'\d ', said), said)
    left = os.listdir(folder)
    report('a job in progress is writing', any(f.endswith('.part') for f in left), str(left))

    click(window, *layout.stop())
    time.sleep(4)
    shot(window, 'stopped')
    left = os.listdir(folder)
    report('Stop leaves nothing behind', not left, str(left))
    report('it says it stopped, with going back to the list as the main thing to do', layout.text('back_to_list') == 'Back to the list' and layout.text('compress') is None,
           f"{layout.heading()} / {layout.text('back_to_list')} / {layout.text('compress')}")
    click(window, *layout.back_to_list())
    report('back on the list, the game is still to do', wait_until(lambda: layout.heading().startswith('*To do 1'), 5), layout.heading())

    # Now the same again, by closing the window and answering yes.
    click(window, *layout.compress())
    time.sleep(8)
    user32.PostMessageW(window, WM_CLOSE, 0, 0)
    question = find('#32770', 'hacBrewPackLZ', 5)
    report('closing during a job asks first', bool(question))
    if question:
        shot(question, 'close_question')
        said = dialog_text(question)
        report('in the words the home screen uses', 'The games already done are kept.' in said, said)
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
    click(window, *Layout(window).add_files())
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


def walk_pick_compressed(tool, keys, work):
    """
    Someone picks only a game that's already compressed with Add files. It has to show up, ticked, with Send to Switch
    to do next, rather than being hidden as a compressed copy beside its original would be, which looks like nothing
    was picked at all.
    """
    made = [f for f in os.listdir(work) if '[LZ4]' in f and f.endswith('.nsp')]
    if not made:
        report('a compressed game to pick', False, 'compressing made none')
        return
    alone = os.path.join(work, 'picked')
    os.makedirs(alone, exist_ok=True)
    compressed = os.path.join(alone, made[0])
    shutil.copyfile(os.path.join(work, made[0]), compressed)

    process, window = open_gui(tool, keys, [])
    if not window:
        return
    time.sleep(0.8)
    click(window, *Layout(window).add_files())
    dialog = find('#32770', 'Add game files', 5)
    report('the picker opens for a compressed game', bool(dialog))
    if dialog:
        edit = dialog
        for cls in ('ComboBoxEx32', 'ComboBox', 'Edit'):
            edit = user32.FindWindowExW(edit, None, cls, None) if edit else None
        if edit and type_name(edit, compressed):
            user32.PostMessageW(dialog, WM_COMMAND, IDOK, 0)
        deadline = time.time() + 15
        while user32.IsWindow(dialog) and time.time() < deadline:
            time.sleep(0.05)
        if user32.IsWindow(dialog):
            user32.PostMessageW(dialog, WM_COMMAND, IDCANCEL, 0)
        layout = Layout(window)
        deadline = time.time() + 15
        while layout.text('compress') is None and time.time() < deadline:
            time.sleep(0.2)
        time.sleep(0.5)
        shot(window, 'picked_compressed')
        report('a compressed game picked on its own is listed, ready to send', layout.text('compress') == 'Send to Switch', str(layout.text('compress')))
    report('closes after picking a compressed game', finish(process, window))
    shutil.rmtree(alone, ignore_errors=True)


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
        if wait_for_name(layout, 'done', 3):
            shot(window, f'{name}_options')
            click(window, *layout.middle('done'))
        finish(process, window)


def main():
    global shots, settings, layout_file
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
    layout_file = os.path.join(work, 'layout.txt')
    write_settings()
    nsp = os.path.join(work, 'A Test Game [0123456789ABCDEF].nsp')
    shutil.copyfile(source, nsp)
    try:
        walk_welcome(tool, keys)
        walk_problems_question(tool, keys)
        walk_empty(tool, keys)
        walk_report_sent(tool, keys)
        walk_search(tool, keys, os.path.dirname(os.path.dirname(large)))
        walk_dialog(tool, keys, nsp)
        walk_no_keys(tool, nsp, work)
        walk_titles(tool, keys, nsp, extra)
        walk_compress(tool, keys, nsp)
        walk_failed(tool, keys, source, work)
        walk_pick_compressed(tool, keys, work)
        walk_minimum(tool, keys, nsp)
        walk_themes(tool, keys, nsp)
        walk_measuring(tool, keys, large, work)
        walk_stop(tool, keys, large, work)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print(f'{failures} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
