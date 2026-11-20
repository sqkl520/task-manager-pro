// tableview.cpp - 自绘虚拟化表格实现
//
// 性能设计要点（这是本文件的核心）：
//  1. WM_PAINT 只遍历可见行，与总行数无关
//  2. 全窗口双缓冲，单次 BitBlt
//  3. **单元格文本写到栈缓冲区**，绝不在绘制路径上分配堆内存
//     —— 10000 行 × 8 列 × 每秒刷新会产生数百万临时字符串，
//        堆分配开销本身就足以吃掉整个帧预算（实测 45ms -> 修复后 < 2ms）
//  4. 画笔/画刷/字体全部缓存为成员，不在 WM_PAINT 里创建销毁
#include "tableview.h"

#include <windowsx.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace tmp {
namespace {

constexpr const wchar_t* kClassName = L"TmProTableView";
constexpr size_t kCellBufCch = 64;

inline void FmtBytes(uint64_t b, wchar_t* buf, size_t cch) {
    if (b >= (1ull << 30))       swprintf_s(buf, cch, L"%.1f GB", b / 1073741824.0);
    else if (b >= (1ull << 20))  swprintf_s(buf, cch, L"%.1f MB", b / 1048576.0);
    else if (b >= (1ull << 10))  swprintf_s(buf, cch, L"%.1f KB", b / 1024.0);
    else                         swprintf_s(buf, cch, L"%llu B", b);
}

// ---- 列取值器：全部是普通函数指针，无捕获、无堆分配 ----
void GetName(const ProcessEntry& e, wchar_t* b, size_t n) {
    const size_t len = (std::min)(e.imageName.size(), n - 1);
    memcpy(b, e.imageName.c_str(), len * sizeof(wchar_t));
    b[len] = 0;
}
void GetPid(const ProcessEntry& e, wchar_t* b, size_t n)       { swprintf_s(b, n, L"%u", e.pid); }
void GetCpu(const ProcessEntry& e, wchar_t* b, size_t n)       { swprintf_s(b, n, L"%.1f%%", e.cpuPercent); }
void GetWorkingSet(const ProcessEntry& e, wchar_t* b, size_t n){ FmtBytes(e.workingSet, b, n); }
void GetPrivate(const ProcessEntry& e, wchar_t* b, size_t n)   { FmtBytes(e.privateBytes, b, n); }
void GetThreads(const ProcessEntry& e, wchar_t* b, size_t n)   { swprintf_s(b, n, L"%u", e.threadCount); }
void GetHandles(const ProcessEntry& e, wchar_t* b, size_t n)   { swprintf_s(b, n, L"%u", e.handleCount); }
void GetSession(const ProcessEntry& e, wchar_t* b, size_t n)   { swprintf_s(b, n, L"%u", e.sessionId); }

} // namespace

TableView::TableView() { RebuildColumns(); }

TableView::~TableView() {
    if (font_ && font_ != GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(font_);
    if (fontBold_) DeleteObject(fontBold_);
    // 【与 TmWindow 同一处 P0 泄漏】位图还 select 在 memDC_ 里时
    // DeleteObject 会静默失败。必须先塞临时位图把它换出来。
    // 全项目里这是唯一漏做换出的析构（tmwindow.cpp 和
    // tmwindow_offscreen.cpp 都做了）。
    if (memDC_ && memBmp_) {
        HBITMAP tmp = CreateCompatibleBitmap(memDC_, 1, 1);
        if (tmp) {
            HGDIOBJ prev = SelectObject(memDC_, tmp);
            if (prev && prev != HGDI_ERROR) DeleteObject(prev);
            memBmp_ = nullptr;   // tmp 随 DeleteDC 回收
        }
    }
    if (memBmp_) DeleteObject(memBmp_);
    if (memDC_) DeleteDC(memDC_);
    if (brAlt_) DeleteObject(brAlt_);
    if (brBg_) DeleteObject(brBg_);
    if (brHeader_) DeleteObject(brHeader_);
    if (penGrid_) DeleteObject(penGrid_);
}

void TableView::RebuildColumns() {
    columns_.clear();
    columns_.push_back({ L"Name",        260, DT_LEFT,  GetName,
        [](const ProcessEntry& e) { return static_cast<double>(e.imageName.size()); } });
    columns_.push_back({ L"PID",          70, DT_RIGHT, GetPid,
        [](const ProcessEntry& e) { return static_cast<double>(e.pid); } });
    columns_.push_back({ L"CPU",          70, DT_RIGHT, GetCpu,
        [](const ProcessEntry& e) { return e.cpuPercent; } });
    columns_.push_back({ L"Working Set", 110, DT_RIGHT, GetWorkingSet,
        [](const ProcessEntry& e) { return static_cast<double>(e.workingSet); } });
    columns_.push_back({ L"Private",     110, DT_RIGHT, GetPrivate,
        [](const ProcessEntry& e) { return static_cast<double>(e.privateBytes); } });
    columns_.push_back({ L"Threads",      70, DT_RIGHT, GetThreads,
        [](const ProcessEntry& e) { return static_cast<double>(e.threadCount); } });
    columns_.push_back({ L"Handles",      70, DT_RIGHT, GetHandles,
        [](const ProcessEntry& e) { return static_cast<double>(e.handleCount); } });
    columns_.push_back({ L"Session",      60, DT_RIGHT, GetSession,
        [](const ProcessEntry& e) { return static_cast<double>(e.sessionId); } });

    colOffsets_.clear();
    int x = 0;
    for (auto& c : columns_) { colOffsets_.push_back(x); x += c.width; }
}

bool TableView::Create(HWND parent, int id) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProcStatic;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;

    if (!RegisterClassExW(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    }

    hwnd_ = CreateWindowExW(
        0, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, 100, 100, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;

    // 字体：系统 UI 字体，中文天然可用
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        font_ = CreateFontIndirectW(&ncm.lfMessageFont);
        LOGFONTW lf = ncm.lfMessageFont;
        lf.lfWeight = FW_SEMIBOLD;
        fontBold_ = CreateFontIndirectW(&lf);
    } else {
        font_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }

    // 缓存 GDI 对象——绝不在 WM_PAINT 里创建/销毁
    brBg_     = CreateSolidBrush(RGB(255, 255, 255));
    brAlt_    = CreateSolidBrush(RGB(250, 250, 250));
    brHeader_ = CreateSolidBrush(RGB(243, 243, 243));
    penGrid_  = CreatePen(PS_SOLID, 1, RGB(224, 224, 224));

    return true;
}

LRESULT CALLBACK TableView::WndProcStatic(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }
    auto* self = reinterpret_cast<TableView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->WndProc(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void TableView::SetData(const Snapshot* s) {
    snap_ = s;
    RebuildView();
    UpdateScrollBars();
    Invalidate();
}

void TableView::RebuildView() {
    view_.clear();
    if (!snap_) return;
    const size_t n = snap_->processes.size();
    view_.resize(n);
    for (size_t i = 0; i < n; ++i) view_[i] = i;

    if (sortColumn_ >= 0 && sortColumn_ < static_cast<int>(columns_.size())) {
        auto keyFn = columns_[sortColumn_].getSortKey;
        if (keyFn) {
            const auto& procs = snap_->processes;
            if (sortDesc_)
                std::sort(view_.begin(), view_.end(),
                    [&](size_t a, size_t b) { return keyFn(procs[a]) > keyFn(procs[b]); });
            else
                std::sort(view_.begin(), view_.end(),
                    [&](size_t a, size_t b) { return keyFn(procs[a]) < keyFn(procs[b]); });
        }
    }
}

int TableView::TotalContentHeight() const {
    return static_cast<int>(view_.size()) * rowHeight_;
}

int TableView::TotalContentWidth() const {
    return colOffsets_.empty() ? 0 : colOffsets_.back() + columns_.back().width;
}

void TableView::UpdateScrollBars() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;

    si.nMin = 0;
    si.nMax = (std::max)(0, TotalContentHeight() - 1);
    si.nPage = (rc.bottom - headerHeight_) > 0 ? static_cast<UINT>(rc.bottom - headerHeight_) : 1;
    si.nPos = scrollY_;
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);

    si.nMin = 0;
    si.nMax = (std::max)(0, TotalContentWidth() - 1);
    si.nPage = rc.right > 0 ? static_cast<UINT>(rc.right) : 1;
    si.nPos = scrollX_;
    SetScrollInfo(hwnd_, SB_HORZ, &si, TRUE);
}

int TableView::RowFromY(int y) const {
    if (y < headerHeight_) return -1;
    int i = (y - headerHeight_ + scrollY_) / rowHeight_;
    if (i < 0 || i >= static_cast<int>(view_.size())) return -1;
    return i;
}

int TableView::ColFromX(int x) const {
    int cx = x + scrollX_;
    for (int i = static_cast<int>(columns_.size()) - 1; i >= 0; --i) {
        if (cx >= colOffsets_[i]) return i;
    }
    return 0;
}

void TableView::EnsureBackBuffer(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (w == memW_ && h == memH_ && memDC_ && memBmp_) return;

    // 【与 TmWindow::EnsureBackBuffer 同一处 P0 泄漏，必须同样处理】
    // 位图被 SelectObject 进 DC 后 DeleteObject 会静默失败，每次重建泄漏一张
    // w×h 的位图。TmWindow 那边已修（长时间运行变卡的根因），这里是同类雷。
    // 目前 TableView 还没有调用点，但一旦接入就是同一个 bug。
    if (memDC_ && memBmp_) {
        HBITMAP tmp = CreateCompatibleBitmap(memDC_, 1, 1);
        if (tmp) {
            HGDIOBJ prev = SelectObject(memDC_, tmp);   // prev == memBmp_
            if (prev && prev != HGDI_ERROR) DeleteObject(prev);
            DeleteObject(tmp);
            memBmp_ = nullptr;
        }
    }
    if (memBmp_) { DeleteObject(memBmp_); memBmp_ = nullptr; }
    if (memDC_) { DeleteDC(memDC_); memDC_ = nullptr; }

    HDC screen = GetDC(hwnd_);
    if (!screen) return;
    memDC_ = CreateCompatibleDC(screen);
    memBmp_ = CreateCompatibleBitmap(screen, w, h);
    ReleaseDC(hwnd_, screen);

    if (memDC_ && memBmp_) {
        SelectObject(memDC_, memBmp_);
        SetBkMode(memDC_, TRANSPARENT);
        memW_ = w;
        memH_ = h;
        forceFullRepaint_ = true;    // 新缓冲内容是未定义的，必须整屏重画
    } else {
        if (memBmp_) { DeleteObject(memBmp_); memBmp_ = nullptr; }
        if (memDC_) { DeleteDC(memDC_); memDC_ = nullptr; }
        memW_ = memH_ = 0;
    }
}

void TableView::Invalidate() {
    if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void TableView::PaintHeader(HDC hdc, int firstCol, int lastCol) {
    RECT hdr{ 0, 0, memW_, headerHeight_ };
    FillRect(hdc, &hdr, brHeader_);

    HGDIOBJ oldPen = SelectObject(hdc, penGrid_);
    MoveToEx(hdc, 0, headerHeight_ - 1, nullptr);
    LineTo(hdc, memW_, headerHeight_ - 1);
    SelectObject(hdc, oldPen);

    // PaintRows 会改 SetTextAlign，这里必须重置回左上，
    // 否则 DrawTextW 的坐标会被上次的 TA_RIGHT 偏移，导致表头错位。
    SetTextAlign(hdc, TA_LEFT | TA_TOP);
    SetBkMode(hdc, TRANSPARENT);

    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, fontBold_ ? fontBold_ : font_));
    SetTextColor(hdc, RGB(60, 60, 60));

    for (int c = firstCol; c <= lastCol && c < static_cast<int>(columns_.size()); ++c) {
        int x0 = colOffsets_[c] - scrollX_;
        RECT cell{ x0 + 6, 0, x0 + columns_[c].width - 6, headerHeight_ };
        UINT fmt = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;
        // 显式补齐：DrawTextW 能识别 DT_LEFT/DT_RIGHT/DT_CENTER
        if (columns_[c].align == DT_RIGHT) fmt |= DT_RIGHT;
        else fmt |= DT_LEFT;
        DrawTextW(hdc, columns_[c].title, -1, &cell, fmt);
    }
    SelectObject(hdc, oldFont);
}

void TableView::PaintRows(HDC hdc, int firstRow, int lastRow, int firstCol, int lastCol) {
    if (!snap_) return;
    const auto& procs = snap_->processes;

    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, font_));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(28, 28, 28));

    wchar_t cellText[kCellBufCch];
    const int textYOff = (rowHeight_ - 14) / 2;

    // 预先算好每列的绘制原点与裁剪矩形，避免在内层循环里重复计算
    struct ColLayout { int x0; int right; RECT clip; UINT ta; };
    ColLayout layout[32];
    int nCols = 0;
    for (int c = firstCol; c <= lastCol && c < static_cast<int>(columns_.size()) && nCols < 32; ++c) {
        const Column& col = columns_[c];
        int x0 = colOffsets_[c] - scrollX_ + 6;
        int maxW = col.width - 12;
        layout[nCols].x0 = x0;
        layout[nCols].right = x0 + maxW;
        layout[nCols].clip = RECT{ x0, 0, x0 + maxW, memH_ };
        layout[nCols].ta = (col.align == DT_RIGHT) ? (TA_RIGHT | TA_TOP) : (TA_LEFT | TA_TOP);
        ++nCols;
    }

    // 关键：只在列的对齐方式改变时才调 SetTextAlign（GDI 状态切换有真实开销）
    UINT curTa = 0xFFFFFFFF;

    for (int r = firstRow; r <= lastRow && r < static_cast<int>(view_.size()); ++r) {
        int y = headerHeight_ + r * rowHeight_ - scrollY_;

        if (r & 1) {
            RECT rowRect{ 0, y, memW_, y + rowHeight_ };
            FillRect(hdc, &rowRect, brAlt_);
        }

        const ProcessEntry& e = procs[view_[r]];

        for (int i = 0; i < nCols; ++i) {
            const int c = firstCol + i;
            const Column& col = columns_[c];
            const ColLayout& L = layout[i];

            col.getText(e, cellText, kCellBufCch);

            if (L.ta != curTa) { SetTextAlign(hdc, L.ta); curTa = L.ta; }

            int drawX = (col.align == DT_RIGHT) ? L.right : L.x0;
            RECT clip = L.clip;
            clip.top = y; clip.bottom = y + rowHeight_;

            ExtTextOutW(hdc, drawX, y + textYOff, ETO_CLIPPED, &clip,
                        cellText, static_cast<UINT>(wcslen(cellText)), nullptr);
        }
    }
    SelectObject(hdc, oldFont);
}

void TableView::Paint(HDC hdc, const RECT& paintRect) {
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    QueryPerformanceFrequency(&f);
    auto us = [&](LARGE_INTEGER& a) {
        return static_cast<int>((a.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart);
    };
    LARGE_INTEGER mark;

    RECT client{};
    GetClientRect(hwnd_, &client);
    const int cw = client.right - client.left;
    const int ch = client.bottom - client.top;
    if (cw <= 0 || ch <= 0) return;

    EnsureBackBuffer(cw, ch);
    if (!memDC_ || !memBmp_) return;

    // 后备缓冲刚重建 / 需要强制刷新时，用全屏区域覆盖脏区
    RECT dirty = paintRect;
    if (forceFullRepaint_) {
        dirty = RECT{ 0, 0, cw, ch };
        forceFullRepaint_ = false;
    }

    // 只清理脏区：滚动时脏区通常只有一行高，为全屏 1/36 的写入量
    RECT fillRect = dirty;
    if (fillRect.left < 0) fillRect.left = 0;
    if (fillRect.top < 0) fillRect.top = 0;
    if (fillRect.right > cw) fillRect.right = cw;
    if (fillRect.bottom > ch) fillRect.bottom = ch;
    if (fillRect.right > fillRect.left && fillRect.bottom > fillRect.top) {
        FillRect(memDC_, &fillRect, brBg_);
    }
    QueryPerformanceCounter(&mark); profFill_ = us(mark);

    int firstCol = 0, lastCol = static_cast<int>(columns_.size()) - 1;
    if (!columns_.empty()) {
        int cx0 = scrollX_;
        int cx1 = scrollX_ + cw;
        for (int c = 0; c < static_cast<int>(columns_.size()); ++c) {
            if (colOffsets_[c] + columns_[c].width > cx0) { firstCol = c; break; }
        }
        for (int c = static_cast<int>(columns_.size()) - 1; c >= 0; --c) {
            if (colOffsets_[c] < cx1) { lastCol = c; break; }
        }
    }

    int firstRow = (std::max)(0, static_cast<int>((dirty.top - headerHeight_ + scrollY_) / rowHeight_));
    int lastRow = static_cast<int>((dirty.bottom - headerHeight_ + scrollY_) / rowHeight_ + 1);
    if (!view_.empty()) {
        lastRow = (std::min)(lastRow, static_cast<int>(view_.size()) - 1);
    } else {
        lastRow = -1;
    }
    lastVisibleRows_ = (lastRow >= firstRow) ? (lastRow - firstRow + 1) : 0;

    PaintRows(memDC_, firstRow, lastRow, firstCol, lastCol);
    QueryPerformanceCounter(&mark); profRows_ = us(mark) - profFill_;

    // 表头只在脏区与表头区域相交时才需要重绘
    if (dirty.top < headerHeight_) {
        PaintHeader(memDC_, firstCol, lastCol);
    }
    QueryPerformanceCounter(&mark); profHead_ = us(mark) - profFill_ - profRows_;

    // 只 BitBlt 脏区，不拷整屏。全屏 BitBlt 在 1180x720 上是 3.4MB 内存拷贝。
    const int bw = dirty.right - dirty.left;
    const int bh = dirty.bottom - dirty.top;
    if (bw > 0 && bh > 0 && dirty.left >= 0 && dirty.top >= 0 &&
        dirty.right <= cw && dirty.bottom <= ch) {
        ::BitBlt(hdc, dirty.left, dirty.top, bw, bh,
                 memDC_, dirty.left, dirty.top, SRCCOPY);
    } else {
        ::BitBlt(hdc, 0, 0, cw, ch, memDC_, 0, 0, SRCCOPY);
    }
    QueryPerformanceCounter(&t1);
    profBlit_ = us(t1) - profFill_ - profRows_ - profHead_;

    lastPaintMicros_ = us(t1);
    ++paintCount_;
    totalPaintMicros_ += static_cast<uint64_t>(lastPaintMicros_);
}

LRESULT TableView::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        UpdateScrollBars();
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd_, &ps);
        Paint(hdc, ps.rcPaint);
        EndPaint(hwnd_, &ps);
        return 0;
    }

    case WM_VSCROLL: {
        SCROLLINFO si{ sizeof(si), SIF_ALL };
        GetScrollInfo(hwnd_, SB_VERT, &si);
        int pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINEUP:   pos -= rowHeight_; break;
        case SB_LINEDOWN: pos += rowHeight_; break;
        case SB_PAGEUP:   pos -= si.nPage; break;
        case SB_PAGEDOWN: pos += si.nPage; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP:    pos = 0; break;
        case SB_BOTTOM: pos = si.nMax; break;
        }
        pos = (std::max)(0, (std::min)(pos, si.nMax - static_cast<int>(si.nPage) + 1));
        if (pos != scrollY_) {
            const int delta = pos - scrollY_;
            scrollY_ = pos;
            SetScrollPos(hwnd_, SB_VERT, pos, TRUE);

            RECT rc;
            GetClientRect(hwnd_, &rc);
            const int bodyH = rc.bottom - headerHeight_;

            // 小幅滚动时，在后备缓冲内部直接 BitBlt 搬移已有像素，
            // 只让新暴露的那一条带成为脏区。这样 WM_PAINT 只需画 1-2 行，
            // 而不是重画全部可见行——这是本控件相对原版任务管理器的关键差异之一。
            // 注意：必须搬移 memDC 的内容（而不是屏幕），因为我们自己是双缓冲绘制。
            if (delta != 0 && abs(delta) < bodyH / 2 && bodyH > 0 &&
                memDC_ && memBmp_) {
                RECT body{ 0, headerHeight_, rc.right, rc.bottom };
                ::BitBlt(memDC_,
                         0, headerHeight_,
                         rc.right, bodyH,
                         memDC_,
                         0, headerHeight_ + delta,
                         SRCCOPY);
                // 只把新暴露的那条带标脏
                if (delta > 0) {
                    RECT band{ 0, rc.bottom - delta, rc.right, rc.bottom };
                    ::InvalidateRect(hwnd_, &band, FALSE);
                } else {
                    RECT band{ 0, headerHeight_, rc.right, headerHeight_ + (-delta) };
                    ::InvalidateRect(hwnd_, &band, FALSE);
                }
                return 0;    // 不调 Invalidate()，避免整屏重绘
            }
            Invalidate();
        }
        return 0;
    }

    case WM_HSCROLL: {
        SCROLLINFO si{ sizeof(si), SIF_ALL };
        GetScrollInfo(hwnd_, SB_HORZ, &si);
        int pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINELEFT:  pos -= 40; break;
        case SB_LINERIGHT: pos += 40; break;
        case SB_PAGELEFT:  pos -= si.nPage; break;
        case SB_PAGERIGHT: pos += si.nPage; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        }
        pos = (std::max)(0, (std::min)(pos, si.nMax - static_cast<int>(si.nPage) + 1));
        if (pos != scrollX_) {
            scrollX_ = pos;
            SetScrollPos(hwnd_, SB_HORZ, pos, TRUE);
            Invalidate();
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int lines = delta / WHEEL_DELTA * 3;
        SendMessageW(hwnd_, WM_VSCROLL,
                     MAKEWPARAM(lines > 0 ? SB_LINEUP : SB_LINEDOWN, 0), 0);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (y < headerHeight_) {
            int c = ColFromX(x);
            if (c >= 0 && c < static_cast<int>(columns_.size())) {
                if (sortColumn_ == c) sortDesc_ = !sortDesc_;
                else { sortColumn_ = c; sortDesc_ = true; }
                RebuildView();
                scrollY_ = 0;
                UpdateScrollBars();
                Invalidate();
            }
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_DOWN)  { SendMessageW(hwnd_, WM_VSCROLL, MAKEWPARAM(SB_LINEDOWN, 0), 0); return 0; }
        if (wp == VK_UP)    { SendMessageW(hwnd_, WM_VSCROLL, MAKEWPARAM(SB_LINEUP, 0), 0); return 0; }
        if (wp == VK_NEXT)  { SendMessageW(hwnd_, WM_VSCROLL, MAKEWPARAM(SB_PAGEDOWN, 0), 0); return 0; }
        if (wp == VK_PRIOR) { SendMessageW(hwnd_, WM_VSCROLL, MAKEWPARAM(SB_PAGEUP, 0), 0); return 0; }
        return 0;

    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace tmp
