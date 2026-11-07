"""扫描 0x4D 帧命令字（只读区间）。"""
import sys
import hidprobe as H

TARGET, USAGE, LEN = sys.argv[1], sys.argv[2], int(sys.argv[3])
HIS = [int(x, 16) for x in sys.argv[4].split(",")]
t = H.require_one(TARGET, USAGE)
vid, pid, up, us, il, ol, fl, prod, man, serial, path = t
print(f"=== {vid:04X}:{pid:04X} {prod}")
hw = H._open(path)                    # 同步句柄：只写（SetFeature 也用它）
hr = H._open(path, overlapped=True)   # 异步句柄：只读
for hi in HIS:
    for lo in range(0x10):
        H.drain(hr, max(il, 64), 30)
        f = bytearray(LEN)
        f[0] = 0x11
        f[1] = 0x4d
        f[2] = hi
        f[3] = lo
        ok, info = H._write_timeout(hw, f, 300)
        if not ok:
            continue
        d = H._read_timeout(hr, max(il, 128), 250)
        if d and any(d):
            print(f"  cmd=0x{hi:02x}{lo:02x} -> {d.hex(' ')}")
H.kernel32.CloseHandle(hw)
H.kernel32.CloseHandle(hr)
