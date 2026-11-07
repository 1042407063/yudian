"""持续发送 0x0900 电量查询，打印应答。"""
import ctypes
import sys
import time
import ctypes.wintypes as wt
import hidprobe as H
from hidprobe import kernel32

TARGET, USAGE, LEN = sys.argv[1], sys.argv[2], int(sys.argv[3])
SECS = float(sys.argv[4])


def build(cmd):
    f = bytearray(64)
    f[0], f[1], f[2], f[3] = 0x4D, 0x01, 0x01, 0x00
    f[4], f[5] = cmd & 0xFF, (cmd >> 8) & 0xFF
    cs = 0
    for i in range(2, 8):
        cs ^= f[i]
    f[8] = cs
    return f


t = H.require_one(TARGET, USAGE)
print(f"=== {TARGET} 轮询 0x0900 {SECS:.0f}s", flush=True)
hw = H._open(t[10])
hr = H._open(t[10], overlapped=True)
frame = build(0x0900)
buf = bytearray(LEN)
buf[0] = 0x11
buf[1:LEN] = frame[:LEN - 1]
b = ctypes.create_string_buffer(bytes(buf), LEN)
end = time.time() + SECS
sent = hits = 0
try:
    while time.time() < end:
        written = wt.DWORD(0)
        kernel32.WriteFile(hw, b, LEN, ctypes.byref(written), None)
        sent += 1
        deadline = time.time() + 0.25
        while time.time() < deadline:
            d = H._read_timeout(hr, max(LEN, 128), 100)
            if not d:
                break
            if len(d) >= 6 and d[:3] == b"\x4d\x01\x01" and d[4] == 0x00 and d[5] == 0x09:
                hits += 1
                print(f"  >>> 应答[{hits}] {d.hex(' ')}", flush=True)
            elif d[:8] != b"\x4d\x01\x01\x01\x00\x00\x00\x00":
                print(f"  事件 {d[:28].hex(' ')}", flush=True)
        time.sleep(0.1)
finally:
    # 长跑脚本里 Ctrl+C 很常见，句柄要么正常关要么随进程回收，
    # 但两个句柄分开关能保证 atexit 之前就释放掉设备占用。
    kernel32.CloseHandle(hw)
    kernel32.CloseHandle(hr)
print(f"发送 {sent} 次，命中 {hits}", flush=True)
