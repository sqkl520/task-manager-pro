// perfdata.cpp - 性能页数据采集实现
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>

#include "perfdata.h"

#include <winternl.h>
#include <ntstatus.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <powrprof.h>
#include <memory>
#include <mutex>
#include <thread>

#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "advapi32.lib")

namespace tmp {

namespace {

typedef NTSTATUS (NTAPI* NtQSI_t)(ULONG, PVOID, ULONG, PULONG);

NtQSI_t GetNtQSI() {
    static auto fn = []() -> NtQSI_t {
        HMODULE n = GetModuleHandleW(L"ntdll.dll");
        if (!n) n = LoadLibraryW(L"ntdll.dll");
        return reinterpret_cast<NtQSI_t>(GetProcAddress(n, "NtQuerySystemInformation"));
    }();
    return fn;
}

// class 8: SystemProcessorPerformanceInformation
// 【实测】必须给精确 960 字节（20 核 × 48），给 64KB 反而返回 STATUS_INFO_LENGTH_MISMATCH
struct PROC_PERF {
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG         InterruptCount;
};
constexpr ULONG kSystemProcessorPerformanceInformation = 8;

// class 2: SystemPerformanceInformation，完整 344 字节
// 关键偏移已本机验证（见 docs/specs/perf_page_spec.md 8.3b）
struct SYS_PERF_INFO {
    LARGE_INTEGER IdleProcessTime;       // +0
    LARGE_INTEGER IoReadTransferCount;   // +8
    LARGE_INTEGER IoWriteTransferCount;  // +16
    LARGE_INTEGER IoOtherTransferCount;  // +24
    ULONG IoReadOperationCount;          // +32
    ULONG IoWriteOperationCount;         // +36
    ULONG IoOtherOperationCount;         // +40
    ULONG AvailablePages;                // +44  ★
    ULONG CommittedPages;                // +48  ★
    ULONG CommitLimit;                   // +52  ★
    ULONG PeakCommitment;                // +56
    ULONG PageFaultCount;                // +60
    ULONG CopyOnWriteCount;              // +64
    ULONG TransitionCount;               // +68
    ULONG CacheTransitionCount;          // +72
    ULONG DemandZeroCount;               // +76
    ULONG PageReadCount;                 // +80
    ULONG PageReadIoCount;               // +84
    ULONG CacheReadCount;                // +88
    ULONG CacheIoCount;                  // +92
    ULONG DirtyPagesWriteCount;          // +96
    ULONG DirtyWriteIoCount;             // +100
    ULONG MappedPagesWriteCount;         // +104
    ULONG MappedWriteIoCount;            // +108
    ULONG PagedPoolPages;                // +112 ★
    ULONG NonPagedPoolPages;             // +116 ★
    ULONG PagedPoolAllocs;               // +120
    ULONG PagedPoolFrees;                // +124
    ULONG NonPagedPoolAllocs;            // +128
    ULONG NonPagedPoolFrees;             // +132
    ULONG FreeSystemPtes;                // +136
    ULONG ResidentSystemCodePage;        // +140
    ULONG TotalSystemDriverPages;        // +144
    ULONG TotalSystemCodePages;          // +148
    ULONG NonPagedPoolLookasideHits;     // +152
    ULONG PagedPoolLookasideHits;        // +156
    ULONG AvailablePagedPoolPages;       // +160
    ULONG ResidentSystemCachePage;       // +164 ★
    // ... 后续字段不关心，但结构体要给够 344 字节
    BYTE  tail[344 - 168];
};
constexpr ULONG kSystemPerformanceInformation = 2;

// class 80: SystemMemoryListInformation，176 字节
//
// 每个字段是 SIZE_T（8 字节），不是 ULONG。之前这里全写成 ULONG，导致：
//   freeBytes     读 +4  → 拿到 ZeroPageCount 的高 4 字节，永远是 0
//   modifiedBytes 读 +8  → 拿到 FreePageCount，把空闲内存当已修改
//   standbyBytes  读 +20 → 错位到 BadPageCount 中间，比真值小 346 倍
// 176 = 22 × 8 这个尺寸本身就是「22 个 SIZE_T」的直接证据；字段偏移也用
// PDH 的 Free & Zero Page List / Modified Page List / Standby Cache Normal
// Priority 三个计数器逐个交叉验证过（见 docs/specs/P_phase2.md §0.3）。
struct SYS_MEM_LIST {
    SIZE_T ZeroPageCount;              // +0x00
    SIZE_T FreePageCount;              // +0x08
    SIZE_T ModifiedPageCount;          // +0x10
    SIZE_T ModifiedNoWritePageCount;   // +0x18
    SIZE_T BadPageCount;               // +0x20
    SIZE_T PageCountByPriority[8];     // +0x28
    SIZE_T RepurposedByPriority[8];    // +0x68
    SIZE_T ModifiedPageCountPageFile;  // +0xA8
};
static_assert(sizeof(SYS_MEM_LIST) == 176, "class 80 实测返回 176 字节");
constexpr ULONG kSystemMemoryListInformation = 80;

// class 3: SystemTimeOfDayInformation
struct SYS_TIME_OF_DAY {
    LARGE_INTEGER BootTime;
    LARGE_INTEGER CurrentTime;
    LARGE_INTEGER TimeZoneBias;
    ULONG TimeZoneId;
    ULONG Reserved;
    ULONGLONG BootTimeBias;
    ULONGLONG SleepTimeBias;
};
constexpr ULONG kSystemTimeOfDayInformation = 3;

constexpr uint64_t kPageSize = 4096;

// PROCESSOR_POWER_INFORMATION 在 winnt.h 里受 _WIN32_WINNT 保护，
// 这里直接手写（字段顺序与 winnt.h 一致）。
typedef struct _TMP_PROCESSOR_POWER_INFORMATION {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
} TMP_PROCESSOR_POWER_INFORMATION;

} // namespace

PerfCollector::PerfCollector() {}
PerfCollector::~PerfCollector() { Stop(); }

void PerfCollector::Start(uint32_t intervalMs) {
    if (running_.exchange(true)) return;
    intervalMs_ = intervalMs ? intervalMs : 1000;
    if (!wakeEvent_) wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ReadStaticCpuInfo();
    thread_ = std::thread(&PerfCollector::ThreadProc, this);
}

void PerfCollector::Stop() {
    if (!running_.exchange(false)) return;
    if (wakeEvent_) SetEvent(wakeEvent_);
    if (thread_.joinable()) thread_.join();
    ClosePdH();
    if (wakeEvent_) { CloseHandle(wakeEvent_); wakeEvent_ = nullptr; }
}

void PerfCollector::RequestRefresh() {
    refresh_.store(true);
    if (wakeEvent_) SetEvent(wakeEvent_);
}

std::shared_ptr<const PerfSnapshot> PerfCollector::GetSnapshot() const {
    std::lock_guard<std::mutex> lk(publishMutex_);
    return published_;
}

void PerfCollector::SetProcessCounts(uint32_t procs, uint32_t threads, uint64_t handles) {
    procCounts_[0].store(procs, std::memory_order_relaxed);
    procCounts_[1].store(threads, std::memory_order_relaxed);
    handleCount_.store(handles, std::memory_order_relaxed);
}

void PerfCollector::ReadStaticCpuInfo() {
    // CPU 型号名与基准频率：注册表，只读一次
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t name[256] = {};
        DWORD sz = sizeof(name), type = 0;
        if (RegQueryValueExW(k, L"ProcessorNameString", nullptr, &type,
                             reinterpret_cast<LPBYTE>(name), &sz) == ERROR_SUCCESS) {
            cpuName_ = name;
        }
        DWORD mhz = 0;
        sz = sizeof(mhz);
        if (RegQueryValueExW(k, L"~MHz", nullptr, &type,
                             reinterpret_cast<LPBYTE>(&mhz), &sz) == ERROR_SUCCESS) {
            baseMhz_ = mhz;
        }
        RegCloseKey(k);
    }

    // 物理核 / 逻辑核 / 缓存
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len > 0) {
        std::vector<BYTE> buf(len);
        if (GetLogicalProcessorInformationEx(RelationProcessorCore,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len)) {
            uint32_t phys = 0;
            DWORD off = 0;
            while (off < len) {
                auto* p = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
                ++phys;
                off += p->Size;
            }
            physCores_ = phys;
        }
    }
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    logCores_ = si.dwNumberOfProcessors;

    // 缓存
    // 说明：GetLogicalProcessorInformationEx(RelationCache) 会对**每个逻辑核**
    // 返回条目。L1 每核两条（数据 + 指令），L2 每核一条，L3 每核一条但共享。
    // 任务管理器的 "L1 缓存" 显示的是【数据缓存总量】(核数 × 单核容量)。
    // 这里取单条最大值 × 物理核数，避免重复累加。
    len = 0;
    GetLogicalProcessorInformationEx(RelationCache, nullptr, &len);
    if (len > 0) {
        std::vector<BYTE> buf(len);
        if (GetLogicalProcessorInformationEx(RelationCache,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len)) {
            uint64_t maxL1 = 0, maxL2 = 0, maxL3 = 0;
            DWORD off = 0;
            while (off < len) {
                auto* p = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
                if (p->Relationship == RelationCache) {
                    // 注意：CACHE_DESCRIPTOR.CacheSize 单位是【字节】，不是 KB
                    uint64_t sz2 = static_cast<uint64_t>(p->Cache.CacheSize);
                    switch (p->Cache.Level) {
                    case 1: if (sz2 > maxL1) maxL1 = sz2; break;
                    case 2: if (sz2 > maxL2) maxL2 = sz2; break;
                    case 3: if (sz2 > maxL3) maxL3 = sz2; break;
                    }
                }
                off += p->Size;
            }
            // L1/L2 是每核独立的，总量 = 单核 × 核数；L3 共享，只取单份
            uint32_t pc = physCores_ ? physCores_ : 1;
            l1_ = maxL1 * pc;
            l2_ = maxL2 * pc;
            l3_ = maxL3;
        }
    }

    // 虚拟化是否启用（本机 VBS 在跑 → 已启用）
    // 用 IsProcessorFeaturePresent 不够，查 Hyper-V 是否存在
    HKEY vk;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard",
                      0, KEY_READ, &vk) == ERROR_SUCCESS) {
        virtEnabled_ = true;   // 有 DeviceGuard 键说明 VBS 相关功能存在
        RegCloseKey(vk);
    }
}

bool PerfCollector::InitPdH() {
    // 【已移除 · 2026-11-21】这里原来建了两个 PDH 查询、共 6 个 `(*)` 通配符计数器：
    //   diskQuery_: \PhysicalDisk(*)\% Idle Time / Disk Read Bytes/sec /
    //               Disk Write Bytes/sec / Avg. Disk sec/Transfer
    //   netQuery_ : \Network Interface(*)\Bytes Sent/sec / Bytes Received/sec
    //
    // **它们从未被读取过。** 全项目零处调用 PdhCollectQueryData /
    // PdhGetFormattedCounterValue（grep 确认为空），这两个句柄只在构造时创建、
    // 在析构时关闭，中间一个数据点都没取。
    //
    // 代价却不小：`(*)` 通配符要在 8 块盘 × 几十个网络接口上展开并注册计数器，
    // 初始化一次就是几十毫秒的纯开销。而且 `(*)` 通配符正是本项目铁律明令禁止的
    // 那类查询（\Thread(*) 曾经 947 ms/次）。
    //
    // 磁盘与网络的真实数据走的是别的路径：
    //   - 磁盘：NtQuerySystemInformation(SystemPerformanceInformation) 的
    //     IoRead/WriteTransferCount 差分（见 CollectDisk）
    //   - 网络：ETW Microsoft-Windows-Kernel-Network（见 netproc）
    //   - GPU：D3DKMT（见 gpuinfo），显式避开 \GPU Engine(*)
    //
    // 所以这个方法现在无事可做，保留是为了不改调用点。
    return false;
}

// 建立「接口索引 -> 友好名称」映射。
//
// 用户看到的是「网络连接」里的名字（"WLAN 2" / "VMware Network Adapter VMnet8"
// / "Tailscale"），而 GetIfTable 只给 dwIndex 数字。
//
// 两条路径都用上了：
//   1. GetAdaptersAddresses 的 FriendlyName（覆盖大部分）
//   2. 注册表 HKLM\SYSTEM\CurrentControlSet\Control\Network\{4d36e972-...}\
//      <GUID>\Connection\Name（兜底，用 GetIfTable 的 wszName GUID 匹配）
//
// 第 2 条是必要的：实测 GetIfTable 把一块无线网卡注册成 8 个 index
// （26/45/46/47/48/49/50/51），其中只有 26 能在 AdaptersAddresses 里找到，
// 其余 7 个都拿不到名字，界面上就会显示成「网卡 45」。
void PerfCollector::BuildIfNameMap() {
    if (ifNameMapBuilt_) return;
    ifNameMapBuilt_ = true;

    // ---- 路径 1：GetAdaptersAddresses ----
    {
        ULONG size = 16 * 1024;
        std::vector<uint8_t> buf(size);
        ULONG ret = ERROR_BUFFER_OVERFLOW;
        for (int attempt = 0; attempt < 3; ++attempt) {
            ret = GetAdaptersAddresses(AF_UNSPEC,
                                       GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                       GAA_FLAG_SKIP_DNS_SERVER,
                                       nullptr,
                                       reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()),
                                       &size);
            if (ret == ERROR_SUCCESS) break;
            if (ret != ERROR_BUFFER_OVERFLOW) break;
            buf.resize(size);
        }
        if (ret == ERROR_SUCCESS) {
            auto* p = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
            while (p) {
                if (p->FriendlyName && p->FriendlyName[0]) {
                    if (p->IfIndex)     ifNameMap_[p->IfIndex] = p->FriendlyName;
                    if (p->Ipv6IfIndex) ifNameMap_[p->Ipv6IfIndex] = p->FriendlyName;
                }
                p = p->Next;
            }
        }
    }

    // ---- 路径 2：注册表（按 GUID 匹配）----
    // 先把 GetIfTable 的 index -> GUID 建起来，再查注册表拿名字。
    {
        HKEY root = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                L"SYSTEM\\CurrentControlSet\\Control\\Network\\"
                L"{4d36e972-e325-11ce-bfc1-08002be10318}",
                0, KEY_READ, &root) == ERROR_SUCCESS) {

            DWORD size = 0;
            if (GetIfTable(nullptr, &size, FALSE) == ERROR_INSUFFICIENT_BUFFER) {
                std::vector<uint8_t> ib(size);
                auto* t = reinterpret_cast<PMIB_IFTABLE>(ib.data());
                if (GetIfTable(t, &size, FALSE) == NO_ERROR) {
                    for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                        const auto& r = t->table[i];
                        // wszName 是 "{GUID}" 形式的宽字符串
                        const wchar_t* guid = r.wszName;
                        if (!guid || guid[0] != L'{') continue;

                        std::wstring sub = std::wstring(guid) + L"\\Connection";
                        HKEY ck = nullptr;
                        if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ, &ck)
                                == ERROR_SUCCESS) {
                            wchar_t name[256] = {};
                            DWORD cb = sizeof(name) - sizeof(wchar_t);
                            DWORD type = 0;
                            if (RegQueryValueExW(ck, L"Name", nullptr, &type,
                                    reinterpret_cast<LPBYTE>(name), &cb) == ERROR_SUCCESS &&
                                type == REG_SZ && name[0]) {
                                // 注册表名字优先于 AdaptersAddresses（用户改过的名字在这里）
                                ifNameMap_[r.dwIndex] = name;
                            }
                            RegCloseKey(ck);
                        }
                    }
                }
            }
            RegCloseKey(root);
        }
    }

    // 诊断：TMP_NETDIAG=1 时把映射表写出来
    {
        wchar_t envb[8] = {};
        if (GetEnvironmentVariableW(L"TMP_NETDIAG", envb, 8) > 0) {
            wchar_t p[MAX_PATH];
            GetModuleFileNameW(nullptr, p, MAX_PATH);
            std::wstring lp = p;
            size_t sl = lp.find_last_of(L"\\/");
            if (sl != std::wstring::npos) lp = lp.substr(0, sl);
            lp += L"\\ifnames.txt";
            FILE* f = nullptr;
            _wfopen_s(&f, lp.c_str(), L"w");
            if (f) {
                for (const auto& kv : ifNameMap_) {
                    fwprintf(f, L"idx=%u name=%s\n", kv.first, kv.second.c_str());
                }
                fclose(f);
            }
        }
    }
}

void PerfCollector::ClosePdH() {
    auto close = [](void*& p) {
        if (p) { PdhCloseQuery(static_cast<PDH_HQUERY>(p)); p = nullptr; }
    };
    // 【已无 PDH 句柄需要关闭·2026-11-21】diskQuery_ / netQuery_ / gpuQuery_ /
    // gpuMemQuery_ 四个成员已删除：前两个是建了从不读的通配符查询（见 InitPdH），
    // 后两个更是从未被赋值过（GPU 走 D3DKMT，根本不建 PDH 查询）。
}

void PerfCollector::ThreadProc() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    InitPdH();

    // GPU：D3DKMT 路径，Init ~5ms（含一次 EnumProcesses）。
    // 放在采集线程里做，不阻塞 UI。
    gpuInfoAvailable_ = gpuInfo_.Init();

    while (running_.load()) {
        auto snap = std::make_shared<PerfSnapshot>();
        if (CollectOnce(*snap)) {
            uint64_t seq;
            {
                std::lock_guard<std::mutex> lk(publishMutex_);
                seq = (published_ ? published_->sequence : 0) + 1;
                snap->sequence = seq;
                published_ = std::move(snap);
            }
            publishedSeq_.store(seq, std::memory_order_release);
        }
        DWORD waitMs = refresh_.exchange(false) ? 0 : intervalMs_;
        if (!running_.load()) break;
        if (waitMs > 0 && wakeEvent_) {
            WaitForSingleObject(wakeEvent_, waitMs);
            refresh_.store(false);
        }
    }
}

bool PerfCollector::CollectOnce(PerfSnapshot& out) {
    auto fn = GetNtQSI();
    if (!fn) return false;

    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    QueryPerformanceFrequency(&f);
    qpcFreq_ = f;    // 磁盘/网络差分要用

    // ---------- CPU 每核 ----------
    // 【实测】必须给精确 logCores * 48 字节
    {
        ULONG need = logCores_ * sizeof(PROC_PERF);
        std::vector<PROC_PERF> per(logCores_);
        ULONG ret = 0;
        NTSTATUS st = fn(kSystemProcessorPerformanceInformation,
                         per.data(), need, &ret);
        if (NT_SUCCESS(st)) {
            out.cores.resize(logCores_);
            uint32_t n = ret / sizeof(PROC_PERF);
            if (n > logCores_) n = logCores_;

            double sumUtil = 0;
            double sumKernelRatio = 0;
            for (uint32_t i = 0; i < n; ++i) {
                uint64_t idle   = static_cast<uint64_t>(per[i].IdleTime.QuadPart);
                uint64_t kernel = static_cast<uint64_t>(per[i].KernelTime.QuadPart);
                uint64_t user   = static_cast<uint64_t>(per[i].UserTime.QuadPart);

                out.cores[i].idleTime = idle;
                out.cores[i].kernelTime = kernel;
                out.cores[i].userTime = user;

                if (hasPrev_ && i < prevCores_.size()) {
                    uint64_t dIdle   = idle   - prevCores_[i].idle;
                    uint64_t dKernel = kernel - prevCores_[i].kernel;
                    uint64_t dUser   = user   - prevCores_[i].user;
                    // KernelTime 已含 IdleTime，不能再相加
                    uint64_t total = dKernel + dUser;
                    if (total > 0) {
                        double util = (1.0 - static_cast<double>(dIdle) /
                                             static_cast<double>(total)) * 100.0;
                        if (util < 0) util = 0;
                        if (util > 100) util = 100;
                        out.cores[i].utilPercent = util;
                        sumUtil += util;
                        sumKernelRatio += static_cast<double>(dKernel - dIdle) /
                                          static_cast<double>(total) * 100.0;
                    }
                }
            }
            if (n > 0) {
                out.cpuTotalPercent = sumUtil / n;
                out.cpuKernelPercent = sumKernelRatio / n;
            }

            prevCores_.resize(n);
            for (uint32_t i = 0; i < n; ++i) {
                prevCores_[i].idle   = static_cast<uint64_t>(per[i].IdleTime.QuadPart);
                prevCores_[i].kernel = static_cast<uint64_t>(per[i].KernelTime.QuadPart);
                prevCores_[i].user   = static_cast<uint64_t>(per[i].UserTime.QuadPart);
            }
            hasPrev_ = true;
        }
    }

    // ---------- 内存组成（NtQSI(2)，344 字节）----------
    {
        SYS_PERF_INFO spi{};
        ULONG ret = 0;
        NTSTATUS st = fn(kSystemPerformanceInformation, &spi, sizeof(spi), &ret);
        if (NT_SUCCESS(st)) {
            out.mem.availableBytes   = static_cast<uint64_t>(spi.AvailablePages) * kPageSize;
            out.mem.committedBytes   = static_cast<uint64_t>(spi.CommittedPages) * kPageSize;
            out.mem.commitLimitBytes = static_cast<uint64_t>(spi.CommitLimit) * kPageSize;
            out.mem.pagedPoolBytes   = static_cast<uint64_t>(spi.PagedPoolPages) * kPageSize;
            out.mem.nonPagedPoolBytes= static_cast<uint64_t>(spi.NonPagedPoolPages) * kPageSize;
            out.mem.cachedBytes      = static_cast<uint64_t>(spi.ResidentSystemCachePage) * kPageSize;
        }
    }

    // ---------- 内存备用列表（NtQSI(80)，176 字节）----------
    {
        SYS_MEM_LIST mli{};
        ULONG ret = 0;
        NTSTATUS st = fn(kSystemMemoryListInformation, &mli, sizeof(mli), &ret);
        if (NT_SUCCESS(st)) {
            out.mem.freeBytes = static_cast<uint64_t>(mli.FreePageCount) * kPageSize;
            out.mem.modifiedBytes = static_cast<uint64_t>(mli.ModifiedPageCount) * kPageSize;
            // 备用页 = PageCountByPriority 之和
            uint64_t standby = 0;
            for (int i = 0; i < 8; ++i) standby += mli.PageCountByPriority[i];
            out.mem.standbyBytes = standby * kPageSize;
            out.mem.cachedBytes += out.mem.standbyBytes + out.mem.modifiedBytes;
        }
    }

    // ---------- 物理内存总量 + 启动时间 ----------
    {
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) out.mem.totalBytes = ms.ullTotalPhys;

        SYS_TIME_OF_DAY tod{};
        ULONG ret = 0;
        if (NT_SUCCESS(fn(kSystemTimeOfDayInformation, &tod, sizeof(tod), &ret))) {
            LARGE_INTEGER now;
            GetSystemTimeAsFileTime(reinterpret_cast<FILETIME*>(&now));
            if (tod.BootTime.QuadPart > 0) {
                out.uptimeSeconds = static_cast<uint64_t>(
                    (now.QuadPart - tod.BootTime.QuadPart) / 10000000LL);
            }
        }
    }
    // 使用中 = 总量 - 可用
    //
    // 【口径说明】这里刻意**不减 cachedBytes**：`ullAvailPhys`（availableBytes）
    // 已经包含「待命（standby）里可回收」的那部分，再减一次就是重复扣除，
    // 会把使用中算少。原版任务管理器「内存」卡用的也是这个口径：
    //   使用中 = TotalPhys - AvailPhys
    // 与进程页表头（TmWindow::SetSnapshot 里同一个减法）完全一致。
    //
    // 【历史】此前这里有两行：先是 `total - available - cached`，但那一行的
    // 减数写成了 `(cached > available ? 0 : 0)` —— 恒为 0，等于什么也没减；
    // 紧跟着第二行又把整个结果覆盖成 `total - available`。
    // 于是第一行是纯死代码，却让读者以为这里在算 cached。已删除。
    if (out.mem.totalBytes > 0) {
        out.mem.inUseBytes = out.mem.totalBytes - out.mem.availableBytes;
    }

    // ---------- CPU 频率 ----------
    // 【实测】CallNtPowerInformation(ProcessorInformation) 在 服务器级 CPU 上
    // 恒返回 3001 MHz（无 Turbo），但仍要读，以便换机器时正确显示。
    {
        const ULONG bufSz = logCores_ * sizeof(TMP_PROCESSOR_POWER_INFORMATION);
        if (bufSz > 0) {
            std::vector<BYTE> buf(bufSz);
            if (CallNtPowerInformation(ProcessorInformation, nullptr, 0,
                                       buf.data(), bufSz) == ERROR_SUCCESS) {
                auto* info = reinterpret_cast<TMP_PROCESSOR_POWER_INFORMATION*>(buf.data());
                if (logCores_ > 0) out.cpuCurrentMhz = info[0].CurrentMhz;
            }
        }
    }

    // ---------- PDH：磁盘 ----------
    auto readWildcard = [](PDH_HQUERY q, PDH_HCOUNTER c,
                           std::vector<std::pair<std::wstring, double>>& outVals) {
        outVals.clear();
        if (!q || !c) return;
        DWORD sz = 0, cnt = 0;
        PDH_STATUS st = PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE,
                                                     &sz, &cnt, nullptr);
        if (st != PDH_MORE_DATA && st != ERROR_SUCCESS) return;
        if (sz == 0) return;
        std::vector<BYTE> buf(sz);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
        st = PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &sz, &cnt, items);
        if (st != ERROR_SUCCESS) return;
        for (DWORD i = 0; i < cnt; ++i) {
            if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA) {
                outVals.emplace_back(items[i].szName, items[i].FmtValue.doubleValue);
            }
        }
    };

    // ---------- 磁盘 ----------
    //
    // 不用 PDH。`\PhysicalDisk(*)\*` 通配符要枚举所有物理磁盘 × 多个计数器，
    // 在 E5 老机器上正是卡顿来源。改用 NtQuerySystemInformation(2) 的
    // 全局 IO 计数器差分 —— 一次调用拿全系统读写字节数，实测 1 秒差分
    // 得到 read=56.28 MB/s write=2.96 MB/s，数字真实。
    //
    // 代价：只能拿到「全系统」聚合值，不能分盘。所以这里产出一条
    // 「磁盘 0」代表整机 IO。要分盘必须用 PDH（慢）或 PDH 的
    // \PhysicalDisk(0 C:)\ 单实例（不走通配符，每次 3-5ms，可接受）。
    {
        SYS_PERF_INFO pi{};
        ULONG retLen = 0;
        auto fn = GetNtQSI();
        if (fn && fn(kSystemPerformanceInformation, &pi, sizeof(pi), &retLen) == 0) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const double dt = prevDiskQpc_.QuadPart
                ? static_cast<double>(now.QuadPart - prevDiskQpc_.QuadPart)
                  / static_cast<double>(qpcFreq_.QuadPart)
                : 0.0;

            uint64_t rNow = static_cast<uint64_t>(pi.IoReadTransferCount.QuadPart);
            uint64_t wNow = static_cast<uint64_t>(pi.IoWriteTransferCount.QuadPart);

            DiskSample d;
            d.name = L"磁盘 0";
            if (dt > 0.05 && prevDiskRead_ > 0) {
                uint64_t dr = (rNow > prevDiskRead_) ? (rNow - prevDiskRead_) : 0;
                uint64_t dw = (wNow > prevDiskWrite_) ? (wNow - prevDiskWrite_) : 0;
                d.readMBps  = static_cast<double>(dr) / dt / 1048576.0;
                d.writeMBps = static_cast<double>(dw) / dt / 1048576.0;
                // 活动时间：用吞吐占「合理上限」的比例近似。
                // 这不是真正的 % Disk Time（那要 PDH），但至少反映负载强度，
                // 而且不会像全 0 那样让人以为没数据。
                double totalMBps = d.readMBps + d.writeMBps;
                double pct = totalMBps / 500.0 * 100.0;   // 500 MB/s 视作饱和
                if (pct > 100.0) pct = 100.0;
                d.activePercent = pct;
                out.diskTotalMBps = totalMBps;
            }
            prevDiskRead_ = rNow;
            prevDiskWrite_ = wNow;
            prevDiskQpc_ = now;
            diskSeeded_ = true;
            out.disks.push_back(std::move(d));
        }
    }

    // ---------- 网络 ----------
    // 不走 PDH 通配符（\Network Interface(*) 在 E5 机上慢）。
    // 用 GetIfTable（XP 时代就有的 API，实测 64 个接口一次调用 < 1ms），
    // 拿累计收发字节做差分。实测可用，数值真实。
    {
        if (!netIfTableSeeded_) {
            DWORD size = 0;
            if (GetIfTable(nullptr, &size, FALSE) == ERROR_INSUFFICIENT_BUFFER) {
                std::vector<uint8_t> buf(size);
                auto* t = reinterpret_cast<PMIB_IFTABLE>(buf.data());
                if (GetIfTable(t, &size, FALSE) == NO_ERROR) {
                    netPrevIn_.clear(); netPrevOut_.clear();
                    for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                        const auto& r = t->table[i];
                        netPrevIn_[r.dwIndex]  = r.dwInOctets;
                        netPrevOut_[r.dwIndex] = r.dwOutOctets;
                    }
                    netIfTableSeeded_ = true;
                    QueryPerformanceCounter(&netPrevQpc_);
                }
            }
        } else {
            DWORD size = 0;
            if (GetIfTable(nullptr, &size, FALSE) == ERROR_INSUFFICIENT_BUFFER) {
                std::vector<uint8_t> buf(size);
                auto* t = reinterpret_cast<PMIB_IFTABLE>(buf.data());
                if (GetIfTable(t, &size, FALSE) == NO_ERROR) {
                    LARGE_INTEGER now;
                    QueryPerformanceCounter(&now);
                    double dt = static_cast<double>(now.QuadPart - netPrevQpc_.QuadPart)
                                / static_cast<double>(qpcFreq_.QuadPart);
                    netSigToIndex_.clear();   // 每轮重新去重
                    BuildIfNameMap();      // 只在第一轮真正建表

                    for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                        const auto& r = t->table[i];
                        // dwType=6 是以太网/802.3；跳过 loopback(24) 和 tunnel
                        if (r.dwType == 24) continue;   // loopback
                        if (r.dwOperStatus != 5) continue;   // 只要 operational

                        auto itIn  = netPrevIn_.find(r.dwIndex);
                        auto itOut = netPrevOut_.find(r.dwIndex);

                        NetSample n;
                        // 【修复】之前写死 "以太网 %lu"（接口索引），用户看到
                        // "以太网 45" 完全不知道是什么。现在用 GetAdaptersAddresses
                        // 拿到的 FriendlyName（就是「网络连接」里显示的名字，
                        // 如 "WLAN 2" / "VMware Network Adapter VMnet1"）。
                        auto itName = ifNameMap_.find(r.dwIndex);
                        if (itName != ifNameMap_.end() && !itName->second.empty()) {
                            n.name = itName->second;
                        } else {
                            wchar_t nm[64];
                            swprintf_s(nm, L"网卡 %lu", r.dwIndex);
                            n.name = nm;
                        }
                        n.linkSpeedBps = r.dwSpeed;

                        if (dt > 0.05 && itIn != netPrevIn_.end() &&
                            itOut != netPrevOut_.end()) {
                            uint64_t din  = (r.dwInOctets  > itIn->second)
                                            ? (r.dwInOctets  - itIn->second) : 0;
                            uint64_t dout = (r.dwOutOctets > itOut->second)
                                            ? (r.dwOutOctets - itOut->second) : 0;
                            n.recvMbps = static_cast<double>(din)  * 8.0 / dt / 1.0e6;
                            n.sendMbps = static_cast<double>(dout) * 8.0 / dt / 1.0e6;
                        }
                        netPrevIn_[r.dwIndex]  = r.dwInOctets;
                        netPrevOut_[r.dwIndex] = r.dwOutOctets;

                        // 只保留真实物理网卡。
                        // 实测本机 GetIfTable 返回 64 个条目，其中绝大多数是
                        // Hyper-V/WFP/VPN 虚拟接口：dwSpeed=100Mbps（假值）、
                        // 累计流量恒为 0 或极小。真实无线网卡是 dwType=71、
                        // 292Mbps、累计收发 2.8GB。
                        if (r.dwSpeed == 0) continue;
                        if (r.dwOperStatus != 5) continue;
                        if (r.dwType != 6 && r.dwType != 71) continue;
                        // 累计流量门槛：真实物理网卡一定用过 >1MB
                        if (r.dwInOctets < 1000000ULL && r.dwOutOctets < 1000000ULL) continue;

                        // 只保留**有友好名称**的接口。
                        //
                        // 【为什么】实测 GetIfTable 把同一块物理网卡注册成
                        // 多个 index（WLAN 有 26/45/46/47/... 等多个），
                        // 但只有 26 能在 GetAdaptersAddresses 里查到名字，
                        // 其余都是拿不到名字的「影子」条目。
                        //
                        // 原版对每个适配器只显示一条，且显示的是友好名称
                        // （"WLAN 2" / "VMware Network Adapter VMnet8"）。
                        // 所以直接丢掉没名字的 —— 之前界面上出现
                        // 「网卡 45」「网卡 47」就是这些影子条目。
                        {
                            auto itN = ifNameMap_.find(r.dwIndex);
                            if (itN == ifNameMap_.end() || itN->second.empty()) continue;
                            n.name = itN->second;
                            n.hasName = true;
                        }

                        // 同一物理网卡可能仍有多个有名字的视图，按累计流量去重
                        uint64_t sig = r.dwInOctets * 1000003ULL + r.dwOutOctets;
                        if (netSigToIndex_.count(sig)) continue;
                        netSigToIndex_[sig] = static_cast<uint32_t>(out.nets.size());
                        out.nets.push_back(std::move(n));
                    }
                    netPrevQpc_ = now;
                }
            }
        }

        double maxMbps = 0.0;
        for (const auto& n : out.nets) {
            double m = (std::max)(n.recvMbps, n.sendMbps);
            if (m > maxMbps) maxMbps = m;
        }
        out.netMaxMbps = maxMbps;
    }

    // ---------- 静态信息填充 ----------
    out.cpuName = cpuName_;
    out.cpuBaseMhz = baseMhz_;
    out.physicalCores = physCores_;
    out.logicalCores = logCores_;
    out.sockets = sockets_;
    out.l1CacheBytes = l1_;
    out.l2CacheBytes = l2_;
    out.l3CacheBytes = l3_;
    out.virtualizationEnabled = virtEnabled_;
    out.totalProcesses = procCounts_[0].load(std::memory_order_relaxed);
    out.totalThreads = procCounts_[1].load(std::memory_order_relaxed);
    out.totalHandles = handleCount_.load(std::memory_order_relaxed);

    // ---- GPU：每引擎使用率 + 适配器信息 ----
    // 走 D3DKMT。pid 列表由主采集器通过 SetProcessCounts 顺带传进来的不算，
    // 这里让 gpuinfo 用自己的 pid 缓存（每 60 轮刷新一次，稳态 ~1.0ms）。
    if (gpuInfoAvailable_) {
        gpuInfo_.Collect();
        out.gpuTotalPercent = gpuInfo_.TotalUtilPercent();

        const auto& engines = gpuInfo_.Engines();
        out.gpuEngines.clear();
        out.gpuEngines.reserve(engines.size());
        for (const auto& e : engines) {
            GpuEngineSample s;
            s.name = e.name;
            s.engineType = e.engineType;
            s.utilPercent = e.utilPercent;
            out.gpuEngines.push_back(std::move(s));
        }

        const auto& a = gpuInfo_.Adapter();
        out.gpuAdapter = a;

        // 侧栏的 "GPU 0" 条目也用它，保持与 GPU 页一致
        out.gpus.clear();
        GpuSample g;
        g.name = a.name.empty() ? L"GPU 0" : a.name;
        g.utilPercent = out.gpuTotalPercent;
        g.dedicatedBytes = a.dedicatedInUse;
        g.sharedBytes = a.sharedInUse;
        out.gpus.push_back(std::move(g));
    }

    QueryPerformanceCounter(&t1);
    out.collectionMs = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 /
                       static_cast<double>(f.QuadPart);
    return true;
}

} // namespace tmp
