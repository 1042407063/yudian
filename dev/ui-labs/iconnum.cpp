// 数字型图标实验室（苹果风）。
// 与 iconlab.cpp 同一套渲染框架，只是换了设计语言：
//   - 单色（近黑 / 近白），只有低电量才上色
//   - 细描边、大圆角、精确对齐
//   - 电量用数字表达
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace Gdiplus;

void MakeRR(GraphicsPath& p, float x, float y, float w, float h, float r) {
    if (w <= 0.05f || h <= 0.05f) return;
    if (r <= 0.15f) {
        p.AddRectangle(RectF(x, y, w, h));
        p.CloseFigure();
        return;
    }
    float d = r * 2.0f;
    if (d > w) d = w;
    if (d > h) d = h;
    p.AddArc(x, y, d, d, 180.0f, 90.0f);
    p.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    p.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    p.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    p.CloseFigure();
}

struct Pal {
    uint32_t ink;    // 主色（浅底近黑，深底近白）
    uint32_t sub;    // 次级
    uint32_t ok, warn, crit;
    bool dark = false;
};

Pal kLightPal = {0x1D1D1F, 0x86868B, 0x34C759, 0xFF9500, 0xFF3B30, false};
Pal kDarkPal  = {0xF5F5F7, 0x98989D, 0x30D158, 0xFF9F0A, 0xFF453A, true};

Color C(uint32_t rgb, BYTE a = 255) {
    return Color(a, (BYTE)((rgb >> 16) & 0xFF), (BYTE)((rgb >> 8) & 0xFF), (BYTE)(rgb & 0xFF));
}

// 苹果的做法：正常电量是单色，只有低电量才上色
uint32_t LevelInk(int lv, const Pal& p) {
    if (lv < 0) return p.sub;
    if (lv <= 10) return p.crit;
    if (lv <= 20) return p.warn;
    return p.ink;
}

std::wstring LevelText(int lv) {
    if (lv < 0) return L"--";
    return std::to_wstring(lv);
}

// 数字渲染：GDI 的字体微调比 GDI+ 锐利得多，但 GDI 画到 32bpp ARGB 上会把 alpha 写成 0
// （图会"消失"）。做法是先用 GDI 画到黑色 DIB 上，再把亮度当 alpha 搬到 ARGB 位图。
std::vector<Bitmap*> g_textPool;

Bitmap* TextMask(const std::wstring& s, int px, int weight, uint32_t color,
                 const wchar_t* face = L"Segoe UI") {
    if (px < 4) return nullptr;
    int W = px * 3, H = px * 2;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;          // 负值 = top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(mem, dib);

    RECT rc{0, 0, W, H};
    HBRUSH bk = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(mem, &rc, bk);
    DeleteObject(bk);

    HFONT f = CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, face);
    HGDIOBJ oldF = SelectObject(mem, f);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(255, 255, 255));
    DrawTextW(mem, s.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE |
                                     DT_NOPREFIX);
    SelectObject(mem, oldF);
    DeleteObject(f);

    Bitmap* out = new Bitmap(W, H, PixelFormat32bppARGB);
    BitmapData bd{};
    Rect lockRect(0, 0, W, H);
    out->LockBits(&lockRect, ImageLockModeWrite, PixelFormat32bppARGB, &bd);
    const uint8_t* src = (const uint8_t*)bits;
    uint8_t* dst = (uint8_t*)bd.Scan0;
    BYTE cr = (BYTE)((color >> 16) & 0xFF);
    BYTE cg = (BYTE)((color >> 8) & 0xFF);
    BYTE cb = (BYTE)(color & 0xFF);
    for (int y = 0; y < H; ++y) {
        const uint8_t* sp = src + y * W * 4;
        uint8_t* dp = dst + y * bd.Stride;
        for (int x = 0; x < W; ++x) {
            // DIB 是 BGRA，取最大分量当亮度
            BYTE a = std::max(sp[0], std::max(sp[1], sp[2]));
            dp[0] = (BYTE)(cb * a / 255);
            dp[1] = (BYTE)(cg * a / 255);
            dp[2] = (BYTE)(cr * a / 255);
            dp[3] = a;
            sp += 4;
            dp += 4;
        }
    }
    out->UnlockBits(&bd);

    SelectObject(mem, oldBmp);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    g_textPool.push_back(out);
    return out;
}

// 按墨迹边界自适应：先量出文字实际占多大，超了容器就等比缩小，
// 再按墨迹中心对齐（按字体框居中会看起来偏上）。
struct InkBox { int x0, y0, x1, y1; };

bool MeasureInk(Bitmap* bmp, InkBox* out) {
    if (!bmp) return false;
    int W = bmp->GetWidth(), H = bmp->GetHeight();
    BitmapData bd{};
    Rect lockRect(0, 0, W, H);
    if (bmp->LockBits(&lockRect, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok) {
        return false;
    }
    int x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; ++y) {
        const uint8_t* row = (const uint8_t*)bd.Scan0 + y * bd.Stride;
        for (int x = 0; x < W; ++x) {
            if (row[x * 4 + 3] > 24) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
        }
    }
    bmp->UnlockBits(&bd);
    if (x1 < 0) return false;
    out->x0 = x0;
    out->y0 = y0;
    out->x1 = x1;
    out->y1 = y1;
    return true;
}

void NumInBox(Graphics& g, float cx, float cy, float pxStart, float maxW, float maxH,
              const std::wstring& text, uint32_t rgb) {
    int px = (int)lround(pxStart);
    if (px < 5) return;
    Bitmap* bmp = TextMask(text, px, 600, rgb);
    InkBox ib{};
    if (!MeasureInk(bmp, &ib)) return;

    float cw = (float)(ib.x1 - ib.x0 + 1);
    float ch = (float)(ib.y1 - ib.y0 + 1);
    float scale = std::min(1.0f, std::min(maxW / cw, maxH / ch));
    if (scale < 0.999f) {
        int np = (int)lround(px * scale);
        if (np < 5) np = 5;
        bmp = TextMask(text, np, 600, rgb);
        if (!MeasureInk(bmp, &ib)) return;
        cw = (float)(ib.x1 - ib.x0 + 1);
        ch = (float)(ib.y1 - ib.y0 + 1);
    }

    float W = (float)bmp->GetWidth(), H = (float)bmp->GetHeight();
    float dx = cx - ((float)ib.x0 + cw / 2.0f);
    float dy = cy - ((float)ib.y0 + ch / 2.0f);
    g.DrawImage(bmp, dx, dy, W, H);
}

void FreeTextPool() {
    for (auto* b : g_textPool) delete b;
    g_textPool.clear();
}

// ---------------------------------------------------------------- 方案

// 低电量时整个容器换成警示色（iOS 徽章的做法），比单画一条彩条干净
uint32_t ContainerFill(int lv, const Pal& p) {
    if (lv >= 0 && lv <= 20) return LevelInk(lv, p);
    return p.ink;
}
uint32_t OnContainer(const Pal& p) {
    return p.dark ? 0x1C1C1E : 0xFFFFFF;   // 反白
}

void Chevron(Graphics& g, float s, uint32_t rgb) {
    PointF v[] = {
        PointF(14.6f * s, 3.8f * s), PointF(9.4f * s, 12.4f * s),
        PointF(12.3f * s, 12.4f * s), PointF(10.6f * s, 19.6f * s),
        PointF(15.6f * s, 10.4f * s), PointF(12.6f * s, 10.4f * s),
    };
    SolidBrush fb(C(rgb));
    g.FillPolygon(&fb, v, 6);
}

// 1 圆角方块：实底 + 反白数字
void N1(Graphics& g, float s, int lv, bool ch, const Pal& p) {
    GraphicsPath bg;
    MakeRR(bg, 0.7f * s, 0.7f * s, 22.6f * s, 22.6f * s, 6.8f * s);
    SolidBrush bb(C(ContainerFill(lv, p)));
    g.FillPath(&bb, &bg);

    if (ch) { Chevron(g, s, OnContainer(p)); return; }
    NumInBox(g, 12.0f * s, 12.2f * s, 16.0f * s, 14.8f * s, 8.6f * s,
             LevelText(lv), OnContainer(p));
}

// 2 实心圆
void N2(Graphics& g, float s, int lv, bool ch, const Pal& p) {
    SolidBrush bb(C(ContainerFill(lv, p)));
    g.FillEllipse(&bb, 1.0f * s, 1.0f * s, 22.0f * s, 22.0f * s);
    if (ch) { Chevron(g, s, OnContainer(p)); return; }
    NumInBox(g, 12.0f * s, 12.2f * s, 16.0f * s, 14.2f * s, 8.6f * s,
             LevelText(lv), OnContainer(p));
}

// 3 圆环：数字在中间，环只画一段
void N3(Graphics& g, float s, int lv, bool ch, const Pal& p) {
    float cx = 12.0f * s, cy = 12.0f * s, r = 10.5f * s;
    Pen track(C(p.ink, 34), 1.5f * s);
    g.DrawEllipse(&track, cx - r, cy - r, r * 2, r * 2);

    if (lv > 0) {
        Pen arc(C(LevelInk(lv, p)), 1.5f * s);
        arc.SetStartCap(LineCapRound);
        arc.SetEndCap(LineCapRound);
        float sweep = std::min(359.9f, (float)(360.0 * lv / 100.0));
        g.DrawArc(&arc, cx - r, cy - r, r * 2, r * 2, -90.0f, sweep);
    }
    if (ch) { Chevron(g, s, p.ink); return; }
    NumInBox(g, cx, cy + 0.2f * s, 14.0f * s, 13.6f * s, 8.2f * s,
             LevelText(lv), p.ink);
}

// 4 圆角方框：只有细描边，最轻
void N4(Graphics& g, float s, int lv, bool ch, const Pal& p) {
    Pen pen(C(p.ink, 240), 1.6f * s);
    pen.SetLineJoin(LineJoinRound);
    GraphicsPath box;
    MakeRR(box, 0.9f * s, 0.9f * s, 22.2f * s, 22.2f * s, 6.6f * s);
    g.DrawPath(&pen, &box);

    if (ch) { Chevron(g, s, p.ink); return; }
    NumInBox(g, 12.0f * s, 12.2f * s, 16.0f * s, 15.2f * s, 9.2f * s,
             LevelText(lv), LevelInk(lv, p));
}

// 5 纯数字：没有容器，只有数字和一条底线
void N5(Graphics& g, float s, int lv, bool ch, const Pal& p) {
    if (ch) { Chevron(g, s, p.ink); return; }
    NumInBox(g, 12.0f * s, 9.8f * s, 19.0f * s, 19.4f * s, 10.6f * s,
             LevelText(lv), LevelInk(lv, p));

    float barY = 18.6f * s, barX = 4.2f * s, barW = 15.6f * s, barH = 1.7f * s;
    SolidBrush tb(C(p.ink, 42));
    GraphicsPath tr;
    MakeRR(tr, barX, barY, barW, barH, barH / 2.0f);
    g.FillPath(&tb, &tr);
    if (lv > 0) {
        float w = std::max(barW * (lv / 100.0f), barH);
        GraphicsPath fr;
        MakeRR(fr, barX, barY, w, barH, barH / 2.0f);
        SolidBrush fb(C(LevelInk(lv, p)));
        g.FillPath(&fb, &fr);
    }
}

// 6 胶囊：横向药丸
void N6(Graphics& g, float s, int lv, bool ch, const Pal& p) {
    GraphicsPath cap;
    MakeRR(cap, 0.9f * s, 5.0f * s, 22.2f * s, 14.0f * s, 7.0f * s);
    SolidBrush bb(C(ContainerFill(lv, p)));
    g.FillPath(&bb, &cap);
    if (ch) { Chevron(g, s, OnContainer(p)); return; }
    NumInBox(g, 12.0f * s, 12.1f * s, 15.0f * s, 15.6f * s, 7.0f * s,
             LevelText(lv), OnContainer(p));
}

typedef void (*DrawFn)(Graphics&, float, int, bool, const Pal&);

// ---------------------------------------------------------------- 排版

void DrawIconAt(Graphics& g, DrawFn fn, float x, float y, int size, int lv, bool ch,
                const Pal& p) {
    GraphicsState st = g.Save();
    g.TranslateTransform(x, y);
    fn(g, (float)size / 24.0f, lv, ch, p);
    g.Restore(st);
}

void DrawIconZoom(Graphics& g, DrawFn fn, float x, float y, int size, int zoom, int lv,
                  bool ch, const Pal& p) {
    Bitmap tmp(size, size, PixelFormat32bppARGB);
    {
        Graphics tg(&tmp);
        tg.SetSmoothingMode(SmoothingModeAntiAlias);
        tg.SetTextRenderingHint(TextRenderingHintAntiAlias);
        tg.Clear(Color(0, 0, 0, 0));
        fn(tg, (float)size / 24.0f, lv, ch, p);
    }
    g.SetInterpolationMode(InterpolationModeNearestNeighbor);
    g.DrawImage(&tmp, (INT)x, (INT)y, size * zoom, size * zoom);
    g.SetInterpolationMode(InterpolationModeDefault);
}

void DrawTextAt(Graphics& g, const std::wstring& text, float x, float y, float w, float h,
                uint32_t rgb, float px, StringAlignment al = StringAlignmentNear,
                bool bold = false) {
    Font f(L"Microsoft YaHei UI", px, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    SolidBrush br(C(rgb));
    StringFormat sf;
    sf.SetAlignment(al);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    RectF rc(x, y, w, h);
    g.DrawString(text.c_str(), -1, &f, rc, &sf, &br);
}

const wchar_t* Names[] = {
    L"1 圆角方块", L"2 实心圆", L"3 圆环", L"4 圆角方框", L"5 纯数字", L"6 胶囊",
};
DrawFn AllFn[] = {N1, N2, N3, N4, N5, N6};

int SavePng(Bitmap& bmp, const wchar_t* path) {
    CLSID clsid;
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return 1;
    std::vector<BYTE> buf(size);
    ImageCodecInfo* ci = reinterpret_cast<ImageCodecInfo*>(buf.data());
    GetImageEncoders(num, size, ci);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(ci[i].MimeType, L"image/png") == 0) {
            clsid = ci[i].Clsid;
            return (bmp.Save(path, &clsid, nullptr) == Ok) ? 0 : 2;
        }
    }
    return 1;
}

struct ColDef {
    int size;
    int lv;
    bool charging;
    int zoom;
    const wchar_t* label;
};

int RenderSheet(const wchar_t* path) {
    const int n = 6;
    ColDef cols[] = {
        {16, 76, false, 1, L"16"},
        {20, 76, false, 1, L"20"},
        {24, 76, false, 1, L"24"},
        {32, 76, false, 1, L"32"},
        {48, 76, false, 1, L"48"},
        {32, 9,  false, 1, L"9%"},
        {32, 62, true,  1, L"充电"},
        {24, 76, false, 5, L"24px 放大 5 倍"},
    };
    const int nc = 8;

    const int rowH = 86, headH = 34, padL = 34, labelW = 104, gap = 14;

    int cellW[nc], left[nc];
    int x = padL + labelW;
    for (int i = 0; i < nc; ++i) {
        int drawW = cols[i].size * cols[i].zoom;
        cellW[i] = std::max(drawW, 30);
        left[i] = x;
        x += cellW[i] + gap;
    }
    int totalW = x + 18;
    int sectionH = n * rowH + 6;
    int totalH = 30 + 34 + sectionH + 18 + 34 + sectionH + 14;

    Bitmap canvas(totalW, totalH, PixelFormat32bppARGB);
    Graphics g(&canvas);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(Color(255, 255, 255, 255));

    DrawTextAt(g, L"尺寸 px", 12.0f, 2.0f, 56.0f, 22.0f, 0x9AA0A8, 11.5f);
    for (int i = 0; i < nc; ++i) {
        DrawTextAt(g, cols[i].label, (float)left[i], 2.0f, (float)cellW[i], 22.0f,
                   0x6B7280, 11.5f, StringAlignmentCenter);
    }

    struct Section { Pal pal; uint32_t bg; uint32_t fg; uint32_t sub; std::wstring title; };
    Section secs[2];
    secs[0] = {kLightPal, 0xF5F5F7, 0x1D1D1F, 0x6E6E73, L"浅色任务栏"};
    secs[1] = {kDarkPal,  0x1C1C1E, 0xF5F5F7, 0x98989D, L"深色任务栏"};

    int y = 30;
    for (int si = 0; si < 2; ++si) {
        Section& sc = secs[si];
        SolidBrush bg(C(sc.bg));
        g.FillRectangle(&bg, RectF(0.0f, (REAL)y, (REAL)totalW, (REAL)(headH + sectionH)));

        DrawTextAt(g, sc.title, 14.0f, (float)y + 5.0f, (float)(totalW - 24), 22.0f,
                   sc.sub, 12.0f, StringAlignmentNear, true);
        y += headH;

        for (int r = 0; r < n; ++r) {
            float ry = (float)(y + r * rowH);
            DrawTextAt(g, Names[r], (float)padL, ry, (float)labelW, (float)rowH, sc.fg,
                       12.5f, StringAlignmentNear);
            for (int c = 0; c < nc; ++c) {
                int drawW = cols[c].size * cols[c].zoom;
                float ix = (float)left[c] + (cellW[c] - drawW) / 2.0f;
                float iy = ry + (rowH - drawW) / 2.0f;
                if (cols[c].zoom > 1) {
                    DrawIconZoom(g, AllFn[r], ix, iy, cols[c].size, cols[c].zoom,
                                 cols[c].lv, cols[c].charging, sc.pal);
                } else {
                    DrawIconAt(g, AllFn[r], ix, iy, cols[c].size, cols[c].lv,
                               cols[c].charging, sc.pal);
                }
            }
        }
        y += sectionH + 18;
    }
    return SavePng(canvas, path);
}

int RenderBig(const wchar_t* path) {
    const int n = 6;
    const int cell = 128, pad = 18, icon = 88, labelH = 28;
    const int bandH = icon + labelH + 30;

    int W = n * cell + pad * 2;
    int H = bandH * 2 + pad;
    Bitmap canvas(W, H, PixelFormat32bppARGB);
    Graphics g(&canvas);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(Color(255, 255, 255, 255));

    struct Band { Pal pal; uint32_t bg; uint32_t fg; };
    Band bands[2] = {
        {kLightPal, 0xF5F5F7, 0x1D1D1F},
        {kDarkPal,  0x1C1C1E, 0xF5F5F7},
    };

    for (int b = 0; b < 2; ++b) {
        int by = pad + b * bandH;
        SolidBrush bg(C(bands[b].bg));
        g.FillRectangle(&bg, RectF(0.0f, (REAL)by, (REAL)W, (REAL)bandH));
        for (int i = 0; i < n; ++i) {
            float cx = (float)(pad + i * cell);
            float ix = cx + (cell - icon) / 2.0f;
            float iy = (float)by + 14.0f;
            DrawIconAt(g, AllFn[i], ix, iy, icon, 76, false, bands[b].pal);
            DrawTextAt(g, Names[i], cx, iy + icon + 2.0f, (float)cell, (float)labelH,
                       bands[b].fg, 12.5f, StringAlignmentCenter);
        }
    }
    return SavePng(canvas, path);
}

int wmain(int argc, wchar_t** argv) {
    GdiplusStartupInput si;
    ULONG_PTR token = 0;
    GdiplusStartup(&token, &si, nullptr);
    bool big = (argc >= 3 && wcscmp(argv[2], L"big") == 0);
    int rc = big ? RenderBig(argc >= 2 ? argv[1] : L"num-big.png")
                 : RenderSheet(argc >= 2 ? argv[1] : L"num-sheet.png");
    FreeTextPool();
    GdiplusShutdown(token);
    return rc;
}
