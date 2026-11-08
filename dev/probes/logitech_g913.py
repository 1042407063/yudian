"""罗技 LIGHTSPEED 接收器（046D:C547 / G913 TKL）HID++ 电量协议实测。

只读：仅发 ROOT/GetFeature、FeatureSet/枚举、特性 func0 这类读取命令。
不发 pairing、不写配置、不刷固件。

    python logitech_g913.py            # 全部集合 × 全部通道
    python logitech_g913.py 046D:C547
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

import hidprobe as hp

kernel32 = hp.kernel32


class Link:
    """一条 HID++ 链路：同步句柄写、异步句柄读（见 probes/README.md 的坑）。"""

    def __init__(self, row, long_mode=None):
        vid, pid, up, us, il, ol, fl, prod, man, serial, self.path = row
        self.up, self.us, self.il = up, us, il
        self.long = (il >= 20) if long_mode is None else long_mode
        self.len = 20 if self.long else 7
        self.swid = 0
        self.hs = hp._open(self.path)                  # 同步，只写
        self.hr = hp._open(self.path, overlapped=True)  # 异步，只读

    def close(self):
        kernel32.CloseHandle(self.hr)
        kernel32.CloseHandle(self.hs)

    def drain(self):
        for _ in range(8):
            if not hp._read_timeout(self.hr, max(self.il, 64), 1):
                break

    def request(self, idx, feature, func, params=b"", timeout=350):
        self.swid = (self.swid + 1) % 15 + 1
        swid = self.swid
        pkt = bytearray(self.len)
        pkt[0] = 0x11 if self.long else 0x10
        pkt[1] = idx
        pkt[2] = feature
        pkt[3] = (func << 4) | swid
        for i, b in enumerate(params[:self.len - 4]):
            pkt[4 + i] = b
        self.drain()
        buf = ctypes.create_string_buffer(bytes(pkt), len(pkt))
        written = wt.DWORD(0)
        ok = kernel32.WriteFile(self.hs, buf, len(pkt), ctypes.byref(written), None)
        if not ok:
            return None, "write-failed-%d" % ctypes.get_last_error()
        end = time.time() + timeout / 1000.0
        while time.time() < end:
            data = hp._read_timeout(self.hr, max(self.il, 64), 80)
            if not data or len(data) < 4:
                continue
            if data[0] not in (0x10, 0x11):
                continue
            if data[1] not in (idx, 0xFF):
                continue
            if data[2] in (0xFF, 0x8F):
                return None, "error-0x%02X" % (data[5] if len(data) > 5 else 0)
            if data[2] == feature and data[3] == ((func << 4) | swid):
                return data, "ok"
        return None, "timeout"

    def feature_index(self, idx, fid, big_endian=False):
        p = bytes([fid >> 8, fid & 0xFF]) if big_endian else bytes([fid & 0xFF, fid >> 8])
        r, st = self.request(idx, 0x00, 0x00, p)
        if r is None or len(r) < 5 or r[4] == 0:
            return 0
        return r[4]

    def feature_set(self, idx):
        """FEATURE SET(0x0001)：返回设备支持的全部特性 ID。"""
        fi = self.feature_index(idx, 0x0001)
        if not fi:
            return []
        head, _ = self.request(idx, fi, 0x00)
        if head is None:
            return []
        count = head[4]
        out = []
        for i in range(min(count, 64)):
            r, _ = self.request(idx, fi, 0x01, bytes([i]))
            if r is not None and len(r) >= 6:
                out.append(((r[4] << 8) | r[5], r[6] if len(r) > 6 else 0, r.hex(" ")))
        return out


def hx(r):
    return "-" if r is None else r.hex(" ")


def main():
    vidpid = sys.argv[1] if len(sys.argv) > 1 else "046D:C547"
    rows = [r for r in hp.enum_rows() if r[2] == 0xFF00 and r[3] in (1, 2)
            and "%04X:%04X" % (r[0], r[1]) == vidpid.upper()]
    if not rows:
        raise SystemExit("没找到 %s 的 HID++ 集合，先跑 python hidprobe.py enum" % vidpid)
    rows.sort(key=lambda r: -r[3])   # 长报文优先

    for row in rows:
        link = Link(row)
        print("\n=== %04X:%04X %04X/%04X IN=%d OUT=%d  %s ===" %
              (row[0], row[1], row[2], row[3], row[4], row[5], row[10]))
        for idx in (1, 2, 3, 4, 5, 6, 0xFF):
            ping, st = link.request(idx, 0x00, 0x01)
            print("  idx=0x%02X ping=%-14s %s" % (idx, st, hx(ping)))
            if ping is None:
                continue
            feats = link.feature_set(idx)
            if feats:
                print("    特性表: " + ", ".join("0x%04X" % f[0] for f in feats))
            for fid in (0x1000, 0x1001, 0x1004, 0x0005, 0x1F20):
                for be in (False, True):
                    fi = link.feature_index(idx, fid, be)
                    if not fi:
                        continue
                    r, s = link.request(idx, fi, 0x00)
                    print("    feat 0x%04X [%s] idx=%d func0 -> %-10s %s" %
                          (fid, "大端" if be else "小端", fi, s, hx(r)))
                    break
        link.close()


if __name__ == "__main__":
    main()
