"""BLE GATT 电量（0x180F / 0x2A19）探针。

验证 Windows 到底能不能读到 BLE 设备的电量，以及设备接口路径长什么样。
只做只读操作：列服务 → 找 Battery Service → 读一次电量。

用法：
    python ble_batt.py
"""
import ctypes
import ctypes.wintypes as wt

setupapi = ctypes.WinDLL("setupapi", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
bthprops = ctypes.WinDLL("BluetoothApis.dll", use_last_error=True)

DIGCF_PRESENT = 0x02
DIGCF_DEVICEINTERFACE = 0x10
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 0x00000001
FILE_SHARE_WRITE = 0x00000002
OPEN_EXISTING = 3
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
ERROR_INSUFFICIENT_BUFFER = 122
ERROR_NOT_FOUND = 1168

# {8232E119-D8CA-4C84-9E63-E221FEA8D1C6}
GUID_BTHLE_DEVICE = "{781AEE18-7733-4CE4-ADD0-91F41C67B592}"


class GUID(ctypes.Structure):
    _fields_ = [("Data1", wt.DWORD), ("Data2", wt.WORD), ("Data3", wt.WORD),
                ("Data4", ctypes.c_ubyte * 8)]


def guid_from_string(s):
    g = GUID()
    ctypes.windll.ole32.CLSIDFromString(s, ctypes.byref(g))
    return g


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD), ("InterfaceClassGuid", GUID),
                ("Flags", wt.DWORD), ("Reserved", ctypes.POINTER(wt.ULONG))]


class BTH_LE_UUID(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("ShortUuid", wt.USHORT), ("LongUuid", GUID)]
    _fields_ = [("IsShortUuid", ctypes.c_ubyte), ("Value", _U)]


class BTH_LE_GATT_SERVICE(ctypes.Structure):
    _fields_ = [("ServiceUuid", BTH_LE_UUID), ("AttributeHandle", wt.USHORT)]


class BTH_LE_GATT_CHARACTERISTIC(ctypes.Structure):
    _fields_ = [("ServiceHandle", wt.USHORT), ("CharacteristicUuid", BTH_LE_UUID),
                ("AttributeHandle", wt.USHORT), ("CharacteristicProperties", wt.USHORT)]


class BTH_LE_GATT_CHARACTERISTIC_VALUE(ctypes.Structure):
    _fields_ = [("DataSize", wt.ULONG), ("Data", ctypes.c_ubyte * 32)]


# ctypes 默认把返回值和参数当 32 位 int，x64 上句柄会被截断——
# 不声明 restype 的话 SetupDiGetClassDevs 拿回来的句柄是废的，枚举恒为 0。
setupapi.SetupDiGetClassDevsW.restype = wt.HANDLE
setupapi.SetupDiGetClassDevsW.argtypes = [ctypes.POINTER(GUID), wt.LPCWSTR, wt.HWND,
                                         wt.DWORD]
setupapi.SetupDiDestroyDeviceInfoList.argtypes = [wt.HANDLE]
setupapi.SetupDiDestroyDeviceInfoList.restype = wt.BOOL
setupapi.SetupDiEnumDeviceInterfaces.argtypes = [
    wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(GUID), wt.DWORD,
    ctypes.POINTER(SP_DEVICE_INTERFACE_DATA)]
setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [
    wt.HANDLE, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA), ctypes.c_void_p, wt.DWORD,
    ctypes.POINTER(wt.DWORD), ctypes.c_void_p]
kernel32.CreateFileW.restype = wt.HANDLE
kernel32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.c_void_p,
                                 wt.DWORD, wt.DWORD, wt.HANDLE]
kernel32.CloseHandle.argtypes = [wt.HANDLE]

bthprops.BluetoothGATTGetServices.argtypes = [
    wt.HANDLE, wt.USHORT, ctypes.c_void_p, ctypes.POINTER(wt.USHORT), wt.ULONG]
bthprops.BluetoothGATTGetCharacteristics.argtypes = [
    wt.HANDLE, ctypes.c_void_p, wt.USHORT, ctypes.c_void_p,
    ctypes.POINTER(wt.USHORT), wt.ULONG]
# 真实签名是 6 个参数：(Device, Characteristic, Flags, BufferSize, Buffer, NeededSize)。
# README 里也写了"6 个参数"。少传一个会把 Flags 当缓冲区大小、把缓冲区指针当
# 输出长度用，轻则读不到，重则直接崩在 API 里。
bthprops.BluetoothGATTGetCharacteristicValue.argtypes = [
    wt.HANDLE, ctypes.c_void_p, wt.ULONG, wt.USHORT, ctypes.c_void_p,
    ctypes.POINTER(wt.USHORT)]
bthprops.BluetoothGATTGetCharacteristicValue.restype = ctypes.c_long


def _detail_path(hdev, did):
    """按 need 分配变长结构。固定 512 字符的 Python 结构在长路径上会越界写。"""
    need = wt.DWORD(0)
    setupapi.SetupDiGetDeviceInterfaceDetailW(hdev, ctypes.byref(did), None, 0,
                                              ctypes.byref(need), None)
    if need.value == 0:
        return None
    buf = ctypes.create_string_buffer(need.value)
    ctypes.cast(buf, ctypes.POINTER(wt.DWORD))[0] = (
        8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6)
    if not setupapi.SetupDiGetDeviceInterfaceDetailW(hdev, ctypes.byref(did), buf,
                                                    need.value, ctypes.byref(need), None):
        return None
    return buf.raw[4:].decode("utf-16-le", "replace").split("\x00")[0]


def enum_ble_paths():
    guid = guid_from_string(GUID_BTHLE_DEVICE)
    hdev = setupapi.SetupDiGetClassDevsW(ctypes.byref(guid), None, None,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    paths = []
    if hdev == INVALID_HANDLE_VALUE:
        return paths
    i = 0
    while True:
        did = SP_DEVICE_INTERFACE_DATA()
        did.cbSize = ctypes.sizeof(did)
        if not setupapi.SetupDiEnumDeviceInterfaces(hdev, None, ctypes.byref(guid), i,
                                                    ctypes.byref(did)):
            break
        i += 1
        path = _detail_path(hdev, did)
        if path:
            paths.append(path)
    setupapi.SetupDiDestroyDeviceInfoList(hdev)   # HDEVINFO 不是普通句柄
    return paths


def read_battery(path):
    h = kernel32.CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, None,
                             OPEN_EXISTING, 0, None)
    if h == INVALID_HANDLE_VALUE:
        h = kernel32.CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, None,
                                 OPEN_EXISTING, 0, None)
        if h == INVALID_HANDLE_VALUE:
            return None, f"打不开 (err={ctypes.get_last_error()})"
    try:
        count = wt.USHORT(0)
        bthprops.BluetoothGATTGetServices(h, 0, None, ctypes.byref(count), 0)
        if count.value == 0:
            return None, "没有服务（未连接或已休眠）"
        buf = (BTH_LE_GATT_SERVICE * count.value)()
        if bthprops.BluetoothGATTGetServices(h, count, buf, ctypes.byref(count), 0) != 0:
            return None, f"GetServices 失败 (err={ctypes.get_last_error()})"

        names = []
        target = None
        for s in buf:
            u = s.ServiceUuid
            short = u.Value.ShortUuid if u.IsShortUuid else 0
            names.append(f"{short:04X}" if u.IsShortUuid else "LONG")
            if u.IsShortUuid and short == 0x180F:
                target = s
        if not target:
            return None, "无 0x180F 服务（有: " + ",".join(names) + "）"

        ccount = wt.USHORT(0)
        bthprops.BluetoothGATTGetCharacteristics(h, ctypes.byref(target), 0, None,
                                                 ctypes.byref(ccount), 0)
        if ccount.value == 0:
            return None, "0x180F 下没有特征"
        cbuf = (BTH_LE_GATT_CHARACTERISTIC * ccount.value)()
        bthprops.BluetoothGATTGetCharacteristics(h, ctypes.byref(target), ccount, cbuf,
                                                 ctypes.byref(ccount), 0)
        for c in cbuf:
            if c.CharacteristicUuid.IsShortUuid and c.CharacteristicUuid.Value.ShortUuid == 0x2A19:
                val = BTH_LE_GATT_CHARACTERISTIC_VALUE()
                need = wt.USHORT(0)
                hr = bthprops.BluetoothGATTGetCharacteristicValue(
                        h, ctypes.byref(c), 0, ctypes.sizeof(val), ctypes.byref(val),
                        ctypes.byref(need))
                if hr != 0:
                    return None, f"读 0x2A19 失败 hr=0x{hr & 0xFFFFFFFF:08X}"
                return val.Data[0], "OK"
        return None, "0x180F 下没有 0x2A19"
    finally:
        kernel32.CloseHandle(h)


def main():
    paths = enum_ble_paths()
    print(f"BLE 设备接口数: {len(paths)}")
    for p in paths:
        short = p.split("#")[1] if "#" in p else p
        pct, msg = read_battery(p)
        print(f"  {short[:70]:70} -> {pct if pct is not None else msg}")


if __name__ == "__main__":
    main()
