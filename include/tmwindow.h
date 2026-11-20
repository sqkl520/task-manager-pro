// tmwindow.h - 主窗口：完整仿照 Windows 任务管理器布局
//
// 布局（严格对应 Win10 任务管理器）：
//   ┌─────────────────────────────────────────────────────┐
//   │ 菜单栏  文件(F) 选项(O) 查看(V)                      │
//   ├──────┬──────────────────────────────────────────────┤
//   │ 导航 │ 页面标题                                      │
//   │ 进程 │ ┌────┬────┬────┬────┬────┐  汇总卡片        │
//   │ 性能 │ │CPU │内存│磁盘│网络│GPU │                  │
//   │ 应用 │ └────┴────┴────┴────┴────┘                  │
//   │ 启动 │ 名称  PID  状态  内存  磁盘  网络  GPU       │ 列头
//   │ 用户 │ ▸ 应用 (12)                                  │ 分组
//   │ 详细 │   ▣ Chrome  1234       120 MB               │ 行
//   │ 服务 │ ▸ 后台进程 (85)                              │
//   ├──────┴──────────────────────────────────────────────┤
//   │ 进程数 480   CPU 12%   内存 8.2/31.9 GB (26%)      │ 状态栏
//   └─────────────────────────────────────────────────────┘
#pragma once

#include "snapshot.h"
#include "classifier.h"
#include "theme.h"
#include "iconcache.h"
#include "tabs.h"
#include "menus.h"
#include "perfpage.h"
#include "pages_view.h"

#include <windows.h>
// NOTIFYICONDATAW / Shell_NotifyIcon 需要 shellapi.h（托盘图标用）
#include <shellapi.h>
#include <vector>

namespace tmp {

#include <string>
#include <memory>
#include <unordered_map>

// 托盘图标回调消息（「最小化时隐藏」用）
constexpr UINT WM_TRAYICON = WM_APP + 1;


// 进程显示名：去掉 .exe 后缀。列表显示和名称排序共用。
std::wstring DisplayName(const std::wstring& imageName);


class TmWindow {
public:
    struct ChartHistory {
        static constexpr int kPoints = 60;
        double cpu[kPoints] = {};
        double mem[kPoints] = {};
        double disk[kPoints] = {};
        double net[kPoints] = {};
        double gpu[kPoints] = {};
        int head = 0;
        int count = 0;
        void Push(double c, double m, double d, double n, double g);
    };

    TmWindow();
    ~TmWindow();

    bool Create(HINSTANCE inst, int nCmdShow);
    HWND Handle() const { return hwnd_; }
    // 加速键表（原版 ACCEL #26100：Delete/Esc/F5）。消息循环用它做翻译。
    HACCEL Accel() const { return accel_; }

    // 由 main 每秒推送新快照（UI 线程调用）
    void SetSnapshot(std::shared_ptr<const Snapshot> snap);
    void SetPerfSnapshot(std::shared_ptr<const PerfSnapshot> snap);

    // 进程页顶部「磁盘 / 网络 / GPU」三张汇总卡片的真实百分比。
    // 一份口径同时供两处消费（迷你曲线 hist_.Push，以及卡片主数值文字
    // PaintCards —— 后者在 src/tmwindow_paint.cpp，不归本模块，见交接单），
    // 避免两套公式算出两个数（本项目已经出现过"数字用聚合值、热力底色用
    // 原始值"的自相矛盾，见 docs/specs/spec_redteam2.md 的 K 条）。
    // 多实例口径、量程与单位换算的唯一说明在 tmwindow.cpp 的实现注释里。
    struct CardMetrics {
        double diskPct = 0.0;   // 磁盘活动时间 %     → 卡片"7%"
        double netPct  = 0.0;   // 网络链路利用率 %   → 卡片"0%"
        double gpuPct  = 0.0;   // GPU 使用率 %       → 卡片"3%"
    };
    CardMetrics SampleCardMetrics() const;

    // 切换到指定导航页（0=进程 1=性能 2=应用历史 3=启动 4=用户 5=详细信息 6=服务）
    void ShowPage(int index);
    // 性能页的子视图切换（0=总体利用率 1=逻辑处理器）
    void SetPerfChart(int mode);
    // 按名称选性能页左侧视图（cpu/memory/disk/network/gpu）
    void SetPerfViewByName(const wchar_t* name);
    // 指定进程页的排序列（0=名称 2=CPU 3=内存 …），主要用于脚本化验证
    void SetSortColumn(int col);
    // 切换「按类型分组」。关闭后进程页变成平铺列表，排序在全局生效。
    void SetGroupByType(bool on);
    // 只把 scrollY_ 钳制到 [0, totalH - viewportH]，**不重建行表**。
    //
    // 【为什么需要这个】滚动（滚轮、滚动条拖动、PgUp/PgDn、Home/End）只改
    // scrollY_，行表内容一点不变。但原实现每次都调 RebuildRows() —— 那是
    // RebuildAppAggregates() + 多次 std::sort + 全进程遍历，600 进程下
    // 每次几十万次操作。拖一次滚动条 = 每个 WM_MOUSEMOVE 一次 = 数百次全量重建，
    // 这正是「越用越卡」的直接来源之一。滚动路径只需要末尾那一步钳制。
    void ClampScroll();
    // 强制把后备缓冲重建为指定尺寸。仅供 --gdileak 回归测试使用：
    // 反复调用可精确复现「WM_SIZE 触发 EnsureBackBuffer 重建」的路径。
    void ForceBackBufferSize(int w, int h);
    // 切换底部「简略信息/详细信息」视图。与 OnMenuCommand(CMD_VIEW_SIMPLE) 同一条路径，
    // 只是给脚本化验证一个入口（--simple）。
    void ToggleSimpleView();
    // 展开所有应用行的子进程（等价于「查看 → 全部展开」）。
    // 供 --expandall 验证「子行高 22」的绘制接线。
    void ExpandAllApps();
    // 模拟点击第 col 个列头（走 HitTestColumn + 排序的完整路径）
    void SimulateHeaderClick(int col);
    // 设置某列宽度（供命令行/脚本化验证用）；persist=false 时不写注册表
    void SetColumnWidth(int col, int w, bool persist = true);

    // 自监控
    int LastPaintMicros() const { return lastPaintMicros_; }
    uint64_t PaintCount() const { return paintCount_; }
    // 把「最近一次 PaintAll 的耗时」和累计次数写进文件。
    // 供 --paintstats 模式在跑一段时间后落盘（不引入额外依赖，
    // 也不改绘制路径本身）。
    void DumpPaintStats(const wchar_t* path) const;
    int VisibleRows() const { return lastVisibleRows_; }
    int RowCount() const { return static_cast<int>(rows_.size()); }

    // 离屏渲染并保存为 BMP 文件。
    // 全程不显示窗口、不抢焦点、不动鼠标 —— 窗口以 SW_HIDE 创建，
    // 这里直接把内容画到内存位图，不经过屏幕。
    bool SaveOffscreen(const wchar_t* path);

private:
    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT, WPARAM, LPARAM);

    void BuildFonts();
    void BuildLayout();
    void RebuildRows();

    // 绘制分区
    // 把整个窗口画到指定 DC（自截图模式用）
    void PaintAll(HDC hdc, const RECT& client);

    void PaintMenuBar(HDC hdc);
    void PaintTabBar(HDC hdc);
    void PaintPageHeader(HDC hdc);
    void PaintCards(HDC hdc);
    void PaintColumnHeader(HDC hdc);
    void PaintRows(HDC hdc);
    void PaintStatusBar(HDC hdc);
    void PaintDivider(HDC hdc);
    void PaintDetailsPage(HDC hdc);
    void RebuildDetailRows();

    // 详细信息页的状态
    std::vector<const ProcessEntry*> detailRows_;
    int detailScrollY_ = 0;

    // 详细信息页**独立的选中状态**。
    //
    // 【安全修复】之前详细信息页复用了进程页的 selectedRow_ 去索引 detailRows_，
    // 那是两个完全不同的容器（进程页是分组扁平化后的 rows_，详细信息页是
    // 排序后的全量指针数组）。结果：
    //   - 在进程页选第 5 行（比如 Claude），切到详细信息页，
    //     selectedRow_ 还是 5，指向 detailRows_[5] —— 完全不同的进程
    //   - 点「结束任务」就会杀掉错的进程
    //   - 而且详细信息页从来不画选中高亮，用户根本看不出选的是谁
    //
    // 现在按 pid 独立跟踪，和进程页互不干扰。
    uint32_t detailSelectedPid_ = 0;
    int detailSelectedRow_ = -1;
    void SyncDetailSelection();

    // 绘制辅助
    void DrawTextClipped(HDC hdc, const wchar_t* s, RECT* rc, UINT fmt, COLORREF color);
    void FillSolid(HDC hdc, const RECT& rc, COLORREF c);
    void DrawHLine(HDC hdc, int x1, int x2, int y, COLORREF c);
    void DrawVLine(HDC hdc, int x, int y1, int y2, COLORREF c);
    void DrawFrame(HDC hdc, const RECT& rc, COLORREF c);

    // ---- 画笔 / 画刷缓存 ----
    //
    // 【为什么需要】FillSolid / DrawHLine / DrawVLine 在每帧绘制里被调用几十次
    // （详细信息页 7 处、工具栏 3 处、卡片区 2 处…），原实现每次都是
    // CreateSolidBrush/CreatePen + DeleteObject。GDI 对象创建有系统级成本，
    // 这是恒定开销（不是泄漏），但 600 进程的机器上每秒几十帧累加起来可观。
    //
    // 【为什么缓存安全】本窗口用到的颜色全部是 theme.h 里的常量
    // （kBgWindow / kBgHeader / kColBtnFill / kDetSelBg …），数量有限。
    // **禁止**把按数据动态插值出来的颜色传进来 —— 那会让缓存无限增长。
    // 进程页列表的热力色不走这三个函数（它有 tmwindow_list.cpp 自己的
    // CachedBrush，且已验证色值来自 9 档固定色）。
    HPEN CachedPen(COLORREF c);
    HBRUSH CachedBrush(COLORREF c);
    std::unordered_map<COLORREF, HPEN> penCache_;
    std::unordered_map<COLORREF, HBRUSH> brushCache_;
    void ReleaseGdiCaches();
    void DrawMiniChart(HDC hdc, const RECT& rc, const double* data, int count, int head);
    void DrawAppIcon(HDC hdc, const RECT& rc, const std::wstring& exeName, uint32_t pid);

    void EnsureBackBuffer(int w, int h);
    void InvalidateAll();

    // 命中测试

    int HitTestGroupHeader(int y) const;
    int HitTestRow(int y) const;
    // 列头命中测试。x 是客户区坐标，y 用来判断是否落在列头带内。
    int HitTestColumn(int x, int y) const;
    // 名称列的实际宽度（列表区宽度减去固定列宽，有最小值）
    int CurrentNameColWidth() const;
    // 应用行的展开三角在客户区坐标下的横向区间。draw 和 hit-test 共用这一份几何，
    // 避免两处各算一遍导致点了没反应。
    void TreeTriangleX(int indentLevel, int& left, int& right) const;

    HWND hwnd_ = nullptr;
    HINSTANCE inst_ = nullptr;
    HMENU mainMenu_ = nullptr;
    HACCEL accel_ = nullptr;
    int updateIntervalMs_ = 1000;
    // 采集器指针。「查看 → 更新速度」要真的改变采集频率，
    // 就必须能触达 Collector；此前 updateIntervalMs_ 只写不读，
    // 四个档位完全是摆设。
    void* collector_ = nullptr;   // 实际类型 Collector*，避免头文件循环依赖
public:
    void AttachCollector(void* c) { collector_ = c; }
private:
    void RebuildMenu();
    // 把 updateIntervalMs_ 同步给采集器（0 = 已暂停）
    void ApplyInterval();
    void LoadPageData(TabId t);
    void OnMenuCommand(UINT id);

    // ---- 菜单命令的实现辅助 ----
    // 取当前选中的进程条目（进程页看 rows_，详细信息页看 detailRows_）。
    // 没有选中时返回 nullptr。
    const ProcessEntry* SelectedProcess() const;
    // 取选中进程的完整 exe 路径（来自 pathCache_，未解析则为空）
    std::wstring SelectedProcessPath() const;
    // 显示一个"操作失败"提示。原版用的是 MessageBox，这里一致。
    void ReportFailure(const wchar_t* action, DWORD err);
    // 结束单个进程；wholeTree 为 true 时递归结束子进程
    bool EndProcessByPid(uint32_t pid, bool wholeTree);
    // 枚举 pid 的所有子孙进程（含自身）
    void CollectProcessTree(uint32_t rootPid, std::vector<uint32_t>& out) const;
    // 重启进程：结束并重新启动原命令行
    void RestartProcess(uint32_t pid);
    // 取进程命令行（PEB 读取，失败返回空）
    std::wstring QueryCommandLine(uint32_t pid);
    // 设置优先级
    bool SetProcessPriority(uint32_t pid, DWORD priorityClass);
    // CPU 亲和性对话框
    void ShowAffinityDialog(uint32_t pid);
    // 属性对话框
    void ShowPropertiesDialog(uint32_t pid);
    // "运行新任务"输入框。返回 false 表示用户取消。
    // 「以系统管理权限创建此任务」复选框的状态由 tmwindow_menus.cpp 内的
    // 文件级状态回传（不改这个头文件）。
    bool PromptForCommand(wchar_t* buf, size_t cch);
    // 创建进程转储文件（MiniDumpWriteDump）
    void CreateDumpFile(uint32_t pid, const std::wstring& imageName);
    // 把选中行复制到剪贴板（制表符分隔，与原版一致）
    void CopySelectedRowToClipboard();
    // 分析等待链（wct.h 的 OpenThreadWaitChainSession / GetThreadWaitChain）
    void AnalyzeWaitChain(uint32_t pid, const std::wstring& imageName);

    // ---- 视图选项状态 ----
    int  resourceValueMode_ = 0;        // 0=百分比 1=值
    bool alwaysOnTop_ = false;
    bool hideWhenMinimized_ = false;
    bool showFullName_ = false;
    bool groupByType_ = true;
    // 「按类型分组」的用户意愿（菜单勾选态就是这个成员）。
    // 与 groupByType_ 分离的原因见下方 ApplyGroupingForSort()：
    // 原版只要不是按名称列排序，进程页就**不显示组头**（扁平列表）；
    // 但「查看 → 按类型分组」的勾选态保持不变，回到名称排序时组头会回来。
    //
    // 【为什么不能直接用 groupByType_ 一并改】
    // 如果排序时把 groupByType_ 置 false，用户点回名称列时组头不会自动恢复，
    // 菜单勾选也被悄悄清掉了 —— 那是"改用户设置"，不是"随排序切换视图"。
    bool minimizeOnUse_ = false;
    // 「使用时最小化」的动作重入标志。ShowWindow(SW_MINIMIZE)/SW_HIDE 会在
    // WM_ACTIVATE 的处理过程中同步回流一次 WM_ACTIVATE，不挡住就会自己套自己。
    // 只在 tmwindow_events.cpp 的 WM_ACTIVATE 分支里 true→false 成对使用。
    bool minimizeOnUseBusy_ = false;
    bool showAllHistory_ = false;       // 显示所有进程的历史记录
    bool simpleView_ = false;   // 底部"简略信息"切换：折叠摘要卡片区
    // 应用行的展开状态：pid -> 是否展开
    //
    // 【不要用 operator[] 读它】`appExpanded_[pid]` 在键不存在时会**插入**一个
    // 默认值 false。而 RebuildRows() 每秒对快照里每个 pid 都要读一次展开状态，
    // 结果就是每秒往这张表里塞 600 个条目且永不清理 —— 一个纯粹由
    // 「读」造成的无界增长。用户机器 600+ 进程跑一小时 ≈ 200 万次插入
    // （pid 复用会撞掉一部分，但新 pid 一直进来）。
    //
    // 只用下面两个访问器：读走 IsAppExpanded()（不插入），写走 SetAppExpanded()。
    std::unordered_map<uint32_t, bool> appExpanded_;
    bool IsAppExpanded(uint32_t pid) const {
        auto it = appExpanded_.find(pid);
        return it != appExpanded_.end() && it->second;
    }
    void SetAppExpanded(uint32_t pid, bool on) { appExpanded_[pid] = on; }
    // 清掉已退出进程的条目，由 RebuildRows 每轮调用（O(n) 一次，可忽略）。
    void PruneAppExpanded();

    // ---- 托盘图标（「最小化时隐藏」需要它，否则窗口一隐就没法叫回来了）----
    NOTIFYICONDATAW nid_{};
    bool trayAdded_ = false;
    void AddTrayIcon();
    void RemoveTrayIcon();
    void RestoreFromTray();

    // 后备缓冲
    HDC memDC_ = nullptr;
    HBITMAP memBmp_ = nullptr;
    // CreateCompatibleDC 时 DC 默认选中的那张 1×1 stock 位图。
    // 换出 memBmp_ 时用它（这是唯一不需要新建临时位图就能把位图顶出 DC 的办法）。
    // 它属于 DC，不能删，随 DeleteDC 一起走。
    HGDIOBJ memOldBmp_ = nullptr;
    int memW_ = 0, memH_ = 0;

    // 字体
    HFONT fontBody_ = nullptr;
    HFONT fontBold_ = nullptr;
    HFONT fontNav_ = nullptr;
    HFONT fontCardTitle_ = nullptr;
    HFONT fontCardValue_ = nullptr;
    HFONT fontCardSub_ = nullptr;
    HFONT fontGroup_ = nullptr;
    HFONT fontPageTitle_ = nullptr;

    // 数据
    std::shared_ptr<const Snapshot> snap_;
    // 最近一份性能快照。进程页顶部 5 张汇总卡片里，磁盘/网络/GPU 三路的
    // 真值只存在于 PerfSnapshot（disks/nets/gpus），而 Snapshot 里没有任何
    // 整机磁盘/网络/GPU 汇总字段 —— 所以 hist_ 推曲线时必须借道这里。
    // 写入：SetPerfSnapshot（tmwindow.cpp）。读取：SetSnapshot 里的
    // hist_.Push（唯一读者）。两份快照来自两个独立采集器、以同频 1Hz 各自
    // 到达，取"最近到达的那份"即可，最多陈旧一拍，对 60 秒曲线无影响。
    std::shared_ptr<const PerfSnapshot> perfSnap_;
    Classification cls_;
    ChartHistory hist_;
    uint64_t uiSeq_ = 0;

    // 布局
    RECT rcMenu_{};
    RECT rcTabs_{};
    RECT rcContent_{};
    RECT rcCards_{};
    RECT rcColHeader_{};
    RECT rcList_{};
    RECT rcStatus_{};
    // 底部工具条（简要信息 / 打开资源监视器 / 打开服务 / 断开连接…）
    // 【实测】Win10 在每个页面底部都有一条工具条，不是只有状态栏。
    RECT rcToolbar_{};

    // 底部工具条按钮的命中区（随页面变化，所以每帧算）
    struct ToolbarButton {
        RECT rc;
        std::wstring text;
        UINT cmd;
    };
    mutable std::vector<ToolbarButton> toolbarButtons_;
    void BuildToolbarButtons() const;
    void PaintToolbar(HDC hdc);
    UINT HitTestToolbar(int x, int y);

    // ---- 滚动条 ----
    // 【缺陷修复】之前列表超出可见区时没有任何滚动条，也看不出还能滚。
    void PaintScrollBar(HDC hdc);
    // 滚动条滑块矩形（用于命中测试和绘制）
    RECT ScrollThumbRect() const;
    // 内容总高度与可视高度
    int ContentHeight() const;
    int ViewportHeight() const;
    // 拖拽状态
    bool scrollDragging_ = false;
    int  scrollDragOffset_ = 0;



    // 导航
    TabBar tabs_;
    PerfPage perfPage_;
    PagesView pagesView_;


    // 列表视图：分组扁平化后的行
    struct ViewRow {
        enum Kind { GroupHeaderRow, ProcessRow } kind;
        ProcessGroup group;        // 组头用
        int groupCount;            // 组头用
        uint32_t pid;              // 进程行用
        uint32_t rootPid;          // 归属的应用根（用于图标/缩进）
        int indentLevel;
        bool hasChildren;          // 有无子进程（展开箭头）
        bool expanded;
    };
    std::vector<ViewRow> rows_;
    std::vector<bool> groupExpanded_{ true, true, true };
    int scrollY_ = 0;
    int selectedRow_ = -1;

    // 选中项跟随 pid，而不是行下标。
    //
    // 【安全修复】之前 selectedRow_ 只是个行下标。列表每秒重建时，
    // 如果选中的进程退出、或者排序导致位置变化，这个下标会指向**另一个进程** ——
    // 用户点「结束任务」就会杀掉错的进程。这是真实的数据丢失风险。
    //
    // 现在记住被选中进程的 pid，每次 RebuildRows 后按 pid 重新定位行号；
    // 进程已消失则清除选中。
    uint32_t selectedPid_ = 0;
    // 根据 selectedPid_ 重新定位 selectedRow_（RebuildRows 末尾调用）
    void SyncSelectionToPid();
    int hoverRow_ = -1;

    // 应用行的聚合值：pid(应用根) -> {cpu, 内存, 进程数}
    //
    // 【关键语义】原版在**折叠**状态下，应用行显示的是整个应用
    // （含所有子进程）的**合计**，不是根进程自己的值。
    // 实测 Claude：根进程 477MB，含全部子进程约 3075MB —— 差 6 倍。
    // 之前我只显示根进程的值，这是进程页最明显的逻辑错误之一。
    //
    // 展开后，子行显示各自的值，根行仍显示合计（和原版一致）。
    struct AppAggregate {
        double cpuPercent = 0.0;
        uint64_t memBytes = 0;
        uint32_t processCount = 0;
        // 磁盘吞吐可以相加（字节/秒是可叠加的物理量）
        double diskBytesPerSec = 0.0;
        // 网络速率也可以相加（各进程的收发字节互不重叠）
        double netBitsPerSec = 0.0;
        // GPU 使用率**不能相加** —— 多个进程并发用同一引擎时，
        // 各自的 utilization 是时间片占比，相加会超过 100%。
        // 这里取各子进程里的**最大值**（与原版观感一致：折叠时显示
        // "这个应用对 GPU 的峰值占用"）。
        double gpuPercentMax = 0.0;
    };
    std::unordered_map<uint32_t, AppAggregate> appAgg_;
    // pid -> entry 索引，RebuildRows 内部复用（避免嵌套 O(n^2)）
    std::unordered_map<uint32_t, const ProcessEntry*> byPid_;
    void RebuildAppAggregates();

    // 排序状态。sortCol_ 是列索引，必须和 tmwindow_list.cpp 的 kCols[] 一致：
    //   0=名称 1=状态 2=CPU 3=内存 4=磁盘 5=网络 6=GPU
    // （原版进程页没有「类型」列，它属于详细信息页）
    //
    // 【默认排序 = 名称升序，不是内存降序 —— 真原版截图实测】
    // docs/reference/original_win10.png 的进程页顺序：
    //   CC Switch → DeepSeek Harness → Microsoft Edge → Node.js → Visual Studio Code
    //   → Windows 命令处理程序 → Windows 资源管理器 → 任务管理器 → 迅雷 → 终端
    //   英文按字母序、中文按拼音序、中文排在英文之后 —— 这正是「名称升序」。
    //
    // 【此前的错误】默认是 sortCol_=3(内存) + sortDesc_=true(降序)，
    //   于是打开程序看到的是内存从大到小排，与原版第一眼就完全不同。
    //   这条错误还连带了另一个现象：用户点「名称」列时，若 sortDesc_ 仍是 true，
    //   名称会变成降序（Z→A、中文在前），看起来"排序逻辑完全不对"。
    static constexpr int kSortColCount = 7;
    int sortCol_ = 0;      // 名称
    bool sortDesc_ = false;  // 升序（A→Z、拼音 a→z）
    // 列头悬停/按下的列索引，-1 表示无。用于绘制排序箭头和按下反馈。
    int hoverCol_ = -1;
    int pressedCol_ = -1;

    // 列头右键弹出菜单时，光标所在的列索引（-1 = 不在任何列上）。
    //
    // 【为什么需要这个成员】TrackPopupMenu 的命令是无参的：菜单返回
    // CMD_HIDE_COLUMN / CMD_COL_TOGGLE_BASE+i 时，OnMenuCommand 拿不到鼠标位置
    // （菜单关闭后 GetCursorPos 已经不可靠，用户可能用键盘选择）。
    // 所以在 WM_RBUTTONUP 弹出菜单**之前**把列索引记在这里，命令处理时读它。
    // 只有 tmwindow_events.cpp（写）和 tmwindow_menus.cpp（读）碰它。
    int headerContextCol_ = -1;
    // 列标题（唯一真值源是 tmwindow_list.cpp 的 kCols[]）。
    // 列头右键菜单要用列名数组，不能让调用点自己再抄一份字符串 ——
    // 抄一份就会出现「菜单里叫磁盘、列头写着硬盘」这种两套真值。
    static int ColumnCount();
    static const wchar_t* ColumnTitle(int idx);

    // ---- 列宽（可拖拽 + 持久化） ----
    // 原版列头边界可拖动改宽，宽度存进
    // HKCU\Software\Microsoft\Windows\CurrentVersion\TaskManager\Preferences
    // （二进制里 56 字节一条记录，+20 偏移是列宽）。
    //
    // 这里不照搬那个二进制格式（它混了 4 个页面的记录、还夹着堆指针垃圾），
    // 而是用自己的一串 DWORD 存在同名键的独立值下，语义等价、互不干扰。
    // colWidths_ 只存**固定列**（不含名称列）。名称列自 2025 起是**固定宽度**
    // （原版池表 234px / 截图实测 236px 含边框，见 tmwindow_list.cpp 的
    // kNameColWidth —— 它不再弹性吃剩余宽度，所以这个数组的第 0 项恒为 0、
    // 且**不参与**任何宽度计算）。
    //
    // 【默认值 —— 两张独立证据给出同一组数】
    //   a) 原版 taskmgr.exe 的 `.data` 列池表（`docs/specs/O_strings.md` §2.1，
    //      表头 0x129e84）：名称 234 / 状态 120 / CPU 73 / 内存 73 /
    //      磁盘 73 / 网络 73 / GPU 73
    //   b) 真原版截图 `docs/reference/original_win10.png` 列间竖线
    //      在 x=236/356/429/502（y=100 采样 #DFDFDF）：
    //      状态 356−236 = 120、CPU 429−356 = 73、内存 502−429 = 73。
    // → 旧默认是 {0,120,73,73,82,82,82}（前 4 列对了，后 3 列的 82 是 8px 栅格
    //   自定值），本轮把后 3 列统一成池表的 73。
    //
    // ⚠ 这个数组会**覆盖** tmwindow_list.cpp 里 kCols[] 的 width
    //   （EffectiveColWidth：`colWidths_[col] > 0 ? colWidths_[col] : kCols[col].width`），
    //   两处必须同步改，否则改了 kCols 却没生效 —— 本项目踩过"改了以为是生效点"
    //   的坑，这里显式记一笔。
    int colWidths_[kSortColCount] = { 0, 120, 73, 73, 73, 73, 73 };

    // ---- 列可见性（隐藏列） ----
    //
    // 【为什么必须在头文件里加这个维度】
    // 此前只有 colWidths_（宽度），没有「是否显示」。列头右键菜单发得出
    // CMD_COL_TOGGLE_BASE + i，但接收侧既没有 case、也没有地方能存
    // 「这一列被隐藏了」这件事 —— 隐藏列在数据结构上就表达不出来。
    //
    // 索引与 colWidths_ / tmwindow_list.cpp 的 kCols[] 完全一致：
    //   0=名称 1=状态 2=CPU 3=内存 4=磁盘 5=网络 6=GPU
    //
    // 【名称列（0）永远可见】原版任务管理器不允许隐藏名称列
    // （隐藏了就没有任何一列能标识进程）。这里在 SetColumnVisible 里硬挡，
    // 不依赖调用方自觉。colVisible_[0] 恒为 true，连加载注册表时都会纠正。
    //
    // 【默认只显示 4 列 —— 两张证据同时指向这个状态】
    //   a) 真原版截图 `docs/reference/original_win10.png` 只有 名称/状态/CPU/内存
    //      四列可见，x=503..1232 的 730px 逐像素扫描非白像素 = 0（x=1233 起是滚动条）；
    //   b) 这 4 列的实测宽 236/120/73/73 与池表 234/120/73/73 逐项吻合，
    //      说明截图里那 4 列就是**默认布局**，不是用户手动选的。
    // → 磁盘 / 网络 / GPU 默认隐藏（用户可从列头右键菜单打开）。
    bool colVisible_[kSortColCount] = { true, true, true, true, false, false, false };

    void LoadColumnWidths();
    void SaveColumnWidths();
    // 排序状态（按哪列排 + 升降序）与组折叠状态的持久化。
    // 原版重启后保留这两项，我方此前一律回默认。
    void LoadSortState();
    void SaveSortState();
    // 可见性持久化。用**独立的值名**（TaskManagerPro_ColumnVisible，7 字节
    // REG_BINARY），不往 colWidths_ 那份二进制里塞字段 —— 老版本写下的
    // TaskManagerPro_ColumnWidths 长度是 7*sizeof(int)，改结构会让它读不出来。
    void LoadColumnVisibility();
    void SaveColumnVisibility();
    // 全部恢复可见（「选择列」命令在没有对话框时的落地动作，也是兜底）
    void ResetColumnVisibility();
    // 该列是否可见。索引越界返回 false；名称列恒 true。
    bool IsColumnVisible(int idx) const;
    // 设置可见性。名称列（idx==0）**永远拒绝隐藏**，返回 false 表示未改动。
    // 返回值 = 是否真的改变了状态。
    bool SetColumnVisible(int idx, bool v);
    // 当前生效的列宽（0 表示名称列）
    int EffectiveColWidth(int col) const;

    // 正在拖拽的列边界：colDragCol_ = 该列左侧的列索引，-1 表示没在拖
    int colDragCol_ = -1;
    int colDragStartX_ = 0;
    int colDragStartW_ = 0;
    // 命中列边界（用于改鼠标指针形状）。返回左邻列索引，-1 表示没命中。
    int HitTestColResize(int x, int y) const;

    // 热力图归一化最大值（全量，避免滚动抖动）
    double maxCpu_ = 1.0, maxMem_ = 1.0, maxDisk_ = 1.0, maxNet_ = 1.0, maxGpu_ = 1.0;
    // 物理内存总量，内存列热力图的量程参照
    uint64_t memTotalBytes_ = 0;

    // 自监控
    int lastPaintMicros_ = 0;
    int lastVisibleRows_ = 0;
    uint64_t paintCount_ = 0;

    // 图标缓存
    IconCache icons_;
    // pid -> 已解析的 exe 完整路径（解析一次就缓存，避免每帧重查）
    std::unordered_map<uint32_t, std::wstring> pathCache_;
    // pid -> 该进程的 createTime，用于检测 pid 复用。
    //
    // 【修复】Windows 的 pid 会被复用：进程退出后新进程可能拿到同一个 pid。
    // 只按 pid 缓存路径/描述/图标，会把旧进程的信息串到新进程上
    // （显示错误的图标、错误的文件描述，甚至「打开文件位置」跳到别人的 exe）。
    //
    // 用 (pid, createTime) 双键校验：createTime 变了就说明是新进程，缓存作废。
    std::unordered_map<uint32_t, LONGLONG> pathCreateTime_;

    // exe 路径 -> 文件描述（版本信息里的 FileDescription）。
    //
    // 【关键】原版任务管理器「名称」列显示的是**文件描述**，不是文件名：
    //   explorer.exe -> "Windows 资源管理器"
    //   dwm.exe      -> "桌面窗口管理器"
    //   Taskmgr.exe  -> "任务管理器"
    //   迅雷的 exe   -> "迅雷"
    // 这是列排序的依据，也是显示的内容。之前我显示的是 exe 文件名，全错了。
    //
    // 按路径缓存（不是按 pid）：同一 exe 的多个实例共用一条。
    mutable std::unordered_map<std::wstring, std::wstring> descCache_;
    // descCache_ 的写入代数。每写入一条新描述就 +1。
    //
    // 【为什么需要】名称列的**显示**走 ProcessDisplayName（实时查 descCache_），
    // 而**排序**用 sortNameCache_（在 RebuildRows 时快照一次）。
    // 如果预取在两次 RebuildRows 之间把某条路径的描述取回来了，
    // 显示会立刻变，排序键还是旧的 exe 文件名 —— 两者不一致。
    // RebuildRows 比对这个代数，发现变了就重建排序键表并重排。
    mutable uint64_t descCacheEpoch_ = 0;
    uint64_t sortNameEpoch_ = 0;   // 上次建 sortNameCache_ 时的 descCacheEpoch_
    // 取某个 exe 的文件描述；取不到时返回空
    std::wstring GetFileDescription(const std::wstring& dosPath) const;
    // 进程的显示名：svchost 特殊处理，其余用文件描述
    std::wstring ProcessDisplayName(uint32_t pid, const std::wstring& imageName);
    // 排序用的名称快照：pid → 显示名。只在按名称列排序时预建（RebuildRows 开头），
    // 让比较器不必每次调 ProcessDisplayName（那会为每次比较分配两个 std::wstring，
    // N log N 次比较 = 2 万多次堆分配）。
    std::unordered_map<uint32_t, std::wstring> sortNameCache_;
    // descCache_ 低频修剪的轮次计数器（每 60 轮修剪一次，见 tmwindow_icons.cpp）。
    uint32_t descPruneTick_ = 0;
    // 预取文件描述的轮转状态（见 tmwindow_icons.cpp 的 kPrefetchPerTick 注释）。
    // pendingDescPaths_ 是待取描述的路径列表，descCursor_ 是本次扫到的位置。
    // descScanTick_ 控制多久重新扫一次待办（新进程会出现新路径）。
    std::vector<std::wstring> pendingDescPaths_;
    size_t descCursor_ = 0;
    uint32_t descScanTick_ = 0;

    // ---- svchost 命名 ----
    //
    // 【实测结论】原版**不拆组**，而是给 svchost 行改名字：
    //   有 `-s <服务名>` → 显示「服务主机: <该服务的 DisplayName>」
    //   无 -s（多服务宿主）→ 显示「服务主机: <组名>」
    // 进程仍然是 1 个 svchost = 1 行。
    //
    // 证据见 docs/specs/spec_svchost.md：MUI 里有一段连续的「服务主机: xxx」
    // 32 项组名表；用户截图那行「服务主机: Remote Desktop Se...」能与
    // pid=1668（-k NetworkService -s TermService）的 DisplayName 逐字对上。
    //
    // 拆组被否的原因：Windows 没有服务级 CPU/内存计量，
    // 拆开后每行的数值只能是假的或重复的。
    std::unordered_map<uint32_t, std::wstring> svcHostName_;   // pid -> 「服务主机: xxx」
    LARGE_INTEGER svcHostLastBuild_{};   // 上次建表时刻（节流用）
    void BuildSvcHostNames();
    void ResolvePathsAsync();

    // 用户名解析（SID → 名字 全局缓存，每个进程只查一次）
    //
    // 【两个必须同时解决的问题】
    //
    // 1. **无界增长**：`userCache_` 按 pid 存，每个出现过的 pid 插一条，
    //    全项目原本没有任何清理。而 ResolveUser 被详细信息页的**绘制循环**
    //    调用（tmwindow_details.cpp:316），停在那页时每帧对每个可见行都查一次，
    //    用户机器 600+ 进程长跑会持续累积。
    //
    // 2. **pid 复用导致显示错人**（正确性 bug，比增长更严重）：
    //    Windows 会复用 pid。进程 A 退出、pid 被进程 B 拿到后，
    //    `userCache_.find(pid)` 仍然命中 A 的用户名 —— 详细信息页的「用户名」
    //    列会显示一个**完全不相干的用户**。原实现没有任何失效判断。
    //
    // 修法与本项目已有的 pathCache_ 一致：存 (用户名, 进程创建时间) 两元组，
    // 命中时比对创建时间，不同就说明是新进程，丢弃重查。
    struct UserCacheEntry {
        std::wstring name;
        int64_t createTime = 0;   // 来自 Snapshot 的 ProcessEntry::createTime
    };
    std::unordered_map<uint32_t, UserCacheEntry> userCache_;
    // 清掉已退出进程的条目。由 RebuildRows 每轮调用（与 PruneAppExpanded 同处）。
    void PruneUserCache();
    // SID → 名字。这张表**不需要**清理：SID 是用户的稳定标识，
    // 同一台机器上活跃用户个数有限（通常个位数到几十），不会随运行时间增长。
    std::unordered_map<std::wstring, std::wstring> sidNameCache_;
    std::wstring ResolveUser(uint32_t pid);
};

} // namespace tmp
