// memprobe.cpp - 精确验证 SystemPerformanceInformation 结构，拿到完整内存组成
// 目的：为性能页「内存页」的内存组成条找到正确的数据源字段
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <winternl.h>
#include <ntstatus.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <vector>
#include <cstring>

#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "psapi.lib")

typedef NTSTATUS (NTAPI* NtQSI_t)(ULONG, PVOID, ULONG, PULONG);
static NtQSI_t g_ntqsi = nullptr;

// 完整的 SystemPerformanceInformation 结构（Win10 x64，344 字节）
// 逐字段对照 phnt (Process Hacker) 的 SYSTEM_PERFORMANCE_INFORMATION
#pragma pack(push, 8)
typedef struct _SYSTEM_PERFORMANCE_INFORMATION_FULL {
    LARGE_INTEGER IdleProcessTime;              // 0
    LARGE_INTEGER IoReadTransferCount;          // 8
    LARGE_INTEGER IoWriteTransferCount;         // 16
    LARGE_INTEGER IoOtherTransferCount;         // 24
    ULONG IoReadOperationCount;                 // 32
    ULONG IoWriteOperationCount;                // 36
    ULONG IoOtherOperationCount;                // 40
    ULONG AvailablePages;                       // 44
    ULONG CommittedPages;                       // 48
    ULONG CommitLimit;                          // 52
    ULONG PeakCommitment;                       // 56
    ULONG PageFaultCount;                       // 60
    ULONG CopyOnWriteCount;                     // 64
    ULONG TransitionCount;                      // 68
    ULONG CacheTransitionCount;                 // 72
    ULONG DemandZeroCount;                      // 76
    ULONG PageReadCount;                        // 80
    ULONG PageReadIoCount;                      // 84
    ULONG CacheReadCount;                       // 88
    ULONG CacheIoCount;                         // 92
    ULONG DirtyPagesWriteCount;                 // 96
    ULONG DirtyWriteIoCount;                    // 100
    ULONG MappedPagesWriteCount;                // 104
    ULONG MappedWriteIoCount;                   // 108
    ULONG PagedPoolPages;                       // 112
    ULONG NonPagedPoolPages;                    // 116
    ULONG PagedPoolAllocs;                      // 120
    ULONG PagedPoolFrees;                       // 124
    ULONG NonPagedPoolAllocs;                   // 128
    ULONG NonPagedPoolFrees;                    // 132
    ULONG FreeSystemPtes;                       // 136
    ULONG ResidentSystemCodePage;               // 140
    ULONG TotalSystemDriverPages;               // 144
    ULONG TotalSystemCodePages;                 // 148
    ULONG NonPagedPoolLookasideHits;            // 152
    ULONG PagedPoolLookasideHits;               // 156
    ULONG AvailablePagedPoolPages;              // 160
    ULONG ResidentSystemCachePage;              // 164
    ULONG ResidentPagedPoolPage;                // 168
    ULONG ResidentSystemDriverPage;             // 172
    ULONG CcFastReadNoWait;                     // 176
    ULONG CcFastReadWait;                       // 180
    ULONG CcFastReadNotPossible;                // 184
    ULONG CcCopyReadNoWait;                     // 188
    ULONG CcCopyReadWait;                       // 192
    ULONG CcCopyReadNoWaitMiss;                 // 196
    ULONG IoReadOperationCount2;                // 200  ← 实际是 Spare1 之类
    ULONG Spare1;                               // 204
    ULONG Spare2;                               // 208
    ULONG Spare3;                               // 212
    ULONG Spare4;                               // 216
    ULONG Spare5;                               // 220
    ULONG Spare6;                               // 224
    ULONG Spare7;                               // 228
    ULONG Spare8;                               // 232
    ULONG Spare9;                               // 236
    ULONG Spare10;                              // 240
    ULONG Spare11;                              // 244
    ULONG Spare12;                              // 248
    ULONG Spare13;                              // 252
    ULONG Spare14;                              // 256
    ULONG Spare15;                              // 260
    ULONG Spare16;                              // 264
    ULONG Spare17;                              // 268
    ULONG Spare18;                              // 272
    ULONG Spare19;                              // 276
    ULONG Spare20;                              // 280
    ULONG Spare21;                              // 284
    ULONG Spare22;                              // 288
    ULONG Spare23;                              // 292
    ULONG Spare24;                              // 296
    ULONG Spare25;                              // 300
    ULONG Spare26;                              // 304
    ULONG Spare27;                              // 308
    ULONG Spare28;                              // 312
    ULONG Spare29;                              // 316
    ULONG Spare30;                              // 320
    ULONG Spare31;                              // 324
    ULONG Spare32;                              // 328
    ULONG Spare33;                              // 332
    ULONG Spare34;                              // 336
    ULONG Spare35;                              // 340
} SPI_FULL;   // 344 bytes
#pragma pack(pop)

// SystemMemoryListInformation = 80 —— 内存列表（使用中/已修改/备用/可用）
typedef struct _SYSTEM_MEMORY_LIST_INFORMATION {
    SIZE_T ZeroPageCount;
    SIZE_T FreePageCount;
    SIZE_T ModifiedPageCount;
    SIZE_T ModifiedNoWritePageCount;
    SIZE_T BadPageCount;
    SIZE_T PageCountByPriority[8];
    SIZE_T RepurposedPagesByPriority[8];
    SIZE_T ModifiedPageCountPageFile;
} SYSTEM_MEMORY_LIST_INFORMATION;

// SystemFileCacheInformation = 21
typedef struct _SYSTEM_FILECACHE_INFORMATION {
    SIZE_T CurrentSize;
    SIZE_T PeakSize;
    ULONG  PageFaultCount;
    SIZE_T MinimumWorkingSet;
    SIZE_T MaximumWorkingSet;
    SIZE_T CurrentSizeIncludingTransitionInPages;
    SIZE_T PeakSizeIncludingTransitionInPages;
    ULONG  TransitionRePurposeCount;
    ULONG  Flags;
} SYSTEM_FILECACHE_INFORMATION;

static ULONG64 g_pageSize = 4096;

static void DumpMemory() {
    wprintf(L"\n=== SystemPerformanceInformation (修正后 344 字节) ===\n");

    SPI_FULL spi{};
    std::memset(&spi, 0, sizeof(spi));
    ULONG need = 0;
    NTSTATUS st = g_ntqsi(2, &spi, sizeof(spi), &need);
    wprintf(L"  status=0x%08X  structSize=%zu  need=%u\n", st, sizeof(spi), need);

    if (NT_SUCCESS(st)) {
        wprintf(L"  --- 关键字段 ---\n");
        wprintf(L"  AvailablePages        = %10u  = %8.2f MB\n",
                spi.AvailablePages, spi.AvailablePages * g_pageSize / 1048576.0);
        wprintf(L"  CommittedPages        = %10u  = %8.2f MB\n",
                spi.CommittedPages, spi.CommittedPages * g_pageSize / 1048576.0);
        wprintf(L"  CommitLimit           = %10u  = %8.2f MB\n",
                spi.CommitLimit, spi.CommitLimit * g_pageSize / 1048576.0);
        wprintf(L"  PeakCommitment        = %10u  = %8.2f MB\n",
                spi.PeakCommitment, spi.PeakCommitment * g_pageSize / 1048576.0);
        wprintf(L"  PagedPoolPages        = %10u  = %8.2f MB\n",
                spi.PagedPoolPages, spi.PagedPoolPages * g_pageSize / 1048576.0);
        wprintf(L"  NonPagedPoolPages     = %10u  = %8.2f MB\n",
                spi.NonPagedPoolPages, spi.NonPagedPoolPages * g_pageSize / 1048576.0);
        wprintf(L"  ResidentSystemCachePage = %8u  = %8.2f MB\n",
                spi.ResidentSystemCachePage, spi.ResidentSystemCachePage * g_pageSize / 1048576.0);
        wprintf(L"  TotalSystemDriverPages  = %8u  = %8.2f MB\n",
                spi.TotalSystemDriverPages, spi.TotalSystemDriverPages * g_pageSize / 1048576.0);
        wprintf(L"  TotalSystemCodePages    = %8u  = %8.2f MB\n",
                spi.TotalSystemCodePages, spi.TotalSystemCodePages * g_pageSize / 1048576.0);
    }

    // 用 PDH 交叉验证内存组成
    wprintf(L"\n=== 交叉验证：PDH \\Memory\\* ===\n");
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS) {
        struct Item { const wchar_t* path; const wchar_t* label; };
        Item items[] = {
            { L"\\Memory\\Available MBytes",                        L"可用 (Available)" },
            { L"\\Memory\\Cache Bytes",                             L"已缓存 (Cache Bytes)" },
            { L"\\Memory\\Modified Page List Bytes",                L"已修改 (Modified)" },
            { L"\\Memory\\Standby Cache Normal Priority Bytes",     L"备用-普通 (Standby Normal)" },
            { L"\\Memory\\Standby Cache Reserve Bytes",             L"备用-预留 (Standby Reserve)" },
            { L"\\Memory\\Standby Cache Core Bytes",                L"备用-核心 (Standby Core)" },
            { L"\\Memory\\Pool Paged Bytes",                        L"分页缓冲池 (Pool Paged)" },
            { L"\\Memory\\Pool Nonpaged Bytes",                     L"非分页缓冲池 (Pool Nonpaged)" },
            { L"\\Memory\\Committed Bytes",                         L"已提交 (Committed)" },
            { L"\\Memory\\Commit Limit",                            L"提交限制 (Commit Limit)" },
            { L"\\Memory\\% Committed Bytes In Use",                L"已提交% (% Committed In Use)" },
        };
        PDH_HCOUNTER counters[11] = {};
        for (int i = 0; i < 11; ++i) {
            PdhAddEnglishCounterW(q, items[i].path, 0, &counters[i]);
        }
        PdhCollectQueryData(q);
        Sleep(300);
        PdhCollectQueryData(q);
        for (int i = 0; i < 11; ++i) {
            PDH_FMT_COUNTERVALUE v{};
            if (PdhGetFormattedCounterValue(counters[i], PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS) {
                const wchar_t* p = items[i].path;
                bool isBytes = (wcsstr(p, L"Bytes") != nullptr) && (wcsstr(p, L"In Use") == nullptr);
                if (isBytes) {
                    wprintf(L"  %-34ls = %14.0f  = %8.2f MB\n", items[i].label,
                            v.doubleValue, v.doubleValue / 1048576.0);
                } else {
                    wprintf(L"  %-34ls = %14.2f\n", items[i].label, v.doubleValue);
                }
            }
        }
        PdhCloseQuery(q);
    }

    // SystemMemoryListInformation (class 80)
    wprintf(L"\n=== SystemMemoryListInformation (class 80) ===\n");
    SYSTEM_MEMORY_LIST_INFORMATION mli{};
    std::memset(&mli, 0, sizeof(mli));
    st = g_ntqsi(80, &mli, sizeof(mli), &need);
    wprintf(L"  status=0x%08X  structSize=%zu  need=%u\n", st, sizeof(mli), need);
    if (NT_SUCCESS(st)) {
        wprintf(L"  ZeroPageCount          = %8zu  = %8.2f MB\n", mli.ZeroPageCount,
                mli.ZeroPageCount * g_pageSize / 1048576.0);
        wprintf(L"  FreePageCount          = %8zu  = %8.2f MB\n", mli.FreePageCount,
                mli.FreePageCount * g_pageSize / 1048576.0);
        wprintf(L"  ModifiedPageCount      = %8zu  = %8.2f MB\n", mli.ModifiedPageCount,
                mli.ModifiedPageCount * g_pageSize / 1048576.0);
        wprintf(L"  ModifiedNoWritePageCount = %6zu  = %8.2f MB\n", mli.ModifiedNoWritePageCount,
                mli.ModifiedNoWritePageCount * g_pageSize / 1048576.0);
        wprintf(L"  BadPageCount           = %8zu\n", mli.BadPageCount);
        SIZE_T standbyTotal = 0;
        for (int i = 0; i < 8; ++i) {
            wprintf(L"    PageCountByPriority[%d] = %10zu = %8.2f MB\n", i,
                    mli.PageCountByPriority[i], mli.PageCountByPriority[i] * g_pageSize / 1048576.0);
            standbyTotal += mli.PageCountByPriority[i];
        }
        wprintf(L"  备用合计               = %8zu  = %8.2f MB\n", standbyTotal,
                standbyTotal * g_pageSize / 1048576.0);
        wprintf(L"  ModifiedPageCountPageFile = %6zu\n", mli.ModifiedPageCountPageFile);
    }

    // 逐字段偏移扫描（帮助修正结构）
    wprintf(L"\n=== 原始 344 字节转储（前 240 字节，按 ULONG 读）===\n");
    ULONG raw[86] = {};
    std::memset(raw, 0, sizeof(raw));
    st = g_ntqsi(2, raw, sizeof(raw), &need);
    if (NT_SUCCESS(st)) {
        for (int i = 0; i < 60; i += 4) {
            wprintf(L"  [%3d] %10u %10u %10u %10u\n", i * 4,
                    raw[i], raw[i+1], raw[i+2], raw[i+3]);
        }
    }
}

int wmain() {
    _setmode(_fileno(stdout), _O_U8TEXT);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    g_ntqsi = reinterpret_cast<NtQSI_t>(GetProcAddress(ntdll, "NtQuerySystemInformation"));

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    g_pageSize = si.dwPageSize;

    wprintf(L"=== 内存组成数据源精测 ===\n");
    wprintf(L"页大小 = %llu 字节\n", g_pageSize);
    DumpMemory();
    wprintf(L"\n=== 完成 ===\n");
    return 0;
}
