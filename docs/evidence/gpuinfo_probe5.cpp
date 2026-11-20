// gpuinfo_probe5.cpp - 第五轮：确定每引擎总使用率的正确口径 + 全量采集成本
//
// 已知（probe4）：
//   T_NODE.GlobalInformation.RunningTime 对 3D 节点恒为 0（驱动/node0 特例），
//   但 VideoDecode/Copy 有值；逐进程 T_PROCESS_NODE 求和与 PDH Utilization 吻合。
// 本探针要回答：
//   1) T_NODE node0 原始字节到底是什么（真的全 0？）
//   2) 多窗口下 max(T_NODE, sumProc) 能否稳定贴合 PDH engtype Utilization
//   3) 逐进程引擎采集的真实成本：
//        EnumProcesses vs NtQuerySystemInformation(SystemProcessInformation)
//        OpenProcess(600) 成本、39 个 GPU 进程 × 14 node 的成本、句柄缓存后的成本
//   4) 总采集是否 < 5ms
#include <windows.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <winternl.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <map>
#include <algorithm>

#include <d3dkmthk.h>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "pdh.lib")

namespace g5 {
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
struct ProcessAdapterInformation {
    uint32_t NbSegments; uint32_t NodeCount; uint32_t VidPnSourceCount; uint32_t VirtualMemoryUsage;
    uint8_t Rest[0x110 - 0x10];
};
struct MemoryStats { uint64_t TotalBytesEvicted; uint32_t AllocsCommitted, AllocsResident; };
struct SegmentInformation { uint64_t CommitLimit, BytesCommitted, BytesResident; MemoryStats Memory; uint32_t Aperture; uint64_t e[5], a, b, c, r[5]; };
struct AdapterInformation { uint32_t NbSegments, NodeCount, VidPnSourceCount, VSyncEnabled, TdrDetectedCount; int64_t z; uint64_t rp; uint8_t Rest[0x308 - 0x28]; };
union QueryResultLocal { AdapterInformation AdapterInformation; SegmentInformation SegmentInformation; NodeInformation NodeInformation; ProcessAdapterInformation ProcessAdapterInformation; };
struct QueryStatisticsLocal {
    uint32_t Type; LUID AdapterLuid; HANDLE hProcess; QueryResultLocal QueryResult;
    union { uint32_t QuerySegmentId; uint32_t QueryProcessNodeId; uint32_t QueryNodeId; uint32_t QueryPhysicalAdapterIndex; };
};
constexpr uint32_t T_ADAPTER = 0, T_PROCESS_ADAPTER = 2, T_NODE = 5, T_PROCESS_NODE = 6;
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
static double Pct(std::vector<double> v, double q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t i = (size_t)(v.size() * q);
    return v[std::min(i, v.size() - 1)];
}
static double Avg(const std::vector<double>& v) {
    if (v.empty()) return 0;
    double s = 0; for (double x : v) s += x; return s / v.size();
}
static const wchar_t* EngName(int t) {
    switch (t) {
        case 0: return L"Other"; case 1: return L"3D"; case 2: return L"VideoDecode";
        case 3: return L"VideoEncode"; case 4: return L"VideoProcessing";
        case 5: return L"SceneAssembly"; case 6: return L"Copy"; case 7: return L"Overlay";
        case 8: return L"Crypto"; default: return L"?";
    }
}

int main() {
    printf("=== probe5: 每引擎口径 + 全量成本 ===\n\n");
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    auto EnumAdapters2 = (g5::PFN_EnumAdapters2)GetProcAddress(gdi, "D3DKMTEnumAdapters2");
    auto QueryAdapterInfo = (g5::PFN_QueryAdapterInfo)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
    auto QueryStatistics = (g5::PFN_QueryStatistics)GetProcAddress(gdi, "D3DKMTQueryStatistics");

    g5::AdapterInfoLocal raw[16]{};
    g5::EnumAdapters2Local ea{}; ea.NumAdapters = 16; ea.pAdapters = raw;
    EnumAdapters2(&ea);
    uint32_t nAd = ea.NumAdapters < 16 ? ea.NumAdapters : 16;
    LUID luid{}; g5::KMT_HANDLE hAd = 0; wchar_t adName[MAX_PATH] = L""; uint32_t nodes = 0;
    for (uint32_t i = 0; i < nAd; ++i) {
        g5::AdapterRegistryInfoLocal reg{};
        g5::QueryAdapterInfoLocal q{ raw[i].hAdapter, g5::kQAIAdapterRegistryInfo, &reg, sizeof(reg) };
        if (QueryAdapterInfo(&q) < 0 || !reg.AdapterString[0]) continue;
        g5::QueryStatisticsLocal qa{}; qa.Type = g5::T_ADAPTER; qa.AdapterLuid = raw[i].AdapterLuid;
        if (QueryStatistics(&qa) < 0) continue;
        luid = raw[i].AdapterLuid; hAd = raw[i].hAdapter;
        wcsncpy_s(adName, reg.AdapterString, _TRUNCATE);
        nodes = qa.QueryResult.AdapterInformation.NodeCount;
        break;
    }
    if (!hAd) { printf("no adapter\n"); return 1; }
    printf("adapter=\"%ls\" nodes=%u\n", adName, nodes);

    std::vector<int> ntype(nodes, -1);
    for (uint32_t n = 0; n < nodes; ++n) {
        g5::NodeMetadataQueryLocal nm{}; nm.NodeOrdinalAndAdapterIndex = n;
        g5::QueryAdapterInfoLocal q{ hAd, g5::kQAINodeMetadata, &nm, sizeof(nm) };
        if (QueryAdapterInfo(&q) >= 0) ntype[n] = (int)nm.NodeData.EngineType;
    }

    // ------------------------------------------------------------------
    // 1) T_NODE node0 原始字节
    // ------------------------------------------------------------------
    printf("\n--- 1) T_NODE 原始字节（node 0 = 3D, node 2 = VideoDecode）---\n");
    for (uint32_t n : { 0u, 2u }) {
        g5::QueryStatisticsLocal qn{}; qn.Type = g5::T_NODE; qn.AdapterLuid = luid; qn.QueryNodeId = n;
        NTSTATUS st = QueryStatistics(&qn);
        printf("  node %u: NTSTATUS=0x%08lX\n", n, (unsigned long)st);
        const uint8_t* b = (const uint8_t*)&qn.QueryResult.NodeInformation;
        for (int row = 0; row < 8; ++row) {
            printf("    +%03X:", row * 16);
            for (int col = 0; col < 16; ++col) printf(" %02X", b[row * 16 + col]);
            printf("\n");
        }
        // NodePerfData 区（0x220 起）
        const uint8_t* pd = b + 0x220;
        printf("    NodePerfData @0x220:");
        for (int i = 0; i < 24; ++i) printf(" %02X", pd[i]);
        printf("\n");
    }

    // ------------------------------------------------------------------
    // 2) pid 枚举成本：EnumProcesses vs NtQuerySystemInformation
    // ------------------------------------------------------------------
    printf("\n--- 2) pid 枚举成本（各 30 次）---\n");
    {
        std::vector<double> ep, nq;
        std::vector<DWORD> b(16384);
        for (int i = 0; i < 30; ++i) {
            double t0 = NowMs();
            DWORD got = 0;
            EnumProcesses(b.data(), (DWORD)(b.size() * sizeof(DWORD)), &got);
            ep.push_back(NowMs() - t0);
        }
        auto pNtQSI = (NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG))
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
        std::vector<BYTE> buf(1 << 20);
        ULONG need = 0;
        for (int i = 0; i < 30; ++i) {
            double t0 = NowMs();
            // SystemProcessInformation = 5
            NTSTATUS s = pNtQSI ? pNtQSI(5, buf.data(), (ULONG)buf.size(), &need) : -1;
            nq.push_back(NowMs() - t0);
            if (i == 0) printf("  NtQSI(5) status=0x%08lX need=%lu bytes\n", (unsigned long)s, need);
        }
        printf("  EnumProcesses: avg=%.3f p50=%.3f p95=%.3f max=%.3f ms\n",
               Avg(ep), Pct(ep, .5), Pct(ep, .95), *std::max_element(ep.begin(), ep.end()));
        printf("  NtQSI(SystemProcessInformation): avg=%.3f p50=%.3f p95=%.3f max=%.3f ms\n",
               Avg(nq), Pct(nq, .5), Pct(nq, .95), *std::max_element(nq.begin(), nq.end()));
    }

    // ------------------------------------------------------------------
    // 3) OpenProcess 全量成本
    // ------------------------------------------------------------------
    printf("\n--- 3) OpenProcess 全量成本 ---\n");
    std::vector<DWORD> pids(16384);
    DWORD got = 0;
    EnumProcesses(pids.data(), (DWORD)(pids.size() * sizeof(DWORD)), &got);
    pids.resize(got / sizeof(DWORD));
    {
        std::vector<HANDLE> hs;
        double t0 = NowMs();
        for (DWORD pid : pids) {
            if (!pid) continue;
            HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
            if (h) hs.push_back(h);
        }
        double t1 = NowMs();
        printf("  进程数=%zu OpenProcess 成功=%zu 耗时=%.3f ms\n", pids.size(), hs.size(), t1 - t0);
        double t2 = NowMs();
        for (HANDLE h : hs) CloseHandle(h);
        printf("  CloseHandle 全部=%.3f ms\n", NowMs() - t2);
    }

    // ------------------------------------------------------------------
    // 4) 每轮成本：句柄缓存后，只对有 GPU 记录的进程查询
    // ------------------------------------------------------------------
    printf("\n--- 4) 每轮成本（句柄缓存 + 仅 GPU 进程 × node）---\n");
    struct Cache { DWORD pid; HANDLE h; uint32_t nodes; bool usable; };
    std::vector<Cache> cache;
    {
        double t0 = NowMs();
        for (DWORD pid : pids) {
            if (!pid) continue;
            HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
            if (!h) continue;
            g5::QueryStatisticsLocal qa{};
            qa.Type = g5::T_PROCESS_ADAPTER; qa.AdapterLuid = luid; qa.hProcess = h;
            if (QueryStatistics(&qa) < 0) { CloseHandle(h); continue; }
            uint32_t nc = qa.QueryResult.ProcessAdapterInformation.NodeCount;
            if (nc == 0) { CloseHandle(h); continue; }
            if (nc > nodes) nc = nodes;
            cache.push_back({ pid, h, nc, true });
        }
        printf("  初次扫描+建缓存: %zu 个 GPU 进程, %.3f ms\n", cache.size(), NowMs() - t0);
    }
    {
        // 纯 node 查询（热）
        std::vector<double> costs;
        std::vector<int64_t> run(nodes, 0);
        for (int r = 0; r < 100; ++r) {
            double t0 = NowMs();
            for (auto& c : cache)
                for (uint32_t n = 0; n < c.nodes; ++n) {
                    g5::QueryStatisticsLocal qn{};
                    qn.Type = g5::T_PROCESS_NODE; qn.AdapterLuid = luid;
                    qn.hProcess = c.h; qn.QueryProcessNodeId = n;
                    if (QueryStatistics(&qn) >= 0) run[n] += qn.QueryResult.NodeInformation.GlobalInformation.RunningTime;
                }
            costs.push_back(NowMs() - t0);
            Sleep(20);
        }
        printf("  纯 node 查询(热, %zu 进程): avg=%.3f p50=%.3f p95=%.3f max=%.3f ms\n",
               cache.size(), Avg(costs), Pct(costs, .5), Pct(costs, .95),
               *std::max_element(costs.begin(), costs.end()));
    }
    {
        // 含 PROCESS_ADAPTER 复核 + node（每轮完整）
        std::vector<double> costs;
        for (int r = 0; r < 100; ++r) {
            double t0 = NowMs();
            for (auto& c : cache) {
                g5::QueryStatisticsLocal qa{};
                qa.Type = g5::T_PROCESS_ADAPTER; qa.AdapterLuid = luid; qa.hProcess = c.h;
                if (QueryStatistics(&qa) < 0) continue;
                uint32_t nc = qa.QueryResult.ProcessAdapterInformation.NodeCount;
                if (nc > nodes) nc = nodes;
                for (uint32_t n = 0; n < nc; ++n) {
                    g5::QueryStatisticsLocal qn{};
                    qn.Type = g5::T_PROCESS_NODE; qn.AdapterLuid = luid;
                    qn.hProcess = c.h; qn.QueryProcessNodeId = n;
                    QueryStatistics(&qn);
                }
            }
            costs.push_back(NowMs() - t0);
            Sleep(20);
        }
        printf("  含 PROCESS_ADAPTER(%zu 进程): avg=%.3f p50=%.3f p95=%.3f max=%.3f ms\n",
               cache.size(), Avg(costs), Pct(costs, .5), Pct(costs, .95),
               *std::max_element(costs.begin(), costs.end()));
    }

    // ------------------------------------------------------------------
    // 5) 多窗口口径一致性：max(T_NODE, sumProc) vs PDH engtype Utilization
    // ------------------------------------------------------------------
    printf("\n--- 5) 多窗口口径对照（5 个 1s 窗口）---\n");
    // PDH 准备
    auto ExpandInsts = [](const wchar_t* pat) {
        std::vector<std::wstring> r;
        DWORD sz = 0;
        if (PdhExpandWildCardPathW(nullptr, pat, nullptr, &sz, 0) != PDH_MORE_DATA) return r;
        std::vector<wchar_t> b(sz + 16);
        DWORD sz2 = (DWORD)b.size();
        if (PdhExpandWildCardPathW(nullptr, pat, b.data(), &sz2, 0) != ERROR_SUCCESS) return r;
        for (size_t i = 0; i < b.size() && b[i];) {
            size_t l = wcslen(&b[i]);
            std::wstring f(&b[i], l);
            size_t lp = f.find(L'('), rp = f.rfind(L')');
            if (lp != std::wstring::npos && rp > lp) r.push_back(f.substr(lp + 1, rp - lp - 1));
            i += l + 1;
        }
        return r;
    };
    wchar_t pat[MAX_PATH * 2];
    swprintf_s(pat, L"\\GPU Engine(pid_*_luid_0x%08X_0x%08X_phys_0_*)\\Utilization Percentage",
               luid.HighPart, luid.LowPart);
    double te0 = NowMs();
    auto insts = ExpandInsts(pat);
    printf("  PDH expand once: %.1f ms, %zu instances\n", NowMs() - te0, insts.size());
    struct Pi { int eng; std::wstring type; PDH_HCOUNTER u; };
    std::vector<Pi> pis;
    PDH_HQUERY hq = nullptr;
    PdhOpenQueryW(nullptr, 0, &hq);
    for (auto& in : insts) {
        Pi p{};
        size_t k = in.find(L"_eng_");
        p.eng = k != std::wstring::npos ? _wtoi(in.c_str() + k + 5) : -1;
        size_t t = in.find(L"engtype_");
        p.type = t != std::wstring::npos ? in.substr(t + 8) : L"?";
        std::wstring u = L"\\GPU Engine(" + in + L")\\Utilization Percentage";
        if (PdhAddEnglishCounterW(hq, u.c_str(), 0, &p.u) == ERROR_SUCCESS) pis.push_back(p);
    }
    printf("  PDH counters=%zu\n", pis.size());
    PdhCollectQueryData(hq);

    std::vector<int64_t> gn0(nodes, 0), gn1(nodes, 0);
    std::vector<std::vector<int64_t>> pn0(cache.size(), std::vector<int64_t>(nodes, 0));
    std::vector<std::vector<int64_t>> pn1(cache.size(), std::vector<int64_t>(nodes, 0));

    for (int w = 0; w < 5; ++w) {
        for (auto& v : pn0) std::fill(v.begin(), v.end(), 0);
        for (auto& v : pn1) std::fill(v.begin(), v.end(), 0);
        std::fill(gn0.begin(), gn0.end(), 0);
        std::fill(gn1.begin(), gn1.end(), 0);

        PdhCollectQueryData(hq);
        double t0 = NowMs();
        for (uint32_t n = 0; n < nodes; ++n) {
            g5::QueryStatisticsLocal qn{}; qn.Type = g5::T_NODE; qn.AdapterLuid = luid; qn.QueryNodeId = n;
            if (QueryStatistics(&qn) >= 0) gn0[n] = qn.QueryResult.NodeInformation.GlobalInformation.RunningTime;
        }
        for (size_t i = 0; i < cache.size(); ++i)
            for (uint32_t n = 0; n < cache[i].nodes; ++n) {
                g5::QueryStatisticsLocal qn{};
                qn.Type = g5::T_PROCESS_NODE; qn.AdapterLuid = luid;
                qn.hProcess = cache[i].h; qn.QueryProcessNodeId = n;
                if (QueryStatistics(&qn) >= 0) pn0[i][n] = qn.QueryResult.NodeInformation.GlobalInformation.RunningTime;
            }
        Sleep(1000);
        for (uint32_t n = 0; n < nodes; ++n) {
            g5::QueryStatisticsLocal qn{}; qn.Type = g5::T_NODE; qn.AdapterLuid = luid; qn.QueryNodeId = n;
            if (QueryStatistics(&qn) >= 0) gn1[n] = qn.QueryResult.NodeInformation.GlobalInformation.RunningTime;
        }
        for (size_t i = 0; i < cache.size(); ++i)
            for (uint32_t n = 0; n < cache[i].nodes; ++n) {
                g5::QueryStatisticsLocal qn{};
                qn.Type = g5::T_PROCESS_NODE; qn.AdapterLuid = luid;
                qn.hProcess = cache[i].h; qn.QueryProcessNodeId = n;
                if (QueryStatistics(&qn) >= 0) pn1[i][n] = qn.QueryResult.NodeInformation.GlobalInformation.RunningTime;
            }
        PdhCollectQueryData(hq);
        double t1 = NowMs();
        double dt = (t1 - t0) / 1000.0;

        std::vector<int64_t> sumProc(nodes, 0), nodeGlob(nodes, 0);
        for (size_t i = 0; i < cache.size(); ++i)
            for (uint32_t n = 0; n < nodes; ++n) {
                int64_t d = pn1[i][n] - pn0[i][n];
                if (d > 0) sumProc[n] += d;
            }
        for (uint32_t n = 0; n < nodes; ++n) {
            int64_t d = gn1[n] - gn0[n];
            nodeGlob[n] = d > 0 ? d : 0;
        }
        std::map<int, double> pdhByEng;
        for (auto& p : pis) {
            PDH_FMT_COUNTERVALUE v{};
            if (PdhGetFormattedCounterValue(p.u, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS)
                pdhByEng[p.eng] += v.doubleValue;
        }
        printf("  window %d (dt=%.4fs):\n", w, dt);
        printf("    %-4s %-14s %10s %10s %10s %10s\n", "node", "engine", "T_NODE%", "sumProc%", "max%", "PDH%");
        std::map<int, double> aggMax, aggPdh, aggProc, aggNode;
        for (uint32_t n = 0; n < nodes; ++n) {
            double g_ = dt > 0 ? nodeGlob[n] / (dt * 1e7) * 100.0 : 0;
            double p_ = dt > 0 ? sumProc[n] / (dt * 1e7) * 100.0 : 0;
            double m_ = std::max(g_, p_);
            double d_ = pdhByEng.count((int)n) ? pdhByEng[(int)n] : 0.0;
            if (g_ > 0.001 || p_ > 0.001 || d_ > 0.001)
                printf("    %-4u %-14ls %9.3f%% %9.3f%% %9.3f%% %9.3f%%\n", n, EngName(ntype[n]), g_, p_, m_, d_);
            if (ntype[n] >= 0) {
                aggNode[ntype[n]] += g_;
                aggProc[ntype[n]] += p_;
                aggMax[ntype[n]] += m_;
                aggPdh[ntype[n]] += d_;
            }
        }
        printf("    -- 按类型: %-14s %10s %10s %10s %10s\n", "engine", "T_NODE", "sumProc", "max", "PDH");
        for (auto& kv : aggMax)
            printf("       %-16ls %9.3f%% %9.3f%% %9.3f%% %9.3f%%\n", EngName(kv.first),
                   aggNode[kv.first], aggProc[kv.first], kv.second, aggPdh[kv.first]);
    }
    PdhCloseQuery(hq);
    for (auto& c : cache) if (c.h) CloseHandle(c.h);

    printf("\n=== done ===\n");
    return 0;
}
