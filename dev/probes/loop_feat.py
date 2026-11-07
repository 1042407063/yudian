"""持续轮询 0x13/0x4a 无线电量命令，捕捉任何有效应答。"""
import ctypes
import sys
import time

import hidprobe as H
from hidprobe import hid, kernel32

TARGET, USAGE, LEN = sys.argv[1], sys.argv[2], int(sys.argv[3])
SECONDS = float(sys.argv[4]) if len(sys.argv) > 4 else 90.0
CMD = bytes.fromhex(sys.argv[5]) if len(sys.argv) > 5 else bytes([0x13, 0x4a, 0x01, 0x00])

t = H.require_one(TARGET, USAGE)
vid, pid, up, us, il, ol, fl, prod, man, serial, path = t
print(f"=== {vid:04X}:{pid:04X} {prod} FEA={fl} 轮询 {SECONDS:.0f}s 命令={CMD.hex(' ')}",
      flush=True)

hs = H._open(path)
hr = H._open(path, overlapped=True)
end = time.time() + SECONDS
i = 0
try:
    while time.time() < end:
        i += 1
        buf = ctypes.create_string_buffer(LEN)
        ctypes.memmove(buf, CMD, min(len(CMD), LEN))
        ok = hid.HidD_SetFeature(hs, buf, LEN)
        err = ctypes.get_last_error()
        time.sleep(0.06)

        rb = ctypes.create_string_buffer(LEN)
        ctypes.memmove(rb, bytes([CMD[0]]), 1)
        got = hid.HidD_GetFeature(hs, rb, LEN)
        gerr = ctypes.get_last_error()
        if got:
            print(f"  [{i}] #{i} SetFeature=ok GetFeature=ok {rb.raw.hex(' ')}", flush=True)
        elif gerr not in (31,):
            print(f"  [{i}] SetFeature={'ok' if ok else err} GetFeature err={gerr}",
                  flush=True)

        # 同时看看有没有输入报告（设备可能把答案推到输入端点）
        d = H._read_timeout(hr, max(il, 128), 120)
        while d:
            if not d.startswith(b"\x4d\x01\x01\x01\x00\x00\x00\x00"):
                print(f"  [{i}] rx {d.hex(' ')}", flush=True)
            d = H._read_timeout(hr, max(il, 128), 60)
finally:
    kernel32.CloseHandle(hs)
    kernel32.CloseHandle(hr)
print("结束", flush=True)
