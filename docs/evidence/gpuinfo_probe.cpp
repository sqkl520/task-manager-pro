// gpuinfo_probe.cpp - 系统级 GPU 信息数据源可行性验证
//
// 目标（全部实测，不假设）：
//   1) D3DKMT_QUERYSTATISTICS NODE 统计 -> 每引擎 RunningTime / 使用率
//   2) KMTQAITYPE_NODEMETADATA        -> 每个 node 的引擎类型 + 友好名
//   3) KMTQAITYPE_GETSEGMENTSIZE      -> 专用/共享显存总量
//   4) KMTQAITYPE_ADAPTERADDRESS      -> PCI 位置
//   5) T_SEGMENT CommitLimit          -> 段容量（显存总量交叉校验）
//   6) 注册表 DriverVersion/DriverDate
//   7) DXGI_ADAPTER_DESC              -> Dedicated/Shared 总量交叉校验
//   8) D3D11 feature level            -> DirectX 版本
//   9) PDH \GPU Engine(*) 对照 + 成本  -> 判定 PDH 是否可用
//  10) 采集总耗时
//
// 编译：_probe/build_gpuinfo_probe.ps1（独立 console exe，不进主项目）
#include <windows.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <map>
#include <algorithm>

// SDK 头仅用于交叉校验结构体布局（交付代码不 include 它）
#include <d3dkmthk.h>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")

// ===========================================================================
// 自己定义的结构体（将来进入 include/gpuinfo.h 的写法），namespace 避免冲突
// ===========================================================================
namespace mygpu {

typedef uint32_t KMT_HANDLE;

struct AdapterInfoLocal {
    KMT_HANDLE hAdapter;
    LUID       AdapterLuid;
    uint32_t   NumOfSources;
    BOOL       bPrecisePresentRegionsPreferred;
};

struct EnumAdapters2Local {
    uint32_t          NumAdapters;
    AdapterInfoLocal* pAdapters;
};

struct QueryAdapterInfoLocal {
    KMT_HANDLE hAdapter;
    uint32_t   Type;
    void*      pPrivateDriverData;
    uint32_t   PrivateDriverDataSize;
};

struct AdapterRegistryInfoLocal {
    wchar_t AdapterString[MAX_PATH];
    wchar_t BiosString[MAX_PATH];
    wchar_t DacType[MAX_PATH];
    wchar_t ChipType[MAX_PATH];
};

struct AdapterAddressLocal {
    uint32_t BusNumber;
    uint32_t DeviceNumber;
    uint32_t FunctionNumber;
};

struct SegmentSizeInfoLocal {
    uint64_t DedicatedVideoMemorySize;
    uint64_t DedicatedSystemMemorySize;
    uint64_t SharedSystemMemorySize;
};

struct NodeMetadataLocal {
    int32_t EngineType;                 // DXGK_ENGINE_TYPE
    wchar_t FriendlyName[32];           // DXGK_MAX_METADATA_NAME_LENGTH
    uint32_t Flags;
    uint8_t  GpuMmuSupported;
    uint8_t  IoMmuSupported;
};

struct NodeMetadataQueryLocal {
    uint32_t NodeOrdinalAndAdapterIndex;  // hi word = phys adapter, lo word = node ordinal
    NodeMetadataLocal NodeData;
};

// --- 统计结构（与 gpuproc.cpp 相同的自写布局）---
struct Counter { uint32_t Count; uint64_t Bytes; };
struct DmaPacketTypeInfo { uint32_t submited, completed, preempted, faulted; };
struct QueuePacketTypeInfo { uint32_t submited, completed; };
struct PacketInformation {
    QueuePacketTypeInfo QueuePacket[8];
    DmaPacketTypeInfo   DmaPacket[4];
};
struct PreemptionInformation { uint32_t PreemptionCounter[16]; };

struct ProcessNodeInformation {
    int64_t  RunningTime;
    uint32_t ContextSwitch;
    PreemptionInformation PreemptionStatistics;
    PacketInformation     PacketStatistics;
    uint64_t Reserved[8];
};

struct NodeInformation {
    ProcessNodeInformation GlobalInformation;
    ProcessNodeInformation SystemInformation;
    uint8_t Tail[0x48];
};

struct MemoryStats { uint64_t TotalBytesEvicted; uint32_t AllocsCommitted, AllocsResident; };

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

union QueryResultLocal {
    AdapterInformation AdapterInformation;
    SegmentInformation SegmentInformation;
    NodeInformation    NodeInformation;
};

struct QueryStatisticsLocal {
    uint32_t Type;
    LUID     AdapterLuid;
    HANDLE   hProcess;
    QueryResultLocal QueryResult;
    union {
        uint32_t QuerySegmentId;
        uint32_t QueryNodeId;
        uint32_t QueryPhysicalAdapterIndex;
    };
};

enum QType {
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
    T_PHYSICAL_ADAPTER = 10,
};

constexpr uint32_t kQAIGetSegmentSize      = 3;
constexpr uint32_t kQAIAdapterAddress      = 6;
constexpr uint32_t kQAIAdapterRegistryInfo = 8;
constexpr uint32_t kQAIDriverVersion       = 13;
constexpr uint32_t kQAINodeMetadata        = 25;
constexpr uint32_t kQAIPhysicalAdapterCount = 30;

using PFN_EnumAdapters2  = NTSTATUS (NTAPI*)(EnumAdapters2Local*);
using PFN_QueryAdapterInfo = NTSTATUS (NTAPI*)(QueryAdapterInfoLocal*);
using PFN_QueryStatistics = NTSTATUS (NTAPI*)(const QueryStatisticsLocal*);
using PFN_CloseAdapter    = NTSTATUS (NTAPI*)(const void*);

} // namespace mygpu

// ===========================================================================
// 布局交叉校验：自写结构 vs SDK
// ===========================================================================
#define CHK(SZ, sdk) printf("  %-52s self=%5zu sdk=%5zu %s\n", #SZ, sizeof(mygpu::SZ), sizeof(sdk), \
    sizeof(mygpu::SZ) == sizeof(sdk) ? "OK" : "MISMATCH")
#define CHKOFF(field, mine, sdktype, sdkfield) printf("  %-52s self=0x%03zx sdk=0x%03zx %s\n", #field, \
    offsetof(mine, field), offsetof(sdktype, sdkfield), \
    offsetof(mine, field) == offsetof(sdktype, sdkfield) ? "OK" : "MISMATCH")

static double NowMs() {
    static LARGE_INTEGER f{};
    static bool init = false;
    if (!init) { QueryPerformanceFrequency(&f); init = true; }
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static const wchar_t* EngineName(int type) {
    switch (type) {
        case 0: return L"Other";
        case 1: return L"3D";
        case 2: return L"Video Decode";
        case 3: return L"Video Encode";
        case 4: return L"Video Processing";
        case 5: return L"Scene Assembly";
        case 6: return L"Copy";
        case 7: return L"Overlay";
        case 8: return L"Crypto";
        default: return L"?";
    }
}

int main() {
    printf("=== 系统级 GPU 信息数据源探针 ===\n\n");

    // ------------------------------------------------------------------
    // 0) 结构体布局交叉校验
    // ------------------------------------------------------------------
    printf("--- 0) 结构体布局（自写 vs SDK d3dkmthk.h）---\n");
    CHK(QueryStatisticsLocal, D3DKMT_QUERYSTATISTICS);
    printf("  %-52s self=0x%03zx sdk=0x%03zx %s\n", "QueryResult offset",
           offsetof(mygpu::QueryStatisticsLocal, QueryResult),
           offsetof(D3DKMT_QUERYSTATISTICS, QueryResult),
           offsetof(mygpu::QueryStatisticsLocal, QueryResult) == offsetof(D3DKMT_QUERYSTATISTICS, QueryResult) ? "OK" : "MISMATCH");
    CHK(NodeInformation, D3DKMT_QUERYSTATISTICS_NODE_INFORMATION);
    CHKOFF(GlobalInformation, mygpu::NodeInformation, D3DKMT_QUERYSTATISTICS_NODE_INFORMATION, GlobalInformation);
    CHKOFF(SystemInformation, mygpu::NodeInformation, D3DKMT_QUERYSTATISTICS_NODE_INFORMATION, SystemInformation);
    CHKOFF(RunningTime, mygpu::ProcessNodeInformation, D3DKMT_QUERYSTATISTICS_PROCESS_NODE_INFORMATION, RunningTime);
    CHK(SegmentInformation, D3DKMT_QUERYSTATISTICS_SEGMENT_INFORMATION);
    CHK(AdapterInformation, D3DKMT_QUERYSTATISTICS_ADAPTER_INFORMATION);
    CHK(AdapterRegistryInfoLocal, D3DKMT_ADAPTERREGISTRYINFO);
    CHK(SegmentSizeInfoLocal, D3DKMT_SEGMENTSIZEINFO);
    CHK(AdapterAddressLocal, D3DKMT_ADAPTERADDRESS);
    CHK(NodeMetadataLocal, DXGK_NODEMETADATA);
    CHK(NodeMetadataQueryLocal, D3DKMT_NODEMETADATA);
    printf("  sizeof(NodePerfData)=%zu (SDK %zu) 在 NodeInformation 内偏移=0x%03zx (SDK 0x%03zx)\n",
           sizeof(D3DKMT_NODE_PERFDATA), sizeof(D3DKMT_NODE_PERFDATA),
           offsetof(mygpu::NodeInformation, Tail[0]),
           offsetof(D3DKMT_QUERYSTATISTICS_NODE_INFORMATION, NodePerfData));
    printf("  DXGK_ENGINE_TYPE 数值: OTHER=%d 3D=%d DECODE=%d ENCODE=%d PROCESSING=%d SCENE=%d COPY=%d\n",
           (int)DXGK_ENGINE_TYPE_OTHER, (int)DXGK_ENGINE_TYPE_3D,
           (int)DXGK_ENGINE_TYPE_VIDEO_DECODE, (int)DXGK_ENGINE_TYPE_VIDEO_ENCODE,
           (int)DXGK_ENGINE_TYPE_VIDEO_PROCESSING, (int)DXGK_ENGINE_TYPE_SCENE_ASSEMBLY,
           (int)DXGK_ENGINE_TYPE_COPY);

    // ------------------------------------------------------------------
    // 1) 加载 gdi32 / 枚举适配器
    // ------------------------------------------------------------------
    printf("\n--- 1) 适配器枚举 ---\n");
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    if (!gdi) { printf("FATAL: gdi32\n"); return 1; }
    auto EnumAdapters2  = (mygpu::PFN_EnumAdapters2)GetProcAddress(gdi, "D3DKMTEnumAdapters2");
    auto QueryAdapterInfo = (mygpu::PFN_QueryAdapterInfo)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
    auto QueryStatistics = (mygpu::PFN_QueryStatistics)GetProcAddress(gdi, "D3DKMTQueryStatistics");
    auto CloseAdapter    = (mygpu::PFN_CloseAdapter)GetProcAddress(gdi, "D3DKMTCloseAdapter");
    printf("  exports: EnumAdapters2=%p QueryAdapterInfo=%p QueryStatistics=%p CloseAdapter=%p\n",
           (void*)EnumAdapters2, (void*)QueryAdapterInfo, (void*)QueryStatistics, (void*)CloseAdapter);
    if (!EnumAdapters2 || !QueryAdapterInfo || !QueryStatistics) { printf("FATAL: exports\n"); return 1; }

    mygpu::AdapterInfoLocal raw[16]{};
    mygpu::EnumAdapters2Local ea{};
    ea.NumAdapters = 16;
    ea.pAdapters = raw;
    NTSTATUS st = EnumAdapters2(&ea);
    printf("  EnumAdapters2 -> 0x%08lX, NumAdapters=%u\n", (unsigned long)st, ea.NumAdapters);

    int realIdx = -1;
    for (uint32_t i = 0; i < ea.NumAdapters && i < 16; ++i) {
        mygpu::AdapterRegistryInfoLocal reg{};
        mygpu::QueryAdapterInfoLocal q{ raw[i].hAdapter, mygpu::kQAIAdapterRegistryInfo, &reg, sizeof(reg) };
        NTSTATUS rs = QueryAdapterInfo(&q);
        printf("  [%u] handle=0x%08X luid=0x%08X:0x%08X regStatus=0x%08lX name=\"%ls\" bios=\"%ls\"\n",
               i, raw[i].hAdapter, raw[i].AdapterLuid.HighPart, raw[i].AdapterLuid.LowPart,
               (unsigned long)rs, rs >= 0 ? reg.AdapterString : L"", rs >= 0 ? reg.BiosString : L"");

        mygpu::AdapterInformation ai{};
        mygpu::QueryStatisticsLocal qa{};
        qa.Type = mygpu::T_ADAPTER;
        qa.AdapterLuid = raw[i].AdapterLuid;
        NTSTATUS as = QueryStatistics(&qa);
        if (as >= 0) {
            printf("       ADAPTER: NbSegments=%u NodeCount=%u VidPnSourceCount=%u\n",
                   qa.QueryResult.AdapterInformation.NbSegments,
                   qa.QueryResult.AdapterInformation.NodeCount,
                   qa.QueryResult.AdapterInformation.VidPnSourceCount);
            if (qa.QueryResult.AdapterInformation.NodeCount > 0 && realIdx < 0 && rs >= 0)
                realIdx = (int)i;
        }
    }
    if (realIdx < 0) { printf("FATAL: no usable adapter\n"); return 1; }
    LUID luid = raw[realIdx].AdapterLuid;
    mygpu::KMT_HANDLE hAd = raw[realIdx].hAdapter;
    printf("  -> 选中适配器 [%d] luid=0x%08X:0x%08X\n", realIdx, luid.HighPart, luid.LowPart);

    // ------------------------------------------------------------------
    // 2) 适配器静态信息
    // ------------------------------------------------------------------
    printf("\n--- 2) 适配器静态信息 ---\n");
    {
        mygpu::SegmentSizeInfoLocal ss{};
        mygpu::QueryAdapterInfoLocal q{ hAd, mygpu::kQAIGetSegmentSize, &ss, sizeof(ss) };
        NTSTATUS s = QueryAdapterInfo(&q);
        printf("  GETSEGMENTSIZE st=0x%08lX dedicated=%.2f MB (%llu) dedSys=%.2f MB shared=%.2f MB (%llu)\n",
               (unsigned long)s, ss.DedicatedVideoMemorySize / 1048576.0,
               (unsigned long long)ss.DedicatedVideoMemorySize,
               ss.DedicatedSystemMemorySize / 1048576.0,
               ss.SharedSystemMemorySize / 1048576.0,
               (unsigned long long)ss.SharedSystemMemorySize);

        mygpu::AdapterAddressLocal ad{};
        mygpu::QueryAdapterInfoLocal q2{ hAd, mygpu::kQAIAdapterAddress, &ad, sizeof(ad) };
        NTSTATUS s2 = QueryAdapterInfo(&q2);
        printf("  ADAPTERADDRESS st=0x%08lX bus=%u device=%u function=%u\n",
               (unsigned long)s2, ad.BusNumber, ad.DeviceNumber, ad.FunctionNumber);

        uint32_t dv = 0;
        mygpu::QueryAdapterInfoLocal q3{ hAd, mygpu::kQAIDriverVersion, &dv, sizeof(dv) };
        NTSTATUS s3 = QueryAdapterInfo(&q3);
        printf("  DRIVERVERSION st=0x%08lX value=0x%08X (%u)\n", (unsigned long)s3, dv, dv);

        uint32_t pac = 0;
        mygpu::QueryAdapterInfoLocal q4{ hAd, mygpu::kQAIPhysicalAdapterCount, &pac, sizeof(pac) };
        NTSTATUS s4 = QueryAdapterInfo(&q4);
        printf("  PHYSICALADAPTERCOUNT st=0x%08lX count=%u\n", (unsigned long)s4, pac);
    }

    // ------------------------------------------------------------------
    // 3) 每 node 引擎元数据
    // ------------------------------------------------------------------
    printf("\n--- 3) NODEMETADATA（每 node 引擎类型/友好名）---\n");
    mygpu::QueryStatisticsLocal qa{};
    qa.Type = mygpu::T_ADAPTER;
    qa.AdapterLuid = luid;
    if (QueryStatistics(&qa) < 0) { printf("FATAL: adapter stats\n"); return 1; }
    uint32_t nodeCount = qa.QueryResult.AdapterInformation.NodeCount;
    uint32_t nbSegments = qa.QueryResult.AdapterInformation.NbSegments;
    printf("  NodeCount=%u NbSegments=%u\n", nodeCount, nbSegments);
    std::vector<int> nodeType(nodeCount, -1);
    std::vector<std::wstring> nodeName(nodeCount);
    int queriesOk = 0;
    for (uint32_t n = 0; n < nodeCount; ++n) {
        mygpu::NodeMetadataQueryLocal nm{};
        nm.NodeOrdinalAndAdapterIndex = (0u << 16) | n;   // phys adapter 0
        mygpu::QueryAdapterInfoLocal q{ hAd, mygpu::kQAINodeMetadata, &nm, sizeof(nm) };
        NTSTATUS s = QueryAdapterInfo(&q);
        if (s < 0) { printf("  node %2u: NODEMETADATA FAILED 0x%08lX\n", n, (unsigned long)s); continue; }
        ++queriesOk;
        nodeType[n] = (int)nm.NodeData.EngineType;
        nodeName[n] = nm.NodeData.FriendlyName;
        printf("  node %2u: EngineType=%d (%ls) FriendlyName=\"%ls\" mmu=%u iommu=%u\n",
               n, (int)nm.NodeData.EngineType,
               EngineName((int)nm.NodeData.EngineType),
               nm.NodeData.FriendlyName,
               (unsigned)nm.NodeData.GpuMmuSupported, (unsigned)nm.NodeData.IoMmuSupported);
    }
    printf("  nodemetadata ok=%d/%u\n", queriesOk, nodeCount);

    // ------------------------------------------------------------------
    // 4) 每段容量（交叉校验显存总量）
    // ------------------------------------------------------------------
    printf("\n--- 4) SEGMENT 容量 ---\n");
    for (uint32_t s = 0; s < nbSegments; ++s) {
        mygpu::QueryStatisticsLocal qs{};
        qs.Type = mygpu::T_SEGMENT;
        qs.AdapterLuid = luid;
        qs.QuerySegmentId = s;
        NTSTATUS sst = QueryStatistics(&qs);
        if (sst < 0) { printf("  segment %u: FAILED 0x%08lX\n", s, (unsigned long)sst); continue; }
        const auto& si = qs.QueryResult.SegmentInformation;
        printf("  segment %u: CommitLimit=%.2f MB committed=%.2f MB resident=%.2f MB aperture=%u segProps=0x%llX sysMemEnd=0x%llX\n",
               s, si.CommitLimit / 1048576.0, si.BytesCommitted / 1048576.0,
               si.BytesResident / 1048576.0, si.Aperture,
               (unsigned long long)si.SegmentProperties,
               (unsigned long long)si.SystemMemoryEndAddress);
    }

    // ------------------------------------------------------------------
    // 5) 注册表 驱动版本/日期 + 显存（对比 qwMemorySize）
    // ------------------------------------------------------------------
    printf("\n--- 5) 注册表适配器信息 ---\n");
    {
        const wchar_t* base = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
        HKEY hClass = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, base, 0, KEY_READ, &hClass) == ERROR_SUCCESS) {
            for (int i = 0; i < 16; ++i) {
                wchar_t sub[8];
                swprintf_s(sub, L"%04d", i);
                HKEY hSub = nullptr;
                if (RegOpenKeyExW(hClass, sub, 0, KEY_READ, &hSub) != ERROR_SUCCESS) continue;
                wchar_t desc[256] = L"", ver[64] = L"", date[64] = L"";
                DWORD sz = sizeof(desc);
                RegQueryValueExW(hSub, L"DriverDesc", nullptr, nullptr, (LPBYTE)desc, &sz);
                sz = sizeof(ver);
                RegQueryValueExW(hSub, L"DriverVersion", nullptr, nullptr, (LPBYTE)ver, &sz);
                sz = sizeof(date);
                RegQueryValueExW(hSub, L"DriverDate", nullptr, nullptr, (LPBYTE)date, &sz);
                uint64_t qw = 0;
                sz = sizeof(qw);
                RegQueryValueExW(hSub, L"HardwareInformation.qwMemorySize", nullptr, nullptr, (LPBYTE)&qw, &sz);
                DWORD mem32 = 0;
                sz = sizeof(mem32);
                RegQueryValueExW(hSub, L"HardwareInformation.MemorySize", nullptr, nullptr, (LPBYTE)&mem32, &sz);
                wchar_t loc[512] = L"";
                sz = sizeof(loc);
                RegQueryValueExW(hSub, L"LocationInformation", nullptr, nullptr, (LPBYTE)loc, &sz);
                if (desc[0])
                    printf("  %ls: desc=\"%ls\" ver=%ls date=%ls qwMem=%llu (%.2f GB) mem32=%u loc=\"%ls\"\n",
                           sub, desc, ver, date, (unsigned long long)qw, qw / 1073741824.0, mem32, loc);
                if (hSub) RegCloseKey(hSub);
            }
            RegCloseKey(hClass);
        } else {
            printf("  RegOpenKeyEx(display class) FAILED err=%lu\n", GetLastError());
        }
    }

    // ------------------------------------------------------------------
    // 6) DXGI 适配器描述
    // ------------------------------------------------------------------
    printf("\n--- 6) DXGI_ADAPTER_DESC ---\n");
    {
        IDXGIFactory1* fac = nullptr;
        HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac);
        printf("  CreateDXGIFactory1 hr=0x%08lX\n", (unsigned long)hr);
        if (SUCCEEDED(hr)) {
            for (UINT i = 0;; ++i) {
                IDXGIAdapter1* a = nullptr;
                if (fac->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
                if (!a) break;
                DXGI_ADAPTER_DESC d{};
                if (SUCCEEDED(a->GetDesc(&d))) {
                    printf("  [%u] \"%ls\" vendor=0x%04X device=0x%04X sub=0x%08X rev=%u luid=0x%08X:0x%08X\n"
                           "      DedicatedVideoMemory=%llu (%.2f MB) DedicatedSystemMemory=%llu SharedSystemMemory=%llu (%.2f MB)\n",
                           i, d.Description, d.VendorId, d.DeviceId, d.SubSysId, d.Revision,
                           d.AdapterLuid.HighPart, d.AdapterLuid.LowPart,
                           (unsigned long long)d.DedicatedVideoMemory, d.DedicatedVideoMemory / 1048576.0,
                           (unsigned long long)d.DedicatedSystemMemory,
                           (unsigned long long)d.SharedSystemMemory, d.SharedSystemMemory / 1048576.0);
                }
                a->Release();
            }
            fac->Release();
        }
    }

    // ------------------------------------------------------------------
    // 7) D3D11 feature level -> DirectX 版本
    // ------------------------------------------------------------------
    printf("\n--- 7) D3D11 feature level ---\n");
    {
        D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
        };
        D3D_FEATURE_LEVEL got{};
        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        double t0 = NowMs();
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                       levels, _countof(levels), D3D11_SDK_VERSION,
                                       &dev, &got, &ctx);
        double dt = NowMs() - t0;
        printf("  D3D11CreateDevice hr=0x%08lX featureLevel=0x%04X (%.2f ms)\n",
               (unsigned long)hr, (unsigned)got, dt);
        if (SUCCEEDED(hr)) {
            IDXGIDevice* dxdev = nullptr;
            if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxdev))) {
                IDXGIAdapter* adp = nullptr;
                if (SUCCEEDED(dxdev->GetAdapter(&adp))) {
                    DXGI_ADAPTER_DESC d{};
                    adp->GetDesc(&d);
                    printf("    设备所在适配器: \"%ls\" luid=0x%08X:0x%08X\n",
                           d.Description, d.AdapterLuid.HighPart, d.AdapterLuid.LowPart);
                    adp->Release();
                }
                dxdev->Release();
            }
            if (ctx) ctx->Release();
            if (dev) dev->Release();
        }
    }

    // ------------------------------------------------------------------
    // 8) 每引擎使用率（T_NODE 差分）+ 与 PDH 对照
    // ------------------------------------------------------------------
    printf("\n--- 8) 每引擎使用率: D3DKMT NODE 差分 ---\n");
    std::vector<int64_t> rt0(nodeCount, 0), rt1(nodeCount, 0);
    auto SampleNodes = [&](std::vector<int64_t>& out, double* ms) {
        double t0 = NowMs();
        for (uint32_t n = 0; n < nodeCount; ++n) {
            mygpu::QueryStatisticsLocal qn{};
            qn.Type = mygpu::T_NODE;
            qn.AdapterLuid = luid;
            qn.QueryNodeId = n;
            if (QueryStatistics(&qn) < 0) { out[n] = 0; continue; }
            out[n] = qn.QueryResult.NodeInformation.GlobalInformation.RunningTime;
        }
        *ms = NowMs() - t0;
    };
    double s1ms = 0, s2ms = 0;
    SampleNodes(rt0, &s1ms);
    double w1 = NowMs();
    Sleep(1000);
    SampleNodes(rt1, &s2ms);
    double w2 = NowMs();
    double dtSec = (w2 - w1) / 1000.0;
    printf("  采样耗时: round1=%.4f ms round2=%.4f ms  walldelta=%.4f s\n", s1ms, s2ms, dtSec);

    struct Agg { int type; std::wstring name; std::vector<int> nodes; int64_t delta; double util; };
    std::map<int, Agg> byType;
    for (uint32_t n = 0; n < nodeCount; ++n) {
        int64_t d = rt1[n] - rt0[n];
        if (d < 0) d = 0;
        double u = dtSec > 0 ? (double)d / (dtSec * 1e7) * 100.0 : 0.0;
        printf("    node %2u %-16ls delta=%12lld  util=%7.3f%%\n",
               n, EngineName(nodeType[n]), (long long)d, u);
        Agg& a = byType[nodeType[n]];
        a.type = nodeType[n];
        a.name = EngineName(nodeType[n]);
        a.nodes.push_back((int)n);
        a.delta += d;
        a.util += u;
    }
    printf("  --- 按引擎类型聚合 ---\n");
    double maxUtil = 0.0;
    for (auto& kv : byType) {
        printf("    %-16ls nodes=%zu delta=%12lld util=%7.3f%%\n",
               kv.second.name.c_str(), kv.second.nodes.size(),
               (long long)kv.second.delta, kv.second.util);
        if (kv.second.util > maxUtil) maxUtil = kv.second.util;
    }
    printf("  总 GPU 使用率（最忙引擎类型）= %.3f%%\n", maxUtil);

    // --- PDH 对照 ---
    printf("\n--- 9) PDH \\GPU Engine(*) 对照（按 LUID 过滤 + 按引擎类型聚合）---\n");
    {
        wchar_t luidPattern[128];
        swprintf_s(luidPattern, L"\\GPU Engine(*luid_0x%08X_0x%08X*)\\Utilization Percentage",
                   luid.HighPart, luid.LowPart);
        wchar_t firstPath[MAX_PATH * 2];
        swprintf_s(firstPath, L"\\GPU Engine(pid_*_luid_0x%08X_0x%08X*)\\Utilization Percentage",
                   luid.HighPart, luid.LowPart);
        printf("  通配路径: %ls\n", firstPath);

        // 先只做 expand 计时
        double te0 = NowMs();
        DWORD sz = 0;
        PDH_STATUS ps = PdhExpandWildCardPathW(nullptr, firstPath, nullptr, &sz, 0);
        double te1 = NowMs();
        printf("  ExpandWildCardPath(1st) st=0x%08lX sz=%lu %.3f ms\n", (unsigned long)ps, sz, te1 - te0);
        std::vector<wchar_t> buf(sz + 16);
        DWORD sz2 = (DWORD)buf.size();
        ps = PdhExpandWildCardPathW(nullptr, firstPath, buf.data(), &sz2, 0);
        double te2 = NowMs();
        int instCount = 0;
        std::vector<std::wstring> insts;
        for (size_t i = 0; i < buf.size() && buf[i];) {
            size_t l = wcslen(&buf[i]);
            std::wstring full(&buf[i], l);
            size_t lp = full.find(L'('), rp = full.rfind(L')');
            if (lp != std::wstring::npos && rp > lp) { insts.push_back(full.substr(lp + 1, rp - lp - 1)); ++instCount; }
            i += l + 1;
        }
        printf("  ExpandWildCardPath(2nd) st=0x%08lX instances=%d %.3f ms\n", (unsigned long)ps, instCount, te2 - te1);

        // 打开查询，逐实例加counter，计时
        PDH_HQUERY hq = nullptr;
        double ta0 = NowMs();
        if (PdhOpenQueryW(nullptr, 0, &hq) == ERROR_SUCCESS) {
            int added = 0;
            for (const auto& in : insts) {
                std::wstring path = L"\\GPU Engine(" + in + L")\\Utilization Percentage";
                PDH_HCOUNTER hc = nullptr;
                if (PdhAddEnglishCounterW(hq, path.c_str(), 0, &hc) == ERROR_SUCCESS) ++added;
            }
            double ta1 = NowMs();
            PdhCollectQueryData(hq);
            double ta2 = NowMs();
            Sleep(1000);
            PdhCollectQueryData(hq);
            double ta3 = NowMs();
            printf("  AddCounter(%d 个) %.3f ms  collect#1 %.3f ms  collect#2 %.3f ms  合计 %.3f ms\n",
                   added, ta1 - ta0, ta2 - ta1, ta3 - ta2, (ta3 - ta0));

            // 读阵列
            std::map<std::wstring, double> pdhByType;
            double tr0 = NowMs();
            PDH_FMT_COUNTERVALUE_ITEM_W* items = nullptr;
            DWORD ibuf = 0, icnt = 0;
            if (PdhGetFormattedCounterArrayW((PDH_HCOUNTER)nullptr, PDH_FMT_DOUBLE, &ibuf, &icnt, nullptr) == PDH_INVALID_ARGUMENT) {}
            // 正确方式：按实例名找 counter 太麻烦，这里直接用 ExpandPath 顺序对应：
            // 用 PdhGetFormattedCounterArray 需要 PDH_HCOUNTER，故逐个 GetFormattedCounterValue。
            std::map<std::wstring, double> utilByType;
            int read = 0;
            for (const auto& in : insts) {
                std::wstring path = L"\\GPU Engine(" + in + L")\\Utilization Percentage";
                PDH_HCOUNTER hc = nullptr;
                if (PdhAddEnglishCounterW(hq, path.c_str(), 0, &hc) != ERROR_SUCCESS) continue;
                PDH_FMT_COUNTERVALUE v{};
                if (PdhGetFormattedCounterValue(hc, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS) {
                    std::wstring t = in;
                    size_t p = t.find(L"engtype_");
                    if (p != std::wstring::npos) t = t.substr(p + 8);
                    utilByType[t] += v.doubleValue;
                    ++read;
                }
                PdhRemoveCounter(hc);
            }
            double tr1 = NowMs();
            printf("  逐实例读值(%d/%d) %.3f ms\n", read, instCount, tr1 - tr0);
            for (auto& kv : utilByType)
                printf("    PDH engtype %-16ls = %7.3f%%\n", kv.first.c_str(), kv.second);
            (void)items; (void)ibuf; (void)icnt; (void)pdhByType;
            PdhCloseQuery(hq);
        } else {
            printf("  PdhOpenQuery FAILED\n");
        }
    }

    // ------------------------------------------------------------------
    // 10) 适配器内存 PDH 成本
    // ------------------------------------------------------------------
    printf("\n--- 10) PDH \\GPU Adapter Memory(*) 成本 ---\n");
    {
        const wchar_t* paths[] = {
            L"\\GPU Adapter Memory(*)\\Dedicated Usage",
            L"\\GPU Adapter Memory(*)\\Shared Usage",
            L"\\GPU Adapter Memory(*)\\Total Committed",
        };
        for (auto p : paths) {
            PDH_HQUERY hq = nullptr;
            double t0 = NowMs();
            if (PdhOpenQueryW(nullptr, 0, &hq) != ERROR_SUCCESS) { printf("  %ls: open fail\n", p); continue; }
            PDH_HCOUNTER hc = nullptr;
            PDH_STATUS s = PdhAddEnglishCounterW(hq, p, 0, &hc);
            PdhCollectQueryData(hq);
            PDH_FMT_COUNTERVALUE_ITEM_W* items = nullptr;
            DWORD bsz = 0, cnt = 0;
            PdhGetFormattedCounterArrayW(hc, PDH_FMT_DOUBLE, &bsz, &cnt, nullptr);
            std::vector<BYTE> b(bsz + 16);
            PDH_STATUS s2 = PdhGetFormattedCounterArrayW(hc, PDH_FMT_DOUBLE, &bsz, &cnt,
                                                        (PDH_FMT_COUNTERVALUE_ITEM_W*)b.data());
            double dt = NowMs() - t0;
            printf("  %-46ls st=0x%08lX/0x%08lX %.3f ms cnt=%lu\n", p, (unsigned long)s,
                   (unsigned long)s2, dt, cnt);
            if (s2 == ERROR_SUCCESS) {
                for (DWORD i = 0; i < cnt; ++i) {
                    auto* it = (PDH_FMT_COUNTERVALUE_ITEM_W*)b.data() + i;
                    printf("      %-60ls = %.2f MB\n", it->szName, it->FmtValue.doubleValue / 1048576.0);
                }
            }
            PdhCloseQuery(hq);
        }
    }

    // ------------------------------------------------------------------
    // 11) 完整采集耗时
    // ------------------------------------------------------------------
    printf("\n--- 11) 完整采集耗时（N node 查询 + segment + adapter）---\n");
    {
        std::vector<double> costs;
        for (int r = 0; r < 200; ++r) {
            double t0 = NowMs();
            mygpu::QueryStatisticsLocal qad{};
            qad.Type = mygpu::T_ADAPTER;
            qad.AdapterLuid = luid;
            QueryStatistics(&qad);
            for (uint32_t n = 0; n < nodeCount; ++n) {
                mygpu::QueryStatisticsLocal qn{};
                qn.Type = mygpu::T_NODE;
                qn.AdapterLuid = luid;
                qn.QueryNodeId = n;
                QueryStatistics(&qn);
            }
            for (uint32_t s = 0; s < nbSegments; ++s) {
                mygpu::QueryStatisticsLocal qs{};
                qs.Type = mygpu::T_SEGMENT;
                qs.AdapterLuid = luid;
                qs.QuerySegmentId = s;
                QueryStatistics(&qs);
            }
            mygpu::SegmentSizeInfoLocal ss{};
            mygpu::QueryAdapterInfoLocal q{ hAd, mygpu::kQAIGetSegmentSize, &ss, sizeof(ss) };
            QueryAdapterInfo(&q);
            costs.push_back(NowMs() - t0);
        }
        std::sort(costs.begin(), costs.end());
        double sum = 0;
        for (double c : costs) sum += c;
        printf("  n=%zu avg=%.4f ms p50=%.4f ms p95=%.4f ms max=%.4f ms\n",
               costs.size(), sum / costs.size(), costs[costs.size() / 2],
               costs[(size_t)(costs.size() * 0.95)], costs.back());
    }

    // ------------------------------------------------------------------
    // 12) 每个 node 反复查询的稳定性（运行时间是否单调）
    // ------------------------------------------------------------------
    printf("\n--- 12) RunningTime 单调性（node 0，连续 5 次 200ms 间隔）---\n");
    {
        int64_t prev = -1; int nonMono = 0;
        for (int i = 0; i < 5; ++i) {
            mygpu::QueryStatisticsLocal qn{};
            qn.Type = mygpu::T_NODE;
            qn.AdapterLuid = luid;
            qn.QueryNodeId = 0;
            QueryStatistics(&qn);
            int64_t v = qn.QueryResult.NodeInformation.GlobalInformation.RunningTime;
            printf("    t%d: RunningTime=%lld (%.3f s)  %s\n", i, (long long)v, v / 1e7,
                   prev >= 0 ? (v >= prev ? "monotonic" : "REGRESSION") : "first");
            if (prev >= 0 && v < prev) ++nonMono;
            prev = v;
            Sleep(200);
        }
        printf("  非单调次数=%d\n", nonMono);
    }

    if (CloseAdapter) CloseAdapter(&hAd);
    FreeLibrary(gdi);
    printf("\n=== done ===\n");
    return 0;
}
