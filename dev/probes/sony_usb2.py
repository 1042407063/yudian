"""连续多帧确认电量字节位置。"""
import time
import hidprobe as H
from hidprobe import kernel32

rows = [r for r in H.enum_rows() if r[0] == 0x054C and r[4] == 64]
if not rows:
    raise SystemExit("没找到手柄的 USB 接口（IN=64）；先跑 `python hidprobe.py enum` 看看枚举结果")
h = H._open(rows[0][10], overlapped=True)
n = 0
end = time.time() + 4
while time.time() < end and n < 5:
    d = H._read_timeout(h, 128, 300)
    if not d or d[0] != 0x01:
        continue
    if len(d) < 55:
        print(f"帧过短 len={len(d)}，跳过")
        continue
    n += 1
    b52, b53, b54 = d[52], d[53], d[54]
    print(f"帧{n}: [52]=0x{b52:02X} [53]=0x{b53:02X} [54]=0x{b54:02X} "
          f"-> 53: {b53 & 0xF}*10={(b53 & 0xF) * 10}% 状态={b53 >> 4}")
kernel32.CloseHandle(h)
