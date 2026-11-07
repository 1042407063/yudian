"""用 HidP_GetValueCaps 列出每个报告 ID 的用途与位宽。"""
import ctypes
import ctypes.wintypes as wt
import sys
import hidprobe as H

hid = H.hid
kernel32 = H.kernel32

class HIDP_VALUE_CAPS(ctypes.Structure):
    # 注意 IsRange / IsStringRange / IsDesignatorRange / IsAbsolute 在 hidpi.h 里
    # 是 BOOLEAN（1 字节），不是 USHORT。写成 USHORT 会让结构体从 72 字节涨到 76，
    # 之后 BitSize / ReportCount / LogicalMin 全部错位 4 字节，打印出来的位宽和范围
    # 都是隔壁字段的值。末尾的 assert 就是防止这种偏移再次发生。
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


if ctypes.sizeof(HIDP_VALUE_CAPS) != 72:
    raise SystemExit(
        f"HIDP_VALUE_CAPS 布局错误：{ctypes.sizeof(HIDP_VALUE_CAPS)} 字节，Windows 是 72 字节。"
        "字段类型写错了，继续跑只会打印错位的位宽/范围。")

hid.HidP_GetValueCaps.argtypes = [ctypes.c_int, ctypes.c_void_p,
                                  ctypes.POINTER(wt.USHORT), ctypes.c_void_p]

KIND = {0: "input", 1: "output", 2: "feature"}
HIDP_INPUT, HIDP_OUTPUT, HIDP_FEATURE = 0, 1, 2

for target in H.find(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None):
    vid, pid, up, us, il, ol, fl, prod, man, serial, path = target
    print(f"\n=== {vid:04X}:{pid:04X} {up:04X}/{us:04X} IN={il} OUT={ol} FEA={fl} {prod}")
    h = H._open(path, access=0)
    pp = ctypes.c_void_p()
    if not hid.HidD_GetPreparsedData(h, ctypes.byref(pp)):
        print("  preparsed 失败", ctypes.get_last_error())
        kernel32.CloseHandle(h)          # 原来这里直接 continue，句柄没关
        continue
    try:
        for kind in (HIDP_INPUT, HIDP_OUTPUT, HIDP_FEATURE):
            n = wt.USHORT(0)
            hid.HidP_GetValueCaps(kind, None, ctypes.byref(n), pp)
            if n.value == 0:
                continue
            arr = (HIDP_VALUE_CAPS * n.value)()
            if not hid.HidP_GetValueCaps(kind, ctypes.cast(arr, ctypes.c_void_p),
                                         ctypes.byref(n), pp):
                print(f"  {KIND[kind]}: 枚举失败")
                continue
            for i in range(n.value):
                c = arr[i]
                print(f"  {KIND[kind]:7} rid=0x{c.ReportID:02X} up=0x{c.UsagePage:04X} "
                      f"usage=0x{c.UsageMin:04X}"
                      f"{'-0x%04X' % c.UsageMax if c.IsRange else ''} "
                      f"bits={c.BitSize} count={c.ReportCount} "
                      f"lmin={c.LogicalMin} lmax={c.LogicalMax}")
    finally:
        hid.HidD_FreePreparsedData(pp)
        kernel32.CloseHandle(h)
