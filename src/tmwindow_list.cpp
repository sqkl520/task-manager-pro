// tmwindow_list.cpp - 列头、进程列表、状态栏的绘制
#include "tmwindow.h"

#include <windowsx.h>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "gdi32.lib")

namespace tmp {

using namespace theme;

// ============ 列定义（严格对照任务管理器进程页） ============
namespace {

struct ColDef {
    const wchar_t* title;
    int width;
    UINT align;
};

// 任务管理器进程页列：名称、状态、CPU、内存、磁盘、网络、GPU
//
// 【全部 7 列宽度现在都来自原版 .data 列池表，不再是"自由裁量"】
// 出处 `docs/specs/O_strings.md` §2.1（taskmgr.exe 列池表头 0x129e84，15 列）：
//   名称 234 / 类型 150 / 状态 120 / 发布者 150 / PID 50 / 进程名称 200 /
//   命令行 260 / CPU 73 / 内存 73 / 磁盘 73 / 网络 73 / GPU 73 / GPU 引擎 200 /
//   电源使用情况 100 / 电源使用情况趋势 100
// 进程页用到的是名称/状态/CPU/内存/磁盘/网络/GPU 这 7 项，宽度逐项照抄池表。
//
// 【交叉验证】真原版截图 `docs/reference/original_win10.png` 实测列边界
//   名称 0..236 | 状态 236..356 | CPU 356..429 | 内存 429..502
// ⇒ 236 / 120 / 73 / 73，与池表 234 / 120 / 73 / 73 逐项吻合（名称差 2px 是边框）。
// 两源独立得出同一组数，所以 磁盘/网络/GPU 的 73 也是**池表实测值**，
// 不再是我方 8px 栅格自定的 82。
//
// 【已废除的旧理由】"六列等宽让数值列视觉节律一致" —— 原版实测三列不齐
// （120 / 73 / 73），"等宽"是我方自创的审美，不是原版行为。
//
// 【类型列不属于进程页】
// 元素归类里的「类型」只出现在 STRINGTABLE **2313**
// （名称 / PID / 类型 / 状态 / 发布者 / 进程名称 / 命令行）—— 那是**详细信息页**的列名表。
// 进程页的列名在别处（名称 / 状态 / CPU / 内存 / 磁盘 / 网络 / GPU），**没有类型列**。
// 类型列归 tmwindow_details.cpp 的 kDetCols。
//
// 【列宽依据（本轮更新）—— 两源独立吻合】
// 此前这里写着「微软规范没有规定 ListView 列宽 … 原版真值不存在，只能按 8px 栅格自定」，
// 后来拿到真原版截图 `docs/reference/original_win10.png`（1251×842）能逐像素量。
// 本轮又拿到**更强的一源**：原版 taskmgr.exe 的 `.data` 列池表（`docs/specs/O_strings.md` §2.1），
// 两源给出同一组数 → 不再是推断。
//
// 【截图实测列边界】列头带 y=92..135 内，列间 1px 竖线出现在 x=236 / 356 / 429 / 502
// （y=100 采样均为 `#DFDFDF`；复核脚本 `docs/evidence/_hh_scan2.ps1` / `_hh_scan5.ps1`）：
//
//   名称   0..236    宽 236（池表 234 + 边框；见 kNameColWidth）
//   状态 236..356    宽 120
//   CPU  356..429    宽  73
//   内存 429..502    宽  73
//   ——  502..1232   730px 纯白（逐像素扫描非白像素 = 0，x=1233 起是滚动条）
//
// 【数值列 73 会不会截断 "162.8 MB/s"】会 —— 但那是原版行为，不是我方的错。
// 原版内存列宽 73、右侧内边距 4，可容纳约 10 个半角字符；原版截图里
// 内存值显示为 `1,413.1 …`（截断成省略号），正是列宽 73 的结果。
// 我方 FmtMemText 产生的 "1.4 GB" 更短，不会到截断那一步。
const ColDef kCols[] = {
    { L"名称",  234, DT_LEFT  },   // 池表 234（截图 236 = 234 + 2px 边框）
    { L"状态",  120, DT_LEFT  },   // 池表 120
    { L"CPU",    73, DT_RIGHT },   // 池表 73
    { L"内存",   73, DT_RIGHT },   // 池表 73
    { L"磁盘",   73, DT_RIGHT },   // 池表 73（此前 82 是无依据的 8px 栅格值）
    { L"网络",   73, DT_RIGHT },   // 池表 73（同上）
    { L"GPU",    73, DT_RIGHT },   // 池表 73（同上）
};
const int kColCount = sizeof(kCols) / sizeof(kCols[0]);

// 【已删除】kFixedColsWidth = 86+82*5。
// 它是「固定列总宽」的硬编码默认值，但全项目零引用 —— 实际宽度由
// `NameColWidth()` 用 `LiveW(i)` 运行时累加得出（见下方），支持拖拽后的宽度。
// 留着这个常量只会在改列宽时误以为它生效。

// 当前生效的列宽数组指针。
//
// 【为什么用文件级指针】kCols/ColX/ColW/NameColWidth 都在这个匿名命名空间里，
// 被几十处绘制代码调用。要让「拖动列边界后立刻生效」，最省事且不易漏改的做法
// 是让这几个辅助函数直接读运行时宽度，而不是把每个调用点都改成传参版本。
// 每次绘制前由 TmWindow 的成员函数把它指向自己的 colWidths_。
const int* g_liveWidths = nullptr;

// 当前生效的列可见性数组指针（与 g_liveWidths 同源的「文件级直读」约定）。
// nullptr = 全部可见（构造期/离屏渲染等尚未挂上时不会误隐藏）。
const bool* g_liveVisible = nullptr;

// 列是否参与布局：名称列恒可见；其余看 g_liveVisible。
inline bool LiveVisible(int idx) {
    if (idx <= 0) return true;              // 名称列永远可见（硬约束，见 SetColumnVisible）
    if (idx >= kColCount) return false;
    return g_liveVisible ? g_liveVisible[idx] : true;
}

inline int LiveW(int idx) {
    if (idx <= 0) return 0;
    if (g_liveWidths && g_liveWidths[idx] > 0) return g_liveWidths[idx];
    return kCols[idx].width;
}

// 【列可见性】所有列几何都从这里派生，隐藏列**不占宽度**。
// 只有「名称列 + 可见的固定列」参与累加，隐藏列一律跳过 ——
// 这是「隐藏 CPU 后后面的列左移填位」得以成立的那一处判断。
inline int VisW(int idx) {
    return LiveVisible(idx) ? LiveW(idx) : 0;
}

// ===== 数据行左侧三段的位置（真原版截图实测）=====
//
// 【证据复核：orig2_rows.png 是我方截图，不得再引用】
// 本轮核对发现 `docs/reference/orig2_rows.png` 是**我方旧版本的截图**，不是原版：
//   · 整图 261×205、781 种颜色；三角所在的小框里只有 **11 种颜色**，
//     而且是**纯 #5A5A5A、零抗锯齿**（像素值只有 5A5A5A / FFFFFF 两档）。
//     这正是 GDI `Polygon()` 的特征；真主题绘制一定有抗锯齿渐变。
//   · 真原版 `original_win10.png` 同尺寸区域有 3980 种颜色。
// 项目历史上已两次踩同一个坑（`m3.png`、`truth1.png` 都是我方截图被当成原版），
// 本轮是第三次，故在这里钉死判据。
//
// 【真原版判据】`original_win10.png`（1251×842）：标题栏「任务管理器」、5 个页签、
//   底部「简略信息(D)」、箭头带抗锯齿。`orig2_full.png`（1250×843）同判据通过，
//   且两者的字符形状**逐像素一致**（下方归一化数据在两图上完全相同）。
//
// ---- 实测：箭头字形（不是三段位置，是那个 ▶ 本身）----
// `original_win10.png` 的 Edge 行 y=239..247，x=12..17（相对客户区左沿）：
//   y=239  x=12 D2D2D2  x=13 BFBFBF
//   y=240  x=12 BFBFBF  x=13 A6A6A6  x=14 BFBFBF
//   y=241  x=13 BFBFBF  x=14 A6A6A6  x=15 BFBFBF
//   y=242  x=14 BFBFBF  x=15 A6A6A6  x=16 BFBFBF
//   y=243  x=14 FAFAFA  x=15 ACACAC  x=16 A6A6A6  x=17 D4D4D4
//   y=244  x=14 BFBFBF  x=15 A6A6A6  x=16 BFBFBF
//   y=245  x=13 BFBFBF  x=14 A6A6A6  x=15 BFBFBF
//   y=246  x=12 BFBFBF  x=13 A6A6A6  x=14 BFBFBF
//   y=247  x=12 D2D2D2  x=13 BFBFBF
// 这才是原版的形状：**9 行高、核心 1px 宽的细描边 V 形折线（chevron）**，
// 尖端朝右（折叠 ▶），不是实心三角。旧注释里的「8×7 实心三角」是错的。
//
// 形状判读（用抗锯齿加权质心，比挑最暗像素稳）：
//   行质心 = 12.59, 13.00, 14.00, 15.00, 15.81, 15.00, 14.00, 13.00, 12.59
// ⇒ 左臂在 x≈12.6、尖端在 x≈15.8，跨度仅 3.2px，上下完全对称。
// 取值规律：每行 1 个核心像素，含抗锯齿共 2~3px 宽的足迹。
// 归一化到「相对核心左沿」的列偏移（下文 kExpandoCore）：
//   行0..8: 0, 0, 1, 2, 3, 2, 1, 0, 0
//
// 三段的推导（x0 = rcList_.left + kIndentPerLevel(6)；本机 rcList_.left=0 → x0=6）：
//   箭头槽左 = x0 + 3 = 9（命中区口径，非字形）
//   图标左 = 30  ⇒ 相对 x0 偏移 24
//   文字左 = 54  ⇒ 图标右沿 45 + 9px 间隔
//
// 【箭头到图标间隔 14px 是关键】原版字形右沿 17 → 图标 30 空 13px；我方此前只有 4px，
// 所以箭头和图标糊在一起。这个 13 不是"沟槽"，是原版真实的留白。
constexpr int kArrowLeftOffset = 3;    // 箭头左 = x0 + 3
constexpr int kArrowWidth      = 8;    // 沟槽宽（保留：命中区仍按 9..17 算，见 TreeTriangleX）
constexpr int kArrowHeight     = 9;    // 字形高（真原版实测 9 行，见下方 DrawRowExpando 表）
constexpr int kRowIconLeft     = 30;   // 图标左沿（绝对，相对客户区）
constexpr int kRowTextLeft     = 54;   // 文字左沿（绝对，相对客户区）

// 名称列的**总占宽**（含展开箭头沟槽 kIndentPerLevel），单位 px（96 DPI 基准）。
//
// 【234 = 原版 .data 列池值；截图实测 236 = 234 + 边框】
//   池表（`docs/specs/O_strings.md` §2.1，表头 0x129e84）第 0 行：名称 ID 37002 宽 **234**。
//   真原版截图 `docs/reference/original_win10.png`（1251×842）逐像素实测：
//   列间竖线在 x=236（`#DFDFDF`，y=100 采样），名称列列头文字 x=9..33；
//   下一条竖线在 x=356 ⇒ 名称列 = 0..236、状态列 = 236..356（宽 120）。
//   236 − 234 = 2px，正是列头单元格左右各 1px 边框带的宽度 → 两源不矛盾。
//   本轮以池表 234 为准（它才是"声明的宽度"，2px 属于边框）。
//
// 【为什么不是弹性】若名称列吃剩余宽度，1251 宽的窗口下它会撑满到 1250，
// 而不是停在 236。原版右侧那 730px 纯白就是"固定列 + 左对齐"的铁证。
//
// 【为什么 234 而 NameColWidth() 返回 228】本文件的坐标口径是
//   `名称列占宽 = NameColWidth() + kIndentPerLevel`（见 ColX/ColW/PaintColumnHeader：
//   名称列的左界从 x0 = left + kIndentPerLevel 起算，绘制时右边再加回 6）。
//   234 − 6 = 228。这样状态列左界 = left + 6 + 228 = 234，与池表逐项对齐。
constexpr int kNameColWidth = 234;

// 计算名称列宽度。返回值是**不含沟槽**的口径（= 总占宽 − kIndentPerLevel）。
//
// 【原版：固定 234】不再用"可用宽度 − 固定列宽"的弹性公式。
//
// 【窄窗兜底 —— 本轮简化，删掉了 180 这个死下限】
//   旧代码是三分支：
//     1) availForName >= want      → want
//     2) availForName >= 180       → availForName
//     3) 否则                       → availForName（<0 时取 0）
//   分支 2 和 3 的返回值**完全相同**（都是 availForName），所以那个 180 阈值
//   从来没有生效过，只是让注释看起来有保护。本轮改成两个分支。
//   真实的保护依然成立：窗口太窄时名称列跟着收窄，而不是硬撑 234 把右边列
//   （磁盘/网络/GPU）推出可视区 —— 出界的列既画不出来也点不到，比名称列窄更糟。
//   默认 4 列时列总宽 = 234+120+73+73 = 500；7 列全开 = 719，普通窗口都放得下。
inline int NameColWidth(int availWidthIgnoringIndent) {
    int fixed = 0;
    for (int i = 1; i < kColCount; ++i) fixed += VisW(i);
    // 【DPI】kNameColWidth 是 96 DPI 基准值；调用方传进来/回去的口径都**不含**
    // kIndentPerLevel（调用方自己减、ColX/ColW 自己加），所以这里减掉未缩放的
    // kIndentPerLevel：总占宽 = kIndentPerLevel + 返回值 = Scaled(234) ✓。
    const int want = theme::Scaled(kNameColWidth) - kIndentPerLevel;   // 本机 = 228
    const int availForName = availWidthIgnoringIndent - fixed;
    if (availForName >= want) return want;
    return availForName > 0 ? availForName : 0;
}

// 第 idx 列的名义左界（从 x0 起算）。隐藏列不占宽度，但其左界仍可计算
// （用于判断「它现在是否还有实际绘制区域」）。
int ColX(int idx, int nameW) {
    int x = 0;
    for (int i = 0; i < idx; ++i) x += (i == 0) ? nameW : VisW(i);
    return x;
}

int ColW(int idx, int nameW) {
    if (idx == 0) return nameW;
    return VisW(idx);
}

// ============ 列宽：加载 / 保存 / 运行时取值 ============

void FmtCpuText(double pct, wchar_t* buf, size_t cch) {
    // 【依据：MUI 字符串 37036 `%s%%`】**原版 CPU 列是先格式化数字、再拼 `%`**，
    // 所以数值为 0 时显示 `0%` 而不是空白。此前这里对 <0.05 直接返回空串，
    // 导致大量行整列空白，与原版「整列铺满百分比」的观感差别很大。
    // 出处 docs/specs/O_strings.md §数值格式（37036 `%s%%`）。
    if (pct < 0.05)            swprintf_s(buf, cch, L"0%%");
    else if (pct < 1.0)        swprintf_s(buf, cch, L"%.1f%%", pct);
    else                       swprintf_s(buf, cch, L"%.0f%%", pct);
}

void FmtMemText(uint64_t bytes, wchar_t* buf, size_t cch) {
    if (bytes == 0) { buf[0] = 0; return; }
    double kb = static_cast<double>(bytes) / 1024.0;
    // 单位串用 `KB` 而不是 `K`。
    // 【证据 docs/evidence/tm/mui3.txt:693】MUI 里内存类单位串是 `%s KB`，
    // 同文件 `:454` 的速率串是 `1 KB/秒` —— Windows 一律写 `KB`，从不写裸 `K`。
    // 此前这里写 `%.1f K`，少一个 B，是明显的拼写级错误。
    //
    // 【未改：MB → GB 切换阈值与千位分隔符】
    // `02_rows.md` 主张原版 1,392.4 MB（≈1.36GB）仍显示 MB、且带千分位逗号，
    // 但它的证据图 `6.png` **本机不存在**，无法复核；且 MUI 里同时存在
    // `%1.0f MB %s` / `%1.0f GB %s` 两组格式串，反证 GB 分支确实存在。
    // 故按「证据不足不改」保留现有 `<1GB 用 MB、否则 GB` 的分支。
    if (kb < 1024.0)               swprintf_s(buf, cch, L"%.1f KB", kb);
    else if (kb < 1048576.0)       swprintf_s(buf, cch, L"%.1f MB", kb / 1024.0);
    else                           swprintf_s(buf, cch, L"%.1f GB", kb / 1048576.0);
}

// 磁盘吞吐：字节/秒 → MB/s。低于 0.05 MB/s 视为 0（否则一堆 0.0 MB/s 噪音）。
//
// 【本函数的注释已重写】
// 旧注释声称「磁盘卡片读作 162.8 MB/s，所以恒一位小数」，依据是照片裁切图；
// 该图来源无法证实（见 DisplayName 上方的「证据复核」段），那条依据作废。
// 改为「已知偏差 + 交接」：
//
// 【已知偏差】MUI 里原版的磁盘速率是 `37034 %s MB/秒`，其中 `%s` 是**单位串**，
// 取自单位表 `32880 字节/秒` / `32881 KB/秒` / `32882 MB/秒` / `32883 GB/秒`
// —— 即原版**按量级自动切单位**，本函数固定 MB/s 与之不符。
// 出处 docs/specs/O_strings.md §速率格式与单位表（逐 id 对读）。
// 【为什么不在这里改】单位切换会改变实际显示文本，属于格式串的行为变更，
// 应该由格式串的 owner（O/序列化一侧）统一处理，不要顺手改半个。
// 【交接】见报告交接单「格式串单位切换」一条。
//
// 小数位 1 位：【无依据，沿用旧值】。MUI 的 `%s` 只是字符串槽，
// 位数由格式化侧决定，资源层没有规定。
void FmtDiskText(double bytesPerSec, wchar_t* buf, size_t cch) {
    buf[0] = 0;
    if (bytesPerSec < 0.05 * 1048576.0) return;   // 低于 0.05 MB/s 视为 0
    swprintf_s(buf, cch, L"%.1f MB/s", bytesPerSec / 1048576.0);
}

// 网络吞吐：比特/秒 → Mbps。
//
// 【已知偏差，同 FmtDiskText】MUI 的 `37035 %s Mbps` 的 `%s` 是单位串，
// 单位表 `32876 bps` / `32877 Kbps` / `32878 Mbps` / `32879 Gbps` ——
// 原版按量级切换，本函数固定 Mbps。不改的理由同上（行为变更交给格式串 owner）。
//
// 阈值 0.05 Mbps (=50 kbps) 以下视为 0：那是后台心跳噪音，显示出来只会
// 满屏 "0.0 Mbps"。【无依据，沿用旧值】
void FmtNetText(double bitsPerSec, wchar_t* buf, size_t cch) {
    buf[0] = 0;
    if (bitsPerSec < 0.05 * 1.0e6) return;
    swprintf_s(buf, cch, L"%.1f Mbps", bitsPerSec / 1.0e6);
}

// GPU 使用率
void FmtGpuText(double pct, wchar_t* buf, size_t cch) {
    buf[0] = 0;
    if (pct < 0.05) return;
    if (pct < 1.0) swprintf_s(buf, cch, L"%.1f%%", pct);
    else           swprintf_s(buf, cch, L"%.0f%%", pct);
}

} // namespace

// 存储位置：与任务管理器同一个注册表键，但用独立的值名，
// 避免破坏原版那份 4840 字节的 Preferences 二进制。
static constexpr wchar_t kRegKey[]    = L"Software\\Microsoft\\Windows\\CurrentVersion\\TaskManager";
static constexpr wchar_t kRegValue[]  = L"TaskManagerPro_ColumnWidths";

void TmWindow::LoadColumnWidths() {
    // 列可见性跟着列宽一起恢复。放在这里（而不是 tmwindow.cpp 的 Create）
    // 是因为 tmwindow.cpp 不归本轮改动范围，而 LoadColumnWidths 已经在
    // Create 里被调用一次 —— 两件事都是「列视图的持久化状态」，同源同点恢复。
    LoadColumnVisibility();

    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return;

    DWORD type = 0, cb = sizeof(colWidths_);
    BYTE buf[sizeof(colWidths_)] = {};
    if (RegQueryValueExW(k, kRegValue, nullptr, &type, buf, &cb) == ERROR_SUCCESS &&
        type == REG_BINARY && cb == sizeof(colWidths_)) {
        memcpy(colWidths_, buf, sizeof(colWidths_));
        // 合法性校正：任何越界值回落到默认，避免读到坏数据后列宽错乱
        for (int i = 1; i < kSortColCount; ++i) {
            if (colWidths_[i] < 40 || colWidths_[i] > 600) {
                colWidths_[i] = kCols[i].width;
            }
        }
    }
    RegCloseKey(k);
}

// ---- 排序状态持久化 ----
//
// 原版把「按哪列排、升序还是降序」记进注册表，重启后保持。
// 我方此前一律回到默认（名称升序），用户每次打开都要重新点。
//
// 存成一个 DWORD：低 8 位 = 排序列，第 8 位 = 是否降序。
// 与列宽用同一个注册表键、不同的值名。
static constexpr wchar_t kRegValueSort[] = L"TaskManagerPro_Sort";
static constexpr wchar_t kRegValueGroup[] = L"TaskManagerPro_GroupCollapsed";

void TmWindow::LoadSortState() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return;

    DWORD type = 0, cb = sizeof(DWORD), v = 0;
    if (RegQueryValueExW(k, kRegValueSort, nullptr, &type,
                         reinterpret_cast<LPBYTE>(&v), &cb) == ERROR_SUCCESS &&
        type == REG_DWORD) {
        const int col = static_cast<int>(v & 0xFF);
        const bool desc = ((v >> 8) & 1) != 0;
        // 合法性：列号越界就保持默认，避免读到坏数据后排序错乱
        if (col >= 0 && col < kSortColCount) {
            sortCol_ = col;
            sortDesc_ = desc;
        }
    }
    // 组折叠状态：一个 DWORD，第 i 位 = 第 i 组是否折叠（1 = 折叠）
    DWORD gv = 0;
    cb = sizeof(DWORD);
    if (RegQueryValueExW(k, kRegValueGroup, nullptr, &type,
                         reinterpret_cast<LPBYTE>(&gv), &cb) == ERROR_SUCCESS &&
        type == REG_DWORD) {
        for (int i = 0; i < 3; ++i) groupExpanded_[i] = ((gv >> i) & 1) == 0;
    }
    RegCloseKey(k);
}

void TmWindow::SaveSortState() {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    DWORD v = static_cast<DWORD>(sortCol_ & 0xFF) | (sortDesc_ ? 0x100u : 0u);
    RegSetValueExW(k, kRegValueSort, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&v), sizeof(v));
    DWORD gv = 0;
    for (int i = 0; i < 3; ++i) if (!groupExpanded_[i]) gv |= (1u << i);
    RegSetValueExW(k, kRegValueGroup, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&gv), sizeof(gv));
    RegCloseKey(k);
}

void TmWindow::SaveColumnWidths() {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(k, kRegValue, 0, REG_BINARY,
                   reinterpret_cast<const BYTE*>(colWidths_), sizeof(colWidths_));
    RegCloseKey(k);
}

int TmWindow::EffectiveColWidth(int col) const {
    if (col <= 0 || col >= kSortColCount) return 0;
    return colWidths_[col] > 0 ? colWidths_[col] : kCols[col].width;
}

void TmWindow::SetColumnWidth(int col, int w, bool persist) {
    if (col <= 0 || col >= kSortColCount) return;
    if (w < 40) w = 40;
    if (w > 600) w = 600;
    colWidths_[col] = w;
    if (persist) SaveColumnWidths();
    InvalidateAll();
}

// ============ 列可见性（隐藏列） ============
//
// 【这一节是「隐藏列」真正成立的地方】
// 之前项目里只有列宽、没有可见性维度，列头右键菜单即使接上线也表达不出
// 「这一列被隐藏了」。这里补齐三个环节：
//   1. 状态：colVisible_[7]（tmwindow.h，默认 前 4 列可见、后 3 列隐藏 ——
//      依据原版截图 + 池表，见那里的注释）
//   2. 持久化：独立的注册表值名，不动原有的 ColumnWidths 二进制
//   3. 几何：tmwindow_list.cpp 的 VisW() —— 隐藏列宽度按 0 计，
//      于是 NameColWidth / ColX / ColW / 绘制 / 命中测试全部自动左移填位

static constexpr wchar_t kRegValueVis[] = L"TaskManagerPro_ColumnVisible";

// kRegKey / kCols / kColCount / kSortColCount 都定义在**之前**的位置：
//   kRegKey 在上面的「列宽：加载/保存」段开头，kCols/kColCount 在匿名命名空间，
//   kSortColCount 是 TmWindow 的 static constexpr 成员。
// 这里只是集中写可见性逻辑，没有前向引用问题。

int TmWindow::ColumnCount() { return kColCount; }

const wchar_t* TmWindow::ColumnTitle(int idx) {
    if (idx < 0 || idx >= kColCount) return nullptr;
    return kCols[idx].title;
}

bool TmWindow::IsColumnVisible(int idx) const {
    if (idx < 0 || idx >= kSortColCount) return false;
    if (idx == 0) return true;              // 名称列恒可见
    return colVisible_[idx];
}

// 名称列（0）**永远拒绝隐藏**，返回 false 且不改状态。
// 这是硬约束：全隐藏后列表里一行字都没有，用户没有任何锚点。
bool TmWindow::SetColumnVisible(int idx, bool v) {
    if (idx <= 0 || idx >= kSortColCount) return false;
    // 名称列由上面的 idx<=0 挡住 →「至少留名称列」自动成立。
    // 其余列允许全部隐藏（与原版一致：可以只剩名称列）。
    if (colVisible_[idx] == v) return false;
    colVisible_[idx] = v;
    // 悬停/按下/排序指针都不能停在一个刚被隐藏的列上 ——
    // 否则列头悬停高亮会画在「那一列原来的位置」（现在已经是别人的格子），
    // 排序箭头则会指着一个不存在的列。
    if (!v) {
        if (hoverCol_ == idx) hoverCol_ = -1;
        if (pressedCol_ == idx) pressedCol_ = -1;
    }
    // 隐藏排序列时把排序落到名称列，否则排序按一个看不见的列进行，
    // 用户看不出列表是按什么排的（绘制侧也会跳过箭头）。
    if (!v && sortCol_ == idx) {
        sortCol_ = 0;
        sortDesc_ = false;
    }
    SaveColumnVisibility();
    RebuildRows();       // 排序列可能变了，行序要重算
    InvalidateAll();
    return true;
}

void TmWindow::ResetColumnVisibility() {
    bool changed = false;
    for (int i = 1; i < kSortColCount; ++i) {
        if (!colVisible_[i]) { colVisible_[i] = true; changed = true; }
    }
    if (changed) {
        SaveColumnVisibility();
        // 原先被隐藏的排序列可能是恢复对象：如果当前排序落在一个
        // 「刚好被恢复」的列上本来就没问题；但恢复前 sortCol_ 已被
        // SetColumnVisible 挪到名称列，这里不必再动排序。
        InvalidateAll();
    }
}

void TmWindow::LoadColumnVisibility() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD type = 0, cb = sizeof(colVisible_);
        BYTE buf[kSortColCount] = {};
        if (RegQueryValueExW(k, kRegValueVis, nullptr, &type, buf, &cb) == ERROR_SUCCESS &&
            type == REG_BINARY && cb == static_cast<DWORD>(kSortColCount)) {
            for (int i = 0; i < kSortColCount; ++i) colVisible_[i] = (buf[i] != 0);
        }
        RegCloseKey(k);
    }
    // 【合法性校正，无论是否读到都执行】
    // 名称列必须可见 —— 即使注册表里被写成 0（老版本/手工改过）也强制改回，
    // 否则启动后列表没有名称列。
    colVisible_[0] = true;
    // 若恢复出来的排序列恰好是隐藏列，排序回落到名称列 —— 否则列表按一个
    // 看不见的列排序，界面上没有任何排序指示，用户会以为排序坏了。
    // （此时还没建行，改 sortCol_ 不影响已有数据。）
    if (sortCol_ < 0 || sortCol_ >= kSortColCount || !colVisible_[sortCol_]) {
        sortCol_ = 0;
        sortDesc_ = false;
    }
}

void TmWindow::SaveColumnVisibility() {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    BYTE buf[kSortColCount] = {};
    for (int i = 0; i < kSortColCount; ++i) buf[i] = colVisible_[i] ? 1 : 0;
    RegSetValueExW(k, kRegValueVis, 0, REG_BINARY, buf, sizeof(buf));
    RegCloseKey(k);
}

// 命中列边界：返回该边界**左侧**那一列的索引。
// 拖拽语义是「拖 A 列的右边界 = 改 A 的宽度」，所以返回左邻列更直接。
int TmWindow::HitTestColResize(int x, int y) const {
    g_liveWidths = colWidths_;
    g_liveVisible = colVisible_;
    if (y < rcColHeader_.top || y >= rcColHeader_.bottom) return -1;
    if (x < rcColHeader_.left || x >= rcColHeader_.right) return -1;

    const int nameW = NameColWidth(rcColHeader_.right - rcColHeader_.left - kIndentPerLevel);

    // 起点与 HitTestColumn / PaintColumnHeader 一致：名称列从 rcColHeader_.left 起，
    // 宽度含那 18px 的展开箭头沟槽。否则拖拽边界与看到的列边界会错开 18px。
    //
    // 【列可见性】隐藏列用 ColW/VisW 的**可见宽度**参与累加（隐藏为 0），
    // 且隐藏列自己不作为拖拽目标：否则会在一个看不见的列上出现双向箭头，
    // 拖了之后「什么都没有的空白处」宽度发生变化，非常诡异。
    int cx = rcColHeader_.left;
    for (int i = 0; i < kColCount; ++i) {
        int w = (i == 0) ? (nameW + kIndentPerLevel) : EffectiveColWidth(i);
        if (i > 0 && !LiveVisible(i)) continue;     // 隐藏列不占位、不可拖
        cx += w;
        // 命中带宽 4px：边界像素本身 + 左侧 2px + 右侧 1px（左闭右开）。
        //
        // 【修正 · _PLAN.md A-14 / 01-10c】原先是 `cx-3 .. cx+3`（7px 宽）。
        // 原版 listview 的可拖拽判定约 4..5px —— 7px 太宽，紧挨着边界点击
        // 会先被判成拖拽、排序点不动（列头点击与拖拽的优先级里拖拽在前）。
        if (x >= cx - 2 && x <= cx + 1) return i;
    }
    return -1;
}



// 进程的显示名称：**保留 .exe 后缀**。
//
// 【规则】取得到文件描述（FileDescription）就用描述，取不到就用**带 .exe 的文件名**。
//
// 【依据等级：无原版像素证据】
// 这条规则原先是靠裁切图读字得出的（`迅雷 (13)` / `Windows 资源管理器 (2)` 是
// 文件描述；`mysqld.exe` / `Tailscale service` 是文件名兜底）。本轮复核发现：
//   - 那批裁切图**没有任何一张能证明来自原版**（见下方"证据复核"），
//   - MUI 字符串表里也没有「拿不到描述就显示文件名」这类说明。
// 所以这条现在是**按 Win32 语义推导**（`SHGetFileInfo` 的 FileDescription 与
// 文件名的关系）而不是「实测」。结论本身合理，保留；但不再声称有像素依据。
//
// 【证据复核 —— 本轮做的一次重要清理】
// 先前多份报告（含 theme.h / tmwindow_list.cpp 的注释）把下列文件称作
// 「真原版裁切图」：`original_win10.png`、`n_col.png`、`c_head_left.png`、
// `c_head_right.png`、`c_heat.png`、`c_bottom.png`、`c_menu.png`、
// 手拍的 4x 放大裁切（未随仓库分发）。本轮用两份独立手段核对：
//   1. 我方旧版指纹色（`#E2EFFA` / `#C6E4F7` / `#E5F3FB`，见
//      U_themecolors.md §5.0）：这批图里**一次都没出现** → 不是"我方渲染产物"；
//   2. 拿它们的高方差区块回查 `truth1.png`、`m3.png`、
//      `truth2.png`、`p0_processes.png` 四张全窗口图：
//      **零命中** → 也不来自这四张。
// 结论：这批裁切图是**来源未记录**的孤立样本，既不能证明是原版、也不能证明是我方，
// 因此**不能当权威**。本项目此后不再用它们做几何/配色依据。
// 唯一能证明来源的图是原版全屏截图 `original_win10.png`（3300×2100）——
// 它是 `truth1.png` 的 3 倍最近邻放大（逐像素 6,930,000 点 differing=0），
// 而 truth1 已由 _FINAL_VERDICT.md 判定为我方截图。所以它同样不可用。
//
// 注意：本函数只做「拿不到描述时的兜底」，恒等返回是正确的 —— 真正的描述解析
// 在 `src/tmwindow_icons.cpp` 的 `ProcessDisplayName()` 里（先 GetFileDescription）。
std::wstring DisplayName(const std::wstring& imageName) {
    return imageName;
}

// ============ 列宽与列头命中测试 ============

int TmWindow::CurrentNameColWidth() const {
    g_liveWidths = colWidths_;
    g_liveVisible = colVisible_;
    return NameColWidth(rcList_.right - rcList_.left - kIndentPerLevel);
}

void TmWindow::TreeTriangleX(int indentLevel, int& left, int& right) const {
    // 展开箭头的水平区间。
    //
    // 【真原版截图实测】docs/reference/orig2_full.png / orig2_rows.png
    // 数据行左侧三段位置（相对客户区左沿，边框在 x=1..2）：
    //     箭头  x=9..16   （宽 8，实心右向三角，色 #5A5A5A）
    //     图标  x=30..45  （16×16）
    //     文字  x=54..
    // 箭头右沿(16) 到图标左沿(30) **间隔 14px** —— 这是原版的关键留白，
    // 我方此前只有 4px，导致"箭头和图标糊在一起"。
    //
    // 【为什么 UIFILE 推不出来】
    // UIFILE 写 TmFirstColumn.padding.left=6、ViewExpandoImageWrapper.width=22、
    // padding=rect(4,5,2,0)。按 DUI 规则推算箭头应在 x≈14，但实测是 9。
    // 说明 DUI 布局引擎在运行时还有一层未文档化的边距。所以这里**直接按实测值定**，
    // 不再从 UIFILE 推算（实测优先于推算，这条在 theme.h 里已写明）。
    //
    // x0 = rcList_.left + kIndentPerLevel(6)，本机 rcList_.left = 0 ⇒ x0 = 6。
    // 箭头左沿 9 ⇒ 相对 x0 偏移 3。
    const int x0 = rcList_.left + kIndentPerLevel;
    const int rowIndent = indentLevel * kTreeIndent;
    left  = x0 + rowIndent + kArrowLeftOffset;    // 6 + 3 = 9
    right = left + kArrowWidth;                    // 9 + 8 = 17（实测右沿 16，含端点）
}

int TmWindow::HitTestColumn(int x, int y) const {
    g_liveWidths = colWidths_;
    g_liveVisible = colVisible_;
    // 只有落在列头带内才算
    if (y < rcColHeader_.top || y >= rcColHeader_.bottom) return -1;
    if (x < rcColHeader_.left || x >= rcColHeader_.right) return -1;

    const int nameW = NameColWidth(rcColHeader_.right - rcColHeader_.left - kIndentPerLevel);

    // 名称列的单元格从 `rcColHeader_.left` 开始（宽度多出 kIndentPerLevel 的沟槽），
    // 其余列仍在原位置 —— 这样后续列边界与 `ColX()` 完全一致，不会整体偏 18px。
    //
    // 【修正】悬停高亮已改为覆盖名称列左侧完整的 18px（见 PaintColumnHeader），
    // 而这里原先从 x0（= left+18）起，导致 `left .. x0-1` 这 18px 高了亮却
    // **点不中任何列**（返回 -1 → 点了不掉排序）。绘制与命中的横向起点必须同一个值。
    //
    // 【列可见性】隐藏列宽度为 0（ColW 走 VisW），既不占位也命中不到，
    // 于是后续列自然左移填补空位；绘制与命中用的是同一套 ColW，不会错位。
    int cx = rcColHeader_.left;
    for (int i = 0; i < kColCount; ++i) {
        int w = ColW(i, nameW);
        if (i == 0) w += kIndentPerLevel;
        if (x >= cx && x < cx + w) return i;
        cx += w;
    }
    // 超出所有列（右侧留白）：**不归任何列**，返回 -1。
    //
    // 【行为修正：名称列改成固定宽度之后，右侧真的出现了留白】
    // 以前名称列是弹性的，`cx` 恰好等于 rcColHeader_.right，右侧不存在留白，
    // 这段兜底永远不会命中 —— 于是"落到最后一个可见列"看着无害。
    // 现在名称列固定 234px（池表值），默认 4 列时列总宽 = 234+120+73+73 = 500，
    // 窗口 1084 宽时右侧有 584px 纯白（原版 1251 宽时是 730px）。如果还沿用
    // "落到最后一列"，用户在**原版里根本不是列的大片空白处**点一下，
    // 就会把内存列排序 —— 那是新造的假行为（本项目最忌讳的一类）。
    // 原版在留白处点击是 no-op，所以这里返回 -1。
    return -1;
}

// ============ 绘制：GDI 对象缓存 ============
//
// 【性能修复 · 每帧 CreateSolidBrush】
// 改前本文件的绘制路径每次填充都走 `CreateSolidBrush` + `FillRect` +
// `DeleteObject`（即 `TmWindow::FillSolid`）。一次进程页重绘的填充次数：
//   选中/悬停底色      1
//   热力图数值列      5 × 可见行数
//   组头底色          组头数
//   展开三角 / 滚动条箭头  每个都 1 次 Create + 1 次 Delete
// 600 进程 / 一屏 20 行时 ≈ 110+ 次 CreateSolidBrush + 110+ 次 DeleteObject。
// 这两条都是 USER32→GDI32 的句柄表操作（内部还要拿 GDI 临界区），
// 单次 1~3µs，合计 0.3~0.7ms/帧 —— 对一台「原版任务管理器一开就卡」的机器
// 属于纯浪费：用到的颜色总共只有二十来种。
//
// 改法：按 COLORREF 缓存画刷，进程生命周期内复用。
//   - 颜色集合来自固定调色板（HeatColor 的 9 档 + 选中/悬停/组头/三角/箭头），
//     缓存条目数有限（≤ 32），不存在无限增长。
//   - 只在 UI 线程访问，不需要加锁。
//   - `BrushCache` 的析构函数负责 DeleteObject，不留下进程级泄漏。
namespace {

struct BrushCache {
    std::unordered_map<COLORREF, HBRUSH> m;
    ~BrushCache() {
        for (auto& kv : m) {
            if (kv.second) DeleteObject(kv.second);
        }
        m.clear();
    }
};

// 取（必要时创建）某个颜色的实心画刷。返回值永不为 nullptr（除非 GDI 耗尽）。
HBRUSH CachedBrush(COLORREF c) {
    static BrushCache cache;
    auto it = cache.m.find(c);
    if (it != cache.m.end()) return it->second;
    HBRUSH b = CreateSolidBrush(c);
    if (b) cache.m.emplace(c, b);
    return b;
}

// `TmWindow::FillSolid` 的缓存版。语义完全相同（实心填充 + 无边框），
// 只是把每帧的画刷创建/销毁摊成一次性。
inline void FillSolidFast(HDC hdc, const RECT& rc, COLORREF c) {
    HBRUSH b = CachedBrush(c);
    if (!b) return;
    FillRect(hdc, &rc, b);
}

// 实心三角（展开箭头 / 排序箭头 / 滚动条箭头）的缓存版。
// 与原实现一致：NULL_PEN 描边 + 实心画刷填充，退出前恢复旧对象。
inline void FillPolygonFast(HDC hdc, const POINT* pts, int n, COLORREF c) {
    HBRUSH b = CachedBrush(c);
    if (!b) return;
    HGDIOBJ oldBrush = SelectObject(hdc, b);
    HGDIOBJ oldPen   = SelectObject(hdc, GetStockObject(NULL_PEN));
    Polygon(hdc, pts, n);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
}

// 【性能修复 · 每帧 CreatePen】
// `TmWindow::DrawHLine` / `DrawVLine`（在 tmwindow.cpp）每次调用都
// CreatePen + SelectObject + MoveToEx/LineTo + 恢复 + DeleteObject。
// 列头一帧要画 1 条横线 + 6 条列间竖线 = 7 次 Create/Delete。
// 线条颜色只有两种（kLineHeader / #E5E5E5），同样没有理由每帧重建。
// 这两个是**本文件内**的替换实现，语义与 TmWindow 版本完全一致。
// （tmwindow.cpp 的版本保留不动 —— 那个文件不归本轮改，且其它页面在用它。）
struct PenCache {
    std::unordered_map<COLORREF, HPEN> m;
    ~PenCache() {
        for (auto& kv : m) {
            if (kv.second) DeleteObject(kv.second);
        }
        m.clear();
    }
};

HPEN CachedPen(COLORREF c) {
    static PenCache cache;
    auto it = cache.m.find(c);
    if (it != cache.m.end()) return it->second;
    HPEN p = CreatePen(PS_SOLID, 1, c);
    if (p) cache.m.emplace(c, p);
    return p;
}

inline void DrawHLineFast(HDC hdc, int x1, int x2, int y, COLORREF c) {
    HPEN p = CachedPen(c);
    if (!p) return;
    HGDIOBJ old = SelectObject(hdc, p);
    MoveToEx(hdc, x1, y, nullptr);
    LineTo(hdc, x2, y);
    SelectObject(hdc, old);
}

inline void DrawVLineFast(HDC hdc, int x, int y1, int y2, COLORREF c) {
    HPEN p = CachedPen(c);
    if (!p) return;
    HGDIOBJ old = SelectObject(hdc, p);
    MoveToEx(hdc, x, y1, nullptr);
    LineTo(hdc, x, y2);
    SelectObject(hdc, old);
}

} // namespace

// ============ 绘制：列头 ============

void TmWindow::PaintColumnHeader(HDC hdc) {
    g_liveWidths = colWidths_;
    g_liveVisible = colVisible_;
    FillSolidFast(hdc, rcColHeader_, kBgHeader);
    DrawHLineFast(hdc, rcColHeader_.left, rcColHeader_.right,
                  rcColHeader_.bottom - 1, kLineHeader);

    HFONT old = static_cast<HFONT>(SelectObject(hdc, fontBold_));
    SetBkMode(hdc, TRANSPARENT);
    SetTextAlign(hdc, TA_LEFT | TA_TOP);

    const int x0 = rcColHeader_.left + kIndentPerLevel;   // 给展开箭头留位
    const int nameW = NameColWidth(rcColHeader_.right - rcColHeader_.left - kIndentPerLevel);
    // 列头「名称」的左距 = 列左界 + kCellPadLeft(8)。
    //
    // 【依据：主题 TMT_SIZINGMARGINS】`GetThemeMargins(HEADER, HEADERITEM,
    // HIS_NORMAL, TMT_SIZINGMARGINS)` 实测 **L2 R3 T9 B14**（系统API实测，
    // 出处 docs/specs/T_uxspec.md §1.3）。L2 是「列头单元格左界到内容区」
    // 的可拉伸带，而 kCellPadLeft=8 是单元格的内容缩进 —— 两者量级一致
    // （2px 边框带 + 6px 视觉留白）。
    // 主题的 CONTENTMARGINS 是 L0 R0 T0 B0，即主题本身不提供额外文字内缩，
    // 所以文字缩进必须由应用给。
    //
    // 【曾经的错误】这里一度写成 `x0 + 24 + kIconSize + 6`（= rel 64），
    // 意图是「对齐行内文字」。那个锚点来自一张来源未记录的裁切图，而且注释
    // 自称算出 46、代码实际是 64，注释与实现早就脱节。
    // 列头文字与行内图标/文字**不对齐**是正常的：列头是单元格语义，
    // 行内是「沟槽+图标+文字」语义，两者左界本来就不同。
    const int nameLeft = rcColHeader_.left + kCellPadLeft;

    // 【排序列高亮：已删除 · 2026-11-21】
    //
    // 这里曾把排序列整列涂成 #E5F3FF，理由是「hdr_orig_arrow.png 里排序列有
    // 浅蓝底 = 当前排序列的视觉提示」。那个归因是错的：
    //   1. 按 S_duispec.md §2.4，`#E5F3FF` 是 `dtb(EXPLORER::LISTVIEW,1,2)` 的
    //      **鼠标悬停色**（4800/4800 像素全同色）；选中色是 `#CDE8FF`。
    //   2. 真原版 `docs/reference/original_win10.png` 的名称列明知已按名称升序
    //      排序（箭头像素在 x=112..119 / y=94..97），整列却是**纯白**：
    //      逐像素统计 x=110..230 / y=90..130 得 #FFFFFF 4930 像素，无 #E5F3FF。
    //   3. 那张 hdr_orig_arrow.png 的浅蓝，最可能是截图时鼠标正停在 CPU 列头上。
    //
    // 之前一直留着，理由是「有可见效果，用户能看出按哪列排」。但排序箭头
    // 本身就是那个提示，这块底色是重复的，而且**让箭头糊在一块色块里**——
    // 用户报「箭头分辨率太低」，一部分观感就来自这里：镂空的 1px 描边
    // 贴在浅蓝底上，对比度被削弱，看起来像一团。
    //
    // 现在删掉，只留箭头。视觉上一个 4 行的镂空 V 形在纯白列头上，与原版一致。

    // ===== 列头带上半部分的资源汇总数值（真原版实测）=====
    //
    // 【为什么列头有 44px】原版列头带 y=92..135（44px）里装的是**两行**：
    //     上半 y=95..106  CPU 列显示总体占用率（如「4%」）、内存列显示内存占用率（如「91%」）
    //     下半 y=117..127 列名（名称/状态/CPU/内存）
    // 这正是列名文字看起来"在带内偏下 8.5px"的原因 —— 不是排版失误，上面还有一行。
    // 出处：docs/reference/original_win10.png 逐像素扫描（y=95..106 有 183 个暗像素，
    //       字形渲染出来是 "4%" 和 "91%"，纯黑 #000000，字高 12px）。
    //
    // 【哪些列有】只有 CPU 和内存两列有。名称/状态/磁盘/网络/GPU 上方都是空白
    // （原版那几列 y=95..106 的暗像素为 0）。
    //
    // 【字号】原版数值字形高 12px（y=95..106），比列名（11px，y=117..127）大 1px。
    // 用 fontGroup_（13px 半粗）近似 —— 本机无法逐像素校准字体渲染，差 1px 可接受。
    double sumCpuPct = 0.0;
    double sumMemPct = 0.0;
    if (snap_) {
        // 【CPU 总体利用率：优先用系统真值】
        // PerfSnapshot::cpuTotalPercent 是逐核 (1 - idle差分/总差分) 取平均得到的
        // 系统级利用率（src/perfdata.cpp:498），这才是"CPU 使用率"的定义值。
        //
        // 【为什么不再用"排除 PID 0 后求和"】那样虽然也能得到接近的数，但：
        //   1. 它依赖每个进程的差分都不丢失（新进程首轮没有基准，会被算成 0）
        //   2. 内核时间、DPC/中断时间不属于任何进程，会漏掉
        //   3. 进程在采集间隙退出，它的 CPU 时间就丢了
        // 系统真值不受这些影响。有 perfSnap_ 就直接用。
        //
        // 【PID 0 那个坑仍然要记住】此前我加了"求和排除 pid 0"来修 94% 的 bug。
        // 根因是 PID 0 是 System Idle Process，它的 CPU 时间 = **系统空闲**时间，
        // 一个人就占 90%+，加进总和后汇总恒为 90~100%。
        // 那个修法是对的（现在保留为兜底），但用系统真值更准。
        if (perfSnap_) {
            sumCpuPct = perfSnap_->cpuTotalPercent;
        } else {
            for (const auto& e : snap_->processes) {
                if (e.pid == 0) continue;    // 兜底：跳过空闲进程
                sumCpuPct += e.cpuPercent;
            }
            if (sumCpuPct > 100.0) sumCpuPct = 100.0;
        }
        if (snap_->totalPhysical > 0) {
            const uint64_t used = snap_->totalPhysical - snap_->availablePhysical;
            sumMemPct = static_cast<double>(used) /
                        static_cast<double>(snap_->totalPhysical) * 100.0;
        }
    }

    for (int i = 0; i < kColCount; ++i) {
        // 【列可见性】隐藏列不画列头、不画分隔线、不占宽度。
        // 用 ColX/ColW（都走 VisW）保持与 HitTestColumn 完全一致。
        if (!LiveVisible(i)) continue;
        int cx = x0 + ColX(i, nameW);
        int colLeft  = (i == 0) ? x0 : cx;
        int colRight = (i == 0) ? (x0 + nameW) : (cx + LiveW(i));

        // 列头底色。
        //
        // 【悬停色 #D9EBF9 = rgb(217,235,249)】
        // 【系统API实测】`DrawThemeBackground(HEADER/ItemsView::Header, HP_HEADERITEM,
        // HIS_HOT)` 离屏渲染取样全格纯色（出处 docs/evidence/exec/U_render4_console.txt
        // 「HEADER class part 1」段：`p=1 s=2 #D9EBF9x6160`）。
        // 这是**列头悬停**色，不是「排序列底色」。
        // 顺带核到按下态 HIS_PRESSED = #BCDCF4（`p=1 s=3`），仍不采用，理由见下。
        //
        // 【排序列】HIS_SORTEDNORMAL 渲染结果与 HIS_NORMAL 完全相同
        // （都是 `#FFFFFFx6132 #E5E5E5x28`）→ 排序列**没有底色**，只有右侧
        // 那条 1px #E5E5E5 分隔线 + 箭头。
        //
        // 【已删：按下态 #CCE4F7】
        // 那个色在主题里不存在（主题按下态是 #BCDCF4），是自创的；而且行为上有害：
        // pressedCol_ 在 WM_LBUTTONDOWN 置位、WM_LBUTTONUP 清除，点击同时立刻
        // 排序 → 正常点一次列头 = **整列闪一下深蓝**。排序反馈应当只有
        // 「箭头出现/翻转」，不整格变色。
        //
        // 按下时鼠标必然停在该列上，`hoverCol_` 本来就等于它，所以
        // 只留悬停分支即可，不需要再单独判断 `pressedCol_`。
        if (i == hoverCol_) {
            // 名称列的悬停块从 `rcColHeader_.left` 起，不是从 x0（= left+18）起。
            // 原先 `colLeft = (i == 0) ? x0 : cx` 让名称列左侧 x=0..17 这 18px
            // 不着色，高亮块左边留一道肉眼可见的缺口。列头单元格是从列表左界
            // 开始的**整块矩形**（HEADERITEM 的渲染是一整格纯色，见上）。
            int hlLeft = (i == 0) ? rcColHeader_.left : colLeft;
            RECT hr{ hlLeft, rcColHeader_.top, colRight, rcColHeader_.bottom - 1 };
            FillSolidFast(hdc, hr, RGB(0xD9, 0xEB, 0xF9));
        }

        int left  = (i == 0) ? nameLeft : (cx + kCellPadLeft);
        int right = (i == 0) ? (x0 + nameW - kCellPadRight)
                             : (colRight - kCellPadRight);
        // 【列头文字垂直位置 —— 真原版实测偏下 8.5px，不是垂直居中】
        // 原版 `docs/reference/original_win10.png`：列头带 y=92..135（44px，带顶 rel 0）。
        // 逐列扫描墨迹（docs/evidence/_hh_scan5.ps1）得到两组内容：
        //   上半 rel 3..14  —— CPU `4%`(x=400..422)、内存 `91%`(x=464..495)
        //   下半 rel 25..35 —— 名称 x=9..33、状态 x=243..266、CPU x=399..422、
        //                      内存 x=472..495（四列同一行带）
        // ⇒ 列头带实际是**两行各 22px**：上半 = 资源总占用率（交接单第 1 条），
        //   下半 = 列名。列名墨迹中心 rel 30，正对下半行（rel 22..43）的中心 32.5 附近。
        //
        // 【偏移量 8 是怎么定出来的 —— 不是猜字体度量，是用我方自己的截图校准】
        // 我方旧截图 `showcase/f1_processes.png` 实测（docs/evidence/_hh_cal.ps1）：
        //   列头带 y=124..157（底线在 157），旧代码用**整条带**做 `DT_VCENTER`，
        //   名称列墨迹落在 y=140..150，中心 145；带中心 = (124+157)/2 = 140.5。
        //   ⇒ 同一份字形在居中绘制时，**墨迹中心天然比矩形中心低 4.5px**
        //     （tmInternalLeading=5 在上、descent=4 在下，上下不对称造成的常量偏移）。
        // 于是要让墨迹中心落在 rel 30（原版实测 122 − 92），设矩形顶为 t：
        //   矩形高 = 43 − t（底边 rel 43 = 带底−1）
        //   (t + 43) / 2 + 4.5 = 30   →   t = 8
        // **取 8**，与实测逐像素吻合。
        //
        // 【为什么不整体平移矩形】上下各加 8 会让矩形越出列头带，
        // 在无裁剪的 memDC 里把半行字画到第一行数据上；把矩形从下方收窄
        // 反而会把文字顶上去。必须收**上边**、留住下边（bottom-1 就是那条底线）。
        //
        // ⚠ 本机无法编译/渲染，8 是「原版实测 + 我方自身截图校准」的结果；
        //   实际渲染若偏 1~2px，只调这一个数，不要动 44。
        // 【列名 y 位置：有汇总数值的列要下移】
        // 真原版实测（docs/reference/hdr_orig_arrow.png，列头带 y=83..140）：
        //   数值墨迹 y=100..111  → 相对列头顶(top) = 4..15
        //   列名墨迹 y=123..131  → 相对列头顶(top) = 27..35
        //   **中间空 16..26，共 11px**
        //
        // 【RECT ≠ 墨迹】DT_VCENTER 垂直居中，RECT 要按"居中后墨迹落位"反推。
        //   列名 RECT = top+20 .. bottom-1（高 23），居中后文字中心约 rel 31，
        //   墨迹落 rel 25..37，与实测 27..35 差 2px。
        //   若 RECT 从 top+8 起（旧值），墨迹会落到 rel 13..25 —— 和数值区(4..15)
        //   重叠在 13..15，这就是"两行文字叠在一起"。
        const int nameTop = (i == 2 || i == 3) ? theme::Scaled(20) : theme::Scaled(8);
        RECT cell{ left, rcColHeader_.top + nameTop, right,
                   rcColHeader_.bottom - 1 };
        UINT fmt = DT_SINGLELINE | DT_VCENTER | kCols[i].align;
        DrawTextClipped(hdc, kCols[i].title, &cell, fmt, kTextHeader);

        // 列头带上半部分的资源汇总数值 —— 只有 CPU(2) 和内存(3) 两列有。
        // 几何来自真原版实测：数值区 y=95..106（带顶 92 起算 = rel 3..14），
        //             列名区 y=117..127（rel 25..35）。
        // 两行不重叠，所以直接用两个独立矩形，不做垂直居中冲突处理。
        if (i == 2 || i == 3) {
            const double v = (i == 2) ? sumCpuPct : sumMemPct;
            wchar_t num[16];
            swprintf_s(num, L"%.0f%%", v);
            HFONT oldSum = static_cast<HFONT>(SelectObject(hdc, fontGroup_));
            RECT sumCell{ left, rcColHeader_.top + theme::Scaled(3),
                          right, rcColHeader_.top + theme::Scaled(17) };
            DrawTextClipped(hdc, num, &sumCell,
                            DT_SINGLELINE | DT_VCENTER | kCols[i].align,
                            kTextPrimary);
            SelectObject(hdc, oldSum);
        }

        // 列间竖线：1px #E5E5E5，**只在列头带**有。
        //
        // 【"数据区没有竖线"这句话只对状态列成立 —— 已订正】
        // 此前这里写着「数据区没有」，出处是主题 HEADERITEM 的渲染采样。
        // 真原版实测（`docs/reference/original_win10.png` y=200/300/600 三行采样）：
        //   x=236（名称↔状态）  #FFFFFF  ← 确实没有
        //   x=356/429/502       #EAD5A0  ← 有，而且是**热力单元格的 1px 描边**
        // 也就是说数据区确实有竖线，但它是热力框的一部分（`kLineHeatCell`），
        // 不是"列分隔线"。热力框由 PaintRows 画（名字列/状态列不染色 → 没有框），
        // 列头带这条仍是列头的分隔线，两者颜色不同、职责不同，不要合并。
        // 复扫脚本 `docs/evidence/_hh_gh.ps1`。
        //
        // 【列头带的这条竖线是**线性渐变**，不是纯色 —— 真原版实测】
        // 此前画纯色 #E5E5E5，依据是主题 `HEADERITEM` HIS_NORMAL 的离屏渲染
        // （`#FFFFFF x6132` + `#E5E5E5 x28`）。但真原版截图的实测是渐变：
        // x=236/356/429/502 四处在列头带内逐行取样（y=92..134）**完全一致**：
        //   y=92  #E8E8E8   ← 带顶最浅
        //   y=100 #DFDFDF
        //   y=113 #D1D1D1
        //   y=127 #C2C2C2
        //   y=134 #BBBBBB   ← 带底最深
        // 42 个步长里降 2 的有 3 处、降 1 的有 39 处 —— 是一条**向上取整**的线性渐变
        // （232 → 187，跨度 45，每行 45/42 ≈1.071）。复扫脚本 `docs/evidence/_hh_hdrgrad.ps1`。
        // 用 `ceil` 而不是四舍五入：按 ceil 公式生成 43 个值与原版**逐行完全一致**（0 处不符），
        // 四舍五入则有 18 处差 1。这个差别肉眼几乎看不出，但既然实测能对上就没理由不对上。
        //
        // 【又一次「标准控件主题值 ≠ taskmgr 实际值」】主题给的是单色 #E5E5E5，
        // 原版画的是渐变。与本轮 kProcHeaderHeight 24→44、kLineHeader
        // #828790→#A0A0A0 是同一类错误：taskmgr 用 DirectUI 自绘，
        // 它的列头不是 DrawThemeBackground(HEADERITEM) 的输出。
        //
        // 【实现注意】必须用 1px 矩形填，不能用 DrawVLineFast ——
        // GDI 的 `LineTo` **不含终点像素**，竖着画会少掉最后一行的颜色；
        // 用 DrawHLineFast 逐行画 1px 横线也会因同一原因变成 2px 宽。
        if (i > 0) {
            const int gradTop = rcColHeader_.top;
            const int gradBot = rcColHeader_.bottom - 2;      // 最后一行是底线(kLineHeader)，不参与渐变
            const int span    = gradBot - gradTop;            // = 44 − 2 = 42，与原版 42 个步长一致
            if (span > 0) {
                // v = 232 − ceil(45 · (y − gradTop) / span)
                // 整数实现：ceil(a/b) = (a + b − 1) / b
                int runStart = gradTop;
                int runV     = 232;
                for (int yy = gradTop; yy <= gradBot; ++yy) {
                    const int num = 45 * (yy - gradTop);
                    const int v   = 232 - (num + span - 1) / span;
                    if (v != runV) {
                        if (yy > runStart) {
                            RECT r{ colLeft, runStart, colLeft + 1, yy };
                            FillSolidFast(hdc, r, RGB(runV, runV, runV));
                        }
                        runStart = yy;
                        runV     = v;
                    }
                }
                RECT r{ colLeft, runStart, colLeft + 1, gradBot + 1 };
                FillSolidFast(hdc, r, RGB(runV, runV, runV));
            }
        }
    }

    // ===== 排序箭头（真原版有，我之前误删了）=====
    //
    // 【我犯过的错，记下来】我曾断言"原版列头没有排序箭头"，依据是扫描
    // x=380..520 找 13px 宽的实心块，结果一个都没找到。**那个扫描方法是错的**：
    // 原版箭头只有 **跨度 7px（x=369..375）、高 4px**，而且是**镂空的 V 形轮廓**
    // （不是实心三角），所以"找 13px 实心块"必然落空。用户指出来之后重新逐像素渲染才发现。
    //
    // 【真原版实测】docs/reference/hdr_orig_arrow.png（逐像素）：
    //   x=369..375（跨度 7）、y=102..105（高 4），**空心轮廓**：
    //   y=102  x=369,375       #.....#      row 0: 两个端点
    //   y=103  x=370,374       #...#        row 1: 两个端点
    //   y=104  x=371,372,373  ###          row 2: 三连（尖端，两斜线在此交汇）
    //   y=105  x=372          #            row 3: 单点（最尖）
    //   相对坐标（箭头左端 x=369 为 0，y=102 为 0）：
    //   row 0: 0, 6 / row 1: 1, 5 / row 2: 2, 3, 4 / row 3: 3
    //   颜色 #4C607A（深蓝灰，不是 #5A5A5A）
    //   位置：在**列名文字的左侧**（CPU 文字 x=400，箭头 x=369）
    //   列头带上还有一个 #E5F3FF 的浅蓝底，x=360..432 —— 那是**排序列的整列底色**
    //
    // 【几何换算】列头带顶 y=83（原版坐标），箭头 y=102..105 → rel 19..22。
    //   箭头中心 rel ≈ 20.5，列头带高 44，所以箭头在带的**上半部分**（对准汇总数值行）。
    //   这与我之前"箭头中心 rel 22 压在列名上"的判断不同 —— 压在列名上是因为
    //   我方列名的 y 位置本身算错了（旧值 top+8），不是箭头的问题。
    if (sortCol_ >= 0 && sortCol_ < kColCount && LiveVisible(sortCol_)) {
        const int cx = x0 + ColX(sortCol_, nameW);
        // 【位置：列左界 + 8，列头顶 + 2】
        // 原版实测（我重新逐像素复核 docs/reference/original_win10.png）：
        //   列头带 = y=92..135（y=135 是 #A0A0A0 底线，x=236 处可见 43 步渐变）
        //   名称列箭头 = x=111..119 / **y=94..97**
        //   → 相对列左上角 = (+8, **+2**)；列左界 x=103（=111-8）
        // 【上一版错在哪】旧值 +19 来自 "列头顶 y=83" —— 那个 83 不是列头顶，
        //   而是页签下方空白区里的某个位置。按 y=83 + 19 = 102 反推出的箭头
        //   y=102..105 与实测的 y=94..97 差 8px。红队 ZZ_redteam.md 指出此点，成立。
        //   我方原实现列头带 y=45..88、箭头 y=64..67（也是 +19），
        //   改成 +2 后箭头落到 y=47..50，与原版的 +2 一致。
        const int ax = cx + theme::Scaled(8);
        const int ay = rcColHeader_.top + theme::Scaled(2);

        // 【排序箭头 · 镂空 V 形轮廓】
        //
        // 原版实测（docs/reference/hdr_orig_arrow.png，逐像素）：
        //   y=102  x=369,375      #.....#
        //   y=103  x=370,374       #...#
        //   y=104  x=371,372,373  ###
        //   y=105  x=372          #
        // 是**轮廓**不是实心 —— 中间是空的，只有两条斜线。
        //
        // 之前画成实心（每行整条 LineTo 填充），放大 12 倍看是个倒梯形色块
        // （第 0 行 9 像素全填充、第 1 行 7、第 2 行 5、第 3 行 3），不是箭头。
        // 用户指出「分辨率太低」，实际是形状错了。
        //
        // 【DPI】图案只有 4 行高，行间距和半宽都**不能**走 Scaled() ——
        // 那样高 DPI 下会变成 8 行、形状失真。箭头是手工像素图案，
        // 只在锚点（ax/ay）上缩放，图案本身固定 4 行。
        // 这与 ScrollBar 的小箭头同理：系统控件在 200% DPI 下也是 1px 线。
        const COLORREF arrowColor = RGB(0x4C, 0x60, 0x7A);
        const int arrowW = 6;   // 最宽处跨度 = 6（x=0..6 共 7 个像素位）
        for (int row = 0; row < 4; ++row) {
            const int y = sortDesc_ ? (ay + row) : (ay + 3 - row);
            const int lx = ax + row;            // 左斜线向右下走
            const int rx = ax + (arrowW - row); // 右斜线向左下走
            SetPixelV(hdc, lx, y, arrowColor);
            if (rx != lx) SetPixelV(hdc, rx, y, arrowColor);
            // 尖端那一行（row=2）原版有中间像素，是两条斜线的交汇
            if (row == 2) SetPixelV(hdc, ax + 3, y, arrowColor);
        }
    }

    SelectObject(hdc, old);
}

// ============ 绘制：进程列表 ============

void TmWindow::PaintRows(HDC hdc) {
    g_liveWidths = colWidths_;
    g_liveVisible = colVisible_;
    if (!snap_) return;

    // 【P0 修复 · byPid_ 为空导致整页空白】
    // RebuildRows 只在「应用行已展开」的分支里才会惰性建这张表
    // （tmwindow.cpp 的两处 `if (byPid_.empty()) { ...填充... }` 都在
    //  `if (r.expanded)` / `appendKids` 内部）。而默认视图是**全部折叠**的
    // （appExpanded_ 默认为空，groupExpanded_ 默认 true），于是 byPid_ 一直是空的 ——
    // 下面每一行都 `find` 不到、走 `if (!pe) continue`，
    // **结果是一行都不画**（列头和组头在，下面全白）。
    //
    // 修法：在本函数开头保证索引存在。RebuildRows 每次换快照都会 clear()，
    // 所以「空」是唯一需要补建的状态；补建是 O(n) 一次，摊到整个快照周期，
    // 比原来「每行 O(n) 线性扫」快 20 倍以上，也比「完全不画」正确。
    if (byPid_.empty() && !snap_->processes.empty()) {
        byPid_.reserve(snap_->processes.size() * 2);
        for (const auto& e : snap_->processes) byPid_[e.pid] = &e;
    }

    const int x0 = rcList_.left + kIndentPerLevel;
    const int nameW = NameColWidth(rcList_.right - rcList_.left - kIndentPerLevel);
    const int bodyTop = rcList_.top;
    const int bodyBot = rcList_.bottom;

    // 【性能修复 · 列左界每帧预算一次】
    // 原来每个数值单元格都调一次 `ColX(col, nameW)`，而 ColX 本身是
    // O(col) 的累加循环。一行 7 列 × 一屏 20 行 = 140 次调用、约 500 次加法，
    // 每个 WM_PAINT 都重算一遍。列宽在一次绘制内不变，提到循环外即可。
    int colX[kColCount + 1];
    colX[0] = 0;
    for (int c = 1; c <= kColCount; ++c) colX[c] = colX[c - 1] + ColW(c - 1, nameW);

    // 【行底的右界 —— 真原版实测：只铺到最后一列右界，不铺满客户区】
    // 原版 `docs/reference/original_win10.png`：
    //   选中行（y=398..425，#CDE8FF）在 x=503 起就是纯白 —— 底色只到 x=502；
    //   组头行（y=136..173）右侧同样从 x=503 起纯白；
    //   740 行数据区的普通行整行本来就是白的，看不出差别。
    // 复扫脚本 `docs/evidence/_hh_rowfill.ps1`。
    // 也就是说原版的行底色范围 = 列总宽（502），不等于窗口宽度（1251）。
    //
    // 【为什么这行代码是必须的】名称列改成固定宽度之前，`rcList_.right` 恰好
    // 等于列总宽（名称列弹性吃满剩余），铺满行 == 铺到列右界，两者无法区分。
    // 改成固定列宽后，右侧出现大片留白，若不收右界就会看到
    // 「选中行的蓝底一直拉到窗口最右边」，而原版是蓝色止于内存列右界 —— 一个
    // 一眼就能看出的假象。这里把行矩形的右界钳到最后一个可见列的右缘。
    // 【算术核对（默认 4 列）】x0 = left + 6（沟槽），ColW(0) = nameW = 228
    //   （= 234 − 6），其余三列 120+73+73 = 266。
    //   colsRight = left + 6 + 228 + 266 = left + 500
    //   = 名称列(0..234) + 三列(234..500) ✓ 与原版 x=502 起的留白口径同构
    //   （原版 4 列最后一列右界 502 = 236+120+73+73，含 2px 边框）。
    //   7 列全开时固定列 120+73*5 = 485，colsRight = left + 719。
    //   注意**不要**在这里再加 kIndentPerLevel：沟槽已经含在 x0 里了。
    //   （本文件口径：名称列总宽 = kIndentPerLevel + ColW(0)。）
    int colsRight = x0;                       // 与 PaintColumnHeader/HitTestColumn 同起点
    for (int c = 0; c < kColCount; ++c) colsRight += ColW(c, nameW);
    if (colsRight < rcList_.left) colsRight = rcList_.left;
    const int rowRight = (std::min)(colsRight, static_cast<int>(rcList_.right));

    // 找行索引范围
    int y = bodyTop - scrollY_;
    int firstVisible = -1, lastVisible = -1;

    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, fontBody_));
    SetBkMode(hdc, TRANSPARENT);

    wchar_t buf[64];

    // 「资源值 ▸ 磁盘/网络 ▸ 百分比」需要的分母 = 全系统总吞吐。
    // 只在那两组被选中百分比模式时才扫一遍快照，平时零开销。
    double sysDiskTotal = 0.0, sysNetTotal = 0.0;
    {
        const int rvm = MenuBuilder::ResourceValueMode();
        const bool wantDisk = ((rvm / 2) == 1 && (rvm % 2) == 0);
        const bool wantNet  = ((rvm / 2) == 2 && (rvm % 2) == 0);
        if ((wantDisk || wantNet) && snap_) {
            if (wantDisk) {
                for (const auto& e : snap_->processes) sysDiskTotal += e.diskBytesPerSec;
            }
            if (wantNet) {
                for (const auto& e : snap_->processes) sysNetTotal += e.netBitsPerSec;
            }
        }
    }

    for (size_t i = 0; i < rows_.size(); ++i) {
        const ViewRow& row = rows_[i];
        // 行高统一走 theme::RowHeightFor()，本文件**不再有**任何行高字面量，
        // 也不再手写 `(kind == GroupHeaderRow) ? ... : ...` 三元式。
        // 【子行 22】组头/父行 28，子进程行（indentLevel > 0）22 —— UIFILE
        // 里父行 28rp、子行 22rp 是两个不同的 item 类型（30001 @0x147c），
        // 不是"同一种行多缩进一级"。
        //
        // 【旧依据已作废】此前这里写着「行高没有规范值，唯一可用的推导式是
        // 行高 = tmHeight + 2×pad，pad 取 5.5 得 28」——那是错的。行高不是
        // 推导出来的，是 UIFILE 声明的（28rp / 22rp）；本机 96 DPI 下
        // 1rp = 1px，所以就是 28 / 22 px。不要再把人引回推导。
        //
        // 【行间不画分隔线】`LVS_EX_GRIDLINES` 默认关闭，主题也不提供行分隔线
        // part（T_uxspec.md §1.4）→ 本函数不画任何横线，靠留白分隔。
        //
        // 下面所有 `y + rh`、`(rh - kIconSize) / 2`、`(rh - boxH) / 2`、
        // 单元格/热力图矩形都用这个 `rh`，所以子行会自动整体变矮，不必另改。
        const int rh = RowHeightFor(row.kind == ViewRow::GroupHeaderRow, row.indentLevel);

        if (y + rh <= bodyTop) { y += rh; continue; }
        if (y >= bodyBot) break;
        if (firstVisible < 0) firstVisible = static_cast<int>(i);
        lastVisible = static_cast<int>(i);

        // 行矩形的右界用 rowRight（= 最后一个可见列右缘，见上方实测说明），
        // 不用 rcList_.right。原版选中行/组头行的底色止于内存列右界 x=502。
        RECT rowRect{ rcList_.left, y, rowRight, y + rh };
        RECT clip = rowRect;
        clip.top = (std::max)(static_cast<int>(clip.top), bodyTop);
        clip.bottom = (std::min)(static_cast<int>(clip.bottom), bodyBot);

        if (row.kind == ViewRow::GroupHeaderRow) {
            // ---- 分组标题行 ----
            // 【底】名称列与右侧空白区 = 纯白 #FFFFFF。
            // 依据：真原版 `docs/reference/original_win10.png` 的组头行
            // y=136..173，x=3..355 **非白像素 0 个**；第二个组头
            // 「后台进程 (105)」y=482..519 的 x=3..12 同样 0 个。
            // 复扫脚本 `docs/evidence/_hh_grp2.ps1`。
            // theme.h 的 kBgGroupHeader 已由 Agent II 从 #F2F2F2 改为 #FFFFFF
            // （见 II_colors.md §2），这里只引用、不重复定义。
            FillSolidFast(hdc, clip, kBgGroupHeader);

            // ---- 值列热力色 + 1px 描边（Agent II 交接 H1-b）----
            // 【实测：不是聚合值，是固定哨兵色】
            // 两个组头的 CPU 列、内存列都恰为 #FFF9E4 —— 那是 HeatColor() 的
            // 「无数据」哨兵档（kFill[0]，thr[0]=255.0），**不是** 0% 对应的
            // #FFF4C4（kFill[1]）。两条反证：
            //   1) 聚合 CPU 即使为 0%，HeatColor(0) 也命中 #FFF4C4，差一档；
            //   2) 11 个进程的内存之和绝不可能是 0%，组头却是最浅档。
            // → 原版把组头行当「没有占比的非进程行」，填固定色。
            // 依据：docs/specs/II_colors.md §1.2（本人已独立复扫确认）。
            {
                const COLORREF ghHeat = GroupHeaderHeatColor();   // = HeatColor(255.0)
                const COLORREF ghLine = kLineHeatCell;            // #EAD5A0 1px 框
                // 只给数值列（2=CPU 3=内存 4=磁盘 5=网络 6=GPU）上色。
                // 状态列(1)不染色 —— 原版 x=236（名称↔状态）没有 #EAD5A0 竖线，
                // 证明它不在热力框内。
                //
                // 【几何：铺满整格 + 左右各 1px 描边，不扣 kCellPadRight】
                // 实测（列宽按原版 73 定案后逐像素对上）：
                //   CPU  格 [356,429)：356=#EAD5A0，357..428=底色(72px)，429=#EAD5A0
                //   内存 格 [429,502)：429=#EAD5A0，430..501=底色(72px)，502=#EAD5A0
                // 即 填色区 = [cx+1, cx+cw-1)，描边在 cx 与 cx+cw-1。
                // 复扫脚本 `docs/evidence/_hh_gh.ps1`。
                for (int c = 2; c < kColCount; ++c) {
                    if (!LiveVisible(c)) continue;
                    const int cx  = x0 + colX[c];
                    const int cw  = LiveW(c);
                    if (cw < 3) continue;
                    const int cRight = cx + cw - 1;          // 右侧描边那一列
                    RECT cr{ cx + 1, clip.top, cRight, clip.bottom };
                    FillSolidFast(hdc, cr, ghHeat);
                    DrawVLineFast(hdc, cx,     clip.top, clip.bottom - 1, ghLine);
                    DrawVLineFast(hdc, cRight, clip.top, clip.bottom - 1, ghLine);
                }
            }

            // 展开/折叠三角。
            // 【几何：无依据，沿用旧值】相对 x0 左移 12，宽 8、高 5~8。
            // 【颜色：无依据，沿用旧值】RGB(90,90,90) = #5A5A5A。
            // 组头的折叠指示器主题层是位图（LVP_GROUPHEADER TS_TRUE = 1x1，
            // 即主题不携带尺寸信息），无法从主题还原。这两个值既没有 API 依据
            // 也没有规范依据，标为沿用旧值；与树展开箭头（15x15 实心三角）
            // 保持实心三角这一共同形态。
            //
            // 【已知与原版的差异 · 未改】原版组头 **没有** 这个三角：
            // 实测 x=3..12 在 y=136..173 / y=482..519 两段都是 0 个非白像素，
            // 文字从 x=13 开始。删掉它会让「组头可折叠」这个可供性消失
            // （原版靠双击展开），属于交互改动而非纯对齐，故保留并记入报告。
            int triX = x0 - 12;
            int triY = y + rh / 2;
            POINT tri[3];
            if (row.expanded) {
                tri[0] = POINT{ triX - 4, triY - 2 };
                tri[1] = POINT{ triX + 4, triY - 2 };
                tri[2] = POINT{ triX,     triY + 3 };
            } else {
                tri[0] = POINT{ triX - 2, triY - 4 };
                tri[1] = POINT{ triX + 3, triY };
                tri[2] = POINT{ triX - 2, triY + 4 };
            }
            // 【性能修复】原为每帧 CreateSolidBrush(tri) + 两次 SelectObject +
            // Polygon + 两次恢复 + DeleteObject。组头只有 3 行，
            // 但颜色恒为 RGB(90,90,90)，没有理由每帧重建。
            FillPolygonFast(hdc, tri, 3, RGB(90, 90, 90));

            // 组名 + 数量
            SelectObject(hdc, fontGroup_);
            wchar_t gtext[128];
            swprintf_s(gtext, L"%s (%d)", GroupTitle(row.group), row.groupCount);
            RECT tr{ x0, y, rcList_.right - 8, y + rh };
            // 【文字色 = 蓝色 #1F59C3，不是正文黑】（Agent II 交接 H1-c）
            // 实测：组头文字框 y=150..165 x=13..76 的直方图里 #1F59C3 出现
            // 61 次（最高频），最暗像素就是它自己（没有任何更暗的像素）
            // → 纯色蓝字，不是我方那种「黑字」。
            // U_colors_final.tsv:76 `TASKMANAGER 16 0 TEXT` 双源吻合。
            // 复扫脚本 `docs/evidence/_hh_grp2.ps1`（x=3..12 全白，文字从 x=13 起）。
            DrawTextClipped(hdc, gtext, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT,
                            kGroupHeaderText);
            SelectObject(hdc, fontBody_);

        } else {
            // ---- 进程行 ----
            //
            // 【P0 修复 · 查找必须命中】
            // 这里此前用 byPid_.find(row.pid)，但 byPid_ 在 tmwindow.cpp 里
            // 只在「应用行已展开」的分支才惰性填充；默认视图全部折叠 → 表是空的
            // → 每一行都 find 不到 → `continue` → **整页一行都不画**。
            // 已在函数开头补建索引（O(n) 一次）。这里再兜一层：
            // 万一仍查不到（理论上不该发生），用 `rows_` 的 pid 反查快照会退化成
            // O(n)/行，那是我们绝对要避免的；所以宁可不画也不做线性扫。
            const ProcessEntry* pe = nullptr;
            {
                auto it = byPid_.find(row.pid);
                if (it != byPid_.end()) pe = it->second;
            }
            if (!pe) { y += rh; continue; }

            bool sel = (static_cast<int>(i) == selectedRow_);
            bool hov = (static_cast<int>(i) == hoverRow_);

            // ---- 本行显示的数值 ----
            //
            // 【核心语义】应用根（有子进程）显示的是整个应用的**合计**，
            // 不是根进程自己的值。原版就是这样：Claude 折叠时显示 ~5.8GB，
            // 而根进程本身只有 477MB。
            //
            // 子进程行（indentLevel > 0）显示各自的值。
            // 这段必须在绘制和热力图计算**之前**，两边用同一套值。
            double shownCpu = pe->cpuPercent;
            uint64_t shownMem = pe->privateBytes ? pe->privateBytes : pe->workingSet;
            double shownDisk = pe->diskBytesPerSec;
            double shownNet = pe->netBitsPerSec;
            double shownGpu = pe->gpuPercent;

            if (row.indentLevel == 0 && row.hasChildren) {
                auto itAgg = appAgg_.find(pe->pid);
                if (itAgg != appAgg_.end()) {
                    shownCpu = itAgg->second.cpuPercent;
                    shownMem = itAgg->second.memBytes;
                    shownDisk = itAgg->second.diskBytesPerSec;
                    shownNet = itAgg->second.netBitsPerSec;
                    // GPU 用最大值（不能相加，见 AppAggregate 注释）
                    shownGpu = itAgg->second.gpuPercentMax;
                }
            }

            // ---- 热力图着色的依据 ----
            //
            // 【死代码清理 · _PLAN.md A-01】原先这里有一整块 `heatT` 计算：
            // 按 sortCol_ 选 maxCpu_/maxMem_/maxDisk_/maxNet_/maxGpu_ 归一化、
            // 再 sqrt 压缩，算出 `heatT`。**但 heatT 算完之后全文件零读取**
            // （grep 确认只有赋值点，没有任何消费者）—— 纯死代码。
            // 而它依赖的 max*_ 五个成员又迫使 RebuildRows() 每帧多做一次
            // 全进程遍历（见 tmwindow.cpp）。整块连根删除。
            //
            // 真正的着色在下面对每个数值列各算一次（HeatColor(hc.pct)），
            // 用的是「该列自己的绝对占比」，本来就不需要 max*_。

            if (sel) {
                // 选中行底色。
                // 【规范推荐做法：alpha 叠加，不是纯色】
                // `Explorer::ListView` 的 LISS_SELECTED 渲染值 #CDE8FF 是
                // **半透明叠加在白底上的结果**，反解得源色 #008AFF @ 19.6%
                // （T_uxspec.md §1.5 的双底解方程：白底 #CDE8FF、品红底反推）。
                // Win10 故意用半透明，这样叠在任何行底色上都成立。
                // 白底上 #008AFF @20% = #CDE8FF，与直接填纯色等价，
                // 但叠加式在将来出现斑马纹/图表单元底色时仍然正确。
                // 这里用等价的白底合成值（列表背景恒为 #FFFFFF，
                // 见 kBgWindow）以保持现有绘制路径简单。
                FillSolidFast(hdc, clip, RGB(0xCD, 0xE8, 0xFF));   // 【系统API实测】LISS_SELECTED
                // 【交接】theme.h 的 kBgNavSelected 现为 #A0CDF0（无依据）。
                // 等 V agent 把它改成 #CDE8FF 后，这里改引用 theme::kBgNavSelected。
            } else if (hov) {
                // 悬停行：#0089FF @10.2% over #FFFFFF = #E5F3FF
                // 【系统API实测】Explorer::ListView LISS_HOT 离屏渲染
                // （U_render2_console.txt：`Explorer::ListView(1,2) #E5F3FF x8000`）。
                // 【交接】theme.h 的 kRowHover 现为 #E5F3FB（蓝通道少 4 级，
                // 且注释自认「未证实」）。等 V agent 改成 #E5F3FF 后，
                // 这里改引用 theme::kRowHover。
                FillSolidFast(hdc, clip, RGB(0xE5, 0xF3, 0xFF));
            } else {
                // 【依据：taskmgr.exe .data 色表，非截图】
                // 不是给整行染色 —— 名称/状态列保持空白（露出窗口白底），
                // 只有**数值列**（CPU/内存/磁盘/网络/GPU）各自按自己的值染色。
                //
                // 色阶与阈值来自 taskmgr.exe 的 `.data RVA 0x1370F0`
                // （9 × 24 字节记录，+0 是 double 阈值、+9..+11 是 RGB）以及
                // 分桶函数 RVA 0x74B04 的反汇编，全部逐字节 dump 过：
                //   阈值 0 / 9 / 19 / 39 / 59 / 79 / 99 / 100 (+255.0 无数据哨兵)
                //   色阶 #FFF9E4 #FFF4C4 #F9ECA8 #FFE487 #FFD264
                //        #FFC63D #FCB816 #FCA71D #FC682A
                // 出处 docs/specs/G_asm.md（§「表本体」「分桶函数」两段）。
                // 这就是 theme.h 的 HeatColor() 的实现依据 —— 本文件只负责调用。
                struct { int col; double pct; } heatCols[] = {
                    // HeatColor 收的是**百分比 0..100**（不是归一化比值）。
                    // 色阶是按绝对占比分档的阶梯查表（无反汇编里的 pow/log）。
                    //
                    // CPU / GPU：本身就是百分比，直接传。
                    // 内存：量程是物理内存总量（原版口径，非列内最大值）。
                    // 全部用**已聚合的显示值**（shownCpu/shownMem/shownDisk/shownNet/shownGpu），
                    // 保证「单元格里的数字」和「这格的染色深浅」永远来自同一个值。
                    { 2, shownCpu },
                    { 3, memTotalBytes_ > 0
                         ? static_cast<double>(shownMem) /
                           static_cast<double>(memTotalBytes_) * 100.0 : 0.0 },
                    // 磁盘 / 网络：没有天然的"100%"量程，用固定满格参照
                    // （磁盘 100 MB/s、网络 100 Mbps）。【无依据，沿用旧值】
                    //
                    // 之前这里用的是 `pe->diskBytesPerSec` / `pe->netBitsPerSec`
                    // ——进程自身值，而文字用的是已聚合的 shownDisk/shownNet。
                    // 结果应用根行显示着 `8.6 MB/s`，热度却按 0 算，同一格自相矛盾。
                    { 4, shownDisk / (100.0 * 1048576.0) * 100.0 },
                    { 5, shownNet  / (100.0 * 1.0e6)    * 100.0 },
                    { 6, shownGpu },
                };
                for (const auto& hc : heatCols) {
                    // 0 值**仍然染色**：分桶函数是 `thr[i] >= value` 的阶梯查表，
                    // value=0 命中 idx=1（阈值 0）→ #FFF4C4，不是留白。
                    // 之前 `if (hc.pct < 0.001) continue;` 把 0 值留白，
                    // 数值列会变成断断续续的白块。已删除该判断。
                    //
                    // 【列可见性】隐藏列不染色 —— 否则会在「已被后面的列
                    // 左移填位」的位置上留下一条无主的彩色竖条。
                    if (!LiveVisible(hc.col)) continue;
                    //
                    // 【性能】FillSolidFast + HeatColor 的 9 档定值 → 画刷缓存
                    // 命中率 100%，本循环不再有任何 CreateSolidBrush。
                    int cx = x0 + colX[hc.col];
                    int cw = (hc.col == 0) ? nameW : LiveW(hc.col);
                    // 【热力块右界：铺满整格，不扣 kCellPadRight】（Agent II 交接 H2）
                    // 实测（列宽 73 定案后逐像素对上）：
                    //   CPU  格 [356,429)：356=#EAD5A0，357..428=#FFF4C4(72px)，429=#EAD5A0
                    //   内存 格 [429,502)：429=#EAD5A0，430..501=#F9ECA8(72px)，502=#EAD5A0
                    // 原版热力块**铺满列宽**，只是被左右各 1px 的 #EAD5A0 描边吃掉。
                    // 旧代码 `- kCellPadRight`(4) 让右界内缩 4px，与原版差 3px，
                    // 且相邻列的描边会错开 —— 一并去掉。
                    // 复扫脚本 `docs/evidence/_hh_gh.ps1`。
                    //
                    // 【文字与色块的关系不变】数值文字仍右对齐到
                    // colRight - kCellPadRight（= 色块右描边内 3px），与原版
                    // 「x=428 仍是底色、文字右界在 425」一致。
                    int chRight = x0 + colX[hc.col] + cw;   // 全宽，不扣 pad
                    if (chRight - cx >= 3) {
                        RECT cr{ cx + 1, clip.top, chRight - 1, clip.bottom };
                        FillSolidFast(hdc, cr, HeatColor(hc.pct));
                        // 【1px 描边 #EAD5A0】实测 y=136..792 全高、x=356/429/502
                        // 三处恒为该色，而 x=236（名称↔状态）没有 —— 说明它是
                        // **热力单元格**的左右框，不是「列间竖线」（状态列不染色
                        // 所以没有框）。UIFILE 双源：gtc(TASKMANAGER,2,0,3802)。
                        // 本文件下方 PaintColumnHeader 的旧注释
                        // 「数据区没有竖线」是错的，已在该处一并订正。
                        DrawVLineFast(hdc, cx,          clip.top, clip.bottom - 1, kLineHeatCell);
                        DrawVLineFast(hdc, chRight - 1, clip.top, clip.bottom - 1, kLineHeatCell);
                    }
                }
            }

            COLORREF textColor = sel ? kTextSelected : kTextPrimary;

            // 图标（按缩进右移）
            //
            // 【几何：无依据，沿用旧值，但与 theme.h 的栅格自洽】
            // 所有行的图标**在同一条竖线上**，无论这行有没有展开箭头 ——
            // 缩进沟槽是**固定预留**的，箭头画在沟槽里（rel 8..15），不推挤图标。
            // 曾经在 `row.hasChildren` 时额外 `rowIndent += 20`，结果应用行整体
            // 右移 20px，看起来像两级缩进。
            //
            // 锚点值来源已降级：原先引用的裁切图无法证明是原版（见 DisplayName
            // 上方的「证据复核」段）。现值靠 theme.h 的栅格支撑：
            //   x0 = 列表左界 + kIndentPerLevel(18)
            //   图标左 = x0 + kIconLeft(10) = rel 28
            //   图标槽 16×16（kIconSize，= SM_CXSMICON 在 96dpi 的值，【系统API实测】）
            //   文字左 = 28 + 16 + 6 = rel 50
            int rowIndent = row.indentLevel * kTreeIndent;
            RECT irc{ kRowIconLeft + rowIndent, y + (rh - theme::kIconSize) / 2,
                      kRowIconLeft + rowIndent + theme::kIconSize,
                      y + (rh - theme::kIconSize) / 2 + theme::kIconSize };
            DrawAppIcon(hdc, irc, pe->imageName, pe->pid);

            // 展开/折叠箭头（只对有子进程的应用行画）
            //
            // 【尺寸：系统API实测 15 × 15】
            // `GetThemePartSize(Explorer::ListView, LVP_EXPANDBUTTON(8) /
            // LVP_COLLAPSEBUTTON(9), TS_TRUE)` 返回 **15 x 15**（TS_MIN 1x1）。
            // 出处 docs/specs/T_uxspec.md §1.6。
            // 这是树展开箭头图元的权威边界框，比旧代码的 7x10 大得多 ——
            // 15x15 正好等于 `SM_CXSMICON - 1`，与图标的度量一致。
            //
            // 【形状：主题提供位图，无法还原矢量】主题里它是位图资产
            // （`U_render2_console.txt` 的 `Explorer::TreeView part 2/4 渲染为全白`
            // —— 箭头是图片，不是可解析的图形）。这里在 15x15 框内画一个
            // 实心三角：15 宽 × 15 高，边长按 2:1 的扁三角（宽 9、高 9）。
            // 实心三角是 Windows 树控件与 ListView 的常见形态；
            // 旧代码用的是「空心 1px 折线」，依据是来源未记录的裁切图，已弃。
            //
            // 【颜色：规范未规定】取 kTextSecondary(#5A5A5A)，与正文文字同族。
            // 旧值 #A7A7A7 无来源。
            if (row.hasChildren) {
                // TreeTriangleX 仍给出沟槽内的水平区间（rel 8..15），
                // 但现在按 15x15 的框居中摆放：框左 = 沟槽左界，
                // 框宽 15 → 正好覆盖 rel 8..22，与图标槽（rel 28..43）不重叠。
                int triL = 0, triR = 0;
                TreeTriangleX(row.indentLevel, triL, triR);
                (void)triR;
                // 【真原版实测：细描边折角，不是实心三角 · 2026-11-21】
                //
                // 此前这里画的是「8×7 实心三角」，依据 docs/reference/orig2_rows.png。
                // **那张图是我方截图，不是原版**：整图只有 267 种颜色，三角是
                // 纯 #5A5A5A、零抗锯齿 —— 这是 GDI Polygon 的特征；真原版截图
                // 有抗锯齿渐变。项目历史上 m3.png / truth1.png 也是同样的坑。
                //
                // 真原版数据（两张截图结果完全一致，互为印证）：
                //   original_win10.png  Edge 行 y=239..247
                //   orig2_full.png      y=313..321
                //   （# = 核心线 1px，# = 抗锯齿过渡）
                //     y+0  ..oo.......      x+0
                //     y+1  ..o#o......      x+1
                //     y+2  ...o#o.....      x+2
                //     y+3  ....o#o....      x+3
                //     y+4  ....o##o...      x+3,x+4   ← 折角
                //     y+5  ....o#o....      x+3
                //     y+6  ...o#o.....      x+2
                //     y+7  ..o#o......      x+1
                //     y+8  ..oo.......      x+0
                //
                // 即**两条短斜线组成的「>」折角，9 行高，每行 1 个核心像素**
                // （折角行 2 个）。不是实心三角，也不是三角形轮廓。
                //
                // 【颜色】原版核心读到 #A6A6A6，但那是白底抗锯齿渲染的结果。
                // GDI 的 SetPixelV 是硬像素、无抗锯齿，直接用 #A6A6A6 会偏淡；
                // 取 kTextSecondary(#5A5A5A)（与正文文字同族），视觉上等价于
                // 原版「一条清晰的深灰折线」，也是我方一直用的值。
                //
                // 【展开态 ▼】两张参考图里全是折叠态，没有展开态样本。
                // 按 90° 旋转推定（原版主题里 EXPANDBUTTON/COLLAPSEBUTTON 是
                // 同一颗字形的两个方向）。如需精确对齐，要一张展开状态的截图。
                if (row.hasChildren) {
                    int triL = 0, triR = 0;
                    TreeTriangleX(row.indentLevel, triL, triR);
                    (void)triR;

                    const int tx0 = triL;

                    // 折叠 ▶：9 行，核心像素相对 tx0 的 x 偏移
                    static const signed char kCx[9] = { 0, 1, 2, 3, 3, 3, 2, 1, 0 };

                    if (row.expanded) {
                        // ▼：▶ 顺时针转 90° —— 两条斜边从顶部两端收拢到底部一点。
                        // 5 行足够（宽度 9 / 2 ≈ 4.5），单独居中。
                        const int vh = 5;
                        const int vy0 = y + (rh - vh) / 2;
                        for (int i = 0; i < vh; ++i) {
                            const int py = vy0 + i;
                            SetPixelV(hdc, tx0 + i, py, kTextSecondary);
                            if (8 - i != i) {
                                SetPixelV(hdc, tx0 + (8 - i), py, kTextSecondary);
                            }
                        }
                    } else {
                        const int ty0 = y + (rh - 9) / 2;
                        for (int i = 0; i < 9; ++i) {
                            SetPixelV(hdc, tx0 + kCx[i], ty0 + i, kTextSecondary);
                            // 折角那一行多一个像素（原版 y+4 是 x+3,x+4）
                            if (i == 4) SetPixelV(hdc, tx0 + 4, ty0 + i, kTextSecondary);
                        }
                    }
                }
            }

            // 名称（含缩进）
            // 【关键】原版显示的是 exe 的文件描述，不是文件名。
            // 排序也用这个（见 RebuildRows 的 case 0）。
            //
            // 应用行在名称后追加 " (N)"，N 是子进程数：组头已经有「应用 (n)」
            // 的计数语义，但进程行也需要 —— 否则用户看不出哪些行可展开。
            // 【" (N)" 这个后缀本身：无依据，沿用旧值】
            // 它原先来自来源未记录的裁切图（见 DisplayName 上方的「证据复核」），
            // 而 MUI 字符串表里没有对应的格式串（`O_strings.md` 未收录）。
            // 保留是因为它不冲突、语义清楚；但不再声称是原版格式。
            //
            // 文字起点 = 图标左界 + 图标宽 + 6px 间距：
            //   rel 28 + 16 + 6 = 50
            // 6px 间距【无依据，沿用旧值】，属 Win32 官方间距集里没有的一个值
            // （官方是 5/7/11/13/16/17/18/23）；不用 5 或 7 是因为现有栅格
            // 已经按 6 对齐，改动会牵动 TreeTriangleX/图标槽的整条链。
            int nameX = kRowTextLeft + rowIndent;
            RECT nameRc{ nameX, y, x0 + nameW - kCellPadRight, y + rh };
            // 【性能 · 只报告，未改】
            // `ProcessDisplayName` 按值返回 std::wstring，每行一次堆分配/释放
            // （内部还会走 GetFileDescription 的哈希查找 —— 那个是 O(1)，
            // 但返回值是拷贝）。一屏 20 行 ≈ 20 次分配/帧。
            // 改成返回 const std::wstring& 需要动 tmwindow_icons.cpp（非本文件范围），
            // 见报告交接单。这里只把「追加 (N)」改成对已有串 append，
            // 避免 `nameText += suffix` 再走一次 operator+ 的临时串。
            std::wstring nameText = ProcessDisplayName(pe->pid, pe->imageName);
            if (row.hasChildren) {
                // 【性能修正 · _PLAN.md A-02】此前每画一个应用行就遍历一遍
                // `cls_.appRootOf`（全量哈希表，562 条）数自己的子进程 ——
                // 每个应用行 O(n)，一屏十几个应用 = 上万次哈希比较，纯浪费。
                // `appAgg_[pid].processCount` 在 RebuildAppAggregates 里已经算好了
                // （含根自己）—— 这个字段此前**零读取**，属于典型的「算了不用」。
                // 子进程数 = processCount - 1。
                int kids = 0;
                auto itAgg = appAgg_.find(pe->pid);
                if (itAgg != appAgg_.end() && itAgg->second.processCount > 1) {
                    kids = static_cast<int>(itAgg->second.processCount) - 1;
                }
                if (kids > 0) {
                    // 【缓冲区安全】`" (%d)"` 最坏情况是 2 + 10 位十进制 + 1 个
                    // 终止符 = 13 个 wchar_t。processCount 是 uint32_t，
                    // 上限 4294967295（10 位）。24 已足够，留了近一倍余量。
                    wchar_t suffix[24];
                    int n = swprintf_s(suffix, L" (%d)", kids);
                    if (n > 0) {
                        // 先 reserve 再 append：否则 append 一旦越过当前容量，
                        // 会整串重新分配 + 拷贝。一屏十几个应用行就是十几次
                        // 多余的堆操作。
                        nameText.reserve(nameText.size() + static_cast<size_t>(n));
                        nameText.append(suffix, static_cast<size_t>(n));
                    }
                }
            }
            DrawTextClipped(hdc, nameText.c_str(), &nameRc,
                            DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS,
                            textColor);

            // 状态
            // 【优先级】已挂起 > 无响应。实测：被 NtSuspendProcess 挂起的进程
            // IsHungAppWindow 返回 0，但若将来改用 SendMessageTimeout 判定，
            // 挂起的 UWP 应用会被错标成无响应。显式按优先级写，防止踩坑。
            //
            // 【列可见性】隐藏的列必须整块跳过：colX[] 里隐藏列宽度是 0，
            // 它与后一列的 x 相同，但 LiveW() 仍是真实宽度 —— 不跳过就会把
            // 本列文字画到后一列的格子里（两列文字叠在一起）。
            if (LiveVisible(1)) {
                const wchar_t* statusText = nullptr;
                if (cls_.suspended.count(pe->pid))          statusText = L"已挂起";
                else if (cls_.notResponding.count(pe->pid)) statusText = L"无响应";

                if (statusText) {
                    RECT sr{ x0 + colX[1] + kCellPadLeft, y,
                             x0 + colX[1] + LiveW(1) - kCellPadRight, y + rh };
                    DrawTextClipped(hdc, statusText, &sr,
                                    DT_SINGLELINE | DT_VCENTER | DT_LEFT, textColor);
                }
            }

            // ---- 数值显示 ----
            // （shownCpu / shownMem 等已在前面算好，这里直接用 —— 热力图和
            //  数字必须用同一套值，否则染色深浅和显示的数字对不上。）
            //
            // 【列可见性】每个数值格都先判 LiveVisible，理由同状态列：
            // 隐藏列在 colX[] 里宽度为 0，与后一列同起点，画了就会叠字。

            // CPU
            if (LiveVisible(2)) {
                int cx = x0 + colX[2];
                FmtCpuText(shownCpu, buf, 64);
                RECT cr{ cx + kCellPadLeft, y, cx + LiveW(2) - kCellPadRight, y + rh };
                DrawTextClipped(hdc, buf, &cr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT, textColor);
            }

            // 内存（专用工作集；应用根显示合计）
            // 【资源值 ▸ 内存 ▸ 百分比/值】「查看 → 资源值」的六项以前是死项：
            // 圆点会动，但这三列的格式化只有一条路径，勾了画面一字不变。
            // 现在按 MenuBuilder::ResourceValueMode() 的第 0 组（内存）在
            // 「MB 值」与「占总物理内存的百分比」之间切换，与原版语义一致。
            if (LiveVisible(3)) {
                int cx = x0 + colX[3];
                if ((MenuBuilder::ResourceValueMode() / 2) == 0 &&
                    (MenuBuilder::ResourceValueMode() % 2) == 0) {
                    double pct = (memTotalBytes_ > 0)
                        ? static_cast<double>(shownMem) / static_cast<double>(memTotalBytes_) * 100.0
                        : 0.0;
                    if (pct <= 0.0) buf[0] = 0;
                    else swprintf_s(buf, 64, L"%.1f%%", pct);
                } else {
                    FmtMemText(shownMem, buf, 64);
                }
                RECT mr{ cx + kCellPadLeft, y, cx + LiveW(3) - kCellPadRight, y + rh };
                DrawTextClipped(hdc, buf, &mr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT, textColor);
            }

            // 磁盘
            // 【资源值 ▸ 磁盘 ▸ 百分比/值】第 1 组。百分比分支按「该进程磁盘
            // 吞吐占全系统磁盘吞吐的比例」显示；系统总吞吐为 0 时留空，
            // 不编造一个 0.0%（那会和「真的 0 字节」混淆）。
            if (LiveVisible(4)) {
                int cx = x0 + colX[4];
                if ((MenuBuilder::ResourceValueMode() / 2) == 1 &&
                    (MenuBuilder::ResourceValueMode() % 2) == 0 && sysDiskTotal > 0.0) {
                    double pct = shownDisk / sysDiskTotal * 100.0;
                    if (pct > 100.0) pct = 100.0;
                    if (pct <= 0.0) buf[0] = 0;
                    else swprintf_s(buf, 64, L"%.1f%%", pct);
                } else {
                    FmtDiskText(shownDisk, buf, 64);
                }
                RECT dr{ cx + kCellPadLeft, y, cx + LiveW(4) - kCellPadRight, y + rh };
                DrawTextClipped(hdc, buf, &dr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT, textColor);
            }

            // 网络
            // 【资源值 ▸ 网络 ▸ 百分比/值】第 2 组，分母同磁盘取全系统。
            if (LiveVisible(5)) {
                int cx = x0 + colX[5];
                if ((MenuBuilder::ResourceValueMode() / 2) == 2 &&
                    (MenuBuilder::ResourceValueMode() % 2) == 0 && sysNetTotal > 0.0) {
                    double pct = shownNet / sysNetTotal * 100.0;
                    if (pct > 100.0) pct = 100.0;
                    if (pct <= 0.0) buf[0] = 0;
                    else swprintf_s(buf, 64, L"%.1f%%", pct);
                } else {
                    FmtNetText(shownNet, buf, 64);
                }
                RECT nr{ cx + kCellPadLeft, y, cx + LiveW(5) - kCellPadRight, y + rh };
                DrawTextClipped(hdc, buf, &nr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT, textColor);
            }

            // GPU
            if (LiveVisible(6)) {
                int cx = x0 + colX[6];
                FmtGpuText(shownGpu, buf, 64);
                RECT gr{ cx + kCellPadLeft, y, cx + LiveW(6) - kCellPadRight, y + rh };
                DrawTextClipped(hdc, buf, &gr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT, textColor);
            }
        }

        y += rh;
    }

    lastVisibleRows_ = (lastVisible >= firstVisible && firstVisible >= 0)
                       ? (lastVisible - firstVisible + 1) : 0;

    SelectObject(hdc, oldFont);
}

// ============ 绘制：应用图标（用首字母色块，零 GDI+ 开销） ============

void TmWindow::DrawAppIcon(HDC hdc, const RECT& rc, const std::wstring& exeName, uint32_t pid) {
    if (exeName.empty()) return;

    // 优先用真实图标：按 pid 直接查已解析的路径，再查图标缓存。
    // 两条都是哈希查找，零阻塞。图标线程在后台慢慢填。
    HICON ico = nullptr;
    auto pit = pathCache_.find(pid);
    if (pit != pathCache_.end() && !pit->second.empty()) {
        ico = icons_.GetIcon(pit->second);
    }

    if (ico) {
        DrawIconEx(hdc, rc.left, rc.top, ico,
                   rc.right - rc.left, rc.bottom - rc.top,
                   0, nullptr, DI_NORMAL);
        return;
    }

    // 回退：通用程序图标。
    //
    // 【做法变更：不再自绘占位图，改用系统图标】
    // 原先是手绘一个「16×13 窗口形状 + 蓝块 + 灰文本区」的占位图，网格逐格取样、
    // 边框 #969697、蓝块 #0275D9 —— 那组数全部来自原版截图取样，
    // 而本轮核实这批裁切图**无法证明来自原版**（详见 DisplayName 上方的
    // 「证据复核」段）：它们既不含我方旧版指纹色，也匹配不上 truth1/m3/truth2/p0
    // 四张全窗口图的任何区域。拿来源不可考的单张图去定 16 格像素的细节，
    // 是上一轮最大的方法论错误，这里不再重犯。
    //
    // 改成直接向系统要一个「通用应用程序」图标：
    //   IDI_APPLICATION 是 user32 预定义的资源，16×16 @96dpi，
    //   尺寸由 rc（= theme::kIconSize = SM_CXSMICON = 16）决定，
    //   形状与配色由系统资产负责，我们**不发明任何像素**。
    // 这条路径只在「拿不到真实 exe 图标」时走到（LocatePaths 未完成 /
    // 进程无权限取图标），是纯兜底，观感重要性低于正确性。
    {
        static HICON genericIcon = LoadIconW(nullptr, IDI_APPLICATION);
        if (genericIcon) {
            DrawIconEx(hdc, rc.left, rc.top, genericIcon,
                       rc.right - rc.left, rc.bottom - rc.top,
                       0, nullptr, DI_NORMAL);
        }
    }
}

// ============ 绘制：状态栏 ============
//
// 【本函数是死代码，且它正是「truth1 是我方截图」的判定证据之一】
// 全项目零调用（tmwindow_events.cpp:83 明确写了「PaintStatusBar 保留函数但
// 不再调用」），rcStatus_ 在 tmwindow.cpp 的 BuildLayout 里被设成
// {0, H, W, H} 的空矩形。原版 Win10 任务管理器底部只有工具条
// （简略信息(D) | 打开资源监视器），**没有独立的进程数/CPU/内存状态栏**。
//
// 它之所以要保留（而不是删掉）：
//   - theme.h 的 kBgStatusBar 注释里挂着「PaintStatusBar 仍在引用它」的理由，
//     V agent 改 theme.h 时依赖这个前提；
//   - 删除本函数会同时要求删 kBgStatusBar，跨文件，本轮不做。
// 【交接】等 tmwindow.h:116 的声明与 theme.h:86-90 的 kBgStatusBar 一起删。
//
// 【注意：下面这条格式串就是证伪 truth1 的证据】
// `L"进程数: %u      CPU %.0f%%      内存 %.1f/%.1f GB (%.0f%%)"`
// truth1.png 底部 y=670..681 的文字逐字符吻合这一条，而 MUI 资源里
// 根本没有「进程数」这个词 —— 故 truth1 是我方程序的截图，不是原版。
// 此后本文件不再把 truth1 当任何几何/配色的依据。
void TmWindow::PaintStatusBar(HDC hdc) {
    FillSolidFast(hdc, rcStatus_, kBgStatusBar);
    DrawHLineFast(hdc, 0, rcStatus_.right, rcStatus_.top, kLineNavBorder);

    if (!snap_) return;

    uint32_t procs = 0;
    uint32_t threads = 0;
    uint64_t handles = 0;
    double cpuTotal = 0.0;
    for (const auto& e : snap_->processes) {
        if (e.pid != 0) ++procs;
        threads += e.threadCount;
        handles += e.handleCount;
        // 【排除 PID 0】同 PaintColumnHeader 的汇总：空闲进程的 CPU 时间
        // 是"系统空闲"的时间，加进来会让 CPU 汇总恒为 90~100%。
        if (e.pid != 0) cpuTotal += e.cpuPercent;
    }
    if (cpuTotal > 100.0) cpuTotal = 100.0;

    double usedGB = 0, totalGB = 0, pct = 0;
    if (snap_->totalPhysical > 0) {
        uint64_t used = snap_->totalPhysical - snap_->availablePhysical;
        usedGB  = used / 1073741824.0;
        totalGB = snap_->totalPhysical / 1073741824.0;
        pct     = static_cast<double>(used) / static_cast<double>(snap_->totalPhysical) * 100.0;
    }

    // 进程数在左，CPU/内存依次排开
    wchar_t text[512];
    swprintf_s(text,
        L"进程数: %u      CPU %.0f%%      内存 %.1f/%.1f GB (%.0f%%)",
        procs, cpuTotal, usedGB, totalGB, pct);

    HFONT old = static_cast<HFONT>(SelectObject(hdc, fontBody_));
    SetBkMode(hdc, TRANSPARENT);
    RECT tr{ rcStatus_.left + 10, rcStatus_.top, rcStatus_.right - 10, rcStatus_.bottom };
    DrawTextClipped(hdc, text, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT, kTextSecondary);
    SelectObject(hdc, old);
}

// ============ 绘制：分区分隔线 ============

void TmWindow::PaintDivider(HDC hdc) {
    (void)hdc;
}



// ============ 垂直滚动条 ============
//
// 【几何全量重定，依据 SM_CXVSCROLL + 主题 part 尺寸】
//   kScrollBarW  = SM_CXVSCROLL = 17   【系统API实测】R_sysmetrics.md §4.2
//                                      与 SPI_GETNONCLIENTMETRICS.iScrollWidth=17
//                                      双源一致；主题 SBP_ARROWBTN TS_TRUE=17x17 三源一致
//   kScrollBtnH  = 17                  【系统API实测】同上 SBP_ARROWBTN
//   thumb 最小长 = 11                  【系统API实测】SBP_THUMBBTNVERT TS_TRUE = 17x11
//                                      的 .cy（T_uxspec.md §8.1 kScrollbarThumbMinLen）
// 旧值 16/16/24 全部无依据（16 略小于系统 17；24 是自创的经验值）。
//
// 【配色】轨道 #F0F0F0 / 滑块 #CDCDCD / 热 #A6A6A6 / 按下 #606060 / 箭头 #606060
//   全部是 SCROLLBAR 类离屏渲染实测（U_themecolors.md §4.6）。旧代码的
//   「轨道纯白」是错的 —— 白是**列表底**，滚动条轨道是 #F0F0F0。
namespace {
constexpr int kScrollBarW = 17;   // 【系统API实测】SM_CXVSCROLL
constexpr int kScrollBtnH = 17;   // 【系统API实测】SBP_ARROWBTN TS_TRUE.cy
constexpr int kThumbMinLen = 11;  // 【系统API实测】SBP_THUMBBTNVERT TS_TRUE.cy
constexpr COLORREF kSbTrack   = RGB(0xF0, 0xF0, 0xF0);
constexpr COLORREF kSbThumb   = RGB(0xCD, 0xCD, 0xCD);
constexpr COLORREF kSbArrow   = RGB(0x60, 0x60, 0x60);

// ---------------------------------------------------------------------------
// 滚动条箭头的镂空字形
// ---------------------------------------------------------------------------
//
// 【图案】original_win10.png 逐像素实测。上箭头 x=1237..1243、y=142..147。
// 归一化成 6 行 × 7 列（列 0 = cx-3，列 6 = cx+3，列 3 = cx）：
//
//   行0:  0 0 0 1 0 0 0     尖端一个点（cx）
//   行1:  0 0 1 1 1 0 0     cx-1 .. cx+1
//   行2:  0 1 1 1 1 1 0     cx-2 .. cx+2（两笔在这里交汇，所以是实心的一行）
//   行3:  1 1 1 0 1 1 1     cx-3..cx-1 与 cx+1..cx+3（中间 cx 空）
//   行4:  1 1 0 0 0 1 1     cx-3,cx-2 与 cx+2,cx+3（空洞 cx-1..cx+1）
//   行5:  1 0 0 0 0 0 1     cx-3, cx+3
//
// 注意外侧边：行2 就已经到满宽 ±3，行3..行5 外侧**一直保持** ±3，
// 收窄的只有内侧。所以这不是「等宽三角挖空」，不能用公式推，必须逐行查表。
// （行3 左笔 -3..-1 是 3 个像素，行4 左笔 -3..-2 是 2 个，行5 只剩 -3。）
//
// 行号 0..5 从上到下；down 时整张表倒序（原版下箭头 y=782..787 实测就是
// 上箭头的精确镜像，没有别的差别）。
constexpr int kArrowH = 6;         // 行数
constexpr int kArrowHalfW = 3;     // 单侧最远偏移，总宽 7
constexpr unsigned kArrowGlyphUp[kArrowH] = {
    0x08,   // 行0  0001000
    0x1C,   // 行1  0011100
    0x3E,   // 行2  0111110
    0x77,   // 行3  1110111
    0x63,   // 行4  1100011
    0x41,   // 行5  1000001
};

} // namespace

void TmWindow::DrawHollowArrow(HDC hdc, const RECT& btn, bool up, COLORREF color) {
    const int cx = (btn.left + btn.right) / 2;
    // 垂直居中：图形高 6，上下留白差不超过 1px。
    // 17px 按钮 ⇒ top+6 .. top+11。
    // 【实测校验】原版上钮 y=136..152（17px）、箭头 y=142..147 ⇒ 偏移 6..11；
    // 下钮 y=776..792、箭头 y=782..787 ⇒ 偏移同样是 6..11。两端同一个公式。
    // （0 基坐标下这也等于 cy-2 .. cy+3，cy = 按钮垂直中心。）
    const int h = btn.bottom - btn.top;
    const int y0 = btn.top + (h - kArrowH + 1) / 2;

    // 【性能】走本文件的画刷缓存，和 FillSolidFast / FillPolygonFast 一致。
    // 此前这里是裸 CreateSolidBrush + DeleteObject，注释还写着「static 拿不到
    // 成员缓存」—— 每帧 2 次 Create/Delete 正是本文件 621 行那段注释专门修掉的模式。
    // 注意必须写 `tmp::` 限定：类里有一个同名的非静态成员 CachedBrush(COLORREF)，
    // 在成员函数体内它会**遮蔽**文件作用域那个自由函数，不限定就编不过（C2352）。
    HBRUSH b = tmp::CachedBrush(color);
    if (!b) return;
    HGDIOBJ oldBrush = SelectObject(hdc, b);
    for (int row = 0; row < kArrowH; ++row) {
        const unsigned bits = kArrowGlyphUp[up ? row : (kArrowH - 1 - row)];
        const int y = y0 + row;
        // 每行是一段或两段连续像素，用 FillRect 逐段填 1px 高的小条：
        // 比 Polygon / LineTo 更精确（不受「闭合路径最后一个像素」影响），
        // 也不会像 FillPolygonFast 那样把中间也涂满。
        int col = 0;
        while (col < kArrowHalfW * 2 + 1) {
            if (!(bits & (1u << (kArrowHalfW * 2 - col)))) { ++col; continue; }
            const int start = col;
            while (col < kArrowHalfW * 2 + 1 &&
                   (bits & (1u << (kArrowHalfW * 2 - col)))) {
                ++col;
            }
            RECT r{ cx - kArrowHalfW + start, y,
                    cx - kArrowHalfW + col,     y + 1 };
            FillRect(hdc, &r, b);
        }
    }
    SelectObject(hdc, oldBrush);
}


int TmWindow::ContentHeight() const {
    // 【子行 22】逐行取实际行高：组头/父行 28、子进程行 22。
    // 这里必须与 PaintRows 的累加完全一致，否则滚动条滑块长度和
    // 滚到底的停位都会偏。
    int h = 0;
    for (const auto& r : rows_) {
        h += RowHeightFor(r.kind == ViewRow::GroupHeaderRow, r.indentLevel);
    }
    return h;
}

int TmWindow::ViewportHeight() const {
    return rcList_.bottom - rcList_.top;
}

RECT TmWindow::ScrollThumbRect() const {
    RECT none{ 0, 0, 0, 0 };
    const int total = ContentHeight();
    const int view = ViewportHeight();
    if (total <= view || view <= 0) return none;   // 不需要滚动条

    // 【右侧 3px 留白：无依据，沿用旧值】轨道右界比列表右界内缩 3px，
    // 让滚动条与窗口边框之间留一条白。规范没有规定这个值。
    // 【交接】tmwindow_events.cpp:383/390 的箭头命中区仍写死 `btnH = 16`，
    // 与本文件的 kScrollBtnH=17 差 1px。那个文件不归本轮改动，见报告交接单。
    const int trackRight = rcList_.right - 3;
    const int trackX = trackRight - kScrollBarW;
    // 滑块只在两端箭头之间滑动
    const int trackTop = rcList_.top + kScrollBtnH;
    const int trackH = view - kScrollBtnH * 2;
    if (trackH <= 0) return none;

    int thumbH = trackH * view / total;
    // 滑块最小长 11（主题 SBP_THUMBBTNVERT TS_TRUE.cy，系统API实测）
    if (thumbH < kThumbMinLen) thumbH = kThumbMinLen;
    if (thumbH > trackH) thumbH = trackH;

    const int maxScroll = total - view;
    // 【兜底钳制】scrollY_ 正常应由 ClampScroll() 维护，但它是 const 方法、
    // 改不了成员。这里用局部钳制值算滑块位置：即使 scrollY_ 一时超界
    // （比如 WM_SIZE 之后、下一次 ClampScroll 之前），滑块也不会被顶出轨道。
    // 实测症状：超界时滑块压住下箭头 16px，而滑块命中判断排在箭头之前，
    // 于是点下箭头变成拖滑块。
    const int sy = (scrollY_ < 0) ? 0 : (scrollY_ > maxScroll ? maxScroll : scrollY_);
    int thumbY = trackTop;
    if (maxScroll > 0) {
        thumbY = trackTop + (trackH - thumbH) * sy / maxScroll;
    }
    return RECT{ trackX + 1, thumbY, trackX + kScrollBarW - 1, thumbY + thumbH };
}

void TmWindow::PaintScrollBar(HDC hdc) {
    g_liveWidths = colWidths_;
    g_liveVisible = colVisible_;
    const int total = ContentHeight();
    const int view = ViewportHeight();
    if (total <= view) return;

    // 【几何与配色：全部 SCROLLBAR 主题实测】
    //   宽 17（SM_CXVSCROLL）、两端箭头按钮各 17×17（SBP_ARROWBTN）、
    //   轨道 #F0F0F0、滑块 #CDCDCD 直角、箭头 #606060。
    // 旧注释引用的「m3.png 取样、宽 16、轨道纯白、
    // 滑块 #CDCDCD」中：m3 是我方旧截图（已证伪），c_heat 是本轮核实为
    // 来源不可考的裁切图；宽 16 与系统值 17 不符；轨道白是把它与列表底混淆。
    // 出处 docs/specs/U_themecolors.md §4.6 / R_sysmetrics.md §4.2。
    // 【右侧 3px 留白：无依据，沿用旧值】轨道右界比列表右界内缩 3px，
    // 让滚动条与窗口边框之间留一条白。规范没有规定这个值。
    // 【交接】tmwindow_events.cpp:383/390 的箭头命中区仍写死 `btnH = 16`，
    // 与本文件的 kScrollBtnH=17 差 1px。那个文件不归本轮改动，见报告交接单。
    const int trackRight = rcList_.right - 3;
    const int trackX = trackRight - kScrollBarW;
    RECT track{ trackX, rcList_.top, trackRight, rcList_.bottom };

    // 轨道：#F0F0F0（SCROLLBAR SBP_TRACK 渲染实测 = COLOR_BTNFACE）
    FillSolidFast(hdc, track, kSbTrack);

    // 两端箭头按钮（各 kScrollBtnH 高，底色同轨道）
    RECT upBtn{ trackX, track.top, trackRight, track.top + kScrollBtnH };
    RECT dnBtn{ trackX, track.bottom - kScrollBtnH, trackRight, track.bottom };
    FillSolidFast(hdc, upBtn, kSbTrack);
    FillSolidFast(hdc, dnBtn, kSbTrack);

    auto drawArrow = [&](const RECT& r, bool up) {
        // 【修复：实心三角 → 镂空轮廓】原实现用 FillPolygonFast 画实心三角，
        // 颜色（#606060）和外接框（宽 7、高 4）都对，但中间被填满了 ——
        // 原版是 SBP_ARROWBTN 的**描边字形**，放大看是空心的。填充版在
        // 17px 的按钮里只剩一团墨，这正是用户报的「箭头分辨率太低」。
        // 详见 TmWindow::DrawHollowArrow 的注释（图案逐像素来自
        // docs/reference/original_win10.png）。
        DrawHollowArrow(hdc, r, up, kSbArrow);
    };
    drawArrow(upBtn, true);
    drawArrow(dnBtn, false);

    // 滑块：#CDCDCD 直角矩形（不圆角）—— SBP_THUMBBTNVERT s1 渲染实测
    RECT th = ScrollThumbRect();
    if (th.right > th.left && th.bottom > th.top) {
        FillSolidFast(hdc, th, kSbThumb);
    }
}
} // namespace tmp
