"""用 SetFeature(report 0x4D) 发送 V2 命令并读取输入报告。"""
import ctypes
import sys
import time
import hidprobe as H
from hidprobe import hid, kernel32

TARGET, USAGE, LEN = sys.argv[1], sys.argv[2], int(sys.argv[3])


def v2(cmd, data=b""):
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


t = H.require_one(TARGET, USAGE)
il = max(t[4], 128)
print(f"=== {TARGET} {USAGE} FEA={t[6]}")
hs = H._open(t[10])
hr = H._open(t[10], overlapped=True)

for cmd in (0x0900, 0x0002, 0x0009):
    H.drain(hr, il, 60)
    frame = v2(cmd)
    b = ctypes.create_string_buffer(bytes(frame), 64)
    ok = hid.HidD_SetFeature(hs, b, 64)
    err = ctypes.get_last_error()
    print(f"\n  cmd=0x{cmd:04x} SetFeature={'ok' if ok else err} "
          f"帧={frame[:10].hex(' ')}")
    hits, others = 0, []
    end = time.time() + 2.0
    while time.time() < end:
        d = H._read_timeout(hr, il, 200)
        if not d:
            continue
        if len(d) > 6 and d[0] == 0x4D and d[4] == (cmd & 0xFF) and d[5] == (cmd >> 8):
            hits += 1
            n = d[3]
            if len(d) < 8 + n:
                continue
            data = d[8:8 + n]
            print(f"    >>> 应答 {d.hex(' ')}")
            print(f"        payload={data.hex(' ')}")
        elif d[:8] != b"\x4d\x01\x01\x01\x00\x00\x00\x00":
            others.append(d)
    print(f"    命中 {hits} 条，其他帧 {len(others)} 条")
    for d in others[:3]:
        print("      其他", d[:28].hex(" "))
kernel32.CloseHandle(hs)
kernel32.CloseHandle(hr)
