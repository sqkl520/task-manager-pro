// tmwindow.cpp - 主窗口实现（完整仿照 Windows 任务管理器视觉）
#include "tmwindow.h"

#include <windowsx.h>
#include <shellapi.h>
#include <algorithm>
#include <cstdio>
#include <cmath>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

namespace tmp {


using namespace theme;

// theme::Dpi() 的存储。0 = 未初始化，首次调用时探测。
// WM_DPICHANGED 到达时由 TmWindow 的窗口过程调 theme::SetDpi() 更新。
// 放在这里而不是头文件里 inline，是为了全项目只有一份定义。
namespace theme { int g_dpiCache = 0; }

// ============ 自然序比较（Windows 资源管理器的排序规则） ============
//
// Windows 任务管理器按「显示名称」排序，规则和资源管理器一致：
//   - 英文忽略大小写（claude 和 Claude 相邻）
//   - 数字按数值大小（Item2 < Item10，不是字母序 Item10 < Item2）
//   - 中文按系统 locale 的拼音序（用 CompareStringEx + SORT_DIGITSASNUMBERS）
//
// 这正是 Explorer 用的：CompareStringEx 配 LINGUISTIC_IGNORECASE 和
// SORT_DIGITSASNUMBERS 标志。
static int NaturalCompare(const wchar_t* a, const wchar_t* b) {
    int r = CompareStringEx(LOCALE_NAME_USER_DEFAULT,
                            LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS,
                            a, -1, b, -1, nullptr, nullptr, 0);
    // CompareStringEx 返回 1(a<b) / 2(a==b) / 3(a>b)，转成 -1/0/+1
    if (r == 0) {
        // 失败时退回 _wcsicmp
        return _wcsicmp(a, b);
    }
    return r - 2;
}

// ============ 数值格式化 ============
//
// 【已删除】FmtCpu / FmtMem / FmtDisk / FmtPercentInt 四个函数（原 41-71 行）。
// 它们全项目零调用，而真正在用的是另外两套：
//   src/tmwindow_list.cpp    FmtCpuText / FmtMemText / FmtDiskText / FmtNetText / FmtGpuText
//   src/tmwindow_details.cpp FmtCpuD / FmtMemD
// 删掉以免「同一概念三份定义、语义还分裂」（FmtDisk 收 MB/s、FmtDiskText 收 字节/秒）。

// ============ ChartHistory ============

void TmWindow::ChartHistory::Push(double c, double m, double d, double n, double g) {
    cpu[head] = c; mem[head] = m; disk[head] = d; net[head] = n; gpu[head] = g;
    head = (head + 1) % kPoints;
    if (count < kPoints) ++count;
}

// ============ 构造/析构 ============

TmWindow::TmWindow() {}
TmWindow::~TmWindow() {
    // 惯性定时器必须显式 kill：窗口句柄在下面还会用（RemoveTrayIcon 等），
    // 但不保险 —— 定时器随窗口销毁才失效，而本对象可能先于窗口析构。
    StopScrollInertia();
    if (hwnd_) KillTimer(hwnd_, kTouchpadTailTimerId);
    icons_.Stop();
    // 托盘图标必须先撤，否则退出后任务栏会留一个"幽灵"图标
    RemoveTrayIcon();
    // 加速键表是我方 CreateAcceleratorTableW 建的，谁建谁销毁
    if (accel_) { DestroyAcceleratorTable(accel_); accel_ = nullptr; }
    auto del = [](HFONT& f) { if (f) { DeleteObject(f); f = nullptr; } };
    del(fontBody_); del(fontBold_); del(fontNav_); del(fontCardTitle_);
    del(fontCardValue_); del(fontCardSub_); del(fontGroup_);
    // 【顺序】位图还被选在 memDC_ 里时 DeleteObject 会失败。
    // 用 CreateCompatibleDC 时 DC 原有的 stock 位图（memOldBmp_）把它换出来，
    // 与 EnsureBackBuffer 同一套做法（见那里的长注释）。
    if (memDC_ && memBmp_) {
        if (memOldBmp_) SelectObject(memDC_, memOldBmp_);
        DeleteObject(memBmp_);
        memBmp_ = nullptr;
    }
    if (memDC_) DeleteDC(memDC_);
    memOldBmp_ = nullptr;
    // 【释放顺序】缓存对象必须在所有 DC 都销毁之后再删。
    ReleaseGdiCaches();
}

// 字体族见 theme.h 的 MakeUiFont：外壳字体 Microsoft YaHei UI 优先，
// 取不到才退系统 message font。三个 BuildFonts（本文件 / perfpage / pages_view）
// 应当共用这一份，否则改一处主题另外六个页面纹丝不动。
void TmWindow::BuildFonts() {
    auto mk = [](int px, int weight) -> HFONT { return MakeUiFont(px, weight); };

    fontBody_      = mk(kFontSizeBody, FW_NORMAL);
    // 列头字重 = 常规，不是半粗。
    // 【依据：以「没有依据」为结论 —— 主题不提供字体】
    // `GetThemeFont(HEADER, HEADERITEM, HIS_NORMAL, TMT_FONT)` 实测返回 na
    // （未定义，见 T_uxspec.md §1.3）→ 主题层根本没指定列头字体，规定上
    // 列头应与正文取同一个系统 9pt 字体、同一字重。FW_SEMIBOLD 会让列头
    // 明显比正文粗，属自创。
    //
    // 【命名遗留】这个成员叫 fontBold_，实际是**常规**体（列头专用）。
    // 改名成 fontHeader_ 更准确，但它在 tmwindow.h 里，本轮不动头文件。
    fontBold_      = mk(kFontSizeHeader, FW_NORMAL);
    fontNav_       = mk(kFontSizeNav, FW_NORMAL);
    fontCardTitle_ = mk(kFontSizeCardTitle, FW_NORMAL);
    fontCardValue_ = mk(kFontSizeCardValue, FW_NORMAL);
    fontCardSub_   = mk(kFontSizeCardSub, FW_NORMAL);
    fontGroup_     = mk(kFontSizeGroup, FW_SEMIBOLD);
    // fontPageTitle_ 不再创建：PaintPageHeader 是空实现，全项目从未
    // SelectObject 过这个字体。theme.h 里的 kFontSizePageTitle 已删除。
}

// ============ 窗口创建 ============

bool TmWindow::Create(HINSTANCE inst, int nCmdShow) {
    inst_ = inst;
    BuildFonts();
    pagesView_.BuildFonts();
    // 读回上次拖拽保存的列宽（若注册表里没有或数据非法，保持默认值）
    LoadColumnWidths();
    LoadSortState();   // 排序/折叠状态，与列宽同源恢复
    // 加速键表：此前 BuildAccelerators() 全工程无人调用，accel_ 从未被赋值。
    accel_ = MenuBuilder::BuildAccelerators();
    icons_.Start();

    const wchar_t* cls = L"TmProMainWindow";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProcStatic;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = cls;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    }

    hwnd_ = CreateWindowExW(
        0, cls, L"任务管理器",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 700,
        nullptr, nullptr, inst, this);
    if (!hwnd_) return false;

    BuildLayout();
    RebuildMenu();     // 必须在这里建菜单，否则 SetMenu 从未被调用，菜单栏为空
    // 强制正常显示：Start-Process 传入的 nCmdShow 有时是 SW_SHOWMINIMIZED，
    // 会让窗口以 -32000 坐标最小化启动，后续截图/自动化全部失效。
    const int showCmd = (nCmdShow == SW_SHOWMINIMIZED || nCmdShow == SW_SHOWMINNOACTIVE)
                        ? SW_SHOWNORMAL : nCmdShow;
    ShowWindow(hwnd_, showCmd);
    SetForegroundWindow(hwnd_);
    UpdateWindow(hwnd_);
    return true;
}

LRESULT CALLBACK TmWindow::WndProcStatic(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }
    auto* self = reinterpret_cast<TmWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->WndProc(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void TmWindow::BuildLayout() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int W = rc.right, H = rc.bottom;

    // Win10 布局：【菜单栏(非客户区)】→【顶部横向标签栏】→【内容区】→【底部工具条】
    //
    // 【底部没有状态栏】Win10 任务管理器客户区底部是**工具条**
    // （简略信息(D) | 打开资源监视器 | 打开服务），不存在独立的
    // 进程数/CPU/内存状态栏。此前自创过一个 24px 灰状态栏，已删除。
    rcMenu_  = RECT{ 0, 0, W, 0 };    // 菜单在非客户区，这里不占客户区空间

    // 标签栏高度 = tabtheme::kHeight = 30px。
    //
    // 【UIFILE 声明 30rp】原版 taskmgr.exe 的 DUI 资源里写着 `tab style height = 30rp`
    // （30001 属性 token @0x1cc）。本机 100% DPI，1rp = 1px。
    // 此前这里按 tmHeight + 13 + 4 推导出 34~36px，比原版高 4~6px —— 那是
    // 用主题边框图元拼高度的猜测，在有 UIFILE 声明的情况下应当让位。
    // MeasureHeight 直接返回 Scaled(30)，只在字体大到装不下时才撑高（防裁字）。
    // 出处：S_duispec.md §1.1、tabs.h 的 kHeight 注释。
    //
    // 【DPI 适配约定 —— 本函数是整窗几何的唯一出口】
    //   theme.h 里所有常量都是 96 DPI 基准值，主题 part 尺寸也不随 DPI 变化
    //   （Aero 只有 NormalSize 变体，R_sysmetrics.md §5.4），所以这里每个几何量
    //   都包一层 theme::Scaled()。本机 96 DPI 下 Scaled(x) == x，与改动前逐像素等价；
    //   125% 下才会真正放大。tabsH 已经由 MeasureHeight 内部缩放过，**不要重复包**。
    const int tabsH = tabtheme::MeasureHeight(fontNav_);
    const int toolbarH = theme::Scaled(kToolbarHeight);
    // 【活动页签向上凸出 2px，需要顶部预留空间】
    // tabtheme::kActiveRaise 让活动页签的顶边画在 bounds_.top - 2。
    // 如果页签带从 y=0 开始，那 2px 会被客户区上沿裁掉，"凸出"就看不见了。
    // 所以页签带整体下移 kActiveRaise，顶部的 2px 用来容纳凸出部分。
    // 底边位置不变（= tabsH），所以内容区起点也不变。
    const int tabRaise = theme::Scaled(tabtheme::kActiveRaise);

    // =====================================================================
    // 【简略信息视图】simpleView_ == true 时**收起整个页签栏**
    //
    // 【为什么这么改 —— 这不是我方发明，是原版行为】
    // 原版点底部「简略信息(D)」会整体切换到 **UIFILE 30024** 那套界面
    // （`taskman` 根 + GenericCommandBar），再点一次切回 30001。两份证据：
    //   1) 30024 的节点表只有 19 个节点，**没有 `tab` 的实例**：
    //      tab / SmallViewPage / drop / chart 只在样式区当 `(style)` 声明出现
    //      （_S_geo2_30024.tsv 第 1..20 行全是 `(style)` 选择器）。
    //      而 30001 有完整的 `tab` 实例（@0x1bc 声明 height=30rp、padding）。
    //      即：简略视图里**根本不存在页签控件**。
    //   2) 30024 的命令条里有一个 `atom(CBExpandoButtonImageArrow)` +
    //      `atom(CBExpandoButtonImageText)`（margin=rect(5rp,0,0,0)）——
    //      那就是「展开/收起」那对互斥按钮，也就是 MUI 37212「简略信息(D)」/
    //      37211「详细信息(D)」。它的作用就是把界面在 30001 ↔ 30024 之间切换。
    //   出处：docs/specs/S_duispec.md §0 表 E2、§1.7、§3.9、§7 第 3 条。
    //
    // 【本轮做到哪一步】做成「同一个窗口内的两种布局」，不是真的换一份 UIFILE：
    //   - 页签栏收起（rcTabs_ 零高）—— 对齐 30024 无 tab 实例
    //   - 列头收起（rcColHeader_ 零高）—— 30024 的简略列表是 TmSmallViewItem，
    //     条目内只有图标 + 友好名 + CPU/内存两列（_S_geo2_30024.tsv:128..137），
    //     高度声明 28rp，**没有 TmColHeader 实例**（那些 TmColHeader 行
    //     属于 processestab_tables，是另一棵树，且是 30024 的完整视图而非简略视图）
    //   - 列表占满整个内容区
    //   - 底部工具条保留（30024 有 GenericCommandBar，48rp）
    //   ⚠ 未做：像素级还原 30024 的 TmSmallViewItem 两列布局。那是另一件事，
    //     已在报告里注明。本轮只交付用户要的「页签栏收起」这个可感知行为。
    //
    // 【为什么把「恢复」写成无条件重算而不是「记住旧值再还原」】
    // 本函数每次调用都**从零算全部几何**，没有任何字段是"只在某个分支里赋值"。
    // 所以切回 normal 时不存在"漏还原某个字段"的可能 —— 这正是最可靠的
    // 进出重置逻辑：不需要维护「进入前的快照」，也就不会漏。
    // =====================================================================
    if (simpleView_) {
        rcTabs_    = RECT{ 0, 0, W, 0 };                    // 零高：无页签栏
        rcContent_ = RECT{ 0, 0, W, H - toolbarH };
        rcToolbar_ = RECT{ 0, H - toolbarH, W, H };
        rcStatus_  = RECT{ 0, H, W, H };
        rcMenu_    = RECT{ 0, 0, W, 0 };

        // 零高 bounds 的副作用是**对的**，不是要绕开的 bug：
        //   tabs_.HitTest 的第一句就是 `y < bounds_.top || y >= bounds_.bottom
        //   → return TabId::Count`，零高时恒不命中 ⇒ 简略视图里点不到页签。
        //   这正好是原版 30024（没有页签控件）的行为。
        tabs_.SetBounds(rcTabs_.left, rcTabs_.top,
                        rcTabs_.right - rcTabs_.left,
                        rcTabs_.bottom - rcTabs_.top);
        tabs_.SetFont(fontNav_);
        tabs_.SetHover(TabId::Count);   // 页签栏没了，别留着一个悬停态

        rcCards_     = RECT{ 0, 0, W, 0 };                  // 零高（原本就恒零高）
        rcColHeader_ = RECT{ 0, 0, W, 0 };                  // 零高：无列头
        rcList_      = RECT{ rcContent_.left, rcContent_.top,
                             rcContent_.right, rcContent_.bottom };

        perfPage_.SetBounds(rcContent_);
        pagesView_.SetBounds(rcContent_);
        return;
    }

    rcTabs_  = RECT{ 0, tabRaise, W, tabRaise + tabsH };
    rcContent_ = RECT{ 0, rcTabs_.bottom, W, H - toolbarH };
    rcToolbar_ = RECT{ 0, H - toolbarH, W, H };
    rcStatus_ = RECT{ 0, H, W, H };   // 空矩形，不再绘制

    tabs_.SetBounds(rcTabs_.left, rcTabs_.top,
                    rcTabs_.right - rcTabs_.left,
                    rcTabs_.bottom - rcTabs_.top);
    tabs_.SetFont(fontNav_);

    // 内容区边距 12。
    // 【无依据，沿用旧值】微软的对话框边距是 11 rp（UX 指南），这里是 12。
    // 差 1px，不值得为此改动卡片/列头/列表三条既有对齐链，故保留并标注。
    // 【DPI】12 按 96 DPI 基准值处理，同样走 Scaled()（本机恒等）。
    const int pad = theme::Scaled(12);

    // Win10 进程页：页签栏下面**没有摘要卡片区**，直接是 13px 空白 + 列头带。
    //
    // 【真原版截图实测 —— P0 结论】`docs/reference/original_win10.png`（1251×842）
    // 逐行扫描，y=79..91 这 13 行在 x=3..1246 范围内**非白像素数 = 0**，
    // y=92 才开始列头带。整个进程页顶部**根本不存在** CPU/内存/磁盘/网络/GPU
    // 那五张卡片 —— 那是性能页才有的东西（也是我方从 Win11 带进来的先入之见）。
    // 复扫脚本 `docs/evidence/_hh_scan3.ps1`。
    //
    // 改法：rcCards_ 高度**恒为 0**，不再随 simpleView_ 变化。
    //   - 保留 rcCards_ 这个成员、保留 PaintCards 函数（不删，避免连锁破坏）；
    //     零高矩形由下游两道防护自动挡掉：
    //       1) src/tmwindow_events.cpp 的 PaintAll：`if (rcCards_.bottom > rcCards_.top)`
    //       2) src/tmwindow_paint.cpp 的 PaintCards 首行：
    //          `if (rcCards_.bottom <= rcCards_.top) return;`
    //   - simpleView_ 的作用**已改到别处**：它现在收起页签栏 + 列头
    //     （见上面「简略信息视图」那一大段），不再影响这块卡片区。
    //     【历史】上一版这里写的是「simpleView_ 从此不再影响布局 → 它变成 no-op」，
    //     那个判断是错的：原版点「简略信息」是切到 UIFILE 30024（无 tab 实例），
    //     有明确的可感知效果，不是 no-op。
    const int gap = theme::Scaled(13);          // 【真原版实测 13px】页签底线 → 列头顶
    const int cardsTop = rcContent_.top + gap;
    rcCards_ = RECT{ rcContent_.left + pad, cardsTop,
                     rcContent_.right - pad, cardsTop };   // 零高：bottom == top

    // 列头高度 = theme::kProcHeaderHeight（44）。
    // 【真原版截图实测 44px】列头带 y=92..135（含 1px 底线 #A0A0A0）⇒ 44px。
    // 此前注释依据 `GetThemePartSize(HEADER, HEADERITEM, HIS_NORMAL, TS_TRUE)`
    // = 6x24 定成 24（加上留白后落到 34），那是拿**标准 SysHeader32 的主题尺寸**
    // 替代 taskmgr 自绘列头的实际尺寸 —— taskmgr 用 DirectUI 自绘，不走标准控件，
    // 主题 part 尺寸对它不适用。实测让位，改 44。详见 theme.h 的 kProcHeaderHeight 注释。
    // 【DPI】主题不随 DPI 变（§5.4），必须自己 Scaled()（本机恒等）。
    //
    // 【布局链】rcContent_.top（= 页签栏底） + 13 → 列头顶；
    //            列头高 44 → 列头底 = 页签栏底 + 57；
    //            原版实测：页签栏底 y=78 → 列头底 y=135，差 57。**完全对上。**
    const int colTop = rcContent_.top + gap;
    rcColHeader_ = RECT{ rcContent_.left, colTop, rcContent_.right,
                         colTop + theme::Scaled(kProcHeaderHeight) };

    // 列表
    rcList_ = RECT{ rcContent_.left, rcColHeader_.bottom, rcContent_.right, rcContent_.bottom };

    // 性能页占满整个内容区
    perfPage_.SetBounds(rcContent_);
    // 四个次要页面同样占满内容区
    pagesView_.SetBounds(rcContent_);
}

// ============ 数据更新 ============

// 切页时统一清理瞬态 UI 状态。
//
// 【为什么需要】这些"瞬态"状态各自只在某一条具体消息里被清除：
//   pressedCol_ —— 只在 WM_LBUTTONUP 里清零。WM_CAPTURECHANGED 的兜底
//     （tmwindow_events.cpp:254）只清了 colDragCol_ 和 scrollDragging_，
//     **仍然漏了 pressedCol_**。一旦卡住就是整列列头永久深蓝。
//   colDragCol_ —— 若正在拖列宽时用 Ctrl+N 切页，拖拽状态会跨页保留。
//
// 【悬停状态那一条已根治】原来的注释写的是「hoverRow_ / hoverCol_ /
//   tabs_.hover_ 只在 WM_MOUSEMOVE 里被改写，而全项目没有 WM_MOUSELEAVE /
//   TrackMouseEvent，所以鼠标移出窗口后它们会永久保持」。现在
//   tmwindow_events.cpp 的 WM_MOUSEMOVE 已挂 TrackMouseEvent(TME_LEAVE)、
//   并有 WM_MOUSELEAVE 分支走 ClearAllHover()，鼠标移出这条路径不再泄漏。
//   这里保留清理，是因为**切页**本身也要清（换页后旧的 hover 行号无意义），
//   两者理由不同、都要留。
void TmWindow::ShowPage(int index) {
    if (index < 0 || index >= static_cast<int>(TabId::Count)) return;

    // 【简略视图里没有"别的页"】原版 30024 是一整棵独立界面，没有页签控件
    // （见 BuildLayout 的长注释）。所以任何切页请求都等于「先退出简略视图」：
    // 保留布局零高、却把 tabs_.Current() 改成性能页，会画出一个没有页签栏、
    // 没有列头的性能页 —— 那是两份界面都不存在的状态。
    // 补 BuildLayout 一次，让页签栏/列头按 normal 布局恢复。
    if (simpleView_) simpleView_ = false;

    // 切页必须打断滚动惯性：滑行目标绑定在旧页的列表上，继续跑就会滑到
    // 新页的列表里（OnInertiaTick 也有一道守卫，这里是更早、更直接的拦截）。
    StopScrollInertia();
    ResetScrollSamples();
    panActive_ = false;

    tabs_.SetCurrent(static_cast<TabId>(index));
    scrollY_ = 0;
    detailScrollY_ = 0;
    hoverRow_ = -1;
    hoverCol_ = -1;
    pressedCol_ = -1;
    colDragCol_ = -1;
    tabs_.SetHover(TabId::Count);
    hoverToolbarCmd_ = 0;
    perfPage_.ClearHover();
    pagesView_.ClearHover();
    BuildLayout();
    LoadPageData(tabs_.Current());
    RebuildMenu();
    InvalidateAll();
}

void TmWindow::SetPerfChart(int mode) {
    perfPage_.SetChart(mode == 1 ? ChartMode::Logical : ChartMode::Overall);
    if (tabs_.Current() == TabId::Performance) InvalidateAll();
}

void TmWindow::SetSortColumn(int col) {
    if (col < 0 || col >= kSortColCount) return;
    sortCol_ = col;
    sortDesc_ = (col != 0);   // 名称默认升序，其余降序（和点击列头一致）
    scrollY_ = 0;
    RebuildRows();
    SaveSortState();   // 用户改了排序 → 持久化（原版重启后保留）
    InvalidateAll();
}

void TmWindow::ExpandAllApps() { OnMenuCommand(CMD_EXPAND_ALL); }

void TmWindow::ToggleSimpleView() {
    // 与 OnMenuCommand(CMD_VIEW_SIMPLE) 走同一条路径，避免两套实现走偏
    OnMenuCommand(CMD_VIEW_SIMPLE);
}

// 自测用：把简略视图**设成**指定状态（幂等）。
//
// 【为什么不直接调用两次 ToggleSimpleView】那依赖当前状态，前面任何一项
// 测试改了它，这一项的结果就不可重复。自测要的是「从确定的起点出发」。
void TmWindow::SetSimpleViewForTest(bool on) {
    if (simpleView_ == on) return;
    simpleView_ = on;
    // 与 CMD_VIEW_SIMPLE 一致：简略视图恒落在进程页（30024 没有页签控件）。
    if (on) tabs_.SetCurrent(TabId::Processes);
    hoverRow_ = -1;
    hoverCol_ = -1;
    tabs_.SetHover(TabId::Count);
    BuildLayout();
    RebuildRows();
    InvalidateAll();
}

// 只做滚动范围钳制，不重建行表。见 include/tmwindow.h 的声明注释。
//
// RebuildRows() 末尾那段钳制逻辑与这里必须保持**同一口径**（都用
// RowHeightFor 逐行累加），否则滚到底会多出/少掉一屏空白。
// 仅供 --gdileak 回归测试。见 include/tmwindow.h 的声明注释。
void TmWindow::ForceBackBufferSize(int w, int h) {
    // 先记下当前有效尺寸，再强制让 EnsureBackBuffer 认为"尺寸变了"，
    // 这样即使传入的 w/h 与现有相同也一定会走重建分支。
    memW_ = -1;
    memH_ = -1;
    EnsureBackBuffer(w, h);
}

// 清掉 appExpanded_ 里已退出进程的条目。
// 见 include/tmwindow.h 的成员注释：这张表原本靠 operator[] 读操作无界增长，
// 现在读走 IsAppExpanded 不插入，但用户点开的那些条目仍会留着，需要这层清理。
void TmWindow::PruneAppExpanded() {
    if (appExpanded_.empty() || !snap_) return;
    // O(N + E) 而不是 O(N×E)：先建存活 pid 集合再扫表。
    // appExpanded_ 通常只有用户点开的几十条，进程数 600，扫表本身极快；
    // 但写成嵌套循环就是 600×几十 每次都跑，没必要。
    std::unordered_set<uint32_t> alive;
    alive.reserve(snap_->processes.size() * 2);
    for (const auto& e : snap_->processes) alive.insert(e.pid);
    for (auto it = appExpanded_.begin(); it != appExpanded_.end(); ) {
        if (alive.count(it->first)) ++it;
        else it = appExpanded_.erase(it);
    }
}

// 清掉 userCache_ 里已退出进程的条目。
// 见 include/tmwindow.h 的成员注释。这张表会被详细信息页的绘制循环按 pid 查询，
// 不清理的话长跑会持续累积。
void TmWindow::PruneUserCache() {
    if (userCache_.empty() || !snap_) return;
    std::unordered_set<uint32_t> alive;
    alive.reserve(snap_->processes.size() * 2);
    for (const auto& e : snap_->processes) alive.insert(e.pid);
    for (auto it = userCache_.begin(); it != userCache_.end(); ) {
        if (alive.count(it->first)) ++it;
        else it = userCache_.erase(it);
    }
}

void TmWindow::ClampScroll() {
    int bodyH = rcList_.bottom - rcList_.top;
    int totalH = 0;
    for (const auto& r : rows_) {
        totalH += RowHeightFor(r.kind == ViewRow::GroupHeaderRow, r.indentLevel);
    }
    int maxScroll = (std::max)(0, totalH - bodyH);
    if (scrollY_ > maxScroll) scrollY_ = maxScroll;
    if (scrollY_ < 0) scrollY_ = 0;
}

void TmWindow::SetGroupByType(bool on) {
    if (groupByType_ == on) return;
    groupByType_ = on;
    scrollY_ = 0;
    RebuildRows();
    InvalidateAll();
    RebuildMenu();
}

void TmWindow::SimulateHeaderClick(int col) {
    // 不直接依赖 kCols/ColX（它们在 tmwindow_list.cpp 的匿名命名空间里），
    // 而是在列头带内逐像素扫描，用 HitTestColumn 反查每个 x 对应哪一列，
    // 找到第 col 列的第一个 x，再在那个位置"点击"。
    // 这样验证的是真实命中路径，而不是我自己的坐标推算。
    const int y = (rcColHeader_.top + rcColHeader_.bottom) / 2;
    int found = -1;
    for (int x = rcColHeader_.left; x < rcColHeader_.right; ++x) {
        if (HitTestColumn(x, y) == col) { found = x; break; }
    }
    if (found < 0) return;

    int hit = HitTestColumn(found, y);
    if (hit < 0) return;
    if (sortCol_ == hit) {
        sortDesc_ = !sortDesc_;
    } else {
        sortCol_ = hit;
        sortDesc_ = (hit != 0);
    }
    scrollY_ = 0;
    RebuildRows();
    SaveSortState();
    InvalidateAll();
}

void TmWindow::SetPerfViewByName(const wchar_t* name) {
    perfPage_.SetViewByName(name);
    if (tabs_.Current() == TabId::Performance) InvalidateAll();
}

void TmWindow::SetPerfSnapshot(std::shared_ptr<const PerfSnapshot> snap) {
    if (!snap) return;
    // 【P0-1 数据接续】进程页顶部 5 张汇总卡片的迷你曲线读的是 hist_
    // （TmWindow::ChartHistory），而 hist_ 由 SetSnapshot 每秒推一次。
    // 磁盘/网络/GPU 的真实值只在 PerfSnapshot 里，所以这里留一份指针，
    // 让 SetSnapshot 推曲线时能取到。两份快照是独立的采集器、独立的
    // 代际号（main.cpp 里 g_collector / g_perf），到达顺序不保证，
    // 所以不能假设"同一轮的 PerfSnapshot 一定在 SetSnapshot 之前到"——
    // 用最近一次到达的那份即可，两者都是 1Hz，差一拍不影响曲线形状。
    perfSnap_ = snap;
    perfPage_.SetData(std::move(snap));
    // 只有当前在性能页才重绘，避免别的页面被性能数据刷新打断
    if (tabs_.Current() == TabId::Performance) {
        InvalidateAll();
    }
}

void TmWindow::SetSnapshot(std::shared_ptr<const Snapshot> snap) {
    if (!snap) return;
    snap_ = std::move(snap);
    uiSeq_ = snap_->sequence;

    // 先解析 exe 路径（增量 + 缓存），再分类。
    //
    // 【顺序很重要】Classifier 判断「Windows 进程」时要看 exe 是不是在
    // System32 下，而路径是懒加载的。之前先 Classify 后 ResolvePaths，
    // 导致分类时拿不到路径，几乎所有系统进程都被错分到「后台进程」。
    ResolvePathsAsync();

    Classifier clf;
    clf.Classify(*snap_, cls_, &pathCache_);

    // 次要页面也需要进程快照（用户页按 session 聚合）
    pagesView_.SetSnapshot(snap_);
    if (tabs_.Current() == TabId::Users) {
        pagesView_.RefreshUsers();
    }

    RebuildRows();
    // detailRows_ 存的是快照内的裸指针，换新快照后必须重建，否则悬垂
    RebuildDetailRows();

    // 历史曲线：用总量算
    // 【排除 PID 0】空闲进程的 CPU 时间 = 系统空闲时间，计入会让曲线恒在 90%+。
    double cpuTotal = 0.0, memUsed = 0.0;
    for (const auto& e : snap_->processes) {
        if (e.pid == 0) continue;
        cpuTotal += e.cpuPercent;
    }
    if (cpuTotal > 100.0) cpuTotal = 100.0;
    if (snap_->totalPhysical > 0) {
        memUsed = static_cast<double>(snap_->totalPhysical - snap_->availablePhysical) /
                  static_cast<double>(snap_->totalPhysical) * 100.0;
    }
    // 【P0-1 修】后三路此前是硬编码的 0.0，于是磁盘/网络/GPU 三张卡片的迷你
    // 曲线永远是贴着底边的死直线。真值只在 PerfSnapshot 里（Snapshot 结构
    // 根本没有整机磁盘/网络/GPU 字段），所以经 perfSnap_ 取。
    const CardMetrics cm = SampleCardMetrics();
    hist_.Push(cpuTotal, memUsed, cm.diskPct, cm.netPct, cm.gpuPct);

    InvalidateAll();
}

// 进程页顶部「磁盘 / 网络 / GPU」三张汇总卡片的真实百分比。
//
// 【为什么要一个函数】这三路的换算口径（多实例取最大还是求和、百分比的
// 饱和量程、单位换算）必须只有一份定义。目前有两个消费点：
//   1. 迷你曲线：SetSnapshot → hist_.Push（本文件）；
//   2. 卡片主数值文字（"7%"）：src/tmwindow_paint.cpp 的 PaintCards ——
//      那是 TmWindow 的成员函数、可以直接调本函数，但它不归本模块改，
//      见报告里的交接单。在它接上之前，卡片文字仍是写死的 "0%"，曲线已真实。
// 两处共用一份公式，避免出现"数字用聚合值、热力底色用原始值"这类自相矛盾
// （本项目已经踩过，见 docs/specs/spec_redteam2.md 的 K 条）。
//
// 【多实例口径】两条规则，与 RebuildAppAggregates 里对 GPU 的说明同源：
//   - 百分比（磁盘活动时间 / 网络链路利用率 / GPU 使用率）取**最大值**：
//     这些是时间片占比，多个实例并行发生，相加会超过 100%（5 个进程各 30%
//     相加 150% 是荒谬的）。取最大值 = "最忙的那一个"，也是原版汇总卡的口径。
//   - 若将来要显示副标题（读/写 MB/s、收发 Mbps、显存 GB），那些是可叠加的
//     物理量，应**求和**——但那是 paint 侧的活，这里不提前算没人读的数。
TmWindow::CardMetrics TmWindow::SampleCardMetrics() const {
    CardMetrics m;
    if (!perfSnap_) return m;
    const PerfSnapshot& p = *perfSnap_;

    // ---- 磁盘 ----
    // 主数值 = 活动时间百分比（proc_semantics.md §5.3：主数值"活动时间百分比"，
    // 副标题"读/写速率"）。
    // 本机 perfdata 只产出 1 条（NtQSI(2) 全局 IO 差分聚合成的「磁盘 0」，
    // 见 perfdata.cpp 的注释），循环对它等价于取那一条；若将来改成按盘产出，
    // 这里的"取最大"规则直接成立，不用再改。
    for (const auto& d : p.disks) {
        if (d.activePercent > m.diskPct) m.diskPct = d.activePercent;
    }
    if (m.diskPct > 100.0) m.diskPct = 100.0;

    // ---- 网络 ----
    // 主数值 = 链路利用率（proc_semantics.md §5.4）。
    // 单位：NetSample 的 sendMbps/recvMbps 是 **Mbps**，linkSpeedBps 是
    // **bits/s**，所以 /1e6 才能相除。
    for (const auto& n : p.nets) {
        const double busy = (std::max)(n.sendMbps, n.recvMbps);
        // linkSpeedBps 为 0 的适配器（虚拟网卡 / 驱动没上报）退回 1 Gbps 口径：
        // 与 perfpage.cpp 里 `mbps / 1000.0 * 100.0` 的既有换算一致，且不会除零。
        const double linkMbps = n.linkSpeedBps ? static_cast<double>(n.linkSpeedBps) / 1e6
                                               : 1000.0;
        const double pct = (linkMbps > 0.0) ? (busy / linkMbps * 100.0) : 0.0;
        if (pct > m.netPct) m.netPct = pct;
    }
    if (m.netPct > 100.0) m.netPct = 100.0;

    // ---- GPU ----
    // 主数值 = 使用率（proc_semantics.md §5.5）。
    for (const auto& g : p.gpus) {
        if (g.utilPercent > m.gpuPct) m.gpuPct = g.utilPercent;
    }
    // 兜底：gpuinfo 可用但 gpus 为空时用整机聚合值 ——
    // perfdata.cpp 里 `g.utilPercent = out.gpuTotalPercent`，本来就是同一个数。
    if (p.gpus.empty() && p.gpuTotalPercent > 0.0) m.gpuPct = p.gpuTotalPercent;
    if (m.gpuPct > 100.0) m.gpuPct = 100.0;

    return m;
}

// 重建应用聚合表：每个应用根累加它自己和所有子进程的 CPU / 内存。
//
// 【为什么必须做】原版折叠时应用行显示的是整个应用的合计。
// 实测 Claude 根进程 477MB，12 个进程合计约 3075MB。
// 只显示根进程的值会让用户严重低估应用的真实占用。
void TmWindow::RebuildAppAggregates() {
    appAgg_.clear();
    if (!snap_) return;

    for (const auto& e : snap_->processes) {
        if (e.pid == 0) continue;
        // 找到这个进程归属的应用根
        auto it = cls_.appRootOf.find(e.pid);
        if (it == cls_.appRootOf.end()) continue;
        uint32_t root = it->second;

        auto& agg = appAgg_[root];
        agg.cpuPercent += e.cpuPercent;
        agg.memBytes += (e.privateBytes ? e.privateBytes : e.workingSet);
        ++agg.processCount;
        // 磁盘 / 网络：可叠加的物理量，直接相加
        agg.diskBytesPerSec += e.diskBytesPerSec;
        agg.netBitsPerSec += e.netBitsPerSec;
        // GPU：**不能相加**。多个进程同时用一个 GPU 引擎时，各自报的
        // utilization 是时间片占比，相加会超过 100%（比如 5 个进程各 30%，
        // 加起来 150% 是荒谬的）。取最大值表示"这个应用最忙的那个进程"。
        if (e.gpuPercent > agg.gpuPercentMax) agg.gpuPercentMax = e.gpuPercent;
    }
}

void TmWindow::RebuildRows() {
    rows_.clear();
    // byPid_ 存的是快照内的裸指针，换快照后必须重建，否则悬垂
    byPid_.clear();
    if (!snap_) return;

    // 清掉已退出进程的展开状态（每轮一次 O(N+E)，可忽略）。
    PruneAppExpanded();
    // 同上：清理用户名缓存里已退出的 pid。
    PruneUserCache();

    // 预建排序用的名称表（只在按名称列排序时需要，其它列白建一次也没多少开销）。
    // 见下方 case 0 的注释：避免比较器里 2×NlogN 次 std::wstring 分配。
    //
    // 【不要在这里触发文件 IO】ProcessDisplayName 内部会走 GetFileDescription，
    // 缓存未命中时是一次真实的 GetFileVersionInfo（实测 1~2 ms）。如果这里
    // 对全部 600 个进程都调一遍，就把 prefetch 的「每轮最多 24 个」上限
    // 完全绕过了 —— 首轮 300 ms 卡顿原封不动地回来。
    //
    // 所以：本轮已经在 descCache_ 里的直接用（缓存命中，零 IO）；
    // 没有的先用 exe 文件名顶上（DisplayName 本来就有这条回退），
    // 等 ResolvePathsAsync 的分批预取慢慢把它换成文件描述。
    // 名称列的显示与排序会随预取推进逐批"变好看"，但不会卡。
    // 只有「按名称排序」且「descCache_ 有变化」时才重建排序键表。
    // 见 include/tmwindow.h 的 descCacheEpoch_ 注释：不重建的话，
    // 预取取回新描述后「显示用新描述、排序用旧文件名」，两者不一致。
    // 三个重建条件：
    //   1. descCache_ 有新描述（预取取回了新路径）
    //   2. 表还是空的（首次）
    //   3. 表的条目数与本轮进程数对不上（进程增删过 —— 切排序后表被保留，
    //      但这期间可能有进程启停，旧表缺条目或含已退出的 pid）
    size_t liveProcs = 0;
    for (const auto& e : snap_->processes) if (e.pid != 0) ++liveProcs;
    const bool needSortKeys = (sortCol_ == 0) &&
                              (sortNameEpoch_ != descCacheEpoch_ ||
                               sortNameCache_.empty() ||
                               sortNameCache_.size() != liveProcs);
    if (needSortKeys) {
        sortNameEpoch_ = descCacheEpoch_;
        sortNameCache_.clear();
        sortNameCache_.reserve(snap_->processes.size() * 2);
        for (const auto& e : snap_->processes) {
            if (e.pid == 0) continue;
            // 只查缓存，不触发 IO
            std::wstring name;
            auto pit = pathCache_.find(e.pid);
            if (_wcsicmp(e.imageName.c_str(), L"svchost.exe") == 0) {
                auto itS = svcHostName_.find(e.pid);
                name = (itS != svcHostName_.end() && !itS->second.empty())
                       ? itS->second : std::wstring(L"服务主机");
            } else if (pit != pathCache_.end() && !pit->second.empty()) {
                auto dit = descCache_.find(pit->second);
                name = (dit != descCache_.end() && !dit->second.empty())
                       ? dit->second : DisplayName(e.imageName);
            } else {
                name = DisplayName(e.imageName);
            }
            sortNameCache_.emplace(e.pid, std::move(name));
        }
    }

    // 先算应用聚合值（应用行显示合计，不是根进程自己的值）
    RebuildAppAggregates();

    // 每组的进程列表
    std::vector<const ProcessEntry*> groups[3];
    for (const auto& e : snap_->processes) {
        if (e.pid == 0) continue;   // Idle 不进列表
        auto it = cls_.groupOf.find(e.pid);
        ProcessGroup g = (it != cls_.groupOf.end()) ? it->second : ProcessGroup::Background;
        groups[static_cast<int>(g)].push_back(&e);
    }

    // 【性能修复】这里原先有两段遍历**全部进程 + 全部应用聚合**的循环，
    // 只为算出 maxCpu_/maxMem_/maxDisk_/maxNet_/maxGpu_ 五个"归一化最大值"。
    // 但这五个成员在本项目里**只被一段死代码读取**（tmwindow_list.cpp 的
    // heatT 计算块，算完赋值给局部变量 heatT 后再无人读）。
    // 真正决定染色深浅的是同一段下面那个 heatCols[]，它用的是**每列各自的
    // 绝对百分比**，与这五个最大值毫无关系。
    //
    // 后果：每次 RebuildRows 都白跑两遍 O(n)。而 RebuildRows 会被滚动条拖动
    // 的每个 WM_MOUSEMOVE 调用 —— 拖一次滚动条就白烧两遍全进程遍历。
    // 所以这里只保留真正被用到的 memTotalBytes_（内存列热力图的量程），
    // 两段归一化循环与 max*_ 的赋值一并删除。
    // （tmwindow.h 里那五个成员与 tmwindow_list.cpp 的死块由各自 owner 清理。）
    memTotalBytes_ = snap_->totalPhysical;

    // 排序：按当前排序列排序，方向由 sortDesc_ 决定。
    // 【原版默认 = 名称升序】见 tmwindow.h 的 sortCol_/sortDesc_ 注释（附原版截图实测）。
    // 点击列的规则：名称列首次点击升序，其余列首次点击降序（大→小更符合直觉），
    // 再次点击同一列翻转。实现在 OnColumnClick / SetSortColumn 里。
    // 排序用的取值函数：应用根用**聚合值**，其余用自己的值。
    //
    // 【一致性要求】显示用聚合值、排序用根进程值会导致明显的错乱：
    // Claude 显示 5.8GB 却排在 2GB 的应用下面。必须两边用同一套数据。
    auto sortCpu = [this](const ProcessEntry* e) -> double {
        auto it = appAgg_.find(e->pid);
        if (it != appAgg_.end() && cls_.appRoots.count(e->pid)) return it->second.cpuPercent;
        return e->cpuPercent;
    };
    auto sortMem = [this](const ProcessEntry* e) -> uint64_t {
        if (cls_.appRoots.count(e->pid)) {
            auto it = appAgg_.find(e->pid);
            if (it != appAgg_.end()) return it->second.memBytes;
        }
        return e->privateBytes ? e->privateBytes : e->workingSet;
    };
    auto sortDisk = [this](const ProcessEntry* e) -> double {
        if (cls_.appRoots.count(e->pid)) {
            auto it = appAgg_.find(e->pid);
            if (it != appAgg_.end()) return it->second.diskBytesPerSec;
        }
        return e->diskBytesPerSec;
    };
    auto sortNet = [this](const ProcessEntry* e) -> double {
        if (cls_.appRoots.count(e->pid)) {
            auto it = appAgg_.find(e->pid);
            if (it != appAgg_.end()) return it->second.netBitsPerSec;
        }
        return e->netBitsPerSec;
    };
    auto sortGpu = [this](const ProcessEntry* e) -> double {
        if (cls_.appRoots.count(e->pid)) {
            auto it = appAgg_.find(e->pid);
            if (it != appAgg_.end()) return it->second.gpuPercentMax;
        }
        return e->gpuPercent;
    };

    auto cmp = [this, &sortCpu, &sortMem, &sortDisk, &sortNet, &sortGpu](
                   const ProcessEntry* a, const ProcessEntry* b) {
        switch (sortCol_) {
        case 0: {   // 名称
            // 【关键修复】原版「名称」列显示的是 exe 的**文件描述**
            // （FileDescription），不是文件名：
            //   explorer.exe -> "Windows 资源管理器"
            //   dwm.exe      -> "桌面窗口管理器"
            //   Taskmgr.exe  -> "任务管理器"
            // 排序也按这个描述排。之前我按 exe 文件名排，所以顺序和原版完全不同。
            //
            // 比较规则用 NaturalCompare（忽略大小写 + 数字按数值 + 中文拼音序），
            // 和 Windows 资源管理器一致。
            //
            // 【排序键预建】ProcessDisplayName 按值返回 std::wstring，每次比较
            // 分配两次堆对象。N log N 次比较（640 进程约 11000 次）就是 2 万多次
            // 分配/释放。这里在 RebuildRows 开头一次性建好 pid → 名称 表，
            // 比较器只查表拿 const wchar_t*，分配次数从 2×NlogN 降到 N。
            auto nameOf = [this](const ProcessEntry* e) -> const wchar_t* {
                auto itN = sortNameCache_.find(e->pid);
                if (itN != sortNameCache_.end()) return itN->second.c_str();
                return e->imageName.c_str();
            };
            int r = NaturalCompare(nameOf(a), nameOf(b));
            if (r != 0) return sortDesc_ ? (r > 0) : (r < 0);
            // 描述相同时（同名多实例）按 pid 稳定排序
            break;
        }
        case 1: {   // 状态：三态排序（已挂起 > 无响应 > 正常）
            // 用 Classification 里的集合，而不是重新算 —— 和显示用同一套判据，
            // 否则会出现「显示出无响应但排序按正常处理」的矛盾。
            auto rank = [this](const ProcessEntry* e) -> int {
                if (cls_.suspended.count(e->pid)) return 2;
                if (cls_.notResponding.count(e->pid)) return 1;
                return 0;
            };
            int ra = rank(a), rb = rank(b);
            if (ra != rb) return sortDesc_ ? (ra > rb) : (ra < rb);
            break;
        }
        case 2:     // CPU：应用根用聚合值
            {
                double ca = sortCpu(a), cb = sortCpu(b);
                if (ca != cb) return sortDesc_ ? (ca > cb) : (ca < cb);
            }
            break;
        case 3:     // 内存：应用根用聚合值（和显示一致）
            {
                uint64_t ma = sortMem(a), mb = sortMem(b);
                if (ma != mb) return sortDesc_ ? (ma > mb) : (ma < mb);
            }
            break;
        case 4:     // 磁盘：应用根用聚合值
            {
                double da = sortDisk(a), db = sortDisk(b);
                if (da != db) return sortDesc_ ? (da > db) : (da < db);
            }
            break;
        case 5:     // 网络：应用根用聚合值
            {
                double na = sortNet(a), nb = sortNet(b);
                if (na != nb) return sortDesc_ ? (na > nb) : (na < nb);
            }
            break;
        case 6:     // GPU：应用根用聚合值（最大值，不是相加）
            {
                double ga = sortGpu(a), gb = sortGpu(b);
                if (ga != gb) return sortDesc_ ? (ga > gb) : (ga < gb);
            }
            break;
        default:
            break;
        }
        // 值相等时的稳定兜底：按 pid 保证顺序确定，避免每帧抖动。
        // 【方向必须跟随 sortDesc_】原来固定 `a->pid < b->pid`，于是降序时
        // 主键降序、同值兜底却升序：两个同名进程 pid 100/200 在名称降序下
        // 仍排成 100,200，与「整体降序」的直觉矛盾（大量 0% 行尤其明显）。
        return sortDesc_ ? (a->pid > b->pid) : (a->pid < b->pid);
    };

    // 【索引 · 一次建好】pid -> entry。本函数三处按 pid 反查：
    // 改动 1 的孤儿守卫、appendKids、分组视图子进程插入。
    // 原来只有后两处各自 `if (byPid_.empty())` 惰性构建，都藏在「应用行已展开」
    // 的分支里 —— 默认全部折叠时这张表在 RebuildRows 里从不被建。
    // ⚠ 改动 1 的孤儿守卫**依赖这张表非空**；删掉这段会让子进程重新变成
    // 顶层行，缺陷静默复现（没有任何断言会报错）。
    if (byPid_.empty()) {
        byPid_.reserve(snap_->processes.size() * 2);
        for (const auto& ce : snap_->processes) byPid_[ce.pid] = &ce;
    }

    // 【性能】「每个应用根有几个子进程」只算一次，O(N)。
    // 原来在平铺分支和分组分支里各有 1~2 处 `for (kv : cls_.appRootOf)` 内层循环，
    // 每个应用行都扫一遍全表 → O(A×N)。而 RebuildRows 会被拖动滚动条的
    // 每个 WM_MOUSEMOVE 调用，640 进程 × 数十应用 = 数万次哈希查找/像素，
    // 拖动时明显卡顿。这里一次遍历把 pid → 子进程数建成表，各处只查表。
    std::unordered_map<uint32_t, int> kidsCount;
    for (const auto& kv : cls_.appRootOf) {
        if (kv.first != kv.second) ++kidsCount[kv.second];
    }
    auto hasKids = [&kidsCount](uint32_t pid) {
        auto it = kidsCount.find(pid);
        return it != kidsCount.end() && it->second > 0;
    };

    // 「查看 → 按类型分组」关闭时的平铺视图，**或者**按非名称列排序时的扁平视图。
    //
    // 【最终结论 · 2026-11-19 · 两个 agent 对立，经逐条复核后判定「联动存在」】
    //
    // 这个问题我审了三轮，结论反复了一次，最终定案如下。
    //
    // 判定依据 — 原版有**三个独立状态字节**（我逐条 grep 全部引用点核实）：
    //   视图对象内  `0x35B8`  = 实际是否分组（组头显隐直接读它）
    //   全局        `0x13A721`= 「按类型分组」的**菜单勾选态**（用户偏好）
    //   全局        `0x13C2E8`= 「分组被排序暂时压制」标记
    //
    // 写入/读取点：
    //   `0x35B8`  写：`0x25913`(构造) / `0xA751B`(SetGroupByType 内部)
    //            读：`0x7FEC8`(组头显隐) / `0xC82F`(建行选组名) / …
    //   `0x13A721`写：`0x92FB7` `sete`，就在「按类型分组」菜单命令(0x7705)的分派里
    //   `0x13C2E8`写：`0x841FD`(=1) / `0x84223`(=dl)，都在 `0x8411C` 内
    //
    // `0x8411C` 的关键分支（反汇编原文）：
    //     0x841E2 test esi,esi
    //     0x841E4 je   0x84209            ; esi == 0 → 走「名称列」分支
    //     0x841E6 cmp  byte[rcx+35B8h],bl ; 非名称列：若当前已分组…
    //     0x841EE cmp  byte[13A721h],bl   ; …且用户偏好是「分组」…
    //     0x841F6 xor  edx,edx
    //     0x841F8 call 0xA7490            ; → SetGroupByType(0)  **强制关分组**
    //     0x841FD mov  byte[13C2E8h],1    ; 记下「被排序压制」
    //     ---------------------------------------------------------------
    //     0x84209 cmp  byte[13C2E8h],bl   ; 名称列：若之前被压制过…
    //     0x84211 cmp  byte[13A721h],bl   ; …且用户偏好是「分组」…
    //     0x84219 mov  dl,1
    //     0x8421B call 0xA7490            ; → SetGroupByType(1)  **恢复分组**
    //     0x84223 mov  byte[13C2E8h],bl   ; 清掉压制标记
    //
    // 这就是「列表变平但菜单勾还打着」的机制：排序只改 `0x35B8` 和 `0x13C2E8`，
    // **从不改 `0x13A721`**（用户偏好）。点回名称列时按 `0x13C2E8` 自动恢复。
    //
    // 为什么上一轮我一度否掉了它：我当时只查了 `0x35B8` 的写点，看到排序函数
    // `0x8626C`/`0x85E5C` 不写它，就下了「正交」的结论。但真正的调用点是
    // `0x8411C` 里的 `0x841F8`/`0x8421B` —— 它对 `0x35B8` 是**间接**影响
    // （通过 `SetGroupByType`），所以我那次 grep 漏掉了。
    //
    // 像素侧的独立印证（我自己测）：
    //   `original_win10.png`  名称列 ▴升序 → 有组头
    //   `sortname_ours2.png`  名称列 ▾降序 → 有组头   ← 升降序都算「名称列」
    //   `sortname_ours1.png`  CPU 列 ▾降序  → 无组头，且全 22 行行距 27~29.5px
    //   `orig2_full.png`      内存列        → 无组头
    // 分界是「名称列 vs 非名称列」，不是「升序 vs 降序」。
    const bool flatBySort = (sortCol_ != 0);

    // 【简略信息视图】原版点底部「简略信息(D)」会切到 UIFILE 30024 的老式简略
    // 界面（`taskman` 根 + GenericCommandBar），那是一个独立控件树，本实现没有
    // 那份布局。这里给一个**语义上等价**的简化：只列顶层应用行，不分组、
    // 不展开子进程、不画组头 —— 也就是「一眼看清有哪些应用在占资源」。
    // 这样那个按钮至少有了真实可见的效果，而不是点完画面毫无变化。
    // ⚠ 与 30024 的原版外观不是像素级对齐；这是行为对齐，已在注释里注明。
    if (simpleView_) {
        std::vector<const ProcessEntry*> apps;
        for (const auto& e : snap_->processes) {
            if (e.pid == 0) continue;
            if (!cls_.appRoots.count(e.pid)) continue;
            apps.push_back(&e);
        }
        std::sort(apps.begin(), apps.end(), cmp);
        for (const auto* e : apps) {
            ViewRow r;
            r.kind = ViewRow::ProcessRow;
            auto itG = cls_.groupOf.find(e->pid);
            r.group = (itG == cls_.groupOf.end()) ? ProcessGroup::Background : itG->second;
            r.groupCount = 0;
            r.pid = e->pid;
            r.rootPid = e->pid;
            r.indentLevel = 0;
            r.hasChildren = false;   // 简略视图不展开
            r.expanded = false;
            rows_.push_back(r);
        }
        goto finish_rows;
    }

    if (!groupByType_ || flatBySort) {
        std::vector<const ProcessEntry*> all;
        all.reserve(snap_ ? snap_->processes.size() : 0);
        if (snap_) {
            for (const auto& e : snap_->processes) {
                if (e.pid == 0) continue;
                all.push_back(&e);
            }
        }
        std::sort(all.begin(), all.end(), cmp);

        // 【缺陷 A 修复 · 2026-11-21】
        //
        // 原来这里多一行 `if (!IsAppExpanded(e->pid)) continue;`，于是：
        //   展开的根 → 子进程在 nestedPids → 跳过顶层行，由 appendKids 输出  ✓
        //   折叠的根 → 子进程不在 nestedPids → 又被当顶层行输出一遍          ✗
        // 而折叠根行显示的已是整棵树的聚合值，于是同一批内存被显示两次：
        // Claude (106) 5.9 GB 正下方紧跟 Claude 1.2 GB、Claude Code 445.2 MB …
        // 实测（--sortcol=3）：屏幕前 18 行内存列之和 13.54 GB = 物理内存的
        // 42.4%，而整机实际只用了 55%（17.57 GB）。
        //
        // 原版实测（docs/reference/orig2_full.png，逐像素）：24 行文字起点
        // 全部 x=53/54，无一行缩进到 70 以上 → 折叠时子进程一行都不显示。
        // 折叠根只占一行、显示聚合值。[确证]
        //
        // 正确语义：一个进程只可能在两处之一出现 ——
        //   (1) 归属某应用根 → 仅在父行展开时由 appendKids 输出一次，折叠时不出现
        //   (2) 其余 → 作为顶层行
        // 与父行是否展开无关。判据查 cls_.appRootOf，每进程 O(1)。
        //
        // 【孤儿守卫 · 成因已更正】
        //
        // 早先这里写的是「EnumWindows 与快照有毫秒级时差」，那是错的：
        // appRootOf 完全由 snap_ 自己的表构建，根要成为 root，必须出现在某个
        // 进程的 parentPid 链上，压根不读 EnumWindows 的当下结果。
        //
        // 真实成因是【陈旧 parentPid + pid 复用】：
        //   父进程早已退出，子进程的 parentPid 仍指着那个 pid；而那个 pid 被
        //   一个**新启动的、有可见窗口的**进程复用 → 该 pid 命中 appRoots →
        //   appRootOf[child] = 复用后的 pid，但新进程不在本轮快照里 →
        //   byPid_ 查不到 → 子进程成了「查得到根、却找不到父行」的孤儿。
        //   若照样跳过，它会连同行一起从列表消失（内存值凭空蒸发），
        //   比重复展示更糟。
        //
        // 所以只有确认父行确实存在（byPid_ 查得到），才允许隐藏。
        // 这只是遮住症状：BuildAppTree 目前没有 CreateTime 校验（classifier.cpp:161），
        // 复用的 pid 仍会被当成同一个父进程。根治要单独提交。
        //
        // 已知不一致（本次不修，仅记录）：hasKids 用的 kidsCount 没有孤儿守卫，
        // 于是孤儿父行的情况可能画出展开三角、展开却是空。老问题，低危。

        // 展开应用行时，把子进程插到它下面。与分组视图一致：只展开一层，
        // 子进程自身按同一比较器排序。
        auto appendKids = [&](const ProcessEntry* root) {
            std::vector<const ProcessEntry*> kids;
            for (const auto& kv : cls_.appRootOf) {
                if (kv.second != root->pid || kv.first == root->pid) continue;
                auto itK = byPid_.find(kv.first);
                if (itK != byPid_.end()) kids.push_back(itK->second);
            }
            std::sort(kids.begin(), kids.end(), cmp);
            for (const auto* ce : kids) {
                ViewRow kr;
                kr.kind = ViewRow::ProcessRow;
                kr.group = (cls_.groupOf.count(ce->pid) ? cls_.groupOf.at(ce->pid)
                                 : ProcessGroup::Background);
                kr.groupCount = 0;
                kr.pid = ce->pid;
                kr.rootPid = root->pid;
                kr.indentLevel = 1;
                kr.hasChildren = false;
                kr.expanded = false;
                rows_.push_back(kr);
            }
        };

        // 归属某应用根 → 只在父行展开时由 appendKids 输出，不作顶层行。
        // 但父行必须确实存在（byPid_ 查得到），否则该进程会从列表彻底消失。
        auto isNestedChild = [this](const ProcessEntry* e) {
            auto itR = cls_.appRootOf.find(e->pid);
            if (itR == cls_.appRootOf.end()) return false;      // 不归属任何应用根
            if (itR->second == e->pid) return false;            // 自映射 = 应用根自己
            return byPid_.find(itR->second) != byPid_.end();    // 父行确实存在才隐藏
        };

        for (const auto* e : all) {
            // 已被父行嵌套输出的子进程，不再作为顶层行输出
            if (isNestedChild(e)) continue;

            ViewRow r;
            r.kind = ViewRow::ProcessRow;
            // 平铺视图下 group 不再用于分组，但要用来填「类型」列，
            // 所以必须写真实分类，不能一律填 Apps。
            {
                auto itG = cls_.groupOf.find(e->pid);
                r.group = (itG == cls_.groupOf.end()) ? ProcessGroup::Background
                                                      : itG->second;
            }
            r.groupCount = 0;
            r.pid = e->pid;
            r.rootPid = e->pid;
            r.indentLevel = 0;
            r.hasChildren = false;
            r.expanded = false;
            // 是应用根时，判断是否有子进程（查预建的 kidsCount 表，O(1)）
            if (cls_.appRoots.count(e->pid)) r.hasChildren = hasKids(e->pid);
            r.expanded = r.hasChildren && IsAppExpanded(e->pid);
            rows_.push_back(r);
            if (r.expanded) appendKids(e);
        }
        goto finish_rows;
    }

    for (int g = 0; g < 3; ++g) {
        // 「应用」组：只显示应用根，子进程折叠在其下
        std::vector<const ProcessEntry*> shown;
        if (g == static_cast<int>(ProcessGroup::Apps)) {
            for (const auto* e : groups[g]) {
                if (cls_.appRoots.count(e->pid)) shown.push_back(e);
            }
        } else {
            shown = groups[g];
        }
        std::sort(shown.begin(), shown.end(), cmp);

        // 【修复】之前是 `shown.empty() && groups[g].empty()`，
        // 当 shown 为空但 groups[g] 非空时（所有进程都是子进程、没有根），
        // 会画出一个只有组头、下面零行的空组。
        if (shown.empty()) continue;

        ViewRow gh;
        gh.kind = ViewRow::GroupHeaderRow;
        gh.group = static_cast<ProcessGroup>(g);
        gh.groupCount = static_cast<int>(shown.size());
        gh.pid = 0; gh.rootPid = 0; gh.indentLevel = 0;
        gh.hasChildren = true; gh.expanded = groupExpanded_[g];
        rows_.push_back(gh);

        if (!groupExpanded_[g]) continue;

        for (const auto* e : shown) {
            ViewRow r;
            r.kind = ViewRow::ProcessRow;
            r.group = static_cast<ProcessGroup>(g);
            r.groupCount = 0;
            r.pid = e->pid;
            r.rootPid = e->pid;
            r.indentLevel = 0;
            r.hasChildren = false;
            r.expanded = false;

            // 应用组：判断有无子进程，决定是否画展开箭头（查表 O(1)）
            if (g == static_cast<int>(ProcessGroup::Apps)) {
                r.hasChildren = hasKids(e->pid);
            }
            r.expanded = r.hasChildren && IsAppExpanded(e->pid);
            rows_.push_back(r);

            // 应用行展开时，把子进程按当前排序列插在它下面，缩进一级。
            // 子进程的 hasChildren 恒为 false —— 层级再深在任务管理器里
            // 也是平铺的，原版只展开一层。
            //
            // 【性能修复】原来是嵌套循环：外层遍历 appRootOf、内层遍历
            // snap_->processes，是 O(appCount × procCount)。拖滚动条时
            // WM_MOUSEMOVE 每像素触发一次 RebuildRows，会明显卡顿。
            // 改用一次遍历建 pid->entry 索引。
            // （byPid_ 已在 RebuildRows 开头无条件建好，这里不再惰性构建。）
            if (r.expanded) {
                std::vector<const ProcessEntry*> kids;
                for (const auto& kv : cls_.appRootOf) {
                    if (kv.second != e->pid || kv.first == e->pid) continue;
                    auto itK = byPid_.find(kv.first);
                    if (itK != byPid_.end()) kids.push_back(itK->second);
                }
                std::sort(kids.begin(), kids.end(), cmp);
                for (const auto* ce : kids) {
                    ViewRow kr;
                    kr.kind = ViewRow::ProcessRow;
                    kr.group = (cls_.groupOf.count(ce->pid) ? cls_.groupOf.at(ce->pid)
                                 : ProcessGroup::Background);
                    kr.groupCount = 0;
                    kr.pid = ce->pid;
                    kr.rootPid = e->pid;       // 归属的应用根，用于图标回退
                    kr.indentLevel = 1;
                    kr.hasChildren = false;
                    kr.expanded = false;
                    rows_.push_back(kr);
                }
            }
        }
    }

    finish_rows:

    // 限制滚动范围
    // 【子行 22】总高必须逐行取实际行高（组头/父行 28、子行 22）。
    // 与 ContentHeight() / PaintRows 的累加保持同一口径，否则最后一屏
    // 会有多出或不足的空白。
    int bodyH = rcList_.bottom - rcList_.top;
    int totalH = 0;
    for (const auto& r : rows_) {
        totalH += RowHeightFor(r.kind == ViewRow::GroupHeaderRow, r.indentLevel);
    }
    int maxScroll = (std::max)(0, totalH - bodyH);
    if (scrollY_ > maxScroll) scrollY_ = maxScroll;
    if (scrollY_ < 0) scrollY_ = 0;

    // 按 pid 重新定位选中行（防止列表刷新后选中跑到别的进程上）
    SyncSelectionToPid();
}

// 选中项跟随 pid。
//
// 列表每秒重建，行下标不稳定：进程退出、排序变化、分组折叠都会让
// 同一个下标指向不同的进程。用户选中某进程后点「结束任务」，
// 中途刷新一次就可能杀掉错的进程。
//
// 这里按 pid 找回行号；pid 已消失则清除选中（这是安全的行为 ——
// 宁可没有选中，也不要让操作落到错误的目标上）。
void TmWindow::SyncSelectionToPid() {
    selectedRow_ = -1;
    if (selectedPid_ == 0) return;

    for (size_t i = 0; i < rows_.size(); ++i) {
        const ViewRow& r = rows_[i];
        if (r.kind == ViewRow::ProcessRow && r.pid == selectedPid_) {
            selectedRow_ = static_cast<int>(i);
            return;
        }
    }

    // 行里找不到。要区分两种情况：
    //   1. 进程真的退出了 → 清除选中（避免操作落到错误目标）
    //   2. 只是所在分组被折叠了 → **保留 selectedPid_**，
    //      这样展开分组后选中还能回来（原版行为）
    //
    // 之前版本一律清除，导致「选中一个后台进程 → 折叠该组 → 展开 → 选中没了」。
    bool stillAlive = false;
    if (snap_) {
        for (const auto& e : snap_->processes) {
            if (e.pid == selectedPid_) { stillAlive = true; break; }
        }
    }
    if (!stillAlive) selectedPid_ = 0;
    // 还活着但看不到（被折叠），保留 pid，等展开后自然找回
}

// ============ 绘制辅助 ============

// ---- 画笔 / 画刷缓存 ----
// 见 include/tmwindow.h 里 penCache_/brushCache_ 的注释。
HPEN TmWindow::CachedPen(COLORREF c) {
    auto it = penCache_.find(c);
    if (it != penCache_.end()) return it->second;
    HPEN p = CreatePen(PS_SOLID, 1, c);
    if (p) penCache_.emplace(c, p);   // 不缓存 NULL，见 perfpage.cpp 同名注释
    return p;
}

HBRUSH TmWindow::CachedBrush(COLORREF c) {
    auto it = brushCache_.find(c);
    if (it != brushCache_.end()) return it->second;
    HBRUSH b = CreateSolidBrush(c);
    if (b) brushCache_.emplace(c, b);   // 不缓存 NULL
    return b;
}

void TmWindow::ReleaseGdiCaches() {
    for (auto& kv : penCache_)   if (kv.second) DeleteObject(kv.second);
    for (auto& kv : brushCache_) if (kv.second) DeleteObject(kv.second);
    penCache_.clear();
    brushCache_.clear();
}

void TmWindow::FillSolid(HDC hdc, const RECT& rc, COLORREF c) {
    // 【性能】原来每次 CreateSolidBrush + DeleteObject。本函数每帧几十次。
    HBRUSH b = CachedBrush(c);
    if (!b) return;   // FillRect 传 NULL 不报错、只是不填充
    FillRect(hdc, &rc, b);
}

void TmWindow::DrawTextClipped(HDC hdc, const wchar_t* s, RECT* rc, UINT fmt, COLORREF color) {
    if (!s || !*s) return;
    SetTextColor(hdc, color);
    int oldMode = GetBkMode(hdc);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, s, -1, rc, fmt | DT_NOPREFIX);
    SetBkMode(hdc, oldMode);
}

void TmWindow::DrawHLine(HDC hdc, int x1, int x2, int y, COLORREF c) {
    HPEN p = CachedPen(c);
    HGDIOBJ old = SelectObject(hdc, p);
    MoveToEx(hdc, x1, y, nullptr);
    LineTo(hdc, x2, y);
    SelectObject(hdc, old);
}

void TmWindow::DrawVLine(HDC hdc, int x, int y1, int y2, COLORREF c) {
    HPEN p = CachedPen(c);
    HGDIOBJ old = SelectObject(hdc, p);
    MoveToEx(hdc, x, y1, nullptr);
    LineTo(hdc, x, y2);
    SelectObject(hdc, old);
}

void TmWindow::DrawFrame(HDC hdc, const RECT& rc, COLORREF c) {
    DrawHLine(hdc, rc.left, rc.right, rc.top, c);
    DrawHLine(hdc, rc.left, rc.right, rc.bottom - 1, c);
    DrawVLine(hdc, rc.left, rc.top, rc.bottom, c);
    DrawVLine(hdc, rc.right - 1, rc.top, rc.bottom, c);
}

void TmWindow::EnsureBackBuffer(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (w == memW_ && h == memH_ && memDC_ && memBmp_) return;

    // 【P0 位图泄漏修复 · 最终版】
    //
    // 位图被 SelectObject 进 DC 之后，DeleteObject 会**静默失败**，
    // 于是每次重建后备缓冲就泄漏一张 w×h 的位图（1084×661 ≈ 2.8 MB）。
    //
    // 要删一个"被选进 DC"的位图，必须先用**另一个**位图把它换出来。
    // 关键点：那个"另一个"不能每次新建 —— 新建的自己也删不掉（同样被选中），
    // 循环下去每次重建泄漏一个 1×1。上一版就是这么错的。
    //
    // 正确做法：记住 CreateCompatibleDC 时 DC 里原有的那张 stock 位图
    // （memOldBmp_），换出时选回它。stock 对象属于系统、由 DC 自己管，
    // 我们不需要也无法删除它。这样每次重建只动两个我们自己的位图，
    // 没有中间产物。
    //
    // 与 src/tmwindow_offscreen.cpp 的写法一致（那里用 oldBmp 换出再删）。
    if (memDC_ && memBmp_) {
        if (memOldBmp_) SelectObject(memDC_, memOldBmp_);   // memBmp_ 脱离 DC
        DeleteObject(memBmp_);                              // 现在能删成功了
        memBmp_ = nullptr;
    }
    if (memDC_) { DeleteDC(memDC_); memDC_ = nullptr; memOldBmp_ = nullptr; }

    HDC screen = GetDC(hwnd_);
    if (!screen) return;
    memDC_ = CreateCompatibleDC(screen);
    memBmp_ = CreateCompatibleBitmap(screen, w, h);
    ReleaseDC(hwnd_, screen);
    if (memDC_ && memBmp_) {
        memOldBmp_ = SelectObject(memDC_, memBmp_);   // 记下 DC 原有的 stock 位图
        SetBkMode(memDC_, TRANSPARENT);
        memW_ = w; memH_ = h;
    } else {
        if (memBmp_) { DeleteObject(memBmp_); memBmp_ = nullptr; }
        if (memDC_) { DeleteDC(memDC_); memDC_ = nullptr; }
        memOldBmp_ = nullptr;
        memW_ = memH_ = 0;
    }
}

void TmWindow::InvalidateAll() {
    if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
}

} // namespace tmp
