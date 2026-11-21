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
//
// 11) 多 GPU（核显 + 独显）：**旧实现只保留一个适配器，这是「核显看不到」的根因**。
//    旧判据是"ADAPTERREGISTRYINFO.AdapterString 非空"，而这个名字在多卡机器上
//    极不稳定：本机 4 个适配器只有 NVIDIA 能读到它，其余 3 个一律返回
//    0xC0000034。于是任何"读不到名字"的真实 GPU（核显、第二块独显）都被整块丢弃。
//
//    修正后的筛选链（docs/evidence/gpuinfo_probe15.cpp 逐项实测，本机 4 个适配器）：
//      ADAPTERTYPE(15)  → SoftwareDevice        过滤 Microsoft Basic Render Driver
//                                                  （实测 0x105，Render=1 Software=1）
//      ADAPTERTYPE(15)  → !RenderSupported      过滤纯显示适配器（ToDesk / GameViewer）
//                                                  （二者均 0x142：Render=0 Display=1 Indirect=1）
//      T_ADAPTER(0)     → NodeCount>0           没有引擎的适配器无法采集
//      名字回退链：ADAPTERREGISTRYINFO(8) → DRIVER_DESCRIPTION(65) → "GPU N"
//
//    关于名字源 (65) KMTQAITYPE_DRIVER_DESCRIPTION（Win10 1809+）：实测 4/4 成功，
//    且给的是**该适配器自己的**名字：
//      [0] GameViewer Virtual Display Adapter   [1] Todesk Virtual Display Adapter
//      [2] NVIDIA GeForce discrete GPU        [3] Microsoft Basic Render Driver
//    另一个看似可行的 (41) PHYSICALADAPTERPNPKEY 的 **software** 键不能用来取名：
//    它返回 Class\{4d36e968-...}\NNNN，而本机 adapter[0]/[1] 与 adapter[2] 返回的是
//    **同一个** \0000（DriverDesc = NVIDIA），用它会把虚拟显示器全叫成 NVIDIA。
//    （它可以用来取 DriverVersion，但本模块仍走原来的 Class 扫描 + DriverDesc 匹配，
//      因为那条路径已在本机 4 个子键上验证过。）
//
//    为什么必须排除虚拟显示适配器（不是"嫌多"，是"是假的"）：
//    实测 ToDesk / GameViewer 的 T_ADAPTER 与 NVIDIA **逐字段相同**
//    （NodeCount=14 / NbSegments=3 / VidPnSources=4，且 PNP 键同为
//      PCI\VEN_10DE&DEV_1E84...），它们的逐进程统计也逐卡相同。
//    它们不是独立 GPU，而是同一 VidMM 后端的镜像；收进来会让侧栏出现 3 个
//    名字不同、数字完全相同的 "GPU"，而且总利用率不变 —— 正是本项目最忌讳的
//    「假功能」。用脚本把这条过滤关掉实测确认过（3 个适配器输出完全相同）。
//
//    原版任务管理器的行为可旁证：它的 GPU 数据源是 PDH
//    \GPU Adapter Memory(luid_...)\Dedicated Usage，实测本机该计数器只有
//    **2 个实例**（NVIDIA 与 Basic Render），ToDesk / GameViewer 完全不出现
//    （docs/evidence/gpuinfo_probe16.cpp）。即原版不列虚拟显示适配器。
//
//    多卡成本：每轮 D3DKMT 查询次数 = Σ_k(nodeCount_k × 2 + segmentCount_k)。
//    实测（docs/evidence/bench_gpuinfo.cpp 用本仓库同一编译选项）：
//      1 张卡：Collect() 稳态 p50 0.699 / avg 1.265 ms（旧实现 0.61 / 1.01）
//      3 张卡：单轮 4~7 ms。所以 Init 时用 kMaxAdapters=4 封顶 ——
//              4 张真实 GPU 已覆盖所有家用场景（核显 + 独显 + 双卡），
//              再多就让渡给"不采集"而不是让一轮超预算。
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
constexpr uint32_t kQaiDriverDescription   = 65;

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
// 无名适配器的命名回退链
//
// 背景：ADAPTERREGISTRYINFO(8) 在本机只对 NVIDIA 返回名字，其余适配器一律
// 0xC0000034（STATUS_OBJECT_NAME_NOT_FOUND）。若因此跳过适配器，核显和多卡
// 机器上的第二块卡就整个消失 —— 这正是「核显看不到」的根因。
//
// 实测各候选源（docs/evidence/gpuinfo_probe15.cpp，本机 4 个适配器逐个打印）：
//   (a) KMTQAITYPE_PHYSICALADAPTERPNPKEY(41, software)  4/4 成功
//       返回 Class\{4d36e968-...}\NNNN，其 DriverDesc 就是显示名。
//       注意：**同一驱动的所有适配器指向同一个 Class 子键**（本机 adapter[0]
//       GameViewer 与 adapter[1] ToDesk 都返回 \0000，而 \0000 的 DriverDesc
//       是 NVIDIA），所以它只能用来取驱动版本，**不能用来取名字**。
//   (b) KMTQAITYPE_DRIVER_DESCRIPTION(65)               4/4 成功，**这是正解**
//       adapter[0] = "GameViewer Virtual Display Adapter"
//       adapter[1] = "Todesk Virtual Display Adapter"
//       adapter[2] = "NVIDIA GeForce discrete GPU"
//       adapter[3] = "Microsoft Basic Render Driver"
//       注意它给的是**该适配器自己的**显示名，与 (a) 的 Class 子键无关。
//       Win10 1809+ 才有；旧系统返回失败时走 (c)。
//   (c) KMTQAITYPE_ADAPTERREGISTRYINFO(8)               1/4 成功（NVIDIA）
//   (d) 占位名 "GPU N"
//
// 顺序：先用 (c)（老系统/老驱动上它最准，且是单卡时代的唯一来源），
//       失败再用 (b)，再失败用 (d)。绝不因为读不到名字就丢掉适配器。
// ---------------------------------------------------------------------------

// 读 KMTQAITYPE_DRIVER_DESCRIPTION(65)。失败返回空串。
std::wstring QueryDriverDescription(PFN_D3DKMT_QUERYADAPTERINFO qai, uint32_t hAdapter) {
    // D3DKMT_DRIVER_DESCRIPTION = { WCHAR DriverDescription[4096]; }
    // SDK 里没有额外的长度字段，`PrivateDriverDataSize` 必须是整个结构大小。
    std::vector<wchar_t> buf(4096, L'\0');
    D3DKMT_QUERYADAPTERINFO_LOCAL q{};
    q.hAdapter = hAdapter;
    q.Type = kQaiDriverDescription;
    q.pPrivateDriverData = buf.data();
    q.PrivateDriverDataSize = static_cast<uint32_t>(buf.size() * sizeof(wchar_t));
    if (!NtOk(qai(&q))) return std::wstring();
    buf.back() = L'\0';
    if (buf[0] == L'\0') return std::wstring();
    return std::wstring(buf.data());
}

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

    // ------------------------------------------------------------------
    // 多卡：每张真实 GPU 一个槽位，取代原来的 gpuLuid / gpuAdapterHandle 单值。
    //
    // 为什么必须多槽：原来的筛选条件里 "ADAPTERREGISTRYINFO.AdapterString 非空"
    // 在本机只有 NVIDIA 能命中，其余 3 个适配器返回 0xC0000034（名字读不到），
    // 于是核显 / 第二块独显会被无名条件整块丢掉。实测本机：
    //   [0] GameViewer 虚拟显示 AdapterString 空
    //   [1] ToDesk     虚拟显示 AdapterString 空
    //   [2] NVIDIA      AdapterString 有值
    //   [3] Basic Render AdapterString 空（软件渲染器）
    // （docs/evidence/gpuinfo_probe15.cpp 的实测输出）
    // ------------------------------------------------------------------
    struct AdapterSlot {
        LUID     luid{};
        uint32_t hAdapter = 0;
        uint32_t nodeCount = 0;
        uint32_t segmentCount = 0;
        std::wstring name;
        uint64_t dedicatedTotal = 0;
        uint64_t sharedTotal = 0;

        // node 号 -> 引擎分组下标（-1 = 该 node 元数据读失败）
        int nodeGroup[kMaxNodes] = {};
        struct Group {
            int engineType = 0;
            std::vector<int> nodes;
        };
        std::vector<Group> groups;

        // 上一轮差分基线
        int64_t prevNodeGlobal[kMaxNodes] = {};
        int64_t prevNodeProcSum[kMaxNodes] = {};

        // 累计发现的有 GPU 记录的进程数（本卡口径）
        int processCount = 0;

        void ResetBaseline() {
            std::fill(prevNodeGlobal, prevNodeGlobal + kMaxNodes, 0);
            std::fill(prevNodeProcSum, prevNodeProcSum + kMaxNodes, 0);
        }
    };
    std::vector<AdapterSlot> adapters;

    // 枚举阶段超过这个数量就放弃后续适配器：
    // 每个适配器每人轮要 1 次 T_ADAPTER + nodeCount 次 T_NODE + segmentCount 次
    // T_SEGMENT。实测单次 D3DKMT 查询约 0.03~1 ms，若被虚拟显示器塞满几十个
    // 适配器，一轮就会超预算。4 张真实 GPU 已覆盖所有家用场景（核显 + 独显 +
    // 双卡 SLI/CrossFire）；超过则只取前 kMaxAdapters 张，其余记日志跳过。
    static constexpr size_t kMaxAdapters = 4;

    // ---- 进程句柄缓存 ----
    // 一个进程可能同时命中多张卡（跨卡进程），所以每项记录哪些卡有记录。
    // 注意这里**不存** node 数：多卡时每张卡的 nodeCount 可以不同，
    // 统一用 adapterMask 里的卡号去 adapters[ai].nodeCount 取，避免两套真值。
    struct ProcEntry {
        uint32_t pid = 0;
        HANDLE   handle = nullptr;
        uint32_t adapterMask = 0;    // bit i = 第 i 个槽位有该进程的记录
    };
    std::vector<ProcEntry> procs;

    bool     hasBaseline = false;
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

    // 静态内存总量不再在 Impl 级保存：多卡时每张卡一份，
    // 存在 AdapterSlot::dedicatedTotal / sharedTotal（单一真值源）。

    bool QueryNodeGlobal(const AdapterSlot& a, uint32_t node, int64_t& out) {
        D3DKMT_QUERYSTATISTICS q{};
        q.Type = kStatsNode;
        q.AdapterLuid = a.luid;
        q.QueryNodeId = node;
        if (!NtOk(QueryStatistics(&q))) return false;
        out = q.QueryResult.NodeInformation.GlobalInformation.RunningTime;
        return true;
    }

    bool QueryProcessNode(const AdapterSlot& a, HANDLE h, uint32_t node, int64_t& out) {
        D3DKMT_QUERYSTATISTICS q{};
        q.Type = kStatsProcessNode;
        q.AdapterLuid = a.luid;
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

    // 读每张卡的显存占用（segment resident）。
    // 口径见文件头第 4 条：Aperture==0 的 BytesResident 之和 = 专用占用，
    // Aperture!=0 的 = 共享占用。
    // 代价：每卡每 segment 一次 D3DKMTQueryStatistics（实测 3 个 segment 0.016 ms）。
    // out 必须与 adapters 等长（由调用方保证）。
    void RefreshMemoryUsage(std::vector<GpuAdapterInfo>* out) {
        for (size_t ai = 0; ai < adapters.size(); ++ai) {
            const AdapterSlot& a = adapters[ai];
            uint64_t dedUse = 0, shrUse = 0;
            for (uint32_t s = 0; s < a.segmentCount; ++s) {
                D3DKMT_QUERYSTATISTICS qs{};
                qs.Type = kStatsSegment;
                qs.AdapterLuid = a.luid;
                qs.QuerySegmentId = s;
                if (!NtOk(QueryStatistics(&qs))) continue;
                const auto& si = qs.QueryResult.SegmentInformation;
                if (si.Aperture) shrUse += si.BytesResident;
                else             dedUse += si.BytesResident;
            }
            if (out && ai < out->size()) {
                (*out)[ai].dedicatedInUse = dedUse;
                (*out)[ai].sharedInUse = shrUse;
            }
        }
    }
};

GpuInfoCollector::GpuInfoCollector() : impl_(new Impl()) {}

GpuInfoCollector::~GpuInfoCollector() {
    if (!impl_) return;
    impl_->ReleaseProcs();
    // 多卡：逐个关闭枚举时打开的适配器句柄（旧实现只有一个）
    if (impl_->CloseAdapter) {
        for (const auto& a : impl_->adapters) {
            uint32_t h = a.hAdapter;
            if (h) impl_->CloseAdapter(&h);
        }
    }
    impl_->adapters.clear();
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

// 合并视图：所有适配器的同名引擎取最忙的那一张卡。
//
// 为什么取 max 不相加：引擎利用率是时间片占比，两张卡各 60% 时相加得 120%
// 是无意义的；任务管理器的总利用率也是"最忙的引擎/最忙的卡"。
// 这张表的用途是喂给只认识单一引擎列表的旧调用方（perfdata.cpp 的
// gpuEngines 与 GPU 页的 4 个引擎小图），逐卡明细在 adapters_[i].engines。
void GpuInfoCollector::BuildMergedEngines() {
    engines_.clear();

    // 顺序：按引擎类型的显示顺序（3D 优先），不是按首次出现顺序 ——
    // 否则多卡时「核显只有 3D、独显有全部引擎」会让顺序随卡序变化。
    // 收集出现过的 engineType
    std::vector<int> types;
    for (const auto& a : adapters_) {
        for (const auto& e : a.engines) {
            if (std::find(types.begin(), types.end(), e.engineType) == types.end())
                types.push_back(e.engineType);
        }
    }
    std::sort(types.begin(), types.end(), [](int a, int b) {
        return EngineDisplayOrder(a) < EngineDisplayOrder(b);
    });

    for (int t : types) {
        GpuEngineSample s;
        s.engineType = t;
        s.name = EngineTypeDisplayName(t);
        int nodes = 0;
        for (const auto& a : adapters_) {
            for (const auto& e : a.engines) {
                if (e.engineType != t) continue;
                // nodeCount 相加是对的：这是"有多少个 node 参与该引擎"
                nodes += e.nodeCount;
                if (e.utilPercent > s.utilPercent) {
                    s.utilPercent = e.utilPercent;
                    s.runningDelta = e.runningDelta;
                }
            }
        }
        s.nodeCount = nodes;
        engines_.push_back(std::move(s));
    }
}

bool GpuInfoCollector::Init() {
    available_ = false;
    adapters_.clear();
    engines_.clear();
    history_.clear();
    totalUtil_ = 0.0;
    lastCollectMs_ = 0.0;

    if (!impl_) impl_ = new Impl();
    impl_->ReleaseProcs();
    impl_->adapters.clear();
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

    // ---- 2. 枚举所有真实 GPU ----
    //
    // 与旧实现的三个差别（旧实现是「选中一个就 break」）：
    //   ① 不再因为 AdapterString 为空就跳过 —— 名字改走 QueryDriverDescription，
    //      实在拿不到才用 "GPU N" 占位。这是「核显看不到」的直接修复。
    //   ② 收集全部通过的适配器，不再 break。
    //   ③ 保留 SoftwareDevice 排除（Microsoft Basic Render Driver）。
    //
    // 另加一条旧实现没有的判据：**排除纯显示适配器**（ToDesk / GameViewer 这类）。
    // 实测依据（docs/evidence/gpuinfo_probe15.cpp）：
    //   ToDesk     ADAPTERTYPE=0x142 → RenderSupported=0 DisplaySupported=1 Indirect=1
    //   GameViewer 同上
    //   NVIDIA     ADAPTERTYPE=0x30B → RenderSupported=1 DisplaySupported=1 Indirect=0
    //   BasicRender ADAPTERTYPE=0x105 → RenderSupported=1 SoftwareDevice=1
    // 这三个虚拟显示适配器的 T_ADAPTER 统计与 NVIDIA **逐字段相同**
    // （NodeCount=14 / NbSegments=3 与 DID 同为 PCI\VEN_10DE&DEV_1E84），
    // 也就是说它们不是独立 GPU，而是同一 VidMM 后端的镜像；若收进来，侧栏会
    // 出现 3 个名字不同、数字完全相同的 "GPU"，且总利用率不会变——纯属假项。
    // 判据用 RenderSupported==0（这些适配器根本不做渲染/计算）而不是靠名字猜，
    // 因为名字是可本地化/可改的，位标志是驱动报的。
    //
    // 原版任务管理器的行为可以旁证：它的 GPU 数据源是 PDH 的
    // \GPU Adapter Memory(luid_...)\Dedicated Usage，实测本机该计数器只有
    // **2 个实例**（NVIDIA 与 Basic Render），ToDesk/GameViewer 完全不出现
    // （docs/evidence/gpuinfo_probe16.cpp）。也就是说原版**不列**虚拟显示适配器。
    {
        D3DKMT_ADAPTERINFO_LOCAL raw[16]{};
        D3DKMT_ENUMADAPTERS2_LOCAL ea{};
        ea.NumAdapters = 16;
        ea.pAdapters = raw;

        if (!NtOk(impl_->EnumAdapters2(&ea))) return false;
        const uint32_t count = ea.NumAdapters < 16 ? ea.NumAdapters : 16;

        for (uint32_t i = 0; i < count; ++i) {
            if (impl_->adapters.size() >= Impl::kMaxAdapters) break;

            // 2a. 排除软件渲染器（原有逻辑，保留）。
            // 注意：必须**先判软件设备再判名字**，因为 Basic Render Driver 在
            // 用 DriverDescription 回退后也能读到名字，光看名字会把它当真实 GPU。
            D3DKMT_ADAPTERTYPE_LOCAL at{};
            D3DKMT_QUERYADAPTERINFO_LOCAL qt{};
            qt.hAdapter = raw[i].hAdapter;
            qt.Type = kQaiAdapterType;
            qt.pPrivateDriverData = &at;
            qt.PrivateDriverDataSize = sizeof(at);
            const bool haveType = NtOk(impl_->QueryAdapterInfo(&qt));
            if (haveType && at.SoftwareDevice) continue;

            // 2b. 排除不做渲染的纯显示适配器（虚拟显示器）。
            if (haveType && !at.RenderSupported) continue;

            // 2c. 统计可用性：NodeCount>0 是硬条件（否则没有引擎可采）。
            D3DKMT_QUERYSTATISTICS qa{};
            qa.Type = kStatsAdapter;
            qa.AdapterLuid = raw[i].AdapterLuid;
            if (!NtOk(impl_->QueryStatistics(&qa))) continue;
            uint32_t nodes = qa.QueryResult.AdapterInformation.NodeCount;
            if (nodes == 0) continue;

            Impl::AdapterSlot slot;
            slot.luid = raw[i].AdapterLuid;
            slot.hAdapter = raw[i].hAdapter;
            slot.nodeCount = nodes > kMaxNodes ? kMaxNodes : nodes;
            uint32_t segs = qa.QueryResult.AdapterInformation.NbSegments;
            slot.segmentCount = segs > kMaxSegments ? kMaxSegments : segs;

            // 2d. 取名：ADAPTERREGISTRYINFO → DRIVER_DESCRIPTION → 占位名。
            D3DKMT_ADAPTERREGISTRYINFO_LOCAL reg{};
            D3DKMT_QUERYADAPTERINFO_LOCAL q1{};
            q1.hAdapter = raw[i].hAdapter;
            q1.Type = kQaiAdapterRegistryInfo;
            q1.pPrivateDriverData = &reg;
            q1.PrivateDriverDataSize = sizeof(reg);
            if (NtOk(impl_->QueryAdapterInfo(&q1)) && reg.AdapterString[0] != L'\0') {
                reg.AdapterString[_countof(reg.AdapterString) - 1] = L'\0';
                slot.name = reg.AdapterString;
            }
            if (slot.name.empty()) {
                // Win10 1809+ 的 KMTQAITYPE_DRIVER_DESCRIPTION，实测对本机
                // 全部 4 个适配器都可读（含 NVIDIA 本身）。
                slot.name = QueryDriverDescription(impl_->QueryAdapterInfo, raw[i].hAdapter);
            }
            // 仍为空则留给第 4 步用"过滤后下标"生成占位名 "GPU N"。

            D3DKMT_SEGMENTSIZEINFO_LOCAL ss{};
            D3DKMT_QUERYADAPTERINFO_LOCAL qs{};
            qs.hAdapter = raw[i].hAdapter;
            qs.Type = kQaiGetSegmentSize;
            qs.pPrivateDriverData = &ss;
            qs.PrivateDriverDataSize = sizeof(ss);
            if (NtOk(impl_->QueryAdapterInfo(&qs))) {
                slot.dedicatedTotal = ss.DedicatedVideoMemorySize;
                slot.sharedTotal = ss.SharedSystemMemorySize;
            }

            impl_->adapters.push_back(std::move(slot));
        }
        if (impl_->adapters.empty()) return false;
    }

    // ---- 3. 逐适配器：node 元数据 → 引擎分组 ----
    for (auto& slot : impl_->adapters) {
        slot.groups.clear();
        for (uint32_t n = 0; n < kMaxNodes; ++n) slot.nodeGroup[n] = -1;

        for (uint32_t n = 0; n < slot.nodeCount; ++n) {
            D3DKMT_NODEMETADATA_LOCAL nm{};
            nm.NodeOrdinalAndAdapterIndex = n;   // 物理适配器 0 + node 号
            D3DKMT_QUERYADAPTERINFO_LOCAL q{};
            q.hAdapter = slot.hAdapter;
            q.Type = kQaiNodeMetadata;
            q.pPrivateDriverData = &nm;
            q.PrivateDriverDataSize = sizeof(nm);
            if (!NtOk(impl_->QueryAdapterInfo(&q))) continue;

            int type = static_cast<int>(nm.NodeData.EngineType);
            if (type < 0 || type > 8) type = kEngineOther;

            int gi = -1;
            for (size_t g = 0; g < slot.groups.size(); ++g) {
                if (slot.groups[g].engineType == type) { gi = static_cast<int>(g); break; }
            }
            if (gi < 0) {
                Impl::AdapterSlot::Group grp;
                grp.engineType = type;
                slot.groups.push_back(grp);
                gi = static_cast<int>(slot.groups.size()) - 1;
            }
            slot.groups[gi].nodes.push_back(static_cast<int>(n));
            slot.nodeGroup[n] = gi;
        }

        // 元数据全失败的适配器不编造引擎名，整卡丢弃（和旧实现一致：
        // 没有引擎就无法给 UI 任何可信数字）。
        // 注意这会连带丢弃它的统计槽位，所以这里用 erase 后的迭代器。
        if (slot.groups.empty()) {
            // 标记为待删（nodeCount=0 表示无效），收集完统一删除
            slot.nodeCount = 0;
            continue;
        }

        std::sort(slot.groups.begin(), slot.groups.end(),
                  [](const Impl::AdapterSlot::Group& a,
                     const Impl::AdapterSlot::Group& b) {
                      return EngineDisplayOrder(a.engineType) < EngineDisplayOrder(b.engineType);
                  });
        for (size_t g = 0; g < slot.groups.size(); ++g) {
            for (int n : slot.groups[g].nodes) slot.nodeGroup[n] = static_cast<int>(g);
        }
    }
    impl_->adapters.erase(
        std::remove_if(impl_->adapters.begin(), impl_->adapters.end(),
                       [](const Impl::AdapterSlot& s) { return s.nodeCount == 0; }),
        impl_->adapters.end());
    if (impl_->adapters.empty()) return false;

    // ---- 4. 逐适配器：静态信息 + 构造 GpuAdapterInfo ----
    adapters_.clear();
    adapters_.reserve(impl_->adapters.size());

    // 注册表 Class\{4d36e968-...} 只扫一次，所有适配器共用（本机 4 个子键）。
    // 结构：DriverDesc → {ver, date, qwMem}
    struct RegClassEntry {
        std::wstring desc;
        std::wstring ver;
        std::wstring date;
        uint64_t     qwMem = 0;
    };
    std::vector<RegClassEntry> regEntries;
    {
        static const wchar_t* kDisplayClass =
            L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
        HKEY hClass = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kDisplayClass, 0, KEY_READ, &hClass) == ERROR_SUCCESS) {
            for (int i = 0; i < 32; ++i) {
                wchar_t subName[16];
                swprintf_s(subName, L"%04d", i);
                HKEY hSub = nullptr;
                if (RegOpenKeyExW(hClass, subName, 0, KEY_READ, &hSub) != ERROR_SUCCESS) continue;

                RegClassEntry e;
                const bool okDesc = ReadRegString(hSub, L"DriverDesc", e.desc);
                ReadRegString(hSub, L"DriverVersion", e.ver);
                ReadRegString(hSub, L"DriverDate", e.date);
                ReadRegU64(hSub, L"HardwareInformation.qwMemorySize", e.qwMem);
                RegCloseKey(hSub);

                if (okDesc && !e.desc.empty()) regEntries.push_back(std::move(e));
            }
            RegCloseKey(hClass);
        }
    }

    for (size_t i = 0; i < impl_->adapters.size(); ++i) {
        Impl::AdapterSlot& slot = impl_->adapters[i];

        GpuAdapterInfo info;
        if (slot.name.empty()) {
            // 最后的兜底：编号占位名。用**过滤后的下标**而不是原始枚举下标，
            // 避免出现 "GPU 2" 而侧栏只有一项的错位感。
            wchar_t ph[32];
            swprintf_s(ph, L"GPU %zu", i);
            info.name = ph;
        } else {
            info.name = slot.name;
        }
        info.nodeCount = slot.nodeCount;
        info.segmentCount = slot.segmentCount;
        info.dedicatedTotal = slot.dedicatedTotal;
        info.sharedTotal = slot.sharedTotal;

        // 4a. 物理位置（PCI 总线/设备/功能）
        {
            D3DKMT_ADAPTERADDRESS_LOCAL ad{};
            D3DKMT_QUERYADAPTERINFO_LOCAL q{};
            q.hAdapter = slot.hAdapter;
            q.Type = kQaiAdapterAddress;
            q.pPrivateDriverData = &ad;
            q.PrivateDriverDataSize = sizeof(ad);
            if (NtOk(impl_->QueryAdapterInfo(&q))) {
                info.pciBus = ad.BusNumber;
                info.pciDevice = ad.DeviceNumber;
                info.pciFunction = ad.FunctionNumber;
                wchar_t buf[96];
                swprintf_s(buf, L"PCI 总线 %u，设备 %u，功能 %u",
                           ad.BusNumber, ad.DeviceNumber, ad.FunctionNumber);
                info.physicalLocation = buf;
            }
        }

        // 4b. 注册表：按 DriverDesc 与适配器名不区分大小写匹配。
        // 同名多条时优先取有 qwMemorySize 那条（本机 NVIDIA 有 4 个同名子键）。
        {
            const RegClassEntry* best = nullptr;
            for (const auto& e : regEntries) {
                if (_wcsicmp(e.desc.c_str(), info.name.c_str()) != 0) continue;
                if (!best || e.qwMem > best->qwMem) best = &e;
            }
            if (best) {
                info.driverVersion = best->ver;
                info.driverDate = NormalizeDriverDate(best->date.c_str());
                if (best->qwMem > slot.dedicatedTotal)
                    info.hwReserved = best->qwMem - slot.dedicatedTotal;
            }
        }

        // 4c. 逐适配器的引擎列表（使用率留 0，等第一轮 Collect 填）
        info.engines.reserve(slot.groups.size());
        for (const auto& g : slot.groups) {
            GpuEngineSample s;
            s.engineType = g.engineType;
            s.name = EngineTypeDisplayName(g.engineType);
            s.nodeCount = static_cast<int>(g.nodes.size());
            info.engines.push_back(std::move(s));
        }

        adapters_.push_back(std::move(info));
    }

    // ---- 5. 合并视图：旧调用方只认识「一份引擎列表」 ----
    // 规则：每个引擎类型取**所有适配器里最忙的那张卡**。这与 TotalUtilPercent()
    // 的「取最大不相加」口径一致，也是任务管理器在「GPU 页」上显示的总利用率。
    BuildMergedEngines();
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

        // 建立句柄缓存：只保留有 GPU 记录的进程。
        // 多卡：一个进程可能同时出现在多张卡上（跨卡进程），
        // 所以对每个 pid 逐卡查 PROCESS_ADAPTER，命中任一卡的进程才保留。
        const size_t kAdapterCount = impl_->adapters.size();
        for (size_t i = 0; i < useCount; ++i) {
            const uint32_t pid = usePids[i];
            if (pid == 0) continue;
            // 必须 PROCESS_QUERY_INFORMATION：PROCESS_QUERY_LIMITED_INFORMATION
            // 会让 D3DKMTQueryStatistics 返回 STATUS_INVALID_PARAMETER。
            HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
            if (!h) continue;

            Impl::ProcEntry e;
            e.pid = pid;
            e.handle = h;
            e.adapterMask = 0;

            for (size_t ai = 0; ai < kAdapterCount && ai < 32; ++ai) {
                D3DKMT_QUERYSTATISTICS qa{};
                qa.Type = kStatsProcessAdapter;
                qa.AdapterLuid = impl_->adapters[ai].luid;
                qa.hProcess = h;
                if (!NtOk(impl_->QueryStatistics(&qa))) continue;
                // PROCESS_ADAPTER 的 NodeCount 实测就是该进程参与的 node 数
                // （本机 38 个 GPU 进程全部报 14，等于适配器 NodeCount）。
                // 这里只用它判断"该卡上有没有这个进程"，具体 node 数一律取
                // adapters[ai].nodeCount（单一真值源）。
                if (qa.QueryResult.ProcessAdapterInformation.NodeCount == 0) continue;
                e.adapterMask |= (1u << ai);
            }

            if (e.adapterMask == 0) {
                // 该进程不在任何一张真实 GPU 上有记录，关掉句柄
                CloseHandle(h);
                continue;
            }
            impl_->procs.push_back(e);
        }

        // 逐卡统计"有 GPU 记录的进程数"
        for (size_t ai = 0; ai < impl_->adapters.size(); ++ai) {
            int n = 0;
            for (const auto& p : impl_->procs) {
                if (p.adapterMask & (1u << ai)) ++n;
            }
            impl_->adapters[ai].processCount = n;
            if (ai < adapters_.size()) adapters_[ai].processCount = n;
        }

        // 记基线（逐卡：全局 node + 逐进程 node 求和）
        for (auto& a : impl_->adapters) {
            a.ResetBaseline();
            for (uint32_t n = 0; n < a.nodeCount; ++n) {
                int64_t v = 0;
                if (impl_->QueryNodeGlobal(a, n, v)) a.prevNodeGlobal[n] = v;
            }
        }
        // 逐进程、逐卡累加进程侧基线。
        for (size_t ai = 0; ai < impl_->adapters.size(); ++ai) {
            Impl::AdapterSlot& a = impl_->adapters[ai];
            for (const auto& p : impl_->procs) {
                if (!(p.adapterMask & (1u << ai))) continue;
                for (uint32_t n = 0; n < a.nodeCount; ++n) {
                    int64_t v = 0;
                    if (impl_->QueryProcessNode(a, p.handle, n, v)) a.prevNodeProcSum[n] += v;
                }
            }
        }
        impl_->prevTimestamp = QpcNowSeconds();
        impl_->hasBaseline = true;

        // 顺带读一次显存（每卡 3 个 segment 只要 0.016 ms），第一轮就有占用值
        impl_->RefreshMemoryUsage(&adapters_);

        lastCollectMs_ = (QpcNowSeconds() - t0) * 1000.0;
        return;   // 本轮建基线，不产出使用率（上一轮的值仍可读）
    }

    // 每 60 轮触发一次重新枚举（新启动的 GPU 进程最多约 1 分钟后可见）
    if (!pids && ++impl_->collectTick >= 60) {
        impl_->collectTick = 0;
        impl_->needPidEnum = true;
    }

    // ------------------------------------------------------------------
    // 2. 正常情况下：逐卡查全局 node + 逐进程 node，做差分
    //
    // 【多卡成本】每一轮的 D3DKMT 查询次数 =
    //     Σ_k ( nodeCount_k × 2        // 全局 T_NODE + 逐进程 T_PROCESS_NODE
    //           + segmentCount_k )      // 显存
    // 实测单次 ~0.03 ms（本机 14 node × 2 + 3 segment ≈ 31 次 ≈ 0.9 ms/卡）。
    // 本机只有 1 张真实 GPU 时与旧实现完全一致；2 张卡约 1.8~2 ms，
    // 4 张卡约 4 ms —— 仍在一轮 5 ms 预算内。
    // 适配器数量在 Init 时已被 kMaxAdapters=4 封顶，不存在"虚拟显示器塞满
    // 几十个适配器"导致的线性膨胀。
    // ------------------------------------------------------------------

    const double now = QpcNowSeconds();
    double dt = impl_->hasBaseline ? (now - impl_->prevTimestamp) : 0.0;
    if (dt < 0.0) dt = 0.0;
    const double denom = dt * 1.0e7;   // RunningTime 单位 100ns

    // 顺带清理已退出的进程。实测（docs/evidence/gpuinfo_probe14.cpp）进程退出后
    // D3DKMT PROCESS_ADAPTER / PROCESS_NODE 全部返回 0xC000000D，所以
    // "该进程在所有卡上都查不到" 就是可靠的退出判据，不需要额外的 GetProcessTimes
    // （那 38 次调用实测 p99 达 23.5 ms，是稳态路径里最大的一笔可省开销）。
    // 句柄绑定的是进程对象本身，pid 被复用也不会把旧数据算到新进程头上，
    // 因为句柄指向的仍是已退出那个对象，它的所有查询都会失败。

    // 每卡一份逐进程 node 求和。维度：kMaxAdapters × kMaxNodes（栈上，小）。
    int64_t procSum[Impl::kMaxAdapters][kMaxNodes] = {};
    // 每卡的全局 node 时间
    int64_t globalNode[Impl::kMaxAdapters][kMaxNodes] = {};

    const size_t kAdapterCount = impl_->adapters.size();
    for (size_t ai = 0; ai < kAdapterCount; ++ai) {
        const auto& a = impl_->adapters[ai];
        for (uint32_t n = 0; n < a.nodeCount; ++n) {
            int64_t v = 0;
            if (impl_->QueryNodeGlobal(a, n, v)) globalNode[ai][n] = v;
        }
    }

    size_t writeIdx = 0;
    for (size_t readIdx = 0; readIdx < impl_->procs.size(); ++readIdx) {
        Impl::ProcEntry& p = impl_->procs[readIdx];
        if (!p.handle) continue;

        bool anyAlive = false;
        for (size_t ai = 0; ai < kAdapterCount; ++ai) {
            if (!(p.adapterMask & (1u << ai))) continue;
            const auto& a = impl_->adapters[ai];
            bool anyOnThisCard = false;
            for (uint32_t n = 0; n < a.nodeCount; ++n) {
                int64_t v = 0;
                if (impl_->QueryProcessNode(a, p.handle, n, v)) {
                    procSum[ai][n] += v;
                    anyOnThisCard = true;
                }
            }
            if (anyOnThisCard) anyAlive = true;
            // 该卡上已无记录（进程退出）时清掉这一位，避免掩码长期虚高
            else p.adapterMask &= ~(1u << ai);
        }
        if (!anyAlive) {
            CloseHandle(p.handle);
            p.handle = nullptr;
            continue;
        }
        if (writeIdx != readIdx) impl_->procs[writeIdx] = p;
        ++writeIdx;
    }
    impl_->procs.resize(writeIdx);

    // 逐卡统计"有 GPU 记录的进程数"
    for (size_t ai = 0; ai < kAdapterCount; ++ai) {
        int n = 0;
        for (const auto& p : impl_->procs) {
            if (p.adapterMask & (1u << ai)) ++n;
        }
        impl_->adapters[ai].processCount = n;
        if (ai < adapters_.size()) adapters_[ai].processCount = n;
    }

    // ---- 逐卡：每引擎使用率 ----
    for (size_t ai = 0; ai < kAdapterCount && ai < adapters_.size(); ++ai) {
        Impl::AdapterSlot& a = impl_->adapters[ai];
        GpuAdapterInfo& info = adapters_[ai];

        std::vector<int64_t> groupDelta(a.groups.size(), 0);

        for (uint32_t n = 0; n < a.nodeCount; ++n) {
            const int gi = a.nodeGroup[n];
            if (gi < 0) continue;

            int64_t dGlobal = globalNode[ai][n] - a.prevNodeGlobal[n];
            int64_t dProc   = procSum[ai][n] - a.prevNodeProcSum[n];
            if (dGlobal < 0) dGlobal = 0;
            if (dProc < 0) dProc = 0;
            // 见文件头第 3 条：本机 3D 节点的全局计数恒为 0，必须取两者较大值
            const int64_t d = dGlobal > dProc ? dGlobal : dProc;
            groupDelta[gi] += d;
        }

        // 写回该卡的引擎列表
        if (info.engines.size() == a.groups.size()) {
            for (size_t g = 0; g < a.groups.size(); ++g) {
                info.engines[g].runningDelta = static_cast<uint64_t>(groupDelta[g]);
                double pct = (denom > 0.0)
                    ? static_cast<double>(groupDelta[g]) / denom * 100.0 : 0.0;
                if (pct < 0.0) pct = 0.0;
                if (pct > 100.0) pct = 100.0;
                info.engines[g].utilPercent = pct;
            }
        }

        // 本卡总使用率 = 本卡最忙的引擎
        double busiest = 0.0;
        for (const auto& e : info.engines) {
            if (e.utilPercent > busiest) busiest = e.utilPercent;
        }
        info.totalUtilPercent = busiest;

        // 保存本轮原始值
        for (uint32_t n = 0; n < a.nodeCount; ++n) {
            a.prevNodeGlobal[n] = globalNode[ai][n];
            a.prevNodeProcSum[n] = procSum[ai][n];
        }
    }
    impl_->prevTimestamp = now;
    impl_->hasBaseline = true;

    // ---- 合并视图 + 总使用率 ----
    // 合并视图必须在逐卡写完引擎值之后重建（BuildMergedEngines 是纯聚合）。
    BuildMergedEngines();
    // 总使用率 = 所有卡里最忙的引擎（不相加：利用率是时间片占比）
    double overall = 0.0;
    for (const auto& a : adapters_) {
        if (a.totalUtilPercent > overall) overall = a.totalUtilPercent;
    }
    totalUtil_ = overall;

    // ---- 显存占用（segment resident，见文件头第 4 条）----
    impl_->RefreshMemoryUsage(&adapters_);

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
    if (!available_ || adapters_.empty()) return std::wstring();

    // 只做一次。实测 480~1000 ms（瓶颈是加载 NVIDIA 用户态驱动），
    // 绝不能放进 Init/Collect。这里用 LoadLibrary + GetProcAddress，
    // 不引入 d3d11.lib 链接依赖。
    //
    // 【多卡限制】只探测**第一张卡**。逐卡探测需要 N × 500~1000 ms
    // （双卡就是 1~2 s），而 D3D11CreateDevice(nullptr, HARDWARE) 只会选中
    // 系统默认卡，无法顺手拿到第二张卡的 feature level；要逐卡探测必须先用
    // DXGI 枚举出 IDXGIAdapter 再按 LUID 匹配，代码量与收益不成比例。
    // 因此第二张卡起的 directXVersion 保持空串，UI 显示 "—"（不编造）。
    if (!adapters_[0].directXVersion.empty()) return adapters_[0].directXVersion;

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

    adapters_[0].directXVersion = ver;
    return adapters_[0].directXVersion;
}

} // namespace tmp
