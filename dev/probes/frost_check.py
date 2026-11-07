"""毛玻璃端到端验证：在面板将要出现的位置铺一块高对比度彩条窗口，
再唤起面板截图。白色壁纸上毛玻璃看不出来，必须给个有内容的背景。

彩条窗口用分层窗口（UpdateLayeredWindow）直接推像素，不需要消息循环。

用法：
    python frost_check.py out.bmp
"""
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys
import time

import shot_panel as sp

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

WS_POPUP = 0x80000000
WS_VISIBLE = 0x10000000
WS_EX_LAYERED = 0x00080000
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_TOPMOST = 0x00000008
ULW_ALPHA = 0x00000002
AC_SRC_OVER = 0
AC_SRC_ALPHA = 1

# 返回值和第 4 个参数在 x64 上都是 64 位：用 c_ssize_t / 显式 argtypes，
# 否则 LPARAM 会按 32 位转换，窗口过程每次都抛 OverflowError（异常被吞掉）。
WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
user32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.DefWindowProcW.restype = ctypes.c_ssize_t


class WNDCLASSW(ctypes.Structure):
    _fields_ = [("style", wt.UINT), ("lpfnWndProc", WNDPROC),
                ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int),
                ("hInstance", wt.HINSTANCE), ("hIcon", wt.HICON),
                ("hCursor", wt.HANDLE), ("hbrBackground", wt.HBRUSH),
                ("lpszMenuName", wt.LPCWSTR), ("lpszClassName", wt.LPCWSTR)]


class BLENDFUNCTION(ctypes.Structure):
    _fields_ = [("BlendOp", ctypes.c_ubyte), ("BlendFlags", ctypes.c_ubyte),
                ("SourceConstantAlpha", ctypes.c_ubyte), ("AlphaFormat", ctypes.c_ubyte)]


STRIPES = [(0x2D, 0x11, 0xE8), (0x00, 0x8A, 0xFF), (0x00, 0xD4, 0xFF),
           (0x86, 0xB8, 0x12), (0xFF, 0x90, 0x1E), (0xFF, 0x61, 0x7B),
           (0xA4, 0x3F, 0xFF), (0xC7, 0xC2, 0x00)]


def make_bg_window(x, y, w, h):
    inst = kernel32.GetModuleHandleW(None)
    proc = WNDPROC(lambda hwnd, msg, wp, lp: user32.DefWindowProcW(hwnd, msg, wp, lp))
    cls = "YudianFrostBackdrop"
    wc = WNDCLASSW()
    wc.style = 0
    wc.lpfnWndProc = proc
    wc.hInstance = inst
    wc.lpszClassName = cls
    user32.RegisterClassW(ctypes.byref(wc))

    hwnd = user32.CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, cls, "bg",
        WS_POPUP | WS_VISIBLE, x, y, w, h, None, None, inst, None)
    if not hwnd:
        raise RuntimeError("背景窗口创建失败")

    bi = sp.BITMAPINFOHEADER()
    bi.biSize = ctypes.sizeof(bi)
    bi.biWidth = w
    bi.biHeight = -h
    bi.biPlanes = 1
    bi.biBitCount = 32
    bits = ctypes.c_void_p()
    screen = user32.GetDC(None)
    mem = gdi32.CreateCompatibleDC(screen)
    dib = gdi32.CreateDIBSection(screen, ctypes.byref(bi), 0, ctypes.byref(bits), None, 0)
    gdi32.SelectObject(mem, dib)

    buf = (ctypes.c_ubyte * (w * h * 4)).from_address(bits.value)
    for yy in range(h):
        r, g, b = STRIPES[(yy // 26) % len(STRIPES)]
        for xx in range(w):
            i = (yy * w + xx) * 4
            buf[i + 0] = b
            buf[i + 1] = g
            buf[i + 2] = r
            buf[i + 3] = 255

    blend = BLENDFUNCTION(AC_SRC_OVER, 0, 255, AC_SRC_ALPHA)
    size = wt.SIZE(w, h)
    dst = wt.POINT(x, y)
    src = wt.POINT(0, 0)
    user32.UpdateLayeredWindow(hwnd, screen, ctypes.byref(dst), ctypes.byref(size),
                               mem, ctypes.byref(src), 0, ctypes.byref(blend), ULW_ALPHA)
    user32.ReleaseDC(None, screen)
    return hwnd, proc, mem, dib


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "frost.bmp"
    hwnd, proc, mem, dib = make_bg_window(1200, 460, 960, 620)
    try:
        sp.close_running()
        time.sleep(0.3)
        subprocess.Popen([sp.EXE], creationflags=sp.DETACHED_PROCESS, close_fds=True)
        time.sleep(1.6)
        main_wnd = sp.find("PeripheralBatteryMain")
        user32.PostMessageW(main_wnd, sp.WM_COMMAND, sp.IDM_SHOW, 0)
        time.sleep(1.6)
        panel = sp.find("PeripheralBatteryPanel")
        r = wt.RECT()
        user32.GetWindowRect(panel, ctypes.byref(r))
        w, h = r.right - r.left, r.bottom - r.top
        sp.write_bmp(out, w, h, sp.grab(r.left, r.top, w, h))
        print(f"panel {w}x{h} -> {out}")
    finally:
        user32.DestroyWindow(hwnd)
        gdi32.DeleteDC(mem)
        gdi32.DeleteObject(dib)


if __name__ == "__main__":
    main()
