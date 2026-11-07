"""干净重复测试：只发 00 5A A5 01 02 00，连做 4 次。"""
import ctypes
import time
import ctypes.wintypes as wt
import hidprobe as H
from hidprobe import kernel32

t = H.require_one(*H.target_args("37D7:2401", "FFA0:0001"))
hw = H._open(t[10])
hr = H._open(t[10], overlapped=True)
buf = bytearray(33)
buf[0] = 0x00
buf[1:6] = bytes([0x5A, 0xA5, 0x01, 0x02, 0x00])
b = ctypes.create_string_buffer(bytes(buf), 33)
for i in range(4):
    H.drain(hr, 128, 60)
    written = wt.DWORD(0)
    ok = kernel32.WriteFile(hw, b, 33, ctypes.byref(written), None)
    print(f"\n第{i+1}次 write={'ok' if ok else ctypes.get_last_error()}")
    end = time.time() + 1.2
    got = None
    while time.time() < end:
        d = H._read_timeout(hr, 128, 180)
        if d:
            got = d
            break
    if not got:
        print("   无应答")
        continue
    # 取到的可能是心跳或按键帧，不先认帧头就按固定偏移解析，读到的是别的字段
    # （"隔一次才应答"这个结论就是这样统计出来的，口径不可靠）。
    if len(got) < 32 or got[1:3] != b"\x5a\xa5":
        print("   非应答帧", got[:16].hex(" "))
        continue
    f = got[1:] if got[0] == 0 else got
    cs = sum(f[2:31]) & 0xFF
    print("  ", got.hex(" "))
    print(f"   设备ID=0x{f[5]:02X} 连接={f[6]} 电量字节=0x{f[11]:02X} "
          f"状态={f[11]>>4} 档位={f[11]&0x0F} -> {(f[11]&0x0F)*20}% 校验={'OK' if cs==f[31] else 'BAD'}")
kernel32.CloseHandle(hw)
kernel32.CloseHandle(hr)
