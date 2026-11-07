"""在特性报告上做 SetFeature -> GetFeature 往返。"""
import ctypes
import sys
import time
import hidprobe as H

TARGET, USAGE, LEN = sys.argv[1], sys.argv[2], int(sys.argv[3])
HEX = sys.argv[4].replace(" ", "")
t = H.require_one(TARGET, USAGE)
vid, pid, up, us, il, ol, fl, prod, man, serial, path = t
print(f"=== {vid:04X}:{pid:04X} {up:04X}/{us:04X} FEA={fl} {prod}")
h = H._open(path)
payload = bytes.fromhex(HEX)
buf = ctypes.create_string_buffer(LEN)
ctypes.memmove(buf, payload, min(len(payload), LEN))
ok = H.hid.HidD_SetFeature(h, buf, LEN)
print("SetFeature ->", bool(ok), "err", ctypes.get_last_error())
time.sleep(0.08)
for rid in (payload[0], 0, 0x06, 0x13):
    rb = ctypes.create_string_buffer(LEN)
    ctypes.memmove(rb, bytes([rid]), 1)
    if H.hid.HidD_GetFeature(h, rb, LEN):
        print(f"GetFeature(rid=0x{rid:02x}) -> {rb.raw.hex(' ')}")
    else:
        print(f"GetFeature(rid=0x{rid:02x}) 失败 err={ctypes.get_last_error()}")
H.kernel32.CloseHandle(h)
