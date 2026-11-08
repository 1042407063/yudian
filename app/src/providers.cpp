// providers.cpp —— 所有读取链路：罗技 HID++ / 索尼 / 厂商私有协议 / 标准 HID / BLE
//
// 负责：把 HidInfo 变成 Reading；解析各厂商报文；判断"这台设备归哪条链路"。
// 不负责：线程、定时、配置、界面排版（那些在 main.cpp / ui.cpp）。
// 不变量：只读设备，不写入任何配置或状态；同一物理设备一轮只产出一条 Reading。
#include "app.h"
#include "hid.h"
#include "decode.h"
#include "logitech_powermodel.h"   // G913 TKL 官方充放电曲线（取自 G HUB 资源）

#include <setupapi.h>
#include <bthledef.h>

#include <algorithm>
#include <map>
#include <string>
#include <cwctype>
#include <cstring>
#include <cstdio>
#include <set>
#include <vector>

// 用户手动刷新时置位，下一次扫描做全量探测（跨文件可见）
std::atomic<bool> g_forceScan{false};

namespace {

// ---------------------------------------------------------------- 链路契约
//
// 加一款设备 = 文件末尾的 kProviders 里加一行 + 写一个 ScanXxx。
// 签名统一成 ScanCtx，是为了让"这一轮扫什么"可以整包传下去，
// 而不是每个函数各自去拿一遍全局状态。
struct ProviderDesc;

struct ScanCtx {
    const std::vector<HidInfo>* all;
    // path -> 认领它的链路。兜底链路只处理不在这张表里的接口。
    const std::map<std::wstring, const ProviderDesc*>* owner;
    bool fullSweep;
};

struct ProviderDesc {
    const wchar_t* id;
    const wchar_t* label;                    // 诊断里的中文名
    bool (*claims)(const HidInfo&);          // nullptr = 不按设备认领
    bool hidEnum;                            // true = 参与 HID 认领分派
    void (*scan)(const ScanCtx&, std::vector<Reading>*);
};

constexpr uint16_t LOGITECH_VID = 0x046D;
constexpr uint16_t SONY_VID = 0x054C;

constexpr uint8_t FEAT_ROOT = 0x00;
constexpr uint8_t FUNC_GET_FEATURE = 0x00;
constexpr uint8_t FUNC_GET_PROTOCOL = 0x01;
constexpr uint16_t FEAT_DEVICE_NAME = 0x0005;
constexpr uint16_t FEAT_BATTERY_LEVEL_STATUS = 0x1000;
constexpr uint16_t FEAT_BATTERY_VOLTAGE = 0x1001;
// 官方 charging_time_slots 的稳定判定电压：到 4100mV 以上才启动涓流计时。
constexpr int kLogiTrickleStartMv = 4100;

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

bool ContainsNoCase(const std::wstring& hay, const wchar_t* needle) {
    return Lower(hay).find(Lower(needle)) != std::wstring::npos;
}

// 物理设备标识。优先用 ContainerId：它由 Windows 按物理设备分配，一台复合设备的
// 所有 HID 集合共享同一个值，而同型号的两台设备各不相同——这正是"面板一行 = 一台
// 设备"需要的粒度。序列号作次选，最后才退回 VID:PID（旧行为，同型号会并成一条）。
std::wstring DeviceIdentity(const HidInfo& d) {
    if (!d.containerId.empty()) {
        std::wstring cid = Lower(d.containerId);
        // Windows 对"没有容器"的设备给这个全零哨兵值（实测某指纹设备就是它）。
        // 它不区分设备，拿来当键会把多台设备并成一台，必须当作无效值。
        if (cid != L"{00000000-0000-0000-ffff-ffffffffffff}") return L"cid:" + cid;
    }
    if (!d.serial.empty()) return L"sn:" + Lower(d.serial);
    wchar_t b[40];
    swprintf(b, 40, L"vidpid:%04X:%04X", d.vid, d.pid);
    return b;
}

std::wstring TransportOf(const std::wstring& path, const std::wstring& product) {
    std::wstring p = Lower(path);
    if (p.find(L"bthenum") != std::wstring::npos ||
        p.find(L"00001124") != std::wstring::npos ||
        p.find(L"00001812") != std::wstring::npos) {
        return L"蓝牙";
    }
    if (ContainsNoCase(product, L"receiver") || ContainsNoCase(product, L"接收器"))
        return L"2.4G 接收器";
    return L"USB";
}

Kind ClassifyName(const std::wstring& name) {
    std::wstring n = Lower(name);
    const wchar_t* mice[] = {L"mouse", L"mice", L"mx master", L"mx anywhere",
                             L"superlight", L"g304", L"g102", L"g502", L"g703",
                             L"g903", L"g pro wireless", L"lift", L"鼠标"};
    const wchar_t* kb[] = {L"keyboard", L"mx keys", L"g913", L"g915", L"g610",
                           L"g512", L"k380", L"k855", L"pop keys", L"craft", L"键盘"};
    const wchar_t* hs[] = {L"headset", L"g733", L"g435", L"zone", L"耳机"};
    for (auto w : mice) if (n.find(w) != std::wstring::npos) return Kind::Mouse;
    for (auto w : kb) if (n.find(w) != std::wstring::npos) return Kind::Keyboard;
    for (auto w : hs) if (n.find(w) != std::wstring::npos) return Kind::Headset;
    return Kind::Other;
}

// 0x0005 报的是完整市场名。G913 TKL 会回 58 字符的
// "G913 TKL LIGHTSPEED Wireless RGB Mechanical Gaming Keyboard"，面板里只剩省略号。
// 在 " LIGHTSPEED" 处截断，保留能认出的型号；不含这个词的设备名原样返回。
std::wstring ShortDeviceName(const std::wstring& name) {
    size_t pos = Lower(name).find(L" lightspeed");
    if (pos == std::wstring::npos) return name;
    std::wstring s = name.substr(0, pos);
    while (!s.empty() && s.back() == L' ') s.pop_back();
    return s.empty() ? name : s;
}

std::wstring StripSuffix(const std::wstring& path, const wchar_t* marker) {
    std::wstring lower = Lower(path);
    size_t pos = lower.find(marker);
    if (pos == std::wstring::npos) return lower;
    return lower.substr(0, pos);
}

// ------------------------------------------------------------------ HID++

struct HppReply {
    bool ok = false;
    int error = -1;
    uint8_t feature = 0;
    uint8_t params[16] = {0};
    int paramLen = 0;
    uint8_t raw[32] = {0};
    int rawLen = 0;
};

class HppLink {
public:
    bool Open(const std::wstring& path) {
        if (!dev_.Open(path)) return false;
        // 试试短报文接口，写失败说明只支持长报文
        uint8_t probe[7] = {0x10, 0xFF, 0x00, 0x00, 0, 0, 0};
        if (dev_.Write(probe, sizeof(probe)) < 0) longMode_ = true;
        return true;
    }
    void Close() { dev_.Close(); }
    bool IsLongMode() const { return longMode_; }

    bool Request(uint8_t devIdx, uint8_t feature, uint8_t func,
                 const uint8_t* params, int nparams, HppReply* out, int timeoutMs) {
        if (!dev_.valid()) return false;
        swid_ = (uint8_t)((swid_ + 1) % 15);
        uint8_t swid = swid_;
        DWORD len = longMode_ ? 20 : 7;
        std::vector<uint8_t> pkt(len, 0);
        pkt[0] = longMode_ ? 0x11 : 0x10;
        pkt[1] = devIdx;
        pkt[2] = feature;
        pkt[3] = (uint8_t)((func << 4) | swid);
        for (int i = 0; i < nparams && i < (longMode_ ? 16 : 3); ++i) {
            pkt[4 + i] = params ? params[i] : 0;
        }

        // 清掉上一轮残留，避免错配
        uint8_t tmp[64];
        for (int i = 0; i < 6; ++i) {
            if (dev_.Read(tmp, sizeof(tmp), 1) <= 0) break;
        }

        if (dev_.Write(pkt.data(), (DWORD)pkt.size()) < 0) return false;

        ULONGLONG deadline = GetTickCount64() + timeoutMs;
        while (GetTickCount64() < deadline) {
            int n = dev_.Read(tmp, sizeof(tmp), 100);
            if (n < 4) continue;
            if (tmp[0] != 0x10 && tmp[0] != 0x11) continue;
            if (tmp[1] != devIdx && tmp[1] != 0xFF) continue;

            if (tmp[2] == 0xFF || tmp[2] == 0x8F) {
                // 错误帧: [id, devIdx, 0x8F, 地址, 功能|swid, 错误码, 0]
                out->ok = false;
                out->error = (n > 5) ? tmp[5] : 1;
                out->rawLen = std::min(n, 32);
                for (int k = 0; k < out->rawLen; ++k) out->raw[k] = tmp[k];
                return true;
            }
            if (tmp[2] == feature && tmp[3] == ((func << 4) | swid)) {
                out->ok = true;
                out->feature = feature;
                out->paramLen = std::min<int>(n - 4, 16);
                for (int i = 0; i < out->paramLen; ++i) out->params[i] = tmp[4 + i];
                out->rawLen = std::min(n, 32);
                for (int k = 0; k < out->rawLen; ++k) out->raw[k] = tmp[k];
                return true;
            }
        }
        return false;   // 超时
    }

    // 注意：规范规定特性 ID 是小端，但实测部分接收器（如 G304 的 C53F）按大端解析。
    // 这里先按小端试，失败再按大端试，并按设备记住结果。
    int GetFeatureIndexOrder(uint8_t devIdx, uint16_t featureId, int timeoutMs, bool bigEndian) {
        uint8_t p[2];
        p[0] = bigEndian ? (uint8_t)(featureId >> 8) : (uint8_t)(featureId & 0xFF);
        p[1] = bigEndian ? (uint8_t)(featureId & 0xFF) : (uint8_t)(featureId >> 8);
        HppReply r;
        if (!Request(devIdx, FEAT_ROOT, FUNC_GET_FEATURE, p, 2, &r, timeoutMs)) return -1;
        if (!r.ok || r.paramLen < 1) return -1;
        return r.params[0] ? r.params[0] : -1;
    }

    int GetFeatureIndex(uint8_t devIdx, uint16_t featureId, int timeoutMs) {
        auto it = orderCache_.find(devIdx);
        if (it != orderCache_.end()) {
            return GetFeatureIndexOrder(devIdx, featureId, timeoutMs, it->second == 1);
        }
        int ix = GetFeatureIndexOrder(devIdx, featureId, timeoutMs, false);
        if (ix > 0) {
            orderCache_[devIdx] = 0;
            return ix;
        }
        ix = GetFeatureIndexOrder(devIdx, featureId, timeoutMs, true);
        if (ix > 0) {
            orderCache_[devIdx] = 1;
            return ix;
        }
        return -1;
    }

    std::wstring DeviceName(uint8_t devIdx, uint8_t featIdx, int timeoutMs) {
        HppReply r;
        if (!Request(devIdx, featIdx, 0x00, nullptr, 0, &r, timeoutMs)) return L"";
        if (!r.ok || r.paramLen < 1) return L"";
        int count = r.params[0];
        if (count <= 0) return L"";

        // 两种固件实现：
        //   a) 返回分片数（<=8），参数是分片序号
        //   b) 返回字符串字节长度（>8），参数是字节偏移
        bool byOffset = count > 8;
        int chunks = byOffset ? (count + 15) / 16 : count;
        if (chunks > 16) return L"";

        std::string raw;
        for (int i = 0; i < chunks; ++i) {
            uint8_t arg = byOffset ? (uint8_t)(i * 16) : (uint8_t)i;
            HppReply c;
            if (!Request(devIdx, featIdx, 0x01, &arg, 1, &c, timeoutMs)) break;
            if (!c.ok) break;
            raw.append(reinterpret_cast<char*>(c.params), c.paramLen);
        }
        size_t zero = raw.find('\0');
        if (zero != std::string::npos) raw.resize(zero);
        std::wstring w = Utf8ToWide(raw);
        while (!w.empty() && (w.back() == L' ' || w.back() == L'\r' || w.back() == L'\n'))
            w.pop_back();
        return w;
    }

    // 返回 false 表示两种电量特性都不支持。
    // 0x1000 给的是设备自报百分比；0x1001 只有大端电压 + 充电标志（G913 TKL
    // 这类 G 系列键盘压根没有 0x1000），此时用罗技电压曲线估算百分比并置 estimated。
    // useOfficialCurve = true 时用 G913 TKL 的官方充放电曲线（G HUB PowerModel），
    // 否则退回内核通用曲线。两条曲线差得很远：4133mV 充电时官方 83%、通用表 97%。
    bool Battery(uint8_t devIdx, int* level, int* voltage, int* charging,
                 std::wstring* status, bool* estimated, bool useOfficialCurve) {
        bool any = false;
        *estimated = false;
        int fi = GetFeatureIndex(devIdx, FEAT_BATTERY_LEVEL_STATUS, 200);
        if (fi > 0) {
            HppReply r;
            if (Request(devIdx, (uint8_t)fi, 0x00, nullptr, 0, &r, 200) && r.ok && r.paramLen >= 2) {
                int raw = r.params[0];
                if (raw != 0xFF) {
                    if (raw > 100) {
                        int masked = raw & 0x7F;
                        *level = (masked <= 100) ? masked : -1;
                    } else {
                        *level = raw;
                    }
                }
                int st = r.params[1];
                switch (st) {
                    case 0: *status = L"放电中"; *charging = 0; break;
                    case 1: *status = L"充电中"; *charging = 1; break;
                    case 2: *status = L"充电中"; *charging = 1; break;
                    case 3: *status = L"已充满"; *charging = 1; break;
                    case 4: *status = L"涓流充电"; *charging = 1; break;
                    case 5: *status = L"电池异常"; break;
                    case 6: *status = L"温度异常"; break;
                    default: break;
                }
                any = true;
            }
        }
        int fv = GetFeatureIndex(devIdx, FEAT_BATTERY_VOLTAGE, 200);
        if (fv > 0) {
            HppReply r;
            if (Request(devIdx, (uint8_t)fv, 0x00, nullptr, 0, &r, 200) && r.ok && r.paramLen >= 2) {
                // 实测 G913 TKL（046D:C547，1 号通道）func0 回帧：
                //   11 01 07 0s  0F 6C 00 00 ...
                //   0F 6C = 3948 mV（大端），第 3 字节 = 充电标志。
                // 旧实现按小端读成 0x6C0F = 27663 mV，被范围检查丢掉，于是整台
                // 设备被标成「未提供电量接口」——这正是读不到键盘电量的原因。
                int mv = DecodeLogitechVoltage(r.params[0], r.params[1]);
                if (mv > 0) {
                    *voltage = mv;
                    any = true;

                    // 标志位（openlogi / Logi Options+ 的 0x1001 布局）：
                    //   bit7 外接电源；bit0..2 外接电源时的充电状态
                    //   0=充电中 1=已充满 2=未充电 3..7=未知；bit5 电量极低。
                    // 必须先解标志再算电量：full 要用来做充满校准。
                    // 实测 G913 TKL 插线后回帧 10 25 80 = 4133 mV + flags 0x80。
                    // 充电状态下的端电压不可信，上层会改用时间模型覆盖这个数。
                    bool extPower = false, full = false;
                    if (r.paramLen >= 3) {
                        uint8_t flags = r.params[2];
                        extPower = (flags & 0x80) != 0;
                        full = extPower && (flags & 0x07) == 1;
                        // 0x1000 已经给了状态时以它为准，这里只补空白。
                        if (status->empty()) {
                            if (!extPower) {
                                *status = L"放电中";
                                *charging = 0;
                            } else {
                                switch (flags & 0x07) {
                                    case 0:  *status = L"充电中";   *charging = 1;  break;
                                    case 1:  *status = L"已充满";   *charging = 1;  break;
                                    case 2:  *status = L"未充电";   *charging = 0;  break;
                                    default: *status = L"外接电源"; *charging = -1; break;
                                }
                            }
                        }
                    }
                    if (*level < 0) {
                        int pct;
                        if (full) {
                            pct = 100;   // 充满校准：G HUB 也把充满对齐到 100%
                        } else if (useOfficialCurve) {
                            // 官方充电/放电是两条独立曲线，按状态分别查表。
                            pct = extPower ? LogitechChargingPercent(mv)
                                           : LogitechDischargePercent(mv);
                        } else {
                            pct = LogitechBatteryPercent(mv, false);
                        }
                        if (pct >= 0) {
                            *level = pct;
                            *estimated = true;
                        }
                    }
                }
            }
        }
        return any;
    }

private:
    HidDevice dev_;
    bool longMode_ = false;
    uint8_t swid_ = 0;
    std::map<int, int> orderCache_;   // 设备通道 -> 特性 ID 字节序（0=小端 1=大端）
};

struct LogitechCache {
    std::map<std::wstring, std::wstring> names;
    std::map<std::wstring, Kind> kinds;
    std::set<std::wstring> nameOk;   // 名字已成功读过，不必再问设备
};

LogitechCache g_logiCache;

// 上一轮有响应的通道、以及每个接口下能用的报文集合。
// 接收器一般只有一个集合会应答、只有 1~2 个通道挂着设备，
// 每轮都盲扫 6 通道 × 2 集合完全是浪费——实测占了扫描 90% 的时间。
std::map<std::wstring, std::set<int>> g_liveIdx;
std::map<std::wstring, std::wstring> g_liveColl;
int g_scanTick = 0;
bool g_fullSweep = true;

// 估算电量的平滑状态。key 用归一化后的型号名，不是接口路径：G913 TKL 插上
// 数据线后会从接收器 C547 重新枚举成有线 C343，接口身份变了但电池还是那颗。
struct LevelState {
    int level;              // 上次显示的百分比
    uint64_t at;            // 上次更新时间
    bool charging;          // 上次是否处于充电
    uint64_t trickleStart;  // 进入官方涓流阶段的时刻，0 = 未进入
    int trickleBase;        // 进入涓流时的显示值
};
std::map<std::wstring, LevelState> g_logiLevelState;

void ScanLogitech(const std::vector<HidInfo>& all, std::vector<Reading>* out) {
    // 全量扫描时让名字缓存失效：同一接收器通道解绑旧鼠标、配对新鼠标后，
    // 电量属于新设备但面板会一直显示旧型号，直到重启。只清 nameOk（"已读到过"），
    // 保留 names 里最后一次的名字，休眠行仍然有名字可显示。
    if (g_fullSweep) g_logiCache.nameOk.clear();

    // 同一接口下可能有短报文(0x0001)和长报文(0x0002)两个集合，
    // 部分接收器只在长报文集合上响应 HID++ 2.0，所以两个都要试。
    std::map<std::wstring, std::vector<HidInfo>> byRoot;
    for (auto& d : all) {
        if (d.vid != LOGITECH_VID) continue;
        if (d.usagePage != 0xFF00) continue;
        if (d.usage != 0x0001 && d.usage != 0x0002) continue;
        std::wstring root = StripSuffix(d.path, L"&col");
        byRoot[root].push_back(d);
    }

    for (auto& pair : byRoot) {
        std::wstring baseKey = pair.first;
        auto colls = pair.second;
        std::sort(colls.begin(), colls.end(), [](const HidInfo& a, const HidInfo& b) {
            return a.usage > b.usage;   // 0x0002 长报文优先
        });
        bool receiver = ContainsNoCase(colls.front().product, L"receiver") ||
                        ContainsNoCase(colls.front().product, L"接收器");

        std::vector<int> indices;
        if (receiver) {
            for (int i = 1; i <= 6; ++i) indices.push_back(i);
        } else {
            indices.push_back(0xFF);
        }

        std::map<int, Reading> merged;   // 通道 -> 最佳读数
        std::map<int, bool> silent;      // 通道 -> 是否静默

        // 先把上次能用的集合排到前面
        const std::wstring& goodColl = g_liveColl[baseKey];
        std::sort(colls.begin(), colls.end(), [&](const HidInfo& a, const HidInfo& b) {
            bool ga = (a.path == goodColl), gb = (b.path == goodColl);
            if (ga != gb) return ga;
            return a.usage > b.usage;
        });

        // 先扫上一轮有响应的通道；只有全量扫描才补扫其余通道
        std::set<int>& live = g_liveIdx[baseKey];
        std::vector<int> order;
        for (int i : indices)
            if (live.count(i)) order.push_back(i);
        if (g_fullSweep) {
            for (int i : indices)
                if (!live.count(i)) order.push_back(i);
        }
        if (order.empty()) order = indices;   // 从没成功过，只能全扫

        for (size_t ci = 0; ci < colls.size(); ++ci) {
            auto& coll = colls[ci];
            HppLink link;
            if (!link.Open(coll.path)) continue;
            int answeredHere = 0;
            // 全量扫描时另一个集合只需探一个通道，确认它是否变成了能用的那个
            std::vector<int> probeList = order;
            if (g_fullSweep && ci > 0 && !probeList.empty()) {
                probeList.resize(1);
            }
            for (int idxI : probeList) {
                uint8_t idx = (uint8_t)idxI;
                std::wstring key = L"logitech:" + baseKey + L":" + std::to_wstring(idxI);

                HppReply probe;
                // 冷启动（还没确认过任何通道）给更长的窗口：刚从休眠边缘醒来的设备
                // 常常要 100~250 ms 才回包，超时一次就整台设备漏掉一轮。确认过通道后
                // 回到 100 ms，免得每 8 轮的例行全扫把时间都耗在空通道上。
                int tmo = (!live.empty() || g_scanTick > 1) ? (receiver ? 100 : 150) : 250;
                bool answered = link.Request(idx, FEAT_ROOT, FUNC_GET_PROTOCOL, nullptr, 0,
                                             &probe, tmo);
                if (!answered) {
                    if (silent.find(idxI) == silent.end()) silent[idxI] = true;
                    continue;
                }
                // 错误帧（0x08 = 该通道没有配对设备）说明这里**没有**设备，
                // 不能记成"确认存在过"。旧实现先把 idx 塞进 live 再看 probe.ok，
                // 于是从未有设备的通道也会在下一轮超时时凭空生成一行"设备休眠"。
                if (!probe.ok) {
                    if (probe.error == 0x08) {
                        live.erase(idxI);
                        silent[idxI] = true;
                        g_logiCache.nameOk.erase(key);
                        g_logiCache.names.erase(key);
                        g_logiCache.kinds.erase(key);
                    }
                    continue;
                }
                silent[idxI] = false;   // 有有效应答
                answeredHere++;
                live.insert(idxI);

                // 设备名一次读到就不再重读：每次要发 3 个分片请求，
                // 设备慢的时候每个都可能吃到 200ms 超时，把扫描拖到几百毫秒。
                // 名字要放在电量之前读：官方曲线只适用于 G913/G915 TKL，得先知道型号。
                std::wstring name;
                bool nameResolved = g_logiCache.nameOk.count(key) > 0;
                if (nameResolved) {
                    name = g_logiCache.names[key];
                } else {
                    int fiName = link.GetFeatureIndex(idx, FEAT_DEVICE_NAME, 200);
                    if (fiName > 0)
                        name = ShortDeviceName(link.DeviceName(idx, (uint8_t)fiName, 200));
                    if (!name.empty()) g_logiCache.nameOk.insert(key);
                }
                if (name.empty()) {
                    auto ni = g_logiCache.names.find(key);
                    if (ni != g_logiCache.names.end() && !ni->second.empty()) {
                        name = ni->second;
                    } else {
                        name = receiver ? (L"罗技设备 " + std::to_wstring(idxI) + L" 号通道")
                                        : (coll.product.empty() ? L"罗技设备" : coll.product);
                    }
                }

                Kind kind = ClassifyName(name);
                g_logiCache.names[key] = name;
                g_logiCache.kinds[key] = kind;

                // G913 TKL / G915 TKL 用 G HUB 的官方 PowerModel 曲线，其余罗技设备
                // 退回内核通用曲线。4133mV 充电时两者差 14 点（83% vs 97%）。
                bool useOfficial = ContainsNoCase(name, L"g913") ||
                                   ContainsNoCase(name, L"g915");

                int level = -1, voltage = 0, charging = -1;
                bool estimated = false;
                std::wstring status;
                bool hasBattery = link.Battery(idx, &level, &voltage, &charging,
                                               &status, &estimated, useOfficial);
                if (hasBattery && level < 0 && voltage == 0) hasBattery = false;

                Reading r;
                // 有线/无线切换时 USB 身份会变（C547 接收器 → C343 有线直连）。
                // 用设备自报的型号名归一化，插线前后才是同一行、同一份历史读数，
                // 面板不会闪出两行，平滑状态也不会被丢掉。占位名不含型号，退回接口路径。
                bool realName = !name.empty() &&
                                name.find(L"罗技设备") == std::wstring::npos;
                std::wstring identity = realName ? (L"model:" + Lower(name)) : baseKey;
                r.key = L"logitech:" + identity + L":" + std::to_wstring(idxI);
                r.name = name;
                r.kind = kind;
                r.vendor = L"罗技";
                if (receiver) {
                    r.transport = L"2.4G 接收器";
                } else {
                    std::wstring tp = TransportOf(coll.path, coll.product);
                    r.transport = (tp == L"USB") ? L"USB 有线" : tp;
                }
                r.level = level;
                r.voltage = voltage;
                r.charging = charging;
                r.status = hasBattery ? status : L"未提供电量接口";
                r.approximate = estimated;
                r.officialCurve = estimated && useOfficial;
                r.provider = L"logitech";
                if (level < 0) r.outcome = hasBattery ? Outcome::NoReply : Outcome::Unsupported;
                if (!hasBattery) r.detail = L"设备已响应，但未暴露电量特性，通常是设备正在休眠";
                // G913 官方 PowerModel：放电查 <discharge>，充电查 <charging>。
                // 充电曲线只到 4.20V = 91%，之后按官方 equation_coefficiency=150
                // 推进（电压稳定在 4100mV 以上才启动），配合限速平滑收尾。
                // 0x1000 自报的精确百分比不参与这套逻辑。
                if (r.level >= 0) {
                    uint64_t nowMs = GetTickCount64();
                    bool nowCharging = (r.charging == 1);
                    bool fullCharged = (r.status == L"已充满");
                    auto sit = g_logiLevelState.find(r.key);
                    if (!estimated || sit == g_logiLevelState.end()) {
                        g_logiLevelState[r.key] = {r.level, nowMs, nowCharging, 0, r.level};
                    } else {
                        LevelState& s = sit->second;
                        if (!nowCharging) s.trickleStart = 0;   // 拔线：退出涓流
                        s.charging = nowCharging;

                        int target = r.level;
                        if (nowCharging) {
                            if (fullCharged) {
                                target = 100;   // 设备自报充满，直接对齐
                            } else if (voltage >= kLogiTrickleStartMv) {
                                if (s.trickleStart == 0) {
                                    s.trickleStart = nowMs;
                                    s.trickleBase = r.level;   // 从官方曲线值起步
                                }
                                int trickle = LogitechTrickleLevel(
                                    s.trickleBase, nowMs - s.trickleStart);
                                target = std::max(r.level, trickle);
                            }
                        }
                        uint64_t elapsed = nowMs >= s.at ? nowMs - s.at : 0;
                        r.level = SmoothEstimatedLevel(s.level, target, elapsed);
                        s.level = r.level;
                        s.at = nowMs;
                    }
                }

                if (estimated) {
                    // 百分比来自电压换算：G913 用 G HUB 官方 PowerModel（充放电两条曲线），
                    // 其余罗技设备用内核通用曲线。写清来源，别让人以为是设备自报。
                    wchar_t buf[160];
                    swprintf(buf, 160,
                             L"%ls电压 %d mV（%ls）· %ls；设备只报电压，不提供百分比",
                             useOfficial ? L"" : L"≈ ",
                             voltage,
                             charging == 1 ? L"充电中" : L"放电中",
                             useOfficial ? L"G HUB 官方充放电曲线"
                                         : L"罗技通用电压曲线");
                    r.detail = buf;
                } else if (level < 0 && voltage > 0) {
                    wchar_t buf[64];
                    swprintf(buf, 64, L"仅电压 %d mV", voltage);
                    r.detail = buf;
                }
                merged[idxI] = r;   // 有效读数覆盖之前的占位
            }
            link.Close();
            if (answeredHere > 0) {
                g_liveColl[baseKey] = coll.path;
                if (!g_fullSweep) break;   // 已经读到设备，另一个集合不必再试
            }
        }

        for (int idxI : indices) {
            auto it = merged.find(idxI);
            if (it != merged.end()) {
                out->push_back(it->second);
                continue;
            }
            // 有静默通道 = 接收器上挂了设备但处于休眠。
            // 接收器：只有确认存在过的通道才出占位，否则每台接收器都会凭空
            // 多出 5 台"幽灵设备"（超时被误读成休眠）。
            // 有线直连（非接收器）只有一个通道，超时就是设备在休眠，必须出占位，
            // 否则插着线时键盘睡一下，面板里就整行消失。
            std::wstring probeKey = L"logitech:" + baseKey + L":" + std::to_wstring(idxI);
            bool known = live.count(idxI) > 0 ||
                         g_logiCache.names.find(probeKey) != g_logiCache.names.end();
            if (silent[idxI] && (known || !receiver)) {
                Reading r;
                auto ni = g_logiCache.names.find(probeKey);
                r.name = (ni != g_logiCache.names.end())
                             ? ni->second
                             : (receiver ? (L"罗技无线设备（" + std::to_wstring(idxI) + L" 号通道）")
                                         : (colls.front().product.empty()
                                                ? L"罗技设备" : colls.front().product));
                // 用和正常读数相同的归一化 key，休眠占位才能接上历史电量，
                // 唤醒后也不会在面板里闪成两行。
                bool realName = !r.name.empty() &&
                                r.name.find(L"罗技设备") == std::wstring::npos;
                r.key = L"logitech:" +
                        (realName ? (L"model:" + Lower(r.name)) : baseKey) + L":" +
                        std::to_wstring(idxI);
                auto ki = g_logiCache.kinds.find(probeKey);
                r.kind = (ki != g_logiCache.kinds.end()) ? ki->second : ClassifyName(r.name);
                r.vendor = L"罗技";
                r.transport = receiver ? L"2.4G 接收器" : L"USB 有线";
                r.status = L"设备休眠";
                r.detail = L"移动鼠标或按键唤醒后可读取电量";
                r.provider = L"logitech";
                r.outcome = Outcome::NoReply;
                out->push_back(r);
            }
        }
    }
}

// C542 这一代 Nano 接收器（M185 / M186 / M220 等）不实现 HID++，
// 也没有任何可读的电量通道——连特性报表都读不出来。
// 照样列一条：让用户看到"程序认到了，是这代硬件不上报"，
// 而不是以为程序没检测到设备。名字只能写到接收器，鼠标型号问不出来。
void ScanLogitechNoHidpp(const std::vector<HidInfo>& all, std::vector<Reading>* out) {
    static const uint16_t kNoHidpp[] = {0xC542};
    uint16_t foundPid = 0;
    for (const auto& d : all) {
        if (d.vid != LOGITECH_VID) continue;
        for (uint16_t pid : kNoHidpp) {
            if (d.pid == pid) foundPid = pid;
        }
    }
    if (!foundPid) return;

    // 万一哪天有固件通过 0xFF00 暴露了 HID++，就不要重复列
    for (const auto& d : all) {
        if (d.vid == LOGITECH_VID && d.pid == foundPid && d.usagePage == 0xFF00) return;
    }

    Reading r;
    r.key = L"logitech-nohidpp:" + std::to_wstring(foundPid);
    r.name = L"罗技 Nano 接收器";
    r.kind = Kind::Other;
    r.vendor = L"罗技";
    r.transport = L"2.4G 接收器";
    r.level = -1;
    r.charging = -1;
    r.status = L"硬件不支持读数";
    r.outcome = Outcome::Unsupported;
    r.detail = L"C542 接收器没有 HID++ 通道（M185 / M186 / M220 这一代），"
               L"鼠标不上报电量，只能看底部的低电量指示灯";
    r.provider = L"logitech-info";
    out->push_back(r);
}

// ------------------------------------------------------------------ 国产外设厂商协议
// 协议全部来自公开实现，只发读取命令，不配对、不写设置、不刷固件。
//   Compx 2.4G（VGN / VXE / ATK / 达尔优 / G-Wolves / Pulsar）
//     https://github.com/k0te1ch/powerpeek  src/battery/CompxProtocol.*
//     —— 社区逆向并在 VGN 蜻蜓 F1 MOBA 真机验证过的版本，本文按它实现
//     https://github.com/Fan4Metal/ATK_tray / mouse_tray
//   Nordic 54L15          : mouse_tray/chipset/nordic54.py
//   MCHOSE (Realtek)      : mouse_tray/chipset/realtek.py（被动接收推送，不发 nudge）

namespace {

// ---------------------------------------------------------------- 迈从 MCHOSE
// 实测两代协议，按接口形状自动分流：
//   V1 —— 20 字节报告集合（usage 0xFF02/0x0002）：写 report 0x13 + 0x4A 取电量，
//          应答末字节 = 前 19 字节求和。0x41E4 的键盘接收器走这条（实测 2.4G 可读）。
//   V2 —— 64 字节**输出报告** 0x4D（ASCII 'M'）：帧头 4D 01 01，命令 0x0900 取设备状态，
//          应答里带 VID/PID、连接方式、充电状态、电量。0x3837 的 A5 V3 接收器走这条。
//          写用同步句柄、读用异步句柄；用一个句柄既写又读，输入报告会静默。
//   旧 —— 0x5253 的 L7 Pro：被动接收 0x13/0x1D 推送，整帧 XOR 0xFF。
constexpr uint16_t kMchoseVids[] = {0x5253, 0x3837, 0x41E4, 0x2023};
constexpr int MCHOSE_V1_LEN = 20;
constexpr int MCHOSE_V2_LEN = 64;

bool IsMchoseVid(uint16_t vid) {
    for (uint16_t v : kMchoseVids) {
        if (v == vid) return true;
    }
    return false;
}

// 型号名可能被改成任意贴牌，VID 对不上时用产品名兜底
bool LooksLikeMchose(const HidInfo& d) {
    if (IsMchoseVid(d.vid)) return true;
    std::wstring s = Lower(d.product + L" " + d.manufacturer);
    return s.find(L"mchose") != std::wstring::npos ||
           s.find(L"迈从") != std::wstring::npos ||
           s.find(L"maicong") != std::wstring::npos;
}

bool IsNordic54(uint16_t vid, uint16_t pid) { return vid == 0x373B && pid == 0x1155; }

// ---------------------------------------------------------------- Compx 2.4G

// 这套接收器有几个必须踩过才知道的点：
//  1. 同一颗鼠标会暴露好几个 HID 集合，只有 usage 0xFF02/0x0002 且输入输出都是
//     17 字节的那个厂商集合能写。以前按 VID/PID 取"第一个匹配"，很可能拿到鼠标
//     集合（8 字节），写 17 字节必然失败——这正是 VGN 读不出电量的主因。
//  2. 帧尾是校验：整帧求和 == 0x55；回复也必须校验，否则噪声会被当成电量。
//  3. 回复第 2 字节是状态，非 0 表示接收器后面没有鼠标；第 6 字节电量、
//     第 7 字节是否插线、第 8~9 字节电压（大端，mV）。
//  4. 型号由接收器自己在 0x10 命令里报（customer/model），比 USB PID 准：
//     VGN 整个 F1 系列共用一个接收器 PID，靠 PID 分不出 Pro / MOBA / F1 S。
constexpr uint8_t COMPX_ID = 0x08;
constexpr uint8_t COMPX_CMD_BATTERY = 0x04;
constexpr uint8_t COMPX_CMD_MODEL = 0x10;
constexpr int COMPX_LEN = 17;
constexpr uint8_t COMPX_SUM = 0x55;

// Compx 自家 VID，以及同族贴牌用过的 VID
constexpr uint16_t kCompxVids[] = {0x3554, 0x373B, 0x260D, 0x33E4};

bool IsCompxVendor(uint16_t vid) {
    for (uint16_t v : kCompxVids) {
        if (v == vid) return true;
    }
    return false;
}

// 型号表：接收器上报的 customer/model -> 名字（出自 powerpeek 的真机记录）
struct CompxModelName {
    int customer;
    int model;
    const wchar_t* name;
};

const CompxModelName kCompxModelNames[] = {
    // customer 2 = VGN / VXE 这条线
    {2, 1, L"VGN 蜻蜓 F1 Pro"},
    {2, 2, L"VGN 蜻蜓 F1 Pro"},
    {2, 23, L"VGN 蜻蜓 F1 Pro"},
    {2, 24, L"VGN 蜻蜓 F1 Pro"},
    {2, 3, L"VGN 蜻蜓 F1 Pro Max"},
    {2, 4, L"VGN 蜻蜓 F1 Pro Max"},
    {2, 25, L"VGN 蜻蜓 F1 Pro Max"},
    {2, 26, L"VGN 蜻蜓 F1 Pro Max"},
    {2, 48, L"VGN 蜻蜓 F1 Pro Max"},
    {2, 52, L"VGN 蜻蜓 F1 Pro Max"},
    {2, 5, L"VGN 蜻蜓 F1 MOBA"},
    {2, 6, L"VGN 蜻蜓 F1 MOBA"},
    {2, 33, L"VGN 蜻蜓 F1 MOBA"},
    {2, 19, L"VGN 蜻蜓 F1 S"},
    {2, 20, L"VGN 蜻蜓 F1 S"},
    {2, 21, L"VGN 蜻蜓 F1 S"},
    {2, 22, L"VGN 蜻蜓 F1 S"},
    {2, 11, L"VXE R1"},
    {2, 12, L"VXE R1"},
    {2, 27, L"VXE R1 Pro Max"},
    {2, 32, L"VXE R1 SE+"},
    {2, 39, L"ATK X1 Pro Max"},
    // customer 1 = ATK 这条线
    {1, 8, L"ATK F1 Ultimate 2.0"},
    {1, 31, L"ATK A9 Mini+"},
    {1, 52, L"ATK A9 Mini+"},
};

const wchar_t* CompxModelNameOf(int customer, int model) {
    for (const auto& m : kCompxModelNames) {
        if (m.customer == customer && m.model == model) return m.name;
    }
    return nullptr;
}

// PID 表只在型号还没读到时兜底用，读不到也不影响电量
struct CompxPidName {
    uint16_t vid;
    uint16_t pid;
    const wchar_t* name;
};

const CompxPidName kCompxPidNames[] = {
    {0x373B, 0x1031, L"ATK F1 Ultimate"},
    {0x373B, 0x102E, L"ATK F1 Ultimate"},
    {0x373B, 0x11D9, L"ATK A9 Ultimate"},
    {0x373B, 0x11B6, L"ATK A9 Ultimate"},
    {0x373B, 0x104D, L"VXE MAD R"},
    {0x373B, 0x103F, L"VXE MAD R"},
    {0x373B, 0x1040, L"VXE MAD R Major Plus"},
    {0x373B, 0x104C, L"VXE MAD R Major Plus"},
    {0x3554, 0xF58A, L"VXE R1 Pro Max"},
    {0x3554, 0xF58C, L"VXE R1 Pro Max"},
    {0x3554, 0xF58E, L"VXE R1 SE+"},
    {0x3554, 0xF58F, L"VXE R1 SE+"},
    {0x3554, 0xF503, L"VGN 蜻蜓 F1 Pro"},
    {0x3554, 0xF502, L"VGN 蜻蜓 F1 Pro"},
    {0x3554, 0xFB3E, L"VGN 蜻蜓 F2 Pro Max"},
    {0x3554, 0xFB3D, L"VGN 蜻蜓 F2 Pro Max"},
    {0x3554, 0xFB56, L"VGN 蜻蜓 F2 Master+"},
    {0x3554, 0xFB57, L"VGN 蜻蜓 F2 Master+"},
    {0x3554, 0xF508, L"Pulsar X2 V2 Mini"},
    {0x3554, 0xF507, L"Pulsar X2 V2 Mini"},
    {0x3554, 0xF524, L"Zaopin Z2 Mini"},
    {0x3554, 0xF526, L"Zaopin Z2 Mini"},
    {0x3554, 0xF5F7, L"Scyrox V8"},
    {0x3554, 0xF5F6, L"Scyrox V8"},
    {0x260D, 0x1175, L"达尔优 A950 Air"},
    {0x260D, 0x1193, L"达尔优 A950 Air"},
    {0x260D, 0x1154, L"达尔优 A950 Wing"},
    {0x260D, 0x1196, L"达尔优 A950 Wing"},
    {0x33E4, 0x3854, L"G-Wolves Lycan"},
    {0x33E4, 0x4719, L"G-Wolves Lycan"},
};

const wchar_t* CompxPidNameOf(uint16_t vid, uint16_t pid) {
    for (const auto& m : kCompxPidNames) {
        if (m.vid == vid && m.pid == pid) return m.name;
    }
    return nullptr;
}

std::wstring StripReceiverSuffix(std::wstring s) {
    static const wchar_t* kSuffixes[] = {
        L" 4k receiver",    L" 8k receiver",       L" 4k dongle",
        L" 8k dongle",      L" 2.4g wireless receiver", L" 2.4ghz receiver",
        L" 2.4g receiver",  L" wireless receiver", L" receiver",
        L" dongle",         L" 接收器",            L" 无线接收器",
    };
    std::wstring low = Lower(s);
    for (const wchar_t* suf : kSuffixes) {
        size_t n = wcslen(suf);
        if (low.size() >= n && low.compare(low.size() - n, n, suf) == 0) {
            s = s.substr(0, s.size() - n);
            break;
        }
    }
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    return s;
}

// "CompX VGN Dragonfly 4K Receiver" -> "VGN Dragonfly"
std::wstring CleanProductName(const std::wstring& product) {
    std::wstring s = StripReceiverSuffix(product);
    static const wchar_t* kPrefixes[] = {L"compx ", L"comp_x ", L"pixart "};
    std::wstring low = Lower(s);
    for (const wchar_t* p : kPrefixes) {
        size_t n = wcslen(p);
        if (low.size() > n && low.compare(0, n, p) == 0) {
            s = s.substr(n);
            break;
        }
    }
    while (!s.empty() && s.back() == L' ') s.pop_back();
    return s;
}

std::wstring CompxBrand(const std::wstring& name, const std::wstring& product) {
    std::wstring s = Lower(name + L" " + product);
    auto has = [&](const wchar_t* k) { return s.find(k) != std::wstring::npos; };
    if (has(L"vgn") || has(L"蜻蜓")) return L"VGN";
    if (has(L"vxe")) return L"VXE";
    if (has(L"atk")) return L"ATK";
    if (has(L"达尔优") || has(L"dareu")) return L"达尔优";
    if (has(L"g-wolves") || has(L"狼")) return L"G-Wolves";
    if (has(L"pulsar")) return L"Pulsar";
    if (has(L"zaopin")) return L"Zaopin";
    if (has(L"scyrox")) return L"Scyrox";
    return L"Compx";
}

// 接收器的厂商集合：0xFF02/0x0002，17 字节进出
bool IsCompxCandidate(const HidInfo& d) {
    if (d.usagePage != 0xFF02 || d.usage != 0x0002) return false;
    if (d.inputLen < COMPX_LEN || d.outputLen < COMPX_LEN) return false;
    if (IsCompxVendor(d.vid)) return true;
    // 贴牌换了 VID：产品名里会带品牌，集合形状一致就按同族处理
    std::wstring p = Lower(d.product);
    static const wchar_t* kBrands[] = {L"vgn", L"vxe", L"atk", L"compx",
                                       L"pulsar", L"dragonfly", L"蜻蜓"};
    for (const wchar_t* b : kBrands) {
        if (p.find(b) != std::wstring::npos) return true;
    }
    return false;
}

void CompxBuild(uint8_t cmd, uint8_t* f) {
    for (int i = 0; i < COMPX_LEN; ++i) f[i] = 0;
    f[0] = COMPX_ID;
    f[1] = cmd;
    unsigned sum = 0;
    for (int i = 0; i < COMPX_LEN - 1; ++i) sum += f[i];
    f[COMPX_LEN - 1] = (uint8_t)(COMPX_SUM - sum);
}

bool CompxChecksumOk(const uint8_t* f) {
    unsigned sum = 0;
    for (int i = 0; i < COMPX_LEN; ++i) sum += f[i];
    return (sum & 0xFF) == COMPX_SUM;
}

// 一次问答。接收器大约每 30 次请求会丢一次，所以丢包重发，不靠加长等待——
// 实测正常回包 1~23 ms，等 120 ms 已经足够。
bool CompxAsk(HidDevice& dev, uint8_t cmd, uint8_t* reply) {
    if (!dev.valid()) return false;

    uint8_t buf[128];
    int readLen = std::max<int>(COMPX_LEN, dev.info().inputLen);
    if (readLen > (int)sizeof(buf)) readLen = (int)sizeof(buf);
    for (int i = 0; i < 4; ++i) {          // 清掉打开前积压的报告
        if (dev.Read(buf, readLen, 1) <= 0) break;
    }

    uint8_t req[COMPX_LEN];
    CompxBuild(cmd, req);
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (dev.Write(req, COMPX_LEN) < 0) return false;
        ULONGLONG deadline = GetTickCount64() + 120;
        while (GetTickCount64() < deadline) {
            int n = dev.Read(buf, readLen, 60);
            if (n < COMPX_LEN) continue;
            if (buf[0] != COMPX_ID || buf[1] != cmd) continue;
            if (!CompxChecksumOk(buf)) continue;
            memcpy(reply, buf, COMPX_LEN);
            return true;                   // 校验通过的应答是最终结果，不再重试
        }
    }
    return false;
}

struct CompxBattery {
    int level = -1;
    bool onCable = false;
    int millivolts = 0;
    bool valid = false;
};

bool ParseCompxBattery(const uint8_t* f, CompxBattery* out) {
    if (f[2] != 0) return false;                  // 状态非 0：接收器后面没有鼠标
    int pct = f[6];
    int mv = (f[8] << 8) | f[9];
    if (pct > 100) return false;
    if (pct == 0 && mv == 0) return false;        // 0% + 0mV = 空数据，不是没电
    out->level = pct;
    out->onCable = f[7] != 0;
    out->millivolts = mv;
    out->valid = true;
    return true;
}

bool ParseCompxModel(const uint8_t* f, int* customer, int* model) {
    if (f[2] != 0 || f[5] < 2) return false;
    if (f[6] == 0 && f[7] == 0) return false;
    *customer = f[6];
    *model = f[7];
    return true;
}

struct CompxState {
    int customer = -1;
    int model = -1;
    bool modelTried = false;
    int modelAttempts = 0;
    bool answered = false;    // 本进程内成功读到过电量
};

std::map<std::wstring, CompxState> g_compxStates;
std::map<std::wstring, std::wstring> g_compxInterface;   // 接口根 -> 上次应答的集合

// Nordic 54L15：64 字节，回包 byte1==0x72 且 byte5==0x07 才算有效，无线时电量在 byte7
bool ReadNordic54(const std::wstring& path, int* level, bool* wired) {
    HidDevice dev;
    if (!dev.Open(path)) return false;
    std::vector<uint8_t> req(64, 0);
    req[0] = 0x08;
    req[1] = 0x7D;   // 无线
    req[2] = 0x72;
    req[3] = 0x02;
    req[4] = 0x00;
    req[5] = 0x01;
    req[6] = 0x07;
    req[7] = 0x01;
    if (dev.Write(req.data(), 64) < 0) return false;

    uint8_t buf[128] = {0};
    int n = dev.Read(buf, 128, 300);
    if (n < 8) return false;
    if (buf[1] != 0x72 || buf[5] != 0x07) return false;
    int pct = buf[7];
    if (pct < 0 || pct > 100) return false;
    *level = pct;
    *wired = false;
    return true;
}

struct VendorBattery {
    int level = -1;
    int charging = -1;      // -1 未知 / 0 放电 / 1 充电
    bool full = false;
    int transport = 0;      // V2: 1 有线 / 2 2.4G / 3 蓝牙；0 未知
    bool valid = false;
    // 应答是否通过协议校验（V2 的 XOR / V1 的求和）。为 false 且 valid 为 true
    // 说明走的是"回退解析"：旧固件算法与文档不一致时的兜底路径。
    bool checksumVerified = false;
};

// ---------------- V1：20 字节报告集合，report 0x13 + 0x4A

// 应答布局（实测 0x41E4 键盘接收器）：
//   13 4A 01 00 02 5E 01 00 ... 总和
//   [1]=0x4A 命令  [4]=数据长度  [5]=电量百分比  [6]=状态  [19]=前 19 字节求和
bool MchoseV1Ask(HidDevice& dev, uint8_t* reply) {
    if (!dev.valid()) return false;

    uint8_t buf[128];
    int readLen = std::max<int>(MCHOSE_V1_LEN, dev.info().inputLen);
    if (readLen > (int)sizeof(buf)) readLen = (int)sizeof(buf);
    for (int i = 0; i < 4; ++i) {           // 清掉打开前积压的报告
        if (dev.Read(buf, readLen, 1) <= 0) break;
    }

    uint8_t req[MCHOSE_V1_LEN] = {0};
    req[0] = 0x13;
    req[1] = 0x4A;
    req[2] = 0x01;
    // 请求本身不带校验：实测接收器接受全零尾部，加校验反而会被当成长度以外的东西

    for (int attempt = 0; attempt < 3; ++attempt) {
        if (dev.Write(req, MCHOSE_V1_LEN) < 0) return false;
        ULONGLONG deadline = GetTickCount64() + 200;
        while (GetTickCount64() < deadline) {
            int n = dev.Read(buf, readLen, 80);
            if (n < MCHOSE_V1_LEN) continue;
            if (buf[0] != 0x13 || buf[1] != 0x4A) continue;
            memcpy(reply, buf, MCHOSE_V1_LEN);
            return true;
        }
    }
    return false;
}

bool ParseMchoseV1(const uint8_t* f, VendorBattery* out) {
    if (f[0] != 0x13 || f[1] != 0x4A) return false;
    if (f[4] < 1) return false;                       // 数据长度不对，不是电量应答
    int pct = f[5];
    if (pct < 0 || pct > 100) return false;
    unsigned sum = 0;
    for (int i = 0; i < MCHOSE_V1_LEN - 1; ++i) sum += f[i];
    if ((uint8_t)sum != f[MCHOSE_V1_LEN - 1]) return false;

    out->level = pct;
    // 状态字节：0/1 = 未插线放电，2 = 充电中，3 = 已充满
    switch (f[6]) {
        case 2: out->charging = 1; break;
        case 3: out->charging = 1; out->full = true; break;
        default: out->charging = 0; break;
    }
    out->valid = true;
    out->checksumVerified = true;      // V1 的求和校验在上面已经过了
    return true;
}

// ---------------- V2：64 字节输出报告 0x4D，命令 0x0900

// 帧结构（照 MCHOSE 网页 HUB 的实现）：
//   [0]=0x4D 报文 ID  [1]=版本  [2]=标志  [3]=数据长度 N
//   [4..5]=命令（小端）  [6]=BizCode  [7]=序号  [8..]=数据
//   [8+N]=异或校验，覆盖 [2]..[7+N]
// outLen 必须是接口的 OutputReportByteLength：WriteFile 的字节数不对会直接 87。
// 返回 false 表示数据段放不下（畸形入参），调用方应放弃这一帧。
bool MchoseV2Build(uint16_t cmd, const uint8_t* data, int n, uint8_t* f, int outLen) {
    if (!f || outLen < 9) return false;
    memset(f, 0, (size_t)outLen);
    if (n < 0) n = 0;
    if (n > outLen - 9) n = outLen - 9;      // 数据段 + 校验字节必须装得下
    f[0] = 0x4D;
    f[1] = 0x01;
    f[2] = 0x01;
    f[3] = (uint8_t)n;
    f[4] = (uint8_t)(cmd & 0xFF);
    f[5] = (uint8_t)(cmd >> 8);
    if (n > 0 && data) memcpy(f + 8, data, (size_t)n);
    uint8_t cs = 0;
    for (int i = 2; i <= 7 + n; ++i) cs ^= f[i];
    f[8 + n] = cs;
    return true;
}

// 0x0900 应答的数据段： [0..1]=VID(小端) [2..3]=PID [10]=连接方式
//                       [11]=充电状态(0 放电 1 充电 2 充满) [12]=电量
//
// 投递方式踩过两个坑：
//   1. 0x4D 是**输出报告**，不是特性报告。用 SetFeature 发出去设备照收不回，
//      必须先 WriteFile 到 report 0x4D。
//   2. 写和读要分两个句柄：写用同步句柄，读用异步句柄。同一个异步句柄既写又读，
//      输入管道会静默——连每秒一次的心跳都收不到，看起来像设备在休眠。
// requireChecksum=true 走严格模式：校验不对直接拒绝。
// requireChecksum=false 是**回退模式**：只认帧头/命令/长度与取值范围，不看校验。
// 保留回退模式的原因：协议文档写的是 XOR，但真机固件若按别的算法算，严格模式会
// 让本来能读的设备变成"读不到"。策略是优先信严格帧，整个窗口都没出现过严格帧
// 才退回旧行为，并把 checksumVerified 置 false，让上层把这件事显示出来。
bool MchoseV2Parse(const uint8_t* buf, int n, VendorBattery* out, bool requireChecksum = true) {
    if (!buf || !out || n < 8) return false;
    if (buf[0] != 0x4D) return false;                    // 只认 V2 帧
    if (buf[4] != 0x00 || buf[5] != 0x09) return false;  // 命令 0x0900
    int len = buf[3];
    // 校验字节在 [8+len]，所以必须严格小于 n；旧实现允许刚好等于，
    // 于是根本没把校验字节读进来，也就永远没验过校验。
    if (len < 13 || 8 + len >= n) return false;          // len=1 是"未就绪"，跳过

    if (requireChecksum) {
        // XOR 覆盖 [2]..[7+len]。不验的话，传输损坏或错位帧只要头、命令、
        // 电量字节碰巧合法就会被当成有效读数。
        uint8_t cs = 0;
        for (int i = 2; i <= 7 + len; ++i) cs ^= buf[i];
        if (cs != buf[8 + len]) return false;
    }

    const uint8_t* d = buf + 8;
    int pct = d[12];
    if (pct > 100) return false;
    out->level = pct;
    out->transport = d[10];
    switch (d[11]) {
        case 1: out->charging = 1; break;
        case 2: out->charging = 1; out->full = true; break;
        default: out->charging = 0; break;
    }
    if (out->full && out->level < 100) out->full = false;
    out->valid = true;
    out->checksumVerified = requireChecksum;
    return true;
}

// 一帧一帧喂进来，内部同时记录"严格通过"和"仅回退可用"两个候选。
struct MchoseV2Sink {
    VendorBattery strict;
    VendorBattery lenient;
    bool hasStrict = false;
    bool hasLenient = false;
};

// 返回 true 表示已经拿到严格通过的帧，调用方可以提前结束等待。
bool MchoseV2Offer(const uint8_t* buf, int n, MchoseV2Sink* sink) {
    if (!sink || sink->hasStrict) return sink && sink->hasStrict;
    if (MchoseV2Parse(buf, n, &sink->strict, true)) {
        sink->hasStrict = true;
        return true;
    }
    if (!sink->hasLenient && MchoseV2Parse(buf, n, &sink->lenient, false)) {
        sink->hasLenient = true;
    }
    return false;
}

// 窗口结束后的裁决：严格帧优先，没有再退回回退帧，都没有才算失败。
bool MchoseV2Resolve(const MchoseV2Sink& sink, VendorBattery* out) {
    if (!out) return false;
    if (sink.hasStrict) { *out = sink.strict; return true; }
    if (sink.hasLenient) { *out = sink.lenient; out->checksumVerified = false; return true; }
    return false;
}

bool MchoseV2Ask(const std::wstring& path, VendorBattery* out) {
    HidDevice reader;
    if (!reader.Open(path)) return false;

    HANDLE writer = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (writer == INVALID_HANDLE_VALUE) return false;

    uint8_t req[256];
    // 必须按接口的 OutputReportByteLength 写：多写/少写一个字节都是 87。
    int wire = reader.info().outputLen;
    if (wire < MCHOSE_V2_LEN) wire = MCHOSE_V2_LEN;
    if (wire > (int)sizeof(req)) wire = (int)sizeof(req);
    if (!MchoseV2Build(0x0900, nullptr, 0, req, wire)) return false;
    DWORD written = 0;
    BOOL sent = WriteFile(writer, req, (DWORD)wire, &written, nullptr);
    CloseHandle(writer);
    if (!sent) return false;

    uint8_t buf[256];
    int readLen = std::max<int>(MCHOSE_V2_LEN, reader.info().inputLen);
    if (readLen > (int)sizeof(buf)) readLen = (int)sizeof(buf);

    ULONGLONG deadline = GetTickCount64() + 800;
    MchoseV2Sink sink;
    while (GetTickCount64() < deadline) {
        int n = reader.Read(buf, readLen, 120);
        if (n < 8) continue;
        if (MchoseV2Offer(buf, n, &sink)) break;   // 严格通过，不用再等
    }
    // 严格帧一次都没出现时退回旧解析，避免"改了校验反而读不到"
    return MchoseV2Resolve(sink, out);
}

// ---------------- 旧 L7 Pro：被动接收 0x13/0x1D 推送
// 电量在设备自己推的 0x13 报告里，sub 0x1D，整帧 XOR 0xFF。
// 只被动等推送，不发 nudge —— 少发一条写命令，少一分风险。
bool ReadMchoseLegacy(const std::wstring& path, int* level, bool* charging) {
    HidDevice dev;
    if (!dev.Open(path)) return false;
    uint8_t buf[128] = {0};
    ULONGLONG deadline = GetTickCount64() + 1200;
    while (GetTickCount64() < deadline) {
        int n = dev.Read(buf, 128, 150);
        if (n < 8) continue;
        if (buf[0] != 0x13 || buf[1] != 0x1D) continue;
        int pct = (int)(buf[5] ^ 0xFF);
        if (pct < 0 || pct > 100) continue;
        bool wired = (buf[4] ^ 0xFF) != 0;
        *level = pct;
        *charging = wired && pct < 100;
        return true;
    }
    return false;
}

// 推送式设备不必每轮都等 2 秒，缓存 90 秒
struct PushCache { int level; bool charging; ULONGLONG ts; };
std::map<std::wstring, PushCache> g_pushCache;

// 有些接收器只报通用名（迈从 41E4:2001 就报 "MCHOSE 2.4G Wireless"），
// 型号只能由用户在 config.ini 里用 name.VID:PID 指定
std::wstring AliasFor(uint16_t vid, uint16_t pid) {
    wchar_t k[16];
    swprintf(k, 16, L"%04X:%04X", vid, pid);
    auto it = g_config.aliases.find(k);
    if (it != g_config.aliases.end()) return it->second;
    return std::wstring();
}

// ---------------------------------------------------------------- 飞智 Flydigi
// Vader 5 Pro 这一代（VID 0x37D7，SDL 里叫 V2）用 5A A5 帧，走 0xFFA0 接口，
// 33 字节报文（首字节是 report id 0）：
//   请求 5A A5 01 02 00 取设备信息；
//   应答 5A A5 01 <total> <index> …，第 5 字节 = 设备号，第 6 字节 = 连接方式
//   （1 有线 / 2 无线），第 11 字节高 4 位 = 0 放电 / 1 充电 / 2 充满，
//   低 4 位 = 电量档位（×20 %，所以只有 0/20/…/100 六个值）。
// 两个坑：一是写要用同步句柄、读用异步句柄；二是设备**隔一次才应答**，必须重试。
constexpr uint16_t FLYDIGI_VID = 0x37D7;
constexpr int FLYDIGI_LEN = 33;

bool LooksLikeFlydigi(const HidInfo& d) {
    if (d.vid == FLYDIGI_VID) return true;
    std::wstring s = Lower(d.product + L" " + d.manufacturer);
    return s.find(L"flydigi") != std::wstring::npos ||
           s.find(L"飞智") != std::wstring::npos;
}

bool FlydigiAsk(const std::wstring& path, VendorBattery* out) {
    HidDevice reader;
    if (!reader.Open(path)) return false;

    HANDLE writer = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (writer == INVALID_HANDLE_VALUE) return false;

    uint8_t req[FLYDIGI_LEN] = {0};
    req[0] = 0x00;                       // report id
    req[1] = 0x5A;
    req[2] = 0xA5;
    req[3] = 0x01;                       // 取设备信息
    req[4] = 0x02;
    req[5] = 0x00;

    uint8_t buf[128];
    int readLen = std::max<int>(FLYDIGI_LEN, reader.info().inputLen);
    if (readLen > (int)sizeof(buf)) readLen = (int)sizeof(buf);

    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
        DWORD written = 0;
        if (!WriteFile(writer, req, FLYDIGI_LEN, &written, nullptr)) break;
        int n = reader.Read(buf, readLen, 350);
        if (n < 16) continue;

        int base = -1;
        if (buf[0] == 0x5A && buf[1] == 0xA5) base = 0;
        else if (buf[0] == 0x00 && buf[1] == 0x5A && buf[2] == 0xA5) base = 1;
        if (base < 0 || buf[base + 2] != 0x01) continue;     // 只认信息应答
        if (base + 12 >= n) continue;

        uint8_t b = buf[base + 11];
        int pct = (b & 0x0F) * 20;
        if (pct > 100) continue;
        int st = b >> 4;
        out->level = pct;
        out->charging = (st == 1 || st == 2) ? 1 : 0;
        out->full = (st == 2);
        out->transport = buf[base + 6];                      // 1 有线 / 2 无线
        out->valid = true;
        ok = true;
    }
    CloseHandle(writer);
    return ok;
}

// ---------------------------------------------------------------- 扫描：Compx

void ScanCompx(const ScanCtx& ctx, std::vector<Reading>* out) {
    const std::vector<HidInfo>& all = *ctx.all;
    // 同一颗接收器的多个集合归到一组，只出一条记录
    std::map<std::wstring, std::vector<HidInfo>> groups;
    for (const auto& d : all) {
        if (IsNordic54(d.vid, d.pid)) continue;
        if (!IsCompxCandidate(d)) continue;
        groups[StripSuffix(d.path, L"&col")].push_back(d);
    }

    for (auto& kv : groups) {
        auto cands = kv.second;
        std::sort(cands.begin(), cands.end(), [](const HidInfo& a, const HidInfo& b) {
            bool ea = (a.inputLen == COMPX_LEN && a.outputLen == COMPX_LEN);
            bool eb = (b.inputLen == COMPX_LEN && b.outputLen == COMPX_LEN);
            if (ea != eb) return ea;
            bool ka = CompxPidNameOf(a.vid, a.pid) != nullptr;
            bool kb = CompxPidNameOf(b.vid, b.pid) != nullptr;
            if (ka != kb) return ka;
            return a.path < b.path;
        });

        // 稳态只问上次应答过的那个集合；只有全量扫描才回头重试其它集合。
        // 接收器上那些 8 字节的鼠标集合，写 17 字节永远是失败的。
        const std::wstring& good = g_compxInterface[kv.first];
        if (!good.empty() && !g_fullSweep) {
            auto it = std::find_if(cands.begin(), cands.end(),
                                   [&](const HidInfo& d) { return d.path == good; });
            if (it != cands.end()) cands.assign(1, *it);
        }

        CompxState& st = g_compxStates[kv.first];
        CompxBattery bat;
        bool answered = false;
        bool emptyReply = false;
        bool opened = false;
        std::wstring goodPath;

        for (const auto& c : cands) {
            HidDevice dev;
            if (!dev.Open(c.path)) continue;
            opened = true;

            uint8_t rep[COMPX_LEN];
            bool got = CompxAsk(dev, COMPX_CMD_BATTERY, rep);
            if (got) {
                answered = true;
                goodPath = c.path;
                if (!ParseCompxBattery(rep, &bat)) emptyReply = true;

                // 型号只在鼠标醒着时问：它和电量走同一个接收器通道。
                // 有的固件不实现 0x10，问三次还不应就不再浪费每轮 360 ms。
                if (!st.modelTried && st.modelAttempts < 3) {
                    st.modelAttempts++;
                    uint8_t mrep[COMPX_LEN];
                    if (CompxAsk(dev, COMPX_CMD_MODEL, mrep)) {
                        int cu = 0, mo = 0;
                        if (ParseCompxModel(mrep, &cu, &mo)) {
                            st.customer = cu;
                            st.model = mo;
                        }
                        st.modelTried = true;      // 有应答（哪怕没报型号）就不用再问
                    } else if (st.modelAttempts >= 3) {
                        st.modelTried = true;
                    }
                }
            }
            dev.Close();
            if (got) break;
        }

        if (answered) {
            g_compxInterface[kv.first] = goodPath;
            if (bat.valid) st.answered = true;
        }

        const HidInfo& d = kv.second.front();
        std::wstring name;
        if (const wchar_t* n = CompxModelNameOf(st.customer, st.model)) {
            name = n;
        } else if (const wchar_t* n = CompxPidNameOf(d.vid, d.pid)) {
            name = n;
        } else {
            name = CleanProductName(d.product);
            if (name.empty()) name = L"2.4G 无线鼠标";
        }

        Reading r;
        r.key = L"compx:" + kv.first;
        r.name = name;
        r.kind = ClassifyName(name);
        if (r.kind == Kind::Other) r.kind = Kind::Mouse;   // 这套接收器上挂的基本都是鼠标
        r.vendor = CompxBrand(name, d.product);
        r.provider = L"compx";

        if (bat.valid) {
            r.level = bat.level;
            r.voltage = bat.millivolts;
            r.charging = bat.onCable ? 1 : 0;
            r.transport = bat.onCable ? L"USB 有线" : L"2.4G 接收器";
            r.status = bat.onCable ? (bat.level >= 100 ? L"已充满" : L"充电中") : L"放电中";
        } else {
            r.level = -1;
            r.charging = -1;
            r.transport = L"2.4G 接收器";
            if (emptyReply) {
                r.status = L"未连接鼠标";
                r.detail = L"接收器有应答，但没有鼠标连在它上面";
                r.outcome = Outcome::NoReply;
            } else if (st.answered) {
                r.status = L"设备休眠";
                r.detail = L"移动鼠标唤醒后会自动更新";
                r.outcome = Outcome::NoReply;
            } else if (!opened) {
                r.status = L"接口被占用";
                r.detail = L"厂商集合打不开，可能被官方驱动独占";
                r.outcome = Outcome::Busy;
            } else {
                r.status = L"电量不可读";
                r.detail = L"接口已打开，但没收到电量应答；跑 --vendor 可看原始帧";
                r.outcome = Outcome::NoReply;
            }
        }
        if (st.customer > 0) {
            wchar_t tag[64];
            swprintf(tag, 64, L"型号 %d:%d", st.customer, st.model);
            r.detail = r.detail.empty() ? tag : (r.detail + L" · " + tag);
        }
        out->push_back(r);
    }
}

// ---------------------------------------------------------------- 扫描：Nordic54 / MCHOSE

void ScanNordic54(const ScanCtx& ctx, std::vector<Reading>* out) {
    const std::vector<HidInfo>& all = *ctx.all;
    std::map<std::wstring, const HidInfo*> picked;
    for (const auto& d : all) {
        if (!IsNordic54(d.vid, d.pid)) continue;
        if (d.usagePage != 0xFF02 || d.usage != 0x0002) continue;
        picked.emplace(DeviceIdentity(d), &d);     // 同型号两台不再互相覆盖
    }

    for (const auto& kv : picked) {
        const HidInfo& d = *kv.second;
        int level = -1;
        bool wired = false;
        bool ok = ReadNordic54(d.path, &level, &wired);

        Reading r;
        r.key = L"nordic54:" + kv.first;
        r.name = L"ATK Zero";
        r.kind = Kind::Mouse;
        r.vendor = L"ATK";
        r.transport = L"2.4G 接收器";
        r.level = ok ? level : -1;
        r.charging = ok ? (wired && level < 100 ? 1 : 0) : -1;
        r.status = ok ? (wired ? (level >= 100 ? L"已充满" : L"充电中") : L"放电中")
                      : L"未返回电量";
        if (!ok) {
            r.detail = L"设备已识别，但本轮没有读到电量";
            r.outcome = Outcome::NoReply;
        }
        r.provider = L"nordic54";
        out->push_back(r);
    }
}

// mi_00 的主集合带 \kbd 后缀 = 这颗接收器的主功能是键盘；鼠标接收器没有这个后缀
bool MchoseIsKeyboard(const std::vector<HidInfo>& items) {
    for (const auto& d : items) {
        std::wstring p = Lower(d.path);
        if (p.find(L"&mi_00") == std::wstring::npos) continue;
        return p.size() >= 4 && p.compare(p.size() - 4, 4, L"\\kbd") == 0;
    }
    return false;
}

// "MCHOSE 2.4G Wireless" 这种通用名换成「无线键盘 / 无线鼠标」，
// "MCHOSE A5 V3 Pro" 这类带型号的原样保留
std::wstring MchoseDisplayName(const std::wstring& product, bool keyboard) {
    std::wstring s = CleanProductName(product);
    std::wstring low = Lower(s);
    bool generic = low.empty() ||
                   low.find(L"2.4g") != std::wstring::npos ||
                   low.find(L"wireless") != std::wstring::npos ||
                   low.find(L"receiver") != std::wstring::npos;
    if (generic) return keyboard ? L"迈从无线键盘" : L"迈从无线鼠标";
    return s;
}

void ScanMchose(const ScanCtx& ctx, std::vector<Reading>* out) {
    const std::vector<HidInfo>& all = *ctx.all;
    // 一颗接收器会挂好几个集合（鼠标 / 键盘 / 厂商），按物理设备归成一条；
    // 两颗同型号接收器各有各的 ContainerId，不会再并成一条互相覆盖。
    std::map<std::wstring, std::vector<HidInfo>> groups;
    for (const auto& d : all) {
        if (!LooksLikeMchose(d)) continue;
        groups[DeviceIdentity(d)].push_back(d);
    }

    for (auto& kv : groups) {
        const std::vector<HidInfo>& items = kv.second;
        const HidInfo* v2 = nullptr;
        const HidInfo* v1 = nullptr;
        const HidInfo* legacy = nullptr;
        for (const auto& d : items) {
            // 协议实际走的是 64 字节**输出**报告 0x4D，所以判据是 outputLen。
            // 合法且常见的形态是 OUT=64 / FEATURE=0，按 featureLen 判会一个都选不中，
            // 随后 V1/legacy 也不匹配，设备看起来像"休眠/被占用"。
            if (!v2 && d.outputLen >= MCHOSE_V2_LEN && d.usagePage >= 0xFF00) v2 = &d;
            if (!v1 && d.usagePage == 0xFF02 && d.usage == 0x0002 &&
                d.inputLen >= MCHOSE_V1_LEN && d.outputLen >= MCHOSE_V1_LEN) {
                v1 = &d;
            }
            if (!legacy && d.vid == 0x5253 && d.usagePage == 0xFF01 &&
                d.inputLen >= 32) {
                legacy = &d;
            }
        }

        const HidInfo& d0 = items.front();
        std::wstring key = L"mchose:" + kv.first;

        VendorBattery bat;
        bool opened = false;
        bool viaV2 = false;

        // V2 优先：0x3837 的 A5 V3 只认这一条
        if (v2) {
            opened = true;
            viaV2 = MchoseV2Ask(v2->path, &bat);
        }
        // V1：0x41E4 的键盘接收器
        if (!bat.valid && v1) {
            HidDevice dev;
            if (dev.Open(v1->path)) {
                opened = true;
                uint8_t rep[MCHOSE_V1_LEN];
                if (MchoseV1Ask(dev, rep)) ParseMchoseV1(rep, &bat);
            }
        }
        // 旧 L7 Pro：只等推送
        if (!bat.valid && legacy) {
            opened = true;
            int lv = -1;
            bool ch = false;
            if (ReadMchoseLegacy(legacy->path, &lv, &ch)) {
                bat.level = lv;
                bat.charging = ch ? 1 : 0;
                bat.valid = true;
            }
        }

        // 设备休眠时保留上一次读数，但标明是缓存
        bool cached = false;
        ULONGLONG cacheTs = 0;
        auto it = g_pushCache.find(key);
        if (!bat.valid && it != g_pushCache.end() &&
            GetTickCount64() - it->second.ts < 90000) {
            bat.level = it->second.level;
            bat.charging = it->second.charging;
            bat.valid = true;
            cached = true;
            cacheTs = it->second.ts;
        }
        if (bat.valid && !cached) {
            g_pushCache[key] = {bat.level, bat.charging == 1, GetTickCount64()};
        }

        bool keyboard = MchoseIsKeyboard(items);
        Reading r;
        r.key = key;
        std::wstring alias = AliasFor(d0.vid, d0.pid);
        r.name = alias.empty() ? MchoseDisplayName(d0.product, keyboard) : alias;
        r.kind = keyboard ? Kind::Keyboard : Kind::Mouse;
        r.vendor = L"迈从";
        r.provider = L"mchose";
        r.level = bat.valid ? bat.level : -1;
        r.charging = bat.valid ? bat.charging : -1;
        if (cached) {
            r.stale = true;              // 缓存值：上层别把时间刷成 now
            r.sampledAt = cacheTs;
        }

        if (bat.transport == 1) r.transport = L"USB 有线";
        else if (bat.transport == 3) r.transport = L"蓝牙";
        else r.transport = L"2.4G 接收器";

        if (bat.valid) {
            if (bat.charging == 1) r.status = bat.full ? L"已充满" : L"充电中";
            else r.status = L"放电中";
            if (cached) r.detail = L"设备未应答，显示上次读数";
            // 回退路径要让用户看得见：数据是好的，但协议校验没过。
            else if (viaV2 && !bat.checksumVerified)
                r.detail = L"应答校验未通过，已回退到旧解析（固件算法可能与协议文档不一致）";
        } else {
            r.status = opened ? L"设备休眠" : L"接口被占用";
            r.detail = opened ? L"移动鼠标或按键唤醒后会自动更新"
                              : L"厂商集合打不开，可能被官方驱动独占";
            r.outcome = opened ? Outcome::NoReply : Outcome::Busy;
        }
        out->push_back(r);
    }
}

// ---------------------------------------------------------------- 扫描：飞智

void ScanFlydigi(const ScanCtx& ctx, std::vector<Reading>* out) {
    const std::vector<HidInfo>& all = *ctx.all;
    std::map<std::wstring, std::vector<HidInfo>> groups;
    for (const auto& d : all) {
        if (!LooksLikeFlydigi(d)) continue;
        // 按物理设备分组，不带产品名：有线模式下同一台手柄的接口分成两组，
        // XInput 那组报 "Controller (Flydigi Vader 5 Pro)"，厂商集合那组报
        // "Flydigi Vader 5 Pro"，带产品名会拆成两条。也不会把两只同型号手柄并掉。
        groups[DeviceIdentity(d)].push_back(d);
    }

    for (auto& kv : groups) {
        const HidInfo* cmd = nullptr;
        for (const auto& d : kv.second) {
            if (d.usagePage == 0xFFA0 && d.inputLen >= FLYDIGI_LEN &&
                d.outputLen >= FLYDIGI_LEN) {
                cmd = &d;
                break;
            }
        }
        const HidInfo& d0 = kv.second.front();
        // 产品名优先取厂商集合那条：XInput 组报的是 "Controller (Flydigi Vader 5 Pro)"，
        // 多一层前缀，面板里显示不干净。
        const std::wstring product = (cmd && !cmd->product.empty()) ? cmd->product : d0.product;
        std::wstring key = L"flydigi:" + kv.first;
        std::wstring alias = AliasFor(d0.vid, d0.pid);

        if (!cmd) {
            // 有线模式（走 XInput）与接收器模式的接口形态不一样，可能根本没有
            // 0xFFA0 + 33 字节的集合。这里如果直接 continue，面板里连一行都不会
            // 出现：用户看到的是「手柄插着，但软件像压根不认识它」。
            // 所以留一条可见记录，并把实际枚举到的接口写进 detail，便于定位。
            std::wstring seen;
            wchar_t seg[64];
            for (const auto& d : kv.second) {
                swprintf(seg, 64, L"%ls%04X/%04X %u/%u", seen.empty() ? L"" : L"，",
                         d.usagePage, d.usage, d.inputLen, d.outputLen);
                seen += seg;
            }
            Reading r;
            r.key = key;
            r.name = alias.empty() ? (product.empty() ? L"飞智手柄" : product) : alias;
            r.kind = Kind::Controller;
            r.vendor = L"飞智";
            r.provider = L"flydigi";
            r.level = -1;
            r.charging = -1;
            r.transport = TransportOf(d0.path, d0.product);
            r.status = L"未找到厂商接口";
            r.detail = L"需要 0xFFA0 / 进出≥33；实际枚举到 " + seen;
            r.outcome = Outcome::Unsupported;
            out->push_back(r);
            continue;
        }

        VendorBattery bat;
        FlydigiAsk(cmd->path, &bat);

        bool cached = false;
        auto it = g_pushCache.find(key);
        if (!bat.valid && it != g_pushCache.end() &&
            GetTickCount64() - it->second.ts < 90000) {
            bat.level = it->second.level;
            bat.charging = it->second.charging;
            bat.valid = true;
            cached = true;
        }
        if (bat.valid && !cached) {
            g_pushCache[key] = {bat.level, bat.charging == 1, GetTickCount64()};
        }

        Reading r;
        r.key = key;
        r.name = alias.empty() ? (product.empty() ? L"飞智手柄" : product) : alias;
        r.kind = Kind::Controller;
        r.vendor = L"飞智";
        r.provider = L"flydigi";
        r.level = bat.valid ? bat.level : -1;
        r.charging = bat.valid ? bat.charging : -1;
        r.transport = (bat.transport == 1) ? L"USB 有线" : L"2.4G 接收器";

        if (bat.valid) {
            if (bat.charging == 1) r.status = bat.full ? L"已充满" : L"充电中";
            else r.status = L"放电中";
            if (cached) r.detail = L"手柄未应答，显示上次读数";
        } else {
            r.status = L"未返回电量";
            r.detail = L"已识别到手柄，但本轮没有读到电量";
            r.outcome = Outcome::NoReply;
        }
        out->push_back(r);
    }
}

// ---------------------------------------------------------------- 蓝牙 BLE
//
// BLE 外设的电量只有一条标准路径：GATT 的 Battery Service（0x180F）里的
// Battery Level（0x2A19）。三个前提缺一不可：设备已配对、当前处于连接态、
// 固件没有把 BAS 裁掉。服务表是系统缓存的，能列出来不代表能读——掉线时读
// 会返回 0x80070001（ERROR_INVALID_FUNCTION），这不是解析错误。
//
// 经典蓝牙音频（华为 FreeBuds 这类）不走这条路：它们没有 GATT 节点，系统
// 属性里也没有任何电量字段，只有 HFP 的 AT 指令拿得到，而那要占用 Windows
// 自己的免手操链路，会影响通话与播放，所以这里不做。

const GUID kGuidBleDevice = {
    0x781AEE18, 0x7733, 0x4CE4, {0xAD, 0xD0, 0x91, 0xF4, 0x1C, 0x67, 0xB5, 0x92}};

// 对应 BTH_LE_GATT_CHARACTERISTIC_VALUE：MinGW 的头里没有这个结构
struct BleCharValue {
    ULONG DataSize;
    UCHAR Data[1];
};

typedef HRESULT(WINAPI* PFN_BleGetServices)(HANDLE, USHORT, PBTH_LE_GATT_SERVICE,
                                            USHORT*, ULONG);
typedef HRESULT(WINAPI* PFN_BleGetChars)(HANDLE, PBTH_LE_GATT_SERVICE, USHORT,
                                         PBTH_LE_GATT_CHARACTERISTIC, USHORT*, ULONG);
typedef HRESULT(WINAPI* PFN_BleGetValue)(HANDLE, PBTH_LE_GATT_CHARACTERISTIC, ULONG,
                                         BleCharValue*, USHORT*, ULONG);

struct BleApi {
    PFN_BleGetServices getServices = nullptr;
    PFN_BleGetChars getChars = nullptr;
    PFN_BleGetValue getValue = nullptr;
    bool ready = false;
};

// 这三个函数只导出在 BluetoothApis.dll，MinGW 的导入库里没有，只能动态取
BleApi& Gatt() {
    static BleApi api;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE dll = LoadLibraryW(L"BluetoothApis.dll");
        if (dll) {
            api.getServices = (PFN_BleGetServices)GetProcAddress(dll, "BluetoothGATTGetServices");
            api.getChars = (PFN_BleGetChars)GetProcAddress(dll, "BluetoothGATTGetCharacteristics");
            api.getValue = (PFN_BleGetValue)GetProcAddress(dll, "BluetoothGATTGetCharacteristicValue");
            api.ready = api.getServices && api.getChars && api.getValue;
        }
    }
    return api;
}

struct BleDev {
    std::wstring path;
    std::wstring name;
};

std::vector<BleDev> EnumBleDevices() {
    std::vector<BleDev> out;
    HDEVINFO set = SetupDiGetClassDevsW(&kGuidBleDevice, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;

    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA did{};
        did.cbSize = sizeof(did);
        if (!SetupDiEnumDeviceInterfaces(set, nullptr, &kGuidBleDevice, i, &did)) break;

        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &did, nullptr, 0, &need, nullptr);
        if (need == 0) continue;

        std::vector<uint8_t> raw(need);
        auto* detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)raw.data();
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA info{};
        info.cbSize = sizeof(info);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &did, detail, need, nullptr, &info))
            continue;

        BleDev dev;
        dev.path = detail->DevicePath;
        wchar_t name[MAX_PATH] = {0};
        DWORD type = 0, size = sizeof(name);
        if (SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_FRIENDLYNAME, &type,
                                              (PBYTE)name, size, &size) &&
            name[0]) {
            dev.name = name;
        }
        out.push_back(dev);
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

// 真的读到电量才返回 true；失败时把 HRESULT 带回去，给 --ble 诊断用
bool ReadBleBattery(const std::wstring& path, int* pct, long* result) {
    BleApi& api = Gatt();
    if (!api.ready) {
        *result = (long)0x8007007E;      // ERROR_MOD_NOT_FOUND
        return false;
    }

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *result = (long)GetLastError();
        return false;
    }

    bool ok = false;

    // 注意：这个「先问数量」的调用**本身**就返回 ERROR_MORE_DATA(0x800700EA)，
    // 那是它的正常行为，不是失败——只要把回填的数量当容量再调一次。把它的
    // HRESULT 当结果判断，会永远读不到东西（这个坑踩过一次）。
    USHORT count = 0;
    api.getServices(h, 0, nullptr, &count, 0);
    std::vector<BTH_LE_GATT_SERVICE> svcs(count + 1);
    USHORT cap = (USHORT)svcs.size();
    HRESULT hr = count > 0 ? api.getServices(h, cap, svcs.data(), &cap, 0)
                           : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    *result = (long)hr;
    count = SUCCEEDED(hr) ? cap : 0;

    for (USHORT i = 0; i < count && !ok; ++i) {
        if (!svcs[i].ServiceUuid.IsShortUuid ||
            svcs[i].ServiceUuid.Value.ShortUuid != 0x180F)
            continue;

        USHORT charCount = 0;
        api.getChars(h, &svcs[i], 0, nullptr, &charCount, 0);   // 同上，返回值不用看
        if (charCount == 0) continue;
        std::vector<BTH_LE_GATT_CHARACTERISTIC> chars(charCount + 1);
        USHORT ccap = (USHORT)chars.size();
        hr = api.getChars(h, &svcs[i], ccap, chars.data(), &ccap, 0);
        *result = (long)hr;
        if (FAILED(hr)) continue;
        charCount = ccap;

        for (USHORT c = 0; c < charCount && !ok; ++c) {
            const BTH_LE_UUID& u = chars[c].CharacteristicUuid;
            if (!u.IsShortUuid || u.Value.ShortUuid != 0x2A19) continue;

            std::vector<uint8_t> buf(sizeof(BleCharValue) + 8, 0);
            auto* v = (BleCharValue*)buf.data();
            USHORT need = 0;
            // 这个函数是 6 个参数：第 5 个是回填需要的字节数，漏了会直接崩
            hr = api.getValue(h, &chars[c], (ULONG)buf.size(), v, &need, 0);
            *result = (long)hr;
            if (SUCCEEDED(hr) && v->DataSize >= 1 && v->Data[0] <= 100) {
                *pct = v->Data[0];
                ok = true;
            }
        }
    }
    CloseHandle(h);
    return ok;
}

void ScanBle(std::vector<Reading>* out) {
    if (!Gatt().ready) return;

    // GATT 读写是要等的：设备掉线时一次枚举能拖到 1.7 秒，每轮扫描都做会把
    // 整体耗时从 130ms 抬到 2s。所以只在全量扫描（第 8 轮 / 手动刷新）时刷新，
    // 其余轮次直接用上一次的结果。
    // 但"用上一次的结果"必须让上层看得见：这里给复用的记录打 stale，并把
    // sampledAt 留在真正读到值的那一刻。旧实现每轮都被上层刷成"当前时间"，
    // 于是默认 2 分钟档下 BLE 实际 16 分钟才更新一次，面板却始终显示刚刚。
    static std::vector<Reading> cache;
    static bool scanned = false;
    const bool refreshed = (g_fullSweep || !scanned);
    if (refreshed) {
        scanned = true;
        cache.clear();
        const ULONGLONG sampledAt = GetTickCount64();
        for (const auto& dev : EnumBleDevices()) {
            int pct = -1;
            long hr = 0;
            // 读不到就不占一行：BLE 设备的服务表即使缓存着，掉线也读不出值，
            // 那种情况摆一行 "--" 只会让面板变吵。
            if (!ReadBleBattery(dev.path, &pct, &hr)) continue;

            Reading r;
            r.key = L"ble:" + dev.path;
            r.name = dev.name.empty() ? L"蓝牙设备" : dev.name;
            r.kind = ClassifyName(r.name);
            r.vendor.clear();        // 传输方式已写蓝牙，厂商再写一遍就重复了
            r.provider = L"ble";
            r.transport = L"蓝牙";
            r.level = pct;
            r.charging = -1;         // BAS 不带充电状态
            r.status = L"未报充电状态";
            r.detail = L"BLE 标准电量服务（0x180F）";
            r.sampledAt = sampledAt;
            cache.push_back(r);
        }
    }
    for (auto r : cache) {           // 按值拷贝：只改这一份的 stale，不动缓存本体
        if (!refreshed) r.stale = true;
        out->push_back(r);
    }
}

}  // namespace

// BLE 诊断：--ble 用它，能直接看出设备是没连上还是没暴露 0x180F
// 命名空间嵌套比较绕，对外的包装放在文件末尾
std::wstring BleDiagnosticsImpl() {
    std::wstring out = L"BLE 电量诊断（GATT 0x180F / 0x2A19）\n";
    out += L"--------------------------------------------------------------\n";
    if (!Gatt().ready) {
        out += L"BluetoothApis.dll 里取不到 GATT 函数，蓝牙栈可能被裁剪过\n";
        return out;
    }

    auto devs = EnumBleDevices();
    if (devs.empty()) {
        out += L"当前没有处于连接态的 BLE 设备接口。\n"
               L"已配对但没连上的设备不会出现在这里；经典蓝牙音频设备\n"
               L"（耳机 / 音箱）也不会——它们没有 GATT 节点。\n";
        return out;
    }

    wchar_t line[512];
    for (const auto& d : devs) {
        int pct = -1;
        long hr = 0;
        bool ok = ReadBleBattery(d.path, &pct, &hr);
        swprintf(line, 512, L"%-30ls %-8ls hr=0x%08lX\n",
                 d.name.empty() ? L"(无名称)" : d.name.c_str(),
                 ok ? (std::to_wstring(pct) + L"%").c_str() : L"读不到",
                 hr & 0xFFFFFFFF);
        out += line;
    }
    return out;
}

// HID 接口清单：排查新设备时让用户直接发这个文件，比问来问去快
std::wstring DumpHidImpl() {
    auto all = EnumHidDevices();
    std::sort(all.begin(), all.end(), [](const HidInfo& a, const HidInfo& b) {
        if (a.vid != b.vid) return a.vid < b.vid;
        if (a.pid != b.pid) return a.pid < b.pid;
        return a.path < b.path;
    });

    std::wstring out = L"VID:PID     UsagePage Usage  IN  OUT FEAT  识别                产品名\n";
    out += L"--------------------------------------------------------------------------\n";
    wchar_t line[768];
    for (const auto& d : all) {
        const wchar_t* tag = L"";
        if (d.vid == LOGITECH_VID && d.usagePage == 0xFF00) {
            tag = L"罗技 HID++";
        } else if (d.vid == LOGITECH_VID && d.pid == 0xC542) {
            tag = L"罗技 Nano（无 HID++）";
        } else if (IsCompxCandidate(d)) {
            tag = L"Compx / VGN 同族";
        } else if (IsCompxVendor(d.vid)) {
            tag = L"Compx 同族（非厂商集合）";
        } else if (IsNordic54(d.vid, d.pid)) {
            tag = L"ATK Zero (Nordic54)";
        } else if (LooksLikeFlydigi(d)) {
            // 同一台手柄有线/接收器两种形态的接口集合不一样，标签要能区分，
            // 否则 --dump 里每个集合都写着「新协议」，反而看不出哪条能问电量。
            tag = (d.usagePage == 0xFFA0 && d.inputLen >= FLYDIGI_LEN &&
                   d.outputLen >= FLYDIGI_LEN)
                      ? L"飞智（5A A5 新协议）"
                      : L"飞智（非厂商集合）";
        } else if (LooksLikeMchose(d)) {
            if (d.outputLen >= MCHOSE_V2_LEN && d.usagePage >= 0xFF00) {
                tag = L"迈从 V2（0x4D 输出报告）";
            } else if (d.usagePage == 0xFF02 && d.usage == 0x0002 &&
                       d.inputLen >= MCHOSE_V1_LEN) {
                tag = L"迈从 V1（0x13/0x4A）";
            } else {
                tag = L"迈从（非厂商集合）";
            }
        } else if (d.vid == SONY_VID) {
            bool bt = (TransportOf(d.path, d.product) == L"蓝牙");
            tag = bt ? L"索尼手柄（蓝牙·无电量）" : L"索尼手柄（USB）";
        } else if (d.usagePage == 0x06 && d.usage == 0x20) {
            tag = L"标准电量页";
        }
        // 注意：MinGW 宽字符 printf 的 %s 指 char*，宽字符串必须用 %ls
        swprintf(line, 768, L"%04X:%04X   %04X      %04X  %3u %4u %4u  %-20ls %ls\n",
                 d.vid, d.pid, d.usagePage, d.usage, d.inputLen, d.outputLen,
                 d.featureLen, tag, d.product.c_str());
        out += line;
    }
    return out;
}

// ------------------------------------------------------------------ 索尼

struct SonyModel {
    uint16_t pid;
    const wchar_t* name;
};

const SonyModel kSonyModels[] = {
    {0x0CE6, L"DualSense 无线控制器"},
    {0x0DF2, L"DualSense Edge 无线控制器"},
    {0x0CDA, L"DualSense 无线控制器"},
    {0x05C4, L"DualShock 4"},
    {0x09CC, L"DualShock 4"},
};

const wchar_t* SonyName(uint16_t pid) {
    for (auto& m : kSonyModels) {
        if (m.pid == pid) return m.name;
    }
    return nullptr;
}

void ScanSony(const ScanCtx& ctx, std::vector<Reading>* out) {
    const std::vector<HidInfo>& all = *ctx.all;
    // 分组分两步：
    //   1) 先按物理设备（ContainerId）分组，两只同型号手柄不会再并成一条；
    //   2) 再把"同一 PID、恰好一组 USB + 一组蓝牙"的合成一条——那是同一只手柄的
    //      两条链路。两只同型号 USB 手柄会得到两组 USB，不符合条件，不会被并掉。
    std::map<std::wstring, std::vector<const HidInfo*>> byId;
    for (auto& d : all) {
        if (d.vid != SONY_VID) continue;
        if (!SonyName(d.pid)) continue;
        if (!(d.usagePage == 0x0001 && d.usage == 0x0005)) continue;
        byId[DeviceIdentity(d)].push_back(&d);
    }

    struct SonyGroup {
        std::wstring id;
        std::vector<const HidInfo*> items;
    };
    std::map<uint16_t, std::vector<SonyGroup>> perPid;
    for (auto& kv : byId) {
        SonyGroup g;
        g.id = kv.first;
        g.items = kv.second;
        perPid[kv.second.front()->pid].push_back(g);
    }

    std::vector<SonyGroup> groups;
    for (auto& kv : perPid) {
        std::vector<SonyGroup>& gs = kv.second;
        std::sort(gs.begin(), gs.end(),
                  [](const SonyGroup& a, const SonyGroup& b) { return a.id < b.id; });
        int usb = -1, bt = -1, nUsb = 0, nBt = 0;
        for (int i = 0; i < (int)gs.size(); ++i) {
            const HidInfo* f = gs[i].items.front();
            if (TransportOf(f->path, f->product) == L"蓝牙") {
                ++nBt;
                if (bt < 0) bt = i;
            } else {
                ++nUsb;
                if (usb < 0) usb = i;
            }
        }
        if (nUsb == 1 && nBt == 1) {
            SonyGroup g;
            g.id = gs[usb].id;          // 键跟着 USB 走：拔掉线时行不会换身份
            g.items = gs[usb].items;
            g.items.insert(g.items.end(), gs[bt].items.begin(), gs[bt].items.end());
            groups.push_back(g);
        } else {
            for (auto& g : gs) groups.push_back(g);
        }
    }

    for (auto& g : groups) {
        const HidInfo* pick = nullptr;
        for (const HidInfo* d : g.items) {
            bool bt = TransportOf(d->path, d->product) == L"蓝牙";
            if (!pick) {
                pick = d;
            } else if (TransportOf(pick->path, pick->product) == L"蓝牙" && !bt) {
                pick = d;                     // USB 优先
            }
        }
        if (!pick) continue;

        const HidInfo& d = *pick;
        const wchar_t* model = SonyName(d.pid);
        bool bt = TransportOf(d.path, d.product) == L"蓝牙";
        bool ds5 = (d.pid == 0x0CE6 || d.pid == 0x0DF2 || d.pid == 0x0CDA);

        Reading r;
        r.key = L"sony:" + g.id;
        r.name = model;
        r.kind = Kind::Controller;
        r.vendor = L"索尼";
        r.transport = TransportOf(d.path, d.product);
        r.provider = L"sony";

        HidDevice dev;
        if (!dev.Open(d.path)) {
            r.status = L"接口被占用";
            r.detail = L"手柄的 HID 接口打不开，可能被 Steam / DS4Windows 独占";
            r.outcome = Outcome::Busy;
            out->push_back(r);
            continue;
        }

        uint8_t buf[128];
        uint8_t report[128] = {0};
        int reportId = 0;
        int reportLen = 0;
        bool simpleOnly = false;      // 蓝牙简化报文：只有按键摇杆，没有电量

        ULONGLONG deadline = GetTickCount64() + 900;
        while (GetTickCount64() < deadline) {
            int n = dev.Read(buf, sizeof(buf), 200);
            if (n <= 0) continue;
            int rid = buf[0];
            if (rid == 0x31 || rid == 0x11) {            // 完整蓝牙报文，优先
                reportId = rid;
                reportLen = std::min(n, 128);
                memcpy(report, buf, reportLen);
                break;
            }
            if (rid == 0x01) {
                if (bt) {                                 // 蓝牙下的 0x01 = 简化报文
                    simpleOnly = true;
                    break;
                }
                reportId = rid;
                reportLen = std::min(n, 128);
                memcpy(report, buf, reportLen);
                break;
            }
        }
        dev.Close();

        if (simpleOnly) {
            r.level = -1;
            r.charging = -1;
            r.status = L"蓝牙不提供电量";
            r.detail = L"Windows 蓝牙只给 DualSense 的简化报文（无电量字段）。"
                       L"换一根能传数据的 USB 线插上即可读取";
            r.outcome = Outcome::Unsupported;
            out->push_back(r);
            continue;
        }
        if (reportId == 0) {
            r.status = L"未收到手柄数据";
            r.detail = L"手柄处于休眠或未连接";
            r.outcome = Outcome::NoReply;
            out->push_back(r);
            continue;
        }

        // 偏移照 Linux hid-playstation：DualSense 的 status[0] 在结构体第 53 字节，
        // USB 报文直接从 0 开始算，蓝牙报文整体右移 2 字节（report id + seq）。
        // DS4 是另一套结构：USB 0x01 第 30 字节，蓝牙 0x11 第 32 字节。
        int off = -1;
        if (reportId == 0x31) off = 54;
        else if (reportId == 0x11) off = 32;
        else if (reportId == 0x01) off = ds5 ? 53 : 30;

        if (off < 0 || off >= reportLen) {
            r.status = L"报文格式不符";
            r.detail = L"收到的报文里没有预期的电量字段";
            r.outcome = Outcome::BadFrame;
            out->push_back(r);
            continue;
        }

        int lv = report[off] & 0x0F;
        int st = (report[off] >> 4) & 0x0F;
        if (lv > 10) {
            r.status = L"电量不可读";
            r.detail = L"电量字段超出有效范围，可能被其它软件改写";
            r.outcome = Outcome::BadFrame;
            out->push_back(r);
            continue;
        }

        switch (st) {
            case 0: r.level = lv * 10; r.status = L"放电中"; r.charging = 0; break;
            case 1: r.level = lv * 10; r.status = L"充电中"; r.charging = 1; break;
            case 2: r.level = 100; r.status = L"已充满"; r.charging = 1; break;
            case 0xA:
            case 0xB:
                r.level = -1;
                r.status = L"电池异常";
                r.detail = L"手柄上报电压或温度超出安全范围";
                r.charging = -1;
                r.outcome = Outcome::BadFrame;
                break;
            default:
                r.level = lv * 10;
                r.status = L"放电中";
                r.charging = 0;
                break;
        }
        out->push_back(r);
    }
}

// ------------------------------------------------------------------ 标准 HID

}  // namespace（下面三个是纯函数，导出给 --selftest 用，见 decode.h）

int ExtractBits(const uint8_t* payload, int payloadLen, int bitOffset, int bitSize) {
    // 描述符来自设备，任何字段都可能是畸形值：负偏移会读到缓冲区之前，
    // bitSize>32 的 1<<k 是有符号左移 UB。这里一次性全部拒绝。
    if (!payload || payloadLen <= 0) return -1;
    if (bitOffset < 0 || bitSize <= 0 || bitSize > 32) return -1;
    if ((long long)bitOffset + bitSize > (long long)payloadLen * 8) return -1;
    uint32_t value = 0;
    for (int k = 0; k < bitSize; ++k) {
        int pos = bitOffset + k;
        if ((payload[pos / 8] >> (pos % 8)) & 1u) value |= (1u << k);
    }
    return (int)value;
}

int NormalizeLevel(int value, int logicalMin, int logicalMax) {
    if (value < 0) return -1;
    long long lo = logicalMin, hi = logicalMax;
    if (hi <= lo) {
        // 描述符没声明范围时按 HID 惯例当 0..100；其它非法区间直接拒绝，
        // 不要像旧实现那样"hi<=100 就把原值当百分比"——0..1 的满电会被读成 1%。
        if (hi == 0) { lo = 0; hi = 100; }
        else return -1;
    }
    if (value < lo || value > hi) return -1;
    long long num = (long long)(value - lo) * 100;
    long long den = hi - lo;
    int pct = (int)((num + den / 2) / den);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

// 从 HidD_GetFeature 返回的缓冲区解出电量。抽成独立函数是为了能脱离设备做纯函数测试：
// 传入构造报文即可断言，不需要真的枚举硬件。
bool DecodeBatteryReport(const std::vector<uint8_t>& buf, const BatteryField& bf,
                         int* level, int* raw) {
    if (buf.size() < 2) return false;          // 只有 Report ID 的报文没有数据区
    const uint8_t* body = buf.data() + 1;      // 首字节永远是 Report ID
    int bodyLen = (int)buf.size() - 1;
    int v = ExtractBits(body, bodyLen, bf.bitOffset, bf.bitSize);
    if (v < 0) return false;
    int pct = NormalizeLevel(v, bf.logicalMin, bf.logicalMax);
    if (pct < 0) return false;
    if (raw) *raw = v;
    if (level) *level = pct;
    return true;
}

namespace {

bool PlausibleLogitechMv(int mv) {
    // 单节锂电的合理区间。内核对 >= 5000 mV 会警告"可能用错曲线"，
    // 2S 电池组不该走这条单节曲线，直接拒绝更安全。
    return mv >= 3000 && mv <= 5000;
}

}  // namespace

int DecodeLogitechVoltage(uint8_t b0, uint8_t b1) {
    int be = ((int)b0 << 8) | b1;
    if (PlausibleLogitechMv(be)) return be;
    int le = ((int)b1 << 8) | b0;
    if (PlausibleLogitechMv(le)) return le;
    return -1;
}

// Linux 内核 drivers/hid/hid-logitech-hidpp.c 的
// hidpp20_map_battery_capacity()：100 个递减电压阈值，
// 找到第一个 mv >= 阈值[i]，百分比 = 100 - i；一个都不满足即 0%。
// 内核自己也注明"这条曲线未必完美适配所有实现 BATTERY_VOLTAGE 的设备"，
// 所以结果只能当估算，界面标「≈」。
int LogitechVoltageToPercent(int millivolts) {
    static const int kCurve[100] = {
        4186, 4156, 4143, 4133, 4122, 4113, 4103, 4094, 4086, 4075,
        4067, 4059, 4051, 4043, 4035, 4027, 4019, 4011, 4003, 3997,
        3989, 3983, 3976, 3969, 3961, 3955, 3949, 3942, 3935, 3929,
        3922, 3916, 3909, 3902, 3896, 3890, 3883, 3877, 3870, 3865,
        3859, 3853, 3848, 3842, 3837, 3833, 3828, 3824, 3819, 3815,
        3811, 3808, 3804, 3800, 3797, 3793, 3790, 3787, 3784, 3781,
        3778, 3775, 3772, 3770, 3767, 3764, 3762, 3759, 3757, 3754,
        3751, 3748, 3744, 3741, 3737, 3734, 3730, 3726, 3724, 3720,
        3717, 3714, 3710, 3706, 3702, 3697, 3693, 3688, 3683, 3677,
        3671, 3666, 3662, 3658, 3654, 3646, 3633, 3612, 3579, 3537,
    };
    if (!PlausibleLogitechMv(millivolts)) return -1;
    for (int i = 0; i < 100; ++i) {
        if (millivolts >= kCurve[i]) return 100 - i;
    }
    return 0;
}

int LogitechBatteryPercent(int millivolts, bool full) {
    // 设备自报「已充满」时直接对齐 100%：这是 G HUB 的充满校准思路，
    // 每次充满都把读数重新对齐到该电池的最大电压。
    if (full) return 100;
    if (!PlausibleLogitechMv(millivolts)) return -1;
    // 充放电共用一条曲线。充电端电压偏高的问题交给上层的时间模型处理，
    // 绝不在这里按状态扣偏移——那会在拔线瞬间制造 19 点断层。
    return LogitechVoltageToPercent(millivolts);
}

namespace {

// 单调表线性插值。表里的 pct10 是 0.1% 单位，返回同样单位。
int LookupCurveDesc(const LogiCurvePoint* t, int n, int mv) {
    if (mv >= t[0].mv) return t[0].pct10;
    if (mv <= t[n - 1].mv) return t[n - 1].pct10;
    for (int i = 1; i < n; ++i) {
        if (mv >= t[i].mv) {
            int x0 = t[i - 1].mv, y0 = t[i - 1].pct10;
            int x1 = t[i].mv, y1 = t[i].pct10;
            if (x0 == x1) return y1;
            double f = (double)(y1 - y0) * (mv - x0) / (double)(x1 - x0);
            return y0 + (int)(f >= 0 ? f + 0.5 : f - 0.5);
        }
    }
    return t[n - 1].pct10;
}

int LookupCurveAsc(const LogiCurvePoint* t, int n, int mv) {
    if (mv <= t[0].mv) return t[0].pct10;
    if (mv >= t[n - 1].mv) return t[n - 1].pct10;
    for (int i = 1; i < n; ++i) {
        if (mv <= t[i].mv) {
            int x0 = t[i - 1].mv, y0 = t[i - 1].pct10;
            int x1 = t[i].mv, y1 = t[i].pct10;
            if (x0 == x1) return y1;
            double f = (double)(y1 - y0) * (mv - x0) / (double)(x1 - x0);
            return y0 + (int)(f >= 0 ? f + 0.5 : f - 0.5);
        }
    }
    return t[n - 1].pct10;
}

int Pct10ToPct(int p10) {
    if (p10 < 0) return -1;
    return (p10 + 5) / 10;
}

}  // namespace

int LogitechDischargePercent(int millivolts) {
    if (!PlausibleLogitechMv(millivolts)) return -1;
    const int n = (int)(sizeof(kLogiDischargeCurve) / sizeof(kLogiDischargeCurve[0]));
    return Pct10ToPct(LookupCurveDesc(kLogiDischargeCurve, n, millivolts));
}

int LogitechChargingPercent(int millivolts) {
    if (!PlausibleLogitechMv(millivolts)) return -1;
    const int n = (int)(sizeof(kLogiChargingCurve) / sizeof(kLogiChargingCurve[0]));
    return Pct10ToPct(LookupCurveAsc(kLogiChargingCurve, n, millivolts));
}

int LogitechTrickleLevel(int base, uint64_t stableElapsedMs) {
    if (base < 0) return -1;
    if (base >= 100) return 100;
    // 官方 equation_coefficiency="150"：每 150 秒涨 1%。
    uint64_t gained = stableElapsedMs / 150000ULL;
    if (gained > 100ULL) gained = 100ULL;
    int v = base + (int)gained;
    return v > 100 ? 100 : v;
}

int SmoothEstimatedLevel(int prev, int next, uint64_t elapsedMs) {
    if (prev < 0 || next < 0) return next;
    if (next > 100) next = 100;
    // 关机几小时再开机，电量本来就该直接跳到真实值，不能慢慢爬两小时。
    if (elapsedMs > 30ULL * 60 * 1000) return next;
    // 2 分钟一轮、2~3 小时充满，正常每轮涨 1~3 点。6 点足够宽松，
    // 只用来挡住"插线 / 拔线瞬间端电压跳变"造成的 20 点假变化。
    const int kMaxStep = 6;
    if (next > prev + kMaxStep) return prev + kMaxStep;
    if (next < prev - kMaxStep) return prev - kMaxStep;
    return next;
}

namespace {

// ---------------------------------------------------------------- 雷柏

// 雷柏办公系列（M200 / M200G / M200G Plus 这一代）在 2.4G 接收器下
// **没有任何可读的电量通道**，三条路都试过了：
//   1. 标准 HID 电量字段：接口里没有 0x06/0x20（用 HidP_GetValueCaps 逐条扫过）；
//   2. 游戏系列的私有通道（0xFF00:0x000E / report 0xBA）：握手、读寄存器、读电量
//      全部无应答，两种发送方式（SetOutputReport / WriteFile）都一样；
//   3. 唯一能读的 8 字节特性报告：鼠标持续活动 54 秒，一个字节都没变——
//      那是版本/能力信息，不是电量。
//
// 所以这里不做"读取"，只把它列出来并说明为什么没有数字。不列的话，用户会以为
// 程序没认出设备。和罗技 C542 那一代接收器是同一类处理。
const uint16_t RAPOO_VID = 0x24AE;

void ScanRapoo(const ScanCtx& ctx, std::vector<Reading>* out) {
    const std::vector<HidInfo>& all = *ctx.all;

    // 同一颗接收器的多个集合（鼠标 / 键盘 / 厂商）归成一台设备
    std::map<std::wstring, std::vector<const HidInfo*>> groups;
    for (const auto& d : all) {
        if (d.vid != RAPOO_VID) continue;
        groups[DeviceIdentity(d)].push_back(&d);
    }

    for (auto& kv : groups) {
        // 只报鼠标：雷柏也做键盘，键盘没有电量需求，列出来只是噪音。
        bool isMouse = false;
        const HidInfo* repr = nullptr;
        for (const HidInfo* d : kv.second) {
            if (d->usagePage == 0x0001 && d->usage == 0x0002) isMouse = true;
            if (!repr && !d->product.empty()) repr = d;
        }
        if (!isMouse) continue;
        if (!repr) repr = kv.second.front();

        Reading r;
        r.key = L"rapoo:" + kv.first;
        r.name = repr->product;
        r.kind = Kind::Mouse;
        r.vendor = L"雷柏";
        r.transport = L"2.4G 接收器";
        r.level = -1;
        r.charging = -1;
        r.status = L"硬件不支持读数";
        r.outcome = Outcome::Unsupported;
        r.detail = L"这只鼠标不上报电量：接收器对电量查询无应答，接口里也没有标准电量字段。"
                   L"接收器只报通用型号，无法区分 M200 / M200G / M200G Plus；"
                   L"只能看鼠标上的低电量指示灯";
        r.provider = L"rapoo";
        out->push_back(r);
    }
}

// ---------------------------------------------------------------- 链路表
//
// claims：这台接口归不归我。先到先得，所以表里的顺序就是优先级。
//         nullptr 表示"我不按设备认领"（BLE 走 GATT，不参与 HID 分派）。
//
// 各链路的认领条件，必须与对应 ScanXxx 内部的筛选保持一致。
bool ClaimsLogitech(const HidInfo& d) { return d.vid == LOGITECH_VID; }
bool ClaimsSony(const HidInfo& d) { return d.vid == SONY_VID; }
bool ClaimsCompx(const HidInfo& d) {
    if (IsNordic54(d.vid, d.pid)) return false;   // 这颗接收器走 Nordic 链路
    return IsCompxVendor(d.vid) || IsCompxCandidate(d);
}
bool ClaimsNordic(const HidInfo& d) { return IsNordic54(d.vid, d.pid); }
bool ClaimsRapoo(const HidInfo& d) { return d.vid == RAPOO_VID; }
bool ClaimsMchose(const HidInfo& d) { return LooksLikeMchose(d); }
bool ClaimsFlydigi(const HidInfo& d) { return LooksLikeFlydigi(d); }

// 罗技是两条函数：HID++ 通道 + "这代硬件不支持"的说明行
void ScanLogitechAll(const ScanCtx& ctx, std::vector<Reading>* out) {
    ScanLogitech(*ctx.all, out);
    if (ScanCancelRequested()) return;
    ScanLogitechNoHidpp(*ctx.all, out);
}

// BLE 不走 HID 枚举，签名里用不到 all
void ScanBleEntry(const ScanCtx&, std::vector<Reading>* out) { ScanBle(out); }

void ScanGeneric(const ScanCtx& ctx, std::vector<Reading>* out) {
    const std::vector<HidInfo>& all = *ctx.all;
    std::map<std::wstring, bool> seen;

    for (auto& d : all) {
        // 已被专用链路认领的设备不再走通用兜底，否则同一台设备会出现两条记录。
        // 判据来自认领表，不再在这里抄一遍各家的匹配条件。
        if (ctx.owner->count(d.path)) continue;

        HidDevice dev;
        if (!dev.Open(d.path)) continue;
        if (!dev.LoadDescriptor()) continue;

        BatteryField bf;
        if (!dev.FindBattery(&bf)) continue;

        int reportLen = 0;
        for (auto& f : dev.fields()) {
            if (f.kind == 2 && f.reportId == bf.reportId) {
                reportLen = std::max(reportLen, (f.bitOffset + f.bitSize + 7) / 8);
            }
        }
        reportLen = std::max(reportLen + 1, 3);

        uint16_t wantLen = d.featureLen > 1 ? d.featureLen
                                            : (uint16_t)std::min(255, std::max(reportLen + 1, 3));
        std::vector<uint8_t> buf;
        if (!dev.GetFeature(buf, wantLen, bf.reportId)) continue;

        int raw = 0, level = -1;
        if (!DecodeBatteryReport(buf, bf, &level, &raw)) continue;

        std::wstring name = d.product.empty() ? L"HID 外设" : d.product;
        // 同一台设备的多个集合只出一行；同型号两台各有各的 ContainerId。
        std::wstring key = L"hid:" + DeviceIdentity(d);
        if (seen[key]) continue;
        seen[key] = true;

        Reading r;
        r.key = key;
        r.name = name;
        r.kind = ClassifyName(name);
        r.vendor = d.manufacturer.empty() ? L"通用 HID" : d.manufacturer;
        r.transport = TransportOf(d.path, d.product);
        r.level = level;
        r.provider = L"generic_hid";
        wchar_t detail[96];
        swprintf(detail, 96, L"标准 HID 电量字段 · 原始值 %d", raw);
        r.detail = detail;
        out->push_back(r);
    }
}

const ProviderDesc kProviders[] = {
    // 顺序 = 优先级。越具体的越靠前。
    {L"logitech", L"罗技", ClaimsLogitech, true, ScanLogitechAll},
    {L"sony", L"索尼", ClaimsSony, true, ScanSony},
    {L"compx", L"厂商接收器", ClaimsCompx, true, ScanCompx},
    {L"nordic54", L"ATK", ClaimsNordic, true, ScanNordic54},
    {L"mchose", L"迈从", ClaimsMchose, true, ScanMchose},
    {L"flydigi", L"飞智", ClaimsFlydigi, true, ScanFlydigi},
    {L"rapoo", L"雷柏", ClaimsRapoo, true, ScanRapoo},        // 只列出，不读数
    {L"ble", L"蓝牙", nullptr, false, ScanBleEntry},          // 走 GATT，不参与 HID 分派
    {L"generic", L"通用 HID", nullptr, true, ScanGeneric},    // 兜底：收所有没人认领的
};

const int kProviderCount = (int)(sizeof(kProviders) / sizeof(kProviders[0]));

}  // namespace

static std::wstring HexLine(const uint8_t* p, int n) {
    wchar_t buf[256];
    std::wstring out;
    for (int i = 0; i < n && i < 24; ++i) {
        swprintf(buf, 256, L"%02X ", p[i]);
        out += buf;
    }
    return out;
}

std::wstring RawDump() {
    std::wstring out;
    std::vector<HidInfo> all = EnumHidDevices();
    for (auto& d : all) {
        if (d.vid != LOGITECH_VID || d.usagePage != 0xFF00) continue;
        out += L"\n[接口] " + d.path + L"\n";
        HppLink link;
        if (!link.Open(d.path)) {
            out += L"  打开失败\n";
            continue;
        }
        out += link.IsLongMode() ? L"  模式: 长 0x11\n" : L"  模式: 短 0x10\n";

        // 先确认特性 ID 的字节序：规范是小端，但实测这台设备要按大端发
        const uint16_t ids[] = {0x0003, 0x0005, 0x1000, 0x1001, 0x1004,
                                0x1814, 0x1802, 0x1805, 0x1300, 0x1B04};
        for (uint16_t fid : ids) {
            for (int order = 0; order < 2; ++order) {
                uint8_t p2[2] = {order == 0 ? (uint8_t)(fid & 0xFF) : (uint8_t)(fid >> 8),
                                 order == 0 ? (uint8_t)(fid >> 8) : (uint8_t)(fid & 0xFF)};
                HppReply r;
                bool ans = link.Request(0x01, 0x00, 0x00, p2, 2, &r, 700);
                wchar_t head[96];
                swprintf(head, 96, L"  feat 0x%04X [%s] -> ", fid,
                         order == 0 ? L"小端" : L"大端");
                out += head;
                if (!ans) {
                    out += L"超时\n";
                } else if (!r.ok) {
                    out += L"错误帧\n";
                } else {
                    out += HexLine(r.raw, r.rawLen);
                    out += L"\n";
                    int idx = r.params[0];
                    if (order == 1 && idx > 0) {
                        // 用拿到的索引直接取电量
                        HppReply b;
                        bool ab = link.Request(0x01, (uint8_t)idx, 0x00, nullptr, 0, &b, 700);
                        out += L"        -> 该特性 func0: ";
                        out += ab ? HexLine(b.raw, b.rawLen) : L"超时";
                        out += L"\n";
                    }
                }
            }
        }
        link.Close();
    }
    return out;
}

std::wstring LogitechDiagnostics() {
    std::wstring out;
    std::vector<HidInfo> all = EnumHidDevices();
    int found = 0;
    for (auto& d : all) {
        if (d.vid != LOGITECH_VID) continue;
        if (d.usagePage != 0xFF00) continue;
        if (d.usage != 0x0001 && d.usage != 0x0002) continue;
        found++;
        out += L"接口: " + d.path + L"\n";
        HppLink link;
        if (!link.Open(d.path)) {
            out += L"  打开失败\n";
            continue;
        }
        wchar_t buf[192];
        out += link.IsLongMode() ? L"  报文模式: 长(0x11)\n" : L"  报文模式: 短(0x10)\n";
        const int indices[] = {0xFF, 1, 2, 3, 4, 5, 6};
        for (int i : indices) {
            HppReply r;
            bool answered = link.Request((uint8_t)i, FEAT_ROOT, FUNC_GET_PROTOCOL,
                                         nullptr, 0, &r, 250);
            if (!answered) {
                swprintf(buf, 192, L"  idx=0x%02X: 无响应（设备休眠或通道为空）\n", i);
            } else if (!r.ok) {
                swprintf(buf, 192, L"  idx=0x%02X: 错误码 0x%02X\n", i, r.error);
            } else {
                swprintf(buf, 192, L"  idx=0x%02X: HID++ %d.%d\n", i, r.params[0],
                         r.paramLen > 1 ? r.params[1] : 0);
                out += buf;
                int fi = link.GetFeatureIndex((uint8_t)i, FEAT_BATTERY_LEVEL_STATUS, 500);
                int fv = link.GetFeatureIndex((uint8_t)i, FEAT_BATTERY_VOLTAGE, 500);
                int level = -1, volt = 0, ch = -1;
                bool estimated = false;
                std::wstring st;
                bool any = link.Battery((uint8_t)i, &level, &volt, &ch, &st, &estimated,
                                        false);
                swprintf(buf, 192,
                         L"      特性0x1000=%d 0x1001=%d | 电量=%d 电压=%d 充电=%d "
                         L"电压估算=%d any=%d\n",
                         fi, fv, level, volt, ch, (int)estimated, (int)any);
            }
            out += buf;
        }
        link.Close();
    }
    if (found == 0) out = L"未发现罗技 HID++ 接口\n";
    return out;
}

std::wstring g_scanTiming;   // 仅在 --list 的报告里用

void MonitorScan(std::vector<Reading>* out) {
    out->clear();
    ULONGLONG now = GetTickCount64();
    g_scanTiming.clear();

    g_scanTick++;
    g_fullSweep = (g_scanTick % 8 == 1);
    if (g_forceScan) {
        g_fullSweep = true;
        g_forceScan = false;
    }

    ULONGLONG t0 = GetTickCount64();
    std::vector<HidInfo> all = EnumHidDevices();
    ULONGLONG enumMs = GetTickCount64() - t0;

    // 认领表：每条链路先对每台接口表态，先到先得。兜底链路只处理没人认领的，
    // 所以"同一台设备出现两条记录"不再取决于几条 if 的先后顺序。
    std::map<std::wstring, const ProviderDesc*> owner;
    for (const auto& d : all) {
        for (int i = 0; i < kProviderCount; ++i) {
            const ProviderDesc& p = kProviders[i];
            if (!p.hidEnum || !p.claims) continue;
            if (p.claims(d)) {
                owner[d.path] = &p;
                break;
            }
        }
    }

    ScanCtx ctx{&all, &owner, g_fullSweep};

    int total = 0, readable = 0;
    int noReply = 0, busy = 0, badFrame = 0, unsupported = 0;
    std::wstring parts;

    for (int i = 0; i < kProviderCount; ++i) {
        const ProviderDesc& p = kProviders[i];
        std::vector<Reading> got;
        ULONGLONG start = GetTickCount64();
        p.scan(ctx, &got);
        ULONGLONG ms = GetTickCount64() - start;

        for (auto& r : got) r.updated = now;
        out->insert(out->end(), got.begin(), got.end());

        wchar_t seg[80];
        swprintf(seg, _countof(seg), L"%ls%ls %d 台/%llums",
                 parts.empty() ? L"" : L" · ", p.label, (int)got.size(), ms);
        parts += seg;

        // 退出时尽快收手：每个厂商链路都可能等设备超时，逐段检查能让退出从
        // "等一整个扫描周期"缩到"最多等当前这一段"。
        if (ScanCancelRequested()) break;
    }

    // 双模外设插数据线后会同时枚举出有线 + 无线两个 USB 设备，先合并再去统计，
    // 否则汇总里的「台数」会比面板上真实的行数多。
    MergeWiredWirelessDuplicates(out);
    total = (int)out->size();
    readable = noReply = busy = badFrame = unsupported = 0;
    for (const auto& r : *out) {
        if (r.level >= 0) {
            ++readable;
        } else {
            switch (r.outcome) {
                case Outcome::NoReply: ++noReply; break;
                case Outcome::Busy: ++busy; break;
                case Outcome::BadFrame: ++badFrame; break;
                case Outcome::Unsupported: ++unsupported; break;
                default: ++noReply; break;
            }
        }
    }

    // 一行汇总给用户看，一行分项给排查用。
    wchar_t head[256];
    swprintf(head, _countof(head),
             L"汇总: %d 台 · 可读 %d · 无读数 %d（未应答 %d / 占用 %d / 报文异常 %d / 不支持 %d） · 枚举 %llums",
             total, readable, total - readable, noReply, busy, badFrame, unsupported, enumMs);
    g_scanTiming = head;
    g_scanTiming += L"\n分项: " + parts;
}

// ------------------------------------------------------------------ 国产接收器诊断

// 读不到电量时让用户直接发这个：每个候选接口、两条命令的原始帧都在里面。
std::wstring VendorDiagnostics() {
    std::wstring out;
    std::vector<HidInfo> all = EnumHidDevices();
    int candidates = 0;

    for (const auto& d : all) {
        if (!IsCompxCandidate(d)) continue;
        candidates++;

        wchar_t head[512];
        swprintf(head, 512,
                 L"接口: %ls\n  VID:PID %04X:%04X  用法 %04X/%04X  报文 IN %u / OUT %u\n"
                 L"  产品名: %ls\n",
                 d.path.c_str(), d.vid, d.pid, d.usagePage, d.usage, d.inputLen,
                 d.outputLen, d.product.empty() ? L"（无）" : d.product.c_str());
        out += head;

        HidDevice dev;
        if (!dev.Open(d.path)) {
            out += L"  打开失败（可能被官方驱动独占）\n\n";
            continue;
        }

        uint8_t rep[COMPX_LEN];
        if (CompxAsk(dev, COMPX_CMD_BATTERY, rep)) {
            out += L"  电量原始帧: " + HexLine(rep, COMPX_LEN) + L"\n";
            CompxBattery bat;
            if (ParseCompxBattery(rep, &bat)) {
                swprintf(head, 512, L"    -> %d%%，%d mV，%ls\n",
                         bat.level, bat.millivolts,
                         bat.onCable ? L"插线" : L"未插线");
            } else {
                swprintf(head, 512, L"    -> 不可用（状态 0x%02X，校验%s）\n",
                         rep[2], CompxChecksumOk(rep) ? L"正确" : L"错误");
            }
            out += head;
        } else {
            out += L"  电量原始帧: 无应答（鼠标休眠 / 集合选错 / 被占用）\n";
        }

        if (CompxAsk(dev, COMPX_CMD_MODEL, rep)) {
            out += L"  型号原始帧: " + HexLine(rep, COMPX_LEN) + L"\n";
            int cu = 0, mo = 0;
            if (ParseCompxModel(rep, &cu, &mo)) {
                const wchar_t* nm = CompxModelNameOf(cu, mo);
                swprintf(head, 512, L"    -> 型号 %d:%d（%ls）\n", cu, mo,
                         nm ? nm : L"不在型号表里");
                out += head;
            } else {
                out += L"    -> 没有报型号\n";
            }
        } else {
            out += L"  型号原始帧: 无应答\n";
        }
        dev.Close();
        out += L"\n";
    }

    if (candidates == 0) {
        out = L"未发现 Compx / VGN 厂商集合（0xFF02/0x0002，17 字节进出）\n";
    }
    return out;
}

std::wstring DumpHidInterfaces() {
    return DumpHidImpl();
}

std::wstring BleDiagnostics() {
    return BleDiagnosticsImpl();
}

// ---------------------------------------------------------------- 状态合并

namespace {

bool IsWiredTransport(const std::wstring& t) {
    return t.find(L"有线") != std::wstring::npos || t == L"USB";
}

bool IsWirelessTransport(const std::wstring& t) {
    return t.find(L"2.4G") != std::wstring::npos ||
           t.find(L"接收器") != std::wstring::npos ||
           t.find(L"蓝牙") != std::wstring::npos;
}

// 只合并"明显是同一台双模外设"的两行。两条都是无线的同型号设备不合并：
// 它们各有各的 ContainerId，本来就是两只鼠标。
bool IsWiredWirelessDuplicate(const Reading& a, const Reading& b) {
    if (a.name.empty() || a.name != b.name) return false;
    if (a.provider != b.provider) return false;
    if (a.kind != b.kind) return false;
    if (a.kind != Kind::Mouse && a.kind != Kind::Keyboard) return false;
    bool aw = IsWiredTransport(a.transport), bw = IsWiredTransport(b.transport);
    if (aw == bw) return false;
    return (aw || IsWirelessTransport(a.transport)) &&
           (bw || IsWirelessTransport(b.transport));
}

// 保留哪条：充电中 > 有读数 > 本轮采样 > 在线 > 有线。
// 用户插线时通常想看到「充电中」那一行，而不是 2.4G 那边的旧读数。
int DuplicateScore(const Reading& r) {
    int s = 0;
    if (r.charging == 1) s += 1000;
    if (r.level >= 0) s += 500;
    if (!r.stale) s += 100;
    if (r.online) s += 50;
    if (IsWiredTransport(r.transport)) s += 10;
    return s;
}

}  // namespace

void MergeWiredWirelessDuplicates(std::vector<Reading>* list) {
    if (!list || list->size() < 2) return;
    std::vector<Reading> out;
    std::vector<bool> used(list->size(), false);
    for (size_t i = 0; i < list->size(); ++i) {
        if (used[i]) continue;
        size_t best = i;
        int merged = 0;
        for (size_t j = i + 1; j < list->size(); ++j) {
            if (used[j]) continue;
            if (!IsWiredWirelessDuplicate((*list)[i], (*list)[j])) continue;
            used[j] = true;
            ++merged;
            if (DuplicateScore((*list)[j]) > DuplicateScore((*list)[best])) best = j;
        }
        used[i] = true;
        Reading r = (*list)[best];
        if (merged > 0) {
            r.detail = L"已合并「USB 有线 + 无线」两个接口，同一台设备只显示一行";
        }
        out.push_back(r);
    }
    *list = std::move(out);
}

void MergeReading(Reading* incoming, const Reading* old, ULONGLONG now) {
    if (!incoming) return;
    Reading& r = *incoming;

    if (old && r.level < 0 && old->level >= 0) {
        // 休眠 / 未应答的占位记录不能覆盖最后一次有效读数，但也不能无限期沿用：
        // 电池耗尽或鼠标被关掉之后，占位记录每轮都会来，旧实现于是把最后那个
        // 20% 一直当成"现在"，看起来就像电量卡住了。
        const ULONGLONG kMaxCarryMs = 15ULL * 60 * 1000;   // 15 分钟
        const ULONGLONG age = (old->sampledAt && now > old->sampledAt)
                                  ? now - old->sampledAt : 0;
        if (old->sampledAt == 0 || age <= kMaxCarryMs) {
            r.level = old->level;
            r.voltage = old->voltage;
            r.charging = old->charging;
            r.approximate = old->approximate;   // 沿用旧值时别把「≈」弄丢
            r.officialCurve = old->officialCurve;
            r.sampledAt = old->sampledAt;
            r.stale = true;
        } else {
            // 超过时限：宁可显示"--"，也不拿一个几小时前的数字当现在。
            // 旧值写进 detail，信息不丢，但不再是主标题。
            wchar_t buf[160];
            swprintf(buf, 160,
                     L"最后一次读到 %d%%（%llu 分钟前），之后设备一直没有响应；"
                     L"可能是电池耗尽、鼠标已关闭或超出接收范围",
                     old->level, (unsigned long long)(age / 60000));
            r.detail = buf;
            r.status = L"长时间无响应";
            r.stale = true;
        }
    }

    // 只有"本轮真的读到了、且不是缓存"才刷新采样时刻。
    // BLE / 迈从的缓存记录自带 sampledAt 和 stale，这里不能再改。
    if (r.level >= 0 && !r.stale && r.sampledAt == 0) r.sampledAt = now;

    r.online = true;
    r.updated = now;        // 仍然记"这一轮看见它了"，用于 grace / 离场
}
