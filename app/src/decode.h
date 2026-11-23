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

// 罗技 HID++ 特性 0x1001（BatteryVoltage）的电压是 2 字节大端 mV。
// 少数固件按小端；只在"大端不合理、小端合理"时才回退，避免把噪声当电压。
// 两种字节序都落在 3000..5000 mV 之外时返回 -1。
int DecodeLogitechVoltage(uint8_t b0, uint8_t b1);

// 只报电压的罗技 G 系列（G913/G915 TKL、G903、G502 LIGHTSPEED…）没有
// 0x1000 百分比特性，用 Linux 内核 hid-logitech-hidpp.c 的
// hidpp20_map_battery_capacity() 电压曲线估算 0..100。
// 这是估算值，不是设备自报；界面必须标「≈」。电压不合理返回 -1。
int LogitechVoltageToPercent(int millivolts);

// 电压→百分比换算。充放电**共用同一条曲线**，绝不按状态扣固定偏移：
// 上一版扣 185 mV「IR 抬升」，充电末期电流变小后就把 99% 压成 75%，拔线
// 不扣又弹回 99%，制造了 19 点的人为断层。G HUB 的做法是充满校准，不是扣电压。
// full（设备报告已充满）直接对齐 100%，与该电池的最大电压对齐。
// 这是**通用**内核曲线，只给没有官方 PowerModel 的罗技设备兜底。
int LogitechBatteryPercent(int millivolts, bool full);

// G913 TKL / G915 TKL 的官方 PowerModel（从 G HUB 设备资源 battery.xml 提取）。
// 放电和充电是两条独立曲线：同一电压下充电端电压被抬高，SoC 必须查充电表，
// 否则就会把 83% 显示成 97%。超出表范围时夹到端点。
int LogitechDischargePercent(int millivolts);
int LogitechChargingPercent(int millivolts);

// 官方 <charging_time_slots equation_coefficiency="150">：
// 充电电压稳定在 4100mV 以上后，每 150 秒涨 1%，补齐充电曲线只到 91% 的缺口。
int LogitechTrickleLevel(int base, uint64_t stableElapsedMs);

// 估算值的变化速率限制：只挡状态切换瞬间端电压的跳变，不拖慢真实充放电。
// 距上次采样超过 30 分钟（设备关机 / 离场）时直接采用新值。
int SmoothEstimatedLevel(int prev, int next, uint64_t elapsedMs);

// ---------------- SinoWealth（中颖）2.4G 键盘私有协议
//
// 狼蛛（AULA）F87 等三模键盘的中颖 / Compx 接收器，在厂商集合上提供
// 20 字节双向通道。实机验证的读取顺序是：
//   1) 先被动听 200ms，设备可能主动推电量；
//   2) 发握手 13 05 01 00 ... 00 19；
//   3) 等 50ms，再发查询 13 4A 01 00 ... 00 5E；
//   4) 应答以 13 4A 或 0A 为锚点，相对锚点 [5] 是电量，[6].bit4 是充电位。
// 只发握手、或把 13 0A 当成唯一帧头并从固定下标 6 取电量，都会漏掉真实回包。
constexpr int SINO_LEN = 20;

// 组一帧握手 / 查询包（command = 0x05 / 0x4A）。返回 false 表示 outLen 装不下。
bool BuildSinoQuery(uint8_t* out, int outLen, uint8_t command);

// 解一帧回包。优先找 13 4A，找不到再找 0A；电量有效范围为 2..100，
// 0/1 是未就绪或关机占位，不能当成真实低电量。charging 可为 nullptr。
bool ParseSinoBattery(const uint8_t* frame, int len, int* level, int* charging);

// 自检：跑一遍内置用例，把 TAP 结果写到 reportPath（UTF-8）。
// 返回失败条数（0 = 全过），供 build.bat 当门禁用。
int RunSelfTest(const std::wstring& reportPath);
