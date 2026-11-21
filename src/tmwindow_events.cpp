// tmwindow_events.cpp - TmWindow 的消息处理
#include "tmwindow.h"

#include <windowsx.h>
#include <shellapi.h>
#include <algorithm>
#include <cstdio>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

namespace tmp {

using namespace theme;

// ============ 整体绘制 ============

void TmWindow::PaintAll(HDC hdc, const RECT& client) {
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    QueryPerformanceFrequency(&f);

    const int W = client.right, H = client.bottom;
    if (W <= 0 || H <= 0) return;

    EnsureBackBuffer(W, H);
    if (!memDC_ || !memBmp_) return;

    RECT full{ 0, 0, W, H };
    FillSolid(memDC_, full, kBgWindow);

    PaintTabBar(memDC_);

    switch (tabs_.Current()) {
    case TabId::Processes:
        // 【简略信息】simpleView_ 打开时 BuildLayout 把 rcCards_ 压成零高矩形。
        // PaintCards 拿到零高矩形会在 cardsTop-1 处画出一条孤立横线（DrawFrame
        // 不判空），所以整段跳过。PaintCards 自己的首行是 `if (!snap_) return;`，
        // 不归本模块改，这里在调用点挡住即可。
        if (rcCards_.bottom > rcCards_.top) PaintCards(memDC_);
        PaintColumnHeader(memDC_);
        PaintRows(memDC_);
        PaintScrollBar(memDC_);
        break;
    case TabId::Details:
        PaintDetailsPage(memDC_);
        break;
    case TabId::Performance:
        perfPage_.Paint(memDC_);
        break;
    case TabId::Users:
        pagesView_.PaintUsers(memDC_);
        break;
    // 标签栏恢复成 7 个后，这两页要接回绘制分支。
    // 它们的实现一直在 pages_view.cpp 里，只是此前没有页签入口。
    case TabId::AppHistory:
        pagesView_.PaintAppHistory(memDC_);
        break;
    case TabId::Startup:
        pagesView_.PaintStartup(memDC_);
        break;
    case TabId::Services:
        // 服务描述/组渐进填充：每次绘制补一批（规格实测每个 2.8ms，
        // 306 个全补要 857ms，不能一次做完）。补完前持续触发重绘。
        if (pagesView_.ServicesNeedEnrich()) {
            if (pagesView_.EnrichServicesBatch(8)) {
                PostMessageW(hwnd_, WM_TIMER, 2, 0);   // 让消息泵空转让出，再继续
            }
        }
        pagesView_.PaintServices(memDC_);
        break;
    default: {
        // 其余页面：显示"尚未实现"提示（后续逐个补齐）
        HFONT old = static_cast<HFONT>(SelectObject(memDC_, fontBody_));
        SetBkMode(memDC_, TRANSPARENT);
        RECT r{ rcContent_.left + 16, rcContent_.top + 40,
                rcContent_.right - 16, rcContent_.top + 70 };
        DrawTextClipped(memDC_, L"此页面尚未实现", &r,
                        DT_SINGLELINE | DT_VCENTER | DT_LEFT, kTextDisabled);
        SelectObject(memDC_, old);
        break;
    }
    }

    PaintToolbar(memDC_);
    // 【真值】真实 Win10 任务管理器底部没有状态栏，只有工具条。
    // PaintStatusBar 保留函数但不再调用。

    ::BitBlt(hdc, 0, 0, W, H, memDC_, 0, 0, SRCCOPY);

    QueryPerformanceCounter(&t1);
    lastPaintMicros_ = static_cast<int>((t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart);
    ++paintCount_;
}

// ============ 命中测试 ============

// 把绘制耗时统计写盘。供 --paintstats 模式使用。
// 只读两个已有成员（lastPaintMicros_ / paintCount_），不影响绘制路径。
void TmWindow::DumpPaintStats(const wchar_t* path) const {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path, L"w") != 0 || !fp) return;
    fwprintf(fp, L"lastPaintMicros=%d\npaintCount=%llu\nvisibleRows=%d\n",
             lastPaintMicros_, static_cast<unsigned long long>(paintCount_),
             lastVisibleRows_);
    fclose(fp);
}


int TmWindow::HitTestGroupHeader(int y) const {    // 【边界检查】与 HitTestRow 一样必须先夹在列表可视区内。
    // 原来没有这两行，滚出屏幕的组头仍能命中：滚动后点列表上方空白处、
    // 或点列头带，会误触发组折叠。而在其它页（详细信息等）点同一 y 甚至会
    // 翻转进程页隐藏的组状态 —— 跨页状态泄漏。
    if (y < rcList_.top || y >= rcList_.bottom) return -1;
    // 逐行累加：行高不再相等（组头/父行 28、子行 22），不能再用除法反查。
    // 必须先命中判定再累加，否则子行会把下面的行全推错 6px。
    int ly = rcList_.top - scrollY_;
    for (size_t i = 0; i < rows_.size(); ++i) {
        const int rh = RowHeightFor(rows_[i].kind == ViewRow::GroupHeaderRow, rows_[i].indentLevel);
        if (y >= ly && y < ly + rh) {
            return (rows_[i].kind == ViewRow::GroupHeaderRow) ? static_cast<int>(i) : -1;
        }
        ly += rh;
    }
    return -1;
}

int TmWindow::HitTestRow(int y) const {
    if (y < rcList_.top || y >= rcList_.bottom) return -1;
    // 同上：累加循环，行高按行类型取。这个循环本来就是 O(n) 且一旦命中即
    // return，命中点之前最多遍历到屏幕首行，实际开销与原来相同。
    int ly = rcList_.top - scrollY_;
    for (size_t i = 0; i < rows_.size(); ++i) {
        const int rh = RowHeightFor(rows_[i].kind == ViewRow::GroupHeaderRow, rows_[i].indentLevel);
        if (y >= ly && y < ly + rh) {
            return (rows_[i].kind == ViewRow::ProcessRow) ? static_cast<int>(i) : -1;
        }
        ly += rh;
    }
    return -1;
}

// ============ 光标判定 ============

// 按坐标决定光标形状。**WM_SETCURSOR 和 WM_MOUSEMOVE 都调它**，
// 两处共用一份判据，避免漂移。
//
// 【为什么不能只在 WM_MOUSEMOVE 里设】鼠标不动就没有 WM_MOUSEMOVE，
// 而窗口重绘 / 焦点切回 / 被别的窗口盖住再露出 / 滚动导致视觉位移，
// 这些时刻光标都该重算。原来 WM_SETCURSOR 直接 `return TRUE`，
// 等于宣示"我管光标"却只在那一条消息里管，于是手型会滞留在空白处。
void TmWindow::ApplyHoverCursor(int x, int y) {
    // 1) 列边界：拖拽改宽
    if (tabs_.Current() == TabId::Processes && HitTestColResize(x, y) >= 0) {
        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        return;
    }

    // 2) 滚动条：HitTestRow 只看 y 不看 x，鼠标停在列表右侧滚动条上时
    //    同样会命中某一行。但滚动条不是可点的"项"，给普通箭头。
    //    必须排在下面的行判据之前。
    if (x >= ScrollBarLeft() && x < rcList_.right) {
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return;
    }

    // 3) 页签栏
    if (tabs_.HitTest(x, y) != TabId::Count) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return;
    }

    switch (tabs_.Current()) {
    case TabId::Processes:
        // 列头（可点排序）、组头（可折叠）、数据行（可选中）
        if (HitTestColumn(x, y) >= 0 ||
            HitTestGroupHeader(y) >= 0 ||
            HitTestRow(y) >= 0) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return;
        }
        break;

    case TabId::Details:
        // 详细信息页扁平列表，没有分组头
        if (detailSelectedRow_ >= -1 &&
            y >= rcContent_.top + theme::kHeaderHeight && y < rcContent_.bottom &&
            x < rcList_.right) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return;
        }
        break;

    case TabId::Startup:
    case TabId::Users:
    case TabId::Services:
    case TabId::AppHistory:
        // 四个次要页：行区（表头以下）可点
        if (pagesView_.HitRowAt(x, y) >= 0) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return;
        }
        break;

    case TabId::Performance:
        // 性能页：侧栏卡片可点选，右侧图表区也可点（切子视图）。
        // 这里只区分「侧栏 / 图表区」两块，具体命中留给 OnClick 自己算 ——
        // 为一个光标去复刻一遍完整的命中逻辑，两处判据迟早漂移。
        {
            const RECT& pb = perfPage_.Bounds();
            const bool inPerf = x >= pb.left && x < pb.right && y >= pb.top && y < pb.bottom;
            if (inPerf) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return;
            }
        }
        break;

    default:
        break;
    }

    // 4) 底部工具栏按钮
    if (HitTestToolbar(x, y) != 0) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return;
    }

    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
}

// ============ 菜单 ============

void TmWindow::RebuildMenu() {
    HMENU old = mainMenu_;
    // 【必须传真实状态】之前这里把四个视图选项全部硬编码成固定值，
    // 于是「按类型分组」「显示完整名称」等菜单项的勾选永远不随实际状态变化 ——
    // 用户点了「按类型分组」，行为变了，但菜单上的 ✓ 不会动，
    // 也无法判断当前到底是不是分组状态。
    mainMenu_ = MenuBuilder::BuildMainMenu(
        static_cast<int>(tabs_.Current()),
        updateIntervalMs_,
        alwaysOnTop_,
        hideWhenMinimized_,
        showFullName_,
        groupByType_,
        minimizeOnUse_,
        showAllHistory_);
    ::SetMenu(hwnd_, mainMenu_);
    ::DrawMenuBar(hwnd_);
    if (old) DestroyMenu(old);
}

void TmWindow::LoadPageData(TabId t) {
    switch (t) {

    case TabId::Users:      pagesView_.RefreshUsers();      break;
    case TabId::Services:   pagesView_.RefreshServices();   break;
    case TabId::Startup:    pagesView_.RefreshStartup();    break;
    case TabId::AppHistory: pagesView_.RefreshAppHistory(); break;

    default: break;
    }
}

// OnMenuCommand 的实现在 tmwindow_menus.cpp（65 个命令的完整实现）。
// ============ 消息处理 ============

LRESULT TmWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_DPICHANGED: {
        // 【必须处理】进程声明了 PER_MONITOR_AWARE_V2，窗口被拖到另一块
        // 不同缩放的显示器上时系统会发这条消息。此前完全没有分支，于是
        // theme::Dpi() 一直返回启动时那台显示器的值 —— 布局按 96 DPI 的
        // 硬编码尺寸画，在高分屏上整体偏小（用户反馈"高分屏怪怪的"）。
        //
        // 两件事：
        //   1) 更新 theme::Dpi() 的缓存，让后续所有 Scaled() 用新值
        //   2) 采纳系统建议的新窗口矩形（不采纳会让窗口尺寸与 DPI 不匹配）
        theme::SetDpi(HIWORD(wp));
        const RECT* prc = reinterpret_cast<const RECT*>(lp);
        if (prc) {
            SetWindowPos(hwnd_, nullptr,
                         prc->left, prc->top,
                         prc->right - prc->left, prc->bottom - prc->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        BuildLayout();
        ClampScroll();
        InvalidateAll();
        return 0;
    }

    case WM_SIZE: {
        BuildLayout();
        // 【必须重新钳制 scrollY_】BuildLayout 会重算 rcList_，窗口变大时
        // 视口变高 → maxScroll 变小。如果 scrollY_ 还停在旧的大值上，
        // 列表底部会留出一大块空白（实测：滚到底后放大 260px，底部空白正好 261px），
        // 同时滚动条滑块被顶出轨道、压住下箭头 —— 因为滑块命中判断排在箭头
        // 之前，点下箭头会变成拖滑块。
        //
        // 采集开着时 1 秒内会被 RebuildRows 自愈，但"更新速度=已暂停"档下
        // 会永久保留。
        ClampScroll();
        InvalidateAll();
        return 0;
    }

    // 「最小化时隐藏」：勾选后最小化时把窗口藏起来、留托盘图标。
    // 这里拦截 SC_MINIMIZE 而不是在 WM_SIZE 里判断，是因为 WM_SIZE
    // 拿不到"是用户最小化还是程序自己改尺寸"的信息。
    case WM_SYSCOMMAND: {
        if ((wp & 0xFFF0) == SC_MINIMIZE && hideWhenMinimized_) {
            ShowWindow(hwnd_, SW_HIDE);
            AddTrayIcon();
            return 0;
        }
        break;   // 其余交给 DefWindowProc
    }

    // 「使用时最小化」：勾选后本程序让出前台就自动最小化。
    //
    // 【此前是死开关】src/tmwindow_menus.cpp:1278 只翻转 minimizeOnUse_ 并重
    // 建菜单（多一个 ✓），全项目没有任何 WM_ACTIVATE / WM_KILLFOCUS 处理，
    // 所以勾选后程序行为零变化。规格见 docs/specs/menu_spec.md:696-703：
    // 「让出前台时自动最小化，处理 WM_ACTIVATE 的 WA_INACTIVE」。
    //
    // 【为什么用 WM_ACTIVATE 而不是 WM_KILLFOCUS】点本程序自己的菜单也会让
    // 窗口短暂失去键盘焦点（WM_KILLFOCUS 会响），那样每点一次菜单就把自己
    // 最小化一次。WM_ACTIVATE 的 WA_INACTIVE 是"整个窗口掉到后台"。
    //
    // 【怎么排除"自己弹出的窗口"（对话框 / 菜单 / 托盘右键弹窗）】
    // 不用再加一个 dialogOpen_ 标志，而是直接看新前台窗口属于哪个进程：
    // MessageBox、DialogBoxIndirectParam、TrackPopupMenu 全部是本进程的
    // 窗口，GetWindowThreadProcessId 会返回我们自己的 pid → 跳过。这比手工
    // 在每个对话框前后加减计数更可靠 —— 本项目有 22 处 MessageBoxW 加若干
    // 个 DialogBox，漏一处就会"点个提示框把主窗口最小化掉"。
    //
    // 【只在可见且未最小化时触发】--shot 离屏模式窗口是 SW_HIDE，
    // IsWindowVisible 为假 → 不触发，避免打断自截图流程；已经最小化
    // （IsIconic）时不重复 Minimize，否则每切一次窗口都会再播一遍动画。
    case WM_ACTIVATE: {
        if (LOWORD(wp) != WA_INACTIVE || !minimizeOnUse_) break;
        if (!IsWindowVisible(hwnd_) || IsIconic(hwnd_)) break;
        HWND fg = GetForegroundWindow();
        DWORD fgPid = 0;
        if (fg) GetWindowThreadProcessId(fg, &fgPid);
        if (fgPid == GetCurrentProcessId()) break;   // 自己的对话框/菜单/弹窗
        // 双保险：MessageBox/DialogBox 刚创建时，新前台窗口可能还没被系统
        // 更新完（WM_ACTIVATE 先到、SetForegroundWindow 后到），上面那次
        // pid 检查会读到上一个外部程序。GetLastActivePopup 直接问
        // "本窗口拥有的弹窗里最近活跃的是哪个"，有**可见**弹窗就跳过。
        // 必须带 IsWindowVisible：隐藏/已销毁但句柄还挂在链上的弹窗会被
        // GetLastActivePopup 一并报出来，不判可见的话这个开关会永久失效。
        HWND popup = GetLastActivePopup(hwnd_);
        if (popup != hwnd_ && IsWindowVisible(popup)) break;
        // 重入保护：ShowWindow(SW_MINIMIZE) 会同步再发一次 WM_ACTIVATE。
        // IsIconic 通常已经为真能挡住，但那一拍依赖 USER32 内部写 WS_MINIMIZE
        // 位的时序，不写死在假设上 —— 递归调用自己会把栈吃穿。
        if (minimizeOnUseBusy_) break;
        minimizeOnUseBusy_ = true;
        if (hideWhenMinimized_) {
            // 两个开关同时勾上：与「最小化时隐藏」一致 —— 直接收起进托盘。
            // 这里不 SendMessage(SC_MINIMIZE) 绕一圈，因为那会在 WM_ACTIVATE
            // 的处理过程中重入 WndProc，没有必要；动作本身只有两行。
            ShowWindow(hwnd_, SW_HIDE);
            AddTrayIcon();
        } else {
            ShowWindow(hwnd_, SW_MINIMIZE);
        }
        minimizeOnUseBusy_ = false;
        break;   // 仍交给 DefWindowProc 维护它自己的激活状态
    }

    // 托盘图标：左键/双击恢复窗口，右键弹出「还原/关闭」
    case WM_TRAYICON: {
        if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) {
            RestoreFromTray();
            return 0;
        }
        if (lp == WM_RBUTTONUP) {
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, CMD_RESTORE_FROM_TRAY, L"还原(&R)");
            AppendMenuW(m, MF_STRING, CMD_CLOSE_APP,         L"关闭(&C)");
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd_);
            TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
            DestroyMenu(m);
            return 0;
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd_, &ps);
        RECT rc;
        GetClientRect(hwnd_, &rc);
        PaintAll(hdc, rc);
        EndPaint(hwnd_, &ps);
        return 0;
    }

    case WM_COMMAND: {
        OnMenuCommand(LOWORD(wp));
        return 0;
    }

    case WM_TIMER: {
        if (wp == 2) {
            // 服务描述渐进填充的续跑标记
            InvalidateAll();
            return 0;
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (colDragCol_ >= 0) {
            // 列宽拖拽结束：持久化
            colDragCol_ = -1;
            ReleaseCapture();
            SaveColumnWidths();
            InvalidateAll();
            return 0;
        }
        if (scrollDragging_) {
            scrollDragging_ = false;
            ReleaseCapture();
            return 0;
        }
        if (pressedCol_ >= 0) {
            pressedCol_ = -1;
            InvalidateAll();
        }
        return 0;
    }

    // 【健壮性修复】拖拽状态只靠 WM_LBUTTONUP 清除是不够的。
    //
    // 按住鼠标拖动列边界时，如果发生以下情况，WM_LBUTTONUP 根本不会到达：
    //   - 用户按 Alt+Tab 切走程序
    //   - 拖拽中弹出模态对话框
    //   - 另一个程序抢走捕获
    // 此时 colDragCol_ / scrollDragging_ 会永久卡在「拖拽中」，
    // 之后任何一次鼠标移动都会被当成拖拽，界面会莫名其妙跟着鼠标变。
    //
    // WM_CAPTURECHANGED 在捕获丢失时必定触发，在这里兜底清理。
    case WM_CAPTURECHANGED: {
        if (colDragCol_ >= 0) {
            colDragCol_ = -1;
            SaveColumnWidths();
            InvalidateAll();
        }
        scrollDragging_ = false;
        return 0;
    }

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);

        // 列宽拖拽中：实时改宽度
        if (colDragCol_ >= 0) {
            int delta = x - colDragStartX_;
            int w = colDragStartW_ + delta;
            if (w < 40) w = 40;        // 最小可读宽度
            if (w > 600) w = 600;      // 防止把整行挤没
            if (colDragCol_ == 0) {
                // 名称列是弹性的，拖它没有意义（宽度由剩余空间决定），
                // 所以拖到名称列右边界时改为调整**第一个可见的固定列**。
                // 【列可见性】不能直接写死 1：第 1 列可能已被隐藏，
                // 那样拖拽会去改一个看不见的列的宽度，画面毫无反应。
                int next = -1;
                for (int i = 1; i < kSortColCount; ++i) {
                    if (IsColumnVisible(i)) { next = i; break; }
                }
                if (next < 0) {          // 只剩名称列：放弃这次拖拽
                    colDragCol_ = -1;
                    ReleaseCapture();
                    return 0;
                }
                colDragCol_ = next;
                colDragStartW_ = EffectiveColWidth(next);
                colDragStartX_ = x;
            } else {
                colWidths_[colDragCol_] = w;
                InvalidateAll();
            }
            return 0;
        }

        // 拖拽滚动条
        if (scrollDragging_) {
            const int total = ContentHeight();
            const int view = ViewportHeight();
            if (total > view && view > 0) {
                const int trackTop = rcList_.top;
                const int trackH = view;
                int thumbH = trackH * view / total;
                if (thumbH < 24) thumbH = 24;
                if (thumbH > trackH) thumbH = trackH;

                int rel = y - scrollDragOffset_ - trackTop;
                int span = trackH - thumbH;
                if (span > 0) {
                    int maxScroll = total - view;
                    int sy = rel * maxScroll / span;
                    if (sy < 0) sy = 0;
                    if (sy > maxScroll) sy = maxScroll;
                    if (sy != scrollY_) {
                        scrollY_ = sy;
                        ClampScroll();   // 拖滚动条：每像素走这里，绝不能全量重建
                        InvalidateAll();
                    }
                }
            }
            return 0;
        }

        // 标签栏悬停
        TabId t = tabs_.HitTest(x, y);
        if (t != tabs_.Hover()) {
            tabs_.SetHover(t);
            InvalidateAll();
        }

        // 列头悬停（只在进程页）
        int hc = (tabs_.Current() == TabId::Processes) ? HitTestColumn(x, y) : -1;
        if (hc != hoverCol_) {
            hoverCol_ = hc;
            InvalidateAll();
        }

        int r = HitTestRow(y);
        if (r != hoverRow_) {
            hoverRow_ = r;

            InvalidateAll();
        }

        // 四个次要页（启动/用户/服务/应用历史）的行悬停走 PagesView 自己那套。
        // 此前 pagesView_.OnHover 根本不存在，hover_ 初始化后再没人写过，
        // 于是这四页的行悬停永远不出现。
        switch (tabs_.Current()) {
            case TabId::Startup:
            case TabId::Users:
            case TabId::Services:
            case TabId::AppHistory:
                pagesView_.SetCurrentTab(static_cast<int>(tabs_.Current()));
                pagesView_.OnHover(x, y);
                break;
            default:
                break;
        }

        // 光标：与 WM_SETCURSOR 走同一个函数，避免两处判据漂移
        ApplyHoverCursor(x, y);
        return 0;
    }

    case WM_SETCURSOR: {
        // 【光标必须在这里设，不能只在 WM_MOUSEMOVE 里设】
        //
        // 原来这里是 `return TRUE;` —— 告诉系统"我已处理光标"，但真正的
        // SetCursor 全在 WM_MOUSEMOVE 分支里。而**鼠标不动就没有
        // WM_MOUSEMOVE**：窗口重绘、焦点切回、别的窗口盖住再露出、滚动导致
        // 视觉位移而鼠标没动 —— 这些时刻光标停在上一次的值不更新。
        //
        // 用户报「ListView 空白处无条件 hand pointer」就是这个：从某一行
        // 移到空白处时若没有新的 WM_MOUSEMOVE，手型就留在那儿了。
        //
        // 正确做法是每次 WM_SETCURSOR 都按当前坐标重算一次。系统在鼠标移动、
        // 窗口激活、光标被别的程序改动后都会发这条消息，比 WM_MOUSEMOVE 可靠。
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd_, &pt);
            ApplyHoverCursor(pt.x, pt.y);
            return TRUE;    // 已自行 SetCursor
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);

        // 滚动条滑块：开始拖拽
        {
            RECT th = ScrollThumbRect();
            if (th.right > th.left &&
                x >= th.left && x < th.right && y >= th.top && y < th.bottom) {
                scrollDragging_ = true;
                scrollDragOffset_ = y - th.top;
                SetCapture(hwnd_);
                return 0;
            }
            // 两端箭头按钮：点一下滚一行。
            //
            // 【补漏】PaintScrollBar 里画了上下箭头，但此前这里没有任何命中测试 ——
            // 用户看得到箭头却点不动，是「画了没接」的典型断链。
            // 箭头区高 16px，位于轨道最上和最下。
            if (th.right > th.left && x >= th.left && x < th.right) {
                // 【17 而不是 16】按钮高是 SM_CXVSCROLL = 17（tmwindow_list.cpp
                // 的 kScrollBtnH）。这里原来写死 16，点击箭头最下面 1px 会落空，
                // 落到"点轨道翻页"的分支上去。两处必须同源。
                const int btnH = 17;
                // 【子行 22】行高不再相等，`scrollY_ ± kRowHeight` 会累积漂移
                // （每越过一个子行就多滚 6px），几次之后箭头就把行滚到半截上。
                // 改成"减去/加上当前那一行自己的行高"：这样无论行高怎么变，
                // 滚过的量恰好等于一行，且行内偏移（相位）严格保持不变，
                // 永远不会漂。向下同理（RebuildRows 末尾按实际总高钳制上限）。
                if (y >= rcList_.top && y < rcList_.top + btnH) {
                    int rhCur = 0;                       // 0 = 没有可滚的行
                    int acc = 0;
                    for (size_t i = 0; i < rows_.size(); ++i) {
                        const int rh = RowHeightFor(rows_[i].kind == ViewRow::GroupHeaderRow,
                                                    rows_[i].indentLevel);
                        if (scrollY_ < acc + rh) { rhCur = rh; break; }   // 停在 rows_[i] 内
                        acc += rh;
                    }
                    scrollY_ -= rhCur;
                    ClampScroll();   // 只钳制范围，不重建行表
                    InvalidateAll();
                    return 0;
                }
                if (y >= rcList_.bottom - btnH && y < rcList_.bottom) {
                    int rhCur = 0;
                    int acc = 0;
                    for (size_t i = 0; i < rows_.size(); ++i) {
                        const int rh = RowHeightFor(rows_[i].kind == ViewRow::GroupHeaderRow,
                                                    rows_[i].indentLevel);
                        if (scrollY_ < acc + rh) { rhCur = rh; break; }
                        acc += rh;
                    }
                    scrollY_ += rhCur;
                    ClampScroll();   // 只钳制范围，不重建行表（滚动不改行表内容）
                    InvalidateAll();
                    return 0;
                }
            }
            // 点轨道空白处：翻页
            if (th.right > th.left && x >= th.left && x < th.right &&
                y >= rcList_.top && y < rcList_.bottom) {
                int page = ViewportHeight();
                scrollY_ += (y < th.top) ? -page : page;
                ClampScroll();   // 只钳制范围，不重建行表
                InvalidateAll();
                return 0;
            }
        }

        // 切换标签
        TabId t = tabs_.HitTest(x, y);
        if (t != TabId::Count) {
            tabs_.SetCurrent(t);
            scrollY_ = 0;
            detailScrollY_ = 0;
            BuildLayout();
            LoadPageData(t);
            InvalidateAll();
            RebuildMenu();
            return 0;
        }

        // 底部工具条
        {
            UINT cmd = HitTestToolbar(x, y);
            if (cmd) {
                OnMenuCommand(cmd);
                return 0;
            }
        }

        // 列边界拖拽（只在进程页生效）。
        // 必须排在「列头点击 → 排序」**之前**，否则拖边界会先触发排序。
        if (tabs_.Current() == TabId::Processes) {
            int dragCol = HitTestColResize(x, y);
            if (dragCol >= 0) {
                colDragCol_ = dragCol;
                colDragStartX_ = x;
                colDragStartW_ = (dragCol == 0) ? CurrentNameColWidth()
                                                : EffectiveColWidth(dragCol);
                SetCapture(hwnd_);
                return 0;
            }
        }

        // 列头点击 → 排序（只在进程页生效）
        if (tabs_.Current() == TabId::Processes) {
            int col = HitTestColumn(x, y);
            if (col >= 0) {
                pressedCol_ = col;
                if (sortCol_ == col) {
                    // 同一列：翻转方向
                    sortDesc_ = !sortDesc_;
                } else {
                    // 换列：原版的行为是新列默认降序（名称列除外，名称默认升序更自然）
                    sortCol_ = col;
                    sortDesc_ = (col != 0);
                }
                scrollY_ = 0;
                RebuildRows();
                SaveSortState();   // 列头点击 → 持久化排序状态
                InvalidateAll();
                return 0;
            }
        }

        // 性能页：左侧列表点击 + 图表切换按钮
        if (tabs_.Current() == TabId::Performance) {
            if (perfPage_.HitTestChartToggle(x, y)) {
                perfPage_.SetChart(
                    perfPage_.CurrentChart() == ChartMode::Logical
                        ? ChartMode::Overall : ChartMode::Logical);
                InvalidateAll();
                return 0;
            }
            if (perfPage_.OnClick(x, y)) {
                InvalidateAll();
                return 0;
            }
        }

        // 四个次要页面的行点击
        // 【启动页此前完全没有接这个分发】PagesView::OnClickStartup() 全工程
        // 零调用点，于是 selStartup_ 永远是 -1 → 启动项左键点不亮（没有选中高亮），
        // 右键的「启用(E)」「禁用(D)」「打开文件所在的位置」「在线搜索」「属性」
        // 五条命令全部在 `if (s < 0) return;` 静默返回 —— 这是「按钮是死的」
        // 里用户最容易撞上的一批。
        switch (tabs_.Current()) {
        case TabId::Users:
            if (pagesView_.OnClickUsers(x, y)) { InvalidateAll(); return 0; }
            break;
        case TabId::Services:
            if (pagesView_.OnClickServices(x, y)) { InvalidateAll(); return 0; }
            break;
        case TabId::Startup:
            if (pagesView_.OnClickStartup(x, y)) { InvalidateAll(); return 0; }
            break;
        default:
            break;
        }

        // 分组折叠（只在进程页生效）
        // 不加这个守卫时，在详细信息页/服务页点同一 y 坐标会翻转进程页
        // 隐藏的组状态，切回去发现组莫名折叠了。
        int gh = (tabs_.Current() == TabId::Processes)
                 ? HitTestGroupHeader(y) : -1;
        if (gh >= 0) {
            int g = static_cast<int>(rows_[gh].group);
            if (g >= 0 && g < 3) {
                groupExpanded_[g] = !groupExpanded_[g];
                RebuildRows();
                SaveSortState();   // 组折叠状态也一起存
                InvalidateAll();
            }
            return 0;
        }

        // ---- 详细信息页的行选中 ----
        // 【安全修复】详细信息页用自己的一套状态（detailRows_ / detailSelectedPid_），
        // 不再复用进程页的 rows_ / selectedRow_。之前用进程页的命中逻辑去点
        // 详细信息页，两边行结构完全不同，选中会指到错进程。
        if (tabs_.Current() == TabId::Details) {
            // 列头高必须与 tmwindow_details.cpp 的 PaintDetailsPage 完全一致。
            // 那里把列头顶到 rcContent_.top、高度用 theme::kHeaderHeight，
            // 所以行区原点 = rcContent_.top + theme::kHeaderHeight。
            // 曾经这里写死 24、而绘制侧从 +40 起，两套原点差 16px，
            // 结果点第 N 行选中第 N+1 行、点列头会选中第一行。
            const int hdrBottom = rcContent_.top + theme::kHeaderHeight;
            // 【不区分父子行】详细信息页是扁平进程列表（detailRows_ 是
            // vector<ProcessEntry*>，没有 ViewRow，也就没有 indentLevel/
            // GroupHeaderRow）。tmwindow_details.cpp 全程按 kRowHeight 画，
            // 这里继续保持 kRowHeight，两边一致才是对的。
            int row = (y - hdrBottom + detailScrollY_) / kRowHeight;
            if (y >= hdrBottom && row >= 0 &&
                row < static_cast<int>(detailRows_.size())) {
                const ProcessEntry* pe = detailRows_[row];
                detailSelectedPid_ = pe ? pe->pid : 0;
                detailSelectedRow_ = row;
                InvalidateAll();
            }
            return 0;
        }

        // 选中行
        int r = HitTestRow(y);
        if (r >= 0) {
            const ViewRow& vr = rows_[r];
            // 应用行的展开三角：点中三角就切换展开状态，不改变选中
            if (vr.kind == ViewRow::ProcessRow && vr.hasChildren) {
                int tl = 0, tr = 0;
                TreeTriangleX(vr.indentLevel, tl, tr);
                if (x >= tl && x < tr) {
                    SetAppExpanded(vr.pid, !IsAppExpanded(vr.pid));
                    RebuildRows();
                    InvalidateAll();
                    return 0;
                }
            }
            selectedRow_ = r;
            // 同时记住 pid —— 列表刷新后靠它重新定位（防止选中跑到别的进程）
            selectedPid_ = (vr.kind == ViewRow::ProcessRow) ? vr.pid : 0;
            InvalidateAll();
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        POINT pt{ x, y };
        ClientToScreen(hwnd_, &pt);

        // ============================================================
        // 列头右键：动态列名清单（打 ✓ = 该列可见，点一下切换）
        //
        // 【P0-8 接线】MenuBuilder::BuildColumnHeaderContext 此前零调用点：
        //   菜单构建函数做对了（按 visible[i] 打勾、ID 落在
        //   CMD_COL_TOGGLE_BASE..CMD_COL_TOGGLE_END 独占区间），但没有人弹它。
        //   命令侧同理无 case。现在三处补齐：本处弹菜单、tmwindow_menus.cpp
        //   的 OnMenuCommand 处理 ID、tmwindow.h/tmwindow_list.cpp 提供
        //   colVisible_ 维度并让绘制跳过隐藏列。
        //
        // 【顺序】必须排在下面的行右键逻辑**之前**：列头带和列表区是两个
        // 互斥的矩形，先判列头才能保证右键列头不会同时选中/弹出进程菜单。
        //
        // 【只对进程页】列名清单是进程页专有（其它页的列头不归这套列模型管）。
        // ============================================================
        if (tabs_.Current() == TabId::Processes &&
            y >= rcColHeader_.top && y < rcColHeader_.bottom &&
            x >= rcColHeader_.left && x < rcColHeader_.right) {
            // 光标所在列：菜单命令无参数，所以在这里存下来，
            // 供 CMD_HIDE_COLUMN 在 OnMenuCommand 里读（见 tmwindow.h 同名成员注释）。
            headerContextCol_ = HitTestColumn(x, y);

            // 列名与可见性都从唯一真值源取：
            //   列名  -> TmWindow::ColumnTitle()（实现在 tmwindow_list.cpp，
            //            直接返回那里 kCols[] 的 title，不再抄第二份字符串）
            //   可见  -> colVisible_（IsColumnVisible 内会强制名称列恒 true）
            const int n = (std::min)(TmWindow::ColumnCount(), kSortColCount);
            const wchar_t* titles[kSortColCount] = {};
            bool visible[kSortColCount] = {};
            for (int i = 0; i < n; ++i) {
                // 【索引 0（名称列）不进菜单】名称列不可隐藏，若把它列进去
                // 就会出现一个「点了什么都不会发生」的菜单项 —— 那是本项目
                // 反复栽过的「可点死项」。BuildColumnHeaderContext 会跳过
                // titles[i] == nullptr 的项，所以这里传 nullptr 让它整项消失，
                // 而 ID 仍是 CMD_COL_TOGGLE_BASE + i，其余列的索引映射不变。
                titles[i]  = (i == 0) ? nullptr : TmWindow::ColumnTitle(i);
                visible[i] = IsColumnVisible(i);
            }

            HMENU hdr = MenuBuilder::BuildColumnHeaderContext(
                titles, visible, n, MenuBuilder::ResourceValueMode(), headerContextCol_);
            if (hdr) {
                TrackPopupMenu(hdr, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
                DestroyMenu(hdr);
            }
            return 0;
        }

        // 详细信息页：右键也走自己的选中逻辑
        if (tabs_.Current() == TabId::Details) {
            // 列头高必须与 tmwindow_details.cpp 的 PaintDetailsPage 完全一致。
            // 那里把列头顶到 rcContent_.top、高度用 theme::kHeaderHeight，
            // 所以行区原点 = rcContent_.top + theme::kHeaderHeight。
            // 曾经这里写死 24、而绘制侧从 +40 起，两套原点差 16px，
            // 结果点第 N 行选中第 N+1 行、点列头会选中第一行。
            const int hdrBottom = rcContent_.top + theme::kHeaderHeight;
            // 【不区分父子行】同左键分支：详细信息页是扁平列表，无子行概念，
            // 与 tmwindow_details.cpp 的 kRowHeight 保持一致。
            int row = (y - hdrBottom + detailScrollY_) / kRowHeight;
            if (y >= hdrBottom && row >= 0 &&
                row < static_cast<int>(detailRows_.size())) {
                const ProcessEntry* pe = detailRows_[row];
                detailSelectedPid_ = pe ? pe->pid : 0;
                detailSelectedRow_ = row;
            }
            HMENU popup = MenuBuilder::BuildDetailsContext(detailSelectedPid_ != 0);
            if (popup) {
                TrackPopupMenu(popup, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
                DestroyMenu(popup);
            }
            InvalidateAll();
            return 0;
        }

        int r = HitTestRow(y);
        if (r >= 0) {
            selectedRow_ = r;
            selectedPid_ = (rows_[r].kind == ViewRow::ProcessRow) ? rows_[r].pid : 0;
        }
        bool hasSel = (selectedRow_ >= 0 && selectedRow_ < static_cast<int>(rows_.size()));
        bool isGroup = false, hasChildren = false, expanded = false;
        if (hasSel) {
            const ViewRow& vr = rows_[selectedRow_];
            isGroup = (vr.kind == ViewRow::GroupHeaderRow);
            hasChildren = vr.hasChildren;
            expanded = vr.expanded;
        }

        HMENU popup = nullptr;
        switch (tabs_.Current()) {
        case TabId::Processes:
            popup = MenuBuilder::BuildProcessContext(hasSel, isGroup, hasChildren, expanded);
            break;
        case TabId::Details:
            popup = MenuBuilder::BuildDetailsContext(hasSel);
            break;
        case TabId::Services:
            // 服务页右键分两个变体：该服务当前是否有关联进程（PID != 0）决定用哪个。
            // 无 PID 时原版只给「停止 / 打开服务 / 在线搜索」，且助记符也不同。
            {
                bool hasPid = false;
                int si = pagesView_.SelectedService();
                const auto& svc = pagesView_.ServiceItems();
                if (si >= 0 && si < static_cast<int>(svc.size())) {
                    hasPid = (svc[si].pid != 0);
                }
                popup = MenuBuilder::BuildServiceContext(hasSel, hasPid);
            }
            break;
        case TabId::Users:
            popup = MenuBuilder::BuildUserContext(hasSel, expanded);
            break;
        case TabId::Performance: {
            // 【30206 vs 30207 是两张不同的菜单资源】
            //   图表区右键 → 30206 Dashboard Context（将图形更改为 / 显示内核时间 / …）
            //   侧栏右键   → 30207 Dashboard Sidebar Context（摘要视图 / 显示图形 / 隐藏图形 / 复制）
            // 以前这里一律弹 30206，于是 30207 那三项**永远见不到**，
            // sidebarSummary / hideSidebarGraph 两个开关也就成了不可达的死项。
            // 分界线就是侧栏宽度 kSidebarWidth（include/perfpage.h:95），
            // 与 PerfPage::OnClick / OnHover 用的判据完全同一套。
            const RECT& pb = perfPage_.Bounds();
            const bool inSidebar = (pt.x >= pb.left &&
                                    pt.x < pb.left + perftheme::kSidebarWidth);
            if (inSidebar) {
                popup = MenuBuilder::BuildPerfSidebarContext(
                    MenuBuilder::PerfToggles().sidebarSummary,
                    MenuBuilder::PerfToggles().hideSidebarGraph);
            } else {
                popup = MenuBuilder::BuildPerfContext(0, false, false);
            }
            break;
        }
        // 恢复的 7 页签里的这两页，原版各自有独立右键菜单（MUI 30214 / 30213）
        case TabId::AppHistory:
            popup = MenuBuilder::BuildAppHistoryContext(hasSel);
            break;
        case TabId::Startup:
            popup = MenuBuilder::BuildStartupContext(hasSel, true);
            break;
        default:
            break;
        }
        if (popup) {
            TrackPopupMenu(popup, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
            DestroyMenu(popup);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);

        // 【精细滚动 · 2026-11-21】原来三处都是 `delta / WHEEL_DELTA` 整除。
        // 真滚轮一格给 ±120，整除没问题；但**触控板两指滑动给的是细分增量**
        // （常见 ±15、±30、±40），整除后全变成 0 —— 精细滚动被整个吃掉，
        // 表现为「触控板滑半天画面不动」。
        //
        // 改法：把余数留着累加。凑不满一格就攒着，下次一起算。
        // 这样真滚轮行为不变（120/120=1，余数 0），触控板也能按比例滚动。
        wheelRemainder_ += delta;
        const int notches = wheelRemainder_ / WHEEL_DELTA;
        wheelRemainder_ -= notches * WHEEL_DELTA;
        if (notches == 0) return 0;      // 还没凑够一格，等下一次

        if (tabs_.Current() == TabId::Details) {
            // 【不区分父子行】详细信息页行高统一 28（tmwindow_details.cpp 全程
            // kRowHeight），滚轮按 kRowHeight 走是正确的。
            detailScrollY_ -= notches * kRowHeight * 3;
            if (detailScrollY_ < 0) detailScrollY_ = 0;
        } else {
            // 四个次要页有自己的滚动状态（PagesView::scroll_）。此前滚轮落进
            // 进程页分支，改的是 scrollY_ 并全量 RebuildRows()，画面一点不动 ——
            // 白烧 CPU，还把进程页的位置悄悄改了。现在按页分发。
            switch (tabs_.Current()) {
                case TabId::Startup:
                case TabId::Users:
                case TabId::Services:
                case TabId::AppHistory:
                    pagesView_.SetCurrentTab(static_cast<int>(tabs_.Current()));
                    pagesView_.ScrollBy(-notches * 3);
                    break;
                default: {
                    // 【子行 22】进程页行高不再统一，按固定 kRowHeight×3 滚会
                    // 让"一次滚三行"在子行区变成四行。取视口首行的实际行高
                    // 作为"一行"的基准，滚动手感才和行数对齐。
                    // 注：滚轮是纯像素滚动，不需要对齐到行边界。
                    int firstRH = kRowHeight;
                    int acc = 0;
                    for (size_t i = 0; i < rows_.size(); ++i) {
                        const int rh = RowHeightFor(rows_[i].kind == ViewRow::GroupHeaderRow,
                                                    rows_[i].indentLevel);
                        if (scrollY_ < acc + rh) { firstRH = rh; break; }
                        acc += rh;
                    }
                    scrollY_ -= notches * firstRH * 3;
                    // 滚轮只改 scrollY_，行表内容不变 → 只钳制范围。
                    // （原来这里调 RebuildRows()，那是每秒几十次的全量重建 + 多次排序。）
                    ClampScroll();
                    break;
                }
            }
        }
        InvalidateAll();
        return 0;
    }

    case WM_KEYDOWN: {
        if ((GetKeyState(VK_CONTROL) & 0x8000) && wp >= '1' && wp <= '7') {
            tabs_.SetCurrent(static_cast<TabId>(wp - '1'));
            BuildLayout();
            InvalidateAll();
            RebuildMenu();
            return 0;
        }

        // 上下键：移动选中项，并让视图跟随滚动。
        //
        // 【补漏】之前上下键只改 scrollY_、不移动选中行 —— 按方向键只会滚屏，
        // 选中的进程原地不动，和原版行为完全不同（原版是按一下换一行选中）。
        if (wp == VK_DOWN || wp == VK_UP) {
            if (!rows_.empty()) {
                int cur = selectedRow_;
                int next;
                if (cur < 0) {
                    next = (wp == VK_DOWN) ? 0 : static_cast<int>(rows_.size()) - 1;
                } else {
                    next = cur + ((wp == VK_DOWN) ? 1 : -1);
                }
                if (next < 0) next = 0;
                if (next >= static_cast<int>(rows_.size())) {
                    next = static_cast<int>(rows_.size()) - 1;
                }
                // 跳过组头行（组头不能被"选中"）
                while (next > 0 && next < static_cast<int>(rows_.size()) &&
                       rows_[next].kind == ViewRow::GroupHeaderRow) {
                    next += (wp == VK_DOWN) ? 1 : -1;
                }
                if (next >= 0 && next < static_cast<int>(rows_.size()) &&
                    rows_[next].kind == ViewRow::ProcessRow) {
                    selectedRow_ = next;
                    selectedPid_ = rows_[next].pid;

                    // 视图跟随：确保选中行在可视区内
                    // 【子行 22】累加用每行实际行高；下面判定可视区时也要用
                    // **这一行**的行高（nextH），否则子行会被当成 28 高，
                    // 滚到最后一个子行时底部会被多切掉 6px。
                    int y = 0;
                    for (int i = 0; i < next; ++i) {
                        y += RowHeightFor(rows_[i].kind == ViewRow::GroupHeaderRow,
                                          rows_[i].indentLevel);
                    }
                    const int nextH = RowHeightFor(rows_[next].kind == ViewRow::GroupHeaderRow,
                                                   rows_[next].indentLevel);
                    int bodyH = rcList_.bottom - rcList_.top;
                    if (y < scrollY_) scrollY_ = y;
                    if (y + nextH > scrollY_ + bodyH) {
                        scrollY_ = y + nextH - bodyH;
                    }
                }
                InvalidateAll();
                return 0;
            }
        }

        // 【注意】Delete 由加速键表处理（原版 ACCEL #26100 第一条：
        // FVIRTKEY|FNOINVERT, VK_DELETE → 结束任务）。TranslateAcceleratorW
        // 命中后会把它翻译成 WM_COMMAND 并**吞掉原始 WM_KEYDOWN**，
        // 所以这里不能再处理一次 —— 否则一次按键会触发两遍结束任务
        // （第二次时进程已不存在，但 OnMenuCommand 仍会跑完整流程）。
        // 若将来去掉加速键表，才需要把下面这段恢复：
        //   if (wp == VK_DELETE) { OnMenuCommand(CMD_END_TASK); return 0; }

        int bodyH = rcList_.bottom - rcList_.top;
        switch (wp) {
        case VK_NEXT: scrollY_ += bodyH; break;
        case VK_PRIOR:scrollY_ -= bodyH; break;
        case VK_HOME: scrollY_ = 0; break;
        case VK_END: {
            int total = ContentHeight();
            scrollY_ = (std::max)(0, total - bodyH);
        } break;
        default: return DefWindowProcW(hwnd_, msg, wp, lp);
        }
        // 键盘滚动同样只改 scrollY_，行表内容不变 → 只需钳制，不必全量重建。
        ClampScroll();
        InvalidateAll();
        return 0;
    }

    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace tmp
