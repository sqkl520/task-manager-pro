// gpuinfo_probe13.cpp - 减少每轮 syscall 数量：自适应 node 分类 + 去掉冗余 GetProcessTimes
//
// 思路：
//   - 14 个全局 T_NODE 查询里有 13 个是有效的，只有 node0(3D) 恒 0。
//   - 如果 Init 后先"学习"几轮（同时查全局与逐进程），就能分辨出哪些 node
//     的全局统计可信。之后只对不可信的 node 做逐进程累加。
//   - 实测 3D 只有 1 个 node，所以稳态 syscall 从 448 降到 ~32。
//   - GetProcessTimes 每轮完全可省：进程退出后所有 QueryProcessNode 都会失败，
//     自然被清理；句柄指向的是原进程对象，pid 复用不会造成串数据。
#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <d3dkmthk.h>
#pragma comment(lib, "psapi.lib")

namespace g13 {
typedef uint32_t KMT_HANDLE;
struct AdapterInfoLocal { KMT_HANDLE hAdapter; LUID AdapterLuid; uint32_t NumOfSources; BOOL bPrecise; };
struct EnumAdapters2Local { uint32_t NumAdapters; AdapterInfoLocal* pAdapters; };
struct QueryAdapterInfoLocal { KMT_HANDLE hAdapter; uint32_t Type; void* pData; uint32_t Size; };
struct AdapterRegistryInfoLocal { wchar_t AdapterString[MAX_PATH]; wchar_t BiosString[MAX_PATH]; wchar_t DacType[MAX_PATH]; wchar_t ChipType[MAX_PATH]; };
#pragma pack(push, 1)
struct NodeMetadataLocal { int32_t EngineType; wchar_t FriendlyName[32]; uint32_t Flags; uint8_t mmu, iommu; };
struct NodeMetadataQueryLocal { uint32_t NodeOrdinalAndAdapterIndex; NodeMetadataLocal NodeData; };
#pragma pack(pop)
struct DmaPacketTypeInfo { uint32_t a, b, c, d; };
struct QueuePacketTypeInfo { uint32_t a, b; };
struct PacketInformation { QueuePacketTypeInfo q[8]; DmaPacketTypeInfo d[4]; };
struct PreemptionInformation { uint32_t c[16]; };
struct ProcessNodeInformation { int64_t RunningTime; uint32_t ContextSwitch; PreemptionInformation pre; PacketInformation pkt; uint64_t Reserved[8]; };
struct NodeInformation { ProcessNodeInformation GlobalInformation, SystemInformation; uint8_t Tail[0x48]; };
struct ProcessAdapterInformation { uint32_t NbSegments, NodeCount, VidPnSourceCount, VirtualMemoryUsage; uint8_t Rest[0x100]; };
struct MemoryStats { uint64_t T; uint32_t c, r; };
struct SegmentInformation { uint64_t CommitLimit, BytesCommitted, BytesResident; MemoryStats Memory; uint32_t Aperture; uint64_t e[5], a, b, c, r[5]; };
struct AdapterInformation { uint32_t NbSegments, NodeCount, VidPnSourceCount, VSyncEnabled, Tdr; int64_t z; uint64_t rp; uint8_t Rest[0x308 - 0x28]; };
union QueryResultLocal { AdapterInformation AdapterInformation; SegmentInformation SegmentInformation; NodeInformation NodeInformation; ProcessAdapterInformation ProcessAdapterInformation; };
struct QueryStatisticsLocal {
    uint32_t Type; LUID AdapterLuid; HANDLE hProcess; QueryResultLocal QueryResult;
    union { uint32_t QuerySegmentId; uint32_t QueryProcessNodeId; uint32_t QueryNodeId; uint32_t QueryPhys; };
};
constexpr uint32_t T_ADAPTER = 0, T_PROCESS_ADAPTER = 2, T_SEGMENT = 3, T_NODE = 5, T_PROCESS_NODE = 6;
constexpr uint32_t kQAIAdapterRegistryInfo = 8, kQAINodeMetadata = 25;
using PFN_EnumAdapters2 = NTSTATUS(NTAPI*)(EnumAdapters2Local*);
using PFN_QueryAdapterInfo = NTSTATUS(NTAPI*)(QueryAdapterInfoLocal*);
using PFN_QueryStatistics = NTSTATUS(NTAPI*)(const QueryStatisticsLocal*);
}
static double NowMs() {
    static LARGE_INTEGER f{}; static bool i = false;
    if (!i) { QueryPerformanceFrequency(&f); i = true; }
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
struct Stat { std::vector<double> v; };
static void Rep(const char* n, Stat& s) {
    if (s.v.empty()) return;
    std::sort(s.v.begin(), s.v.end());
    double sum = 0; for (double x : s.v) sum += x;
    printf("  %-40s avg=%7.3f p50=%7.3f p90=%7.3f p95=%7.3f p99=%7.3f max=%7.3f\n", n,
           sum / s.v.size(), s.v[s.v.size() / 2], s.v[s.v.size() * 90 / 100],
           s.v[s.v.size() * 95 / 100], s.v[s.v.size() * 99 / 100], s.v.back());
}

int main() {
    printf("=== probe13: 自适应 node 分类后的稳态成本 ===\n\n");
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    auto EnumAdapters2 = (g13::PFN_EnumAdapters2)GetProcAddress(gdi, "D3DKMTEnumAdapters2");
    auto QueryAdapterInfo = (g13::PFN_QueryAdapterInfo)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
    auto QueryStatistics = (g13::PFN_QueryStatistics)GetProcAddress(gdi, "D3DKMTQueryStatistics");
    g13::AdapterInfoLocal raw[16]{};
    g13::EnumAdapters2Local ea{}; ea.NumAdapters = 16; ea.pAdapters = raw;
    EnumAdapters2(&ea);
    LUID luid{}; uint32_t nodes = 0, segs = 0; g13::KMT_HANDLE hAd = 0;
    for (uint32_t i = 0; i < ea.NumAdapters && i < 16; ++i) {
        g13::AdapterRegistryInfoLocal reg{};
        g13::QueryAdapterInfoLocal q{ raw[i].hAdapter, g13::kQAIAdapterRegistryInfo, &reg, sizeof(reg) };
        if (QueryAdapterInfo(&q) < 0 || !reg.AdapterString[0]) continue;
        g13::QueryStatisticsLocal qa{}; qa.Type = g13::T_ADAPTER; qa.AdapterLuid = raw[i].AdapterLuid;
        if (QueryStatistics(&qa) < 0) continue;
        luid = raw[i].AdapterLuid; hAd = raw[i].hAdapter;
        nodes = qa.QueryResult.AdapterInformation.NodeCount;
        segs = qa.QueryResult.AdapterInformation.NbSegments;
        break;
    }
    printf("nodes=%u segs=%u\n", nodes, segs);
    std::vector<int> ntype(nodes, -1);
    for (uint32_t n = 0; n < nodes; ++n) {
        g13::NodeMetadataQueryLocal nm{}; nm.NodeOrdinalAndAdapterIndex = n;
        g13::QueryAdapterInfoLocal q{ hAd, g13::kQAINodeMetadata, &nm, sizeof(nm) };
        if (QueryAdapterInfo(&q) >= 0) ntype[n] = (int)nm.NodeData.EngineType;
    }

    // 取 GPU 进程
    std::vector<DWORD> pids(16384);
    DWORD got = 0;
    EnumProcesses(pids.data(), (DWORD)(pids.size() * sizeof(DWORD)), &got);
    pids.resize(got / sizeof(DWORD));
    std::vector<HANDLE> hs;
    for (DWORD pid : pids) {
        if (!pid) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!h) continue;
        g13::QueryStatisticsLocal qa{};
        qa.Type = g13::T_PROCESS_ADAPTER; qa.AdapterLuid = luid; qa.hProcess = h;
        if (QueryStatistics(&qa) < 0) { CloseHandle(h); continue; }
        hs.push_back(h);
    }
    printf("GPU 进程 %zu 个\n\n", hs.size());

    // ---- 学习阶段：5 轮全量，判断每个 node 的全局统计是否可信 ----
    std::vector<bool> globalTrusted(nodes, false);
    std::vector<int64_t> gPrev(nodes, 0);
    for (uint32_t n = 0; n < nodes; ++n) {
        g13::QueryStatisticsLocal qn{}; qn.Type = g13::T_NODE; qn.AdapterLuid = luid; qn.QueryNodeId = n;
        if (QueryStatistics(&qn) >= 0) gPrev[n] = qn.QueryResult.NodeInformation.GlobalInformation.RunningTime;
    }
    printf("学习阶段（5 轮，间隔 300ms）:\n");
    for (int round = 0; round < 5; ++round) {
        Sleep(300);
        std::vector<int64_t> gCur(nodes, 0), pSum(nodes, 0);
        for (uint32_t n = 0; n < nodes; ++n) {
            g13::QueryStatisticsLocal qn{}; qn.Type = g13::T_NODE; qn.AdapterLuid = luid; qn.QueryNodeId = n;
            if (QueryStatistics(&qn) >= 0) gCur[n] = qn.QueryResult.NodeInformation.GlobalInformation.RunningTime;
        }
        for (HANDLE h : hs)
            for (uint32_t n = 0; n < nodes; ++n) {
                g13::QueryStatisticsLocal qn{};
                qn.Type = g13::T_PROCESS_NODE; qn.AdapterLuid = luid; qn.hProcess = h; qn.QueryProcessNodeId = n;
                if (QueryStatistics(&qn) >= 0) pSum[n] += qn.QueryResult.NodeInformation.GlobalInformation.RunningTime;
            }
        printf("  round %d: ", round);
        for (uint32_t n = 0; n < nodes; ++n) {
            int64_t dg = gCur[n] - gPrev[n];
            int64_t dp = pSum[n];
            if (dg > 0) globalTrusted[n] = true;
            if (dg > 0 || dp > 0)
                printf("[n%u t%d g=%lld p=%lld] ", n, ntype[n], (long long)(dg > 0 ? dg : 0), (long long)dp);
            gPrev[n] = gCur[n];
        }
        printf("\n");
    }
    printf("\n分类结果（true = 全局统计可信，不必查逐进程）:\n  ");
    int trustedCount = 0, untrustedCount = 0;
    for (uint32_t n = 0; n < nodes; ++n) {
        printf("n%u(t%d)=%s ", n, ntype[n], globalTrusted[n] ? "T" : "F");
        if (globalTrusted[n]) ++trustedCount; else ++untrustedCount;
    }
    printf("\n  可信 %d / 不可信 %d\n\n", trustedCount, untrustedCount);

    // ---- 稳态：只对不可信 node 查逐进程 ----
    Stat sFull, sNoGpt;
    for (int r = 0; r < 300; ++r) {
        double a = NowMs();
        // 全局 14 次
        std::vector<int64_t> gCur(nodes, 0);
        for (uint32_t n = 0; n < nodes; ++n) {
            g13::QueryStatisticsLocal qn{}; qn.Type = g13::T_NODE; qn.AdapterLuid = luid; qn.QueryNodeId = n;
            if (QueryStatistics(&qn) >= 0) gCur[n] = qn.QueryResult.NodeInformation.GlobalInformation.RunningTime;
        }
        // 只对不可信 node 查逐进程
        for (HANDLE h : hs)
            for (uint32_t n = 0; n < nodes; ++n) {
                if (globalTrusted[n]) continue;
                g13::QueryStatisticsLocal qn{};
                qn.Type = g13::T_PROCESS_NODE; qn.AdapterLuid = luid; qn.hProcess = h; qn.QueryProcessNodeId = n;
                QueryStatistics(&qn);
            }
        // segment
        for (uint32_t s = 0; s < segs; ++s) {
            g13::QueryStatisticsLocal qs{}; qs.Type = g13::T_SEGMENT; qs.AdapterLuid = luid; qs.QuerySegmentId = s;
            QueryStatistics(&qs);
        }
        sFull.v.push_back(NowMs() - a);
        Sleep(20);
    }
    Rep("自适应稳态（无 GetProcessTimes）", sFull);

    // 对照：全量逐进程（当前实现）
    Stat sAll;
    for (int r = 0; r < 300; ++r) {
        double a = NowMs();
        for (uint32_t n = 0; n < nodes; ++n) {
            g13::QueryStatisticsLocal qn{}; qn.Type = g13::T_NODE; qn.AdapterLuid = luid; qn.QueryNodeId = n;
            QueryStatistics(&qn);
        }
        for (HANDLE h : hs)
            for (uint32_t n = 0; n < nodes; ++n) {
                g13::QueryStatisticsLocal qn{};
                qn.Type = g13::T_PROCESS_NODE; qn.AdapterLuid = luid; qn.hProcess = h; qn.QueryProcessNodeId = n;
                QueryStatistics(&qn);
            }
        for (uint32_t s = 0; s < segs; ++s) {
            g13::QueryStatisticsLocal qs{}; qs.Type = g13::T_SEGMENT; qs.AdapterLuid = luid; qs.QuerySegmentId = s;
            QueryStatistics(&qs);
        }
        sAll.v.push_back(NowMs() - a);
        Sleep(20);
    }
    Rep("对照：全量逐进程（当前实现）", sAll);

    for (HANDLE h : hs) CloseHandle(h);
    printf("\n=== done ===\n");
    return 0;
}
