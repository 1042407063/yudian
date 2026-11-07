// main.cpp —— 入口、消息循环、扫描线程、配置
//
// 负责：单实例、配置读写、开机自启、扫描线程的节拍、窗口消息、低电量判定。
// 不负责：界面绘制（ui.cpp）、设备协议（providers.cpp）、HID 底层（hid.cpp）。
// 不变量：g_snapshot 只由扫描线程经 PostMessage 移交；配置写入必须是"整份替换"。
#include "app.h"
#include "hid.h"
#include "decode.h"

#include <gdiplus.h>
#include <shellapi.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>


using namespace Gdiplus;

std::vector<Reading> g_snapshot;
Config g_config;
HINSTANCE g_inst = nullptr;
HWND g_mainWnd = nullptr;

namespace {

const wchar_t* MAIN_CLASS = L"PeripheralBatteryMain";
const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* RUN_VALUE = L"PeripheralBattery";
const wchar_t* MUTEX_NAME = L"PeripheralBattery.SingleInstance";
const wchar_t* WINDOW_TITLE = L"余电";

HANDLE g_wakeEvent = nullptr;
HANDLE g_scanThread = nullptr;
std::atomic<bool> g_quit{false};
std::atomic<int> g_intervalSec{30};
std::atomic<bool> g_scanManual{false};   // 这一轮是用户点的刷新：面板要播增删动画

std::map<std::wstring, ULONGLONG> g_notified;

// ---------------------------------------------------------------- 配置

std::wstring ConfigDir() {
    wchar_t buf[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L".");
    dir += L"\\PeripheralBattery";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring ConfigPath() { return ConfigDir() + L"\\config.ini"; }

// 配置格式版本。没有这个键的文件按 0 处理（2026-10 之前的所有版本）。
// 加一条迁移规则 = 这里 +1，并在 MigrateConfig 里加一个分支。
constexpr int kConfigSchema = 1;

// 把旧配置整理成当前语义。返回值表示"内存里的值变了，应该落盘一次"。
// 注意：只做值与档位的归一，不碰 UI 状态。
bool MigrateConfig(Config& c, int fromSchema) {
    bool changed = (fromSchema < kConfigSchema);
    if (fromSchema < 1) {
        // 1) 小于 60 秒的值只可能来自旧版（旧默认 30 秒），按新默认 2 分钟算；
        if (c.interval < kIntervals[0]) {
            c.interval = 120;
            changed = true;
        }
        // 2) 手改过配置留下的非档位值吸到最近的档位，否则菜单里会出现"一个都没勾"。
        int best = kIntervals[0];
        for (int v : kIntervals) {
            if (std::abs(v - c.interval) < std::abs(best - c.interval)) best = v;
        }
        if (best != c.interval) {
            c.interval = best;
            changed = true;
        }
    }
    return changed;
}

// ---------------------------------------------------------------- 自启

// ---------------------------------------------------------------- 设备状态

struct MonitorState {
    std::map<std::wstring, Reading> devices;
    std::map<std::wstring, int> misses;

    // manual=true：用户主动点了刷新，这一轮没扫到的设备直接剔除（面板要播离场动画）；
    // manual=false：自动轮询给两次机会，避免蓝牙短暂重连就把设备删掉。
    void Scan(std::vector<Reading>* out, bool manual) {
        std::vector<Reading> fresh;
        MonitorScan(&fresh);
        ULONGLONG now = GetTickCount64();

        std::map<std::wstring, bool> seen;
        for (auto& r : fresh) {
            auto it = devices.find(r.key);
            MergeReading(&r, (it != devices.end()) ? &it->second : nullptr, now);
            devices[r.key] = r;
            misses[r.key] = 0;
            seen[r.key] = true;
        }

        const int limit = manual ? 1 : 2;
        for (auto it = devices.begin(); it != devices.end();) {
            if (seen.count(it->first)) {
                ++it;
                continue;
            }
            int m = ++misses[it->first];
            if (m >= limit) {
                misses.erase(it->first);
                it = devices.erase(it);
            } else {
                it->second.online = false;
                ++it;
            }
        }

        out->clear();
        for (auto& kv : devices) out->push_back(kv.second);

        auto kindRank = [](Kind k) {
            switch (k) {
                case Kind::Mouse: return 0;
                case Kind::Keyboard: return 1;
                case Kind::Controller: return 2;
                case Kind::Headset: return 3;
                default: return 4;
            }
        };
        std::sort(out->begin(), out->end(), [&](const Reading& a, const Reading& b) {
            if (a.online != b.online) return a.online;
            int ka = kindRank(a.kind), kb = kindRank(b.kind);
            if (ka != kb) return ka < kb;
            return a.name < b.name;
        });
    }
};

MonitorState g_monitor;
bool g_demoMode = false;   // --mock：用演示数据跑界面，方便无硬件时预览

std::vector<Reading> DemoDevices() {
    std::vector<Reading> list;
    auto add = [&](const wchar_t* name, Kind kind, const wchar_t* vendor,
                   const wchar_t* transport, int level, int charging,
                   const wchar_t* status, bool online) {
        Reading r;
        r.key = name;
        r.name = name;
        r.kind = kind;
        r.vendor = vendor;
        r.transport = transport;
        r.level = level;
        r.charging = charging;
        r.status = status;
        r.online = online;
        r.provider = L"demo";
        r.updated = GetTickCount64();
        list.push_back(r);
    };
    add(L"MX Master 3S", Kind::Mouse, L"罗技", L"蓝牙", 76, 0, L"放电中", true);
    add(L"MX Keys S", Kind::Keyboard, L"罗技", L"2.4G 接收器", 52, 0, L"放电中", true);
    add(L"DualSense 无线控制器", Kind::Controller, L"索尼", L"蓝牙", 18, 0, L"电量偏低", true);
    add(L"MCHOSE A5 Pro", Kind::Mouse, L"迈从", L"2.4G 接收器", 43, 0, L"放电中", true);
    add(L"飞智 八爪鱼 4", Kind::Controller, L"飞智", L"USB", 91, 1, L"充电中", true);
    add(L"G913 TKL 无线机械键盘", Kind::Keyboard, L"罗技", L"2.4G 接收器", 8, 0, L"电量极低", true);
    add(L"WH-1000XM5", Kind::Headset, L"索尼", L"蓝牙", 64, 0, L"放电中", true);
    return list;
}

std::vector<Reading> ScanReal() {
    std::vector<Reading> list;
    MonitorScan(&list);
    ULONGLONG now = GetTickCount64();
    for (auto& r : list) r.updated = now;
    return list;
}

int RunListToFile(const wchar_t* path) {
    std::vector<Reading> list;
    MonitorScan(&list);
    FILE* f = _wfopen(path, L"wb");
    if (!f) return 1;
    {
        DPI_AWARENESS_CONTEXT ctx = GetThreadDpiAwarenessContext();
        DPI_AWARENESS aw = GetAwarenessFromDpiAwarenessContext(ctx);
        int sysDpi = GetDpiForSystem();
        const wchar_t* awName = L"Unknown";
        switch (aw) {
            case DPI_AWARENESS_INVALID: awName = L"Invalid"; break;
            case DPI_AWARENESS_UNAWARE: awName = L"Unaware"; break;
            case DPI_AWARENESS_SYSTEM_AWARE: awName = L"SystemAware"; break;
            case DPI_AWARENESS_PER_MONITOR_AWARE: awName = L"PerMonitorAware"; break;
        }
        fprintf(f, "dpi=%d awareness=%s\n", sysDpi, WideToUtf8(awName).c_str());
    }
    fprintf(f, "devices=%d\n", (int)list.size());
    for (auto& d : list) {
        std::wstring line = d.name + L" | " + KindLabel(d.kind) + L" | " +
                            (d.level >= 0 ? std::to_wstring(d.level) + L"%" : L"--") + L" | " +
                            d.transport + L" | " + d.StatusText() + L" | " + d.provider;
        fprintf(f, "%s\n", WideToUtf8(line).c_str());
    }
    fclose(f);

    FILE* g = _wfopen(path, L"ab");
    if (g) {
        std::wstring diag = L"\n" + g_scanTiming + L"\n--- 罗技 HID++ 通道 ---\n" +
                            LogitechDiagnostics() + L"\n--- Compx / VGN 厂商集合 ---\n" +
                            VendorDiagnostics();
        fprintf(g, "%s", WideToUtf8(diag).c_str());
        fclose(g);
    }
    return 0;
}

// ---------------------------------------------------------------- 扫描线程

DWORD WINAPI ScanThreadProc(LPVOID) {
    bool first = true;
    while (!g_quit) {
        if (!first) {
            WaitForSingleObject(g_wakeEvent, (DWORD)g_intervalSec * 1000);
        }
        first = false;
        if (g_quit) break;
        ResetEvent(g_wakeEvent);
        bool manual = g_scanManual.exchange(false);

        auto* result = new std::vector<Reading>();
        if (g_demoMode) {
            *result = DemoDevices();
        } else {
            g_monitor.Scan(result, manual);
        }
        if (g_quit) {
            delete result;
            break;
        }
        if (!PostMessageW(g_mainWnd, WM_APP_SCAN_DONE, 0, (LPARAM)result)) {
            delete result;
            break;
        }
    }
    return 0;
}

// ---------------------------------------------------------------- 通知

void CheckLowBattery(const std::vector<Reading>& list) {
    if (!g_config.notifyLow) return;
    ULONGLONG now = GetTickCount64();
    for (auto& d : list) {
        // stale = 这一轮没真读到，百分比是沿用的旧值。拿它报警等于用几小时前
        // 的状态打扰用户；真低电会在设备应答的那一轮正常触发。
        if (!d.online || d.level < 0 || d.stale) continue;
        if (d.charging == 1 || d.level > g_config.lowThreshold) {
            g_notified.erase(d.key);
            continue;
        }
        auto it = g_notified.find(d.key);
        if (it == g_notified.end() || now - it->second > 30ULL * 60 * 1000) {
            g_notified[d.key] = now;
            // 文案由 UI 层组装：数字要单独染色，调用方不该关心排版
            UiShowToast(d.name.c_str(), d.level);
        }
    }
}

// ---------------------------------------------------------------- 窗口

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == taskbarCreated) {
        UiDestroy();
        UiCreate();
        UiApplyDevices(g_snapshot, false);
        return 0;
    }

    switch (msg) {
        case WM_TIMER:
            if (wp == 5) {                 // 系统菜单正在显示：给它补圆角
                UiRoundSystemMenus();
                return 0;
            }
            break;
        case WM_SETTINGCHANGE:
            // 系统切换深浅色时广播这条，lParam 是区段名。托盘程序可能常驻数天，
            // 不响应的话会一直是启动那一刻的配色。
            if (lp && lstrcmpiW(reinterpret_cast<LPCWSTR>(lp), L"ImmersiveColorSet") == 0) {
                UiRebuildTheme();
            }
            return 0;
        case WM_APP_TRAYMSG: {
            WORD ev = LOWORD(lp);
            if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK) {
                UiTogglePanel();
            } else if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
                POINT pt;
                GetCursorPos(&pt);
                UiShowMenu(pt);
            }
            return 0;
        }
        case WM_APP_SCAN_DONE: {
            auto* list = reinterpret_cast<std::vector<Reading>*>(lp);
            if (list) {
                UiApplyDevices(*list);
                CheckLowBattery(*list);
                delete list;
            }
            return 0;
        }
        case WM_APP + 4:      // 立即刷新（wp=1 表示全量探测）
            // 只置位、不清位：紧接着到达的普通刷新（wp=0）不能把已经排队的手动
            // 全量请求降级。旧写法是直接赋值，用户点了刷新又打开面板就丢了全量。
            if (wp != 0) {
                g_forceScan = true;
                g_scanManual = true;
            }
            if (g_wakeEvent) SetEvent(g_wakeEvent);
            return 0;
        case WM_APP + 5:      // 第二个实例要求显示面板
            UiTogglePanel();
            return 0;
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == IDM_SHOW) {
                UiTogglePanel();
            } else if (id == IDM_REFRESH) {
                UiRefreshNow(true);   // 走"手动刷新"路径：全量探测 + 增删动画
            } else if (id == IDM_AUTOSTART) {
                bool next = !AutostartGet();
                // 只有注册表真的写成功才更新配置和开关；否则会出现"显示已开启、
                // 重启后不自启"（组策略禁用 HKCU Run 时就是这样）。
                if (AutostartSet(next)) {
                    g_config.autostart = next;
                    g_config.Save();
                    UiSetAutostart(next);
                } else {
                    UiSetAutostart(AutostartGet());
                    UiShowToast(L"开机自启设置失败", -1);
                }
            } else if (id >= IDM_INTERVAL_BASE && id < IDM_INTERVAL_BASE + kIntervalCount) {
                int sec = kIntervals[id - IDM_INTERVAL_BASE];
                g_config.interval = sec;
                g_config.Save();
                g_intervalSec = sec;
                UiApplyDevices(g_snapshot, false);   // 只是重绘，不算新数据
                if (g_wakeEvent) SetEvent(g_wakeEvent);   // 新间隔立即生效
            } else if (id == IDM_QUIT) {
                DestroyWindow(hwnd);
            } else if (id == IDM_RESET_POS) {
                UiResetPanelPos();
            }
            return 0;
        }
        case WM_ENDSESSION:
            if (wp) DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            g_quit = true;
            if (g_wakeEvent) SetEvent(g_wakeEvent);
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

// providers 的扫描段落之间用它判断要不要提前收手（g_quit 是内部链接，导出成函数）
bool ScanCancelRequested() { return g_quit.load(); }

void Config::Load() {
    int schema = 0;
    FILE* f = _wfopen(ConfigPath().c_str(), L"rb");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = s.substr(0, eq);
        std::string val = s.substr(eq + 1);
        while (!val.empty() && (val.back() == '\n' || val.back() == '\r' || val.back() == ' '))
            val.pop_back();
        if (key == "schema") {
            schema = atoi(val.c_str());
        } else if (key == "interval") {
            int v = atoi(val.c_str());
            if (v >= 5 && v <= 3600) interval = v;
        } else if (key == "low_threshold") {
            int v = atoi(val.c_str());
            if (v >= 1 && v <= 100) lowThreshold = v;
        } else if (key == "notify_low") {
            notifyLow = (val == "1");
        } else if (key == "autostart") {
            autostart = (val == "1");
        } else if (key == "theme") {
            theme = Utf8ToWide(val);
        } else if (key.rfind("name.", 0) == 0) {
            aliases[Utf8ToWide(key.substr(5))] = Utf8ToWide(val);
        } else if (key == "panel_x") {
            panelX = atoi(val.c_str());
        } else if (key == "panel_y") {
            panelY = atoi(val.c_str());
        }
    }
    fclose(f);

    // 版本迁移集中在 MigrateConfig，这里只负责"要不要写回"。
    if (MigrateConfig(*this, schema)) Save();
}

void Config::Save() {
    // 先写一份完整的临时文件，再一次性替换正式文件。
    // 旧写法用 "wb" 直接打开正式文件：写到一半断电 / 崩溃，配置就只剩半截，
    // 刷新间隔、阈值、别名、面板位置全回到默认值。替换是原子的，失败时原文件完好。
    const std::wstring path = ConfigPath();
    const std::wstring tmp = path + L".tmp";

    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return;
    fprintf(f, "schema=%d\n", kConfigSchema);
    fprintf(f, "interval=%d\n", interval);
    fprintf(f, "low_threshold=%d\n", lowThreshold);
    fprintf(f, "notify_low=%d\n", notifyLow ? 1 : 0);
    fprintf(f, "autostart=%d\n", autostart ? 1 : 0);
    fprintf(f, "theme=%s\n", WideToUtf8(theme).c_str());
    fprintf(f, "panel_x=%d\n", panelX);
    fprintf(f, "panel_y=%d\n", panelY);
    for (const auto& kv : aliases) {
        fprintf(f, "name.%s=%s\n", WideToUtf8(kv.first).c_str(),
                WideToUtf8(kv.second).c_str());
    }
    const bool wrote = (fflush(f) == 0);
    if (fclose(f) != 0 || !wrote) {
        DeleteFileW(tmp.c_str());
        return;
    }

    // MOVEFILE_WRITE_THROUGH：改名本身也要落盘，不能只停留在缓存里。
    if (!MoveFileExW(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        // 杀毒 / 同步盘偶尔会短暂占用目标文件，退让一次再试；仍失败就保留 .tmp，
        // 下一次保存会覆盖它，正式文件始终是上一版完整内容。
        Sleep(50);
        MoveFileExW(tmp.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
}

std::wstring ExePath() {
    // GetModuleFileNameW 缓冲区不够时返回容量并置 ERROR_INSUFFICIENT_BUFFER，
    // 固定 MAX_PATH 会静默截断，写进注册表的就是一条跑不起来的命令。
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0) return std::wstring();
        if (n < (DWORD)buf.size() || buf.size() >= 32768) return std::wstring(buf.data(), n);
        buf.resize(buf.size() * 2);
    }
}

bool AutostartGet() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD size = sizeof(buf), type = 0;
    LONG rc = RegQueryValueExW(key, RUN_VALUE, nullptr, &type, (LPBYTE)buf, &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_SZ) return false;

    // 只认"指向当前 exe"的项：程序换过路径后，旧值不该让开关显示成"已开启"。
    std::wstring v = buf;
    if (v.size() >= 2 && v.front() == L'"') {
        size_t e = v.find(L'"', 1);
        v = (e == std::wstring::npos) ? v.substr(1) : v.substr(1, e - 1);
    }
    std::wstring exe = ExePath();
    return !exe.empty() && lstrcmpiW(v.c_str(), exe.c_str()) == 0;
}

bool AutostartSet(bool enable) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;
    bool ok = false;
    if (enable) {
        std::wstring cmd = L"\"" + ExePath() + L"\"";
        ok = RegSetValueExW(key, RUN_VALUE, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                            (DWORD)((cmd.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    } else {
        LONG rc = RegDeleteValueW(key, RUN_VALUE);
        ok = (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND);   // 本来就没有也算关掉
    }
    RegCloseKey(key);
    return ok;
}

int CurrentInterval() { return (int)g_intervalSec; }

const wchar_t* KindLabel(Kind k) {
    switch (k) {
        case Kind::Mouse: return L"鼠标";
        case Kind::Keyboard: return L"键盘";
        case Kind::Controller: return L"手柄";
        case Kind::Headset: return L"耳机";
        default: return L"外设";
    }
}

std::wstring Reading::StatusText() const {
    if (!online) return L"休眠 / 未连接";
    if (charging == 1) return L"充电中";
    if (level < 0) return status.empty() ? L"电量不可读" : status;
    if (IsCritical()) return L"电量极低";
    if (IsLow()) return L"电量偏低";
    return status.empty() ? L"放电中" : status;
}

std::wstring Reading::AgeText() const {
    ULONGLONG now = GetTickCount64();
    ULONGLONG base = sampledAt ? sampledAt : updated;   // 优先真实采样时刻
    ULONGLONG delta = (now > base) ? (now - base) / 1000 : 0;
    if (delta < 60) return L"刚刚";
    if (delta < 3600) return std::to_wstring(delta / 60) + L" 分钟前";
    return std::to_wstring(delta / 3600) + L" 小时前";
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    g_inst = inst;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    GdiplusStartupInput gdiInput;
    ULONG_PTR gdiToken = 0;
    GdiplusStartup(&gdiToken, &gdiInput, nullptr);

    // 自检要在读配置之前结束：它是构建门禁，不该碰 %APPDATA% 里的任何文件。
    {
        int earlyArgc = 0;
        LPWSTR* earlyArgv = CommandLineToArgvW(GetCommandLineW(), &earlyArgc);
        if (earlyArgv) {
            bool selftest = (earlyArgc >= 2 && std::wstring(earlyArgv[1]) == L"--selftest");
            std::wstring out = (earlyArgc >= 3) ? std::wstring(earlyArgv[2]) : L"selftest.txt";
            LocalFree(earlyArgv);
            if (selftest) {
                int rc = RunSelfTest(out);
                GdiplusShutdown(gdiToken);
                return rc;
            }
        }
    }

    g_config.Load();
    g_intervalSec = g_config.interval;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 2 && std::wstring(argv[1]) == L"--mock") {
        g_demoMode = true;
    }
    if (argc >= 3) {
        std::wstring cmd = argv[1];
        if (cmd == L"--shot" || cmd == L"--shot-dark") {
            bool dark = (cmd == L"--shot-dark");
            bool demo = false;
            bool empty = false;
            std::vector<Reading> data;
            if (argc >= 4) {
                std::wstring mode = argv[3];
                if (mode == L"demo") demo = true;
                if (mode == L"dark") dark = true;
                if (mode == L"demo-dark") { demo = true; dark = true; }
                if (mode == L"empty") empty = true;
                if (mode == L"scanning") {
                    UiSetScanningPreview(true);
                    bool ok = UiRenderShot(ScanReal(), dark, argv[2]);
                    UiSetScanningPreview(false);
                    LocalFree(argv);
                    GdiplusShutdown(gdiToken);
                    return ok ? 0 : 2;
                }
                if (mode == L"armed") {
                    demo = true;
                    UiSetQuitPreview(true);
                }
                if (mode == L"multi") {
                    demo = true;
                    UiSetScrollPreview(2);
                }
            }
            data = demo ? DemoDevices() : (empty ? std::vector<Reading>() : ScanReal());
            bool ok = UiRenderShot(data, dark, argv[2]);
            LocalFree(argv);
            GdiplusShutdown(gdiToken);
            return ok ? 0 : 2;
        }
        if (cmd == L"--shot-tray" && argc >= 3) {
            int rc = RenderTrayStrip(argv[2]);
            LocalFree(argv);
            GdiplusShutdown(gdiToken);
            return rc;
        }
        if (cmd == L"--shot-seq" && argc >= 3) {
            int frames = argc >= 4 ? _wtoi(argv[3]) : 4;
            int gap = argc >= 5 ? _wtoi(argv[4]) : 600;
            int rc = RenderShotSequence(ScanReal(), false, argv[2], frames, gap);
            LocalFree(argv);
            GdiplusShutdown(gdiToken);
            return rc;
        }
        if (cmd == L"--dump" && argc >= 3) {
            FILE* f = _wfopen(argv[2], L"wb");
            if (f) {
                fprintf(f, "%s", WideToUtf8(DumpHidInterfaces()).c_str());
                fclose(f);
            }
            LocalFree(argv);
            GdiplusShutdown(gdiToken);
            return 0;
        }
        if (cmd == L"--ble" && argc >= 3) {
            FILE* f = _wfopen(argv[2], L"wb");
            if (f) {
                fprintf(f, "%s", WideToUtf8(BleDiagnostics()).c_str());
                fclose(f);
            }
            LocalFree(argv);
            GdiplusShutdown(gdiToken);
            return 0;
        }
        if (cmd == L"--bench" && argc >= 3) {
            FILE* f = _wfopen(argv[2], L"wb");
            if (f) {
                for (int i = 0; i < 5; ++i) {
                    ULONGLONG t0 = GetTickCount64();
                    std::vector<Reading> v;
                    MonitorScan(&v);
                    ULONGLONG dt = GetTickCount64() - t0;
                    fprintf(f, "scan %d: %llu ms, devices=%d\n", i + 1,
                            (unsigned long long)dt, (int)v.size());
                    fflush(f);
                }
                fclose(f);
            }
            LocalFree(argv);
            GdiplusShutdown(gdiToken);
            return 0;
        }
        if (cmd == L"--shot-toast") {
            bool dk = (argc >= 4 && std::wstring(argv[3]) == L"dark");
            int rc = UiRenderToast(argv[2], dk);
            LocalFree(argv);
            GdiplusShutdown(gdiToken);
            return rc;
        }
        if (cmd == L"--make-ico") {
            bool ok = UiWriteAppIcon(argv[2]);
            LocalFree(argv);
            GdiplusShutdown(gdiToken);
            return ok ? 0 : 2;
        }
        if (cmd == L"--raw") {
            FILE* f = _wfopen(argv[2], L"wb");
            if (f) {
                fprintf(f, "%s", WideToUtf8(RawDump()).c_str());
                fclose(f);
            }
            LocalFree(argv);
            GdiplusShutdown(gdiToken);
            return 0;
        }
        if (cmd == L"--vendor") {
            FILE* f = _wfopen(argv[2], L"wb");
            if (f) {
                fprintf(f, "%s", WideToUtf8(VendorDiagnostics()).c_str());
                fclose(f);
            }
            LocalFree(argv);
            GdiplusShutdown(gdiToken);
            return 0;
        }
        if (cmd == L"--list") {
            int rc = RunListToFile(argv[2]);
            LocalFree(argv);
            GdiplusShutdown(gdiToken);
            return rc;
        }
    }
    if (argv) LocalFree(argv);

    HANDLE mutex = CreateMutexW(nullptr, TRUE, MUTEX_NAME);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(MAIN_CLASS, nullptr);
        if (existing) PostMessageW(existing, WM_APP + 5, 0, 0);
        return 0;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = inst;
    wc.lpszClassName = MAIN_CLASS;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    RegisterClassExW(&wc);

    g_mainWnd = CreateWindowExW(0, MAIN_CLASS, WINDOW_TITLE, 0, 0, 0, 0, 0,
                                nullptr, nullptr, inst, nullptr);
    if (!g_mainWnd) return 1;

    g_intervalSec = g_config.interval;
    UiCreate();
    UiSetAutostart(AutostartGet());

    g_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_scanThread = CreateThread(nullptr, 0, ScanThreadProc, nullptr, 0, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_quit = true;
    if (g_wakeEvent) SetEvent(g_wakeEvent);
    DWORD waited = WAIT_OBJECT_0;
    if (g_scanThread) {
        // 一次扫描本来就可能超过 1.5 秒（README 记录过首次全量 1.8 秒），所以给足
        // 10 秒，并且**检查结果**。旧实现等 1.5 秒就 CloseHandle 继续销毁 UI/GDI+，
        // 而扫描线程那时还在跑，会访问已经销毁的对象。
        waited = WaitForSingleObject(g_scanThread, 10000);
        if (waited == WAIT_OBJECT_0) CloseHandle(g_scanThread);
    }
    if (g_wakeEvent) CloseHandle(g_wakeEvent);

    if (waited != WAIT_OBJECT_0) {
        // 线程卡在驱动里没回来（HidD_GetFeature / WriteFile 不可取消）。这时销毁
        // UI 与 GDI+ 会被它踩到，直接结束进程比带着竞态清理更安全。
        // 注意：这里不用 TerminateThread —— 那会留下未释放的锁和半更新的堆。
        ExitProcess(0);
    }

    UiDestroy();
    if (mutex) CloseHandle(mutex);
    GdiplusShutdown(gdiToken);
    return 0;
}
