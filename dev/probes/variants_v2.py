"""试不同投递方式，观察是否能引出 0x0900 状态帧。"""
import ctypes
import sys
import time
import ctypes.wintypes as wt
import hidprobe as H
from hidprobe import kernel32

TARGET, USAGE, LEN = sys.argv[1], sys.argv[2], int(sys.argv[3])


def v2(cmd, data=b"", ver=0x01, flags=0x01, seq=0x00, biz=0x00):
    n = len(data)
    f = bytearray(64)
    f[0], f[1], f[2], f[3] = 0x4D, ver, flags, n
    f[4], f[5] = cmd & 0xFF, (cmd >> 8) & 0xFF
    f[6], f[7] = biz, seq
    f[8:8 + n] = data
    cs = 0
    for i in range(2, 8 + n):
        cs ^= f[i]
    f[8 + n] = cs
    return f


t = H.require_one(TARGET, USAGE)
path = t[10]
il = max(t[4], 128)
print(f"=== {TARGET} {USAGE}")
hr = H._open(path, overlapped=True)
hw = H._open(path)

CASES = {
    "A 11|v2 0900": (11, v2(0x0900)),
    "B 11|v2 0900 BE": (11, v2(0x0009)),
    "C 13|v2 0900": (0x13, v2(0x0900)),
    "D 11|v2 0900 无flags": (11, v2(0x0900, flags=0x00)),
    "E 11|v2 0900 biz1": (11, v2(0x0900, biz=0x01)),
    "F 11|v2 0900 seq1": (11, v2(0x0900, seq=0x01)),
    "G 11|v2 0000": (11, v2(0x0000)),
}
for label, (rid, frame) in CASES.items():
    H.drain(hr, il, 60)
    buf = bytearray(LEN)
    buf[0] = rid
    buf[1:LEN] = frame[:LEN - 1]
    b = ctypes.create_string_buffer(bytes(buf), LEN)
    written = wt.DWORD(0)
    ok = kernel32.WriteFile(hw, b, LEN, ctypes.byref(written), None)
    err = ctypes.get_last_error()
    hits = 0
    events = []
    deadline = time.time() + 2.5
    while time.time() < deadline:
        d = H._read_timeout(hr, il, 200)
        if not d:
            continue
        if len(d) >= 6 and d[:3] == b"\x4d\x01\x01" and d[4] == 0x00 and d[5] == 0x09:
            hits += 1
            events.append(d.hex(" "))
        elif d[:8] != b"\x4d\x01\x01\x01\x00\x00\x00\x00":
            events.append("其他 " + d[:24].hex(" "))
    print(f"  {label:22} write={'ok' if ok else err} 0x0900帧={hits}")
    for e in events[:3]:
        print("      ", e)
kernel32.CloseHandle(hw)
kernel32.CloseHandle(hr)
