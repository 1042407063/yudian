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
