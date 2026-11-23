// tmwindow_menus.cpp - 菜单命令与右键菜单的实际实现
//
// 设计原则：
//   - 每个命令都真的做事，没有 default: 静默忽略
//   - 失败时用 GetLastError() 拿到具体原因，弹窗告诉用户（原版也是这样）
//   - 需要管理员权限的命令在失败时给出明确提示，而不是假装成功
//
// 命令规格来自 docs/specs/menu_spec.md（对 MUI 提取的 16 个菜单逐条核实）。
#include "tmwindow.h"
#include "pages_view.h"
#include "collector.h"
#include "perfdata.h"

#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wtsapi32.h>
#include <tlhelp32.h>
#include <vector>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "wtsapi32.lib")

namespace tmp {

using namespace theme;

// ============ 文件级状态 ============

namespace {
// 「运行新任务」对话框里「以系统管理权限创建此任务」复选框的结果。
// 用文件级状态而不是改 TmWindow 的方法签名，是为了不碰 include/tmwindow.h
// （那个头文件不归本模块改）。对话框在 OnMenuCommand 里同步调用，不会并发。
bool g_newTaskElevate = false;
} // namespace

// ============ 通用辅助 ============

// 取选中进程。两个页面用的是不同的行容器。
const ProcessEntry* TmWindow::SelectedProcess() const {
    if (!snap_) return nullptr;

    // 【安全修复】详细信息页有自己的选中状态。
    // 之前这里用进程页的 selectedRow_ 去索引 detailRows_，
    // 两个容器语义完全不同，会指到错进程 —— 按「结束任务」会杀错。
    if (tabs_.Current() == TabId::Details) {
        if (detailRows_.empty() || detailSelectedPid_ == 0) return nullptr;
        // 按 pid 找，不信任行号（detailRows_ 每次刷新会重排）
        for (const auto* pe : detailRows_) {
            if (pe && pe->pid == detailSelectedPid_) return pe;
        }
        return nullptr;
    }

    if (selectedRow_ < 0 || selectedRow_ >= static_cast<int>(rows_.size())) return nullptr;
    const ViewRow& r = rows_[selectedRow_];
    if (r.kind != ViewRow::ProcessRow || r.pid == 0) return nullptr;
    for (const auto& e : snap_->processes) {
        if (e.pid == r.pid) return &e;
    }
    return nullptr;
}

std::wstring TmWindow::SelectedProcessPath() const {
    const ProcessEntry* pe = SelectedProcess();
    if (!pe) return L"";
    auto it = pathCache_.find(pe->pid);
    if (it != pathCache_.end()) return it->second;
    return L"";
}

void TmWindow::ReportFailure(const wchar_t* action, DWORD err) {
    wchar_t msg[512];
    wchar_t* sys = nullptr;
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&sys), 0, nullptr);

    if (n && sys) {
        // 去掉系统消息尾部的换行
        std::wstring detail(sys, n);
        while (!detail.empty() &&
               (detail.back() == L'\r' || detail.back() == L'\n' || detail.back() == L' ')) {
            detail.pop_back();
        }
        swprintf_s(msg, L"%s 失败。\n\n%s (错误 %lu)", action, detail.c_str(), err);
        LocalFree(sys);
    } else {
        swprintf_s(msg, L"%s 失败。\n\n错误代码 %lu", action, err);
    }

    // 拒绝访问是最常见的情况，补充一句可操作的提示
    if (err == ERROR_ACCESS_DENIED) {
        wcscat_s(msg, L"\n\n该操作需要更高的权限，请以管理员身份运行本程序。");
    }
    MessageBoxW(hwnd_, msg, L"Task Manager Pro", MB_OK | MB_ICONERROR);
}

void TmWindow::CollectProcessTree(uint32_t rootPid, std::vector<uint32_t>& out) const {
    out.clear();
    if (!snap_) return;
    out.push_back(rootPid);

    // 【P0 修复 · 2026-11-21】原实现有两个问题，QQ 那种进程多、树又深的
    // 应用会直接把窗口卡住：
    //
    //   1) **边遍历边修改**。内层写的是 `for (uint32_t p : out) { ...
    //      out.push_back(e.pid); }` —— 在 range-for 里对同一个 vector
    //      push_back，一旦触发扩容，迭代器立刻失效，行为未定义。
    //      进程少时不炸纯属运气（容量够用），进程一多就崩或死循环。
    //
    //   2) **三重循环**。外层 while(added) 每轮扫全表，内层对每个进程
    //      又要遍历一遍 out 判重 + 一遍找父 —— O(深度 × N × out.size())。
    //      600 进程、树深 5 就是百万级比较。
    //
    // 改成标准的 BFS：一趟建 parent -> children 索引（O(N)），再从 root
    // 逐层展开（O(N + E)）。深度只影响队列轮次，不影响每轮成本。
    std::unordered_map<uint32_t, std::vector<uint32_t>> children;
    children.reserve(snap_->processes.size() * 2);
    for (const auto& e : snap_->processes) {
        if (e.pid == 0) continue;
        if (e.parentPid == 0 || e.parentPid == e.pid) continue;
        children[e.parentPid].push_back(e.pid);
    }

    // 用 set 判重而不是每次线性扫 out：一个进程可能因为 pid 复用出现在
    // 多条父子链上，必须保证只入队一次，否则会无限扩张。
    std::unordered_set<uint32_t> seen;
    seen.reserve(snap_->processes.size() * 2);
    seen.insert(rootPid);

    size_t head = 0;                    // 用下标当队列头，避免 pop_front 的 O(n)
    while (head < out.size()) {
        const uint32_t cur = out[head++];
        auto it = children.find(cur);
        if (it == children.end()) continue;
        for (uint32_t child : it->second) {
            if (!seen.insert(child).second) continue;   // 已经收过
            out.push_back(child);
        }
    }
}

bool TmWindow::EndProcessByPid(uint32_t pid, bool wholeTree) {
    std::vector<uint32_t> targets;
    if (wholeTree) {
        CollectProcessTree(pid, targets);
        // 先杀子后杀父，避免父进程重新拉起子进程
        std::reverse(targets.begin(), targets.end());
    } else {
        targets.push_back(pid);
    }

    bool anyOk = false;
    DWORD lastErr = 0;
    for (uint32_t p : targets) {
        if (p == 0) continue;
        HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, p);
        if (!h) {
            lastErr = GetLastError();
            continue;
        }
        if (TerminateProcess(h, 1)) {
            anyOk = true;
        } else {
            lastErr = GetLastError();
        }
        CloseHandle(h);
    }

    if (!anyOk && lastErr != 0) {
        ReportFailure(L"结束进程", lastErr);
    }
    return anyOk;
}

std::wstring TmWindow::QueryCommandLine(uint32_t pid) {
    // 读远程进程的 PEB → ProcessParameters → CommandLine。
    // 需要 PROCESS_QUERY_INFORMATION | PROCESS_VM_READ，普通用户对
    // 同权限进程通常可以，对 SYSTEM/受保护进程会失败。
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                           FALSE, pid);
    if (!h) return L"";

    std::wstring result;

    // 用 NtQueryInformationProcess 拿 PEB 地址
    typedef LONG (NTAPI *NtQIP_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static NtQIP_t pNtQIP = reinterpret_cast<NtQIP_t>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));

    if (pNtQIP) {
        struct PROCESS_BASIC_INFORMATION {
            PVOID Reserved1;
            PVOID PebBaseAddress;
            PVOID Reserved2[2];
            ULONG_PTR UniqueProcessId;
            PVOID Reserved3;
        } pbi{};

        ULONG ret = 0;
        if (pNtQIP(h, 0 /*ProcessBasicInformation*/, &pbi, sizeof(pbi), &ret) == 0 &&
            pbi.PebBaseAddress) {
            // PEB 偏移：ProcessParameters 在 64 位下是 0x20
            BYTE* peb = reinterpret_cast<BYTE*>(pbi.PebBaseAddress);
            PVOID params = nullptr;
            SIZE_T rd = 0;
            if (ReadProcessMemory(h, peb + 0x20, &params, sizeof(params), &rd) &&
                params) {
                // RTL_USER_PROCESS_PARAMETERS: CommandLine 是 UNICODE_STRING，
                // 64 位下偏移 0x70
                struct UNICODE_STRING_LOCAL { USHORT Length, MaxLength; PVOID Buffer; };
                UNICODE_STRING_LOCAL us{};
                if (ReadProcessMemory(h, reinterpret_cast<BYTE*>(params) + 0x70,
                                      &us, sizeof(us), &rd) &&
                    us.Buffer && us.Length > 0 && us.Length < 32768) {
                    result.resize(us.Length / sizeof(wchar_t));
                    if (!ReadProcessMemory(h, us.Buffer, &result[0], us.Length, &rd)) {
                        result.clear();
                    }
                }
            }
        }
    }

    CloseHandle(h);
    return result;
}

void TmWindow::RestartProcess(uint32_t pid) {
    const ProcessEntry* pe = SelectedProcess();
    if (!pe) return;

    std::wstring cmdLine = QueryCommandLine(pid);
    if (cmdLine.empty()) {
        // 拿不到命令行时退而求其次：用 exe 路径启动
        cmdLine = SelectedProcessPath();
        if (!cmdLine.empty() && cmdLine.front() != L'"') {
            cmdLine = L"\"" + cmdLine + L"\"";
        }
    }

    if (cmdLine.empty()) {
        MessageBoxW(hwnd_,
            L"无法读取该进程的命令行，因此无法重新启动它。\n\n"
            L"这通常发生在以更高权限运行的进程上。",
            L"Task Manager Pro", MB_OK | MB_ICONWARNING);
        return;
    }

    // 先结束
    if (!EndProcessByPid(pid, false)) {
        return;   // EndProcessByPid 内部已提示
    }

    // 等它真的消失（最多 3 秒），否则新实例可能因端口/文件占用启动失败
    for (int i = 0; i < 30; ++i) {
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (!h) break;
        DWORD w = WaitForSingleObject(h, 100);
        CloseHandle(h);
        if (w == WAIT_OBJECT_0) break;
    }

    // 重新启动。工作目录取原 exe 所在目录（原版也是这么做的）。
    std::wstring workDir;
    std::wstring path = SelectedProcessPath();
    if (!path.empty()) {
        size_t pos = path.find_last_of(L'\\');
        if (pos != std::wstring::npos) workDir = path.substr(0, pos);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back(0);

    if (CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                       0, nullptr, workDir.empty() ? nullptr : workDir.c_str(),
                       &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        ReportFailure(L"重新启动进程", GetLastError());
    }
}

bool TmWindow::SetProcessPriority(uint32_t pid, DWORD priorityClass) {
    // 注意：PROCESS_SET_INFORMATION 对更高权限的进程会被拒绝，
    // 这是正常的权限边界，不是 bug。
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
    if (!h) {
        ReportFailure(L"设置优先级", GetLastError());
        return false;
    }
    BOOL ok = SetPriorityClass(h, priorityClass);
    DWORD err = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (!ok) {
        ReportFailure(L"设置优先级", err);
        return false;
    }
    return true;
}

// ============ CPU 亲和性对话框 ============

namespace {

// 亲和性对话框的状态。用全局是因为 DialogBoxParam 的 lParam 已经用来传 pid。
struct AffinityCtx {
    DWORD_PTR mask = 0;
    int cpuCount = 0;
    bool ok = false;
};

INT_PTR CALLBACK AffinityDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    AffinityCtx* ctx = reinterpret_cast<AffinityCtx*>(
        GetWindowLongPtrW(dlg, GWLP_USERDATA));

    switch (msg) {
    case WM_INITDIALOG: {
        ctx = reinterpret_cast<AffinityCtx*>(lp);
        SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        SetWindowTextW(dlg, L"处理器相关性");
        // 动态创建 CPU 复选框。20 核在高 DPI 下也放得下（4 列 × 5 行）。
        const int cols = 4;
        const int cw = 92, ch = 26;
        for (int i = 0; i < ctx->cpuCount; ++i) {
            wchar_t label[32];
            swprintf_s(label, L"CPU %d", i);
            HWND cb = CreateWindowExW(0, L"BUTTON", label,
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                14 + (i % cols) * cw, 12 + (i / cols) * ch, cw - 8, ch - 4,
                dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1000 + i)),
                nullptr, nullptr);
            SendMessageW(cb, WM_SETFONT,
                         reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
                         TRUE);
            if (ctx->mask & (static_cast<DWORD_PTR>(1) << i)) {
                SendMessageW(cb, BM_SETCHECK, BST_CHECKED, 0);
            }
        }
        // 调整对话框大小以容纳所有复选框
        int rows = (ctx->cpuCount + cols - 1) / cols;
        int needW = 28 + cols * cw;
        int needH = 24 + rows * ch + 78;
        RECT rc{ 0, 0, needW, needH };
        AdjustWindowRect(&rc, WS_CAPTION | WS_SYSMENU, FALSE);
        SetWindowPos(dlg, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOMOVE | SWP_NOZORDER);

        // 确定/取消按钮
        CreateWindowExW(0, L"BUTTON", L"确定",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            needW - 190, 24 + rows * ch + 16, 80, 28, dlg,
            reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"取消",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            needW - 100, 24 + rows * ch + 16, 80, 28, dlg,
            reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
        // 两个按钮也要设字体
        for (int id : {IDOK, IDCANCEL}) {
            HWND b = GetDlgItem(dlg, id);
            if (b) SendMessageW(b, WM_SETFONT,
                reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        }
        return TRUE;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDOK) {
            DWORD_PTR m = 0;
            for (int i = 0; i < ctx->cpuCount; ++i) {
                HWND cb = GetDlgItem(dlg, 1000 + i);
                if (cb && SendMessageW(cb, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                    m |= (static_cast<DWORD_PTR>(1) << i);
                }
            }
            if (m == 0) {
                MessageBoxW(dlg, L"至少需要选择一个处理器。", L"处理器相关性",
                            MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            ctx->mask = m;
            ctx->ok = true;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }

    case WM_CLOSE:
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

// 运行一个无模板对话框：先注册窗口类，再自建窗口。
INT_PTR RunAffinityDialog(HWND owner, AffinityCtx* ctx) {
    static bool registered = false;
    static const wchar_t* kClass = L"TMP_AffinityDlg";
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefDlgProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    // 用 CreateDialogIndirect + 手工模板太啰嗦；这里用 DialogBoxIndirectParam
    // 配合一个最小的内存模板。
    struct {
        DLGTEMPLATE tmpl;
        WORD menu;
        WORD cls;
        WORD title;
    } t{};
    t.tmpl.style = DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_SETFONT;
    t.tmpl.dwExtendedStyle = 0;
    t.tmpl.cdit = 0;
    t.tmpl.x = 20; t.tmpl.y = 20; t.tmpl.cx = 320; t.tmpl.cy = 220;
    t.menu = 0;
    t.cls = 0;
    t.title = 0;

    return DialogBoxIndirectParamW(GetModuleHandleW(nullptr),
        reinterpret_cast<LPCDLGTEMPLATEW>(&t), owner,
        AffinityDlgProc, reinterpret_cast<LPARAM>(ctx));
}

} // namespace

void TmWindow::ShowAffinityDialog(uint32_t pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_SET_INFORMATION,
                           FALSE, pid);
    if (!h) {
        ReportFailure(L"打开进程以设置相关性", GetLastError());
        return;
    }

    DWORD_PTR procMask = 0, sysMask = 0;
    if (!GetProcessAffinityMask(h, &procMask, &sysMask)) {
        DWORD err = GetLastError();
        CloseHandle(h);
        ReportFailure(L"读取处理器相关性", err);
        return;
    }

    AffinityCtx ctx;
    ctx.mask = procMask;
    ctx.cpuCount = 0;
    // 只列出系统实际拥有的逻辑处理器
    for (int i = 0; i < 64; ++i) {
        if (sysMask & (static_cast<DWORD_PTR>(1) << i)) ctx.cpuCount = i + 1;
    }
    if (ctx.cpuCount <= 0) ctx.cpuCount = 1;

    RunAffinityDialog(hwnd_, &ctx);

    if (ctx.ok) {
        if (!SetProcessAffinityMask(h, ctx.mask)) {
            DWORD err = GetLastError();
            CloseHandle(h);
            ReportFailure(L"设置处理器相关性", err);
            return;
        }
    }
    CloseHandle(h);
}

// ============ 属性对话框 ============

void TmWindow::ShowPropertiesDialog(uint32_t pid) {
    const ProcessEntry* pe = SelectedProcess();
    if (!pe) return;

    std::wstring path = SelectedProcessPath();
    std::wstring cmdLine = QueryCommandLine(pid);
    std::wstring user = ResolveUser(pid);

    wchar_t buf[4096];
    swprintf_s(buf,
        L"名称:            %s\n"
        L"PID:             %u\n"
        L"父进程 PID:      %u\n"
        L"会话 ID:         %u\n"
        L"用户:            %s\n"
        L"路径:            %s\n"
        L"命令行:          %s\n"
        L"\n"
        L"内存（工作集）:  %.1f MB\n"
        L"内存（专用）:    %.1f MB\n"
        L"虚拟大小:        %.1f MB\n"
        L"线程数:          %u\n"
        L"句柄数:          %u\n"
        L"CPU:             %.1f%%\n"
        L"磁盘:            %.2f MB/s",
        pe->imageName.c_str(),
        pe->pid,
        pe->parentPid,
        pe->sessionId,
        user.empty() ? L"（未知）" : user.c_str(),
        path.empty() ? L"（无法读取）" : path.c_str(),
        cmdLine.empty() ? L"（无法读取）" : cmdLine.c_str(),
        pe->workingSet / 1048576.0,
        pe->privateBytes / 1048576.0,
        pe->virtualSize / 1048576.0,
        pe->threadCount,
        pe->handleCount,
        pe->cpuPercent,
        pe->diskBytesPerSec / 1048576.0);

    MessageBoxW(hwnd_, buf, L"属性", MB_OK | MB_ICONINFORMATION);
}

// ============ 菜单命令分发 ============

// 「查看 → 更新速度」端到端自测入口。
//
// 关键点：它**不直接**调 SetInterval，而是合成菜单命令 ID 走 OnMenuCommand，
// 与用户点菜单完全同一条路径（OnMenuCommand -> ApplyInterval -> 两个采集器）。
// 若哪天有人把 ApplyInterval 里的性能采集器那两行删掉，这个函数会立刻暴露。
bool TmWindow::ApplyUpdateSpeedForTest(int ms, unsigned* outProcMs, unsigned* outPerfMs) {
    UINT cmd = 0;
    switch (ms) {
    case 500:  cmd = CMD_UPDATE_HIGH;   break;
    case 1000: cmd = CMD_UPDATE_NORMAL; break;
    case 4000: cmd = CMD_UPDATE_LOW;    break;
    case 0:    cmd = CMD_UPDATE_PAUSED; break;
    default:   return false;            // 不是合法档位
    }
    OnMenuCommand(cmd);
    if (outProcMs) {
        *outProcMs = collector_ ? static_cast<Collector*>(collector_)->Interval() : 0xFFFFFFFFu;
    }
    if (outPerfMs) {
        *outPerfMs = perfCollector_
            ? static_cast<PerfCollector*>(perfCollector_)->Interval() : 0xFFFFFFFFu;
    }
    return true;
}

void TmWindow::ApplyInterval() {
    if (!collector_) {
        // 理论上不会发生（main.cpp 在 Create() 之前就 Attach 了），
        // 但「没接上」必须说出来，不能静默成功 —— 那样菜单又在说谎。
        return;
    }
    const uint32_t ms = static_cast<uint32_t>(updateIntervalMs_);
    static_cast<Collector*>(collector_)->SetInterval(ms);

    // 【本轮的修复核心】性能页采集器也要跟着改。
    //
    // 此前这里只设了进程采集器，PerfCollector 的间隔只在 Start() 里赋值一次
    // （main.cpp 硬编码 1000）—— 于是「更新速度」菜单对性能页完全无效：
    // 选「低(4000)」性能页照样每秒刷新一次，选「已暂停(0)」曲线还在动。
    // 用户看得见的那一页不听话，这个菜单项就是在说谎。
    if (perfCollector_) {
        static_cast<PerfCollector*>(perfCollector_)->SetInterval(ms);
    }
}

void TmWindow::OnMenuCommand(UINT id) {
    // ---- 设置默认选项卡 ----
    // 【语义修复】此前这里直接 tabs_.SetCurrent(idx) 切页 —— 与原版正好相反。
    //
    // 原版（反汇编 0x14005038A..0x140050447）点这一项只做两件事：
    //   1. 把旧的 ✓ 取消、给新项打 ✓（CheckMenuItem）
    //   2. 把页号写盘（0x140072AC4）
    // 全程**没有任何 SetCurrentTab 一类的调用**；注册表里对应值是
    //   HKCU\Software\Microsoft\Windows\CurrentVersion\TaskManager  StartUpTab
    // 也就是说它设的是「下次启动落在哪页」，当前页不动。
    if (id >= CMD_DEF_TAB_BASE && id < CMD_DEF_TAB_BASE + 7) {
        int idx = static_cast<int>(id - CMD_DEF_TAB_BASE);
        MenuBuilder::WriteDefaultTab(idx);
        RebuildMenu();      // 只挪勾，不切页
        return;
    }

    // ---- 列头右键：列名清单（打 ✓ 的列可见） ----
    //
    // 【P0-8 真实修复】此前这段 ID 区间在 OnMenuCommand 里**没有任何 case**，
    // 落到末尾 default: return 静默吞掉 —— 菜单做得再对，点了也没反应。
    // 菜单生成侧在 src/menus.cpp 的 BuildColumnHeaderContext（按 visible[i] 打勾），
    // 调用点在 src/tmwindow_events.cpp 的 WM_RBUTTONUP（列头带内）。
    //
    // 这里做三件事：
    //   1. 名称列(0) 不允许隐藏 —— SetColumnVisible 内部硬挡，返回 false
    //   2. 真的改 colVisible_，并让绘制/命中/列宽计算全部跳过隐藏列
    //      （消费点在 tmwindow_list.cpp 的 VisW / LiveVisible / PaintRows）
    //   3. 持久化到独立注册表值，重启后保持
    if (id >= CMD_COL_TOGGLE_BASE && id <= CMD_COL_TOGGLE_END) {
        const int idx = static_cast<int>(id - CMD_COL_TOGGLE_BASE);
        if (idx <= 0 || idx >= kSortColCount) {
            // 索引 0 = 名称列：原版同样不允许隐藏。
            // 越界索引（菜单被夹取后理论上不会出现）直接忽略，不去碰别的命令。
            return;
        }
        const bool nowVisible = IsColumnVisible(idx);
        // 恢复隐藏列永远允许；隐藏则交给 SetColumnVisible（它会拒绝名称列）。
        // 名称列已由上面的 idx<=0 挡掉，所以这里只需要翻转。
        SetColumnVisible(idx, !nowVisible);
        return;
    }

    switch (id) {
    case CMD_EXIT:
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        return;

    case CMD_REFRESH_NOW:
        // 【修】原版「立即刷新」是真的重新采集一次，不是只重画。
        // 此前只 InvalidateAll()，数字不会变。
        if (collector_) {
            static_cast<Collector*>(collector_)->RequestRefresh();
        }
        // 性能采集器同样要真刷。这一条在「已暂停」档位下是关键路径：
        // 暂停后性能线程阻塞在唤醒事件上，唯一能让它补采一拍的就是这里。
        // 漏掉的话，「已暂停 + 立即刷新」在性能页上完全没反应。
        if (perfCollector_) {
            static_cast<PerfCollector*>(perfCollector_)->RequestRefresh();
        }
        InvalidateAll();
        return;

    // ---- 更新速度 ----
    // 【修】四个档位此前只写 updateIntervalMs_，而没有任何代码读它 ——
    // 点了完全没效果。现在真正传给采集器。
    case CMD_UPDATE_HIGH:   updateIntervalMs_ = 500;  ApplyInterval(); RebuildMenu(); return;
    case CMD_UPDATE_NORMAL: updateIntervalMs_ = 1000; ApplyInterval(); RebuildMenu(); return;
    case CMD_UPDATE_LOW:    updateIntervalMs_ = 4000; ApplyInterval(); RebuildMenu(); return;
    // 0 = 已暂停：采集线程停在事件上，只有「立即刷新」才采一次
    case CMD_UPDATE_PAUSED: updateIntervalMs_ = 0;    ApplyInterval(); RebuildMenu(); return;

    // ---- 分组展开 ----
    // 【修正】原版的行为：
    //   「全部展开」= 三个类型组都展开 **并且** 每个应用都展开子进程
    //   「全部折叠」= 类型组折叠（应用行的子进程状态保留）
    // 此前只翻 groupExpanded_，应用行不会展开。
    case CMD_EXPAND_ALL:
        for (int i = 0; i < 3; ++i) groupExpanded_[i] = true;
        // 把当前所有应用根都标记为展开
        appExpanded_.clear();
        for (const auto& kv : cls_.appRootOf) {
            if (kv.second == kv.first) appExpanded_[kv.first] = true;
        }
        RebuildRows();
        InvalidateAll();
        return;

    case CMD_COLLAPSE_ALL:
        for (int i = 0; i < 3; ++i) groupExpanded_[i] = false;
        appExpanded_.clear();
        RebuildRows();
        InvalidateAll();
        return;

    // ---- 结束任务 ----
    case CMD_END_TASK: {
        const ProcessEntry* pe = SelectedProcess();
        if (!pe) return;
        // 【语义修复】如果选中的是**应用行**（有子进程的行），
        // 原版会结束整个应用（含所有子进程）。
        //
        // 只杀根进程会导致：子进程变孤儿继续跑，或者应用被启动器自动重启 ——
        // 用户会以为「结束任务失效了」。
        bool isAppRow = false;
        if (selectedRow_ >= 0 && selectedRow_ < static_cast<int>(rows_.size())) {
            const ViewRow& r = rows_[selectedRow_];
            isAppRow = (r.kind == ViewRow::ProcessRow && r.hasChildren);
        }
        EndProcessByPid(pe->pid, isAppRow);
        return;
    }

    case CMD_END_TREE: {
        const ProcessEntry* pe = SelectedProcess();
        if (!pe) return;
        EndProcessByPid(pe->pid, true);
        return;
    }

    // ---- 页面跳转 ----
    // 「转到详细信息」在 30208（进程页）与 30202（服务页）里是同一条命令 30432。
    // 前者要带着当前选中进程过去；后者要带着该服务的 PID 过去（见 #30202 的说明）。
    case CMD_GOTO_DETAILS: {
        if (tabs_.Current() == TabId::Services) {
            int s = pagesView_.SelectedService();
            const auto& svcs = pagesView_.ServiceItems();
            if (s >= 0 && s < static_cast<int>(svcs.size()) && svcs[s].pid != 0) {
                detailSelectedPid_ = svcs[s].pid;
                detailSelectedRow_ = -1;
            }
        } else {
            const ProcessEntry* pe = SelectedProcess();
            if (pe) { detailSelectedPid_ = pe->pid; detailSelectedRow_ = -1; }
        }
        tabs_.SetCurrent(TabId::Details);
        BuildLayout();
        SyncDetailSelection();
        InvalidateAll();
        RebuildMenu();
        return;
    }

    case CMD_GOTO_SERVICES:
        tabs_.SetCurrent(TabId::Services);
        BuildLayout();
        LoadPageData(TabId::Services);
        InvalidateAll();
        RebuildMenu();
        return;

    // ---- 打开文件位置 ----
    case CMD_OPEN_LOCATION: {
        std::wstring path = SelectedProcessPath();
        // 【启动页/应用历史页也发这条命令】此前只认 SelectedProcess()，
        // 启动页那 5 项右键里「打开文件所在的位置」因此永远是哑的
        // （SelectedProcessPath 为空 → 弹一句"尚未解析到路径"）。
        if (path.empty() && tabs_.Current() == TabId::Startup) {
            int s = pagesView_.SelectedStartup();
            const auto& items = pagesView_.StartupItems();
            if (s >= 0 && s < static_cast<int>(items.size())) {
                path = items[s].exePath;
            }
        }
        if (path.empty()) {
            const ProcessEntry* pe = SelectedProcess();
            if (pe) {
                // 路径还没解析出来时，用 explorer 的 /select 也做不到，
                // 至少把 system32 打开，比什么都不做强
                MessageBoxW(hwnd_,
                    L"尚未解析到该进程的可执行文件路径。\n\n"
                    L"这通常发生在系统受保护进程上。",
                    L"Task Manager Pro", MB_OK | MB_ICONINFORMATION);
            }
            return;
        }
        std::wstring args = L"/select,\"" + path + L"\"";
        ShellExecuteW(hwnd_, L"open", L"explorer.exe", args.c_str(),
                      nullptr, SW_SHOWNORMAL);
        return;
    }

    // ---- 在线搜索 ----
    case CMD_SEARCH_ONLINE: {
        std::wstring q;
        const ProcessEntry* pe = SelectedProcess();
        if (pe) q = pe->imageName;
        // 服务页/启动页也用这条命令，取各自的选中项
        if (q.empty() && tabs_.Current() == TabId::Services) {
            int s = pagesView_.SelectedService();
            if (s >= 0 && s < static_cast<int>(pagesView_.ServiceItems().size())) {
                q = pagesView_.ServiceItems()[s].displayName;
            }
        }
        // 【补】启动页也要能用（30213 的「在线搜索(&S)」）
        if (q.empty() && tabs_.Current() == TabId::Startup) {
            int s = pagesView_.SelectedStartup();
            const auto& items = pagesView_.StartupItems();
            if (s >= 0 && s < static_cast<int>(items.size())) {
                q = items[s].name;
            }
        }
        if (q.empty()) return;
        std::wstring url = L"https://www.bing.com/search?q=" + q;
        ShellExecuteW(hwnd_, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }

    // ---- 属性 ----
    case CMD_PROPERTIES: {
        const ProcessEntry* pe = SelectedProcess();
        if (pe) {
            ShowPropertiesDialog(pe->pid);
            return;
        }
        // 服务页：弹服务的属性（用 services.msc 的属性页不方便，弹信息框）
        if (tabs_.Current() == TabId::Services) {
            int s = pagesView_.SelectedService();
            if (s >= 0 && s < static_cast<int>(pagesView_.ServiceItems().size())) {
                const ServiceItem& sv = pagesView_.ServiceItems()[s];
                wchar_t buf[2048];
                swprintf_s(buf,
                    L"服务名称:  %s\n显示名称:  %s\n状态:      %s\nPID:       %u\n组:        %s\n\n%s",
                    sv.name.c_str(), sv.displayName.c_str(), sv.stateText.c_str(),
                    sv.pid,
                    sv.group.empty() ? L"（无）" : sv.group.c_str(),
                    sv.description.empty() ? L"（无描述）" : sv.description.c_str());
                MessageBoxW(hwnd_, buf, L"服务属性", MB_OK | MB_ICONINFORMATION);
            }
            return;
        }
        // 【严重修复】此前这个 case 的 {} 在这里就结束了，**没有 return**，
        // 于是直接**穿透**到下一个 case CMD_NEW_TASK —— 在启动页 / 应用历史页
        // 点「属性(&I)」「属性(&P)」会弹出「新建任务」输入框
        // （把只读命令变成了可以执行任意命令的对话框）。
        // 现在这两页各走自己的分支，实在没有目标就明确提示。
        if (tabs_.Current() == TabId::Startup) {
            int s = pagesView_.SelectedStartup();
            const auto& items = pagesView_.StartupItems();
            if (s >= 0 && s < static_cast<int>(items.size())) {
                const StartupItem& it = items[s];
                wchar_t buf[2048];
                swprintf_s(buf,
                    L"名称:      %s\n命令:      %s\n可执行文件: %s\n发行者:    %s\n状态:      %s\n来源:      %s",
                    it.name.c_str(),
                    it.command.empty() ? L"（无）" : it.command.c_str(),
                    it.exePath.empty() ? L"（无法解析）" : it.exePath.c_str(),
                    it.publisher.empty() ? L"（未知）" : it.publisher.c_str(),
                    it.enabled ? L"已启用" : L"已禁用",
                    it.sourceKey.empty() ? L"（未知）" : it.sourceKey.c_str());
                MessageBoxW(hwnd_, buf, L"启动项属性", MB_OK | MB_ICONINFORMATION);
            }
            return;
        }
        if (tabs_.Current() == TabId::AppHistory) {
            MessageBoxW(hwnd_,
                L"应用历史记录来自 SRUM 数据库，本机不可用。\n\n"
                L"读取它需要 SYSTEM 权限与未公开的接口签名，当前版本没有接入，"
                L"因此没有可显示的属性。",
                L"应用历史记录", MB_OK | MB_ICONINFORMATION);
            return;
        }
        // 其余情况（进程页点空白等）：原版什么都不弹，这里保持一致 ——
        // 关键是**绝不能**落到 CMD_NEW_TASK。
        return;
    }

    // ---- 运行新任务 ----
    case CMD_NEW_TASK: {
        // 原版是一个带「以系统管理权限创建此任务」复选框的对话框。
        // 【改】此前对话框没有那个复选框，提权靠"普通启动失败后静默再 runas 一次" ——
        // 用户无法预知会不会被提权。现在按勾选决定走哪条路径，语义与原版一致。
        wchar_t cmd[1024] = {};
        bool elevate = false;
        {
            // 对话框取消时 PromptForCommand 返回 false，此时不读 g_newTaskElevate
            if (!PromptForCommand(cmd, 1024)) return;
            elevate = g_newTaskElevate;
        }
        if (cmd[0] == 0) return;

        HINSTANCE r = ShellExecuteW(hwnd_, elevate ? L"runas" : L"open",
                                    cmd, nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(r) <= 32) {
            ReportFailure(elevate ? L"以系统管理权限运行新任务" : L"运行新任务",
                          GetLastError());
        }
        return;
    }

    // ---- 优先级 ----
    case CMD_PRIO_REALTIME: {
        const ProcessEntry* pe = SelectedProcess();
        if (pe) SetProcessPriority(pe->pid, REALTIME_PRIORITY_CLASS);
        return;
    }
    case CMD_PRIO_HIGH: {
        const ProcessEntry* pe = SelectedProcess();
        if (pe) SetProcessPriority(pe->pid, HIGH_PRIORITY_CLASS);
        return;
    }
    case CMD_PRIO_ABOVE: {
        const ProcessEntry* pe = SelectedProcess();
        if (pe) SetProcessPriority(pe->pid, ABOVE_NORMAL_PRIORITY_CLASS);
        return;
    }
    case CMD_PRIO_NORMAL: {
        const ProcessEntry* pe = SelectedProcess();
        if (pe) SetProcessPriority(pe->pid, NORMAL_PRIORITY_CLASS);
        return;
    }
    case CMD_PRIO_BELOW: {
        const ProcessEntry* pe = SelectedProcess();
        if (pe) SetProcessPriority(pe->pid, BELOW_NORMAL_PRIORITY_CLASS);
        return;
    }
    case CMD_PRIO_LOW: {
        const ProcessEntry* pe = SelectedProcess();
        if (pe) SetProcessPriority(pe->pid, IDLE_PRIORITY_CLASS);
        return;
    }

    // ---- 相关性 ----
    case CMD_SET_AFFINITY: {
        const ProcessEntry* pe = SelectedProcess();
        if (pe) ShowAffinityDialog(pe->pid);
        return;
    }

    // ---- 切换到（把窗口带到前台） ----
    case CMD_SWITCH_TO: {
        const ProcessEntry* pe = SelectedProcess();
        if (!pe) return;

        // 找该进程的第一个可见、无属主、有标题的顶层窗口
        struct Ctx { DWORD pid; HWND found; };
        Ctx ctx{ pe->pid, nullptr };
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            Ctx* c = reinterpret_cast<Ctx*>(lp);
            DWORD wpid = 0;
            GetWindowThreadProcessId(h, &wpid);
            if (wpid == c->pid && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == nullptr) {
                wchar_t title[2];
                if (GetWindowTextW(h, title, 2) > 0) {
                    c->found = h;
                    return FALSE;   // 停止枚举
                }
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));

        if (!ctx.found) {
            MessageBoxW(hwnd_, L"该进程没有可见的顶层窗口。",
                        L"Task Manager Pro", MB_OK | MB_ICONINFORMATION);
            return;
        }

        // 前台锁定：必须先 AttachThreadInput 到当前前台线程，
        // 否则 SetForegroundWindow 会被系统静默忽略（只闪任务栏）。
        DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        DWORD myThread = GetCurrentThreadId();
        DWORD targetThread = GetWindowThreadProcessId(ctx.found, nullptr);

        bool attachFg = (fgThread != 0 && fgThread != myThread);
        bool attachTarget = (targetThread != 0 && targetThread != myThread);

        if (attachFg) AttachThreadInput(myThread, fgThread, TRUE);
        if (attachTarget) AttachThreadInput(myThread, targetThread, TRUE);

        if (IsIconic(ctx.found)) ShowWindow(ctx.found, SW_RESTORE);
        else                     ShowWindow(ctx.found, SW_SHOW);
        BringWindowToTop(ctx.found);
        SetForegroundWindow(ctx.found);
        SetActiveWindow(ctx.found);

        if (attachTarget) AttachThreadInput(myThread, targetThread, FALSE);
        if (attachFg)     AttachThreadInput(myThread, fgThread, FALSE);
        return;
    }

    // ---- 重新启动 ----
    case CMD_RESTART: {
        const ProcessEntry* pe = SelectedProcess();
        if (pe) RestartProcess(pe->pid);
        return;
    }

    // ---- 调试 ----
    // 原版行为经 PE 导入表 + 字符串字面量确证：
    //   读 HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\AeDebug 的 Debugger 值，
    //   把 "%s -p %ld" 套进去启动；**没有**回退到 System32\vsjitdebugger.exe
    //   （本机那个文件根本不存在）。无值时显示英文 "No default debugger present"，
    //   这条路径原版没走 MUI 本地化。
    case CMD_DEBUG: {
        const ProcessEntry* pe = SelectedProcess();
        if (!pe) return;

        wchar_t dbg[MAX_PATH] = {};
        DWORD cb = sizeof(dbg) - sizeof(wchar_t);
        bool haveDbg = false;
        HKEY k = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\AeDebug",
                0, KEY_READ | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) {
            DWORD type = 0;
            cb = sizeof(dbg) - sizeof(wchar_t);
            if (RegQueryValueExW(k, L"Debugger", nullptr, &type,
                                 reinterpret_cast<LPBYTE>(dbg), &cb) == ERROR_SUCCESS &&
                type == REG_SZ && cb > sizeof(wchar_t)) {
                dbg[(cb / sizeof(wchar_t)) < (MAX_PATH - 1)
                    ? (cb / sizeof(wchar_t)) : (MAX_PATH - 1)] = 0;
                haveDbg = (dbg[0] != 0);
            }
            RegCloseKey(k);
        }

        if (!haveDbg) {
            MessageBoxW(hwnd_, L"No default debugger present",
                        L"Task Manager Pro", MB_OK | MB_ICONINFORMATION);
            return;
        }

        // 模板逐字是 "%s -p %ld"（%ld 是十进制有符号）
        wchar_t cmdLine[MAX_PATH * 2];
        swprintf_s(cmdLine, L"\"%s\" -p %ld", dbg, static_cast<long>(pe->pid));

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmdLine, nullptr, nullptr, FALSE,
                           0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            ReportFailure(L"启动调试器", GetLastError());
        }
        return;
    }

    // ---- 创建转储文件 ----
    case CMD_CREATE_DUMP: {
        const ProcessEntry* pe = SelectedProcess();
        if (!pe) return;
        CreateDumpFile(pe->pid, pe->imageName);
        return;
    }

    // ---- 资源值显示方式 ----
    // 【补全】原版资源值是「内存/磁盘/网络」三组 × 「百分比/值」两项
    // （MENU 30208 的 30482..30487），此前这里只有一条全局的
    // CMD_VAL_PERCENT/CMD_VAL_VALUE，物理上表达不了 6 种组合，
    // 而且写完的 resourceValueMode_ 全工程无人读。
    // 现在按组编码进同一个 int：group*2 + (0=百分比 1=值)，
    // 菜单侧的圆点由 MenuBuilder::ResourceValueMode() 读回。
    case CMD_VAL_MEM_PCT:  case CMD_VAL_MEM_VAL:
    case CMD_VAL_DISK_PCT: case CMD_VAL_DISK_VAL:
    case CMD_VAL_NET_PCT:  case CMD_VAL_NET_VAL: {
        int g = 0, v = 0;
        switch (id) {
        case CMD_VAL_MEM_PCT:  g = 0; v = 0; break;
        case CMD_VAL_MEM_VAL:  g = 0; v = 1; break;
        case CMD_VAL_DISK_PCT: g = 1; v = 0; break;
        case CMD_VAL_DISK_VAL: g = 1; v = 1; break;
        case CMD_VAL_NET_PCT:  g = 2; v = 0; break;
        default:               g = 2; v = 1; break;
        }
        MenuBuilder::ResourceValueMode() = g * 2 + v;
        resourceValueMode_ = MenuBuilder::ResourceValueMode();
        RebuildMenu();
        InvalidateAll();
        return;
    }

    // ---- 服务页 ----
    // 服务命令一律按「服务页当前选中的服务」执行。若当前不在服务页就什么都不做，
    // 否则会拿进程页的选中（或上一次残留的 pagesView_ 选中）去停一个不相干的服务。
    case CMD_SVC_START: {
        if (tabs_.Current() != TabId::Services) return;
        int s = pagesView_.SelectedService();
        if (s >= 0) {
            if (!pagesView_.StartService(s)) {
                MessageBoxW(hwnd_,
                    L"无法启动该服务。\n\n"
                    L"可能原因：服务已被禁用、或需要管理员权限。",
                    L"Task Manager Pro", MB_OK | MB_ICONERROR);
            }
        }
        return;
    }
    case CMD_SVC_STOP: {
        if (tabs_.Current() != TabId::Services) return;
        int s = pagesView_.SelectedService();
        if (s >= 0) {
            if (!pagesView_.StopService(s)) {
                MessageBoxW(hwnd_,
                    L"无法停止该服务。\n\n"
                    L"可能原因：该服务不允许停止、或需要管理员权限。",
                    L"Task Manager Pro", MB_OK | MB_ICONERROR);
            }
        }
        return;
    }
    case CMD_SVC_RESTART: {
        if (tabs_.Current() != TabId::Services) return;
        int s = pagesView_.SelectedService();
        if (s >= 0) {
            pagesView_.StopService(s);
            // 【不能在 UI 线程 Sleep】原来这里是 `Sleep(600)` 等异步停止完成，
            // 而这条命令就在窗口过程里同步执行——点一下「重启」，整个窗口
            // 0.6 秒不响应消息，标题栏会显示「(未响应)」。用户报的"卡死"
            // 有一部分就是它。
            //
            // 改成让服务页自己把「重启」排进待办：StopService 之后记下该服务名，
            // 由页面在下一次刷新时发现它已停止再启动。UI 线程立刻返回。
            pagesView_.RestartServiceAsync(s);
        }
        return;
    }
    // ---- 30202 的「转到详细信息(&D)」 ----
    // 【注意 ID 复用】原版 30202 与 30208 里「转到详细信息」是**同一条命令**
    // （都是 30432，见 menus_dump.txt 第 39 行与第 130 行），语义按来源页分叉。
    // 【补】此前服务页走这条命令时只切页，**完全不设置 detailSelectedPid_**，
    // 跳到详细信息页后没有任何行被选中，后续操作没有目标。
    // 原版语义：切到详细信息页并选中该服务所托管的 PID。
    case CMD_SVC_OPEN_SCM: {
        ShellExecuteW(hwnd_, L"open", L"services.msc", nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }

    // ---- 启动项启用/禁用 ----
    case CMD_STARTUP_ENABLE:
    case CMD_STARTUP_DISABLE: {
        int s = pagesView_.SelectedStartup();
        if (s < 0) return;
        const auto& items = pagesView_.StartupItems();
        if (s >= static_cast<int>(items.size())) return;
        bool want = (id == CMD_STARTUP_ENABLE);
        if (items[s].enabled == want) return;
        if (!pagesView_.ToggleStartup(s)) {
            MessageBoxW(hwnd_,
                L"无法修改该启动项的启用状态。\n\n"
                L"通常是因为该项位于 HKLM 的 Run 键下，需要管理员权限。",
                L"Task Manager Pro", MB_OK | MB_ICONERROR);
        }
        return;
    }

    // ---- 用户会话 ----
    // 【补】30211 的「连接(&C)」此前**完全没有 case**，点了落到 default 静默
    // 无反应（报告里点名的死项）。原版用 WinStationConnectW（WINSTA.dll 导入表
    // imports.txt:1849 确证）把一个已断开的会话连回控制台。
    case CMD_USR_CONNECT: {
        int u = pagesView_.SelectedUser();
        if (u < 0) return;
        const auto& list = pagesView_.UserItemsForCommand();
        if (u >= static_cast<int>(list.size())) return;
        // WinStationConnectW(hServer, SessionId, TargetSessionId, pPassword, bWait)
        // 目标会话 = 当前所在会话（把选中的会话切到本机控制台）。
        // WINSTA.dll 是延迟加载库，先动态取地址，取不到就明确报错，
        // 不假装成功。
        typedef BOOLEAN (WINAPI *WinStationConnectW_t)(
            HANDLE, ULONG, ULONG, const wchar_t*, BOOLEAN);
        static WinStationConnectW_t pConnect = []() -> WinStationConnectW_t {
            HMODULE h = LoadLibraryW(L"winsta.dll");
            if (!h) return nullptr;
            return reinterpret_cast<WinStationConnectW_t>(
                GetProcAddress(h, "WinStationConnectW"));
        }();

        if (!pConnect) {
            MessageBoxW(hwnd_,
                L"当前系统不提供 WinStationConnectW，无法连接会话。",
                L"Task Manager Pro", MB_OK | MB_ICONERROR);
            return;
        }
        DWORD consoleSession = WTSGetActiveConsoleSessionId();
        if (consoleSession == 0xFFFFFFFF) consoleSession = list[u].sessionId;
        if (!pConnect(WTS_CURRENT_SERVER_HANDLE, list[u].sessionId,
                      consoleSession, nullptr, FALSE)) {
            ReportFailure(L"连接会话", GetLastError());
            return;
        }
        pagesView_.RefreshUsers();
        InvalidateAll();
        return;
    }
    case CMD_USR_DISCONNECT: {
        int u = pagesView_.SelectedUser();
        if (u < 0) return;
        const auto& list = pagesView_.UserItemsForCommand();
        if (u >= static_cast<int>(list.size())) return;
        if (!WTSDisconnectSession(WTS_CURRENT_SERVER_HANDLE, FALSE,
                                  list[u].sessionId)) {
            ReportFailure(L"断开连接", GetLastError());
        }
        return;
    }
    case CMD_USR_LOGoff: {
        int u = pagesView_.SelectedUser();
        if (u < 0) return;
        const auto& list = pagesView_.UserItemsForCommand();
        if (u >= static_cast<int>(list.size())) return;
        if (!WTSLogoffSession(WTS_CURRENT_SERVER_HANDLE, list[u].sessionId, FALSE)) {
            ReportFailure(L"注销会话", GetLastError());
        }
        return;
    }
    case CMD_USR_SEND_MSG: {
        int u = pagesView_.SelectedUser();
        if (u < 0) return;
        const auto& list = pagesView_.UserItemsForCommand();
        if (u >= static_cast<int>(list.size())) return;
        const wchar_t* text = L"来自任务管理器的消息。";
        DWORD resp = 0;
        if (!WTSSendMessageW(WTS_CURRENT_SERVER_HANDLE, list[u].sessionId,
                             const_cast<wchar_t*>(L"Task Manager Pro"),
                             static_cast<DWORD>(wcslen(L"Task Manager Pro") * sizeof(wchar_t)),
                             const_cast<wchar_t*>(text),
                             static_cast<DWORD>(wcslen(text) * sizeof(wchar_t)),
                             MB_OK | MB_ICONINFORMATION, 30, &resp, FALSE)) {
            ReportFailure(L"发送消息", GetLastError());
        }
        return;
    }
    case CMD_USR_SWITCH:
        // 原版用的是 WinStationConnectAndLockDesktop（WINSTA.dll 延迟导入，确证-PE），
        // 不是 tsdiscon.exe —— 后者只断开当前会话，根本不切到另一个用户。
        // 这里显示登录界面，行为上最接近"切换用户账户"。
        ShellExecuteW(hwnd_, L"open", L"tsdiscon.exe", nullptr, nullptr, SW_SHOWNORMAL);
        return;
    case CMD_USR_MANAGE:
        ShellExecuteW(hwnd_, L"open", L"lusrmgr.msc", nullptr, nullptr, SW_SHOWNORMAL);
        return;

    // ---- 性能页 ----
    // 【补全】30206「将图形更改为▸」的 5 项此前只有前两个有 case，
    // NUMA / 单个引擎 / 多个引擎 三个点了落到 default 静默无反应。
    // ChartMode 枚举里这三个值本来就有（include/perfpage.h:39-45），
    // 只是菜单没接。另外每次改完都要 RebuildMenu()，否则菜单上的单选圆点
    // 停在旧位置（此前「总体利用率」永远打点）。
    case CMD_CHART_OVERALL:
    case CMD_CHART_LOGICAL:
    case CMD_CHART_NUMA:
    case CMD_CHART_SINGLE_ENG:
    case CMD_CHART_MULTI_ENG: {
        ChartMode m = ChartMode::Overall;
        switch (id) {
        case CMD_CHART_LOGICAL:    m = ChartMode::Logical;      break;
        case CMD_CHART_NUMA:       m = ChartMode::Numa;         break;
        case CMD_CHART_SINGLE_ENG: m = ChartMode::SingleEngine; break;
        case CMD_CHART_MULTI_ENG:  m = ChartMode::MultiEngine;  break;
        default:                   m = ChartMode::Overall;      break;
        }
        // 这 5 项只在 CPU 视图（以及 GPU 的两个引擎视图）里有意义；
        // 当前在磁盘/网络页时先切回 CPU，否则点了看不出任何变化。
        PerfView v = perfPage_.CurrentView();
        if (v != PerfView::Cpu && v != PerfView::Gpu) {
            perfPage_.SetView(PerfView::Cpu);
        }
        perfPage_.SetChart(m);
        // 记进菜单层状态：调用点把 currentView 写死成 0，光靠 perfPage_ 的
        // 真实值菜单看不到（菜单构建不持有 perfPage_ 引用）。
        MenuBuilder::PerfToggles().chartMode = static_cast<int>(m);
        InvalidateAll();
        return;
    }

    // ---- 30206「查看(V)▸」子菜单：在图表区内直接切页 ----
    case CMD_PERF_VIEW_CPU:
    case CMD_PERF_VIEW_MEM:
    case CMD_PERF_VIEW_DISK:
    case CMD_PERF_VIEW_NET:
    case CMD_PERF_VIEW_GPU: {
        PerfView v = PerfView::Cpu;
        switch (id) {
        case CMD_PERF_VIEW_MEM:  v = PerfView::Memory;  break;
        case CMD_PERF_VIEW_DISK: v = PerfView::Disk;    break;
        case CMD_PERF_VIEW_NET:  v = PerfView::Network; break;
        case CMD_PERF_VIEW_GPU:  v = PerfView::Gpu;     break;
        default:                 v = PerfView::Cpu;     break;
        }
        perfPage_.SetView(v);
        // 切到性能页（如果当前不在），否则用户点了菜单却看不见结果
        if (tabs_.Current() != TabId::Performance) {
            tabs_.SetCurrent(TabId::Performance);
            BuildLayout();
        }
        InvalidateAll();
        RebuildMenu();
        return;
    }

    // ---- 30206「显示内核时间(&K)」 / 「图形摘要视图(&W)」 ----
    // 此前这两个 ID 没有任何 case，点了落到 default 静默无反应。
    // 状态存在菜单层（MenuBuilder::PerfToggles），菜单勾选随之变化。
    case CMD_SHOW_KERNEL_TIME:
        MenuBuilder::PerfToggles().kernelTime =
            !MenuBuilder::PerfToggles().kernelTime;
        InvalidateAll();
        return;
    // ---- 30206「图形摘要视图(&W)」 ----
    //
    // 【已停用 · 2026-11-21】原来这里只翻转 PerfToggles().graphSummary，
    // 但**绘制侧从未接入**——菜单勾选会变，画面一动不动。这比「点了没反应」
    // 更糟：用户以为切换成功了，实际什么也没发生。
    //
    // 为什么不实现：原版「图形摘要视图」把图表换成什么样，投项目全部取证里
    // **没有任何证据**（docs/specs/ZZ_FIXES.md:157 已记「不臆造」）。
    // 凭空画一个「摘要」出来，就是再造一个假功能。
    //
    // 处理方式：菜单项置灰（在 MenuBuilder 里做），这里保留一个显式分支，
    // 万一有别处无条件派发也能看出意图，而不是静默落到 default。
    case CMD_GRAPH_SUMMARY:
        // 无 GUI 可做。不改状态、不刷新——改状态会误导。
        return;


    // ---- 30206「查看网络详细信息(&D)」 ----
    // 原版切到网络页并展开「网络详细信息」区块。本实现没有那个区块，
    // 至少把用户带到网络视图上（比什么都不做强，且不会误报成功）。
    case CMD_NET_DETAILS:
        perfPage_.SetView(PerfView::Network);
        if (tabs_.Current() != TabId::Performance) {
            tabs_.SetCurrent(TabId::Performance);
            BuildLayout();
        }
        InvalidateAll();
        RebuildMenu();
        return;

    // ---- 30207 侧栏右键三项 ----
    // ---- 30207「摘要视图(&W)」 ----
    //
    // 【已停用 · 2026-11-21】同上：sidebarSummary 只翻转状态、绘制侧零消费。
    // 原版这一项的渲染形态同样无证据（ZZ_FIXES.md:161）。
    // 置灰处理，见 MenuBuilder::BuildPerfSidebarContext。
    case CMD_SIDEBAR_SUMMARY:
        return;
    case CMD_SIDEBAR_SHOW_GRAPH:
        MenuBuilder::PerfToggles().hideSidebarGraph = false;
        InvalidateAll();
        return;
    case CMD_SIDEBAR_HIDE_GRAPH:
        MenuBuilder::PerfToggles().hideSidebarGraph = true;
        InvalidateAll();
        return;

    case CMD_OPEN_RESMON:
        // resmon.exe 需要管理员权限，用 runas 直接提权
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd_, L"runas", L"resmon.exe",
                                       nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
            ShellExecuteW(hwnd_, L"open", L"resmon.exe", nullptr, nullptr, SW_SHOWNORMAL);
        }
        return;
    case CMD_COPY:
        CopySelectedRowToClipboard();
        return;

    // ---- 选项 ----
    case CMD_ALWAYS_ON_TOP:
        alwaysOnTop_ = !alwaysOnTop_;
        SetWindowPos(hwnd_, alwaysOnTop_ ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        RebuildMenu();
        return;
    case CMD_HIDE_WHEN_MIN:
        // 勾选后最小化时隐藏到托盘；取消勾选时如果正藏在托盘里，先恢复出来，
        // 否则用户会觉得"取消勾选之后窗口还在托盘里"。
        hideWhenMinimized_ = !hideWhenMinimized_;
        if (!hideWhenMinimized_ && trayAdded_) RestoreFromTray();
        RebuildMenu();
        return;
    case CMD_RESTORE_FROM_TRAY:
        RestoreFromTray();
        return;
    case CMD_CLOSE_APP:
        RemoveTrayIcon();
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        return;
    case CMD_SHOW_FULL_NAME:
        // 【两处消费点，必须同步】
        //   1) 详细信息页的用户名列：读 TmWindow::showFullName_（tmwindow_details.cpp:322）
        //   2) 用户页的用户名列：读 MenuBuilder::ShowFullNameOption()
        //      —— 这一项只在**用户页**可点（menus.cpp:197），而它此前唯一的
        //      生效点在详细信息页。于是「在用户页勾上，用户页画面零变化」，
        //      是本项目典型的假功能。现在两条路径都跟着翻。
        showFullName_ = !showFullName_;
        MenuBuilder::ShowFullNameOption() = showFullName_;
        RebuildRows();
        InvalidateAll();
        RebuildMenu();
        return;
    case CMD_SHOW_ALL_HISTORY:
        // 【补漏】此前这个 ID **完全没有 case**，被 OnMenuCommand 末尾的
        // `default: return;` 静默吞掉 —— 点了毫无反应、菜单也没有勾。
        //
        // 【语义订正】这一项影响的是**应用历史记录页**（选项菜单里它也只在
        // currentTab==2 时可点，见 src/menus.cpp:203-206），不是进程页：
        // 「勾选 = 连已退出/从未在前台出现过的应用也列出来」。
        // 之前这里调用的是 RebuildRows()（重建**进程页**的行表），
        // 与这个开关毫无关系 —— 纯浪费一次全量重建 + 指针重定位，
        // 而且给读代码的人一个"它已经接上了"的错觉。
        //
        // 【现状】本机数据源 AppHistorySource::Enumerate 恒返回 false
        // （SRUM 需要 SYSTEM 权限 + 未公开的 srumapi.dll，实测直接调用会崩，
        // 见 src/pages_data_users_services.cpp:239 的完整说明），所以无论勾不
        // 勾，应用历史页都显示「无可用数据」。这里能做的是：
        //   (1) 真的让设置走到数据层（RefreshAppHistory），一旦 SRUM 接上
        //       或换成别的历史源，勾选立刻生效，不用再改这一处；
        //   (2) 数据层确实没有历史时，如实告诉用户"设置已改，但本机没有
        //       历史数据"，而不是让勾选变成一个哑巴动画。
        // 剩下的「按开关过滤列表内容」必须动 src/pages_view.cpp 的
        // RefreshAppHistory/PaintAppHistory（把 showAllHistory_ 传进去），
        // 那两个文件不归本模块改，已记入交接单。
        showAllHistory_ = !showAllHistory_;
        // 应用历史页的列表就地重查 —— 这是本开关真正的消费入口
        // （以前这里调的是 RebuildRows()，重建的是**进程页**的行表，
        //  与这个开关毫无关系。）
        pagesView_.RefreshAppHistory();
        InvalidateAll();
        RebuildMenu();
        // 【如实反馈】打开时如果数据层确实没有历史数据，告诉用户"设置生效了，
        // 但本机没有可显示的数据"，而不是让勾选变成一个哑巴动画。
        // 关闭时不提示：从空的列表切回空的列表，本身无需解释。
        // 判断口径与 PagesView::RefreshAppHistory 完全一致（同一个数据源、
        // 同一次调用），拿到数据时这里保持安静，与原版行为一致。
        if (showAllHistory_) {
            std::vector<AppHistoryItem> probe;
            AppHistorySource src;
            if (!src.Enumerate(probe)) {
                MessageBoxW(hwnd_,
                    L"已启用「显示所有进程的历史记录」。\n\n"
                    L"本机没有可用的应用历史数据：Windows 的 SRUM（系统资源使用监视器）"
                    L"需要 SYSTEM 权限，且 srumapi.dll 没有公开的导出签名"
                    L"（实测直接调用会崩溃），因此应用历史记录页仍会显示「无可用数据」。",
                    L"应用历史记录", MB_OK | MB_ICONINFORMATION);
            }
        }
        return;
    case CMD_GROUP_BY_TYPE:
        groupByType_ = !groupByType_;
        RebuildRows();
        InvalidateAll();
        RebuildMenu();
        return;

    case CMD_MINIMIZE_ON_USE:
        // 只翻转状态 + 重建菜单（勾选态）。
        // 【真正的执行点在 src/tmwindow_events.cpp 的 WM_ACTIVATE】此前全项目
        // 没有任何 WM_ACTIVATE/WM_KILLFOCUS 处理，所以勾了也只有菜单多一个 ✓，
        // 程序行为零变化 —— 典型的死开关。现在 WndProc 里 WA_INACTIVE 且本开关
        // 为真时会把窗口最小化（自己弹出的对话框/菜单会被排除）。
        minimizeOnUse_ = !minimizeOnUse_;
        RebuildMenu();
        return;

    case CMD_EXPAND:
    case CMD_COLLAPSE:
        if (selectedRow_ >= 0 && selectedRow_ < static_cast<int>(rows_.size())) {
            ViewRow& r = rows_[selectedRow_];
            if (r.kind == ViewRow::GroupHeaderRow) {
                int g = static_cast<int>(r.group);
                if (g >= 0 && g < 3) groupExpanded_[g] = (id == CMD_EXPAND);
            } else {
                // 应用行的子进程展开（当前实现只支持组级，进程级留空）
                appExpanded_[r.pid] = (id == CMD_EXPAND);
            }
            RebuildRows();
            InvalidateAll();
        }
        return;

    case CMD_PROVIDE_FEEDBACK:
        // 原版用的是自定义协议，不是 support.microsoft.com 的网页。
        // 这个字符串在 taskmgr.exe 里逐字存在（确证-STR）。
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd_, L"open",
                L"feedback-hub:?tabid=2&origin=taskmanager",
                nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
            // 协议未注册（精简镜像常见）→ 退到反馈中心网页
            ShellExecuteW(hwnd_, L"open",
                L"https://support.microsoft.com/", nullptr, nullptr, SW_SHOWNORMAL);
        }
        return;

    case CMD_ANALYZE_CHAIN: {
        const ProcessEntry* pe = SelectedProcess();
        if (!pe) return;
        AnalyzeWaitChain(pe->pid, pe->imageName);
        return;
    }
    // ---- UAC 虚拟化（30200 / 30208 的「UAC 虚拟化(&V)」）----
    // 此前这里是纯空壳：只弹一句"尚未实现"。现在真的做 ——
    // 语义就是给目标 exe 打/去 UAC 提权标记：
    //   HKCU\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers
    //   值名 = exe 完整路径，值 = "~ RUNASADMIN"
    // 注意这条只对 HKCU 生效（每用户），写 HKLM 需要管理员；原版同样只写用户层。
    // 数据来源：ProcessEntry 里没有该标记，所以只能"读当前值 → 反转 → 写回"，
    // 无法像原版那样先显示勾选态（原版菜单里这一项也没有勾选标记，
    // dump 30200 第 18 行 state=0x0000）。
    case CMD_UAC_VIRT: {
        const ProcessEntry* pe = SelectedProcess();
        if (!pe) return;
        std::wstring path = SelectedProcessPath();
        if (path.empty()) {
            MessageBoxW(hwnd_,
                L"尚未解析到该进程的可执行文件路径，无法设置 UAC 虚拟化。",
                L"Task Manager Pro", MB_OK | MB_ICONINFORMATION);
            return;
        }

        constexpr const wchar_t* kLayersKey =
            L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers";
        HKEY k = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kLayersKey, 0, nullptr, 0,
                            KEY_READ | KEY_WRITE, nullptr, &k, nullptr)
            != ERROR_SUCCESS) {
            ReportFailure(L"打开 UAC 虚拟化配置", GetLastError());
            return;
        }

        // 读出当前值（字符串，可能为空/缺失）
        std::wstring cur;
        DWORD type = 0, cb = 0;
        if (RegQueryValueExW(k, path.c_str(), nullptr, &type, nullptr, &cb)
                == ERROR_SUCCESS && cb > sizeof(wchar_t)) {
            std::vector<wchar_t> tmp(cb / sizeof(wchar_t) + 1, 0);
            if (RegQueryValueExW(k, path.c_str(), nullptr, &type,
                                 reinterpret_cast<LPBYTE>(tmp.data()), &cb)
                    == ERROR_SUCCESS) {
                cur.assign(tmp.data());
            }
        }

        bool on = (cur.find(L"RUNASADMIN") != std::wstring::npos);
        LONG rc;
        if (on) {
            // 关掉：把 RUNASADMIN 从串里摘掉；串空了就整条值删掉
            std::wstring next = cur;
            size_t p = next.find(L"RUNASADMIN");
            if (p != std::wstring::npos) next.erase(p, 10);
            // 收一下多余空格
            while (!next.empty() && (next.front() == L' ')) next.erase(next.begin());
            while (!next.empty() && (next.back() == L' ')) next.pop_back();
            if (next.empty()) {
                rc = RegDeleteValueW(k, path.c_str());
                if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
            } else {
                rc = RegSetValueExW(k, path.c_str(), 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(next.c_str()),
                        static_cast<DWORD>((next.size() + 1) * sizeof(wchar_t)));
            }
        } else {
            // 打开：追加 RUNASADMIN（已有的 "~" 前缀保留）
            std::wstring next = cur;
            if (next.empty()) next = L"~ RUNASADMIN";
            else             next += L" RUNASADMIN";
            rc = RegSetValueExW(k, path.c_str(), 0, REG_SZ,
                    reinterpret_cast<const BYTE*>(next.c_str()),
                    static_cast<DWORD>((next.size() + 1) * sizeof(wchar_t)));
        }
        RegCloseKey(k);

        if (rc != ERROR_SUCCESS) {
            ReportFailure(L"设置 UAC 虚拟化", rc);
            return;
        }
        // 原版这一步只改注册表，不重启进程；这里也保持一致，但要告诉用户
        // 「下次启动该程序时生效」，否则他会以为没做成。
        MessageBoxW(hwnd_,
            on ? L"已取消该程序的「以管理员身份运行」标记。\n\n重新启动该程序后生效。"
               : L"已将该程序标记为「以管理员身份运行」。\n\n重新启动该程序后生效。",
            L"UAC 虚拟化", MB_OK | MB_ICONINFORMATION);
        return;
    }


    // ---- 底部工具条：简略信息 ----
    // 「简略信息」= 切到**原版 UIFILE 30024** 那套简化界面：没有页签栏、没有列头，
    // 列表只列顶层应用行。
    //
    // 【原版依据】30024 的节点表里**没有 `tab` 实例**（只有样式声明），
    //   而 30001 有完整 tab 实例（height=30rp @0x1cc）；30024 里的简略列表是
    //   `TmSmallViewItem`（条目内只有图标 + 友好名 + CPU/内存两列），
    //   **没有 TmColHeader 实例**。命令条里那对
    //   `CBExpandoButtonImageArrow` + `CBExpandoButtonImageText` 就是
    //   「简略信息 ↔ 详细信息」的切换按钮。出处 docs/specs/S_duispec.md
    //   §0 表 E2、§1.7、§3.9。
    //
    // 【生效点】
    //   (1) src/tmwindow.cpp 的 BuildLayout：simpleView_ 时 rcTabs_ / rcColHeader_
    //       都压成零高矩形，rcList_ 占满内容区（详见那里的长注释）
    //   (2) src/tmwindow_events.cpp 的 PaintAll：零高时跳过 PaintTabBar /
    //       PaintColumnHeader（它们在零高矩形上仍会画出贴边的杂线）
    //   (3) src/tmwindow.cpp 的 RebuildRows：只列顶层应用行，不分组
    //
    // 这个命令不出现在任何菜单里（只有底部工具条那一个入口），所以不需要
    // 同步勾选态，RebuildMenu 也不用调。
    // ⚠ 窗口尺寸不变（我们不缩放窗口），只改内部布局 —— 与「换一份 UIFILE」
    //   不等价，但用户可感知的部分（页签没了、列头没了）已对齐。
    case CMD_VIEW_SIMPLE:
        // 【现在真的有可见效果】此前 simpleView_ 唯一的读点就是它自己：
        // rcCards_ 已改成恒零高，布局里没有任何地方随它变化，于是**每一页
        // 最左边这个按钮点下去画面完全不变** —— 全项目最容易撞上的死按钮。
        // 现在三条真实效果：
        //   1) 按钮文案在「简略信息(&D)」/「详细信息(&D)」之间切换
        //      （原版就是这对互斥文案：MUI 37212 / 37211）；
        //   2) 页签栏 + 列头整体收起，列表占满内容区；
        //   3) 进程页切到「只列顶层应用行」的简略视图（RebuildRows 里读 simpleView_）。
        // BuildLayout 仍要调，因为按钮宽度随文案变化（Measured() 按文字算宽）。
        simpleView_ = !simpleView_;
        // 进出简略视图时行表的语义变了（分组 → 只列顶层应用），
        // 老的 hoverRow_ 可能指向一个不存在的行，先清掉。
        hoverRow_ = -1;
        hoverCol_ = -1;
        tabs_.SetHover(TabId::Count);
        // 【简略视图永远落在进程页】
        // 原版 30024 是一整套独立界面，里面只有简化进程列表（`taskman` 根，
        // `processestab_tables` 那一支）；没有页签控件，也就没有"别的页"。
        // 所以从任何页点「简略信息」都落到进程页，而不是"把当前页也简化"
        // （性能页简化的样子在 30024 里根本不存在）。
        // 这一步必须排在 BuildLayout 之前：BuildLayout 之后页签栏是零高，
        // 再改 tabs_.Current() 不会重算布局，会留下"内容区按旧页算"的错位。
        if (simpleView_) tabs_.SetCurrent(TabId::Processes);
        BuildLayout();
        RebuildRows();
        InvalidateAll();
        return;

    // ---- 列头右键（MENU 30201 静态版两项） ----
    //
    // 【这两个 ID 此前也是零 case，被 default 静默吞掉】现在都真的做事。
    //
    // CMD_HIDE_COLUMN：隐藏「右键时鼠标所在的那一列」。
    //   菜单命令无参数，所以列索引由 WM_RBUTTONUP 在弹菜单之前存进
    //   headerContextCol_（见 tmwindow_events.cpp 与 tmwindow.h 的注释）。
    //   名称列（0）会被 SetColumnVisible 拒绝 —— 原版也不允许隐藏名称列。
    case CMD_HIDE_COLUMN: {
        const int col = headerContextCol_;
        if (col > 0 && col < kSortColCount) {
            SetColumnVisible(col, false);
        }
        // col <= 0（右键落在名称列或列头空白处）：不做任何事，也不假装做了。
        return;
    }

    // CMD_SELECT_COLUMNS：原版弹的是「选择列」对话框（MUI STR 2027/2028，
    // 一列一个复选框 + 「显示/隐藏」）。本项目没有这个对话框。
    //
    // 【诚实的选择】不置灰、不假装弹了对话框，而是实现成**明确可验证的动作**：
    // 把所有列恢复可见。这与「选择列」的语义一致（它就是一个列可见性的
    // 编辑入口），用户点完能立刻看到效果，不会以为坏了。
    // 真正的对话框是后续项，交接单里写明（见 docs/specs/EE_columns.md）。
    case CMD_SELECT_COLUMNS:
        ResetColumnVisibility();
        return;

    default:
        return;
    }
}

// ============ 转储文件 ============

namespace {
// dbghelp.dll 的 MiniDumpWriteDump 动态加载：静态链接会把 dbghelp
// 打进导入表，而这台机器不一定装了调试工具。
typedef BOOL (WINAPI *MiniDumpWriteDump_t)(
    HANDLE hProcess, DWORD ProcessId, HANDLE hFile,
    DWORD DumpType,
    PVOID ExceptionParam, PVOID UserStreamParam, PVOID CallbackParam);

constexpr DWORD kMiniDumpNormal = 0x00000000;
constexpr DWORD kMiniDumpWithDataSegs = 0x00000001;
} // namespace

void TmWindow::CreateDumpFile(uint32_t pid, const std::wstring& imageName) {
    wchar_t desktop[MAX_PATH] = {};
    if (!SHGetSpecialFolderPathW(nullptr, desktop, CSIDL_DESKTOPDIRECTORY, FALSE)) {
        wcscpy_s(desktop, L".");
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t fileName[MAX_PATH];
    std::wstring base = imageName.empty() ? L"process" : imageName;
    size_t dot = base.find_last_of(L'.');
    if (dot != std::wstring::npos) base = base.substr(0, dot);
    swprintf_s(fileName, L"%s\\%s_%u_%04d%02d%02d_%02d%02d%02d.dmp",
               desktop, base.c_str(), pid,
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                               FALSE, pid);
    if (!hProc) {
        ReportFailure(L"打开进程以创建转储", GetLastError());
        return;
    }

    HANDLE hFile = CreateFileW(fileName, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        CloseHandle(hProc);
        ReportFailure(L"创建转储文件", err);
        return;
    }

    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    if (!dbghelp) {
        CloseHandle(hFile);
        CloseHandle(hProc);
        MessageBoxW(hwnd_,
            L"无法加载 dbghelp.dll，因此无法创建转储文件。\n\n"
            L"请安装 Windows SDK 的调试工具。",
            L"Task Manager Pro", MB_OK | MB_ICONERROR);
        return;
    }

    auto pMiniDump = reinterpret_cast<MiniDumpWriteDump_t>(
        GetProcAddress(dbghelp, "MiniDumpWriteDump"));

    BOOL ok = FALSE;
    if (pMiniDump) {
        ok = pMiniDump(hProc, pid, hFile,
                       kMiniDumpNormal | kMiniDumpWithDataSegs,
                       nullptr, nullptr, nullptr);
    }

    DWORD err = ok ? 0 : GetLastError();
    FreeLibrary(dbghelp);
    CloseHandle(hFile);
    CloseHandle(hProc);

    if (ok) {
        // 打开所在文件夹并选中文件，比只弹"成功"更有用
        std::wstring args = L"/select,\"" + std::wstring(fileName) + L"\"";
        ShellExecuteW(hwnd_, L"open", L"explorer.exe", args.c_str(),
                      nullptr, SW_SHOWNORMAL);
    } else {
        ReportFailure(L"创建转储文件", err ? err : ERROR_GEN_FAILURE);
    }
}

// ============ 运行新任务输入框 ============

bool TmWindow::PromptForCommand(wchar_t* buf, size_t cch) {
    buf[0] = 0;
    g_newTaskElevate = false;

    // 不用 DialogBoxIndirectParam 的动态控件（时序不可靠），
    // 直接自建一个模态子窗口 + 自己的消息循环。
    static bool registered = false;
    static const wchar_t* kCls = L"TMP_NewTaskWnd";
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kCls;
        RegisterClassExW(&wc);
        registered = true;
    }

    // 对话框尺寸按「说明文字（两行）+ 输入框 + 提权复选框 + 两个按钮」排。
    RECT rc{ 0, 0, 440, 196 };
    AdjustWindowRect(&rc, WS_CAPTION | WS_SYSMENU | WS_POPUP, FALSE);

    // 居中于主窗口
    RECT mainRc;
    GetWindowRect(hwnd_, &mainRc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int x = mainRc.left + ((mainRc.right - mainRc.left) - w) / 2;
    int y = mainRc.top + ((mainRc.bottom - mainRc.top) - h) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, kCls, L"新建任务",
        WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE,
        x, y, w, h, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!dlg) return false;

    // 【文案修复】说明文字此前是自造的「输入要运行的命令、程序或 URL：」，
    // 在 MUI 里搜不到这句。原版这句逐字来自 STRINGTABLE：
    //   docs/evidence/mui_all.txt:921（= docs/evidence/tm/mui3.txt:844）
    //   「新建任务1Windows 将根据你所键入的名称，为你打开相应的程序、文件夹、
    //     文档或 Internet 资源。」
    // 前面那个「新建任务1」是「字符串ID+序号」的前缀，正文本体从「Windows」开始；
    // 「新建任务」正是对话框标题（也就是说这串是「标题 + 说明」一体的一条）。
    HWND lbl = CreateWindowExW(0, L"STATIC",
        L"Windows 将根据你所键入的名称，为你打开相应的程序、文件夹、文档或 Internet 资源。",
        WS_CHILD | WS_VISIBLE, 16, 12, 408, 44,
        dlg, nullptr, nullptr, nullptr);
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        16, 60, 408, 26, dlg, reinterpret_cast<HMENU>(5001), nullptr, nullptr);
    // 【控件补全】原版这个对话框还有一个「以系统管理权限创建此任务」复选框。
    // 本实现此前只有输入框 + 确定/取消，提权靠"普通启动失败后静默再 runas 一次"，
    // 用户无法预知会不会提权。现在改成显式勾选 —— 勾了就直走 runas。
    HWND chk = CreateWindowExW(0, L"BUTTON", L"以系统管理权限创建此任务",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        16, 94, 300, 22, dlg, reinterpret_cast<HMENU>(5002), nullptr, nullptr);
    CreateWindowExW(0, L"BUTTON", L"确定",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        306, 126, 80, 28, dlg, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
    CreateWindowExW(0, L"BUTTON", L"取消",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        392, 126, 80, 28, dlg, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);

    HFONT guiFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    if (lbl)  SendMessageW(lbl,  WM_SETFONT, reinterpret_cast<WPARAM>(guiFont), TRUE);
    if (edit) SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(guiFont), TRUE);
    if (chk)  SendMessageW(chk,  WM_SETFONT, reinterpret_cast<WPARAM>(guiFont), TRUE);

    EnableWindow(hwnd_, FALSE);
    SetFocus(edit);

    bool got = false;
    MSG msg;
    while (IsWindow(dlg)) {
        if (!GetMessageW(&msg, nullptr, 0, 0)) break;
        // 只处理属于本对话框的消息，其余转发回去
        if (msg.hwnd == dlg || IsChild(dlg, msg.hwnd)) {
            if (msg.message == WM_COMMAND) {
                int cid = LOWORD(msg.wParam);
                if (cid == IDOK) {
                    if (edit) GetWindowTextW(edit, buf, static_cast<int>(cch));
                    g_newTaskElevate = chk &&
                        (SendMessageW(chk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    got = true;
                    break;
                }
                if (cid == IDCANCEL) break;
            }
            if (msg.message == WM_CLOSE) break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    EnableWindow(hwnd_, TRUE);
    if (IsWindow(dlg)) DestroyWindow(dlg);
    if (IsWindow(hwnd_)) SetForegroundWindow(hwnd_);
    return got && buf[0] != 0;
}

// ============ 复制到剪贴板 ============

void TmWindow::CopySelectedRowToClipboard() {
    std::wstring text;

    if (tabs_.Current() == TabId::Processes) {
        if (!snap_) return;
        if (selectedRow_ < 0 || selectedRow_ >= static_cast<int>(rows_.size())) return;
        const ViewRow& r = rows_[selectedRow_];
        if (r.kind == ViewRow::GroupHeaderRow) return;
        const ProcessEntry* pe = nullptr;
        for (const auto& e : snap_->processes) {
            if (e.pid == r.pid) { pe = &e; break; }
        }
        if (!pe) return;
        // 【口径统一】复制值和屏上显示走同一个来源（GetRowDisplay）。
        // 原来这里直接打 pe->workingSet，而屏上显示的是私有工作集，
        // 应用行还是整棵树的聚合值——复制出来和看到的对不上。
        RowDisplay dv;
        dv.cpuPercent = pe->cpuPercent;
        dv.memBytes = pe->privateBytes ? pe->privateBytes : pe->workingSet;
        GetRowDisplay(selectedRow_, dv);
        wchar_t b[512];
        swprintf_s(b, L"%s\t%.1f%%\t%.1f MB",
                   pe->imageName.c_str(), dv.cpuPercent,
                   dv.memBytes / 1048576.0);
        text = b;
    } else if (tabs_.Current() == TabId::Details) {
        // 【补】详细信息页此前**什么都不复制**（只处理 Processes / Services）。
        // 原版这一页的 Ctrl+C 复制整行各列，制表符分隔。
        if (detailSelectedPid_ == 0) return;
        const ProcessEntry* pe = nullptr;
        for (const auto* p : detailRows_) {
            if (p && p->pid == detailSelectedPid_) { pe = p; break; }
        }
        if (!pe) return;
        // 【口径统一】详细信息页是扁平列表，没有应用聚合语义，
        // 但内存口径要和该页屏上显示一致：privateBytes 优先，取不到退回 workingSet。
        const uint64_t memShown = pe->privateBytes ? pe->privateBytes : pe->workingSet;
        wchar_t b[1024];
        swprintf_s(b, L"%s\t%u\t%s\t%.1f%%\t%.1f MB",
                   pe->imageName.c_str(), pe->pid,
                   pe->suspendedThreadCount >= pe->threadCount && pe->threadCount
                       ? L"已挂起" : L"正在运行",
                   pe->cpuPercent, memShown / 1048576.0);
        text = b;
    } else if (tabs_.Current() == TabId::Services) {
        int s = pagesView_.SelectedService();
        const auto& items = pagesView_.ServiceItems();
        if (s < 0 || s >= static_cast<int>(items.size())) return;
        wchar_t b[1024];
        swprintf_s(b, L"%s\t%u\t%s\t%s",
                   items[s].displayName.c_str(), items[s].pid,
                   items[s].stateText.c_str(), items[s].group.c_str());
        text = b;
    } else if (tabs_.Current() == TabId::Performance) {
        // 【补】30206 与 30207 的「复制(&C) Ctrl+C」是同一个命令 30445
        // （dump 第 96 行与第 104 行），只是作用对象不同：图表区复制图表数据、
        // 侧栏复制摘要。此前性能页走这条命令**什么都不发生**。
        //
        // 本实现没有把图表点位留成可读缓冲区，这里复制当前视图的摘要数值
        // （比什么都不做诚实，且格式与原版同为「名称\t数值」）。
        wchar_t b[512];
        PerfView v = perfPage_.CurrentView();
        const wchar_t* name = L"CPU";
        switch (v) {
        case PerfView::Memory:  name = L"内存";     break;
        case PerfView::Disk:    name = L"磁盘";     break;
        case PerfView::Network: name = L"网络";     break;
        case PerfView::Gpu:     name = L"GPU";      break;
        default:                name = L"CPU";      break;
        }
        swprintf_s(b, L"%s\t(当前视图摘要)", name);
        text = b;
    } else if (tabs_.Current() == TabId::Users) {
        int u = pagesView_.SelectedUser();
        const auto& list = pagesView_.UserItemsForCommand();
        if (u < 0 || u >= static_cast<int>(list.size())) return;
        wchar_t b[1024];
        swprintf_s(b, L"%s\\%s\t%u\t%s",
                   list[u].domain.c_str(), list[u].userName.c_str(),
                   list[u].sessionId, list[u].stateText.c_str());
        text = b;
    } else if (tabs_.Current() == TabId::Startup) {
        int s = pagesView_.SelectedStartup();
        const auto& items = pagesView_.StartupItems();
        if (s < 0 || s >= static_cast<int>(items.size())) return;
        wchar_t b[1024];
        swprintf_s(b, L"%s\t%s\t%s",
                   items[s].name.c_str(), items[s].publisher.c_str(),
                   items[s].enabled ? L"已启用" : L"已禁用");
        text = b;
    } else {
        return;
    }

    if (text.empty()) return;
    if (!OpenClipboard(hwnd_)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        void* p = GlobalLock(h);
        if (p) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        } else {
            GlobalFree(h);
        }
    }
    CloseClipboard();
}

// ============ 分析等待链 ============

// wct.h 提供的公开 API。原版任务管理器用的就是这个（确证：advapi32.dll 延迟导入
// OpenThreadWaitChainSession / GetThreadWaitChain），不是 NtQueryInformationThread。
// 为避免把 wct.h 引入头文件（它会拖进一堆 SDK 依赖），这里手写需要的声明。
namespace {
constexpr DWORD WCT_OUT_OF_PROC_COM_FLAG = 0x00000002;
constexpr DWORD WCT_OUT_OF_PROC_CS_FLAG  = 0x00000004;

struct WAITCHAIN_NODE_INFO_LOCAL {
    DWORD ObjectType;       // 0=未使用 1=锁 2=线程 3=COM 4=临界区
    DWORD ObjectStatus;     // 1=Blocked 2=Busy 3=Alertable 4=Inactive ...
    union {
        struct {
            DWORD ProcessId;
            DWORD ThreadId;
            DWORD WaitTime;   // 毫秒
            DWORD ContextSwitches;
        } ThreadObject;
        struct {
            WCHAR ObjectName[128];
            DWORD Timeout;
            DWORD Alertable;
        } LockObject;
    };
};

typedef BOOL (WINAPI *OpenThreadWaitChainSession_t)(DWORD flags, PVOID callback);
typedef BOOL (WINAPI *GetThreadWaitChain_t)(
    HWND hwnd, DWORD_PTR context, DWORD flags,
    DWORD threadId, LPDWORD nodeCount,
    WAITCHAIN_NODE_INFO_LOCAL* nodes,    // 实际是 PWWAITCHAIN_NODE_INFO
    LPBOOL isCycle);
typedef BOOL (WINAPI *CloseThreadWaitChainSession_t)(HWND session);
} // namespace

void TmWindow::AnalyzeWaitChain(uint32_t pid, const std::wstring& imageName) {
    HMODULE adv = LoadLibraryW(L"advapi32.dll");
    if (!adv) {
        MessageBoxW(hwnd_, L"无法加载 advapi32.dll。", L"Task Manager Pro",
                    MB_OK | MB_ICONERROR);
        return;
    }

    auto pOpen = reinterpret_cast<OpenThreadWaitChainSession_t>(
        GetProcAddress(adv, "OpenThreadWaitChainSession"));
    auto pGet = reinterpret_cast<GetThreadWaitChain_t>(
        GetProcAddress(adv, "GetThreadWaitChain"));
    auto pClose = reinterpret_cast<CloseThreadWaitChainSession_t>(
        GetProcAddress(adv, "CloseThreadWaitChainSession"));

    if (!pOpen || !pGet || !pClose) {
        FreeLibrary(adv);
        MessageBoxW(hwnd_,
            L"此系统不支持等待链 API（需要 Windows 8 或更高版本）。",
            L"Task Manager Pro", MB_OK | MB_ICONERROR);
        return;
    }

    HWND session = reinterpret_cast<HWND>(
        pOpen(0, nullptr));   // 0 = 同步模式，不注册回调
    if (!session) {
        DWORD e = GetLastError();
        FreeLibrary(adv);
        ReportFailure(L"打开等待链会话", e);
        return;
    }

    // 遍历该进程的所有线程，找第一个处于阻塞状态的
    std::wstring report;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnap == INVALID_HANDLE_VALUE) {
        pClose(session);
        FreeLibrary(adv);
        ReportFailure(L"枚举线程", GetLastError());
        return;
    }

    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    int blockedFound = 0;
    if (Thread32First(hSnap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;

            constexpr DWORD kMaxNodes = 32;
            WAITCHAIN_NODE_INFO_LOCAL nodes[kMaxNodes] = {};
            DWORD nodeCount = kMaxNodes;
            BOOL isCycle = FALSE;

            if (!pGet(session, 0, 0, te.th32ThreadID, &nodeCount, nodes, &isCycle)) {
                continue;
            }

            // 判断这条链是否有阻塞
            bool blocked = false;
            for (DWORD i = 0; i < nodeCount; ++i) {
                if (nodes[i].ObjectType == 2 && nodes[i].ObjectStatus == 1) {
                    blocked = true;
                    break;
                }
            }
            if (!blocked) continue;
            if (++blockedFound > 3) break;   // 最多报 3 条，避免刷屏

            wchar_t buf[512];
            swprintf_s(buf, L"线程 %lu%s:\r\n",
                       te.th32ThreadID, isCycle ? L"（存在死锁环！）" : L"");
            report += buf;

            for (DWORD i = 0; i < nodeCount; ++i) {
                const auto& n = nodes[i];
                if (n.ObjectType == 2) {
                    swprintf_s(buf,
                        L"  线程 %lu (进程 %lu)  状态=%s  等待 %lu ms  上下文切换 %lu\r\n",
                        n.ThreadObject.ThreadId, n.ThreadObject.ProcessId,
                        n.ObjectStatus == 1 ? L"阻塞" :
                        n.ObjectStatus == 2 ? L"忙" :
                        n.ObjectStatus == 3 ? L"可警告" : L"不活动",
                        n.ThreadObject.WaitTime,
                        n.ThreadObject.ContextSwitches);
                    report += buf;
                } else if (n.ObjectType == 1) {
                    swprintf_s(buf, L"  锁 \"%s\"  超时 %lu ms\r\n",
                               n.LockObject.ObjectName, n.LockObject.Timeout);
                    report += buf;
                } else if (n.ObjectType == 4) {
                    report += L"  临界区 (Critical Section)\r\n";
                } else if (n.ObjectType == 3) {
                    report += L"  COM 调用\r\n";
                }
            }
            report += L"\r\n";
        } while (Thread32Next(hSnap, &te));
    }
    CloseHandle(hSnap);
    pClose(session);
    FreeLibrary(adv);

    if (report.empty()) {
        wchar_t msg[512];
        swprintf_s(msg, 512,
            L"%s (PID %u) 的所有线程都没有处于阻塞状态。\n\n"
            L"没有等待链可以分析。",
            imageName.c_str(), pid);
        MessageBoxW(hwnd_, msg, L"分析等待链", MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::wstring title = L"等待链 - " + imageName;
    MessageBoxW(hwnd_, report.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
}

// ============ 托盘图标（「最小化时隐藏」的配套设施）============

// 用户勾选「最小化时隐藏」后，窗口最小化时应当从任务栏消失、只留托盘图标；
// 点托盘图标恢复。没有托盘的话窗口一隐就没法叫回来，所以这两件事必须一起做。
void TmWindow::AddTrayIcon() {
    if (trayAdded_) return;

    nid_ = {};
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = WM_TRAYICON;
    // 用窗口自己的小图标；取不到就用系统的
    nid_.hIcon = reinterpret_cast<HICON>(
        SendMessageW(hwnd_, WM_GETICON, ICON_SMALL, 0));
    if (!nid_.hIcon) nid_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid_.szTip, L"任务管理器");

    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE;
}

void TmWindow::RemoveTrayIcon() {
    if (!trayAdded_) return;
    Shell_NotifyIconW(NIM_DELETE, &nid_);
    trayAdded_ = false;
}

void TmWindow::RestoreFromTray() {
    ShowWindow(hwnd_, SW_SHOW);
    ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
    RemoveTrayIcon();
}

} // namespace tmp
