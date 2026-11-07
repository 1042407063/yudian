"""经典特性协议：report 0x11，20 字节，XOR 0xFF 取反。"""
import ctypes
import sys
import time
import hidprobe as H
from hidprobe import hid, kernel32

t = H.require_one(*H.target_args("3837:1018", "FF01:0001"))
h = H._open(t[10])
print("FEA", t[6])

def try_one(label, payload, n):
    buf = bytearray(n)
    buf[:len(payload)] = payload
    b = ctypes.create_string_buffer(bytes(buf), n)
    ok = hid.HidD_SetFeature(h, b, n)
    e1 = ctypes.get_last_error()
    time.sleep(0.06)
    rb = ctypes.create_string_buffer(n)
    ctypes.memmove(rb, bytes([0x11]), 1)
    got = hid.HidD_GetFeature(h, rb, n)
    e2 = ctypes.get_last_error()
    print(f"  {label:26} set={bool(ok)}({e1}) get={bool(got)}({e2})")
    if got:
        print("      ", rb.raw.hex(" "))
        inv = bytes(x ^ 0xFF for x in rb.raw)
        print("      取反后:", inv.hex(" "))
        print(f"      data[8]={inv[8] if n>8 else '?'} data[9]={inv[9] if n>9 else '?'}")

for n in (20, 64):
    print(f"\n-- 长度 {n} --")
    try_one("取反 0x06 + 0xFF 填充", bytes([0x11, 0x06 ^ 0xFF]) + b"\xff" * (n - 2), n)
    try_one("原样 0x06 + 0x00 填充", bytes([0x11, 0x06]) + b"\x00" * (n - 2), n)
    try_one("0x11 0x41 轮询率查询", bytes([0x11, 0x41 ^ 0xFF]) + b"\xff" * (n - 2), n)
kernel32.CloseHandle(h)
