// gpuprobe.cpp - D3DKMT 每进程 GPU 可行性验证
// 目的：实测验证结构体布局、适配器枚举、每进程 GPU 时间/内存，并测耗时。
// 编译：单独 console exe，不进入主项目。
#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>

// SDK 头仅用于交叉校验结构体尺寸（probe 私有，不进交付代码）
#include <d3dkmthk.h>

#pragma comment(lib, "psapi.lib")

// ============================================================================
// 自己定义的结构体（与交付代码完全一致的写法），放在 namespace 里避免和 SDK 冲突
// ============================================================================
namespace mygpu {

typedef uint32_t D3DKMT_HANDLE_T;

struct Counter { uint32_t Count; uint64_t Bytes; };

struct DmaPacketTypeInfo { uint32_t submited, completed, preempted, faulted; };
struct QueuePacketTypeInfo { uint32_t submited, completed; };

struct PacketInformation {
    QueuePacketTypeInfo QueuePacket[8];
    DmaPacketTypeInfo   DmaPacket[4];
};
struct PreemptionInformation { uint32_t PreemptionCounter[16]; };

// 0x110 = 272
struct ProcessNodeInformation {
    int64_t  RunningTime;       // 微秒
    uint32_t ContextSwitch;
    PreemptionInformation PreemptionStatistics;
    PacketInformation     PacketStatistics;
    uint64_t Reserved[8];
};

struct NodeInformation {
    ProcessNodeInformation GlobalInformation;
    ProcessNodeInformation SystemInformation;
    // 后面是 D3DKMT_NODE_PERFDATA (0x38) + Reserved[3] + 尾部对齐，共 0x48
    uint8_t  Tail[0x48];
};

struct SystemMemory {
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

// 0x90 = 144
struct ProcessInformation {
    uint32_t NodeCount;
    uint32_t VidPnSourceCount;
    SystemMemory SystemMemory;
    uint64_t Reserved[7];
};

struct DmaBuffer { Counter Size; uint32_t AllocationListBytes; uint32_t PatchLocationListBytes; };
struct CommitmentData { uint64_t TotalBytesEvictedFromProcess; uint64_t BytesBySegmentPreference[5]; };
struct Policy {
    uint64_t PreferApertureForRead[5];
    uint64_t PreferAperture[5];
    uint64_t MemResetOnPaging;
    uint64_t RemovePagesFromWorkingSetOnPaging;
    uint64_t MigrationEnabled;
};
struct ProcessInterferenceCounters { uint64_t InterferenceCount[9]; };

// 0x110 = 272
struct ProcessAdapterInformation {
    uint32_t NbSegments;
    uint32_t NodeCount;
    uint32_t VidPnSourceCount;
    uint32_t VirtualMemoryUsage;
    DmaBuffer DmaBuffer;
    CommitmentData CommitmentData;
    Policy _Policy;
    ProcessInterferenceCounters ProcessInterferenceCounters;
    uint32_t ClientHint;
};

struct VideoMemory {
    uint32_t AllocsCommitted;
    Counter  AllocsResidentInP[5];
    Counter  AllocsResidentInNonPreferred;
    uint64_t TotalBytesEvictedDueToPreparation;
};
struct ProcessSegmentPolicy { uint64_t UseMRU; };

// 0xD8 = 216
struct ProcessSegmentInformation {
    uint64_t BytesCommitted;
    uint64_t MaximumWorkingSet;
    uint64_t MinimumWorkingSet;
    uint32_t NbReferencedAllocationEvictedInPeriod;
    uint32_t Padding;
    VideoMemory VideoMemory;
    ProcessSegmentPolicy _Policy;
    uint64_t Reserved[8];
};

// 0x40 = 64
struct ProcessSegmentGroupInformation {
    uint64_t Budget;
    uint64_t Requested;
    uint64_t Usage;
    uint64_t Demoted[5];
};

struct MemoryStats {
    uint64_t TotalBytesEvicted;
    uint32_t AllocsCommitted;
    uint32_t AllocsResident;
};

// 0x98 = 152
struct SegmentInformation {
    uint64_t CommitLimit;
    uint64_t BytesCommitted;
    uint64_t BytesResident;
    MemoryStats Memory;
    uint32_t Aperture;
    uint64_t TotalBytesEvictedByPriority[5];
    uint64_t SystemMemoryEndAddress;
    uint64_t PowerFlags;
    uint64_t SegmentProperties;
    uint64_t Reserved[5];
};

struct AdapterInformation {
    uint32_t NbSegments;
    uint32_t NodeCount;
    uint32_t VidPnSourceCount;
    uint32_t VSyncEnabled;
    uint32_t TdrDetectedCount;
    int64_t  ZeroLengthDmaBuffers;
    uint64_t RestartedPeriod;
    uint8_t  Rest[0x308 - 0x28];
};

union QueryResult {
    AdapterInformation              AdapterInformation;
    SegmentInformation              SegmentInformation;
    NodeInformation                 NodeInformation;
    ProcessInformation              ProcessInformation;
    ProcessAdapterInformation       ProcessAdapterInformation;
    ProcessSegmentInformation       ProcessSegmentInformation;
    ProcessNodeInformation          ProcessNodeInformation;
    ProcessSegmentGroupInformation  ProcessSegmentGroupInformation;
};

// 0x328 = 808
struct QueryStatistics {
    uint32_t    Type;
    LUID        AdapterLuid;
    HANDLE      hProcess;
    QueryResult QueryResult;
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

enum Type {
    T_ADAPTER = 0,
    T_PROCESS = 1,
    T_PROCESS_ADAPTER = 2,
    T_SEGMENT = 3,
    T_PROCESS_SEGMENT = 4,
    T_NODE = 5,
    T_PROCESS_NODE = 6,
    T_VIDPNSOURCE = 7,
    T_PROCESS_VIDPNSOURCE = 8,
    T_PROCESS_SEGMENT_GROUP = 9,
};

} // namespace mygpu

// ---------------------------------------------------------------------------
// 静态校验：我的结构与 SDK 定义必须完全一致
// ---------------------------------------------------------------------------
#define CHK_SIZE(mine, sdk) \
    static_assert(sizeof(mygpu::mine) == sizeof(::sdk), #mine " size mismatch vs " #sdk)

CHK_SIZE(QueryStatistics, D3DKMT_QUERYSTATISTICS);
CHK_SIZE(QueryResult, D3DKMT_QUERYSTATISTICS_RESULT);
CHK_SIZE(ProcessInformation, D3DKMT_QUERYSTATISTICS_PROCESS_INFORMATION);
CHK_SIZE(ProcessAdapterInformation, D3DKMT_QUERYSTATISTICS_PROCESS_ADAPTER_INFORMATION);
CHK_SIZE(ProcessSegmentInformation, D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT_INFORMATION);
CHK_SIZE(ProcessNodeInformation, D3DKMT_QUERYSTATISTICS_PROCESS_NODE_INFORMATION);
CHK_SIZE(NodeInformation, D3DKMT_QUERYSTATISTICS_NODE_INFORMATION);
CHK_SIZE(SegmentInformation, D3DKMT_QUERYSTATISTICS_SEGMENT_INFORMATION);
CHK_SIZE(ProcessSegmentGroupInformation, D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT_GROUP_INFORMATION);
CHK_SIZE(SystemMemory, D3DKMT_QUERYSTATISTICS_SYSTEM_MEMORY);
CHK_SIZE(PacketInformation, D3DKMT_QUERYSTATISTICS_PACKET_INFORMATION);
CHK_SIZE(PreemptionInformation, D3DKMT_QUERYSTATISTICS_PREEMPTION_INFORMATION);
CHK_SIZE(CommitmentData, D3DKMT_QUERYSTATISTICS_COMMITMENT_DATA);
CHK_SIZE(DmaBuffer, D3DKMT_QUERYSTATISTICS_DMA_BUFFER);
CHK_SIZE(VideoMemory, D3DKMT_QUERYSTATISTICS_VIDEO_MEMORY);
CHK_SIZE(Counter, D3DKMT_QUERYSTATISTICS_COUNTER);

static_assert(sizeof(mygpu::QueryStatistics) == 0x328, "QueryStatistics must be 0x328");
static_assert(offsetof(mygpu::QueryStatistics, AdapterLuid) == 0x04, "LUID offset");
static_assert(offsetof(mygpu::QueryStatistics, hProcess) == 0x10, "hProcess offset");
static_assert(offsetof(mygpu::QueryStatistics, QueryResult) == 0x18, "QueryResult offset");
static_assert(offsetof(mygpu::QueryStatistics, QueryNodeId) == 0x320, "input union offset");
static_assert(offsetof(mygpu::ProcessNodeInformation, RunningTime) == 0x00, "RunningTime offset");
static_assert(offsetof(mygpu::ProcessSegmentGroupInformation, Usage) == 0x10, "Usage offset");
static_assert(offsetof(mygpu::ProcessSegmentInformation, BytesCommitted) == 0x00, "BytesCommitted");
static_assert(offsetof(mygpu::ProcessSegmentInformation, VideoMemory) == 0x20, "VideoMemory");
static_assert(offsetof(mygpu::AdapterInformation, NbSegments) == 0x00, "NbSegments");

// ============================================================================
// 动态加载
// ============================================================================
typedef NTSTATUS (NTAPI* PFN_OpenAdapterFromDeviceName)(void*);
typedef NTSTATUS (NTAPI* PFN_CloseAdapter)(const void*);
typedef NTSTATUS (NTAPI* PFN_QueryStatistics)(const mygpu::QueryStatistics*);
typedef NTSTATUS (NTAPI* PFN_EnumAdapters2)(void*);
typedef NTSTATUS (NTAPI* PFN_QueryAdapterInfo)(void*);

struct Km {
    HMODULE gdi32 = nullptr;
    PFN_QueryStatistics       QueryStatistics = nullptr;
    PFN_EnumAdapters2         EnumAdapters2 = nullptr;
    PFN_QueryAdapterInfo      QueryAdapterInfo = nullptr;
    PFN_OpenAdapterFromDeviceName OpenAdapterFromDeviceName = nullptr;
    PFN_CloseAdapter          CloseAdapter = nullptr;
};

struct AdapterInfoEx {
    LUID luid{};
    uint32_t hAdapter = 0;
    wchar_t  name[128] = L"";
    wchar_t  chip[128] = L"";
};

static void PrintLastErr(const char* what, NTSTATUS st) {
    printf("  %-28s status=0x%08lX\n", what, (unsigned long)st);
}

int main() {
    printf("=== D3DKMT per-process GPU probe ===\n");
    printf("sizeof(D3DKMT_QUERYSTATISTICS)=0x%zX\n\n", sizeof(mygpu::QueryStatistics));

    Km km;
    km.gdi32 = LoadLibraryW(L"gdi32.dll");
    if (!km.gdi32) { printf("FATAL: LoadLibrary gdi32 failed %lu\n", GetLastError()); return 1; }
    km.QueryStatistics = (PFN_QueryStatistics)GetProcAddress(km.gdi32, "D3DKMTQueryStatistics");
    km.EnumAdapters2   = (PFN_EnumAdapters2)GetProcAddress(km.gdi32, "D3DKMTEnumAdapters2");
    km.QueryAdapterInfo = (PFN_QueryAdapterInfo)GetProcAddress(km.gdi32, "D3DKMTQueryAdapterInfo");
    km.OpenAdapterFromDeviceName = (PFN_OpenAdapterFromDeviceName)GetProcAddress(km.gdi32, "D3DKMTOpenAdapterFromDeviceName");
    km.CloseAdapter    = (PFN_CloseAdapter)GetProcAddress(km.gdi32, "D3DKMTCloseAdapter");

    printf("--- gdi32.dll exports ---\n");
    printf("  D3DKMTQueryStatistics           = %p\n", (void*)km.QueryStatistics);
    printf("  D3DKMTEnumAdapters2             = %p\n", (void*)km.EnumAdapters2);
    printf("  D3DKMTQueryAdapterInfo          = %p\n", (void*)km.QueryAdapterInfo);
    printf("  D3DKMTOpenAdapterFromDeviceName = %p\n", (void*)km.OpenAdapterFromDeviceName);
    printf("  D3DKMTCloseAdapter              = %p\n", (void*)km.CloseAdapter);
    if (!km.QueryStatistics) { printf("FATAL: no D3DKMTQueryStatistics\n"); return 1; }

    // ---- 枚举适配器 ----
    printf("\n--- adapters ---\n");
    std::vector<AdapterInfoEx> adapters;
    {
        D3DKMT_ADAPTERINFO raw[16]{};
        D3DKMT_ENUMADAPTERS2 ea{};
        ea.NumAdapters = 16;
        ea.pAdapters = raw;
        NTSTATUS st = km.EnumAdapters2 ? km.EnumAdapters2(&ea) : -1;
        if (st < 0) { PrintLastErr("EnumAdapters2", st); }
        else {
            printf("  EnumAdapters2 -> %lu adapters\n", (unsigned long)ea.NumAdapters);
            for (ULONG i = 0; i < ea.NumAdapters && i < 16; ++i) {
                AdapterInfoEx a;
                a.luid = raw[i].AdapterLuid;
                a.hAdapter = raw[i].hAdapter;
                if (km.QueryAdapterInfo) {
                    D3DKMT_ADAPTERREGISTRYINFO ri{};
                    D3DKMT_QUERYADAPTERINFO qai{};
                    qai.hAdapter = raw[i].hAdapter;
                    qai.Type = KMTQAITYPE_ADAPTERREGISTRYINFO;
                    qai.pPrivateDriverData = &ri;
                    qai.PrivateDriverDataSize = sizeof(ri);
                    NTSTATUS s2 = km.QueryAdapterInfo(&qai);
                    if (s2 >= 0) {
                        wcsncpy_s(a.name, ri.AdapterString, _TRUNCATE);
                        wcsncpy_s(a.chip, ri.ChipType, _TRUNCATE);
                    }
                }
                adapters.push_back(a);
                printf("  [%lu] hAdapter=%u LUID=%08lX:%08lX sources=%lu name=\"%ls\" chip=\"%ls\"\n",
                       (unsigned long)i, raw[i].hAdapter,
                       (unsigned long)a.luid.HighPart, (unsigned long)a.luid.LowPart,
                       (unsigned long)raw[i].NumOfSources, a.name, a.chip);
            }
        }
    }
    if (adapters.empty()) { printf("FATAL: no adapters\n"); return 1; }

    // ---- 每适配器 ADAPTER 统计 ----
    printf("\n--- D3DKMT_QUERYSTATISTICS_ADAPTER per adapter ---\n");
    for (size_t i = 0; i < adapters.size(); ++i) {
        mygpu::QueryStatistics q{};
        q.Type = mygpu::T_ADAPTER;
        q.AdapterLuid = adapters[i].luid;
        NTSTATUS st = km.QueryStatistics(&q);
        if (st < 0) { printf("  adapter %zu: FAILED status=0x%08lX\n", i, (unsigned long)st); continue; }
        printf("  adapter %zu \"%ls\": NbSegments=%u NodeCount=%u VidPnSourceCount=%u\n",
               i, adapters[i].name, q.QueryResult.AdapterInformation.NbSegments,
               q.QueryResult.AdapterInformation.NodeCount,
               q.QueryResult.AdapterInformation.VidPnSourceCount);
        // 段信息
        for (uint32_t s = 0; s < q.QueryResult.AdapterInformation.NbSegments && s < 8; ++s) {
            mygpu::QueryStatistics qs{};
            qs.Type = mygpu::T_SEGMENT;
            qs.AdapterLuid = adapters[i].luid;
            qs.QuerySegmentId = s;
            NTSTATUS sst = km.QueryStatistics(&qs);
            if (sst < 0) { printf("    segment %u: FAILED 0x%08lX\n", s, (unsigned long)sst); continue; }
            const auto& si = qs.QueryResult.SegmentInformation;
            printf("    segment %u: limit=%.2f MB committed=%.2f MB resident=%.2f MB aperture=%u\n",
                   s, si.CommitLimit / 1048576.0, si.BytesCommitted / 1048576.0,
                   si.BytesResident / 1048576.0, si.Aperture);
        }
        // 节点全局 RunningTime（用于交叉校验）
        for (uint32_t n = 0; n < q.QueryResult.AdapterInformation.NodeCount && n < 8; ++n) {
            mygpu::QueryStatistics qn{};
            qn.Type = mygpu::T_NODE;
            qn.AdapterLuid = adapters[i].luid;
            qn.QueryNodeId = n;
            NTSTATUS nst = km.QueryStatistics(&qn);
            if (nst < 0) { printf("    node %u: FAILED 0x%08lX\n", n, (unsigned long)nst); continue; }
            printf("    node %u: global RunningTime=%lld us  system=%lld us  ctxsw=%u\n", n,
                   (long long)qn.QueryResult.NodeInformation.GlobalInformation.RunningTime,
                   (long long)qn.QueryResult.NodeInformation.SystemInformation.RunningTime,
                   qn.QueryResult.NodeInformation.GlobalInformation.ContextSwitch);
        }
    }

    // ---- 进程列表 ----
    std::vector<uint32_t> pids;
    {
        std::vector<DWORD> buf(8192);
        DWORD got = 0;
        if (EnumProcesses(buf.data(), (DWORD)(buf.size() * sizeof(DWORD)), &got)) {
            pids.assign(buf.begin(), buf.begin() + got / sizeof(DWORD));
        }
    }
    printf("\n--- %zu processes ---\n", pids.size());

    // 采样函数
    struct Sample {
        uint32_t pid;
        int64_t  runningUs = 0;     // 所有节点求和
        uint32_t maxNode = 0;
        int64_t  perNode[8]{};
        uint64_t commitLocal = 0;
        uint64_t commitNonLocal = 0;
        uint64_t systemMemBytes = 0;
        uint32_t nodeCount = 0;
        bool     hasGpu = false;
    };

    auto Snapshot = [&](std::vector<Sample>& out, double* ms, uint32_t* okCount,
                        uint32_t* failCount, uint32_t* openFail) {
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        out.clear();
        out.reserve(pids.size());
        *okCount = 0; *failCount = 0; *openFail = 0;

        for (uint32_t pid : pids) {
            if (pid == 0) continue;
            HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!hp) { ++(*openFail); continue; }
            Sample s;
            s.pid = pid;
            bool any = false;
            for (size_t ai = 0; ai < adapters.size(); ++ai) {
                // 1) PROCESS 级：拿 NodeCount / SystemMemory
                mygpu::QueryStatistics qp{};
                qp.Type = mygpu::T_PROCESS;
                qp.AdapterLuid = adapters[ai].luid;
                qp.hProcess = hp;
                NTSTATUS st = km.QueryStatistics(&qp);
                if (st < 0) { ++(*failCount); continue; }
                uint32_t nodes = qp.QueryResult.ProcessInformation.NodeCount;
                if (nodes > 8) nodes = 8;
                s.systemMemBytes += qp.QueryResult.ProcessInformation.SystemMemory.BytesAllocated;
                if (nodes == 0) continue;

                // 2) 每节点 RunningTime
                for (uint32_t n = 0; n < nodes; ++n) {
                    mygpu::QueryStatistics qn{};
                    qn.Type = mygpu::T_PROCESS_NODE;
                    qn.AdapterLuid = adapters[ai].luid;
                    qn.hProcess = hp;
                    qn.QueryProcessNodeId = n;
                    NTSTATUS nst = km.QueryStatistics(&qn);
                    if (nst < 0) continue;
                    int64_t rt = qn.QueryResult.ProcessNodeInformation.RunningTime;
                    s.perNode[n] += rt;
                    s.runningUs += rt;
                    if (n == 0 || rt > s.perNode[s.maxNode]) s.maxNode = n;
                    any = true;
                }
                if (nodes > s.nodeCount) s.nodeCount = nodes;

                // 3) 段组（LOCAL / NON_LOCAL）内存
                for (uint32_t g = 0; g < 2; ++g) {
                    mygpu::QueryStatistics qg{};
                    qg.Type = mygpu::T_PROCESS_SEGMENT_GROUP;
                    qg.AdapterLuid = adapters[ai].luid;
                    qg.hProcess = hp;
                    qg.QueryProcessSegmentGroup = g;
                    NTSTATUS gst = km.QueryStatistics(&qg);
                    if (gst < 0) continue;
                    if (g == 0) s.commitLocal += qg.QueryResult.ProcessSegmentGroupInformation.Usage;
                    else        s.commitNonLocal += qg.QueryResult.ProcessSegmentGroupInformation.Usage;
                    any = true;
                }
            }
            s.hasGpu = any;
            if (any) ++(*okCount);
            out.push_back(s);
            CloseHandle(hp);
        }
        QueryPerformanceCounter(&t1);
        *ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    };

    // ---- 第 1 轮 ----
    std::vector<Sample> s1, s2;
    double ms1 = 0, ms2 = 0;
    uint32_t ok = 0, bad = 0, ofail = 0;

    LARGE_INTEGER qf, w1, w2;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&w1);
    Snapshot(s1, &ms1, &ok, &bad, &ofail);
    printf("  round1: %.3f ms  gpu-procs=%u  queryFails=%u  openFails=%u\n", ms1, ok, bad, ofail);

    Sleep(1000);
    QueryPerformanceCounter(&w2);
    Snapshot(s2, &ms2, &ok, &bad, &ofail);
    printf("  round2: %.3f ms  gpu-procs=%u  queryFails=%u  openFails=%u\n", ms2, ok, bad, ofail);

    double deltaSec = (double)(w2.QuadPart - w1.QuadPart) / (double)qf.QuadPart;
    printf("  wall delta = %.4f s\n", deltaSec);

    // ---- 差分 ----
    printf("\n--- per-process GPU (delta over %.3fs) ---\n", deltaSec);
    printf("%-8s %-26s %12s %9s %9s %12s %12s\n",
           "PID", "name", "runDelta_us", "GPU%", "nodeC", "dedMB", "sharedMB");

    struct Row { uint32_t pid; std::wstring name; int64_t d; double pct; uint32_t nodes; uint64_t ded, sh; };
    std::vector<Row> rows;
    for (const auto& a : s1) {
        const Sample* b = nullptr;
        for (const auto& c : s2) if (c.pid == a.pid) { b = &c; break; }
        if (!b || !b->hasGpu) continue;
        int64_t d = b->runningUs - a.runningUs;
        if (d < 0) d = 0;
        double pct = deltaSec > 0 ? (double)d / (deltaSec * 1e6) * 100.0 : 0.0;
        Row r;
        r.pid = a.pid; r.d = d; r.pct = pct; r.nodes = b->nodeCount;
        r.ded = b->commitLocal; r.sh = b->commitNonLocal;
        wchar_t nm[MAX_PATH] = L"";
        HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, a.pid);
        if (hp) {
            DWORD n = MAX_PATH;
            QueryFullProcessImageNameW(hp, 0, nm, &n);
            CloseHandle(hp);
        }
        const wchar_t* base = wcsrchr(nm, L'\\');
        r.name = base ? base + 1 : nm;
        rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) {
        if (x.d != y.d) return x.d > y.d;
        return x.ded > y.ded;
    });
    for (size_t i = 0; i < rows.size() && i < 40; ++i) {
        printf("%-8u %-26ls %12lld %8.2f%% %9u %12.1f %12.1f\n",
               rows[i].pid, rows[i].name.c_str(), (long long)rows[i].d, rows[i].pct,
               rows[i].nodes, rows[i].ded / 1048576.0, rows[i].sh / 1048576.0);
    }

    // ---- 交叉校验：所有进程 RunningTime 增量之和 vs 适配器全局节点增量 ----
    printf("\n--- cross-check: sum(process) vs adapter-global node RunningTime ---\n");
    for (size_t ai = 0; ai < adapters.size(); ++ai) {
        mygpu::QueryStatistics qA{};
        qA.Type = mygpu::T_ADAPTER;
        qA.AdapterLuid = adapters[ai].luid;
        if (km.QueryStatistics(&qA) < 0) continue;
        uint32_t nc = qA.QueryResult.AdapterInformation.NodeCount;
        for (uint32_t n = 0; n < nc && n < 8; ++n) {
            mygpu::QueryStatistics q{};
            q.Type = mygpu::T_NODE;
            q.AdapterLuid = adapters[ai].luid;
            q.QueryNodeId = n;
            if (km.QueryStatistics(&q) < 0) continue;
            int64_t g = q.QueryResult.NodeInformation.GlobalInformation.RunningTime;
            printf("  adapter %zu \"%ls\" node %u: global=%lld us (=%.2f%% of %.2fs)\n",
                   ai, adapters[ai].name, n, (long long)g, (double)g / (deltaSec * 1e6) * 100.0, deltaSec);
        }
    }

    // ---- 总提交内存对比 ----
    printf("\n--- GPU memory totals ---\n");
    uint64_t sumDed = 0, sumSh = 0, sumSys = 0;
    for (const auto& r : rows) { sumDed += r.ded; sumSh += r.sh; }
    for (const auto& s : s2) sumSys += s.systemMemBytes;
    printf("  sum(process commitLocal)    = %.2f MB\n", sumDed / 1048576.0);
    printf("  sum(process commitNonLocal) = %.2f MB\n", sumSh / 1048576.0);
    printf("  sum(process SystemMemory)   = %.2f MB\n", sumSys / 1048576.0);
    for (size_t ai = 0; ai < adapters.size(); ++ai) {
        mygpu::QueryStatistics qA{};
        qA.Type = mygpu::T_ADAPTER;
        qA.AdapterLuid = adapters[ai].luid;
        if (km.QueryStatistics(&qA) < 0) continue;
        uint64_t totCommit = 0, totResident = 0;
        for (uint32_t s = 0; s < qA.QueryResult.AdapterInformation.NbSegments && s < 8; ++s) {
            mygpu::QueryStatistics qs{};
            qs.Type = mygpu::T_SEGMENT;
            qs.AdapterLuid = adapters[ai].luid;
            qs.QuerySegmentId = s;
            if (km.QueryStatistics(&qs) < 0) continue;
            totCommit += qs.QueryResult.SegmentInformation.BytesCommitted;
            totResident += qs.QueryResult.SegmentInformation.BytesResident;
        }
        printf("  adapter %zu \"%ls\" segment sum: committed=%.2f MB resident=%.2f MB\n",
               ai, adapters[ai].name, totCommit / 1048576.0, totResident / 1048576.0);
    }

    // ---- 稳定性：多轮耗时 ----
    printf("\n--- timing: 10 rounds ---\n");
    double total = 0, mn = 1e9, mx = 0;
    for (int i = 0; i < 10; ++i) {
        double m = 0; uint32_t o2 = 0, b2 = 0, f2 = 0;
        Snapshot(s2, &m, &o2, &b2, &f2);
        total += m; if (m < mn) mn = m; if (m > mx) mx = m;
        printf("  round %2d: %.3f ms\n", i + 1, m);
    }
    printf("  total=%.3f ms  avg=%.3f ms  min=%.3f ms  max=%.3f ms\n",
           total, total / 10.0, mn, mx);

    // ---- 关闭句柄 ----
    for (auto& a : adapters) {
        if (km.CloseAdapter && a.hAdapter) {
            D3DKMT_CLOSEADAPTER ca{};
            ca.hAdapter = a.hAdapter;
            km.CloseAdapter(&ca);
        }
    }
    return 0;
}
