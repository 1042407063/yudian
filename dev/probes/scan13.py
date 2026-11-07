"""扫描 report id 0x13 的子命令空间。"""
import ctypes, sys, time
import hidprobe as H

TARGET = sys.argv[1]
USAGE = sys.argv[2]
LEN = int(sys.argv[3])
LO = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0
HI = int(sys.argv[5], 16) if len(sys.argv) > 5 else 0x40

t = H.require_one(TARGET, USAGE)
vid, pid, up, us, il, ol, fl, prod, man, serial, path = t
print(f"=== {vid:04X}:{pid:04X} {prod}  rid=0x13 子命令 0x{LO:02x}..0x{HI:02x}")
hw = H._open(path)                    # 同步句柄：只写（SetFeature 也用它）
hr = H._open(path, overlapped=True)   # 异步句柄：只读
hits = 0
for sub in range(LO, HI):
    payload = bytearray(LEN)
    payload[0] = 0x13
    payload[1] = sub
    ok, info = H._write_timeout(hw, payload, 200)
    if not ok:
        print(f"  sub=0x{sub:02x} 写入失败 {info}")
        continue
    n = 0
    for _ in range(3):
        d = H._read_timeout(hr, max(LEN, 64), 60)
        if not d:
            break
        if any(d):
            n += 1
            print(f"  sub=0x{sub:02x} -> {d.hex(' ')}")
            hits += 1
print("命中", hits)
H.kernel32.CloseHandle(hw)
H.kernel32.CloseHandle(hr)
