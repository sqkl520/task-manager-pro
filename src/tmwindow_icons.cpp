// tmwindow_icons.cpp - 路径解析与真实图标接入
//
// 性能要点：
//   - 路径用 NtQuerySystemInformation(88)，实测 0.001ms/次，435 个进程仅 0.53ms
//   - 只解析一次，结果缓存在 pathCache_（pid -> path）
//   - 图标交给 IconCache 后台线程，绘制路径只查表
#include "tmwindow.h"
#include <algorithm>
#include "ntpath.h"

#include <winternl.h>
#include <ntstatus.h>
#include <winsvc.h>
#include <vector>
#include <unordered_set>

#pragma comment(lib, "ntdll.lib")

namespace tmp {

namespace {

typedef NTSTATUS (NTAPI* NtQSI_t)(ULONG, PVOID, ULONG, PULONG);
constexpr ULONG kSystemProcessIdInformation = 88;

typedef struct _TMP_SPIDI {
    HANDLE         ProcessId;
    UNICODE_STRING ImageName;
} TMP_SPIDI;

NtQSI_t GetNtQSI() {
    static auto fn = []() -> NtQSI_t {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) ntdll = LoadLibraryW(L"ntdll.dll");
        return reinterpret_cast<NtQSI_t>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
    }();
    return fn;
}

} // namespace

void TmWindow::ResolvePathsAsync() {
    if (!snap_) return;

    auto fn = GetNtQSI();
    if (!fn) return;

    // 只解析还没缓存过的 pid，避免每秒重复
    std::unordered_set<uint32_t> alive;
    alive.reserve(snap_->processes.size() * 2);

    wchar_t buf[1024];
    int resolved = 0;
    const bool firstPass = pathCache_.empty();

    for (const auto& e : snap_->processes) {
        alive.insert(e.pid);

        // pid 复用检测：createTime 变了说明是新进程，旧缓存作废。
        auto it = pathCache_.find(e.pid);
        if (it != pathCache_.end()) {
            auto itCt = pathCreateTime_.find(e.pid);
            const bool sameProcess = (itCt != pathCreateTime_.end() &&
                                      itCt->second == e.createTime.QuadPart);
            if (!sameProcess) {
                // 同一个 pid 但创建时间不同 → 新进程，丢弃旧缓存
                pathCache_.erase(it);
                it = pathCache_.end();
            }
        }
        if (it != pathCache_.end() && !it->second.empty()) {
            icons_.RequestIcon(it->second);
            continue;
        }
        if (e.pid == 0) continue;

        TMP_SPIDI info{};
        info.ProcessId = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(e.pid));
        info.ImageName.Length = 0;
        info.ImageName.MaximumLength = sizeof(buf);
        info.ImageName.Buffer = buf;

        ULONG needed = 0;
        NTSTATUS st = fn(kSystemProcessIdInformation, &info, sizeof(info), &needed);
        if (NT_SUCCESS(st) && info.ImageName.Length > 0) {
            std::wstring ntPath(info.ImageName.Buffer, info.ImageName.Length / sizeof(wchar_t));

            // 关键：class 88 给的是 \Device\HarddiskVolume3\... 这种 NT 设备路径，
            // SHGetFileInfo 不认，必须转成 C:\... 才能拿到真实图标。
            // 实测（图标探测程序输出，未随仓库分发）：NT 路径直接喂进去必然返回 NULL HICON。
            std::wstring dosPath = NtPathToDos(ntPath);
            pathCreateTime_[e.pid] = e.createTime.QuadPart;
            if (dosPath.empty()) {
                // 内核伪路径（Registry/System/Memory Compression），没有可执行文件
                pathCache_[e.pid] = L"";
                ++resolved;
                continue;
            }

            pathCache_[e.pid] = dosPath;
            icons_.RequestIcon(dosPath);
            ++resolved;
            // 首轮全量解析（实测 class 88 只要 0.001ms/次，435 个仅 0.5ms，
            // 完全可以一次做完；后续轮次只解析新出现的 pid）
            if (firstPass && resolved >= 600) break;
        }
    }

    // 清理已退出进程的缓存。
    //
    // 【修复】原来是 `if (pathCache_.size() > 2000)` 才清理，但本机只有 640 左右
    // 进程，这个阈值永远触发不到 —— 缓存只增不减，退出进程的条目一直留着，
    // 遇到 pid 复用就会串到新进程上。
    //
    // 现在每次都清（O(n)，640 条目可以忽略）。配合 pathCreateTime_ 的
    // 双键校验，双重保险。
    for (auto it = pathCache_.begin(); it != pathCache_.end(); ) {
        if (alive.find(it->first) == alive.end()) it = pathCache_.erase(it);
        else ++it;
    }
    for (auto it = pathCreateTime_.begin(); it != pathCreateTime_.end(); ) {
        if (alive.find(it->first) == alive.end()) it = pathCreateTime_.erase(it);
        else ++it;
    }
    // svchost 名字表也清掉已退出的
    for (auto it = svcHostName_.begin(); it != svcHostName_.end(); ) {
        if (alive.find(it->first) == alive.end()) it = svcHostName_.erase(it);
        else ++it;
    }

    // descCache_ 的键是 **exe 路径**（不是 pid），所以不能按 alive 修剪。
    // 正确口径：本轮 pathCache_ 里还在使用的那些路径之外的，全部作废。
    //
    // 【为什么不是每秒做】这段要新建一个 unordered_set<wstring> 并把
    // pathCache_ 里 600 条路径字符串全复制进去，实测每秒约 2~3 ms。
    // 而它防的是「长期运行累积」——那是以分钟为尺度的，没必要每秒跑。
    // 改成每 60 轮一次（≈1 分钟），开销摊薄到可忽略。
    if ((++descPruneTick_ % 60u) == 0u) {
        std::unordered_set<std::wstring> livePaths;
        livePaths.reserve(pathCache_.size());
        for (const auto& kv : pathCache_) {
            if (!kv.second.empty()) livePaths.insert(kv.second);
        }
        for (auto it = descCache_.begin(); it != descCache_.end(); ) {
            if (livePaths.find(it->first) == livePaths.end()) it = descCache_.erase(it);
            else ++it;
        }
    }

    // 预取文件描述。
    //
    // 【为什么要在这里做】排序发生在紧接着的 RebuildRows 里，比较器调
    // ProcessDisplayName -> GetFileDescription。如果那时 descCache_ 还空着，
    // 每个比较都要现场读一次版本信息（O(n log n) 次文件 IO）。
    //
    // 【性能修复 · 实测 14.4 ms → 目标 <1 ms】
    // 原实现建了一个 `unordered_set<wstring> seen` 去重：对 pathCache_ 里
    // 600 条路径逐个 `seen.insert(kv.second)` —— 每条都要复制一个 std::wstring
    // 并算哈希。实测该阶段 14.4 ms，而真正的 GetFileVersionInfo 只有 9.4 次/轮
    // （约 0.5 ms）。**开销 96% 花在去重集合上，不是文件 IO。**
    //
    // 去重本来就由 descCache_ 自己承担：第一个同名路径取完描述后，
    // descCache_ 里就有了这个键，后续同路径的 `count()` 直接命中跳过。
    // 所以 seen 集合是纯多余的一层。去掉它，同一个循环既去重又取数。
    //
    // 【分批预取】GetFileVersionInfo 实测每次 1~2 ms（比注释早期估计的
    // 0.05 ms 慢 20~40 倍，慢盘/网络路径/大 exe 尤其明显）。启动首轮有
    // 140+ 个不同路径，一次读完全部要 300 ms —— 那一帧会明显卡住。
    // 改成每轮最多读 kPrefetchPerTick 个，剩余的下轮继续：
    //   - 未取到描述的进程这几轮先显示 exe 文件名（DisplayName 的回退路径），
    //     拿到后再换成文件描述，视觉上是"名称列逐批变好看"，不是卡顿；
    //   - 稳态下每轮只有 0~2 个新路径，这个上限根本不会触发。
    //
    // 【必须轮转，不能每轮从头遍历】pathCache_ 是 unordered_map，迭代顺序
    // **不保证稳定**（尤其每轮还有 erase 会改变桶结构）。如果每轮都从
    // `begin()` 开始取 24 个，那些"恰好总排在后面"的路径可能长期轮不到 ——
    // 表现是名称列里少数几项永远是 exe 文件名而不是文件描述。这是功能退化。
    //
    // 修法：记住上一轮停下的位置，这轮从那里继续，绕回后重置。
    // 用 round-robin 的游标即可，不依赖地图的内部顺序。
    constexpr int kPrefetchPerTick = 24;
    int prefetched = 0;
    // 收集所有待处理路径（每轮一次 O(P)，P = 不同 exe 路径数，通常 < 200）
    if (pendingDescPaths_.empty() || (++descScanTick_ % 30u) == 0u) {
        pendingDescPaths_.clear();
        for (const auto& kv : pathCache_) {
            if (kv.second.empty()) continue;
            if (descCache_.count(kv.second)) continue;
            pendingDescPaths_.push_back(kv.second);
        }
        // 去重（同一 exe 可能被多个 pid 引用）
        std::sort(pendingDescPaths_.begin(), pendingDescPaths_.end());
        pendingDescPaths_.erase(
            std::unique(pendingDescPaths_.begin(), pendingDescPaths_.end()),
            pendingDescPaths_.end());
        descCursor_ = 0;
    }
    while (prefetched < kPrefetchPerTick && descCursor_ < pendingDescPaths_.size()) {
        const std::wstring& p = pendingDescPaths_[descCursor_++];
        if (descCache_.count(p)) continue;   // 期间已被别处取到
        GetFileDescription(p);
        ++prefetched;
    }
    if (descCursor_ >= pendingDescPaths_.size()) pendingDescPaths_.clear();

    // svchost 的显示名（「服务主机: xxx」）。
    //
    // 服务列表不会频繁变化，但 svchost 进程会重启换 pid，所以每次快照
    // 都重建一遍。实测 EnumServicesStatusEx 约 44ms —— 这个开销不能每秒付。
    // 折中：只在发现**有 svchost 的 pid 不在表里**时重建（进程数量变化时）。
    {
        bool needBuild = false;
        for (const auto& e : snap_->processes) {
            if (_wcsicmp(e.imageName.c_str(), L"svchost.exe") != 0) continue;
            if (svcHostName_.find(e.pid) == svcHostName_.end()) { needBuild = true; break; }
        }
        if (needBuild && svcHostLastBuild_.QuadPart != 0) {
            // 距上次建表不足 10 秒就先跳过，避免频繁付 44ms
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
            double elapsed = static_cast<double>(now.QuadPart - svcHostLastBuild_.QuadPart)
                             / static_cast<double>(freq.QuadPart);
            if (elapsed < 10.0) needBuild = false;
        }
        if (needBuild) {
            BuildSvcHostNames();
            QueryPerformanceCounter(&svcHostLastBuild_);
        }
    }

    // 诊断：TMP_DESCDIAG=1 时把缓存内容写出来
    {
        wchar_t envb[8] = {};
        if (GetEnvironmentVariableW(L"TMP_DESCDIAG", envb, 8) > 0) {
            wchar_t p[MAX_PATH];
            GetModuleFileNameW(nullptr, p, MAX_PATH);
            std::wstring lp = p;
            size_t sl = lp.find_last_of(L"\\/");
            if (sl != std::wstring::npos) lp = lp.substr(0, sl);
            lp += L"\\descdiag.txt";
            FILE* f = nullptr;
            _wfopen_s(&f, lp.c_str(), L"w");
            if (f) {
                fwprintf(f, L"pathCache=%zu descCache=%zu svcHostName=%zu\n",
                         pathCache_.size(), descCache_.size(), svcHostName_.size());
                fwprintf(f, L"--- svchost names ---\n");
                for (const auto& kv : svcHostName_) {
                    fwprintf(f, L"  pid=%u => %s\n", kv.first, kv.second.c_str());
                }
                fwprintf(f, L"--- descCache (first 20) ---\n");
                int n = 0;
                for (const auto& kv : descCache_) {
                    fwprintf(f, L"  %s => [%s]\n",
                             kv.first.c_str(), kv.second.c_str());
                    if (++n >= 40) break;
                }
                fclose(f);
            }
        }
    }
    }
// ============ 文件描述（原版「名称」列的真实内容） ============
//
// 原版任务管理器的「名称」列显示的是 exe 版本信息里的 FileDescription：
//   explorer.exe -> "Windows 资源管理器"
//   dwm.exe      -> "桌面窗口管理器"
//   Taskmgr.exe  -> "任务管理器"
// 而不是文件名。这也是名称列排序的依据。
//
// 用 GetFileVersionInfoW + VerQueryValueW 读取 \VarFileInfo\Translation
// 拿到语言/代码页，再取 \StringFileInfo\<lang><cp>\FileDescription。
std::wstring TmWindow::GetFileDescription(const std::wstring& dosPath) const {
    if (dosPath.empty()) return std::wstring();

    auto it = descCache_.find(dosPath);
    if (it != descCache_.end()) return it->second;

    std::wstring desc;
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(dosPath.c_str(), &dummy);
    if (size > 0 && size < 4 * 1024 * 1024) {
        std::vector<uint8_t> buf(size);
        if (GetFileVersionInfoW(dosPath.c_str(), 0, size, buf.data())) {
            // 先找翻译表，取第一个语言/代码页
            struct LANGANDCP { WORD lang; WORD cp; };
            LANGANDCP* tr = nullptr;
            UINT trSize = 0;
            if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation",
                               reinterpret_cast<LPVOID*>(&tr), &trSize) &&
                tr && trSize >= sizeof(LANGANDCP)) {
                wchar_t sub[128];
                swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\FileDescription",
                           tr[0].lang, tr[0].cp);
                wchar_t* val = nullptr;
                UINT valSize = 0;
                if (VerQueryValueW(buf.data(), sub,
                                   reinterpret_cast<LPVOID*>(&val), &valSize) &&
                    val && valSize > 0) {
                    desc.assign(val);
                    // 去掉首尾空白
                    while (!desc.empty() && (desc.back() == L' ' || desc.back() == L'\r' ||
                                             desc.back() == L'\n' || desc.back() == L'\t')) {
                        desc.pop_back();
                    }
                    while (!desc.empty() && (desc.front() == L' ' || desc.front() == L'\t')) {
                        desc.erase(desc.begin());
                    }
                }
            }
        }
    }

    descCache_[dosPath] = desc;
    ++descCacheEpoch_;   // 通知 RebuildRows：排序键需要重建
    return desc;
}

// 进程的显示名。
//   svchost.exe → 「服务主机: <服务显示名或组名>」（见 BuildSvcHostNames）
//   其余 → 文件描述，拿不到时回退到「去掉 .exe 的文件名」
std::wstring TmWindow::ProcessDisplayName(uint32_t pid,
                                          const std::wstring& imageName) {
    // svchost 特殊处理
    if (_wcsicmp(imageName.c_str(), L"svchost.exe") == 0) {
        auto itS = svcHostName_.find(pid);
        if (itS != svcHostName_.end() && !itS->second.empty()) return itS->second;
        // 还没建表或查不到，先给个通用名
        return L"服务主机";
    }

    auto it = pathCache_.find(pid);
    if (it != pathCache_.end() && !it->second.empty()) {
        std::wstring d = GetFileDescription(it->second);
        if (!d.empty()) return d;
    }
    return DisplayName(imageName);
}

// 建 svchost 的显示名表：pid -> 「服务主机: xxx」
//
// 【规则，来自 docs/specs/spec_svchost.md 的实测】
//   EnumServicesStatusExW 返回每个服务的 lpDisplayName 和 dwProcessId。
//   一个 svchost 若只承载 1 个服务 → 用该服务的名字。
//   若承载多个 → 用第一个服务的名字（原版也是这样，不是拼接）。
//
// lpDisplayName 已经由系统解析过间接字符串（@dll,-id），
// 所以会得到本地化的名字，实测与 taskmgr 截图逐字一致。
void TmWindow::BuildSvcHostNames() {
    svcHostName_.clear();

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return;

    DWORD bytesNeeded = 0, count = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
                          SERVICE_STATE_ALL, nullptr, 0,
                          &bytesNeeded, &count, nullptr, nullptr);
    if (bytesNeeded == 0) { CloseServiceHandle(scm); return; }

    std::vector<BYTE> buf(bytesNeeded + 8192);
    if (!EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
                               SERVICE_STATE_ALL, buf.data(),
                               static_cast<DWORD>(buf.size()),
                               &bytesNeeded, &count, nullptr, nullptr)) {
        CloseServiceHandle(scm);
        return;
    }

    auto* arr = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buf.data());
    for (DWORD i = 0; i < count; ++i) {
        DWORD pid = arr[i].ServiceStatusProcess.dwProcessId;
        if (pid == 0) continue;               // 未运行的服务没有宿主

        const wchar_t* disp = arr[i].lpDisplayName;
        if (!disp || !disp[0]) disp = arr[i].lpServiceName;
        if (!disp || !disp[0]) continue;

        // 同一个 svchost 承载多个服务时，保留**第一个**服务的名字。
        // 原版显示的就是其中一个（不是拼接全部），
        // 拼接会让名字长得没法看。
        if (svcHostName_.find(pid) == svcHostName_.end()) {
            svcHostName_[pid] = std::wstring(L"服务主机: ") + disp;
        }
    }
    CloseServiceHandle(scm);
}
} // namespace tmp
