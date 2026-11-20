// tmwindow_toolbar.cpp - 底部工具条
//
// Windows 10 任务管理器在每个页面底部有一条工具条，内容和当前页有关：
//   进程页     简略信息(D)                        结束任务(E)
//   性能页     简略信息(D)   打开资源监视器
//   服务页     简略信息(D)   打开服务(V)
//   用户页     简略信息(D)   断开连接(C)
//   详细信息   简略信息(D)                        结束任务(E)
//
// 每个「按钮」都是无边框文字 + 左侧图标：
//   - 「简略信息」前是一个圆圈内 ^ 的小图标（原版用来折叠摘要视图）
//   - 「打开资源监视器」前是资源监视器程序的彩色小图标（**不是放大镜**）
//   - 「打开服务」前是放大镜图标，文字是**蓝色**
//   - 「结束任务」「断开连接」有 1px 灰边框 + 灰字（禁用态）
//
// ===========================================================================
// 【几何与配色的依据 —— 本轮整段重写】
//
// 本文件此前的常量全部以 truth1.png 为基准反解。该截图经复核已确认
// **是我方自己程序的截图**，不是原版任务管理器：标题栏与
// p0_processes.png 逐字节相同、标题栏里没有标题文字、窗口图标是
// IDI_APPLICATION、页签只有 5 个、底部是我方的「进程数: …」状态串。
// 详见 docs/specs/_FINAL_VERDICT.md。所以那些按截图标定的客户区
// 绝对坐标全部作废，本文件不再引用任何截图。
//
// 现在每个数值只允许来自下面三类来源，并逐个在注释里注明：
//   1.【系统API实测】本机 Win10 22H2 19045 / 96 DPI 下 GetSystemMetrics /
//     GetThemePartSize / GetThemeMargins / GetTextMetricsW / GetSysColor
//     的返回值。原始数据见 docs/specs/R_sysmetrics.md、T_uxspec.md。
//   2.【微软规范】docs.microsoft.com/windows/win32/uxguide 正文给出的 rp 值
//     （命令按钮 75x23 rp、相关控件间 7 rp、对话框边距 11 rp…）。
//   3.【推导】由 1、2 算出的值，推导式就写在常量上方。
// 三类都不成立的，标【无依据，沿用旧值】并写明它只是手感值。
//
// 布局是**相对布局**：横向由「窗口内边距 + 按钮内边距 + 图标槽 + 图标文字
// 间距」逐级累加，纵向由「工具条中线 ± 内容高的一半」得到。全文没有任何
// 写死的客户区绝对坐标。
// ===========================================================================

#include "tmwindow.h"

#include <windowsx.h>
#include <shellapi.h>

#pragma comment(lib, "shell32.lib")

namespace tmp {

using namespace theme;

namespace {

// ---------------------------------------------------------------------------
// 尺寸基准
// ---------------------------------------------------------------------------

// 正文文字行高。【系统API实测】fontBody_ 是 theme::kFontSizeBody(12) 的
// Microsoft YaHei UI，GetTextMetricsW(lfMessageFont @ lfHeight=-12) 返回
// tmHeight = 17（R_sysmetrics.md §2.3）。本文件所有纵向几何都以它为基准。
constexpr int kTextHeight = 17;

// 小图标边长。【系统API实测】GetSystemMetrics(SM_CXSMICON) = 16（R_sysmetrics.md
// §4.2）；【微软规范】UX 指南 cmd-toolbars：「工具条图标 16x16 rp」（T_uxspec.md §3）。
constexpr int kIconBoxSize = 16;

// 1px 细线。【系统API实测】GetSystemMetrics(SM_CXBORDER) = 1，且不随 DPI 缩放
// （R_sysmetrics.md §4.2、T_uxspec.md §7.4）—— 细线不要乘 DPI。
constexpr int kLineW = 1;

// ---------------------------------------------------------------------------
// 间距：全部取微软 Win32 UX 指南的 rp 间距集
// ---------------------------------------------------------------------------
// 【微软规范】uxguide/vis-layout 的间距表（T_uxspec.md §7.2 转载原表）：
//   相关控件间 4 DLU = 7 rp，不相关控件间 7 DLU = 11 rp，对话框边距 7 DLU = 11 rp。
// 注意：4/8/12 的栅格是 **UWP/XAML 的 epx 规定，不是 Win32 规则**
// （T_uxspec.md §7.2 对此有专门纠正，官方 Win32 间距集是 5/7/11/13/16/17/18/23），
// 所以这里用 7 / 11，而不是 8 / 12。
constexpr int kSpaceRelated   = 7;   // 相关控件间（图标↔文字、按钮内边距）
constexpr int kSpaceUnrelated = 11;  // 不相关控件间 / 窗口内容边距

// 工具条左右内边距 = 窗口内容边距 = 11。
// 【微软规范】对话框边距 7 DLU = 11 rp（同上表）—— 这是 Win32 里"内容离窗口边"
// 的官方值，工具条是内容区的下边界，取同一值。
// 【如实记录】列表自己的单元格左内缩是 theme::kCellPadLeft = 8（另一个文件），
// 所以工具条图标会比第一列文字左移 3px 的差值存在。那个 8 没有微软出处，
// 而 11 有；本轮只改本文件，不为了凑对齐去把两个都改成无依据的数。
constexpr int kToolbarPadX = kSpaceUnrelated;

// 图标按钮内边距：图标左留白 / 文字右留白。
// 【微软规范】相关控件间 7 rp —— 图标与文字同属一个按钮内的相关元素。
constexpr int kBtnPadX = kSpaceRelated;

// 图标与文字的间距。【微软规范】相关控件间 7 rp。
constexpr int kIconTextGap = kSpaceRelated;

// 图标按钮的文字左内缩 = 内边距 + 图标槽 + 图标文字间距。
// 【推导】7 + 16 + 7 = 30。
// 此前「圆圈图标用 23、16x16 图标用 30」两个值是把两种图标当成不同宽度反解
// 出来的假坐标；两者占的都是同一个 16px 槽，文字起点必须相同，故合并为一值。
constexpr int kTextLeftWithIcon = kBtnPadX + kIconBoxSize + kIconTextGap;   // = 30

// 圆圈 chevron 的圆心距按钮左边界 = 图标槽中心。【推导】7 + 16/2 = 15。
constexpr int kIconCenterX = kBtnPadX + kIconBoxSize / 2;                  // = 15

// ---------------------------------------------------------------------------
// 命令按钮（结束任务 / 断开连接）
// ---------------------------------------------------------------------------
// 文字距按钮外框的左右内缩。
// 【系统API实测】BP_PUSHBUTTON TMT_SIZINGMARGINS = L6 R6 T5 B5（R_sysmetrics.md §5.5）。
constexpr int kBtnTextInset = 6;

// 【微软规范】命令按钮最小 75 x 23 rp（uxguide/vis-layout、ctrl-command-buttons，
// 原文「75 x 23 relative pixels」，T_uxspec.md §3 引用）。
constexpr int kPushBtnMinW = 75;

// ---------------------------------------------------------------------------
// 按钮高
// ---------------------------------------------------------------------------
// 【真原版截图实测 21px】`docs/reference/original_win10.png` 的「结束任务(E)」按钮：
//   左/右边框 x=1157 / x=1239 从 y=807 贯穿到 y=827；
//   上边框整行 #ADADAD 在 y=807（x=1150..1245 命中 83 个），下边框在 y=827；
//   ⇒ 按钮外框高 = 827 − 807 + 1 = **21px**。
//   按钮内文字墨迹 y=811..823（13 行），中心 817，与按钮中心 (807+827)/2 = 817 重合
//   ⇒ 文字在按钮内垂直居中，DT_VCENTER 即可，不需要额外偏移。
//
// 推导：21 = 文字行高 17（【系统API实测】tmHeight，见 kTextHeight）+ 2 × 2。
// 上下内边距 2，与上面实测「框 21、字 17」互洽（(21−17)/2 = 2）。
//
// 【旧值 23 已废】旧推导取 BP_PUSHBUTTON TMT_CONTENTMARGINS 的 T/B = 3，
// 得 17+6 = 23，并拿"微软 UX 指南 75x23 rp"交叉验证。那是**标准控件主题值/
// 文档值**，不是原版 taskmgr 的实际渲染尺寸 —— 同类错误在本项目已犯过两次
// （kLineHeader 的 #828790、kHeaderHeight 的 24）。真原版实测 21，以实测为准。
constexpr int kBtnPadY = 2;
constexpr int kToolbarBtnHeight = kTextHeight + 2 * kBtnPadY;           // = 21

// 16x16 图标在按钮内的上边 = 中线 - 8。
// 【推导】kIconBoxSize/2 = 8。此前写死 7 是为了对上那张假截图里的墨迹 y=669。
constexpr int kIconTopOffset = kIconBoxSize / 2;                           // = 8

// ---------------------------------------------------------------------------
// 【已删除】竖分隔线常量 kSeparatorHeight
//
// 原来在这里定义 kSeparatorHeight = 18，依据是「TOOLBAR TP_BUTTON TS_TRUE = 7x18，
// 内容高即 18」。但真原版截图实测：工具条内容区 y=794..841 在 x=110..1150
// （两组按钮之间的全部空隙）**非白像素 = 0** —— 原版根本没有这条竖线。
// 常量与绘制代码一起删除，理由详见 PaintToolbar 末尾的注释。
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 图标内部形状
// ---------------------------------------------------------------------------
// 圆圈直径 = 图标槽 16 - 2 x 1px 描边 = 14，半径 7。【推导】
constexpr int kChevronRadius = (kIconBoxSize - 2 * kLineW) / 2;            // = 7

// 放大镜圆心距图标槽左边界的偏移。
// 【推导】墨迹外接框 = 半径 5 的圆 + 伸出到 (+8,+8) 的手柄，宽 13
// （x 从 cx-5 到 cx+8）。要在 16px 槽内左右各留 1px，圆心距槽左 = 1 + 5 = 6。
// 比 chevron 的圆心（槽中心 8）略偏左 2px 是**故意的**：手柄向右伸出 3px，
// 外接框才是居中的。旧代码用一个 +5 的 kMagnifierNudge 硬推，属手感值，已删除。
constexpr int kMagnifierCenterInBox = 6;

// ---------------------------------------------------------------------------
// 颜色 —— 每个都标了出处，且优先走 GetSysColor / 主题实测值
// ---------------------------------------------------------------------------

// 工具条底色 #FFFFFF。
// 【系统API实测】GetSysColor(COLOR_WINDOW) = #FFFFFF（R_sysmetrics.md §6.1）；
// 且【系统API实测】TP_BUTTON s1 NORMAL 状态**不绘制任何背景**（T_uxspec.md §3.2），
// 所以工具条透出的是父窗口底色，即白色。
constexpr COLORREF kColToolbarBg = RGB(0xFF, 0xFF, 0xFF);

// 工具条顶部分隔线 #A0A0A0。
// 【真原版截图实测】`docs/reference/original_win10.png` y=793 整行扫描，
// 1251 像素里 1246 个是 #A0A0A0，顶线 1px 贯穿整条工具条，直到窗口客户区底部
// （y=794..841 纯白内容区）⇒ 工具条高 48px（含顶线，见 theme::kToolbarHeight）。
//
// **必须换掉旧的 #E5E5E5**：那个值来自 HEADERITEM 右边线的主题取色，
// 属"拿标准控件主题值替代原版实际值"—— 与 kLineHeader 曾经犯过的是同一个错。
// 现在走 theme::kLineToolbar，语义独立于 kLineHeader（列头底线），
// 将来任一条线要改不会误伤另一条。
constexpr COLORREF kColToolbarTopLine = kLineToolbar;

// 【已删除】组间竖分隔线 kColSeparator = #D9D9D9。
// 它唯一的消费者是 PaintToolbar 里那条竖线，而真原版实测那条线不存在
// （工具条内容区 x=110..1150 非白像素 = 0）。见 PaintToolbar 末尾的说明。

// 普通按钮文字 #000000。
// 【系统API实测】GetSysColor(COLOR_BTNTEXT) = COLOR_WINDOWTEXT = #000000
// （R_sysmetrics.md §6.1），且 TP_BUTTON TMT_TEXTCOLOR = #000000（T_uxspec.md §3.2）。
// 旧值 #1A1A1A 是从假截图取的"最暗像素"。
constexpr COLORREF kColText = RGB(0x00, 0x00, 0x00);

// 链接文字（打开资源监视器 / 打开服务）#0066CC。
// 【系统API实测】GetSysColor(COLOR_HOTLIGHT) = #0066CC（R_sysmetrics.md §6.1、
// T_uxspec.md §5.1）；【微软规范】UX 指南 HyperLinkText = #0066CC（T_uxspec.md §5.4）。
// 两源一致。旧值 #0F6CBD 同样是从假截图的"最暗像素"取的。
constexpr COLORREF kColLinkText = RGB(0x00, 0x66, 0xCC);

// 命令按钮的**常态**外观：填充 #E1E1E1 / 边框 #ADADAD / 文字纯黑。
// 【真原版截图实测】docs/reference/original_win10.png
//   「结束任务(E)」按钮 y=807..827、x=1157..1229：
//     边框行 y=807 / y=827 = #ADADAD
//     填充 y=808..826      = #E1E1E1
//     文字                 = #000000 纯黑
// 这三个值正好等于 BP_PUSHBUTTON s1 NORMAL（填充 #E1E1E1、边框 #ADADAD，
// T_uxspec.md §3.1 系统API实测），即原版这两个按钮画的就是**正常态**按钮。
constexpr COLORREF kColBtnFill   = RGB(0xE1, 0xE1, 0xE1);
constexpr COLORREF kColBtnBorder = RGB(0xAD, 0xAD, 0xAD);

// 禁用命令按钮：填充 #CCCCCC / 边框 #BFBFBF / 文字 #838383。
// 【系统API实测】BP_PUSHBUTTON s4 DISABLED 的渲染值（T_uxspec.md §3.1 表）：
//   填充 #CCCCCC、边框 #BFBFBF、TMT_TEXTCOLOR #838383。
// 旧边框 #C8C8C8 无出处，已按 §3.1 改为 #BFBFBF。
// 附注：TP_BUTTON s4 的文字是 COLOR_GRAYTEXT #6D6D6D，那只适用于"工具条按钮
// 被禁用"的情形；这两个控件是真正的 pushbutton，故取 s4 的 #838383。
// 【重要】这是**禁用态**配色，只在 isDisabled 为真时使用。此前代码把
// "是命令按钮" 误当成 "禁用"，导致常态按钮画成了这套灰。已修正。
constexpr COLORREF kColBtnDisabledFill   = RGB(0xCC, 0xCC, 0xCC);
constexpr COLORREF kColBtnDisabledBorder = RGB(0xBF, 0xBF, 0xBF);
constexpr COLORREF kColBtnDisabledText   = RGB(0x83, 0x83, 0x83);

// 圆圈 chevron 的描边 #6D6D6D。
// 【系统API实测】GetSysColor(COLOR_GRAYTEXT) = #6D6D6D（R_sysmetrics.md §6.1）；
// 它也是 TP_BUTTON s4 的工具条文字灰，用于"弱化的图形元素"最合适。
// 旧值 #999999 不在规范灰阶表里（T_uxspec.md §5.3）。
constexpr COLORREF kColIconGray = RGB(0x6D, 0x6D, 0x6D);

// 「结束任务 / 断开连接」是真正的命令按钮（带边框、文字居中）；
// 「简略信息 / 打开资源监视器 / 打开服务」是无边框的图标文字按钮。
bool IsPushButton(UINT cmd) {
    return cmd == CMD_END_TASK || cmd == CMD_USR_DISCONNECT;
}

// 图标按钮的文字左内缩；命令按钮是居中排版，返回 0，由 DT_CENTER 负责。
int TextLeftFor(UINT cmd) {
    return IsPushButton(cmd) ? 0 : kTextLeftWithIcon;
}

// 放大镜图标（16x16），用矢量画，避免依赖外部资源。
// 半径 5 / 手柄 4px / 2px 笔画：【无依据，沿用旧值】—— 这是图形本身的长相，
// 不是布局几何；只要外接框仍落在 16px 槽内（见 kMagnifierCenterInBox）即可。
void DrawMagnifier(HDC hdc, int cx, int cy, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HGDIOBJ oldp = SelectObject(hdc, pen);
    HGDIOBJ oldb = SelectObject(hdc, GetStockObject(NULL_BRUSH));

    const int r = 5;
    Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
    MoveToEx(hdc, cx + 4, cy + 4, nullptr);
    LineTo(hdc, cx + 8, cy + 8);

    SelectObject(hdc, oldb);
    SelectObject(hdc, oldp);
    DeleteObject(pen);
}

// 「简略信息」前的圆圈内 ^ 图标
void DrawChevronCircle(HDC hdc, int cx, int cy, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, kLineW, color);
    HGDIOBJ oldp = SelectObject(hdc, pen);
    HGDIOBJ oldb = SelectObject(hdc, GetStockObject(NULL_BRUSH));

    const int r = kChevronRadius;
    Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);

    // ^ 字形：半宽 3、高 4。
    // 【无依据，沿用旧值】—— 圆圈直径有推导（kChevronRadius），但内部这个
    // 折线只有"在 14px 圆里还看得清"这一个约束，没有任何规范给出 chevron 的
    // 尺寸，如实标为手感值。
    MoveToEx(hdc, cx - 3, cy + 2, nullptr);
    LineTo(hdc, cx, cy - 2);
    LineTo(hdc, cx + 3, cy + 2);

    SelectObject(hdc, oldb);
    SelectObject(hdc, oldp);
    DeleteObject(pen);
}

// 「打开资源监视器」前的图标：原版是资源监视器程序的彩色图表图标
// （白底上一个彩色折线小图表），不是放大镜。
void DrawResMonIcon(HDC hdc, int x, int y) {
    const int w = kIconBoxSize, h = kIconBoxSize;

    // 白底。【系统API实测】GetSysColor(COLOR_WINDOW) = #FFFFFF（R_sysmetrics.md §6.1）。
    HBRUSH bg = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
    RECT rc{ x, y, x + w, y + h };
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    // 边框 #ADADAD = 【系统API实测】BP_PUSHBUTTON s1 NORMAL 的按钮描边
    // （T_uxspec.md §3.1 / §8.4 kBorderStrong）。旧值 #888888 不在规范灰阶表里。
    HPEN pen = CreatePen(PS_SOLID, kLineW, RGB(0xAD, 0xAD, 0xAD));
    HGDIOBJ oldp = SelectObject(hdc, pen);
    HGDIOBJ oldb = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, x, y, x + w, y + h);
    SelectObject(hdc, oldb);
    SelectObject(hdc, oldp);
    DeleteObject(pen);

    // 内部彩色折线。三色直接取项目图表调色板（theme.h 的 kChartLineCpu /
    // kChartLineMem / kChartLineDisk），它们来自 aero.msstyles 的 CHARTVIEW
    // 主题类 part 1-28（T_uxspec.md §6.2），是真正有出处的系列色，
    // 比此前手写的 RGB(0xD03030)/RGB(0x30A030)/RGB(0x3060C0) 可靠。
    // 折线点位是图标本身的长相：【无依据，沿用旧值】，只需保证落在 16x16 内。
    const struct { COLORREF c; POINT pts[5]; } lines[] = {
        { kChartLineCpu,  {{2,11},{5,7},{8,9},{11,4},{14,6}} },
        { kChartLineDisk, {{2,13},{5,11},{8,12},{11,9},{14,10}} },
        { kChartLineMem,  {{2,9},{5,10},{8,6},{11,7},{14,3}} },
    };
    for (const auto& l : lines) {
        HPEN lp = CreatePen(PS_SOLID, kLineW, l.c);
        HGDIOBJ op = SelectObject(hdc, lp);
        for (int i = 0; i < 5; ++i) {
            int px = x + l.pts[i].x;
            int py = y + l.pts[i].y;
            if (i == 0) MoveToEx(hdc, px, py, nullptr);
            else        LineTo(hdc, px, py);
        }
        SelectObject(hdc, op);
        DeleteObject(lp);
    }
}

} // namespace

void TmWindow::BuildToolbarButtons() const {
    toolbarButtons_.clear();
    if (rcToolbar_.right <= rcToolbar_.left) return;

    const int cy = (rcToolbar_.top + rcToolbar_.bottom) / 2;
    int x = rcToolbar_.left + kToolbarPadX;

    auto Measured = [&](const wchar_t* text) {
        HDC hdc = GetDC(hwnd_);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, fontBody_));
        SIZE sz{};
        GetTextExtentPoint32W(hdc, text, static_cast<int>(wcslen(text)), &sz);
        SelectObject(hdc, old);
        ReleaseDC(hwnd_, hdc);
        return sz.cx;
    };

    auto Add = [&](const wchar_t* text, UINT cmd, bool rightSide) {
        const int textW = Measured(text);
        int w;
        if (IsPushButton(cmd)) {
            // 命令按钮：左右各留 kBtnTextInset，且不小于规范最小宽 75。
            w = 2 * kBtnTextInset + textW;
            if (w < kPushBtnMinW) w = kPushBtnMinW;
        } else {
            // 图标按钮：内边距 + 图标槽 + 图标文字间距 + 文字 + 右内边距。
            w = kTextLeftWithIcon + textW + kBtnPadX;
        }
        const int h = kToolbarBtnHeight;

        RECT r{};
        if (rightSide) {
            // 右组以客户区右界为锚，向左生长。
            r.left   = rcToolbar_.right - kToolbarPadX - w;
            r.right  = rcToolbar_.right - kToolbarPadX;
        } else {
            r.left   = x;
            r.right  = x + w;
            x = r.right + kSpaceUnrelated;
        }
        r.top    = cy - h / 2;
        r.bottom = r.top + h;
        toolbarButtons_.push_back({ r, text, cmd });
    };

    // 左侧：简略信息 / 详细信息（所有页都有）
    // 【原版是两个互斥文案，不是一个开关】MUI 里成对存在
    //   37212 `简略信息(&D)` / 37211 `详细信息(&D)`（O_strings.md:447/522）。
    // 原版点它切到 UIFILE 30024 的老式简略视图（`taskman` 根 +
    // GenericCommandBar），再点一次切回 30001。所以按钮文字要随状态变，
    // 否则用户点完看不到任何反馈 —— 这正是它长期是死按钮的原因。
    Add(simpleView_ ? L"详细信息(&D)" : L"简略信息(&D)", CMD_VIEW_SIMPLE, false);

    // 右侧：随页面变化
    switch (tabs_.Current()) {
    case TabId::Performance:
        Add(L"打开资源监视器", CMD_OPEN_RESMON, true);
        break;
    case TabId::Services:
        Add(L"打开服务(&V)", CMD_SVC_OPEN_SCM, true);
        break;
    case TabId::Users:
        Add(L"断开连接(&C)", CMD_USR_DISCONNECT, true);
        break;
    case TabId::Processes:
    case TabId::Details:
        Add(L"结束任务(&E)", CMD_END_TASK, true);
        break;
    default:
        break;
    }
}

void TmWindow::PaintToolbar(HDC hdc) {
    BuildToolbarButtons();

    // 工具条：白底 + 一条 1px 顶部分隔线。
    //   - 底色 #FFFFFF：【系统API实测】COLOR_WINDOW，且 TP_BUTTON s1 不绘制背景，
    //     工具条透出父窗口底色（见 kColToolbarBg 注释）。
    //   - 顶线 #A0A0A0：【真原版截图实测】y=793 整行 1246/1251 像素为该色。
    //     旧值 #E5E5E5 来自 HEADERITEM 右边线的主题取色，已作废
    //     （见 kColToolbarTopLine 注释）。
    //   - 整条高 48px（含顶线）：theme::kToolbarHeight，原版 y=793..841。
    FillSolid(hdc, rcToolbar_, kColToolbarBg);
    DrawHLine(hdc, rcToolbar_.left, rcToolbar_.right, rcToolbar_.top,
              kColToolbarTopLine);

    const int cy = (rcToolbar_.top + rcToolbar_.bottom) / 2;

    HFONT old = static_cast<HFONT>(SelectObject(hdc, fontBody_));
    SetBkMode(hdc, TRANSPARENT);

    for (const auto& b : toolbarButtons_) {
        const bool isLink     = (b.cmd == CMD_OPEN_RESMON || b.cmd == CMD_SVC_OPEN_SCM);
        // 【曾经的逻辑错误】这里原本写的是 `IsPushButton(b.cmd)`，即把
        // "这是个带边框的命令按钮" 误当成 "它是禁用态"。于是「结束任务」和
        // 「断开连接」永远画成灰底灰字的禁用外观。
        //
        // 真原版实测（docs/reference/original_win10.png，x=1157..1229 y=807..827）：
        //   填充 #E1E1E1、边框 #ADADAD、文字 #000000  ← 这是**可用态**外观
        // 即原版这两个按钮平时就是浅灰填充 + 深灰边框的正常按钮，
        // 不是"禁用"的视觉。
        //
        // 现在拆成两个概念：
        //   isPushButton —— 决定要不要画边框填充（语义：命令按钮）
        //   isDisabled   —— 真正的禁用态（当前没有触发条件，保留接口）
        const bool isPushButton = IsPushButton(b.cmd);
        const bool isDisabled   = false;   // 暂无禁用逻辑；将来接上时改这里

        // 文字色：
        //   链接      COLOR_HOTLIGHT #0066CC（系统API实测，见 kColLinkText）
        //   真禁用    BP_PUSHBUTTON s4 的 #838383（见 kColBtnDisabledText）
        //   命令按钮  原版实测纯黑 #000000
        //   普通      COLOR_BTNTEXT  #000000（系统API实测，见 kColText）
        COLORREF textColor = isLink     ? kColLinkText
                           : isDisabled ? kColBtnDisabledText
                                        : kColText;

        // 命令按钮的常态外观（原版实测）：填充 #E1E1E1 + 边框 #ADADAD。
        // 禁用态另有配色（#CCCCCC/#BFBFBF），只在 isDisabled 时用。
        if (isDisabled) {
            FillSolid(hdc, b.rc, kColBtnDisabledFill);
            DrawFrame(hdc, b.rc, kColBtnDisabledBorder);
        } else if (isPushButton) {
            FillSolid(hdc, b.rc, kColBtnFill);
            DrawFrame(hdc, b.rc, kColBtnBorder);
        }

        // 图标：一律画在以「按钮左边界 + kBtnPadX」为左沿的 16x16 槽里。
        if (b.cmd == CMD_VIEW_SIMPLE) {
            DrawChevronCircle(hdc, b.rc.left + kIconCenterX, cy, kColIconGray);
        } else if (b.cmd == CMD_OPEN_RESMON) {
            // 资源监视器的真实图标（彩色小图表），不是放大镜
            DrawResMonIcon(hdc, b.rc.left + kBtnPadX, cy - kIconTopOffset);
        } else if (isLink) {
            // 圆心在图标槽内的相对偏移，故要加上槽的左沿 kBtnPadX。
            DrawMagnifier(hdc, b.rc.left + kBtnPadX + kMagnifierCenterInBox, cy,
                          kColLinkText);
        }

        // 文字：图标按钮左对齐于 kTextLeftWithIcon，命令按钮居中。
        RECT tr = b.rc;
        tr.left += TextLeftFor(b.cmd);
        SetTextColor(hdc, textColor);
        DrawTextW(hdc, b.text.c_str(), -1, &tr,
                  DT_SINGLELINE | DT_VCENTER | (IsPushButton(b.cmd) ? DT_CENTER : DT_LEFT)
                  | DT_NOPREFIX);
    }

    // 【已删除 · 假功能】左右两组之间的竖分隔线。
    //
    // 原版**没有**这条线。`docs/reference/original_win10.png` 的工具条内容区
    // y=794..841，逐行扫 x=110..1150（两组按钮之间的全部空隙），
    // **非白像素 = 0**，一行都没有。也就是说「简略信息(D)」与「结束任务(E)」
    // 之间是纯白，我方却画了一条 #D9D9D9 竖线 —— 一个肉眼可见的假元素。
    //
    // 旧依据是「TOOLBAR TP_SEPARATOR」这个标准控件 part，属"标准控件有 → 原版也该有"
    // 的同一类错误（本文件已经因为这种推论踩过 #E5E5E5 顶线、23px 按钮高两次）。
    //
    // 因此连同以下的常量一起删除：kSeparatorHeight、kColSeparator。
    // 将来若在别的页（性能页「打开资源监视器」）拿到"确有分隔线"的实测证据再加回来。

    SelectObject(hdc, old);
}

// 工具条点击命中测试。返回命中的命令 ID，0 表示没命中。
UINT TmWindow::HitTestToolbar(int x, int y) {
    if (y < rcToolbar_.top || y >= rcToolbar_.bottom) return 0;
    BuildToolbarButtons();
    for (const auto& b : toolbarButtons_) {
        if (x >= b.rc.left && x < b.rc.right && y >= b.rc.top && y < b.rc.bottom) {
            return b.cmd;
        }
    }
    return 0;
}

} // namespace tmp
