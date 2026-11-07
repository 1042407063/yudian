"""批量探测：对指定厂商集合尝试多种帧格式，打印所有非零回包。"""
import sys
import hidprobe as H
import ctypes
import ctypes.wintypes as wt

VIDPID = sys.argv[1]
USAGE = sys.argv[2]
LEN = int(sys.argv[3])


def frame_compx(total, cs_pos=None):
    """Compx：0x08 0x04 + 填充，校验 = 0x55 - sum(其余字节)"""
    if cs_pos is None:
        cs_pos = total - 1
    f = bytearray(total)
    f[0] = 0x08
    f[1] = 0x04
    s = sum(f[:cs_pos]) & 0xFF
    f[cs_pos] = (0x55 - s) & 0xFF
    return f


def show(data):
    return data.hex(" ")


def try_frame(hw, hr, payload, label, reads=3, timeout=250):
    # 写用同步句柄、读用异步句柄：同一个异步句柄既写又读会让输入管道静默
    # （README 结论第 5 条），那样扫出来的"无回包"不成立。
    ok, info = H._write_timeout(hw, payload, 500)
    err = info
    out = []
    for _ in range(reads):
        d = H._read_timeout(hr, max(LEN, 128), timeout)
        if not d:
            break
        out.append(d)
    print(f"  {label:44} write={'ok' if ok else 'FAIL(%d)' % err} "
          f"replies={len(out)}")
    for d in out:
        if any(d):
            print(f"        -> {show(d)}")
    return out


def main():
    targets = H.find(VIDPID, USAGE)
    if not targets:
        print("未找到接口")
        return
    vid, pid, up, us, il, ol, fl, prod, man, serial, path = targets[0]
    print(f"=== {vid:04X}:{pid:04X} {up:04X}/{us:04X} IN={il} OUT={ol} FEA={fl}")
    print(f"    {prod}")

    # 1) 特性报告：逐个读
    if fl:
        h = H._open(path)
        print(f"\n-- 特性报告 0..7 (len={fl}) --")
        for rid in range(0, 8):
            buf = ctypes.create_string_buffer(fl)
            ctypes.memmove(buf, bytes([rid]), 1)
            if H.hid.HidD_GetFeature(h, buf, fl):
                raw = buf.raw
                nz = any(raw[1:])
                print(f"  id={rid:#04x} {' '.join(f'{b:02x}' for b in raw[:min(fl, 32)])}"
                      f"{'  ...' if fl > 32 else ''}{'' if nz else '  (全零)'}")
            else:
                print(f"  id={rid:#04x} 读取失败 err={ctypes.get_last_error()}")
        H.kernel32.CloseHandle(h)

    # 2) 厂商帧
    print(f"\n-- 写命令尝试 ({LEN} 字节) --")
    hw = H._open(path)                    # 同步句柄：只写
    hr = H._open(path, overlapped=True)   # 异步句柄：只读

    cands = [
        ("Compx 08/04 校验在末字节", frame_compx(LEN)),
        ("Compx 08/04 校验在 [16]", frame_compx(LEN, 16)),
        ("Compx 08/10 型号 校验末字节", (lambda f: (f.__setitem__(1, 0x10), f)[1])(frame_compx(LEN))),
        ("全零", bytearray(LEN)),
        ("0x01 开头", bytearray([1] + [0] * (LEN - 1))),
        ("0x02 开头", bytearray([2] + [0] * (LEN - 1))),
        ("0x03 开头", bytearray([3] + [0] * (LEN - 1))),
        ("0x04 开头", bytearray([4] + [0] * (LEN - 1))),
        ("0x05 开头", bytearray([5] + [0] * (LEN - 1))),
        ("0x08 开头", bytearray([8] + [0] * (LEN - 1))),
        ("0x0A 开头", bytearray([0x0A] + [0] * (LEN - 1))),
        ("0x10 开头", bytearray([0x10] + [0] * (LEN - 1))),
        ("0x11 开头", bytearray([0x11] + [0] * (LEN - 1))),
        ("0x13 开头", bytearray([0x13] + [0] * (LEN - 1))),
        ("0x5A 开头", bytearray([0x5A] + [0] * (LEN - 1))),
        ("0xAA 开头", bytearray([0xAA] + [0] * (LEN - 1))),
        ("0xFF 开头", bytearray([0xFF] + [0] * (LEN - 1))),
    ]
    # 报告 ID 前缀形态：首字节当 report id，第二字节当命令
    for rid in (1, 2, 3, 4):
        for cmd in (0x04, 0x01, 0x02, 0x10):
            f = bytearray(LEN)
            f[0] = rid
            f[1] = cmd
            s = sum(f[:17]) & 0xFF if LEN >= 17 else 0
            f[LEN - 1] = (0x55 - s) & 0xFF
            cands.append((f"rid={rid} cmd={cmd:02x} 0x55校验", f))

    for label, payload in cands:
        try_frame(hw, hr, payload, label, reads=2, timeout=220)

    H.kernel32.CloseHandle(hw)
    H.kernel32.CloseHandle(hr)


main()
