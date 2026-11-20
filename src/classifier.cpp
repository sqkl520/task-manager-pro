// classifier.cpp - 进程分组实现
// 全部规则来自 docs/specs/proc_semantics.md 的本机实测结论。
#include "classifier.h"

#include <algorithm>
#include <unordered_map>

namespace tmp {
namespace {

// 关键系统进程白名单（实测必须归「Windows 进程」）
const wchar_t* kWindowsWhitelist[] = {
    L"System", L"Registry", L"Memory Compression", L"smss.exe", L"csrss.exe",
    L"wininit.exe", L"winlogon.exe", L"services.exe", L"lsass.exe",
    L"dwm.exe", L"fontdrvhost.exe", L"WUDFHost.exe", L"audiodg.exe",
    L"sihost.exe", L"taskhostw.exe", L"ctfmon.exe", L"SearchIndexer.exe",
    L"svchost.exe",
};

bool InWhitelist(const std::wstring& name) {
    for (const wchar_t* w : kWindowsWhitelist) {
        if (_wcsicmp(name.c_str(), w) == 0) return true;
    }
    return false;
}

struct EnumCtx {
    Classification* out;
};

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return TRUE;

    // 【无响应探测】必须在 IsWindowVisible 之后、其余排除规则之前做。
    //
    // 原因：无响应的窗口往往**没有标题**（程序卡死时 GetWindowTextLength 可能拿不到），
    // 如果放在第 5 条排除规则之后，这些最重要的目标反而被漏掉。
    //
    // 判据用 IsHungAppWindow，不用 SendMessageTimeout：
    //   实测 SendMessageTimeout 的 timeout 是真实阻塞（5000ms 就阻塞 5018ms），
    //   放不进 15ms 预算；而且窗口已被系统标记 hung 时它的返回是 ret==0、
    //   GetLastError 也是 0，按 ERROR_TIMEOUT 判定会全部漏判。
    //   IsHungAppWindow 是纯查询，34 个可见窗口总耗时约 0.04ms。
    //
    // 范围必须限定为可见窗口：全量盲扫 686 个窗口会报出 17-51 个假阳性
    // （不可见的 IME/MSCTFIME UI 窗口，含 crashpad_handler 这类无 UI 进程），
    // 且实测出现过 516ms 尖峰。
    if (!IsWindowVisible(hwnd)) return TRUE;

    if (IsHungAppWindow(hwnd)) {
        ctx->out->notResponding.insert(pid);
    }

    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if (style & WS_CHILD) return TRUE;

    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;

    int titleLen = GetWindowTextLengthW(hwnd);
    if (titleLen <= 0) return TRUE;

    // 命中：这是一个「应用」根
    ctx->out->appRoots.insert(pid);
    wchar_t title[512] = {};
    GetWindowTextW(hwnd, title, 512);
    if (title[0] && ctx->out->appTitle.find(pid) == ctx->out->appTitle.end()) {
        ctx->out->appTitle[pid] = title;
    }
    return TRUE;
}

} // namespace

void Classifier::CollectAppRoots(Classification& out) {
    EnumCtx ctx{ &out };
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&ctx));
}


bool Classifier::IsSuspended(const Snapshot& snap, uint32_t pid) {
    for (const auto& e : snap.processes) {
        if (e.pid != pid) continue;
        if (e.threadCount == 0) return false;   // 正在退出的进程不算挂起
        // 全部线程都挂起才算
        if (e.suspendedThreadCount == 0) return false;
        return e.suspendedThreadCount >= e.threadCount;
    }
    return false;
}

void Classifier::DetectSuspended(const Snapshot& snap, Classification& out) {
    for (const auto& e : snap.processes) {
        if (e.threadCount == 0) continue;
        if (e.suspendedThreadCount > 0 && e.suspendedThreadCount >= e.threadCount) {
            out.suspended.insert(e.pid);
        }
    }
}

bool Classifier::LooksLikeWindowsProcess(const ProcessEntry& e, bool isSession0,
                                         const std::wstring* fullPath) {
    // 会话 0 的一律是 Windows 进程（服务宿主都在会话 0）
    if (isSession0) return true;
    if (InWhitelist(e.imageName)) return true;

    // 精确路径判断（调用方传入 pathCache_ 里的真实路径）
    const std::wstring* p = fullPath;
    if (!p && !e.imagePath.empty()) p = &e.imagePath;
    if (p && !p->empty()) {
        if (p->find(L"\\Windows\\System32\\") != std::wstring::npos ||
            p->find(L"\\Windows\\SysWOW64\\") != std::wstring::npos ||
            p->find(L"\\Windows\\WinSxS\\") != std::wstring::npos ||
            p->find(L"\\Windows\\SystemApps\\") != std::wstring::npos ||
            p->find(L"\\Windows\\ImmersiveControlPanel\\") != std::wstring::npos) {
            return true;
        }
        // Program Files / 用户目录下的明确不是 Windows 进程
        if (p->find(L"\\Program Files") != std::wstring::npos ||
            p->find(L"\\Users\\") != std::wstring::npos) {
            return false;
        }
    }

    // ---- 启发式：按进程名判断是否 Windows 自带组件 ----
    // 原版的「Windows 进程」组包含大量没有可见窗口的系统组件，
    // 它们的共同特征是名字里带这些特征词。
    static const wchar_t* kPatterns[] = {
        L"RuntimeBroker", L"SearchHost", L"SearchApp", L"SearchUI",
        L"StartMenuExperienceHost", L"ShellExperienceHost",
        L"TextInputHost", L"ApplicationFrameHost", L"SystemSettings",
        L"WindowsTerminal", L"LockApp", L"UserOOBEBroker",
        L"SecurityHealth", L"SecHealthUI", L"Smartscreen",
        L"CompPkgSrv", L"backgroundTaskHost", L"Widgets", L"WidgetService",
        L"PhoneExperienceHost", L"GameBar", L"XboxGame", L"GameInput",
        L"MoUsoCoreWorker", L"UsoClient", L"WaaSMedic", L"TrustedInstaller",
        L"dllhost", L"WmiPrvSE", L"taskhostw", L"explorer",     // explorer 见下方特判
        L"SettingSyncHost", L"InputSwitch", L"cmd", L"conhost",
        L"WindowsInternal", L"WUDFHost", L"Registry", L"MemCompression",
        L"LogonUI", L"winlogon", L"UserInit", L"TabTip",
        L"DmClient", L"DataExchange", L"SgrmBroker", L"SecurityHealthService",
        L"StoreDesktopExtension", L"Sysmon", L"SystemSettingsBroker",
        L"ShellHost", L"OneSettings", L"AppResolverUX", L"PickerHost",
    };
    for (const wchar_t* pat : kPatterns) {
        if (wcsstr(e.imageName.c_str(), pat) != nullptr) {
            // explorer.exe 有可见窗口时已被判为「应用」，到不了这里；
            // 能到这里说明它没窗口（极少见），归 Windows 进程更合理。
            return true;
        }
    }

    return false;
}

void Classifier::BuildAppTree(const Snapshot& snap, Classification& out) {
    // 建 pid -> parentPid 与 pid -> 索引
    std::unordered_map<uint32_t, size_t> idx;
    idx.reserve(snap.processes.size() * 2);
    for (size_t i = 0; i < snap.processes.size(); ++i) {
        idx[snap.processes[i].pid] = i;
    }

    // 对每个非根进程，向上爬找到最近的 app root
    for (const auto& e : snap.processes) {
        if (out.appRoots.count(e.pid)) {
            out.appRootOf[e.pid] = e.pid;
            continue;
        }
        // 沿父子链向上，最多 8 层，防环
        uint32_t cur = e.pid;
        uint32_t root = 0;
        for (int depth = 0; depth < 8; ++depth) {
            auto it = idx.find(cur);
            if (it == idx.end()) break;
            const ProcessEntry& ce = snap.processes[it->second];
            uint32_t parent = ce.parentPid;
            if (parent == 0 || parent == cur) break;
            if (out.appRoots.count(parent)) { root = parent; break; }
            cur = parent;
        }
        if (root != 0) out.appRootOf[e.pid] = root;
    }
}

void Classifier::AssignGroups(const Snapshot& snap, Classification& out,
                              const std::unordered_map<uint32_t, std::wstring>* pathMap) {
    for (const auto& e : snap.processes) {
        // 1. 应用根，或归属于某个应用根的子进程
        if (out.appRoots.count(e.pid) || out.appRootOf.count(e.pid)) {
            out.groupOf[e.pid] = ProcessGroup::Apps;
            continue;
        }
        // 2. Windows 进程
        bool session0 = (e.sessionId == 0);
        const std::wstring* full = nullptr;
        if (pathMap) {
            auto it = pathMap->find(e.pid);
            if (it != pathMap->end() && !it->second.empty()) full = &it->second;
        }
        if (LooksLikeWindowsProcess(e, session0, full)) {
            out.groupOf[e.pid] = ProcessGroup::WindowsProcs;
            continue;
        }
        // 3. 其余全是后台进程
        out.groupOf[e.pid] = ProcessGroup::Background;
    }
}

void Classifier::Classify(const Snapshot& snap, Classification& out,
                          const std::unordered_map<uint32_t, std::wstring>* pathMap) {
    out.Clear();
    CollectAppRoots(out);
    DetectSuspended(snap, out);
    BuildAppTree(snap, out);
    AssignGroups(snap, out, pathMap);
}

} // namespace tmp
