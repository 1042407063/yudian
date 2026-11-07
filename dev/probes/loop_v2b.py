"""单异步句柄：SetFeature(0x4D,0x0900) + 读输入报告，统计心跳判断是否醒着。"""
import ctypes
import sys
import time
import hidprobe as H
from hidprobe import hid, kernel32

TARGET, USAGE, SECS = sys.argv[1], sys.argv[2], float(sys.argv[3])
t = H.require_one(TARGET, USAGE)
il = max(t[4], 128)
print(f"=== {TARGET} {USAGE} {SECS:.0f}s", flush=True)
h = H._open(t[10], overlapped=True)

f = bytearray(64)
f[0], f[1], f[2], f[3] = 0x4D, 0x01, 0x01, 0x00
f[4], f[5] = 0x00, 0x09
cs = 0
for i in range(2, 8):
    cs ^= f[i]
f[8] = cs
buf = ctypes.create_string_buffer(bytes(f), 64)

end = time.time() + SECS
tx = hits = hb = 0
last = 0
while time.time() < end:
    if time.time() - last > 0.8:
        ok = hid.HidD_SetFeature(h, buf, 64)
        err = ctypes.get_last_error()
        tx += 1
        last = time.time()
        if not ok and tx == 1:
            print(f"  SetFeature 失败 err={err}", flush=True)
    d = H._read_timeout(h, il, 150)
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
kernel32.CloseHandle(h)
