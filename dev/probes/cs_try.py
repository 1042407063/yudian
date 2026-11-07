"""尝试不同校验位置/算法，看哪种能引出应答。"""
import ctypes
import sys
import hidprobe as H
from hidprobe import kernel32
import ctypes.wintypes as wt

TARGET, USAGE, LEN = sys.argv[1], sys.argv[2], int(sys.argv[3])
t = H.require_one(TARGET, USAGE)
vid, pid, up, us, il, ol, fl, prod, man, serial, path = t
print(f"=== {vid:04X}:{pid:04X} {prod} len={LEN}")
hw = H._open(path)
hr = H._open(path, overlapped=True)

COMBOS = [0x0900, 0x0101, 0x0a05]
for cmd in COMBOS:
    for label, loom in [("xor[0..62]", (0, 63)),
                        ("xor[1..62]", (1, 63)),
                        ("xor[1..59]@60", (1, 60)),
                        ("xor[2..62]@63", (2, 63)),
                        ("none", None)]:
        H.drain(hr, max(il, 64), 60)
        f = bytearray(LEN)
        f[0] = 0x11
        f[1] = 0x4d
        f[2] = (cmd >> 8) & 0xFF
        f[3] = cmd & 0xFF
        if loom:
            a, b = loom
            cs = 0
            for i in range(a, b):
                cs ^= f[i]
            f[b] = cs
        buf = ctypes.create_string_buffer(bytes(f), LEN)
        written = wt.DWORD(0)
        ok = kernel32.WriteFile(hw, buf, LEN, ctypes.byref(written), None)
        err = ctypes.get_last_error()
        d = H._read_timeout(hr, max(il, 128), 500)
        mark = "  <<< 有回包" if d else ""
        print(f"  cmd=0x{cmd:04x} {label:16} write={'ok' if ok else err} "
              f"cs=0x{(f[63] if loom and loom[1] == 63 else f[60]):02x}{mark}")
        if d:
            print("      ", d.hex(" "))
kernel32.CloseHandle(hw)
kernel32.CloseHandle(hr)
