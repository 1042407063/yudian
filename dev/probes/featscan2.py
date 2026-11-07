"""扫描 0x40-0x60 的特性报告 ID。"""
import ctypes
import sys
import hidprobe as H

TARGET, USAGE, LEN = sys.argv[1], sys.argv[2], int(sys.argv[3])
_REST = [a for a in sys.argv[4:] if a != "--yes"]
LO = int(_REST[0], 16) if len(_REST) > 0 else 0x40
HI = int(_REST[1], 16) if len(_REST) > 1 else 0x60
# 与 featscan 一致：默认只读。这个区间正好覆盖 0x4D（迈从的输出报告），
# 对未知 ID 写全零有可能把设备改到别的状态。
DO_SET = "--yes" in sys.argv[4:]
t = H.require_one(TARGET, USAGE)
print(f"=== {TARGET} {USAGE} FEA={t[6]}")
print("    只读模式" if not DO_SET else "    ！会向每个特性 ID 写入全零（--yes）")
h = H._open(t[10])
gets, sets = [], []
for rid in range(LO, HI):
    rb = ctypes.create_string_buffer(LEN)
    ctypes.memmove(rb, bytes([rid]), 1)
    if H.hid.HidD_GetFeature(h, rb, LEN):
        gets.append((rid, rb.raw.hex(" ")))
    if DO_SET:
        sb = ctypes.create_string_buffer(LEN)
        ctypes.memmove(sb, bytes([rid]), 1)
        if H.hid.HidD_SetFeature(h, sb, LEN):
            sets.append(rid)
print("可 Get:", [f"0x{r:02x}" for r, _ in gets])
for r, d in gets:
    print(f"  rid=0x{r:02x} -> {d}")
if DO_SET:
    print("可 Set:", [f"0x{x:02x}" for x in sets])
H.kernel32.CloseHandle(h)
