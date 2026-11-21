// gpuinfo_probe15.cpp - 多 GPU 适配器枚举与命名来源验证
//
// 目的：确认"核显/虚拟显示器看不到"的真实原因，并逐个验证候选命名来源。
// 对每个 D3DKMTEnumAdapters2 返回的适配器打印：
//   LUID / hAdapter / NumOfSources
//   KMTQAITYPE_ADAPTERREGISTRYINFO(8)  -> AdapterString（当前实现唯一的取名来源）
//   KMTQAITYPE_ADAPTERTYPE(15)         -> SoftwareDevice / IndirectDisplayDevice / DisplaySupported
//   KMTQAITYPE_ADAPTERADDRESS(6)       -> PCI 位置
//   KMTQAITYPE_ADAPTERGUID(4)          -> GUID（用于查注册表 Video\{GUID}\0000\DriverDesc）
//   KMTQAITYPE_PHYSICALADAPTERPNPKEY(41) -> PnP 键
//   KMTQAITYPE_DRIVER_DESCRIPTION(65)  -> DriverDescription（Win10 1809+）
//   T_ADAPTER(0)                       -> NodeCount / NbSegments / VidPnSourceCount
//   T_ADAPTER 的每个 node 的引擎类型
// 再与 EnumDisplayDevices / 注册表 Video 分支 / DXGI 交叉对照。
//
// 编译（独立 console exe，不进主项目）：
//   cl /nologo /utf-8 /std:c++17 /EHsc /O2 /W3 /MD /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN
//      gpuinfo_probe15.cpp /Fe:gpuinfo_probe15.exe /link /SUBSYSTEM:CONSOLE dxgi.lib
#include <windows.h>
#include <winternl.h>
#include <objbase.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "dxgi.lib")

namespace mygpu {

struct D3DKMT_COUNTER { uint32_t Count; uint64_t Bytes; };
struct D3DKMT_DMA_PACKET_TYPE_INFORMATION {
    uint32_t PacketSubmited, PacketCompleted, PacketPreempted, PacketFaulted;
};
struct D3DKMT_QUEUE_PACKET_TYPE_INFORMATION { uint32_t PacketSubmited, PacketCompleted; };
struct D3DKMT_PACKET_INFORMATION {
    D3DKMT_QUEUE_PACKET_TYPE_INFORMATION QueuePacket[8];
    D3DKMT_DMA_PACKET_TYPE_INFORMATION   DmaPacket[4];
};
struct D3DKMT_PREEMPTION_INFORMATION { uint32_t PreemptionCounter[16]; };
struct D3DKMT_PROCESS_NODE_INFORMATION {
    int64_t  RunningTime;
    uint32_t ContextSwitch;
    D3DKMT_PREEMPTION_INFORMATION PreemptionStatistics;
    D3DKMT_PACKET_INFORMATION     PacketStatistics;
    uint64_t Reserved[8];
};
struct D3DKMT_NODE_INFORMATION {
    D3DKMT_PROCESS_NODE_INFORMATION GlobalInformation;
    D3DKMT_PROCESS_NODE_INFORMATION SystemInformation;
    uint8_t Tail[0x48];
};
struct D3DKMT_MEMORY_STATS { uint64_t TotalBytesEvicted; uint32_t AllocsCommitted, AllocsResident; };
struct D3DKMT_SEGMENT_INFORMATION {
    uint64_t CommitLimit, BytesCommitted, BytesResident;
    D3DKMT_MEMORY_STATS Memory;
    uint32_t Aperture;
    uint64_t TotalBytesEvictedByPriority[5];
    uint64_t SystemMemoryEndAddress, PowerFlags, SegmentProperties;
    uint64_t Reserved[5];
};
struct D3DKMT_PROCESS_ADAPTER_INFORMATION {
    uint32_t NbSegments, NodeCount, VidPnSourceCount, VirtualMemoryUsage;
    uint8_t  Rest[0x110 - 0x10];
};
struct D3DKMT_ADAPTER_INFORMATION {
    uint32_t NbSegments, NodeCount, VidPnSourceCount, VSyncEnabled, TdrDetectedCount;
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
struct D3DKMT_QUERYSTATISTICS {
    uint32_t Type;
    LUID     AdapterLuid;
    HANDLE   hProcess;
    D3DKMT_QUERYSTATISTICS_RESULT QueryResult;
    union { uint32_t QuerySegmentId, QueryProcessNodeId, QueryNodeId, QueryPhysAdapterIndex; };
};

enum : uint32_t { kStatsAdapter = 0, kStatsSegment = 3, kStatsNode = 5 };

struct D3DKMT_ADAPTERINFO_LOCAL {
    uint32_t hAdapter;
    LUID     AdapterLuid;
    uint32_t NumOfSources;
    BOOL     bPrecisePresentRegionsPreferred;
};
struct D3DKMT_ENUMADAPTERS2_LOCAL { uint32_t NumAdapters; D3DKMT_ADAPTERINFO_LOCAL* pAdapters; };
struct D3DKMT_QUERYADAPTERINFO_LOCAL {
    uint32_t hAdapter, Type; void* pPrivateDriverData; uint32_t PrivateDriverDataSize;
};
struct D3DKMT_ADAPTERREGISTRYINFO_LOCAL {
    wchar_t AdapterString[MAX_PATH], BiosString[MAX_PATH], DacType[MAX_PATH], ChipType[MAX_PATH];
};
struct D3DKMT_ADAPTERADDRESS_LOCAL { uint32_t BusNumber, DeviceNumber, FunctionNumber; };
struct D3DKMT_SEGMENTSIZEINFO_LOCAL {
    uint64_t DedicatedVideoMemorySize, DedicatedSystemMemorySize, SharedSystemMemorySize;
};
union D3DKMT_ADAPTERTYPE_LOCAL {
    struct {
        uint32_t RenderSupported:1, DisplaySupported:1, SoftwareDevice:1, PostDevice:1,
                 HybridDiscrete:1, HybridIntegrated:1, IndirectDisplayDevice:1,
                 Paravirtualized:1, Reserved:24;
    };
    uint32_t Value;
};
#pragma pack(push, 1)
struct DXGK_NODEMETADATA_LOCAL {
    int32_t EngineType; wchar_t FriendlyName[32]; uint32_t Flags;
    uint8_t GpuMmuSupported, IoMmuSupported;
};
struct D3DKMT_NODEMETADATA_LOCAL { uint32_t NodeOrdinalAndAdapterIndex; DXGK_NODEMETADATA_LOCAL NodeData; };
#pragma pack(pop)

// 41 KMTQAITYPE_PHYSICALADAPTERPNPKEY
struct D3DKMT_QUERY_PHYSICAL_ADAPTER_PNP_KEY_LOCAL {
    uint32_t PhysicalAdapterIndex;
    uint32_t PnPKeyType;        // 1=HARDWARE 2=SOFTWARE
    wchar_t* pDest;
    uint32_t* pCchDest;
};

using PFN_EnumAdapters2    = NTSTATUS (NTAPI*)(D3DKMT_ENUMADAPTERS2_LOCAL*);
using PFN_QueryAdapterInfo = NTSTATUS (NTAPI*)(D3DKMT_QUERYADAPTERINFO_LOCAL*);
using PFN_QueryStatistics  = NTSTATUS (NTAPI*)(const D3DKMT_QUERYSTATISTICS*);
using PFN_CloseAdapter     = NTSTATUS (NTAPI*)(const void*);

PFN_EnumAdapters2    pEnum = nullptr;
PFN_QueryAdapterInfo pQai  = nullptr;
PFN_QueryStatistics  pStat = nullptr;
PFN_CloseAdapter     pClose = nullptr;

} // namespace mygpu

using namespace mygpu;

static const wchar_t* EngineName(int t) {
    switch (t) {
        case 0: return L"Other";  case 1: return L"3D";       case 2: return L"VideoDecode";
        case 3: return L"VideoEncode"; case 4: return L"VideoProcessing";
        case 6: return L"Copy";   case 7: return L"Overlay";  default: return L"?";
    }
}

static bool ReadRegStr(HKEY k, const wchar_t* n, std::wstring& out) {
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(k, n, nullptr, &type, nullptr, &size) != ERROR_SUCCESS) return false;
    if (type != REG_SZ && type != REG_EXPAND_SZ) return false;
    if (size < sizeof(wchar_t) || size > 64 * 1024) return false;
    std::vector<wchar_t> b(size / sizeof(wchar_t) + 1, 0);
    if (RegQueryValueExW(k, n, nullptr, &type, (LPBYTE)b.data(), &size) != ERROR_SUCCESS) return false;
    out.assign(b.data());
    return true;
}

int wmain() {
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    pEnum  = (PFN_EnumAdapters2)GetProcAddress(gdi, "D3DKMTEnumAdapters2");
    pQai   = (PFN_QueryAdapterInfo)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
    pStat  = (PFN_QueryStatistics)GetProcAddress(gdi, "D3DKMTQueryStatistics");
    pClose = (PFN_CloseAdapter)GetProcAddress(gdi, "D3DKMTCloseAdapter");
    if (!pEnum || !pQai || !pStat) { wprintf(L"[FATAL] D3DKMT export missing\n"); return 1; }

    wprintf(L"===== 1. D3DKMTEnumAdapters2 =====\n");
    D3DKMT_ADAPTERINFO_LOCAL raw[16]{};
    D3DKMT_ENUMADAPTERS2_LOCAL ea{ 16, raw };
    NTSTATUS st = pEnum(&ea);
    wprintf(L"status=0x%08X NumAdapters=%u\n\n", (unsigned)st, ea.NumAdapters);
    const uint32_t count = ea.NumAdapters < 16 ? ea.NumAdapters : 16;

    for (uint32_t i = 0; i < count; ++i) {
        wprintf(L"--- adapter[%u] luid=0x%04X:%08X hAdapter=0x%X NumOfSources=%u ---\n",
                i, raw[i].AdapterLuid.HighPart, raw[i].AdapterLuid.LowPart,
                raw[i].hAdapter, raw[i].NumOfSources);

        // 8 ADAPTERREGISTRYINFO
        D3DKMT_ADAPTERREGISTRYINFO_LOCAL reg{};
        D3DKMT_QUERYADAPTERINFO_LOCAL q1{ raw[i].hAdapter, 8, &reg, sizeof(reg) };
        st = pQai(&q1);
        wprintf(L"  QAI(8 ADAPTERREGISTRYINFO) st=0x%08X AdapterString=[%s] Bios=[%s] Chip=[%s]\n",
                (unsigned)st, reg.AdapterString, reg.BiosString, reg.ChipType);

        // 15 ADAPTERTYPE
        D3DKMT_ADAPTERTYPE_LOCAL at{};
        D3DKMT_QUERYADAPTERINFO_LOCAL q2{ raw[i].hAdapter, 15, &at, sizeof(at) };
        st = pQai(&q2);
        wprintf(L"  QAI(15 ADAPTERTYPE) st=0x%08X value=0x%08X Render=%u Display=%u Software=%u "
                L"Post=%u HybridDisc=%u HybridInt=%u Indirect=%u ParaVirt=%u\n",
                (unsigned)st, at.Value, at.RenderSupported, at.DisplaySupported, at.SoftwareDevice,
                at.PostDevice, at.HybridDiscrete, at.HybridIntegrated,
                at.IndirectDisplayDevice, at.Paravirtualized);

        // 6 ADAPTERADDRESS
        D3DKMT_ADAPTERADDRESS_LOCAL ad{};
        D3DKMT_QUERYADAPTERINFO_LOCAL q3{ raw[i].hAdapter, 6, &ad, sizeof(ad) };
        st = pQai(&q3);
        wprintf(L"  QAI(6 ADAPTERADDRESS) st=0x%08X bus=%u dev=%u fn=%u\n",
                (unsigned)st, ad.BusNumber, ad.DeviceNumber, ad.FunctionNumber);

        // 4 ADAPTERGUID
        GUID guid{};
        D3DKMT_QUERYADAPTERINFO_LOCAL q4{ raw[i].hAdapter, 4, &guid, sizeof(guid) };
        st = pQai(&q4);
        wchar_t gs[64] = L"";
        if (st >= 0) StringFromGUID2(guid, gs, 64);
        wprintf(L"  QAI(4 ADAPTERGUID) st=0x%08X guid=%s\n", (unsigned)st, gs);

        // 41 PHYSICALADAPTERPNPKEY
        {
            wchar_t dest[512] = L"";
            uint32_t cch = 512;
            D3DKMT_QUERY_PHYSICAL_ADAPTER_PNP_KEY_LOCAL pk{};
            pk.PhysicalAdapterIndex = 0;
            pk.PnPKeyType = 1;   // HARDWARE
            pk.pDest = dest;
            pk.pCchDest = &cch;
            D3DKMT_QUERYADAPTERINFO_LOCAL q5{ raw[i].hAdapter, 41, &pk, sizeof(pk) };
            st = pQai(&q5);
            wprintf(L"  QAI(41 PNPKEY hardware) st=0x%08X cch=%u key=[%s]\n",
                    (unsigned)st, cch, dest);
            // 由 "...\Device Parameters" 的父键读 FriendlyName / DeviceDesc / Service
            {
                std::wstring k = dest;
                size_t pos = k.rfind(L"\\Device Parameters");
                if (pos != std::wstring::npos) {
                    std::wstring parent = k.substr(0, pos);
                    HKEY hk = nullptr;
                    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, parent.c_str(), 0, KEY_READ, &hk)
                        == ERROR_SUCCESS) {
                        std::wstring fn, ddsc, dsvc;
                        ReadRegStr(hk, L"FriendlyName", fn);
                        ReadRegStr(hk, L"DeviceDesc", ddsc);
                        ReadRegStr(hk, L"Service", dsvc);
                        wprintf(L"    reg FriendlyName=[%s] DeviceDesc=[%s] Service=[%s]\n",
                                fn.c_str(), ddsc.c_str(), dsvc.c_str());
                        RegCloseKey(hk);
                    }
                }
            }

            wchar_t dest2[512] = L"";
            uint32_t cch2 = 512;
            D3DKMT_QUERY_PHYSICAL_ADAPTER_PNP_KEY_LOCAL pk2{};
            pk2.PhysicalAdapterIndex = 0;
            pk2.PnPKeyType = 2;  // SOFTWARE
            pk2.pDest = dest2;
            pk2.pCchDest = &cch2;
            D3DKMT_QUERYADAPTERINFO_LOCAL q6{ raw[i].hAdapter, 41, &pk2, sizeof(pk2) };
            st = pQai(&q6);
            wprintf(L"  QAI(41 PNPKEY software) st=0x%08X cch=%u key=[%s]\n",
                    (unsigned)st, cch2, dest2);
            // software 键就是 Class\{4d36e968-...}\NNNN，可直接读 DriverDesc
            {
                std::wstring k = dest2;
                std::wstring prefix = L"\\REGISTRY\\MACHINE\\";
                if (k.compare(0, prefix.size(), prefix) == 0) k = k.substr(prefix.size());
                HKEY hk = nullptr;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, k.c_str(), 0, KEY_READ, &hk)
                    == ERROR_SUCCESS) {
                    std::wstring dd, dv, mfg;
                    ReadRegStr(hk, L"DriverDesc", dd);
                    ReadRegStr(hk, L"DriverVersion", dv);
                    ReadRegStr(hk, L"ProviderName", mfg);
                    wprintf(L"    class DriverDesc=[%s] ver=[%s] provider=[%s]\n",
                            dd.c_str(), dv.c_str(), mfg.c_str());
                    RegCloseKey(hk);
                } else {
                    wprintf(L"    [class key open failed]\n");
                }
            }
        }

        // 1 UMDRIVERNAME（wname[260] 结构）
        {
            wchar_t umd[260] = L"";
            D3DKMT_QUERYADAPTERINFO_LOCAL q{ raw[i].hAdapter, 1, umd, sizeof(umd) };
            st = pQai(&q);
            wprintf(L"  QAI(1 UMDRIVERNAME) st=0x%08X name=[%s]\n", (unsigned)st, umd);
        }
        // 60 QUERY_ADAPTER_UNIQUE_GUID
        {
            wchar_t ug[64] = L"";
            D3DKMT_QUERYADAPTERINFO_LOCAL q{ raw[i].hAdapter, 60, ug, sizeof(ug) };
            st = pQai(&q);
            wprintf(L"  QAI(60 ADAPTER_UNIQUE_GUID) st=0x%08X guid=[%s]\n", (unsigned)st, ug);
        }
        // 52 ADAPTERGUID_RENDER
        {
            GUID rg{};
            D3DKMT_QUERYADAPTERINFO_LOCAL q{ raw[i].hAdapter, 52, &rg, sizeof(rg) };
            st = pQai(&q);
            wchar_t rs[64] = L"";
            if (st >= 0) StringFromGUID2(rg, rs, 64);
            wprintf(L"  QAI(52 ADAPTERGUID_RENDER) st=0x%08X guid=%s\n", (unsigned)st, rs);
        }

        // 65 DRIVER_DESCRIPTION（4096 wchar 结构，Win10 1809+）
        {
            std::vector<uint8_t> buf(4096 * sizeof(wchar_t), 0);
            D3DKMT_QUERYADAPTERINFO_LOCAL q7{ raw[i].hAdapter, 65, buf.data(),
                                              (uint32_t)buf.size() };
            st = pQai(&q7);
            const wchar_t* desc = reinterpret_cast<const wchar_t*>(buf.data());
            wprintf(L"  QAI(65 DRIVER_DESCRIPTION) st=0x%08X desc=[%s]\n", (unsigned)st, desc);

            std::vector<uint8_t> buf2(4096 * sizeof(wchar_t), 0);
            D3DKMT_QUERYADAPTERINFO_LOCAL q8{ raw[i].hAdapter, 66, buf2.data(),
                                              (uint32_t)buf2.size() };
            st = pQai(&q8);
            const wchar_t* desc2 = reinterpret_cast<const wchar_t*>(buf2.data());
            wprintf(L"  QAI(66 DRIVER_DESCRIPTION_RENDER) st=0x%08X desc=[%s]\n",
                    (unsigned)st, desc2);
        }

        // 3 GETSEGMENTSIZE
        D3DKMT_SEGMENTSIZEINFO_LOCAL ss{};
        D3DKMT_QUERYADAPTERINFO_LOCAL q9{ raw[i].hAdapter, 3, &ss, sizeof(ss) };
        st = pQai(&q9);
        wprintf(L"  QAI(3 GETSEGMENTSIZE) st=0x%08X dedicated=%llu shared=%llu\n",
                (unsigned)st, (unsigned long long)ss.DedicatedVideoMemorySize,
                (unsigned long long)ss.SharedSystemMemorySize);

        // 0 T_ADAPTER
        D3DKMT_QUERYSTATISTICS qa{};
        qa.Type = kStatsAdapter;
        qa.AdapterLuid = raw[i].AdapterLuid;
        st = pStat(&qa);
        wprintf(L"  T_ADAPTER st=0x%08X NodeCount=%u NbSegments=%u VidPnSources=%u\n",
                (unsigned)st, qa.QueryResult.AdapterInformation.NodeCount,
                qa.QueryResult.AdapterInformation.NbSegments,
                qa.QueryResult.AdapterInformation.VidPnSourceCount);

        if (st >= 0) {
            uint32_t nodes = qa.QueryResult.AdapterInformation.NodeCount;
            if (nodes > 64) nodes = 64;
            wprintf(L"  engine types:");
            for (uint32_t n = 0; n < nodes; ++n) {
                D3DKMT_NODEMETADATA_LOCAL nm{};
                nm.NodeOrdinalAndAdapterIndex = n;
                D3DKMT_QUERYADAPTERINFO_LOCAL qn{ raw[i].hAdapter, 25, &nm, sizeof(nm) };
                if (pQai(&qn) >= 0) wprintf(L" %u:%s", n, EngineName(nm.NodeData.EngineType));
                else                wprintf(L" %u:ERR", n);
            }
            wprintf(L"\n");
        }
        wprintf(L"\n");
    }

    // 注册表 Video 分支
    wprintf(L"===== 2. HKLM\\SYSTEM\\CurrentControlSet\\Control\\Video 子键 =====\n");
    {
        const wchar_t* kVideo = L"SYSTEM\\CurrentControlSet\\Control\\Video";
        HKEY hV = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kVideo, 0, KEY_READ, &hV) == ERROR_SUCCESS) {
            for (int i = 0; i < 64; ++i) {
                wchar_t gname[256] = L"";
                DWORD glen = 256;
                if (RegEnumKeyExW(hV, i, gname, &glen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                    break;
                HKEY hG = nullptr;
                if (RegOpenKeyExW(hV, gname, 0, KEY_READ, &hG) != ERROR_SUCCESS) continue;
                // 子键名通常是 {GUID}，其下有 0000 / 0001 ...
                for (int j = 0; j < 8; ++j) {
                    wchar_t sub[16];
                    swprintf_s(sub, L"%04d", j);
                    HKEY hS = nullptr;
                    if (RegOpenKeyExW(hG, sub, 0, KEY_READ, &hS) != ERROR_SUCCESS) continue;
                    std::wstring dd, dv, desc;
                    ReadRegStr(hS, L"DriverDesc", dd);
                    ReadRegStr(hS, L"DriverVersion", dv);
                    ReadRegStr(hS, L"Description", desc);
                    wprintf(L"  %s\\%s  DriverDesc=[%s] Description=[%s] ver=[%s]\n",
                            gname, sub, dd.c_str(), desc.c_str(), dv.c_str());
                    RegCloseKey(hS);
                }
                RegCloseKey(hG);
            }
            RegCloseKey(hV);
        } else {
            wprintf(L"  [open failed]\n");
        }
    }

    // Display Class 分支（现有实现用的那个）
    wprintf(L"\n===== 3. HKLM\\...\\Control\\Class\\{4d36e968-...} 子键 =====\n");
    {
        const wchar_t* kCls = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
        HKEY hC = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kCls, 0, KEY_READ, &hC) == ERROR_SUCCESS) {
            for (int i = 0; i < 32; ++i) {
                wchar_t sub[16];
                swprintf_s(sub, L"%04d", i);
                HKEY hS = nullptr;
                if (RegOpenKeyExW(hC, sub, 0, KEY_READ, &hS) != ERROR_SUCCESS) continue;
                std::wstring dd, dv;
                ReadRegStr(hS, L"DriverDesc", dd);
                ReadRegStr(hS, L"DriverVersion", dv);
                std::wstring inf;
                ReadRegStr(hS, L"InfSection", inf);
                wprintf(L"  %s DriverDesc=[%s] ver=[%s] InfSection=[%s]\n",
                        sub, dd.c_str(), dv.c_str(), inf.c_str());
                RegCloseKey(hS);
            }
            RegCloseKey(hC);
        }
    }

    // EnumDisplayDevices
    wprintf(L"\n===== 4. EnumDisplayDevices =====\n");
    for (DWORD i = 0; ; ++i) {
        DISPLAY_DEVICEW dd{};
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(nullptr, i, &dd, 0)) break;
        wprintf(L"  adapter[%lu] DeviceString=[%s] DeviceName=[%s] flags=0x%08X\n",
                i, dd.DeviceString, dd.DeviceName, dd.StateFlags);
        for (DWORD j = 0; ; ++j) {
            DISPLAY_DEVICEW mon{};
            mon.cb = sizeof(mon);
            if (!EnumDisplayDevicesW(dd.DeviceName, j, &mon, 0)) break;
            wprintf(L"      monitor[%lu] DeviceString=[%s] DeviceID=[%s] flags=0x%08X\n",
                    j, mon.DeviceString, mon.DeviceID, mon.StateFlags);
        }
    }

    // DXGI
    wprintf(L"\n===== 5. DXGI EnumAdapters1 =====\n");
    {
        HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
        using PFN_CreateDXGIFactory1 = HRESULT (WINAPI*)(REFIID, void**);
        auto create = (PFN_CreateDXGIFactory1)GetProcAddress(dxgi, "CreateDXGIFactory1");
        // IID_IDXGIFactory1 = {770aae78-f26f-4dba-a829-253c83d1b387}
        static const GUID iidFactory1 =
            { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
        void* fac = nullptr;
        if (create && SUCCEEDED(create(iidFactory1, &fac)) && fac) {
            auto vt = *(void***)fac;
            // IDXGIFactory1: 0 QueryInterface 1 AddRef 2 Release 3 SetPrivateData ...
            //               12 EnumAdapters1
            using PFN_EnumAdapters1 = HRESULT (STDMETHODCALLTYPE*)(void*, UINT, void**);
            auto enumA = (PFN_EnumAdapters1)vt[12];
            for (UINT a = 0; ; ++a) {
                void* ad = nullptr;
                if (FAILED(enumA(fac, a, &ad)) || !ad) break;
                auto avt = *(void***)ad;
                // IDXGIAdapter::GetDesc 在 IDXGIAdapter1 的虚表第 10 项
                // IDXGIAdapter1: ... 8 GetDesc1, 9 GetDesc(?) 顺序：
                //   IUnknown(3) + IDXGIObject(4) => 7, then IDXGIAdapter::EnumOutputs=7,
                //   GetDesc=8, CheckInterfaceSupport=9, IDXGIAdapter1::GetDesc1=10
                struct DXGI_ADAPTER_DESC1_LOCAL {
                    wchar_t Description[128];
                    UINT VendorId, DeviceId, SubSysId, Revision;
                    SIZE_T DedicatedVideoMemory, DedicatedSystemMemory, SharedSystemMemory;
                    LUID AdapterLuid;
                    UINT Flags;
                };
                DXGI_ADAPTER_DESC1_LOCAL d{};
                using PFN_GetDesc1 = HRESULT (STDMETHODCALLTYPE*)(void*, DXGI_ADAPTER_DESC1_LOCAL*);
                auto getDesc1 = (PFN_GetDesc1)avt[10];
                HRESULT hr = getDesc1(ad, &d);
                if (SUCCEEDED(hr)) {
                    wprintf(L"  dxgi[%u] desc=[%s] luid=0x%04X:%08X vendor=0x%04X dev=0x%04X "
                            L"dedVid=%llu flags=0x%X\n",
                            a, d.Description, d.AdapterLuid.HighPart, d.AdapterLuid.LowPart,
                            d.VendorId, d.DeviceId,
                            (unsigned long long)d.DedicatedVideoMemory, d.Flags);
                } else {
                    wprintf(L"  dxgi[%u] GetDesc1 hr=0x%08X\n", a, (unsigned)hr);
                }
                ((ULONG (STDMETHODCALLTYPE*)(void*))avt[2])(ad);
            }
            ((ULONG (STDMETHODCALLTYPE*)(void*))vt[2])(fac);
        }
        FreeLibrary(dxgi);
    }

    // 清理
    for (uint32_t i = 0; i < count; ++i) {
        if (pClose) { uint32_t h = raw[i].hAdapter; pClose(&h); }
    }
    FreeLibrary(gdi);
    return 0;
}
