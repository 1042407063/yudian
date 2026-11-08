"""从 G HUB 安装资源里提取罗技设备的官方 PowerModel 曲线，生成 C++ 头文件。

G HUB 把每个型号的电池模型放在
    C:\\ProgramData\\LGHUB\\depots\\<build>\\<device_dir>\\battery.xml
例如 G913 TKL / G915 TKL 是 g915_tkl_us（G913 是中国型号名，硬件同 G915 TKL），
文件里 <powermodel pid="0xc343"> 的 <discharge> / <charging> 就是 G HUB 自己用的表。

只读 G HUB 的安装资源，不碰设备、不改 G HUB 配置。
换电池、换固件或 G HUB 大版本更新后重跑一次即可。

    python extract_ghub_powermodel.py                 # 自动找 G913/G915 TKL
    python extract_ghub_powermodel.py <battery.xml> <输出.h>
"""
import glob
import os
import re
import sys

DEFAULT_OUT = os.path.join(os.path.dirname(__file__), "..", "..",
                           "app", "src", "logitech_powermodel.h")


def find_battery_xml():
    pats = [
        r"C:\ProgramData\LGHUB\depots\*\g915_tkl_*\battery.xml",
        r"C:\ProgramData\LGHUB\depots\*\g913*\battery.xml",
    ]
    hits = []
    for p in pats:
        hits.extend(glob.glob(p))
    if not hits:
        raise SystemExit("没找到 G HUB 的 battery.xml。先确认 G HUB 已安装，"
                         "并让它在设备管理器里识别过一次键盘。")
    hits.sort(key=os.path.getmtime, reverse=True)
    return hits[0]


def cdata(xml, tag):
    m = re.search(r"<%s[^>]*>(.*?)</%s>" % (tag, tag), xml, re.S)
    if not m:
        raise SystemExit("battery.xml 里没有 <%s> 段" % tag)
    return m.group(1)


def parse(txt, n):
    out = []
    for line in txt.splitlines():
        line = line.strip()
        if not line or line.startswith("<") or "]]>" in line:
            continue
        parts = [x.strip() for x in line.split(",")]
        if len(parts) != n:
            continue
        try:
            out.append(tuple(float(x) for x in parts))
        except ValueError:
            pass
    return out


def compress(arr):
    """连续相同的百分比压成区间端点，查表用线性插值不会失真。"""
    out = []
    for mv, pct in arr:
        if out and out[-1][1] == pct:
            if len(out) >= 2 and out[-2][1] == pct:
                out[-1] = (mv, pct)
            else:
                out.append((mv, pct))
        else:
            out.append((mv, pct))
    return out


def fmt(name, arr, comment):
    lines = [comment, "constexpr LogiCurvePoint %s[] = {" % name]
    for i in range(0, len(arr), 8):
        lines.append("    " + ", ".join("{%d,%d}" % x for x in arr[i:i + 8]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else find_battery_xml()
    dst = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_OUT
    print("source:", src)

    xml = open(src, encoding="utf-8").read()
    pid = re.search(r'<powermodel[^>]*pid="(0x[0-9a-fA-F]+)"', xml)
    pid = pid.group(1) if pid else "?"

    dp = parse(cdata(xml, "discharge"), 3)
    if not dp:
        raise SystemExit("discharge 段没有数据")
    total = max(x[2] for x in dp)
    mono = []
    for _t, v, m in dp:
        mv = round(v * 1000)
        pct = round((1 - m / total) * 100 * 10)
        if not mono or mv < mono[-1][0]:
            mono.append((mv, pct))       # 平台段电压会回落，只取严格递减子集

    cp = [(round(v * 1000), round(soc * 10)) for v, soc in parse(cdata(xml, "charging"), 2)]
    if not cp:
        raise SystemExit("charging 段没有数据")

    mono, cp = compress(mono), compress(cp)
    hdr = """// logitech_powermodel.h —— 罗技 G913 TKL / G915 TKL 的**官方**电量曲线
//
// 来源：G HUB 安装包内设备资源
//   %s
//   <powermodel pid="%s">  (Harpy Wireless)
//
// 这是 G HUB 自己用的 PowerModel，不是社区猜测：
//   <discharge> 每行「时间(min), CCV(V), 累计放电(mWh)」，总容量 %d mWh；
//     这里已换算成「电压(mV) -> 剩余容量(0.1%%)」。
//   <charging> 每行「CCV(V), SoC(%%))」，官方充电曲线只到 4.20V = 91%%；
//     剩余 9%% 由 <charging_time_slots equation_coefficiency="150"> 推进：
//     电压稳定在 4100mV 以上后，每 150 秒涨 1%%。
//
// 连续相同的百分比已压缩成区间端点，查表用线性插值。
// 只读常量，不依赖 G HUB 运行；G HUB 升级后 depot 号会变，但出厂曲线不变，
// 换电池或重新标定后用 extract_ghub_powermodel.py 重新生成即可。
#pragma once
#include <cstdint>

struct LogiCurvePoint {
    int16_t mv;      // 电压，毫伏
    int16_t pct10;   // 百分比 x10（0..1000），保留一位小数
};

""" % (src, pid, int(total))
    body = fmt("kLogiDischargeCurve", mono,
               "// 放电：电压降序。官方曲线在平台段有回落，已按时间顺序取严格递减子集。")
    body += "\n\n" + fmt("kLogiChargingCurve", cp,
                         "// 充电：电压升序。4.20V 以上官方不再给值，由涓流时间推进。")
    open(dst, "w", encoding="utf-8").write(hdr + body + "\n")
    print("wrote:", os.path.abspath(dst))
    print("discharge points=%d (%d..%d mV)  charging points=%d (%d..%d mV, max %d%%)" %
          (len(mono), mono[0][0], mono[-1][0], len(cp), cp[0][0], cp[-1][0], cp[-1][1] // 10))


if __name__ == "__main__":
    main()
