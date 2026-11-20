// gpuinfo_probe12.cpp - 定位稳态 Collect 的偶发 20~30ms 尖峰来自哪一步
#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <map>
#include <algorithm>
#include <d3dkmthk.h>
#pragma comment(lib, "psapi.lib")

namespace g12 {
typedef uint32_t KMT_HANDLE;
struct AdapterInfoLocal { KMT_HANDLE hAdapter; LUID AdapterLuid; uint32_t NumOfSources; BOOL bPrecise; };
struct EnumAdapters2Local { uint32_t NumAdapters; AdapterInfoLocal* pAdapters; };
struct QueryAdapterInfoLocal { KMT_HANDLE hAdapter; uint32_t Type; void* pData; uint32_t Size; };
struct AdapterRegistryInfoLocal { wchar_t AdapterString[MAX_PATH]; wchar_t BiosString[MAX_PATH]; wchar_t DacType[MAX_PATH]; wchar_t ChipType[MAX_PATH]; };
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
constexpr uint32_t kQAIAdapterRegistryInfo = 8;
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
static void Add(Stat& s, double x) { s.v.push_back(x); }
static void Rep(const char* n, Stat& s) {
    if (s.v.empty()) return;
    std::sort(s.v.begin(), s.v.end());
    double sum = 0; for (double x : s.v) sum += x;
    printf("  %-38s avg=%7.3f p50=%7.3f p95=%7.3f p99=%7.3f max=%7.3f\n", n, sum / s.v.size(),
           s.v[s.v.size() / 2], s.v[s.v.size() * 95 / 100], s.v[s.v.size() * 99 / 100], s.v.back());
}

int main() {
    printf("=== probe12: 稳态 Collect 子步骤尖峰定位 ===\n\n");
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    auto EnumAdapters2 = (g12::PFN_EnumAdapters2)GetProcAddress(gdi, "D3DKMTEnumAdapters2");
    auto QueryAdapterInfo = (g12::PFN_QueryAdapterInfo)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
    auto QueryStatistics = (g12::PFN_QueryStatistics)GetProcAddress(gdi, "D3DKMTQueryStatistics");
    g12::AdapterInfoLocal raw[16]{};
    g12::EnumAdapters2Local ea{}; ea.NumAdapters = 16; ea.pAdapters = raw;
    EnumAdapters2(&ea);
    LUID luid{}; uint32_t nodes = 0, segs = 0;
    for (uint32_t i = 0; i < ea.NumAdapters && i < 16; ++i) {
        g12::AdapterRegistryInfoLocal reg{};
        g12::QueryAdapterInfoLocal q{ raw[i].hAdapter, g12::kQAIAdapterRegistryInfo, &reg, sizeof(reg) };
        if (QueryAdapterInfo(&q) < 0 || !reg.AdapterString[0]) continue;
        g12::QueryStatisticsLocal qa{}; qa.Type = g12::T_ADAPTER; qa.AdapterLuid = raw[i].AdapterLuid;
        if (QueryStatistics(&qa) < 0) continue;
        luid = raw[i].AdapterLuid; nodes = qa.QueryResult.AdapterInformation.NodeCount;
        segs = qa.QueryResult.AdapterInformation.NbSegments;
        break;
    }
    printf("nodes=%u segs=%u\n\n", nodes, segs);

    std::vector<DWORD> pids(16384);
    DWORD got = 0;
    EnumProcesses(pids.data(), (DWORD)(pids.size() * sizeof(DWORD)), &got);
    pids.resize(got / sizeof(DWORD));
    std::vector<HANDLE> hs;
    for (DWORD pid : pids) {
        if (!pid) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!h) continue;
        g12::QueryStatisticsLocal qa{};
        qa.Type = g12::T_PROCESS_ADAPTER; qa.AdapterLuid = luid; qa.hProcess = h;
        if (QueryStatistics(&qa) < 0) { CloseHandle(h); continue; }
        hs.push_back(h);
    }
    printf("GPU 进程 %zu 个\n\n", hs.size());

    Stat sGlobal, sTimes, sProc, sSeg, sFull;
    for (int r = 0; r < 300; ++r) {
        // 1) 全局 node
        double a = NowMs();
        for (uint32_t n = 0; n < nodes; ++n) {
            g12::QueryStatisticsLocal qn{}; qn.Type = g12::T_NODE; qn.AdapterLuid = luid; qn.QueryNodeId = n;
            QueryStatistics(&qn);
        }
        double b = NowMs();
        Add(sGlobal, b - a);

        // 2) GetProcessTimes
        FILETIME f1, f2, f3, f4;
        for (HANDLE h : hs) GetProcessTimes(h, &f1, &f2, &f3, &f4);
        double c = NowMs();
        Add(sTimes, c - b);

        // 3) 逐进程 node
        for (HANDLE h : hs)
            for (uint32_t n = 0; n < nodes; ++n) {
                g12::QueryStatisticsLocal qn{};
                qn.Type = g12::T_PROCESS_NODE; qn.AdapterLuid = luid; qn.hProcess = h; qn.QueryProcessNodeId = n;
                QueryStatistics(&qn);
            }
        double d = NowMs();
        Add(sProc, d - c);

        // 4) segment
        for (uint32_t s = 0; s < segs; ++s) {
            g12::QueryStatisticsLocal qs{}; qs.Type = g12::T_SEGMENT; qs.AdapterLuid = luid; qs.QuerySegmentId = s;
            QueryStatistics(&qs);
        }
        double e = NowMs();
        Add(sSeg, e - d);
        Add(sFull, e - a);
        Sleep(20);
    }
    Rep("1) 全局 T_NODE ×14", sGlobal);
    Rep("2) GetProcessTimes ×N", sTimes);
    Rep("3) 逐进程 T_PROCESS_NODE ×N×14", sProc);
    Rep("4) segment ×3", sSeg);
    Rep("合计 Stage 3", sFull);

    for (HANDLE h : hs) CloseHandle(h);
    printf("\n=== done ===\n");
    return 0;
}
