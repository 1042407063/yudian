"""干净探测：同步写句柄 + 异步读句柄，避免 CancelIo 污染管道。"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

import hidprobe as H
from hidprobe import hid, kernel32


def open_rw(path):
    return H._open(path), H._open(path, overlapped=True)


def drain(hr, length, ms=150):
    n = 0
    while H._read_timeout(hr, length, ms):
        n += 1
        if n > 64:
            break
    return n


def collect(hr, length, ms, maxn=8):
    out = []
    while len(out) < maxn:
        d = H._read_timeout(hr, length, ms)
        if not d:
            break
        out.append(d)
    return out


def main():
    target, usage, length = sys.argv[1], sys.argv[2], int(sys.argv[3])
    t = H.require_one(target, usage)
    vid, pid, up, us, il, ol, fl, prod, man, serial, path = t
    print(f"=== {vid:04X}:{pid:04X} {up:04X}/{us:04X} IN={il} OUT={ol} FEA={fl} {prod}")
    hw, hr = open_rw(path)
    print("句柄 ok:", bool(hw), bool(hr))

    print("\n-- 被动监听 4 秒（不发送）--")
    frames = collect(hr, max(il, 64), 400, maxn=10)
    print(f"  收到 {len(frames)} 条")
    for d in frames[:5]:
        print("   ", d.hex(" "))

    def send(label, payload, wait=900):
        drain(hr, max(il, 64), 120)
        if label.startswith("F:"):
            label = label[2:]
            buf = ctypes.create_string_buffer(len(payload))
            ctypes.memmove(buf, bytes(payload), len(payload))
            ok = hid.HidD_SetFeature(hw, buf, len(payload))
            err = ctypes.get_last_error()
        else:
            written = wt.DWORD(0)
            buf = ctypes.create_string_buffer(bytes(payload), len(payload))
            ok = kernel32.WriteFile(hw, buf, len(payload), ctypes.byref(written), None)
            err = ctypes.get_last_error()
        frames = collect(hr, max(il, 64), wait, maxn=6)
        print(f"\n  {label:26} write={'ok' if ok else f'FAIL({err})'} rx={len(frames)}")
        for d in frames:
            print("      ", d.hex(" "))

    for arg in sys.argv[4:]:
        feat = arg.startswith("F:")
        head = bytes.fromhex(arg[2:] if feat else arg)
        payload = bytearray(length)
        payload[:len(head)] = head
        send(("F:" + arg[2:] if feat else arg), payload)

    kernel32.CloseHandle(hw)
    kernel32.CloseHandle(hr)


main()
