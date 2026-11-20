// tmwindow_offscreen.cpp - 离屏渲染自截图
//
// 为什么需要这个：
//   之前我用 PowerShell 的 Start-Process + PrintWindow 截图，
//   每次都会创建可见窗口并抢焦点，用户正在干别的事就会被打断。
//
// 这个实现让程序**自己**把内容画到内存位图，然后写成 BMP 文件。
// 窗口以 SW_HIDE 创建，全程不出现在屏幕上，不碰输入队列。
#include "tmwindow.h"

#include <cstdio>

namespace tmp {

bool TmWindow::SaveOffscreen(const wchar_t* path) {
    if (!hwnd_ || !path) return false;

    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int W = rc.right - rc.left;
    const int H = rc.bottom - rc.top;
    if (W <= 0 || H <= 0) return false;

    // 建一个与屏幕兼容的内存 DC + 位图
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, W, H);
    ReleaseDC(nullptr, screen);
    if (!mem || !bmp) {
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        return false;
    }
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    // 直接用现有的绘制流程画到这块位图
    PaintAll(mem, rc);

    SelectObject(mem, oldBmp);

    // 取像素并写 BMP
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;      // 负数 = 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    const int stride = W * 4;
    const int dataSize = stride * H;
    std::vector<uint8_t> pixels(static_cast<size_t>(dataSize));

    HDC screen2 = GetDC(nullptr);
    int got = GetDIBits(screen2, bmp, 0, H, pixels.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen2);

    DeleteObject(bmp);
    DeleteDC(mem);

    if (got != H) return false;

    // 写 32 位 BMP（保留 alpha 通道无用，但结构简单可靠）
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"wb");
    if (!f) return false;

    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;   // 'BM'
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + dataSize;

    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
    fwrite(pixels.data(), 1, static_cast<size_t>(dataSize), f);
    fclose(f);
    return true;
}

} // namespace tmp
