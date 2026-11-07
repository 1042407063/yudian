"""对指定 report id 逐长度尝试 WriteFile，找出该报告的实际输出长度。"""
import ctypes
import sys
import ctypes.wintypes as wt
import hidprobe as H
from hidprobe import kernel32

TARGET, USAGE = sys.argv[1], sys.argv[2]
RID = int(sys.argv[3], 16)
t = H.require_one(TARGET, USAGE)
path = t[10]
print(f"=== {TARGET} rid=0x{RID:02x}")
hw = H._open(path)
hr = H._open(path, overlapped=True)
for n in list(range(2, 24)) + [32, 40, 56, 64, 65]:
    H.drain(hr, 128, 50)
    buf = ctypes.create_string_buffer(n)
    ctypes.memmove(buf, bytes([RID]), 1)
    written = wt.DWORD(0)
    ok = kernel32.WriteFile(hw, buf, n, ctypes.byref(written), None)
    err = ctypes.get_last_error()
    d = H._read_timeout(hr, 128, 300)
    if ok or err != 87:
        print(f"  len={n:3} -> {'ok' if ok else f'err{err}'}"
              f"{'  rx: ' + d.hex(' ') if d else ''}")
kernel32.CloseHandle(hw)
kernel32.CloseHandle(hr)
