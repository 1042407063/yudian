"""找出 DualSense 输出报告 0x31 / 0x02 的合法长度。"""
import ctypes
import ctypes.wintypes as wt
import hidprobe as H
from hidprobe import kernel32

t = H.require_one(*H.target_args("054C:0CE6", "0001:0005"))
hw = H._open(t[10])
for rid in (0x31, 0x02, 0x05):
    for n in (77, 78, 79, 80, 100, 142, 288, 547):
        buf = bytearray(n)
        buf[0] = rid
        b = ctypes.create_string_buffer(bytes(buf), n)
        written = wt.DWORD(0)
        ok = kernel32.WriteFile(hw, b, n, ctypes.byref(written), None)
        err = ctypes.get_last_error()
        if ok or err != 87:
            print(f"  rid=0x{rid:02X} len={n} -> {'ok' if ok else 'err %d' % err}")
kernel32.CloseHandle(hw)
