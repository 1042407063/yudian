"""列出被接受的 report id（写入成功即视为该 ID 有效）。"""
import sys
import hidprobe as H

TARGET, USAGE, LEN = sys.argv[1], sys.argv[2], int(sys.argv[3])
t = H.require_one(TARGET, USAGE)
vid, pid, up, us, il, ol, fl, prod, man, serial, path = t
print(f"=== {vid:04X}:{pid:04X} {prod} {up:04X}/{us:04X} len={LEN}")
hw = H._open(path)                    # 同步句柄：只写（SetFeature 也用它）
hr = H._open(path, overlapped=True)   # 异步句柄：只读
ok_ids, fail = [], {}
for rid in range(0, 0x100):
    payload = bytearray(LEN)
    payload[0] = rid
    ok, info = H._write_timeout(hw, payload, 120)
    if ok:
        ok_ids.append(rid)
    else:
        fail[info] = fail.get(info, 0) + 1
print("写入被接受:", " ".join(f"{x:02x}" for x in ok_ids))
print("失败统计:", fail)
H.kernel32.CloseHandle(hw)
H.kernel32.CloseHandle(hr)
