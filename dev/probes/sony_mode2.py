"""按 hid-playstation 的写法：crc = 0xA2 + sum(buf[1..76])，再取补。"""
import ctypes
import time
import collections
import ctypes.wintypes as wt
import hidprobe as H
from hidprobe import kernel32

t = H.require_one(*H.target_args("054C:0CE6", "0001:0005"))
hw = H._open(t[10])
hr = H._open(t[10], overlapped=True)

def sample(sec=0.9):
    cnt = collections.Counter()
    first = {}
    end = time.time() + sec
    while time.time() < end:
        d = H._read_timeout(hr, 128, 200)
        if not d:
            continue
        cnt[d[0]] += 1
        first.setdefault(d[0], d)
    return cnt, first

def build(seed, start, flags1):
    b = bytearray(78)
    b[0] = 0x31
    b[1] = flags1
    b[2] = 0xFF
    b[3] = 0x10
    crc = seed
    for i in range(start, 77):
        crc = (crc + b[i]) & 0xFF
    b[77] = (0x100 - crc) & 0xFF
    return b

cases = [
    ("0xA2+body", 0xA2, 1),
    ("0x00+body", 0x00, 1),
    ("0xA1+body", 0xA1, 1),
    ("0xA2+全帧", 0xA2, 0),
]
for label, seed, start in cases:
    for flags1 in (0x02, 0x00):
        b = build(seed, start, flags1)
        buf = ctypes.create_string_buffer(bytes(b), 78)
        written = wt.DWORD(0)
        ok = kernel32.WriteFile(hw, buf, 78, ctypes.byref(written), None)
        time.sleep(0.2)
        cnt, first = sample()
        tag = ",".join(f"0x{k:02X}x{v}" for k, v in sorted(cnt.items()))
        print(f"{label:10} flags1=0x{flags1:02X} crc=0x{b[77]:02X} write={'ok' if ok else 'fail'} 输入={tag}")
        if 0x31 in first:
            print(f"   >>> 切换成功 [53]=0x{first[0x31][53]:02X}")
            raise SystemExit
kernel32.CloseHandle(hw)
kernel32.CloseHandle(hr)
