// hid.cpp —— HID 枚举、报表描述符解析、设备读写
//
// 负责：把 Windows 的 SetupAPI / HID 接口包成 HidInfo / HidDevice；解析描述符。
// 不负责：判断"这是什么品牌的设备"、电量百分比换算、界面文案（providers.cpp）。
// 不变量：只读设备（GetFeature / Read），不写配置、不改设备状态。
//         描述符来自设备，任何字段都可能是畸形值，解析层必须先自证合法。
#include "hid.h"

#include <initguid.h>
#include <hidclass.h>
#include <hidsdi.h>

#include <algorithm>
#include <cstring>

#pragma comment(lib, "setupapi.lib")

// 取 HID 报表描述符的两个控制码。实测（Win11 26200）0x000B0007 在所有接口上
// 都返回 ERROR_INVALID_FUNCTION，整条通用 HID 兜底因此从未真正跑起来；用户态
// 能拿到描述符的是 HID_CTL_CODE(100)。先试 100，失败再退回 1，老系统不会变差。
// 不用 hidclass.h 里的宏：它展开成 HID_CTL_CODE 需要 FILE_DEVICE_KEYBOARD，
// 在只包含 setupapi/hidsdi 的翻译单元里不可见。
namespace {
constexpr DWORD kIoctlGetCollectionDescriptor = 0x000B0193;   // HID_CTL_CODE(100)
constexpr DWORD kIoctlGetReportDescriptor = 0x000B0007;       // HID_CTL_CODE(1)
}

// MinGW 的 setupapi.h 没有这个常量（Windows SDK 里有）。
// 取值按 SDK 的枚举顺序：SPDRP_LOCATION_PATHS = 0x23，下一个就是它。
#ifndef SPDRP_BASE_CONTAINERID
#define SPDRP_BASE_CONTAINERID 0x00000024
#endif

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string a(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &a[0], n, nullptr, nullptr);
    return a;
}

static bool IsBatteryUsage(uint16_t page, uint16_t usage) {
    if (page == 0x06 && usage == 0x20) return true;   // Battery Strength
    if (page == 0x84 && (usage == 0x44 || usage == 0x43 || usage == 0x40)) return true;
    return false;
}

bool ReportField::isBattery() const {
    for (uint16_t u : usages) {
        if (IsBatteryUsage(usagePage, u)) return true;
    }
    return false;
}

// ---------------------------------------------------------------- 描述符解析

namespace {

struct Globals {
    uint16_t usagePage = 0;
    int reportSize = 0;
    int reportCount = 1;
    int reportId = 0;
    int logicalMin = 0;
    int logicalMax = 0;
};

int SignedValue(uint32_t raw, int size) {
    if (size <= 0) return 0;
    int bits = size * 8;
    if (bits < 32 && (raw & (1u << (bits - 1)))) {
        return (int)(raw - (1u << bits));
    }
    return (int)raw;
}

}  // namespace

std::vector<ReportField> ParseDescriptor(const std::vector<uint8_t>& blob) {
    std::vector<ReportField> out;
    std::map<std::pair<int, int>, int> offsets;   // (reportId, kind) -> bit offset
    std::vector<Globals> stack;
    Globals g;
    std::vector<std::pair<uint16_t, uint16_t>> usages;   // (page, usage)
    int usageMin = -1;

    size_t i = 0;
    while (i < blob.size()) {
        uint8_t prefix = blob[i++];
        if (prefix == 0xFE) {                    // 长条目
            if (i + 1 >= blob.size()) break;
            uint8_t size = blob[i];
            i += 2 + size;
            continue;
        }
        int size = prefix & 0x03;
        if (size == 3) size = 4;
        int type = (prefix >> 2) & 0x03;
        int tag = prefix & 0xFC;

        uint32_t raw = 0;
        for (int k = 0; k < size && i + k < blob.size(); ++k) {
            raw |= (uint32_t)blob[i + k] << (8 * k);
        }
        i += size;

        if (type == 1) {                          // Global
            switch (tag) {
                case 0x04: g.usagePage = (uint16_t)raw; break;
                case 0x14: g.logicalMin = SignedValue(raw, size); break;
                case 0x24: g.logicalMax = SignedValue(raw, size); break;
                case 0x74: g.reportSize = (int)raw; break;
                case 0x84: g.reportId = (int)raw; break;
                case 0x94: g.reportCount = (int)raw; break;
                case 0xA4: stack.push_back(g); break;
                case 0xB4: if (!stack.empty()) { g = stack.back(); stack.pop_back(); } break;
                default: break;
            }
        } else if (type == 2) {                   // Local
            if (tag == 0x08) {
                usages.emplace_back(g.usagePage, (uint16_t)raw);
            } else if (tag == 0x18) {
                usageMin = (int)raw;
            } else if (tag == 0x28 && usageMin >= 0) {
                for (int u = usageMin; u <= (int)raw && u - usageMin < 256; ++u) {
                    usages.emplace_back(g.usagePage, (uint16_t)u);
                }
                usageMin = -1;
            }
        } else if (type == 0) {                   // Main
            if (tag == 0x80 || tag == 0x90 || tag == 0xB0) {
                auto key = std::make_pair(g.reportId,
                                          (tag == 0x80) ? 0 : (tag == 0x90 ? 1 : 2));
                const int itemStart = offsets[key];

                // 一个 Main Item 里有 reportCount 个子字段，每个子字段对应一个 usage。
                // 原实现整项只产出一个字段、bitOffset 指向第一个子字段，电量 usage 排在
                // 第二项之后就会读到别人的字节（实测 raw=17 而不是 100）。
                // 这里按 count 展开；只有"整项里没有电量 usage"时才退回单字段，
                // 避免给键盘这类 reportCount 很大的输入报表白白造出一堆字段。
                bool maybeBattery = false;
                for (auto& u : usages) {
                    if (IsBatteryUsage(u.first, u.second)) { maybeBattery = true; break; }
                }

                const int fieldBits = g.reportSize;
                const int fieldCount = g.reportCount;
                long long span = (fieldBits > 0 && fieldCount > 0)
                                     ? (long long)fieldBits * fieldCount : 0;
                if (span > 1073741823LL) span = 1073741823LL;   // 防 int 溢出

                if (maybeBattery && fieldBits > 0 && fieldCount > 1) {
                    const size_t nusg = usages.size();
                    const int expand = (fieldCount > 256) ? 256 : fieldCount;  // 防畸形描述符
                    for (int k = 0; k < expand; ++k) {
                        // 位宽来自设备，1..32 之外的值不可能对应真实字段。
                        // 在源头就丢掉，而不是等 ExtractBits 再拒绝一次。
                        if (fieldBits <= 0 || fieldBits > 32) break;
                        long long off = (long long)itemStart + (long long)k * fieldBits;
                        if (off > 0x7FFFFFFFLL) break;
                        size_t ui = (k < (int)nusg) ? (size_t)k : (nusg ? nusg - 1 : 0);
                        ReportField f;
                        f.kind = (tag == 0x80) ? 0 : (tag == 0x90 ? 1 : 2);
                        f.reportId = (uint8_t)g.reportId;
                        f.usagePage = nusg ? usages[ui].first : g.usagePage;
                        if (nusg) f.usages.push_back(usages[ui].second);
                        f.bitOffset = (int)off;
                        f.bitSize = fieldBits;
                        f.logicalMin = g.logicalMin;
                        f.logicalMax = g.logicalMax;
                        out.push_back(f);
                    }
                } else {
                    if (fieldBits > 0 && fieldBits <= 32) {   // 非法位宽不产出字段
                        ReportField f;
                        f.kind = (tag == 0x80) ? 0 : (tag == 0x90 ? 1 : 2);
                        f.reportId = (uint8_t)g.reportId;
                        f.usagePage = usages.empty() ? g.usagePage : usages.front().first;
                        for (auto& u : usages) f.usages.push_back(u.second);
                        f.bitOffset = itemStart;
                        f.bitSize = fieldBits;
                        f.logicalMin = g.logicalMin;
                        f.logicalMax = g.logicalMax;
                        out.push_back(f);
                    }
                }

                // 偏移累加用 long long 再夹住：畸形描述符里 span 被钳到 1GB 位，
                // 连续几项仍可能把 int 加成负数，后面所有字段的偏移就都错了。
                long long nextOffset = (long long)itemStart + span;
                if (nextOffset > 0x7FFFFFFFLL) nextOffset = 0x7FFFFFFFLL;
                offsets[key] = (int)nextOffset;
                usages.clear();
                usageMin = -1;
            } else if (tag == 0xC0 || tag == 0xA0) {
                usages.clear();
                usageMin = -1;
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------- 枚举

std::vector<HidInfo> EnumHidDevices() {
    std::vector<HidInfo> result;
    GUID guid;
    HidD_GetHidGuid(&guid);

    HDEVINFO devInfo = SetupDiGetClassDevsW(&guid, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return result;

    for (DWORD idx = 0;; ++idx) {
        SP_DEVICE_INTERFACE_DATA did{};
        did.cbSize = sizeof(did);
        if (!SetupDiEnumDeviceInterfaces(devInfo, nullptr, &guid, idx, &did)) break;

        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &did, nullptr, 0, &need, nullptr);
        if (need == 0) continue;

        std::vector<uint8_t> buf(need);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA devNode{};
        devNode.cbSize = sizeof(devNode);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &did, detail, need, nullptr, &devNode))
            continue;

        HidInfo info;
        info.path = detail->DevicePath;

        // ContainerId 用来区分"同型号的两台设备"：它按物理设备分配，
        // 一台复合设备的所有 HID 集合共享同一个值。拿不到就让上层退回 VID:PID。
        wchar_t cid[128] = {0};
        DWORD regType = 0;
        DWORD regSize = sizeof(cid);
        if (SetupDiGetDeviceRegistryPropertyW(devInfo, &devNode, SPDRP_BASE_CONTAINERID,
                                              &regType, (PBYTE)cid, regSize, nullptr) &&
            cid[0]) {
            info.containerId = cid;
        }

        HANDLE h = CreateFileW(info.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            h = CreateFileW(info.path.c_str(), 0,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        }
        if (h == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attrs{};
        attrs.Size = sizeof(attrs);
        if (HidD_GetAttributes(h, &attrs)) {
            info.vid = attrs.VendorID;
            info.pid = attrs.ProductID;
            info.version = attrs.VersionNumber;
        }

        wchar_t tmp[256] = {0};
        if (HidD_GetProductString(h, tmp, sizeof(tmp))) {
            info.product = tmp;
            tmp[0] = 0;
        }
        if (HidD_GetManufacturerString(h, tmp, sizeof(tmp))) {
            info.manufacturer = tmp;
        }
        wchar_t sn[128] = {0};
        if (HidD_GetSerialNumberString(h, sn, sizeof(sn))) info.serial = sn;

        PHIDP_PREPARSED_DATA prep = nullptr;
        if (HidD_GetPreparsedData(h, &prep)) {
            HIDP_CAPS caps{};
            if (HidP_GetCaps(prep, &caps) == HIDP_STATUS_SUCCESS) {
                info.usagePage = caps.UsagePage;
                info.usage = caps.Usage;
                info.inputLen = caps.InputReportByteLength;
                info.outputLen = caps.OutputReportByteLength;
                info.featureLen = caps.FeatureReportByteLength;
                info.valid = true;
            }
            HidD_FreePreparsedData(prep);
        }
        CloseHandle(h);

        if (info.valid) result.push_back(info);
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return result;
}

// ---------------------------------------------------------------- 设备

bool HidDevice::Open(const std::wstring& path) {
    Close();
    info_ = HidInfo{};   // 复用同一对象时不能残留上一次的产品名/报告长度
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        handle_ = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    }
    if (handle_ == INVALID_HANDLE_VALUE) return false;

    info_.path = path;
    HIDD_ATTRIBUTES attrs{};
    attrs.Size = sizeof(attrs);
    if (HidD_GetAttributes(handle_, &attrs)) {
        info_.vid = attrs.VendorID;
        info_.pid = attrs.ProductID;
        info_.version = attrs.VersionNumber;
    }
    wchar_t tmp[256] = {0};
    if (HidD_GetProductString(handle_, tmp, sizeof(tmp))) info_.product = tmp;
    tmp[0] = 0;
    if (HidD_GetManufacturerString(handle_, tmp, sizeof(tmp))) info_.manufacturer = tmp;

    PHIDP_PREPARSED_DATA prep = nullptr;
    if (HidD_GetPreparsedData(handle_, &prep)) {
        HIDP_CAPS caps{};
        if (HidP_GetCaps(prep, &caps) == HIDP_STATUS_SUCCESS) {
            info_.usagePage = caps.UsagePage;
            info_.usage = caps.Usage;
            info_.inputLen = caps.InputReportByteLength;
            info_.outputLen = caps.OutputReportByteLength;
            info_.featureLen = caps.FeatureReportByteLength;
            info_.valid = true;
        }
        HidD_FreePreparsedData(prep);
    }
    return true;
}

void HidDevice::Close() {
    if (handle_ != INVALID_HANDLE_VALUE) {
        CancelIoEx(handle_, nullptr);
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    fields_.clear();
    descLoaded_ = false;
}

bool HidDevice::LoadDescriptor() {
    if (descLoaded_) return !fields_.empty();
    descLoaded_ = true;
    if (handle_ == INVALID_HANDLE_VALUE) return false;

    // 描述符用 IOCTL 取，而 DeviceIoControl 在 FILE_FLAG_OVERLAPPED 句柄上配 NULL
    // OVERLAPPED 的语义并不保证（实测只有个别接口能成）。这里单独开一个同步句柄，
    // 读完就关；每次扫描每台设备只多一次 CreateFile。
    HANDLE io = CreateFileW(info_.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr);
    if (io == INVALID_HANDLE_VALUE) {
        io = CreateFileW(info_.path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, 0, nullptr);
    }
    const bool ownIo = (io != INVALID_HANDLE_VALUE);
    if (!ownIo) io = handle_;      // 连只读都开不出来时，退而用现有句柄试一次

    std::vector<uint8_t> raw(4096);

    // 两个控制码都试，只有"内容像报表描述符"才采用：
    //   HID_CTL_CODE(100) 在 Win11 上能成功，但返回的是以 "HidP KDR" 开头的
    //   内部结构，不是描述符；HID_CTL_CODE(1) 在 Win11 上直接
    //   ERROR_INVALID_FUNCTION，在老系统上才是正确入口。
    // 结论：用户态拿不到原始报表描述符，这层兜底没有可靠数据源，只能安全失败
    // （返回 false，不产生任何读数），而不是拿内部结构解析出"全零字段"。
    const DWORD codes[] = {kIoctlGetCollectionDescriptor, kIoctlGetReportDescriptor};
    bool ok = false;
    for (DWORD code : codes) {
        DWORD returned = 0;
        if (!DeviceIoControl(io, code, nullptr, 0, raw.data(), (DWORD)raw.size(),
                             &returned, nullptr)) {
            continue;
        }
        if (returned < 8) continue;
        if (memcmp(raw.data(), "HidP KDR", 8) == 0) continue;   // 内部结构，不是描述符
        std::vector<uint8_t> blob(raw.begin(), raw.begin() + returned);
        auto parsed = ParseDescriptor(blob);
        bool sane = false;
        for (const auto& f : parsed) {
            if (f.bitSize > 0 && f.bitSize <= 32) { sane = true; break; }
        }
        if (!sane) continue;
        fields_ = std::move(parsed);
        ok = true;
        break;
    }
    if (ownIo) CloseHandle(io);
    return ok;   // 失败时 fields_ 保持为空
}

bool HidDevice::FindBattery(BatteryField* out) const {
    const ReportField* best = nullptr;
    for (const auto& f : fields_) {
        if (f.kind != 2 || f.bitSize <= 0 || !f.isBattery()) continue;
        if (!best || (best->usagePage != 0x06 && f.usagePage == 0x06)) best = &f;
    }
    if (!best) return false;
    out->reportId = best->reportId;
    out->bitOffset = best->bitOffset;
    out->bitSize = best->bitSize;
    out->logicalMin = best->logicalMin;
    out->logicalMax = best->logicalMax;
    out->valid = true;
    return true;
}

bool HidDevice::GetFeature(std::vector<uint8_t>& buf, uint16_t len, uint8_t reportId) {
    if (handle_ == INVALID_HANDLE_VALUE) return false;
    if (len < 2) len = (info_.featureLen > 1) ? info_.featureLen : 8;
    buf.assign(len, 0);
    buf[0] = reportId;                 // 首字节必须是报表 ID
    if (!HidD_GetFeature(handle_, buf.data(), len)) return false;
    return true;
}

int HidDevice::Write(const uint8_t* data, DWORD len) {
    if (handle_ == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return -1;
    DWORD written = 0;
    BOOL ok = WriteFile(handle_, data, len, nullptr, &ov);
    int result = -1;
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, 500) == WAIT_OBJECT_0 &&
                GetOverlappedResult(handle_, &ov, &written, FALSE)) {
                result = (int)written;
            } else {
                CancelIoEx(handle_, &ov);
                GetOverlappedResult(handle_, &ov, &written, TRUE);
                result = -1;
            }
        }
    } else {
        GetOverlappedResult(handle_, &ov, &written, FALSE);
        result = (int)written;
    }
    CloseHandle(ov.hEvent);
    return result;
}

int HidDevice::Read(uint8_t* buf, DWORD len, int timeoutMs) {
    if (handle_ == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return -1;
    DWORD got = 0;
    BOOL ok = ReadFile(handle_, buf, len, nullptr, &ov);
    int result = -1;
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            DWORD w = WaitForSingleObject(ov.hEvent, (DWORD)timeoutMs);
            if (w == WAIT_OBJECT_0 && GetOverlappedResult(handle_, &ov, &got, FALSE)) {
                result = (int)got;
            } else {
                CancelIoEx(handle_, &ov);
                GetOverlappedResult(handle_, &ov, &got, TRUE);
                result = 0;
            }
        } else if (err == ERROR_HANDLE_EOF || err == ERROR_OPERATION_ABORTED) {
            result = 0;
        }
    } else {
        GetOverlappedResult(handle_, &ov, &got, FALSE);
        result = (int)got;
    }
    CloseHandle(ov.hEvent);
    return result;
}
