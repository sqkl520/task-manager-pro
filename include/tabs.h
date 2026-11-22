// tabs.h - Win10 任务管理器的顶部横向标签栏
//
// ============================================================================
// 【形态结论：Win10 Win32 TabControl 是「带边框方块」，不是下划线】
//
// 依据（全部是系统 API 实测，出处 docs/specs/T_uxspec.md §2.4，非印象）：
//   1. 主题为每个 TABITEM 提供**四向可拉伸边框**图元（TABITEMBOTHEDGE /
//      TABITEMLEFTEDGE / TABITEMRIGHTEDGE / TOPTABITEM* 共 8 个变体）——
//      下划线方案只需要一条横线图元，不需要左右边缘 part。
//   2. TABITEM 有 TMT_SIZINGMARGINS = L2 R2 T13 B4（右侧 2px 是可拉伸边框带），
//      且 TMT_BORDERSIZE = 0（边框由位图画，不是 1px 描边）。
//   3. 离屏渲染（DrawThemeBackground 到 DIB 后取样）TABITEM s1 = #F0F0F0 填充
//      + #D9D9D9 边框，是一块**有边界的面**，不是一条线。
//   4. TOPTABITEM 与 TABITEM 的度量完全相同（5x19, L2 R2 T13 B4），只是朝向不同。
//
// 下划线（underline indicator）是 WinUI / UWP **Pivot** 的范式，不是 Win32
// TabControl 的范式。微软官方从未文档化「Win10 页签改成了下划线」。
// → 复刻 Win10 任务管理器就用**带边框方块**。
//
// 【历史教训 —— 保留，以免重犯】
// 本文件曾一度把页签改成「纯白底 + 活动页签 2px 蓝下划线」，依据是
// `m3.png`。事后查明 m3.png 是**本项目自己旧版本的运行截图**：
// 底部有「进程数: 562  CPU 95%  内存 …」这行状态栏，那正是
// src/tmwindow_list.cpp 里 PaintStatusBar 的格式串，MUI 资源里根本没有
// 「进程数」这个词；蓝下划线也是我方旧版自己画的。
// 教训：**截图必须能证明来源，否则不能当权威**。本文件此后不再把任何
// 截图当作几何/配色的依据，只引用系统 API 实测与微软规范。
//
// 【页签个数 = 7，依据是 MUI 字符串资源，不是截图】
// 本机 Client MUI 的字符串表里 7 个标题齐全：
//   32400 详细信息 / 32401 服务 / 32402 性能 / 32403 进程 /
//   32404 启动 / 32405 用户 / 32406 应用历史记录
//   出处：LoadLibraryExW(LOAD_LIBRARY_AS_DATAFILE) + LoadStringW 直接读
//         zh-CN\taskmgr.exe.mui 实测（原始码点见 docs/project_rules.md 记录）。
//   【注意】docs/specs/O_strings.md 的编号整体偏 +1，是二手转录偏移，勿再引用。
// 之前「只有 5 个」的判断来自一张无法证明来源的截图，已推翻。
// ============================================================================
#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace tmp {

enum class TabId {
    Processes = 0,
    Performance,
    AppHistory,
    Startup,
    Users,
    Details,
    Services,
    Count
};

inline const wchar_t* TabTitle(TabId t) {
    switch (t) {
    case TabId::Processes:   return L"进程";
    case TabId::Performance: return L"性能";
    case TabId::AppHistory:  return L"应用历史记录";
    case TabId::Startup:     return L"启动";
    case TabId::Users:       return L"用户";
    case TabId::Details:     return L"详细信息";
    case TabId::Services:    return L"服务";
    default:                 return L"";
    }
}

// 标签栏的视觉规格。
// 每条都标了依据等级：
//   【系统API实测】     = GetThemePartSize / GetThemeMargins / GetSysColor / 离屏渲染取样
//   【微软规范】        = 微软官方 UX 指南有明确数值
//   【规范未规定，按XX推导】 = 上面两者都没有，写明推导式
//   【无依据，沿用旧值】 = 既无 API 也无规范，保留现值并标明
namespace tabtheme {

// ---------- 几何 ----------
// 页签总高。规范没有直接给，只能合成为：tmHeight + T + B
//   T/B 来自 TABITEM 的 TMT_SIZINGMARGINS = L2 R2 **T13 B4**
//   【系统API实测】GetThemeMargins(TAB, TABITEM, TIS_NORMAL, TMT_SIZINGMARGINS)
// 注意 T13/B4 是**可拉伸边框区**（顶部 13px 是圆角+高光带），不是文字内边距。
// 保留它们只是为了在 UIFILE 值不可用时有个兜底合成式（见 MeasureHeight）。
//
// 【单位】本 namespace 的 int 几何常量都是 **96 DPI 基准 px**（= UIFILE 的 rp 原样）。
// 主题的 TABITEM 图元尺寸不随 DPI 变化（Aero 只有 NormalSize 变体，
// docs/specs/R_sysmetrics.md §5.4），所以 125%/150% 下必须自己乘。
// 缩放已收敛在 tabs.cpp 的 MeasureHeight() / RecomputeWidths() 两个出口，
// 调用方（tmwindow.cpp 的 BuildLayout）拿到的就是当前 DPI 下的 px，不要重复乘。
constexpr int kPadTop      = 13;   // 【系统API实测】TABITEM SIZINGMARGINS.T
constexpr int kPadBottom   = 4;    // 【系统API实测】TABITEM SIZINGMARGINS.B

// 参考字体的 tmHeight：12px「Microsoft YaHei UI」= 17
// 【系统API实测】SPI_GETNONCLIENTMETRICS 的 lfMessageFont(lfHeight=-12) +
// CreateFontIndirectW + GetTextMetricsW → tmHeight 17（ascent 13 / descent 4）
// 同表（-15 → 20、-18 → 24），见 docs/specs/R_sysmetrics.md §2.3.2
constexpr int kTextHeightRef = 17;

// 页签高 = 30px。
// 【UIFILE 声明 30rp】证据：原版 taskmgr.exe 的 DUI 资源里
//     tab  style  height = 30rp      （30001 属性 token @0x1cc）
//     tab  style  padding = rect(0,2,0,0)   （@0x1d0）
//     tab  style  borderthickness = rect(0,0,0,0)   （@0x1c8）
//   UIFILE 是原版程序的声明式布局，优先级高于任何推导式。
//   本机 100% DPI，1rp = 1px，故 30rp = 30px。
//   125%/150% 下由 MeasureHeight() 内部乘 dpi/96（30rp → 37/45px），
//   调用方不要自己再乘一次。
//
//   ⚠【依据降级 · 2026-11 独立复核】数值 30 仍然正确，但归因要说清：
//   `sheet="tab"` 这个样式在 30001 里出现 **0 次**，`class="WdcTab"` 也是 0 次，
//   30001 里**没有任何 tab 控件实例**（它按 4 个 *tab_graphs / *tab_tables 平铺，
//   页签容器由宿主 C++ 端绘制）。`sheet="tab"` 只在 30024 里出现 1 次（atom(tabctrl)）。
//   所以「tab 样式 30rp」证明的是**这个样式值**，不等于「原版页签高 30」这一结论的
//   完整证据链 —— 后者更可能由宿主代码或 30024 决定。
//   见 docs/specs/ZZ_uifile_audit.md §4.3 注 1。
//
// 【为什么不再是 17+13+4=34】那是"用主题 TABITEM 的可拉伸边框区拼高度"的推算法，
//   在 UIFILE 面前是多余的一层猜测。34 比原版高 4px，7 个页签叠起来观感会偏松。
//   30 才是原版真实值。
constexpr int kHeight      = 30;

// 活动页签向上凸出的高度（px）。
// 【真原版截图实测】docs/reference/original_win10.png：
//   活动页签「进程」顶边 y=54，非活动页签顶边 y=56 → **凸出 2px**
//   两者底边都在 y=78，但活动页签底部不封口，与内容区连通。
// 这是"标签页"层次感的核心：当前项从一排平齐的方块里跳出来。
// 底色/边框/断开底线 单独做还不够 —— 顶边不抬高就还是一排按钮。
constexpr int kActiveRaise = 2;

// 按**实际页签字体**重算页签高：tmHeight + 13 + 4。
// 存在的理由：页签字体来自 theme::kFontSizeNav（当前 13px → tmHeight 19 → 36px）。
// 把高度绑定到真实字体，避免「字体改了、页签高度不跟着改、文字被裁」。
// 取不到 metrics 时回退 kHeight。实现在 tabs.cpp。
int MeasureHeight(HFONT font);

// 页签左右内边距。
//
// 【真原版截图实测：每边 7px】
// 原版 docs/reference/original_win10.png 五个页签的实测数据：
//   页签      宽   文字宽   左留白  右留白
//   进程      39    24       8       7
//   性能      34    24       5       5
//   用户      36    23       7       6
//   详细信息  60    48       7       5
//   服务      34    24       7       3
// 左右合计 10~15px，均值 12 → **每边约 6~7px**。
// 右侧偏小的那几项是被下一个页签的左边框"吃掉"了（边框共用）。
// 所以取 7 作为单边留白，页签宽 = 7 + 文字宽 + 7 = 文字宽 + 14，
// 与"性能 34 = 24+10""用户 36 = 23+13"在一个量级。
//
// 【旧值 11 作废】那是按"微软 UX 间距集里的不相关控件间距 11rp"取的，
// 属于把对话框布局规范套到页签上。实测原版用的是更紧的 7。
constexpr int kPadX        = 7;

// 页签之间紧贴，靠各自的 1px 边框分隔（TMT_BORDERSIZE=0，边框是位图；
// 我们自绘用 1px 线代替，见 tabs.cpp Paint 的说明）。
constexpr int kGap         = 0;

// ---------- 配色（TABITEM 四状态离屏渲染取样）----------
// 出处 docs/specs/U_themecolors.md §4.4 / T_uxspec.md §2.3，全部为
// DrawThemeBackground 到 32bpp DIB 后的像素结果，非主题 TMT_FILLCOLOR
// （后者是类级回退常量 #4C85C7，与实际渲染色无关，不可用）。
constexpr COLORREF kBg       = RGB(0xFF, 0xFF, 0xFF);  // 【系统API实测】TABBODY 渲染 #FFFFFF
constexpr COLORREF kActiveBg = RGB(0xFF, 0xFF, 0xFF);  // 【系统API实测】TABITEM s3 SELECTED 填充
constexpr COLORREF kInactiveBg = RGB(0xF0, 0xF0, 0xF0);// 【系统API实测】TABITEM s1 NORMAL 填充
// 悬停。旧值 #F7F7F7 是自创的（无任何来源）；spec 的 TIS_HOT 是淡蓝。
constexpr COLORREF kHoverBg  = RGB(0xD8, 0xEA, 0xF9);  // 【系统API实测】TABITEM s2 HOT 填充
constexpr COLORREF kBorder   = RGB(0xD9, 0xD9, 0xD9);  // 【系统API实测】TABITEM 全状态边框
// 文字色：主题对 TAB / TaskManager / Explorer::TabView 的 TABITEM 全部
// 返回 na（TMT_TEXTCOLOR 未定义）→ 取系统 COLOR_BTNTEXT = #000000。
// 非活动页签也用 #000000：页签的「非活动」不是「禁用」，用 COLOR_GRAYTEXT
// (#6D6D6D) 在语义上会变成「不可点」，且会引入一个规范没有规定的值。
// 两者的区分交给底色（#F0F0F0 vs #FFFFFF），这是 aero Tab 自己的做法。
constexpr COLORREF kTextOn   = RGB(0x00, 0x00, 0x00);  // 【系统API实测】COLOR_BTNTEXT
constexpr COLORREF kTextOff  = kTextOn;                // 同上（刻意同值，见上段）
// 【已停用-已删除】kUnderline：蓝下划线是 WinUI Pivot 的范式，不是 Win32
// TabControl 的；当初把它写进来依据的是 m3.png（我方旧截图）。已删除常量，
// 「带边框方块 vs 下划线」的结论保留在本文件头部注释里。
// 【已停用-已删除】kLine(#E5E5E5)：那是**列头分隔线**的颜色，不属于页签；
// 当初写进来只是为了给 m3 方案画一条通栏底线，m3 已证伪，常量全项目零引用，删除。

// 页签栏底边线（通栏）。页签自己的下边框只覆盖页签区间，右边留白处需要一条
// 收边线，否则页签带与内容区之间没有边界。
// 【规范未规定，按 TABBODY 边框推导】TABBODY 的 TMT_BORDERCOLOR 与 TABITEM
// 边框同族 #D9D9D9；渲染取样里 TABBODY 是纯 #FFFFFF（边框位图在 200x40 的
// 样本里没画出来），所以取同族边框色作为通栏收边。
constexpr COLORREF kBottomLine = kBorder;

} // namespace tabtheme

// 标签栏（无独立窗口，作为主窗口的一个绘制区域）
class TabBar {
public:
    void SetBounds(int x, int y, int w, int h);
    void SetFont(HFONT f) { font_ = f; }

    TabId Current() const { return current_; }
    void SetCurrent(TabId t);
    void SetHover(TabId t) { hover_ = t; }
    TabId Hover() const { return hover_; }

    // 命中测试。返回 Count 表示没命中。
    TabId HitTest(int x, int y) const;

    // 绘制到指定 DC
    void Paint(HDC hdc) const;

    // 每个标签的宽度（首次绘制后缓存）
    void RecomputeWidths(HDC hdc);

private:
    RECT bounds_{};
    TabId current_ = TabId::Processes;
    TabId hover_ = TabId::Count;
    HFONT font_ = nullptr;
    mutable std::vector<int> widths_;   // 每个标签宽度
    mutable bool widthsValid_ = false;
};

} // namespace tmp
