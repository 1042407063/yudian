"""用输出报告 0x4D 发 0x0900，异步句柄读应答。"""
import ctypes
import sys
import time
import ctypes.wintypes as wt
import hidprobe as H
from hidprobe import kernel32

TARGET, USAGE, SECS = sys.argv[1], sys.argv[2], float(sys.argv[3])
t = H.require_one(TARGET, USAGE)
il = max(t[4], 128)
print(f"=== {TARGET} {USAGE} {SECS:.0f}s 输出报告 0x4D", flush=True)
hr = H._open(t[10], overlapped=True)
hw = H._open(t[10])

frame = bytearray(64)
frame[0], frame[1], frame[2], frame[3] = 0x4D, 0x01, 0x01, 0x00
frame[4], frame[5] = 0x00, 0x09
cs = 0
for i in range(2, 8):
    cs ^= frame[i]
frame[8] = cs
b = ctypes.create_string_buffer(bytes(frame), 64)

end = time.time() + SECS
tx = hits = hb = 0
last = 0
while time.time() < end:
    if time.time() - last > 0.7:
        written = wt.DWORD(0)
        ok = kernel32.WriteFile(hw, b, 64, ctypes.byref(written), None)
        err = ctypes.get_last_error()
        tx += 1
        last = time.time()
        if not ok:
            print(f"  写入失败 {err}", flush=True)
    d = H._read_timeout(hr, il, 150)
    if not d:
        continue
    if d[:8] == b"\x4d\x01\x01\x01\x00\x00\x00\x00":
        hb += 1
        continue
    if d[0] == 0x4D and d[4] == 0x00 and d[5] == 0x09:
        hits += 1
        n = d[3]
        if len(d) < 8 + n:
                continue
        data = d[8:8 + n]
        print(f"  >>> 应答 {d.hex(' ')}", flush=True)
        if n >= 13:
            print(f"      VID=0x{data[1]:02x}{data[0]:02x} PID=0x{data[3]:02x}{data[2]:02x} "
                  f"连接={data[10]} 充电={data[11]} 电量={data[12]}%", flush=True)
    else:
        print(f"  帧 {d[:30].hex(' ')}", flush=True)
print(f"发送 {tx} 次，心跳 {hb}，命中 {hits}", flush=True)
kernel32.CloseHandle(hw)
kernel32.CloseHandle(hr)
