// menus.cpp - 原生菜单构建
// 全部文案逐字来自 docs/evidence/muidlg.txt（MUI 资源提取）与
// docs/evidence/tm/menus_dump.txt（LoadMenuW 枚举出的 16 个 RT_MENU 权威 dump），不臆造。
#include "menus.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

namespace tmp {

// ============================================================================
// 菜单层共享状态
//
// 有两组状态必须"弹菜单时读得到、点命令时写得进"，但真正消费它们的绘制代码
// （perfpage.cpp / tmwindow_list.cpp）不在菜单模块里，所以在菜单层先落一份状态，
// 保证 **菜单上的勾选/圆点不再是死的**。绘制侧接上之后只需读同一个值。
// ============================================================================

int& MenuBuilder::ResourceValueMode() {
    // 编码：group * 2 + (0=百分比, 1=值)
    //   group 0 = 内存(M)  1 = 磁盘(K/I)  2 = 网络(N)
    // 依据 MENU 30208 / 30216：资源值下是「内存/磁盘/网络」三组，每组两个
    // 互斥子项（30482..30487），且原版截图里
    // 「值」前面是**实心圆点**，不是对勾 —— 即三组各自是一组单选。
    //
    // 默认 1 = 内存→「值」。截图实拍值上带圆点，故默认值是 1 而不是 0。
    static int mode = 1;
    return mode;
}

// 「选项 → 显示完整账户名」的共享状态。见 include/menus.h 的声明注释。
bool& MenuBuilder::ShowFullNameOption() {
    static bool on = false;
    return on;
}

MenuBuilder::PerfMenuToggles& MenuBuilder::PerfToggles() {
    static PerfMenuToggles t;
    return t;
}

// ============================================================================
// 「设置默认选项卡」的持久化
//
// 依据（反汇编 0x14005038A → 0x140050447，见 11_mainmenu.md §2.6）：
// 原版点这一项**只做两件事**：CheckMenuItem 挪勾 + 把页号写盘，
// 全程**没有任何切换页面的调用**。写盘目标是
//   HKCU\Software\Microsoft\Windows\CurrentVersion\TaskManager  StartUpTab
//
// 本实现用**自己的键**存同一个语义的量，避免改写系统任务管理器的真实设置
// （同 GUI_DIFF 里列宽的处理方式：语义等价、互不干扰）。
// 读的时候自己的键没有就回落到系统那个值，这样用户原有的「默认选项卡」偏好
// 不会被无视。
// ============================================================================
namespace {

constexpr const wchar_t* kOwnRegKey = L"Software\\TaskManagerPro";
constexpr const wchar_t* kSysRegKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\TaskManager";
constexpr const wchar_t* kDefaultTabValue = L"StartUpTab";

bool QueryTabValue(HKEY root, const wchar_t* sub, int& out) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    DWORD v = 0, cb = sizeof(v), type = 0;
    LONG r = RegQueryValueExW(k, kDefaultTabValue, nullptr, &type,
                              reinterpret_cast<LPBYTE>(&v), &cb);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || type != REG_DWORD || v > 6) return false;
    out = static_cast<int>(v);
    return true;
}

} // namespace

int MenuBuilder::ReadDefaultTab(int fallback) {
    int v = 0;
    if (QueryTabValue(HKEY_CURRENT_USER, kOwnRegKey, v)) return v;
    if (QueryTabValue(HKEY_CURRENT_USER, kSysRegKey, v)) return v;
    return fallback;
}

void MenuBuilder::WriteDefaultTab(int index) {
    if (index < 0 || index > 6) return;
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kOwnRegKey, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) {
        return;
    }
    DWORD v = static_cast<DWORD>(index);
    RegSetValueExW(k, kDefaultTabValue, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&v), sizeof(v));
    RegCloseKey(k);
}

// ============================================================================
// 「资源值(&V)」子菜单 —— MENU 30208 与 30216 共用
//
// dump 30208（进程行右键）里是：
//   资源值(&V) ▸ 内存(&M) ▸ {百分比, 值}
//                磁盘(&K) ▸ {百分比, 值}
//                网络(&N) ▸ {百分比, 值}
// dump 30216（附加列头）里是同一份，只有「磁盘」的助记符从 (&K) 变 (&I)。
// 参数 diskMnemonic 就是为这个差异准备的。
// ============================================================================
HMENU MenuBuilder::BuildResourceValueSubmenu(int mode, wchar_t diskMnemonic) {
    HMENU resVal = CreatePopupMenu();
    const struct { const wchar_t* title; wchar_t mnemonic; UINT pctCmd; UINT valCmd; } groups[] = {
        { L"内存", L'M', CMD_VAL_MEM_PCT,  CMD_VAL_MEM_VAL  },
        { L"磁盘", diskMnemonic, CMD_VAL_DISK_PCT, CMD_VAL_DISK_VAL },
        { L"网络", L'N', CMD_VAL_NET_PCT,  CMD_VAL_NET_VAL  },
    };
    for (int g = 0; g < 3; ++g) {
        HMENU sub = CreatePopupMenu();
        AppendMenuW(sub, MF_STRING, groups[g].pctCmd, L"百分比");
        AppendMenuW(sub, MF_STRING, groups[g].valCmd, L"值");
        // 单选圆点：与「更新速度」同一手法，但这里用系统原生 API
        // CheckMenuRadioItem（原版导入表里就有它，见 imports.txt:882），
        // 它一次把整组设成 radio 类型并只勾一项 —— 比手工 MFT_RADIOCHECK
        // 更稳：应用程序不设 checked 时不会整组丢失圆点。
        int sel = -1;
        if (mode / 2 == g) sel = mode % 2;
        if (sel >= 0) CheckMenuRadioItem(sub, 0, 1, sel, MF_BYPOSITION);
        else {
            // 没有任何一组匹配时也要把类型设成 radio，否则某组一旦被点过，
            // 其余组会画成对勾 —— 原版三组永远都是圆点。
            for (int i = 0; i < 2; ++i) {
                MENUITEMINFOW mii{};
                mii.cbSize = sizeof(mii);
                mii.fMask = MIIM_FTYPE;
                if (GetMenuItemInfoW(sub, i, TRUE, &mii)) {
                    mii.fType |= MFT_RADIOCHECK;
                    SetMenuItemInfoW(sub, i, TRUE, &mii);
                }
            }
        }
        wchar_t label[32];
        swprintf_s(label, L"%s(&%c)", groups[g].title, groups[g].mnemonic);
        AppendMenuW(resVal, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), label);
    }
    return resVal;
}

HMENU MenuBuilder::BuildMainMenu(int currentTab, int updateIntervalMs,
                                 bool alwaysOnTop, bool hideWhenMin,
                                 bool showFullName, bool groupByType,
                                 bool minimizeOnUse, bool showAllHistory) {
    HMENU root = CreateMenu();

    // ---------- 文件(&F) ----------
    // 【MENU 30205 原文，menus_dump.txt:46-48】
    //   运行新任务(&N)
    //   退出(&X)
    // 两项**紧邻、没有分隔线**；而且「运行新任务」**不带省略号**
    // （MUI 原文 mui_all.txt:83 也是 `运行新任务(&N)`）。
    // 此前这里多了一条分隔线、文字还自己加了三个点。
    HMENU fileMenu = CreatePopupMenu();
    AppendMenuW(fileMenu, MF_STRING, CMD_NEW_TASK, L"运行新任务(&N)");
    AppendMenuW(fileMenu, MF_STRING, CMD_EXIT, L"退出(&X)");
    AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), L"文件(&F)");

    // ---------- 选项(&O) ----------
    // 【MENU 30205 原文，menus_dump.txt:49-64】
    //   置于顶层 / 使用时最小化 / 最小化时隐藏 / 设置默认选项卡 ▸
    //   ─────────
    //   显示完整账户名(&S)
    //   ─────────
    //   显示所有进程的历史记录(&P)
    // 注意：**两条**分隔线（dump 第 61 行与第 63 行）。此前只画了第一条，
    // 后两项被挤成一段。
    HMENU optMenu = CreatePopupMenu();
    AppendMenuW(optMenu, MF_STRING | (alwaysOnTop ? MF_CHECKED : 0),
                CMD_ALWAYS_ON_TOP, L"置于顶层(&A)");
    AppendMenuW(optMenu, MF_STRING | (minimizeOnUse ? MF_CHECKED : 0),
                CMD_MINIMIZE_ON_USE, L"使用时最小化(&M)");
    AppendMenuW(optMenu, MF_STRING | (hideWhenMin ? MF_CHECKED : 0),
                CMD_HIDE_WHEN_MIN, L"最小化时隐藏(&H)");

    // 设置默认选项卡 —— 这一项的存在直接证明 Win10 用"选项卡"
    //
    // 【语义】勾选的是**已保存的默认页**，不是当前停留的页。
    // 原版点它只写盘 + 挪勾，不切页（反汇编 0x14005038A..0x140050447，
    // 全程只有 CheckMenuItem 与持久化，没有 SetCurrentTab 一类调用）。
    // 此前这里用 currentTab 打勾、点一下直接切页，语义正好相反。
    const int savedTab = ReadDefaultTab(0);
    HMENU defTab = CreatePopupMenu();
    for (int i = 0; i < 7; ++i) {
        static const wchar_t* titles[] = {
            L"进程(&R)", L"性能(&E)", L"应用历史记录(&I)",
            L"启动(&T)", L"用户(&U)", L"详细信息(&L)", L"服务(&V)"
        };
        UINT flags = MF_STRING | ((savedTab == i) ? MF_CHECKED : 0);
        AppendMenuW(defTab, flags, CMD_DEF_TAB_BASE + i, titles[i]);
    }
    AppendMenuW(optMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(defTab),
                L"设置默认选项卡(&D)");

    AppendMenuW(optMenu, MF_SEPARATOR, 0, nullptr);
    // 【按页置灰，依据 taskmgr.exe 的菜单可用性刷新函数 0x14006B0E0】
    // 该项只在「用户」页(索引4)可用。
    {
        UINT f = MF_STRING | (showFullName ? MF_CHECKED : 0);
        if (currentTab != 4) f |= MF_GRAYED;
        AppendMenuW(optMenu, f, CMD_SHOW_FULL_NAME, L"显示完整账户名(&S)");
    }
    // 第二条分隔线（dump 第 63 行）
    AppendMenuW(optMenu, MF_SEPARATOR, 0, nullptr);
    // 只在「应用历史记录」页(索引2)可用（0x14006B146：eax=2; sub eax,edi）
    {
        UINT f = MF_STRING | (showAllHistory ? MF_CHECKED : 0);
        if (currentTab != 2) f |= MF_GRAYED;
        AppendMenuW(optMenu, f, CMD_SHOW_ALL_HISTORY, L"显示所有进程的历史记录(&P)");
    }
    AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(optMenu), L"选项(&O)");

    // ---------- 查看(&V) ----------
    // 【MENU 30205 原文，menus_dump.txt:65-75】
    //   立即刷新 / 更新速度 ▸{高,正常,低,已暂停} / ─── / 按类型分组 / 全部展开 / 全部折叠
    HMENU viewMenu = CreatePopupMenu();
    AppendMenuW(viewMenu, MF_STRING, CMD_REFRESH_NOW, L"立即刷新(&R)");

    HMENU speed = CreatePopupMenu();
    // 「更新速度」的四个档位是**互斥单选**，原版用 CheckMenuRadioItem
    // 画实心圆点 ● 而不是对勾 ✓（导入表 imports.txt:882 有该 API，
    // 调用点 disasm 0x45567 的参数 edx=0x7708 / r8d=0x770B 正好覆盖
    // 30472..30475 这条连续 ID 段）。
    //
    // 【隐患修复】此前是 AppendMenu 打 MF_CHECKED，再用 MIIM_FTYPE 逐项
    // 补 MFT_RADIOCHECK。MSDN 规定 MFT_RADIOCHECK 必须配合 MFS_CHECKED 才画
    // 圆点；一旦某个时刻四项都不 checked，整组圆点会集体消失（原版永远有且
    // 只有一个点）。改用 CheckMenuRadioItem 后这条耦合自动消失，
    // 并且和原版用的是同一个 API。
    struct SpeedItem { UINT cmd; int ms; const wchar_t* text; };
    const SpeedItem items[] = {
        { CMD_UPDATE_HIGH,   500,  L"高(&H)" },
        { CMD_UPDATE_NORMAL, 1000, L"正常(&N)" },
        { CMD_UPDATE_LOW,    4000, L"低(&L)" },
        { CMD_UPDATE_PAUSED, 0,    L"已暂停(&P)" },
    };
    const int speedCount = static_cast<int>(sizeof(items) / sizeof(items[0]));
    int speedSel = -1;
    for (int i = 0; i < speedCount; ++i) {
        AppendMenuW(speed, MF_STRING, items[i].cmd, items[i].text);
        if (items[i].ms == updateIntervalMs) speedSel = i;
    }
    if (speedSel < 0) speedSel = 1;   // 认不出就按「正常」，保证永远有一个圆点
    CheckMenuRadioItem(speed, 0, speedCount - 1, speedSel, MF_BYPOSITION);
    AppendMenuW(viewMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(speed),
                L"更新速度(&U)");

    AppendMenuW(viewMenu, MF_SEPARATOR, 0, nullptr);
    // 「按类型分组」是**可独立开关的复选**，用对勾 ✓（一个菜单里可以有多个 ✓，
    // 列头菜单的 类型/状态/CPU/内存 四项同时勾选可证）。
    //
    // 它只在「进程」页(索引0)可用：可用性刷新函数 0x14006B0E0 里对该项做
    // test edi,edi; je enabled，即只有 pageIndex==0 才启用，其余 6 页置灰。
    {
        bool groupApplies = (currentTab == 0);
        UINT flags = MF_STRING | (groupByType ? MF_CHECKED : 0);
        if (!groupApplies) flags |= MF_GRAYED;
        AppendMenuW(viewMenu, flags, CMD_GROUP_BY_TYPE, L"按类型分组(&G)");
    }
    // 【按页置灰】原版可用页 = {进程0, 应用历史记录2, 启动3, 用户4}
    // （0x14006B0E0 收尾段：test edi,edi / lea eax,[rdi-2]; cmp eax,2; jbe enabled）
    {
        bool applies = (currentTab == 0 || currentTab == 2 ||
                        currentTab == 3 || currentTab == 4);
        UINT f = MF_STRING | (applies ? 0 : MF_GRAYED);
        AppendMenuW(viewMenu, f, CMD_EXPAND_ALL, L"全部展开(&E)");
        AppendMenuW(viewMenu, f, CMD_COLLAPSE_ALL, L"全部折叠(&C)");
    }
    AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(viewMenu), L"查看(&V)");

    return root;
}

// ============================================================================
// MENU 30208 —— 进程页行右键（menus_dump.txt:107-133，17 项 / 2 条分隔线）
//
//   展开(&P) / 折叠(&P) / 切换到(&T) / 重新启动(&R) / 结束任务(&E) /
//   资源值(&V)▸ / 提供反馈(&B) / ─── / 调试(&D) / 创建转储文件(&C) /
//   运行新任务(&N) / 置于顶层(&A) / ─── / 转到详细信息(&G) /
//   打开文件所在的位置(&O) / 在线搜索(&S) / 属性(&I)
//
// 此前这一页有四处结构性错误：
//   1. 「结束任务」被挪到最末，还多包了一条分隔线（现在是第 5 项，前后无分隔线）；
//   2. 「重新启动」被绑在 hasChildren 上，后台进程根本没有这一项（现在常驻）；
//   3. 「展开」「折叠」按状态只生成一项（现在两项并列，按状态置灰其一）；
//   4. 分隔线 5 条 vs 原版 2 条。
// 另外原版**没有「组头专属菜单」**这个概念，组头右键同样弹这一份，
// 只是与进程相关的项置灰 —— 所以 isGroupRow 只影响置灰，不影响项集。
// ============================================================================
HMENU MenuBuilder::BuildProcessContext(bool hasSelection, bool isGroupRow,
                                       bool hasChildren, bool expanded) {
    HMENU m = CreatePopupMenu();

    // 组头行：展开/折叠 可用（按状态二选一置灰），其余进程相关项一律灰。
    // 原版在弹出前按上下文标志对 0x76E5/0x76E6/0x76FA
    // （30437 展开 / 30438 折叠 / 30458 切换到）分别 EnableMenuItem，
    // 见 disasm 0x93CA1-0x93CAF。
    const bool procOk = hasSelection && !isGroupRow;   // 与具体进程相关的项
    UINT enProc  = procOk ? MF_ENABLED : MF_GRAYED;
    UINT enExpand   = (hasSelection && (isGroupRow || hasChildren) && !expanded)
                      ? MF_ENABLED : MF_GRAYED;
    UINT enCollapse = (hasSelection && (isGroupRow || hasChildren) && expanded)
                      ? MF_ENABLED : MF_GRAYED;

    AppendMenuW(m, MF_STRING | enExpand,   CMD_EXPAND,   L"展开(&P)");
    AppendMenuW(m, MF_STRING | enCollapse, CMD_COLLAPSE, L"折叠(&P)");
    // 「切换到」对组头行无意义（组头不对应任何进程），与进程相关项一同置灰。
    AppendMenuW(m, MF_STRING | enProc, CMD_SWITCH_TO, L"切换到(&T)");
    AppendMenuW(m, MF_STRING | enProc, CMD_RESTART,  L"重新启动(&R)");
    AppendMenuW(m, MF_STRING | enProc, CMD_END_TASK, L"结束任务(&E)");

    // 资源值(&V)：三组 × 两项，每组单选圆点。进程页这里是 磁盘(&K)。
    HMENU resVal = BuildResourceValueSubmenu(ResourceValueMode(), L'K');
    AppendMenuW(m, MF_POPUP | enProc, reinterpret_cast<UINT_PTR>(resVal),
                L"资源值(&V)");

    AppendMenuW(m, MF_STRING | enProc, CMD_PROVIDE_FEEDBACK, L"提供反馈(&B)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);          // dump 第 124 行
    AppendMenuW(m, MF_STRING | enProc, CMD_DEBUG, L"调试(&D)");
    AppendMenuW(m, MF_STRING | enProc, CMD_CREATE_DUMP, L"创建转储文件(&C)");
    // 这两项不依赖选中行，原版任何情况下都可点
    AppendMenuW(m, MF_STRING, CMD_NEW_TASK, L"运行新任务(&N)");
    AppendMenuW(m, MF_STRING, CMD_ALWAYS_ON_TOP, L"置于顶层(&A)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);          // dump 第 129 行
    AppendMenuW(m, MF_STRING | enProc, CMD_GOTO_DETAILS, L"转到详细信息(&G)");
    AppendMenuW(m, MF_STRING | enProc, CMD_OPEN_LOCATION, L"打开文件所在的位置(&O)");
    AppendMenuW(m, MF_STRING | enProc, CMD_SEARCH_ONLINE, L"在线搜索(&S)");
    AppendMenuW(m, MF_STRING | enProc, CMD_PROPERTIES, L"属性(&I)");
    return m;
}

// ============================================================================
// MENU 30200 —— 详细信息页行右键（menus_dump.txt:1-24，16 项 / 3 条分隔线）
//
//   结束任务 / 结束进程树 / 提供反馈
//   ───
//   设置优先级▸(6) / 设置相关性
//   ───
//   分析等待链 / 调试 / UAC 虚拟化 / 创建转储文件
//   ───
//   打开文件所在的位置 / 在线搜索(&N) / 属性(&R) / 转到服务(&S)
//
// 此前三条分隔线**数量对、位置全错**：多了一条（结束进程树之后）、
// 少了一条（设置相关性之后）、又多了一条（转到服务之前）。
// 「设置优先级」是单选项，当前级别应打 ●：disasm 里原版用 CheckMenuRadioItem，
// 这里同样用；currentPriority 传 ProcessPriorityClass，未知时传 -1 表示不画点。
// ============================================================================
HMENU MenuBuilder::BuildDetailsContext(bool hasSelection, DWORD currentPriority) {
    HMENU m = CreatePopupMenu();
    UINT en = hasSelection ? MF_ENABLED : MF_GRAYED;

    AppendMenuW(m, MF_STRING | en, CMD_END_TASK, L"结束任务(&E)");
    AppendMenuW(m, MF_STRING | en, CMD_END_TREE, L"结束进程树(&T)");
    AppendMenuW(m, MF_STRING | en, CMD_PROVIDE_FEEDBACK, L"提供反馈(&B)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);          // dump 第 6 行

    HMENU prio = CreatePopupMenu();
    const struct { UINT cmd; DWORD cls; const wchar_t* text; } prios[] = {
        { CMD_PRIO_REALTIME, REALTIME_PRIORITY_CLASS,     L"实时(&R)"     },
        { CMD_PRIO_HIGH,     HIGH_PRIORITY_CLASS,         L"高(&H)"       },
        { CMD_PRIO_ABOVE,    ABOVE_NORMAL_PRIORITY_CLASS, L"高于正常(&A)" },
        { CMD_PRIO_NORMAL,   NORMAL_PRIORITY_CLASS,       L"正常(&N)"     },
        { CMD_PRIO_BELOW,    BELOW_NORMAL_PRIORITY_CLASS, L"低于正常(&B)" },
        { CMD_PRIO_LOW,      IDLE_PRIORITY_CLASS,         L"低(&L)"       },
    };
    const int prioCount = static_cast<int>(sizeof(prios) / sizeof(prios[0]));
    int prioSel = -1;
    for (int i = 0; i < prioCount; ++i) {
        AppendMenuW(prio, MF_STRING, prios[i].cmd, prios[i].text);
        if (currentPriority != 0xFFFFFFFFu && prios[i].cls == currentPriority) prioSel = i;
    }
    if (prioSel >= 0) {
        CheckMenuRadioItem(prio, 0, prioCount - 1, prioSel, MF_BYPOSITION);
    } else {
        // 未知当前级别时仍把这组设成 radio 类型，避免下一轮画成对勾
        for (int i = 0; i < prioCount; ++i) {
            MENUITEMINFOW mii{};
            mii.cbSize = sizeof(mii);
            mii.fMask = MIIM_FTYPE;
            if (GetMenuItemInfoW(prio, i, TRUE, &mii)) {
                mii.fType |= MFT_RADIOCHECK;
                SetMenuItemInfoW(prio, i, TRUE, &mii);
            }
        }
    }
    AppendMenuW(m, MF_POPUP | en, reinterpret_cast<UINT_PTR>(prio), L"设置优先级(&P)");

    AppendMenuW(m, MF_STRING | en, CMD_SET_AFFINITY, L"设置相关性(&F)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);          // dump 第 15 行
    AppendMenuW(m, MF_STRING | en, CMD_ANALYZE_CHAIN, L"分析等待链(&A)");
    AppendMenuW(m, MF_STRING | en, CMD_DEBUG, L"调试(&D)");
    AppendMenuW(m, MF_STRING | en, CMD_UAC_VIRT, L"UAC 虚拟化(&V)");
    AppendMenuW(m, MF_STRING | en, CMD_CREATE_DUMP, L"创建转储文件(&C)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);          // dump 第 20 行
    AppendMenuW(m, MF_STRING | en, CMD_OPEN_LOCATION, L"打开文件所在的位置(&O)");
    AppendMenuW(m, MF_STRING | en, CMD_SEARCH_ONLINE, L"在线搜索(&N)");
    AppendMenuW(m, MF_STRING | en, CMD_PROPERTIES, L"属性(&R)");
    AppendMenuW(m, MF_STRING | en, CMD_GOTO_SERVICES, L"转到服务(&S)");
    return m;
}

// ============================================================================
// MENU 30202 / 30210 —— 服务页行右键（两个变体）
//
// 30202（该服务有 PID，可操作，menus_dump.txt:31-39，6 项 / 1 条分隔线）：
//   开始(&S) / 停止(&T) / 重新启动(&R) / ─── / 打开服务(&V) / 在线搜索(&O) /
//   转到详细信息(&D)
// 30210（该服务无 PID，menus_dump.txt:143-147，3 项 / 0 条分隔线）：
//   停止(&S) / 打开服务(&O) / 在线搜索(&S)
//
// 选择逻辑（变体怎么选）：dump 里两张菜单名字都叫 "Service Context"，
// 都用 MUI 的「服务」页；区别只在项集 —— 有 PID 才有「开始/重新启动/转到详细信息」
// （这三项都要对进程动手）。所以判据是**该服务当前是否有关联进程（pid != 0）**，
// 这正是调用点 tmwindow_events.cpp 现在用的条件，逻辑正确。
//
// 逐项置灰依据原版实拍：该服务正在运行时「开始(S)」是灰的。
// ServiceItem 里已有 canStart / canStop（按 stateRaw 算好），
// 这里用可选参数接进来（缺省保持可用，等价于旧行为）。
// ============================================================================
HMENU MenuBuilder::BuildServiceContext(bool hasSelection, bool hasPid,
                                      bool canStart, bool canStop) {
    HMENU m = CreatePopupMenu();
    UINT en = hasSelection ? MF_ENABLED : MF_GRAYED;
    UINT enStart = (hasSelection && canStart) ? MF_ENABLED : MF_GRAYED;
    UINT enStop  = (hasSelection && canStop)  ? MF_ENABLED : MF_GRAYED;

    // 【两个变体的助记符差异是刻意的，不要"统一"】
    // 变成 30210 时「停止」是 (&S) 不是 (&T)，「打开服务」是 (&O) 不是 (&V)。
    if (!hasPid) {
        AppendMenuW(m, MF_STRING | enStop, CMD_SVC_STOP,      L"停止(&S)");
        AppendMenuW(m, MF_STRING | en,     CMD_SVC_OPEN_SCM,  L"打开服务(&O)");
        AppendMenuW(m, MF_STRING | en,     CMD_SEARCH_ONLINE, L"在线搜索(&S)");
        return m;
    }

    AppendMenuW(m, MF_STRING | enStart, CMD_SVC_START,    L"开始(&S)");
    AppendMenuW(m, MF_STRING | enStop,  CMD_SVC_STOP,     L"停止(&T)");
    AppendMenuW(m, MF_STRING | enStop,  CMD_SVC_RESTART,  L"重新启动(&R)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);      // dump 第 36 行，唯一一条
    AppendMenuW(m, MF_STRING | en, CMD_SVC_OPEN_SCM, L"打开服务(&V)");
    AppendMenuW(m, MF_STRING | en, CMD_SEARCH_ONLINE, L"在线搜索(&O)");
    // 原版「转到详细信息」紧接在「在线搜索」之后，**没有分隔线**。
    // 此前这里多画了一条（crops/svc_menu_6x.png 实拍只有一条线）。
    AppendMenuW(m, MF_STRING | en, CMD_GOTO_DETAILS, L"转到详细信息(&D)");
    return m;
}

// ============================================================================
// MENU 30211 —— 用户页行右键（menus_dump.txt:150-159，9 项 / 1 条分隔线）
//   展开(&P) / 折叠(&P) / 连接(&C) / 断开连接(&D) / 注销(&I) / 发送消息(&S) /
//   切换用户账户(&W) / ─── / 管理用户账户(&M)
// 此前：展开/折叠 只生成一项；且它们后面多了一条分隔线（原版第 3 项直接是「连接」）。
// ============================================================================
HMENU MenuBuilder::BuildUserContext(bool hasSelection, bool expanded) {
    HMENU m = CreatePopupMenu();
    UINT en = hasSelection ? MF_ENABLED : MF_GRAYED;
    // 用户页没有分组树，展开/折叠恒灰（原版细节未证实到资源级，这里按
    // 「不可用」处理，至少不会让用户点了没反应）。
    UINT enExpand   = MF_GRAYED;
    UINT enCollapse = MF_GRAYED;
    (void)expanded;

    AppendMenuW(m, MF_STRING | enExpand,   CMD_EXPAND,   L"展开(&P)");
    AppendMenuW(m, MF_STRING | enCollapse, CMD_COLLAPSE, L"折叠(&P)");
    AppendMenuW(m, MF_STRING | en, CMD_USR_CONNECT,    L"连接(&C)");
    AppendMenuW(m, MF_STRING | en, CMD_USR_DISCONNECT, L"断开连接(&D)");
    AppendMenuW(m, MF_STRING | en, CMD_USR_LOGoff,     L"注销(&I)");
    AppendMenuW(m, MF_STRING | en, CMD_USR_SEND_MSG,   L"发送消息(&S)");
    AppendMenuW(m, MF_STRING | en, CMD_USR_SWITCH,     L"切换用户账户(&W)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);          // dump 第 158 行
    AppendMenuW(m, MF_STRING | en, CMD_USR_MANAGE, L"管理用户账户(&M)");
    return m;
}

// ============================================================================
// MENU 30213 —— 启动页行右键（menus_dump.txt:162-170，8 项 / 1 条分隔线）
//   展开(&P) / 折叠(&P) / 启用(&E) / 禁用(&D) / ─── /
//   打开文件所在的位置(&O) / 在线搜索(&S) / 属性(&I)
//
// 此前缺「展开/折叠」两项，且「启用/禁用」按 enabled 只生成一项 ——
// 第二参被调用点写死 true，于是已禁用的启动项也永远只显示「禁用」，
// 用户无法重新启用任何启动项。
// ============================================================================
HMENU MenuBuilder::BuildStartupContext(bool hasSelection, bool enabled) {
    HMENU m = CreatePopupMenu();
    UINT en = hasSelection ? MF_ENABLED : MF_GRAYED;
    // 原版启动页是按用户分组的树，有真实展开语义；本实现没有分组树，
    // 但两项仍然并列显示（保持项集与原版一致），恒灰。
    AppendMenuW(m, MF_STRING | MF_GRAYED, CMD_EXPAND, L"展开(&P)");
    AppendMenuW(m, MF_STRING | MF_GRAYED, CMD_COLLAPSE, L"折叠(&P)");
    // 两项并列，按当前状态置灰其一（原版 dump 165-166 相邻两项）
    AppendMenuW(m, MF_STRING | ((hasSelection && !enabled) ? MF_ENABLED : MF_GRAYED),
                CMD_STARTUP_ENABLE,  L"启用(&E)");
    AppendMenuW(m, MF_STRING | ((hasSelection && enabled) ? MF_ENABLED : MF_GRAYED),
                CMD_STARTUP_DISABLE, L"禁用(&D)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);          // dump 第 167 行
    AppendMenuW(m, MF_STRING | en, CMD_OPEN_LOCATION, L"打开文件所在的位置(&O)");
    AppendMenuW(m, MF_STRING | en, CMD_SEARCH_ONLINE, L"在线搜索(&S)");
    AppendMenuW(m, MF_STRING | en, CMD_PROPERTIES, L"属性(&I)");
    return m;
}

// ============================================================================
// MENU 30217 —— 启动页「子进程行」右键（menus_dump.txt:198-201，3 项 / 0 条线）
//   打开文件所在的位置(&O) / 在线搜索(&S) / 属性(&I)
// 原版把它和 30213（启动项行）分开：展开一个启动项后会列出它的可执行文件行，
// 那一行的右键只有这三项。本实现启动页还没有分组树，所以这张资源暂时没有
// 调用点 —— 但保留完整实现，等启动页树做出来后直接接上即可。
// ============================================================================
HMENU MenuBuilder::BuildStartupProcessContext(bool hasSelection) {
    HMENU m = CreatePopupMenu();
    UINT en = hasSelection ? MF_ENABLED : MF_GRAYED;
    AppendMenuW(m, MF_STRING | en, CMD_OPEN_LOCATION, L"打开文件所在的位置(&O)");
    AppendMenuW(m, MF_STRING | en, CMD_SEARCH_ONLINE, L"在线搜索(&S)");
    AppendMenuW(m, MF_STRING | en, CMD_PROPERTIES, L"属性(&I)");
    return m;
}

// ============================================================================
// MENU 30214 —— 应用历史页行右键（menus_dump.txt:173-176，3 项 / **0 条分隔线**）
//   切换到(&T) / 在线搜索(&S) / 属性(&P)
// 注意「属性」这里是 (&P)，与其它页的 (&I) / (&R) 不同，原版就是如此。
// 此前多画了一条分隔线。
// ============================================================================
HMENU MenuBuilder::BuildAppHistoryContext(bool hasSelection) {
    HMENU m = CreatePopupMenu();
    UINT en = hasSelection ? MF_ENABLED : MF_GRAYED;
    AppendMenuW(m, MF_STRING | en, CMD_SWITCH_TO, L"切换到(&T)");
    AppendMenuW(m, MF_STRING | en, CMD_SEARCH_ONLINE, L"在线搜索(&S)");
    AppendMenuW(m, MF_STRING | en, CMD_PROPERTIES, L"属性(&P)");
    return m;
}

// ============================================================================
// MENU 30201 —— 列头右键（静态版）（menus_dump.txt:27-29）
//   隐藏列(&D) / 选择列(&S)
//
// 【列头右键到底弹哪张】这不是二选一，而是两张资源承担不同角色，
// 证据来自反汇编：
//   1. 30216 在 0x140092B87 处被 LoadMenuW 装载后，**只取第 0 个子菜单**
//      （内存(&M)），随后整份菜单 RemoveMenu + DestroyMenu。
//      也就是说 30216 是被当**子菜单零件**用的，从不整份弹出。
//   2. 列头右键真正弹出的菜单是**运行期动态生成**的：0x140076F9E 处有一段
//      InsertMenuW 循环，遍历一个字符串数组逐项插入列名
//      （类型 / 状态 / 发布者 / PID / 进程名称 / 命令行 / CPU / 内存 / 磁盘 /
//      网络 / GPU / GPU 引擎 / 电源使用情况 / 电源使用情况趋势），
//      已显示的列前面带 ✓（原版实拍），
//      末尾一条分隔线后接「资源值(V) ▸」，其子菜单就是取来的 30216 第 0 个子菜单
//      （原版实拍：内存(M) ▸ {百分比, ●值}）。
//   3. 所以 **30201（隐藏列/选择列）不是最终形态**，它对应的是"不支持列清单"的
//      简化场景；30216 是零件，永不单独整份弹出。
//   结论与修复方向：列头右键 = 动态列名清单（打 ✓ + 可切换）+ 分隔线 +
//     「资源值(&V)」子菜单（复用 BuildResourceValueSubmenu）。
//   本模块已提供 BuildColumnHeaderContext(列名数组, 可见标志, 数量, 资源值模式)，
//   调用点只需在 WM_RBUTTONUP 里判断 y 是否落在 rcColHeader_ 内。
//
// 【P0-8 状态：接线与命令处理已由并行改动补齐】
//   - 弹出的位置在 src/tmwindow_events.cpp 的 WM_RBUTTONUP 列头带分支
//     （`tabs_.Current() == TabId::Processes && y ∈ rcColHeader_`）；
//   - 命令处理在 src/tmwindow_menus.cpp 的 OnMenuCommand：
//     `CMD_COL_TOGGLE_BASE..CMD_COL_TOGGLE_END` 走 SetColumnVisible(idx, ...)，
//     `CMD_HIDE_COLUMN` 用 headerContextCol_ 隐藏光标所在列，
//     `CMD_SELECT_COLUMNS` 有独立分支；
//   - 列可见性维度在 include/tmwindow.h 的 colVisible_[kSortColCount]，
//     绘制/命中/列宽计算在 tmwindow_list.cpp 里跳过隐藏列。
//   也就是说这条**在 DD 提交时已经真的能用了**，本文件这一侧只剩「生成正确菜单」
//   的职责，本次的改动就是给它加上运行期 ID 夹取（见 BuildColumnHeaderContext）。
// ============================================================================
HMENU MenuBuilder::BuildHeaderContext() {
    HMENU m = CreatePopupMenu();
    // ⚠ 本函数**仍然零调用点**：真实列头右键弹的是 BuildColumnHeaderContext()
    //   （运行期动态列名清单）。这里保留的是 MENU 30201 那张「简化版」资源形状，
    //   供将来其它页（没有列清单模型的页）复用。它的两个 ID 现在**已有 case**
    //   （CMD_HIDE_COLUMN / CMD_SELECT_COLUMNS 在 OnMenuCommand 里都有处理），
    //   所以将来接上调用点就能直接用，不会再是死项。
    AppendMenuW(m, MF_STRING, CMD_HIDE_COLUMN, L"隐藏列(&D)");
    AppendMenuW(m, MF_STRING, CMD_SELECT_COLUMNS, L"选择列(&S)");
    return m;
}

HMENU MenuBuilder::BuildColumnHeaderContext(const wchar_t* const* titles,
                                            const bool* visible, int count,
                                            int resValueMode,
                                            int hoveredColumn) {
    // 【P0-8】hoveredColumn 用于让菜单知道「光标在哪一列」。
    //   当前消费者在 OnMenuCommand 的 CMD_HIDE_COLUMN 分支（它读的是
    //   TmWindow::headerContextCol_，与本参数同源但存的是成员变量）——
    //   所以这里仍然不消费它，只是**记录一个意图**：本函数不需要额外项来
    //   表达「隐藏这一列」，那是 CMD_HIDE_COLUMN 的职责，不是列名清单的。
    (void)hoveredColumn;
    HMENU m = CreatePopupMenu();
    // 【P0-8 真实修复】ID 区间夹取。
    //   CMD_COL_TOGGLE_BASE..CMD_COL_TOGGLE_END 是一段**独占**区间（menus.h 的
    //   static_assert 已经在编译期保证它不和 40140 那批服务命令重叠）。
    //   但编译期只保护「区间常量没被改」，保护不了「调用方传了 count > 48」：
    //   一旦 count=49，CMD_COL_TOGGLE_BASE+48 = 40248 就越界到别人的命令上，
    //   点一个列名会去执行毫不相干的动作（历史上正是这么撞上 CMD_SVC_START 的）。
    //   所以这里在生成侧做运行时夹取，越界项直接不生成。
    int n = count;
    if (n > kColumnToggleCount) n = kColumnToggleCount;
    if (n < 0) n = 0;
    for (int i = 0; i < n; ++i) {
        if (!titles[i]) continue;
        UINT f = MF_STRING | ((visible && visible[i]) ? MF_CHECKED : 0);
        AppendMenuW(m, f, ColumnToggleCommand(i), titles[i]);
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP,
                reinterpret_cast<UINT_PTR>(BuildResourceValueSubmenu(resValueMode, L'I')),
                L"资源值(&V)");
    return m;
}

// ============================================================================
// MENU 30206 —— 性能页图表区右键（menus_dump.txt:78-96）
//   将图形更改为(&H)▸{总体利用率, 逻辑处理器, NUMA 节点, 单个引擎, 多个引擎}
//   显示内核时间(&K)
//   ───
//   图形摘要视图(&W)
//   查看(&V)▸{CPU(&P), 内存(&M), 磁盘(&D), 网络(&N), GPU(&G)}
//   查看网络详细信息(&D)
//   ───
//   复制(&C) Ctrl+C
//
// 此前：缺整个「查看▸」子菜单（5 项）、缺「查看网络详细信息」、
// 分隔线位置全错（「将图形更改为」后面多一条、「图形摘要视图」后面该有却没有）。
// ============================================================================
HMENU MenuBuilder::BuildPerfContext(int currentView, bool kernelTime, bool summary) {
    HMENU m = CreatePopupMenu();
    // 这三个状态的真值在菜单层（PerfToggles），因为调用点把参数写死成
    // (0,false,false)（见 src/tmwindow_events.cpp 的 BuildPerfContext(0,false,false)），
    // 光靠传入值会「总体利用率」永远打点、两个复选项永不打勾。
    // 这里先看调用点传入的值，再看菜单层记录 —— 用户在菜单上改过就以记录为准。
    // 【P0-6 后续】graphSummary / kernelTime 的**绘制侧仍未接入**（见 DD_perf_p0.md
    //   的「未修」表）；chartMode 那一项本次已真正接到绘制（perfPage_.SetChart）。
    const PerfMenuToggles& tg = PerfToggles();
    const bool kt = tg.kernelTime || kernelTime;
    const bool gs = tg.graphSummary || summary;
    const int  cv = (currentView > 0) ? currentView
                                      : (tg.chartMode >= 0 ? tg.chartMode : currentView);

    HMENU charts = CreatePopupMenu();
    const struct { UINT cmd; const wchar_t* text; } chartItems[] = {
        { CMD_CHART_OVERALL,    L"总体利用率(&O)" },
        { CMD_CHART_LOGICAL,    L"逻辑处理器(&L)" },
        { CMD_CHART_NUMA,       L"NUMA 节点(&N)" },
        { CMD_CHART_SINGLE_ENG, L"单个引擎(&S)" },
        { CMD_CHART_MULTI_ENG,  L"多个引擎(&E)" },
    };
    const int chartCount = static_cast<int>(sizeof(chartItems) / sizeof(chartItems[0]));
    for (int i = 0; i < chartCount; ++i) {
        AppendMenuW(charts, MF_STRING, chartItems[i].cmd, chartItems[i].text);
    }
    // 原版这一组是单选（ChartMode 互斥）。用 CheckMenuRadioItem 一次设定，
    // 并且保证认不出当前模式时也落在「总体利用率」上 —— 永远有且只有一个圆点。
    int chartSel = (cv >= 0 && cv < chartCount) ? cv : 0;
    CheckMenuRadioItem(charts, 0, chartCount - 1, chartSel, MF_BYPOSITION);
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(charts), L"将图形更改为(&H)");

    // 「将图形更改为」后面**没有**分隔线（dump 第 85 行紧接第 86 行）
    AppendMenuW(m, MF_STRING | (kt ? MF_CHECKED : 0),
                CMD_SHOW_KERNEL_TIME, L"显示内核时间(&K)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);          // dump 第 86 行之后
    // 【置灰 · 2026-11-21】绘制侧未接入（原版摘要视图的渲染形态无证据，
    // 见 ZZ_FIXES.md:157）。留着可勾选就是假功能：勾了画面不变。
    // 置灰并去掉勾选态，用户能看出「这里暂不支持」而不是以为切换成功了。
    AppendMenuW(m, MF_STRING | MF_GRAYED,
                CMD_GRAPH_SUMMARY, L"图形摘要视图(&W)");

    // 「查看(&V)」子菜单 —— 在图表区直接切页，此前完全缺失
    HMENU viewSub = CreatePopupMenu();
    const struct { UINT cmd; const wchar_t* text; } viewItems[] = {
        { CMD_PERF_VIEW_CPU,  L"CPU(&P)"  },
        { CMD_PERF_VIEW_MEM,  L"内存(&M)" },
        { CMD_PERF_VIEW_DISK, L"磁盘(&D)" },
        { CMD_PERF_VIEW_NET,  L"网络(&N)" },
        { CMD_PERF_VIEW_GPU,  L"GPU(&G)"  },
    };
    const int viewCount = static_cast<int>(sizeof(viewItems) / sizeof(viewItems[0]));
    for (int i = 0; i < viewCount; ++i) {
        AppendMenuW(viewSub, MF_STRING, viewItems[i].cmd, viewItems[i].text);
    }
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(viewSub), L"查看(&V)");
    AppendMenuW(m, MF_STRING, CMD_NET_DETAILS, L"查看网络详细信息(&D)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);          // dump 第 95 行
    // 【快捷键右对齐】原版这一项的字节是 `复制(&C) \bCtrl+C`（od -c 实测，
    // menus_dump.txt:96）。0x08 是 Win32 菜单的「右对齐快捷键」分隔符；
    // 之前用的是 \t，只会按制表位推进，快捷键文字不会贴右边缘。
    AppendMenuW(m, MF_STRING, CMD_COPY, L"复制(&C)\bCtrl+C");
    return m;
}

// ============================================================================
// MENU 30207 —— 性能页左侧栏右键（menus_dump.txt:99-104）
//   摘要视图(&W) / 显示图形(&S) / 隐藏图形(&H) / ─── / 复制(&C) Ctrl+C
//
// 与 30206 是**两张不同的资源**：原版侧栏右键弹 30207，图表区右键弹 30206。
// 分隔它们的边界现成可用：perftheme::kSidebarWidth = 210
// （include/perfpage.h:46），判断 x < bounds.left + kSidebarWidth 即可。
// 调用点在 tmwindow_events.cpp 的 WM_RBUTTONUP（不在本模块文件范围内），
// 接线方式：TabId::Performance 分支里先判 x，再选 BuildPerfSidebarContext()
// 还是 BuildPerfContext(...)。
// ============================================================================
HMENU MenuBuilder::BuildPerfSidebarContext(bool summaryView, bool hideGraph) {
    HMENU m = CreatePopupMenu();
    // 【置灰 · 2026-11-21】同 30206 的「图形摘要视图」：绘制侧未接入，
    // 原版渲染形态无证据（ZZ_FIXES.md:161）。置灰优于假可勾选。
    AppendMenuW(m, MF_STRING | MF_GRAYED,
                CMD_SIDEBAR_SUMMARY, L"摘要视图(&W)");
    AppendMenuW(m, MF_STRING | (!hideGraph ? MF_CHECKED : 0),
                CMD_SIDEBAR_SHOW_GRAPH, L"显示图形(&S)");
    AppendMenuW(m, MF_STRING | (hideGraph ? MF_CHECKED : 0),
                CMD_SIDEBAR_HIDE_GRAPH, L"隐藏图形(&H)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);          // dump 第 103 行
    AppendMenuW(m, MF_STRING, CMD_COPY, L"复制(&C)\bCtrl+C");
    return m;
}

// ============================================================================
// MENU 30204 —— Other Context（menus_dump.txt:42-43）
// 只有一个「结束进程(&E)」，注意文案与其它菜单的「结束任务(&E)」不同。
// 触发场景本地资源无法确定（docs/specs/diff_menu.md 未记录）→ 未证实，
// 保留实现但不接调用点。
// ============================================================================
HMENU MenuBuilder::BuildOtherContext(bool hasSelection) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (hasSelection ? MF_ENABLED : MF_GRAYED),
                CMD_END_TASK, L"结束进程(&E)");
    return m;
}

// ============================================================================
// MENU 30215 —— 托盘图标右键（menus_dump.txt:179-183）
//   还原(&R) / 关闭(&C) / ─── / 置于顶层(&A)
// 本模块此前没有这张菜单（托盘菜单是 tmwindow_events.cpp 里现拼的两项，
// 少了分隔线与「置于顶层」）。这里提供完整版本供托盘回调使用。
// ============================================================================
HMENU MenuBuilder::BuildTrayContext(bool alwaysOnTop) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_RESTORE_FROM_TRAY, L"还原(&R)");
    AppendMenuW(m, MF_STRING, CMD_CLOSE_APP,         L"关闭(&C)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);        // dump 第 182 行
    AppendMenuW(m, MF_STRING | (alwaysOnTop ? MF_CHECKED : 0),
                CMD_ALWAYS_ON_TOP, L"置于顶层(&A)");
    return m;
}

HACCEL MenuBuilder::BuildAccelerators() {
    // 【按原版 ACCEL 资源 #26100 逐字节还原】
    //
    // 原版在 `zh-CN\taskmgr.exe.mui` 里有一张 4 条的 ACCEL 表，二进制实测：
    //   03 00 2e 00 f3 76 00 00   → FVIRTKEY|FNOINVERT, VK_DELETE, cmd 0x76F3=30451
    //   01 00 1b 00 e3 76 00 00   → FVIRTKEY,            VK_ESCAPE, cmd 0x76E3=30435
    //   07 00 1b 00 e4 76 00 00   → FVIRTKEY|FNOINVERT|FSHIFT, VK_ESCAPE, cmd 30436
    //   81 00 74 00 bc 98 00 00   → FVIRTKEY(+0x80 旧 FEND 位), VK_F5, cmd 0x98BC=39100
    //
    // 其中：
    //   Delete      → 结束任务（对应我方 CMD_END_TASK）
    //   Esc         → 退出（对应 CMD_EXIT）
    //   F5          → 刷新（原版 cmd 是 39100，不是菜单里的 30476；对应 CMD_REFRESH_NOW）
    //   Shift+Esc   → cmd 30436，**在 16 个 MENU 资源里都找不到**，语义未证实，故不复刻
    ACCEL acc[] = {
        { static_cast<BYTE>(FVIRTKEY | FNOINVERT), VK_DELETE, CMD_END_TASK    },
        { static_cast<BYTE>(FVIRTKEY),             VK_ESCAPE, CMD_EXIT        },
        // 第 4 条原版是 81 00 74 00：fVirt=0x81（FVIRTKEY 加 0x80 旧 FEND 位，
        // **不含 FSHIFT**），按键 VK_F5=0x74，cmd 39100。这里用 FVIRTKEY 即可。
        { static_cast<BYTE>(FVIRTKEY),             VK_F5,     CMD_REFRESH_NOW },
    };
    return CreateAcceleratorTableW(acc, sizeof(acc) / sizeof(acc[0]));
}

} // namespace tmp
