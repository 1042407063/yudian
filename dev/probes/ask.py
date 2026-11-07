"""干净问答：先排空积压，再写命令，收集所有回包并校验末字节求和。"""
import sys
import hidprobe as H

TARGET, USAGE, LEN = sys.argv[1], sys.argv[2], int(sys.argv[3])
RID = int(sys.argv[4], 16)
SUBS = [int(x, 16) for x in sys.argv[5].split(",")]

t = H.require_one(TARGET, USAGE)
vid, pid, up, us, il, ol, fl, prod, man, serial, path = t
print(f"=== {vid:04X}:{pid:04X} {prod} rid=0x{RID:02X}")
hw = H._open(path)                    # 同步句柄：只写（SetFeature 也用它）
hr = H._open(path, overlapped=True)   # 异步句柄：只读


def drain():
    H.drain(hr, max(LEN, 64), 40)


for sub in SUBS:
    drain()
    payload = bytearray(LEN)
    payload[0] = RID
    payload[1] = sub
    ok, info = H._write_timeout(hw, payload, 300)
    frames = []
    while True:
        d = H._read_timeout(hr, max(LEN, 64), 220)
        if not d:
            break
        frames.append(d)
        if len(frames) >= 4:
            break
    print(f"\n  cmd=0x{sub:02x} write={'ok' if ok else info} frames={len(frames)}")
    for d in frames:
        # ReadFile 返回的是该接口真实报告长度，可能小于 LEN；直接 body[LEN-1]
        # 会在"校验"这一步抛 IndexError，看起来像设备校验不过。
        if len(d) < LEN:
            print(f"    {d.hex(' ')}  短帧 len={len(d)} < {LEN}，跳过校验")
            continue
        body = d[:LEN]
        cs_ok = (sum(body[:LEN - 1]) & 0xFF) == body[LEN - 1]
        print(f"    {d.hex(' ')}  sum_ok={cs_ok}")
H.kernel32.CloseHandle(hw)
H.kernel32.CloseHandle(hr)
