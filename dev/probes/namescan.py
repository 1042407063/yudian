"""扫 MCHOSE V1 命令空间，找返回 ASCII 字符串（型号名）的应答。"""
import ctypes
import time
import ctypes.wintypes as wt
import hidprobe as H
from hidprobe import kernel32

t = H.require_one(*H.target_args("41E4:2001", "FF02:0002"))
hw = H._open(t[10])
hr = H._open(t[10], overlapped=True)
print(f"=== {t[0]:04X}:{t[1]:04X}")

def ask(sub, params=b"", wait=260):
    H.drain(hr, 64, 40)
    buf = bytearray(20)
    buf[0] = 0x13
    buf[1] = sub
    if params:
        buf[2:2 + len(params)] = params
    b = ctypes.create_string_buffer(bytes(buf), 20)
    written = wt.DWORD(0)
    ok = kernel32.WriteFile(hw, b, 20, ctypes.byref(written), None)
    if not ok:
        return None
    d = H._read_timeout(hr, 128, wait)
    return d

seen = {}
for sub in range(0x00, 0x100):
    d = ask(sub)
    if not d:
        continue
    body = d[2:]
    printable = bytes(x for x in body if 32 <= x < 127)
    if len(printable) >= 3:
        print(f"  cmd=0x{sub:02X}: {d.hex(' ')}")
        print(f"      可打印段: {printable.decode('ascii', 'replace')}")
        seen[sub] = d
print("命中", len(seen))
kernel32.CloseHandle(hw)
kernel32.CloseHandle(hr)
