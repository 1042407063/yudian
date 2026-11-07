// app.h —— 公共类型与全局声明
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
// GDI+ 需要 PROPID（WIN32_LEAN_AND_MEAN 下不会自动带入）
#include <objidl.h>
#include <propidl.h>

#include <atomic>
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <cwchar>

// 安全的宽字符拷贝（不依赖 *_s 系列，MinGW 下更稳）
inline void CopyW(wchar_t* dst, size_t cap, const wchar_t* src) {
    if (!dst || cap == 0) return;
    if (!src) {
        dst[0] = 0;
        return;
    }
    size_t n = wcslen(src);
    if (n >= cap) n = cap - 1;
    wmemcpy(dst, src, n);
    dst[n] = 0;
}

enum class Kind {
    Mouse,
    Keyboard,
    Controller,
    Headset,
    Other,
};

const wchar_t* KindLabel(Kind k);

// 读不到电量时"为什么读不到"。界面文案仍由 status / detail 决定，
// 这个代号只用来统计和断言：它能被计数、能被比较，中文句子不能。
enum class Outcome {
    Ok = 0,       // 读到了电量
    NoReply,      // 设备没应答（休眠 / 未连接 / 不在位）
    Busy,         // 接口打不开（被官方驱动或别的软件独占）
    BadFrame,     // 有应答但内容不对（格式 / 校验 / 越界）
    Unsupported,  // 这条链路或这台设备不提供电量
};

struct Reading {
    std::wstring key;
    std::wstring name;
    std::wstring vendor;
    std::wstring transport;
    std::wstring status;
    std::wstring detail;
    std::wstring provider;
    Kind kind = Kind::Other;
    Outcome outcome = Outcome::Ok;
    int level = -1;        // -1 表示不可读
    int voltage = 0;       // mV
    int charging = -1;     // -1 未知 / 0 放电 / 1 充电
    bool online = true;
    ULONGLONG updated = 0;     // 最后一次"这一轮看到它"的时间（grace / 离场判断用）
    ULONGLONG sampledAt = 0;   // 最后一次真正读到数值的时间（"X 分钟前"用它）
    // true = 当前显示的是缓存值（设备休眠、或 BLE 这一轮没刷新），不是本轮采样。
    // UI 据此把年龄显示出来，避免把旧值当新值。
    bool stale = false;

    bool IsCritical() const { return level >= 0 && level <= 10; }
    bool IsLow() const { return level > 10 && level <= 20; }
    bool IsMid() const { return level > 20 && level <= 50; }
    std::wstring StatusText() const;
    std::wstring AgeText() const;
};

// 刷新间隔档位（秒）。菜单、配置归一、落盘校验共用这一份。
// 30 秒档已经取消：鼠标满电能用几十小时，一个百分点要几十分钟才掉，
// 30 秒轮询拿不到任何新信息，只是白白多唤醒一次 USB / 2.4G。
inline constexpr int kIntervals[] = {60, 120, 300, 600, 1800};
inline constexpr int kIntervalCount = (int)(sizeof(kIntervals) / sizeof(kIntervals[0]));

struct Config {
    int interval = 120;              // 默认 2 分钟，见 kIntervals
    int lowThreshold = 20;
    bool notifyLow = true;
    bool autostart = false;
    std::wstring theme = L"auto";   // auto / light / dark
    // 面板被拖动后的卡片左上角（物理像素）；-1 = 还没拖过，贴右下角
    int panelX = -1;
    int panelY = -1;
    // 设备别名：键是 "VID:PID"（四位十六进制大写），值是显示名。
    // 有些接收器只报通用名（迈从 41E4:2001 只报 "MCHOSE 2.4G Wireless"），
    // 型号得由用户指定。
    std::map<std::wstring, std::wstring> aliases;

    void Load();
    void Save();
};

// 由 UI 线程持有。扫描线程只产出一份新快照，经 PostMessage 把所有权交过来，
// 除了 UiApplyDevices（以及 --shot 的预览渲染）之外都只读。
extern std::vector<Reading> g_snapshot;
extern Config g_config;
extern HINSTANCE g_inst;
extern HWND g_mainWnd;

// 托盘 / 面板
void UiCreate();
void UiDestroy();
// 面板可见时，增删设备会播过渡动画；面板收起时直接换列表（打开时已是最终状态）
void UiApplyDevices(const std::vector<Reading>& list, bool newData = true);
void UiTogglePanel();
void UiHidePanel();
bool UiPanelVisible();
bool UiWriteAppIcon(const wchar_t* path);
int UiRenderToast(const wchar_t* path, bool dark);
void UiSetScanningPreview(bool on);   // 仅供截图预览扫描中状态
void UiSetQuitPreview(bool on);       // 仅供截图预览「退出」的二次确认状态
void UiSetScrollPreview(int row);     // 仅供截图预览列表滚动
bool UiRenderShot(const std::vector<Reading>& demo, bool dark, const wchar_t* path);
int RenderTrayStrip(const wchar_t* path);
int RenderShotSequence(const std::vector<Reading>& demo, bool dark,
                       const wchar_t* prefix, int frames, int gapMs);
void UiSetAutostart(bool on);
void UiShowMenu(POINT pt);
void UiRoundSystemMenus();   // 给正在显示的系统菜单补圆角（菜单弹出期间定时调用）
void UiShowToast(const wchar_t* title, int level);
void UiRefreshNow(bool full = false);
void UiResetPanelPos();       // 面板回到默认右下角（右键菜单用）

// 主题
bool IsDarkTheme();
void UiRebuildTheme();

// 扫描
void MonitorScan(std::vector<Reading>* out);
// 把一条新扫到的记录并入已有状态：休眠占位保留最后一次有效读数并标 stale。
// 单独暴露是为了能脱离硬件做纯函数测试（见 review 验证脚本）。
void MergeReading(Reading* incoming, const Reading* old, ULONGLONG now);
int CurrentInterval();          // 当前刷新间隔（秒），全局唯一数据源
std::wstring LogitechDiagnostics();
std::wstring VendorDiagnostics();   // VGN / VXE / ATK 这类 Compx 接收器的原始帧
std::wstring DumpHidInterfaces();
std::wstring BleDiagnostics();      // BLE（GATT 0x180F）诊断，--ble 用
extern std::wstring g_scanTiming;
extern std::atomic<bool> g_forceScan;   // 置 true 后下一次扫描做全量探测
std::wstring RawDump();

// 配置
bool AutostartSet(bool enable);
bool AutostartGet();
std::wstring ExePath();

// 扫描线程的退出标志：providers 在耗时的扫描段落之间用它检查，好让退出尽快生效。
// 用函数而不是 extern atomic：g_quit 定义在 main.cpp 的匿名命名空间里（内部链接）。
bool ScanCancelRequested();

// 消息
#define WM_APP_SCAN_DONE (WM_APP + 1)
#define WM_APP_TRAY      (WM_APP + 2)
#define WM_APP_TRAYMSG   (WM_APP + 3)

#define TRAY_UID 0x51

enum MenuId {
    IDM_SHOW = 100,
    IDM_REFRESH,
    IDM_AUTOSTART,
    IDM_QUIT,
    IDM_RESET_POS,
    IDM_INTERVAL_BASE = 200,   // + 索引
};
