// gpuinfo_probe2.cpp - 第二轮：修正 NODEMETADATA 打包 + 多 LUID 辨析 + 内存口径
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <map>
#include <algorithm>

#include <d3dkmthk.h>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "dxgi.lib")

namespace g2 {

typedef uint32_t KMT_HANDLE;
struct AdapterInfoLocal { KMT_HANDLE hAdapter; LUID AdapterLuid; uint32_t NumOfSources; BOOL bPrecise; };
struct EnumAdapters2Local { uint32_t NumAdapters; AdapterInfoLocal* pAdapters; };
struct QueryAdapterInfoLocal { KMT_HANDLE hAdapter; uint32_t Type; void* pData; uint32_t Size; };
struct AdapterRegistryInfoLocal { wchar_t AdapterString[MAX_PATH]; wchar_t BiosString[MAX_PATH]; wchar_t DacType[MAX_PATH]; wchar_t ChipType[MAX_PATH]; };
struct AdapterAddressLocal { uint32_t Bus, Device, Function; };
struct SegmentSizeInfoLocal { uint64_t DedicatedVideoMemorySize, DedicatedSystemMemorySize, SharedSystemMemorySize; };

// 关键：SDK 在 #pragma pack(push,1) 区间内定义 DXGK_NODEMETADATA -> 必须 1 字节打包
#pragma pack(push, 1)
struct NodeMetadataLocal {
    int32_t EngineType;
    wchar_t FriendlyName[32];
    uint32_t Flags;
    uint8_t  GpuMmuSupported;
    uint8_t  IoMmuSupported;
};
struct NodeMetadataQueryLocal {
    uint32_t NodeOrdinalAndAdapterIndex;
    NodeMetadataLocal NodeData;
};
#pragma pack(pop)

struct Counter { uint32_t Count; uint64_t Bytes; };
struct DmaPacketTypeInfo { uint32_t a, b, c, d; };
struct QueuePacketTypeInfo { uint32_t a, b; };
struct PacketInformation { QueuePacketTypeInfo q[8]; DmaPacketTypeInfo d[4]; };
struct PreemptionInformation { uint32_t c[16]; };
struct ProcessNodeInformation {
    int64_t RunningTime; uint32_t ContextSwitch;
    PreemptionInformation pre; PacketInformation pkt; uint64_t Reserved[8];
};
struct NodeInformation {
    ProcessNodeInformation GlobalInformation, SystemInformation;
    uint8_t Tail[0x48];
};
struct MemoryStats { uint64_t TotalBytesEvicted; uint32_t AllocsCommitted, AllocsResident; };
struct SegmentInformation {
    uint64_t CommitLimit, BytesCommitted, BytesResident;
    MemoryStats Memory; uint32_t Aperture;
    uint64_t TotalBytesEvictedByPriority[5];
    uint64_t SystemMemoryEndAddress, PowerFlags, SegmentProperties, Reserved[5];
};
struct AdapterInformation {
    uint32_t NbSegments, NodeCount, VidPnSourceCount, VSyncEnabled, TdrDetectedCount;
    int64_t ZeroLengthDmaBuffers; uint64_t RestartedPeriod;
    uint8_t Rest[0x308 - 0x28];
};
union QueryResultLocal { AdapterInformation AdapterInformation; SegmentInformation SegmentInformation; NodeInformation NodeInformation; };
struct QueryStatisticsLocal {
    uint32_t Type; LUID AdapterLuid; HANDLE hProcess; QueryResultLocal QueryResult;
    union { uint32_t QuerySegmentId; uint32_t QueryNodeId; uint32_t QueryPhysicalAdapterIndex; };
};

constexpr uint32_t T_ADAPTER = 0, T_SEGMENT = 3, T_NODE = 5;
constexpr uint32_t kQAIGetSegmentSize = 3, kQAIAdapterAddress = 6, kQAIAdapterRegistryInfo = 8, kQAINodeMetadata = 25;

using PFN_EnumAdapters2 = NTSTATUS(NTAPI*)(EnumAdapters2Local*);
using PFN_QueryAdapterInfo = NTSTATUS(NTAPI*)(QueryAdapterInfoLocal*);
using PFN_QueryStatistics = NTSTATUS(NTAPI*)(const QueryStatisticsLocal*);
}

static double NowMs() {
    static LARGE_INTEGER f{};
    static bool i = false;
    if (!i) { QueryPerformanceFrequency(&f); i = true; }
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

int main() {
    printf("=== probe2: NODEMETADATA(pack1) / 多 LUID / 内存口径 ===\n\n");
    printf("sizeof(NodeMetadataLocal)=%zu (SDK %zu)  sizeof(NodeMetadataQueryLocal)=%zu (SDK %zu)\n",
           sizeof(g2::NodeMetadataLocal), sizeof(DXGK_NODEMETADATA),
           sizeof(g2::NodeMetadataQueryLocal), sizeof(D3DKMT_NODEMETADATA));

    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    auto EnumAdapters2 = (g2::PFN_EnumAdapters2)GetProcAddress(gdi, "D3DKMTEnumAdapters2");
    auto QueryAdapterInfo = (g2::PFN_QueryAdapterInfo)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
    auto QueryStatistics = (g2::PFN_QueryStatistics)GetProcAddress(gdi, "D3DKMTQueryStatistics");
    auto CloseAdapter = (NTSTATUS(NTAPI*)(const void*))GetProcAddress(gdi, "D3DKMTCloseAdapter");

    g2::AdapterInfoLocal raw[16]{};
    g2::EnumAdapters2Local ea{};
    ea.NumAdapters = 16;
    ea.pAdapters = raw;
    EnumAdapters2(&ea);
    uint32_t nAd = ea.NumAdapters < 16 ? ea.NumAdapters : 16;
    printf("adapters=%u\n", nAd);

    // ---------------------------------------------------------------
    // A) 每个适配器：NODEMETADATA(pack1) 是否成功 + 节点运行时间活动度
    // ---------------------------------------------------------------
    printf("\n--- A) 每适配器 NODEMETADATA(pack=1) ---\n");
    struct AdInfo { LUID luid; g2::KMT_HANDLE h; uint32_t nodes, segs; wchar_t name[MAX_PATH]; bool nmOk; };
    std::vector<AdInfo> ads;
    for (uint32_t i = 0; i < nAd; ++i) {
        AdInfo a{};
        a.luid = raw[i].AdapterLuid;
        a.h = raw[i].hAdapter;
        g2::AdapterRegistryInfoLocal reg{};
        g2::QueryAdapterInfoLocal q{ raw[i].hAdapter, g2::kQAIAdapterRegistryInfo, &reg, sizeof(reg) };
        NTSTATUS rs = QueryAdapterInfo(&q);
        if (rs >= 0) wcsncpy_s(a.name, reg.AdapterString, _TRUNCATE);

        g2::QueryStatisticsLocal qa{};
        qa.Type = g2::T_ADAPTER;
        qa.AdapterLuid = a.luid;
        if (QueryStatistics(&qa) < 0) continue;
        a.nodes = qa.QueryResult.AdapterInformation.NodeCount;
        a.segs = qa.QueryResult.AdapterInformation.NbSegments;

        printf("  [%u] luid=0x%08X:0x%08X name=\"%ls\" nodes=%u segs=%u\n",
               i, a.luid.HighPart, a.luid.LowPart, a.name, a.nodes, a.segs);

        // NODEMETADATA 逐 node
        int ok = 0;
        for (uint32_t n = 0; n < a.nodes; ++n) {
            g2::NodeMetadataQueryLocal nm{};
            nm.NodeOrdinalAndAdapterIndex = (0u << 16) | n;
            g2::QueryAdapterInfoLocal nq{ a.h, g2::kQAINodeMetadata, &nm, sizeof(nm) };
            NTSTATUS s = QueryAdapterInfo(&nq);
            if (s < 0) continue;
            ++ok;
            printf("       node %2u: type=%d name=\"%ls\" mmu=%u iommu=%u\n",
                   n, (int)nm.NodeData.EngineType, nm.NodeData.FriendlyName,
                   (unsigned)nm.NodeData.GpuMmuSupported, (unsigned)nm.NodeData.IoMmuSupported);
        }
        a.nmOk = (ok > 0);
        printf("       NODEMETADATA ok=%d/%u\n", ok, a.nodes);
        ads.push_back(a);
    }

    // ---------------------------------------------------------------
    // B) 三个 NVIDIA LUID 谁在真实活动
    // ---------------------------------------------------------------
    printf("\n--- B) 每适配器节点 RunningTime 差分（1s）---\n");
    struct Snap { std::vector<int64_t> rt; };
    std::vector<Snap> s1(ads.size()), s2(ads.size());
    for (size_t a = 0; a < ads.size(); ++a) s1[a].rt.assign(ads[a].nodes, 0);
    auto SampleAll = [&](std::vector<Snap>& out, double* ms) {
        double t0 = NowMs();
        for (size_t a = 0; a < ads.size(); ++a) {
            out[a].rt.assign(ads[a].nodes, 0);
            for (uint32_t n = 0; n < ads[a].nodes; ++n) {
                g2::QueryStatisticsLocal qn{};
                qn.Type = g2::T_NODE;
                qn.AdapterLuid = ads[a].luid;
                qn.QueryNodeId = n;
                if (QueryStatistics(&qn) >= 0)
                    out[a].rt[n] = qn.QueryResult.NodeInformation.GlobalInformation.RunningTime;
            }
        }
        *ms = NowMs() - t0;
    };
    double m1 = 0, m2 = 0;
    SampleAll(s1, &m1);
    double w1 = NowMs();
    Sleep(1000);
    SampleAll(s2, &m2);
    double w2 = NowMs();
    double dt = (w2 - w1) / 1000.0;
    printf("  sample ms: %.4f / %.4f  wall=%.4fs\n", m1, m2, dt);
    for (size_t a = 0; a < ads.size(); ++a) {
        int64_t total = 0; int active = 0;
        for (uint32_t n = 0; n < ads[a].nodes; ++n) {
            int64_t d = s2[a].rt[n] - s1[a].rt[n];
            if (d > 0) ++active;
            total += d;
        }
        printf("  [%zu] luid=0x%08X name=\"%ls\": totalDelta=%lld activeNodes=%d\n",
               a, ads[a].luid.LowPart, ads[a].name, (long long)total, active);
    }

    // ---------------------------------------------------------------
    // C) 段身份：哪个 segment 是本地显存
    // ---------------------------------------------------------------
    printf("\n--- C) SEGMENT 身份（选定真实适配器）---\n");
    int realA = -1;
    for (size_t a = 0; a < ads.size(); ++a) if (ads[a].name[0]) { realA = (int)a; break; }
    if (realA < 0) { printf("no named adapter\n"); return 1; }
    printf("  真实适配器 index=%d name=\"%ls\" luid=0x%08X\n", realA, ads[realA].name, ads[realA].luid.LowPart);
    for (uint32_t s = 0; s < ads[realA].segs; ++s) {
        g2::QueryStatisticsLocal qs{};
        qs.Type = g2::T_SEGMENT;
        qs.AdapterLuid = ads[realA].luid;
        qs.QuerySegmentId = s;
        if (QueryStatistics(&qs) < 0) { printf("  seg %u fail\n", s); continue; }
        const auto& si = qs.QueryResult.SegmentInformation;
        printf("  seg %u: CommitLimit=%llu (%.1f MB) committed=%llu (%.1f MB) resident=%llu (%.1f MB) aperture=%u\n",
               s, (unsigned long long)si.CommitLimit, si.CommitLimit / 1048576.0,
               (unsigned long long)si.BytesCommitted, si.BytesCommitted / 1048576.0,
               (unsigned long long)si.BytesResident, si.BytesResident / 1048576.0, si.Aperture);
    }

    // ---------------------------------------------------------------
    // D) PDH 每实例读值调试 + 常驻查询成本 + 与 D3DKMT 对照
    // ---------------------------------------------------------------
    printf("\n--- D) PDH GPU Engine 每实例调试 ---\n");
    LUID rl = ads[realA].luid;
    wchar_t pattern[MAX_PATH];
    swprintf_s(pattern, L"\\GPU Engine(pid_*_luid_0x%08X_0x%08X_phys_0_*)\\Utilization Percentage",
               rl.HighPart, rl.LowPart);
    printf("  pattern=%ls\n", pattern);
    double e0 = NowMs();
    DWORD sz = 0;
    PDH_STATUS ps = PdhExpandWildCardPathW(nullptr, pattern, nullptr, &sz, 0);
    double e1 = NowMs();
    std::vector<wchar_t> b(sz + 16);
    DWORD sz2 = (DWORD)b.size();
    ps = PdhExpandWildCardPathW(nullptr, pattern, b.data(), &sz2, 0);
    double e2 = NowMs();
    std::vector<std::wstring> insts;
    for (size_t i = 0; i < b.size() && b[i];) {
        size_t l = wcslen(&b[i]);
        std::wstring f(&b[i], l);
        size_t lp = f.find(L'('), rp = f.rfind(L')');
        if (lp != std::wstring::npos && rp > lp) insts.push_back(f.substr(lp + 1, rp - lp - 1));
        i += l + 1;
    }
    printf("  expand: 1st %.3f ms (st=0x%08lX sz=%lu), 2nd %.3f ms, instances=%zu\n",
           e1 - e0, (unsigned long)ps, sz, e2 - e1, insts.size());

    PDH_HQUERY hq = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &hq) == ERROR_SUCCESS) {
        struct C { PDH_HCOUNTER h; std::wstring eng; };
        std::vector<C> cs;
        double ta0 = NowMs();
        for (auto& in : insts) {
            std::wstring p = L"\\GPU Engine(" + in + L")\\Utilization Percentage";
            PDH_HCOUNTER hc = nullptr;
            if (PdhAddEnglishCounterW(hq, p.c_str(), 0, &hc) != ERROR_SUCCESS) continue;
            std::wstring eng = in;
            size_t k = eng.find(L"engtype_");
            if (k != std::wstring::npos) eng = eng.substr(k + 8);
            cs.push_back({ hc, eng });
        }
        double ta1 = NowMs();
        double c1 = NowMs();
        PdhCollectQueryData(hq);
        double c1e = NowMs();
        Sleep(1000);
        double c2 = NowMs();
        PdhCollectQueryData(hq);
        double c2e = NowMs();
        double tr = NowMs();
        std::map<std::wstring, double> byEng;
        int readOk = 0;
        PDH_STATUS last = 0;
        for (auto& c : cs) {
            PDH_FMT_COUNTERVALUE v{};
            PDH_STATUS s = PdhGetFormattedCounterValue(c.h, PDH_FMT_DOUBLE, nullptr, &v);
            if (s == ERROR_SUCCESS) { byEng[c.eng] += v.doubleValue; ++readOk; }
            else last = s;
        }
        double tre = NowMs();
        printf("  addCounter %zu 个 %.3f ms | collect1 %.3f ms | collect2 %.3f ms | read %d/%zu %.3f ms (last status 0x%08lX)\n",
               cs.size(), ta1 - ta0, c1e - c1, c2e - c2, readOk, cs.size(), tre - tr, (unsigned long)last);
        for (auto& kv : byEng)
            printf("    PDH engtype %-16ls = %7.3f%%\n", kv.first.c_str(), kv.second);

        // 常驻查询：只 collect + 读阵列（真实每秒成本）
        std::vector<double> cc, rr;
        for (int i = 0; i < 30; ++i) {
            double t0 = NowMs();
            PdhCollectQueryData(hq);
            double t1 = NowMs();
            for (auto& c : cs) {
                PDH_FMT_COUNTERVALUE v{};
                PdhGetFormattedCounterValue(c.h, PDH_FMT_DOUBLE, nullptr, &v);
            }
            double t2 = NowMs();
            cc.push_back(t1 - t0); rr.push_back(t2 - t1);
            Sleep(50);
        }
        std::sort(cc.begin(), cc.end()); std::sort(rr.begin(), rr.end());
        double sc = 0, sr = 0;
        for (double x : cc) sc += x;
        for (double x : rr) sr += x;
        printf("  常驻(532 counter) collect avg=%.3f p95=%.3f max=%.3f | read avg=%.3f p95=%.3f max=%.3f ms\n",
               sc / cc.size(), cc[cc.size() * 95 / 100], cc.back(),
               sr / rr.size(), rr[rr.size() * 95 / 100], rr.back());
        PdhCloseQuery(hq);
    }

    // ---------------------------------------------------------------
    // E) Adapter Memory 常驻查询成本（复用同一查询）
    // ---------------------------------------------------------------
    printf("\n--- E) PDH Adapter Memory 常驻查询 ---\n");
    {
        const wchar_t* paths[] = {
            L"\\GPU Adapter Memory(*)\\Dedicated Usage",
            L"\\GPU Adapter Memory(*)\\Shared Usage",
        };
        for (auto p : paths) {
            PDH_HQUERY q = nullptr;
            PDH_HCOUNTER hc = nullptr;
            if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) continue;
            if (PdhAddEnglishCounterW(q, p, 0, &hc) != ERROR_SUCCESS) { PdhCloseQuery(q); continue; }
            PdhCollectQueryData(q);
            Sleep(1000);
            std::vector<double> costs;
            DWORD cnt = 0;
            std::vector<BYTE> buf;
            for (int i = 0; i < 20; ++i) {
                double t0 = NowMs();
                PdhCollectQueryData(q);
                DWORD bs = 0, c = 0;
                PdhGetFormattedCounterArrayW(hc, PDH_FMT_DOUBLE, &bs, &c, nullptr);
                buf.assign(bs + 16, 0);
                PdhGetFormattedCounterArrayW(hc, PDH_FMT_DOUBLE, &bs, &c,
                                             (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data());
                costs.push_back(NowMs() - t0);
                cnt = c;
                Sleep(50);
            }
            std::sort(costs.begin(), costs.end());
            double s = 0;
            for (double x : costs) s += x;
            printf("  %-42ls cnt=%lu avg=%.3f p95=%.3f max=%.3f ms\n", p, cnt,
                   s / costs.size(), costs[costs.size() * 95 / 100], costs.back());
            for (DWORD i = 0; i < cnt; ++i) {
                auto* it = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data() + i;
                printf("     %-56ls = %.2f MB\n", it->szName, it->FmtValue.doubleValue / 1048576.0);
            }
            PdhCloseQuery(q);
        }
    }

    // ---------------------------------------------------------------
    // F) 段 resident vs PDH dedicated usage 对照
    // ---------------------------------------------------------------
    printf("\n--- F) 段 resident/committed vs PDH Dedicated Usage ---\n");
    {
        g2::QueryStatisticsLocal qs{};
        qs.Type = g2::T_SEGMENT;
        qs.AdapterLuid = rl;
        qs.QuerySegmentId = 1;
        QueryStatistics(&qs);
        printf("  segment1: committed=%.2f MB resident=%.2f MB limit=%.2f MB\n",
               qs.QueryResult.SegmentInformation.BytesCommitted / 1048576.0,
               qs.QueryResult.SegmentInformation.BytesResident / 1048576.0,
               qs.QueryResult.SegmentInformation.CommitLimit / 1048576.0);

        // NODEMETADATA 得到的引擎名（如果 pack=1 成功）
        for (size_t a = 0; a < ads.size(); ++a) {
            if (!ads[a].name[0]) continue;
            printf("  adapter \"%ls\" engine names:\n", ads[a].name);
            for (uint32_t n = 0; n < ads[a].nodes; ++n) {
                g2::NodeMetadataQueryLocal nm{};
                nm.NodeOrdinalAndAdapterIndex = n;
                g2::QueryAdapterInfoLocal nq{ ads[a].h, g2::kQAINodeMetadata, &nm, sizeof(nm) };
                if (QueryAdapterInfo(&nq) < 0) continue;
                printf("    node %2u -> %-16ls (type %d)\n", n, nm.NodeData.FriendlyName,
                       (int)nm.NodeData.EngineType);
            }
            break;
        }
    }

    for (auto& a : ads) if (CloseAdapter) CloseAdapter(&a.h);
    FreeLibrary(gdi);
    printf("\n=== done ===\n");
    return 0;
}
