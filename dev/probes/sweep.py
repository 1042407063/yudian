"""一轮扫描：多种 0x0900 请求变体，捕捉 0x0900 应答/其他帧。"""
import ctypes
import sys
import time
import hidprobe as H
from hidprobe import hid, kernel32

TARGET, USAGE, SECS = sys.argv[1], sys.argv[2], float(sys.argv[3])
t = H.require_one(TARGET, USAGE)
il = max(t[4], 128)
print(f"=== {TARGET} {USAGE} 扫 {SECS:.0f}s", flush=True)
h = H._open(t[10], overlapped=True)


def build(cmd, data=b"", flags=0x01, biz=0x00, ver=0x01):
    n = len(data)
    f = bytearray(64)
    f[0], f[1], f[2], f[3] = 0x4D, ver, flags, n
    f[4], f[5] = cmd & 0xFF, (cmd >> 8) & 0xFF
    f[6] = biz
    f[8:8 + n] = data
    cs = 0
    for i in range(2, 8 + n):
        cs ^= f[i]
    f[8 + n] = cs
    return f


VARIANTS = [
    ("0900 N=0", build(0x0900)),
    ("0900 N=1 idx0", build(0x0900, b"\x00")),
    ("0900 N=1 idx1", build(0x0900, b"\x01")),
    ("0900 N=4", build(0x0900, b"\x00\x00\x00\x00")),
    ("0901 N=0", build(0x0901)),
    ("8900 N=0", build(0x8900)),
    ("0900 flags2", build(0x0900, b"", flags=0x02)),
    ("0900 biz1", build(0x0900, b"", biz=0x01)),
    ("0002 N=0", build(0x0002)),
]

start = time.time()
i = 0
hits = hb = 0
last = 0
while time.time() - start < SECS:
    name, frame = VARIANTS[i % len(VARIANTS)]
    buf = ctypes.create_string_buffer(bytes(frame), 64)
    ok = hid.HidD_SetFeature(h, buf, 64)
    err = ctypes.get_last_error()
    deadline = time.time() + 1.4
    got = False
    while time.time() < deadline:
        d = H._read_timeout(h, il, 120)
        if not d:
            continue
        if d[:8] == b"\x4d\x01\x01\x01\x00\x00\x00\x00":
            hb += 1
            continue
        if d[0] == 0x4D and d[4] == 0x00 and d[5] == 0x09:
            hits += 1
            got = True
            n = d[3]
            if len(d) < 8 + n:
                continue
            data = d[8:8 + n]
            print(f"  >>> [{name}] 应答 {d.hex(' ')}", flush=True)
            if n >= 13:
                print(f"      VID=0x{data[1]:02x}{data[0]:02x} PID=0x{data[3]:02x}{data[2]:02x} "
                      f"连接={data[10]} 充电={data[11]} 电量={data[12]}%", flush=True)
        else:
            print(f"  帧[{name}] {d[:30].hex(' ')}", flush=True)
    print(f"  -> {name} set={'ok' if ok else err} 命中={got}", flush=True)
    i += 1
print(f"共 {i} 轮，心跳 {hb}，命中 {hits}", flush=True)
kernel32.CloseHandle(h)
