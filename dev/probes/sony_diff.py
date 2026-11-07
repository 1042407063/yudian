"""连续抓几帧，全字节打印，看哪些字节在动。"""
import time
import hidprobe as H
from hidprobe import kernel32

t = H.require_one(*H.target_args("054C:0CE6", "0001:0005"))
h = H._open(t[10], overlapped=True)
frames = []
end = time.time() + 3
while time.time() < end and len(frames) < 4:
    d = H._read_timeout(h, max(t[4], 128), 300)
    if d:
        frames.append(d)
for i, d in enumerate(frames):
    print(f"帧{i} len={len(d)}")
    print("  " + " ".join(f"{b:02X}" for b in d))
kernel32.CloseHandle(h)
