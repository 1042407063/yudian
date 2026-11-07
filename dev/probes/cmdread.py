"""SetFeature/Write 之后监听输入报告。"""
import ctypes
import sys
import time
import hidprobe as H

TARGET, USAGE, LEN = sys.argv[1], sys.argv[2], int(sys.argv[3])
HEX = sys.argv[4].replace(" ", "")
MODE = sys.argv[5] if len(sys.argv) > 5 else "feature"
t = H.require_one(TARGET, USAGE)
vid, pid, up, us, il, ol, fl, prod, man, serial, path = t
print(f"=== {vid:04X}:{pid:04X} {up:04X}/{us:04X} IN={il} OUT={ol} FEA={fl} {prod}")
hw = H._open(path)                    # 同步句柄：只写（SetFeature 也用它）
hr = H._open(path, overlapped=True)   # 异步句柄：只读
payload = bytes.fromhex(HEX)

# 排空
H.drain(hr, max(il, 64), 40)

if MODE == "feature":
    buf = ctypes.create_string_buffer(LEN)
    ctypes.memmove(buf, payload, min(len(payload), LEN))
    ok = H.hid.HidD_SetFeature(hw, buf, LEN)
    print("SetFeature ->", bool(ok), "err", ctypes.get_last_error())
else:
    ok, info = H._write_timeout(hw, payload, 400)
    print("Write ->", ok, info)

frames = []
for _ in range(8):
    d = H._read_timeout(hr, max(il, 128), 300)
    if not d:
        break
    frames.append(d)
print(f"输入报告 {len(frames)} 条")
for d in frames:
    print("   ", d.hex(" "))
H.kernel32.CloseHandle(hw)
H.kernel32.CloseHandle(hr)
