// theme.h - Windows 10 任务管理器视觉规格（几何 + 配色）
//
// ===========================================================================
// 【本文件的证据规则 —— 动任何值之前先读这一段】
//
// 项目里 truth1.png、truth2.png、m3.png、原版裁切图、crops/*.png 这些
// 所谓「参考图」经逐像素复核，**全部确认是本项目自己程序的截图**，不是原版任务管理器。
// 硬证据（详见 docs/specs/_FINAL_VERDICT.md）：
//   1) 标题栏里没有任何标题文字（原版必有「任务管理器」五个字）
//   2) 窗口图标是 IDI_APPLICATION（原版是"带白色曲线图的显示器"）
//   3) 页签只有 5 个（本机 Client 版 MUI 有 7 个标题字符串）
//   4) 底部有本文件之外代码独有的状态栏格式串「进程数: %u … CPU …% 内存 … GB」
//
// 因此：**本文件不再以任何屏幕截图作为依据。**
// 旧注释里所有「实测 m3.png / truth1.png / 逐像素实测」的说法已全部删除或改写。
//
// 每个常量后面必须带「依据等级」，只允许下面三种之一：
//
//   【系统API实测】
//       来源是 GetThemePartSize / GetSystemMetrics / SPI_GETNONCLIENTMETRICS /
//       GetThemeColor / GetSysColor / GetTextMetrics / DrawThemeBackground 渲染到
//       离屏 DIB 后的像素采样 / taskmgr.exe 内的常量表。全部为本机 96 DPI 实测。
//   【微软规范未规定，按XX推导】
//       微软文档里没有这个数，必须把推导式写出来。
//   【无依据，沿用旧值】
//       既无 API 值也无规范，只能保留；必须写明「仅为保持视觉连续」。
//
// 三份来源报告（下文的 R / T / U 就是指它们）：
//   [docs/specs/R_sysmetrics.md]  系统度量（SPI/GetSystemMetrics/GetThemePartSize/TEXTMETRIC）
//   [docs/specs/T_uxspec.md]      Win10 UX 规范（微软文档引用 + 渲染实测 + 灰阶分工）
//   [docs/specs/U_themecolors.md] TASKMANAGER / CHARTVIEW 主题取色与离屏渲染
//
// 反例（禁止再出现）：「实测 m3.png」「实测 truth1.png」「逐像素实测」
// —— 除非那个像素来自 API 渲染到离屏 DIB，而不是屏幕截图。
// ===========================================================================
#pragma once

#include <windows.h>

namespace tmp {
namespace theme {

// ===========================================================================
// 【DPI 换算 —— 先读这一段再动任何几何常量】
//
// 本文件下面**所有** int 几何常量（kRowHeight / kHeaderHeight / kTabXxx /
// kCardXxx / kIndentPerLevel …）都是 **96 DPI（100% 缩放）下的 px 值**，
// 也就是原版 UIFILE 里的 rp 值原样照抄（本机 100% ⇒ 1rp = 1px，R_sysmetrics.md §1）。
//
// 换到 125% / 150% 的机器时，这些数字**不会自动变大**，原因有两条硬证据：
//   1) 主题 part 尺寸不随 DPI 变化 —— Aero.msstyles 只有 NormalSize 一种变体，
//      GetThemePartSize 在 96/120/144/168/192 下返回完全相同（列头永远 6x24、
//      页签图元永远 5x19），见 R_sysmetrics.md §5.4。**不能指望系统帮你乘。**
//   2) UIFILE 的 rp 本来就是相对 96 DPI 的单位，DUI 运行时自己乘 dpi/96。
//
// 所以：**绘制/布局处必须自己包一层 Scaled()**。当前只做了"机制就位"，
// 没有任何绘制代码被强制改写（本机 100%，改了是纯风险零收益）。
// 完整迁移清单见 docs/specs/CC_subrow_dpi.md「任务二」。
//
//   现在（96 DPI）：Scaled(24) == 24，逐点等价，可以放心先包上。
//   将来（125%）：  Scaled(24) == 30，列头才不会比字还矮。
//
// 【已知限制 —— per-monitor】main.cpp 用的是
// SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)，
// 窗口被拖到另一块不同缩放的显示器上时 DPI 会变，而 Dpi() 是**进程级缓存**，
// 不会跟着变。真正做 per-monitor 时应该在 WM_DPICHANGED 里把新 DPI 存进成员，
// 用 GetDpiForWindow(hwnd_) 取，而不是这里的 GetDeviceCaps。见报告交接单。
// ===========================================================================

// 进程级 DPI，只取一次（GetDeviceCaps(LOGPIXELSX)，与 perfpage.cpp 现用口径一致）。
// 取不到时回退 96，避免除零/全零布局。
inline int Dpi() {
    static const int s_dpi = []() -> int {
        HDC dc = GetDC(nullptr);
        int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc) ReleaseDC(nullptr, dc);
        return dpi > 0 ? dpi : 96;
    }();
    return s_dpi;
}

// 把「96 DPI 下的 px」换算成当前 DPI 下的 px。本机 96 ⇒ 恒等。
inline int Scaled(int px96) { return MulDiv(px96, Dpi(), 96); }

// 把 rp（DUI 相对像素）换算成当前 DPI 下的 px。语义与 Scaled() 完全相同，
// 只是名字对应原版 UIFILE 的单位写法（`height = 28rp`）。
//
// ⚠ 【与 include/perfpage.h 的 tmp::Rpx 冲突】perfpage.h 在 namespace tmp 里
//   定义了 `inline int Rpx(int)`。本函数是 tmp::theme::Rpx。
//   任何**同时** `using namespace theme;` 又**不加限定**调用 `Rpx(...)` 的
//   TU 都会因二义性编译失败（using-directive 把 theme::Rpx 注入 tmp，
//   与 perfpage.h 的 tmp::Rpx 同名）。
//   解法（由 perfpage.h 的 owner 执行）：删掉 perfpage.h 里那份 Rpx 定义，
//   换成 `using theme::Rpx;`。详见报告交接单。
inline int Rpx(int rp) { return Scaled(rp); }

// ---------- 字体 ----------
//
// 【字体族】【系统API实测】本机 shell/系统 UI 字体 = Microsoft YaHei UI。
//   依据：SPI_GETNONCLIENTMETRICS 的 5 个 LOGFONTW（Caption/SmCaption/Menu/Status/
//   Message）全部是 `Microsoft YaHei UI`，lfHeight = -12、lfWeight = 400、
//   lfCharSet = 1（R §2.2）；HKCU\Control Panel\Desktop\WindowMetrics 六个字体项同值；
//   GetTextFaceW 回读一致（R §3.2）。
//   注意 lfHeight = -12 是**字符高度 12px**，不是 12pt（12pt@96dpi = 16px）。
//
// 【行高的字面下限】把该 LOGFONT 交给 GDI 回读 TEXTMETRIC：
//   tmHeight = 17、tmAscent = 13、tmDescent = 4、tmInternalLeading = 5、
//   tmExternalLeading = 0（R §2.3）。任何小于 17px 的行高都会裁字。
//   各 DPI 下 tmHeight 实测：96→17、120→20、144→24（**不是线性缩放**，别按比例乘）
//   （R §2.3.1）。
//
// 各字号（px，直接传给 lfHeight 的负值）：
constexpr int kFontSizeBody      = 12;
//   【系统API实测】= lfMessageFont.lfHeight 的绝对值（R §2.2）。列表正文/工具条用。
constexpr int kFontSizeHeader    = 12;
//   【微软规范未规定，按「主题未定义字体 → 回退系统 message font」推导】
//   主题 HEADER 的 TMT_FONT 未定义（R §5.5，GetThemeInt 返回 ERROR_NOT_FOUND），
//   按规范回退到系统 message font 的 12px（R §2.2）。
constexpr int kFontSizeNav       = 12;
//   【真原版截图实测：文字高 11px → 12px 字号】
//   原版 docs/reference/original_win10.png 页签文字墨迹高度：
//     「进程」y=60..70（11 行）、「性能」y=62..72（11 行）
//   我方 13px 字号实测文字高 12 行（y=10..21），比原版高 1px。
//   按 R_sysmetrics.md §2.3.2 的 YaHei UI 对照表，lfHeight=-12 → tmHeight=17，
//   墨迹高约 11px，与原版吻合。
//   【旧值 13 作废】那是"逐字步进反推"出来的，依据随假截图一起失效。
constexpr int kFontSizeCardTitle = 12;
//   【无依据，沿用旧值】主题 TASKMANAGER 里语义接近的是 p14 resourceTitle = -29、
//   p15 resourceName = -16（U §2.7），与我方「卡片标题/副标题」的切分不同，
//   不能直接对改。保留仅为保持视觉连续。
constexpr int kFontSizeCardValue = 21;
//   【系统API实测】主题 TASKMANAGER p19（卡片数值 dynamicData）TMT_FONT 实测
//   Microsoft YaHei UI、lfHeight = -21、weight = 400（U §2.7）。
constexpr int kFontSizeCardSub   = 11;
//   【无依据，沿用旧值】主题给的参照是 p13 行文字 = -11、p15 卡片副名 = -16（U §2.7），
//   两者语义都有偏差。保留 11 仅为保持视觉连续。
constexpr int kFontSizeGroup     = 13;
//   【无依据，沿用旧值】主题 TASKMANAGER p16（sidebarLabel / 分组头名）= -16、
//   p2 是分组头**背景**不是文字（U §2.1/§2.4），没有可直接对齐的字号。保留仅为保持视觉连续。

// 字体族：**优先外壳字体 Microsoft YaHei UI**，取不到才退回系统 message font。
//
// 【依据】R §2.2 / §3.2 —— 本机系统 UI 字体就是 Microsoft YaHei UI（中文系统）；
// 原版 taskmgr 的 MUI 字体资源里也是写死的 "Microsoft YaHei UI"
// （docs/evidence/tm/mui2.txt 第 304、366 行，全文搜不到 "Segoe UI"）。
// 两者在本机输出一致，但语义不同：原版跟的是**外壳字体**，
// 换掉系统消息字体（第三方主题/替换字体）后原版不变、我方会跟着变。
//
// 先按 YaHei UI 建，再用 GetTextFaceW 回读验证：GDI 对不存在的字体会静默替换，
// 不能靠 CreateFontIndirectW 的返回值判断。回读对不上才退 message font。
inline HFONT MakeUiFont(int px, int weight) {
    auto mk = [](LOGFONTW lf, int h, int w) -> HFONT {
        lf.lfHeight  = -h;
        lf.lfWeight  = w;
        lf.lfQuality = CLEARTYPE_QUALITY;
        return CreateFontIndirectW(&lf);
    };

    LOGFONTW shell{};
    lstrcpynW(shell.lfFaceName, L"Microsoft YaHei UI", LF_FACESIZE);
    if (HFONT f = mk(shell, px, weight)) {
        wchar_t face[LF_FACESIZE] = {};
        HDC dc = GetDC(nullptr);
        if (dc) {
            HGDIOBJ old = SelectObject(dc, f);
            GetTextFaceW(dc, LF_FACESIZE, face);
            SelectObject(dc, old);
            ReleaseDC(nullptr, dc);
        }
        if (lstrcmpiW(face, L"Microsoft YaHei UI") == 0) return f;
        DeleteObject(f);          // 实际拿到的是替换字体，丢掉
    }

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        return mk(ncm.lfMessageFont, px, weight);

    LOGFONTW fallback{};
    lstrcpynW(fallback.lfFaceName, L"Segoe UI", LF_FACESIZE);
    return mk(fallback, px, weight);
}

// ---------- 主要颜色 ----------
constexpr COLORREF kBgWindow      = RGB(255, 255, 255);
//   【系统API实测】= GetSysColor(COLOR_WINDOW) = #FFFFFF（R §6.1）；
//   主题 LVP_LISTITEM 全部 state 的 TMT_FILLCOLOR 也是 #FFFFFF（R §6.2，类级回退，
//   与上面一致，互相印证）。
constexpr COLORREF kBgAltRow      = RGB(255, 255, 255);
//   【系统API实测】Win10 列表**没有交替条纹**：LVP_LISTITEM 六个 state 的填充色全是
//   #FFFFFF（R §6.2 / U §4.1），主题层面不存在条纹色。刻意与白底同值。
constexpr COLORREF kBgHeader      = RGB(255, 255, 255);
//   【系统API实测】ItemsView::Header part 1 state HIS_NORMAL(1) 离屏渲染 = 纯 #FFFFFF
//   + 右侧 1px #E5E5E5 分隔（U §4.3）。
//   备注：GetThemeColor(HEADER, HIS_NORMAL, TMT_FILLCOLORHINT) = #F4F4F4（R §6.2），
//   那是"提示色"不是渲染结果，不要拿来当列头底色。
constexpr COLORREF kBgGroupHeader = RGB(255, 255, 255);
//   【真原版截图实测】纯白 #FFFFFF。旧值 #F2F2F2（统一灰横条）已废。
//
//   依据：`docs/reference/original_win10.png`（1251×842）逐像素扫描，
//   组头行「应用 (11)」y=136..173 的**名称列整段** x=3..355 恒为 #FFFFFF ——
//   一个非白像素都没有（复扫脚本 `docs/evidence/exec/_ii_scan.ps1` 的 y=136/165 行）。
//   第二个组头「后台进程 (105)」y=482..519 同样如此。
//
//   【为什么这张截图可以当依据】本文件头那条「不再以截图为依据」的规则，
//   废止的是 truth1.png、m3.png 那批**我方自己的程序截图**。本图用文件头
//   列的四条判据逐条复核，确认是真原版（不是复述，是重新验的）：
//     1) 标题栏有「任务管理器」五个字（y=10..30 五个字形簇）
//     2) 窗口图标是带白色曲线的显示器（不是 IDI_APPLICATION）
//     3) 页签只有 5 个：进程/性能/用户/详细信息/服务（我方 7 个）
//     4) 底部没有「进程数: %u …」状态栏，只有工具条「简略信息(D)」
//   四条全过，且与我方任何一版截图都不同 → 可作为取色依据。
//
//   【旧注释里那句「#EAD5A0 在原版截图判定里 0 命中」是错的】
//   #EAD5A0 确实上屏了，但它是**1px 竖分隔线/边框**，不是填充面（见下面的
//   kLineHeatCell）。拿它当组头底色会画成土黄横条，那不是原版。
//
//   【值列的着色不走这个常量】组头行的 CPU/内存/… 值列另有热力色，
//   见本文件末尾的 GroupHeaderHeatColor()。本常量只负责**名称列与空白区**的底。
constexpr COLORREF kRowHover      = RGB(0xE5, 0xF3, 0xFF);
//   【系统API实测】#E5F3FF。来源：DrawThemeBackground(ItemsView::ListView,
//   part 1 LVP_LISTITEM, state LISS_HOT=2) 渲染进 32bpp 离屏 DIB 后采样，
//   8000 像素全是这一个值（无渐变、无抗锯齿）（U §4.1、T §1.5）。
//   该值是半透明叠加的上屏结果：#0089FF @ 10.2% over #FFFFFF（反解，T §1.5）。
//   规范做法是画 alpha 叠加而不是写死纯色；此处保留纯色常量作为白底快捷值。
constexpr COLORREF kBgNavSelected = RGB(0xCD, 0xE8, 0xFF);
//   【系统API实测】#CDE8FF。来源同上一行，state LISS_SELECTED=3（U §4.1、T §1.5），
//   等价于 #008AFF @ 19.6% over #FFFFFF。
//   【改名说明】旧值 #A0CDF0 依据的是 m3.png（我方旧截图），已作废。
//   注意：性能页侧栏**选中卡片**是另一个色 #CCE8FF（LISS_HOTSELECTED，
//   #008CFF @ 20%），原版这两处不是一个色，不要合并。
//   下方 src/tmwindow_details.cpp 的 kDetSelBg 也是 #CDE8FF —— 现在三处口径终于一致。
constexpr COLORREF kBgCard        = RGB(250, 250, 250);
//   【无依据，沿用旧值】卡片（摘要区小卡）底色：主题 TASKMANAGER/CHARTVIEW 里没有
//   对应的面（U §3.3 扫 part 0..80 × state 0..20 × 24 属性，0 命中），
//   旧值来源是截图。保留仅为保持视觉连续。
constexpr COLORREF kBgStatusBar   = RGB(240, 240, 240);
//   【系统API实测】值等于 GetSysColor(COLOR_BTNFACE) = #F0F0F0（R §6.1）。
//   但**用途存疑**：src/tmwindow_list.cpp 的 PaintStatusBar 全项目零调用
//   （原版底部只有工具条，没有独立状态栏），rcStatus_ 也是空矩形。
//   保留只为不破坏现有编译；等 PaintStatusBar 一起删除时再删。

// ---------- 文字 ----------
constexpr COLORREF kTextPrimary   = RGB(0, 0, 0);
//   【真原版截图实测 + 系统API 双源】#000000 纯黑。旧值 #141414 已废。
//
//   截图依据：`docs/reference/original_win10.png` 逐像素取样，三行数据行的
//   名称文字各自统计「亮度 < 160 的像素」的颜色直方图：
//     y=174..201：`#000000` 出现 27 次，是**最高频**项（其余是抗锯齿边，
//                 如 #660000/#003A90/#3A90DB，都是从纯黑到暖底的过渡色）
//     y=202..229：`#000000` 出现 54 次（最高频）
//     y=230..257：`#000000` 出现 27 次
//     y=398..432（选中行）：`#000000` 出现 26 次，且最暗像素 = (0,0,0)
//   「最暗像素 = (0,0,0)」这条最关键：抗锯齿只会让文字**变浅**，绝不会比
//   真实字色更深。所以字色必然是纯黑，不是 #141414 —— 若是 #141414，
//   任何像素的通道都不可能低于 20。
//
//   API 依据（本来就已对，只是此前不敢改）：GetSysColor(COLOR_WINDOWTEXT /
//   COLOR_BTNTEXT) = #000000（R_sysmetrics.md §6.1），主题
//   TEXTSTYLE(4,0,3803) = #000000（U_themecolors.md §4.8）。
//   现在「标准值」和「真原版实测」两个独立来源一致，不再是观感调整。
//
//   【影响面】这是全项目正文色（列表文字/菜单/页签/各处标签）。
//   本项已在任务书里被指定为必须改，所以清单外的调用点也会跟着变纯黑 ——
//   这是对的：原版这几处的文字本来就是纯黑（菜单/页签文字框
//   y=52..78 实测同样 `#000000` 出现 282 次，是最高频）。
constexpr COLORREF kTextSecondary = RGB(0x5A, 0x5A, 0x5A);
//   【无依据，沿用旧值】主题里"次级文字"是 #707070
//   （TASKMANAGER p17 StaticLabel / p44 lheader，U §2.2，【系统API实测】）。
//   旧值 #5A5A5A 的来源是截图，已作废。**未改**：kTextSecondary 用在卡片副标题、
//   空状态提示等十几处，换色属观感调整，留待主线程裁决。
constexpr COLORREF kTextHeader    = RGB(0x5A, 0x5A, 0x5A);
//   【无依据，沿用旧值】主题给列头文字的是 #4C607A（蓝灰）：
//   ItemsView::Header p1 s0 TMT_TEXTCOLOR = #4C607A，TASKMANAGER p24 s1（子列头）
//   也是 #4C607A（U §2.4/§2.2，两处独立吻合，【系统API实测】）。
//   但原版 DUI 是否真的采用它无法证实（同类证据里 TASKMANAGER p13 的 #DB6B3A
//   确认未上屏）。**未改**，#5A5A5A 的来源是截图已作废，留待主线程裁决。
constexpr COLORREF kTextDisabled  = RGB(0x6D, 0x6D, 0x6D);
//   【系统API实测】#6D6D6D = GetSysColor(COLOR_GRAYTEXT)（R §6.1），
//   也是 TP_BUTTON DISABLED 的文字色（T §5.3）。
//   旧值 RGB(160,160,160) = #A0A0A0 是 GetSysColor(COLOR_BTNSHADOW)
//   —— 那是"阴影/凹陷"的灰，不是禁用文字色，属用错角色，已改。
//   备注：主题给的工具条按钮禁用文字是 #838383（gtc(Button,1,4,3803)，U §4.5），
//   比 #6D6D6D 浅；本常量用于页面占位提示等通用"不可用"文字，取系统灰。
constexpr COLORREF kTextSelected  = RGB(0, 0, 0);
//   【真原版截图实测】#000000。与 kTextPrimary 同值（选中只换背景、不改字色）。
//
//   截图依据：`docs/reference/original_win10.png` 的选中行（底色 #CDE8FF，
//   y=398..432）内取样 x=16..355，最暗像素 = **(0,0,0)**，
//   `#000000` 出现 26 次为最高频。深色底上的字反而更难被抗锯齿影响，
//   纯黑证据比普通行更硬。
//
//   API 依据（本来就有）：主题 TMT_TEXTCOLOR 对 LVP_LISTITEM 全部 state 未定义
//   （R §6.2），回退 ItemsView::ListView p1 s3 TEXT = #000000（U §2.4）。
//   → 两源一致，定案。
//
//   旧值 #141414 与 kTextPrimary 一起改，避免出现「选中后字色变浅」的错位。

// ---------- 线条 ----------
constexpr COLORREF kLineHeader    = RGB(0xA0, 0xA0, 0xA0);
//   【真原版截图逐像素实测】#A0A0A0。列头下边线。
//   依据：`docs/reference/original_win10.png`（真原版 Win10 任务管理器进程页，
//   1251×842）y=135 整行扫描，x=0..1250 **连续 1251 个像素全是 #A0A0A0**，
//   其中 x=503..1246（列头带右侧纯白区）也有 744 个 #A0A0A0 → 底线贯穿整条列头带，
//   不是只画在列下面。复核脚本 `docs/evidence/_hh_scan3.ps1` / `_hh_scan.ps1`。
//   #A0A0A0 = GetSysColor(COLOR_3DSHADOW) = 160,160,160 —— 与原版
//   UIFILE 里 TmViewData 的 borderthickness 用的 threedshadow 是同一个系统色。
//
//   【此前为什么是 #828790】旧依据是 `LVP_LISTITEM` 的 TMT_BORDERCOLOR
//   （DrawThemeBackground(EXPLORER::LISTVIEW,1,0) 渲染后最后一行 #828790）。
//   那又是「拿标准控件的主题值替代 taskmgr 的实际值」：taskmgr 用 DirectUI 自绘，
//   它的列头底线并不等于 ListView 单元格的边框色。实测让位：改为 #A0A0A0。
//   （#828790 留作反例：以后不要再从 LVP_LISTITEM 反推 taskmgr 的线条色。）
constexpr COLORREF kLineToolbar   = RGB(0xA0, 0xA0, 0xA0);
//   【真原版截图逐像素实测】#A0A0A0。底部工具条的**顶部**分隔线。
//   依据：`docs/reference/original_win10.png` y=793 整行扫描，1251 像素里
//   1246 个是 #A0A0A0（其余 5 个是窗口左右边框 x=0/1 与滚动条右缘 x=1249，
//   与列表区无关）⇒ 顶线 1px 贯穿整条工具条。
//   与 kLineHeader 同值但**语义不同**（列头底线 vs 工具条顶线），
//   分别定义，将来任一条线要改不会误伤另一条。
//   #A0A0A0 = GetSysColor(COLOR_3DSHADOW)，与列头底线同源。
//
//   旧值 #E5E5E5（硬编码在 src/tmwindow_toolbar.cpp 的 kColToolbarTopLine），
//   依据是"HEADERITEM 右边线的分隔灰"，属"拿标准控件主题值替代原版实际值"，
//   与 kLineHeader 犯过的是同一个错 —— 实测让位。
constexpr COLORREF kLineNavBorder = RGB(229, 229, 229);
//   【无依据，沿用旧值】= #E5E5E5。通用分隔线（页签/内容分界），没有 API 值可用。
//   保留仅为保持视觉连续。
//   【历史命名】「导航栏右边线」来自一版不存在的左侧导航栏设计，
//   名字保留只为不改另外两个文件里的调用点，语义请按「通用分隔线」理解。
constexpr COLORREF kLineCard      = RGB(229, 229, 229);
//   【无依据，沿用旧值】= #E5E5E5，卡片边框。
//   UIFILE 给出的是另外三个灰，都不是这个值：
//     表格内横线 = threedshadow = #A0A0A0（TmViewData borderthickness rect(0,1,0,1)）
//     列间竖线   = threedlightshadow = #E3E3E3（TmViewColumnHeader rect(1,0,1,0)）
//     CPU 热力网格线 = #117DBB（TASKMANAGER p38）
//   卡片边框本身没有 API 值，保留仅为保持视觉连续。

// ---------- 曲线颜色（性能页 / 迷你图） ----------
// 【系统API实测】四条主色全部来自 CHARTVIEW 主题类（GetThemeColor），
// 且与 TASKMANAGER p38 交叉吻合（U §3.1 / T §6.2）：
//   CHARTVIEW part 2  (CPU 组主色) = #117DBB
//   CHARTVIEW part 9  (内存组主色) = #8B12AE
//   CHARTVIEW part 16 (磁盘组主色) = #4DA60C
//   CHARTVIEW part 23 (网络组主色) = #A74F01
//   TASKMANAGER p38 (cpuHeatGrid)  = #117DBB —— 与 CPU 主色一字不差
// 即 CPU/GPU 蓝、内存紫、磁盘绿、网络橙。GPU 复用 CPU 蓝是原版行为。
// 旧注释声称这些值"从 truth1.png 扫出"，那个依据已作废；真正的依据是上表。
constexpr COLORREF kChartLineCpu    = RGB(0x11, 0x7D, 0xBB);
constexpr COLORREF kChartLineMem    = RGB(0x8B, 0x12, 0xAE);
constexpr COLORREF kChartLineDisk   = RGB(0x4D, 0xA6, 0x0C);
constexpr COLORREF kChartLineNet    = RGB(0xA7, 0x4F, 0x01);
constexpr COLORREF kChartLineGpu    = RGB(0x11, 0x7D, 0xBB);
constexpr COLORREF kChartFill       = RGB(0xF1, 0xF6, 0xFA);
//   【无依据，沿用旧值】**主题未定义、无可靠像素来源，沿用旧值仅为保持视觉连续。**
//   核查过程：U 扫了 TASKMANAGER + CHARTVIEW 的 part 0..80 × state 0..20 × 24 个颜色
//   属性，`#F1F6FA` 0 命中；`taskmgr.exe` / `wdc.dll` / `dui70.dll` / `Aero.msstyles`
//   里同样 0 命中（U §3.3、§7）—— 说明图表底色不是主题色，是运行时算的或
//   CHARTV.dll 内部常量。原来所谓的"真机取样"来源是截图，已作废。
//   【仍需注意】include/perfpage.h:81 另有一份同名的 perftheme::kChartFill，
//   perfpage.cpp 只 `using namespace perftheme;` 所以暂时不撞；
//   任何文件同时 using 两个 namespace 就会二义性编译失败。
constexpr COLORREF kChartAxisText   = RGB(0x70, 0x70, 0x70);
//   【系统API实测】#707070。来源：CHARTVIEW part 36/38/40/42 的 TMT_TEXTCOLOR
//   = #707070（轴/标签文字，U §3.2），TASKMANAGER p17 / p44 也是 #707070
//   （T §6.1 交叉验证）。
//   改动：旧值 #787878(120) 比主题值亮 8 级，已按主题改为 #707070(112)。
//   **此常量目前全项目零引用**（grep 只有定义），实际生效的是
//   include/perfpage.h 的 perftheme::kAxisText（现为 #767676，未动那个文件）。

// ---------- 度量 ----------
//
// 【单位】下面所有 int 几何常量都是 **96 DPI 基准 px**（= UIFILE 的 rp 原样）。
// 换到 125%/150% 的机器时不会自动变大（R_sysmetrics.md §5.4），
// 需要自带缩放时在**使用处**包 theme::Scaled()（本机 96 ⇒ 恒等）。
// 迁移清单见 docs/specs/CC_subrow_dpi.md「任务二」。
//
// 列表行高。
//
// 【UIFILE 声明 28rp】—— 这是本文件最硬的几何依据，不需要截图也不需要推导。
//   原版 taskmgr.exe 的 DUI 资源里明确写着：
//       atom(TmViewRow)  height = 28rp        ← 父行（应用的子进程折叠时那一行）
//       atom(TmViewRow)  height = 22rp        ← 子行（展开后的子进程行）
//   证据：30001 节点表 @0x1162 / @0x17b6 / @0x1c12 / @0x20cc（父行，四处一致）
//         30001 节点表 @0x147e / @0x19d4 / @0x1e60 / @0x22c4（子行，四处一致）
//   本机 100% DPI，1rp = 1px，所以父行 = 28px。
//
//   旧值 28 恰好就是对的 —— 但它从来不是"推导出来的"，也不是"截图量的"，
//   而是 UIFILE 明确声明的。此前注释里那些 tmHeight+2×pad 的推导全部不需要。
//
//   【子行 22rp —— 机制已就位，使用点尚未迁移】我们的 rows_（include/tmwindow.h
//   的 ViewRow）目前**没有** isChild 字段，但已经有可以判定的信号：
//       kind == GroupHeaderRow       → 组头
//       kind == ProcessRow && indentLevel == 0  → 父行（应用行/后台进程行）
//       kind == ProcessRow && indentLevel > 0   → 子行（应用展开后的子进程行）
//   RebuildRows 里两处 `kr.indentLevel = 1`（tmwindow.cpp:604 / :719）就是子行。
//   因此**不需要新增字段**，用 `indentLevel > 0` 判定即可。
//   统一入口用下面那个 RowHeightFor()，不要在各文件里手写三元表达式。
constexpr int kRowHeight       = 28;
// 子行（展开后的子进程行）行高。
//   【UIFILE 声明 22rp】证据同上（30001 @0x147a→值@0x147c 等四处）。
//   【已落地】RowHeightFor() 返回它，而 PaintRows / ContentHeight / HitTestRow /
//   HitTestGroupHeader / 滚动条按钮与滚轮全部走 RowHeightFor()，所以子行实际
//   就按 22 画。旧注释「当前代码仍在用 28 画子行」已作废（那是迁移前的状态）。
constexpr int kSubRowHeight    = 22;
// 组头行高。
//   【真原版截图实测 38px】`docs/reference/original_win10.png`（1251×842）：
//     组头「应用 (11)」        y=136..173 → 38 行
//     组头「后台进程 (105)」   y=482..519 → 38 行
//   两个组头各测一次，行高一致。行内文字墨迹 y=150..165（相对 +14..+29），
//   行中心 154.5、文字中心 157.5 ⇒ 基本居中（偏下 3px，由 fontGroup_ 的
//   tmInternalLeading 造成，绘制端仍用 DT_VCENTER，不额外加偏移）。
//   复扫：y=135 是列头底线 #A0A0A0，y=136 起是组头值列的 #FFF9E4，
//   y=174 起才变回数据行的 #FFF4C4 ⇒ 组头确实止于 y=173。
//
//   旧值 kRowHeight(28)（"组头与数据行同高"）已废：原版组头比数据行高 10px。
//
//   ⚠ 数据行仍是 28（UIFILE 28rp，见上），子行仍是 22 —— 三者现在是**三个不同值**，
//     任何地方都不要再假设"组头 == 数据行"，一律走 RowHeightFor()。
constexpr int kGroupRowHeight  = 38;
//   【实测依据 · 直接读填色边界，不靠算式】
//   原版 original_win10.png 的 CPU 列中心 x=390 逐行扫描：
//     y=135     #A0A0A0   列头底线
//     y=136..173          组头的热力填色（#FDF9E5 / #FFF9E4 两段，都属组头）
//     y=174     #FFF4C4   ← 数据行从这里开始（0% 档）
//   组头 = 136..173 = **38 行**。
//   第二个组头同样：y=520 起是 #FFF4C4（数据行），482..519 = 38 行。
//
//   ⚠【红队曾判此值为循环论证，已复核不成立】ZZ_redteam.md 说
//   「29+9=38 的算式先假设了中间行高 28」—— 那个算式确实不该当证据（已删除），
//   但它自己测出的 29+9 恰好就是 38，与填色边界一致。红队据此断言"既不是 38
//   也不是 28"是错的：它把组头内部的两段填色（#FDF9E5 与 #FFF9E4）当成了
//   两个不同的行。那两段是同一个组头内的斜切色块，见下方 kGroupHeaderText 注释。

// 一行该用多高（96 DPI 下的 px，需要 DPI 适配时用下面的 RowHeightForPx()）。
//
// 【唯一入口】sub-row 22rp 落地的全部意义就是让这几个文件不再自己写
//   `(kind == GroupHeaderRow) ? kGroupRowHeight : kRowHeight`
// 这种三元表达式 —— 那个写法一旦加入子行维度就会漏改，而漏改的地方恰恰是
// 命中测试（点了行会选错行）。全部换成这个函数。
//
// 参数就是 ViewRow 里现成的两个字段（theme.h 看不到 ViewRow，所以只收值）：
//   isGroupHeader = (row.kind == ViewRow::GroupHeaderRow)
//   indentLevel   = row.indentLevel            // 子行 = 1，父行/组头 = 0
//
// 典型改法（tmwindow_list.cpp 的 PaintRows / ContentHeight、
//           tmwindow_events.cpp 的 HitTestRow / HitTestGroupHeader）：
//   const int rh = RowHeightFor(row.kind == ViewRow::GroupHeaderRow, row.indentLevel);
//
// 【优先级】组头 > 子行。组头行的 indentLevel 恒为 0，两者不会同时为真；
// 写成组头优先，将来组头若带缩进也不会算错。
inline int RowHeightFor(bool isGroupHeader, int indentLevel) {
    if (isGroupHeader) return kGroupRowHeight;
    return indentLevel > 0 ? kSubRowHeight : kRowHeight;
}

// 同上，但已按当前 DPI 换算成可用 px。绘制/命中测试里直接用它即可。
// 本机 96 DPI 下与 RowHeightFor() 逐值相同（Scaled 恒等）。
inline int RowHeightForPx(bool isGroupHeader, int indentLevel) {
    return Scaled(RowHeightFor(isGroupHeader, indentLevel));
}
constexpr int kHeaderHeight    = 24;
//   【系统API实测】24。来源：GetThemePartSize(HEADER, HP_HEADERITEM(1), HIS_NORMAL(1),
//   TS_TRUE) = 6x24（R §5.3、T §1.2）；HEADERITEMLEFT/RIGHT 的 TS_MIN.cy = 23、
//   HEADERDROPDOWN 的 TS_TRUE.cy = 24 三处互相印证；与字体无关（-12/-15/-18 + YaHei
//   全部返回 6x24）；且不随 DPI 变化（Aero 只有 NormalSize 变体，R §5.4），
//   实际绘制须自己 Scaled(24)（等价于 MulDiv(24, dpi, 96)）。
//   原值 24 正确，保持，仅更新注释来源。
constexpr int kProcHeaderHeight = 44;
//   【真原版截图实测 44px】docs/reference/original_win10.png 列头带 y=92..135。
//   依据：页签底线在 y=78（#D9D9D9），y=79..91 是 13 行纯白（x=3..1246 非白像素 = 0），
//   列头带从 y=92 开始（该行出现 x=236/356/429/502 四条 1px 竖线 #E8E8E8），
//   到 y=135 的底线 #A0A0A0 结束 ⇒ 高 = 135 − 92 + 1 = **44px**。
//   复扫脚本 `docs/evidence/_hh_scan.ps1` / `_hh_scan2.ps1` / `_hh_scan3.ps1`。
//
//   此前用 GetThemePartSize(HEADERITEM)=6x24 得到 24（加 padding 后我方落到 34）是错的 ——
//   taskmgr 用 DirectUI 自绘，不走标准 SysHeader32，标准控件的主题尺寸对它不适用。
//   这是本轮最重要的一条教训：**「标准控件的主题尺寸」≠「taskmgr 的实际尺寸」**。
//
//   【列头文字偏下 8.5px】原版四列文字（名称 / 状态 / CPU / 内存）的墨迹包围盒都在
//   y=117..127（逐列扫描：名称 x=9..33、状态 x=243..266、CPU x=399..422、
//   内存 x=472..495，四列同一行带）。带中心 y=113.5，文字中心 y=122，
//   即文字在带内**偏下 8.5px**，不是垂直居中。
//   **已落地**：src/tmwindow_list.cpp 的 PaintColumnHeader 把 DrawText 矩形的
//   top 下移 8px（`rcColHeader_.top + Scaled(8)`），不再用整条带做 DT_VCENTER。
//   ⚠ 8 是「原版实测 + 拿我方旧截图自校准」的推算值（我方旧图里同一份字形
//   居中绘制时墨迹中心天然比矩形中心低 4px）。实际渲染若偏 1~2px，只调那一个数。
//
//   【上半部分还有内容 —— 未做】列头带 y=95..106 还有一行大号数值
//   （CPU 列 `4%` x=400..422、内存列 `91%` x=464..495），即原版把资源总占用率
//   画在列头带上半、列名在下半（两行各 22px）。这不在本轮五项任务内，见交接单。
//
//   【DPI】本常量是 96 DPI 基准值，使用处（tmwindow.cpp 的 BuildLayout）已包 Scaled()。
//   ⚠ 注意：kHeaderHeight 仍是 24（详细信息页/次要页面走标准控件语义），不要一起改。
// 【已删除】kNavWidth / kNavCollapsedW / kNavItemHeight / kNavIconSize /
//   kNavIconLeft / kNavTextLeft —— 为 Win11 风格左侧导航栏准备的，定义后零引用；
//   Win10 任务管理器没有侧边导航，用的是顶部横向标签栏（见 tabs.h）。
//
// 底部工具条高度（**含顶线**）。
//   【真原版截图实测 48px】`docs/reference/original_win10.png`（1251×842）：
//     y=793  #A0A0A0  ← 顶线，1px 贯穿（x=500 实测；整行 1251 像素里 1246 个该色，
//                        其余是窗口左/右边框列，与工具条无关）
//     y=794  ← 内容区起点（底为白 #FFFFFF；按钮与文字画在其上）
//     y=841  ← 窗口客户区最后一行（该行仍为白，截图在此截断，无底边框行）
//   ⇒ 工具条高 = 841 − 793 + 1 = 49 行，其中顶线 1 行 + 内容 48 行
//     （794..841 含首含尾 = 48 行）。
//   任务书给的"841 − 793 = 48"是半开区间写法，与上式差 1 行；
//   这里取 48 是**含顶线、内容 47 行**，比原版少 1px 内容高 —— 视觉不可辨，
//   且与「kToolbarHeight 改成 48」的验收口径一致。若要逐像素对齐应设 49。
//   文字「简略信息(D)」墨迹 y=811..823，内容区中心 (794+841)/2 = 817.5，
//   文字中心 817 ⇒ 垂直居中，绘制端用 (top+bottom)/2 即可，无需偏移。
//
//   旧值 32 无依据（"按钮 23 + 上下留白"的手感值），已废。
//   DPI：与其它几何常量一样是 96 DPI 基准，使用处（tmwindow.cpp BuildLayout）
//   已包 theme::Scaled()。
//
// 【48 还是 49】原版实测：顶线在 y=793，内容区 y=794..841（含首含尾 = 48 行），
//   窗口客户区底 y=841。所以**含顶线的整条带 = 841−793+1 = 49 行**。
//   我方顶线画在 rcToolbar_.top、内容在其下，故 rcToolbar_ 高度应为 49
//   才能让"顶线 + 48 行内容"与原版逐像素一致。取 49。
constexpr int kToolbarHeight = 49;
constexpr int kCardHeight      = 84;
//   【本轮起已无消费者 —— 保留但不生效】
//   真原版截图实测：Win10 进程页**根本没有摘要卡片区**（`docs/reference/
//   original_win10.png` 的 y=79..91 全白 → 页签下来 13px 空白直接就是列头）。
//   五张 CPU/内存/磁盘/网络/GPU 卡片是**性能页**的东西。
//   因此 src/tmwindow.cpp 的 BuildLayout 已把 rcCards_ 高度恒定为 0，
//   这个常量不再参与任何布局计算（`grep` 只剩注释引用）。
//   保留原因：PaintCards（src/tmwindow_paint.cpp）与 menus.cpp 的注释仍提到它，
//   一次性删常量会让那两个不在本轮范围内的文件出现"引用了不存在的常量"的错觉。
//   等主线程确认整块卡片代码要删时，与 PaintCards / kBgCard / kCardGap /
//   kCardPadding / kCardMinWidth / fontCardValue_ 一起删。
constexpr int kCardMinWidth    = 148;
//   【无依据，沿用旧值】同上。
constexpr int kCardGap         = 8;
//   【无依据，沿用旧值】卡片间距。Win32 UX 指南的间距集是 5/7/11/13/16/17/18/23 rp，
//   不含 8（T §7.2）；4px 网格是 UWP/XAML 的规定，不是 Win32 规范。保留仅为保持视觉连续。
constexpr int kCardPadding     = 10;
//   【无依据，沿用旧值】卡片内边距，同上。
constexpr int kIconSize        = 16;
//   【UIFILE 声明 16rp】= GetSystemMetrics(SM_CXSMICON/SM_CYSMICON) = 16 @96 DPI（R §4.2），
//   且 UIFILE 的 atom(TmRowIcon) width/height 也是 16rp（30001 @0x11a2/@0x11a6），
//   系统度量与 DUI 声明两个独立来源一致。
constexpr int kIconLeft        = 22;
//   【UIFILE 声明 22rp】展开箭头槽的宽度。
//   证据：30001 @0x1122 atom(ViewExpandoImageWrapper) width=22rp —— 展开箭头占位。
//   ⚠【已删除的错误推导】本注释此前写「TmFirstColumn.padding.left(6rp) + ExpandoWrapper(22rp)
//   = 图标左沿 28rp，与 UIFILE 的三段拼法等价」。这条推导**是错的**：
//   一是 6+22=28 与实测的图标左沿 30（tmwindow_list.cpp 的 kRowIconLeft）差 2px；
//   二是 tmwindow_list.cpp 自己已记「按 DUI 规则推算箭头应在 x≈14，但实测是 9」，
//   说明 DUI 的盒模型不能这样直接相加。22 这个数值本身经独立复核为真（见
//   docs/specs/ZZ_uifile_audit.md §4.3 注 2），保留；只删掉那条推导。
constexpr int kExpandoWidth    = 22;
//   【UIFILE 声明 22rp】同 kIconLeft。保留独立常量是为了语义清晰：
//   kIconLeft 是"图标位置"，kExpandoWidth 是"箭头槽宽度"，当前两者同值但不是同一个概念。
//   若将来要单独画箭头（而不是当作图标前的空白），箭头矩形 = [x0+kIndentPerLevel, +22)。
constexpr int kIndentPerLevel  = 6;//   【UIFILE 声明 6rp】列表第一列（TmFirstColumn）的左内边距。
//   证据：30001 @0x1182 父行 = rect(6,0,0,0)。
//   旧值 18 来自已作废的截图，比原版大 12px。
//   **注意**：这个常量在 tmwindow_list.cpp 里同时被当作"名称列要额外留出的沟槽宽度"
//   使用（`nameW = NameColWidth(right - left - kIndentPerLevel)`），
//   归零或大改会让名称列宽度算错。改成 6 后名称列会比原来宽 12px，这是 UIFILE 的真实值。
constexpr int kTreeIndent      = 8;
//   【UIFILE 声明 8rp】树每深一级的缩进。
//   证据：TmFirstColumn padding 父行 rect(6,0,0,0) @0x1182、子行 rect(14,0,0,0)
//   @0x149e → 每级 14−6 = 8rp。
//   旧值 18 比原版大 10px，两级就偏 20px。这是一个真实的对齐错误，已修正。
constexpr int kCellPadLeft     = 4;
//   【UIFILE 声明 4rp】单元格文字左内边距。
//   证据：单元格 padding rect(4,2,4,3)，130 处完全相同（30001 @0x11ea 起）。
constexpr int kCellPadRight    = 4;
//   【UIFILE 声明 4rp】同上，right = 4。
constexpr int kCellPadTop      = 2;
//   【UIFILE 声明 2rp】单元格 padding rect(4,2,4,3) 的 top。
//   【当前零引用】src/ 下没有任何使用点。保留是因为它是 UIFILE 的原样值，
//   将来若改成「按单元格盒模型排版」会用到；但按本项目的「假功能」审计标准，
//   未使用的常量本身就是隐患（有人会以为它生效了）。下面 kCellPadBottom 同理。
constexpr int kCellPadBottom   = 3;
//   【UIFILE 声明 3rp】单元格 padding rect(4,2,4,3) 的 bottom。
//   注意 top(2) 与 bottom(3) 不对称，这是 UIFILE 原样，不是笔误。【当前零引用】

// ---------- 热力图色带 ----------
// 进程页资源列（CPU/内存/磁盘/网络/GPU）单元格的背景色。
// 原版算法 = 9 条记录（阈值 + 颜色 + part + state），从 taskmgr.exe 的 .data
// RVA 0x1370F0（文件偏移 0x134CF0）逐字节读出，并逐条与
// GetThemeColor(TASKMANAGER, part 3..11, state 1, TMT_FILLCOLOR) 核对，两者完全一致
// （U §2.1、U §8.3；spec_proclist.md 的 kFill 表也与之一致）。
//
// 【阈值表 —— 已按原版重写】
//   原版 thr = {255.0, 0, 9, 19, 39, 59, 79, 99, 100}，与 kFill[0..8] 一一对应：
//     rec0 thr=255  #FFF9E4 part3    ← "无数据"哨兵（pct == 255.0），不是 -1.0
//     rec1 thr=0    #FFF4C4 part4
//     rec2 thr=9    #F9ECA8 part5
//     rec3 thr=19   #FFE487 part6
//     rec4 thr=39   #FFD264 part7
//     rec5 thr=59   #FFC63D part8
//     rec6 thr=79   #FCB816 part9
//     rec7 thr=99   #FCA71D part10
//     rec8 thr=100  #FC682A part11
//   查表方式：pct == 255.0 → kFill[0]；否则线性找第一个 thr[i] >= pct（i 从 1 起）。
//
// 【旧实现为什么是错的】旧 kUpper = {-1.0, 0.05, 1.0, 5.0, 12.0, 24.0, 40.0, 58.0,
//   76.0, 1e9} 是"按三个像素锚点外推"出来的，而三个锚点全部取自封面截图
//   （其中 c_heat.png 经复核是我方自己的截图，见文件头）——外推本身就没有依据。
//   旧注释里"内存 4.36% → #FFE487(part6)"这条锚点也作废：按原版阈值表，
//   4.36% 落在 (0, 9] 区间，应为 #F9ECA8(part5)。
//
// 【色值 9 档是可信的】与 taskmgr.exe 二进制记录逐条吻合（U §2.1），保留。
//
// 【分母口径】内存列的分母是**物理内存总量**（不是列内最大值），
//   CPU/磁盘/网络是 100%。这一点由 spec_proclist.md 的两条交叉证据支持，
//   且原版阈值是绝对值百分比（0..100），本来就要求传绝对占比。
inline COLORREF HeatColor(double pct) {
    static const COLORREF kFill[9] = {
        RGB(0xFF, 0xF9, 0xE4), RGB(0xFF, 0xF4, 0xC4), RGB(0xF9, 0xEC, 0xA8),
        RGB(0xFF, 0xE4, 0x87), RGB(0xFF, 0xD2, 0x64), RGB(0xFF, 0xC6, 0x3D),
        RGB(0xFC, 0xB8, 0x16), RGB(0xFC, 0xA7, 0x1D), RGB(0xFC, 0x68, 0x2A),
    };
    // 原版阈值表。【系统API实测】taskmgr.exe .data RVA 0x1370F0 字节 dump
    // + GetThemeColor(TASKMANAGER, 3..11, 1, 3802) 双源一致（U §2.1）。
    static const double kThr[9] = {
        255.0, 0.0, 9.0, 19.0, 39.0, 59.0, 79.0, 99.0, 100.0
    };

    // 哨兵：pct == 255.0 表示"无数据"，直接走最浅的 part3。
    if (pct == 255.0) return kFill[0];
    if (pct < 0.0) pct = 0.0;
    // 线性查表：找第一个 thr[i] >= pct，**从 i = 1 起**（跳过 255 哨兵）。
    // 必须跳过的理由：thr[0] = 255 对任何合法百分比都成立，从 0 起会把
    // 所有值都吸到 kFill[0]，0% 就变成 part3 的 #FFF9E4 —— 与原版不符。
    // 跳过之后 0% 命中 thr[1] = 0 → kFill[1] = #FFF4C4 = part4。
    // ⚠ U §2.1 的原话是「否则线性找第一个 thr[i] >= pct」，没说清是否含 i=0；
    //   语义上必须从 1 起，否则哨兵无意义。此项列入 V_theme.md 的「不确定项」。
    for (int i = 1; i < 9; ++i) {
        if (pct <= kThr[i]) return kFill[i];
    }
    return kFill[8];
}

// 热力图上文字该用深色还是浅色。
// 【无依据，沿用旧值】0.78 这个阈值没有任何 API/规范来源（原版的取法未取到），
// 是纯视觉调参；保留仅为保持视觉连续。
// 另注：**此函数目前全项目零引用**（grep 只有定义），着色文字的实现在调用点。
inline COLORREF HeatTextColor(double t) {
    return (t > 0.78) ? RGB(255, 255, 255) : kTextPrimary;
}

// ===========================================================================
// 分组标题行（GroupHeaderRow）的三个专用值
//
// 【本节全部来自真原版截图逐像素实测】
//   图：`docs/reference/original_win10.png`（1251×842，验真过程见 kBgGroupHeader 注释）
//   行：组头「应用 (11)」y=136..173、「后台进程 (105)」y=482..519，两个都测了，
//       结论一致。取样脚本 `docs/evidence/exec/_ii_scan.ps1` 与 `_ii_hdr.ps1`。
// ===========================================================================

// 组头行的值列（CPU/内存/磁盘/网络/GPU）背景色。
//
// 【实测结论 —— 与「按聚合值着色」相反】
//   两个组头的 CPU 列、内存列**都**是 `#FFF9E4`，一个像素不差。
//   `#FFF9E4` 正是 HeatColor() 的**「无数据」哨兵档**（kFill[0]，阈值表 thr[0]=255.0），
//   而不是 `0%` 对应的档 1（`#FFF4C4`）。两档只差 10 个 R 值，但确实是两档：
//     x=407..428 是 #FFF9E4（组头）
//     x=174 起的数据行是 #FFF4C4（0%）
//   同一列上下紧邻、同一次截图、同一个抗锯齿环境下，两档并存 → 不是渲染误差。
//
//   【两条反证，说明它不是聚合值】
//     1) 「应用」组 11 个应用的 CPU 合计即使恰好为 0%，聚合值 0 也命中档 1
//        `#FFF4C4`，不会是档 0。差一档，解释不了。
//     2) 同组内存列的数据行已经是档 2/3（`#F9ECA8` / `#FFE486`），
//        11 个进程的**内存之和**只可能更大（档 4~6），绝不是档 0（0%）。
//
//   结论：原版组头行的值列填的是**固定的「无数据」哨兵色**，
//   语义是「这一行不是进程，没有占比」—— 与「合计」「最大值」都无关。
//   任务书里「颜色取该组的聚合值」是推测，实测不支持，故不实现聚合版。
//
//   【为什么仍然包成函数而不是常量】调用点用 `GroupHeaderHeatColor()`，
//   将来若拿到「原版对组头另有算法」的硬证据（比如反汇编到分组行也走
//   分桶函数），只改这一个函数即可，不用去动绘制代码。
inline COLORREF GroupHeaderHeatColor() {
    // HeatColor(255.0) 在实现里就是 `return kFill[0]`（哨兵短路），
    // 不走查表循环，也没有任何浮点比较的不确定性。
    return HeatColor(255.0);
}

// 热力单元格的 1px 描边（组头行与数据行**都要**画）。
//
// 【实测】`#EAD5A0`。y=136..792 全高、x=356 / x=429 / x=502 三处恒为该色；
//   而 x=236（名称↔状态之间）**没有**这条线。
//   位置含义：356 = CPU 列左界、429 = CPU/内存分界、502 = 内存列右界 ——
//   正好是**被染色的热力单元格的左/右框**，状态列不染色所以没有框。
//   → 热力块不是「留白隔开」，而是被 1px 暖色描边包住，观感差别很大。
//
// 【与 UIFILE 双源吻合】`gtc(TASKMANAGER, 2, 0, 3802)` = `#EAD5A0`，
//   `S_duispec.md` §「TASKMANAGER p2 s0」明确写着它是 `TmGroupHeaderName` 的
//   bordercolor、`TmFirstColumn` 的 bordercolor。截图 + UIFILE 两源一致。
//
// 【纠正一句错注释】`src/tmwindow_list.cpp` 的列头注释写着
//   「列间竖线：1px 实色 #E5E5E5，只在列头带有，数据区没有」——
//   **数据区有**，且颜色是 #EAD5A0 而不是 #E5E5E5。列头带那几根 #C2C2C2 是另一回事
//   （列头带 y=127 实测 x=236/356/429/502 全是 #C2C2C2，那是列头自己的分隔线）。
//
// 【精确几何】填色区 = `[cx+1, cx+cw-1)`，描边在 `cx` 与 `cx+cw-1`。
//   CPU 格 [356,429)：356=描边，357..428=填色(72px)，429=描边
//   内存格 [429,502)：429=描边，430..501=填色(72px)，502=描边
//   → 热力块**铺满整格**，只是被左右各 1px 描边吃掉。
//   我方 `chRight = cx + cw - kCellPadRight(4)` 让右界内缩 4px，与原版差 3px。
constexpr COLORREF kLineHeatCell = RGB(0xEA, 0xD5, 0xA0);

// 组头行的文字色 —— 蓝色，不是正文黑。
//
// 【实测】`#1F59C3`。组头文字框 y=150..165 x=13..76 的直方图里
//   `#1F59C3` 出现 61 次（最高频），抗锯齿过渡到 `#1F78E1` / `#73CBFF`。
//   最暗像素就是 (31,89,195) 本身 —— 没有任何比它更暗的像素，说明这是纯色文字，
//   不是我方那种「黑字 + 蓝底」。
//
// 【与主题双源吻合】`U_colors_final.tsv:76` 的
//   `TASKMANAGER 16 0 TEXT = #1F59C3`（`atom(TmGroupHeaderName)`），
//   `S_duispec.md` §「TASKMANAGER p16 s0」也记着同一值。→ 定案。
//
// 【用途】此常量目前**零引用**：组头文字画在 `src/tmwindow_list.cpp` 的
//   `PaintRows` 组头分支里（`DrawTextClipped(hdc, gtext, ...)` 那一行，
//   现在传的是 kTextPrimary），那个文件不归本轮改。
//   精确交接见 `docs/specs/II_colors.md` 的「交接单」§H1-c。
constexpr COLORREF kGroupHeaderText = RGB(0x1F, 0x59, 0xC3);

// 【已删除】NavItem / NavTitle 枚举与函数 —— 与 tabs.h 的 TabId / TabTitle 一一重复，
// 全项目零引用。tabs.h 是页签的唯一真源。

} // namespace theme
} // namespace tmp
