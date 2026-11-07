"""把 BMP 的指定区域放大保存，用于检查图标这类小尺寸细节。

用法：
    python zoom.py in.bmp out.bmp x y w h scale
"""
import ctypes
import ctypes.wintypes as wt
import sys

import shot_panel as sp


def read_bmp(path):
    """读未压缩 BMP，返回 (宽, 高, 每行字节, bpp, 行列表，自上而下)。"""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 54 or data[:2] != b"BM":
        raise SystemExit(f"{path} 不是 BMP 文件")
    off = int.from_bytes(data[10:14], "little")
    w = int.from_bytes(data[18:22], "little", signed=True)
    h = int.from_bytes(data[22:26], "little", signed=True)
    bpp = int.from_bytes(data[28:30], "little")
    comp = int.from_bytes(data[30:34], "little")
    if comp != 0:
        raise SystemExit(f"只支持未压缩 BMP（compression={comp}）")
    if bpp not in (24, 32):
        raise SystemExit(f"只支持 24/32bpp，当前 {bpp}bpp")
    top_down = h < 0
    h = abs(h)
    stride = ((w * bpp + 31) // 32) * 4          # 行按 4 字节对齐，不是 w*bpp
    px = data[off:]
    if len(px) < stride * h:
        raise SystemExit(f"像素数据不足：需要 {stride*h} 字节，实际 {len(px)}")
    rows = [px[i * stride:(i + 1) * stride] for i in range(h)]
    if not top_down:
        rows.reverse()
    return w, h, stride, bpp, rows


def pixel(row, xx, bpp):
    """取一个像素并统一成 BGRA（write_bmp 只写 32bpp）。"""
    if bpp == 32:
        return row[xx * 4:xx * 4 + 4]
    i = xx * 3
    b, g, r = row[i], row[i + 1], row[i + 2]
    return bytes((b, g, r, 255))


def main():
    src, dst = sys.argv[1], sys.argv[2]
    x, y, cw, ch, k = (int(v) for v in sys.argv[3:8])
    w, h, _stride, bpp, rows = read_bmp(src)
    x = max(0, min(x, w - 1))
    y = max(0, min(y, h - 1))
    cw = min(cw, w - x)
    ch = min(ch, h - y)
    out_rows = []
    for yy in range(y, y + ch):
        row = rows[yy]
        line = b"".join(pixel(row, xx, bpp) * k for xx in range(x, x + cw))
        out_rows.extend([line] * k)
    sp.write_bmp(dst, cw * k, ch * k, b"".join(out_rows))
    print(f"{cw}x{ch} x{k} ({bpp}bpp -> 32bpp) -> {dst}")


if __name__ == "__main__":
    main()
