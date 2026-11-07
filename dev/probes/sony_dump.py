"""抓 DualSense 输入报告，打印原始字节与候选电量字节。"""
import time
import hidprobe as H
from hidprobe import kernel32

t = H.require_one(*H.target_args("054C:0CE6", "0001:0005"))
print(f"=== {t[0]:04X}:{t[1]:04X} IN={t[4]} 产品={t[7]}")
print(f"    路径={t[10]}")
h = H._open(t[10], overlapped=True)
seen = {}
end = time.time() + 6
while time.time() < end and len(seen) < 4:
    d = H._read_timeout(h, max(t[4], 128), 400)
    if not d:
        continue
    rid = d[0]
    if rid in seen:
        continue
    seen[rid] = d
    print(f"\n-- report 0x{rid:02X} 长度 {len(d)} --")
    print("   " + d.hex(" "))
    for off in (51, 52, 53, 54):
        if off < len(d):
            b = d[off]
            print(f"   [{off}] = 0x{b:02X} 档位={b & 0x0F} 状态={b >> 4} -> {(b & 0x0F) * 10}%")
kernel32.CloseHandle(h)
