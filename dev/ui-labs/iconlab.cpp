// 图标实验室：把候选设计并排渲染到一张对比图。
// 独立于主程序，改图标时不用碰 ui.cpp。
//
// 注意（MinGW 坑）：宽字符 printf 的 %s 指 char*，不是 wchar_t*，
// 拼中文一律用 std::wstring，不要用 swprintf。
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>   // MinGW 的 gdiplus.h 依赖 PROPID
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace Gdiplus;

// ---------------------------------------------------------------- 基础

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
    uint32_t ok, mid, low, crit;
    Color stroke;      // 中性描边（不参与电量配色）
    bool dark = false;
};

Pal kLightPal = {0x16A34A, 0xE0A800, 0xF97316, 0xDC2626, Color(255, 58, 68, 82), false};
Pal kDarkPal  = {0x34D399, 0xFBBF24, 0xFB923C, 0xF87171, Color(255, 236, 240, 248), true};

uint32_t LevelCol(int lv, const Pal& p) {
    if (lv < 0) return 0x8A919C;
    if (lv <= 10) return p.crit;
    if (lv <= 20) return p.low;
    if (lv <= 50) return p.mid;
    return p.ok;
}

Color C(uint32_t rgb, BYTE a = 255) {
    return Color(a, (BYTE)((rgb >> 16) & 0xFF), (BYTE)((rgb >> 8) & 0xFF), (BYTE)(rgb & 0xFF));
}

// 闪电：先描一圈底色再填充。用矩形光晕会压掉电池描边，这是之前发现的坑。
void Bolt(Graphics& g, float s, uint32_t fill, uint32_t outline) {
    PointF v[] = {
        PointF(13.3f * s, 7.4f * s), PointF(9.7f * s, 13.0f * s),
        PointF(11.9f * s, 13.0f * s), PointF(10.7f * s, 17.4f * s),
        PointF(14.4f * s, 11.3f * s), PointF(12.1f * s, 11.3f * s),
    };
    SolidBrush br(C(fill));
    Pen pen(C(outline), 1.5f * s);
    pen.SetLineJoin(LineJoinRound);
    g.FillPolygon(&br, v, 6);
    g.DrawPolygon(&pen, v, 6);
}

// ---------------------------------------------------------------- 候选设计

// 1 横向描边电池：最传统的形态，小尺寸最容易认
void Icon1(Graphics& g, float s, int lv, bool ch, const Pal& p) {
    Pen pen(p.stroke, 2.0f * s);
    pen.SetLineJoin(LineJoinRound);
    GraphicsPath body;
    MakeRR(body, 2.4f * s, 7.2f * s, 15.6f * s, 9.6f * s, 3.2f * s);
    g.DrawPath(&pen, &body);

    Pen nub(p.stroke, 2.1f * s);
    nub.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
    g.DrawLine(&nub, 19.1f * s, 10.9f * s, 19.1f * s, 13.1f * s);

    float inX = 4.6f * s, inTop = 9.4f * s, inH = 5.2f * s, fullW = 13.4f * s;
    if (lv >= 0) {
        float w = fullW * (lv / 100.0f);
        if (lv > 0) w = std::max(w, 2.0f * s);
        GraphicsPath f;
        MakeRR(f, inX, inTop, w, inH, std::min(1.6f * s, w / 2.0f));
        SolidBrush fb(C(LevelCol(lv, p)));
        g.FillPath(&fb, &f);
    } else {
        GraphicsPath d;
        MakeRR(d, inX + fullW / 2.0f - 2.0f * s, inTop + inH / 2.0f - 1.0f * s,
               4.0f * s, 2.0f * s, 1.0f * s);
        SolidBrush fb(C(LevelCol(lv, p)));
        g.FillPath(&fb, &d);
    }
    if (ch) {
        Bolt(g, s, p.dark ? 0x1B1F25 : 0xFFFFFF, p.dark ? 0xFFFFFF : 0x16181D);
    }
}

// 2 横向扁平电量条：无描边，中性轨道 + 实色填充
void Icon2(Graphics& g, float s, int lv, bool ch, const Pal& p) {
    const BYTE trackA = p.dark ? 70 : 62;
    SolidBrush track(Color(trackA, p.stroke.GetR(), p.stroke.GetG(), p.stroke.GetB()));
    GraphicsPath body;
    MakeRR(body, 2.2f * s, 7.2f * s, 15.6f * s, 9.6f * s, 4.8f * s);
    g.FillPath(&track, &body);
    GraphicsPath nub;
    MakeRR(nub, 17.4f * s, 9.9f * s, 3.4f * s, 4.2f * s, 1.4f * s);
    g.FillPath(&track, &nub);

    if (lv >= 0 && lv > 0) {
        float w = std::max(15.6f * s * (lv / 100.0f), 2.6f * s);
        GraphicsPath f;
        MakeRR(f, 2.2f * s, 7.2f * s, w, 9.6f * s, std::min(4.8f * s, w / 2.0f));
        SolidBrush fb(C(LevelCol(lv, p)));
        g.FillPath(&fb, &f);
    }
    if (ch) {
        Bolt(g, s, p.dark ? 0x1B1F25 : 0xFFFFFF, p.dark ? 0xFFFFFF : 0x16181D);
    }
}

// 3 竖向粗描边电池：竖版，含凸点
void Icon3(Graphics& g, float s, int lv, bool ch, const Pal& p) {
    Pen pen(p.stroke, 2.3f * s);
    pen.SetLineJoin(LineJoinRound);
    GraphicsPath body;
    MakeRR(body, 5.8f * s, 4.8f * s, 12.4f * s, 14.6f * s, 3.0f * s);
    g.DrawPath(&pen, &body);

    GraphicsPath nub;
    MakeRR(nub, 9.6f * s, 2.2f * s, 4.8f * s, 2.8f * s, 0.9f * s);
    SolidBrush nb(p.stroke);
    g.FillPath(&nb, &nub);

    float pad = 2.3f * s;
    float inX = 5.8f * s + pad, inW = 12.4f * s - pad * 2.0f;
    float inTop = 4.8f * s + pad, inBot = 19.4f * s - pad;
    if (lv >= 0) {
        float h = (inBot - inTop) * (lv / 100.0f);
        if (lv > 0) h = std::max(h, 2.2f * s);
        if (h > 0.4f) {
            GraphicsPath f;
            MakeRR(f, inX, inBot - h, inW, h, std::min(2.6f * s, h / 2.0f));
            SolidBrush fb(C(LevelCol(lv, p)));
            g.FillPath(&fb, &f);
        }
    } else {
        GraphicsPath d;
        MakeRR(d, inX, inBot - 2.2f * s, inW, 2.2f * s, 1.1f * s);
        SolidBrush fb(C(LevelCol(lv, p)));
        g.FillPath(&fb, &d);
    }
    if (ch) {
        Bolt(g, s, p.dark ? 0x1B1F25 : 0xFFFFFF, p.dark ? 0xFFFFFF : 0x16181D);
    }
}

// 4 实心徽章：彩色圆角方块 + 白色电池负形，小尺寸最跳
void Icon4(Graphics& g, float s, int lv, bool ch, const Pal& p) {
    GraphicsPath badge;
    MakeRR(badge, 1.2f * s, 1.2f * s, 21.6f * s, 21.6f * s, 5.4f * s);
    SolidBrush bb(C(LevelCol(lv, p)));
    g.FillPath(&bb, &badge);

    Pen pen(Color(255, 255, 255, 255), 1.9f * s);
    pen.SetLineJoin(LineJoinRound);
    GraphicsPath body;
    MakeRR(body, 3.9f * s, 8.4f * s, 13.4f * s, 7.2f * s, 2.2f * s);
    g.DrawPath(&pen, &body);
    Pen nub(Color(255, 255, 255, 255), 2.4f * s);
    nub.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
    g.DrawLine(&nub, 18.4f * s, 10.7f * s, 18.4f * s, 13.3f * s);

    if (lv >= 0 && lv > 0) {
        float fullW = 10.6f * s;
        float w = std::max(fullW * (lv / 100.0f), 2.0f * s);
        GraphicsPath f;
        MakeRR(f, 5.3f * s, 9.8f * s, w, 4.4f * s, std::min(1.4f * s, w / 2.0f));
        SolidBrush fb(Color(255, 255, 255, 255));
        g.FillPath(&fb, &f);
    }
    if (ch) Bolt(g, s, 0xFFFFFF, LevelCol(lv, p));
}

// 5 圆环仪表：最"现代"，但识别度依赖形状记忆
void Icon5(Graphics& g, float s, int lv, bool ch, const Pal& p) {
    float cx = 12.0f * s, cy = 12.0f * s, r = 7.9f * s;
    Pen track(Color(p.dark ? 76 : 54, p.stroke.GetR(), p.stroke.GetG(), p.stroke.GetB()),
               2.7f * s);
    g.DrawEllipse(&track, cx - r, cy - r, r * 2, r * 2);

    Pen arc(C(LevelCol(lv, p)), 2.7f * s);
    arc.SetStartCap(LineCapRound);
    arc.SetEndCap(LineCapRound);
    if (lv < 0) {
        SolidBrush dot(C(LevelCol(lv, p)));
        g.FillEllipse(&dot, cx - 1.7f * s, cy - 1.7f * s, 3.4f * s, 3.4f * s);
    } else if (lv > 0) {
        float sweep = std::min(359.9f, (float)(360.0 * lv / 100.0));
        g.DrawArc(&arc, cx - r, cy - r, r * 2, r * 2, -90.0f, sweep);
    }
    if (ch) {
        SolidBrush halo(Color(255, p.dark ? 32 : 242, p.dark ? 36 : 243, p.dark ? 43 : 247));
        g.FillEllipse(&halo, cx - 4.6f * s, cy - 4.6f * s, 9.2f * s, 9.2f * s);
        Bolt(g, s, p.dark ? 0x1B1F25 : 0xFFFFFF, p.dark ? 0xFFFFFF : 0x16181D);
    }
}

// 6 竖向极简：细描边 + 顶部平端子（弱化凸点，竖版里最有设计感的）
void Icon6(Graphics& g, float s, int lv, bool ch, const Pal& p) {
    Pen pen(p.stroke, 1.75f * s);
    pen.SetLineJoin(LineJoinRound);

    GraphicsPath cap;
    MakeRR(cap, 8.9f * s, 2.0f * s, 6.2f * s, 3.0f * s, 1.2f * s);
    SolidBrush cb(p.stroke);
    g.FillPath(&cb, &cap);

    GraphicsPath body;
    MakeRR(body, 6.6f * s, 4.4f * s, 10.8f * s, 16.6f * s, 3.4f * s);
    g.DrawPath(&pen, &body);

    float pad = 1.75f * s;
    float inX = 6.6f * s + pad, inW = 10.8f * s - pad * 2.0f;
    float inTop = 4.4f * s + pad, inBot = 21.0f * s - pad;
    if (lv >= 0) {
        float h = (inBot - inTop) * (lv / 100.0f);
        if (lv > 0) h = std::max(h, 2.0f * s);
        if (h > 0.4f) {
            GraphicsPath f;
            MakeRR(f, inX, inBot - h, inW, h, std::min(2.6f * s, h / 2.0f));
            SolidBrush fb(C(LevelCol(lv, p)));
            g.FillPath(&fb, &f);
        }
    } else {
        GraphicsPath d;
        MakeRR(d, inX, inBot - 2.0f * s, inW, 2.0f * s, 1.0f * s);
        SolidBrush fb(C(LevelCol(lv, p)));
        g.FillPath(&fb, &d);
    }
    if (ch) {
        Bolt(g, s, p.dark ? 0x1B1F25 : 0xFFFFFF, p.dark ? 0xFFFFFF : 0x16181D);
    }
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

// 先把图标画到 size×size 的位图，再用最近邻放大。
// 这样看到的是 16px 的真实像素，而不是矢量重绘出来的理想形态。
void DrawIconZoom(Graphics& g, DrawFn fn, float x, float y, int size, int zoom, int lv,
                  bool ch, const Pal& p) {
    Bitmap tmp(size, size, PixelFormat32bppARGB);
    {
        Graphics tg(&tmp);
        tg.SetSmoothingMode(SmoothingModeAntiAlias);
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

struct ColDef {
    int size;
    int lv;
    bool charging;
    int zoom;
    const wchar_t* label;
};

int RenderSheet(const wchar_t* path) {
    DrawFn fns[] = {Icon1, Icon2, Icon3, Icon4, Icon5, Icon6};
    const int n = 6;

    ColDef cols[] = {
        {16, 18, false, 1, L"16"},
        {20, 18, false, 1, L"20"},
        {24, 18, false, 1, L"24"},
        {32, 18, false, 1, L"32"},
        {48, 18, false, 1, L"48"},
        {48, 75, false, 1, L"75%"},
        {48, 5,  false, 1, L"5%"},
        {48, 62, true,  1, L"充电"},
        {16, 18, false, 5, L"16px 放大 5 倍"},
    };
    const int nc = 9;

    const int rowH = 86;
    const int headH = 34;
    const int padL = 34;
    const int labelW = 30;
    const int gap = 14;

    int cellW[nc];
    int left[nc];
    int x = padL + labelW;
    for (int i = 0; i < nc; ++i) {
        int drawW = cols[i].size * cols[i].zoom;
        cellW[i] = std::max(drawW, 34);
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

    DrawTextAt(g, L"尺寸 px", 12.0f, 2.0f, 58.0f, 22.0f, 0x9AA0A8, 11.5f);
    for (int i = 0; i < nc; ++i) {
        DrawTextAt(g, cols[i].label, (float)left[i], 2.0f, (float)cellW[i], 22.0f,
                   0x6B7280, 11.5f, StringAlignmentCenter);
    }

    struct Section { Pal pal; uint32_t bg; uint32_t fg; uint32_t sub; std::wstring title; };
    Section secs[2];
    secs[0] = {kLightPal, 0xF2F3F6, 0x16181D, 0x6B7280, L"浅色任务栏（白底）"};
    secs[1] = {kDarkPal,  0x20242B, 0xEDEFF4, 0x9BA3AF, L"深色任务栏（深底）"};

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
            std::wstring num(1, (wchar_t)(L'1' + r));
            DrawTextAt(g, num, 12.0f, ry, 24.0f, (float)rowH, sc.fg, 14.0f,
                       StringAlignmentCenter, true);

            for (int c = 0; c < nc; ++c) {
                int drawW = cols[c].size * cols[c].zoom;
                float ix = (float)left[c] + (cellW[c] - drawW) / 2.0f;
                float iy = ry + (rowH - drawW) / 2.0f;
                if (cols[c].zoom > 1) {
                    DrawIconZoom(g, fns[r], ix, iy, cols[c].size, cols[c].zoom,
                                 cols[c].lv, cols[c].charging, sc.pal);
                } else {
                    DrawIconAt(g, fns[r], ix, iy, cols[c].size, cols[c].lv,
                               cols[c].charging, sc.pal);
                }
            }
        }
        y += sectionH + 18;
    }

    CLSID clsid;
    bool ok = false;
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size > 0) {
        std::vector<BYTE> buf(size);
        ImageCodecInfo* ci = reinterpret_cast<ImageCodecInfo*>(buf.data());
        GetImageEncoders(num, size, ci);
        for (UINT i = 0; i < num; ++i) {
            if (wcscmp(ci[i].MimeType, L"image/png") == 0) {
                clsid = ci[i].Clsid;
                ok = true;
                break;
            }
        }
    }
    if (!ok) return 1;
    return (canvas.Save(path, &clsid, nullptr) == Ok) ? 0 : 2;
}


// 大图对照：只看设计本身，不纠结小尺寸
int RenderBig(const wchar_t* path) {
    DrawFn fns[] = {Icon1, Icon2, Icon3, Icon4, Icon5, Icon6};
    const wchar_t* names[] = {
        L"1 横向描边", L"2 横向扁平", L"3 竖向粗描边",
        L"4 实心徽章", L"5 圆环仪表", L"6 竖向极简",
    };
    const int n = 6;
    const int cell = 116, pad = 16, icon = 84, labelH = 26;
    const int bandH = icon + labelH + 34;

    int W = n * cell + pad * 2;
    int H = bandH * 2 + pad;
    Bitmap canvas(W, H, PixelFormat32bppARGB);
    Graphics g(&canvas);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(Color(255, 255, 255, 255));

    struct Band { Pal pal; uint32_t bg; uint32_t fg; };
    Band bands[2] = {
        {kLightPal, 0xF4F5F8, 0x16181D},
        {kDarkPal,  0x1E2228, 0xEDEFF4},
    };

    for (int b = 0; b < 2; ++b) {
        int by = pad + b * bandH;
        SolidBrush bg(C(bands[b].bg));
        g.FillRectangle(&bg, RectF(0.0f, (REAL)by, (REAL)W, (REAL)bandH));

        for (int i = 0; i < n; ++i) {
            float cx = (float)(pad + i * cell);
            float ix = cx + (cell - icon) / 2.0f;
            float iy = (float)by + 16.0f;
            DrawIconAt(g, fns[i], ix, iy, icon, 18, false, bands[b].pal);
            DrawTextAt(g, names[i], cx, iy + icon + 4.0f, (float)cell, (float)labelH,
                       bands[b].fg, 12.5f, StringAlignmentCenter);
        }
    }

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
            return (canvas.Save(path, &clsid, nullptr) == Ok) ? 0 : 2;
        }
    }
    return 1;
}

int wmain(int argc, wchar_t** argv) {
    GdiplusStartupInput si;
    ULONG_PTR token = 0;
    GdiplusStartup(&token, &si, nullptr);
    bool big = (argc >= 3 && wcscmp(argv[2], L"big") == 0);
    int rc = big ? RenderBig(argc >= 2 ? argv[1] : L"icon-big.png")
                 : RenderSheet(argc >= 2 ? argv[1] : L"icon-candidates.png");
    GdiplusShutdown(token);
    return rc;
}
