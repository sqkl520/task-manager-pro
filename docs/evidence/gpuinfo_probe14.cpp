// gpuinfo_probe14.cpp - 验证：进程退出后（句柄仍持有）D3DKMT 查询是否失败
// 这是能否从稳态路径去掉 GetProcessTimes 的前提。
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <d3dkmthk.h>

namespace g14 {
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
constexpr uint32_t T_ADAPTER = 0, T_PROCESS_ADAPTER = 2, T_PROCESS_NODE = 6;
constexpr uint32_t kQAIAdapterRegistryInfo = 8;
using PFN_EnumAdapters2 = NTSTATUS(NTAPI*)(EnumAdapters2Local*);
using PFN_QueryAdapterInfo = NTSTATUS(NTAPI*)(QueryAdapterInfoLocal*);
using PFN_QueryStatistics = NTSTATUS(NTAPI*)(const QueryStatisticsLocal*);
}

int main() {
    printf("=== probe14: 进程退出后 D3DKMT 查询行为 ===\n\n");
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    auto EnumAdapters2 = (g14::PFN_EnumAdapters2)GetProcAddress(gdi, "D3DKMTEnumAdapters2");
    auto QueryAdapterInfo = (g14::PFN_QueryAdapterInfo)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
    auto QueryStatistics = (g14::PFN_QueryStatistics)GetProcAddress(gdi, "D3DKMTQueryStatistics");
    g14::AdapterInfoLocal raw[16]{};
    g14::EnumAdapters2Local ea{}; ea.NumAdapters = 16; ea.pAdapters = raw;
    EnumAdapters2(&ea);
    LUID luid{};
    for (uint32_t i = 0; i < ea.NumAdapters && i < 16; ++i) {
        g14::AdapterRegistryInfoLocal reg{};
        g14::QueryAdapterInfoLocal q{ raw[i].hAdapter, g14::kQAIAdapterRegistryInfo, &reg, sizeof(reg) };
        if (QueryAdapterInfo(&q) < 0 || !reg.AdapterString[0]) continue;
        luid = raw[i].AdapterLuid;
        break;
    }

    // 找一个有 GPU 记录的进程（用 nvidia-smi 之类的，或直接遍历）
    // 这里用一个"短命但会用 GPU 的进程"：启动 gpuload.exe 然后等它退出
    printf("步骤 1: 启动一个 GPU 进程并抓句柄\n");
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    wchar_t appPath[] = L"D:\\Projects\\Task Manager Pro\\_probe\\gpuload.exe";
    wchar_t cmd[] = L"\"D:\\Projects\\Task Manager Pro\\_probe\\gpuload.exe\" 3 5000";
    if (!CreateProcessW(appPath, cmd, nullptr, nullptr, FALSE, 0, nullptr,
                        L"D:\\Projects\\Task Manager Pro\\_probe", &si, &pi)) {
        printf("CreateProcess 失败 %lu\n", GetLastError());
        return 1;
    }
    DWORD pid = pi.dwProcessId;
    printf("  pid=%lu\n", pid);
    CloseHandle(pi.hThread);

    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) { printf("OpenProcess 失败\n"); return 1; }

    auto Probe = [&](const char* label) {
        g14::QueryStatisticsLocal qa{};
        qa.Type = g14::T_PROCESS_ADAPTER; qa.AdapterLuid = luid; qa.hProcess = h;
        NTSTATUS s1 = QueryStatistics(&qa);
        g14::QueryStatisticsLocal qn{};
        qn.Type = g14::T_PROCESS_NODE; qn.AdapterLuid = luid; qn.hProcess = h; qn.QueryProcessNodeId = 0;
        NTSTATUS s2 = QueryStatistics(&qn);
        FILETIME c, e, k, u;
        BOOL gpt = GetProcessTimes(h, &c, &e, &k, &u);
        DWORD exitCode = 0;
        BOOL gec = GetExitCodeProcess(h, &exitCode);
        DWORD w = WaitForSingleObject(h, 0);
        printf("  %-22s PROC_ADAPTER=0x%08lX NODE0=0x%08lX GetProcessTimes=%d exitCode=%lu(gec=%d) wait=%lu\n",
               label, (unsigned long)s1, (unsigned long)s2, gpt, gec ? exitCode : 0, gec, w);
    };

    Probe("运行中");
    Sleep(1000);
    Probe("运行中(+1s)");
    printf("  等待进程退出...\n");
    WaitForSingleObject(pi.hProcess, 30000);
    Sleep(300);
    printf("  进程已退出（句柄 pi.hProcess 有效，测试 h 仍在）\n");
    Probe("退出后");
    Sleep(1500);
    Probe("退出后(+1.5s)");

    CloseHandle(h);
    CloseHandle(pi.hProcess);
    printf("\n=== done ===\n");
    return 0;
}
