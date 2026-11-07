"""实机截取展示面板，用于验证毛玻璃 / 图标这类必须看真实合成效果的东西。

--shot 走的是离线渲染，抓不到"面板背后"的桌面，所以毛玻璃只能在真机上看。

用法：
    python shot_panel.py out.bmp [等待毫秒]

流程：杀掉旧实例 → 启动 yudian.exe → 给主窗口发 IDM_SHOW →
      等出现动画走完 → 按面板窗口矩形从屏幕 BitBlt 一块出来存成 BMP。
"""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)

SRCCOPY = 0x00CC0020
CAPTUREBLT = 0x40000000          # 少了这个，分层窗口（面板自己）截出来是空的
WM_COMMAND = 0x0111
IDM_SHOW = 100
DETACHED_PROCESS = 0x00000008

EXE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "..", "app", "build", "yudian.exe")


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
                ("biPlanes", wt.WORD), ("biBitCount", wt.WORD),
                ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG),
                ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD)]


def find(cls):
    h = user32.FindWindowW(cls, None)
    if not h:
        raise RuntimeError("窗口不存在: " + cls)
    return h


def grab(x, y, w, h):
    screen = user32.GetDC(None)
    mem = gdi32.CreateCompatibleDC(screen)
    bi = BITMAPINFOHEADER()
    bi.biSize = ctypes.sizeof(bi)
    bi.biWidth = w
    bi.biHeight = -h              # 负数 = 自上而下，省得再翻行
    bi.biPlanes = 1
    bi.biBitCount = 32
    bits = ctypes.c_void_p()
    dib = gdi32.CreateDIBSection(screen, ctypes.byref(bi), 0, ctypes.byref(bits), None, 0)
    old = gdi32.SelectObject(mem, dib)
    ok = gdi32.BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT)
    gdi32.SelectObject(mem, old)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(None, screen)
    if not ok:
        gdi32.DeleteObject(dib)
        raise RuntimeError("BitBlt 失败")
    data = ctypes.string_at(bits, w * h * 4)
    gdi32.DeleteObject(dib)       # 像素已经拷出来了，位图句柄要显式释放
    return data


def write_bmp(path, w, h, data):
    """32bpp BGRA 自上而下 → BMP（BMP 要求自下而上）"""
    stride = w * 4
    rows = [data[y * stride:(y + 1) * stride] for y in range(h)]
    body = b"".join(reversed(rows))
    header = ctypes.create_string_buffer(40)
    bi = BITMAPINFOHEADER()
    bi.biSize = 40
    bi.biWidth = w
    bi.biHeight = h
    bi.biPlanes = 1
    bi.biBitCount = 32
    ctypes.memmove(header, ctypes.byref(bi), 40)
    fh = b"BM" + (14 + 40 + len(body)).to_bytes(4, "little") + b"\0\0\0\0" + (54).to_bytes(4, "little")
    with open(path, "wb") as f:
        f.write(fh)
        f.write(header.raw)
        f.write(body)


def close_running(exe_name="yudian.exe", wait=1.5):
    """先礼后兵：先给已有实例发 WM_CLOSE 让它自己退，超时才按镜像名强杀。

    直接 `taskkill /IM` 会杀掉机器上所有同名进程，也不认"其实是我另一个副本"。
    注意产物名必须和 build.bat 一致（yudian.exe），否则这里永远匹配不到。
    """
    wnd = user32.FindWindowW("PeripheralBatteryMain", None)
    if wnd:
        user32.PostMessageW(wnd, 0x0010, 0, 0)          # WM_CLOSE
        end = time.time() + wait
        while time.time() < end:
            if not user32.FindWindowW("PeripheralBatteryMain", None):
                return True
            time.sleep(0.05)
        subprocess.run(["taskkill", "/IM", exe_name, "/F"], capture_output=True)
    return True


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "panel-live.bmp"
    wait = int(sys.argv[2]) if len(sys.argv) > 2 else 1400

    close_running()
    time.sleep(0.4)
    subprocess.Popen([EXE], creationflags=DETACHED_PROCESS, close_fds=True)
    time.sleep(1.5)

    main_wnd = find("PeripheralBatteryMain")
    user32.PostMessageW(main_wnd, WM_COMMAND, IDM_SHOW, 0)
    time.sleep(wait / 1000.0)

    panel = find("PeripheralBatteryPanel")
    r = wt.RECT()
    user32.GetWindowRect(panel, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    data = grab(r.left, r.top, w, h)
    write_bmp(out, w, h, data)
    print(f"panel {w}x{h} at ({r.left},{r.top}) -> {out}")


if __name__ == "__main__":
    main()
