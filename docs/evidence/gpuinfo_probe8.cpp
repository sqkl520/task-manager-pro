// gpuinfo_probe8.cpp - 第八轮：KMD 驱动版本 / DXGI 工厂成本 / ADAPTERPERFDATA
#include <windows.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdint>
#include <vector>

#include <d3dkmthk.h>
#pragma comment(lib, "dxgi.lib")

namespace g8 {
typedef uint32_t KMT_HANDLE;
struct AdapterInfoLocal { KMT_HANDLE hAdapter; LUID AdapterLuid; uint32_t NumOfSources; BOOL bPrecise; };
struct EnumAdapters2Local { uint32_t NumAdapters; AdapterInfoLocal* pAdapters; };
struct QueryAdapterInfoLocal { KMT_HANDLE hAdapter; uint32_t Type; void* pData; uint32_t Size; };
struct AdapterRegistryInfoLocal { wchar_t AdapterString[MAX_PATH]; wchar_t BiosString[MAX_PATH]; wchar_t DacType[MAX_PATH]; wchar_t ChipType[MAX_PATH]; };
struct AdapterAddressLocal { uint32_t Bus, Device, Function; };
#pragma pack(push, 1)
struct NodeMetadataLocal { int32_t EngineType; wchar_t FriendlyName[32]; uint32_t Flags; uint8_t mmu, iommu; };
struct NodeMetadataQueryLocal { uint32_t NodeOrdinalAndAdapterIndex; NodeMetadataLocal NodeData; };
#pragma pack(pop)
constexpr uint32_t kQAIGetSegmentSize = 3, kQAIAdapterAddress = 6, kQAIAdapterRegistryInfo = 8;
constexpr uint32_t kQAINodeMetadata = 25, kQAIPhysicalAdapterPNPKey = 41;
constexpr uint32_t kQAIKmdDriverVersion = 49, kQAINodePerfData = 61, kQAIAdapterPerfData = 62;
constexpr uint32_t kQAIAdapterPerfDataCaps = 63, kQAIGpuVersion = 64;
using PFN_EnumAdapters2 = NTSTATUS(NTAPI*)(EnumAdapters2Local*);
using PFN_QueryAdapterInfo = NTSTATUS(NTAPI*)(QueryAdapterInfoLocal*);
}
static double NowMs() {
    static LARGE_INTEGER f{}; static bool i = false;
    if (!i) { QueryPerformanceFrequency(&f); i = true; }
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

int main() {
    printf("=== probe8: 驱动版本/PNP/PerfData/DXGI 成本 ===\n\n");
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    auto EnumAdapters2 = (g8::PFN_EnumAdapters2)GetProcAddress(gdi, "D3DKMTEnumAdapters2");
    auto QueryAdapterInfo = (g8::PFN_QueryAdapterInfo)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
    g8::AdapterInfoLocal raw[16]{};
    g8::EnumAdapters2Local ea{}; ea.NumAdapters = 16; ea.pAdapters = raw;
    EnumAdapters2(&ea);
    g8::KMT_HANDLE hAd = 0; uint32_t nodes = 0;
    for (uint32_t i = 0; i < ea.NumAdapters && i < 16; ++i) {
        g8::AdapterRegistryInfoLocal reg{};
        g8::QueryAdapterInfoLocal q{ raw[i].hAdapter, g8::kQAIAdapterRegistryInfo, &reg, sizeof(reg) };
        if (QueryAdapterInfo(&q) < 0 || !reg.AdapterString[0]) continue;
        hAd = raw[i].hAdapter;
        break;
    }
    if (!hAd) { printf("no adapter\n"); return 1; }

    // KMD driver version
    for (int k = 0; k < 2; ++k) {
        LARGE_INTEGER v{};
        g8::QueryAdapterInfoLocal q{ hAd, g8::kQAIKmdDriverVersion, &v, sizeof(v) };
        double t0 = NowMs();
        NTSTATUS s = QueryAdapterInfo(&q);
        printf("KMD_DRIVER_VERSION: st=0x%08lX value=0x%016llX (hi=%u lo=%u) %.4f ms\n",
               (unsigned long)s, (unsigned long long)v.QuadPart,
               (unsigned)(v.QuadPart >> 32), (unsigned)(v.QuadPart & 0xFFFFFFFF), NowMs() - t0);
    }
    // PNP key
    {
        wchar_t key[MAX_PATH * 2] = L"";
        g8::QueryAdapterInfoLocal q{ hAd, g8::kQAIPhysicalAdapterPNPKey, key, sizeof(key) };
        NTSTATUS s = QueryAdapterInfo(&q);
        printf("PHYSICALADAPTERPNPKEY: st=0x%08lX key=\"%ls\"\n", (unsigned long)s, key);
    }
    // GPU version (bios/architecture)
    {
        DXGK_GPUVERSION gv{};
        g8::QueryAdapterInfoLocal q{ hAd, g8::kQAIGpuVersion, &gv, sizeof(gv) };
        NTSTATUS s = QueryAdapterInfo(&q);
        printf("GPUVERSION: st=0x%08lX bios=\"%ls\" arch=\"%ls\"\n", (unsigned long)s,
               gv.BiosVersion, gv.GpuArchitecture);
    }
    // ADAPTER_PERFDATA (温度/功耗/频率)
    {
        D3DKMT_ADAPTER_PERFDATA pd{};
        g8::QueryAdapterInfoLocal q{ hAd, g8::kQAIAdapterPerfData, &pd, sizeof(pd) };
        NTSTATUS s = QueryAdapterInfo(&q);
        printf("ADAPTERPERFDATA: st=0x%08lX memFreq=%llu MHz max=%llu MHz fan=%u rpm power=%u/10%% temp=%u (%.1f C) pcieBW=%llu\n",
               (unsigned long)s, (unsigned long long)(pd.MemoryFrequency / 1000000),
               (unsigned long long)(pd.MaxMemoryFrequency / 1000000), pd.FanRPM, pd.Power,
               pd.Temperature, pd.Temperature / 10.0, (unsigned long long)pd.PCIEBandwidth);
    }
    // ADAPTER_PERFDATA_CAPS
    {
        D3DKMT_ADAPTER_PERFDATACAPS caps{};
        g8::QueryAdapterInfoLocal q{ hAd, g8::kQAIAdapterPerfDataCaps, &caps, sizeof(caps) };
        NTSTATUS s = QueryAdapterInfo(&q);
        printf("ADAPTERPERFDATACAPS: st=0x%08lX maxMemBW=%llu maxPCIEBW=%llu maxFan=%u tempMax=%u tempWarn=%u\n",
               (unsigned long)s, (unsigned long long)caps.MaxMemoryBandwidth,
               (unsigned long long)caps.MaxPCIEBandwidth, caps.MaxFanRPM,
               caps.TemperatureMax, caps.TemperatureWarning);
    }
    // NODE_PERFDATA for each node（频率）
    {
        for (uint32_t n = 0; n < 3; ++n) {
            D3DKMT_NODE_PERFDATA np{};
            np.NodeOrdinal = n;
            np.PhysicalAdapterIndex = 0;
            g8::QueryAdapterInfoLocal q{ hAd, g8::kQAINodePerfData, &np, sizeof(np) };
            NTSTATUS s = QueryAdapterInfo(&q);
            printf("NODEPERFDATA node %u: st=0x%08lX freq=%llu MHz max=%llu MHz\n",
                   n, (unsigned long)s, (unsigned long long)(np.Frequency / 1000000),
                   (unsigned long long)(np.MaxFrequency / 1000000));
        }
    }
    (void)nodes;

    // DXGI CreateDXGIFactory1 成本
    printf("\nDXGI 成本:\n");
    {
        std::vector<double> costs;
        for (int i = 0; i < 10; ++i) {
            double t0 = NowMs();
            IDXGIFactory1* f = nullptr;
            HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f);
            costs.push_back(NowMs() - t0);
            if (i == 0) printf("  CreateDXGIFactory1 hr=0x%08lX\n", (unsigned long)hr);
            if (f) f->Release();
        }
        for (double c : costs) printf("  %.3f ms\n", c);
        HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
        printf("  dxgi.dll 已加载=%p\n", (void*)dxgi);
        if (dxgi) FreeLibrary(dxgi);
    }
    printf("\n=== done ===\n");
    return 0;
}
