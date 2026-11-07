"""扫描所有 HID 接口，判断哪些设备暴露了「标准电量字段」。

用途：新设备插上后，先跑这个，回答一个二选一的问题——
  * 有 0x06/0x20（Generic Device Controls / Battery Strength）→ 按标准写法就能读；
  * 没有 → 要么走厂商私有协议（得另做），要么硬件根本不上报（软件无解）。

为什么不用报表描述符：Windows 用户态拿不到原始描述符（见 docs 里的 N17），
而 HidP_GetValueCaps 由系统解析好再交给我们，字段名、位宽、取值范围都是现成的。

只读：不写任何特性报表，不改变设备状态。

用法：
    python battery_scan.py              # 扫全部接口
    python battery_scan.py 24AE:xxxx    # 只看某个 VID:PID
"""
import ctypes
import ctypes.wintypes as wt
import sys

import hidprobe as H

hid = H.hid
kernel32 = H.kernel32

HIDP_INPUT, HIDP_OUTPUT, HIDP_FEATURE = 0, 1, 2
KIND = {HIDP_INPUT: "input", HIDP_OUTPUT: "output", HIDP_FEATURE: "feature"}

# 电量相关的用途页/用途
UP_GEN_DEVICE_CONTROLS = 0x06
USAGE_BATTERY_STRENGTH = 0x20
# 有些设备把电量放在厂商页里（0xFF00 起），先只报告出来，不猜含义
def is_vendor_page(up):
    return 0xFF00 <= up <= 0xFFFF


class HIDP_VALUE_CAPS(ctypes.Structure):
    # BOOLEAN 是 1 字节，写成 USHORT 会让整个结构错位（见 caps.py 的教训）
    _fields_ = [("UsagePage", wt.USHORT), ("ReportID", ctypes.c_ubyte),
                ("IsAlias", ctypes.c_ubyte), ("BitField", wt.USHORT),
                ("LinkCollection", wt.USHORT), ("LinkUsage", wt.USHORT),
                ("LinkUsagePage", wt.USHORT), ("IsRange", ctypes.c_ubyte),
                ("IsStringRange", ctypes.c_ubyte), ("IsDesignatorRange", ctypes.c_ubyte),
                ("IsAbsolute", ctypes.c_ubyte), ("HasNull", ctypes.c_ubyte),
                ("Reserved", ctypes.c_ubyte), ("BitSize", wt.USHORT),
                ("ReportCount", wt.USHORT), ("Reserved2", wt.USHORT * 5),
                ("UnitsExp", wt.ULONG), ("Units", wt.ULONG),
                ("LogicalMin", ctypes.c_long), ("LogicalMax", ctypes.c_long),
                ("PhysicalMin", ctypes.c_long), ("PhysicalMax", ctypes.c_long),
                ("UsageMin", wt.USHORT), ("UsageMax", wt.USHORT),
                ("StringMin", wt.USHORT), ("StringMax", wt.USHORT),
                ("DesignatorMin", wt.USHORT), ("DesignatorMax", wt.USHORT),
                ("DataIndexMin", wt.USHORT), ("DataIndexMax", wt.USHORT)]


# 结构体直接用 hidprobe 里那份：两边各定义一个同名类型会互相覆盖 argtypes，
# 报「expected LP_HIDP_CAPS instance」。这里只做尺寸自检，不再另立一套。
HIDP_CAPS = H.HIDP_CAPS


if ctypes.sizeof(HIDP_VALUE_CAPS) != 72:
    raise SystemExit(f"HIDP_VALUE_CAPS 布局错误：{ctypes.sizeof(HIDP_VALUE_CAPS)} != 72")
if ctypes.sizeof(HIDP_CAPS) != 64:
    raise SystemExit(f"HIDP_CAPS 布局错误：{ctypes.sizeof(HIDP_CAPS)} != 64")

# HidP_GetCaps 的 argtypes 已由 hidprobe 绑定，不要覆盖
hid.HidP_GetValueCaps.argtypes = [ctypes.c_int, ctypes.c_void_p,
                                  ctypes.POINTER(wt.USHORT), ctypes.c_void_p]
hid.HidP_GetValueCaps.restype = ctypes.c_long


def value_caps(pp, kind):
    """返回该报告类型下的全部 Value Caps。"""
    n = wt.USHORT(0)
    hid.HidP_GetValueCaps(kind, None, ctypes.byref(n), pp)
    if n.value == 0:
        return []
    arr = (HIDP_VALUE_CAPS * n.value)()
    if not hid.HidP_GetValueCaps(kind, ctypes.cast(arr, ctypes.c_void_p),
                                 ctypes.byref(n), pp):
        return []
    return [arr[i] for i in range(n.value)]


def scan_one(row):
    vid, pid, up, us, il, ol, fl, prod, man, serial, path = row
    tag = f"{vid:04X}:{pid:04X} {up:04X}/{us:04X} IN={il} OUT={ol} FEA={fl}"
    try:
        h = H._open(path, access=0)
    except OSError as e:
        print(f"  {tag}  打开失败：{e}")
        return None
    pp = ctypes.c_void_p()
    try:
        if not hid.HidD_GetPreparsedData(h, ctypes.byref(pp)):
            print(f"  {tag}  preparsed 失败")
            return None
        caps = HIDP_CAPS()
        if hid.HidP_GetCaps(pp, ctypes.byref(caps)) < 0:
            print(f"  {tag}  HidP_GetCaps 失败")
            return None

        found_battery = []
        vendor = []
        for kind in (HIDP_FEATURE, HIDP_INPUT):
            for c in value_caps(pp, kind):
                if c.UsagePage == UP_GEN_DEVICE_CONTROLS and (
                        c.UsageMin == USAGE_BATTERY_STRENGTH or
                        (c.IsRange and c.UsageMin <= USAGE_BATTERY_STRENGTH <= c.UsageMax)):
                    found_battery.append((kind, c))
                elif is_vendor_page(c.UsagePage):
                    vendor.append((kind, c))

        if found_battery:
            print(f"  {tag}  {prod}")
            for kind, c in found_battery:
                print(f"      ★ 标准电量字段：{KIND[kind]} rid=0x{c.ReportID:02X} "
                      f"bits={c.BitSize} count={c.ReportCount} "
                      f"范围 {c.LogicalMin}..{c.LogicalMax}")
            return "battery"
        if vendor:
            pages = sorted({f"0x{c.UsagePage:04X}" for _, c in vendor})
            print(f"  {tag}  {prod}")
            print(f"      无标准电量，但有厂商页 {', '.join(pages)}（可能要私有协议）")
            return "vendor"
        return None
    finally:
        if pp:
            hid.HidD_FreePreparsedData(pp)
        kernel32.CloseHandle(h)


def main():
    want = sys.argv[1].upper() if len(sys.argv) > 1 else None
    rows = H.enum_rows()
    print(f"共 {len(rows)} 个 HID 接口，逐个检查电量字段…\n")
    stats = {"battery": [], "vendor": [], "none": 0}
    for row in rows:
        vid, pid = row[0], row[1]
        if want:
            try:
                wv, wp = want.split(":")
                if vid != int(wv, 16) or pid != int(wp, 16):
                    continue
            except ValueError:
                pass
        r = scan_one(row)
        if r == "battery":
            stats["battery"].append(f"{vid:04X}:{pid:04X} {row[7]}")
        elif r == "vendor":
            stats["vendor"].append(f"{vid:04X}:{pid:04X} {row[7]}")
        else:
            stats["none"] += 1

    print("\n--- 结论 ---")
    if stats["battery"]:
        print(f"有标准电量字段的接口 {len(stats['battery'])} 个：")
        for s in stats["battery"]:
            print("   ", s)
        print("→ 这类设备可以按标准写法读电量，不需要厂商协议。")
    else:
        print("没有发现任何接口暴露标准电量字段。")
        if stats["vendor"]:
            print(f"有 {len(stats['vendor'])} 个接口带厂商页，但那需要逐型号逆向，不能通用。")
        else:
            print("也没有厂商页 —— 这类设备通常根本没有电量采样电路，软件读不到。")
    print(f"（其余 {stats['none']} 个接口既无电量字段也无厂商页）")


main()
