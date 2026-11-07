// 临时测试工具：打开面板 → 发送滚轮消息 → PrintWindow 存 BMP
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <cstdio>

static bool SaveBmp(HBITMAP bmp, HDC srcDC, int w, int h, const wchar_t* path) {
    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(bi);
    bi.biWidth = w;
    bi.biHeight = h;         // 正数 = 自下而上
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    int rowSize = ((w * 32 + 31) / 32) * 4;
    int imageSize = rowSize * h;
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + imageSize;

    BYTE* bits = new BYTE[imageSize];
    if (!GetDIBits(srcDC, bmp, 0, h, bits, (BITMAPINFO*)&bi, DIB_RGB_COLORS)) {
        delete[] bits;
        return false;
    }
    FILE* f = _wfopen(path, L"wb");
    if (!f) {
        delete[] bits;
        return false;
    }
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&bi, sizeof(bi), 1, f);
    fwrite(bits, imageSize, 1, f);
    fclose(f);
    delete[] bits;
    return true;
}

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int wheels = argc > 1 ? _wtoi(argv[1]) : 1;
    const wchar_t* out = argc > 2 ? argv[2] : L"_panel.bmp";

    HWND main = FindWindowW(L"PeripheralBatteryMain", nullptr);
    if (!main) {
        wprintf(L"FAIL: 主窗口未找到\n");
        return 1;
    }
    HWND panel = FindWindowW(L"PeripheralBatteryPanel", nullptr);
    if (!panel || !IsWindowVisible(panel)) {
        PostMessageW(main, WM_APP + 5, 0, 0);  // 面板没开就让它打开（这条消息是切换语义）
        Sleep(900);
        panel = FindWindowW(L"PeripheralBatteryPanel", nullptr);
    }
    if (!panel || !IsWindowVisible(panel)) {
        wprintf(L"FAIL: 面板未打开 (panel=%p)\n", panel);
        return 2;
    }

    for (int i = 0; i < wheels; ++i) {
        SendMessageW(panel, WM_MOUSEWHEEL, MAKEWPARAM(0, (WORD)(-WHEEL_DELTA)), 0);
        Sleep(60);
    }
    Sleep(300);

    RECT rc{};
    GetWindowRect(panel, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BOOL ok = PrintWindow(panel, mem, 2 /*PW_RENDERFULLCONTENT*/);
    SelectObject(mem, old);

    bool saved = SaveBmp(bmp, mem, w, h, out);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    wprintf(L"panel=%p size=%dx%d printwindow=%d saved=%d -> %ls\n",
            panel, w, h, ok, saved, out);
    return saved ? 0 : 3;
}
