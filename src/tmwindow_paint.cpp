// tmwindow_paint.cpp - TmWindow 的绘制实现（第二部分）
// 从 tmwindow.cpp 拆分，便于维护。所有函数都在 tmp 命名空间内。
#include "tmwindow.h"

#include <windowsx.h>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <vector>

#pragma comment(lib, "gdi32.lib")

namespace tmp {

using namespace theme;

// ---------- 本文件的 DPI 约定 ----------
//
// 【为什么一律写 theme::Rpx() 而不是裸 Rpx()】
//   本文件有 `using namespace theme;`，而 include/perfpage.h 在 namespace tmp 里
//   也定义了 `inline int Rpx(int)`。裸写 `Rpx(...)` 会因 using-directive 把
//   theme::Rpx 注入 tmp 而**二义性编译失败**（C2668: 对重载函数的调用不明确）。
//   实测复现过（scratch/probe_rpx.cpp），theme.h 的注释也警告过。
//   不要「顺手」去掉限定，也不要改 perfpage.h（不归本文件）。
//
// 【哪些数字缩放、哪些不缩放】
//   缩放：theme.h 的 96 DPI 基准常量（kCardGap / kCardPadding / kCardMinWidth）
//         在本文件**使用处**包 Rpx()；本文件内的局部行内偏移（卡片三段文字的
//         分段边界、图表留白、菜单位置）同样按基准值包 Rpx()。
//   不缩放：1px 描边（CreatePen 宽度、DrawFrame 的 ±1）、
//         DrawMiniChart 的 `w-1` / `h-2` / `rc.bottom-1`（相对矩形边界的取整修正，
//         与 DPI 无关）。逐条理由写在各处。

// ============ 绘制：菜单栏 ============

// 【本函数当前不进绘制路径】rcMenu_ 在 BuildLayout 里被压成零高矩形
// （`RECT{ 0, 0, W, 0 }`，见 tmwindow.cpp:185 / :248），而全项目对
// PaintMenuBar 的调用点数为 0。保留它是为了不破坏接口；下面的几何
// 仍然按 DPI 处理好，避免将来接回时留一个 96 DPI-only 的坑。
void TmWindow::PaintMenuBar(HDC hdc) {
    FillSolid(hdc, rcMenu_, kBgWindow);
    // -1 是物理像素修正（画在最后一行上），**不缩放**。
    DrawHLine(hdc, 0, rcMenu_.right, rcMenu_.bottom - 1, kLineNavBorder);

    HFONT old = static_cast<HFONT>(SelectObject(hdc, fontBody_));
    SetBkMode(hdc, TRANSPARENT);

    // 菜单位置/宽度是 96 DPI 基准值，逐个走 Rpx()。
    // 【注意】这三个 x/w 是「绝对左沿 + 宽度」的硬编码对，x(n+1) != x(n)+w(n)
    // 的缝隙是靠 w 与 x 的差吸收的（8+64=72 < 76，留 4px）。缩放后缝也会跟着
    // 缩放，比例关系保持不变。
    struct Item { const wchar_t* text; int x; int w; };
    Item items[] = {
        { L"文件(F)", theme::Rpx(8),   theme::Rpx(64) },
        { L"选项(O)", theme::Rpx(76),  theme::Rpx(66) },
        { L"查看(V)", theme::Rpx(146), theme::Rpx(66) },
    };
    for (auto& it : items) {
        RECT r{ it.x, 0, it.x + it.w, rcMenu_.bottom - 1 };
        DrawTextClipped(hdc, it.text, &r, DT_SINGLELINE | DT_VCENTER | DT_LEFT, kTextPrimary);
    }
    SelectObject(hdc, old);
}

// ============ 绘制：顶部标签栏 ============

void TmWindow::PaintTabBar(HDC hdc) {
    tabs_.Paint(hdc);
}

// Win10 任务管理器在内容区顶部没有独立的大标题——标签栏已经表明当前页。
// 这里保留一个空实现，仅为接口完整性（其他地方仍在调用）。
void TmWindow::PaintPageHeader(HDC hdc) {
    (void)hdc;
}

// ============ 绘制：迷你曲线 ============

void TmWindow::DrawMiniChart(HDC hdc, const RECT& rc, const double* data, int count, int head) {
    if (count < 2) return;
    const int n = ChartHistory::kPoints;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    // 最小可绘制尺寸。4 是 96 DPI 基准值：低于这个尺寸折线会退化成几段无法辨认的
    // 短划。委托方 rc 已经是当前 DPI 的像素，所以门限也要跟着放大，否则 150%
    // 下会在一个「物理 6px」的小矩形里硬画。
    const int minSide = theme::Rpx(4);
    if (w < minSide || h < minSide) return;

    double mx = 100.0;
    for (int i = 0; i < count; ++i) {
        int idx = (head - count + i + n * 2) % n;
        if (data[idx] > mx) mx = data[idx];
    }

    // 【-1 / -2 不缩放】下面三处是**像素栅格修正**，不是布局量：
    //   `w - 1`  —— 最后一个采样点落在 rc.right-1，避免画到矩形外一列
    //   `h - 2`  —— 曲线留出 1px 顶部余量（GDI 折线以中心画，会上下各溢半像素）
    //   `rc.bottom - 1` —— 填充多边形的底边
    // 它们描述的是「相对矩形边界的 ±1」，与 DPI 无关；乘 Scaled() 后
    // 125% 下 `w - 1` 变成 `w - 1`（MulDiv(1,120,96)=1 恰好不动），
    // 150% 下会变成 2，反而把曲线从边上推开。
    std::vector<POINT> pts;
    pts.reserve(count);
    for (int i = 0; i < count; ++i) {
        int idx = (head - count + i + n * 2) % n;
        double v = data[idx] / mx;
        if (v > 1.0) v = 1.0;
        if (v < 0) v = 0;
        int px = rc.left + (count > 1 ? (i * (w - 1)) / (count - 1) : 0);
        int py = rc.bottom - 1 - static_cast<int>(v * (h - 2));
        pts.push_back(POINT{ px, py });
    }

    if (!pts.empty()) {
        std::vector<POINT> poly = pts;
        poly.push_back(POINT{ pts.back().x, rc.bottom - 1 });
        poly.push_back(POINT{ pts.front().x, rc.bottom - 1 });
        HBRUSH fb = CreateSolidBrush(kChartFill);
        HGDIOBJ ob = SelectObject(hdc, fb);
        HGDIOBJ op = SelectObject(hdc, GetStockObject(NULL_PEN));
        Polygon(hdc, poly.data(), static_cast<int>(poly.size()));
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        DeleteObject(fb);
    }

    // 画笔宽度 1：折线是最细可见线，**不缩放**。缩放到 2px（200% 时）会让
    // 迷你曲线明显变粗，而「原版在高 DPI 下是否加粗」没有证据 —— 见报告
    // §「明确不缩放」第 2 条。边界 1px 描边同此口径。
    HPEN pen = CreatePen(PS_SOLID, 1, kChartLineCpu);
    HGDIOBJ old = SelectObject(hdc, pen);
    Polyline(hdc, pts.data(), static_cast<int>(pts.size()));
    SelectObject(hdc, old);
    DeleteObject(pen);
}

// ============ 绘制：汇总卡片 ============

// ⚠️【本函数已不会再被调用 —— 保留不删，仅作说明，勿当成活代码】
//
// 背景：真原版 Win10 任务管理器**进程页没有摘要卡片区**（`_ORIGINAL_VS_OURS.md`
// §二；复扫 `docs/reference/original_win10.png` y=79..91 共 13 行非白像素数 = 0）。
// 因此 `src/tmwindow.cpp` 的 `BuildLayout` 已把 `rcCards_` 压成**零高矩形**
// （`bottom == top`，见该文件 :220-221），于是：
//
//   src/tmwindow_events.cpp:40   `if (rcCards_.bottom > rcCards_.top) PaintCards(...)`
//                                → 条件恒假，本函数永不进入
//   本函数 :135                  `if (rcCards_.bottom <= rcCards_.top) return;`
//                                → 第二道保险，即使被直接调用也立刻返回
//
// 【本函数没有副作用 —— 已逐行核对，删除它不会丢状态】
//   - 只读成员：snap_ / rcCards_ / hist_ / perfSnap_（经 SampleCardMetrics）
//   - 唯一写操作是 GDI 绘制（FillSolid/DrawFrame/DrawTextClipped/DrawMiniChart）
//   - **不修改任何成员**、不清缓存、不推进任何索引
//   → 删掉它不会造成「某个状态没人初始化」这类连锁问题。
//
// 【但它留下了两条「算了没人读」的链，需要主线程决定是否一起清理】
//   1. `TmWindow::hist_`（ChartHistory）：唯一的读者就是本函数 →
//      DrawMiniChart（:237）。本函数死后 hist_ 变成**只写不读**。
//      而写入发生在 `src/tmwindow.cpp:390-392` 的 `hist_.Push(...)`（每秒一次，
//      每次算一遍全进程 CPU 合计 + 一次 SampleCardMetrics）。**这是白算。**
//   2. `TmWindow::SampleCardMetrics()`：两个调用点，本函数（:136）与
//      `src/tmwindow.cpp:391`（供 hist_.Push）。后者是为了喂 hist_，
//      而 hist_ 又没有读者了 → 该函数整体也可能一并变成死代码。
//      **注意**：它同时是 `CardMetrics` 结构唯一的消费者，
//      删它要连 `include/tmwindow.h:80-85` 的声明一起删（跨文件，不归本轮）。
//
//   这两条**不影响正确性**，只是每秒浪费一点 CPU。本轮按任务书要求
//   「保留函数、不连锁破坏」处理，只在报告里记录。详见
//   `docs/specs/II_colors.md` §「卡片区删除的影响分析」。
void TmWindow::PaintCards(HDC hdc) {
    // 简略信息模式下 rcCards_ 是零高矩形（BuildLayout 里 simpleView_ ? 0 : kCardHeight）。
    // 调用点已经挡了一道，这里再自设一道，避免零高填充画出孤立的 1px 横线。
    if (rcCards_.bottom <= rcCards_.top) return;
    if (!snap_) return;

    // 【排除 PID 0】空闲进程的 CPU 时间 = 系统空闲时间，计入会让卡片恒显示 90%+。
    double cpuTotal = 0.0;
    for (const auto& e : snap_->processes) {
        if (e.pid == 0) continue;
        cpuTotal += e.cpuPercent;
    }
    if (cpuTotal > 100.0) cpuTotal = 100.0;

    double memUsedGB = 0, memTotalGB = 0, memPct = 0;
    if (snap_->totalPhysical > 0) {
        uint64_t used = snap_->totalPhysical - snap_->availablePhysical;
        memUsedGB  = used / 1073741824.0;
        memTotalGB = snap_->totalPhysical / 1073741824.0;
        memPct     = static_cast<double>(used) / static_cast<double>(snap_->totalPhysical) * 100.0;
    }

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const unsigned nCpu = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;

    struct Card {
        const wchar_t* title;
        wchar_t value[64];
        wchar_t sub[64];
        const double* data;
    };

    Card cards[5]{};
    cards[0].title = L"CPU";
    swprintf_s(cards[0].value, L"%.0f%%", cpuTotal);
    swprintf_s(cards[0].sub, L"%u 核", nCpu);
    cards[0].data = hist_.cpu;
    // 内存卡片：主数值改用"已用 GB"单值，副标题放总量与百分比，
    // 避免 "25.8/31.9 GB" 在窄卡片里被截断（原版几个数字是分行排的）
    cards[1].title = L"内存";
    swprintf_s(cards[1].value, L"%.1f GB", memUsedGB);
    swprintf_s(cards[1].sub, L"%.0f%%  %.1f GB", memPct, memTotalGB);
    cards[1].data = hist_.mem;

    // 磁盘 / 网络 / GPU 三张卡片：数值来自 SampleCardMetrics()，与迷你曲线
    // 用同一份口径（curve 侧 hist_.Push 在 tmwindow.cpp 里也调它）。
    // 此前这三行硬编码 "0%"，卡片永远显示 0、曲线永远贴底 —— 5 张卡里 3 张是假的。
    CardMetrics cm = SampleCardMetrics();

    cards[2].title = L"磁盘";
    swprintf_s(cards[2].value, L"%.0f%%", cm.diskPct);
    cards[2].sub[0] = 0;
    cards[2].data = hist_.disk;

    cards[3].title = L"网络";
    swprintf_s(cards[3].value, L"%.0f%%", cm.netPct);
    cards[3].sub[0] = 0;
    cards[3].data = hist_.net;

    cards[4].title = L"GPU";
    swprintf_s(cards[4].value, L"%.0f%%", cm.gpuPct);
    cards[4].sub[0] = 0;
    cards[4].data = hist_.gpu;

    // 【DPI —— 卡片区的三个来源要分清】
    //   kCardGap / kCardMinWidth / kCardPadding 是 theme.h 的 96 DPI 基准常量，
    //     在本文件**使用处**换算（theme.h 明确要求「在使用处包 theme::Scaled()」，
    //     不能改常量本身 —— 那会连带影响 tmwindow.cpp 的 BuildLayout 与 menus.cpp）。
    //   下面 5 / 21 / 49 / 3 / 10 / 40 / 8 七个数是本文件内的**局部**行内偏移，
    //     没有共享者，同样按 96 DPI 基准处理。
    //     ⚠ 其中 21 / 49 不是字号，是三个文字框的分段边界：
    //       title 5..21、value 21..49、sub 49..bottom-3。
    //       它们**必须**与 fontCardTitle_/Value_/Sub_ 的 lfHeight 同步放大 ——
    //       只放大框不放大字，文字在框里的相对高度会塌下去（96 DPI 下
    //       fontCardValue_ 的 tmHeight≈30 占 value 框 28px 的 107%，本来就略裁；
    //       150% 下若框放大到 42px 而字仍是 21px 的 30px 高，比例降到 71%，
    //       观感变成"小字躺在大框里"，这与用户投诉的"高分屏怪怪的"是同一类问题）。
    //       字体缩放不在本文件 —— 见 docs/audit/B02_paint_details_dpi.md §2。
    const int cardGap  = theme::Rpx(kCardGap);
    const int cardPad  = theme::Rpx(kCardPadding);
    const int totalW = rcCards_.right - rcCards_.left;
    const int nCards = 5;
    int cardW = (totalW - cardGap * (nCards - 1)) / nCards;
    if (cardW < theme::Rpx(kCardMinWidth)) cardW = theme::Rpx(kCardMinWidth);

    for (int i = 0; i < nCards; ++i) {
        int x = rcCards_.left + i * (cardW + cardGap);
        RECT card{ x, rcCards_.top, x + cardW, rcCards_.bottom };

        FillSolid(hdc, card, kBgCard);
        // DrawFrame 内部是 rc.bottom-1 / rc.right-1 的 1px 描边，不缩放。
        DrawFrame(hdc, card, kLineCard);

        RECT tr{ card.left + cardPad, card.top + theme::Rpx(5),
                 card.right - cardPad, card.top + theme::Rpx(21) };
        SelectObject(hdc, fontCardTitle_);
        DrawTextClipped(hdc, cards[i].title, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT,
                        kTextSecondary);

        SelectObject(hdc, fontCardValue_);
        RECT vr{ card.left + cardPad, card.top + theme::Rpx(21),
                 card.right - cardPad, card.top + theme::Rpx(49) };
        // 规格书：主数值随负载变色（CPU 阈值 50% 橙 / 90% 红；内存阈值 80%）
        COLORREF vcolor = kTextPrimary;
        if (i == 0) {          // CPU
            if (cpuTotal >= 90.0)      vcolor = RGB(0xC4, 0x2B, 0x1C);
            else if (cpuTotal >= 50.0) vcolor = RGB(0xB8, 0x5C, 0x00);
        } else if (i == 1) {   // 内存
            if (memPct >= 80.0) vcolor = RGB(0xB8, 0x5C, 0x00);
        }
        DrawTextClipped(hdc, cards[i].value, &vr, DT_SINGLELINE | DT_VCENTER | DT_LEFT,
                        vcolor);

        SelectObject(hdc, fontCardSub_);
        RECT sr{ card.left + cardPad, card.top + theme::Rpx(49),
                 card.right - cardPad, card.bottom - theme::Rpx(3) };
        DrawTextClipped(hdc, cards[i].sub, &sr, DT_SINGLELINE | DT_VCENTER | DT_LEFT,
                        kTextSecondary);

        // 右侧迷你曲线：占卡片右半部，与文字区上下留白
        int chartW = (card.right - card.left) / 2 - cardPad;
        if (chartW < theme::Rpx(40)) chartW = theme::Rpx(40);
        RECT cr{ card.right - cardPad - chartW, card.top + theme::Rpx(10),
                 card.right - cardPad, card.bottom - theme::Rpx(10) };
        // 8 是「矩形太小就别画」的门限，与 DrawMiniChart 内部 minSide 同一口径。
        const int minChart = theme::Rpx(8);
        if (cr.right - cr.left > minChart && cr.bottom - cr.top > minChart) {
            DrawMiniChart(hdc, cr, cards[i].data, hist_.count, hist_.head);
        }
    }
}

} // namespace tmp
