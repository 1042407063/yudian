"""失败重试一次，验证稳定性。"""
import ctypes
import time
import ctypes.wintypes as wt
import hidprobe as H
from hidprobe import kernel32

t = H.require_one(*H.target_args("37D7:2401", "FFA0:0001"))
hw = H._open(t[10])
hr = H._open(t[10], overlapped=True)
buf = bytearray(33)
buf[0] = 0x00
buf[1:6] = bytes([0x5A, 0xA5, 0x01, 0x02, 0x00])
b = ctypes.create_string_buffer(bytes(buf), 33)

def ask(timeout=450, tries=3):
    for k in range(tries):
        H.drain(hr, 128, 40)
        written = wt.DWORD(0)
        kernel32.WriteFile(hw, b, 33, ctypes.byref(written), None)
        d = H._read_timeout(hr, 128, timeout)
        if d:
            return d, k + 1
        time.sleep(0.05)
    return None, tries

ok = 0
for i in range(6):
    d, n = ask()
    if d and len(d) >= 32 and d[1:3] == b"\x5a\xa5":
        f = d[1:] if d[0] == 0 else d
        ok += 1
        print(f"  第{i+1}次 OK(第{n}次尝试) 电量字节=0x{f[11]:02X} -> {(f[11]&0xF)*20}%")
    elif d:
        print(f"  第{i+1}次 非应答帧 {d[:16].hex(' ')}")
    else:
        print(f"  第{i+1}次 全部失败")
print(f"成功率 {ok}/6")
kernel32.CloseHandle(hw)
kernel32.CloseHandle(hr)
