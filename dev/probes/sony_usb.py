"""抓 USB 模式下的 0x01 报文，打印全部字节。"""
import time
import hidprobe as H
from hidprobe import kernel32

rows = [r for r in H.enum_rows() if r[0] == 0x054C and r[4] == 64]
if not rows:
    raise SystemExit("没找到手柄的 USB 接口（IN=64）；先跑 `python hidprobe.py enum` 看看枚举结果")
t = rows[0]
print(f"USB 接口: IN={t[4]} OUT={t[5]} FEA={t[6]}")
h = H._open(t[10], overlapped=True)
got = None
end = time.time() + 4
while time.time() < end and got is None:
    d = H._read_timeout(h, 128, 300)
    if d and d[0] == 0x01:
        got = d
if got is None:
    print("没收到 0x01")
else:
    print("长度", len(got))
    for i in range(0, len(got), 16):
        print(f"  {i:02d}: " + " ".join(f"{b:02X}" for b in got[i:i+16]))
    print("\n候选电量字节（高4位<=3 且 低4位<=10）：")
    for i in range(40, min(len(got), 60)):
        b = got[i]
        if (b >> 4) <= 3 and (b & 0xF) <= 10:
            print(f"  [{i}] = 0x{b:02X} 档位={b & 0xF} 状态={b >> 4}")
kernel32.CloseHandle(h)
