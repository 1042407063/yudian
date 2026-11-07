"""试不同校验算法下发 0x31 输出报告，观察输入流是否切到 0x31。"""
import ctypes
import time
import collections
import ctypes.wintypes as wt
import hidprobe as H
from hidprobe import kernel32

t = H.require_one(*H.target_args("054C:0CE6", "0001:0005"))
hw = H._open(t[10])
hr = H._open(t[10], overlapped=True)

def sample(sec=1.0):
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

def build(seed, flags1=0x02, flag0=0x00, valid0=0x00):
    b = bytearray(78)
    b[0] = 0x31
    b[1] = flags1
    b[2] = 0xFF          # seq
    b[3] = 0x10          # tag
    b[39] = valid0
    b[44] = flag0
    crc = seed
    for i in range(77):
        crc = (crc + b[i]) & 0xFF
    b[77] = (0x100 - crc) & 0xFF
    return b

for label, seed in [("无种子", 0x00), ("0xA2 种子", 0xA2), ("0xA1 种子", 0xA1)]:
    for flags1 in (0x02, 0x00, 0xF7):
        b = build(seed, flags1=flags1)
        buf = ctypes.create_string_buffer(bytes(b), 78)
        written = wt.DWORD(0)
        ok = kernel32.WriteFile(hw, buf, 78, ctypes.byref(written), None)
        time.sleep(0.25)
        cnt, first = sample(0.9)
        tag = ",".join(f"0x{k:02X}x{v}" for k, v in sorted(cnt.items()))
        print(f"{label:10} flags1=0x{flags1:02X} write={'ok' if ok else 'fail'} 输入={tag}")
        if 0x31 in first:
            d = first[0x31]
            print("   >>> 已切换! [53]=0x%02X" % d[53])
            raise SystemExit
kernel32.CloseHandle(hw)
kernel32.CloseHandle(hr)
