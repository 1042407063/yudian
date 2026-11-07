"""跨接口测试：在 FF0B 发送，在 FF01 读应答。"""
import ctypes
import sys
import time
import hidprobe as H
from hidprobe import hid, kernel32

t1 = H.require_one(*H.target_args("3837:1018", "FF01:0001"))
t2 = H.require_one(*H.target_args("3837:1018", "FF0B:00C4"))
hr = H._open(t1[10], overlapped=True)
hs = H._open(t2[10])

frame = bytearray(64)
frame[0], frame[1], frame[2], frame[3] = 0x4D, 0x01, 0x01, 0x00
frame[4], frame[5] = 0x00, 0x09
cs = 0
for i in range(2, 8):
    cs ^= frame[i]
frame[8] = cs

print("FF0B 长度", t2[4], t2[5], t2[6])
for rid in (0x2A, 0x2B):
    for n in (t2[6], 61, 64):
        buf = bytearray(n)
        buf[0] = rid
        take = min(n - 1, len(frame))
        buf[1:1 + take] = frame[:take]
        b = ctypes.create_string_buffer(bytes(buf), n)
        ok = hid.HidD_SetFeature(hs, b, n)
        err = ctypes.get_last_error()
        print(f"\nSetFeature FF0B rid=0x{rid:02x} len={n} -> {bool(ok)} err={err}")
        end = time.time() + 1.5
        while time.time() < end:
            d = H._read_timeout(hr, 128, 200)
            if not d:
                continue
            if d[:8] == b"\x4d\x01\x01\x01\x00\x00\x00\x00":
                continue
            print("   rx", d[:30].hex(" "))
            if d[0] == 0x4D and d[4] == 0x00 and d[5] == 0x09:
                n2 = d[3]
                if len(d) < 8 + n2 or n2 < 13:
                    continue          # 短帧/未就绪帧：不要直接索引 data[12]
                data = d[8:8 + n2]
                print(f"   >>> 电量 {data[12]}% 充电 {data[11]} 连接 {data[10]}")
kernel32.CloseHandle(hs)
kernel32.CloseHandle(hr)
