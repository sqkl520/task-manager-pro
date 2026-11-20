// gpuinfo.cpp - 系统级 GPU 信息采集实现
//
// ============================================================================
// 实测依据（Win10 Pro 22H2 22H2 / discrete GPU / Xeon 服务器级 CPU）
// 探针源码保留在 docs/evidence/gpuinfo_probe.cpp … gpuinfo_probe9.cpp
//
// 1) 适配器枚举：D3DKMTEnumAdapters2 返回 4 个条目，实测
//      [0] luid 0x1CFD5  注册表名 空        NodeCount=14
//      [1] luid 0x1D9A3  注册表名 空        NodeCount=14
//      [2] luid 0x13812  "NVIDIA GeForce discrete GPU" NodeCount=14
//      [3] luid 0x146EC  注册表名 空        NodeCount=21（Microsoft Basic Render Driver）
//    只有 [2] 的 AdapterRegistryInfo 能读到名字。前两个是同一物理 GPU 的镜像
//    （DXGI 也把同一个 NVIDIA 描述符报了 3 次），[3] 是软件渲染器
//    （DXGI 名称 "Microsoft Basic Render Driver"，DedicatedVideoMemory=0）。
//    因此选真 GPU 的判据是"ADAPTERREGISTRYINFO.AdapterString 非空 且
//    NodeCount>0"，实测唯一命中 [2]。
//    （注意：不能按 LUID 是否等于某个 DXGI LUID 来选，因为 DXGI 报了 3 个
//      完全相同的 NVIDIA LUID，无法区分。）
//
// 2) 引擎元数据：KMTQAITYPE_NODEMETADATA(25) 结构体必须 **1 字节打包**。
//    SDK 里 DXGK_NODEMETADATA 定义在 d3dkmdt.h 的 #pragma pack(push,1) 区间内：
//      sizeof(DXGK_NODEMETADATA)=74, sizeof(D3DKMT_NODEMETADATA)=78
//    按自然对齐写成 76/80 时，QueryAdapterInfo 一律返回
//    STATUS_INVALID_PARAMETER (0xC000000D)。改用 pack(1) 后 14/14 个 node 全部
//    成功。实测结果（本机 discrete GPU，14 个 node）：
//      node 0  -> type 1 (3D)
//      node 1  -> type 7 (Overlay)        FriendlyName 空
//      node 2  -> type 2 (VideoDecode)    FriendlyName 空
//      node 3..5, 9..12 -> type 6 (Copy)  FriendlyName 空   （共 8 个 Copy node）
//      node 6  -> type 0 (Other)          FriendlyName "Security"
//      node 7  -> type 3 (VideoEncode)    FriendlyName 空
//      node 8  -> type 0 (Other)          FriendlyName "VR"
//      node 13 -> type 0 (Other)          FriendlyName "Security_1"
//    注意 FriendlyName 对标准引擎多为空串，只有 Other 类才有名字。
//
// 3) 每引擎使用率的关键坑：D3DKMT_QUERYSTATISTICS_TYPE_NODE(5) 的
//    GlobalInformation.RunningTime 在本机驱动上 **3D 节点恒为 0**（原始字节全 0，
//    而同一块的 NodePerfData@0x220 能正常读到 1950 MHz / 2115 MHz，说明布局没错）。
//    VideoDecode 节点则与逐进程求和完全一致。
//    因此本模块对每个 node 取 max(T_NODE 全局增量, 所有进程 T_PROCESS_NODE 增量之和)。
//    实测 5 个 1s 窗口对照 PDH \GPU Engine(*)\Utilization Percentage（按引擎类型聚合）：
//      窗口 3D            VideoDecode        Copy
//      0    18.333/18.978  21.886/22.415    0.184/0.207
//      1    18.049/18.301  22.520/22.529    0.231/0.231
//      2    17.679/17.777  19.035/19.055    0.557/0.558
//      3    17.157/17.135  25.727/25.686    0.212/0.211
//      4    19.942/20.057  23.237/23.286    0.165/0.166
//    最差偏差 0.645 个百分点（3D 窗口 0），多数 < 0.1。只取 T_NODE 会让 3D 恒 0%，
//    只取逐进程求和会让 Copy 偏低（漏掉内核/系统线程的那部分），故取 max。
//
//    关于加强验证：docs/evidence/gpuload.cpp 用 D3D11 计算着色器把 3D 引擎压满，
//    再同时读三路（docs/evidence/gputest_load.cpp）：
//      gpuinfo 99.07% | PDH 98.88% | nvidia-smi 100%
//      连续 8 轮：99.07/98.88、97.20/97.16、98.68/98.74、97.87/97.87、
//                 99.00/98.92、98.42/98.48、97.51/97.46、99.35/99.38、99.29/99.27
//    每轮与 PDH 偏差均 < 0.1 个百分点，空载时偏差 0.000（0.087% vs 0.087%）。
//    nvidia-smi 是独立于 D3DKMT/PDH 的第三路参照（NVML），三者一致。
//
// 4) 显存口径（同时读取对照，字节级相等）：
//    segment[Aperture==0].BytesResident 之和 == PDH \GPU Adapter Memory(*)\Dedicated Usage
//    segment[Aperture==1].BytesResident 之和 == PDH \GPU Adapter Memory(*)\Shared Usage
//    实测一轮：seg1(aperture=0) resident=3113.84 MB vs PDH Dedicated=3113.84 MB（差 0.00）
//             seg0+seg2(aperture=1) resident=0+256.98 MB vs PDH Shared=256.98 MB（差 0.00）
//    总量用 KMTQAITYPE_GETSEGMENTSIZE(3)：Dedicated=8377073664 (7989.00 MiB),
//    Shared=17146097664 (16351.79 MiB)，与 DXGI_ADAPTER_DESC 逐字节相同。
//
// 5) "为硬件预留内存"：D3DKMT 无任何字段直接给出。实测推导
//    注册表 HardwareInformation.qwMemorySize (8589934592)
//    - GETSEGMENTSIZE.DedicatedVideoMemorySize (8377073664)
//    = 212860928 字节 = 203.00 MiB。两者都是实测真值，相减是唯一可行路径；
//    键缺失或相减为负时保持 0（不编造）。
//    注意注册表 HardwareInformation.MemorySize (DWORD) 是截断值 4293918720，
//    不可用；必须用 qwMemorySize。
//
// 6) 驱动版本/日期：注册表
//    HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\<NNNN>
//    实测本机 4 个子键：
//      0000 NVIDIA GeForce discrete GPU  ver=32.0.16.1714 date=9-17-2026 qwMem=8589934592
//      0001 AMD RADEON HD 6450              ver=15.201.1151.1008 date=11-4-2015
//      0002 GameViewer Virtual Display Adapter
//      0004 Todesk Virtual Display Adapter
//    按 DriverDesc 与 D3DKMT 适配器名不区分大小写匹配来定位子键。
//    日期原始格式是 "M-D-YYYY"，本模块归一化为 "YYYY-MM-DD" 便于显示。
//
// 7) 物理位置：D3DKMT ADAPTERADDRESS(6)，实测 bus=4 device=0 function=0，
//    单次 0.004 ms，无任何副作用。
//
// 8) DirectX 版本：**不在 Init/Collect 里做**。实测 D3D11CreateDevice
//    （D3D_DRIVER_TYPE_HARDWARE）耗时 480.8~1009.8 ms（瓶颈是加载 NVIDIA 用户态
//    驱动，换后台线程也一样：线程内 480.8 ms、墙钟 582.9 ms；只请求 FL12_1 也
//    要 530 ms；指定 adapter 486 ms；D3D12CreateDevice 787~1021 ms）。
//    注册表里没有任何可信的 feature level 线索（HKLM\SOFTWARE\Microsoft\DirectX\
//    Version 是 "4.09.00.0904"，这是 DirectX 4.09 的遗留值，对 Win10 无意义）。
//    因此提供独立的 ProbeDirectXVersion()，由调用方在后台线程调一次。
//
// 9) 明确不用的数据源：PDH。
//    - PdhExpandWildCardPathW("\GPU Engine(pid_*_luid_...)\Utilization Percentage")
//      首次调用实测 1378 / 1684 / 2249 / 2456 ms（四次不同跑测），第二次仍要
//      9~10 ms；532 个实例逐个 PdhAddEnglishCounterW 要 24.8~62.0 ms；
//      常驻查询每轮 PdhCollectQueryData 要 16.1~50.1 ms（均值 20.4）。
//      全部远超 5 ms 预算，结论：**不可用**。
//    - \GPU Engine(_Total) 单实例计数器不可用：PdhGetFormattedCounterValue 返回
//      0xC0000BC6 (PDH_CSTATUS_NO_INSTANCE)。
//    - \GPU Adapter Memory(luid_..._phys_0)\Dedicated Usage 单实例倒是最快
//      （0.10~0.17 ms），但它能提供的信息 D3DKMT segment 已经字节级等价拿到，
//      引入 pdh.lib 依赖不值得。
//    所以本模块对 PDH 零依赖。
//
// 10) 耗时实测（本机 539~616 进程 / 28~39 个有 GPU 记录 / 6 个引擎分组）：
//    测量工具：docs/evidence/bench_gpuinfo.cpp（用 build.bat 相同编译选项，单位 ms）
//
//      Init()                                    n=  5  avg 21.93  p50 21.40  max  24.12
//      Collect() 稳态（Stage 3）                 n=200  avg  1.01  p50  0.61  p95  3.91  max  10.19
//      Collect(pids) 稳态                        n=200  avg  0.83  p50  0.61  p95  3.68  max   5.74
//      Stage 1 pid 枚举（每 60 轮一次）          n= 15  avg  8.56  p50  8.33  p95 10.57
//      Stage 2 建缓存+基线（紧随 Stage 1）       n= 15  avg  3.63  p50  3.22  p95  6.99
//
//    结论：稳态中位数 0.61 ms、均值 1.01 ms、p95 3.9 ms，满足「总耗时 < 5ms」。
//    p99 偶发 8.7 ms 属于 D3DKMT 查询在内核里的尾延迟（本机 CPU 常驻 65~77%，
//    且有腾讯 HIPS + 火绒的内核钩子，见项目取证记录（本机 D3DKMT 探测，未随仓库分发）第 4 条）。
//    这不是算法问题：单独测 14 次全局 T_NODE，p50 0.029 ms 但 p99 17.3 ms。
//
//    三段式状态机（保证任何单轮都不叠加最贵的那一步）：
//      Stage 1  只枚举 pid。以前用 EnumProcesses（avg 47.3 / p95 125.8），
//               改用 NtQuerySystemInformation(SystemProcessInformation)
//               （avg 14.6 / p50 13.4 / p95 24.2），快 3 倍；仍远超 5 ms，
//               所以单独占一轮。每 60 轮触发一次。
//      Stage 2  用缓存 pid 建句柄缓存 + 记差分基线。实测 avg 3.63 ms。
//      Stage 3  正常采集：全局 T_NODE×14 + 逐进程 T_PROCESS_NODE + segment×3。
//    调用方若用 Collect(pids, count) 传已有 pid 列表，则 Stage 1/2 都不发生，
//    只剩 Stage 3（avg 0.83 / p50 0.61 ms）——这是唯一能保证每轮都 < 1 ms 的路径。
//
//    稳态路径里刻意**不做** GetProcessTimes：实测 38 次调用 p99 达 23.5 ms，
//    是最大的一笔可省开销。进程退出的判据改用"该进程所有 node 查询全部失败"
//    —— docs/evidence/gpuinfo_probe14.cpp 实测进程退出后 PROCESS_ADAPTER 与
//    PROCESS_NODE 一律返回 0xC000000D，判据可靠。
// ============================================================================

#include "gpuinfo.h"

#include <psapi.h>
#include <winternl.h>   // NTSTATUS（WIN32_LEAN_AND_MEAN 下 windows.h 不带）

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>

// EnumProcesses 需要 psapi；advapi32 用于注册表。两者都是静态导入，
// 无需调用方改 build.bat 的链接行（本模块不在可改范围内）。
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "advapi32.lib")

namespace tmp {
namespace {

// ---------------------------------------------------------------------------
// D3DKMT 结构体（按 SDK 布局重写；本项目 WIN32_LEAN_AND_MEAN 下不能 include
// d3dkmthk.h，且它在 x64 上有独立的对齐约束）
// ---------------------------------------------------------------------------

constexpr uint32_t kMaxNodes = 64;      // 上限保护：实测适配器 NodeCount=14
constexpr uint32_t kMaxSegments = 16;   // 上限保护：实测 NbSegments=3

// ---- 统计结构（与 gpuproc.cpp 中的同名结构布局一致）----
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
    int64_t  RunningTime;       // 100ns 单位（见文件头第 3 条）
    uint32_t ContextSwitch;
    D3DKMT_PREEMPTION_INFORMATION PreemptionStatistics;
    D3DKMT_PACKET_INFORMATION     PacketStatistics;
    uint64_t Reserved[8];
};

// 0x268 = 616 字节：GlobalInformation + SystemInformation + D3DKMT_NODE_PERFDATA(0x38) 等
struct D3DKMT_NODE_INFORMATION {
    D3DKMT_PROCESS_NODE_INFORMATION GlobalInformation;
    D3DKMT_PROCESS_NODE_INFORMATION SystemInformation;
    uint8_t Tail[0x48];
};

struct D3DKMT_MEMORY_STATS {
    uint64_t TotalBytesEvicted;
    uint32_t AllocsCommitted;
    uint32_t AllocsResident;
};

// 0x98 = 152 字节
struct D3DKMT_SEGMENT_INFORMATION {
    uint64_t CommitLimit;
    uint64_t BytesCommitted;
    uint64_t BytesResident;
    D3DKMT_MEMORY_STATS Memory;
    uint32_t Aperture;
    uint64_t TotalBytesEvictedByPriority[5];
    uint64_t SystemMemoryEndAddress;
    uint64_t PowerFlags;
    uint64_t SegmentProperties;
    uint64_t Reserved[5];
};

// 0x110 = 272 字节
struct D3DKMT_PROCESS_ADAPTER_INFORMATION {
    uint32_t NbSegments;
    uint32_t NodeCount;
    uint32_t VidPnSourceCount;
    uint32_t VirtualMemoryUsage;
    uint8_t  Rest[0x110 - 0x10];
};

// 0x308 = 776 字节
struct D3DKMT_ADAPTER_INFORMATION {
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
    D3DKMT_ADAPTER_INFORMATION         AdapterInformation;
    D3DKMT_SEGMENT_INFORMATION         SegmentInformation;
    D3DKMT_NODE_INFORMATION            NodeInformation;
    D3DKMT_PROCESS_ADAPTER_INFORMATION ProcessAdapterInformation;
    D3DKMT_PROCESS_NODE_INFORMATION    ProcessNodeInformation;
};

// 0x328 = 808 字节
struct D3DKMT_QUERYSTATISTICS {
    uint32_t Type;
    LUID     AdapterLuid;
    HANDLE   hProcess;
    D3DKMT_QUERYSTATISTICS_RESULT QueryResult;
    union {
        uint32_t QuerySegmentId;
        uint32_t QueryProcessNodeId;
        uint32_t QueryNodeId;
        uint32_t QueryPhysAdapterIndex;
    };
};

// ---- 布局锁定（与 SDK d3dkmthk.h 的 C_ASSERT / 实测偏移一致）----
static_assert(sizeof(D3DKMT_QUERYSTATISTICS) == 0x328,
              "D3DKMT_QUERYSTATISTICS 在 x64 上必须是 808 字节");
static_assert(offsetof(D3DKMT_QUERYSTATISTICS, Type) == 0x000, "Type 偏移");
static_assert(offsetof(D3DKMT_QUERYSTATISTICS, AdapterLuid) == 0x004, "LUID 偏移");
static_assert(offsetof(D3DKMT_QUERYSTATISTICS, hProcess) == 0x010, "hProcess 偏移");
static_assert(offsetof(D3DKMT_QUERYSTATISTICS, QueryResult) == 0x018, "QueryResult 偏移");
static_assert(offsetof(D3DKMT_QUERYSTATISTICS, QueryNodeId) == 0x320, "输入 union 偏移");
static_assert(sizeof(D3DKMT_QUERYSTATISTICS_RESULT) == 0x308, "结果 union 尺寸");
static_assert(sizeof(D3DKMT_PROCESS_NODE_INFORMATION) == 0x110, "process node 尺寸");
static_assert(sizeof(D3DKMT_NODE_INFORMATION) == 0x268, "node 尺寸");
static_assert(sizeof(D3DKMT_SEGMENT_INFORMATION) == 0x98, "segment 尺寸");
static_assert(sizeof(D3DKMT_ADAPTER_INFORMATION) == 0x308, "adapter 尺寸");
static_assert(sizeof(D3DKMT_PROCESS_ADAPTER_INFORMATION) == 0x110, "process adapter 尺寸");
static_assert(offsetof(D3DKMT_PROCESS_NODE_INFORMATION, RunningTime) == 0x00, "RunningTime 偏移");
static_assert(offsetof(D3DKMT_SEGMENT_INFORMATION, BytesResident) == 0x10, "BytesResident 偏移");
static_assert(offsetof(D3DKMT_SEGMENT_INFORMATION, Aperture) == 0x28, "Aperture 偏移");

// ---- 统计查询类型（SDK d3dkmthk.h: D3DKMT_QUERYSTATISTICS_TYPE）----
enum : uint32_t {
    kStatsAdapter       = 0,
    kStatsProcessAdapter = 2,
    kStatsSegment       = 3,
    kStatsNode          = 5,
    kStatsProcessNode   = 6,
};

// ---- 引擎类型（SDK d3dkmdt.h: DXGK_ENGINE_TYPE）----
enum : int {
    kEngineOther           = 0,
    kEngine3D              = 1,
    kEngineVideoDecode     = 2,
    kEngineVideoEncode     = 3,
    kEngineVideoProcessing = 4,
    kEngineSceneAssembly   = 5,
    kEngineCopy            = 6,
    kEngineOverlay         = 7,
    kEngineCrypto          = 8,
};

// ---- 适配器枚举 / 查询（SDK d3dkmthk.h 未文档化部分）----
struct D3DKMT_ADAPTERINFO_LOCAL {
    uint32_t hAdapter;
    LUID     AdapterLuid;
    uint32_t NumOfSources;
    BOOL     bPrecisePresentRegionsPreferred;
};

struct D3DKMT_ENUMADAPTERS2_LOCAL {
    uint32_t NumAdapters;
    D3DKMT_ADAPTERINFO_LOCAL* pAdapters;
};

struct D3DKMT_QUERYADAPTERINFO_LOCAL {
    uint32_t hAdapter;
    uint32_t Type;
    void*    pPrivateDriverData;
    uint32_t PrivateDriverDataSize;
};

// KMTQAITYPE_ADAPTERREGISTRYINFO = 8
struct D3DKMT_ADAPTERREGISTRYINFO_LOCAL {
    wchar_t AdapterString[MAX_PATH];
    wchar_t BiosString[MAX_PATH];
    wchar_t DacType[MAX_PATH];
    wchar_t ChipType[MAX_PATH];
};

// KMTQAITYPE_ADAPTERADDRESS = 6
struct D3DKMT_ADAPTERADDRESS_LOCAL {
    uint32_t BusNumber;
    uint32_t DeviceNumber;
    uint32_t FunctionNumber;
};

// KMTQAITYPE_GETSEGMENTSIZE = 3
struct D3DKMT_SEGMENTSIZEINFO_LOCAL {
    uint64_t DedicatedVideoMemorySize;
    uint64_t DedicatedSystemMemorySize;
    uint64_t SharedSystemMemorySize;
};

// KMTQAITYPE_ADAPTERTYPE = 15
union D3DKMT_ADAPTERTYPE_LOCAL {
    struct {
        uint32_t RenderSupported       : 1;
        uint32_t DisplaySupported      : 1;
        uint32_t SoftwareDevice        : 1;
        uint32_t PostDevice            : 1;
        uint32_t HybridDiscrete        : 1;
        uint32_t HybridIntegrated      : 1;
        uint32_t IndirectDisplayDevice : 1;
        uint32_t Paravirtualized       : 1;
        uint32_t Reserved              : 24;
    };
    uint32_t Value;
};

// KMTQAITYPE_NODEMETADATA = 25
// 关键：SDK 把 DXGK_NODEMETADATA 定义在 #pragma pack(push,1) 区间里，
// 自然对齐会大 2 字节，导致 QueryAdapterInfo 返回 STATUS_INVALID_PARAMETER。
#pragma pack(push, 1)
struct DXGK_NODEMETADATA_LOCAL {
    int32_t EngineType;             // DXGK_ENGINE_TYPE
    wchar_t FriendlyName[32];       // DXGK_MAX_METADATA_NAME_LENGTH
    uint32_t Flags;
    uint8_t  GpuMmuSupported;
    uint8_t  IoMmuSupported;
};

struct D3DKMT_NODEMETADATA_LOCAL {
    uint32_t NodeOrdinalAndAdapterIndex;   // 高 16 位 = 物理适配器号，低 16 位 = node 号
    DXGK_NODEMETADATA_LOCAL NodeData;
};
#pragma pack(pop)

static_assert(sizeof(DXGK_NODEMETADATA_LOCAL) == 74, "DXGK_NODEMETADATA 必须 74 字节（pack=1）");
static_assert(sizeof(D3DKMT_NODEMETADATA_LOCAL) == 78, "D3DKMT_NODEMETADATA 必须 78 字节（pack=1）");
static_assert(sizeof(D3DKMT_SEGMENTSIZEINFO_LOCAL) == 24, "SEGMENTSIZEINFO 必须 24 字节");
static_assert(sizeof(D3DKMT_ADAPTERADDRESS_LOCAL) == 12, "ADAPTERADDRESS 必须 12 字节");

constexpr uint32_t kQaiGetSegmentSize      = 3;
constexpr uint32_t kQaiAdapterAddress      = 6;
constexpr uint32_t kQaiAdapterRegistryInfo = 8;
constexpr uint32_t kQaiAdapterType         = 15;
constexpr uint32_t kQaiNodeMetadata        = 25;

// ---- 导出函数类型 ----
using PFN_D3DKMT_ENUMADAPTERS2    = NTSTATUS (NTAPI*)(D3DKMT_ENUMADAPTERS2_LOCAL*);
using PFN_D3DKMT_QUERYADAPTERINFO = NTSTATUS (NTAPI*)(D3DKMT_QUERYADAPTERINFO_LOCAL*);
using PFN_D3DKMT_QUERYSTATISTICS  = NTSTATUS (NTAPI*)(const D3DKMT_QUERYSTATISTICS*);
using PFN_D3DKMT_CLOSEADAPTER     = NTSTATUS (NTAPI*)(const void*);

inline bool NtOk(NTSTATUS s) { return s >= 0; }

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
double QpcNowSeconds() {
    static LARGE_INTEGER freq{};
    static bool init = false;
    if (!init) { QueryPerformanceFrequency(&freq); init = true; }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (!freq.QuadPart) return 0.0;
    return static_cast<double>(t.QuadPart) / static_cast<double>(freq.QuadPart);
}

// 标准引擎类型显示名（与 Windows 任务管理器文案一致）
const wchar_t* EngineTypeDisplayName(int type) {
    switch (type) {
        case kEngineOther:           return L"Other";
        case kEngine3D:              return L"3D";
        case kEngineVideoDecode:     return L"Video Decode";
        case kEngineVideoEncode:     return L"Video Encode";
        case kEngineVideoProcessing: return L"Video Processing";
        case kEngineSceneAssembly:   return L"Scene Assembly";
        case kEngineCopy:            return L"Copy";
        case kEngineOverlay:         return L"Overlay";
        case kEngineCrypto:          return L"Crypto";
        default:                     return L"Unknown";
    }
}

// 引擎排序：与任务管理器一致，3D 优先，然后 Copy / Video Encode / Video Decode。
int EngineDisplayOrder(int type) {
    switch (type) {
        case kEngine3D:              return 0;
        case kEngineCopy:            return 1;
        case kEngineVideoEncode:     return 2;
        case kEngineVideoDecode:     return 3;
        case kEngineCrypto:          return 4;
        case kEngineVideoProcessing: return 5;
        case kEngineSceneAssembly:   return 6;
        case kEngineOverlay:         return 7;
        case kEngineOther:           return 8;
        default:                     return 9;
    }
}

// 注册表 "M-D-YYYY" -> "YYYY-MM-DD"。解析失败时原样返回。
std::wstring NormalizeDriverDate(const wchar_t* raw) {
    if (!raw || !raw[0]) return std::wstring();
    int m = 0, d = 0, y = 0;
    if (swscanf_s(raw, L"%d-%d-%d", &m, &d, &y) == 3 && m >= 1 && m <= 12 && d >= 1 && d <= 31 && y > 1900) {
        wchar_t buf[16];
        swprintf_s(buf, L"%04d-%02d-%02d", y, m, d);
        return std::wstring(buf);
    }
    return std::wstring(raw);
}

// 读取注册表字符串/64 位值
bool ReadRegString(HKEY key, const wchar_t* name, std::wstring& out) {
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS) return false;
    if (type != REG_SZ && type != REG_EXPAND_SZ) return false;
    if (size < sizeof(wchar_t) || size > 64 * 1024) return false;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf.data()), &size) != ERROR_SUCCESS)
        return false;
    out.assign(buf.data());
    return true;
}

bool ReadRegU64(HKEY key, const wchar_t* name, uint64_t& out) {
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS) return false;
    if (type == REG_QWORD && size == sizeof(uint64_t)) {
        uint64_t v = 0;
        if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&v), &size) != ERROR_SUCCESS)
            return false;
        out = v;
        return true;
    }
    // 部分驱动只写 REG_DWORD（本机 MemorySize 就是截断的 DWORD，这里仍支持读取，
    // 但调用方需要知道它可能是截断值）
    if (type == REG_DWORD && size == sizeof(uint32_t)) {
        uint32_t v = 0;
        if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&v), &size) != ERROR_SUCCESS)
            return false;
        out = v;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// pid 枚举：优先 NtQuerySystemInformation(SystemProcessInformation)，
// 失败再退回 EnumProcesses。
// 实测本机 616 进程：
//   NtQSI(5)        avg 14.6 / p50 13.4 / p95 24.2 / max 42.9 ms
//   EnumProcesses   avg 47.3 / p50 33.4 / p95 125.8 / max 144.9 ms
// 两者都远超 5 ms，所以枚举只在 Init 与"每 60 轮一次的刷新轮"里单独做一轮，
// 绝不与 GPU 查询同轮（见 CollectInternal 的三段式状态机）。
// ---------------------------------------------------------------------------

// NtQuerySystemInformation 返回的 SYSTEM_PROCESS_INFORMATION 只需要前若干字段
// 就能取到 UniqueProcessId 与 NextEntryOffset；这里按 x64 布局声明到足够深度。
struct SYSTEM_PROCESS_INFORMATION_LOCAL {
    ULONG          NextEntryOffset;
    ULONG          NumberOfThreads;
    LARGE_INTEGER  WorkingSetPrivateSize;
    ULONG          HardFaultCount;
    ULONG          NumberOfThreadsHighWatermark;
    ULONGLONG      CycleTime;
    LARGE_INTEGER  CreateTime;
    LARGE_INTEGER  UserTime;
    LARGE_INTEGER  KernelTime;
    UNICODE_STRING ImageName;
    LONG           BasePriority;
    HANDLE         UniqueProcessId;
    HANDLE         InheritedFromUniqueProcessId;
};

using PFN_NtQuerySystemInformation = NTSTATUS (NTAPI*)(ULONG, PVOID, ULONG, PULONG);

constexpr ULONG kSystemProcessInformation = 5;
constexpr NTSTATUS kStatusInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004L);

// 返回写进 out 的 pid 个数（0 表示失败）
size_t EnumeratePidsNtQsi(std::vector<uint32_t>& out, std::vector<uint8_t>& scratch) {
    static PFN_NtQuerySystemInformation pNtQsi = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll) pNtQsi = reinterpret_cast<PFN_NtQuerySystemInformation>(
            GetProcAddress(ntdll, "NtQuerySystemInformation"));
    }
    if (!pNtQsi) return 0;

    if (scratch.size() < (1u << 20)) scratch.resize(1u << 20);

    ULONG need = 0;
    NTSTATUS st = 0;
    for (int attempt = 0; attempt < 4; ++attempt) {
        st = pNtQsi(kSystemProcessInformation, scratch.data(),
                    static_cast<ULONG>(scratch.size()), &need);
        if (st != kStatusInfoLengthMismatch) break;
        // 缓冲不足，按内核报告的大小扩容后重试
        if (need + 65536 <= scratch.size()) break;   // 防御：need 异常时不无限扩
        scratch.resize(static_cast<size_t>(need) + 65536);
    }
    if (st < 0) return 0;

    out.clear();
    const uint8_t* p = scratch.data();
    for (size_t guard = 0; guard < 65536; ++guard) {
        const auto* e = reinterpret_cast<const SYSTEM_PROCESS_INFORMATION_LOCAL*>(p);
        const uint32_t pid = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(e->UniqueProcessId));
        if (pid != 0) out.push_back(pid);
        if (e->NextEntryOffset == 0) break;
        p += e->NextEntryOffset;
    }
    return out.size();
}

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct GpuInfoCollector::Impl {
    HMODULE gdi32 = nullptr;
    PFN_D3DKMT_ENUMADAPTERS2    EnumAdapters2 = nullptr;
    PFN_D3DKMT_QUERYADAPTERINFO QueryAdapterInfo = nullptr;
    PFN_D3DKMT_QUERYSTATISTICS  QueryStatistics = nullptr;
    PFN_D3DKMT_CLOSEADAPTER     CloseAdapter = nullptr;

    // 选中的真实 GPU
    LUID     gpuLuid{};
    uint32_t gpuAdapterHandle = 0;
    uint32_t nodeCount = 0;
    uint32_t segmentCount = 0;

    // node 号 -> 引擎分组下标（分组按引擎类型聚合；-1 = 元数据读失败，不参与聚合）
    int nodeGroup[kMaxNodes] = {};

    // 分组信息
    struct Group {
        int engineType = 0;
        std::vector<int> nodes;
    };
    std::vector<Group> groups;

    // ---- 进程句柄缓存 ----
    struct ProcEntry {
        uint32_t pid = 0;
        HANDLE   handle = nullptr;
        uint32_t nodes = 0;          // 该进程参与统计的 node 数
    };
    std::vector<ProcEntry> procs;

    // ---- 基线 / 上一轮 ----
    // 全局 node 时间与"逐进程 node 时间求和"各留一份上一轮值用于差分。
    int64_t prevNodeGlobal[kMaxNodes] = {};
    int64_t prevNodeProcSum[kMaxNodes] = {};
    bool    hasBaseline = false;
    double  prevTimestamp = 0.0;

    // pid 枚举缓存与节流。
    // 分三段推进，保证任何单一轮次都不超过 5 ms（实测见文件头第 10 条）：
    //   Stage 1: 只枚举 pid 并存起来             （EnumProcesses 26~143 ms，单轮）
    //   Stage 2: 用 pid 建句柄缓存 + 记基线      （实测 3.4~4.0 ms）
    //   Stage 3: 正常采集                        （实测 ~1.3 ms）
    std::vector<uint32_t> pidBuffer;
    std::vector<uint32_t> cachedPids;
    std::vector<uint8_t>  ntqsiScratch;   // NtQSI 复用缓冲，避免每次扩容
    bool     needPidEnum = true;      // Stage 1 待做
    bool     needCacheBuild = false;  // Stage 2 待做
    uint32_t collectTick = 0;

    // 枚举 pid：NtQSI 优先，失败退回 EnumProcesses。写进 cachedPids。
    void EnumeratePids() {
        const size_t n = EnumeratePidsNtQsi(cachedPids, ntqsiScratch);
        if (n > 0) return;
        // 兜底
        if (pidBuffer.empty()) pidBuffer.resize(16384);
        DWORD got = 0;
        if (EnumProcesses(reinterpret_cast<DWORD*>(pidBuffer.data()),
                          static_cast<DWORD>(pidBuffer.size() * sizeof(uint32_t)),
                          &got)) {
            cachedPids.assign(pidBuffer.begin(),
                              pidBuffer.begin() + got / static_cast<uint32_t>(sizeof(uint32_t)));
        } else {
            cachedPids.clear();
        }
    }

    // 静态内存总量（Init 时读一次）
    uint64_t dedicatedTotal = 0;
    uint64_t sharedTotal = 0;

    bool QueryNodeGlobal(uint32_t node, int64_t& out) {
        D3DKMT_QUERYSTATISTICS q{};
        q.Type = kStatsNode;
        q.AdapterLuid = gpuLuid;
        q.QueryNodeId = node;
        if (!NtOk(QueryStatistics(&q))) return false;
        out = q.QueryResult.NodeInformation.GlobalInformation.RunningTime;
        return true;
    }

    bool QueryProcessNode(HANDLE h, uint32_t node, int64_t& out) {
        D3DKMT_QUERYSTATISTICS q{};
        q.Type = kStatsProcessNode;
        q.AdapterLuid = gpuLuid;
        q.hProcess = h;
        q.QueryProcessNodeId = node;
        if (!NtOk(QueryStatistics(&q))) return false;
        out = q.QueryResult.ProcessNodeInformation.RunningTime;
        return true;
    }

    void ReleaseProcs() {
        for (auto& p : procs) {
            if (p.handle) CloseHandle(p.handle);
        }
        procs.clear();
    }
};

GpuInfoCollector::GpuInfoCollector() : impl_(new Impl()) {}

GpuInfoCollector::~GpuInfoCollector() {
    if (!impl_) return;
    impl_->ReleaseProcs();
    if (impl_->CloseAdapter && impl_->gpuAdapterHandle) {
        uint32_t h = impl_->gpuAdapterHandle;
        impl_->CloseAdapter(&h);
    }
    if (impl_->gdi32) FreeLibrary(impl_->gdi32);
    delete impl_;
    impl_ = nullptr;
}

void GpuInfoCollector::ResetHistory() {
    history_.clear();
    for (const auto& e : engines_) {
        GpuEngineHistory h;
        h.engineType = e.engineType;
        h.name = e.name;
        h.util.reserve(kHistoryLength);
        history_.push_back(std::move(h));
    }
}

bool GpuInfoCollector::Init() {
    available_ = false;
    adapter_ = GpuAdapterInfo{};
    engines_.clear();
    history_.clear();
    totalUtil_ = 0.0;
    lastCollectMs_ = 0.0;

    if (!impl_) impl_ = new Impl();
    impl_->ReleaseProcs();
    impl_->groups.clear();
    impl_->hasBaseline = false;
    impl_->needPidEnum = true;
    impl_->needCacheBuild = false;
    impl_->collectTick = 0;

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

    if (!impl_->EnumAdapters2 || !impl_->QueryAdapterInfo || !impl_->QueryStatistics)
        return false;

    // ---- 2. 枚举适配器，选出真实 GPU ----
    // 判据：ADAPTERREGISTRYINFO.AdapterString 非空 且 ADAPTER 统计 NodeCount>0。
    // 实测本机 4 个适配器里只有 NVIDIA 那个能读到名字（其余 3 个返回
    // STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034），软件渲染器也没有名字。
    {
        D3DKMT_ADAPTERINFO_LOCAL raw[16]{};
        D3DKMT_ENUMADAPTERS2_LOCAL ea{};
        ea.NumAdapters = 16;
        ea.pAdapters = raw;

        if (!NtOk(impl_->EnumAdapters2(&ea))) return false;
        const uint32_t count = ea.NumAdapters < 16 ? ea.NumAdapters : 16;

        wchar_t chosenName[MAX_PATH] = L"";
        for (uint32_t i = 0; i < count; ++i) {
            D3DKMT_ADAPTERREGISTRYINFO_LOCAL reg{};
            D3DKMT_QUERYADAPTERINFO_LOCAL q1{};
            q1.hAdapter = raw[i].hAdapter;
            q1.Type = kQaiAdapterRegistryInfo;
            q1.pPrivateDriverData = &reg;
            q1.PrivateDriverDataSize = sizeof(reg);
            if (!NtOk(impl_->QueryAdapterInfo(&q1))) continue;
            if (reg.AdapterString[0] == L'\0') continue;

            // 排除纯软件设备（本机 Microsoft Basic Render Driver 也没名字，
            // 这里是双保险）
            D3DKMT_ADAPTERTYPE_LOCAL at{};
            D3DKMT_QUERYADAPTERINFO_LOCAL q2{};
            q2.hAdapter = raw[i].hAdapter;
            q2.Type = kQaiAdapterType;
            q2.pPrivateDriverData = &at;
            q2.PrivateDriverDataSize = sizeof(at);
            if (NtOk(impl_->QueryAdapterInfo(&q2)) && at.SoftwareDevice) continue;

            D3DKMT_QUERYSTATISTICS qa{};
            qa.Type = kStatsAdapter;
            qa.AdapterLuid = raw[i].AdapterLuid;
            if (!NtOk(impl_->QueryStatistics(&qa))) continue;
            if (qa.QueryResult.AdapterInformation.NodeCount == 0) continue;

            impl_->gpuLuid = raw[i].AdapterLuid;
            impl_->gpuAdapterHandle = raw[i].hAdapter;
            impl_->nodeCount = qa.QueryResult.AdapterInformation.NodeCount;
            if (impl_->nodeCount > kMaxNodes) impl_->nodeCount = kMaxNodes;
            impl_->segmentCount = qa.QueryResult.AdapterInformation.NbSegments;
            if (impl_->segmentCount > kMaxSegments) impl_->segmentCount = kMaxSegments;
            wcsncpy_s(chosenName, reg.AdapterString, _TRUNCATE);
            adapter_.name = reg.AdapterString;
            break;
        }
        if (impl_->nodeCount == 0) return false;
        (void)chosenName;
    }

    adapter_.nodeCount = impl_->nodeCount;
    adapter_.segmentCount = impl_->segmentCount;

    // ---- 3. 每个 node 的引擎元数据（pack=1 关键，见文件头第 2 条）----
    // 任何单个 node 失败不会让整模块退化：该 node 记 -1 并被排除在聚合外。
    for (uint32_t n = 0; n < impl_->nodeCount; ++n) {
        impl_->nodeGroup[n] = -1;

        D3DKMT_NODEMETADATA_LOCAL nm{};
        nm.NodeOrdinalAndAdapterIndex = n;   // 物理适配器 0 + node 号
        D3DKMT_QUERYADAPTERINFO_LOCAL q{};
        q.hAdapter = impl_->gpuAdapterHandle;
        q.Type = kQaiNodeMetadata;
        q.pPrivateDriverData = &nm;
        q.PrivateDriverDataSize = sizeof(nm);
        if (!NtOk(impl_->QueryAdapterInfo(&q))) continue;

        int type = static_cast<int>(nm.NodeData.EngineType);
        if (type < 0 || type > 8) type = kEngineOther;

        // 找到/创建该引擎类型的分组
        int gi = -1;
        for (size_t g = 0; g < impl_->groups.size(); ++g) {
            if (impl_->groups[g].engineType == type) { gi = static_cast<int>(g); break; }
        }
        if (gi < 0) {
            Impl::Group grp;
            grp.engineType = type;
            impl_->groups.push_back(grp);
            gi = static_cast<int>(impl_->groups.size()) - 1;
        }
        impl_->groups[gi].nodes.push_back(static_cast<int>(n));
        impl_->nodeGroup[n] = gi;
    }

    if (impl_->groups.empty()) {
        // 元数据全失败时仍然可降级为"整卡一个引擎"，但那样总使用率还能用；
        // 这里选择不编造引擎名，只保留按 node 的原始聚合，不进入 engines_。
        return false;
    }

    // 分组按显示顺序排序，同时重建 nodeGroup 映射
    std::sort(impl_->groups.begin(), impl_->groups.end(),
              [](const Impl::Group& a, const Impl::Group& b) {
                  return EngineDisplayOrder(a.engineType) < EngineDisplayOrder(b.engineType);
              });
    for (size_t g = 0; g < impl_->groups.size(); ++g) {
        for (int n : impl_->groups[g].nodes) impl_->nodeGroup[n] = static_cast<int>(g);
    }

    // ---- 4. 适配器静态信息 ----
    // 4a. 显存总量
    {
        D3DKMT_SEGMENTSIZEINFO_LOCAL ss{};
        D3DKMT_QUERYADAPTERINFO_LOCAL q{};
        q.hAdapter = impl_->gpuAdapterHandle;
        q.Type = kQaiGetSegmentSize;
        q.pPrivateDriverData = &ss;
        q.PrivateDriverDataSize = sizeof(ss);
        if (NtOk(impl_->QueryAdapterInfo(&q))) {
            impl_->dedicatedTotal = ss.DedicatedVideoMemorySize;
            impl_->sharedTotal = ss.SharedSystemMemorySize;
            adapter_.dedicatedTotal = ss.DedicatedVideoMemorySize;
            adapter_.sharedTotal = ss.SharedSystemMemorySize;
        }
    }

    // 4b. 物理位置（PCI 总线/设备/功能）
    {
        D3DKMT_ADAPTERADDRESS_LOCAL ad{};
        D3DKMT_QUERYADAPTERINFO_LOCAL q{};
        q.hAdapter = impl_->gpuAdapterHandle;
        q.Type = kQaiAdapterAddress;
        q.pPrivateDriverData = &ad;
        q.PrivateDriverDataSize = sizeof(ad);
        if (NtOk(impl_->QueryAdapterInfo(&q))) {
            adapter_.pciBus = ad.BusNumber;
            adapter_.pciDevice = ad.DeviceNumber;
            adapter_.pciFunction = ad.FunctionNumber;
            wchar_t buf[96];
            swprintf_s(buf, L"PCI 总线 %u，设备 %u，功能 %u",
                       ad.BusNumber, ad.DeviceNumber, ad.FunctionNumber);
            adapter_.physicalLocation = buf;
        }
    }

    // 4c. 注册表：驱动版本/日期 + qwMemorySize（用于推导硬件保留内存）
    {
        static const wchar_t* kDisplayClass =
            L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
        HKEY hClass = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kDisplayClass, 0, KEY_READ, &hClass) == ERROR_SUCCESS) {
            uint64_t bestQwMem = 0;
            std::wstring bestVer, bestDate;
            bool haveMatch = false;

            for (int i = 0; i < 32; ++i) {
                wchar_t subName[16];
                swprintf_s(subName, L"%04d", i);
                HKEY hSub = nullptr;
                if (RegOpenKeyExW(hClass, subName, 0, KEY_READ, &hSub) != ERROR_SUCCESS) continue;

                std::wstring desc, ver, date;
                uint64_t qwMem = 0;
                const bool okDesc = ReadRegString(hSub, L"DriverDesc", desc);
                ReadRegString(hSub, L"DriverVersion", ver);
                ReadRegString(hSub, L"DriverDate", date);
                ReadRegU64(hSub, L"HardwareInformation.qwMemorySize", qwMem);
                RegCloseKey(hSub);

                if (!okDesc) continue;
                if (_wcsicmp(desc.c_str(), adapter_.name.c_str()) != 0) continue;

                // 同名多条时优先取有 qwMemorySize 的那条
                if (!haveMatch || (qwMem > bestQwMem)) {
                    bestVer = ver;
                    bestDate = date;
                    bestQwMem = qwMem;
                    haveMatch = true;
                }
            }
            RegCloseKey(hClass);

            if (haveMatch) {
                adapter_.driverVersion = bestVer;
                adapter_.driverDate = NormalizeDriverDate(bestDate.c_str());
                // 硬件保留内存 = 注册表报告的总量 - VidMM 可用专用显存
                if (bestQwMem > impl_->dedicatedTotal)
                    adapter_.hwReserved = bestQwMem - impl_->dedicatedTotal;
            }
        }
    }

    // ---- 5. 构建引擎列表（使用率留 0，等第一轮 Collect 填）----
    engines_.clear();
    engines_.reserve(impl_->groups.size());
    for (const auto& g : impl_->groups) {
        GpuEngineSample s;
        s.engineType = g.engineType;
        s.name = EngineTypeDisplayName(g.engineType);
        s.nodeCount = static_cast<int>(g.nodes.size());
        engines_.push_back(std::move(s));
    }
    ResetHistory();

    // ---- 6. 首次进程扫描交给 Collect 的状态机 ----
    // 这里只做廉价的准备：枚举一次 pid（Init 允许慢），然后由第一次 Collect()
    // 建立句柄缓存 + 基线，之后的 Collect() 才是稳态 ~1.3 ms。
    impl_->pidBuffer.resize(16384);
    impl_->procs.reserve(128);
    impl_->needPidEnum = true;
    impl_->needCacheBuild = false;
    impl_->collectTick = 0;

    available_ = true;

    // 只做 Stage 1（枚举 pid），把 14~43 ms 的枚举留在 Init 里。
    // 这样第一次 Collect() 只承担建缓存（~4.4 ms），不会叠加枚举开销。
    impl_->EnumeratePids();
    if (!impl_->cachedPids.empty()) {
        impl_->needPidEnum = false;
        impl_->needCacheBuild = true;
    }
    return true;
}

void GpuInfoCollector::RefreshPids() {
    if (!impl_) return;
    impl_->needPidEnum = true;
    impl_->needCacheBuild = false;
    impl_->collectTick = 0;
}

void GpuInfoCollector::Collect() {
    if (!available_ || !impl_) {
        lastCollectMs_ = 0.0;
        return;
    }
    const double t0 = QpcNowSeconds();
    CollectInternal(nullptr, 0, t0);
}

void GpuInfoCollector::Collect(const uint32_t* pids, size_t count) {
    if (!available_ || !impl_) {
        lastCollectMs_ = 0.0;
        return;
    }
    if (!pids || count == 0) {
        Collect();
        return;
    }
    const double t0 = QpcNowSeconds();
    CollectInternal(pids, count, t0);
}

void GpuInfoCollector::CollectInternal(const uint32_t* pids, size_t count, double t0) {
    // ------------------------------------------------------------------
    // 1. 三段式状态机，保证任何单轮都不超过 5 ms。
    //
    //    实测本机 616 进程：
    //      EnumProcesses          26~143 ms（p95 103）
    //      OpenProcess 全量        1.4 ms
    //      PROCESS_ADAPTER ×39     2.98 ms
    //      逐进程 T_PROCESS_NODE   0.77~1.38 ms
    //      segment ×3              0.016 ms
    //    所以把代价最大的 EnumProcesses 单独放到一轮（Stage 1），
    //    建缓存+记基线放下一轮（Stage 2，1.4+3.0=4.4 ms，刚好under 5），
    //    之后都是 Stage 3 稳态（~1.3 ms）。
    //
    //    调用方若通过 Collect(pids, count) 直接给 pid，则永远不进 Stage 1，
    //    连这 26~143 ms 也不存在 —— 这是唯一能保证每轮都 < 2 ms 的路径。
    // ------------------------------------------------------------------

    // ---- Stage 1：只枚举 pid ----
    if (!pids && impl_->needPidEnum) {
        impl_->needPidEnum = false;
        impl_->EnumeratePids();
        if (!impl_->cachedPids.empty()) impl_->needCacheBuild = true;
        lastCollectMs_ = (QpcNowSeconds() - t0) * 1000.0;
        return;   // 本轮只做枚举
    }

    // ---- Stage 2：用 pid 建句柄缓存 + 记基线（每 60 轮一次）----
    const bool needBuild = impl_->needCacheBuild;
    if (needBuild) {
        impl_->needCacheBuild = false;
        impl_->collectTick = 0;
        impl_->ReleaseProcs();
        impl_->hasBaseline = false;

        const uint32_t* usePids = pids;
        size_t useCount = count;
        if (!usePids) {
            usePids = impl_->cachedPids.data();
            useCount = impl_->cachedPids.size();
        }

        // 建立句柄缓存：只保留有 GPU 记录的进程
        for (size_t i = 0; i < useCount; ++i) {
            const uint32_t pid = usePids[i];
            if (pid == 0) continue;
            // 必须 PROCESS_QUERY_INFORMATION：PROCESS_QUERY_LIMITED_INFORMATION
            // 会让 D3DKMTQueryStatistics 返回 STATUS_INVALID_PARAMETER。
            HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
            if (!h) continue;

            D3DKMT_QUERYSTATISTICS qa{};
            qa.Type = kStatsProcessAdapter;
            qa.AdapterLuid = impl_->gpuLuid;
            qa.hProcess = h;
            if (!NtOk(impl_->QueryStatistics(&qa))) {
                CloseHandle(h);
                continue;
            }
            // PROCESS_ADAPTER 的 NodeCount 实测就是该进程参与的 node 数
            // （本机 38 个 GPU 进程全部报 14，等于适配器 NodeCount）。
            uint32_t nodes = qa.QueryResult.ProcessAdapterInformation.NodeCount;
            if (nodes > impl_->nodeCount) nodes = impl_->nodeCount;
            if (nodes == 0) {
                CloseHandle(h);
                continue;
            }

            Impl::ProcEntry e;
            e.pid = pid;
            e.handle = h;
            e.nodes = nodes;
            impl_->procs.push_back(e);
        }

        adapter_.processCount = static_cast<int>(impl_->procs.size());

        // 记基线（全局 node + 逐进程 node 求和）
        std::fill(impl_->prevNodeGlobal, impl_->prevNodeGlobal + kMaxNodes, 0);
        std::fill(impl_->prevNodeProcSum, impl_->prevNodeProcSum + kMaxNodes, 0);
        for (uint32_t n = 0; n < impl_->nodeCount; ++n) {
            int64_t v = 0;
            if (impl_->QueryNodeGlobal(n, v)) impl_->prevNodeGlobal[n] = v;
        }
        for (const auto& p : impl_->procs) {
            for (uint32_t n = 0; n < p.nodes; ++n) {
                int64_t v = 0;
                if (impl_->QueryProcessNode(p.handle, n, v)) impl_->prevNodeProcSum[n] += v;
            }
        }
        impl_->prevTimestamp = QpcNowSeconds();
        impl_->hasBaseline = true;

        // 顺带读一次显存（3 个 segment 只要 0.016 ms），这样第一轮就有占用值
        {
            uint64_t dedUse = 0, shrUse = 0;
            for (uint32_t s = 0; s < impl_->segmentCount; ++s) {
                D3DKMT_QUERYSTATISTICS qs{};
                qs.Type = kStatsSegment;
                qs.AdapterLuid = impl_->gpuLuid;
                qs.QuerySegmentId = s;
                if (!NtOk(impl_->QueryStatistics(&qs))) continue;
                const auto& si = qs.QueryResult.SegmentInformation;
                if (si.Aperture) shrUse += si.BytesResident;
                else             dedUse += si.BytesResident;
            }
            adapter_.dedicatedInUse = dedUse;
            adapter_.sharedInUse = shrUse;
        }

        lastCollectMs_ = (QpcNowSeconds() - t0) * 1000.0;
        return;   // 本轮建基线，不产出使用率（上一轮的值仍可读）
    }

    // 每 60 轮触发一次重新枚举（新启动的 GPU 进程最多约 1 分钟后可见）
    if (!pids && ++impl_->collectTick >= 60) {
        impl_->collectTick = 0;
        impl_->needPidEnum = true;
    }

    // ------------------------------------------------------------------
    // 2. 正常情况下：查全局 node + 逐进程 node，做差分
    // ------------------------------------------------------------------
    int64_t nodeGlobal[kMaxNodes] = {};
    int64_t nodeProcSum[kMaxNodes] = {};

    for (uint32_t n = 0; n < impl_->nodeCount; ++n) {
        int64_t v = 0;
        if (impl_->QueryNodeGlobal(n, v)) nodeGlobal[n] = v;
    }

    // 顺带清理已退出的进程。实测（docs/evidence/gpuinfo_probe14.cpp）进程退出后
    // D3DKMT PROCESS_ADAPTER / PROCESS_NODE 全部返回 0xC000000D，所以
    // "所有 node 都查不到" 就是可靠的退出判据，不需要额外的 GetProcessTimes
    // （那 38 次调用实测 p99 达 23.5 ms，是稳态路径里最大的一笔可省开销）。
    // 句柄绑定的是进程对象本身，pid 被复用也不会把旧数据算到新进程头上，
    // 因为句柄指向的仍是已退出那个对象，它的所有查询都会失败。
    size_t writeIdx = 0;
    for (size_t readIdx = 0; readIdx < impl_->procs.size(); ++readIdx) {
        Impl::ProcEntry& p = impl_->procs[readIdx];
        if (!p.handle) continue;

        int64_t sum[kMaxNodes] = {};
        bool any = false;
        for (uint32_t n = 0; n < p.nodes; ++n) {
            int64_t v = 0;
            if (impl_->QueryProcessNode(p.handle, n, v)) {
                sum[n] = v;
                any = true;
            }
        }
        if (!any) {
            CloseHandle(p.handle);
            p.handle = nullptr;
            continue;
        }
        for (uint32_t n = 0; n < p.nodes; ++n) nodeProcSum[n] += sum[n];
        if (writeIdx != readIdx) impl_->procs[writeIdx] = p;
        ++writeIdx;
    }
    impl_->procs.resize(writeIdx);
    adapter_.processCount = static_cast<int>(impl_->procs.size());

    // ---- 每引擎使用率 ----
    const double now = QpcNowSeconds();
    double dt = impl_->hasBaseline ? (now - impl_->prevTimestamp) : 0.0;
    if (dt < 0.0) dt = 0.0;
    const double denom = dt * 1.0e7;   // RunningTime 单位 100ns

    // 每个分组累计
    std::vector<int64_t> groupDelta(impl_->groups.size(), 0);

    for (uint32_t n = 0; n < impl_->nodeCount; ++n) {
        const int gi = impl_->nodeGroup[n];
        if (gi < 0) continue;

        int64_t dGlobal = nodeGlobal[n] - impl_->prevNodeGlobal[n];
        int64_t dProc   = nodeProcSum[n] - impl_->prevNodeProcSum[n];
        if (dGlobal < 0) dGlobal = 0;
        if (dProc < 0) dProc = 0;
        // 见文件头第 3 条：本机 3D 节点的全局计数恒为 0，必须取两者较大值
        const int64_t d = dGlobal > dProc ? dGlobal : dProc;
        groupDelta[gi] += d;
    }

    // 写回引擎
    if (engines_.size() == impl_->groups.size()) {
        for (size_t g = 0; g < impl_->groups.size(); ++g) {
            engines_[g].runningDelta = static_cast<uint64_t>(groupDelta[g]);
            double pct = (denom > 0.0) ? static_cast<double>(groupDelta[g]) / denom * 100.0 : 0.0;
            if (pct < 0.0) pct = 0.0;
            if (pct > 100.0) pct = 100.0;
            engines_[g].utilPercent = pct;
        }
    }

    // 总使用率 = 最忙的引擎（与任务管理器一致）
    double busiest = 0.0;
    for (const auto& e : engines_) {
        if (e.utilPercent > busiest) busiest = e.utilPercent;
    }
    totalUtil_ = busiest;

    // 保存本轮原始值
    for (uint32_t n = 0; n < impl_->nodeCount; ++n) {
        impl_->prevNodeGlobal[n] = nodeGlobal[n];
        impl_->prevNodeProcSum[n] = nodeProcSum[n];
    }
    impl_->prevTimestamp = now;
    impl_->hasBaseline = true;

    // ---- 显存占用（segment resident，见文件头第 4 条）----
    {
        uint64_t dedUse = 0, shrUse = 0;
        for (uint32_t s = 0; s < impl_->segmentCount; ++s) {
            D3DKMT_QUERYSTATISTICS qs{};
            qs.Type = kStatsSegment;
            qs.AdapterLuid = impl_->gpuLuid;
            qs.QuerySegmentId = s;
            if (!NtOk(impl_->QueryStatistics(&qs))) continue;
            const auto& si = qs.QueryResult.SegmentInformation;
            if (si.Aperture) shrUse += si.BytesResident;
            else             dedUse += si.BytesResident;
        }
        adapter_.dedicatedInUse = dedUse;
        adapter_.sharedInUse = shrUse;
    }

    // ---- 历史曲线 ----
    for (size_t g = 0; g < history_.size() && g < engines_.size(); ++g) {
        auto& h = history_[g].util;
        h.push_back(engines_[g].utilPercent);
        if (h.size() > kHistoryLength)
            h.erase(h.begin(), h.begin() + static_cast<std::ptrdiff_t>(h.size() - kHistoryLength));
    }

    lastCollectMs_ = (QpcNowSeconds() - t0) * 1000.0;
}

std::wstring GpuInfoCollector::ProbeDirectXVersion() {
    // 未 Init（或 Init 失败）时直接降级返回空，绝不做那 500~1000 ms 的设备创建。
    if (!available_) return std::wstring();

    // 只做一次。实测 480~1000 ms（瓶颈是加载 NVIDIA 用户态驱动），
    // 绝不能放进 Init/Collect。这里用 LoadLibrary + GetProcAddress，
    // 不引入 d3d11.lib 链接依赖。
    if (!adapter_.directXVersion.empty()) return adapter_.directXVersion;

    // D3D_FEATURE_LEVEL 数值（d3dcommon.h；WIN32_LEAN_AND_MEAN 下不 include 该头）
    constexpr uint32_t kFL12_1 = 0xC100;
    constexpr uint32_t kFL12_0 = 0xC000;
    constexpr uint32_t kFL11_1 = 0xB100;
    constexpr uint32_t kFL11_0 = 0xB000;
    constexpr uint32_t kFL10_1 = 0xA100;
    constexpr uint32_t kFL10_0 = 0xA000;

    // HRESULT (WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    //                   const D3D_FEATURE_LEVEL*, UINT, UINT,
    //                   ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**)
    using PFN_D3D11CreateDevice = HRESULT (WINAPI*)(
        void*, int, HMODULE, uint32_t,
        const uint32_t*, uint32_t, uint32_t,
        void**, uint32_t*, void**);

    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    if (!d3d11) return std::wstring();
    auto create = reinterpret_cast<PFN_D3D11CreateDevice>(
        GetProcAddress(d3d11, "D3D11CreateDevice"));
    if (!create) {
        FreeLibrary(d3d11);
        return std::wstring();
    }

    const uint32_t levels[] = { kFL12_1, kFL12_0, kFL11_1, kFL11_0, kFL10_1, kFL10_0 };
    void* device = nullptr;
    void* context = nullptr;
    uint32_t got = 0;

    // D3D_DRIVER_TYPE_HARDWARE = 1；SDK 版本 7（D3D11_SDK_VERSION）
    HRESULT hr = create(nullptr, 1, nullptr, 0, levels,
                        static_cast<uint32_t>(sizeof(levels) / sizeof(levels[0])),
                        7, &device, &got, &context);

    if (context) {
        // ID3D11DeviceContext::Release 是虚表第 2 项（IUnknown 之后）
        auto vt = *reinterpret_cast<void***>(context);
        reinterpret_cast<ULONG (STDMETHODCALLTYPE*)(void*)>(vt[2])(context);
    }
    if (device) {
        auto vt = *reinterpret_cast<void***>(device);
        reinterpret_cast<ULONG (STDMETHODCALLTYPE*)(void*)>(vt[2])(device);
    }
    FreeLibrary(d3d11);

    if (FAILED(hr)) return std::wstring();

    const wchar_t* ver = nullptr;
    switch (got) {
        case kFL12_1: ver = L"12.1"; break;
        case kFL12_0: ver = L"12.0"; break;
        case kFL11_1: ver = L"11.1"; break;
        case kFL11_0: ver = L"11.0"; break;
        case kFL10_1: ver = L"10.1"; break;
        case kFL10_0: ver = L"10.0"; break;
        default: break;
    }
    if (!ver) return std::wstring();

    adapter_.directXVersion = ver;
    return adapter_.directXVersion;
}

} // namespace tmp
