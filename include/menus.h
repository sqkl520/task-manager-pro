// menus.h - 原生菜单构建
//
// 全部文案来自 taskmgr.exe.mui 的 MENU 资源直接提取（见 docs/evidence/muidlg.txt），
// 不是猜的：
//   MENU 30205 主菜单栏    MENU 30208 进程右键    MENU 30200 详细信息右键
//   MENU 30206 性能页右键  MENU 30202 服务右键    MENU 30211 用户右键
//   MENU 30213 启动右键    MENU 30214 应用历史右键 MENU 30201 列头右键
//   MENU 30209 窗口右键    MENU 30215 托盘右键    MENU 30217 启动进程右键
#pragma once

#include <windows.h>
#include <functional>

namespace tmp {

// 菜单命令 ID（从 40000 起，避开系统占用）
enum MenuCmd : UINT {
    // 文件
    CMD_NEW_TASK        = 40001,
    CMD_EXIT            = 40002,
    // 托盘右键（不在菜单栏里，由 WM_TRAYICON 使用）
    CMD_RESTORE_FROM_TRAY = 40005,
    CMD_CLOSE_APP         = 40006,
    // 选项
    CMD_ALWAYS_ON_TOP   = 40010,
    CMD_MINIMIZE_ON_USE = 40011,
    CMD_HIDE_WHEN_MIN   = 40012,
    CMD_SHOW_FULL_NAME  = 40013,
    CMD_SHOW_ALL_HISTORY= 40014,
    // 设置默认选项卡子项
    CMD_DEF_TAB_BASE    = 40020,   // +0..6
    // 查看
    CMD_REFRESH_NOW     = 40030,
    CMD_UPDATE_HIGH     = 40031,
    CMD_UPDATE_NORMAL   = 40032,
    CMD_UPDATE_LOW      = 40033,
    CMD_UPDATE_PAUSED   = 40034,
    CMD_GROUP_BY_TYPE   = 40035,
    CMD_EXPAND_ALL      = 40036,
    CMD_COLLAPSE_ALL    = 40037,
    // 进程右键
    CMD_EXPAND          = 40100,
    CMD_COLLAPSE        = 40101,
    CMD_SWITCH_TO       = 40102,
    CMD_RESTART         = 40103,
    CMD_END_TASK        = 40104,
    CMD_END_TREE        = 40105,
    CMD_PROVIDE_FEEDBACK= 40106,
    CMD_SET_PRIORITY    = 40107,
    CMD_SET_AFFINITY    = 40108,
    CMD_ANALYZE_CHAIN   = 40109,
    CMD_DEBUG           = 40110,
    CMD_UAC_VIRT        = 40111,
    CMD_CREATE_DUMP     = 40112,
    CMD_OPEN_LOCATION   = 40113,
    CMD_SEARCH_ONLINE   = 40114,
    CMD_PROPERTIES      = 40115,
    CMD_GOTO_SERVICES   = 40116,
    CMD_GOTO_DETAILS    = 40117,
    // 优先级子项
    CMD_PRIO_REALTIME   = 40120,
    CMD_PRIO_HIGH       = 40121,
    CMD_PRIO_ABOVE      = 40122,
    CMD_PRIO_NORMAL     = 40123,
    CMD_PRIO_BELOW      = 40124,
    CMD_PRIO_LOW        = 40125,
    // 列头/列右键（MENU 30201 静态版；真实列头右键是运行期动态生成的，见下）
    CMD_HIDE_COLUMN     = 40130,
    CMD_SELECT_COLUMNS  = 40131,
    // 列头右键的「列名清单」：每个可见列一条，ID = CMD_COL_TOGGLE_BASE + 列池索引
    // 依据 disasm 0x140076F9E 的 InsertMenuW 循环（遍历列名字符串数组逐项插入）。
    //
    // 区间必须独占连续 48 个 ID，不能与任何其它命令重叠 —— 曾经放在 40132，
    // 于是 +8..11 = 40140..40143 撞上 CMD_SVC_START/STOP/RESTART/OPEN_SCM、
    // +18..23 撞上 CMD_USR_*、+28..29 撞上 CMD_STARTUP_*、+38..46 撞上
    // CMD_CHART_* / CMD_COPY。点列头菜单里的一个列名会去启动服务或切性能页。
    // 现在挪到 40200..40247，独占且留足余量。
    CMD_COL_TOGGLE_BASE = 40200,   // +0..47（详细信息页 48 个可选列）
    CMD_COL_TOGGLE_END  = 40247,   // 最后一个，用于 static_assert 护栏    // 服务右键
    CMD_SVC_START       = 40140,
    CMD_SVC_STOP        = 40141,
    CMD_SVC_RESTART     = 40142,
    CMD_SVC_OPEN_SCM    = 40143,
    // 用户右键
    CMD_USR_CONNECT     = 40150,
    CMD_USR_DISCONNECT  = 40151,
    CMD_USR_LOGoff      = 40152,
    CMD_USR_SEND_MSG    = 40153,
    CMD_USR_SWITCH      = 40154,
    CMD_USR_MANAGE      = 40155,
    // 启动项右键
    CMD_STARTUP_ENABLE  = 40160,
    CMD_STARTUP_DISABLE = 40161,
    // 性能页
    CMD_CHART_OVERALL   = 40170,
    CMD_CHART_LOGICAL   = 40171,
    CMD_CHART_NUMA      = 40172,
    CMD_CHART_SINGLE_ENG= 40173,
    CMD_CHART_MULTI_ENG = 40174,
    CMD_SHOW_KERNEL_TIME= 40175,
    CMD_GRAPH_SUMMARY   = 40176,
    CMD_COPY            = 40177,
    CMD_OPEN_RESMON     = 40178,
    // 30206「查看(V)▸」子菜单五项（在图表区内直接切页）
    CMD_PERF_VIEW_CPU   = 40182,
    CMD_PERF_VIEW_MEM   = 40183,
    CMD_PERF_VIEW_DISK  = 40184,
    CMD_PERF_VIEW_NET   = 40185,
    CMD_PERF_VIEW_GPU   = 40186,
    // 30206「查看网络详细信息(&D)」
    CMD_NET_DETAILS     = 40187,
    // 30207 性能页侧栏右键三项
    CMD_SIDEBAR_SUMMARY    = 40188,
    CMD_SIDEBAR_SHOW_GRAPH = 40189,
    CMD_SIDEBAR_HIDE_GRAPH = 40191,
    // 资源值显示方式：三组（内存/磁盘/网络）× 两项（百分比/值）。
    // 依据 MENU 30208 / 30216 —— 数组原 ID 是 30482..30487，这里是命令 ID，
    // 与资源 ID 无关，只需一一对应。
    CMD_VAL_MEM_PCT     = 40180,
    CMD_VAL_MEM_VAL     = 40181,
    CMD_VAL_DISK_PCT    = 40192,
    CMD_VAL_DISK_VAL    = 40193,
    CMD_VAL_NET_PCT     = 40194,
    CMD_VAL_NET_VAL     = 40195,
    // 底部工具条
    CMD_VIEW_SIMPLE     = 40190,   // 简略信息(D)：折叠摘要视图
};

// 菜单构建器
class MenuBuilder {
public:
    // ---- 菜单层共享状态 ----
    // 「资源值」三组的单选状态。编码 group*2 + (0=百分比 1=值)：
    //   group 0=内存 1=磁盘 2=网络。默认 1（内存→值），依据
    //   原版实拍圆点在「值」上。
    static int& ResourceValueMode();
    // 「选项 → 显示完整账户名(&S)」的当前值。
    // 菜单项只在用户页可点，绘制侧的用户名列需要读到同一个值才能生效；
    // 以前它只存在 TmWindow::showFullName_ 里，用户页根本读不到。
    static bool& ShowFullNameOption();
    // 30206 / 30207 的两个图形开关。绘制侧（perfpage.cpp）接上之前，
    // 至少保证菜单勾选随点击变化，不是死勾。
    struct PerfMenuToggles {
        bool kernelTime = false;    // 30206 显示内核时间(&K)
        bool graphSummary = false;  // 30206 图形摘要视图(&W)
        bool sidebarSummary = false; // 30207 摘要视图(&W)
        bool hideSidebarGraph = false; // 30207 隐藏图形(&H)
        // 当前「将图形更改为」选中项（ChartMode 的序号，-1 = 未知）。
        // 调用点目前传字面量 0，导致「总体利用率」永远打点；有了这份状态，
        // 用户点过之后圆点就跟得上实际视图。
        int chartMode = -1;
    };
    static PerfMenuToggles& PerfToggles();
    // 「设置默认选项卡」的持久化页号（HKCU\Software\TaskManagerPro\StartUpTab，
    // 自己键没有就回落到系统 TaskManager 键的同名值）。
    static int  ReadDefaultTab(int fallback);
    static void WriteDefaultTab(int index);

    // 构建主菜单栏（文件/选项/查看）。返回 HMENU，调用方负责 DestroyMenu。
    // currentTab / updateSpeed 用于设置勾选态。
    // minimizeOnUse / showAllHistory 同样只影响勾选态，但要如实反映。
    static HMENU BuildMainMenu(int currentTab, int updateIntervalMs,
                               bool alwaysOnTop, bool hideWhenMin,
                               bool showFullName, bool groupByType,
                               bool minimizeOnUse = false,
                               bool showAllHistory = false);

    // 各页右键菜单
    // hasSelection=无选中时整份置灰；isGroupRow=组头行（只有展开/折叠可用）；
    // hasChildren/expanded 决定「展开」「折叠」哪一项亮。
    static HMENU BuildProcessContext(bool hasSelection, bool isGroupRow,
                                     bool hasChildren, bool expanded);
    // currentPriority 传 GetPriorityClass 的结果，未知传 0xFFFFFFFF。
    static HMENU BuildDetailsContext(bool hasSelection,
                                     DWORD currentPriority = 0xFFFFFFFFu);
    // 服务页右键有**两个变体**（原版 MENU 30202 与 30210）：
    //   hasPid=true  → 30202：开始/停止/重新启动 ─ 打开服务/在线搜索 ─ 转到详细信息
    //   hasPid=false → 30210：停止(&S)/打开服务(&O)/在线搜索(&S)，无分隔线
    // 注意两者的「停止」助记符不同（&T vs &S），「打开服务」也不同（&V vs &O）。
    // canStart/canStop 由 ServiceItem 的同名字段喂进来，用于逐项置灰
    // （原版实拍：服务运行中「开始(S)」是灰的）。
    static HMENU BuildServiceContext(bool hasSelection, bool hasPid = true,
                                     bool canStart = true, bool canStop = true);
    static HMENU BuildUserContext(bool hasSelection, bool expanded);
    static HMENU BuildStartupContext(bool hasSelection, bool enabled);
    // MENU 30217：启动页子进程行的右键（目前无调用点，实现保留）
    static HMENU BuildStartupProcessContext(bool hasSelection);
    static HMENU BuildAppHistoryContext(bool hasSelection);
    // MENU 30201（简化版：隐藏列/选择列）
    static HMENU BuildHeaderContext();
    // 真实的列头右键：运行期生成的列名清单 + 资源值子菜单。
    //   titles/visible/count —— 当前页可切换的列
    //   resValueMode —— ResourceValueMode() 的值（这里用磁盘(&I)）
    //   hoveredColumn —— 光标所在列，-1 表示不在某列上
    // 发出 CMD_COL_TOGGLE_BASE + 索引。
    static HMENU BuildColumnHeaderContext(const wchar_t* const* titles,
                                          const bool* visible, int count,
                                          int resValueMode,
                                          int hoveredColumn = -1);
    // MENU 30206：性能页图表区右键
    static HMENU BuildPerfContext(int currentView, bool kernelTime, bool summary);
    // MENU 30207：性能页左侧栏右键
    static HMENU BuildPerfSidebarContext(bool summaryView, bool hideGraph);
    // MENU 30204：Other Context（单项「结束进程」），触发场景未证实
    static HMENU BuildOtherContext(bool hasSelection);
    // MENU 30215：托盘图标右键（还原/关闭 ─ 置于顶层）
    static HMENU BuildTrayContext(bool alwaysOnTop);

    // 「资源值」子菜单（30208 与 30216 共用的零件）。
    // diskMnemonic：30208 是 L'K'，30216 是 L'I'。
    static HMENU BuildResourceValueSubmenu(int mode, wchar_t diskMnemonic);

    // 加速键表。返回 HACCEL，调用方负责 DestroyAcceleratorTable。
    static HACCEL BuildAccelerators();
};

// 护栏：CMD_COL_TOGGLE_BASE + 索引 的区间必须独占，且后面紧跟的 40248 起不能有别的命令。
// 这三条 static_assert 在编译期挡住「以后又往撞车区间里加命令」。
static_assert(MenuCmd::CMD_COL_TOGGLE_BASE == 40200,
              "CMD_COL_TOGGLE_BASE 被改动时请同步复核下面两条不重叠断言");
static_assert(MenuCmd::CMD_COL_TOGGLE_END == MenuCmd::CMD_COL_TOGGLE_BASE + 47,
              "CMD_COL_TOGGLE_END 必须是 BASE+47（48 个列池索引）");
static_assert(MenuCmd::CMD_COL_TOGGLE_END < 40248,
              "CMD_COL_TOGGLE_BASE 往后会与 40248+ 的新命令重叠");

// 列名清单最多几项。菜单生成侧据此做运行时夹取 ——
// 编译期 static_assert 只保证区间常量没被改，保证不了调用方传了 count > 48
// （多出来的 ID 会落到 40248+ 撞上别的命令）。定义成常量后两边共用一个真值。
constexpr int kColumnToggleCount =
    static_cast<int>(MenuCmd::CMD_COL_TOGGLE_END - MenuCmd::CMD_COL_TOGGLE_BASE) + 1;
static_assert(kColumnToggleCount == 48, "列切换 ID 区间应为 48 个");

// 列池索引 → 菜单命令 ID。越界索引返回 0。
// ⚠ 0 在 Win32 里**是合法菜单 ID**（相当于「无命令」），所以调用方拿到 0 必须
//   自己跳过这一项，不能直接塞进 AppendMenuW —— BuildColumnHeaderContext 里
//   就是先夹取 count 再循环，不会走到 0。
// 用途有两个，语义必须一致：
//   menus.cpp   BuildColumnHeaderContext 生成菜单项时用
//   tmwindow_menus.cpp  OnMenuCommand 解析 id 时用（交接给那份文件的负责人）
inline UINT ColumnToggleCommand(int index) {
    if (index < 0 || index >= kColumnToggleCount) return 0;
    return MenuCmd::CMD_COL_TOGGLE_BASE + static_cast<UINT>(index);
}

} // namespace tmp
