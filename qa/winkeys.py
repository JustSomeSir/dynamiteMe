"""Presses a key in a game window (Windows only). Used for the title steps that wait for "any key"
and have no Lua handler we can call (login notice, press-start screen).

Safety: the key is sent only when the game's own window is the foreground window. If Windows refuses
to bring it to the front, nothing is sent, so a key can never land in another program.
"""

import ctypes
import sys
import time

ENTER = 0x1C  # keyboard scan code (set 1)


def available():
    return sys.platform == "win32"


def _window_of(pid):
    user32 = ctypes.windll.user32
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def cb(hwnd, _):
        wpid = ctypes.c_ulong(0)
        user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(wpid))
        if wpid.value == pid and user32.IsWindowVisible(ctypes.c_void_p(hwnd)) and \
                user32.GetWindowTextLengthW(ctypes.c_void_p(hwnd)) > 0:
            found.append(hwnd)
        return True

    user32.EnumWindows(cb, None)
    return found[0] if found else None


def _to_front(hwnd):
    user32, kernel32 = ctypes.windll.user32, ctypes.windll.kernel32
    user32.GetForegroundWindow.restype = ctypes.c_void_p
    if user32.GetForegroundWindow() == hwnd:
        return True
    user32.SetForegroundWindow(ctypes.c_void_p(hwnd))
    time.sleep(0.2)
    if user32.GetForegroundWindow() == hwnd:
        return True
    # Windows lets only the foreground thread hand focus over: attach to it for the call
    fg = user32.GetForegroundWindow()
    fg_thread = user32.GetWindowThreadProcessId(ctypes.c_void_p(fg), None) if fg else 0
    me = kernel32.GetCurrentThreadId()
    if fg_thread and fg_thread != me:
        user32.AttachThreadInput(me, fg_thread, True)
        user32.BringWindowToTop(ctypes.c_void_p(hwnd))
        user32.SetForegroundWindow(ctypes.c_void_p(hwnd))
        user32.AttachThreadInput(me, fg_thread, False)
        time.sleep(0.2)
    return user32.GetForegroundWindow() == hwnd


class _KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", ctypes.c_ushort), ("wScan", ctypes.c_ushort), ("dwFlags", ctypes.c_ulong),
                ("time", ctypes.c_ulong), ("dwExtraInfo", ctypes.c_void_p)]


class _INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("ki", _KEYBDINPUT), ("pad", ctypes.c_byte * 32)]
    _anonymous_ = ("u",)
    _fields_ = [("type", ctypes.c_ulong), ("u", _U)]


def _send(scan, up):
    inp = _INPUT()
    inp.type = 1  # INPUT_KEYBOARD
    inp.ki = _KEYBDINPUT(0, scan, 0x0008 | (0x0002 if up else 0), 0, None)  # SCANCODE [| KEYUP]
    return ctypes.windll.user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(_INPUT)) == 1


def press(pid, scan=ENTER, hold_s=0.08):
    """Returns (sent, reason)."""
    if not available():
        return False, "not Windows"
    hwnd = _window_of(pid)
    if not hwnd:
        return False, "no visible window for pid %d" % pid
    if not _to_front(hwnd):
        return False, "Windows did not bring the game window to the front"
    if ctypes.windll.user32.GetForegroundWindow() != hwnd:  # re-check right before typing
        return False, "game window lost focus"
    if not _send(scan, False):
        return False, "SendInput failed"
    time.sleep(hold_s)
    _send(scan, True)
    return True, "ok"
