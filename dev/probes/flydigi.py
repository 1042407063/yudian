"""飞智 Vader 5 Pro：在 0xFFA0 上发 5A A5 01 02 00，看应答。"""
import ctypes
import sys
import time
import ctypes.wintypes as wt
import hidprobe as H
from hidprobe import kernel32

t = H.require_one(*H.target_args("37D7:2401", "FFA0:0001"))
print(f"=== {t[0]:04X}:{t[1]:04X} IN={t[4]} OUT={t[5]} FEA={t[6]}")
hw = H._open(t[10])
hr = H._open(t[10], overlapped=True)

FRAME = bytes([0x5A, 0xA5, 0x01, 0x02, 0x00])
for label, head in [("帧在[0]，33字节", FRAME),
                    ("report id 0 + 帧", b"\x00" + FRAME),
                    ("report id 3 + 帧", b"\x03" + FRAME)]:
    H.drain(hr, 128, 50)
    buf = bytearray(33)
    buf[:len(head)] = head
    b = ctypes.create_string_buffer(bytes(buf), 33)
    written = wt.DWORD(0)
    ok = kernel32.WriteFile(hw, b, 33, ctypes.byref(written), None)
    err = ctypes.get_last_error()
    print(f"\n{label}: write={'ok' if ok else err}")
    end = time.time() + 1.5
    while time.time() < end:
        d = H._read_timeout(hr, 128, 200)
        if not d:
            continue
        print("   rx", d.hex(" "))
kernel32.CloseHandle(hw)
kernel32.CloseHandle(hr)
