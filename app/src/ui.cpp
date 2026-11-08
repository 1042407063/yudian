// ui.cpp —— 托盘图标 + 自绘弹出面板
// ui.cpp —— 托盘图标、面板、提示卡片、右键菜单
//
// 负责：把一份 Reading 列表画出来；处理鼠标 / 键盘交互；动画与主题。
// 不负责：读设备、判断电量含义、决定何时刷新（main.cpp / providers.cpp）。
// 不变量：只消费 g_snapshot，不在绘制路径里做任何设备 I/O 或阻塞等待。
#include "app.h"
#include "hid.h"

#include <gdiplus.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>
#include <ctime>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <set>

#pragma comment(lib, "dwmapi.lib")

using namespace Gdiplus;

void UiHideToast();   // 定义在文件末尾，面板打开时要先收掉提示窗
// 关闭叉号在文件后半段定义（提示卡也用它），面板标题栏要提前调用
void DrawCloseGlyph(Graphics& g, float cx, float cy, float r, uint32_t color, float stroke);
// 刷新箭头同理：面板标题栏在文件前半段就要画，实现在后面
void DrawRefreshGlyph(Graphics& g, float cx, float cy, float r, uint32_t color,
                      float stroke);

// 主题只有一条解析路径。g_dark（绘制用的暗色标志）与 g_colors（配色表）
// 必须同时更新，否则两者会打架——提示卡图标就因为各自读主题，在深色卡上
// 画出了一整块亮白方块。
void SetThemePalette(bool dark);
void RefreshTheme();
void UiRefreshToastTheme();

namespace {


struct Colors {
    uint32_t bg, border, text, sub, track, tile, hover, accent;
    uint32_t ok, mid, low, critical;
    // 重新设计时追加：顶部/底部渐变、按压态、阴影色
    uint32_t bg2, press, shadow;
};

const Colors kLight = {
    0xF7F8FA,  // bg        卡片顶部：偏冷的浅灰，比纯白更像「材质」而不是一张白纸
    0xE6E6EB,  // border    发丝线，比旧版略冷
    0x141416,  // text      比 #1D1D1F 再深一点，白底上更实
    0x6F6F76,  // sub
    0xEDEDF1,  // track
    0xF6F6F9,  // tile
    0xF1F1F6,  // hover
    0x007AFF,  // accent
    0x2FB14E,  // ok
    0xE8900F,  // mid
    0xDB7B05,  // low
    0xE5484D,  // critical
    0xF1F2F6,  // bg2       卡片底部，约 2% 的渐变只负责"有厚度"，不该被看见
    0xE9E9EF,  // press
    0x000000,  // shadow
};

const Colors kDark = {
    0x1E1E22,
    0x3A3A40,
    0xF2F2F7,
    0x9C9CA4,
    0x3A3A40,
    0x2A2A2F,
    0x2C2C32,
    0x0A84FF,
    0x32D74B,
    0xFF9F0A,
    0xFF9F0A,
    0xFF453A,
    0x18181B,  // bg2
    0x34343A,  // press
    0x000000,  // shadow
};

const wchar_t* PANEL_CLASS = L"PeripheralBatteryPanel";

Colors g_colors = kLight;
bool g_dark = false;   // 与 g_colors 同步；只由 SetThemePalette / RefreshTheme 写入
HWND g_panel = nullptr;
HICON g_trayIcon = nullptr;
int g_dpi = 96;
int g_hover = -1;
bool g_tracking = false;
bool g_autostart = false;
RECT g_refreshRect{};
RECT g_switchRect{};
RECT g_closeRect{};
RECT g_quitRect{};
bool g_pressClose = false;
bool g_pressQuit = false;
bool g_quitArmed = false;      // 退出要按两次：第一次只是上膛
bool g_menuOpen = false;       // 右键菜单弹着时不要因为失焦把面板收掉
bool g_hidingSelf = false;     // 主动收起（× / Esc）不算"失焦"
ULONGLONG g_lastDeactivate = 0;
time_t g_lastUpdate = 0;   // 本轮数据到达时的墙钟时间，绘制只读不算
int g_scroll = 0;          // 列表滚动偏移（行）
bool g_scanning = false;   // 正在扫描：刷新按钮转圈、页脚显示"正在更新"
float g_spinAngle = 0.0f;
bool g_pressRefresh = false;
int g_spinTicks = 0;

// ---- 拖动 / 动画 / 分层窗口
bool g_dragPending = false;    // 在标题栏按下，还没超过阈值
bool g_dragging = false;
POINT g_dragStartCursor{};     // 按下时的屏幕坐标
POINT g_dragStartCard{};       // 按下时的卡片左上角
float g_animHover[4] = {0, 0, 0, 0};   // 刷新/关闭/退出/行的悬停淡入
float g_animSwitch = 0.0f;     // 开关滑块位置 0..1
float g_animOpen = 1.0f;       // 面板出现进度 0..1
bool g_animTimer = false;
int g_hoverMask = 0;           // bit0 刷新 bit1 关闭 bit2 退出

// ---- 行级增删动画
// 面板绘制用的是 g_display：设备离场时这一行还会留 ~220ms 播完折叠动画，
// 播完才真正从列表里删掉。g_snapshot 始终是"当前真实数据"，托盘提示读它。
struct RowAnim {
    float p = 1.0f;       // 0..1：同时驱动行高与不透明度
    bool leaving = false; // 正在离场
};
std::vector<Reading> g_display;
std::map<std::wstring, RowAnim> g_anims;
std::vector<Reading> g_lastFresh;    // 最近一次扫描的完整列表（决定最终顺序）
std::vector<Reading> g_pendingAdds;  // 等离场收完再进场的设备

const int LOGICAL_W = 400;
const int HEADER_H = 68;
const int ROW_H = 64;
const int FOOTER_H = 48;
const int PAD = 18;
const int MAX_ROWS = 5;
const int EMPTY_H = 176;
const int CORNER = 16;
// 阴影余量必须大于"模糊扩散 + 下移"，否则阴影会在窗口边缘被切平
const int SHADOW_MARGIN = 36;
const int SHADOW_BLUR = 8;      // 三次盒式模糊的半径
const int SHADOW_OFFSET = 10;   // 阴影整体下移

int S(int v) { return MulDiv(v, g_dpi, 96); }

int VisibleRows() { return std::min<int>(MAX_ROWS, (int)g_display.size()); }
int ScrollMax() { return std::max(0, (int)g_display.size() - MAX_ROWS); }

Color C(uint32_t rgb, BYTE a = 255) {
    return Color(a, (BYTE)((rgb >> 16) & 0xFF), (BYTE)((rgb >> 8) & 0xFF),
                 (BYTE)(rgb & 0xFF));
}

// 这里原来有个 BandColor()：列表行左侧那条彩色电量带的取色函数。
// 改成"只在数字上染色"之后没人调用了，-Wall 报 unused-function，已删除。

void MakeRoundRect(GraphicsPath& path, float x, float y, float w, float h, float r) {
    if (r <= 0.1f) {
        path.AddRectangle(RectF(x, y, w, h));
        return;
    }
    float d = r * 2;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

// 字体按 (字号, 字重) 缓存。重绘频繁，每次重建 GDI+ 字体太浪费。
std::map<int, Font*> g_fontCache;

// family: 0 = 微软雅黑（中文），1 = Segoe UI（数字，比例更接近 SF Pro）
Font* GetFont(float px, int weight, int family = 0) {
    int key = (int)std::lround(px * 10) * 1000 + (weight / 100) * 10 + family;
    auto it = g_fontCache.find(key);
    if (it != g_fontCache.end()) return it->second;
    int style = (weight >= 600) ? FontStyleBold : FontStyleRegular;
    const wchar_t* face = (family == 1) ? L"Segoe UI" : L"Microsoft YaHei UI";
    Font* f = new Font(face, px, style, UnitPixel);
    g_fontCache[key] = f;
    return f;
}

void ClearFontCache() {
    for (auto& kv : g_fontCache) delete kv.second;
    g_fontCache.clear();
}

void DrawStr(Graphics& g, const std::wstring& text, Font* font, uint32_t color,
              RectF rect, StringAlignment halign = StringAlignmentNear,
              StringAlignment valign = StringAlignmentCenter, bool ellipsis = true,
              float alpha = 1.0f) {
    StringFormat sf;
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    if (ellipsis) sf.SetTrimming(StringTrimmingEllipsisCharacter);
    sf.SetAlignment(halign);
    sf.SetLineAlignment(valign);
    SolidBrush brush(C(color, (BYTE)std::lround(255.0f * std::max(0.0f, alpha))));
    g.DrawString(text.c_str(), (INT)text.size(), font, rect, &sf, &brush);
}

// ------------------------------------------------------------------ 品类图标

void DrawKindIcon(Graphics& g, Kind kind, RectF box, uint32_t color, float alpha = 1.0f) {
    GraphicsState st = g.Save();
    g.TranslateTransform(box.X, box.Y);
    g.ScaleTransform(box.Width / 24.0f, box.Height / 24.0f);
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    BYTE a = (BYTE)std::lround(255.0f * std::max(0.0f, std::min(1.0f, alpha)));
    Pen pen(C(color, a), 1.6f);
    pen.SetLineJoin(LineJoinRound);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    SolidBrush brush(C(color, a));

    switch (kind) {
        case Kind::Mouse: {
            GraphicsPath p;
            MakeRoundRect(p, 6.6f, 2.8f, 10.8f, 18.4f, 5.2f);
            g.DrawPath(&pen, &p);
            g.DrawLine(&pen, 12.0f, 3.0f, 12.0f, 9.4f);
            Pen thick(C(color), 2.6f);
            thick.SetStartCap(LineCapRound);
            thick.SetEndCap(LineCapRound);
            g.DrawLine(&thick, 12.0f, 5.8f, 12.0f, 7.6f);
            break;
        }
        case Kind::Keyboard: {
            GraphicsPath p;
            MakeRoundRect(p, 2.6f, 7.0f, 18.8f, 10.2f, 2.6f);
            g.DrawPath(&pen, &p);
            for (float cx : {6.2f, 9.4f, 12.6f, 15.8f, 18.8f}) {
                g.FillEllipse(&brush, cx - 0.85f, 10.6f - 0.85f, 1.7f, 1.7f);
            }
            GraphicsPath bar;
            MakeRoundRect(bar, 8.0f, 13.2f, 8.0f, 1.7f, 0.85f);
            g.FillPath(&brush, &bar);
            break;
        }
        case Kind::Controller: {
            GraphicsPath p;
            MakeRoundRect(p, 2.2f, 8.2f, 19.6f, 9.0f, 4.5f);
            g.DrawPath(&pen, &p);
            g.DrawLine(&pen, 8.2f, 11.0f, 8.2f, 14.4f);
            g.DrawLine(&pen, 6.6f, 12.7f, 9.8f, 12.7f);
            g.FillEllipse(&brush, 14.6f, 10.8f, 1.8f, 1.8f);
            g.FillEllipse(&brush, 16.8f, 12.8f, 1.8f, 1.8f);
            break;
        }
        case Kind::Headset: {
            g.DrawArc(&pen, RectF(4.6f, 4.4f, 14.8f, 14.8f), 180.0f, 180.0f);
            GraphicsPath a;
            MakeRoundRect(a, 2.6f, 12.4f, 3.6f, 6.2f, 1.6f);
            GraphicsPath b;
            MakeRoundRect(b, 17.8f, 12.4f, 3.6f, 6.2f, 1.6f);
            g.DrawPath(&pen, &a);
            g.DrawPath(&pen, &b);
            break;
        }
        default: {
            GraphicsPath p;
            MakeRoundRect(p, 3.2f, 8.0f, 14.2f, 8.4f, 2.2f);
            g.DrawPath(&pen, &p);
            GraphicsPath cap;
            MakeRoundRect(cap, 18.2f, 10.4f, 2.2f, 3.6f, 1.0f);
            g.FillPath(&brush, &cap);
            GraphicsPath fill;
            MakeRoundRect(fill, 5.6f, 10.4f, 5.2f, 3.6f, 1.0f);
            g.FillPath(&brush, &fill);
            break;
        }
    }
    g.Restore(st);
}

// ------------------------------------------------------------------ 托盘图标

// ------------------------------------------------------------------ 图标

// 图标只有一个样子：和 exe 完全相同的墨底 + 白闪电，**不随主题反色**。
// 之前按任务栏明暗翻成白底黑闪电，结果深色下托盘是白的、提示卡是黑的，
// 同一个程序出现两套视觉，自己跟自己打架。
// 唯一按主题调整的是深色下补一圈 1px 描边：墨底和深色任务栏太接近，
// 没有轮廓就只剩一个白闪电悬在任务栏上。
void DrawChipGlyph(Graphics& g, float s, bool dark) {
    GraphicsPath bg;
    MakeRoundRect(bg, 0.7f * s, 0.7f * s, 22.6f * s, 22.6f * s, 6.8f * s);
    SolidBrush fill(C(0x1D1D1F));
    g.FillPath(&fill, &bg);

    if (dark) {
        Pen edge(C(0xFFFFFF, 66), std::max(1.0f, 0.8f * s));   // 25% 白，16px 下也留得住轮廓
        edge.SetLineJoin(LineJoinRound);
        g.DrawPath(&edge, &bg);
    }

    PointF v[] = {
        PointF(14.6f * s, 3.8f * s), PointF(9.4f * s, 12.4f * s),
        PointF(12.3f * s, 12.4f * s), PointF(10.6f * s, 19.6f * s),
        PointF(15.6f * s, 10.4f * s), PointF(12.6f * s, 10.4f * s),
    };
    SolidBrush bolt(C(0xFFFFFF));
    g.FillPolygon(&bolt, v, 6);
}

HICON MakeTrayIcon(int size) {
    Bitmap bmp(size, size, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    g.Clear(Color(0, 0, 0, 0));
    // 托盘图标 = 程序图标，恒定不变。
    // 托盘里放会变的数字会一直抓到余光，而且和 exe 图标不是同一个东西。
    // 电量看面板、悬停提示和低电量提醒。
    DrawChipGlyph(g, (float)size / 24.0f, g_dark);

    HBITMAP color = nullptr;
    bmp.GetHBITMAP(Color(0, 0, 0, 0), &color);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    if (color) DeleteObject(color);
    if (mask) DeleteObject(mask);
    return icon;
}

// ------------------------------------------------------------------ 面板绘制

int PanelWidth() { return S(LOGICAL_W); }

int PanelHeight(int count) {
    int rows = std::min(count, MAX_ROWS);
    int listH = count > 0 ? rows * S(ROW_H) + S(4) : S(EMPTY_H);
    int h = S(HEADER_H) + 1 + listH + 1 + S(FOOTER_H);
    return std::min(h, S(560));
}

int AnimatedRowHeight(const std::wstring& key) {
    auto it = g_anims.find(key);
    float p = (it == g_anims.end()) ? 1.0f : it->second.p;
    return (int)std::lround((float)S(ROW_H) * p);
}

// 面板此刻该有多高。行高是动画的，增删过程中每一帧都可能不一样。
int PanelHeightNow() {
    int listH = S(EMPTY_H);
    if (!g_display.empty()) {
        listH = S(4);
        int visible = VisibleRows();
        for (int i = 0; i < visible; ++i) {
            int di = g_scroll + i;
            if (di >= (int)g_display.size()) break;
            listH += AnimatedRowHeight(g_display[di].key);
        }
    }
    int h = S(HEADER_H) + 1 + listH + 1 + S(FOOTER_H);
    return std::min(h, S(560));
}

// 窗口比卡片大一圈：四周的余量用来画柔和阴影。所有定位、命中测试
// 都换算成卡片坐标，窗口坐标只在 SetWindowPos / UpdateLayeredWindow 出现。
int ShadowPx() { return S(SHADOW_MARGIN); }
int WindowWidth() { return PanelWidth() + ShadowPx() * 2; }
int WindowHeight(int count) { return PanelHeight(count) + ShadowPx() * 2; }
int WindowHeightNow() { return PanelHeightNow() + ShadowPx() * 2; }

uint32_t Mix(uint32_t a, uint32_t b, float t) {
    t = std::max(0.0f, std::min(1.0f, t));
    auto ch = [&](int shift) {
        int x = (a >> shift) & 0xFF, y = (b >> shift) & 0xFF;
        return (uint32_t)std::lround(x + (y - x) * t);
    };
    return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

// 电池胶囊：细描边 + 实心填充，苹果菜单栏那种形态
void DrawBatteryPill(Graphics& g, float x, float y, float bodyW, float bodyH,
                     int level, uint32_t ink, float alpha = 1.0f) {
    float r = bodyH * 0.36f;
    BYTE stroke = (BYTE)std::lround(170.0f * std::max(0.0f, alpha));
    BYTE solid = (BYTE)std::lround(255.0f * std::max(0.0f, alpha));
    Pen pen(C(ink, stroke), 1.1f);
    pen.SetLineJoin(LineJoinRound);
    GraphicsPath body;
    MakeRoundRect(body, x, y, bodyW, bodyH, r);
    g.DrawPath(&pen, &body);

    float nubW = bodyW * 0.105f;
    GraphicsPath nub;
    MakeRoundRect(nub, x + bodyW + 0.9f, y + bodyH * 0.28f, nubW, bodyH * 0.44f,
                  nubW * 0.45f);
    SolidBrush nb(C(ink, stroke));
    g.FillPath(&nb, &nub);

    float pad = bodyH * 0.23f;
    float innerW = bodyW - pad * 2.0f;
    float innerH = bodyH - pad * 2.0f;
    if (level > 0) {
        float w = std::max(innerW * std::min(1.0f, level / 100.0f), innerH * 0.9f);
        GraphicsPath f;
        MakeRoundRect(f, x + pad, y + pad, w, innerH, innerH * 0.38f);
        SolidBrush fb(C(ink, solid));
        g.FillPath(&fb, &f);
    }
}

void DrawContent(Graphics& g, int w, int h) {
    {
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        // 分层窗口上不能用 ClearType：亚像素抗锯齿需要不透明底色，
        // 在带 alpha 的画布上会渗色。灰度 AA 在任何背景上都成立。
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

        float dpi = g_dpi / 96.0f;

        // 卡片：1% 的垂直渐变 + 1px 描边。渐变只负责"有厚度"，
        // 一旦能看出色差就变成廉价拟物了。
        {
            GraphicsPath card;
            MakeRoundRect(card, 0.5f, 0.5f, (float)w - 1.0f, (float)h - 1.0f,
                          (float)S(CORNER));

            // 不透明底：以前这里会铺一张「背后的桌面」当毛玻璃，但那是一张抓屏照片，
            // 桌面一变就成了已经不存在的东西；改成定时重拍又带来停顿感和额外开销。
            // 卡片本身不需要透视——1px 描边 + 柔和阴影已经足够把它和背景分开。
            LinearGradientBrush bgGrad(PointF(0.0f, 0.0f), PointF(0.0f, (REAL)h),
                                       C(g_colors.bg), C(g_colors.bg2));
            g.FillPath(&bgGrad, &card);
            Pen border(C(g_colors.border), 1.0f);
            g.DrawPath(&border, &card);
        }

        int online = 0;
        int lowest = -1;
        for (auto& d : g_snapshot) {
            if (!d.online) continue;
            online++;
            if (d.level >= 0 && (lowest < 0 || d.level < lowest)) lowest = d.level;
        }

        // ---- 顶部
        Font* fTitle = GetFont(16.0f * dpi, FW_BOLD);
        Font* fSub = GetFont(11.5f * dpi, FW_NORMAL);
        DrawStr(g, L"余电", fTitle, g_colors.text,
                RectF((float)S(PAD), (float)S(12), (float)S(180), (float)S(24)),
                StringAlignmentNear, StringAlignmentNear, false);

        std::wstring sub = std::to_wstring(online) + L" 台在线";
        if (lowest >= 0) sub += L" · 最低 " + std::to_wstring(lowest) + L"%";
        DrawStr(g, sub, fSub, g_colors.sub,
                RectF((float)S(PAD), (float)S(38), (float)S(240), (float)S(16)),
                StringAlignmentNear, StringAlignmentNear, false);

        // 右上角两颗按钮。位置从右往左排：最右永远是关闭，刷新在它左边。
        float btn = (float)S(28);
        float closeX = (float)(w - S(PAD)) - btn;
        float refreshX = closeX - (float)S(6) - btn;
        float btnY = (float)S(11);
        float cr = btn / 2.0f + (float)S(3);

        // ---- 刷新
        {
            float ccx = refreshX + btn / 2.0f, ccy = btnY + btn / 2.0f;

            // 悬停/按压：圆形底按动画进度淡入，硬切会很生硬
            float hov = g_pressRefresh ? 1.0f : g_animHover[0];
            if (hov > 0.01f) {
                SolidBrush cb(C(g_pressRefresh ? g_colors.press : g_colors.hover,
                                (BYTE)std::lround(255.0f * hov)));
                g.FillEllipse(&cb, ccx - cr, ccy - cr, cr * 2, cr * 2);
            }

            uint32_t iconInk = Mix(g_colors.sub, g_colors.text, hov);
            if (g_scanning) {
                // 环形 spinner：淡色轨道 + 270° 圆弧，比旋转文字字形干净
                float r = (float)S(6.6f);
                Pen track(C(g_colors.sub, 55), (float)S(2.1f));
                g.DrawEllipse(&track, ccx - r, ccy - r, r * 2, r * 2);
                Pen arc(C(g_colors.text, 235), (float)S(2.1f));
                arc.SetStartCap(LineCapRound);
                arc.SetEndCap(LineCapRound);
                g.DrawArc(&arc, ccx - r, ccy - r, r * 2, r * 2, g_spinAngle, 270.0f);
            } else {
                // 半径与扫描态轨道一致，切换时不会跳大小；线宽比关闭叉号略粗，
                // 圆弧的视觉重量本来就比直线轻一档
                DrawRefreshGlyph(g, ccx, ccy, (float)S(6.6f), iconInk, (float)S(2.1f));
            }
            g_refreshRect.left = (LONG)(ccx - cr);
            g_refreshRect.top = (LONG)(ccy - cr);
            g_refreshRect.right = (LONG)(ccx + cr);
            g_refreshRect.bottom = (LONG)(ccy + cr);
        }

        // ---- 关闭（只关面板，程序留在托盘）
        {
            float ccx = closeX + btn / 2.0f, ccy = btnY + btn / 2.0f;
            float hov = g_pressClose ? 1.0f : g_animHover[1];
            if (hov > 0.01f) {
                SolidBrush cb(C(g_pressClose ? g_colors.press : g_colors.hover,
                                (BYTE)std::lround(255.0f * hov)));
                g.FillEllipse(&cb, ccx - cr, ccy - cr, cr * 2, cr * 2);
            }
            DrawCloseGlyph(g, ccx, ccy, (float)S(4.8f),
                           Mix(g_colors.sub, g_colors.text, hov),
                           (float)S(1.7f));
            g_closeRect.left = (LONG)(ccx - cr);
            g_closeRect.top = (LONG)(ccy - cr);
            g_closeRect.right = (LONG)(ccx + cr);
            g_closeRect.bottom = (LONG)(ccy + cr);
        }

        Pen hair(C(g_colors.border), 1.0f);
        int yHeader = S(HEADER_H);
        // 0.5 像素对齐：1px 线画在整数坐标上会被摊成两条半透明线
        g.DrawLine(&hair, 0.0f, (float)yHeader + 0.5f, (float)w, (float)yHeader + 0.5f);

        int count = (int)g_display.size();
        if (count == 0) {
            float cy = (float)yHeader + (float)S(32);
            DrawKindIcon(g, Kind::Other,
                         RectF((float)w / 2.0f - (float)S(17), cy,
                               (float)S(34), (float)S(34)), g_colors.sub);
            Font* f1 = GetFont(13.0f * dpi, FW_NORMAL);
            Font* f2 = GetFont(11.0f * dpi, FW_NORMAL);
            DrawStr(g, L"未发现可读取电量的外设", f1, g_colors.text,
                    RectF(0, cy + (float)S(46), (float)w, (float)S(20)),
                    StringAlignmentCenter);
            DrawStr(g, L"请确认设备已开机，接收器或蓝牙已连接", f2, g_colors.sub,
                    RectF((float)S(PAD), cy + (float)S(72),
                          (float)(w - S(PAD) * 2), (float)S(16)),
                    StringAlignmentCenter, StringAlignmentNear);
            DrawStr(g, L"无线鼠标休眠时需先晃动唤醒", f2, g_colors.sub,
                    RectF((float)S(PAD), cy + (float)S(92),
                          (float)(w - S(PAD) * 2), (float)S(16)),
                    StringAlignmentCenter, StringAlignmentNear);
        } else {
            Font* fName = GetFont(13.0f * dpi, FW_NORMAL);
            Font* fMeta = GetFont(11.0f * dpi, FW_NORMAL);
            Font* fPct = GetFont(15.5f * dpi, FW_BOLD, 1);

            int visible = VisibleRows();
            float rightEdge = (float)(w - S(PAD));
            float pctW = (float)S(46);
            float pctX = rightEdge - pctW;
            float pillW = (float)S(28), pillH = (float)S(12);
            float pillX = pctX - (float)S(12) - pillW;
            float textW = pillX - (float)S(56) - (float)S(12);

            // 行高是动画的：进入从 0 长到 ROW_H，离场缩回 0。
            // 内容按整行高度画，再裁到当前高度——这样文字不会被压扁，
            // 而是像 iOS 列表那样被"揭开/收起"。
            float top = (float)yHeader;
            for (int i = 0; i < visible; ++i) {
                int di = g_scroll + i;
                if (di >= count) break;
                const Reading& d = g_display[di];

                auto ai = g_anims.find(d.key);
                float p = (ai == g_anims.end()) ? 1.0f : ai->second.p;
                p = std::max(0.0f, std::min(1.0f, p));
                bool leavingRow = (ai != g_anims.end() && ai->second.leaving);
                float rowH = (float)S(ROW_H) * p;
                if (rowH < 0.5f) continue;

                // 内容比高度先走完：离场时快速淡出（否则会残留一条被切掉的字），
                // 进场时前半程就淡入，避免"先看到一个透明框再显字"。
                // 进场：先让空行把位置撑开（p<0.65 不画内容，避免出现被切掉半截的字），
                // 最后 1/3 段再把文字淡入。离场反过来，先快速淡出再收高度。
                float alpha = leavingRow
                                  ? p * p
                                  : std::max(0.0f, std::min(1.0f, (p - 0.65f) / 0.35f));

                GraphicsState clip = g.Save();
                g.SetClip(RectF(0.0f, top, (float)w, rowH));

                // 行悬停：淡入的高亮块，比直接切颜色柔和
                float hov = (i == g_hover) ? g_animHover[3] * alpha : 0.0f;
                if (hov > 0.01f) {
                    GraphicsPath hp;
                    MakeRoundRect(hp, (float)S(8), top + (float)S(4),
                                  (float)(w - S(16)), (float)S(ROW_H - 8), (float)S(10));
                    SolidBrush hb(C(g_colors.hover, (BYTE)std::lround(255.0f * hov)));
                    g.FillPath(&hb, &hp);
                }

                // 品类图标：不垫色块，苹果列表里图标就是图标
                float iy = top + ((float)S(ROW_H) - (float)S(22)) / 2.0f;
                DrawKindIcon(g, d.kind,
                             RectF((float)S(PAD), iy, (float)S(22), (float)S(22)),
                             d.online ? g_colors.sub : Mix(g_colors.sub, g_colors.bg, 0.55f),
                             alpha);

                uint32_t nameColor = d.online ? g_colors.text : g_colors.sub;
                RectF nameRect((float)S(56), top + (float)S(12), textW, (float)S(20));

                Font* fNameRow = fName;
                RectF measured;
                g.MeasureString(d.name.c_str(), (INT)d.name.size(), fNameRow,
                                PointF(0.0f, 0.0f), &measured);
                if (measured.Width > nameRect.Width) {
                    fNameRow = GetFont(11.5f * dpi, FW_NORMAL);
                }
                DrawStr(g, d.name, fNameRow, nameColor, nameRect,
                        StringAlignmentNear, StringAlignmentCenter, true, alpha);

                std::wstring meta = d.transport;
                if (!d.vendor.empty()) {
                    if (!meta.empty()) meta += L" · ";
                    meta += d.vendor;
                }
                if (!meta.empty()) meta += L" · ";
                // 缓存值（休眠中、或 BLE 这一轮没刷新）必须把年龄摆出来，
                // 否则"设备休眠 · 82%"会被读成"刚刚读到 82%"。
                if (d.online && !d.stale) {
                    meta += d.StatusText();
                } else {
                    meta += d.StatusText() + L" · 上次 " + d.AgeText();
                }
                // 只有电压的设备必须写清百分比从哪来：G913/G915 用的是 G HUB
                // 官方曲线（和官方软件同源），其余罗技型号用通用表。
                // 设备自报的（迈从 / 飞智 / 索尼等）两者都不标。
                if (d.level >= 0) {
                    // 面板一行放不下「G HUB 官方曲线」，缩写；全称在 detail 里。
                    if (d.officialCurve) meta += L" · 官方曲线";
                    else if (d.approximate) meta += L" · 电压估算";
                }
                DrawStr(g, meta, fMeta, g_colors.sub,
                        RectF((float)S(56), top + (float)S(35), textW, (float)S(15)),
                        StringAlignmentNear, StringAlignmentCenter, true, alpha);

                // 颜色只在两种情况下出现：低电量告警、充电中。
                // 充电中整行电量（数字 + 电池胶囊）用绿色 g_colors.ok，
                // 优先级高于低电量红/橙——插着线就不该报低电量。
                uint32_t lvlInk = (!d.online || d.level < 0)
                                      ? g_colors.sub
                                      : (d.charging == 1
                                             ? g_colors.ok
                                             : (d.level <= 10
                                                    ? g_colors.critical
                                                    : (d.level <= 20 ? g_colors.low
                                                                     : g_colors.text)));

                // 右边界固定在 pctX + pctW，但把矩形向左放宽 S(16)：
                // 「100%」比「94%」宽约 8px，按两位数留宽会把百分号裁掉。
                // 估算值多一个「≈」，再往左借 S(8)。
                // 官方曲线不加「≈」：数值与 G HUB 同源，加了反而像我们自己猜的。
                bool showApprox = d.approximate && !d.officialCurve;
                std::wstring pctText = d.level >= 0
                    ? ((showApprox ? L"≈" : L"") + std::to_wstring(d.level) + L"%")
                    : L"--";
                float pctExtra = showApprox ? (float)S(8) : 0.0f;
                DrawStr(g, pctText, fPct, lvlInk,
                        RectF(pctX - (float)S(16) - pctExtra, top + (float)S(21),
                              pctW + (float)S(16) + pctExtra, (float)S(22)),
                        StringAlignmentFar, StringAlignmentCenter, false, alpha);

                float pillY = top + ((float)S(ROW_H) - pillH) / 2.0f;
                DrawBatteryPill(g, pillX, pillY, pillW, pillH, d.level, lvlInk, alpha);

                if (i < visible - 1 && di < count - 1) {
                    // 分隔线跟着"当前行高"走，展开过程中也能看清行的边界
                    float sepA = leavingRow ? alpha : std::min(1.0f, p * 2.0f);
                    Pen sep(C(g_colors.border, (BYTE)std::lround(255.0f * sepA)), 1.0f);
                    g.DrawLine(&sep, (float)S(56), top + rowH + 0.5f,
                               rightEdge, top + rowH + 0.5f);
                }

                g.Restore(clip);
                top += rowH;
            }

            if (ScrollMax() > 0) {
                float trackX = (float)(w - S(8));
                float trackW = (float)S(3);
                float trackY = (float)yHeader + (float)S(10);
                float trackH = (float)(h - S(FOOTER_H) - yHeader - S(20));
                float ratio = (float)visible / (float)count;
                float thumbH = std::max(trackH * ratio, (float)S(20));
                float maxScroll = (float)ScrollMax();
                float progress = maxScroll > 0 ? (float)g_scroll / maxScroll : 0.0f;
                float thumbY = trackY + (trackH - thumbH) * progress;
                GraphicsPath thumb;
                MakeRoundRect(thumb, trackX, thumbY, trackW, thumbH, trackW / 2.0f);
                SolidBrush thb(C(g_colors.sub, 80));
                g.FillPath(&thb, &thumb);
            }
        }

        // ---- 底部
        int yFooter = h - S(FOOTER_H);
        g.DrawLine(&hair, 0.0f, (float)yFooter + 0.5f, (float)w, (float)yFooter + 0.5f);

        Font* fFoot = GetFont(11.0f * dpi, FW_NORMAL);
        wchar_t stamp[96];
        int iv = CurrentInterval();
        if (g_quitArmed) {
            // 退出是两步的：先把话说明白，再等第二下
            DrawStr(g, L"再点一次「确认」退出", fFoot, g_colors.critical,
                    RectF((float)S(PAD), (float)yFooter, (float)S(204),
                          (float)S(FOOTER_H)));
            goto footer_switch;
        }
        if (g_scanning) {
            // 点下去立刻有反馈，不用等扫描结束
            DrawStr(g, L"正在更新…", fFoot, g_colors.sub,
                    RectF((float)S(PAD), (float)yFooter, (float)S(204),
                          (float)S(FOOTER_H)));
            goto footer_switch;
        }
        if (g_lastUpdate == 0) {
            swprintf(stamp, 96, L"--:--:-- 更新 · %d %s自动刷新",
                     iv >= 60 ? iv / 60 : iv, iv >= 60 ? L"分钟" : L"秒");
        } else {
            struct tm tmv;
            localtime_s(&tmv, &g_lastUpdate);
            if (iv >= 60) {
                swprintf(stamp, 96, L"%02d:%02d:%02d 更新 · %d 分钟自动刷新",
                         tmv.tm_hour, tmv.tm_min, tmv.tm_sec, iv / 60);
            } else {
                swprintf(stamp, 96, L"%02d:%02d:%02d 更新 · %d 秒自动刷新",
                         tmv.tm_hour, tmv.tm_min, tmv.tm_sec, iv);
            }
        }
        DrawStr(g, stamp, fFoot, g_colors.sub,
                RectF((float)S(PAD), (float)yFooter, (float)S(204), (float)S(FOOTER_H)));

footer_switch:
        // 页脚右侧：退出 → 自启开关。退出放最右，和标题栏的关闭分开，
        // 两个"关程序"的按钮挨在一起最容易点错。
        float qW = (float)S(36), qH = (float)S(22);
        float qX = (float)(w - S(PAD)) - qW;
        float qY = (float)yFooter + ((float)S(FOOTER_H) - qH) / 2.0f;
        {
            float hov = g_quitArmed ? 1.0f : (g_pressQuit ? 1.0f : g_animHover[2]);
            if (hov > 0.01f) {
                GraphicsPath qp;
                MakeRoundRect(qp, qX, qY, qW, qH, qH / 2.0f);
                SolidBrush qb(C(g_quitArmed ? g_colors.critical : g_colors.hover,
                                (BYTE)std::lround(255.0f * hov)));
                g.FillPath(&qb, &qp);
            }
        }
        DrawStr(g, g_quitArmed ? L"确认" : L"退出", fFoot,
                g_quitArmed ? 0xFFFFFF
                            : Mix(g_colors.sub, g_colors.text, g_animHover[2]),
                RectF(qX, qY, qW, qH), StringAlignmentCenter, StringAlignmentCenter, false);
        g_quitRect.left = (LONG)qX;
        g_quitRect.top = (LONG)(qY - S(4));
        g_quitRect.right = (LONG)(qX + qW);
        g_quitRect.bottom = (LONG)(qY + qH + S(4));

        float swW = (float)S(38), swH = (float)S(22);
        float swX = qX - (float)S(10) - swW;
        float swY = (float)yFooter + ((float)S(FOOTER_H) - swH) / 2.0f;
        DrawStr(g, L"开机自启", fFoot, g_colors.sub,
                RectF(swX - (float)S(70), (float)yFooter, (float)S(62), (float)S(FOOTER_H)),
                StringAlignmentFar);

        GraphicsPath st;
        MakeRoundRect(st, swX, swY, swW, swH, swH / 2.0f);
        SolidBrush sb(C(Mix(g_colors.track, g_colors.ok, g_animSwitch)));
        g.FillPath(&sb, &st);
        float knob = swH - (float)S(4);
        float kx = swX + (float)S(2) + (swW - knob - (float)S(4)) * g_animSwitch;
        SolidBrush kb(C(0xFFFFFF));
        g.FillEllipse(&kb, kx, swY + (float)S(2), knob, knob);

        g_switchRect.left = (LONG)(swX - S(70));
        g_switchRect.top = (LONG)(swY - S(6));
        g_switchRect.right = (LONG)(swX + swW);
        g_switchRect.bottom = (LONG)(swY + swH + S(6));
    }
}

// ------------------------------------------------------------------ 分层窗口：阴影与呈现
//
// 面板改成 WS_EX_LAYERED + UpdateLayeredWindow 自绘：
//   · 只有逐像素 alpha 才能画出真正柔和的阴影（CS_DROPSHADOW 是硬边的一像素轮廓）
//   · 出现动画、悬停淡入也才有地方落脚
// 窗口比卡片大一圈，四周是阴影余量；鼠标命中要按卡片坐标算。

uint8_t g_mulTable[256 * 256];
bool g_mulReady = false;

void EnsureMulTable() {
    if (g_mulReady) return;
    for (int a = 0; a < 256; ++a)
        for (int v = 0; v < 256; ++v)
            g_mulTable[a * 256 + v] = (uint8_t)((v * a + 127) / 255);
    g_mulReady = true;
}

struct PanelSurface {
    HBITMAP dib = nullptr;
    HDC dc = nullptr;
    void* bits = nullptr;
    HGDIOBJ old = nullptr;   // 创建时被 SelectObject 顶掉的原对象，销毁前要还回去
    int w = 0, h = 0;
};

PanelSurface g_surface;
Gdiplus::Bitmap* g_shadowBmp = nullptr;
int g_shadowW = 0, g_shadowH = 0;
bool g_shadowDark = false;
Gdiplus::Bitmap* g_canvas = nullptr;

void DestroySurface() {
    // Win32 不允许删除仍被选入 DC 的位图：DeleteObject 会失败，随后 DeleteDC
    // 也不会替我们删它，于是每次面板尺寸变化（设备增删、DPI 变化）漏一个 DIB。
    // 正确顺序是把原对象选回去，再删 DC，最后删位图。
    if (g_surface.dc) {
        if (g_surface.old) SelectObject(g_surface.dc, g_surface.old);
        DeleteDC(g_surface.dc);
    }
    if (g_surface.dib) DeleteObject(g_surface.dib);
    g_surface = PanelSurface{};
}

bool EnsureSurface(int w, int h) {
    if (g_surface.dib && g_surface.w == w && g_surface.h == h) return true;
    DestroySurface();
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;          // 自上而下，逐行处理不用翻
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!dib || !bits) {
        if (dc) DeleteDC(dc);
        return false;
    }
    g_surface.old = SelectObject(dc, dib);   // 记住被顶掉的原对象，销毁时要还
    g_surface.dib = dib;
    g_surface.dc = dc;
    g_surface.bits = bits;
    g_surface.w = w;
    g_surface.h = h;
    return true;
}

// 滑动窗口盒式模糊。做三遍近似高斯：一遍会留下明显的方块边界。
void BoxBlur(std::vector<uint8_t>& buf, int w, int h, int r) {
    if (r <= 0) return;
    std::vector<uint8_t> tmp(buf.size());
    int div = r * 2 + 1;
    auto clampi = [](int v, int hi) { return std::min(std::max(v, 0), hi); };

    for (int y = 0; y < h; ++y) {
        const uint8_t* s = buf.data() + (size_t)y * w;
        uint8_t* d = tmp.data() + (size_t)y * w;
        int sum = 0;
        for (int i = -r; i <= r; ++i) sum += s[clampi(i, w - 1)];
        for (int x = 0; x < w; ++x) {
            d[x] = (uint8_t)(sum / div);
            sum += s[clampi(x + r + 1, w - 1)] - s[clampi(x - r, w - 1)];
        }
    }
    for (int x = 0; x < w; ++x) {
        int sum = 0;
        for (int i = -r; i <= r; ++i) sum += tmp[(size_t)clampi(i, h - 1) * w + x];
        for (int y = 0; y < h; ++y) {
            buf[(size_t)y * w + x] = (uint8_t)(sum / div);
            sum += tmp[(size_t)clampi(y + r + 1, h - 1) * w + x] -
                   tmp[(size_t)clampi(y - r, h - 1) * w + x];
        }
    }
}

// 阴影 = 贴地层（收得紧）+ 环境层（散得开）。只有一层的阴影看起来像描边。
void BuildShadowBitmap(int winW, int winH, int cardW, int cardH) {
    if (g_shadowBmp && g_shadowW == winW && g_shadowH == winH && g_shadowDark == g_dark)
        return;

    int m = ShadowPx();
    std::vector<uint8_t> base((size_t)winW * winH, 0);
    {
        Bitmap mask(winW, winH, PixelFormat32bppARGB);
        Graphics mg(&mask);
        mg.SetSmoothingMode(SmoothingModeAntiAlias);
        mg.Clear(Color(0, 0, 0, 0));
        GraphicsPath p;
        MakeRoundRect(p, (float)m, (float)m + (float)S(SHADOW_OFFSET),
                      (float)cardW, (float)cardH, (float)S(CORNER));
        SolidBrush wb(Color(255, 255, 255, 255));
        mg.FillPath(&wb, &p);

        BitmapData bd{};
        Rect rc(0, 0, winW, winH);
        if (mask.LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &bd) == Ok) {
            for (int y = 0; y < winH; ++y) {
                const uint8_t* s = (const uint8_t*)bd.Scan0 + (size_t)y * bd.Stride;
                uint8_t* d = base.data() + (size_t)y * winW;
                for (int x = 0; x < winW; ++x) d[x] = s[x * 4 + 3];
            }
            mask.UnlockBits(&bd);
        }
    }

    std::vector<uint8_t> tight = base, wide = base;
    int rTight = std::max(1, S(2));
    int rWide = std::max(2, S(SHADOW_BLUR));
    for (int i = 0; i < 3; ++i) {
        BoxBlur(tight, winW, winH, rTight);
        BoxBlur(wide, winW, winH, rWide);
    }

    int aTight = g_dark ? 96 : 40;
    int aWide = g_dark ? 160 : 38;

    Bitmap* bmp = new Bitmap(winW, winH, PixelFormat32bppARGB);
    BitmapData bd{};
    Rect rc(0, 0, winW, winH);
    if (bmp->LockBits(&rc, ImageLockModeWrite, PixelFormat32bppARGB, &bd) == Ok) {
        for (int y = 0; y < winH; ++y) {
            uint8_t* d = (uint8_t*)bd.Scan0 + (size_t)y * bd.Stride;
            size_t off = (size_t)y * winW;
            for (int x = 0; x < winW; ++x) {
                int a = tight[off + x] * aTight / 255 + wide[off + x] * aWide / 255;
                d[x * 4 + 0] = 0;
                d[x * 4 + 1] = 0;
                d[x * 4 + 2] = 0;
                d[x * 4 + 3] = (uint8_t)std::min(a, 255);
            }
        }
        bmp->UnlockBits(&bd);
    }
    delete g_shadowBmp;
    g_shadowBmp = bmp;
    g_shadowW = winW;
    g_shadowH = winH;
    g_shadowDark = g_dark;
}

void EnsureCanvas(int w, int h) {
    if (g_canvas && (int)g_canvas->GetWidth() == w && (int)g_canvas->GetHeight() == h) return;
    delete g_canvas;
    g_canvas = new Bitmap(w, h, PixelFormat32bppARGB);
}

// 出现动画：卡片从下方 10px 处升起，阴影原地不动——看起来像卡片落进自己的影子里
int PanelSlide() {
    if (g_animOpen >= 1.0f) return 0;
    float e = 1.0f - (1.0f - g_animOpen) * (1.0f - g_animOpen) * (1.0f - g_animOpen);
    return (int)std::lround((1.0f - e) * S(10));
}

float PanelAlpha() {
    if (g_animOpen >= 1.0f) return 1.0f;
    return std::min(1.0f, g_animOpen / 0.55f);   // 前 55% 就把不透明度拉满
}

void ComposePanel(Graphics& g, int cardW, int cardH, int slide) {
    int m = ShadowPx();
    int winW = cardW + m * 2, winH = cardH + m * 2;
    BuildShadowBitmap(winW, winH, cardW, cardH);
    if (g_shadowBmp) g.DrawImage(g_shadowBmp, 0, 0, winW, winH);

    GraphicsState st = g.Save();
    g.TranslateTransform((REAL)m, (REAL)m + (REAL)slide);
    DrawContent(g, cardW, cardH);
    g.Restore(st);
}

bool CopyToLayered(HWND hwnd, int w, int h, float alpha) {
    if (!g_canvas) return false;
    if (!EnsureSurface(w, h)) return false;
    EnsureMulTable();

    BitmapData bd{};
    Rect rc(0, 0, w, h);
    if (g_canvas->LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok)
        return false;

    uint8_t* dst0 = (uint8_t*)g_surface.bits;
    int aMul = (int)std::lround(alpha * 255.0f);
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = (const uint8_t*)bd.Scan0 + (size_t)y * bd.Stride;
        uint8_t* d = dst0 + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x, s += 4, d += 4) {
            int a = s[3];
            if (aMul != 255) a = (a * aMul + 127) / 255;
            d[3] = (uint8_t)a;
            if (a == 255) {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
            } else if (a == 0) {
                d[0] = d[1] = d[2] = 0;
            } else {
                d[0] = g_mulTable[a * 256 + s[0]];
                d[1] = g_mulTable[a * 256 + s[1]];
                d[2] = g_mulTable[a * 256 + s[2]];
            }
        }
    }
    g_canvas->UnlockBits(&bd);

    HDC screen = GetDC(nullptr);
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    POINT src{0, 0};
    SIZE size{w, h};
    RECT wr{};
    GetWindowRect(hwnd, &wr);
    POINT dst{wr.left, wr.top};
    BOOL ok = UpdateLayeredWindow(hwnd, screen, &dst, &size, g_surface.dc, &src, 0,
                                 &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
    return ok != FALSE;
}

void PaintPanel(HWND hwnd) {
    int cardW = PanelWidth();
    int cardH = PanelHeightNow();
    int m = ShadowPx();
    int winW = cardW + m * 2, winH = cardH + m * 2;

    EnsureCanvas(winW, winH);
    if (!g_canvas) return;

    {
        Graphics g(g_canvas);
        g.Clear(Color(0, 0, 0, 0));
        ComposePanel(g, cardW, cardH, PanelSlide());
    }
    CopyToLayered(hwnd, winW, winH, PanelAlpha());
}

bool GetEncoderClsid(const wchar_t* mime, CLSID* clsid) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<uint8_t> buf(size);
    auto* codecs = reinterpret_cast<ImageCodecInfo*>(buf.data());
    GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, mime) == 0) {
            *clsid = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

bool RenderFrameTo(const wchar_t* path) {
    int cardW = PanelWidth();
    int cardH = PanelHeightNow();
    int m = ShadowPx();
    int winW = cardW + m * 2, winH = cardH + m * 2;

    // 和真窗口走同一条合成路径，预览里的阴影就是屏幕上的阴影
    Bitmap canvas(winW, winH, PixelFormat32bppARGB);
    {
        Graphics g(&canvas);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.Clear(C(g_dark ? 0x101317 : 0xEFF1F5));
        ComposePanel(g, cardW, cardH, 0);
    }

    CLSID clsid;
    bool ok = false;
    if (GetEncoderClsid(L"image/png", &clsid)) {
        ok = (canvas.Save(path, &clsid, nullptr) == Ok);
    }
    return ok;
}

// 把运行时托盘图标按不同电量渲染成一条对比图，用于检查小尺寸观感
int RenderTrayStripImpl(const wchar_t* path) {
    SetThemePalette(false);
    const int sizes[] = {16, 20, 24, 32, 40, 48};
    const int n = 6;
    const int cell = 84, pad = 18, rowH = 84;
    Bitmap canvas(cell * n + pad * 2, rowH * 2 + pad * 2, PixelFormat32bppARGB);
    Graphics g(&canvas);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(255, 246, 246, 248));
    SolidBrush darkRow(Color(255, 32, 32, 32));
    g.FillRectangle(&darkRow, 0, pad + rowH, cell * n + pad * 2, rowH);

    for (int row = 0; row < 2; ++row) {
        for (int i = 0; i < n; ++i) {
            int sz = sizes[i];
            int px = pad + i * cell + (cell - sz) / 2;
            int py = pad + row * rowH + (rowH - sz) / 2;
            GraphicsState st = g.Save();
            g.TranslateTransform((REAL)px, (REAL)py);
            DrawChipGlyph(g, (float)sz / 24.0f, row == 1);
            g.Restore(st);
        }
    }

    CLSID clsid;
    bool ok = false;
    if (GetEncoderClsid(L"image/png", &clsid)) {
        ok = (canvas.Save(path, &clsid, nullptr) == Ok);
    }
    return ok ? 0 : 1;
}

bool RenderShotImpl(const std::vector<Reading>& demo, bool dark, const wchar_t* path) {
    SetThemePalette(dark);
    g_snapshot = demo;
    g_display = demo;          // 预览是静态帧：没有行动画
    g_anims.clear();
    g_lastUpdate = time(nullptr);
    g_dpi = 96;
    g_autostart = true;
    g_animSwitch = 1.0f;    // 预览是静态帧：开关直接就位
    g_animOpen = 1.0f;
    return RenderFrameTo(path);
}

// 回归测试：同一进程内每隔 gapMs 重绘一帧。
// 时间戳若在绘制时现算，帧与帧之间会差 1 秒；正确实现下所有帧必须完全一致。
// 把位图编码成 PNG 字节（ICO 内嵌 PNG，Vista 以后都支持）
static bool EncodePng(Bitmap& bmp, std::vector<BYTE>* out) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<BYTE> buf(size);
    ImageCodecInfo* ci = reinterpret_cast<ImageCodecInfo*>(buf.data());
    GetImageEncoders(num, size, ci);
    CLSID clsid{};
    bool found = false;
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(ci[i].MimeType, L"image/png") == 0) {
            clsid = ci[i].Clsid;
            found = true;
            break;
        }
    }
    if (!found) return false;

    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(nullptr, TRUE, &stream) != S_OK) return false;
    if (bmp.Save(stream, &clsid, nullptr) != Ok) {
        stream->Release();
        return false;
    }
    STATSTG st{};
    stream->Stat(&st, STATFLAG_NONAME);
    DWORD len = st.cbSize.LowPart;
    HGLOBAL h = nullptr;
    if (GetHGlobalFromStream(stream, &h) != S_OK) {
        stream->Release();
        return false;
    }
    if (void* p = GlobalLock(h)) {
        out->assign((BYTE*)p, (BYTE*)p + len);
        GlobalUnlock(h);
    }
    stream->Release();
    return !out->empty();
}

// 生成多尺寸 ICO，供 exe 内嵌使用
bool WriteAppIconImpl(const wchar_t* path) {
    const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 128, 256};
    const int n = 9;
    std::vector<std::vector<BYTE>> blobs;
    for (int i = 0; i < n; ++i) {
        int sz = sizes[i];
        Bitmap bmp(sz, sz, PixelFormat32bppARGB);
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.Clear(Color(0, 0, 0, 0));
        // 品牌图标用闪电，不放数字——静态图标上的数字会显得在报错
        DrawChipGlyph(g, (float)sz / 24.0f, false);
        std::vector<BYTE> png;
        if (!EncodePng(bmp, &png)) return false;
        blobs.push_back(png);
    }

    FILE* f = _wfopen(path, L"wb");
    if (!f) return false;
    WORD reserved = 0, type = 1, count = (WORD)n;
    fwrite(&reserved, 2, 1, f);
    fwrite(&type, 2, 1, f);
    fwrite(&count, 2, 1, f);
    DWORD offset = 6 + 16 * n;
    for (int i = 0; i < n; ++i) {
        BYTE w = (sizes[i] >= 256) ? 0 : (BYTE)sizes[i];
        BYTE h = w;
        BYTE colors = 0, res = 0;
        WORD planes = 1, bits = 32;
        DWORD bytes = (DWORD)blobs[i].size();
        fwrite(&w, 1, 1, f);
        fwrite(&h, 1, 1, f);
        fwrite(&colors, 1, 1, f);
        fwrite(&res, 1, 1, f);
        fwrite(&planes, 2, 1, f);
        fwrite(&bits, 2, 1, f);
        fwrite(&bytes, 4, 1, f);
        fwrite(&offset, 4, 1, f);
        offset += bytes;
    }
    for (int i = 0; i < n; ++i) fwrite(blobs[i].data(), 1, blobs[i].size(), f);
    fclose(f);
    return true;
}

int RenderShotSequenceImpl(const std::vector<Reading>& demo, bool dark,
                           const wchar_t* prefix, int frames, int gapMs) {
    if (frames < 2) frames = 2;
    if (frames > 10) frames = 10;
    SetThemePalette(dark);
    g_snapshot = demo;
    g_display = demo;          // 预览是静态帧：没有行动画
    g_anims.clear();
    g_lastUpdate = time(nullptr);
    g_dpi = 96;
    g_autostart = true;
    g_animSwitch = 1.0f;
    g_animOpen = 1.0f;
    for (int i = 0; i < frames; ++i) {
        std::wstring p = std::wstring(prefix) + L"_" + std::to_wstring(i) + L".png";
        if (!RenderFrameTo(p.c_str())) return 1;
        if (i + 1 < frames) Sleep((DWORD)gapMs);
    }
    return 0;
}

// 当前系统的内部版本号：Win10 是 1xxxx，Win11 是 22000 起。
// 只在需要"Win11 才有的行为"时用来分流，Win10 上走各自的退路。
DWORD WindowsBuild() {
    static DWORD cached = 0;
    if (cached) return cached;
    struct OsVerInfo {
        ULONG size;
        ULONG major;
        ULONG minor;
        ULONG build;
        ULONG platform;
        WCHAR csd[128];
    };
    typedef LONG(WINAPI * RtlGetVersionPtr)(OsVerInfo*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto fn = (RtlGetVersionPtr)GetProcAddress(ntdll, "RtlGetVersion");
        if (fn) {
            OsVerInfo vi{};
            vi.size = sizeof(vi);
            if (fn(&vi) == 0) cached = vi.build;
        }
    }
    return cached;
}

void ApplyPanelCorners(HWND hwnd) {
    const DWORD build = WindowsBuild();
    // 让 DWM 用暗色画窗口边框和阴影：否则深色卡片外面会有一圈亮色描边
    BOOL immersiveDark = g_dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &immersiveDark, sizeof(immersiveDark)))) {
        DwmSetWindowAttribute(hwnd, 19, &immersiveDark, sizeof(immersiveDark));   // 1809~1909
    }

    // 圆角只靠卡片自己画（分层窗口逐像素 alpha），**不要**再设 DWMWA_WINDOW_CORNER_PREFERENCE。
    //
    // 之前这里请求了 DWMWCP_ROUND，结果 DWM 会沿**整个窗口矩形**补一圈 1px 描边。
    // 而窗口比卡片大一圈阴影余量（36 DIP），于是卡片外面凭空多出一个方框：
    // 白底上特别刺眼（实测描边灰度 165，正常应完全透明），深色下则是一圈亮线。
    // 实测：不设这一项时，窗口四边 alpha 为 0，白底截图上就是纯白。
    if (build < 22000) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        HRGN rgn = CreateRoundRectRgn(0, 0, rc.right + 1, rc.bottom + 1, S(28), S(28));
        SetWindowRgn(hwnd, rgn, TRUE);
    }
}

// 面板默认贴住工作区右下角（任务栏正上方）；拖过之后按记住的位置放，
// 但一定要夹回屏幕内——换了分辨率或拔掉显示器不能让它消失。
const int PANEL_MARGIN = 8;

void ClampCardToMonitor(int* x, int* y, int w, int h, POINT cursor) {
    HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;
    int maxX = mi.rcWork.right - w;
    int maxY = mi.rcWork.bottom - h;
    if (maxX < mi.rcWork.left) maxX = mi.rcWork.left;
    if (maxY < mi.rcWork.top) maxY = mi.rcWork.top;
    *x = std::max<int>(mi.rcWork.left, std::min(*x, maxX));
    *y = std::max<int>(mi.rcWork.top, std::min(*y, maxY));
}

// 存下来的位置还有效吗：中心点必须落在某块显示器上。
// 然后整体夹进工作区，避免"换了显示器之后只剩一条边在屏幕里"。
bool SavedPanelPos(int* x, int* y, int w, int h) {
    if (g_config.panelX < 0 || g_config.panelY < 0) return false;
    POINT c{g_config.panelX + w / 2, g_config.panelY + h / 2};
    if (!MonitorFromPoint(c, MONITOR_DEFAULTTONULL)) return false;
    *x = g_config.panelX;
    *y = g_config.panelY;
    ClampCardToMonitor(x, y, w, h, c);
    return true;
}

// 只算坐标，不动窗口。毛玻璃要在窗口可见之前按这个坐标抓屏，
// 所以定位逻辑必须先能单独调用一次。
bool ComputePanelPos(HWND hwnd, HMONITOR mon, int* outX, int* outY) {
    int w = PanelWidth();
    int h = PanelHeightNow();

    int x = 0, y = 0;
    if (!SavedPanelPos(&x, &y, w, h)) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!mon || !GetMonitorInfoW(mon, &mi)) {
            mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            if (!GetMonitorInfoW(mon, &mi)) return false;
        }
        x = mi.rcWork.right - w - S(PANEL_MARGIN);
        y = mi.rcWork.bottom - h - S(PANEL_MARGIN);
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y < mi.rcWork.top) y = mi.rcWork.top;
    }
    *outX = x;
    *outY = y;
    return true;
}

void PlacePanel(HWND hwnd, HMONITOR mon) {
    int x = 0, y = 0;
    if (!ComputePanelPos(hwnd, mon, &x, &y)) return;
    int m = ShadowPx();
    SetWindowPos(hwnd, HWND_TOPMOST, x - m, y - m, WindowWidth(), WindowHeightNow(),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void StartAnim(HWND hwnd) {
    if (g_animTimer) return;
    g_animTimer = true;
    SetTimer(hwnd, 3, 16, nullptr);
}

void ApplyFreshOrder();   // 定义在下面：离场收完后把排队的新行放进来

// 返回 false 表示都收敛了，可以停掉动画定时器
bool AnimateTick() {
    bool active = false;

    if (g_animOpen < 1.0f) {
        g_animOpen = std::min(1.0f, g_animOpen + 0.11f);
        if (g_animOpen < 1.0f) active = true;
    }

    auto approach = [&](float& v, float target, float k) {
        float d = target - v;
        if (std::fabs(d) > 0.004f) {
            v += d * k;
            active = true;
        } else {
            v = target;
        }
    };

    for (int i = 0; i < 3; ++i)
        approach(g_animHover[i], (g_hoverMask & (1 << i)) ? 1.0f : 0.0f, 0.30f);
    approach(g_animHover[3], g_hover >= 0 ? 1.0f : 0.0f, 0.30f);
    approach(g_animSwitch, g_autostart ? 1.0f : 0.0f, 0.28f);

    // 行增删：16 ms 一帧，12 帧 ≈ 190 ms（先收后进，两段约 380 ms）
    const float step = 1.0f / 12.0f;
    for (auto& kv : g_anims) {
        RowAnim& a = kv.second;
        if (a.leaving) {
            a.p = std::max(0.0f, a.p - step);
            if (a.p > 0.0f) active = true;
        } else if (a.p < 1.0f) {
            a.p = std::min(1.0f, a.p + step);
            if (a.p < 1.0f) active = true;
        }
    }

    // 收完的行真正删掉
    std::vector<std::wstring> dead;
    for (auto& kv : g_anims) {
        if (kv.second.leaving && kv.second.p <= 0.0f) dead.push_back(kv.first);
    }
    if (!dead.empty()) {
        for (auto& k : dead) g_anims.erase(k);
        g_display.erase(std::remove_if(g_display.begin(), g_display.end(),
                                       [&](const Reading& d) {
                                           return std::find(dead.begin(), dead.end(), d.key) !=
                                                  dead.end();
                                       }),
                        g_display.end());
        active = true;
    }

    // 离场都收完了：把排队的新设备放进来，接着播进场
    if (!g_pendingAdds.empty()) {
        bool leaving = false;
        for (auto& kv : g_anims) {
            if (kv.second.leaving) { leaving = true; break; }
        }
        if (!leaving) {
            ApplyFreshOrder();
            active = true;
        }
    }

    return active;
}

// 按最新扫描的顺序重建列表；队列里的新增行从 0 开始长
void ApplyFreshOrder() {
    std::map<std::wstring, Reading> existing;
    for (auto& d : g_display) existing[d.key] = d;

    std::vector<Reading> next;
    next.reserve(g_lastFresh.size());
    for (auto& f : g_lastFresh) {
        auto it = existing.find(f.key);
        if (it != existing.end()) {
            next.push_back(it->second);
        } else {
            next.push_back(f);
            RowAnim a;
            a.p = 0.0f;
            g_anims[f.key] = a;
        }
    }
    g_display = std::move(next);
    g_pendingAdds.clear();
}

// 把新数据并进绘制列表。分两拍：
//   1) 消失的行先原地收起来（同时存活行更新数值）
//   2) 收完之后再按新顺序插入新增行，让它们从 0 长出来
// 之所以不一起做：面板最多显示 5 行，离场行还没收完时插入新行会把新行挤出可视区，
// 进场动画等于没播。
void MergeDisplayAnimated(const std::vector<Reading>& fresh) {
    g_lastFresh = fresh;

    std::map<std::wstring, Reading> freshByKey;
    for (auto& d : fresh) freshByKey[d.key] = d;

    // 1) 还在的设备原地更新数值；消失的标成离场
    for (auto& d : g_display) {
        auto it = freshByKey.find(d.key);
        if (it != freshByKey.end()) {
            d = it->second;
            auto a = g_anims.find(d.key);
            if (a != g_anims.end()) {
                a->second.leaving = false;
                a->second.p = 1.0f;   // 又被扫到了：取消离场
            }
        } else {
            // 注意不能只在"已有动画条目"时才标记：面板收起期间直接替换过列表的
            // 行是没有条目的，漏掉就会直接跳变、播不出离场。
            RowAnim& a = g_anims[d.key];
            a.leaving = true;
            a.p = 1.0f;
        }
    }

    // 2) 新出现的一律先排队
    g_pendingAdds.clear();
    for (auto& f : fresh) {
        bool shown = std::any_of(g_display.begin(), g_display.end(),
                                 [&](const Reading& d) { return d.key == f.key; });
        if (!shown) g_pendingAdds.push_back(f);
    }

    bool leaving = false;
    for (auto& kv : g_anims) {
        if (kv.second.leaving) { leaving = true; break; }
    }
    if (!leaving && !g_pendingAdds.empty()) ApplyFreshOrder();
}

bool HasRowAnim() {
    for (auto& kv : g_anims) {
        if (kv.second.leaving || kv.second.p < 1.0f) return true;
    }
    return false;
}

// 面板收起时把动画直接落到终态：离场的删掉，排队的新行直接并进来。
// 否则下次打开会看到一个停在半路的行。
void SettleRows() {
    for (auto it = g_display.begin(); it != g_display.end();) {
        auto a = g_anims.find(it->key);
        if (a != g_anims.end() && a->second.leaving) {
            it = g_display.erase(it);
        } else {
            ++it;
        }
    }
    if (!g_pendingAdds.empty()) ApplyFreshOrder();
    g_anims.clear();
    g_pendingAdds.clear();
}

int HitRowIndex(int y) {
    float top = (float)(S(HEADER_H) + 1);
    int visible = VisibleRows();
    for (int i = 0; i < visible; ++i) {
        int di = g_scroll + i;
        if (di >= (int)g_display.size()) break;
        auto it = g_anims.find(g_display[di].key);
        float p = (it == g_anims.end()) ? 1.0f : it->second.p;
        float h = (float)S(ROW_H) * p;
        if ((float)y >= top && (float)y < top + h) return g_scroll + i;
        top += h;
    }
    return -1;
}

LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            PaintPanel(hwnd);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST: {
            // 阴影余量不吃点击：只有卡片矩形内才是面板
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &p);
            int m = ShadowPx();
            RECT card{m, m, m + PanelWidth(), m + PanelHeightNow()};
            if (!PtInRect(&card, p)) return HTTRANSPARENT;
            return HTCLIENT;
        }
        case WM_MOUSEMOVE: {
            // 客户区坐标 → 卡片坐标；下面所有命中矩形都是卡片坐标
            POINT pt{GET_X_LPARAM(lp) - ShadowPx(), GET_Y_LPARAM(lp) - ShadowPx()};

            if (g_dragPending) {
                POINT cur;
                GetCursorPos(&cur);
                int dx = cur.x - g_dragStartCursor.x;
                int dy = cur.y - g_dragStartCursor.y;
                int thresh = S(3);
                if (!g_dragging && (std::abs(dx) > thresh || std::abs(dy) > thresh))
                    g_dragging = true;
                if (g_dragging) {
                    int cardW = PanelWidth();
                    int cardH = PanelHeightNow();
                    int cx = g_dragStartCard.x + dx;
                    int cy = g_dragStartCard.y + dy;
                    ClampCardToMonitor(&cx, &cy, cardW, cardH, cur);
                    SetWindowPos(hwnd, HWND_TOPMOST, cx - ShadowPx(), cy - ShadowPx(), 0, 0,
                                 SWP_NOSIZE | SWP_NOACTIVATE);
                    SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
                }
                return 0;
            }

            bool overRefresh = PtInRect(&g_refreshRect, pt) != 0;
            bool overClose = PtInRect(&g_closeRect, pt) != 0;
            bool overQuit = PtInRect(&g_quitRect, pt) != 0;
            bool overSwitch = PtInRect(&g_switchRect, pt) != 0;
            bool overButton = overRefresh || overClose || overQuit || overSwitch;
            bool inHeader = pt.y >= 0 && pt.y < S(HEADER_H);

            SetCursor(LoadCursor(nullptr, overButton ? IDC_HAND
                                          : (inHeader ? IDC_SIZEALL : IDC_ARROW)));

            int mask = (overRefresh ? 1 : 0) | (overClose ? 2 : 0) | (overQuit ? 4 : 0);
            bool dirty = false;
            if (mask != g_hoverMask) {
                g_hoverMask = mask;
                StartAnim(hwnd);
                dirty = true;
            }

            int idx = -1;
            if (pt.y >= S(HEADER_H) + 1 && !g_display.empty()) idx = HitRowIndex(pt.y);
            if (idx != g_hover) {
                g_hover = idx;
                if (idx >= 0) g_animHover[3] = 0.0f;   // 新行从 0 淡入
                StartAnim(hwnd);
                dirty = true;
            }
            if (dirty) InvalidateRect(hwnd, nullptr, FALSE);
            if (!g_tracking) {
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
                g_tracking = true;
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (ScrollMax() <= 0) return 0;
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            int steps = delta / WHEEL_DELTA;
            if (steps == 0) steps = (delta > 0) ? 1 : -1;
            g_scroll -= steps;
            g_scroll = std::max(0, std::min(ScrollMax(), g_scroll));
            g_hover = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_TIMER:
            if (wp == 1) {
                if (!g_scanning || ++g_spinTicks > 300) {   // 10 秒兜底
                    g_scanning = false;
                    KillTimer(hwnd, 1);
                } else {
                    g_spinAngle += 26.0f;
                    if (g_spinAngle >= 360.0f) g_spinAngle -= 360.0f;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wp == 2) {                 // 退出的确认窗口过期，自动撤销
                KillTimer(hwnd, 2);
                if (g_quitArmed) {
                    g_quitArmed = false;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            if (wp == 3) {                 // 悬停淡入 / 开关滑块 / 出现动画 / 行增删
                bool active = AnimateTick();
                // 行高在变，窗口高度得跟着走（贴底的面板会自然地往上长/往下收）
                RECT wr{};
                GetWindowRect(hwnd, &wr);
                if ((int)(wr.bottom - wr.top) != WindowHeightNow()) {
                    PlacePanel(hwnd, MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
                }
                if (!active) {
                    KillTimer(hwnd, 3);
                    g_animTimer = false;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            return 0;
        case WM_MOUSELEAVE:
            g_tracking = false;
            if (g_hover != -1 || g_hoverMask) {
                g_hover = -1;
                g_hoverMask = 0;
                StartAnim(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            POINT pt{GET_X_LPARAM(lp) - ShadowPx(), GET_Y_LPARAM(lp) - ShadowPx()};

            // 只要不是点在「确认」上，任何按下都撤销待确认的退出。
            // 标题栏按下会进拖动分支，不在这里处理的话上膛状态会一直挂着。
            if (g_quitArmed && !PtInRect(&g_quitRect, pt)) {
                g_quitArmed = false;
                KillTimer(hwnd, 2);
                InvalidateRect(hwnd, nullptr, FALSE);
            }

            // 标题栏空白处 = 拖动把手（按钮除外）
            if (pt.y >= 0 && pt.y < S(HEADER_H) && !PtInRect(&g_refreshRect, pt) &&
                !PtInRect(&g_closeRect, pt)) {
                POINT cur;
                GetCursorPos(&cur);
                RECT wr{};
                GetWindowRect(hwnd, &wr);
                g_dragPending = true;
                g_dragging = false;
                g_dragStartCursor = cur;
                g_dragStartCard = POINT{wr.left + ShadowPx(), wr.top + ShadowPx()};
                SetCapture(hwnd);
                SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
                return 0;
            }

            bool dirty = false;
            if (PtInRect(&g_refreshRect, pt)) {
                g_pressRefresh = true;
                dirty = true;
            }
            if (PtInRect(&g_closeRect, pt)) {
                g_pressClose = true;
                dirty = true;
            }
            if (PtInRect(&g_quitRect, pt)) {
                g_pressQuit = true;
                dirty = true;
            }
            if (dirty) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT pt{GET_X_LPARAM(lp) - ShadowPx(), GET_Y_LPARAM(lp) - ShadowPx()};

            if (g_dragPending) {
                bool moved = g_dragging;
                g_dragPending = false;
                g_dragging = false;
                if (GetCapture() == hwnd) ReleaseCapture();
                if (moved) {
                    // 记住卡片左上角（不是窗口左上角，窗口含阴影余量）
                    RECT wr{};
                    GetWindowRect(hwnd, &wr);
                    g_config.panelX = wr.left + ShadowPx();
                    g_config.panelY = wr.top + ShadowPx();
                    g_config.Save();
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            bool wasRefresh = g_pressRefresh;
            bool wasClose = g_pressClose;
            bool wasQuit = g_pressQuit;
            g_pressRefresh = false;
            g_pressClose = false;
            g_pressQuit = false;

            if (wasRefresh && PtInRect(&g_refreshRect, pt)) {
                if (!g_scanning) {
                    g_scanning = true;
                    g_spinAngle = 0.0f;
                    g_spinTicks = 0;
                    SetTimer(hwnd, 1, 33, nullptr);
                    UiRefreshNow(true);      // 手动刷新：全量探测
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wasClose && PtInRect(&g_closeRect, pt)) {
                UiHidePanel();               // 只收起面板，程序还在托盘里
                return 0;
            }
            if (wasQuit && PtInRect(&g_quitRect, pt)) {
                if (g_quitArmed) {
                    g_quitArmed = false;
                    KillTimer(hwnd, 2);
                    PostMessageW(g_mainWnd, WM_COMMAND, IDM_QUIT, 0);
                } else {
                    g_quitArmed = true;
                    SetTimer(hwnd, 2, 4000, nullptr);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            // 点到别处就撤销待确认的退出：避免上膛状态一直挂着
            if (g_quitArmed) {
                g_quitArmed = false;
                KillTimer(hwnd, 2);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (PtInRect(&g_switchRect, pt)) {
                PostMessageW(g_mainWnd, WM_COMMAND, IDM_AUTOSTART, 0);
                return 0;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_CAPTURECHANGED:
            // 被别的窗口抢走捕获（Alt+Tab 之类）：当作拖动取消，别留下悬空状态
            g_dragPending = false;
            g_dragging = false;
            return 0;
        case WM_RBUTTONUP: {
            // 面板上右键也能出菜单：托盘图标找不到时还有一条路
            POINT pt;
            GetCursorPos(&pt);
            UiShowMenu(pt);
            return 0;
        }
        case WM_DPICHANGED: {
            g_dpi = HIWORD(wp);
            ClearFontCache();                 // 旧尺寸的字体全部作废
            // 拖动跨到另一块缩放不同的显示器时，别在这里重排——
            // PlacePanel 会按"记住的位置/右下角"把正在拖的面板拽回去
            if (!g_dragging) PlacePanel(hwnd, MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && !g_menuOpen) {
                if (!g_hidingSelf) g_lastDeactivate = GetTickCount64();
                if (g_quitArmed) {          // 失焦收起时撤销待确认的退出
                    g_quitArmed = false;
                    KillTimer(hwnd, 2);
                }
                SettleRows();               // 收起时把增删动画落到终态
                ShowWindow(hwnd, SW_HIDE);
            }
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) UiHidePanel();
            return 0;
        case WM_DESTROY:
            g_panel = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void EnsurePanel() {
    if (g_panel) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        // 不要 CS_DROPSHADOW：分层窗口自己画阴影，系统的硬边轮廓叠上来会脏
        wc.style = 0;
        wc.lpfnWndProc = PanelProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = PANEL_CLASS;
        RegisterClassExW(&wc);
        registered = true;
    }
    g_panel = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, PANEL_CLASS,
                              L"", WS_POPUP, 0, 0, WindowWidth(), WindowHeight(0),
                              g_mainWnd, nullptr, g_inst, nullptr);
}

void UpdateTrayIcon() {
    int size = GetSystemMetrics(SM_CXSMICON);
    HICON icon = MakeTrayIcon(size > 0 ? size : 16);
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_mainWnd;
    nid.uID = TRAY_UID;
    nid.uFlags = NIF_ICON;
    nid.hIcon = icon;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    if (g_trayIcon) DestroyIcon(g_trayIcon);
    g_trayIcon = icon;
}

std::wstring BuildTooltip() {
    if (g_snapshot.empty()) return L"余电：未发现可读取电量的设备";
    std::wstring s = L"余电";
    int n = 0;
    for (auto& d : g_snapshot) {
        if (n++ >= 6) break;
        s += L"\n";
        s += d.name;
        s += L"  ";
        s += (d.level >= 0)
                 ? ((d.approximate && !d.officialCurve ? L"≈" : L"") +
                    std::to_wstring(d.level) + L"%")
                 : L"--";
        if (!d.online) s += L"（休眠）";
        else if (d.charging == 1) s += L"（充电中）";
    }
    if (g_snapshot.size() > 6) s += L"\n…另有 " + std::to_wstring(g_snapshot.size() - 6) + L" 台";
    if (s.size() > 120) s = s.substr(0, 120);
    return s;
}

// 系统菜单窗口的类名是 #32768，它由 comctl32 画，默认方角 + 1px 硬边，
// 和面板/提示卡的圆角不在一个世纪。TrackPopupMenu 自己会跑消息循环，
// 所以挂个短定时器，等菜单窗口一出现就给它补上 DWM 圆角。
// 这样键盘操作、子菜单、勾选标记全都还是系统的，不用自己重写一套菜单。
void RoundSystemMenuWindows() {
    // 圆角浮层是 Win11 才有的能力。Win10 上系统菜单本来就全是方角，
    // 保持原样才是"像这个系统"；DWM 属性在 Win10 会返回失败，这里提前挡掉。
    if (WindowsBuild() < 22000) return;
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND w, LPARAM) -> BOOL {
            wchar_t cls[32]{};
            GetClassNameW(w, cls, 32);
            if (wcscmp(cls, L"#32768") == 0 && IsWindowVisible(w)) {
                DWORD pref = 2;   // DWMWCP_ROUND
                DwmSetWindowAttribute(w, 33, &pref, sizeof(pref));
            }
            return TRUE;
        },
        0);
}

}  // namespace

bool UiWriteAppIcon(const wchar_t* path) {
    return WriteAppIconImpl(path);
}

bool UiRenderShot(const std::vector<Reading>& demo, bool dark, const wchar_t* path) {
    return RenderShotImpl(demo, dark, path);
}

int RenderShotSequence(const std::vector<Reading>& demo, bool dark,
                       const wchar_t* prefix, int frames, int gapMs) {
    return RenderShotSequenceImpl(demo, dark, prefix, frames, gapMs);
}

int RenderTrayStrip(const wchar_t* path) {
    return RenderTrayStripImpl(path);
}

// ------------------------------------------------------------------ 对外接口

bool IsDarkTheme() {
    if (g_config.theme == L"light") return false;
    if (g_config.theme == L"dark") return true;
    HKEY key;
    DWORD value = 1, size = sizeof(value), type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &key) == ERROR_SUCCESS) {
        RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type, (LPBYTE)&value, &size);
        RegCloseKey(key);
    }
    return value == 0;
}

void SetThemePalette(bool dark) {
    g_dark = dark;
    g_colors = dark ? kDark : kLight;
}

void RefreshTheme() {
    SetThemePalette(IsDarkTheme());
}

void UiRebuildTheme() {
    RefreshTheme();
    // 阴影是按当前主题预先烘好的，换主题必须扔掉重做
    delete g_shadowBmp;
    g_shadowBmp = nullptr;
    g_shadowW = g_shadowH = 0;
    if (g_panel) InvalidateRect(g_panel, nullptr, FALSE);
    UiRefreshToastTheme();      // 提示卡还开着的话，跟着换色，别留一张白卡贴在深色桌面上
    UpdateTrayIcon();
}

void UiCreate() {
    g_dpi = GetDpiForSystem();
    RefreshTheme();

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_mainWnd;
    nid.uID = TRAY_UID;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAYMSG;
    nid.hIcon = MakeTrayIcon(GetSystemMetrics(SM_CXSMICON));
    CopyW(nid.szTip, 128, L"余电：正在读取…");
    Shell_NotifyIconW(NIM_ADD, &nid);
    if (g_trayIcon) DestroyIcon(g_trayIcon);
    g_trayIcon = nid.hIcon;
}

void UiDestroy() {
    ClearFontCache();
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_mainWnd;
    nid.uID = TRAY_UID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (g_trayIcon) {
        DestroyIcon(g_trayIcon);
        g_trayIcon = nullptr;
    }
}

void UiApplyDevices(const std::vector<Reading>& list, bool newData) {
    bool visible = g_panel && IsWindowVisible(g_panel);

    g_snapshot = list;
    if (newData) {
        g_lastUpdate = time(nullptr);   // 只有新扫描结果才刷新时间
        if (g_scanning) {
            g_scanning = false;
            if (g_panel) KillTimer(g_panel, 1);
        }

        // 判据是"用户此刻看不看得见"，不是手动还是自动扫描：
        //   面板开着 → 增删都走过渡动画，行不会凭空出现/消失
        //   面板收着 → 直接替换，下次打开已经是最终状态（另有打开动画）
        if (visible) {
            // 第一次拿到数据直接铺满，不要从 0 长出来
            if (!g_display.empty()) {
                MergeDisplayAnimated(list);
                if (HasRowAnim() && g_panel) StartAnim(g_panel);
            } else {
                g_display = list;
                g_anims.clear();
            }
        } else {
            g_display = list;
            g_anims.clear();
        }
    } else if (g_display.empty() && !list.empty()) {
        // 非扫描数据的重绘（改刷新间隔、重建托盘）：没有动画状态时补一次
        g_display = list;
        g_anims.clear();
    }
    UpdateTrayIcon();

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_mainWnd;
    nid.uID = TRAY_UID;
    nid.uFlags = NIF_TIP;
    std::wstring tip = BuildTooltip();
    CopyW(nid.szTip, 128, tip.c_str());
    Shell_NotifyIconW(NIM_MODIFY, &nid);

    g_scroll = std::max(0, std::min(ScrollMax(), g_scroll));   // 列表变短时收敛

    if (g_panel && IsWindowVisible(g_panel)) {
        // 行数/行高变了就重排：始终从右下角往上长，不会长到任务栏底下去
        PlacePanel(g_panel, MonitorFromWindow(g_panel, MONITOR_DEFAULTTONEAREST));
        InvalidateRect(g_panel, nullptr, FALSE);
    }
}

bool UiPanelVisible() {
    return g_panel && IsWindowVisible(g_panel);
}

void UiHidePanel() {
    if (g_panel && g_scanning) {
        g_scanning = false;
        KillTimer(g_panel, 1);
    }
    if (g_panel) {
        if (g_quitArmed) {
            g_quitArmed = false;
            KillTimer(g_panel, 2);
        }
        g_dragPending = false;
        g_dragging = false;
        if (GetCapture() == g_panel) ReleaseCapture();
        if (g_animTimer) {
            KillTimer(g_panel, 3);
            g_animTimer = false;
        }
        g_hoverMask = 0;
        g_pressClose = false;
        g_pressQuit = false;
        g_pressRefresh = false;
        SettleRows();               // 收起时把增删动画落到终态
        g_hidingSelf = true;
        ShowWindow(g_panel, SW_HIDE);
        g_hidingSelf = false;
    }
}

void UiSetAutostart(bool on) {
    if (g_autostart != on && g_panel) StartAnim(g_panel);
    g_autostart = on;
    if (g_panel) InvalidateRect(g_panel, nullptr, FALSE);
}

void UiResetPanelPos() {
    g_config.panelX = -1;
    g_config.panelY = -1;
    g_config.Save();
    if (g_panel && IsWindowVisible(g_panel)) {
        POINT cursor;
        GetCursorPos(&cursor);
        PlacePanel(g_panel, MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST));
        InvalidateRect(g_panel, nullptr, FALSE);
    }
}

void UiTogglePanel() {
    UiHideToast();
    EnsurePanel();
    if (IsWindowVisible(g_panel)) {
        UiHidePanel();
        return;
    }
    // 面板刚因失焦自动隐藏时，这次点击视为「关闭」，不要重新弹开
    if (g_lastDeactivate && GetTickCount64() - g_lastDeactivate < 260) {
        g_lastDeactivate = 0;
        return;
    }

    POINT cursor;
    GetCursorPos(&cursor);
    HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);

    g_hover = -1;
    g_hoverMask = 0;
    g_quitArmed = false;
    g_pressRefresh = g_pressClose = g_pressQuit = false;
    g_animSwitch = g_autostart ? 1.0f : 0.0f;   // 开关状态直接就位，不要每次打开都滑一下
    g_animOpen = 0.0f;                          // 出现动画：淡入 + 上浮
    PlacePanel(g_panel, mon);
    g_scroll = 0;
    InvalidateRect(g_panel, nullptr, FALSE);
    ShowWindow(g_panel, SW_SHOW);
    SetForegroundWindow(g_panel);
    StartAnim(g_panel);
    // 数据还新就不扫了：每次开面板都触发扫描会让"更新时间"莫名其妙地跳
    if (g_lastUpdate == 0 || time(nullptr) - g_lastUpdate >= 3) UiRefreshNow(false);
}

void UiRoundSystemMenus() {
    RoundSystemMenuWindows();
}

void UiShowMenu(POINT pt) {
    HMENU menu = CreatePopupMenu();
    // 菜单文案跟着当前状态走，别让用户猜这个「打开」点了会发生什么
    AppendMenuW(menu, MF_STRING, IDM_SHOW,
                UiPanelVisible() ? L"收起面板" : L"打开展示面板");
    AppendMenuW(menu, MF_STRING, IDM_REFRESH, L"立即刷新");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    MENUITEMINFOW mii{};
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_STATE | MIIM_ID | MIIM_STRING;
    mii.fState = g_autostart ? MFS_CHECKED : MFS_UNCHECKED;
    mii.wID = IDM_AUTOSTART;
    mii.dwTypeData = (LPWSTR)L"开机自启";
    InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &mii);

    HMENU sub = CreatePopupMenu();
    for (int i = 0; i < kIntervalCount; ++i) {
        wchar_t label[32];
        if (kIntervals[i] < 60)
            swprintf(label, 32, L"%d 秒", kIntervals[i]);
        else
            swprintf(label, 32, L"%d 分钟", kIntervals[i] / 60);
        UINT flags = MF_STRING | (CurrentInterval() == kIntervals[i] ? MF_CHECKED : 0);
        AppendMenuW(sub, flags, IDM_INTERVAL_BASE + i, label);
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, L"刷新间隔");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_config.panelX < 0 ? MF_GRAYED : 0), IDM_RESET_POS,
                L"面板回到右下角");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_QUIT, L"退出");

    g_menuOpen = true;
    SetForegroundWindow(g_mainWnd);
    // 菜单圆角：菜单窗口是在 TrackPopupMenu 里创建的，只能一边等一边补
    SetTimer(g_mainWnd, 5, 10, nullptr);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_mainWnd, nullptr);
    KillTimer(g_mainWnd, 5);
    g_menuOpen = false;
    DestroyMenu(menu);
    PostMessageW(g_mainWnd, WM_NULL, 0, 0);   // 已知问题：不补这条，菜单可能不消失
}

// ------------------------------------------------------------------ 低电量提示窗
// 不用 Shell_NotifyIcon 的气泡：那是系统画的，黄色三角 + 灰色图标 + 系统字体，
// 和程序里的苹果风格完全脱节。这里自己画一个小卡片。

const wchar_t* TOAST_CLASS = L"PeripheralBatteryToast";
HWND g_toast = nullptr;
std::wstring g_toastTitle;
int g_toastLevel = -1;
int g_toastSlide = 0;
RECT g_toastCloseRect{};
bool g_toastHoverClose = false;
bool g_toastTracking = false;

int ToastW() { return S(384); }
int ToastH() { return S(80); }

struct TextRun {
    std::wstring text;
    Font* font;
    uint32_t color;
};

// 同一行混排不同色 / 不同字重（正文里把 20% 单独染色）。
// 测量与绘制必须共用同一个 StringFormat：默认格式带额外内边距，
// 逐段拼接时会裂出可见的缝；GenericTypographic 拿到的是紧排宽度。
void DrawTextRuns(Graphics& g, float x, float y, float h, float maxW,
                  const std::vector<TextRun>& runs) {
    StringFormat sf(StringFormat::GenericTypographic());
    sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsNoWrap |
                      StringFormatFlagsNoClip);
    sf.SetAlignment(StringAlignmentNear);
    sf.SetLineAlignment(StringAlignmentCenter);

    float cx = x;
    RectF bounds;
    for (const auto& r : runs) {
        if (r.text.empty()) continue;
        g.MeasureString(r.text.c_str(), (INT)r.text.size(), r.font,
                        PointF(0.0f, 0.0f), &sf, &bounds);
        float w = bounds.Width;
        if (cx + w > x + maxW) break;          // 放不下就截断，不硬挤
        SolidBrush br(C(r.color));
        g.DrawString(r.text.c_str(), (INT)r.text.size(), r.font,
                     RectF(cx, y, w + 1.0f, h), &sf, &br);
        cx += w;
    }
}

// 正文里百分比的强调色：浅色卡上用深一档的橙 / 红保证小字对比度，
// 深色卡上用亮一档，直接套浅色那组会在深底上发闷。
uint32_t ToastAccent(int level, bool dark) {
    if (level < 0) return dark ? 0x98989D : 0x86868B;
    if (level <= 10) return dark ? 0xFF6961 : 0xD70015;
    if (level <= 20) return dark ? 0xFFB340 : 0xC93400;
    return dark ? 0xF5F5F7 : 0x1D1D1F;
}

void DrawCloseGlyph(Graphics& g, float cx, float cy, float r, uint32_t color, float stroke) {
    Pen pen(C(color), stroke);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawLine(&pen, cx - r, cy - r, cx + r, cy + r);
    g.DrawLine(&pen, cx + r, cy - r, cx - r, cy + r);
}

// 刷新箭头：圆弧 + 切向实心三角。
// 原来用的是字体字形 "↻"，16.5px 的字形在灰度抗锯齿的分层画布上只剩一圈灰点，
// 和旁边的矢量叉号放在一起明显不是一个清晰度。这里全部走路径，边缘由 GDI+ 抗锯齿生成。
void DrawRefreshGlyph(Graphics& g, float cx, float cy, float r, uint32_t color,
                      float stroke) {
    const float a0 = -58.0f;      // 缺口留在右上，视觉重心更稳
    const float sweep = 282.0f;   // 少扫几度，箭头才不会被圆头线帽糊住

    Pen arc(C(color), stroke);
    arc.SetStartCap(LineCapRound);
    arc.SetEndCap(LineCapRound);
    g.DrawArc(&arc, cx - r, cy - r, r * 2, r * 2, a0, sweep);

    const float rad = 3.14159265358979f / 180.0f;
    float aEnd = (a0 + sweep) * rad;
    float ex = cx + r * cosf(aEnd), ey = cy + r * sinf(aEnd);
    float tx = -sinf(aEnd), ty = cosf(aEnd);   // 顺时针切向
    float nx = cosf(aEnd), ny = sinf(aEnd);    // 径向

    // 箭头要明显大于线宽，否则在 28px 按钮里只剩一个圆环，读不出「刷新」
    float len = stroke * 2.6f;
    float half = stroke * 1.5f;
    PointF pts[3] = {
        PointF(ex + tx * len, ey + ty * len),
        PointF(ex - nx * half, ey - ny * half),
        PointF(ex + nx * half, ey + ny * half),
    };
    SolidBrush head(C(color));
    g.FillPolygon(&head, pts, 3);
}

void DrawToastContent(Graphics& g, int w, int h) {
    {
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        // 提示窗现在也是分层窗口，和面板一样不能用 ClearType：亚像素抗锯齿需要
        // 不透明底色，在带 alpha 的画布上会渗色。
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        float dpi = g_dpi / 96.0f;

        // 卡片：和面板同一套材质——不透明底色 + 1px 描边，圆角统一到 CORNER，
        // 两处放在一起才像同一个程序画的。
        GraphicsPath card;
        MakeRoundRect(card, 0.5f, 0.5f, (float)w - 1.0f, (float)h - 1.0f,
                      (float)S(CORNER));
        SolidBrush bg(C(g_dark ? g_colors.tile : g_colors.bg));
        g.FillPath(&bg, &card);
        Pen border(C(g_colors.border), 1.0f);
        g.DrawPath(&border, &card);

        // 左侧就是程序图标本身（墨底白闪电）。数字不进图标：
        // 16px 的小方块里塞「20」既不清楚，又和正文里的 20% 重复。
        float icon = (float)S(36);
        float ix = (float)S(16);
        float iy = ((float)h - icon) / 2.0f;
        GraphicsState st = g.Save();
        g.TranslateTransform(ix, iy);
        DrawChipGlyph(g, icon / 24.0f, g_dark);
        g.Restore(st);

        float ccx = (float)w - (float)S(20), ccy = (float)S(20);
        float hit = (float)S(11);
        if (g_toastHoverClose) {
            SolidBrush hb(C(g_colors.hover));
            g.FillEllipse(&hb, ccx - hit, ccy - hit, hit * 2, hit * 2);
        }
        DrawCloseGlyph(g, ccx, ccy, (float)S(4.8f),
                       g_toastHoverClose ? g_colors.text : g_colors.sub, (float)S(1.7f));
        g_toastCloseRect.left = (LONG)(ccx - hit);
        g_toastCloseRect.top = (LONG)(ccy - hit);
        g_toastCloseRect.right = (LONG)(ccx + hit);
        g_toastCloseRect.bottom = (LONG)(ccy + hit);

        float tx = ix + icon + (float)S(12);
        float tw = (float)w - tx - (float)S(38);
        Font* fTitle = GetFont(13.0f * dpi, FW_NORMAL);
        Font* fBody = GetFont(11.5f * dpi, FW_NORMAL);
        Font* fNum = GetFont(11.5f * dpi, FW_SEMIBOLD);

        DrawStr(g, g_toastTitle, fTitle, g_colors.text,
                RectF(tx, (float)S(19), tw, (float)S(19)));

        // 正文：百分比单独染色加粗，数字才是这条通知的信息
        if (g_toastLevel >= 0) {
            std::vector<TextRun> runs = {
                {L"电量偏低，剩余\u00A0", fBody, g_colors.sub},
                {std::to_wstring(g_toastLevel) + L"%", fNum,
                 ToastAccent(g_toastLevel, g_dark)},
                {L"，建议尽快充电。", fBody, g_colors.sub},
            };
            DrawTextRuns(g, tx, (float)S(38), (float)S(18), tw, runs);
        } else {
            DrawStr(g, L"电量偏低，建议尽快充电。", fBody, g_colors.sub,
                    RectF(tx, (float)S(39), tw, (float)S(17)));
        }
    }
}

void UiRefreshToastTheme() {
    if (!g_toast) return;
    ApplyPanelCorners(g_toast);
    InvalidateRect(g_toast, nullptr, TRUE);
}

// 提示窗和面板走同一套呈现管线：阴影位图 + 卡片平移 + 分层提交。
// 这样两处的阴影、圆角、玻璃层是同一份参数画出来的，不会一个软一个硬。
void ComposeToast(Graphics& g, int cardW, int cardH, int slide) {
    int m = ShadowPx();
    int winW = cardW + m * 2, winH = cardH + m * 2;
    BuildShadowBitmap(winW, winH, cardW, cardH);
    if (g_shadowBmp) g.DrawImage(g_shadowBmp, 0, 0, winW, winH);

    GraphicsState st = g.Save();
    g.TranslateTransform((REAL)m, (REAL)m + (REAL)slide);
    DrawToastContent(g, cardW, cardH);
    g.Restore(st);
}

void RenderToastLayered(HWND hwnd) {
    int cardW = ToastW(), cardH = ToastH();
    int m = ShadowPx();
    int winW = cardW + m * 2, winH = cardH + m * 2;

    EnsureCanvas(winW, winH);
    if (!g_canvas) return;
    {
        Graphics g(g_canvas);
        g.Clear(Color(0, 0, 0, 0));
        ComposeToast(g, cardW, cardH, g_toastSlide);
    }
    CopyToLayered(hwnd, winW, winH, 1.0f);
}

void PaintToast(HWND hwnd) {
    // 分层窗口的内容由 UpdateLayeredWindow 提交，但 WM_PAINT 仍要消费掉，
    // 否则系统会认为这块区域一直没画完、反复投递。
    PAINTSTRUCT ps;
    BeginPaint(hwnd, &ps);
    EndPaint(hwnd, &ps);
    RenderToastLayered(hwnd);
}

int RenderToastShot(const wchar_t* path, bool dark) {
    SetThemePalette(dark);
    g_toastTitle = L"G304 Lightspeed Wireless Gaming Mouse";
    g_toastLevel = 20;
    g_toastSlide = 0;
    g_dpi = 96;

    int cardW = ToastW(), cardH = ToastH();
    int m = ShadowPx();
    int pad = 18;
    int w = cardW + m * 2, h = cardH + m * 2;
    Bitmap canvas(w + pad * 2, h + pad * 2, PixelFormat32bppARGB);
    Graphics g(&canvas);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(Color(255, dark ? 40 : 236, dark ? 40 : 237, dark ? 40 : 240));
    GraphicsState st = g.Save();
    g.TranslateTransform((REAL)pad, (REAL)pad);
    ComposeToast(g, cardW, cardH, 0);     // 离线渲染没有桌面可抓，走纯色回退
    g.Restore(st);

    CLSID clsid;
    if (!GetEncoderClsid(L"image/png", &clsid)) return 1;
    return (canvas.Save(path, &clsid, nullptr) == Ok) ? 0 : 2;
}

// 窗口比卡片大 ShadowPx() 一圈：四周是阴影余量，卡片贴在工作区右下角。
void ToastPlace(HWND hwnd) {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int cardW = ToastW(), cardH = ToastH();
    int m = ShadowPx();
    int x = work.right - cardW - S(16) - m;
    int y = work.bottom - cardH - S(16) - m;
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, cardW + m * 2, cardH + m * 2,
                 SWP_NOACTIVATE);
}

void UiHideToast() {
    if (!g_toast) return;
    KillTimer(g_toast, 1);
    KillTimer(g_toast, 2);
    ShowWindow(g_toast, SW_HIDE);
}

// 关闭热区是在「卡片坐标」里记的：DrawToastContent 只知道自己画的那块卡片，
// 不知道窗口里还套着一圈阴影余量。鼠标消息给的却是「窗口客户区坐标」，两者
// 差一个 (m, m + slide) 的平移。不换算的话热区会整体偏到左上方的阴影里——
// 点可见的 X 会被判成「点在卡片上」，于是关掉通知的同时把面板叫了出来。
POINT ToastCardPoint(POINT pt) {
    const int m = ShadowPx();
    pt.x -= m;
    pt.y -= m + g_toastSlide;
    return pt;
}

LRESULT CALLBACK ToastProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            PaintToast(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE: {
            POINT pt = ToastCardPoint(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            bool over = PtInRect(&g_toastCloseRect, pt) != 0;
            if (over != g_toastHoverClose) {
                g_toastHoverClose = over;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (!g_toastTracking) {
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
                g_toastTracking = true;
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_toastTracking = false;
            if (g_toastHoverClose) {
                g_toastHoverClose = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP: {
            POINT pt = ToastCardPoint(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            const bool onClose = PtInRect(&g_toastCloseRect, pt) != 0;
            // 点在阴影余量上（卡片外）也算「只是关掉通知」：那圈半透明阴影
            // 同样能收到点击，把它当成卡片的点击会让人莫名其妙看到面板弹出来。
            const bool inCard = pt.x >= 0 && pt.y >= 0 &&
                                pt.x < ToastW() && pt.y < ToastH();
            UiHideToast();
            // 关通知与开面板是两件事：只有点在卡片正文上才去叫面板。
            if (!onClose && inCard) PostMessageW(g_mainWnd, WM_APP + 5, 0, 0);
            return 0;
        }
        case WM_TIMER:
            if (wp == 1) {                       // 入场：卡片从下方 8px 升上来，阴影不动
                g_toastSlide -= 2;
                if (g_toastSlide <= 0) {
                    g_toastSlide = 0;
                    KillTimer(hwnd, 1);
                }
                // 窗口位置不动，只重画：和面板一样，滑的是卡片不是整窗。
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wp == 2) {                       // 停留结束
                UiHideToast();
                return 0;
            }
            return 0;
        case WM_DESTROY:
            g_toast = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void EnsureToast() {
    if (g_toast) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        // 不再用 CS_DROPSHADOW：那是系统画的一像素硬边轮廓，和面板自绘的柔和
        // 阴影放在一起像两个年代的控件。阴影改由 BuildShadowBitmap 画。
        wc.style = 0;
        wc.lpfnWndProc = ToastProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursor(nullptr, IDC_HAND);
        wc.lpszClassName = TOAST_CLASS;
        RegisterClassExW(&wc);
        registered = true;
    }
    // WS_EX_LAYERED：逐像素 alpha 才能有真圆角和柔阴影，也才能画毛玻璃。
    g_toast = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        TOAST_CLASS, L"", WS_POPUP, 0, 0, ToastW(), ToastH(),
        g_mainWnd, nullptr, g_inst, nullptr);
}

void UiShowToast(const wchar_t* title, int level) {
    // 主题可能在运行中变过（托盘程序常驻数天），弹之前重新解析一次
    RefreshTheme();
    if (g_panel) InvalidateRect(g_panel, nullptr, FALSE);

    g_toastTitle = title ? title : L"";
    g_toastLevel = level;

    EnsureToast();
    if (!g_toast) return;

    UiHidePanel();                       // 和面板同一角落，先把面板收起来
    g_toastSlide = S(8);
    ToastPlace(g_toast);

    // 先把分层表面画好再显示，避免先露出一个空窗口
    RenderToastLayered(g_toast);
    ShowWindow(g_toast, SW_SHOWNOACTIVATE);
    ApplyPanelCorners(g_toast);
    SetTimer(g_toast, 1, 16, nullptr);
    SetTimer(g_toast, 2, 6000, nullptr);
}

void UiRefreshNow(bool full) {
    PostMessageW(g_mainWnd, WM_APP + 4, full ? 1 : 0, 0);
}

void UiSetScanningPreview(bool on) {
    g_scanning = on;
    g_spinAngle = 46.0f;
}

void UiSetQuitPreview(bool on) {
    g_quitArmed = on;
}

void UiSetScrollPreview(int row) {
    g_scroll = row;
}

int UiRenderToast(const wchar_t* path, bool dark) {
    return RenderToastShot(path, dark);
}
