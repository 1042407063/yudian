"""对同一接口依次尝试多种发送方式与帧布局，打印输入报告。"""
import ctypes
import sys
import hidprobe as H

TARGET, USAGE, LEN = sys.argv[1], sys.argv[2], int(sys.argv[3])
t = H.require_one(TARGET, USAGE)
vid, pid, up, us, il, ol, fl, prod, man, serial, path = t
print(f"=== {vid:04X}:{pid:04X} {up:04X}/{us:04X} IN={il} OUT={ol} FEA={fl} {prod}")
hw = H._open(path)                    # 同步句柄：只写（SetFeature 也用它）
hr = H._open(path, overlapped=True)   # 异步句柄：只读


def pad(head):
    f = bytearray(LEN)
    f[:len(head)] = bytes(head)
    return f


def run(label, mode, frame):
    H.drain(hr, max(il, 64), 40)
    if mode == "feature":
        buf = ctypes.create_string_buffer(LEN)
        ctypes.memmove(buf, bytes(frame), LEN)
        ok = H.hid.HidD_SetFeature(hw, buf, LEN)
        info = ctypes.get_last_error()
    else:
        ok, info = H._write_timeout(hw, frame, 400)
    frames = []
    for _ in range(10):
        d = H._read_timeout(hr, max(il, 128), 200)
        if not d:
            break
        frames.append(d)
    print(f"\n  {label:44} send={'ok' if ok else 'fail(%d)' % info} rx={len(frames)}")
    for d in frames:
        print("      ", d.hex(" "))


# 0x11 输出报告，载荷以 0x4D 帧开头
run("write 11 | 4d 09 00", "write", pad([0x11, 0x4d, 0x09, 0x00]))
run("write 11 | 09 00", "write", pad([0x11, 0x09, 0x00]))
run("write 11 | 4d 01 01", "write", pad([0x11, 0x4d, 0x01, 0x01]))
run("feature 11 | 4d 09 00", "feature", pad([0x11, 0x4d, 0x09, 0x00]))
run("feature 13 | 4a 01 00", "feature", pad([0x13, 0x4a, 0x01, 0x00]))
run("feature 13 | 4a 01 00 (den nibble)", "feature", pad([0x13, 0x4a, 0x01, 0x00, 0x02]))
H.kernel32.CloseHandle(hw)
H.kernel32.CloseHandle(hr)
