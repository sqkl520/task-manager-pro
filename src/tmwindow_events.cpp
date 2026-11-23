// tmwindow_events.cpp - TmWindow 的消息处理
#include "tmwindow.h"

#include <windowsx.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
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

    // 【简略信息】rcTabs_ 是零高矩形时不画页签栏。
    //
    // 为什么要在调用点判而不是让 TabBar::Paint 自己判：TabBar::Paint 先对
    // bounds_ 做 FillRect（零高矩形 FillRect 无害），但它会画出每个页签的
    // 边框和文字 —— 那些矩形的 top/bottom 都等于 0，画出来是一条贴着上沿的
    // 杂线。所以必须在调用点挡住。
    if (rcTabs_.bottom > rcTabs_.top) PaintTabBar(memDC_);

    switch (tabs_.Current()) {
    case TabId::Processes:
        // 【简略信息】simpleView_ 打开时 rcCards_ 压成零高矩形。
        // PaintCards 拿到零高矩形会在 cardsTop-1 处画出一条孤立横线（DrawFrame
        // 不判空），所以整段跳过。PaintCards 自己的首行是 `if (!snap_) return;`，
        // 不归本模块改，这里在调用点挡住即可。
        if (rcCards_.bottom > rcCards_.top) PaintCards(memDC_);
        // 【简略信息】rcColHeader_ 零高时同样跳过列头（见 BuildLayout 的说明：
        // 30024 的简略列表没有 TmColHeader 实例）。
        if (rcColHeader_.bottom > rcColHeader_.top) PaintColumnHeader(memDC_);
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
        const int rh = RowHeightForPx(rows_[i].kind == ViewRow::GroupHeaderRow, rows_[i].indentLevel);
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
        const int rh = RowHeightForPx(rows_[i].kind == ViewRow::GroupHeaderRow, rows_[i].indentLevel);
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
            y >= rcContent_.top + theme::Rpx(theme::kHeaderHeight) && y < rcContent_.bottom &&
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

// ============ 滚动系统 ============
//
// 四套输入、一份状态：
//   鼠标滚轮    WM_MOUSEWHEEL，delta 是 120 整数倍 → 立即滚 3 行 + 阻尼惯性
//   触控板两指  WM_MOUSEWHEEL，delta 不是 120 整数倍 → 按比例精确位移，静默后惯性
//   滚动条拖动  WM_LBUTTONDOWN/WM_MOUSEMOVE        → 直接精确位移，**无惯性**
//   触屏滑动    WM_GESTURE / GID_PAN               → 跟手精确位移，抬手带惯性
//
// 【为什么把"哪个列表"抽成 ScrollTarget】
// 三种列表的偏移量（scrollY_ / detailScrollY_ / pagesView_.scroll_）各有各的
// 单位历史和钳制口径。四套输入如果各自 switch(tabs_.Current()) 一遍，就会
// 有四份互不一致的判据 —— 本项目已经因为"绘制一个原点、命中最写一个原点"
// 踩过 16px 错位。现在输入只负责算出**像素位移**，落点统一由
// ApplyScrollDelta 处理，钳制只有一处。

TmWindow::ScrollTarget TmWindow::CurrentScrollTarget() {
    switch (tabs_.Current()) {
    case TabId::Processes:
        return ScrollTarget::ProcessList;
    case TabId::Details:
        return ScrollTarget::DetailList;
    case TabId::Startup:
    case TabId::Users:
    case TabId::Services:
    case TabId::AppHistory:
        // PagesView 自己不知道当前是哪一页 —— 它的 currentTab_ 只在绘制/点击时
        // 被同步。这里顺手同步一次，保证读写的确实是"用户看见的那张表"。
        pagesView_.SetCurrentTab(static_cast<int>(tabs_.Current()));
        return ScrollTarget::PagesTable;
    default:
        // 性能页：图表不是列表，没有滚动语义。返回 None 让所有输入
        // 安全地变成 no-op（原来滚轮会偷偷改进程页的 scrollY_，切回去才发现位置变了）。
        return ScrollTarget::None;
    }
}

int TmWindow::ScrollOffset(ScrollTarget t) const {
    switch (t) {
    case ScrollTarget::ProcessList: return scrollY_;
    case ScrollTarget::DetailList:  return detailScrollY_;
    case ScrollTarget::PagesTable:  return pagesView_.ScrollPixels();
    default:                        return 0;
    }
}

int TmWindow::MaxScrollOffset(ScrollTarget t) const {
    switch (t) {
    case ScrollTarget::ProcessList: {
        // 与 ClampScroll 同一口径（ContentHeight 内部按 RowHeightFor 逐行累加）
        const int total = ContentHeight();
        const int view = ViewportHeight();
        return (std::max)(0, total - view);
    }
    case ScrollTarget::DetailList: {
        const int total = static_cast<int>(detailRows_.size()) * theme::Rpx(kRowHeight);
        return (std::max)(0, total - DetailViewportHeight());
    }
    case ScrollTarget::PagesTable:
        return pagesView_.MaxScrollPixels();
    default:
        return 0;
    }
}

int TmWindow::ScrollViewportPx(ScrollTarget t) const {
    switch (t) {
    case ScrollTarget::ProcessList: return ViewportHeight();
    case ScrollTarget::DetailList:  return DetailViewportHeight();
    case ScrollTarget::PagesTable: {
        const RECT& b = pagesView_.Bounds();
        const int h = static_cast<int>(b.bottom - b.top) - theme::Rpx(theme::kHeaderHeight);
        return h > 0 ? h : 0;
    }
    default: return 0;
    }
}

int TmWindow::ScrollRowPx(ScrollTarget t) const {
    switch (t) {
    case ScrollTarget::ProcessList:
        // 【子行 22】进程页行高不统一，取视口首行的实际行高当"一行"，
        // 否则在子行区"滚三行"会变成四行。
        return FirstVisibleRowHeight();
    default:
        // 详细信息页与四个次要页行高统一 kRowHeight（各自的绘制全程用它）
        return theme::Rpx(kRowHeight);
    }
}

int TmWindow::FirstVisibleRowHeight() const {
    int acc = 0;
    for (size_t i = 0; i < rows_.size(); ++i) {
        const int rh = RowHeightForPx(rows_[i].kind == ViewRow::GroupHeaderRow,
                                    rows_[i].indentLevel);
        if (scrollY_ < acc + rh) return rh;
        acc += rh;
    }
    return theme::Rpx(kRowHeight);
}

int TmWindow::DetailViewportHeight() const {
    // 实现在 tmwindow_details.cpp —— 那里有 listTop/listBot 的唯一定义点。
    // 在这里照着抄一份 `rcContent_.bottom - 2 - (top + kHeaderHeight)` 是
    // 本项目反复踩过的坑（两套原点差 16px），所以只保留一个声明。
    return DetailListHeight();
}

void TmWindow::SetScrollOffset(ScrollTarget t, int v) {
    const int maxS = MaxScrollOffset(t);
    if (v < 0) v = 0;
    if (v > maxS) v = maxS;
    switch (t) {
    case ScrollTarget::ProcessList:
        if (scrollY_ == v) return;
        scrollY_ = v;
        break;
    case ScrollTarget::DetailList:
        if (detailScrollY_ == v) return;
        detailScrollY_ = v;
        break;
    case ScrollTarget::PagesTable: {
        // PagesView 内部自己钳制并清 hover_。**但重绘要在这里做** ——
        // PagesView 不是窗口，它不会自己 InvalidateRect。原来滚轮分支末尾
        // 有一次统一的 InvalidateAll，现在位移走这个函数，漏掉就会
        // "触控板滑动数据变了、屏幕一动不动"。
        const int before = pagesView_.ScrollPixels();
        pagesView_.SetScrollPixels(v);
        if (pagesView_.ScrollPixels() != before) InvalidateAll();
        return;
    }
    default:
        return;
    }
    InvalidateAll();
}

int TmWindow::ApplyScrollDelta(ScrollTarget t, int deltaPx) {
    if (t == ScrollTarget::None || deltaPx == 0) return 0;
    const int before = ScrollOffset(t);
    SetScrollOffset(t, before + deltaPx);
    return ScrollOffset(t) - before;
}

bool TmWindow::ClampScrollTarget(ScrollTarget t) {
    // 【谁在调】惯性/触控板的每一帧末尾，以及 WM_SIZE / WM_DPICHANGED
    // 之后。返回 true 表示这一帧被边界钳制过。
    //
    // 注意：ApplyScrollDelta → SetScrollOffset 内部已经在钳制了，所以
    // 这个函数不是"唯一钳制点"，而是**探测点** —— 惯性定时器用它判断
    // "这一帧是不是撞墙了"，从而决定要不要立刻清零速度。
    // 名实不符会误导后来者，故在这里写清楚。
    if (t == ScrollTarget::None) return false;
    const int before = ScrollOffset(t);
    const int maxS = MaxScrollOffset(t);
    int v = before;
    if (v < 0) v = 0;
    if (v > maxS) v = maxS;
    if (v != before) {
        SetScrollOffset(t, v);
        return true;    // 撞到边界（被钳过）
    }
    return false;
}

// ---- 速度采样 ----

void TmWindow::ResetScrollSamples() {
    scrollSampleCount_ = 0;
    scrollGesturePos_ = 0.0;
    preciseScrollFrac_ = 0.0;
    lastScrollSampleTick_ = 0;
}

void TmWindow::PushScrollSample(double pos, DWORD now) {
    // 距上次采样超过 250ms 视为"新手势"（手指中途离开过），
    // 否则上次的位移会被算进这一把的速度里，甩一把方向会错。
    if (lastScrollSampleTick_ != 0 && now - lastScrollSampleTick_ > 250) {
        scrollSampleCount_ = 0;
    }
    if (scrollSampleCount_ >= kScrollSampleMax) {
        for (int i = 1; i < kScrollSampleMax; ++i) {
            scrollSamples_[i - 1] = scrollSamples_[i];
        }
        --scrollSampleCount_;
    }
    scrollSamples_[scrollSampleCount_].t = now;
    scrollSamples_[scrollSampleCount_].pos = pos;
    ++scrollSampleCount_;
    lastScrollSampleTick_ = now;
}

// 从采样窗口估速度，单位 px/帧（正 = 向下滚）。
//
// 【为什么取跨 ≥30ms 的样本对而不是最后两点】相邻两帧的位移可能只有 1~2px，
// 定时器抖动（16ms 实际可能是 12 或 21ms）会让速度估出 ±50% 的偏差，
// 表现为"甩一下速度忽大忽小、偶尔还反向"。跨 30ms 以上取平均就稳得多。
double TmWindow::EstimateScrollVelocity(DWORD now) const {
    if (scrollSampleCount_ < 2) return 0.0;
    const ScrollSample& last = scrollSamples_[scrollSampleCount_ - 1];
    // 最后一次采样已经过去太久 → 手指早就停了，不该再甩出去
    if (last.t != 0 && now - last.t > 250) return 0.0;

    for (int i = scrollSampleCount_ - 2; i >= 0; --i) {
        const DWORD dt = last.t - scrollSamples_[i].t;
        if (dt == 0) continue;
        if (dt < 30 && i > 0) continue;   // 太近，再往前找一个更早的样本
        const double dpos = last.pos - scrollSamples_[i].pos;
        return dpos / static_cast<double>(dt) * static_cast<double>(kInertiaTickMs);
    }
    return 0.0;
}

// ---- 惯性 ----

void TmWindow::StartScrollInertia(double velocity) {
    const ScrollTarget t = CurrentScrollTarget();
    if (t == ScrollTarget::None) { StopScrollInertia(); return; }

    if (velocity > kScrollMaxVelocity)  velocity = kScrollMaxVelocity;
    if (velocity < -kScrollMaxVelocity) velocity = -kScrollMaxVelocity;

    // 速度不够就干脆不滑。否则会先启动定时器、下一帧立刻停，
    // 每甩一下白白多一次 SetTimer/KillTimer 和一次重绘。
    if (std::fabs(velocity) < kScrollStopVelocity) { StopScrollInertia(); return; }

    inertiaTarget_ = t;
    scrollVelocity_ = velocity;
    scrollFrac_ = 0.0;

    if (!inertiaTimer_) {
        inertiaTimer_ = SetTimer(hwnd_, kInertiaTimerId, kInertiaTickMs, nullptr);
        if (!inertiaTimer_) {
            // SetTimer 失败（系统定时器耗尽）：退化成"不动"，
            // 绝不能留下 velocity 非 0 而定时器不跑的状态 —— 下一次
            // 滚轮会从那个陈旧速度继续累加。
            scrollVelocity_ = 0.0;
            inertiaTarget_ = ScrollTarget::None;
        }
    }
    // 【定时器已在跑时不重置】连续触控板增量/触屏 move 会反复调本函数，
    // 每次都 KillTimer+SetTimer 会让这一帧被永久推迟（观感是"越滑越顿"）。
    // 注意滚轮那条路不一样：OnWheelDelta 每次刻意先 StopScrollInertia()
    // 再重新起速度 —— 一格就该重开一次滑行，那是产品行为，不是浪费。
}

void TmWindow::StopScrollInertia() {
    if (inertiaTimer_) {
        KillTimer(hwnd_, kInertiaTimerId);
        inertiaTimer_ = 0;
    }
    scrollVelocity_ = 0.0;
    scrollFrac_ = 0.0;
    inertiaTarget_ = ScrollTarget::None;
}

void TmWindow::OnInertiaTick() {
    if (inertiaTarget_ == ScrollTarget::None) { StopScrollInertia(); return; }

    // 页被切走（用户点了页签/菜单/自测调 ShowPage）→ 目标已经不存在，
    // 继续滑就会滑到别的列表上。立即停。
    if (inertiaTarget_ != CurrentScrollTarget()) { StopScrollInertia(); return; }

    // 速度已经低于阈值 → 直接归零并停表（用户原话的最后一条要求）
    if (std::fabs(scrollVelocity_) < kScrollStopVelocity) { StopScrollInertia(); return; }

    // 亚像素余数：速度 1.4px/帧 时，每帧真滚 1px、把 0.4 留给下一帧。
    // 若直接取整丢掉小数，末段的 0.5~1.0px/帧 会全部变成 0，滑行提前刹死。
    scrollFrac_ += scrollVelocity_;
    const int whole = static_cast<int>(scrollFrac_);
    scrollFrac_ -= static_cast<double>(whole);

    if (whole != 0) {
        const int moved = ApplyScrollDelta(inertiaTarget_, whole);
        // 每帧末尾统一钳制一次（ApplyScrollDelta 内部已钳，这里是**复检**：
        // 行表/快照变化可能让上一帧合法的位置在这一帧越界）。
        const bool clamped = ClampScrollTarget(inertiaTarget_);
        if (moved != whole || clamped) {
            // 撞到边界（一步都没走 / 只走了一部分 / 被钳过）：速度立即清零。
            // 【不能保留速度】否则下一帧还会再撞一次，画面看起来是
            // "在底部抽搐"，而且速度要衰减好几帧才归零。
            StopScrollInertia();
            return;
        }
    }

    scrollVelocity_ *= kScrollDamping;
    if (std::fabs(scrollVelocity_) < kScrollStopVelocity) {
        // 阈值到了，但这一帧可能还剩不足 1px 的余数没消费。
        // 把它一次性补齐（最多 1px），避免"差半像素没滚到底"。
        if (std::fabs(scrollFrac_) >= 0.5) {
            ApplyScrollDelta(inertiaTarget_,
                             scrollFrac_ > 0 ? 1 : -1);
        }
        StopScrollInertia();
    }
}

// ============ 四套滚动的输入层 ============
//
// 这一层的职责只有一件：把**消息**翻译成 (target, 像素位移)，然后交给
// ApplyScrollDelta / StartScrollInertia。它不碰任何列表的私有状态。

// 自测专用：直接驱动惯性帧。返回实际跑了多少帧（中途停表即提前返回）。
//
// 【为什么不 Sleep(16) 真等】一次完整滑行 40+ 帧 = 0.7 秒，自测里跑两项
// 就要 1.4 秒纯等待，且结果受定时器抖动影响。这里调的是**同一个**
// OnInertiaTick，唯一差别是不经过消息队列 —— 判据仍然落在真实状态上。
int TmWindow::RunInertiaTicksForTest(int n) {
    int ran = 0;
    for (int i = 0; i < n; ++i) {
        if (!inertiaTimer_) break;
        OnInertiaTick();
        ++ran;
    }
    return ran;
}

// ---- 鼠标滚轮 / 触控板（同一条 WM_MOUSEWHEEL，靠 delta 分叉）----
//
// 【怎么区分滚轮和触控板】
//   真滚轮：一格是 WHEEL_DELTA(120) 的整数倍 ⇒ delta % 120 == 0 且 delta != 0
//           典型值 ±120，高精度滚轮有 ±240 / ±360，老式鼠标可能发 ±40 的
//           "半格"，此时代码会走触控板分支 —— 但那条路是"按比例精确位移"，
//           对半个格也是正确行为（滚半行而不是滚 0 行），所以不会出错。
//   触控板：Windows 把两指滑动的细分增量直接塞进 WM_MOUSEWHEEL，常见
//           ±15 / ±30 / ±40，**不是** 120 的整数倍。
//   GetMessageExtraInfo 的 MI_WP_SIGNATURE(0xFF515700) 可以判"这消息来自
//   鼠标还是触控板"，但那是未公开的约定（低字节是光标 id，还要掩码），
//   而 delta 判据只用公开文档里的语义，更稳。所以选 delta。
void TmWindow::OnWheelDelta(int delta) {
    if (delta == 0) return;

    const ScrollTarget target = CurrentScrollTarget();
    if (target == ScrollTarget::None) {
        // 【性能页】没有可滚列表，但左栏（侧栏）可能有超出视口的卡片 ——
        // 分盘后 8 张磁盘卡 + 网卡 + GPU 会超过一屏，旧行为是静默丢弃。
        // 现在把滚轮转给侧栏。只有真的滚动了才重绘，避免无谓 invalidate。
        if (tabs_.Current() == TabId::Performance && perfPage_.SidebarNeedsScroll()) {
            // 一格 3 行，行高按卡片高度算；触控板按比例精确位移
            const bool realWheel = (delta % WHEEL_DELTA) == 0;
            int px;
            if (realWheel) {
                wheelRemainder_ += delta;
                const int notches = wheelRemainder_ / WHEEL_DELTA;
                wheelRemainder_ -= notches * WHEEL_DELTA;
                if (notches == 0) return;
                px = -notches * 60;      // 一格滚 60px（约一张卡的 88%）
            } else {
                px = -delta / 2;         // 触控板：精确位移
            }
            if (perfPage_.ScrollSidebar(px)) {
                InvalidateRect(hwnd_, &perfPage_.Bounds(), FALSE);
            }
            return;
        }
        return;   // 性能页且侧栏不需要滚：确实无事可做
    }

    const bool isRealWheel = (delta % WHEEL_DELTA) == 0;

    if (!isRealWheel) {
        // 触控板：直接精确位移，**不累积余数**。
        //
        // 【为什么触控板不走 wheelRemainder_ 那条老路】上一轮为了让细分增量
        // 不被整除吃掉，加了余数累积；但那是把"位移"攒成"行"再滚，
        // 结果是触控板滑动变成一跳一跳的整数行 —— 用户明确要求"不将输入
        // 映射为行偏移，而是滚动偏移"。触控板本身就是连续的像素量，
        // 应该原样按比例换算成像素直接应用。
        OnPreciseWheel(delta);
        return;
    }

    // 真滚轮：一格 = 3 行。余数累积保留（兼容 ±40 这类"半格"滚轮，
    // 以及一次消息里带 2~3 格的情况）。
    wheelRemainder_ += delta;
    const int notches = wheelRemainder_ / WHEEL_DELTA;
    wheelRemainder_ -= notches * WHEEL_DELTA;
    if (notches == 0) return;   // 还没凑够一格

    // 用户原话：收到鼠标滚动事件 → 设置同向的速度和反向的加速度。
    // 换成阻尼模型后等价于「立即位移 + 起一个同向衰减的滑行」。
    //
    // 【为什么还要"立即位移"】如果只起速度不先走，滚轮会有一帧（16ms）延迟，
    // 快速连滚时能感到"粘手"。Windows 自己的列表也是"先跟手一格，再惯性"。
    StopScrollInertia();

    const int rowPx = ScrollRowPx(target);
    const int step  = -notches * rowPx * 3;   // 负 notches = 向下滚（内容上移）
    const int moved = ApplyScrollDelta(target, step);

    // 【撞边界就不起惯性】已经在底部还继续往下滚时，位移是 0。
    // 此时若照常起惯性，定时器会每帧撞一次墙、再由 StopScrollInertia 清掉 ——
    // 表面上"没事"，实际每滚一下多一次 SetTimer+一次 invalidate，
    // 而且速度字段会被写脏（自测抓到过：到底后 inertia=1、速度非 0）。
    if (moved == 0) return;

    // 初速度：本格位移的 kScrollMaxVelocity 上限之内、约 1.5 行。
    //
    // 【怎么定的】阻尼模型下总滑行距离 = v0 / (1 - d) = v0 / 0.08 = 12.5·v0。
    // 希望"滚一格之后再滑 1.5 行"（28px×1.5=42px），于是
    //     v0 = 42 × 0.08 ≈ 3.4 px/帧
    // 衰减到 0.5 阈值要 ln(0.5/3.4)/ln(0.92) ≈ 23 帧（0.37 秒）——
    // 尾巴短、不拖沓，一格总共走 4.5 行。
    // 若按"一格初速度 = 3 行像素(84px/帧)"，滑行距离会是 84/0.08 = 1050px
    // ≈ 37 行，一格滚半屏 —— 那是甩动的手感，不是滚轮的手感，故不采用。
    //
    // 【符号必须与 step 同向】step = -notches×…，速度也要 -notches×…。
    // 写成 +notches 会让"向下滚"起的惯性往**上**滑 —— 松手后画面倒退回原处。
    const double v = static_cast<double>(-notches) * static_cast<double>(rowPx)
                     * kWheelGlideRows * (1.0 - kScrollDamping);
    StartScrollInertia(v);
}

// ---- 触控板精确位移 ----
//
// 输入单位是 WHEEL_DELTA 的比例。换算成像素：一格(120) 对应 3 行。
// 于是 delta=15 → 0.125 格 → 0.375 行 ≈ 10.5px。
//
// 【亚像素余数】delta=15 在 kRowHeight=28 下是 10.5px。若每帧取整，
// 两次 ±15 就丢掉 1px。preciseScrollFrac_ 存小数部分，攒够 1px 再滚。
void TmWindow::OnPreciseWheel(int delta) {
    const ScrollTarget target = CurrentScrollTarget();
    if (target == ScrollTarget::None) return;

    StopScrollInertia();   // 新手势必定打断旧滑行

    const int rowPx = ScrollRowPx(target);
    // 像素位移 = -delta/120 × 3行 × 行高
    const double px = -static_cast<double>(delta) / static_cast<double>(WHEEL_DELTA)
                      * 3.0 * static_cast<double>(rowPx);
    preciseScrollFrac_ += px;
    const int whole = static_cast<int>(preciseScrollFrac_);
    preciseScrollFrac_ -= static_cast<double>(whole);
    if (whole != 0) ApplyScrollDelta(target, whole);

    // 采样：用于"静默 90ms 后估速度起惯性"。
    // 【必须 += px，不是 -px】px 已经是「滚动偏移的增量」（正 = 向下滚），
    // 与 scrollY_ 同向。取反会让触控板甩动的惯性方向整个反过来 ——
    // 向上滑一把，松手后画面往下滑回去。
    scrollGesturePos_ += px;
    PushScrollSample(scrollGesturePos_, GetTickCount());

    // 每次细分增量都重置 tail 定时器 —— 只要还在滑，就不算"抬手"。
    KillTimer(hwnd_, kTouchpadTailTimerId);
    SetTimer(hwnd_, kTouchpadTailTimerId, kTouchpadTailMs, nullptr);
}

// ---- 触屏 pan ----
//
// WM_GESTURE 的 GID_PAN 给的是**手指当前位置**，不是增量，所以要自己存
// 上一帧的位置求差。begin 时只记起点、不滚（否则第一帧会跳一大截，
// 因为 ptsLocation 可能是"识别成 pan 那一刻"的位置，与按下点不同）。
void TmWindow::OnPanTo(int clientY, bool begin, bool end) {
    const ScrollTarget target = CurrentScrollTarget();
    if (target == ScrollTarget::None) { panActive_ = false; return; }

    if (begin) {
        StopScrollInertia();
        ResetScrollSamples();
        panLast_.y = clientY;
        panActive_ = true;
        PushScrollSample(scrollGesturePos_, GetTickCount());
        return;
    }
    if (!panActive_) {
        // 没收到 begin 就先收到 move（手势被系统中途回收）：
        // 把它当作起点，别按一个陈旧的 panLast_ 算出巨大位移。
        panLast_.y = clientY;
        panActive_ = true;
        return;
    }

    // 手指往上滑（clientY 变小）→ 内容上移 → 列表向下滚 → 位移为正
    const int dy = panLast_.y - clientY;
    panLast_.y = clientY;

    if (dy != 0) {
        preciseScrollFrac_ += static_cast<double>(dy);
        const int whole = static_cast<int>(preciseScrollFrac_);
        preciseScrollFrac_ -= static_cast<double>(whole);
        if (whole != 0) ApplyScrollDelta(target, whole);
        scrollGesturePos_ += static_cast<double>(dy);
        PushScrollSample(scrollGesturePos_, GetTickCount());
    }

    if (end) {
        // 抬手：用最后一段的位移估算初速度，起惯性。
        // 【为什么不在 begin 就按系统的惯性滑】GC_PAN_WITH_INERTIA 已被我们
        // 屏蔽（见 WM_GESTURENOTIFY），系统的惯性不会叠加；这里用自己测的
        // 速度，参数与滚轮/触控板共用一套阻尼，手感一致。
        const double v = EstimateScrollVelocity(GetTickCount());
        panActive_ = false;
        ResetScrollSamples();
        StartScrollInertia(v);
    }
}

// ============ 悬停状态：单一清空出口 ============
//
// 鼠标离开客户区时把**所有** HOT 状态清掉。
//
// 【为什么必须集中在一个函数里】悬停状态散在 6 个地方（页签栏 / 列头 /
// 进程页行 / 次要页行 / 性能页侧栏 / 底部工具条）。历史教训是：每加一处
// 就顺手在 WM_MOUSEMOVE 里写一句赋值，但**没有人**在 WM_MOUSELEAVE 里
// 补对应的清零 —— 于是鼠标一移出窗口，那片高亮就永久留在屏幕上。
// 项目里已有两条专门记录这个坑的注释（tmwindow.cpp 的 ShowPage 兜底、
// tmwindow_events.cpp 的 WM_MOUSEMOVE），本轮把根治点补齐。
//
// 新增任何 hover 状态时，**必须**往这里加一行，否则就是一条新的泄漏。
void TmWindow::ClearAllHover() {
    bool dirty = false;

    if (tabs_.Hover() != TabId::Count) { tabs_.SetHover(TabId::Count); dirty = true; }
    if (hoverCol_ >= 0)                { hoverCol_ = -1;               dirty = true; }
    if (hoverRow_ >= 0)                { hoverRow_ = -1;               dirty = true; }
    if (hoverToolbarCmd_ != 0)         { hoverToolbarCmd_ = 0;         dirty = true; }
    if (perfPage_.HoverItem() != -1)   { perfPage_.ClearHover();       dirty = true; }

    // PagesView 自己也有 hover_。它的 OnHover 在坐标越界时会自清，但鼠标
    // 移出窗口时根本没有 OnHover 调用 —— 所以这里显式清一次。
    if (pagesView_.ClearHover()) dirty = true;

    if (dirty) InvalidateAll();
}

// 脚本化 / 自测用的「把鼠标放到 (x,y)」。
//
// 【必须走真实消息路径，不能直接改成员】原因是「hover 到底有没有接上」
// 这件事的全部内容就是「WM_MOUSEMOVE 分支里有没有写那个状态」。
// 如果这里直接 `hoverToolbarCmd_ = HitTestToolbar(x,y)`，那么即使
// WM_MOUSEMOVE 分支整个被删掉，这个验证依然 PASS —— 那就成了假验证，
// 正是本项目反复出现的「自测全绿、功能没接」。
//
// 所以这里**只投递消息**，处理逻辑一份、不分叉。
// 传 (-1,-1) 等价于鼠标移出客户区（走 ClearAllHover，与 WM_MOUSELEAVE 同一条路）。
void TmWindow::FakeMouseMove(int x, int y) {
    if (x < 0 || y < 0) {
        ClearAllHover();
        return;
    }
    LPARAM lp = MAKELPARAM(x, y);
    // 直接调 WndProc 而不是 SendMessageW：后者在窗口 SW_HIDE 的离屏/自测
    // 模式下可能被系统丢弃（隐藏窗口仍会收消息，但走一遍消息队列没有必要，
    // 而且 SendMessage 会重入 WndProc，调试时栈不好看）。
    WndProc(WM_MOUSEMOVE, 0, lp);
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
        const int prevDpi = theme::Dpi();
        theme::SetDpi(HIWORD(wp));
        // 【必须重建字体】BuildFonts 里的 kFontSize* 是 96 DPI 基准值，
        // 现在经 Rpx() 缩放。不重建的话字体还是旧 DPI 的字号，而框已经
        // 按新 DPI 变大 —— 卡片数值的 tmHeight/框高 填充率会掉到 50%，
        // 文字挤在框的左上角。这正是 WM_DPICHANGED 之前只调 BuildLayout
        // 不够的地方。
        if (theme::Dpi() != prevDpi) {
            BuildFonts();
            pagesView_.OnDpiChanged(prevDpi);
        }
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
        // 【ALT+TAB / 点别的程序离开窗口 → 清悬停】
        //
        // 【为什么 WM_MOUSELEAVE 不够】TME_LEAVE 只在**光标离开客户区**时投递。
        // 下面这几种情况光标物理上还留在窗口矩形内，但窗口已经不是活动窗口：
        //   - Alt+Tab 切走
        //   - 点任务栏/另一个窗口且没有遮挡本窗口
        // 此时那片 hover 高亮会一直亮着，直到用户把鼠标在窗口里挪一下 ——
        // 用户看到的是「任务管理器在后台，但有个按钮一直亮着」。
        // WM_ACTIVATE 的 WA_INACTIVE 是"整个窗口掉到后台"的权威信号。
        //
        // 【为什么不用 WM_KILLFOCUS】点本程序自己的菜单也会让窗口失去键盘焦点
        // （下面的注释记了同一件事），那样每点一次菜单就闪一下悬停。
        // 本程序自己的弹窗（MessageBox / 对话框 / 托盘菜单）触发 WM_ACTIVATE
        // 时前台窗口属于本进程，可按 pid 排除 —— 但那套判断在下面几行，
        // 而悬停该在「真的离开了」时清。这里条件从简：只要 WA_INACTIVE 就清，
        // 因为即使是我们自己的对话框挡住，清掉悬停也是对的（用户看不到底下的
        // 按钮）。宁可多清一次，不留泄漏。
        if (LOWORD(wp) == WA_INACTIVE) ClearAllHover();

        // 下面这一段是「使用时最小化」，逻辑不变。
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

    case WM_MOUSEWHEEL: {
        OnWheelDelta(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    }

    // ---- 触屏手势：必须先在这里声明"我要 pan"，否则收不到 WM_GESTURE ----
    //
    // 【为什么需要这一步】系统的手势引擎默认不开 pan（只有父窗口偏好），
    // 必须先响应 WM_GESTURENOTIFY 并调用 SetGestureConfig 明确声明要哪些手势，
    // 之后同一串触摸才会被识别成 GID_PAN 并发 WM_GESTURE。
    // 直接处理 WM_GESTURE 而不处理 WM_GESTURENOTIFY 是"代码写了但永远不触发"的
    // 经典形态 —— 本项目已有过多处"画了没接"的教训。
    case WM_GESTURENOTIFY: {
        GESTURECONFIG gc{};
        gc.dwID = 0;   // 0 = 整个窗口的全局手势偏好
        // 只要双指/单指纵向 pan。**不要** GC_PAN_WITH_INERTIA：
        // 系统的惯性会和我们自己的阻尼惯性叠加，抬手后滑两倍距离。
        gc.dwWant  = GC_PAN | GC_PAN_WITH_SINGLE_FINGER_VERTICALLY;
        // 横向单指 pan 会抢走"左右拖动"的手势，缩放/旋转我们不需要。
        gc.dwBlock = GC_PAN_WITH_SINGLE_FINGER_HORIZONTALLY |
                     GC_PAN_WITH_INERTIA | GC_ZOOM | GC_ROTATE;
        gestureReady_ = SetGestureConfig(hwnd_, 0, 1, &gc, sizeof(gc)) != FALSE;
        return 0;   // 必须吃掉：文档要求处理 WM_GESTURENOTIFY 才算声明完成
    }

    case WM_GESTURE: {
        GESTUREINFO gi{};
        gi.cbSize = sizeof(gi);
        const HGESTUREINFO hgi = reinterpret_cast<HGESTUREINFO>(lp);
        if (!GetGestureInfo(hgi, &gi)) return 0;

        bool handled = false;
        if (gi.dwID == GID_PAN) {
            // ptsLocation 是**屏幕坐标**，必须换成客户区坐标 ——
            // 否则窗口不在 (0,0) 时每一帧都会掺进窗口位置造成的巨大跳变。
            POINT pt{ gi.ptsLocation.x, gi.ptsLocation.y };
            ScreenToClient(hwnd_, &pt);
            OnPanTo(pt.y,
                    (gi.dwFlags & GF_BEGIN) != 0,
                    (gi.dwFlags & GF_END) != 0);
            // 【为什么要看 gestureReady_】WM_GESTURE 看似只在 SetGestureConfig
            // 成功后才会来，但系统手势引擎也可能按**父窗口偏好**投递（文档：
            // "未在 dwWant/dwBlock 里声明的项继承父窗口设置"）。若我们声明失败
            // 却仍收到 pan，说明拿到的是别人的配置 —— 此时仍按 pan 处理是对的
            // （用户就是在滑），但记录一下，便于自测/排障判断"是不是我们的配置生效了"。
            (void)gestureReady_;
            handled = true;
        }
        // 每条 WM_GESTURE 都必须关句柄，否则系统手势资源泄漏。
        CloseGestureInfoHandle(hgi);
        if (handled) return 0;
        break;   // 其它手势交回默认处理
    }

    case WM_TIMER: {
        if (wp == 2) {
            // 服务描述渐进填充的续跑标记
            InvalidateAll();
            return 0;
        }
        if (wp == kInertiaTimerId) {
            OnInertiaTick();
            return 0;
        }
        if (wp == kTouchpadTailTimerId) {
            // 触控板"抬手"判定：系统不给我们两指抬起的消息，
            // 静默 90ms 就是事实上的结束。
            KillTimer(hwnd_, kTouchpadTailTimerId);
            const double v = EstimateScrollVelocity(GetTickCount());
            ResetScrollSamples();
            StartScrollInertia(v);
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
        if (detailScrollDragging_) {
            detailScrollDragging_ = false;
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
        detailScrollDragging_ = false;
        return 0;
    }

    // 【ALT+TAB / 点别的程序离开】必须清悬停。
    //
    // 【为什么 WM_MOUSELEAVE 不够】TME_LEAVE 只在**光标离开客户区**时投递。
    // 光标物理上还留在窗口矩形内、但窗口已经不是活动窗口的情况：
    //   - Alt+Tab 切走
    //   - 点任务栏/另一个窗口且没有遮挡本窗口
    // 此时那片 hover 高亮会一直亮着，直到用户把鼠标在窗口里挪一下 ——
    // 用户看到的是「任务管理器在后台，但有个按钮一直亮着」。
    //
    // 【不用 WM_KILLFOCUS 的原因】点本程序自己的菜单也会让窗口失去键盘焦点，
    // 那样每点一次菜单就把 hover 清一次，菜单交互期间悬停会闪。
    //
    // 【处理位置】C++ 的 switch 不允许同一个 case 值出现两次，所以本逻辑
    // 并入了下面的 WM_ACTIVATE 分支（见那里 WA_INACTIVE 的处理）。

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);

        // 【悬停泄漏的根治点】挂 TrackMouseEvent(TME_LEAVE)。
        //
        // 没有这一句时，鼠标移出窗口不会产生任何消息：WM_MOUSEMOVE 只在你
        // **在客户区里动**的时候才来，鼠标溜出窗口边界后消息就断了，于是
        // hoverRow_ / hoverCol_ / tabs_.hover_ / hoverToolbarCmd_ 全部永久
        // 保持最后一次的值 —— 用户看到的是「整行一直是浅蓝」「页签一直亮着」。
        // 项目历史上这是被反复踩到的坑（tmwindow.cpp 的 ShowPage 里有一条
        // 【交接】注释专门记了它，只能在切页时兜一层）。
        //
        // 【为什么每帧都调】Windows 的 TrackMouseEvent 是一次性的：投递
        // WM_MOUSELEAVE 之后注册即失效，再次进入客户区必须重新调用。
        // 只在 trackingLeave_ 为假时调，避免鼠标每移动 1px 就多一次 syscall
        // （WM_MOUSEMOVE 是高频消息）。
        if (!trackingLeave_) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd_;
            if (TrackMouseEvent(&tme)) trackingLeave_ = true;
        }

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

        // 拖拽滚动条（进程页）
        if (scrollDragging_) {
            const int total = ContentHeight();
            const int view = ViewportHeight();
            if (total > view && view > 0) {
                // 【几何必须与绘制同源】
                // 绘制侧 ScrollThumbRect()（tmwindow_list.cpp:2248）用的是
                //   trackTop = rcList_.top + btnH
                //   trackH   = view - btnH*2
                //   thumbH   = clamp(trackH*view/total, kThumbMinLen, trackH)
                // 拖拽侧以前自己抄了一份不同的式子（trackTop = rcList_.top、
                // trackH = view、最小 24）。实测 view=600 total=2000 时，
                // scrollY=0 抓滑块往下拖会立刻跳到 57：rel 多算了一个按钮的
                // 高度，再被 (rel/span)*maxScroll 放大。
                //
                // 现在直接从绘制结果反解：ScrollThumbRect() 已知，
                // thumbH 就是它的高度；trackH 由绘制公式反推。
                const RECT thm = ScrollThumbRect();
                const int btnH = theme::Rpx(17);          // kScrollBtnH（SCROLLBAR 主题实测）
                const int trackTop = rcList_.top + btnH;
                const int trackH = view - btnH * 2;
                const int drewThumbH = thm.bottom - thm.top;
                int thumbH = drewThumbH > 0 ? drewThumbH : theme::Rpx(11);
                if (thumbH > trackH) thumbH = trackH;

                const int maxScroll = total - view;
                const int span = trackH - thumbH;
                if (span > 0) {
                    const int rel = y - scrollDragOffset_ - trackTop;
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

        // 拖拽滚动条（详细信息页）。与进程页同样是直接精确位移，**无惯性**。
        if (detailScrollDragging_) {
            const int viewH = DetailViewportHeight();
            const int contentH = static_cast<int>(detailRows_.size()) * theme::Rpx(kRowHeight);
            const int maxScroll = contentH - viewH;
            const int trackTop = static_cast<int>(rcContent_.top) + theme::Rpx(theme::kHeaderHeight) +
                                 16;   // kDetScrollBtnH，见 tmwindow_details.cpp
            const int trackH = viewH - theme::Rpx(16) * 2;
            if (maxScroll > 0 && trackH > 0) {
                int thumbH = trackH * viewH / contentH;
                if (thumbH < theme::Rpx(24)) thumbH = theme::Rpx(24);
                if (thumbH > trackH) thumbH = trackH;
                const int span = trackH - thumbH;
                if (span > 0) {
                    const int rel = y - detailScrollDragOffset_ - trackTop;
                    int sy = rel * maxScroll / span;
                    if (sy < 0) sy = 0;
                    if (sy > maxScroll) sy = maxScroll;
                    if (sy != detailScrollY_) {
                        detailScrollY_ = sy;
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

        // 性能页侧栏卡片悬停。
        // 【此前是「变量存在、绘制读了、但没人写」**没有**这回事】**：
        //   PerfPage::OnHover 是有的、PaintSidebar 也读了 hoverItem_ 并用
        //   kSidebarHover 填色，唯独 TmWindow 从来没调过 OnHover ——
        //   于是 hoverItem_ 永远是 -1，侧栏卡片**永远不亮**。
        //   与「pagesView_.OnHover 不存在」是同一类缺陷（漏了调用点）。
        if (tabs_.Current() == TabId::Performance) {
            const int before = perfPage_.HoverItem();
            perfPage_.OnHover(x, y);
            if (perfPage_.HoverItem() != before) InvalidateAll();
        }

        // 底部工具条按钮悬停。
        // 【本轮新增】此前 toolBar 完全没有 hover 状态：HitTestToolbar 只被
        // 点击路径和光标路径调用，绘制侧从不查鼠标位置，所以「简略信息(D)」
        // 「结束任务(E)」这些按钮点下去有反应、悬停却毫无反馈。
        // 存 cmd 而不是下标：按钮表每帧重建，下标会跨页指错（见头文件注释）。
        {
            const UINT cmd = HitTestToolbar(x, y);
            if (cmd != hoverToolbarCmd_) {
                hoverToolbarCmd_ = cmd;
                InvalidateAll();
            }
        }

        // 光标：与 WM_SETCURSOR 走同一个函数，避免两处判据漂移
        ApplyHoverCursor(x, y);
        return 0;
    }

    // 【悬停泄漏的根治点之二】鼠标离开客户区：清空**所有** hover 状态。
    //
    // 少了这个分支，TrackMouseEvent 也只是让系统知道"我要通知"而已，
    // 状态照样留在最后一帧的值上。凡新增 hover 状态都必须走 ClearAllHover()，
    // 不要在这里逐字段手写 —— 漏一个就是一条泄漏。
    case WM_MOUSELEAVE: {
        trackingLeave_ = false;   // 已投递，下次进入客户区要重新挂
        ClearAllHover();
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

        // ---- 详细信息页的滚动条 ----
        //
        // 【补漏】PaintDetailsPage 早就画了滑块和两端箭头（见那边 383~420 行），
        // 但 TMWindow 里没有任何人做命中测试 —— 画了拖不动，是"画了没接"的
        // 第三种形态。现在补齐：拖滑块 / 点箭头 / 点轨道翻页三条路径。
        // 拖动是**直接精确位移**，与进程页一致，绝不加惯性。
        if (tabs_.Current() == TabId::Details) {
            RECT th = DetailScrollThumbRect();
            const int btnH = theme::Rpx(16);   // kDetScrollBtnH，见 tmwindow_details.cpp
            const int listTop = static_cast<int>(rcContent_.top) + theme::Rpx(theme::kHeaderHeight);
            const int listBot = static_cast<int>(rcContent_.bottom) - theme::Rpx(2);
            const int sbRight = static_cast<int>(rcContent_.right) - theme::Rpx(3);
            const int sbLeft = sbRight - DetailScrollBarWidth();
            const bool inSb = (x >= sbLeft && x < sbRight &&
                               y >= listTop && y < listBot);

            if (th.right > th.left &&
                x >= th.left && x < th.right && y >= th.top && y < th.bottom) {
                // 滑块本身（必须排在箭头判定之前，否则压住箭头的 1px 会点错）
                detailScrollDragging_ = true;
                detailScrollDragOffset_ = y - th.top;
                StopScrollInertia();
                SetCapture(hwnd_);
                return 0;
            }
            if (inSb && th.right > th.left) {
                if (y >= listTop && y < listTop + btnH) {
                    // 上箭头：上一行。行高统一 kRowHeight。
                    StopScrollInertia();
                    SetScrollOffset(ScrollTarget::DetailList, detailScrollY_ - theme::Rpx(kRowHeight));
                    return 0;
                }
                if (y >= listBot - btnH && y < listBot) {
                    StopScrollInertia();
                    SetScrollOffset(ScrollTarget::DetailList, detailScrollY_ + theme::Rpx(kRowHeight));
                    return 0;
                }
                // 轨道空白：翻一页
                StopScrollInertia();
                const int page = DetailViewportHeight();
                SetScrollOffset(ScrollTarget::DetailList,
                                detailScrollY_ + ((y < th.top) ? -page : page));
                return 0;
            }
        }

        // 滚动条滑块：开始拖拽
        {
            RECT th = ScrollThumbRect();
            if (th.right > th.left &&
                x >= th.left && x < th.right && y >= th.top && y < th.bottom) {
                scrollDragging_ = true;
                scrollDragOffset_ = y - th.top;
                // 拖滚动条 = 用户接管了位置，必须立刻打断惯性，
                // 否则滑行会和手指争夺 scrollY_，滑块会来回抖。
                StopScrollInertia();
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
                    // 点箭头 = 用户明确要精确走一行。必须停掉惯性，
                    // 否则每一帧的滑行都会把这一行覆盖掉。
                    StopScrollInertia();
                    int rhCur = 0;                       // 0 = 没有可滚的行
                    int acc = 0;
                    for (size_t i = 0; i < rows_.size(); ++i) {
                        const int rh = RowHeightForPx(rows_[i].kind == ViewRow::GroupHeaderRow,
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
                    StopScrollInertia();
                    int rhCur = 0;
                    int acc = 0;
                    for (size_t i = 0; i < rows_.size(); ++i) {
                        const int rh = RowHeightForPx(rows_[i].kind == ViewRow::GroupHeaderRow,
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
                StopScrollInertia();
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
            // 切页必须打断惯性：滑行目标是旧页，继续跑会滑到新页的列表上
            // （OnInertiaTick 里也有同样的守卫，两处都要有 —— 这里拦住的是
            // "切页后新页还没画就又开始滑"的时序）。
            StopScrollInertia();
            ResetScrollSamples();
            panActive_ = false;
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

        // 表头点击排序（四个次要页）。必须排在行点击之前 ——
        // 表头带是独立区域，落在那里就不该走行选中逻辑。
        if (pagesView_.HitHeaderAt(x, y) >= 0) {
            if (pagesView_.ClickHeader(pagesView_.HitHeaderAt(x, y))) {
                InvalidateAll();
            }
            return 0;
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
            const int hdrBottom = rcContent_.top + theme::Rpx(theme::kHeaderHeight);
            // 【不区分父子行】详细信息页是扁平进程列表（detailRows_ 是
            // vector<ProcessEntry*>，没有 ViewRow，也就没有 indentLevel/
            // GroupHeaderRow）。tmwindow_details.cpp 全程按 kRowHeight 画，
            // 这里继续保持 kRowHeight，两边一致才是对的。
            int row = (y - hdrBottom + detailScrollY_) / theme::Rpx(kRowHeight);
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
            const int hdrBottom = rcContent_.top + theme::Rpx(theme::kHeaderHeight);
            // 【不区分父子行】同左键分支：详细信息页是扁平列表，无子行概念，
            // 与 tmwindow_details.cpp 的 kRowHeight 保持一致。
            int row = (y - hdrBottom + detailScrollY_) / theme::Rpx(kRowHeight);
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

    // 【注意】WM_MOUSEWHEEL 分支在文件上半部分（滚动系统那一节之后），
    // 这里**不能**再有一个 —— 同一 switch 里重复 case 值直接编译错误
    // （C2196: case 值 522 已使用）。本轮之前这里残留过一个只调
    // OnWheelDelta 的重复分支，已删除。

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
                        y += RowHeightForPx(rows_[i].kind == ViewRow::GroupHeaderRow,
                                          rows_[i].indentLevel);
                    }
                    const int nextH = RowHeightForPx(rows_[next].kind == ViewRow::GroupHeaderRow,
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

        // 【必须按页签分派】scrollY_ 是**进程页**的滚动状态。没有守卫时，
        // 在详细信息页 / 次要页按 PageDown 会偷偷改进程页位置 —— 切回去
        // 才发现列表跳了一段。这与滚轮那个 bug 是同一类的下一半：
        // 滚轮已经走 CurrentScrollTarget() 分派，键盘没有。
        if (tabs_.Current() != TabId::Processes) {
            // 非进程页：交给各自的滚动目标处理，语义与滚轮一致
            const ScrollTarget tgt = CurrentScrollTarget();
            const int rowPx = ScrollRowPx(tgt);
            const int pagePx = (rcList_.bottom - rcList_.top) / (rowPx > 0 ? rowPx : 1) * rowPx;
            switch (wp) {
            case VK_NEXT:  ApplyScrollDelta(tgt, pagePx);  ClampScrollTarget(tgt); InvalidateAll(); return 0;
            case VK_PRIOR: ApplyScrollDelta(tgt, -pagePx); ClampScrollTarget(tgt); InvalidateAll(); return 0;
            case VK_HOME:  SetScrollOffset(tgt, 0);        InvalidateAll(); return 0;
            case VK_END:   SetScrollOffset(tgt, MaxScrollOffset(tgt)); InvalidateAll(); return 0;
            default: break;
            }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        }

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
