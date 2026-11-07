// hid.h —— HID 枚举、报表描述符解析、设备读写
#pragma once

#include "app.h"

// MinGW 的 hidpi.h 缺少 USAGE 定义（Windows SDK 里在 winnt.h）
#ifndef USAGE_DEFINED_LOCAL
#define USAGE_DEFINED_LOCAL
typedef USHORT USAGE, *PUSAGE;
#endif

#include <winternl.h>     // NTSTATUS
#include <hidpi.h>
#include <setupapi.h>

struct HidInfo {
    std::wstring path;
    std::wstring product;
    std::wstring manufacturer;
    // 物理设备标识：ContainerId 由 Windows 按"同一物理设备"分配，一台复合设备的
    // 所有 HID 集合共享同一个值，同型号的两台设备各不相同。序列号作为次选。
    std::wstring containerId;
    std::wstring serial;
    uint16_t vid = 0;
    uint16_t pid = 0;
    uint16_t version = 0;
    uint16_t usagePage = 0;
    uint16_t usage = 0;
    uint16_t inputLen = 0;
    uint16_t outputLen = 0;
    uint16_t featureLen = 0;
    bool valid = false;
};

// 描述符里的一个字段
struct ReportField {
    uint8_t reportId = 0;
    int kind = 0;          // 0=input 1=output 2=feature
    uint16_t usagePage = 0;
    std::vector<uint16_t> usages;
    int bitOffset = 0;
    int bitSize = 0;
    int logicalMin = 0;
    int logicalMax = 0;
    bool isBattery() const;
};

std::vector<ReportField> ParseDescriptor(const std::vector<uint8_t>& blob);

struct BatteryField {
    uint8_t reportId = 0;
    int bitOffset = 0;
    int bitSize = 0;
    int logicalMin = 0;
    int logicalMax = 0;
    bool valid = false;
};

class HidDevice {
public:
    ~HidDevice() { Close(); }

    bool Open(const std::wstring& path);
    void Close();
    bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }
    const HidInfo& info() const { return info_; }

    bool GetFeature(std::vector<uint8_t>& buf, uint16_t len, uint8_t reportId = 0);
    int  Write(const uint8_t* data, DWORD len);
    int  Read(uint8_t* buf, DWORD len, int timeoutMs);

    bool LoadDescriptor();
    const std::vector<ReportField>& fields() const { return fields_; }
    bool FindBattery(BatteryField* out) const;

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HidInfo info_;
    std::vector<ReportField> fields_;
    bool descLoaded_ = false;
};

std::vector<HidInfo> EnumHidDevices();

std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);
