"""HID 探测工具：枚举接口、监听输入报告、发送厂商命令并打印原始回包。

用法:
    python hidprobe.py enum
    python hidprobe.py listen 41E4:2001 5
    python hidprobe.py send 41E4:2001 FF02:0002 08040000000000000000000000000000000000
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

setupapi = ctypes.WinDLL("setupapi", use_last_error=True)
hid = ctypes.WinDLL("hid", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

DIGCF_PRESENT = 0x02
DIGCF_DEVICEINTERFACE = 0x10
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 0x00000001
FILE_SHARE_WRITE = 0x00000002
OPEN_EXISTING = 3
FILE_FLAG_OVERLAPPED = 0x40000000
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
ERROR_IO_PENDING = 997
WAIT_OBJECT_0 = 0


class GUID(ctypes.Structure):
    _fields_ = [("Data1", wt.DWORD), ("Data2", wt.WORD),
                ("Data3", wt.WORD), ("Data4", ctypes.c_ubyte * 8)]


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD), ("InterfaceClassGuid", GUID),
                ("Flags", wt.DWORD), ("Reserved", ctypes.POINTER(wt.ULONG))]


class HIDD_ATTRIBUTES(ctypes.Structure):
    _fields_ = [("Size", wt.ULONG), ("VendorID", wt.USHORT),
                ("ProductID", wt.USHORT), ("VersionNumber", wt.USHORT)]


class HIDP_CAPS(ctypes.Structure):
    _fields_ = [("Usage", wt.USHORT), ("UsagePage", wt.USHORT),
                ("InputReportByteLength", wt.USHORT),
                ("OutputReportByteLength", wt.USHORT),
                ("FeatureReportByteLength", wt.USHORT),
                ("Reserved", wt.USHORT * 17),
                ("NumberLinkCollectionNodes", wt.USHORT),
                ("NumberInputButtonCaps", wt.USHORT),
                ("NumberInputValueCaps", wt.USHORT),
                ("NumberInputDataIndices", wt.USHORT),
                ("NumberOutputButtonCaps", wt.USHORT),
                ("NumberOutputValueCaps", wt.USHORT),
                ("NumberOutputDataIndices", wt.USHORT),
                ("NumberFeatureButtonCaps", wt.USHORT),
                ("NumberFeatureValueCaps", wt.USHORT),
                ("NumberFeatureDataIndices", wt.USHORT)]


class OVERLAPPED(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_void_p), ("InternalHigh", ctypes.c_void_p),
                ("Offset", wt.DWORD), ("OffsetHigh", wt.DWORD),
                ("hEvent", wt.HANDLE)]


hid.HidD_GetAttributes.argtypes = [wt.HANDLE, ctypes.POINTER(HIDD_ATTRIBUTES)]
hid.HidD_GetPreparsedData.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_void_p)]
hid.HidD_GetProductString.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.ULONG]
hid.HidD_GetManufacturerString.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.ULONG]
hid.HidD_FreePreparsedData.argtypes = [ctypes.c_void_p]
hid.HidP_GetCaps.argtypes = [ctypes.c_void_p, ctypes.POINTER(HIDP_CAPS)]
hid.HidD_GetFeature.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.ULONG]
hid.HidD_SetFeature.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.ULONG]
hid.HidD_GetSerialNumberString.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.ULONG]

setupapi.SetupDiGetClassDevsW.restype = wt.HANDLE
setupapi.SetupDiEnumDeviceInterfaces.argtypes = [
    wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(GUID), wt.DWORD,
    ctypes.POINTER(SP_DEVICE_INTERFACE_DATA)]
setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [
    wt.HANDLE, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA), ctypes.c_void_p,
    wt.DWORD, ctypes.POINTER(wt.DWORD), ctypes.c_void_p]
# HDEVINFO 不是普通内核句柄：必须用 SetupDiDestroyDeviceInfoList 释放，
# CloseHandle 会失败并泄漏设备信息集。
setupapi.SetupDiDestroyDeviceInfoList.argtypes = [wt.HANDLE]
setupapi.SetupDiDestroyDeviceInfoList.restype = wt.BOOL

kernel32.CreateFileW.restype = wt.HANDLE
kernel32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.c_void_p,
                                 wt.DWORD, wt.DWORD, wt.HANDLE]
kernel32.CreateEventW.restype = wt.HANDLE
kernel32.CreateEventW.argtypes = [ctypes.c_void_p, wt.BOOL, wt.BOOL, wt.LPCWSTR]
kernel32.CloseHandle.argtypes = [wt.HANDLE]
kernel32.CloseHandle.restype = wt.BOOL
kernel32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
kernel32.ReadFile.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD,
                              ctypes.POINTER(wt.DWORD), ctypes.c_void_p]
kernel32.WriteFile.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD,
                               ctypes.POINTER(wt.DWORD), ctypes.c_void_p]
kernel32.GetOverlappedResult.argtypes = [wt.HANDLE, ctypes.c_void_p,
                                         ctypes.POINTER(wt.DWORD), wt.BOOL]
kernel32.CancelIoEx.argtypes = [wt.HANDLE, ctypes.c_void_p]
kernel32.CancelIoEx.restype = wt.BOOL
kernel32.DeviceIoControl.argtypes = [wt.HANDLE, wt.DWORD, ctypes.c_void_p, wt.DWORD,
                                     ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD), ctypes.c_void_p]


def _hid_guid():
    g = GUID()
    ctypes.memmove(ctypes.byref(g), bytes.fromhex("b2551e4d6ff1cf1188cb001111000030"), 16)
    return g


def _detail_path(hdev, did):
    """取接口路径。SP_DEVICE_INTERFACE_DETAIL_DATA_W 是变长结构。

    用固定 512 字符的 Python 结构接收、却把系统要求的 need 当缓冲区大小传进去，
    路径一长就是越界写；传结构自身大小又会让长路径直接失败。按 need 分配原始
    字节才是对的：cbSize 按指针宽度填，DevicePath 固定在偏移 4（WCHAR 对齐 2）。
    """
    need = wt.DWORD(0)
    setupapi.SetupDiGetDeviceInterfaceDetailW(hdev, ctypes.byref(did), None, 0,
                                              ctypes.byref(need), None)
    if need.value == 0:
        return None
    buf = ctypes.create_string_buffer(need.value)
    cb = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6
    ctypes.cast(buf, ctypes.POINTER(wt.DWORD))[0] = cb
    if not setupapi.SetupDiGetDeviceInterfaceDetailW(hdev, ctypes.byref(did), buf,
                                                    need.value, ctypes.byref(need), None):
        return None
    return buf.raw[4:].decode("utf-16-le", "replace").split("\x00")[0]


def enum():
    guid = _hid_guid()
    hdev = setupapi.SetupDiGetClassDevsW(ctypes.byref(guid), None, None,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    if hdev == INVALID_HANDLE_VALUE:
        raise OSError("SetupDiGetClassDevs failed")
    rows = []
    i = 0
    while True:
        did = SP_DEVICE_INTERFACE_DATA()
        did.cbSize = ctypes.sizeof(did)
        if not setupapi.SetupDiEnumDeviceInterfaces(hdev, None, ctypes.byref(guid), i,
                                                    ctypes.byref(did)):
            break
        i += 1
        path = _detail_path(hdev, did)
        if not path:
            continue
        h = kernel32.CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, None,
                                 OPEN_EXISTING, 0, None)
        if h == INVALID_HANDLE_VALUE:
            continue
        attrs = HIDD_ATTRIBUTES()
        attrs.Size = ctypes.sizeof(attrs)
        hid.HidD_GetAttributes(h, ctypes.byref(attrs))
        caps = HIDP_CAPS()
        pp = ctypes.c_void_p()
        if hid.HidD_GetPreparsedData(h, ctypes.byref(pp)):
            hid.HidP_GetCaps(pp, ctypes.byref(caps))
            hid.HidD_FreePreparsedData(pp)
        pb = ctypes.create_unicode_buffer(256)
        hid.HidD_GetProductString(h, pb, ctypes.sizeof(pb))
        mb = ctypes.create_unicode_buffer(256)
        hid.HidD_GetManufacturerString(h, mb, ctypes.sizeof(mb))
        sb = ctypes.create_unicode_buffer(256)
        hid.HidD_GetSerialNumberString(h, sb, ctypes.sizeof(sb))
        kernel32.CloseHandle(h)
        rows.append((attrs.VendorID, attrs.ProductID, caps.UsagePage, caps.Usage,
                     caps.InputReportByteLength, caps.OutputReportByteLength,
                     caps.FeatureReportByteLength, pb.value, mb.value, sb.value, path))
    setupapi.SetupDiDestroyDeviceInfoList(hdev)

    print(f"{'VID:PID':11} {'UsgPg':5} {'Usg':4} {'IN':>3} {'OUT':>3} {'FEA':>3}  "
          f"{'产品名':32} 路径")
    for r in sorted(rows):
        vid, pid, up, us, il, ol, fl, prod, man, serial, path = r
        print(f"{vid:04X}:{pid:04X}  {up:04X}  {us:04X} {il:4d} {ol:4d} {fl:4d}  "
              f"{prod[:32]:32} {path}")
    return rows


def find(vidpid, usage=None):
    want_vid, want_pid = [int(x, 16) for x in vidpid.split(":")]
    out = []
    for r in enum_rows():
        if r[0] != want_vid or r[1] != want_pid:
            continue
        if usage:
            up, us = [int(x, 16) for x in usage.split(":")]
            if r[2] != up or r[3] != us:
                continue
        out.append(r)
    return out


def target_args(default_vidpid, default_usage=None):
    """允许命令行覆盖脚本里写死的 VID:PID / usage。

    脚本里的默认值只是"作者那台设备"，换一台就得改源码——这正是这些一次性脚本
    最常见的坏味道。传参即可复用。
    """
    vidpid = sys.argv[1] if len(sys.argv) > 1 else default_vidpid
    usage = sys.argv[2] if len(sys.argv) > 2 else default_usage
    return vidpid, usage


def require_one(vidpid, usage=None):
    """取唯一匹配的接口。找不到时给一句能照着做的提示，而不是 IndexError。"""
    rows = find(vidpid, usage)
    if not rows:
        raise SystemExit(
            f"未找到接口 {vidpid} usage={usage or '*'}\n"
            f"  先跑 `python hidprobe.py enum`，确认设备已连接、VID:PID 与 usage 正确")
    if len(rows) > 1:
        print(f"提示：{vidpid} usage={usage or '*'} 匹配到 {len(rows)} 个接口，使用第一个")
    return rows[0]


def drain(h, length, timeout_ms=40, limit=64):
    """排空积压的输入报告。必须有次数上限——设备持续推送时无界循环不会退出。"""
    n = 0
    while n < limit and _read_timeout(h, length, timeout_ms):
        n += 1
    return n


_ENUM_CACHE = []

def enum_rows():
    if not _ENUM_CACHE:
        import io
        import contextlib
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            _ENUM_CACHE.extend(enum())
    return _ENUM_CACHE


def _open(path, overlapped=False, access=GENERIC_READ | GENERIC_WRITE):
    flags = FILE_FLAG_OVERLAPPED if overlapped else 0
    h = kernel32.CreateFileW(path, access,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, None,
                             OPEN_EXISTING, flags, None)
    if h == INVALID_HANDLE_VALUE:
        err = ctypes.get_last_error()
        raise OSError(f"CreateFile failed err={err}")
    return h


def _read_timeout(h, length, timeout_ms):
    ev = kernel32.CreateEventW(None, True, False, None)
    if not ev:
        return None
    ov = OVERLAPPED()
    ov.hEvent = ev
    buf = ctypes.create_string_buffer(length)
    got = wt.DWORD(0)
    ok = kernel32.ReadFile(h, buf, length, ctypes.byref(got), ctypes.byref(ov))
    if not ok:
        err = ctypes.get_last_error()
        if err != ERROR_IO_PENDING:
            kernel32.CloseHandle(ev)
            return None
        rc = kernel32.WaitForSingleObject(ev, timeout_ms)
        if rc != WAIT_OBJECT_0:
            # 只 CancelIo 然后马上关事件是错的：内核此时仍可能持有 OVERLAPPED、
            # 事件句柄和读缓冲区指针，Python 一旦回收这些对象就是 use-after-free。
            # 必须等 I/O 真正结束（GetOverlappedResult 的 bWait=TRUE）再释放。
            kernel32.CancelIoEx(h, ctypes.byref(ov))
            kernel32.GetOverlappedResult(h, ctypes.byref(ov), ctypes.byref(got), True)
            kernel32.CloseHandle(ev)
            return None
        if not kernel32.GetOverlappedResult(h, ctypes.byref(ov), ctypes.byref(got), False):
            kernel32.CloseHandle(ev)
            return None
    kernel32.CloseHandle(ev)
    return buf.raw[:got.value]


def _write_timeout(h, data, timeout_ms=1000):
    ev = kernel32.CreateEventW(None, True, False, None)
    if not ev:
        return False, -1
    ov = OVERLAPPED()
    ov.hEvent = ev
    buf = ctypes.create_string_buffer(bytes(data), len(data))
    written = wt.DWORD(0)
    ok = kernel32.WriteFile(h, buf, len(data), ctypes.byref(written), ctypes.byref(ov))
    if not ok:
        err = ctypes.get_last_error()
        if err != ERROR_IO_PENDING:
            kernel32.CloseHandle(ev)
            return False, err
        rc = kernel32.WaitForSingleObject(ev, timeout_ms)
        if rc != WAIT_OBJECT_0:
            kernel32.CancelIoEx(h, ctypes.byref(ov))
            kernel32.GetOverlappedResult(h, ctypes.byref(ov), ctypes.byref(written), True)
            kernel32.CloseHandle(ev)
            return False, -1
        if not kernel32.GetOverlappedResult(h, ctypes.byref(ov), ctypes.byref(written), False):
            kernel32.CloseHandle(ev)
            return False, -1
    kernel32.CloseHandle(ev)
    return True, written.value


def listen(vidpid, seconds, usage=None):
    targets = find(vidpid, usage)
    if not targets:
        print("没找到接口")
        return
    for r in targets:
        vid, pid, up, us, il, ol, fl, prod, man, serial, path = r
        print(f"\n=== {vid:04X}:{pid:04X} {up:04X}/{us:04X} IN={il} OUT={ol} FEA={fl} ===")
        print(f"    {path}")
        try:
            h = _open(path, overlapped=True)
        except OSError as e:
            print("   打开失败:", e)
            continue
        end = time.time() + seconds
        n = 0
        while time.time() < end:
            data = _read_timeout(h, max(il, 64), 300)
            if data:
                n += 1
                print(f"   [{n:3d}] len={len(data):3d}  {data.hex(' ')}")
                if n > 60:
                    break
        if n == 0:
            print("   （无输入报告）")
        kernel32.CloseHandle(h)


def xfer(vidpid, usage, payload_hex, reads=4, timeout_ms=250, via="write"):
    """写一帧然后收应答。

    读写必须用两个句柄：写（以及特性报告的 Get/Set）用**同步**句柄，收包用异步
    句柄。旧实现只开一个 FILE_FLAG_OVERLAPPED 句柄，却用 NULL lpOverlapped 去
    WriteFile、还用 HidD_SetFeature 去写——这两件事对异步句柄都是未定义行为，
    于是 send 子命令给出的"写失败/无回包"结论并不可信。
    """
    targets = find(vidpid, usage)
    if not targets:
        print("没找到接口")
        return
    try:
        payload = bytes.fromhex(payload_hex.replace(" ", ""))
    except ValueError as e:
        raise SystemExit(f"payload 不是合法十六进制: {e}")
    for r in targets:
        vid, pid, up, us, il, ol, fl, prod, man, serial, path = r
        print(f"\n=== {vid:04X}:{pid:04X} {up:04X}/{us:04X} IN={il} OUT={ol} FEA={fl} "
              f"发送 {len(payload)} 字节 via {via} ===")
        try:
            hs = _open(path)                    # 同步：写 + 特性报告
            hr = _open(path, overlapped=True)   # 异步：只负责收
        except OSError as e:
            print("   打开失败:", e)
            continue
        try:
            buf = ctypes.create_string_buffer(payload, len(payload))
            if via == "feature":
                ok = hid.HidD_SetFeature(hs, buf, len(payload))
                print("   SetFeature ->", bool(ok), ctypes.get_last_error())
            else:
                written = wt.DWORD(0)
                ok = kernel32.WriteFile(hs, buf, len(payload), ctypes.byref(written), None)
                print("   WriteFile ->", bool(ok), "written=", written.value,
                      "err=", ctypes.get_last_error())
            for i in range(reads):
                data = _read_timeout(hr, max(il, 128), timeout_ms)
                if data:
                    print(f"   reply[{i}] len={len(data)}  {data.hex(' ')}")
                else:
                    print(f"   reply[{i}] 无")
                    break
            if via == "feature":
                fb = ctypes.create_string_buffer(max(fl, 128))
                if hid.HidD_GetFeature(hs, fb, ctypes.sizeof(fb)):
                    print("   GetFeature:", fb.raw[:max(fl, 16)].hex(" "))
        finally:
            kernel32.CloseHandle(hr)
            kernel32.CloseHandle(hs)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    cmd = sys.argv[1]
    if cmd == "enum":
        enum()
    elif cmd == "listen":
        listen(sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 3.0,
               sys.argv[4] if len(sys.argv) > 4 else None)
    elif cmd == "desc":
        dump_desc(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
    elif cmd == "send":
        via = sys.argv[6] if len(sys.argv) > 6 else "write"
        xfer(sys.argv[2], sys.argv[3], sys.argv[4], via=via)
    else:
        print(__doc__)




def dump_desc(vidpid, usage=None):
    targets = find(vidpid, usage)
    for r in targets:
        vid, pid, up, us, il, ol, fl, prod, man, serial, path = r
        h = _open(path, access=0)
        raw = ctypes.create_string_buffer(4096)
        returned = wt.DWORD(0)
        ok = kernel32.DeviceIoControl(h, 0x000B0007, None, 0,
                                      raw, 4096, ctypes.byref(returned), None)
        if not ok:
            print("描述符读取失败", ctypes.get_last_error())
            kernel32.CloseHandle(h)
            continue
        data = raw.raw[:returned.value]
        print(f"\n=== {vid:04X}:{pid:04X} {up:04X}/{us:04X} IN={il} OUT={ol} FEA={fl} "
              f"descriptor={len(data)}B ===")
        for i in range(0, len(data), 16):
            print(f"  {i:04x}  " + " ".join(f"{b:02x}" for b in data[i:i + 16]))
        kernel32.CloseHandle(h)

if __name__ == "__main__":
    main()
