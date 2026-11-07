"""长时间监听 DualSense，统计报告 ID 分布。"""
import time
import collections
import hidprobe as H
from hidprobe import kernel32

t = H.require_one(*H.target_args("054C:0CE6", "0001:0005"))
h = H._open(t[10], overlapped=True)
cnt = collections.Counter()
first = {}
end = time.time() + 8
while time.time() < end:
    d = H._read_timeout(h, max(t[4], 128), 300)
    if not d:
        continue
    cnt[d[0]] += 1
    if d[0] not in first:
        first[d[0]] = d
print("报告 ID 分布:", {hex(k): v for k, v in cnt.items()})
for rid, d in first.items():
    print(f"\n0x{rid:02X} len={len(d)}: {d[:40].hex(' ')} ...")
    if len(d) > 54:
        for off in (52, 53):
            b = d[off]
            print(f"   [{off}]=0x{b:02X} 档位={b & 0xF} 状态={b >> 4}")
kernel32.CloseHandle(h)
