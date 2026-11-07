// decode.h —— 描述符 / 报文解码的纯函数
//
// 这几个函数不碰设备、不碰全局状态、不分配系统资源，只做"字节 -> 数字"的换算。
// 单独暴露的唯一理由：让 `--selftest` 能脱离硬件断言它们。
// 新增解析逻辑时，先在这里的函数体内实现，再在 selftest.cpp 里补一条用例。
#pragma once

#include <cstdint>
#include <vector>

#include "hid.h"   // BatteryField

// 从报文数据区按位取出一个整数。payload 不含 Report ID。
// 任何越界 / 畸形参数都返回 -1（描述符来自设备，不可信）。
int ExtractBits(const uint8_t* payload, int payloadLen, int bitOffset, int bitSize);

// 把原始值按 logical range 线性映射成 0..100。
// 区间非法或值越界返回 -1；未声明范围（hi==0）时按 0..100 处理。
int NormalizeLevel(int value, int logicalMin, int logicalMax);

// 从 HidD_GetFeature 的缓冲区解出电量。首字节是 Report ID，数据从第 2 字节开始。
bool DecodeBatteryReport(const std::vector<uint8_t>& buf, const BatteryField& bf,
                         int* level, int* raw);

// 自检：跑一遍内置用例，把 TAP 结果写到 reportPath（UTF-8）。
// 返回失败条数（0 = 全过），供 build.bat 当门禁用。
int RunSelfTest(const std::wstring& reportPath);
