// gpuinfo_probe10.cpp - 定位 Collect 的 5~27ms 来源
#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <map>
#include <algorithm>
#include <d3dkmthk.h>
#pragma comment(lib, "psapi.lib")

namespace g10 {
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
static double Pct(std::vector<double> v, double q) {
    std::sort(v.begin(), v.end());
    return v[std::min((size_t)(v.size() * q), v.size() - 1)];
}
static double Avg(const std::vector<double>& v) {
    double s = 0; for (double x : v) s += x; return v.empty() ? 0 : s / v.size();
}
static void Rep(const char* n, std::vector<double> v) {
    printf("  %-44s avg=%7.3f p50=%7.3f p95=%7.3f max=%7.3f\n", n, Avg(v), Pct(v, .5), Pct(v, .95),
           *std::max_element(v.begin(), v.end()));
}

int main() {
    printf("=== probe10: Collect 耗时拆解 ===\n\n");
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    auto EnumAdapters2 = (g10::PFN_EnumAdapters2)GetProcAddress(gdi, "D3DKMTEnumAdapters2");
    auto QueryAdapterInfo = (g10::PFN_QueryAdapterInfo)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
    auto QueryStatistics = (g10::PFN_QueryStatistics)GetProcAddress(gdi, "D3DKMTQueryStatistics");
    g10::AdapterInfoLocal raw[16]{};
    g10::EnumAdapters2Local ea{}; ea.NumAdapters = 16; ea.pAdapters = raw;
    EnumAdapters2(&ea);
    LUID luid{}; uint32_t nodes = 0, segs = 0;
    for (uint32_t i = 0; i < ea.NumAdapters && i < 16; ++i) {
        g10::AdapterRegistryInfoLocal reg{};
        g10::QueryAdapterInfoLocal q{ raw[i].hAdapter, g10::kQAIAdapterRegistryInfo, &reg, sizeof(reg) };
        if (QueryAdapterInfo(&q) < 0 || !reg.AdapterString[0]) continue;
        g10::QueryStatisticsLocal qa{}; qa.Type = g10::T_ADAPTER; qa.AdapterLuid = raw[i].AdapterLuid;
        if (QueryStatistics(&qa) < 0) continue;
        luid = raw[i].AdapterLuid; nodes = qa.QueryResult.AdapterInformation.NodeCount;
        segs = qa.QueryResult.AdapterInformation.NbSegments;
        break;
    }
    printf("nodes=%u segs=%u\n\n", nodes, segs);

    // 建缓存，记录每进程真实 node 数
    std::vector<DWORD> pids(16384);
    DWORD got = 0;
    EnumProcesses(pids.data(), (DWORD)(pids.size() * sizeof(DWORD)), &got);
    pids.resize(got / sizeof(DWORD));
    struct C { DWORD pid; HANDLE h; uint32_t nn; uint64_t ct; };
    std::vector<C> cs;
    {
        double t0 = NowMs();
        for (DWORD pid : pids) {
            if (!pid) continue;
            HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
            if (!h) continue;
            g10::QueryStatisticsLocal qa{};
            qa.Type = g10::T_PROCESS_ADAPTER; qa.AdapterLuid = luid; qa.hProcess = h;
            if (QueryStatistics(&qa) < 0) { CloseHandle(h); continue; }
            uint32_t nn = qa.QueryResult.ProcessAdapterInformation.NodeCount;
            FILETIME c, e, k, u; uint64_t ct = 0;
            if (GetProcessTimes(h, &c, &e, &k, &u)) ct = ((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime;
            cs.push_back({ pid, h, nn, ct });
        }
        printf("建缓存: %zu 进程 %.3f ms\n", cs.size(), NowMs() - t0);
    }
    std::map<uint32_t, int> hist;
    for (auto& c : cs) hist[c.nn]++;
    printf("每进程 node 数分布: ");
    for (auto& kv : hist) printf("%u nodes ×%d  ", kv.first, kv.second);
    printf("\n\n");

    // 逐项成本
    {
        std::vector<double> t;
        for (int r = 0; r < 100; ++r) {
            double a = NowMs();
            for (uint32_t n = 0; n < nodes; ++n) {
                g10::QueryStatisticsLocal qn{}; qn.Type = g10::T_NODE; qn.AdapterLuid = luid; qn.QueryNodeId = n;
                QueryStatistics(&qn);
            }
            t.push_back(NowMs() - a);
            Sleep(10);
        }
        Rep("仅全局 T_NODE ×14", t);
    }
    {
        std::vector<double> t;
        for (int r = 0; r < 100; ++r) {
            double a = NowMs();
            FILETIME fa,fb,fc,fd; for (auto& c : cs) GetProcessTimes(c.h, &fa, &fb, &fc, &fd);
            t.push_back(NowMs() - a);
            Sleep(10);
        }
        Rep("仅 GetProcessTimes ×N", t);
    }
    {
        std::vector<double> t;
        for (int r = 0; r < 100; ++r) {
            double a = NowMs();
            for (auto& c : cs)
                for (uint32_t n = 0; n < c.nn; ++n) {
                    g10::QueryStatisticsLocal qn{};
                    qn.Type = g10::T_PROCESS_NODE; qn.AdapterLuid = luid; qn.hProcess = c.h; qn.QueryProcessNodeId = n;
                    QueryStatistics(&qn);
                }
            t.push_back(NowMs() - a);
            Sleep(10);
        }
        Rep("逐进程 T_PROCESS_NODE (用真实 nn)", t);
    }
    {
        std::vector<double> t;
        for (int r = 0; r < 100; ++r) {
            double a = NowMs();
            for (auto& c : cs)
                for (uint32_t n = 0; n < nodes; ++n) {
                    g10::QueryStatisticsLocal qn{};
                    qn.Type = g10::T_PROCESS_NODE; qn.AdapterLuid = luid; qn.hProcess = c.h; qn.QueryProcessNodeId = n;
                    QueryStatistics(&qn);
                }
            t.push_back(NowMs() - a);
            Sleep(10);
        }
        Rep("逐进程 T_PROCESS_NODE (全 14 node)", t);
    }
    {
        std::vector<double> t;
        for (int r = 0; r < 100; ++r) {
            double a = NowMs();
            for (uint32_t s = 0; s < segs; ++s) {
                g10::QueryStatisticsLocal qs{}; qs.Type = g10::T_SEGMENT; qs.AdapterLuid = luid; qs.QuerySegmentId = s;
                QueryStatistics(&qs);
            }
            t.push_back(NowMs() - a);
            Sleep(10);
        }
        Rep("segment ×3", t);
    }
    // 完整一轮（真实实现路径）
    {
        std::vector<double> t;
        for (int r = 0; r < 100; ++r) {
            double a = NowMs();
            for (uint32_t n = 0; n < nodes; ++n) {
                g10::QueryStatisticsLocal qn{}; qn.Type = g10::T_NODE; qn.AdapterLuid = luid; qn.QueryNodeId = n;
                QueryStatistics(&qn);
            }
            for (auto& c : cs) {
                FILETIME fa,fb,fc,fd; GetProcessTimes(c.h, &fa, &fb, &fc, &fd);
                for (uint32_t n = 0; n < c.nn; ++n) {
                    g10::QueryStatisticsLocal qn{};
                    qn.Type = g10::T_PROCESS_NODE; qn.AdapterLuid = luid; qn.hProcess = c.h; qn.QueryProcessNodeId = n;
                    QueryStatistics(&qn);
                }
            }
            for (uint32_t s = 0; s < segs; ++s) {
                g10::QueryStatisticsLocal qs{}; qs.Type = g10::T_SEGMENT; qs.AdapterLuid = luid; qs.QuerySegmentId = s;
                QueryStatistics(&qs);
            }
            t.push_back(NowMs() - a);
            Sleep(10);
        }
        Rep("完整一轮（推荐实现）", t);
    }
    // 只有全局 T_NODE 的极简版
    {
        std::vector<double> t;
        for (int r = 0; r < 100; ++r) {
            double a = NowMs();
            for (uint32_t n = 0; n < nodes; ++n) {
                g10::QueryStatisticsLocal qn{}; qn.Type = g10::T_NODE; qn.AdapterLuid = luid; qn.QueryNodeId = n;
                QueryStatistics(&qn);
            }
            for (uint32_t s = 0; s < segs; ++s) {
                g10::QueryStatisticsLocal qs{}; qs.Type = g10::T_SEGMENT; qs.AdapterLuid = luid; qs.QuerySegmentId = s;
                QueryStatistics(&qs);
            }
            t.push_back(NowMs() - a);
            Sleep(10);
        }
        Rep("仅 T_NODE×14 + segment×3", t);
    }
    for (auto& c : cs) CloseHandle(c.h);
    printf("\n=== done ===\n");
    return 0;
}
