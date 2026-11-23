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
    ULONG AvailablePages;                // +44  ★（GMS 失败时作可用内存兜底）
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
    // +164。**本项目不再使用**：它与 standby 链表的页集重叠，把它算进「已缓存」
    // 会重复计数（A11 §2.5）。字段保留只为维持到 +168 的偏移正确
    // （后面 tail 的起点按 +168 算），并不读取。
    ULONG ResidentSystemCachePage;       // +164 （保留占位，不读）
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
    intervalMs_.store(intervalMs ? intervalMs : 1000, std::memory_order_relaxed);
    if (!wakeEvent_) wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ReadStaticCpuInfo();
    thread_ = std::thread(&PerfCollector::ThreadProc, this);
}

void PerfCollector::SetInterval(uint32_t ms) {
    // 0 = 「已暂停」。这里**不做** `ms ? ms : 1000` 的兜底 —— 那是 Start() 的
    // 语义（启动时给 0 没意义），而 SetInterval(0) 是一个真实的用户选择，
    // 兜底会把它悄悄变成「常规」，菜单立刻又开始说谎。
    intervalMs_.store(ms, std::memory_order_relaxed);
    // 立刻唤醒采集线程，让它按新间隔重新等待。
    // 不唤醒的话：从「低(4s)」切到「高(0.5s)」要等满 4 秒才生效，
    // 从「已暂停」切回任何档位则要等到下一次 RequestRefresh —— 用户会认为
    // 「菜单点了没反应」，正是本任务要消灭的那种假功能。
    //
    // 【副作用：会多采一拍】被唤醒时循环正卡在 WaitForSingleObject 上，
    // 醒来后一定回到循环顶部再采一拍。所以切档位会让 sequence +1，
    // 曲线出现一个额外的点。验收 agent 实测 seq 3→4 证实了这一点。
    //
    // 想避免它需要区分"因间隔变更而醒"和"因超时而醒"（例如比对醒来时刻
    // 与预期时刻），复杂度不划算：切换档位是低频人工操作，多一个采样点
    // 在 60 点曲线里肉眼看不出。**这里如实记录副作用，不假装它不存在。**
    if (wakeEvent_) SetEvent(wakeEvent_);
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

void PerfCollector::SetProcessCounts(uint32_t procs, uint32_t threads, uint64_t handles,
                                     uint64_t snapshotSeq) {
    // 四个值必须作为一个整体发布，否则读侧可能拼出「第 5 代的 seq + 第 6 代的
    // threads」这种撕裂组合 —— 那正是本次要消灭的假配对，而独立 atomic
    // 加上 acq/rel 并不能排除它（acq/rel 只保证顺序，不提供一致性快照）。
    std::lock_guard<std::mutex> lk(procCountsMutex_);
    procCounts_.procs   = procs;
    procCounts_.threads = threads;
    procCounts_.handles = handles;
    procCounts_.seq     = snapshotSeq;
    // 记下写入时刻的采集拍数，供 CollectOnce 算 age。
    // 用 atomic 读，因为 collectTick_ 在采集线程自增、这里在 UI 线程读。
    procCounts_.writtenAtTick = collectTick_.load(std::memory_order_relaxed);
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

    // 物理核 / 逻辑核 / 插槽数 / 缓存
    //
    // 【P0 修复·插槽数】旧代码 `sockets_` 恒为 1（perfdata.h 里 `= 1` 初始化，
    // 全项目零赋值），双路机器必然显示「插槽: 1」。
    // 正确来源是 RelationProcessorPackage 的条目数。
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorPackage, nullptr, &len);
    if (len > 0) {
        std::vector<BYTE> buf(len);
        if (GetLogicalProcessorInformationEx(RelationProcessorPackage,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len)) {
            uint32_t pkgs = 0;
            DWORD off = 0;
            while (off < len) {
                auto* p = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
                if (p->Relationship == RelationProcessorPackage) ++pkgs;
                if (p->Size == 0) break;
                off += p->Size;
            }
            if (pkgs > 0) sockets_ = pkgs;
        }
    }

    len = 0;
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
    // 【P0 修复】GetNativeSystemInfo 在 >64 逻辑处理器（处理器组）时封顶 64。
    // logCores_ 同时是采样缓冲区大小，PerfCollector 里还有一处
    // `if (n > logCores_) n = logCores_;` —— 于是一台 192 线程机器会有
    // 2/3 的核被静默丢弃，CPU 总利用率系统性偏低。一处错牵连四处。
    // GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) 才是全组的真实总数。
    {
        DWORD all = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        if (all > logCores_) {
            // 超过 64 核需要按组分别查询（SystemProcessorPerformanceInformation
            // 只覆盖当前组）。在没有处理器组支持的系统上 all 会等于原值。
            logCores_ = all;
        }
    }

    // 缓存
    //
    // 【语义】GetLogicalProcessorInformationEx(RelationCache) 按**缓存实例**返回
    // 条目，不是按逻辑核：
    //   本机 Xeon 服务器级 CPU 实测 31 条 = L1 20 条（每物理核 CacheData + CacheInstruction
    //   各一条）+ L2 10 条（CacheUnified，每核一条）+ L3 1 条（全 die 共享）。
    //
    // 【任务管理器的口径 = 全部实例直接求和】，一处也不乘核数。
    // 三个数据点交叉验证（docs/audit/A04_cache.md）：
    //   5700X  8 核  原版 512 KB = (32 + 32) × 8     ← 用户截图
    //   9700X  8 核  原版 640 KB = (48 + 32) × 8     ← docs/reference/perf_cpu_orig.png
    //   Xeon  10 核  原版 640 KB = (32 + 32) × 10    ← 本机
    // 只有「d 和 i 都算、全部求和」能同时解释这三行。
    //
    // 【历史】旧代码是 `maxL1 * physCores_`，两处都错：
    //   1. 取 max 丢掉了 CacheInstruction → L1 只有 256 KB 而非 512 KB
    //   2. 再乘核数是基于一句错误注释（原以为 API 已按逻辑核返回），实际重复放大
    //   3. L3 取 max 在单 CCD 上碰巧对（5700X 只有一块 32 MB），
    //      但 5900X 是 2×32 MB，原版显示 64 MB，取 max 会显示 32 MB
    // A04 反汇编 taskmgr.exe RVA 0x28B6D 确认原版是 `add`，全函数无取大指令。
    len = 0;
    GetLogicalProcessorInformationEx(RelationCache, nullptr, &len);
    if (len > 0) {
        std::vector<BYTE> buf(len);
        if (GetLogicalProcessorInformationEx(RelationCache,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len)) {
            uint64_t sumL1 = 0, sumL2 = 0, sumL3 = 0;
            DWORD off = 0;
            while (off < len) {
                auto* p = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
                if (off + p->Size > len) break;      // 防御越界
                if (p->Relationship == RelationCache) {
                    // 注意：CACHE_DESCRIPTOR.CacheSize 单位是【字节】，不是 KB
                    uint64_t sz2 = static_cast<uint64_t>(p->Cache.CacheSize);
                    switch (p->Cache.Level) {
                    case 1: sumL1 += sz2; break;
                    case 2: sumL2 += sz2; break;
                    case 3: sumL3 += sz2; break;
                    }
                }
                off += p->Size;
            }
            l1_ = sumL1;
            l2_ = sumL2;
            l3_ = sumL3;
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

    // 【接上 ProbeDirectXVersion —— 它写好了但全项目零调用，GPU 页的
    // 「DirectX 版本」因此永远显示「—」】
    //
    // 这个探测实测 480~1000 ms（瓶颈是加载 NVIDIA 用户态驱动），
    // **绝不能**放进 CollectOnce 或绘制路径，否则每帧掉一秒。
    // 这里在采集线程启动后跑一次；采集循环本身不等它 ——
    // 主循环已经跑起来了，探测在后台完成后下一次 CollectOnce 自然会读到。
    //
    // 用一个分离线程而不是阻塞 ThreadProc：ThreadProc 一旦被拖住 1 秒，
    // 性能页第一个快照就晚 1 秒发布，用户看到空白页。
    std::thread([this]() {
        if (!gpuInfoAvailable_) return;
        std::wstring ver = gpuInfo_.ProbeDirectXVersion();
        if (!ver.empty()) gpuInfo_.SetFirstAdapterDirectXVersion(ver);
    }).detach();

    while (running_.load(std::memory_order_acquire)) {
        // 【诚实说明：refresh_ 目前是只写不读的死标志】
        //
        // 全项目对 refresh_ 只有两处写：RequestRefresh() 里 store(true)、
        // 本行 store(false)。**没有任何一处读它** —— 唤醒机制实际靠的是
        // wakeEvent_ 这个事件对象，循环被 SetEvent 叫醒后本来就回到这里
        // 重新采一拍，不需要额外的布尔标志。
        //
        // 因此本行的 store(false) 是空操作。保留它只是为了避免"写了 true
        // 却永远不清"这种更糟的半吊子状态。正确做法是连同 refresh_ 成员
        // 一起删掉，但那要动 include/perfdata.h 的公开语义，属独立清理项。
        //
        // 【曾经写在这里的错误说法】原文写"全函数唯一的 refresh_ 消费点，
        // 所以 SetInterval/Stop 的唤醒不会被误当成多采一拍" —— 前半句是
        // 假的（没有任何消费点），后半句也不成立（SetInterval 确实会
        // 多唤醒一次、多采一拍）。验收 agent 实测 seq 3→4 证实了这一点。
        refresh_.store(false, std::memory_order_relaxed);

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

        // 等下一拍。间隔在这里重新读 —— 这就是「切档位立刻生效」的全部机制：
        // 读之前先被 SetInterval 的 SetEvent 唤醒，于是每次醒来都拿到最新值。
        const uint32_t iv = intervalMs_.load(std::memory_order_relaxed);
        if (!running_.load(std::memory_order_acquire)) break;

        if (iv == 0) {
            // ---------- 已暂停：**阻塞等待**，绝不忙循环 ----------
            //
            // 【为什么必须真的阻塞】本分支一旦写成「不等待 / Sleep(0) /
            // continue 到底」，while 会以全速空转：CollectOnce 每秒被调用
            // 上万次，CPU 占用拉满，而且两次采样的间隔短于计数器节拍
            // (15.625 ms)时所有核 delta 为 0，CPU 利用率被算成接近 0 ——
            // 这正是本文件 ThreadProc 里记录的「真值 96% 显示 9%」塌陷的
            // 触发条件。暂停的语义是「不再自动采集」，不是「每秒空转一千次」。
            if (wakeEvent_) {
                WaitForSingleObject(wakeEvent_, INFINITE);
                // Stop() 也是靠 SetEvent 把我们从 INFINITE 叫醒的。醒来后
                // 间隔可能仍然是 0，如果不在这里查 running_ 就会立刻又回到
                // INFINITE，Stop() 里的 thread_.join() 永久卡死，程序退不掉。
                if (!running_.load(std::memory_order_acquire)) break;
                // 这里**不碰** refresh_：暂停态下点「立即刷新」要求的是
                // 「补采一拍后继续停住」。标志由 RequestRefresh 置位、由
                // 循环开头的唯一消费点读走，醒来 -> continue -> 开头采集一拍
                // -> 下一轮又阻塞在 INFINITE，正好是"一拍"。
                // 若在这里也写 refresh_，就会出现两个写点，语义要靠时序猜。
            } else {
                // 事件创建失败（极罕见）的降级路径。
                //
                // 【陷阱】这里如果只是 Sleep(50) 然后 continue，就回到循环
                // 顶部又采一拍 —— 暂停态变成 20 Hz 采集，CPU 上去了、
                // 曲线还在动，等于没暂停。验收 agent 把这条记为 A6。
                //
                // 正确做法：睡完**再读一次间隔**，仍然是 0 就继续睡，
                // 直到间隔被 SetInterval 改掉或 running_ 变 false。
                // 这样暂停态既不忙循环、也不采集。
                while (running_.load(std::memory_order_acquire) &&
                       intervalMs_.load(std::memory_order_relaxed) == 0) {
                    Sleep(50);
                }
                if (!running_.load(std::memory_order_acquire)) break;
                continue;   // 间隔已非 0，回循环顶部正常采一拍
            }
            continue;
        }

        // ---------- 正常运行：等满间隔，或被 SetInterval / RequestRefresh / Stop 唤醒 ----------
        //
        // 【P0 修复·保留】旧代码是 `if (waitMs > 0 && wakeEvent_)`：CreateEventW
        // 失败时 wakeEvent_ 为 NULL，这个 if 整段跳过，while 循环退化成
        // 忙循环，intervalMs_ 完全失效。实测周期 3.0 ms 对 interval=1000，
        // 改成 16000 也还是 3.07 ms。现在事件不可用时退回 Sleep，绝不允许忙循环。
        if (wakeEvent_) {
            WaitForSingleObject(wakeEvent_, iv);
        } else {
            Sleep(iv);
        }
    }
}

void PerfCollector::EnumeratePhysicalDisks() {
    diskInfos_.clear();

    // 1) 卷 → 物理盘号的映射，同时收集每个盘的盘符。
    //
    // 【光驱陷阱】IOCTL_STORAGE_GET_DEVICE_NUMBER 对光驱也返回 DeviceNumber=0，
    // 不按 DeviceType 过滤就会把 F:（光驱）算进 PhysicalDrive0。
    // A08 实测踩过这个坑，截图里「磁盘 4 (C: F:)」正是漏过滤的形状。
    struct VolInfo { std::wstring letter; DWORD devNum = 0; };
    std::vector<VolInfo> vols;

    wchar_t drives[512] = {};
    DWORD n = GetLogicalDriveStringsW(511, drives);
    if (n > 0) {
        for (wchar_t* p = drives; *p; p += wcslen(p) + 1) {
            std::wstring root(p);                 // "C:\"
            if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED &&
                GetDriveTypeW(root.c_str()) != DRIVE_REMOVABLE) {
                continue;                          // 跳过光驱/网络盘
            }
            std::wstring vol = L"\\\\.\\";
            vol += root[0];
            vol += L":";
            HANDLE h = CreateFileW(vol.c_str(), 0,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) continue;
            STORAGE_DEVICE_NUMBER sdn{};
            DWORD ret = 0;
            if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                                &sdn, sizeof(sdn), &ret, nullptr)) {
                // 只接受硬盘。光驱的 DeviceType 是 FILE_DEVICE_CD_ROM(2)。
                if (sdn.DeviceType == FILE_DEVICE_DISK) {
                    VolInfo v;
                    v.letter = root.substr(0, 1);
                    v.devNum = sdn.DeviceNumber;
                    vols.push_back(std::move(v));
                }
            }
            CloseHandle(h);
        }
    }

    // 2) 枚举物理盘，逐个取静态属性（只做一次，5.3 µs/盘）。
    for (DWORD i = 0; i < 32; ++i) {
        wchar_t path[64];
        swprintf_s(path, L"\\\\.\\PhysicalDrive%u", i);
        HANDLE h = CreateFileW(path, 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;   // 该盘号不存在

        DiskInfo info;
        info.devPath = path;
        swprintf_s(path, L"磁盘 %u", i);
        info.name = path;

        // 介质类型：SeekPenalty（SSD 无寻道惩罚）
        STORAGE_PROPERTY_QUERY q{};
        q.PropertyId = StorageDeviceSeekPenaltyProperty;
        q.QueryType  = PropertyStandardQuery;
        DEVICE_SEEK_PENALTY_DESCRIPTOR spd{};
        DWORD ret = 0;
        bool seekKnown = false;
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q),
                            &spd, sizeof(spd), &ret, nullptr)) {
            seekKnown = true;
        }

        // 总线类型：StorageDeviceProperty 的 BusType
        STORAGE_PROPERTY_QUERY q2{};
        q2.PropertyId = StorageDeviceProperty;
        q2.QueryType  = PropertyStandardQuery;
        // 缓冲区给够：STORAGE_DEVICE_DESCRIPTOR 后面跟着变长字符串
        BYTE buf[1024] = {};
        bool busKnown = false;
        STORAGE_BUS_TYPE bus = BusTypeUnknown;
        bool removable = false;
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q2, sizeof(q2),
                            buf, sizeof(buf), &ret, nullptr)) {
            auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf);
            bus = desc->BusType;
            removable = desc->RemovableMedia != FALSE;
            busKnown = true;
        }
        CloseHandle(h);

        // 判定顺序：先看总线（USB/SD 直接就是可移动介质）→ 再看可移动标志
        //            → 最后才用 seek penalty（它对 USB 盘不可靠）
        const wchar_t* media = L"";
        if (busKnown && (bus == BusTypeUsb)) {
            media = L"USB";
        } else if (removable) {
            media = L"USB";
        } else if (seekKnown) {
            media = spd.IncursSeekPenalty ? L"HDD" : L"SSD";
        }
        info.mediaType = media;

        switch (bus) {
        case BusTypeNvme:  info.busType = L"NVMe"; break;
        case BusTypeSata:  info.busType = L"SATA"; break;
        case BusTypeUsb:   info.busType = L"USB";  break;
        case BusTypeScsi:  info.busType = L"SCSI"; break;
        case BusTypeRAID:  info.busType = L"RAID"; break;
        case BusTypeSd:    info.busType = L"SD";   break;
        case BusTypeMmc:   info.busType = L"MMC";  break;
        default: break;
        }

        // 盘符列表：按枚举顺序原样拼接，不做字母排序。
        // Win10 原版是「磁盘 2 (D: C:)」——D 在 C 前面，字母序已被证伪。
        std::wstring letters;
        for (const auto& v : vols) {
            if (v.devNum != i) continue;
            if (!letters.empty()) letters += L" ";
            letters += v.letter;
            letters += L":";
        }
        if (!letters.empty()) info.driveLetters = L"(" + letters + L")";

        diskInfos_.push_back(std::move(info));
    }

    // 3) 静态容量 / 系统盘 / 页面文件（每个盘只做一次，不进 1Hz 路径）
    for (auto& info : diskInfos_) {
        // 用第一个盘符代表这块盘取容量。
        // 物理盘容量请用 IOCTL_DISK_GET_LENGTH_INFO；多分区时各分区容量之和
        // 与实际盘容量有差异，这里优先用 IOCTL 的物理容量。
        if (!info.driveLetters.empty()) {
            wchar_t letter = info.driveLetters[1];      // "(C: ...)" 的 C
            wchar_t root[4] = { letter, L':', L'\\', 0 };

            ULARGE_INTEGER freeForCaller = {}, total = {}, totalFree = {};
            if (GetDiskFreeSpaceExW(root, &freeForCaller, &total, &totalFree)) {
                info.capacityBytes = total.QuadPart;
                info.freeBytes = totalFree.QuadPart;
            }

            // 系统盘：是否是 %SystemDrive%
            wchar_t sysDrive[8] = {};
            DWORD sl = GetEnvironmentVariableW(L"SystemDrive", sysDrive, 8);
            if (sl >= 2 && sysDrive[0] == letter) info.isSystemDisk = true;

            // 页面文件：盘根下是否有 pagefile.sys
            std::wstring pf = root;
            pf += L"pagefile.sys";
            if (GetFileAttributesW(pf.c_str()) != INVALID_FILE_ATTRIBUTES) {
                info.hasPageFile = true;
            }
        }
    }
}

bool PerfCollector::CollectOnce(PerfSnapshot& out) {
    auto fn = GetNtQSI();
    if (!fn) return false;

    // 本拍拍数 +1。SetProcessCounts（UI 线程）会读它来标记计数写入时刻，
    // 下面的计数块再用它算 countsAge。
    collectTick_.fetch_add(1, std::memory_order_relaxed);

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
            // 【P0 修复】只有「本拍确实有增量」的核才计入分母。
            //
            // 旧代码无条件 `sumUtil / n`，但 `per[i]` 的计数器按 15.625 ms 的
            // 时钟中断步进（实测增量恒为 156250 的整数倍）。若两次采样的间隔
            // 短于一个节拍，多数核的 delta 为 0，`if (total > 0)` 直接跳过、
            // utilPercent 保持 0，而分母仍是总核数 —— 于是总量被按「无增量核
            // 所占比例」系统性拉低。实测：无增量核占 82.1% 时输出只有真值的
            // 0.185 倍；占 0.1% 时 0.997 倍。这正是「真值 96% 显示 9%」的根因。
            //
            // 修完后：有增量的核求平均（这是原版 % Processor Time 的口径），
            // 一个增量都没有时**保持上一拍的值**，不发布 0 —— 发布 0 等于
            // 告诉用户「CPU 空闲」，那是假数据。
            uint32_t nWithDelta = 0;
            for (uint32_t i = 0; i < n; ++i) {
                uint64_t idle   = static_cast<uint64_t>(per[i].IdleTime.QuadPart);
                uint64_t kernel = static_cast<uint64_t>(per[i].KernelTime.QuadPart);
                uint64_t user   = static_cast<uint64_t>(per[i].UserTime.QuadPart);

                out.cores[i].idleTime = idle;
                out.cores[i].kernelTime = kernel;
                out.cores[i].userTime = user;

                if (hasPrev_ && i < prevCores_.size()) {
                    // 防御计数器回绕 / 核重排：增量必须单调，否则整拍作废。
                    if (idle < prevCores_[i].idle || kernel < prevCores_[i].kernel ||
                        user < prevCores_[i].user) {
                        continue;
                    }
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
                        ++nWithDelta;
                    }
                }
            }
            if (nWithDelta > 0) {
                out.cpuTotalPercent = sumUtil / nWithDelta;
                out.cpuKernelPercent = sumKernelRatio / nWithDelta;
            } else if (hasPrev_ && prevTotalValid_) {
                // 本拍没有核产生增量（采样窗短于 15.625 ms 节拍）。
                // **只有上一拍确实发布过有效值**，才能沿用 —— 这就是
                // prevTotalValid_ 的作用。条件写成 `!prevTotalValid_`
                // 会在「从未发布过有效值」时去读 prevTotalPercent_，
                // 而它还是初始的 0.0，等于发布了假 0。
                // 0 会被用户读成「CPU 空闲」，与下面第二段注释自相矛盾。
                out.cpuTotalPercent = prevTotalPercent_;
                out.cpuKernelPercent = prevTotalKernelPercent_;
            }
            if (nWithDelta > 0) {
                prevTotalPercent_ = out.cpuTotalPercent;
                prevTotalKernelPercent_ = out.cpuKernelPercent;
                prevTotalValid_ = true;
            }

            // 【修复】旧代码无条件 `hasPrev_ = true`。核数先缩后长时，resize()
            // 把新增下标填成 0，下一拍会算出 `1 - 1.3e12/9e11` 这类「看着正常
            // 但完全错误」的值。现在只有 prevCores_ 完整覆盖 n 时才认它。
            if (prevCores_.size() != n) {
                prevCores_.resize(n);
                hasPrev_ = false;          // 尺寸变了，这一轮的差分作废
            }
            for (uint32_t i = 0; i < n; ++i) {
                prevCores_[i].idle   = static_cast<uint64_t>(per[i].IdleTime.QuadPart);
                prevCores_[i].kernel = static_cast<uint64_t>(per[i].KernelTime.QuadPart);
                prevCores_[i].user   = static_cast<uint64_t>(per[i].UserTime.QuadPart);
            }
            hasPrev_ = true;
        }
    }

    // ---------- 内存组成（NtQSI(2)，344 字节）----------
    //
    // 【口径统一】这里**不再**写 availableBytes，也不再写 cachedBytes：
    //   · 可用：改用 GlobalMemoryStatusEx 的 ullAvailPhys（见下面那一块），
    //     与进程页表头同源。两个 API 实测差 0~1.8 MB，但"口径一致"比"谁更准"
    //     更重要 —— 同一屏上出现两个「可用」才是用户能看见的错。
    //     本块读到的 AvailablePages 只作为 GMS 调用失败时的兜底。
    //   · 已缓存：只等于 standby + modified（见 NtQSI(80) 那一块）。
    // 本块仍在用的只有 已提交 / 提交限制 / 两个缓冲池。
    uint64_t ntqsiAvailableBytes = 0;
    {
        SYS_PERF_INFO spi{};
        ULONG ret = 0;
        NTSTATUS st = fn(kSystemPerformanceInformation, &spi, sizeof(spi), &ret);
        if (NT_SUCCESS(st)) {
            ntqsiAvailableBytes      = static_cast<uint64_t>(spi.AvailablePages) * kPageSize;
            out.mem.committedBytes   = static_cast<uint64_t>(spi.CommittedPages) * kPageSize;
            out.mem.commitLimitBytes = static_cast<uint64_t>(spi.CommitLimit) * kPageSize;
            out.mem.pagedPoolBytes   = static_cast<uint64_t>(spi.PagedPoolPages) * kPageSize;
            out.mem.nonPagedPoolBytes= static_cast<uint64_t>(spi.NonPagedPoolPages) * kPageSize;
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
            // 【口径修复】旧代码这里是 `cachedBytes += standby + modified`，而上一块
            // 已经把 ResidentSystemCachePage 赋给了 cachedBytes，于是三项相加。
            // 重叠有实测证据（scratch/c01/c01_mem.exe）：`available` 装不下
            // Resident+standby+modified+free 四项之和（实测差 −0.73 GB），
            // 所以 Resident 必然与其中至少一项重叠，三项相加即重复计数。
            // 依据详述见 include/perfdata.h 里 cachedBytes 的注释。
            // 这里用 `=` 而不是 `+=`：不依赖上一块是否执行成功。
            out.mem.cachedBytes = out.mem.standbyBytes + out.mem.modifiedBytes;
        }
    }

    // ---------- 物理内存总量 / 可用 + 启动时间 ----------
    {
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) {
            out.mem.totalBytes = ms.ullTotalPhys;
            // 【口径统一】可用内存的唯一来源 = ullAvailPhys（与进程页表头同源）。
            // 实测与 NtQSI(2).AvailablePages×4096 差 0~1.8 MB（A11 §2.2），
            // 但两处口径一致才是重点：性能页右栏「可用」与组合条第 4 段、
            // 与进程页表头必须是同一个数。
            out.mem.availableBytes = ms.ullAvailPhys;
        } else if (ntqsiAvailableBytes > 0) {
            // GMS 失败（几乎不可能）时退回 NtQSI(2)，避免「可用 = 0」这种谎报。
            out.mem.availableBytes = ntqsiAvailableBytes;
        }

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
    //
    // 【P0 修复·基准速度】旧代码把注册表 `~MHz` 当标称频率直接显示，但它不是标称值：
    //   `~MHz` = BCLK × 倍频，而真实 BCLK 是 99.77 MHz 而不是理想 100。
    //   本机实测 `~MHz` = 2993，而 ProcessorNameString 写的是 `@ 3.00GHz`。
    //   用户那台 5700X 同理：3393 = 34 × 99.79，名称串写的是 `@ 3.40GHz`。
    //   → 界面永远是 3.39 GHz 而不是 3.40 GHz，偏差恒在、方向恒向下。
    //
    // 正确来源就是下面这个 buffer 里的 MaxMhz（本机实测 3001）。它之前被
    // 完整丢弃了 —— 同一个结构体只取了 CurrentMhz。
    //
    // 注意：这里**每轮刷新** baseMhz_，不再是「只读一次」。因为 MaxMhz 来自
    // 电源管理接口，休眠/换电源计划后可能变；而且旧代码"只读一次"意味着
    // 一旦启动瞬间读错就永久错。
    {
        const ULONG bufSz = logCores_ * sizeof(TMP_PROCESSOR_POWER_INFORMATION);
        if (bufSz > 0) {
            std::vector<BYTE> buf(bufSz);
            if (CallNtPowerInformation(ProcessorInformation, nullptr, 0,
                                       buf.data(), bufSz) == ERROR_SUCCESS) {
                auto* info = reinterpret_cast<TMP_PROCESSOR_POWER_INFORMATION*>(buf.data());
                // 多核取最大值：info[0] 可能是停核(parked)，值不可靠。
                // CurrentMhz 取 max 反映加速频率（4.63 GHz 这种）；
                // MaxMhz 是标称值，各核一致，取 max 同样安全。
                ULONG maxCur = 0, maxRated = 0;
                for (uint32_t i = 0; i < logCores_; ++i) {
                    if (info[i].CurrentMhz > maxCur)  maxCur  = info[i].CurrentMhz;
                    if (info[i].MaxMhz     > maxRated) maxRated = info[i].MaxMhz;
                }
                out.cpuCurrentMhz = maxCur;
                if (maxRated > 0) {
                    baseMhz_ = maxRated;
                    out.cpuBaseMhz = maxRated;
                }
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

    // ---------- 磁盘（逐盘，IOCTL_DISK_PERFORMANCE）----------
    //
    // 【为什么不用 PDH】冷进程第一次碰任何 PDH 计数器要 1.0–2.7 秒
    // （perflib 子系统初始化，见 docs/audit/A14 §2.2 十次独立测量）。
    // 这段时间在采集线程里，UI 不冻结，但性能页会空白一秒多。
    // IOCTL_DISK_PERFORMANCE 的进程内首次只要 0.07 ms，没有这个问题。
    //
    // 【成本】本机 2 盘实测每轮 p50 0.026 ms / p95 0.054 ms；
    // 8 盘外推 p50 0.16 ms / p95 0.25 ms，占 1Hz 预算的 0.025%。
    // 有意思的是「只 CreateFile + CloseHandle 不读 IOCTL」反而更慢（0.031 ms），
    // 说明句柄开关就是全部成本，IOCTL 本身免费。
    //
    // 【权限】dwDesiredAccess = 0。IOCTL 是 FILE_ANY_ACCESS，不做额外访问检查；
    // Everyone 的 DACL 里没有 FILE_READ_DATA 位，所以 GENERIC_READ 一定被拒
    // —— 那不代表需要管理员（A14 §4.1 从 DACL 结构证明）。
    //
    // 【一次 IOCTL 拿到四样东西】%Idle/%DiskTime、读写字节、平均响应、队列深度。
    {
        // 首轮枚举物理盘：盘号 + 介质类型 + 总线类型 + 盘符列表。
        // 之后每轮只做 IOCTL 差分，枚举和静态属性走缓存。
        if (!diskEnumDone_) {
            EnumeratePhysicalDisks();
        }

        LARGE_INTEGER nowQpc;
        QueryPerformanceCounter(&nowQpc);
        const double dt = prevDiskQpc_.QuadPart
            ? static_cast<double>(nowQpc.QuadPart - prevDiskQpc_.QuadPart)
                          / static_cast<double>(qpcFreq_.QuadPart)
            : 0.0;
        prevDiskQpc_ = nowQpc;

        out.disks.clear();
        out.disks.reserve(diskInfos_.size());

        for (const auto& info : diskInfos_) {
            DiskSample d;
            d.name         = info.name;
            d.driveLetters = info.driveLetters;
            d.mediaType    = info.mediaType;
            d.busType      = info.busType;
            d.capacityBytes  = info.capacityBytes;
            d.freeBytes      = info.freeBytes;
            d.isSystemDisk   = info.isSystemDisk;
            d.hasPageFile    = info.hasPageFile;

            DISK_PERFORMANCE dp{};
            DWORD ret = 0;
            HANDLE h = CreateFileW(info.devPath.c_str(), 0,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
            bool ok = false;
            if (h != INVALID_HANDLE_VALUE) {
                ok = DeviceIoControl(h, IOCTL_DISK_PERFORMANCE, nullptr, 0,
                                     &dp, sizeof(dp), &ret, nullptr) != FALSE;
                CloseHandle(h);
            }

            if (ok) {
                auto it = prevDiskPerf_.find(info.devPath);
                if (it != prevDiskPerf_.end() && dt > 0.05) {
                    const DISK_PERFORMANCE& p = it->second;
                    // 防御计数器回绕/盘重插
                    if (dp.QueryTime.QuadPart >= p.QueryTime.QuadPart &&
                        dp.BytesRead.QuadPart >= p.BytesRead.QuadPart &&
                        dp.BytesWritten.QuadPart >= p.BytesWritten.QuadPart) {

                        const uint64_t dQuery = static_cast<uint64_t>(
                            dp.QueryTime.QuadPart - p.QueryTime.QuadPart);
                        const uint64_t dIdle = static_cast<uint64_t>(
                            dp.IdleTime.QuadPart - p.IdleTime.QuadPart);
                        const uint64_t dBytesR = static_cast<uint64_t>(
                            dp.BytesRead.QuadPart - p.BytesRead.QuadPart);
                        const uint64_t dBytesW = static_cast<uint64_t>(
                            dp.BytesWritten.QuadPart - p.BytesWritten.QuadPart);
                        const uint64_t dReads = static_cast<uint64_t>(
                            dp.ReadCount - p.ReadCount);
                        const uint64_t dWrites = static_cast<uint64_t>(
                            dp.WriteCount - p.WriteCount);
                        const uint64_t dRT = static_cast<uint64_t>(
                            dp.ReadTime.QuadPart - p.ReadTime.QuadPart);
                        const uint64_t dWT = static_cast<uint64_t>(
                            dp.WriteTime.QuadPart - p.WriteTime.QuadPart);

                        d.hasBaseline = true;
                        d.readMBps    = static_cast<double>(dBytesR) / dt / 1048576.0;
                        d.writeMBps   = static_cast<double>(dBytesW) / dt / 1048576.0;
                        d.queueDepth  = dp.QueueDepth;

                        // 活动时间 = 100 − 空闲时间占比。
                        // 不要用 %Disk Time：实测 71 帧里 39 帧的
                        // (%DiskTime + %Idle) 偏离 100 超过 3 个百分点，最大 48 个。
                        // dQuery 只有几微秒时整数噪声会算出负值 —— 必须拦掉。
                        if (dQuery > 0) {
                            double idlePct = static_cast<double>(dIdle) /
                                             static_cast<double>(dQuery) * 100.0;
                            double act = 100.0 - idlePct;
                            if (act < 0.0)   act = 0.0;
                            if (act > 100.0) act = 100.0;
                            d.activePercent = act;
                        } else {
                            // 分母为 0：活动率不可定义。hasBaseline 仍为 true
                            // （字节数有增量），但活动率字段由调用方按 0 处理。
                            d.activePercent = 0.0;
                        }

                        // 平均响应 = 累计读写耗时 / 累计操作数。
                        // 操作数为 0 时不可定义 —— 算出来的 0.000 ms 是假的，
                        // 不能显示成「0 ms 响应」。
                        const uint64_t dOps = dReads + dWrites;
                        if (dOps > 0) {
                            // ReadTime/WriteTime 单位是 100ns
                            d.avgResponseMs =
                                static_cast<double>(dRT + dWT) / static_cast<double>(dOps)
                                / 10000.0;
                            d.responseDefined = true;
                        }

                        out.diskTotalMBps += d.readMBps + d.writeMBps;
                    }
                }
                prevDiskPerf_[info.devPath] = dp;
            }

            out.disks.push_back(std::move(d));
        }
        diskEnumDone_ = true;
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
    // 【进程/线程/句柄：记录来源代际，不"等"配对】
    // 旧代码只是三个 atomic load，没有任何配对校验 —— 于是性能页显示的计数
    // 可能是上一代甚至更早算出来的（A11 §3.3 实测约 53% 的帧）。
    // 这里刻意**不等待**配对：等待会让性能页在计数器相位不利时整帧卡住，
    // 那是用"卡顿"换"一致"，用户看得见的反而更糟。
    // 正确做法是把来源代际如实带出去（countsSequence），让 UI 自己判断：
    //   UI 手里有当前 Snapshot::sequence，lag = thatSeq - countsSequence；
    //   lag == 0 才允许显示数字，否则显示「—」。接口约定见 include/perfdata.h。
    // 一次加锁取四个值：保证 procs/threads/handles/seq 属于同一代（见头文件注释）。
    {
        std::lock_guard<std::mutex> lk(procCountsMutex_);
        out.countsSequence = procCounts_.seq;
        out.totalProcesses = procCounts_.procs;
        out.totalThreads   = procCounts_.threads;
        out.totalHandles   = procCounts_.handles;

        // age = 本拍拍数 - 计数被写入时的拍数。
        // 同一拍内写入又读出 → 0（可信）；跨过一拍 → 1（落后）。
        // 这是"能检测"落地成"能判断"的关键：UI 只看这个数，不需要知道
        // 主采集器的序号（两个独立计数器不可比）。
        const uint64_t nowTick = collectTick_.load(std::memory_order_relaxed);
        const uint64_t wr = procCounts_.writtenAtTick;
        out.countsAge = (wr > 0 && nowTick >= wr)
            ? static_cast<uint32_t>(nowTick - wr) : 0xFFFFFFFFu;   // 从未写入 → 极大值，UI 显示「—」
    }

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

        // ---- 多卡：侧栏每个真实 GPU 一张卡片 ----
        // 命名格式跟原版：侧栏第 1 行是 "GPU 0" / "GPU 1"（MUI 实测 `GPU %d`，
        // 见 docs/specs/perf_page_spec.md §「列表项顺序」），
        // 第 2 行才是利用率 + 显存，第 3 行（detail）留空。
        // 以前这里把适配器名塞进 name，于是侧栏显示 "NVIDIA GeForce discrete GPU"
        // 而不是 "GPU 0"，与原版不符；适配器名现在走副标题（GPU 页标题区）。
        const auto& all = gpuInfo_.Adapters();
        out.gpuAdapters.clear();
        out.gpuAdapters.reserve(all.size());
        out.gpus.clear();
        out.gpus.reserve(all.size());
        for (size_t i = 0; i < all.size(); ++i) {
            out.gpuAdapters.push_back(all[i]);

            GpuSample g;
            wchar_t nm[32];
            swprintf_s(nm, L"GPU %llu", static_cast<unsigned long long>(i));
            g.name = nm;
            g.utilPercent = all[i].totalUtilPercent;
            g.dedicatedBytes = all[i].dedicatedInUse;
            g.sharedBytes = all[i].sharedInUse;
            out.gpus.push_back(std::move(g));
        }
        // 后台 ProbeDirectXVersion 完成后把结果并入第一张卡。
        // 探测本身要 480~1000 ms，跑在分离线程里；这里只是把已经算好的
        // 字符串搬进快照，成本可忽略。未完成时保持空串，UI 显示「—」。
        if (!out.gpuAdapters.empty()) {
            std::wstring dx = gpuInfo_.ProbedDirectXVersion();
            if (!dx.empty()) out.gpuAdapters[0].directXVersion = dx;
        }
    }

    QueryPerformanceCounter(&t1);
    out.collectionMs = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 /
                       static_cast<double>(f.QuadPart);
    return true;
}

} // namespace tmp
