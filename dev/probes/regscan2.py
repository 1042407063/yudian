"""0x41 读块命令扫 0x40-0xFF 地址，找 ASCII。"""
import ctypes
import ctypes.wintypes as wt
import hidprobe as H
from hidprobe import kernel32

t = H.require_one(*H.target_args("41E4:2001", "FF02:0002"))
hw = H._open(t[10])
hr = H._open(t[10], overlapped=True)

def ask(addr, idx=0, wait=180):
    H.drain(hr, 64, 30)
    buf = bytearray(20)
    buf[0], buf[1], buf[2], buf[3] = 0x13, 0x41, addr, idx
    b = ctypes.create_string_buffer(bytes(buf), 20)
    written = wt.DWORD(0)
    if not kernel32.WriteFile(hw, b, 20, ctypes.byref(written), None):
        return None
    return H._read_timeout(hr, 128, wait)

hits = 0
for addr in range(0x40, 0x100):
    d = ask(addr)
    if not d or d[1] != 0x41:
        continue
    body = d[6:18]
    printable = [x for x in body if 32 <= x < 127]
    if len(printable) >= 4:
        hits += 1
        print(f"  addr=0x{addr:02X}: {d.hex(' ')} -> {bytes(printable).decode('ascii','replace')}")
print("命中", hits)
kernel32.CloseHandle(hw)
kernel32.CloseHandle(hr)
