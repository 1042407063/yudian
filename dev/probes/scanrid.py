"""扫描 report id：首字节当报告 ID，其余为零，看哪些 ID 被接受、是否有回包。"""
import sys
import hidprobe as H

TARGET, USAGE, LEN = sys.argv[1], sys.argv[2], int(sys.argv[3])
t = H.require_one(TARGET, USAGE)
vid, pid, up, us, il, ol, fl, prod, man, serial, path = t
print(f"=== {vid:04X}:{pid:04X} {prod} {up:04X}/{us:04X} len={LEN}")
hw = H._open(path)                    # 同步句柄：只写（SetFeature 也用它）
hr = H._open(path, overlapped=True)   # 异步句柄：只读
for rid in range(0, 0x30):
    payload = bytearray(LEN)
    payload[0] = rid
    ok, info = H._write_timeout(hw, payload, 200)
    replies = []
    if ok:
        for _ in range(3):
            d = H._read_timeout(hr, max(LEN, 64), 60)
            if not d:
                break
            if any(d):
                replies.append(d)
    status = "写入ok" if ok else f"写入失败({info})"
    if replies:
        for d in replies:
            print(f"  rid=0x{rid:02x} {status} -> {d.hex(' ')}")
    elif ok:
        print(f"  rid=0x{rid:02x} {status} 无回包")
H.kernel32.CloseHandle(hw)
H.kernel32.CloseHandle(hr)
