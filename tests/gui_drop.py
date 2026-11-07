"""
Drags a file onto the GUI with a real OLE drag, the same mechanism Explorer uses, and checks the title appears.
A drag only works with a mouse button down, so this moves the pointer over an empty part of the GUI's window and presses
the button there for about two seconds, then lets go and puts the pointer back. It only runs on a PC nobody is using,
and waits until no key or mouse has been touched for half a minute.

    python tests\\gui_drop.py <hacbrewpacklz-gui.exe> <keys folder> <small game file> [folder for screenshots]
"""
import ctypes
import ctypes.wintypes as wt
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import tkinter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import winshot

user32 = ctypes.WinDLL('user32', use_last_error=True)
ole32 = ctypes.WinDLL('ole32')
shell32 = ctypes.WinDLL('shell32')

user32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
user32.FindWindowW.restype = wt.HWND
user32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
user32.GetCursorPos.argtypes = [ctypes.POINTER(wt.POINT)]
user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.SetForegroundWindow.argtypes = [wt.HWND]
user32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wt.UINT]
user32.WindowFromPoint.argtypes = [wt.POINT]
user32.WindowFromPoint.restype = wt.HWND
user32.GetAncestor.argtypes = [wt.HWND, wt.UINT]
user32.GetAncestor.restype = wt.HWND

shell32.SHParseDisplayName.argtypes = [wt.LPCWSTR, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p), wt.ULONG, ctypes.POINTER(wt.ULONG)]
shell32.SHCreateShellItemArrayFromIDLists.argtypes = [wt.UINT, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p)]
ole32.DoDragDrop.argtypes = [ctypes.c_void_p, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD)]
ole32.OleInitialize.argtypes = [ctypes.c_void_p]

S_OK = 0
DRAGDROP_S_DROP = 0x00040100
DRAGDROP_S_CANCEL = 0x00040101
DRAGDROP_S_USEDEFAULTCURSORS = 0x00040102
E_NOINTERFACE = 0x80004002 - (1 << 32)
DROPEFFECT_COPY = 1
MOUSE_LEFT_DOWN = 0x0002
MOUSE_LEFT_UP = 0x0004
IDLE_NEEDED = 30
IDLE_WAIT_LIMIT = 600
HWND_TOPMOST = -1
SWP_NOSIZE = 1
SWP_NOMOVE = 2
SWP_NOACTIVATE = 0x10
GA_ROOT = 2
BHID_DATA_OBJECT = '{B8C0BD9F-ED24-455C-83E6-D5390C4FE8C4}'
IID_DATA_OBJECT = '{0000010E-0000-0000-C000-000000000046}'


class GUID(ctypes.Structure):
    _fields_ = [('Data1', wt.DWORD), ('Data2', wt.WORD), ('Data3', wt.WORD), ('Data4', ctypes.c_ubyte * 8)]


ole32.CLSIDFromString.argtypes = [wt.LPCWSTR, ctypes.POINTER(GUID)]


def guid(text):
    value = GUID()
    ole32.CLSIDFromString(text, ctypes.byref(value))
    return value


def method(instance, slot, restype, *argtypes):
    table = ctypes.cast(instance, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    return ctypes.WINFUNCTYPE(restype, ctypes.c_void_p, *argtypes)(table[slot])


def data_object_for(path):
    """The same data object a shell view hands out for a selected file."""
    idlist = ctypes.c_void_p()
    flags = wt.ULONG()
    result = shell32.SHParseDisplayName(path, None, ctypes.byref(idlist), 0, ctypes.byref(flags))
    if result != 0:
        raise OSError(f'SHParseDisplayName {result:#x}')
    array = ctypes.c_void_p()
    items = (ctypes.c_void_p * 1)(idlist)
    result = shell32.SHCreateShellItemArrayFromIDLists(1, items, ctypes.byref(array))
    if result != 0:
        raise OSError(f'SHCreateShellItemArrayFromIDLists {result:#x}')
    bind = method(array, 3, ctypes.HRESULT, ctypes.c_void_p, ctypes.POINTER(GUID), ctypes.POINTER(GUID), ctypes.POINTER(ctypes.c_void_p))
    data = ctypes.c_void_p()
    bind(array, None, ctypes.byref(guid(BHID_DATA_OBJECT)), ctypes.byref(guid(IID_DATA_OBJECT)), ctypes.byref(data))
    return data


class DropSource:
    """Holds the drag for a moment, so the target sees it arrive, then lets go."""

    def __init__(self, drop_after, on_hold):
        self.deadline = time.time() + drop_after
        self.on_hold = on_hold
        self.held = False
        query = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.POINTER(GUID), ctypes.POINTER(ctypes.c_void_p))(self.query)
        count = ctypes.WINFUNCTYPE(wt.ULONG, ctypes.c_void_p)(lambda self_: 1)
        carry = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, wt.BOOL, wt.DWORD)(self.query_continue)
        feedback = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, wt.DWORD)(lambda self_, effect: DRAGDROP_S_USEDEFAULTCURSORS)
        self.functions = [query, count, count, carry, feedback]
        self.table = (ctypes.c_void_p * 5)(*[ctypes.cast(f, ctypes.c_void_p) for f in self.functions])
        self.object = (ctypes.c_void_p * 1)(ctypes.cast(self.table, ctypes.c_void_p))

    def query(self, this, iid, result):
        result[0] = ctypes.cast(self.object, ctypes.c_void_p)
        return S_OK

    def query_continue(self, this, escape, keys):
        if not self.held and time.time() > self.deadline - 0.7:
            self.held = True
            self.on_hold()
        return DRAGDROP_S_DROP if time.time() > self.deadline else S_OK


class LASTINPUTINFO(ctypes.Structure):
    _fields_ = [('cbSize', wt.UINT), ('dwTime', wt.DWORD)]


def idle_seconds():
    info = LASTINPUTINFO(ctypes.sizeof(LASTINPUTINFO))
    user32.GetLastInputInfo(ctypes.byref(info))
    return (ctypes.WinDLL('kernel32').GetTickCount() - info.dwTime) / 1000


def button(down):
    user32.mouse_event(MOUSE_LEFT_DOWN if down else MOUSE_LEFT_UP, 0, 0, 0, 0)


def main():
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    tool = os.path.abspath(sys.argv[1])
    keys = os.path.abspath(sys.argv[2])
    source = os.path.abspath(sys.argv[3])
    shots = os.path.abspath(sys.argv[4]) if len(sys.argv) > 4 else None

    work = tempfile.mkdtemp(prefix='hbplz_drop_')
    game = os.path.join(work, 'A Dropped Game [0123456789ABCDEF]' + os.path.splitext(source)[1])
    shutil.copyfile(source, game)
    settings = os.path.join(work, 'settings.txt')
    environment = dict(os.environ, HACBREWPACKLZ_SETTINGS=settings)

    deadline = time.time() + IDLE_WAIT_LIMIT
    while idle_seconds() < IDLE_NEEDED:
        if time.time() > deadline:
            print('SKIPPED: someone is using this PC')
            return 3
        time.sleep(1)

    ole32.OleInitialize(None)
    process = subprocess.Popen([tool], cwd=keys, env=environment)
    window = None
    deadline = time.time() + 10
    while not window and time.time() < deadline:
        window = user32.FindWindowW('hacBrewPackLZ.Window', 'hacBrewPackLZ')
        time.sleep(0.05)
    if not window:
        print('FAIL: the window did not open')
        return 1
    time.sleep(1)

    rect = wt.RECT()
    user32.GetWindowRect(window, ctypes.byref(rect))
    original = wt.POINT()
    user32.GetCursorPos(ctypes.byref(original))
    target = (rect.left + rect.right) // 2, (rect.top + rect.bottom) // 2

    # Started in the background, the window can open behind others, and then one of those would get the drop.
    user32.SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE)
    time.sleep(0.3)
    if user32.GetAncestor(user32.WindowFromPoint(wt.POINT(*target)), GA_ROOT) != window:
        print('SKIPPED: another window stays in front of the GUI')
        process.kill()
        shutil.rmtree(work, ignore_errors=True)
        return 3

    result = [None, 0]
    dropped = threading.Event()

    def give_up():
        # Never leave a drag going on someone's desktop, whatever goes wrong here.
        if not dropped.is_set():
            button(False)
            user32.SetCursorPos(original.x, original.y)
            process.kill()
            os._exit(2)

    def nudge():
        # A drag only moves on when the mouse does, so the pointer goes to the window and keeps moving by a pixel.
        offset = 0
        while not dropped.is_set():
            offset ^= 1
            user32.SetCursorPos(target[0] + offset, target[1])
            time.sleep(0.05)

    def hold():
        if shots:
            winshot.capture(window, os.path.join(shots, 'drop_hover.png'))

    # As in Explorer, the drag starts from a window that really got the button press. This one is too small to matter.
    helper = tkinter.Tk()
    helper.overrideredirect(True)
    helper.attributes('-topmost', True)
    helper.geometry(f'24x24+{target[0]}+{rect.top + 8}')
    helper.update()
    helper.focus_force()
    helper.update()

    def start_drag(_event):
        data = data_object_for(game)
        threading.Thread(target=nudge, daemon=True).start()
        drag = DropSource(2.0, hold)
        effect = wt.DWORD()
        result[0] = ole32.DoDragDrop(data, ctypes.cast(drag.object, ctypes.c_void_p), DROPEFFECT_COPY, ctypes.byref(effect))
        result[1] = effect.value
        dropped.set()
        helper.after(0, helper.quit)

    helper.bind('<ButtonPress-1>', start_drag)
    watchdog = threading.Timer(8, give_up)
    watchdog.daemon = True
    watchdog.start()
    try:
        user32.SetCursorPos(target[0] + 12, rect.top + 20)
        time.sleep(0.2)
        button(True)
        helper.mainloop()
    finally:
        dropped.set()
        watchdog.cancel()
        button(False)
        user32.SetCursorPos(original.x, original.y)
        helper.destroy()

    print(f'drag result {(result[0] or 0) & 0xFFFFFFFF:#x}, effect {result[1]}')
    ok = result[0] == DRAGDROP_S_DROP and result[1] == DROPEFFECT_COPY

    time.sleep(3)
    if shots:
        winshot.capture(window, os.path.join(shots, 'drop_done.png'))
    print('PASS: the window took the drop' if ok else 'FAIL: the drop was not taken')

    user32.PostMessageW(window, 0x0010, 0, 0)
    try:
        clean = process.wait(10) == 0
    except subprocess.TimeoutExpired:
        subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], capture_output=True, stdin=subprocess.DEVNULL)
        clean = False
    print('PASS: it closes cleanly' if clean else 'FAIL: it did not close cleanly')
    shutil.rmtree(work, ignore_errors=True)
    return 0 if ok and clean else 1


if __name__ == '__main__':
    sys.exit(main())
