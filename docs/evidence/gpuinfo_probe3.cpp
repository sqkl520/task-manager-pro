// gpuinfo_probe3.cpp - 第三轮：口径交叉校验 + 耗时精测 + DirectX 版本成本
//
// 校验项：
//  A) D3DKMT NODE RunningTime 单位（100ns?）对 PDH \GPU Engine(*)\Running Time
//  B) D3DKMT 每引擎类型使用率 == PDH 每 engtype Utilization Percentage
//  C) segment resident vs PDH Dedicated/Shared Usage
//  D) 单次查询 / 完整采集耗时分布（200 次）
//  E) D3D11CreateDevice 冷/热成本 + D3D11 feature level
//  F) 硬件保留内存推导（注册表 qwMemorySize - GETSEGMENTSIZE）
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <map>
#include <algorithm>

#include <d3dkmthk.h>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "d3d11.lib")

namespace g3 {
typedef uint32_t KMT_HANDLE;
struct AdapterInfoLocal { KMT_HANDLE hAdapter; LUID AdapterLuid; uint32_t NumOfSources; BOOL bPrecise; };
struct EnumAdapters2Local { uint32_t NumAdapters; AdapterInfoLocal* pAdapters; };
struct QueryAdapterInfoLocal { KMT_HANDLE hAdapter; uint32_t Type; void* pData; uint32_t Size; };
struct AdapterRegistryInfoLocal { wchar_t AdapterString[MAX_PATH]; wchar_t BiosString[MAX_PATH]; wchar_t DacType[MAX_PATH]; wchar_t ChipType[MAX_PATH]; };
struct AdapterAddressLocal { uint32_t Bus, Device, Function; };
struct SegmentSizeInfoLocal { uint64_t DedicatedVideoMemorySize, DedicatedSystemMemorySize, SharedSystemMemorySize; };

#pragma pack(push, 1)
struct NodeMetadataLocal {
    int32_t EngineType; wchar_t FriendlyName[32]; uint32_t Flags;
    uint8_t GpuMmuSupported; uint8_t IoMmuSupported;
};
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
static double Pct(std::vector<double> v, double q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t i = (size_t)(v.size() * q);
    if (i >= v.size()) i = v.size() - 1;
    return v[i];
}
static double Avg(const std::vector<double>& v) {
    if (v.empty()) return 0;
    double s = 0; for (double x : v) s += x; return s / v.size();
}
static const wchar_t* EngName(int t) {
    switch (t) {
        case 0: return L"Other"; case 1: return L"3D"; case 2: return L"Video Decode";
        case 3: return L"Video Encode"; case 4: return L"Video Processing";
        case 5: return L"Scene Assembly"; case 6: return L"Copy";
        case 7: return L"Overlay"; case 8: return L"Crypto"; default: return L"?";
    }
}

int main() {
    printf("=== probe3: 口径交叉校验 + 耗时精测 ===\n\n");
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    auto EnumAdapters2 = (g3::PFN_EnumAdapters2)GetProcAddress(gdi, "D3DKMTEnumAdapters2");
    auto QueryAdapterInfo = (g3::PFN_QueryAdapterInfo)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
    auto QueryStatistics = (g3::PFN_QueryStatistics)GetProcAddress(gdi, "D3DKMTQueryStatistics");

    g3::AdapterInfoLocal raw[16]{};
    g3::EnumAdapters2Local ea{}; ea.NumAdapters = 16; ea.pAdapters = raw;
    EnumAdapters2(&ea);
    uint32_t nAd = ea.NumAdapters < 16 ? ea.NumAdapters : 16;

    // 找真实适配器（有注册表名的）
    LUID luid{}; g3::KMT_HANDLE hAd = 0; wchar_t adName[MAX_PATH] = L""; uint32_t nodes = 0, segs = 0;
    for (uint32_t i = 0; i < nAd; ++i) {
        g3::AdapterRegistryInfoLocal reg{};
        g3::QueryAdapterInfoLocal q{ raw[i].hAdapter, g3::kQAIAdapterRegistryInfo, &reg, sizeof(reg) };
        if (QueryAdapterInfo(&q) < 0 || !reg.AdapterString[0]) continue;
        g3::QueryStatisticsLocal qa{};
        qa.Type = g3::T_ADAPTER; qa.AdapterLuid = raw[i].AdapterLuid;
        if (QueryStatistics(&qa) < 0) continue;
        luid = raw[i].AdapterLuid; hAd = raw[i].hAdapter;
        wcsncpy_s(adName, reg.AdapterString, _TRUNCATE);
        nodes = qa.QueryResult.AdapterInformation.NodeCount;
        segs = qa.QueryResult.AdapterInformation.NbSegments;
        break;
    }
    if (!hAd) { printf("no real adapter\n"); return 1; }
    printf("adapter=\"%ls\" luid=0x%08X:0x%08X nodes=%u segs=%u\n\n", adName, luid.HighPart, luid.LowPart, nodes, segs);

    // 节点引擎类型
    std::vector<int> ntype(nodes, -1);
    for (uint32_t n = 0; n < nodes; ++n) {
        g3::NodeMetadataQueryLocal nm{};
        nm.NodeOrdinalAndAdapterIndex = n;
        g3::QueryAdapterInfoLocal q{ hAd, g3::kQAINodeMetadata, &nm, sizeof(nm) };
        if (QueryAdapterInfo(&q) >= 0) ntype[n] = (int)nm.NodeData.EngineType;
    }
    printf("node -> engine type: ");
    for (uint32_t n = 0; n < nodes; ++n) printf("%u:%ls ", n, EngName(ntype[n]));
    printf("\n\n");

    // ------------------------------------------------------------------
    // A/B) D3DKMT vs PDH 同时采样
    // ------------------------------------------------------------------
    printf("--- A/B) D3DKMT NODE vs PDH（同时采样 1s 窗口）---\n");
    // 准备 PDH：ExpandWildCard 一次（成本高，只做一次），逐实例加 2 个计数器
    wchar_t pat[MAX_PATH * 2];
    swprintf_s(pat, L"\\GPU Engine(pid_*_luid_0x%08X_0x%08X_*)\\Running Time", luid.HighPart, luid.LowPart);
    DWORD esz = 0;
    PdhExpandWildCardPathW(nullptr, pat, nullptr, &esz, 0);
    std::vector<wchar_t> ebuf(esz + 16);
    DWORD esz2 = (DWORD)ebuf.size();
    PdhExpandWildCardPathW(nullptr, pat, ebuf.data(), &esz2, 0);
    std::vector<std::wstring> insts;
    for (size_t i = 0; i < ebuf.size() && ebuf[i];) {
        size_t l = wcslen(&ebuf[i]);
        std::wstring f(&ebuf[i], l);
        size_t lp = f.find(L'('), rp = f.rfind(L')');
        if (lp != std::wstring::npos && rp > lp) insts.push_back(f.substr(lp + 1, rp - lp - 1));
        i += l + 1;
    }
    printf("PDH instances for this LUID = %zu\n", insts.size());

    PDH_HQUERY hq = nullptr;
    struct PC { PDH_HCOUNTER rt; int eng; };
    std::vector<PC> pcs;
    PdhOpenQueryW(nullptr, 0, &hq);
    for (auto& in : insts) {
        std::wstring p = L"\\GPU Engine(" + in + L")\\Running Time";
        PDH_HCOUNTER hc = nullptr;
        if (PdhAddEnglishCounterW(hq, p.c_str(), 0, &hc) != ERROR_SUCCESS) continue;
        std::wstring e = in;
        size_t k = e.find(L"engtype_");
        int eng = -1;
        if (k != std::wstring::npos) {
            std::wstring t = e.substr(k + 8);
            if (t == L"3D") eng = 1; else if (t == L"Copy") eng = 6;
            else if (t == L"VideoDecode") eng = 2; else if (t == L"VideoEncode") eng = 3;
            else if (t == L"LegacyOverlay") eng = 7; else if (t == L"Security") eng = 0;
            else if (t.front() == L'S') eng = 0; else if (t == L"VR") eng = 0;
        }
        pcs.push_back({ hc, eng });
    }
    printf("PDH counters added = %zu\n", pcs.size());

    // 同时取 D3DKMT node 初始值 + PDH 第一次 collect
    std::vector<int64_t> rt0(nodes, 0), rt1(nodes, 0);
    PdhCollectQueryData(hq);
    double w1 = NowMs();
    for (uint32_t n = 0; n < nodes; ++n) {
        g3::QueryStatisticsLocal qn{}; qn.Type = g3::T_NODE; qn.AdapterLuid = luid; qn.QueryNodeId = n;
        if (QueryStatistics(&qn) >= 0) rt0[n] = qn.QueryResult.NodeInformation.GlobalInformation.RunningTime;
    }
    Sleep(1000);
    for (uint32_t n = 0; n < nodes; ++n) {
        g3::QueryStatisticsLocal qn{}; qn.Type = g3::T_NODE; qn.AdapterLuid = luid; qn.QueryNodeId = n;
        if (QueryStatistics(&qn) >= 0) rt1[n] = qn.QueryResult.NodeInformation.GlobalInformation.RunningTime;
    }
    PdhCollectQueryData(hq);
    double w2 = NowMs();
    double dtSec = (w2 - w1) / 1000.0;
    printf("wall = %.4f s\n", dtSec);

    std::map<int, int64_t> d3dByEng;
    printf("  per-node delta (D3DKMT):\n");
    for (uint32_t n = 0; n < nodes; ++n) {
        int64_t d = rt1[n] - rt0[n];
        if (d < 0) d = 0;
        if (d) printf("    node %2u %-16ls delta=%10lld -> %7.3f%%\n", n, EngName(ntype[n]),
                      (long long)d, d / (dtSec * 1e7) * 100.0);
        d3dByEng[ntype[n]] += d;
    }
    std::map<int, int64_t> pdhByEng;
    int64_t pdhTotal = 0;
    for (auto& pc : pcs) {
        PDH_FMT_COUNTERVALUE v{};
        if (PdhGetFormattedCounterValue(pc.rt, PDH_FMT_DOUBLE, nullptr, &v) != ERROR_SUCCESS) continue;
        int64_t ns = (int64_t)(v.doubleValue * 100.0);  // double 是 100ns 单位
        pdhByEng[pc.eng] += ns;
        pdhTotal += ns;
    }
    printf("  %-16ls %14s %14s %14s\n", "engine", "D3DKMT(delta)", "PDH(delta)", "ratio");
    int64_t d3dTotal = 0;
    for (auto& kv : d3dByEng) d3dTotal += kv.second;
    for (auto& kv : d3dByEng) {
        int64_t p = pdhByEng.count(kv.first) ? pdhByEng[kv.first] : 0;
        double ratio = p ? (double)kv.second / (double)p : 0;
        printf("  %-16ls %14lld %14lld %14.4f\n", EngName(kv.first),
               (long long)kv.second, (long long)p, ratio);
    }
    printf("  %-16ls %14lld %14lld %14.4f\n", "TOTAL", (long long)d3dTotal, (long long)pdhTotal,
           pdhTotal ? (double)d3dTotal / (double)pdhTotal : 0);
    printf("  D3DKMT 每引擎使用率:\n");
    for (auto& kv : d3dByEng)
        printf("    %-16ls %7.3f%%  (PDH %7.3f%%)\n", EngName(kv.first),
               kv.second / (dtSec * 1e7) * 100.0,
               pdhByEng.count(kv.first) ? pdhByEng[kv.first] / (dtSec * 1e7) * 100.0 : 0.0);
    PdhCloseQuery(hq);

    // ------------------------------------------------------------------
    // C) segment resident vs PDH Adapter Memory
    // ------------------------------------------------------------------
    printf("\n--- C) segment resident vs PDH Adapter Memory（同时读）---\n");
    {
        PDH_HQUERY q1 = nullptr, q2 = nullptr;
        PDH_HCOUNTER d1 = nullptr, s1 = nullptr;
        PdhOpenQueryW(nullptr, 0, &q1);
        PdhOpenQueryW(nullptr, 0, &q2);
        PdhAddEnglishCounterW(q1, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &d1);
        PdhAddEnglishCounterW(q2, L"\\GPU Adapter Memory(*)\\Shared Usage", 0, &s1);
        PdhCollectQueryData(q1); PdhCollectQueryData(q2);
        Sleep(1000);
        PdhCollectQueryData(q1); PdhCollectQueryData(q2);

        auto ReadArr = [](PDH_HCOUNTER hc, const wchar_t* luidSub, double* mb) {
            DWORD bs = 0, c = 0;
            PdhGetFormattedCounterArrayW(hc, PDH_FMT_DOUBLE, &bs, &c, nullptr);
            std::vector<BYTE> b(bs + 16);
            if (PdhGetFormattedCounterArrayW(hc, PDH_FMT_DOUBLE, &bs, &c,
                    (PDH_FMT_COUNTERVALUE_ITEM_W*)b.data()) != ERROR_SUCCESS) return;
            for (DWORD i = 0; i < c; ++i) {
                auto* it = (PDH_FMT_COUNTERVALUE_ITEM_W*)b.data() + i;
                if (wcsstr(it->szName, luidSub)) *mb = it->FmtValue.doubleValue;
            }
        };
        wchar_t sub[64];
        swprintf_s(sub, L"luid_0x%08X_0x%08X_phys_0", luid.HighPart, luid.LowPart);
        double pdhDed = -1, pdhShr = -1;
        ReadArr(d1, sub, &pdhDed);
        ReadArr(s1, sub, &pdhShr);

        uint64_t segResident[8] = {}, segCommitted[8] = {}, segLimit[8] = {};
        uint32_t segAperture[8] = {};
        for (uint32_t s = 0; s < segs && s < 8; ++s) {
            g3::QueryStatisticsLocal qs{}; qs.Type = g3::T_SEGMENT; qs.AdapterLuid = luid; qs.QuerySegmentId = s;
            if (QueryStatistics(&qs) < 0) continue;
            segResident[s] = qs.QueryResult.SegmentInformation.BytesResident;
            segCommitted[s] = qs.QueryResult.SegmentInformation.BytesCommitted;
            segLimit[s] = qs.QueryResult.SegmentInformation.CommitLimit;
            segAperture[s] = qs.QueryResult.SegmentInformation.Aperture;
        }
        printf("  PDH Dedicated=%.2f MB  Shared=%.2f MB\n",
               pdhDed / 1048576.0, pdhShr / 1048576.0);
        for (uint32_t s = 0; s < segs && s < 8; ++s)
            printf("  seg %u aperture=%u limit=%.1f MB committed=%.2f MB resident=%.2f MB\n",
                   s, segAperture[s], segLimit[s] / 1048576.0,
                   segCommitted[s] / 1048576.0, segResident[s] / 1048576.0);
        // 各口径组合对照
        printf("  对照:\n");
        for (uint32_t s = 0; s < segs && s < 8; ++s) {
            printf("    seg%u resident vs PDH Dedicated: %.2f vs %.2f (diff %.2f MB)\n",
                   s, segResident[s] / 1048576.0, pdhDed / 1048576.0,
                   segResident[s] / 1048576.0 - pdhDed / 1048576.0);
        }
        uint64_t nonLocalRes = 0, nonLocalCom = 0;
        for (uint32_t s = 0; s < segs && s < 8; ++s)
            if (segAperture[s]) { nonLocalRes += segResident[s]; nonLocalCom += segCommitted[s]; }
        printf("    nonLocal(ap=1) resident=%.2f committed=%.2f  vs PDH Shared=%.2f\n",
               nonLocalRes / 1048576.0, nonLocalCom / 1048576.0, pdhShr / 1048576.0);
        PdhCloseQuery(q1); PdhCloseQuery(q2);
    }

    // ------------------------------------------------------------------
    // D) 耗时精测
    // ------------------------------------------------------------------
    printf("\n--- D) 耗时精测 ---\n");
    {
        std::vector<double> one, full, oneQ;
        for (int i = 0; i < 300; ++i) {
            double t0 = NowMs();
            g3::QueryStatisticsLocal qn{}; qn.Type = g3::T_NODE; qn.AdapterLuid = luid; qn.QueryNodeId = 0;
            QueryStatistics(&qn);
            oneQ.push_back(NowMs() - t0);
        }
        for (int i = 0; i < 300; ++i) {
            double t0 = NowMs();
            g3::QueryStatisticsLocal qa{}; qa.Type = g3::T_ADAPTER; qa.AdapterLuid = luid;
            QueryStatistics(&qa);
            for (uint32_t n = 0; n < nodes; ++n) {
                g3::QueryStatisticsLocal qn{}; qn.Type = g3::T_NODE; qn.AdapterLuid = luid; qn.QueryNodeId = n;
                QueryStatistics(&qn);
            }
            for (uint32_t s = 0; s < segs; ++s) {
                g3::QueryStatisticsLocal qs{}; qs.Type = g3::T_SEGMENT; qs.AdapterLuid = luid; qs.QuerySegmentId = s;
                QueryStatistics(&qs);
            }
            full.push_back(NowMs() - t0);
        }
        // 含 nodemetadata 的完整 init 成本
        double t0 = NowMs();
        for (uint32_t n = 0; n < nodes; ++n) {
            g3::NodeMetadataQueryLocal nm{}; nm.NodeOrdinalAndAdapterIndex = n;
            g3::QueryAdapterInfoLocal q{ hAd, g3::kQAINodeMetadata, &nm, sizeof(nm) };
            QueryAdapterInfo(&q);
        }
        double nmMs = NowMs() - t0;
        printf("  单次 T_NODE 查询: avg=%.4f p50=%.4f p95=%.4f max=%.4f ms\n",
               Avg(oneQ), Pct(oneQ, 0.5), Pct(oneQ, 0.95), oneQ.empty() ? 0 : *std::max_element(oneQ.begin(), oneQ.end()));
        printf("  完整 Collect(adapter+%u node+%u seg): avg=%.4f p50=%.4f p95=%.4f max=%.4f ms\n",
               nodes, segs, Avg(full), Pct(full, 0.5), Pct(full, 0.95),
               full.empty() ? 0 : *std::max_element(full.begin(), full.end()));
        printf("  %u 次 NODEMETADATA: %.4f ms（仅 Init 一次）\n", nodes, nmMs);
    }

    // ------------------------------------------------------------------
    // E) D3D11 feature level 冷/热成本
    // ------------------------------------------------------------------
    printf("\n--- E) D3D11CreateDevice 成本 ---\n");
    {
        double tl0 = NowMs();
        HMODULE d3d = LoadLibraryW(L"d3d11.dll");
        double tl1 = NowMs();
        printf("  LoadLibrary(d3d11.dll) = %.3f ms (handle=%p)\n", tl1 - tl0, (void*)d3d);
        auto pCreate = d3d ? (decltype(&D3D11CreateDevice))GetProcAddress(d3d, "D3D11CreateDevice") : nullptr;
        printf("  D3D11CreateDevice = %p\n", (void*)pCreate);
        D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
        };
        for (int i = 0; i < 3; ++i) {
            D3D_FEATURE_LEVEL got{};
            ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
            double t0 = NowMs();
            HRESULT hr = pCreate ? pCreate(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                           levels, _countof(levels), D3D11_SDK_VERSION,
                                           &dev, &got, &ctx) : E_FAIL;
            double dt = NowMs() - t0;
            printf("  attempt %d: hr=0x%08lX FL=0x%04X  %.3f ms\n", i, (unsigned long)hr, (unsigned)got, dt);
            if (ctx) ctx->Release();
            if (dev) dev->Release();
        }
        if (d3d) FreeLibrary(d3d);
    }

    // ------------------------------------------------------------------
    // F) 硬件保留内存推导
    // ------------------------------------------------------------------
    printf("\n--- F) 硬件保留内存推导 ---\n");
    {
        g3::SegmentSizeInfoLocal ss{};
        g3::QueryAdapterInfoLocal q{ hAd, g3::kQAIGetSegmentSize, &ss, sizeof(ss) };
        QueryAdapterInfo(&q);
        const wchar_t* base = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
        HKEY hk = nullptr;
        uint64_t qw = 0;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, base, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
            for (int i = 0; i < 16; ++i) {
                wchar_t sub[8]; swprintf_s(sub, L"%04d", i);
                HKEY hs = nullptr;
                if (RegOpenKeyExW(hk, sub, 0, KEY_READ, &hs) != ERROR_SUCCESS) continue;
                wchar_t desc[256] = L""; DWORD sz = sizeof(desc);
                RegQueryValueExW(hs, L"DriverDesc", nullptr, nullptr, (LPBYTE)desc, &sz);
                uint64_t t = 0; sz = sizeof(t);
                RegQueryValueExW(hs, L"HardwareInformation.qwMemorySize", nullptr, nullptr, (LPBYTE)&t, &sz);
                if (_wcsicmp(desc, adName) == 0) qw = t;
                RegCloseKey(hs);
            }
            RegCloseKey(hk);
        }
        printf("  registry qwMemorySize = %llu (%.2f MiB)\n", (unsigned long long)qw, qw / 1048576.0);
        printf("  GETSEGMENTSIZE.DedicatedVideoMemorySize = %llu (%.2f MiB)\n",
               (unsigned long long)ss.DedicatedVideoMemorySize, ss.DedicatedVideoMemorySize / 1048576.0);
        if (qw > ss.DedicatedVideoMemorySize)
            printf("  => hardwareReserved = %llu bytes (%.2f MiB)\n",
                   (unsigned long long)(qw - ss.DedicatedVideoMemorySize),
                   (qw - ss.DedicatedVideoMemorySize) / 1048576.0);
        else printf("  => qwMemorySize 不大于 Dedicated，无法推导\n");
        printf("  SharedSystemMemorySize = %llu (%.2f MiB)\n",
               (unsigned long long)ss.SharedSystemMemorySize, ss.SharedSystemMemorySize / 1048576.0);
    }

    printf("\n=== done ===\n");
    return 0;
}
