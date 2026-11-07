"""用 0x41/0x42/0x44 读块命令扫寄存器地址，找 ASCII 字符串。"""
import ctypes
import time
import ctypes.wintypes as wt
import hidprobe as H
from hidprobe import kernel32

t = H.require_one(*H.target_args("41E4:2001", "FF02:0002"))
hw = H._open(t[10])
hr = H._open(t[10], overlapped=True)

def ask(cmd, addr, idx=0, wait=200):
    H.drain(hr, 64, 30)
    buf = bytearray(20)
    buf[0], buf[1], buf[2], buf[3] = 0x13, cmd, addr, idx
    b = ctypes.create_string_buffer(bytes(buf), 20)
    written = wt.DWORD(0)
    if not kernel32.WriteFile(hw, b, 20, ctypes.byref(written), None):
        return None
    return H._read_timeout(hr, 128, wait)

hits = 0
for cmd in (0x41, 0x42, 0x44):
    for addr in range(0x00, 0x40):
        d = ask(cmd, addr)
        if not d or d[1] != cmd:
            continue
        body = d[6:18]
        printable = [x for x in body if 32 <= x < 127]
        if len(printable) >= 4:
            hits += 1
            print(f"  cmd=0x{cmd:02X} addr=0x{addr:02X}: {d.hex(' ')}")
            print(f"      -> {bytes(printable).decode('ascii','replace')}")
print("命中", hits)
kernel32.CloseHandle(hw)
kernel32.CloseHandle(hr)
