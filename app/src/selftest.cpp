// selftest.cpp —— 纯函数自检（`yudian.exe --selftest [报告路径]`）
//
// 为什么要有这个文件：2026-10-29 那轮审查发现了 C1/C2/C8/C12 四个解析错误，
// 修好之后写过的验证程序放在临时目录里，已经随系统清理消失。于是"这段代码对不对"
// 又变成只能靠真机和记忆来回答。
//
// 这里把当时的断言固定进仓库：不联网、不碰设备、不依赖任何测试框架，
// 用同一个 exe 跑，结果写成 TAP 文本，失败时返回非 0，build.bat 拿它当门禁。
// 用例只覆盖纯函数；需要真机的部分（枚举、报文时序）仍由 --list / --vendor 覆盖。
#include "app.h"
#include "decode.h"

#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

int g_total = 0;
int g_failed = 0;
std::wstring g_body;

void Check(const wchar_t* name, bool ok) {
    ++g_total;
    if (!ok) ++g_failed;
    g_body += ok ? L"ok " : L"not ok ";
    g_body += std::to_wstring(g_total);
    g_body += L" - ";
    g_body += name;
    g_body += L"\n";
}

std::vector<uint8_t> Bytes(std::initializer_list<int> bytes) {
    std::vector<uint8_t> out;
    for (int b : bytes) out.push_back((uint8_t)b);
    return out;
}

BatteryField Field(int bitOffset, int bitSize, int logicalMin, int logicalMax) {
    BatteryField f;
    f.reportId = 0;
    f.bitOffset = bitOffset;
    f.bitSize = bitSize;
    f.logicalMin = logicalMin;
    f.logicalMax = logicalMax;
    f.valid = true;
    return f;
}

void RunCases() {
    // ---------------------------------------------- 位提取：畸形描述符必须被拒绝
    const uint8_t two[2] = {0x00, 0x64};
    Check(L"ExtractBits off=8 size=8 reads 100", ExtractBits(two, 2, 8, 8) == 100);
    Check(L"ExtractBits off=0 size=8 reads 0", ExtractBits(two, 2, 0, 8) == 0);
    Check(L"ExtractBits negative offset rejected", ExtractBits(two, 2, -8, 8) == -1);
    Check(L"ExtractBits size 40 rejected", ExtractBits(two, 2, 0, 40) == -1);
    Check(L"ExtractBits size 0 rejected", ExtractBits(two, 2, 0, 0) == -1);
    Check(L"ExtractBits past end rejected", ExtractBits(two, 2, 24, 16) == -1);
    Check(L"ExtractBits null payload rejected", ExtractBits(nullptr, 2, 0, 8) == -1);

    // ---------------------------------------------- 归一化：C2 回归
    // 旧实现"hi<=100 就把原值当百分比"，0..1 的满电会被读成 1%。
    Check(L"NormalizeLevel 0..100 passthrough", NormalizeLevel(55, 0, 100) == 55);
    Check(L"NormalizeLevel 0..1 full is 100", NormalizeLevel(1, 0, 1) == 100);
    Check(L"NormalizeLevel 0..255 full is 100", NormalizeLevel(255, 0, 255) == 100);
    Check(L"NormalizeLevel 10..20 mid is 50", NormalizeLevel(15, 10, 20) == 50);
    Check(L"NormalizeLevel out of range rejected", NormalizeLevel(200, 0, 100) == -1);
    Check(L"NormalizeLevel empty range rejected", NormalizeLevel(5, 5, 5) == -1);
    Check(L"NormalizeLevel missing range is 0..100", NormalizeLevel(42, 0, 0) == 42);
    Check(L"NormalizeLevel negative value rejected", NormalizeLevel(-3, 0, 100) == -1);

    // ---------------------------------------------- 报文解码：C1 回归
    int level = -1;
    int raw = 0;
    // 首字节永远是 Report ID，数据从第 2 字节开始。旧实现按 reportId!=0 判断，
    // 无 ID 的报文多跳一个字节，满电读成 0。
    Check(L"Decode reportId=0 reads data from byte 2",
          DecodeBatteryReport(Bytes({0x00, 0x64}), Field(0, 8, 0, 100), &level, &raw) && level == 100);
    Check(L"Decode reportId=5 reads data from byte 2",
          DecodeBatteryReport(Bytes({0x05, 0x64}), Field(0, 8, 0, 100), &level, &raw) && level == 100);
    Check(L"Decode 0..1 full frame is 100",
          DecodeBatteryReport(Bytes({0x00, 0x01}), Field(0, 8, 0, 1), &level, &raw) && level == 100);
    Check(L"Decode keeps raw value",
          DecodeBatteryReport(Bytes({0x00, 0x64}), Field(0, 8, 0, 100), &level, &raw) && raw == 100);
    // 只有 Report ID 的报文没有数据区。
    Check(L"Decode short frame rejected",
          !DecodeBatteryReport(Bytes({0x00}), Field(0, 8, 0, 100), &level, &raw));
    // 位偏移越过数据区。
    Check(L"Decode past end rejected",
          !DecodeBatteryReport(Bytes({0x00, 0x64}), Field(16, 8, 0, 100), &level, &raw));
    // 原始值落在声明区间之外。
    Check(L"Decode out of range value rejected",
          !DecodeBatteryReport(Bytes({0x00, 200}), Field(0, 8, 0, 100), &level, &raw));

    // ---------------------------------------------- 罗技 0x1001：电压字节序
    // G913 TKL 实测回帧 0F 6C 00 = 3948 mV（大端）。旧实现按小端读成
    // 0x6C0F = 27663 mV 被丢弃，整台键盘显示「未提供电量接口」。
    Check(L"LogiVoltage 0F6C is 3948 mV (big endian)",
          DecodeLogitechVoltage(0x0F, 0x6C) == 3948);
    Check(L"LogiVoltage 1004 is 4100 mV",
          DecodeLogitechVoltage(0x10, 0x04) == 4100);
    Check(L"LogiVoltage falls back to little endian when BE implausible",
          DecodeLogitechVoltage(0x6C, 0x0F) == 3948);
    Check(L"LogiVoltage rejects garbage (0,0)",
          DecodeLogitechVoltage(0x00, 0x00) == -1);
    Check(L"LogiVoltage prefers big endian when both orders plausible",
          DecodeLogitechVoltage(0x0F, 0x10) == 3856);

    // ---------------------------------------------- 罗技电压曲线（内核 100 点表）
    Check(L"LogiCurve 4186 mV -> 100%", LogitechVoltageToPercent(4186) == 100);
    Check(L"LogiCurve above full clamps to 100%", LogitechVoltageToPercent(4200) == 100);
    Check(L"LogiCurve 3948 mV -> 73%", LogitechVoltageToPercent(3948) == 73);
    Check(L"LogiCurve 3949 mV -> 74%", LogitechVoltageToPercent(3949) == 74);
    Check(L"LogiCurve 3811 mV -> 50%", LogitechVoltageToPercent(3811) == 50);
    Check(L"LogiCurve 3717 mV -> 20%", LogitechVoltageToPercent(3717) == 20);
    Check(L"LogiCurve 3537 mV -> 1%", LogitechVoltageToPercent(3537) == 1);
    Check(L"LogiCurve 3536 mV -> 0%", LogitechVoltageToPercent(3536) == 0);
    Check(L"LogiCurve 3500 mV -> 0%", LogitechVoltageToPercent(3500) == 0);
    Check(L"LogiCurve rejects out-of-range voltage", LogitechVoltageToPercent(20000) == -1);
    Check(L"LogiCurve rejects zero voltage", LogitechVoltageToPercent(0) == -1);

    // ---------------------------------------------- 充电状态：不能把端电压当开路电压
    // 实测 G913 TKL：放电 3948 mV / flags 0x00 → 插线 4133 mV / flags 0x80。
    // 旧逻辑直接套放电曲线 = 73% → 97%，插线瞬间凭空涨 24 个点。
    // 充放电共用同一条曲线。旧实现按状态扣 185mV，充电末期把 99% 压成 75%，
    // 拔线不扣又弹回 99%——这 19 点断层是人为制造的，必须不再复现。
    Check(L"LogiBattery 3948 mV -> 73%",
          LogitechBatteryPercent(3948, false) == 73);
    Check(L"LogiBattery 4133 mV -> 97% (same curve, no state offset)",
          LogitechBatteryPercent(4133, false) == 97);
    Check(L"LogiBattery 4179 mV -> 99%",
          LogitechBatteryPercent(4179, false) == 99);
    Check(L"LogiBattery full flag calibrates to 100%",
          LogitechBatteryPercent(4000, true) == 100);
    Check(L"LogiBattery rejects invalid voltage",
          LogitechBatteryPercent(0, false) == -1);

    // ---------------------------------------------- G913 官方 PowerModel（与 G HUB 同源）
    // 放电表：3948mV 官方 76.4%，4200mV 100%，3800mV 49.5%
    Check(L"G913 discharge 3948 mV -> 76%", LogitechDischargePercent(3948) == 76);
    Check(L"G913 discharge 4200 mV -> 100%", LogitechDischargePercent(4200) == 100);
    Check(L"G913 discharge 3800 mV -> 50%", LogitechDischargePercent(3800) == 50);
    Check(L"G913 discharge clamps below table -> 0%", LogitechDischargePercent(3300) == 0);
    // 充电表是独立的一条：4133mV 官方 83.3%，4179mV 88.9%，表顶 4.20V = 91%
    Check(L"G913 charging 4133 mV -> 83%", LogitechChargingPercent(4133) == 83);
    Check(L"G913 charging 4179 mV -> 89%", LogitechChargingPercent(4179) == 89);
    Check(L"G913 charging clamps at 4.20V -> 91%", LogitechChargingPercent(4250) == 91);
    Check(L"G913 charging rejects invalid voltage", LogitechChargingPercent(0) == -1);
    // 官方 charging_time_slots equation_coefficiency=150：每 150 秒涨 1%
    Check(L"G913 trickle +1% after 150s", LogitechTrickleLevel(91, 150000) == 92);
    Check(L"G913 trickle +4% after 10min", LogitechTrickleLevel(91, 600000) == 95);
    Check(L"G913 trickle clamps at 100%", LogitechTrickleLevel(99, 3600000) == 100);
    Check(L"G913 trickle rejects invalid base", LogitechTrickleLevel(-1, 0) == -1);

    // ---------------------------------------------- 估算值速率限制
    Check(L"Smooth blocks a 24-point plug-in jump", SmoothEstimatedLevel(73, 97, 120000) == 79);
    Check(L"Smooth blocks a 13-point unplug drop", SmoothEstimatedLevel(73, 60, 120000) == 67);
    Check(L"Smooth passes normal small change", SmoothEstimatedLevel(73, 75, 120000) == 75);
    Check(L"Smooth bypasses after 31 minutes offline",
          SmoothEstimatedLevel(73, 97, 31ULL * 60 * 1000) == 97);
    Check(L"Smooth with no history uses new value", SmoothEstimatedLevel(-1, 55, 0) == 55);

    // ---------------------------------------------- 托盘：自动最低电量 / 固定设备
    {
        auto Device = [](const wchar_t* key, int level, bool online) {
            Reading r;
            r.key = key;
            r.name = key;
            r.level = level;
            r.online = online;
            return r;
        };
        std::vector<Reading> devices = {
            Device(L"mouse", 72, true), Device(L"pad", 18, true),
            Device(L"offline", 2, false), Device(L"unknown", -1, true),
        };
        Check(L"Tray UID is stable for the same device key",
              TrayUidFor(L"mouse") == TrayUidFor(L"mouse"));
        Check(L"Tray UID differs for separate devices",
              TrayUidFor(L"mouse") != TrayUidFor(L"pad"));
        Check(L"Tray UID does not collide with legacy icons",
              TrayUidFor(L"mouse") > TRAY_BATTERY_UID);
        Check(L"Tray layout revision avoids the Explorer bitmap cache identity",
              TrayUidFor(L"mouse") == LegacyTrayUidFor(L"mouse") + 0x8000u);
        g_snapshot = devices;
        Check(L"Tray index lookup follows device key, not a global serial",
              TrayUidForIndex(0) == TrayUidFor(L"mouse") &&
              TrayUidForIndex(1) == TrayUidFor(L"pad"));
        Check(L"Tray index lookup rejects a missing device", TrayUidForIndex(9) == 0);
        g_snapshot.clear();
    }

    // ---------------------------------------------- 设备图标分类
    Check(L"Classify Sony over-ear headphones as Headset", ClassifyName(L"WH-1000XM5") == Kind::Headset);
    Check(L"Classify Bluetooth earbuds as Headset", ClassifyName(L"Galaxy Buds3 Pro earbuds") == Kind::Headset);
    Check(L"Classify generic headphones as Headset", ClassifyName(L"Bluetooth Headphones") == Kind::Headset);
    Check(L"Classify Xbox controller as Controller", ClassifyName(L"Xbox Wireless Controller") == Kind::Controller);
    Check(L"Classify DualShock as Controller", ClassifyName(L"DualShock 4") == Kind::Controller);
    Check(L"Classify generic gamepad as Controller", ClassifyName(L"USB Gamepad") == Kind::Controller);
    Check(L"Classify mouse as Mouse", ClassifyName(L"Wireless Mouse") == Kind::Mouse);
    Check(L"Classify keyboard as Keyboard", ClassifyName(L"Bluetooth Keyboard") == Kind::Keyboard);
    Check(L"Unknown device remains Other", ClassifyName(L"USB Input Device") == Kind::Other);

    // ---------------------------------------------- 狼蛛 F87 / 中颖 20 字节协议
    {
        uint8_t handshake[SINO_LEN];
        uint8_t query[SINO_LEN];
        Check(L"Sino handshake builds a 20-byte frame",
              BuildSinoQuery(handshake, SINO_LEN, 0x05));
        Check(L"Sino handshake starts with 13 05 01 and checksum 19",
              handshake[0] == 0x13 && handshake[1] == 0x05 && handshake[2] == 0x01 &&
              handshake[SINO_LEN - 1] == 0x19);
        Check(L"Sino query starts with 13 4A 01 and checksum 5E",
              BuildSinoQuery(query, SINO_LEN, 0x4A) &&
              query[0] == 0x13 && query[1] == 0x4A && query[2] == 0x01 &&
              query[SINO_LEN - 1] == 0x5E);
        Check(L"Sino query refuses a short buffer",
              !BuildSinoQuery(query, SINO_LEN - 1, 0x4A));
        Check(L"Sino query refuses an unknown command",
              !BuildSinoQuery(query, SINO_LEN, 0xFF));

        // F87 实机形状：锚点 13 4A，相对锚点 [5]=电量，[6].bit4=充电。
        std::vector<uint8_t> ok(SINO_LEN, 0);
        ok[0] = 0x13; ok[1] = 0x4A; ok[2] = 0x01; ok[5] = 92; ok[6] = 0x10;
        int lv = -1, ch = -1;
        Check(L"Sino 13 4A reply parses 92% and charging",
              ParseSinoBattery(ok.data(), (int)ok.size(), &lv, &ch) && lv == 92 && ch == 1);

        std::vector<uint8_t> discharging = ok;
        discharging[6] = 0;
        ch = -1;
        Check(L"Sino reply without bit 0x10 is discharging",
              ParseSinoBattery(discharging.data(), (int)discharging.size(), &lv, &ch) && ch == 0);

        // 前导 report ID 不影响锚点搜索。
        std::vector<uint8_t> lead(SINO_LEN + 1, 0);
        for (int i = 0; i < SINO_LEN; ++i) lead[i + 1] = ok[i];
        lv = -1; ch = -1;
        Check(L"Sino reply with a leading report ID still parses",
              ParseSinoBattery(lead.data(), (int)lead.size(), &lv, &ch) && lv == 92 && ch == 1);

        // 另一种实机应答只以 0A 为锚点，字段同样相对锚点。
        std::vector<uint8_t> shortAnchor(SINO_LEN, 0);
        shortAnchor[0] = 0x0A; shortAnchor[5] = 87; shortAnchor[6] = 0;
        lv = -1;
        Check(L"Sino 0A-only reply parses relative battery offset",
              ParseSinoBattery(shortAnchor.data(), (int)shortAnchor.size(), &lv, &ch) && lv == 87);

        // 旧版记录的 13 0A ... [6]=电量，可通过位于下标 1 的 0A 锚点兼容。
        std::vector<uint8_t> legacy(SINO_LEN, 0);
        legacy[0] = 0x13; legacy[1] = 0x0A; legacy[6] = 76;
        lv = -1;
        Check(L"Sino legacy 13 0A layout remains compatible",
              ParseSinoBattery(legacy.data(), (int)legacy.size(), &lv, &ch) && lv == 76);

        // 完整 13 4A 锚点优先于报文前部无关的 0A 字节。
        std::vector<uint8_t> twoAnchors(SINO_LEN + 4, 0);
        twoAnchors[0] = 0x0A; twoAnchors[5] = 33;
        twoAnchors[7] = 0x13; twoAnchors[8] = 0x4A;
        twoAnchors[12] = 76; twoAnchors[13] = 0x10;
        lv = -1; ch = -1;
        Check(L"Sino prefers the full 13 4A anchor over unrelated 0A",
              ParseSinoBattery(twoAnchors.data(), (int)twoAnchors.size(), &lv, &ch) &&
              lv == 76 && ch == 1);

        std::vector<uint8_t> ack = ok;
        ack[1] = 0x05;
        Check(L"Sino handshake ack is not a battery packet",
              !ParseSinoBattery(ack.data(), (int)ack.size(), &lv, &ch));

        std::vector<uint8_t> bad = ok;
        bad[5] = 200;
        Check(L"Sino rejects an out-of-range level",
              !ParseSinoBattery(bad.data(), (int)bad.size(), &lv, &ch));
        bad[5] = 0;
        Check(L"Sino rejects the zero offline placeholder",
              !ParseSinoBattery(bad.data(), (int)bad.size(), &lv, &ch));
        bad[5] = 1;
        Check(L"Sino rejects the one not-ready placeholder",
              !ParseSinoBattery(bad.data(), (int)bad.size(), &lv, &ch));
        Check(L"Sino rejects a packet truncated before the charging byte",
              !ParseSinoBattery(ok.data(), 6, &lv, &ch));
    }

    // ---------------------------------------------- 双模外设：有线 + 无线去重
    auto Dup = [](const wchar_t* transport, int level, int charging) {
        Reading r;
        r.key = transport;
        r.name = L"MCHOSE A5 V3 Pro";
        r.kind = Kind::Mouse;
        r.provider = L"mchose";
        r.transport = transport;
        r.level = level;
        r.charging = charging;
        return r;
    };
    {
        std::vector<Reading> v = {Dup(L"2.4G 接收器", 78, 0), Dup(L"USB 有线", 80, 1)};
        MergeWiredWirelessDuplicates(&v);
        Check(L"Dup wired+wireless same mouse merges to one row", v.size() == 1);
        Check(L"Dup keeps the charging row",
              v.size() == 1 && v[0].charging == 1 && v[0].level == 80);
    }
    {
        std::vector<Reading> v = {Dup(L"2.4G 接收器", 78, 0), Dup(L"2.4G 接收器", 55, 0)};
        MergeWiredWirelessDuplicates(&v);
        Check(L"Dup two wireless same-model devices stay separate", v.size() == 2);
    }
    {
        std::vector<Reading> v = {Dup(L"2.4G 接收器", 78, 0), Dup(L"USB 有线", 80, 1)};
        v[1].name = L"MCHOSE K99 V2";
        MergeWiredWirelessDuplicates(&v);
        Check(L"Dup different names are not merged", v.size() == 2);
    }
}

}  // namespace

int RunSelfTest(const std::wstring& reportPath) {
    g_total = 0;
    g_failed = 0;
    g_body.clear();

    RunCases();

    std::wstring text;
    text += L"TAP version 13\n";
    text += L"1.." + std::to_wstring(g_total) + L"\n";
    text += g_body;
    text += L"# " + std::to_wstring(g_total) + L" checks, " +
            std::to_wstring(g_failed) + L" failed\n";

    const std::string utf8 = WideToUtf8(text);
    if (!reportPath.empty()) {
        FILE* f = _wfopen(reportPath.c_str(), L"wb");
        if (f) {
            fwrite(utf8.data(), 1, utf8.size(), f);
            fclose(f);
        }
        // 同时写一个只含 0/1 的标记文件。批处理里判断 GUI 程序的退出码
        // 受 start /wait 的实现影响，用文件做判据不会出现"测试没跑却算通过"。
        FILE* rc = _wfopen((reportPath + L".rc").c_str(), L"wb");
        if (rc) {
            fputs(g_failed > 0 ? "1" : "0", rc);
            fclose(rc);
        }
    }

    // 从命令行直接运行时把结果也打到控制台（GUI 子系统默认没有控制台）。
    if (AttachConsole(ATTACH_PARENT_PROCESS) || GetConsoleWindow()) {
        FILE* con = nullptr;
        if (freopen_s(&con, "CONOUT$", "w", stdout) == 0 && con) {
            fwrite(utf8.data(), 1, utf8.size(), con);
            fflush(con);
        }
    }
    return g_failed > 0 ? 1 : 0;
}
