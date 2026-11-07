// 提示窗实测：等它弹出来，量位置和尺寸，抓一张真实渲染图，确认没抢焦点。
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

static bool SaveBmp(HWND hwnd, const wchar_t* path) {
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BOOL ok = PrintWindow(hwnd, mem, 2);

    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = -h;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;

    int stride = w * 4;
    std::vector<BYTE> bits(stride * h);
    GetDIBits(mem, bmp, 0, h, bits.data(), (BITMAPINFO*)&ih, DIB_RGB_COLORS);

    FILE* f = _wfopen(path, L"wb");
    bool saved = false;
    if (f) {
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = fh.bfOffBits + (DWORD)bits.size();
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&ih, sizeof(ih), 1, f);
        fwrite(bits.data(), 1, bits.size(), f);
        fclose(f);
        saved = true;
    }

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok && saved;
}

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HWND toast = nullptr;
    for (int i = 0; i < 120; ++i) {          // 最多等 12 秒
        toast = FindWindowW(L"PeripheralBatteryToast", nullptr);
        if (toast && IsWindowVisible(toast)) break;
        Sleep(100);
    }
    if (!toast || !IsWindowVisible(toast)) {
        wprintf(L"FAIL: 提示窗没有出现\n");
        return 1;
    }

    RECT rc{};
    GetWindowRect(toast, &rc);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);

    HWND fg = GetForegroundWindow();
    HWND fgRoot = GetAncestor(fg, GA_ROOT);

    wprintf(L"提示窗: %ld,%ld %ldx%ld\n", rc.left, rc.top,
            rc.right - rc.left, rc.bottom - rc.top);
    wprintf(L"工作区右下: %ld,%ld\n", work.right, work.bottom);
    wprintf(L"距右边缘 %ld px, 距底边 %ld px\n", work.right - rc.right, work.bottom - rc.bottom);
    wprintf(L"抢焦点: %s (前台窗口=%p, 提示窗=%p)\n",
            fgRoot == toast ? L"是（错误）" : L"否（正确）", (void*)fgRoot, (void*)toast);

    const wchar_t* out = argc >= 2 ? argv[1] : L"_toast.bmp";
    bool saved = SaveBmp(toast, out);
    wprintf(L"截图: %s\n", saved ? L"成功" : L"失败");
    return 0;
}
