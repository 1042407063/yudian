"""单句柄异步读写：发送 Protocol V2 电量查询并收集所有帧。"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

import hidprobe as H
from hidprobe import hid, kernel32

kernel32.CancelIoEx.argtypes = [wt.HANDLE, ctypes.c_void_p]


class OV(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_void_p), ("InternalHigh", ctypes.c_void_p),
                ("Offset", wt.DWORD), ("OffsetHigh", wt.DWORD), ("hEvent", wt.HANDLE)]


def rd(h, length, ms):
    ev = kernel32.CreateEventW(None, True, False, None)
    ov = OV()
    ov.hEvent = ev
    buf = ctypes.create_string_buffer(length)
    got = wt.DWORD(0)
    ok = kernel32.ReadFile(h, buf, length, ctypes.byref(got), ctypes.byref(ov))
    if not ok:
        if ctypes.get_last_error() != 997:
            kernel32.CloseHandle(ev)
            return None
        rc = kernel32.WaitForSingleObject(ev, ms)
        if rc != 0:
            kernel32.CancelIoEx(h, ctypes.byref(ov))
            kernel32.GetOverlappedResult(h, ctypes.byref(ov), ctypes.byref(got), True)
            kernel32.CloseHandle(ev)
            return None
        kernel32.GetOverlappedResult(h, ctypes.byref(ov), ctypes.byref(got), False)
    kernel32.CloseHandle(ev)
    return buf.raw[:got.value] if got.value else None


def wr(h, data, ms=800):
    ev = kernel32.CreateEventW(None, True, False, None)
    ov = OV()
    ov.hEvent = ev
    buf = ctypes.create_string_buffer(bytes(data), len(data))
    got = wt.DWORD(0)
    ok = kernel32.WriteFile(h, buf, len(data), ctypes.byref(got), ctypes.byref(ov))
    if not ok:
        err = ctypes.get_last_error()
        if err != 997:
            kernel32.CloseHandle(ev)
            return False, err
        rc = kernel32.WaitForSingleObject(ev, ms)
        if rc != 0:
            kernel32.CancelIoEx(h, ctypes.byref(ov))
            kernel32.GetOverlappedResult(h, ctypes.byref(ov), ctypes.byref(got), True)
            kernel32.CloseHandle(ev)
            return False, -1
        kernel32.GetOverlappedResult(h, ctypes.byref(ov), ctypes.byref(got), False)
    kernel32.CloseHandle(ev)
    return True, got.value


def build_v2(cmd, data=b""):
    n = len(data)
    f = bytearray(64)
    f[0], f[1], f[2], f[3] = 0x4D, 0x01, 0x01, n
    f[4], f[5] = cmd & 0xFF, (cmd >> 8) & 0xFF
    f[8:8 + n] = data
    cs = 0
    for i in range(2, 8 + n):
        cs ^= f[i]
    f[8 + n] = cs
    return f


def describe(d):
    if d[:3] != b"\x4d\x01\x01":
        return "非 V2 帧"
    n = d[3]
    cmd = d[4] | (d[5] << 8)
    if len(d) < 9 + n:
        return f"V2 cmd=0x{cmd:04x} len={n} 截断（收到 {len(d)} 字节）"
    return f"V2 cmd=0x{cmd:04x} len={n} data={d[8:8 + n].hex(' ')} cs=0x{d[8 + n]:02x}"


TARGET, USAGE, LEN, SECS = sys.argv[1], sys.argv[2], int(sys.argv[3]), float(sys.argv[4])
t = H.require_one(TARGET, USAGE)
print(f"=== {t[0]:04X}:{t[1]:04X} {t[7]} 运行 {SECS:.0f}s", flush=True)
h = H._open(t[10], overlapped=True)

frame = build_v2(0x0900)
payload = bytearray(LEN)
payload[0] = 0x11
payload[1:LEN] = frame[:LEN - 1]

start = time.time()
last_send = 0
stats = {"tx": 0, "heartbeat": 0, "event": 0, "answer": 0}
while time.time() - start < SECS:
    if time.time() - last_send > 0.6:
        ok, info = wr(h, payload)
        stats["tx"] += 1
        last_send = time.time()
        if not ok:
            print(f"  写入失败 {info}", flush=True)
    d = rd(h, max(LEN, 128), 200)
    if not d:
        continue
    if d[:8] == b"\x4d\x01\x01\x01\x00\x00\x00\x00":
        stats["heartbeat"] += 1
        continue
    if d[:3] == b"\x4d\x01\x01" and d[4] == 0x00 and d[5] == 0x09:
        stats["answer"] += 1
        print(f"  >>> 电量应答 {d.hex(' ')}", flush=True)
    else:
        stats["event"] += 1
        print(f"  帧 {describe(d)} | {d[:32].hex(' ')}", flush=True)

print(f"统计 {stats}", flush=True)
kernel32.CloseHandle(h)
