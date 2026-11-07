"""发输出报告 0x31，看输入流是否切换到完整报告。"""
import ctypes
import time
import collections
import ctypes.wintypes as wt
import hidprobe as H
from hidprobe import kernel32

t = H.require_one(*H.target_args("054C:0CE6", "0001:0005"))
hw = H._open(t[10])
hr = H._open(t[10], overlapped=True)

def sample(sec, label):
    cnt = collections.Counter()
    first = {}
    end = time.time() + sec
    while time.time() < end:
        d = H._read_timeout(hr, 128, 250)
        if not d:
            continue
        cnt[d[0]] += 1
        first.setdefault(d[0], d)
    print(f"{label}: {[(hex(k), v) for k, v in cnt.items()]}")
    for rid, d in first.items():
        if rid == 0x31 and len(d) > 54:
            b = d[53]
            print(f"   0x31 [53]=0x{b:02X} 档位={b & 0xF} 状态={b >> 4} -> {(b & 0xF) * 10}%")
            print("   前 24 字节:", d[:24].hex(" "))

sample(1, "发送前")

# 78 字节输出报告 0x31，全零
buf = bytearray(78)
buf[0] = 0x31
b = ctypes.create_string_buffer(bytes(buf), 78)
written = wt.DWORD(0)
ok = kernel32.WriteFile(hw, b, 78, ctypes.byref(written), None)
print("写入 0x31 x78 ->", bool(ok), ctypes.get_last_error())
time.sleep(0.2)
sample(2, "发送后(全零)")

kernel32.CloseHandle(hw)
kernel32.CloseHandle(hr)
