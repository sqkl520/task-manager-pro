// perfsrc.cpp - 性能页各数据源的实测成本
#define _WIN32_WINNT 0x0A00
// 目标：给"性能页"每一项数据给出经过本机实测的调用成本与可行性判定
#include <windows.h>
#include <winternl.h>
#include <ntstatus.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <powrprof.h>
#include <iphlpapi.h>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "advapi32.lib")

// ---------- 计时 ----------
static LARGE_INTEGER g_freq;
static double NowMs() {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / double(g_freq.QuadPart);
}

struct Stats {
    std::vector<double> v;
    void Add(double d) { v.push_back(d); }
    void Report(const wchar_t* name, const wchar_t* note = L"") {
        if (v.empty()) { wprintf(L"  %-40ls : <no data>\n", name); return; }
        std::sort(v.begin(), v.end());
        double sum = 0; for (double d : v) sum += d;
        double avg = sum / v.size();
        double p50 = v[v.size() * 50 / 100];
        double p95 = v[std::min(v.size() - 1, v.size() * 95 / 100)];
        double mx = v.back();
        wprintf(L"  %-40ls : avg %8.3f ms  p50 %8.3f  p95 %8.3f  max %8.3f  %ls\n",
                name, avg, p50, p95, mx, note);
    }
};

typedef NTSTATUS (NTAPI* NtQSI_t)(ULONG, PVOID, ULONG, PULONG);
static NtQSI_t g_ntqsi = nullptr;

// ---------- 1. CPU 总/每核利用率 ----------
// SystemProcessorPerformanceInformation = 8
typedef struct _SPPI {
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG InterruptCount;
} SPPI;

static void TestCpuUtil() {
    wprintf(L"\n=== 1. CPU 利用率 ===\n");
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const ULONG nCpu = si.dwNumberOfProcessors;

    // 类 8 需要精确 48 字节 × 核数
    std::vector<SPPI> a(nCpu), b(nCpu);
    ULONG need = 0;
    NTSTATUS st = g_ntqsi(8, a.data(), nCpu * sizeof(SPPI), &need);
    wprintf(L"  NtQSI(8) status=0x%08X need=%u bytes  (exact size = %zu)\n",
            st, need, nCpu * sizeof(SPPI));

    Stats s;
    for (int i = 0; i < 30; ++i) {
        double t0 = NowMs();
        g_ntqsi(8, b.data(), nCpu * sizeof(SPPI), &need);
        s.Add(NowMs() - t0);
    }
    s.Report(L"NtQSI(8) SystemProcessorPerfInfo", L"(每核，1 次拿全部)");

    // 试给大缓冲
    std::vector<BYTE> big(64 * 1024);
    st = g_ntqsi(8, big.data(), 64 * 1024, &need);
    wprintf(L"  NtQSI(8) with 64KB buffer -> status=0x%08X need=%u  %ls\n",
            st, need,
            st == STATUS_INFO_LENGTH_MISMATCH ? L"(给大会返回 LENGTH_MISMATCH!)" : L"");

    // 对比 PDH \Processor(_Total)\% Processor Time
    PDH_HQUERY q = nullptr;
    PDH_HCOUNTER c = nullptr;
    PdhOpenQueryW(nullptr, 0, &q);
    PdhAddEnglishCounterW(q, L"\\Processor(_Total)\\% Processor Time", 0, &c);
    PdhCollectQueryData(q);
    Stats sPdh;
    for (int i = 0; i < 20; ++i) {
        double t0 = NowMs();
        PdhCollectQueryData(q);
        sPdh.Add(NowMs() - t0);
        Sleep(5);
    }
    sPdh.Report(L"PDH \\Processor(_Total)\\% Proc Time", L"(总利用率)");
    PdhCloseQuery(q);

    // 每核 PDH
    PDH_HQUERY q2 = nullptr;
    PdhOpenQueryW(nullptr, 0, &q2);
    PDH_HCOUNTER c2 = nullptr;
    PdhAddEnglishCounterW(q2, L"\\Processor(*)\\% Processor Time", 0, &c2);
    PdhCollectQueryData(q2);
    Stats sPdh2;
    for (int i = 0; i < 15; ++i) {
        double t0 = NowMs();
        PdhCollectQueryData(q2);
        sPdh2.Add(NowMs() - t0);
        Sleep(5);
    }
    sPdh2.Report(L"PDH \\Processor(*)\\% Proc Time", L"(20 核通配符)");
    PdhCloseQuery(q2);
}

// ---------- 2. CPU 频率 ----------
static void TestCpuFreq() {
    wprintf(L"\n=== 2. CPU 频率与型号 ===\n");

    // 注册表型号名
    {
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                0, KEY_READ, &k) == ERROR_SUCCESS) {
            wchar_t name[256] = {};
            DWORD cb = sizeof(name);
            if (RegQueryValueExW(k, L"ProcessorNameString", nullptr, nullptr,
                                 reinterpret_cast<LPBYTE>(name), &cb) == ERROR_SUCCESS) {
                wprintf(L"  注册表 ProcessorNameString: %ls\n", name);
            }
            DWORD mhz = 0; cb = sizeof(mhz);
            if (RegQueryValueExW(k, L"~MHz", nullptr, nullptr,
                                 reinterpret_cast<LPBYTE>(&mhz), &cb) == ERROR_SUCCESS) {
                wprintf(L"  注册表 ~MHz               : %u MHz\n", mhz);
            }
            RegCloseKey(k);
        }
    }

    // CallNtPowerInformation(ProcessorInformation = 11)
    // 结构大小 = 核数 × sizeof(PROCESSOR_POWER_INFORMATION)
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const ULONG nCpu = si.dwNumberOfProcessors;

    struct PPI {
        ULONG Number;
        ULONG MaxMhz;
        ULONG CurrentMhz;
        ULONG MhzLimit;
        ULONG MaxIdleState;
        ULONG CurrentIdleState;
    };
    std::vector<PPI> ppi(nCpu, PPI{});

    Stats s;
    NTSTATUS st = 0;
    for (int i = 0; i < 20; ++i) {
        double t0 = NowMs();
        st = CallNtPowerInformation(ProcessorInformation, nullptr, 0,
                                    ppi.data(), nCpu * sizeof(PPI));
        s.Add(NowMs() - t0);
    }
    wprintf(L"  CallNtPowerInformation status=0x%08X  structSize=%zu\n",
            st, sizeof(PPI));
    if (st == 0) {
        wprintf(L"    core0: Max=%u Current=%u Limit=%u MaxIdle=%u CurIdle=%u\n",
                ppi[0].MaxMhz, ppi[0].CurrentMhz, ppi[0].MhzLimit,
                ppi[0].MaxIdleState, ppi[0].CurrentIdleState);
        // 检查所有核频率是否一致
        bool allSame = true;
        for (ULONG i = 1; i < nCpu; ++i)
            if (ppi[i].CurrentMhz != ppi[0].CurrentMhz) allSame = false;
        wprintf(L"    所有 %u 核 CurrentMhz 是否一致: %ls\n", nCpu,
                allSame ? L"YES (恒定)" : L"NO (有变化)");
    }
    s.Report(L"CallNtPowerInformation(11)", L"(每核频率)");

    // PDH \Processor Information(_Total)\% Processor Performance
    PDH_HQUERY q = nullptr; PDH_HCOUNTER c = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS) {
        PDH_STATUS ps = PdhAddEnglishCounterW(q,
            L"\\Processor Information(_Total)\\% Processor Performance", 0, &c);
        wprintf(L"  PDH \\Processor Information(_Total)\\%% Processor Performance -> 0x%08X\n", ps);
        if (ps == ERROR_SUCCESS) {
            PdhCollectQueryData(q);
            Stats s2;
            for (int i = 0; i < 10; ++i) {
                double t0 = NowMs();
                PdhCollectQueryData(q);
                s2.Add(NowMs() - t0);
                Sleep(5);
            }
            s2.Report(L"PDH % Processor Performance");
        }
        PdhCloseQuery(q);
    }
}

// ---------- 3. 内存组成 ----------
// SystemPerformanceInformation = 2
// 需要完整结构。这里用 OFFICIAL 布局（Win10 x64）
typedef struct _SYSTEM_PERFORMANCE_INFORMATION_FULL {
    LARGE_INTEGER IdleProcessTime;
    LARGE_INTEGER IoReadTransferCount;
    LARGE_INTEGER IoWriteTransferCount;
    LARGE_INTEGER IoOtherTransferCount;
    ULONG IoReadOperationCount;
    ULONG IoWriteOperationCount;
    ULONG IoOtherOperationCount;
    ULONG AvailablePages;
    ULONG CommittedPages;
    ULONG CommitLimit;
    ULONG PeakCommitment;
    ULONG PageFaultCount;
    ULONG CopyOnWriteCount;
    ULONG TransitionCount;
    ULONG CacheTransitionCount;
    ULONG DemandZeroCount;
    ULONG PageReadCount;
    ULONG PageReadIoCount;
    ULONG CacheReadCount;
    ULONG CacheIoCount;
    ULONG DirtyPagesWriteCount;
    ULONG DirtyWriteIoCount;
    ULONG MappedPagesWriteCount;
    ULONG MappedWriteIoCount;
    ULONG PagedPoolPages;
    ULONG NonPagedPoolPages;
    ULONG PagedPoolAllocs;
    ULONG PagedPoolFrees;
    ULONG NonPagedPoolAllocs;
    ULONG NonPagedPoolFrees;
    ULONG FreeSystemPtes;
    ULONG ResidentSystemCodePage;
    ULONG TotalSystemDriverPages;
    ULONG TotalSystemCodePages;
    ULONG NonPagedPoolLookasideHits;
    ULONG PagedPoolLookasideHits;
    ULONG AvailablePagedPoolPages;
    ULONG ResidentSystemCachePage;
    ULONG ResidentPagedPoolPage;
    ULONG ResidentSystemDriverPage;
    ULONG CcFastReadNoWait;
    ULONG CcFastReadWait;
    ULONG CcFastReadNotPossible;
    ULONG CcCopyReadNoWait;
    ULONG CcCopyReadWait;
    ULONG CcCopyReadNoWaitMiss;
    ULONG IoReadOperationCount2;
    ULONG Spare1;
    ULONG Spare2;
} SPI_FULL;

static void TestMemory() {
    wprintf(L"\n=== 3. 内存组成 ===\n");

    SPI_FULL spi{};
    ULONG need = 0;
    NTSTATUS st = g_ntqsi(2, &spi, sizeof(spi), &need);
    wprintf(L"  NtQSI(2) SystemPerformanceInformation status=0x%08X structSize=%zu need=%u\n",
            st, sizeof(spi), need);

    const ULONG pageSize = 4096;   // 后面用 GetSystemInfo 校验
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const ULONG64 ps = si.dwPageSize;

    if (NT_SUCCESS(st)) {
        wprintf(L"    AvailablePages    = %u  (%.1f MB)\n", spi.AvailablePages,
                spi.AvailablePages * ps / 1048576.0);
        wprintf(L"    CommittedPages    = %u  (%.1f MB)\n", spi.CommittedPages,
                spi.CommittedPages * ps / 1048576.0);
        wprintf(L"    CommitLimit       = %u  (%.1f MB)\n", spi.CommitLimit,
                spi.CommitLimit * ps / 1048576.0);
        wprintf(L"    PagedPoolPages    = %u  (%.2f MB)\n", spi.PagedPoolPages,
                spi.PagedPoolPages * ps / 1048576.0);
        wprintf(L"    NonPagedPoolPages = %u  (%.2f MB)\n", spi.NonPagedPoolPages,
                spi.NonPagedPoolPages * ps / 1048576.0);
        wprintf(L"    ResidentSystemCachePage = %u (%.1f MB)\n",
                spi.ResidentSystemCachePage, spi.ResidentSystemCachePage * ps / 1048576.0);
    }

    Stats s;
    for (int i = 0; i < 30; ++i) {
        double t0 = NowMs();
        g_ntqsi(2, &spi, sizeof(spi), &need);
        s.Add(NowMs() - t0);
    }
    s.Report(L"NtQSI(2) SystemPerformanceInfo", L"(内存组成，1 次全拿)");

    // 对比 GlobalMemoryStatusEx
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    Stats s2;
    for (int i = 0; i < 20; ++i) {
        double t0 = NowMs();
        GlobalMemoryStatusEx(&ms);
        s2.Add(NowMs() - t0);
    }
    s2.Report(L"GlobalMemoryStatusEx", L"(总数/可用)");
    wprintf(L"    物理: %.2f GB 总 / %.2f GB 可用  提交: %.2f/%.2f GB\n",
            ms.ullTotalPhys / 1073741824.0,
            ms.ullAvailPhys / 1073741824.0,
            (ms.ullTotalPageFile - ms.ullAvailPageFile) / 1073741824.0,
            ms.ullTotalPageFile / 1073741824.0);

    // PDH \Memory\* 对比
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS) {
        const wchar_t* counters[] = {
            L"\\Memory\\Available MBytes",
            L"\\Memory\\Cache Bytes",
            L"\\Memory\\Modified Page List Bytes",
            L"\\Memory\\Standby Cache Normal Priority Bytes",
            L"\\Memory\\Pool Paged Bytes",
            L"\\Memory\\Pool Nonpaged Bytes",
        };
        Stats sPdh;
        PDH_HCOUNTER arr[6] = {};
        for (int i = 0; i < 6; ++i) {
            PdhAddEnglishCounterW(q, counters[i], 0, &arr[i]);
        }
        PdhCollectQueryData(q);
        for (int i = 0; i < 15; ++i) {
            double t0 = NowMs();
            PdhCollectQueryData(q);
            sPdh.Add(NowMs() - t0);
            Sleep(5);
        }
        sPdh.Report(L"PDH \\Memory\\* (6 个计数器)");

        for (int i = 0; i < 6; ++i) {
            PDH_FMT_COUNTERVALUE v{};
            if (PdhGetFormattedCounterValue(arr[i], PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS) {
                wprintf(L"    %-46ls = %.2f\n", counters[i], v.doubleValue);
            }
        }
        PdhCloseQuery(q);
    }
}

// ---------- 4. 磁盘 ----------
static void TestDisk() {
    wprintf(L"\n=== 4. 磁盘 ===\n");

    // 枚举物理磁盘
    wprintf(L"  --- 物理磁盘枚举 ---\n");
    HANDLE h = CreateFileW(L"\\\\.\\PhysicalDrive0", 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        STORAGE_PROPERTY_QUERY q{};
        q.PropertyId = StorageDeviceProperty;
        q.QueryType = PropertyStandardQuery;
        BYTE buf[1024] = {};
        DWORD ret = 0;
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q),
                            buf, sizeof(buf), &ret, nullptr)) {
            auto* d = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf);
            if (d->ProductIdOffset) {
                wprintf(L"    PhysicalDrive0 product: %hs\n", buf + d->ProductIdOffset);
            }
            if (d->VendorIdOffset) {
                wprintf(L"    PhysicalDrive0 vendor : %hs\n", buf + d->VendorIdOffset);
            }
        }
        CloseHandle(h);
    }

    // PDH 物理磁盘
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS) {
        const wchar_t* cs[] = {
            L"\\PhysicalDisk(_Total)\\% Idle Time",
            L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec",
            L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec",
            L"\\PhysicalDisk(_Total)\\Avg. Disk sec/Transfer",
            L"\\PhysicalDisk(_Total)\\Current Disk Queue Length",
        };
        PDH_HCOUNTER arr[5] = {};
        PDH_STATUS st = 0;
        for (int i = 0; i < 5; ++i) {
            st = PdhAddEnglishCounterW(q, cs[i], 0, &arr[i]);
            if (st != ERROR_SUCCESS) wprintf(L"    FAIL add %ls -> 0x%08X\n", cs[i], st);
        }
        PdhCollectQueryData(q);
        Sleep(1000);
        PdhCollectQueryData(q);

        Stats s;
        for (int i = 0; i < 15; ++i) {
            double t0 = NowMs();
            PdhCollectQueryData(q);
            s.Add(NowMs() - t0);
            Sleep(5);
        }
        s.Report(L"PDH \\PhysicalDisk(_Total)\\* (5 个)");

        for (int i = 0; i < 5; ++i) {
            PDH_FMT_COUNTERVALUE v{};
            if (PdhGetFormattedCounterValue(arr[i], PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS) {
                wprintf(L"    %-46ls = %.3f\n", cs[i], v.doubleValue);
            }
        }

        // 通配符：每个物理磁盘
        PDH_HCOUNTER cWild = nullptr;
        st = PdhAddEnglishCounterW(q, L"\\PhysicalDisk(*)\\% Idle Time", 0, &cWild);
        wprintf(L"  PDH \\PhysicalDisk(*)\\%% Idle Time -> 0x%08X\n", st);
        if (st == ERROR_SUCCESS) {
            PdhCollectQueryData(q);
            Sleep(200);
            PdhCollectQueryData(q);
            DWORD sz = 0;
            PdhGetFormattedCounterArrayW(cWild, PDH_FMT_DOUBLE, &sz, nullptr, nullptr);
            if (sz > 0) {
                std::vector<BYTE> itemBuf(sz);
                DWORD cnt = 0;
                auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(itemBuf.data());
                if (PdhGetFormattedCounterArrayW(cWild, PDH_FMT_DOUBLE, &sz, &cnt, items) == ERROR_SUCCESS) {
                    wprintf(L"    实例数 = %u\n", cnt);
                    for (DWORD i = 0; i < cnt && i < 8; ++i) {
                        wprintf(L"      %ls = %.2f\n", items[i].szName, items[i].FmtValue.doubleValue);
                    }
                }
            }
        }
        PdhCloseQuery(q);
    }
}

// ---------- 5. 网络 ----------
static void TestNetwork() {
    wprintf(L"\n=== 5. 网络 ===\n");

    // GetIfTable（旧版 IP Helper，不需要 winsock2/netioapi）
    DWORD tblSz = 0;
    if (GetIfTable(nullptr, &tblSz, FALSE) == ERROR_INSUFFICIENT_BUFFER && tblSz > 0) {
        std::vector<BYTE> nb(tblSz);
        auto* tbl = reinterpret_cast<PMIB_IFTABLE>(nb.data());
        if (GetIfTable(tbl, &tblSz, FALSE) == NO_ERROR) {
            wprintf(L"  GetIfTable: %u 个接口\n", tbl->dwNumEntries);
            for (DWORD k = 0; k < tbl->dwNumEntries && k < 8; ++k) {
                auto& r = tbl->table[k];
                wprintf(L"    [%u] ifIndex=%lu type=%lu oper=%lu inOctets=%lu outOctets=%lu\n",
                        k, r.dwIndex, r.dwType, r.dwOperStatus, r.dwInOctets, r.dwOutOctets);
            }
        }
    }

    Stats sNet;
    for (int i = 0; i < 20; ++i) {
        DWORD probe = 0;
        double t0 = NowMs();
        GetIfTable(nullptr, &probe, FALSE);
        sNet.Add(NowMs() - t0);
    }
    sNet.Report(L"GetIfTable size-probe", L"(枚举全部适配器)");

    // PDH 网络接口
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS) {
        PDH_HCOUNTER c = nullptr;
        PDH_STATUS st = PdhAddEnglishCounterW(q,
            L"\\Network Interface(*)\\Bytes Received/sec", 0, &c);
        wprintf(L"  PDH \\Network Interface(*)\\Bytes Received/sec -> 0x%08X\n", st);
        if (st == ERROR_SUCCESS) {
            PdhCollectQueryData(q);
            Sleep(500);
            PdhCollectQueryData(q);
            DWORD sz = 0;
            PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &sz, nullptr, nullptr);
            if (sz > 0) {
                std::vector<BYTE> b(sz);
                DWORD cnt = 0;
                auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(b.data());
                if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &sz, &cnt, items) == ERROR_SUCCESS) {
                    wprintf(L"    实例数 = %u\n", cnt);
                    for (DWORD i = 0; i < cnt && i < 10; ++i) {
                        wprintf(L"      %ls = %.2f B/s\n", items[i].szName, items[i].FmtValue.doubleValue);
                    }
                }
            }
        }
        PdhCloseQuery(q);
    }

    // 系统级总吞吐
    PDH_HQUERY q2 = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q2) == ERROR_SUCCESS) {
        PDH_HCOUNTER c2 = nullptr;
        PDH_STATUS st = PdhAddEnglishCounterW(q2,
            L"\\Network Interface(*)\\Bytes Total/sec", 0, &c2);
        wprintf(L"  PDH \\Network Interface(*)\\Bytes Total/sec -> 0x%08X\n", st);
        PdhCloseQuery(q2);
    }
}

// ---------- 6. GPU ----------
static void TestGpu() {
    wprintf(L"\n=== 6. GPU ===\n");

    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) {
        wprintf(L"  PdhOpenQuery failed\n");
        return;
    }

    // 先看实例数
    PDH_HCOUNTER c = nullptr;
    PDH_STATUS st = PdhAddEnglishCounterW(q,
        L"\\GPU Engine(*)\\Utilization Percentage", 0, &c);
    wprintf(L"  PdhAddEnglishCounter(\\GPU Engine(*)\\Utilization Percentage) -> 0x%08X\n", st);

    if (st == ERROR_SUCCESS) {
        Stats s;
        for (int i = 0; i < 5; ++i) {
            double t0 = NowMs();
            PdhCollectQueryData(q);
            s.Add(NowMs() - t0);
            Sleep(200);
        }
        s.Report(L"PDH \\GPU Engine(*) collect", L"(全部引擎实例)");

        DWORD sz = 0;
        PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &sz, nullptr, nullptr);
        if (sz > 0) {
            std::vector<BYTE> b(sz);
            DWORD cnt = 0;
            auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(b.data());
            if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &sz, &cnt, items) == ERROR_SUCCESS) {
                wprintf(L"    实例数 = %u\n", cnt);
                for (DWORD i = 0; i < cnt && i < 6; ++i) {
                    wprintf(L"      %ls = %.2f\n", items[i].szName, items[i].FmtValue.doubleValue);
                }
            }
        }
    }

    // GPU 适配器内存
    PDH_HCOUNTER c2 = nullptr;
    st = PdhAddEnglishCounterW(q, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &c2);
    wprintf(L"  PdhAddEnglishCounter(\\GPU Adapter Memory(*)\\Dedicated Usage) -> 0x%08X\n", st);

    // GPU 进程内存（实例多）
    PDH_HCOUNTER c3 = nullptr;
    st = PdhAddEnglishCounterW(q, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &c3);
    wprintf(L"  PdhAddEnglishCounter(\\GPU Process Memory(*)\\Dedicated Usage) -> 0x%08X\n", st);

    PdhCloseQuery(q);

    // DXGI 枚举适配器
    wprintf(L"  --- DXGI 适配器 ---\n");
    typedef HRESULT (WINAPI* CreateDXGIFactory1_t)(REFIID, void**);
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (dxgi) {
        auto fn = reinterpret_cast<CreateDXGIFactory1_t>(
            GetProcAddress(dxgi, "CreateDXGIFactory1"));
        if (fn) {
            // 只检查能否创建，不深入 COM
            void* factory = nullptr;
            // IID_IDXGIFactory1 = {770aae78-f26f-4dba-a829-253c83d1b387}
            static const GUID IID_IDXGIFactory1_ =
                {0x770aae78,0xf26f,0x4dba,{0xa8,0x29,0x25,0x3c,0x83,0xd1,0xb3,0x87}};
            HRESULT hr = fn(IID_IDXGIFactory1_, &factory);
            wprintf(L"    CreateDXGIFactory1 -> 0x%08X\n", hr);
            if (SUCCEEDED(hr) && factory) {
                wprintf(L"    (COM 枚举需完整 IDXGIFactory1 实现，此处仅验证可用)\n");
            }
        }
        FreeLibrary(dxgi);
    }
}

// ---------- 7. 系统基础信息（正常运行时间等） ----------
static void TestSystemInfo() {
    wprintf(L"\n=== 7. 系统基础信息 ===\n");

    ULONG need = 0;
    // SystemTimeOfDayInformation = 3 -> 有 BootTime
    struct TOD {
        LARGE_INTEGER BootTime;
        LARGE_INTEGER CurrentTime;
        LARGE_INTEGER TimeZoneBias;
        ULONG TimeZoneId;
        ULONG Reserved;
        ULONGLONG BootTimeBias;
        ULONGLONG SleepTimeBias;
    } tod{};
    NTSTATUS st = g_ntqsi(3, &tod, sizeof(tod), &need);
    wprintf(L"  NtQSI(3) SystemTimeOfDayInfo status=0x%08X size=%zu\n", st, sizeof(tod));
    if (NT_SUCCESS(st)) {
        FILETIME ft;
        ft.dwLowDateTime = tod.BootTime.LowPart;
        ft.dwHighDateTime = static_cast<DWORD>(tod.BootTime.HighPart);
        SYSTEMTIME stLocal{};
        FileTimeToSystemTime(&ft, &stLocal);
        wprintf(L"    启动时间: %04u-%02u-%02u %02u:%02u:%02u\n",
                stLocal.wYear, stLocal.wMonth, stLocal.wDay,
                stLocal.wHour, stLocal.wMinute, stLocal.wSecond);

        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER b, n;
        b.LowPart = ft.dwLowDateTime; b.HighPart = ft.dwHighDateTime;
        n.LowPart = now.dwLowDateTime; n.HighPart = now.dwHighDateTime;
        ULONGLONG upSec = (n.QuadPart - b.QuadPart) / 10000000ULL;
        wprintf(L"    正常运行时间: %llu:%02llu:%02llu\n",
                upSec / 3600, (upSec / 60) % 60, upSec % 60);
    }

    Stats s;
    for (int i = 0; i < 20; ++i) {
        double t0 = NowMs();
        g_ntqsi(3, &tod, sizeof(tod), &need);
        s.Add(NowMs() - t0);
    }
    s.Report(L"NtQSI(3) SystemTimeOfDayInfo", L"(启动时间/正常运行时间)");
}

// ---------- 8. 每核独立曲线所需 ----------
static void TestPerCoreCalc() {
    wprintf(L"\n=== 8. 每核利用率的计算可行性 ===\n");

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const ULONG n = si.dwNumberOfProcessors;

    std::vector<SPPI> prev(n), cur(n);
    ULONG need = 0;
    g_ntqsi(8, prev.data(), n * sizeof(SPPI), &need);
    Sleep(1000);
    double t0 = NowMs();
    g_ntqsi(8, cur.data(), n * sizeof(SPPI), &need);
    double dt = NowMs() - t0;
    wprintf(L"  单次采集 %.3f ms, 采集到 %u 核\n", dt, n);

    wprintf(L"  每核利用率（示例）：\n");
    for (ULONG i = 0; i < n; ++i) {
        LONGLONG idle = cur[i].IdleTime.QuadPart - prev[i].IdleTime.QuadPart;
        LONGLONG kernel = cur[i].KernelTime.QuadPart - prev[i].KernelTime.QuadPart;
        LONGLONG user = cur[i].UserTime.QuadPart - prev[i].UserTime.QuadPart;
        LONGLONG total = kernel + user;
        double util = total > 0 ? (1.0 - double(idle) / double(total)) * 100.0 : 0.0;
        if (util < 0) util = 0;
        if (util > 100) util = 100;
        if (i < 8 || i == n - 1) {
            wprintf(L"    CPU %2u: %5.1f%%  (kernel=%lld user=%lld idle=%lld)\n",
                    i, util, kernel, user, idle);
        }
    }
}

int wmain() {
    _setmode(_fileno(stdout), _O_U8TEXT);
    QueryPerformanceFrequency(&g_freq);

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    g_ntqsi = reinterpret_cast<NtQSI_t>(GetProcAddress(ntdll, "NtQuerySystemInformation"));

    wprintf(L"=== Task Manager Pro :: 性能页数据源实测 ===\n");
    wprintf(L"机器: Xeon 服务器级 CPU (10C/20T) / Win10 22H2\n");

    TestCpuUtil();
    TestCpuFreq();
    TestMemory();
    TestDisk();
    TestNetwork();
    TestGpu();
    TestSystemInfo();
    TestPerCoreCalc();

    wprintf(L"\n=== 完成 ===\n");
    return 0;
}
