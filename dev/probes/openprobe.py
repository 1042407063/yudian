"""测试：每次新开接口时，设备是否会主动推送 0x0900 状态帧。"""
import sys
import time
import hidprobe as H
from hidprobe import kernel32

TARGET, USAGE, LEN = sys.argv[1], sys.argv[2], int(sys.argv[3])
ROUNDS = int(sys.argv[4]) if len(sys.argv) > 4 else 3
t = H.require_one(TARGET, USAGE)
il = max(t[4], 128)
print(f"=== {TARGET} {USAGE} 每轮新开接口读 4 秒")
for r in range(ROUNDS):
    time.sleep(6)
    h = H._open(t[10], overlapped=True)
    frames = []
    end = time.time() + 4
    while time.time() < end:
        d = H._read_timeout(h, il, 300)
        if d:
            frames.append(d)
    kernel32.CloseHandle(h)
    status = [f for f in frames if len(f) > 20 and f[:3] == b"\x4d\x01\x01"
              and f[4] == 0x00 and f[5] == 0x09]
    print(f"  第{r+1}轮: 共 {len(frames)} 帧, 0x0900 帧 {len(status)}")
    for f in status:
        n = f[3]
        data = f[8:8 + n]
        print(f"     {f.hex(' ')}")
        if n >= 13:
            print(f"     VID=0x{data[1]:02x}{data[0]:02x} PID=0x{data[3]:02x}{data[2]:02x} "
                  f"连接={data[10]} 充电={data[11]} 电量={data[12]}%")
