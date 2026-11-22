// tmwindow_details.cpp - 「详细信息」页
//
// 任务管理器的详细信息页是一个扁平的全进程表格（不分组、无卡片），
// 复用同一套虚拟化绘制。
//
// ============================================================================
// 列族与默认列的依据（全部来自本机 taskmgr.exe 的直接读取，可复现）
// ============================================================================
//
// 列 ID 与名字字符串 ID 的关系是 `strId = colId + 1`，名字去
// STRINGTABLE 2001–2004（`docs/evidence/exec/_b_strtable.js` 直解 MUI 的 RT_STRING 复核过）。
//
// 详细信息页的可选列池在 `.rdata` 文件偏移 0xF4328 起，**48 条 × 56 字节**：
//     +0 = flags(0x0007)  +8 = width  +16 = colId
// 而「默认可见哪几列 + 各自的初始宽度」是紧接着的一张小结构，文件偏移 0xF3ED0：
//     0x00: 0000209D     可见性位掩码 = 8349
//     0x04: 00000020     32（含义未证实，exe 与注册表两处都是 32）
//     0x08: 48 个宽度，按池索引 0..47
//     0xC8: 0,1,2,…,47   与宽度表一一对应的顺序数组
//
// 直接读出的结论：
//   * 掩码 0x209D 的置位只有 6 个 —— 池索引 { 0, 2, 3, 4, 7, 13 }
//     （`0x209D = 0b0010_0000_1001_1101`）。全 exe 里 0x209D 只出现这一次，
//     注册表 reg_Preferences.bin 里也只出现一次（0x0FB8），两份 200 字节记录
//     byte-for-byte 完全相同 ⇒ 该用户从没改过这一页，屏上跑的就是出厂默认。
//     **所以「默认 6 列」是定论，没有第 7 列、也没有「另一套配置」。**
//   * 池索引 → 列（名字 = strId 换算后的 MUI 文本）：
//       0  名称 (colId 32003)                     宽 145
//       2  PID  (colId 32004)                     宽  50
//       3  状态 (colId 32035 → strId 32036)        宽 100
//       4  用户名 (colId 32005)                    宽  80
//       7  CPU  (colId 32007)                     宽  40
//       13 内存(活动的专用工作集) (colId 32013)      宽  80
//   * **原版详细信息页没有「类型」列** —— 48 个可选列里根本没有这个词，
//     「类型」只存在于进程页列族（37xxx）。上一轮把 proc_header_3x.png 当成
//     本页、从而引入「类型」列，是把因果关系推反了。
//
// 【宽度取哪一组 —— 用原版截图实测定案：取池里的这一组】
// 池里带宽度，「掩码 + 32 + 48 宽度」那份记录（0xF3ED8）也带 48 个宽度，
// 两者在 名称(145 vs 100) / 状态(100 vs 80) 等处不一致。用原版服务页的
// 4 倍放大裁切（原版实拍，未随仓库分发，867×45 逻辑）反推：
//
//   在列头带内扫「连续 85% 高度的浅灰竖条」，得到列边界（逻辑坐标）
//       4 | 184 | 234 | 643 | 743 | 843     （另有 860 = 滚动条左沿）
//   服务页 5 列是 名称 / PID / 描述 / 状态 / 组，于是
//       名称 = 184 -   4 = 180   ← 池 180，**完全相等**
//       PID  = 234 - 184 =  50   ← 池  50，**完全相等**
//       描述 = 643 - 234 = 409   ← 池 0，即「弹性列吃剩余宽度」
//       状态 = 743 - 643 = 100   ← 池 100，**完全相等**（记录那份写 80，不成立）
//       组   = 843 - 743 = 100   ← 池 100，**完全相等**
//
// 五列里四列逐像素等于池宽，唯一不等的那一列在池里被标成 0（弹性）。
// ⇒ **生效的是池里的宽度，48 宽记录不是显示宽度**
//   （0x140008C57 把那份记录整块拷进页配置对象，应是「未自定义时的初始/参考值」）
//
// 【第二重独立验证：服务页也是同一个规律】
// 紧跟本页记录之后（文件偏移 0xF4060）还有一条小记录，是**服务页**的默认配置：
//     mask = 0x0000001F（5 位全置，即 5 列全显示），紧跟 5 个宽度
//     100, 50, 120, 80, 80
// 而服务页的列定义池（0xF4208）是 180/50/0/100/100，上面实测显示宽度是
// 180/50/409/100/100 —— **同样只有池那组对得上，记录那组（100/50/120/80/80）三列全错**。
// 两个页面、两套记录、都指向同一个结论，可以定案。
//
// 另外，本页 6 列总宽 = 145+50+100+80+40+80 = 495px，远小于窗口宽度，
// 右侧留白 —— 这正是原版 ListView 的行为（列宽固定、不拉伸填满，见
// `docs/specs/spec_proclist.md` 对 4.png 的实测「列只到 x=660，右边留 190px 空白」）。
// 如果宽度取错成「弹性填充」，这个特征就消失了。
//
// 因此本页 6 列宽度取池里的 145 / 50 / 100 / 80 / 40 / 80（全部固定，不弹性拉伸）。
#include "tmwindow.h"

#include <windowsx.h>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <vector>

namespace tmp {

using namespace theme;

namespace {

// 列定义。
//
// 【结构修正】以前这里是 `{title, width, align}` 三字段，绘制代码再用
// 硬编码下标 DetColX(0..5) 一格一格画。插列/换列时下标全部要人工 +1，
// 上一轮加「类型」列就是这么逐个改的（改对了，但没有任何机制防止下次改错）。
// 现在加 `field` 枚举，绘制循环里 switch(field)，插列时字段跟着走。
enum DetField {
    DF_NAME = 0,
    DF_PID,
    DF_STATUS,
    DF_USER,
    DF_CPU,
    DF_MEM,
};

struct DetCol {
    const wchar_t* title;
    int width;        // 默认宽度，取 .rdata 列定义池（0xF4328）里的值，见上方说明
    UINT align;
    DetField field;
};

const DetCol kDetCols[] = {
    { L"名称",                  145, DT_LEFT,  DF_NAME   },
    { L"PID",                    50, DT_RIGHT, DF_PID    },
    { L"状态",                  100, DT_LEFT,  DF_STATUS },
    { L"用户名",                 80, DT_LEFT,  DF_USER   },
    { L"CPU",                    40, DT_RIGHT, DF_CPU    },
    // 列名必须是「内存(活动的专用工作集)」——MUI id 32014 原文，少「活动的」三字
    // 就是另一个可选列（32015 内存(专用工作集)）。
    { L"内存(活动的专用工作集)",    80, DT_RIGHT, DF_MEM    },
};
const int kDetColCount = sizeof(kDetCols) / sizeof(kDetCols[0]);

// 列左界 = 「前 idx 列的 96 DPI 基准宽之和」整体换算到当前 DPI。
//
// 【为什么在累加后再换算，而不是逐列换算后累加】
// 实测（scratch/dpi_probe/details_geom_probe.exe 的 [F] 段，逐 DPI 打印）：
//   96/120/144/192/240 下两种写法**完全一致，漂移 0**；
//   168 下逐列累加从第 2 列起比整体换算多 1px，且一直保持 1px，不再累积。
// 也就是说：漂移确实存在，但**量级只有 1px，不随列数增长**（因为 MulDiv 是
// 四舍五入，误差项互相抵掉，不是单向累积）。
// 仍选整体换算，理由是它让「列宽之和」与「下一列左界」在数学上恒等 ——
// 逐列累加时两者会差 1px，而列间描边画在 DetColX(i)、单元格右界取自
// DetColRight(i)-pad，两者一旦不同源就会出现「描边压住文字」这类 1px 错位。
// 这是本项目反复踩的「两套原点」模式在 DPI 维度的复现，代价为零，所以避开。
//
// 【单位】kDetCols[].width 是 96 DPI 基准值（= taskmgr.exe 列定义池里的原值），
// 这里不改常量本身，只在取用时换算。
int DetColX(int idx) {
    int x = 0;
    for (int i = 0; i < idx && i < kDetColCount; ++i) x += kDetCols[i].width;
    return theme::Rpx(x);
}

// 列右界（= 下一列的左界）。右界从这里取而不是 `DetColX(i) + Rpx(width)`：
// 后者的取整点与下一条描边的位置不同，会在高 DPI 下差 1px。
int DetColRight(int idx) { return DetColX(idx + 1); }

// 详细信息页选中行底色。
//
// 【系统API实测】#CDE8FF。这是列表选中行的权威值，两个独立来源一致：
//   1) UIFILE 里列表选中态是 dtb(EXPLORER::LISTVIEW, 1, 3)，用 DrawThemeBackground
//      渲染到 32bpp 离屏 DIB 后采样，纯色 #CDE8FF（U_themecolors.md §4.1）；
//   2) 等价的半透明合成：源色 #008AFF @ 19.6% over #FFFFFF（T_uxspec.md §1.5）。
// theme::kBgNavSelected 已同步改为 #CDE8FF，两处口径一致。
// （历史：此处曾独立硬编码 #CDE8FF，而 theme 那边还是 #A0CDF0 —— 后者出自
//  m3.png，那张已确认是我方旧版截图。现在两边已经统一，这个常量可以视为
//  theme::kBgNavSelected 的别名，保留是为了不动本文件里已有的调用点。）
constexpr COLORREF kDetSelBg = RGB(205, 232, 255);
static_assert(kDetSelBg == tmp::theme::kBgNavSelected,
              "详情页选中行必须与列表统一用主题的 LISS_SELECTED 渲染色");

// 滚动条。原版是标准 ListView 的垂直滚动条，与进程页同款：
// 宽 16px、白轨道、#CDCDCD 直角滑块、两端 16px 箭头按钮。
// （进程页那套常量在 tmwindow_list.cpp 的匿名命名空间里，取不到，故在此复刻。
//  两份值必须同步：进程页 kScrollBarW=17 / kScrollBtnH=17 是 SCROLLBAR 主题
//  TS_TRUE 实测（U_themecolors.md §4.6），本页 16/16 是旧值，两处差 1px 的
//  问题是既有的、不属于本轮 DPI 范围，见报告交接单。）
//
// 【单位】下面两个是 96 DPI 基准值，**所有使用处都必须包 Rpx()**。
// 不能像以前那样直接参与几何 —— 16px 箭头按钮在高 DPI 下会比字形还小。
constexpr int kDetScrollW = 16;
constexpr int kDetScrollBtnH = 16;
// 滑块最小长度。96 DPI 基准 24，滚轮/拖动都要同一口径。
constexpr int kDetThumbMinLen = 24;
// 内容区右缘到滚动条右界的距离（96 DPI 基准）。
constexpr int kDetSbRightInset = 3;
// 行区底部留白（96 DPI 基准 2px，原有几何，不为滚动而改）。
constexpr int kDetListBottomInset = 2;

// ---------- 本文件的 DPI 约定（改任何数字前先读这段）----------
//
// 【两类数字，处理方式不同】
//   96 DPI 基准常量（kDetCols[].width / kDetScrollW / kDetScrollBtnH /
//     kDetThumbMinLen / kDetSbRightInset / kDetListBottomInset）：常量本身
//     保持基准值不动，**每个使用点包 theme::Rpx()**。这是 theme.h 定的约定
//     （`所有 int 几何常量都是 96 DPI 基准 px`），本文件此前 0 处遵守。
//   物理像素修正（1px 描边、±1 取整、SaveDC 哨兵）：**不缩放**。理由见
//     PaintDetailsPage 里列间竖线处的注释与报告 §「明确不缩放」。
//
// 【为什么一律写 theme::Rpx() 而不是裸 Rpx()】
//   本文件有 `using namespace theme;`（第 80 行），而 include/perfpage.h 在
//   namespace tmp 里也定义了 `inline int Rpx(int)`。裸写 `Rpx(...)` 会因
//   using-directive 把 theme::Rpx 注入 tmp 而**二义性编译失败**
//   （C2668: "tmp::Rpx": 对重载函数的调用不明确）。
//   theme.h 已经警告过这一点；此处实测复现后统一加 theme:: 限定。
//   不要「顺手」把限定删掉，也不要改 perfpage.h（不归本文件）。

void FmtCpuD(double pct, wchar_t* b, size_t n) {
    b[0] = 0;
    if (pct < 0.05) return;
    if (pct < 1.0) swprintf_s(b, n, L"%.1f%%", pct);
    else           swprintf_s(b, n, L"%.0f%%", pct);
}

void FmtMemD(uint64_t bytes, wchar_t* b, size_t n) {
    if (!bytes) { b[0] = 0; return; }
    double kb = static_cast<double>(bytes) / 1024.0;
    // 单位后缀是 " K"（MUI id 32884 原文就是 `" K"`，没有 B），不是 " KB"。
    if (kb < 1024.0)         swprintf_s(b, n, L"%.1f K", kb);
    else if (kb < 1048576.0) swprintf_s(b, n, L"%.1f MB", kb / 1024.0);
    else                     swprintf_s(b, n, L"%.1f GB", kb / 1048576.0);
}

} // namespace

// 详细信息页列表几何的**唯一**定义点。
//
// 【为什么要抽出来】PaintDetailsPage 里 listTop/listBot/viewH 是一串局部
// const，滚动系统（tmwindow_events.cpp 的 ScrollTarget）需要同一组数字才能
// 算出正确的 maxScroll 和滑块命中区。此前那组数字在本文件里算一遍、
// 在 events.cpp 里再写一遍字面量 `rcContent_.bottom - 2`，正是本项目
// 踩过两次的"两套原点"错位模式（进程页列头 / 详细信息页行区各一次）。
//
// 行区起点 = rcContent_.top + kHeaderHeight（列头紧贴内容区顶端）
// 行区终点 = rcContent_.bottom - kDetListBottomInset（底部 2px 是原有几何，不为滚动而改）
//
// 【DPI】本函数是**行区几何的唯一出口**，tmwindow_events.cpp 的
// DetailViewportHeight() 直接转调它。因此这里必须已按当前 DPI 换算，
// 调用方**不要再包一层 Rpx()** —— 那会造成双重缩放（120 DPI 下 24→30→37）。
int TmWindow::DetailListHeight() const {
    const int h = static_cast<int>(rcContent_.bottom) - theme::Rpx(kDetListBottomInset) -
                  (static_cast<int>(rcContent_.top) + theme::Rpx(kHeaderHeight));
    return h > 0 ? h : 0;
}

int TmWindow::DetailScrollBarWidth() const { return theme::Rpx(kDetScrollW); }

RECT TmWindow::DetailScrollThumbRect() const {
    RECT none{ 0, 0, 0, 0 };
    const int viewH = DetailListHeight();
    const int btnH  = theme::Rpx(kDetScrollBtnH);
    if (viewH <= btnH * 2) return none;

    const int contentH = static_cast<int>(detailRows_.size()) * theme::Rpx(kRowHeight);
    if (contentH <= viewH) return none;         // 不需要滚动条

    const int maxScroll = contentH - viewH;
    const int sbRight = static_cast<int>(rcContent_.right) - theme::Rpx(kDetSbRightInset);
    const int sbLeft  = sbRight - theme::Rpx(kDetScrollW);
    const int listTop = static_cast<int>(rcContent_.top) + theme::Rpx(kHeaderHeight);
    const int listBot = static_cast<int>(rcContent_.bottom) - theme::Rpx(kDetListBottomInset);

    const int trackTop = listTop + btnH;
    const int trackH   = viewH - btnH * 2;
    int thumbH = trackH * viewH / contentH;
    if (thumbH < theme::Rpx(kDetThumbMinLen)) thumbH = theme::Rpx(kDetThumbMinLen);
    if (thumbH > trackH) thumbH = trackH;

    // 与绘制侧同一式子；detailScrollY_ 理论上已由 SetScrollOffset 钳制，
    // 这里再夹一次，防止中途快照变化导致滑块被顶出轨道（进程页踩过：
    // 滑块压住下箭头，点箭头变成拖滑块）。
    int sy = detailScrollY_;
    if (sy < 0) sy = 0;
    if (sy > maxScroll) sy = maxScroll;
    const int thumbY = trackTop + (maxScroll > 0 ? (trackH - thumbH) * sy / maxScroll : 0);
    (void)listBot;
    return RECT{ sbLeft, thumbY, sbRight, thumbY + thumbH };
}

void TmWindow::PaintDetailsPage(HDC hdc) {
    if (!snap_) return;
    if (detailRows_.empty()) RebuildDetailRows();

    // ---------------------------------------------------------------
    // 几何。
    //
    // 【重要修正】此前这里把列头画在 `rcContent_.top + 40`、行区从 `+64` 起，
    // 而 tmwindow_events.cpp:492 / :534 的点击命中用 `rcContent_.top + 24` 当行区顶 ——
    // 两套原点差 16px，结果点第 N 行选中第 N+1 行，点列头会选第一行。
    //
    // 原版这一页就是一个填满内容区的标准 ListView：列头紧贴内容区顶端。
    // 现在把列头顶到 `rcContent_.top`、高度用通用的 kHeaderHeight(24)，
    // 于是 `hdr.bottom == rcContent_.top + 24`，与 events.cpp 的命中原点天然一致，
    // **不需要改 events.cpp 就修好了这 16px 错位**。
    //
    // 【DPI —— 与 events.cpp 的双重缩放风险】
    // 下面每个几何量都包了 Rpx()。**events.cpp 里对应几处仍是未缩放的
    // `theme::kHeaderHeight` / `- 2` / `- 3` 字面量**（:1348-1350、:1591、
    // :1690、:1214-1216），那是别人的文件，本轮不动。
    // 本机 96 DPI 下两边逐值相同，所以现在没有可见问题；但在 125%/150% 下
    // 绘制侧的行区原点会比命中侧高 Scaled(24)-24 = 6/12px，点第 N 行会选中第 N-1 行。
    // 交接单见 docs/audit/B02_paint_details_dpi.md §「共享常量与跨文件镜像」。
    // ---------------------------------------------------------------
    const int hdrTop    = rcContent_.top;
    const int hdrBottom = rcContent_.top + theme::Rpx(kHeaderHeight);
    const int listTop   = hdrBottom;
    const int listBot   = rcContent_.bottom - theme::Rpx(kDetListBottomInset);
    // 行区高度改用唯一出口 DetailListHeight()，不再在这里算第二遍。
    const int viewH     = DetailListHeight();

    const int rowH = theme::Rpx(kRowHeight);
    const int contentH = static_cast<int>(detailRows_.size()) * rowH;
    const int maxScroll = (std::max)(0, contentH - viewH);

    // 滚动位置钳制。
    //
    // 【修复】此前 detailScrollY_ 只有下限 0 的钳制（tmwindow_events.cpp:613），
    // 滚轮一直往下滚可以让它无限增大 —— 滚过头就整屏空白，而且没有任何办法回到顶部
    // （只有切页才清零）。这里在绘制前把上界钳住，等于把「滚过头」这个状态消灭掉：
    // 无论滚轮事件怎么加，屏上永远不会离开最后一行。
    if (detailScrollY_ > maxScroll) detailScrollY_ = maxScroll;
    if (detailScrollY_ < 0)         detailScrollY_ = 0;

    // 列坐标原点 = 内容区左界。
    //
    // 【对齐方式已按原版实测修正】08_details.md D-11 说「列头与行内文字错位 20px，
    // 列头应改成对齐图标后的文字起点」。我实测否掉了这个说法：
    // 对原版 4x 裁切（未随仓库分发）逐列扫墨迹 ——
    //     列头「名称」墨迹起点   逻辑 x = 10.0
    //     行内齿轮图标起点       逻辑 x =  9.0 ～ 10.0
    // ⇒ **原版列头是与行内图标的左沿对齐的，不是与图标后面的文字对齐。**
    // 所以第 0 列列头左界取 iconLeft（= x0 + kIconLeft = +10），
    // **不做**「对齐到文字」那 22px 的额外偏移 —— 那样列头会跑到图标右边，
    // 与原版正好相反。其余列按 kCellPadLeft，与进程页 PaintColumnHeader 一致。
    const int x0       = rcContent_.left;
    const int iconLeft = x0 + theme::Rpx(kIconLeft);               // 行内图标 / 第 0 列列头左界
    // 文字左界 = 图标右缘 + 6px 间距。6 是 96 DPI 基准值（与 tmwindow_list.cpp
    // 的 `kIconSize + 6` 同源），必须一起缩放。
    const int textLeft = iconLeft + theme::Rpx(kIconSize) + theme::Rpx(6);

    // 滚动条几何
    const bool hasSb = contentH > viewH && viewH > 0;
    const int sbRight = rcContent_.right - theme::Rpx(kDetSbRightInset);
    const int sbLeft  = sbRight - theme::Rpx(kDetScrollW);

    // ---------------------------------------------------------------
    // 列头
    // ---------------------------------------------------------------
    RECT hdr{ rcContent_.left, hdrTop, rcContent_.right, hdrBottom };
    FillSolid(hdc, hdr, kBgHeader);
    DrawHLine(hdc, hdr.left, hdr.right, hdr.bottom - 1, kLineHeader);

    // 列间竖线。
    // 【对照】进程页 PaintColumnHeader 有这条线（1px，#D8D8D8，纵向从 top+6 到 bottom-2），
    // 而详细信息页此前一条都没有 —— 两页画法完全脱节。
    // 原版服务页列头实测（4x 裁切扫描，未随仓库分发）列边界处
    // 确有 8px 宽（= 2 逻辑 px）的浅灰竖条 rgb(239,239,239)~(244,244,244)；
    // 4x 缩放会把 1px 线糊成 2px 并提亮，故仍取进程页同款 #D8D8D8 以求两页一致。
    for (int i = 1; i < kDetColCount; ++i) {
        const int cx = x0 + DetColX(i);
        // 竖线本身恒为 1px（不缩放 —— 1 物理像素的描边在任何 DPI 下都是一条线，
        // 缩放成 1.25px 会被 GDI 取整成 1 或 2，反而出现有的线粗有的线细）。
        // 上下内缩 6px / 2px 是留白，按 96 DPI 基准缩放。
        FillSolid(hdc, RECT{ cx, hdr.top + theme::Rpx(6), cx + 1, hdr.bottom - theme::Rpx(2) },
                  RGB(0xD8, 0xD8, 0xD8));
    }

    HFONT old = static_cast<HFONT>(SelectObject(hdc, fontBold_));
    SetBkMode(hdc, TRANSPARENT);
    SetTextAlign(hdc, TA_LEFT | TA_TOP);

    for (int i = 0; i < kDetColCount; ++i) {
        const int cx    = x0 + DetColX(i);
        // 列头左界：第 0 列与行内图标左沿对齐（原版实测 列头墨迹 10.0 / 图标 9.0），
        // 其余列按 kCellPadLeft。
        const int left  = (i == 0) ? iconLeft : (cx + theme::Rpx(kCellPadLeft));
        const int right = x0 + DetColRight(i) - theme::Rpx(kCellPadRight);
        RECT cell{ left, hdr.top, right, hdr.bottom };
        DrawTextClipped(hdc, kDetCols[i].title, &cell,
                        DT_SINGLELINE | DT_VCENTER | kDetCols[i].align, kTextHeader);
    }

    // ---------------------------------------------------------------
    // 行
    // ---------------------------------------------------------------
    SelectObject(hdc, fontBody_);
    int y = listTop - detailScrollY_;
    wchar_t buf[64];

    // 【裁剪】行区之外的像素一律不画 —— 与进程页 PaintRows 同一类缺陷。
    //
    // 本页的 `detailScrollY_` 由两处写：滚轮（`-= notches * kRowHeight * 3`，
    // 整数倍）和上面的钳制 `if (detailScrollY_ > maxScroll) = maxScroll`。
    // 而 `maxScroll = contentH - viewH`，其中 `viewH = listBot - listTop` ——
    // 这个差值**通常不是 kRowHeight 的整数倍**（内容高是行高的整数倍，
    // 视口高不是），所以钳到上界时 `detailScrollY_` 就带了一个非整行的余数，
    // 第一行的 y 会小于 listTop，行底色/图标/文字上半截画进列头带。
    // 滚轮滚到底就必然触发，不需要拖滚动条。
    //
    // 下面的每个单元格 RECT 只约束文字，DrawAppIcon / FillSolid 是直接落 DC 的。
    // 【为什么不用改了 RECT 的 `clip` 就够】那个 clip 只用于**选中行底色**，
    // 图标（DrawAppIcon(irc)）和文字（rc 用的是 `y`）都没用它，仍会越界。
    const int savedDC = SaveDC(hdc);
    IntersectClipRect(hdc, rcContent_.left, listTop, rcContent_.right, listBot);

    for (const auto* pe : detailRows_) {
        if (y + rowH <= listTop) { y += rowH; continue; }
        if (y >= listBot) break;

        RECT rowRc{ x0, y, hasSb ? sbLeft : rcContent_.right, y + rowH };
        RECT clip = rowRc;
        clip.top = (std::max)(static_cast<int>(clip.top), listTop);
        clip.bottom = (std::min)(static_cast<int>(clip.bottom), listBot);

        // 无交替行条纹 —— 原版 kBgAltRow == kBgList == #FFFFFF，本页也一致。

        // 选中高亮：按 pid 判定（行号每秒都会因重排而变）。
        // 之前这里是 kBgNavSelected，见 kDetSelBg 的注释。
        if (pe && detailSelectedPid_ != 0 && pe->pid == detailSelectedPid_) {
            FillSolid(hdc, clip, kDetSelBg);
        }

        // 逐列绘制。
        for (int i = 0; i < kDetColCount; ++i) {
            const int cx = x0 + DetColX(i);

            switch (kDetCols[i].field) {
            case DF_NAME: {
                // 名称 = 图标 + 文件名（本页不显示进程描述，原版这一列是 ImageName）
                // 图标在行内垂直居中：(行高 - 图标高) / 2。两个量都已换算过，
                // 差值也必然是当前 DPI 的 px，不能再包 Rpx()。
                const int iconTop = y + (rowH - theme::Rpx(kIconSize)) / 2;
                RECT irc{ iconLeft, iconTop,
                          iconLeft + theme::Rpx(kIconSize), iconTop + theme::Rpx(kIconSize) };
                DrawAppIcon(hdc, irc, pe->imageName, pe->pid);

                RECT rc{ textLeft, y, x0 + DetColRight(i) - theme::Rpx(kCellPadRight), y + rowH };
                rc.right = (std::min)(rc.right, rowRc.right);
                DrawTextClipped(hdc, pe->imageName.c_str(), &rc,
                                DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS,
                                kTextPrimary);
                break;
            }
            case DF_PID: {
                swprintf_s(buf, L"%u", pe->pid);
                RECT rc{ cx + theme::Rpx(kCellPadLeft), y, x0 + DetColRight(i) - theme::Rpx(kCellPadRight),
                         y + rowH };
                DrawTextClipped(hdc, buf, &rc, DT_SINGLELINE | DT_VCENTER | DT_RIGHT,
                                kTextPrimary);
                break;
            }
            case DF_STATUS: {
                // 优先级：已挂起 > 无响应；都没有就是空（原版空状态不写字）。
                const wchar_t* statusText = nullptr;
                if (cls_.suspended.count(pe->pid))          statusText = L"已挂起";
                else if (cls_.notResponding.count(pe->pid)) statusText = L"无响应";
                if (statusText) {
                    RECT rc{ cx + theme::Rpx(kCellPadLeft), y, x0 + DetColRight(i) - theme::Rpx(kCellPadRight),
                             y + rowH };
                    DrawTextClipped(hdc, statusText, &rc, DT_SINGLELINE | DT_VCENTER | DT_LEFT,
                                    kTextPrimary);
                }
                break;
            }
            case DF_USER: {
                std::wstring u = ResolveUser(pe->pid);
                // 「选项 → 显示完整账户名」真正生效：
                //   勾选   → "域\用户"（完整，例如 NT AUTHORITY\SYSTEM）
                //   不勾选 → 只显示 "用户"（例如 SYSTEM）
                // 判据就是开关本身，按最后一个反斜杠切；和原版一样不做「本机账户才剥」
                // 的额外判断（原版这一列显示什么，exe 里没有可静态验证的证据）。
                if (!showFullName_) {
                    const size_t bs = u.find_last_of(L'\\');
                    if (bs != std::wstring::npos && bs + 1 < u.size()) {
                        u = u.substr(bs + 1);
                    }
                }
                if (!u.empty()) {
                    RECT rc{ cx + theme::Rpx(kCellPadLeft), y, x0 + DetColRight(i) - theme::Rpx(kCellPadRight),
                             y + rowH };
                    DrawTextClipped(hdc, u.c_str(), &rc,
                                    DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS,
                                    kTextPrimary);
                }
                break;
            }
            case DF_CPU: {
                FmtCpuD(pe->cpuPercent, buf, 64);
                RECT rc{ cx + theme::Rpx(kCellPadLeft), y, x0 + DetColRight(i) - theme::Rpx(kCellPadRight),
                         y + rowH };
                DrawTextClipped(hdc, buf, &rc, DT_SINGLELINE | DT_VCENTER | DT_RIGHT,
                                kTextPrimary);
                break;
            }
            case DF_MEM: {
                // WorkingSetPrivateSize 优先；取不到时退回工作集（否则这一格会全空）。
                FmtMemD(pe->privateBytes ? pe->privateBytes : pe->workingSet, buf, 64);
                RECT rc{ cx + theme::Rpx(kCellPadLeft), y, x0 + DetColRight(i) - theme::Rpx(kCellPadRight),
                         y + rowH };
                DrawTextClipped(hdc, buf, &rc, DT_SINGLELINE | DT_VCENTER | DT_RIGHT,
                                kTextPrimary);
                break;
            }
            }
        }

        y += rowH;
    }

    SelectObject(hdc, old);
    RestoreDC(hdc, savedDC);   // 与行区开头的 SaveDC / IntersectClipRect 配对
                               // （滚动条在行区之外单独画，不能被这段裁剪影响）

    // ---------------------------------------------------------------
    // 垂直滚动条
    //
    // 【修复】此前本页完全不画滚动条（PaintScrollBar 只在进程页分支里调用），
    // 600 个进程只能靠滚轮，用户不知道自己在哪、还剩多少。
    // 原版是标准 ListView，必有垂直滚动条（还带上下箭头按钮）。
    // ---------------------------------------------------------------
    const int btnH = theme::Rpx(kDetScrollBtnH);
    if (hasSb && viewH > btnH * 2) {
        RECT track{ sbLeft, listTop, sbRight, listBot };
        FillSolid(hdc, track, RGB(0xFF, 0xFF, 0xFF));

        RECT upBtn{ sbLeft, track.top, sbRight, track.top + btnH };
        RECT dnBtn{ sbLeft, track.bottom - btnH, sbRight, track.bottom };
        FillSolid(hdc, upBtn, RGB(0xF0, 0xF0, 0xF0));
        FillSolid(hdc, dnBtn, RGB(0xF0, 0xF0, 0xF0));

        auto drawArrow = [&](const RECT& r, bool up) {
            // 【修复：实心三角 → 镂空轮廓】与进程页同一个字形
            // （TmWindow::DrawHollowArrow）。原版是标准 ListView 的滚动条，
            // 两页的箭头是同一颗 SBP_ARROWBTN 字形。
            // 【颜色】原为 #BCBCBC，无任何注释/规范支持，是照抄旧截图
            // （m3.png 系旧版自截图，已证伪）的浅色。原版实测 #606060
            // （original_win10.png 上箭头 x=1237..1243/y=142..147 全部是
            // 0x60,0x60,0x60），与进程页同一个值，故一并改成 #606060。
            DrawHollowArrow(hdc, r, up, RGB(0x60, 0x60, 0x60));
        };
        drawArrow(upBtn, true);
        drawArrow(dnBtn, false);

        // 滑块：只在两端按钮之间滑动；#CDCDCD 直角矩形，最小长度按 DPI 缩放。
        const int trackTop = track.top + btnH;
        const int trackH   = viewH - btnH * 2;
        if (trackH > 0) {
            const int thumbMin = theme::Rpx(kDetThumbMinLen);
            int thumbH = trackH * viewH / contentH;
            if (thumbH < thumbMin) thumbH = thumbMin;
            if (thumbH > trackH) thumbH = trackH;
            const int thumbY = trackTop + (trackH - thumbH) * detailScrollY_ / maxScroll;
            // 滑块左右各内缩 1px（与 DetailScrollThumbRect 返回的右界对齐）：
            // 1 是物理像素修正，**不缩放** —— 缩成 1.25 会被取整吃掉。
            FillSolid(hdc, RECT{ sbLeft + 1, thumbY, sbRight - 1, thumbY + thumbH },
                      RGB(0xCD, 0xCD, 0xCD));
        }
    }
}

void TmWindow::RebuildDetailRows() {
    detailRows_.clear();
    if (!snap_) return;
    for (const auto& e : snap_->processes) {
        // pid 0（System Idle Process）不进列表。
        //
        // 说明：原版详细信息页是否列出 pid 0，我**没有可验证的证据**
        // （手上没有一张原版详细信息页截图，MUI 里也没有对应文案）。
        // 这里维持现状，不按「08_details.md D-12」改 —— 那一条的前提
        // （「原版可以选中 System Idle Process」）本身未经证实。
        if (e.pid == 0) continue;
        detailRows_.push_back(&e);
    }
    std::sort(detailRows_.begin(), detailRows_.end(),
              [](const ProcessEntry* a, const ProcessEntry* b) {
                  return a->imageName < b->imageName;
              });

    // 重建后按 pid 重新定位选中行（列表重排后行号会变）
    SyncDetailSelection();
}

// 详细信息页的选中跟随 pid。
//
// 与进程页的 SyncSelectionToPid 同样的思路：行号不稳定（每秒重排），
// 只有 pid 是可靠的。进程真退出才清选中。
void TmWindow::SyncDetailSelection() {
    detailSelectedRow_ = -1;
    if (detailSelectedPid_ == 0) return;

    for (size_t i = 0; i < detailRows_.size(); ++i) {
        if (detailRows_[i] && detailRows_[i]->pid == detailSelectedPid_) {
            detailSelectedRow_ = static_cast<int>(i);
            return;
        }
    }
    // 快照里也没有了 → 进程真退出
    detailSelectedPid_ = 0;
}

} // namespace tmp
