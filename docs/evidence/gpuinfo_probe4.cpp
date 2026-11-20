// gpuinfo_probe4.cpp - 第四轮：T_NODE 全局统计 vs 逐进程 T_PROCESS_NODE vs PDH
//
// 核心疑问（probe3 暴露）：
//   D3DKMT T_NODE node0(3D) delta = 0，但 PDH engtype 3D = 2.497%；
//   D3DKMT T_NODE node9(Copy) = 1.08%，但 PDH Copy = 0.018%。
// 必须确认"每引擎使用率"到底该用哪个数据源。
//
// 做法：同一 2 秒窗口内同时采集
//   (a) T_NODE.GlobalInformation.RunningTime 每节点
//   (b) 所有进程 T_PROCESS_NODE.RunningTime 按节点求和
//   (c) PDH \GPU Engine(*)\Utilization Percentage 按 eng_N / engtype 聚合
//   (d) PDH \GPU Engine(*)\Running Time 两次 collect 的差值
#include <windows.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
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

namespace g4 {
typedef uint32_t KMT_HANDLE;
struct AdapterInfoLocal { KMT_HANDLE hAdapter; LUID AdapterLuid; uint32_t NumOfSources; BOOL bPrecise; };
struct EnumAdapters2Local { uint32_t NumAdapters; AdapterInfoLocal* pAdapters; };
struct QueryAdapterInfoLocal { KMT_HANDLE hAdapter; uint32_t Type; void* pData; uint32_t Size; };
struct AdapterRegistryInfoLocal { wchar_t AdapterString[MAX_PATH]; wchar_t BiosString[MAX_PATH]; wchar_t DacType[MAX_PATH]; wchar_t ChipType[MAX_PATH]; };
#pragma pack(push, 1)
struct NodeMetadataLocal { int32_t EngineType; wchar_t FriendlyName[32]; uint32_t Flags; uint8_t mmu, iommu; };
struct NodeMetadataQueryLocal { uint32_t NodeOrdinalAndAdapterIndex; NodeMetadataLocal NodeData; };
#pragma pack(pop)
struct Counter { uint32_t Count; uint64_t Bytes; };
struct DmaPacketTypeInfo { uint32_t a, b, c, d; };
struct QueuePacketTypeInfo { uint32_t a, b; };
struct PacketInformation { QueuePacketTypeInfo q[8]; DmaPacketTypeInfo d[4]; };
struct PreemptionInformation { uint32_t c[16]; };
struct ProcessNodeInformation { int64_t RunningTime; uint32_t ContextSwitch; PreemptionInformation pre; PacketInformation pkt; uint64_t Reserved[8]; };
struct NodeInformation { ProcessNodeInformation GlobalInformation, SystemInformation; uint8_t Tail[0x48]; };
struct MemoryStats { uint64_t TotalBytesEvicted; uint32_t AllocsCommitted, AllocsResident; };
struct SegmentInformation { uint64_t CommitLimit, BytesCommitted, BytesResident; MemoryStats Memory; uint32_t Aperture; uint64_t e[5], a, b, c, r[5]; };
struct AdapterInformation { uint32_t NbSegments, NodeCount, VidPnSourceCount, VSyncEnabled, TdrDetectedCount; int64_t z; uint64_t rp; uint8_t Rest[0x308 - 0x28]; };
union QueryResultLocal { AdapterInformation AdapterInformation; SegmentInformation SegmentInformation; NodeInformation NodeInformation; };
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
static const wchar_t* EngName(int t) {
    switch (t) {
        case 0: return L"Other"; case 1: return L"3D"; case 2: return L"VideoDecode";
        case 3: return L"VideoEncode"; case 4: return L"VideoProcessing";
        case 5: return L"SceneAssembly"; case 6: return L"Copy"; case 7: return L"Overlay";
        case 8: return L"Crypto"; default: return L"?";
    }
}

int main() {
    printf("=== probe4: T_NODE vs 逐进程 T_PROCESS_NODE vs PDH ===\n\n");
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    auto EnumAdapters2 = (g4::PFN_EnumAdapters2)GetProcAddress(gdi, "D3DKMTEnumAdapters2");
    auto QueryAdapterInfo = (g4::PFN_QueryAdapterInfo)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
    auto QueryStatistics = (g4::PFN_QueryStatistics)GetProcAddress(gdi, "D3DKMTQueryStatistics");

    g4::AdapterInfoLocal raw[16]{};
    g4::EnumAdapters2Local ea{}; ea.NumAdapters = 16; ea.pAdapters = raw;
    EnumAdapters2(&ea);
    uint32_t nAd = ea.NumAdapters < 16 ? ea.NumAdapters : 16;

    LUID luid{}; g4::KMT_HANDLE hAd = 0; wchar_t adName[MAX_PATH] = L""; uint32_t nodes = 0;
    for (uint32_t i = 0; i < nAd; ++i) {
        g4::AdapterRegistryInfoLocal reg{};
        g4::QueryAdapterInfoLocal q{ raw[i].hAdapter, g4::kQAIAdapterRegistryInfo, &reg, sizeof(reg) };
        if (QueryAdapterInfo(&q) < 0 || !reg.AdapterString[0]) continue;
        g4::QueryStatisticsLocal qa{}; qa.Type = g4::T_ADAPTER; qa.AdapterLuid = raw[i].AdapterLuid;
        if (QueryStatistics(&qa) < 0) continue;
        luid = raw[i].AdapterLuid; hAd = raw[i].hAdapter;
        wcsncpy_s(adName, reg.AdapterString, _TRUNCATE);
        nodes = qa.QueryResult.AdapterInformation.NodeCount;
        break;
    }
    if (!hAd) { printf("no adapter\n"); return 1; }
    printf("adapter=\"%ls\" luid=0x%08X:0x%08X nodes=%u\n", adName, luid.HighPart, luid.LowPart, nodes);

    std::vector<int> ntype(nodes, -1);
    std::vector<std::wstring> nname(nodes);
    for (uint32_t n = 0; n < nodes; ++n) {
        g4::NodeMetadataQueryLocal nm{}; nm.NodeOrdinalAndAdapterIndex = n;
        g4::QueryAdapterInfoLocal q{ hAd, g4::kQAINodeMetadata, &nm, sizeof(nm) };
        if (QueryAdapterInfo(&q) >= 0) { ntype[n] = (int)nm.NodeData.EngineType; nname[n] = nm.NodeData.FriendlyName; }
    }
    printf("node map: ");
    for (uint32_t n = 0; n < nodes; ++n)
        printf("%u=%ls%s ", n, EngName(ntype[n]), nname[n].empty() ? L"" : nname[n].c_str());
    printf("\n\n");

    // --- 取 pid 列表 ---
    std::vector<DWORD> pids(16384);
    DWORD got = 0;
    EnumProcesses(pids.data(), (DWORD)(pids.size() * sizeof(DWORD)), &got);
    pids.resize(got / sizeof(DWORD));
    printf("processes = %zu\n", pids.size());

    // --- PDH 准备：Utilization Percentage + Running Time（全 LUID 过滤）---
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
    double te1 = NowMs();
    printf("PDH expand instances=%zu in %.1f ms (只做一次)\n", insts.size(), te1 - te0);

    struct Pi { std::wstring inst; int eng; std::wstring type; PDH_HCOUNTER util, rt; };
    std::vector<Pi> pis;
    PDH_HQUERY hq = nullptr;
    PdhOpenQueryW(nullptr, 0, &hq);
    for (auto& in : insts) {
        Pi p; p.inst = in;
        size_t k = in.find(L"_eng_");
        int eng = -1;
        if (k != std::wstring::npos) eng = _wtoi(in.c_str() + k + 5);
        p.eng = eng;
        size_t t = in.find(L"engtype_");
        p.type = t != std::wstring::npos ? in.substr(t + 8) : L"?";
        std::wstring u = L"\\GPU Engine(" + in + L")\\Utilization Percentage";
        std::wstring r = L"\\GPU Engine(" + in + L")\\Running Time";
        if (PdhAddEnglishCounterW(hq, u.c_str(), 0, &p.util) != ERROR_SUCCESS) continue;
        if (PdhAddEnglishCounterW(hq, r.c_str(), 0, &p.rt) != ERROR_SUCCESS) p.rt = nullptr;
        pis.push_back(p);
    }
    printf("PDH counters ok=%zu\n", pis.size());

    // --- 窗口起点 ---
    PdhCollectQueryData(hq);
    double w0 = NowMs();

    // (a) T_NODE
    std::vector<int64_t> g0(nodes, 0), g1(nodes, 0);
    for (uint32_t n = 0; n < nodes; ++n) {
        g4::QueryStatisticsLocal qn{}; qn.Type = g4::T_NODE; qn.AdapterLuid = luid; qn.QueryNodeId = n;
        if (QueryStatistics(&qn) >= 0) g0[n] = qn.QueryResult.NodeInformation.GlobalInformation.RunningTime;
    }
    // (b) 逐进程 PROCESS_NODE 起点
    // 只对"有 GPU 记录"的进程查询：为避免每轮 OpenProcess 全量，这里探针里全量做一次基线
    struct PH { HANDLE h = nullptr; uint32_t nodes = 0; bool usable = false; };
    std::vector<std::pair<DWORD, PH>> ph;
    std::vector<std::vector<int64_t>> p0;
    std::vector<std::vector<int64_t>> p1;
    {
        double tb = NowMs();
        for (DWORD pid : pids) {
            if (!pid) continue;
            HANDLE hp = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
            if (!hp) continue;
            g4::QueryStatisticsLocal qa{};
            qa.Type = g4::T_PROCESS_ADAPTER; qa.AdapterLuid = luid; qa.hProcess = hp;
            if (QueryStatistics(&qa) < 0) { CloseHandle(hp); continue; }
            PH e; e.h = hp;
            e.nodes = qa.QueryResult.AdapterInformation.NodeCount;   // 注意：这里拿 NodeCount
            // union 布局:T_PROCESS_ADAPTER 时结构不同，仅在探针里谨慎读取（见下方打印校验）
            std::vector<int64_t> v(nodes, 0);
            for (uint32_t n = 0; n < e.nodes && n < nodes; ++n) {
                g4::QueryStatisticsLocal qn{}; qn.Type = g4::T_PROCESS_NODE; qn.AdapterLuid = luid;
                qn.hProcess = hp; qn.QueryProcessNodeId = n;
                if (QueryStatistics(&qn) >= 0) v[n] = qn.QueryResult.NodeInformation.GlobalInformation.RunningTime;
            }
            p0.push_back(v);
            ph.push_back({ pid, e });
        }
        printf("逐进程基线: 有 GPU 记录进程=%zu, 耗时=%.2f ms\n", p0.size(), NowMs() - tb);
    }

    Sleep(2000);

    // (a') T_NODE 终点
    for (uint32_t n = 0; n < nodes; ++n) {
        g4::QueryStatisticsLocal qn{}; qn.Type = g4::T_NODE; qn.AdapterLuid = luid; qn.QueryNodeId = n;
        if (QueryStatistics(&qn) >= 0) g1[n] = qn.QueryResult.NodeInformation.GlobalInformation.RunningTime;
    }
    // (b') 逐进程终点
    for (size_t i = 0; i < ph.size(); ++i) {
        std::vector<int64_t> v(nodes, 0);
        for (uint32_t n = 0; n < nodes; ++n) {
            g4::QueryStatisticsLocal qn{}; qn.Type = g4::T_PROCESS_NODE; qn.AdapterLuid = luid;
            qn.hProcess = ph[i].second.h; qn.QueryProcessNodeId = n;
            if (QueryStatistics(&qn) >= 0) v[n] = qn.QueryResult.NodeInformation.GlobalInformation.RunningTime;
        }
        p1.push_back(v);
    }
    // (c/d) PDH 终点
    PdhCollectQueryData(hq);
    double w1 = NowMs();
    double dt = (w1 - w0) / 1000.0;
    printf("窗口 = %.4f s\n\n", dt);

    // --- 汇总 ---
    std::vector<int64_t> sumProc(nodes, 0);
    for (size_t i = 0; i < p0.size() && i < p1.size(); ++i)
        for (uint32_t n = 0; n < nodes; ++n) {
            int64_t d = p1[i][n] - p0[i][n];
            if (d > 0) sumProc[n] += d;
        }

    std::map<int, double> pdhUtilByEng;
    std::map<std::wstring, double> pdhUtilByType;
    std::map<int, int> pdhEngToType;
    std::map<int, double> pdhRtByEng;
    std::map<std::wstring, double> pdhRtByType;
    for (auto& p : pis) {
        PDH_FMT_COUNTERVALUE v{};
        if (PdhGetFormattedCounterValue(p.util, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS) {
            pdhUtilByEng[p.eng] += v.doubleValue;
            pdhUtilByType[p.type] += v.doubleValue;
            pdhEngToType[p.eng] = -1;
        }
        if (p.rt) {
            PDH_FMT_COUNTERVALUE r{};
            if (PdhGetFormattedCounterValue(p.rt, PDH_FMT_DOUBLE, nullptr, &r) == ERROR_SUCCESS) {
                pdhRtByEng[p.eng] += r.doubleValue;
                pdhRtByType[p.type] += r.doubleValue;
            }
        }
    }

    printf("%-4s %-14s %14s %14s %14s %14s %10s %10s\n",
           "node", "engine", "T_NODE(100ns)", "sumProc(100ns)", "T_NODE %", "sumProc %", "PDHutil%", "PDHrt%");
    for (uint32_t n = 0; n < nodes; ++n) {
        int64_t dg = g1[n] - g0[n]; if (dg < 0) dg = 0;
        double gu = dt > 0 ? dg / (dt * 1e7) * 100.0 : 0;
        double pu = dt > 0 ? sumProc[n] / (dt * 1e7) * 100.0 : 0;
        double du = pdhUtilByEng.count((int)n) ? pdhUtilByEng[(int)n] : 0;
        double dr = dt > 0 && pdhRtByEng.count((int)n) ? pdhRtByEng[(int)n] / (dt * 1e7) * 100.0 : 0;
        printf("%-4u %-14ls %14lld %14lld %13.3f%% %13.3f%% %9.3f%% %9.3f%%\n",
               n, EngName(ntype[n]), (long long)dg, (long long)sumProc[n], gu, pu, du, dr);
    }

    printf("\n--- 按引擎类型聚合 ---\n");
    std::map<int, std::pair<int64_t, int64_t>> byType;  // type -> (T_NODE, sumProc)
    for (uint32_t n = 0; n < nodes; ++n) {
        int64_t dg = g1[n] - g0[n]; if (dg < 0) dg = 0;
        byType[ntype[n]].first += dg;
        byType[ntype[n]].second += sumProc[n];
    }
    printf("%-16s %13s %13s %13s %13s\n", "engine", "T_NODE %", "sumProc %", "PDHutil%", "PDHrt%");
    for (auto& kv : byType)
        printf("%-16ls %12.3f%% %12.3f%% %12.3f%% %12.3f%%\n", EngName(kv.first),
               dt > 0 ? kv.second.first / (dt * 1e7) * 100.0 : 0,
               dt > 0 ? kv.second.second / (dt * 1e7) * 100.0 : 0,
               pdhUtilByType.count(std::wstring(EngName(kv.first))) ? pdhUtilByType[std::wstring(EngName(kv.first))] : 0.0,
               dt > 0 && pdhRtByType.count(std::wstring(EngName(kv.first))) ? pdhRtByType[std::wstring(EngName(kv.first))] / (dt * 1e7) * 100.0 : 0.0);

    // 未在 node map 出现的 PDH engtype（不应发生）
    printf("\nPDH 出现但 node map 没有的 engtype: ");
    for (auto& kv : pdhUtilByType) {
        bool found = false;
        for (uint32_t n = 0; n < nodes; ++n) if (kv.first == EngName(ntype[n])) { found = true; break; }
        if (!found) printf("[%ls %.3f%%] ", kv.first.c_str(), kv.second);
    }
    printf("\n");

    for (auto& e : ph) if (e.second.h) CloseHandle(e.second.h);
    PdhCloseQuery(hq);

    // --- 额外：T_NODE SystemInformation / ContextSwitch 是否也有数据 ---
    printf("\n--- T_NODE 细节（node 0..3, 窗口前后各一次）---\n");
    for (uint32_t n = 0; n < nodes && n < 4; ++n) {
        g4::QueryStatisticsLocal qn{}; qn.Type = g4::T_NODE; qn.AdapterLuid = luid; qn.QueryNodeId = n;
        if (QueryStatistics(&qn) < 0) continue;
        const auto& g = qn.QueryResult.NodeInformation.GlobalInformation;
        const auto& s = qn.QueryResult.NodeInformation.SystemInformation;
        printf("  node %u(%ls): globalRT=%lld ctxSw=%u | sysRT=%lld sysCtxSw=%u\n",
               n, EngName(ntype[n]), (long long)g.RunningTime, g.ContextSwitch,
               (long long)s.RunningTime, s.ContextSwitch);
    }
    printf("\n=== done ===\n");
    return 0;
}
