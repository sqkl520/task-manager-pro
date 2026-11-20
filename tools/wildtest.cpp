// wildtest.cpp - 诊断 PdhGetFormattedCounterArrayW 为什么拿不到实例
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <vector>

#pragma comment(lib, "pdh.lib")

static void StatusName(PDH_STATUS st) {
    const wchar_t* s = L"?";
    switch (st) {
    case 0:                                s = L"SUCCESS/VALID_DATA"; break;
    case PDH_MORE_DATA:                    s = L"PDH_MORE_DATA"; break;
    case PDH_CSTATUS_NO_INSTANCE:          s = L"PDH_CSTATUS_NO_INSTANCE"; break;
    case PDH_CSTATUS_NO_OBJECT:            s = L"PDH_CSTATUS_NO_OBJECT"; break;
    case PDH_CSTATUS_NO_COUNTER:           s = L"PDH_CSTATUS_NO_COUNTER"; break;
    case PDH_INVALID_ARGUMENT:             s = L"PDH_INVALID_ARGUMENT"; break;
    case PDH_INVALID_HANDLE:               s = L"PDH_INVALID_HANDLE"; break;
    case PDH_NO_DATA:                      s = L"PDH_NO_DATA"; break;
    case PDH_CSTATUS_INVALID_DATA:         s = L"PDH_CSTATUS_INVALID_DATA"; break;
    default: break;
    }
    wprintf(L"0x%08X (%ls)", st, s);
}

static void Test(const wchar_t* path) {
    wprintf(L"\n--- %ls ---\n", path);

    PDH_HQUERY q = nullptr;
    PDH_STATUS st = PdhOpenQueryW(nullptr, 0, &q);
    if (st != ERROR_SUCCESS) { wprintf(L"  PdhOpenQuery: "); StatusName(st); wprintf(L"\n"); return; }

    PDH_HCOUNTER c = nullptr;
    st = PdhAddEnglishCounterW(q, path, 0, &c);
    wprintf(L"  PdhAddEnglishCounter: "); StatusName(st); wprintf(L"\n");
    if (st != ERROR_SUCCESS) {
        // 试本地化版本
        st = PdhAddCounterW(q, path, 0, &c);
        wprintf(L"  PdhAddCounter(localized): "); StatusName(st); wprintf(L"\n");
        if (st != ERROR_SUCCESS) { PdhCloseQuery(q); return; }
    }

    st = PdhCollectQueryData(q);
    Sleep(500);
    st = PdhCollectQueryData(q);
    wprintf(L"  PdhCollectQueryData: "); StatusName(st); wprintf(L"\n");

    // 第一次：询问所需大小
    DWORD sz = 0;
    DWORD cnt = 0;
    st = PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &sz, &cnt, nullptr);
    wprintf(L"  GetArray(1st, sz=0,cnt=0): "); StatusName(st);
    wprintf(L"  -> sz=%u cnt=%u\n", sz, cnt);

    if (sz > 0) {
        std::vector<BYTE> buf(sz);
        DWORD cnt2 = 0;
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
        st = PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &sz, &cnt2, items);
        wprintf(L"  GetArray(2nd): "); StatusName(st);
        wprintf(L"  -> cnt=%u\n", cnt2);
        for (DWORD i = 0; i < cnt2 && i < 10; ++i) {
            wprintf(L"    [%2u] %-56ls = %.3f\n", i, items[i].szName,
                    items[i].FmtValue.doubleValue);
        }
        if (cnt2 > 10) wprintf(L"    ... (%u more)\n", cnt2 - 10);
    } else {
        // sz==0：试试单值路径
        PDH_FMT_COUNTERVALUE v{};
        st = PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE, nullptr, &v);
        wprintf(L"  GetFormattedCounterValue (single): "); StatusName(st);
        wprintf(L"  -> %.3f\n", v.doubleValue);

    }

    PdhCloseQuery(q);
}

int wmain() {
    _setmode(_fileno(stdout), _O_U8TEXT);
    wprintf(L"=== PdhGetFormattedCounterArray 诊断 ===\n");

    Test(L"\\PhysicalDisk(*)\\% Idle Time");
    Test(L"\\PhysicalDisk(_Total)\\% Idle Time");
    Test(L"\\Processor(*)\\% Processor Time");
    Test(L"\\Process(*)\\% Processor Time");
    Test(L"\\GPU Engine(*)\\Utilization Percentage");
    Test(L"\\Network Interface(*)\\Bytes Received/sec");

    // 用 PdhExpandWildCardPath 看能不能展开
    wprintf(L"\n=== PdhExpandWildCardPath ===\n");
    for (const wchar_t* p : { L"\\PhysicalDisk(*)\\% Idle Time",
                              L"\\GPU Engine(*)\\Utilization Percentage",
                              L"\\Network Interface(*)\\Bytes Received/sec" }) {
        DWORD len = 0;
        PDH_STATUS st = PdhExpandWildCardPathW(nullptr, p, nullptr, &len, 0);
        wprintf(L"  %-50ls 1st call -> ", p); StatusName(st);
        wprintf(L" len=%u\n", len);
        if (len > 0 && len < 100000) {
            std::vector<wchar_t> buf(len + 2);
            st = PdhExpandWildCardPathW(nullptr, p, buf.data(), &len, 0);
            wprintf(L"    2nd: "); StatusName(st); wprintf(L"\n");
            int n = 0;
            for (const wchar_t* s = buf.data(); *s; s += wcslen(s) + 1) {
                if (n < 8) wprintf(L"      %ls\n", s);
                ++n;
            }
            wprintf(L"    展开出 %d 个路径\n", n);
        }
    }

    wprintf(L"\n=== 完成 ===\n");
    return 0;
}
