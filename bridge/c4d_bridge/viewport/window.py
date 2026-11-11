# SPDX-License-Identifier: GPL-3.0-or-later
"""Control of Blender's own window from outside its API (Windows only for now).

Blender offers no Python call to hide or resize its window. A hidden window keeps rendering; a
minimized one does not, so the window is never minimized.
"""

import ctypes
import os
import sys

_SW_HIDE, _SW_SHOWNOACTIVATE = 0, 4
_SWP_NOMOVE, _SWP_NOZORDER, _SWP_NOACTIVATE, _SWP_NOSENDCHANGING = 0x0002, 0x0004, 0x0010, 0x0400


class _Rect(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long), ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


def _user32():
    user32 = ctypes.windll.user32
    user32.ShowWindow.argtypes = [ctypes.c_void_p, ctypes.c_int]
    user32.GetClientRect.argtypes = [ctypes.c_void_p, ctypes.POINTER(_Rect)]
    user32.GetWindowRect.argtypes = [ctypes.c_void_p, ctypes.POINTER(_Rect)]
    user32.SetWindowPos.argtypes = [ctypes.c_void_p, ctypes.c_void_p] + [ctypes.c_int] * 4 + [ctypes.c_uint]
    return user32


def find():
    """The handle of this Blender's main window, or None."""
    if sys.platform != "win32":
        return None
    user32 = _user32()
    pid = os.getpid()
    found = []
    callback_type = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def callback(hwnd, _lparam):
        owner = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(owner))
        name = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(ctypes.c_void_p(hwnd), name, 64)
        if owner.value == pid and name.value == "GHOST_WindowClass":
            found.append(hwnd)
        return True

    user32.EnumWindows(callback_type(callback), 0)
    return found[0] if found else None


def show(hwnd, visible):
    if hwnd:
        _user32().ShowWindow(hwnd, _SW_SHOWNOACTIVATE if visible else _SW_HIDE)


def set_client_size(hwnd, width, height):
    """Resize the window so that its client area, and with it the 3D viewport, is width x height.

    SWP_NOSENDCHANGING keeps Windows from clamping the window to the desktop, so the size is
    exact and may exceed the screen.
    """
    if not hwnd:
        return
    user32 = _user32()
    outer, client = _Rect(), _Rect()
    user32.GetWindowRect(hwnd, ctypes.byref(outer))
    user32.GetClientRect(hwnd, ctypes.byref(client))
    extra_width = (outer.right - outer.left) - (client.right - client.left)
    extra_height = (outer.bottom - outer.top) - (client.bottom - client.top)
    user32.SetWindowPos(hwnd, None, 0, 0, width + extra_width, height + extra_height,
                        _SWP_NOMOVE | _SWP_NOZORDER | _SWP_NOACTIVATE | _SWP_NOSENDCHANGING)


def lower_process_priority():
    """Run this Blender below normal priority. A CPU render uses every core; without this the
    application the user works in (Cinema 4D) stutters while the live render refines."""
    if sys.platform != "win32":
        return
    kernel32 = ctypes.windll.kernel32
    kernel32.GetCurrentProcess.restype = ctypes.c_void_p
    kernel32.SetPriorityClass.argtypes = [ctypes.c_void_p, ctypes.c_uint]
    kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), 0x00004000)  # BELOW_NORMAL_PRIORITY_CLASS
