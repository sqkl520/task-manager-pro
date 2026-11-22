// perfpage.h - 性能页绘制
//
// 几何依据：原版 taskmgr.exe 的 DUI UIFILE 资源（声明式布局）——
//   docs/evidence/exec/_S_geo2_30001.tsv / _S_geo2_30024.tsv
//   docs/specs/S_duispec.md（元素树 + rp 值 + 主题引用解析）
// 配色依据：docs/specs/U_themecolors.md（OpenThemeData + GetThemeColor 实测）
// ⚠ truth1.png、truth2.png 已复核判定为**我方自己程序的截图**（_FINAL_VERDICT.md），
//   不再作为任何几何/像素的依据。
//
//   ┌──────────┬────────────────────────────────────────┐
//   │ CPU      │ CPU                                    │
//   │ 内存     │ Intel(R) Xeon(R) CPU 服务器级 CPU ...    │
//   │ 磁盘 0   │ ┌────────────────────────────────────┐ │
//   │ 磁盘 1   │ │100%│       ╱╲                        │ │
//   │ 以太网   │ │    │  ╱╲__╱  ╲___                  │ │
//   │ Wi-Fi    │ │  0%│__╱                            │ │
//   │ GPU 0    │ └────────────────────────────────────┘ │
//   │          │ ┌────┐┌────┐┌────┐┌────┐┌────┐        │
//   │[列表]    │ │CPU0││CPU1││CPU2││CPU3││CPU4│  逻辑   │
//   │ 210px    │ │ 45%││ 38%││ 51%││ 22%││ 19%│  处理器 │
//   │          │ └────┘└────┘└────┘└────┘└────┘        │
//   │          │ 利用率 45%  速度 3.00 GHz  运行时间 ... │
//   └──────────┴────────────────────────────────────────┘
#pragma once

#include "perfdata.h"
#include "theme.h"

#include <windows.h>
#include <vector>
#include <unordered_map>
#include <string>

namespace tmp {

enum class PerfView {
    Cpu = 0,
    Memory,
    Disk,
    Network,
    Gpu,
    Count
};

// 「将图形更改为」子视图（MENU 30206）
//
// 【P0-6 修复】这 5 个值之前只有 Overall / Logical 有绘制分支，
//   Numa / SingleEngine / MultiEngine 三个是「可选、打勾、纹丝不动」。
//   现在每个值都有真实绘制分支，并且 SetChart() 会把页面切到能画出它的那一页：
//     Cpu 页：Overall  = 单条总利用率曲线（hist_.cpu）
//             Logical  = 逐逻辑处理器网格（hist_.coreHist）
//             Numa     = 逐 NUMA 节点网格（GetNumaProcessorNodeEx 把逻辑处理器
//                        分到节点，逐节点对 hist_.coreHist 取平均）
//     Gpu 页：Overall / Logical / Numa = 总利用率曲线 + 显存 + 4 个引擎小图（默认布局）
//             SingleEngine = 放大的单个引擎（选当前占用最高的那个）曲线
//             MultiEngine  = 满屏 4 个引擎网格
//   数据全部来自已有的 PerfSnapshot（cores / gpuEngines），不需要采集层新增字段。
enum class ChartMode {
    Overall = 0,     // 总体利用率（CPU 页）/ 总利用率（GPU 页默认布局）
    Logical,         // 逻辑处理器格子（CPU 页）
    Numa,            // NUMA 节点（CPU 页）
    SingleEngine,    // 单个引擎（GPU 页）
    MultiEngine      // 多个引擎（GPU 页）
};

// ===========================================================================
// 单位约定：DUI 的 rp（relative pixel）
//
//   px = rp * dpi / 96        （DUI 文档口径；本机 100% ⇒ 1rp = 1px）
//
// 【系统API实测】本机 GetDpiForSystem()=96、GetDpiForMonitor(MDT_EFFECTIVE_DPI)=96x96、
//   SPI_GETLOGICALDPIOVERRIDE=0（docs/specs/R_sysmetrics.md §1）。
// 所有来自原版 UIFILE 的几何值一律**按 rp 存常量**，绘制时用 Rpx() 换算，
// 这样换到 125%/150% 机器时不需要逐个改数字。
// ===========================================================================
inline int Rpx(int rp) {
    static const int s_dpi = []() -> int {
        HDC dc = GetDC(nullptr);
        int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc) ReleaseDC(nullptr, dc);
        return dpi > 0 ? dpi : 96;
    }();
    return MulDiv(rp, s_dpi, 96);
}

namespace perftheme {

// ===== 侧栏（performancetab_graphs / dashSidebar*）=====
// 【UIFILE 声明】dashSidebarScrollViewer padding = rect(3rp,7rp,0rp,0rp)
//   来源：docs/evidence/exec/_S_geo2_30001.tsv @0x4c78（S_duispec.md §1.2）
constexpr int kSidebarTopPad  = 7;    // 顶内边距 7rp（旧值 13 反解自已证伪的 truth1.png）
constexpr int kSidebarLeftPad = 3;    // 左内边距 3rp（旧代码把卡片左沿硬编码为 side.left+8）
// 【UIFILE 未定义】侧栏宽度/卡片宽由 BorderLayout + 内容动态决定，UIFILE 无 height/width 声明
//   （dashSidebarEntry 只声明 padding: rect(0,0,3rp,0)，那是右侧留白）。
//   下面两项保留旧值，仅表示当前视觉基线，**不是原版数据**：210/202 反解自已证伪的 truth1。
constexpr int kSidebarWidth  = 210;   // 【UIFILE 未定义，沿用旧值】
constexpr int kSidebarCardW  = 202;   // 【UIFILE 未定义，沿用旧值】
// 【UIFILE 未定义】卡片高由「迷你图 45rp + 若干行文字」撑出，UIFILE 从不声明卡片高度。
//   61/68 两档反解自已证伪的 truth1，保留仅为不改变现有步进；项之间无间隙。
constexpr int kSidebarCardH2 = 61;    // 【UIFILE 未定义，沿用旧值】
constexpr int kSidebarCardH3 = 68;    // 【UIFILE 未定义，沿用旧值】
// 【UIFILE 声明】chart 样式：width=60rp @0x290、height=45rp @0x28c、
//   padding=rect(0rp,5rp,0rp,0rp) @0x288、margin=rect(20rp,10rp,10rp,10rp) @0x284
//   —— 这就是侧栏卡片里的迷你图（dashSidebarCpuChart 等挂 sheet="chart"）。
constexpr int kMiniChartW = 60;       // 声明宽 60rp（旧值 61，差 1）
constexpr int kMiniChartH = 45;       // 元素高 45rp；扣 padding-top 5rp 后可视内容高 40rp
// 【UIFILE 声明】chartArea 位置参数 rect(15rp,0rp,0rp,0rp) —— CPU/内存/GPU 三页共用
//   （_S_geo2_30001.tsv @0x572a / @0x5cd2 / @0x6b16）；磁盘/网络页是 rect(1rp,0,0,0)
//   （@0x61de / @0x628e / @0x6756）。旧值 16 无出处。
constexpr int kChartPad       = 15;   // 图表区左内缩 15rp（宽松组：CPU/内存/GPU）
constexpr int kChartPadTight  = 1;    // 图表区左内缩 1rp（紧贴组：磁盘/网络）
// 【UIFILE 声明】主图容器 sheet="staticResourceChart"：height = 80rp @0x320。
//   注：chartArea 的 layoutpos=client 会把容器拉伸填满剩余空间，所以 80rp 是**基准最小高**，
//   不是最终像素高。旧值 160 反解自已证伪的 truth1，且与声明值差一倍。
constexpr int kChartHeight    = 80;
constexpr int kStatsRowHeight = 24;   // 【UIFILE 未定义，沿用旧值】右侧属性行高由字体+margin 撑出
// 【UIFILE 声明】performancetab_tables 根 padding = rect(17rp,0rp,0rp,0rp)
//   （_S_geo2_30001.tsv @0x5180）—— 整个右侧内容区的左内缩。
//   ⚠ 这个值同时被 PaintContent 的 inner 和 HitTestChartToggle 的 inner 使用，
//     两处必须一致，故提取成常量。
constexpr int kContentPadL    = 17;

// ===== 侧栏颜色 =====
constexpr COLORREF kSidebarBg     = RGB(255, 255, 255);
// ↑ 【UIFILE 引用系统色】dashSidebar sheet="drop" background=window；本机
//   GetSysColor(COLOR_WINDOW)=#FFFFFF（R_sysmetrics.md §5.1），与旧值一致。
constexpr COLORREF kSidebarSel    = RGB(0xCC, 0xE8, 0xFF);
// ↑ 【主题实测 DrawThemeBackground】dtb(EXPLORER::LISTVIEW,1,6) LISS_HOTSELECTED 渲染底 = #CCE8FF
//   （U_themecolors.md §4.1）。注意：UIFILE 的 dashSidebarEntryViewer **没有**引用 1,6，
//   所以这是「值对得上、引用链缺失」的一项，原样保留。
constexpr COLORREF kSidebarHover  = RGB(0xE5, 0xF3, 0xFF);
// ↑ 【主题实测 DrawThemeBackground】dtb(EXPLORER::LISTVIEW,1,2) LISS_HOT = #E5F3FF
//   （U_themecolors.md §4.1 / §8.2，8000 像素纯色无渐变）。旧值 #E5F3FB 蓝通道差 4。
constexpr COLORREF kSidebarLine   = RGB(0xE5, 0xE5, 0xE5);
// ↑ 【主题未定义，沿用旧值】UIFILE 的 dashSidebarDivider 用的是 sgraphic(31222/31223/31224)
//   贴图（45rp×8rp），不是纯色线，取不到 RGB。
constexpr COLORREF kSidebarAccent = RGB(0x00, 0x78, 0xD7);
// ↑ 【无依据，按旧值保留】UIFILE 里**没有任何「3px 竖条」元素**：dashSidebarEntry 的
//   padding: rect(0,0,3rp,0) 是右侧留白，不是左侧强调条。本常量当前零引用，值未改。

// ===== 图表颜色 =====
constexpr COLORREF kChartBg       = RGB(0xF1, 0xF6, 0xFA);
// ⚠ 已停用：主图网格不再用单一常量。改用下方 GridColorFor(view_) 按页取色
//   （原版内存页实测 #ECDEF0，CPU 组 #D9EAF4）。此常量仅保留避免破坏外部引用。
constexpr COLORREF kChartGrid     = RGB(0xE8, 0xF0, 0xF9);
constexpr COLORREF kChartFill     = RGB(0xF1, 0xF6, 0xFA);
constexpr COLORREF kChartAreaBg   = RGB(0xF1, 0xF6, 0xFA);
// ↑ 以上四个是 **CPU 页**的值。
//
// 【每页的网格色不同 —— 真原版截图实测】
// docs/reference/perf_mem_orig.png 内存页的网格线实测是 **#ECDEF0（浅紫）**，
// 不是我方一直用的 #E8F0F9（浅蓝）。间距也是 31px 而不是 16px。
//
// 这与 CHARTVIEW 主题给每页配的「主色 + 浅色」一致（U_themecolors.md §3.1）：
//   CPU  主 #117DBB  浅 #D9EAF4
//   内存 主 #8B12AE  浅 #ECDEF0
//   磁盘 主 #4DA60C  浅 #DBEDCE
//   网络 主 #A74F01  浅 #EEDECF
// **我方此前所有页都用了 CPU 那套**，所以内存页的网格是蓝的、配色不协调。
//
// 【仍无主题依据的部分】#F1F6FA（填充）、#DEECF5、#6AAED4 在 TASKMANAGER +
// CHARTVIEW 两个类 part 0..80 × state 0..20 × 24 个颜色属性里 0 命中，
// 在 taskmgr.exe / wdc.dll / dui70.dll / Aero.msstyles 里也 0 命中 ——
// 这些是 CHARTV.dll 内部常量或运行时算出，主题取不到。
// 但「每页网格色」这一项现在有了原版截图的直接实测值，不再是无依据。
constexpr COLORREF kGridCpu    = RGB(0xD9, 0xEA, 0xF4);   // CHARTVIEW part 3
constexpr COLORREF kGridMem    = RGB(0xEC, 0xDE, 0xF0);   // CHARTVIEW part 10；原版截图实测
constexpr COLORREF kGridDisk   = RGB(0xDB, 0xED, 0xCE);   // CHARTVIEW part 17
constexpr COLORREF kGridNet    = RGB(0xEE, 0xDE, 0xCF);   // CHARTVIEW part 24
constexpr COLORREF kGridGpu    = kGridCpu;                // 与 CPU 同色

// 按页取网格线颜色。
// 【为什么需要】绘制主图的 PaintChart 被 5 个页面共用，网格色必须跟着 view_ 走；
//   以前所有页共用 kChartGrid（= CPU 那套浅蓝），内存页因此画成了蓝色网格。
inline COLORREF GridColorFor(PerfView v) {
    switch (v) {
    case PerfView::Cpu:     return kGridCpu;
    case PerfView::Memory:  return kGridMem;
    case PerfView::Disk:    return kGridDisk;
    case PerfView::Network: return kGridNet;
    case PerfView::Gpu:     return kGridGpu;
    default:                return kGridCpu;
    }
}
// 主图表曲线颜色：原版每页不同（与左侧卡片的迷你图同色）。
constexpr COLORREF kChartLineCpu  = RGB(0x11, 0x7D, 0xBB);
// ↑ 【主题实测 GetThemeColor】CHARTVIEW part 2/4 FILL（TMT_FILLCOLOR=3802）= #117DBB；
//   同一值也是 TASKMANAGER part 38 cpuHeatGrid 的 FILL（U_themecolors.md §2.5 / §3.1 / §3.3）
constexpr COLORREF kChartLineMem  = RGB(0x8B, 0x12, 0xAE);
// ↑ 【主题实测 GetThemeColor】CHARTVIEW part 9 FILL = #8B12AE（§3.1；UIFILE 里是
//   BarchartMemoryHolder 的 bordercolor，gtc(CHARTVIEW,9,0,3802)）
constexpr COLORREF kChartLineDisk = RGB(0x4D, 0xA6, 0x0C);
// ↑ 【主题实测 GetThemeColor】CHARTVIEW part 16 FILL = #4DA60C（§3.1）
constexpr COLORREF kChartLineNet  = RGB(0xA7, 0x4F, 0x01);
// ↑ 【主题实测 GetThemeColor】CHARTVIEW part 23 FILL = #A74F01（§3.1）
constexpr COLORREF kChartLineGpu  = RGB(0x11, 0x7D, 0xBB);
// ↑ 【主题实测 GetThemeColor】与 CPU 同组：CHARTVIEW part 2/4 = #117DBB（§3.1）
constexpr COLORREF kChartLine     = kChartLineCpu;   // 兼容默认值
// 轴文字：原版 lheader/rheader 的 fg 是 gtc(TASKMANAGER,44,0,3803) = #707070，
// ChartView part 36/38/40/42 的 TEXT 也是 #707070（U_themecolors.md §3.2）。
// 旧值 #767676 反解自已证伪的 truth2，偏亮 6 级。
constexpr COLORREF kAxisText      = RGB(0x70, 0x70, 0x70);
// ↑ 【主题实测 GetThemeColor】#707070（两个独立来源：CHARTVIEW 36/38/40/42 与 TASKMANAGER 17/44）

// 标题/正文文字。主题给的是 resourceTitle=#000000、staticLabel=#707070，
// 与下面两项都有差；但性能页没有可信像素来源，暂不动值，只把差异写明。
constexpr COLORREF kTitleText     = RGB(0x1A, 0x1A, 0x1A);
// ↑ 【主题未定义，沿用旧值】UIFILE 的 atom(resourceTitle) 引用 gtc(TASKMANAGER,14,0,3803)=#000000
constexpr COLORREF kSubText       = RGB(0x5A, 0x5A, 0x5A);
// ↑ 【主题未定义，沿用旧值】staticLabel/dynamicLabel 引用 gtc(TASKMANAGER,17,0,3803)=#707070
}

// 性能页的数据历史（每项 60 点）
struct PerfHistory {
    static constexpr int kPoints = 60;
    // 最多记录 64 个逻辑处理器的历史。Win10 的「逻辑处理器」视图每格是一张
    // 微型面积图，所以需要逐核的环形缓冲，不能只留当前值。
    static constexpr int kMaxCores = 64;
    // 逐**实例**历史的容量：磁盘/网卡/GPU 各自的 sidecard 与主图需要用
    // 「被选中的那一个」的数据，不能再用聚合值代替（P0-7）。
    // 8 与 RebuildSidebarItems 里的 8 张卡片上限一致。
    static constexpr int kMaxInstances = 8;

    double cpu[kPoints] = {};
    // 内核时间曲线的独立历史。「查看 → 显示内核时间」勾上时在 CPU 主图上
    // 叠加第二条曲线。采集层一直有 PerfSnapshot::cpuKernelPercent，但以前
    // 只存了总利用率一条线，于是那个菜单项是**死开关**（勾上只多个对勾）。
    double cpuKernel[kPoints] = {};
    double mem[kPoints] = {};
    double disk[kPoints] = {};
    double net[kPoints] = {};
    double gpu[kPoints] = {};
    // 磁盘：逐盘的「活动时间 %」与「传输速率 MB/s」历史。
    // 以前只有一条聚合曲线（取 disks[0]），点第二块磁盘时图上画的还是第一块。
    double diskActiveI[kMaxInstances][kPoints] = {};
    double diskRateI[kMaxInstances][kPoints] = {};
    // 网络：逐适配器的收/发历史（MB/s）。以前用「最忙的适配器」聚合，
    // 导致侧栏高亮的是「以太网」而曲线可能画的是「WLAN」。
    double netSentI[kMaxInstances][kPoints] = {};
    double netRecvI[kMaxInstances][kPoints] = {};
    // GPU：逐适配器的总利用率历史。
    double gpuI[kMaxInstances][kPoints] = {};
    // 已提交（commit）历史：Win10 内存页第二条曲线的数据源
    double memCommit[kPoints] = {};
    // GPU 各引擎的使用率历史。索引对应 [engineSlot][point]
    // 槽位固定 4 个：3D / Copy / Video Encode / Video Decode
    static constexpr int kEngineSlots = 4;
    double gpuEngine[kEngineSlots][kPoints] = {};
    // 【多卡】逐适配器的引擎历史：[实例][引擎槽][点]。
    // 旧版只有上面那张合并表，于是核显 + 独显时点开 GPU 1 看到的是"两张卡里
    // 更忙的那个"的引擎曲线，和标题上的 GPU 1 对不上。8×4×60×8B = 15 KB。
    double gpuEngineI[kMaxInstances][kEngineSlots][kPoints] = {};
    // 最近一轮快照里实测到的实例数（≤ kMaxInstances）。
    // 用于越界回退：侧栏选中项失效时不能拿越界下标去画。
    int diskCount = 0;
    int netCount = 0;
    int gpuCount = 0;

    // 逐核历史：coreHist[coreIndex][pointIndex]
    double coreHist[kMaxCores][kPoints] = {};

    int head = 0;
    int count = 0;
    int coreCount = 0;

    void Push(double c, double m, double d, double n, double g);
    // 多带一个逐核数组的版本
    void PushCores(const std::vector<CpuCoreSample>* cores);

    // 取第 i 个（0 = 最旧）
    double At(const double* arr, int i) const {
        int idx = (head - count + i + kPoints * 2) % kPoints;
        return arr[idx];
    }
    // 取某核第 i 个值
    double AtCore(int core, int i) const {
        if (core < 0 || core >= kMaxCores) return 0.0;
        int idx = (head - count + i + kPoints * 2) % kPoints;
        return coreHist[core][idx];
    }
};

class PerfPage {
public:
    void SetData(std::shared_ptr<const PerfSnapshot> snap);
    void SetBounds(const RECT& rc) { bounds_ = rc; }
    const RECT& Bounds() const { return bounds_; }

    PerfView CurrentView() const { return view_; }
    void SetView(PerfView v);
    // 按名称直接选视图，便于脚本化验证（"cpu"/"memory"/"disk"/"network"/"gpu"）
    void SetViewByName(const wchar_t* name);
    ChartMode CurrentChart() const { return chart_; }
    // 设置「将图形更改为」子视图。
    // 【P0-6】会顺带把页面绑定到能画出该模式的页（Logical/Numa → CPU，
    //   SingleEngine/MultiEngine → GPU），所以调用方不需要自己再 SetView。
    void SetChart(ChartMode m);

    // 点击左侧列表 → 返回是否命中
    bool OnClick(int x, int y);
    bool HitTestChartToggle(int x, int y) const;
    void OnHover(int x, int y);
    // 当前悬停的侧栏卡片下标（items_ 的索引），-1 = 无。
    int HoverItem() const { return hoverItem_; }
    // 清空悬停态。鼠标移出窗口时由 TmWindow::ClearAllHover 调用 ——
    // 没有这一步时 OnHover 不再被调用，hoverItem_ 会永久保持最后的值，
    // 卡片一直亮着悬停色（项目历史上的「hover 泄漏」）。
    void ClearHover() { hoverItem_ = -1; }

    // ---- 侧栏滚动 ----
    // 性能页侧栏没有 ScrollViewer 的滚动通路（TmWindow 对性能页返回
    // ScrollTarget::None），而原版 DUI 是 asneeded 滚动的。分盘后 8 张磁盘卡
    // 加网卡 + GPU 会超出视口，旧代码 `if (item.bottom > bounds_.bottom) break;`
    // 让超出的卡**静默消失**，用户既看不到也滚不到。
    //
    // 返回 true 表示这次滚动被消费（侧栏确实需要滚）。
    bool ScrollSidebar(int deltaPx);
    bool SidebarNeedsScroll() const;
    void ResetSidebarScroll() { sidebarScroll_ = 0; }

    void Paint(HDC hdc) const;

    // 自监控
    int LastPaintMicros() const { return lastPaintMicros_; }

private:
    void PaintSidebar(HDC hdc) const;
    void PaintContent(HDC hdc) const;
    void PaintCpuPage(HDC hdc, const RECT& rc) const;
    void PaintMemPage(HDC hdc, const RECT& rc) const;
    void PaintDiskPage(HDC hdc, const RECT& rc) const;
    void PaintNetPage(HDC hdc, const RECT& rc) const;
    void PaintGpuPage(HDC hdc, const RECT& rc) const;

    // 把 selInst_[view_] 夹到 [0, count) 里。count 为 0 时返回 -1。
    // 【边界】侧栏项是按上一轮快照生成的，快照更新后实例数可能变少，
    //   所以每次绘制都要重新夹一次，不能让旧下标越界。
    int ClampInstance(int count) const;
    // 视图被外部切换（菜单「查看▸」/ --view= 启动参数）后，把侧栏高亮同步到
    // 该视图的第一张卡片，否则会出现「高亮在 CPU 卡上、正文却是磁盘页」。
    void SyncSelectionToView();
    // CPU 页图表上方那张模式按钮的当前文字（逻辑处理器 / NUMA 节点 / 总体利用率）。
    // 【P0-6】按钮是切换入口，文字必须跟着 chart_ 走，否则用户切到「NUMA 节点」
    //   后按钮仍写「总体利用率」，等于没有反馈。
    const wchar_t* CpuChartModeName() const;

    // 主图表。maxLabel/minLabel 是纵轴上下两端的刻度文字。
    // 【资源佐证】原版每张图都有独立的「上限标签」和「底部 0」元素：
    //   tasksmgr UIFILE 30001 里 primaryCpuChart 的 title = resstr(33836)="100%"，
    //   primaryCpuChart_bottom → RateLabelCpu = resstr(33824)="0"；
    //   磁盘页 diskTimeTitle = resstr(34462)="100%"、diskTime_bottom → RateLabelDiskTime = "0"。
    // 也就是说两端文字都在**图表矩形之外**：上限在图表上方那一行（右对齐），
    // 0 在图表下方那一行（左对齐）。之前本函数把 "100%"/"0%" 硬写在图表内部左上/左下角，
    // 且把 unit 参数直接 (void) 丢弃 —— 两个错都在这一个签名里修掉。
    // overlay != nullptr 时按同一时间轴与刻度再画一条内核时间曲线
    // （「查看 → 显示内核时间」）。传 nullptr 就是原来的单曲线行为。
    void PaintChart(HDC hdc, const RECT& rc, const double* data, int count, int head,
                    const wchar_t* maxLabel, const wchar_t* minLabel, COLORREF color,
                    const double* overlay = nullptr) const;
    void PaintLogicalCpuGrid(HDC hdc, const RECT& rc) const;
    // NUMA 节点视图（CPU 页 chart_ == ChartMode::Numa）。
    // 用 GetNumaProcessorNodeEx 把逻辑处理器分到 NUMA 节点，逐节点对
    // hist_.coreHist 求平均后画格子 —— 数据全部来自已有快照，不需要采集层改动。
    void PaintNumaCpuGrid(HDC hdc, const RECT& rc) const;
    // 一格「历史面积图」小卡：左上角标签 + 60 点曲线 + 网格。
    // vals 是按时间正序（0 = 最旧）的 n 个 0..100 值。
    // 逻辑处理器格子和 NUMA 节点格子共用这段绘制。
    void PaintHistCell(HDC hdc, const RECT& cell, const double* vals, int n,
                       const wchar_t* label) const;
    // 单个 GPU 引擎的小面积图（GPU 页「多个引擎」视图）
    void PaintEngineChart(HDC hdc, const RECT& rc, int engineType, const wchar_t* label,
                          int inst = -1) const;
    void PaintStatsRows(HDC hdc, const RECT& rc) const;
    void PaintStatsRowsGeneric(HDC hdc, const RECT& rc) const;
    void PaintTitleBlock(HDC hdc, const RECT& rc, const wchar_t* title,
                         const wchar_t* sub, const wchar_t* rightInfo) const;

    void DrawText2(HDC hdc, const wchar_t* s, RECT* rc, UINT fmt, COLORREF c) const;
    void Fill(HDC hdc, const RECT& rc, COLORREF c) const;
    void HLine(HDC hdc, int x1, int x2, int y, COLORREF c) const;
    void VLine(HDC hdc, int x, int y1, int y2, COLORREF c) const;

    // 左侧列表项（动态生成：CPU/内存/磁盘N/网络/GPUN）
    //
    // 【UIFILE 声明】侧栏结构（docs/evidence/exec/_S_geo2_30001.tsv / S_duispec.md §3.2）：
    //   performancetab_graphs
    //   └ TmScrollViewer   padding = rect(3rp,7rp,0rp,0rp)
    //     └ dashSidebar (sheet=drop)
    //       └ dashSidebarEntries (BorderLayout)
    //         ├ dashSidebar{Cpu,Memory,Disk,Network,Gpu}Entry
    //         │ └ dashSidebarEntry  padding = rect(0,0,3rp,0)
    //         │   └ ...Button
    //         │     ├ ...Chart   sheet=chart  ← 60rp × 45rp，margin rect(20,10,10,10)
    //         │     ├ ...Picture sgraphic(31213..31217, 2)   ← 类别图标贴图
    //         │     ├ sidebarLabel      gtf(TASKMANAGER,16,0) = 16px
    //         │     ├ sidebar_*_util    class=sidebarData (17 → 12px)
    //         │     └ sidebar_*_speed   class=sidebarData, padding rect(5rp,0,0,0)
    //         └ dashSidebarDivider  padding rect(0,7,0,0)，两段各 45rp × 8rp
    //
    // **UIFILE 没有声明卡片高度**：条目高度由「迷你图 45rp + 若干行文字」撑出，
    // 具体像素值 UIFILE 不管，只能取运行时结果。旧注释里「约 100px 高 / 步进 122px /
    // 右边界 x=212（864px 宽截图）」全部反解自已证伪的 truth1.png，已删除。
    struct SidebarItem {
        std::wstring name;      // 第 1 行：CPU / 内存 / 磁盘 0 (D: E: C:) / 以太网
        std::wstring sub;       // 第 2 行：2% 5.53 GHz / SSD / Tailscale
        std::wstring detail;    // 第 3 行：读取: 0 KB/秒 写入: 2.0C... / 发送: 0 Kbps
        PerfView view;
        int index;              // 磁盘/网络/GPU 的序号（-1 = 无实例的占位项）
        COLORREF chartColor;    // 该卡片迷你图表的曲线颜色（每类不同）
    };
    void RebuildSidebarItems();
    // 保证 items_ 是当前快照对应的那一份。
    //
    // 【为什么要有这个入口】原来 items_ 只在 PaintSidebar 里惰性重建：
    //   `if (itemsDirty_) { RebuildSidebarItems(); itemsDirty_ = false; }`
    // 而 OnHover / OnClick 直接遍历 items_ 做命中测试。于是**第一帧绘制之前**
    // 鼠标移到侧栏上时，items_ 还是空的 —— 悬停不亮、点击选不中。
    // 实际交互里这条窗口很窄（鼠标进入窗口通常已经画过一帧），
    // 但它同时也让「自测用 FakeMouseMove 直接投消息」这类路径完全失效，
    // 且顺序依赖本身就是不该有的隐式契约。三处调用点统一走这里。
    void EnsureSidebarItems();
    // 卡片左侧的迷你历史图表
    void PaintSidebarMiniChart(HDC hdc, const RECT& rc, const SidebarItem& it) const;

    RECT bounds_{};
    // ---- 画笔 / 画刷缓存 ----
    //
    // 【为什么需要】本页每帧重绘会做大量单像素直线和纯色填充：
    //   - PaintChart 的网格：横向 y 每 31px 一条、纵向 x 每 50px 一条，
    //     1100×400 的图表 ≈ 13 + 21 = 34 条线
    //   - PaintLogicalCpuGrid：20 核，每格一张迷你图（面积填充 + 折线 + 边框）
    //   - PaintSidebar 的 5 张卡片，每张都有面积填充 + 折线
    // 每个 HLine/VLine 原本都是一次 CreatePen + DeleteObject，每次 Fill 都是
    // 一次 CreateSolidBrush + DeleteObject。600 进程的机器上这些调用在
    // 「越用越卡」里占很大比重 —— 不是泄漏，是 GDI 系统调用的累积开销。
    //
    // 【为什么缓存是安全的】本页用到的颜色全部来自固定常量：
    //   kGridCpu/Mem/Disk/Net/Gpu（4 个页面主色）、kChartFill、各 kChartLine*、
    //   侧栏的 5 个 chartColor。**没有一个是按数据动态插值的**
    //   （热力图那种 9 档色只在进程页列表用，不走这两个辅助函数）。
    // 所以缓存表的大小上界是「常量色个数」，不会随运行时间增长。
    // 返回的 GDI 对象归 PerfPage 所有，在析构函数里统一删除 —— 调用方
    // **不要** DeleteObject 它们。
    HPEN CachedPen(COLORREF c) const;
    HBRUSH CachedBrush(COLORREF c) const;
    // 缓存表。mutable 因为绘制函数都是 const。
    mutable std::unordered_map<COLORREF, HPEN> penCache_;
    mutable std::unordered_map<COLORREF, HBRUSH> brushCache_;

    std::shared_ptr<const PerfSnapshot> snap_;
    PerfHistory hist_;
    PerfView view_ = PerfView::Cpu;
    // CPU 页默认视图 = **逻辑处理器网格**。
    // 【真原版截图实测】docs/reference/perf_cpu_orig.png：
    //   原版打开 CPU 页看到的是 4×4 = 16 格的逐核网格（该机 8 核 16 线程），
    //   顶部小标题是「60 秒内的利用率 %」，右上角**没有**任何切换按钮。
    //   我方此前默认 Overall（一张"总体利用率"大图），与原版完全不同。
    // 【为什么不是 Overall】原版只有用户主动从右键菜单选「总体利用率」才切过去；
    //   默认就是逐核网格。改动后 `CpuChartModeName()` 会返回「逻辑处理器」。
    ChartMode chart_ = ChartMode::Logical;
    int hoverItem_ = -1;

    // 侧栏滚动偏移（像素）。0 = 顶部。
    mutable int sidebarScroll_ = 0;

    // 侧栏内容的垂直布局：返回每一项的 top 与高度。
    // PaintSidebar / OnClick / OnHover 必须用同一份布局，否则加滚动后
    // 「点到的卡」和「画出的卡」会错位。
    void SidebarLayout(std::vector<int>& tops, std::vector<int>& heights) const;
    int  SidebarContentHeight() const;
    int  SidebarViewportHeight() const;
    // 侧栏里**被选中卡片的下标**（items_ 的下标），只用于高亮。
    mutable int selectedItem_ = 0;
    // 被选中卡片对应的**实例下标**（snap_->disks/nets/gpus 的下标）。
    // 【P0-7】这是页面绘制真正要读的东西 —— 以前只有 selectedItem_ 被读，
    // 绘制侧永远用 [0]，于是「点磁盘 2，显示磁盘 0」。
    // 按视图分开存：用户点「磁盘 2」再点「网络」再点回「磁盘」应该还是磁盘 2，
    // 共用一个变量时会被网络页的选中项覆盖掉。
    // -1 表示"该视图还没有实例"，绘制回退到 0。
    mutable int selInst_[static_cast<int>(PerfView::Count)] =
        { -1, -1, -1, -1, -1 };
    mutable std::vector<SidebarItem> items_;
    mutable bool itemsDirty_ = true;
    mutable int lastPaintMicros_ = 0;

    HFONT fontTitle_ = nullptr;
    HFONT fontSub_ = nullptr;
    HFONT fontBody_ = nullptr;
    HFONT fontBold_ = nullptr;
    HFONT fontBig_ = nullptr;
    HFONT fontSmall_ = nullptr;
    HFONT fontTiny_ = nullptr;
    HFONT fontBody2_ = nullptr;
    HFONT fontValue_ = nullptr;
    HFONT fontStat_ = nullptr;   // 统计区数值，21px
    HFONT fontSide1_ = nullptr;  // 侧栏 L1 名称 16px
    HFONT fontSide2_ = nullptr;  // 侧栏 L2 数值 12px
    HFONT fontSide3_ = nullptr;  // 侧栏 L3 明细 11px

public:
    void BuildFonts();
    ~PerfPage();
};

} // namespace tmp
