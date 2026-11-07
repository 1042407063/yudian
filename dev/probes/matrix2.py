"""针对 0x4D / 0x13 两种帧头的参数变体。"""
import ctypes
import sys
import hidprobe as H

TARGET, USAGE, LEN = sys.argv[1], sys.argv[2], int(sys.argv[3])
t = H.require_one(TARGET, USAGE)
vid, pid, up, us, il, ol, fl, prod, man, serial, path = t
print(f"=== {vid:04X}:{pid:04X} {prod} len={LEN}")
hw = H._open(path)                    # 同步句柄：只写（SetFeature 也用它）
hr = H._open(path, overlapped=True)   # 异步句柄：只读

CASES = {
    "11|13 4a 01 00": [0x11, 0x13, 0x4a, 0x01, 0x00],
    "11|13 4a 01": [0x11, 0x13, 0x4a, 0x01],
    "11|4d 09 00 00": [0x11, 0x4d, 0x09, 0x00, 0x00],
    "11|4d 09 00 01": [0x11, 0x4d, 0x09, 0x00, 0x01],
    "11|4d 09 00 02": [0x11, 0x4d, 0x09, 0x00, 0x02],
    "11|4d 09 01 00": [0x11, 0x4d, 0x09, 0x01, 0x00],
    "11|4d 0a 05": [0x11, 0x4d, 0x0a, 0x05],
    "11|4d 01 00": [0x11, 0x4d, 0x01, 0x00],
    "11|4d 01 01": [0x11, 0x4d, 0x01, 0x01],
}
for label, head in CASES.items():
    H.drain(hr, max(il, 64), 30)
    f = bytearray(LEN)
    f[:len(head)] = bytes(head)
    ok, info = H._write_timeout(hw, f, 300)
    frames = []
    for _ in range(6):
        d = H._read_timeout(hr, max(il, 128), 250)
        if not d:
            break
        frames.append(d)
    print(f"  {label:20} send={'ok' if ok else info} rx={len(frames)}")
    for d in frames:
        print("      ", d.hex(" "))
H.kernel32.CloseHandle(hw)
H.kernel32.CloseHandle(hr)
