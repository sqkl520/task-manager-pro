// gpuproc.cpp - 每进程 GPU 使用率采集（D3DKMT）实现
//
// ============================================================================
// 实测依据（Win10 Pro 22H2 22H2 / discrete GPU）
// 探针源码保留在 docs/evidence/gpuprobe.cpp、docs/evidence/gputest.cpp、docs/evidence/bench_final.cpp
//
// 1) 路径：gdi32.dll 未文档化导出 D3DKMTEnumAdapters2 + D3DKMTQueryAdapterInfo
//    + D3DKMTQueryStatistics。全部 GetProcAddress 动态解析，不引入 import lib。
//    （gdi32.dll 里这些仍然是"按名字导出"，实测 5 个函数全部解析成功。）
//
// 2) 结构体布局：D3DKMT_QUERYSTATISTICS 在 x64 上必须是 0x328 字节，
//    与 Windows SDK d3dkmthk.h 里的 C_ASSERT 完全一致；本文件在下方用
//    static_assert 锁死 size + 关键字段偏移，编译期即可发现布局漂移。
//
// 3) 每进程查询必须用 PROCESS_QUERY_INFORMATION 打开进程句柄。
//    实测：换成 PROCESS_QUERY_LIMITED_INFORMATION 会得到
//    STATUS_INVALID_PARAMETER (0xC000000D)；普通管理员权限即可，
//    但若拿不到 SE_DEBUG 权限则只能采到本会话/低权限进程。
//
// 4) 适配器枚举会返回 4 个条目，但其中 3 个（虚拟显示 / 远程桌面适配器）
//    对 T_PROCESS / T_PROCESS_SEGMENT_GROUP 返回完全相同的数据 —— 它们
//    是同一套 VidMM 统计的镜像。必须按"D3DKMT_ADAPTERTYPE.Render &&
//    !Software && 有 AdapterString"筛出真实 GPU，否则会重复计算。
//
// 5) 时间单位：D3DKMT_QUERYSTATISTICS_PROCESS_NODE_INFORMATION.RunningTime
//    是 100ns（不是 SDK 注释写的微秒）。实测与 PDH
//    \GPU Engine(*)\Running Time（文档明确 100ns）同步窗口对比，
//    d3d/pdh 合计比 = 1.004~1.063（窗口对齐误差范围）；单引擎 Copy 节点
//    精确到 1.000。因此百分比 = delta / (deltaWall * 1e7) * 100。
//
// 6) 百分比口径：与任务管理器一致，取该进程所有引擎中的最大值（不是求和）。
//    实测与 PDH \GPU Engine(*)\Utilization Percentage 的每 pid 最大值对比：
//    28.370% vs 28.503%、0.072% vs 0.072%、0.052% vs 0.051%…比值 0.99~1.02。
//
// 7) 内存：PROCESS_SEGMENT_GROUP[LOCAL].Usage     == PDH "Local Usage"
//            PROCESS_SEGMENT_GROUP[NON_LOCAL].Usage == PDH "Shared Usage"
//    实测 38/39 个进程字节级精确相等，最差相对误差 0.0045（唯一不等的是
//    正在剧烈变化的 dwm.exe，采样瞬间差异）。
//
// 8) 耗时（实测，Xeon 服务器级 CPU / 系统负载 60~77% / 590~630 个进程）：
//    - 调用方提供 pid（推荐）：中位 0.9~1.2 ms
//    - 内部缓存 pid + 每 10 轮刷新：中位 1.0~1.4 ms，刷新轮偶见 10~14 ms
//    - EnumProcesses 本身就要 11~57 ms，因此绝不在每轮里直接枚举
//    瓶颈全部在 pid 枚举，D3DKMT 查询本身（39 个进程 × 14 引擎）约 0.9 ms。
// ============================================================================

#include "gpuproc.h"

#include <psapi.h>
#include <winternl.h>   // NTSTATUS / NT_SUCCESS（WIN32_LEAN_AND_MEAN 下 windows.h 不带）

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

// EnumProcesses 在 SDK 里被重定向到 Kernel32 的 K32EnumProcesses，
// 需要 psapi.lib。这里自动链接，避免调用方为了加这个模块还要改 build.bat 的
// 链接行（build.bat 不在本模块的可改范围内）。
#pragma comment(lib, "psapi.lib")

namespace tmp {
namespace {

// ---------------------------------------------------------------------------
// D3DKMT 结构体定义
// 逐字段对齐 Windows SDK 的 d3dkmthk.h（该头依赖部分未公开宏，且本项目用
// WIN32_LEAN_AND_MEAN 编译，直接 include 会失败，因此在此按布局重写）。
// 下方 static_assert 锁死尺寸，等价于 SDK 里的 C_ASSERT。
// ---------------------------------------------------------------------------

constexpr uint32_t kMaxNodes = 32;   // 上限保护：实测适配器 NodeCount=14

struct D3DKMT_COUNTER {
    uint32_t Count;
    uint64_t Bytes;
};

struct D3DKMT_DMA_PACKET_TYPE_INFORMATION {
    uint32_t PacketSubmited;
    uint32_t PacketCompleted;
    uint32_t PacketPreempted;
    uint32_t PacketFaulted;
};

struct D3DKMT_QUEUE_PACKET_TYPE_INFORMATION {
    uint32_t PacketSubmited;
    uint32_t PacketCompleted;
};

struct D3DKMT_PACKET_INFORMATION {
    D3DKMT_QUEUE_PACKET_TYPE_INFORMATION QueuePacket[8];
    D3DKMT_DMA_PACKET_TYPE_INFORMATION   DmaPacket[4];
};

struct D3DKMT_PREEMPTION_INFORMATION {
    uint32_t PreemptionCounter[16];
};

// 0x110 = 272 字节
struct D3DKMT_PROCESS_NODE_INFORMATION {
    int64_t  RunningTime;       // 100ns 单位（见文件头第 5 条）
    uint32_t ContextSwitch;
    D3DKMT_PREEMPTION_INFORMATION PreemptionStatistics;
    D3DKMT_PACKET_INFORMATION     PacketStatistics;
    uint64_t Reserved[8];
};

struct D3DKMT_SYSTEM_MEMORY {
    uint64_t BytesAllocated;
    uint64_t BytesReserved;
    uint32_t SmallAllocationBlocks;
    uint32_t LargeAllocationBlocks;
    uint64_t WriteCombinedBytesAllocated;
    uint64_t WriteCombinedBytesReserved;
    uint64_t CachedBytesAllocated;
    uint64_t CachedBytesReserved;
    uint64_t SectionBytesAllocated;
    uint64_t SectionBytesReserved;
    uint64_t BytesZeroed;
};

// 0x90 = 144 字节
struct D3DKMT_PROCESS_INFORMATION {
    uint32_t NodeCount;
    uint32_t VidPnSourceCount;
    D3DKMT_SYSTEM_MEMORY SystemMemory;
    uint64_t Reserved[7];
};

struct D3DKMT_DMA_BUFFER {
    D3DKMT_COUNTER Size;
    uint32_t AllocationListBytes;
    uint32_t PatchLocationListBytes;
};

struct D3DKMT_COMMITMENT_DATA {
    uint64_t TotalBytesEvictedFromProcess;
    uint64_t BytesBySegmentPreference[5];
};

struct D3DKMT_POLICY {
    uint64_t PreferApertureForRead[5];
    uint64_t PreferAperture[5];
    uint64_t MemResetOnPaging;
    uint64_t RemovePagesFromWorkingSetOnPaging;
    uint64_t MigrationEnabled;
};

struct D3DKMT_PROCESS_INTERFERENCE_COUNTERS {
    uint64_t InterferenceCount[9];
};

// 0x110 = 272 字节
struct D3DKMT_PROCESS_ADAPTER_INFORMATION {
    uint32_t NbSegments;
    uint32_t NodeCount;
    uint32_t VidPnSourceCount;
    uint32_t VirtualMemoryUsage;
    D3DKMT_DMA_BUFFER DmaBuffer;
    D3DKMT_COMMITMENT_DATA CommitmentData;
    D3DKMT_POLICY _Policy;
    D3DKMT_PROCESS_INTERFERENCE_COUNTERS ProcessInterferenceCounters;
    uint32_t ClientHint;
};

struct D3DKMT_VIDEO_MEMORY {
    uint32_t AllocsCommitted;
    D3DKMT_COUNTER AllocsResidentInP[5];
    D3DKMT_COUNTER AllocsResidentInNonPreferred;
    uint64_t TotalBytesEvictedDueToPreparation;
};

struct D3DKMT_PROCESS_SEGMENT_POLICY {
    uint64_t UseMRU;
};

// 0xD8 = 216 字节
struct D3DKMT_PROCESS_SEGMENT_INFORMATION {
    uint64_t BytesCommitted;
    uint64_t MaximumWorkingSet;
    uint64_t MinimumWorkingSet;
    uint32_t NbReferencedAllocationEvictedInPeriod;
    uint32_t Padding;
    D3DKMT_VIDEO_MEMORY VideoMemory;
    D3DKMT_PROCESS_SEGMENT_POLICY _Policy;
    uint64_t Reserved[8];
};

// 0x40 = 64 字节
struct D3DKMT_PROCESS_SEGMENT_GROUP_INFORMATION {
    uint64_t Budget;
    uint64_t Requested;
    uint64_t Usage;
    uint64_t Demoted[5];
};

struct D3DKMT_MEMORY {
    uint64_t TotalBytesEvicted;
    uint32_t AllocsCommitted;
    uint32_t AllocsResident;
};

// 0x98 = 152 字节
struct D3DKMT_SEGMENT_INFORMATION {
    uint64_t CommitLimit;
    uint64_t BytesCommitted;
    uint64_t BytesResident;
    D3DKMT_MEMORY Memory;
    uint32_t Aperture;
    uint64_t TotalBytesEvictedByPriority[5];
    uint64_t SystemMemoryEndAddress;
    uint64_t PowerFlags;
    uint64_t SegmentProperties;
    uint64_t Reserved[5];
};

// 本模块不使用 T_NODE（适配器全局统计），但 union 尺寸由它决定，故保留按
// SDK 布局的完整尺寸 0x268 = 616。前 0x220 是两个 ProcessNodeInformation，
// 其后是 D3DKMT_NODE_PERFDATA + Reserved，共 0x48 字节。
struct D3DKMT_NODE_INFORMATION {
    D3DKMT_PROCESS_NODE_INFORMATION GlobalInformation;
    D3DKMT_PROCESS_NODE_INFORMATION SystemInformation;
    uint8_t Tail[0x48];
};

// 0x308 = 776 字节（各成员并集，故整体尺寸取最大成员）
struct D3DKMT_QUERYSTATISTICS_ADAPTER_INFORMATION {
    uint32_t NbSegments;
    uint32_t NodeCount;
    uint32_t VidPnSourceCount;
    uint32_t VSyncEnabled;
    uint32_t TdrDetectedCount;
    int64_t  ZeroLengthDmaBuffers;
    uint64_t RestartedPeriod;
    uint8_t  Rest[0x308 - 0x28];
};

union D3DKMT_QUERYSTATISTICS_RESULT {
    D3DKMT_QUERYSTATISTICS_ADAPTER_INFORMATION AdapterInformation;
    D3DKMT_SEGMENT_INFORMATION                 SegmentInformation;
    D3DKMT_NODE_INFORMATION                    NodeInformation;
    D3DKMT_PROCESS_INFORMATION                 ProcessInformation;
    D3DKMT_PROCESS_ADAPTER_INFORMATION         ProcessAdapterInformation;
    D3DKMT_PROCESS_SEGMENT_INFORMATION         ProcessSegmentInformation;
    D3DKMT_PROCESS_NODE_INFORMATION            ProcessNodeInformation;
    D3DKMT_PROCESS_SEGMENT_GROUP_INFORMATION   ProcessSegmentGroupInformation;
};

// 0x328 = 808 字节
struct D3DKMT_QUERYSTATISTICS {
    uint32_t    Type;
    LUID        AdapterLuid;
    HANDLE      hProcess;
    D3DKMT_QUERYSTATISTICS_RESULT QueryResult;
    union {
        uint32_t QuerySegmentId;
        uint32_t QueryProcessSegmentId;
        uint32_t QueryProcessSegmentGroup;
        uint32_t QueryNodeId;
        uint32_t QueryProcessNodeId;
        uint32_t QueryVidPnSourceId;
        uint32_t QueryProcessVidPnSourceId;
        uint32_t QueryPhysAdapterIndex;
    };
};

// ---- 布局锁定（对齐 SDK d3dkmthk.h 的 C_ASSERT 与实测偏移）----
static_assert(sizeof(D3DKMT_QUERYSTATISTICS) == 0x328,
              "D3DKMT_QUERYSTATISTICS must be 808 bytes on x64");
static_assert(offsetof(D3DKMT_QUERYSTATISTICS, Type) == 0x000, "Type offset");
static_assert(offsetof(D3DKMT_QUERYSTATISTICS, AdapterLuid) == 0x004, "LUID offset");
static_assert(offsetof(D3DKMT_QUERYSTATISTICS, hProcess) == 0x010, "hProcess offset");
static_assert(offsetof(D3DKMT_QUERYSTATISTICS, QueryResult) == 0x018, "QueryResult offset");
static_assert(offsetof(D3DKMT_QUERYSTATISTICS, QueryNodeId) == 0x320, "input union offset");
static_assert(sizeof(D3DKMT_QUERYSTATISTICS_RESULT) == 0x308, "result union size");
static_assert(sizeof(D3DKMT_PROCESS_NODE_INFORMATION) == 0x110, "process node size");
static_assert(sizeof(D3DKMT_PROCESS_ADAPTER_INFORMATION) == 0x110, "process adapter size");
static_assert(sizeof(D3DKMT_PROCESS_SEGMENT_GROUP_INFORMATION) == 0x40, "segment group size");
static_assert(sizeof(D3DKMT_PROCESS_SEGMENT_INFORMATION) == 0xD8, "process segment size");
static_assert(sizeof(D3DKMT_SEGMENT_INFORMATION) == 0x98, "segment size");
static_assert(sizeof(D3DKMT_NODE_INFORMATION) == 0x268, "node size");
static_assert(offsetof(D3DKMT_PROCESS_NODE_INFORMATION, RunningTime) == 0x00, "RunningTime offset");
static_assert(offsetof(D3DKMT_PROCESS_SEGMENT_GROUP_INFORMATION, Usage) == 0x10, "Usage offset");
static_assert(offsetof(D3DKMT_PROCESS_SEGMENT_INFORMATION, BytesCommitted) == 0x00, "BytesCommitted");

// ---------------------------------------------------------------------------
// 未文档化枚举 / 结构（只用得到的部分）
// ---------------------------------------------------------------------------
enum D3DKMT_QUERYSTATISTICS_TYPE : uint32_t {
    D3DKMT_QUERYSTATISTICS_TYPE_ADAPTER               = 0,
    D3DKMT_QUERYSTATISTICS_TYPE_PROCESS               = 1,
    D3DKMT_QUERYSTATISTICS_TYPE_PROCESS_ADAPTER       = 2,
    D3DKMT_QUERYSTATISTICS_TYPE_SEGMENT               = 3,
    D3DKMT_QUERYSTATISTICS_TYPE_PROCESS_SEGMENT       = 4,
    D3DKMT_QUERYSTATISTICS_TYPE_NODE                  = 5,
    D3DKMT_QUERYSTATISTICS_TYPE_PROCESS_NODE          = 6,
    D3DKMT_QUERYSTATISTICS_TYPE_VIDPNSOURCE           = 7,
    D3DKMT_QUERYSTATISTICS_TYPE_PROCESS_VIDPNSOURCE   = 8,
    D3DKMT_QUERYSTATISTICS_TYPE_PROCESS_SEGMENT_GROUP = 9,
};

constexpr uint32_t kSegmentGroupLocal    = 0;   // D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL
constexpr uint32_t kSegmentGroupNonLocal = 1;

struct ADAPTERINFO_LOCAL {
    uint32_t hAdapter;
    LUID     AdapterLuid;
    uint32_t NumOfSources;
    BOOL     bPrecisePresentRegionsPreferred;
};

struct ENUMADAPTERS2_LOCAL {
    uint32_t          NumAdapters;
    ADAPTERINFO_LOCAL* pAdapters;
};

// KMTQAITYPE_ADAPTERREGISTRYINFO = 8
struct ADAPTERREGISTRYINFO_LOCAL {
    wchar_t AdapterString[MAX_PATH];
    wchar_t BiosString[MAX_PATH];
    wchar_t DacType[MAX_PATH];
    wchar_t ChipType[MAX_PATH];
};

// KMTQAITYPE_ADAPTERTYPE = 15
union ADAPTERTYPE_LOCAL {
    struct {
        uint32_t RenderSupported       : 1;
        uint32_t DisplaySupported      : 1;
        uint32_t SoftwareDevice        : 1;
        uint32_t PostDevice            : 1;
        uint32_t HybridDiscrete        : 1;
        uint32_t HybridIntegrated      : 1;
        uint32_t IndirectDisplayDevice : 1;
        uint32_t Paravirtualized       : 1;
        uint32_t ACGSupported          : 1;
        uint32_t SupportSetTimingsFromVidPn : 1;
        uint32_t Detachable            : 1;
        uint32_t ComputeOnly           : 1;
        uint32_t Prototype             : 1;
        uint32_t RuntimePowerManagement: 1;
        uint32_t Reserved              : 18;
    };
    uint32_t Value;
};

struct QUERYADAPTERINFO_LOCAL {
    uint32_t hAdapter;
    uint32_t Type;
    void*    pPrivateDriverData;
    uint32_t PrivateDriverDataSize;
};

constexpr uint32_t kKmtQueryAdapterInfoTypeAdapterRegistryInfo = 8;
constexpr uint32_t kKmtQueryAdapterInfoTypeAdapterType        = 15;

// ---------------------------------------------------------------------------
// 内部辅助
// ---------------------------------------------------------------------------
using PFN_D3DKMT_ENUMADAPTERS2      = NTSTATUS (NTAPI*)(ENUMADAPTERS2_LOCAL*);
using PFN_D3DKMT_QUERYADAPTERINFO   = NTSTATUS (NTAPI*)(QUERYADAPTERINFO_LOCAL*);
using PFN_D3DKMT_QUERYSTATISTICS    = NTSTATUS (NTAPI*)(const D3DKMT_QUERYSTATISTICS*);
using PFN_D3DKMT_CLOSEADAPTER       = NTSTATUS (NTAPI*)(const void*);

constexpr NTSTATUS kStatusSuccess = 0;

inline bool NtOk(NTSTATUS s) { return s >= kStatusSuccess; }

// QueryPerformanceCounter 换算
double QpcNowSeconds() {
    static LARGE_INTEGER freq{};
    static bool init = false;
    if (!init) { QueryPerformanceFrequency(&freq); init = true; }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (!freq.QuadPart) return 0.0;
    return static_cast<double>(t.QuadPart) / static_cast<double>(freq.QuadPart);
}

} // namespace

// ---------------------------------------------------------------------------
// Impl：所有可变状态放这里，头文件保持轻量
// ---------------------------------------------------------------------------
struct GpuProcCollector::Impl {
    HMODULE gdi32 = nullptr;
    PFN_D3DKMT_ENUMADAPTERS2    EnumAdapters2 = nullptr;
    PFN_D3DKMT_QUERYADAPTERINFO QueryAdapterInfo = nullptr;
    PFN_D3DKMT_QUERYSTATISTICS  QueryStatistics = nullptr;
    PFN_D3DKMT_CLOSEADAPTER     CloseAdapter = nullptr;

    // 选中的真实 GPU
    LUID      gpuLuid{};
    uint32_t  gpuAdapterHandle = 0;
    uint32_t  nodeCount = 0;

    // ---- 句柄缓存：只在有新 pid 时 OpenProcess ----
    struct HandleEntry {
        HANDLE   handle = nullptr;
        uint64_t createTime = 0;   // 句柄对应进程的创建时间（检测 pid 复用）
        bool     stale = false;    // 周期性失效标记，下一轮按需重开
        bool     openFailed = false; // 打不开（系统/受保护进程），降低重试频率
        bool     usable = false;   // PROCESS_ADAPTER 是否成功（该进程有 GPU 记录）
        bool     peeked = false;   // 是否已确认过 GPU 记录状态
        uint32_t nodeCount = 0;    // 该进程的引擎数
    };
    struct ProcessInfo { uint64_t createTime = 0; };
    // 按 pid 下标直接索引，先按 65536 预分配，访问到更大 pid 时由
    // EnsureCapacity 继续按 4096 一档增长。
    //
    // 【内存占用订正】原注释写「每项 16 字节 = 64 MB 上限」，两个数都错了：
    //   HandleEntry 实际 24 字节（HANDLE 8 + u64 8 + 4×bool 4 + u32 4，对齐到 8）。
    //   本机实测最大 pid ≈ 69460，EnsureCapacity 会涨到 73728 项 ≈ **1.77 MB**。
    //   理论上限（按 pid 空间 2^22）是 96 MB，但那要求 pid 真的涨到 400 万，
    //   Windows 的 pid 从 4 的倍数起递增、长时间运行也很少超过十万，
    //   所以实际占用稳定在 2 MB 量级，**不是随时间增长的泄漏**。
    //   它只在遇到更大的 pid 时跳一档（整块 resize + memset），
    //   表现为偶发的采集抖动，不是稳态变慢。
    std::vector<HandleEntry> handleCache;
    uint32_t handleCacheGeneration = 0;   // 用于定期丢弃失效句柄

    // ---- 上一轮快照（用于差分）----
    // 用哈希表而不是按 pid 下标的大数组：PrevProc 每个 264 字节，
    // 若按 pid 下标预分配 65536 项要 17 MB，每次增容都是大块 memcpy，
    // 实测会吃掉每次采集 3 ms 以上。实际只有几十个进程有 GPU 数据，
    // 哈希表规模天然与之匹配。
    struct PrevProc {
        int64_t  run[kMaxNodes] = {};
        bool     present = false;
    };
    std::unordered_map<uint32_t, PrevProc> prev;

    void EnsureCapacity(uint32_t pid) {
        if (pid < handleCache.size()) return;
        size_t want = (static_cast<size_t>(pid) / 4096 + 1) * 4096;
        handleCache.resize(want);
    }
    bool     hasPrev = false;
    double   prevTimestamp = 0.0;

    // 复用缓冲区，避免每轮分配
    std::vector<uint32_t> pidBuffer;
    std::vector<uint32_t> cachedPids;    // 缓存的 pid 列表（见 Collect 中的说明）
    uint32_t pidRefreshTick = 0;
    // 上一轮见过的 pid 列表。用于在两轮之间找出"已退出、但 handleCache 里
    // 还挂着句柄"的进程并关闭它们。见 CollectFromPids 末尾的 6b 段。
    std::vector<uint32_t> lastPidList;
};

GpuProcCollector::GpuProcCollector() : impl_(new Impl()) {}

GpuProcCollector::~GpuProcCollector() {
    if (!impl_) return;
    // 释放缓存的进程句柄
    for (auto& e : impl_->handleCache) {
        if (e.handle) CloseHandle(e.handle);
    }
    // 关闭适配器
    if (impl_->CloseAdapter && impl_->gpuAdapterHandle) {
        uint32_t h = impl_->gpuAdapterHandle;
        impl_->CloseAdapter(&h);
    }
    if (impl_->gdi32) FreeLibrary(impl_->gdi32);
    delete impl_;
    impl_ = nullptr;
}

bool GpuProcCollector::Init() {
    available_ = false;
    adapterName_[0] = L'\0';
    if (!impl_) impl_ = new Impl();

    // ---- 1. 加载 gdi32 并解析 D3DKMT 导出 ----
    impl_->gdi32 = LoadLibraryW(L"gdi32.dll");
    if (!impl_->gdi32) return false;

    impl_->EnumAdapters2 = reinterpret_cast<PFN_D3DKMT_ENUMADAPTERS2>(
        GetProcAddress(impl_->gdi32, "D3DKMTEnumAdapters2"));
    impl_->QueryAdapterInfo = reinterpret_cast<PFN_D3DKMT_QUERYADAPTERINFO>(
        GetProcAddress(impl_->gdi32, "D3DKMTQueryAdapterInfo"));
    impl_->QueryStatistics = reinterpret_cast<PFN_D3DKMT_QUERYSTATISTICS>(
        GetProcAddress(impl_->gdi32, "D3DKMTQueryStatistics"));
    impl_->CloseAdapter = reinterpret_cast<PFN_D3DKMT_CLOSEADAPTER>(
        GetProcAddress(impl_->gdi32, "D3DKMTCloseAdapter"));

    // QueryStatistics 是唯一真正必需的；Adapter 枚举失败还可以靠 fallback。
    if (!impl_->QueryStatistics) return false;

    // ---- 2. 枚举适配器，挑出"真实 GPU" ----
    //    实测本机枚举出 4 个：2 个虚拟显示适配器(Indirect) + 1 个 NVIDIA + 1 个软件渲染。
    //    其中前 3 个的 PROCESS 统计完全相同（同一 VidMM 后端的镜像），
    //    因此必须严格筛选，否则会重复计数。
    if (impl_->EnumAdapters2 && impl_->QueryAdapterInfo) {
        ADAPTERINFO_LOCAL raw[16]{};
        ENUMADAPTERS2_LOCAL ea{};
        ea.NumAdapters = 16;
        ea.pAdapters = raw;

        if (NtOk(impl_->EnumAdapters2(&ea))) {
            uint32_t count = ea.NumAdapters < 16 ? ea.NumAdapters : 16;
            for (uint32_t i = 0; i < count; ++i) {
                ADAPTERREGISTRYINFO_LOCAL reg{};
                QUERYADAPTERINFO_LOCAL q1{};
                q1.hAdapter = raw[i].hAdapter;
                q1.Type = kKmtQueryAdapterInfoTypeAdapterRegistryInfo;
                q1.pPrivateDriverData = &reg;
                q1.PrivateDriverDataSize = sizeof(reg);
                bool haveName = NtOk(impl_->QueryAdapterInfo(&q1)) && reg.AdapterString[0] != L'\0';

                ADAPTERTYPE_LOCAL at{};
                QUERYADAPTERINFO_LOCAL q2{};
                q2.hAdapter = raw[i].hAdapter;
                q2.Type = kKmtQueryAdapterInfoTypeAdapterType;
                q2.pPrivateDriverData = &at;
                q2.PrivateDriverDataSize = sizeof(at);
                bool haveType = NtOk(impl_->QueryAdapterInfo(&q2));

                // 真实 GPU 判定：支持渲染、非软件设备、有可读的名字
                bool isRealGpu = haveName && (!haveType || (at.RenderSupported && !at.SoftwareDevice));
                if (!isRealGpu) continue;

                // 确认能对它做统计查询
                D3DKMT_QUERYSTATISTICS qs{};
                qs.Type = D3DKMT_QUERYSTATISTICS_TYPE_ADAPTER;
                qs.AdapterLuid = raw[i].AdapterLuid;
                qs.hProcess = nullptr;
                if (!NtOk(impl_->QueryStatistics(&qs))) continue;

                impl_->gpuLuid = raw[i].AdapterLuid;
                impl_->gpuAdapterHandle = raw[i].hAdapter;
                impl_->nodeCount = qs.QueryResult.AdapterInformation.NodeCount;
                if (impl_->nodeCount > kMaxNodes) impl_->nodeCount = kMaxNodes;
                wcsncpy_s(adapterName_, reg.AdapterString, _TRUNCATE);
                break;
            }
        }
    }

    // ---- 3. 兜底：没有任何可用适配器则降级 ----
    //    实测对无效 LUID 查询会返回 STATUS_INVALID_PARAMETER，不存在"省略 LUID"
    //    的通用路径，因此找不到真实 GPU 就明确降级为不可用。
    if (impl_->nodeCount == 0) return false;

    impl_->pidBuffer.resize(16384);
    // 句柄缓存按 pid 下标直接索引，先按 65536 预分配（约 1.6 MB），
    // 访问到更大 pid 时由 EnsureCapacity 继续按 4096 一档增长。
    impl_->handleCache.assign(65536, Impl::HandleEntry{});
    impl_->prev.reserve(1024);

    // 预先枚举一次 pid，让首次 Collect() 就能返回数据（Init 只调用一次，
    // 这次枚举的开销不影响每秒的采集预算）。
    {
        DWORD got = 0;
        if (EnumProcesses(reinterpret_cast<DWORD*>(impl_->pidBuffer.data()),
                          static_cast<DWORD>(impl_->pidBuffer.size() * sizeof(uint32_t)),
                          &got)) {
            const uint32_t n = got / static_cast<uint32_t>(sizeof(uint32_t));
            impl_->cachedPids.assign(impl_->pidBuffer.begin(),
                                     impl_->pidBuffer.begin() + n);
        }
    }

    available_ = true;
    return true;
}

void GpuProcCollector::Collect(std::vector<GpuProcSample>& out) {
    out.clear();
    if (!available_ || !impl_ || !impl_->QueryStatistics) {
        lastCollectMs_ = 0.0;
        return;
    }

    const double t0 = QpcNowSeconds();

    // ---- 1. 取 pid 列表 ----
    // 关键实测：本机 600+ 进程时 EnumProcesses 自己就要 11~57 ms（内核要重建
    // 整份进程快照，约 1.2 MB），而 GPU 查询本身只要 0.9~1.7 ms。
    // 所以绝不允许每轮都枚举：这里缓存 pid 列表，并且把"刷新 pid"和"跑 GPU
    // 查询"拆成两次调用 —— 刷新轮只做枚举就返回，保证任何单次调用都不会
    // 同时承担枚举 + 查询的开销。
    // 调用方若已有 pid 列表（复用主进程快照），请用
    // Collect(pids, count, out)，那是唯一能稳定 < 2 ms 的路径。
    const bool needRefresh = impl_->cachedPids.empty();
    if (needRefresh) {
        DWORD got = 0;
        if (impl_->pidBuffer.empty()) impl_->pidBuffer.resize(16384);
        const double tr0 = QpcNowSeconds();
        if (!EnumProcesses(reinterpret_cast<DWORD*>(impl_->pidBuffer.data()),
                           static_cast<DWORD>(impl_->pidBuffer.size() * sizeof(uint32_t)),
                           &got)) {
            lastCollectMs_ = (QpcNowSeconds() - t0) * 1000.0;
            return;
        }
        const uint32_t n = got / static_cast<uint32_t>(sizeof(uint32_t));
        impl_->cachedPids.assign(impl_->pidBuffer.begin(),
                                 impl_->pidBuffer.begin() + n);
        (void)tr0;
        // 本轮只完成 pid 枚举，CPU 交给下一次调用（避免单次超预算）。
        lastCollectMs_ = (QpcNowSeconds() - t0) * 1000.0;
        return;
    }

    CollectFromPids(impl_->cachedPids.data(), impl_->cachedPids.size(), out, t0);

    // 每 10 轮清掉一次 pid 缓存，让下一次调用重新枚举（新进程最多 10 秒后可见）。
    // 清空而不立即枚举，可把 EnumProcesses 的开销隔离到单独一轮。
    if (++impl_->pidRefreshTick >= 10) {
        impl_->pidRefreshTick = 0;
        impl_->cachedPids.clear();
    }
}

void GpuProcCollector::RefreshPids() {
    if (!impl_) return;
    impl_->cachedPids.clear();
    impl_->pidRefreshTick = 0;
}

void GpuProcCollector::Collect(const uint32_t* pids, size_t count,
                               std::vector<GpuProcSample>& out) {
    out.clear();
    if (!available_ || !impl_ || !impl_->QueryStatistics || !pids || count == 0) {
        lastCollectMs_ = 0.0;
        return;
    }
    const double t0 = QpcNowSeconds();
    CollectFromPids(pids, count, out, t0);
}

void GpuProcCollector::CollectFromPids(const uint32_t* pids, size_t count,
                                       std::vector<GpuProcSample>& out, double t0) {
    // 周期性让缓存句柄"过期"：不在这里逐个 CloseHandle（600 个句柄的关闭
    // 成本会集中到一轮），而是打上 stale 标记，由下面的循环按需重开。
    // 同时清掉 peeked，让"上轮无 GPU 记录"的进程也重新复验一次。
    if ((impl_->handleCacheGeneration++ % 60u) == 0u) {
        for (auto& e : impl_->handleCache) {
            if (e.handle) { e.stale = true; e.peeked = false; }
        }
    }

    out.reserve(64);
    std::vector<int64_t> maxNodeDeltas;
    maxNodeDeltas.reserve(64);

    for (size_t i = 0; i < count; ++i) {
        const uint32_t pid = pids[i];
        if (pid == 0) continue;

        impl_->EnsureCapacity(pid);
        Impl::HandleEntry& entry = impl_->handleCache[pid];

        // ---- 2. 句柄 ----
        // 必须 PROCESS_QUERY_INFORMATION：PROCESS_QUERY_LIMITED_INFORMATION
        // 会让 D3DKMTQueryStatistics 返回 STATUS_INVALID_PARAMETER。
        //
        // 已知打不开的进程（系统/受保护进程）记 openFailed，之后每 30 轮才重试，
        // 避免每轮对 500+ 个 pid 做无谓的 OpenProcess。
        constexpr uint32_t kProbeEvery = 30;
        if (!entry.handle && entry.openFailed) {
            if ((impl_->handleCacheGeneration % kProbeEvery) != 0) continue;
            entry.openFailed = false;
        }

        // 注意：Windows 会复用 pid。stale 标记每 60 轮让所有句柄重开一次，
        // 重开时用 GetProcessTimes 重新确认进程身份，避免把已退出进程的
        // 统计数据张冠李戴到复用了同一 pid 的新进程上。
        if (!entry.handle || entry.stale) {
            if (entry.handle) { CloseHandle(entry.handle); entry.handle = nullptr; }
            HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
            FILETIME c, e, k, u;
            if (!h || !GetProcessTimes(h, &c, &e, &k, &u)) {
                if (h) CloseHandle(h);
                entry.usable = false;
                entry.nodeCount = 0;
                entry.stale = false;
                // 【补上遗漏】上面那个「打不开的进程每 30 轮才重试」的节流
                // （`if (!entry.handle && entry.openFailed)`）依赖这个标记，
                // 但它此前**从未被置 true** —— 等于节流是死的，受保护进程
                // 每轮都白跑一次 OpenProcess。用户机器 600+ 进程里有几十个
                // 系统/受保护进程，每秒几十次失败的 OpenProcess 完全是浪费。
                entry.openFailed = true;
                continue;
            }
            entry.handle = h;
            entry.createTime = (static_cast<uint64_t>(c.dwHighDateTime) << 32) | c.dwLowDateTime;
            entry.stale = false;
        } else if (entry.handle && entry.createTime != 0 && entry.usable) {
            // 【pid 复用校验】原来 `entry.createTime` 只写不读，
            // 注释宣称的「重开时重新确认进程身份」从未实现。
            //
            // 【必须把条件收紧到 entry.usable，否则是每秒 600 次 syscall】
            // 第一版把这段挂在单纯的 else 上，结果是：
            //   - stale 每 60 轮才置一次，所以 59/60 的轮次都走这里；
            //   - 而且它排在 `if (entry.peeked && !entry.usable) continue`
            //     **之前** —— 那 560 个「无 GPU 记录」的进程每秒都被摸一次，
            //     而它们本来正是靠 peeked 被跳过的。
            // 加上 entry.usable 之后，只有真正在出 GPU 数据的进程
            // （本机约 38 个）才付这个代价，量级从 600/秒 降到 38/秒。
            //
            // 再收紧一档：只在「上一拍没校验过」时校验 —— 用 entry.stale
            // 那一拍本来就已重开句柄、不需要额外校验，所以这里只在
            // 非 stale 且 usable 的轮次做。
            FILETIME c2, e2, k2, u2;
            if (GetProcessTimes(entry.handle, &c2, &e2, &k2, &u2)) {
                const uint64_t ct2 = (static_cast<uint64_t>(c2.dwHighDateTime) << 32) | c2.dwLowDateTime;
                if (ct2 != entry.createTime) {
                    // pid 已被复用 → 关掉旧句柄，本轮跳过（下轮重新打开新进程）
                    CloseHandle(entry.handle);
                    entry.handle = nullptr;
                    entry.usable = false;
                    entry.peeked = false;
                    entry.nodeCount = 0;
                    entry.createTime = 0;
                    continue;
                }
            }
        }

        // ---- 3. PROCESS_ADAPTER：既有每进程引擎数，也是"有无 GPU 记录"的门 ----
        // 绝大多数进程（600 个里的 560 个）没有任何 GPU 记录，查询会直接失败。
        // 这类进程记 usable=false 后就不再重试，否则每轮都要 CloseHandle +
        // 下一轮 OpenProcess，白白增加抖动与开销。
        D3DKMT_QUERYSTATISTICS qa{};
        qa.Type = D3DKMT_QUERYSTATISTICS_TYPE_PROCESS_ADAPTER;
        qa.AdapterLuid = impl_->gpuLuid;
        qa.hProcess = entry.handle;
        if (entry.peeked && !entry.usable) continue;   // 上轮已确认无 GPU 记录
        if (!NtOk(impl_->QueryStatistics(&qa))) {
            entry.usable = false;
            entry.nodeCount = 0;
            entry.peeked = true;
            continue;
        }
        uint32_t nodes = qa.QueryResult.ProcessAdapterInformation.NodeCount;
        if (nodes > kMaxNodes) nodes = kMaxNodes;
        entry.usable = true;
        entry.nodeCount = nodes;

        // ---- 4. 每引擎 RunningTime + 两个段组的内存 ----
        GpuProcSample s;
        s.pid = pid;

        int64_t run[kMaxNodes] = {};
        for (uint32_t n = 0; n < nodes; ++n) {
            D3DKMT_QUERYSTATISTICS qn{};
            qn.Type = D3DKMT_QUERYSTATISTICS_TYPE_PROCESS_NODE;
            qn.AdapterLuid = impl_->gpuLuid;
            qn.hProcess = entry.handle;
            qn.QueryProcessNodeId = n;
            if (NtOk(impl_->QueryStatistics(&qn))) {
                run[n] = qn.QueryResult.ProcessNodeInformation.RunningTime;
            }
        }

        for (uint32_t grp = 0; grp < 2; ++grp) {
            D3DKMT_QUERYSTATISTICS qg{};
            qg.Type = D3DKMT_QUERYSTATISTICS_TYPE_PROCESS_SEGMENT_GROUP;
            qg.AdapterLuid = impl_->gpuLuid;
            qg.hProcess = entry.handle;
            qg.QueryProcessSegmentGroup = grp;
            if (!NtOk(impl_->QueryStatistics(&qg))) continue;
            uint64_t usage = qg.QueryResult.ProcessSegmentGroupInformation.Usage;
            if (grp == kSegmentGroupLocal) s.dedicatedBytes += usage;
            else                           s.sharedBytes += usage;
        }

        // ---- 5. 与上一轮差分 ----
        Impl::PrevProc& p = impl_->prev[pid];
        const bool havePrev = impl_->hasPrev && p.present;
        if (havePrev) {
            int64_t maxNodeDelta = 0;
            int64_t sumDelta = 0;
            for (uint32_t n = 0; n < nodes; ++n) {
                int64_t d = run[n] - p.run[n];
                if (d < 0) d = 0;
                sumDelta += d;
                if (d > maxNodeDelta) maxNodeDelta = d;
            }
            s.gpuRunningDelta = static_cast<uint64_t>(sumDelta);
            maxNodeDeltas.push_back(maxNodeDelta);
        } else {
            s.gpuRunningDelta = 0;
            maxNodeDeltas.push_back(0);
        }

        // 存本轮值
        p.present = true;
        for (uint32_t n = 0; n < kMaxNodes; ++n) p.run[n] = (n < nodes) ? run[n] : 0;

        out.push_back(s);
    }

    // ---- 6. 统一时间换算 ----
    // 以两次 Collect() 的实际墙钟间隔为分母；RunningTime 单位 100ns。
    const double tNow = QpcNowSeconds();
    double dt = impl_->hasPrev ? (tNow - impl_->prevTimestamp) : 0.0;
    if (dt < 0.0) dt = 0.0;
    const double denom = dt * 1.0e7;
    for (size_t i = 0; i < out.size() && i < maxNodeDeltas.size(); ++i) {
        if (!impl_->hasPrev || denom <= 0.0) { out[i].gpuPercent = 0.0; continue; }
        double pct = static_cast<double>(maxNodeDeltas[i]) / denom * 100.0;
        if (pct < 0.0) pct = 0.0;
        if (pct > 100.0) pct = 100.0;   // 单个引擎不会超过 100%
        out[i].gpuPercent = pct;
    }

    impl_->hasPrev = true;
    impl_->prevTimestamp = tNow;

    // ---- 6b. 关闭已退出进程的缓存句柄 ----
    //
    // 【真实句柄泄漏修复 · 实测每秒 +43 个】
    // handleCache 按 pid 下标缓存进程句柄，但**进程退出后没有任何地方关闭它**：
    //   - 遍历只走本轮的 `pids` 列表，已退出的 pid 不在其中，永远走不到；
    //   - 60 轮一次的 stale 标记也只对"还在列表里的"条目生效；
    //   - 只有 GpuProcCollector 析构时才统一 CloseHandle。
    // 结果：handleCache 里挂着的活句柄持续增长（实测 22 秒涨到 1284 个，
    // 且 20 次采集里 1400 次 OpenProcess、1385 次成功）。用户机器 600+
    // 进程、进程高频启停，这个数字只增不减。
    //
    // 用本轮 pid 集合做过滤：不在集合里的条目直接 CloseHandle + 清零。
    // 下次该 pid 再出现时会重新 OpenProcess（等价于"新进程"，正确行为）。
    //
    // 【为什么不遍历整张 handleCache】它按 pid 下标索引，容量 6.9 万项，
    // 每轮扫一遍是 6.9 万次比较（约 0.1 ms，能接受但不必要）。
    // 更好的是反过来：只有"曾经打开过句柄"的 pid 才需要检查，
    // 而这类 pid 必然在上一轮的 pids 里。所以用上一轮的 pid 列表检查即可。
    if (!impl_->lastPidList.empty()) {
        std::unordered_set<uint32_t> alivePids;
        alivePids.reserve(count * 2);
        for (size_t i = 0; i < count; ++i) {
            if (pids[i] != 0) alivePids.insert(pids[i]);
        }
        for (uint32_t oldPid : impl_->lastPidList) {
            if (oldPid == 0) continue;
            if (alivePids.count(oldPid)) continue;      // 本轮还在，保留句柄
            if (oldPid >= impl_->handleCache.size()) continue;
            Impl::HandleEntry& e = impl_->handleCache[oldPid];
            if (e.handle) {
                CloseHandle(e.handle);
                e.handle = nullptr;
                e.usable = false;
                e.peeked = false;
                e.openFailed = false;
                e.nodeCount = 0;
            }
        }
    }

    // 记下本轮 pid，供下一轮做"已退出进程"的句柄回收。
    impl_->lastPidList.assign(pids, pids + count);

    // ---- 7. 清掉已退出进程的差分基准 ----
    //
    // 【无界增长修复】上面 `impl_->prev[pid]` 是 operator[] 写入语义，每个
    // 出现过 GPU 活动的 pid 都会插一条 PrevProc（264 字节）。这里原来
    // 全项目 0 处 erase —— pid 又会被系统复用给新进程，于是这张表只增不减，
    // 长时间运行会持续吃内存，且 pid 复用时旧基准会污染新进程的差分。
    //
    // 【存活集合必须用 pids（本轮全部进程），不能用 out（本轮有 GPU 数据的）】
    // 这两者语义完全不同：
    //   pids = 系统里当前存在的进程
    //   out  = 其中「adapter 查询成功且有 GPU 记录」的那几个
    // 用 out 的后果：某个进程这一轮 adapter 查询失败（驱动复位、TDR、
    // 进程刚好在切换 GPU 状态），它就不在 out 里 → 基准被当成"进程已退出"删掉
    // → 下一轮它又有数据时，首帧 gpuPercent = 0 → 1 Hz 下**交替闪烁 0%**。
    // 极端情况：任一轮 out 为空（gpuLuid 失效）→ 整张 prev 被清空 → 全表 GPU% 归零。
    //
    // 用 pids 才是正确的"进程是否还活着"判据。
    if (!impl_->prev.empty() && pids && count > 0) {
        std::unordered_set<uint32_t> alive;
        alive.reserve(count * 2);
        for (size_t i = 0; i < count; ++i) {
            if (pids[i] != 0) alive.insert(pids[i]);
        }
        for (auto it = impl_->prev.begin(); it != impl_->prev.end(); ) {
            if (alive.count(it->first)) ++it;
            else it = impl_->prev.erase(it);
        }
    }

    lastCollectMs_ = (QpcNowSeconds() - t0) * 1000.0;
}

} // namespace tmp
