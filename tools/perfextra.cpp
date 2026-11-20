// perfextra.cpp - 补测：GPU 实例数、磁盘实例数、网络适配器、PDH 通配符成本
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <iphlpapi.h>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <vector>
#include <string>
#include <algorithm>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "iphlpapi.lib")

static LARGE_INTEGER g_freq;
static double NowMs() {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / double(g_freq.QuadPart);
}

// 枚举 PDH 通配符实例
static void EnumWildcard(PDH_HQUERY q, const wchar_t* path) {
    PDH_HCOUNTER c = nullptr;
    PDH_STATUS st = PdhAddEnglishCounterW(q, path, 0, &c);
    wprintf(L"\n  %ls\n    AddCounter -> 0x%08X\n", path, st);
    if (st != ERROR_SUCCESS) return;

    PdhCollectQueryData(q);
    Sleep(200);
    PdhCollectQueryData(q);

    DWORD sz = 0;
    PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &sz, nullptr, nullptr);
    if (sz == 0) { wprintf(L"    <no instances>\n"); return; }
    std::vector<BYTE> b(sz);
    DWORD cnt = 0;
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(b.data());
    PDH_STATUS st2 = PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &sz, &cnt, items);
    wprintf(L"    实例数 = %u   (GetFormattedCounterArray -> 0x%08X)\n", cnt, st2);
    for (DWORD i = 0; i < cnt && i < 12; ++i) {
        wprintf(L"      [%2u] %-62ls = %.3f\n", i, items[i].szName,
                items[i].FmtValue.doubleValue);
    }
    if (cnt > 12) wprintf(L"      ... (%u more)\n", cnt - 12);
}

// 测量 collect 成本
static void MeasureCollect(PDH_HQUERY q, const wchar_t* label, int n = 10) {
    std::vector<double> v;
    for (int i = 0; i < n; ++i) {
        double t0 = NowMs();
        PdhCollectQueryData(q);
        v.push_back(NowMs() - t0);
        Sleep(150);
    }
    if (v.empty()) return;
    std::sort(v.begin(), v.end());
    double sum = 0; for (double d : v) sum += d;
    wprintf(L"    collect %-48ls avg %7.3f ms  p95 %7.3f  max %7.3f\n",
            label, sum / v.size(), v[v.size() * 95 / 100], v.back());
}

int wmain() {
    _setmode(_fileno(stdout), _O_U8TEXT);
    QueryPerformanceFrequency(&g_freq);

    wprintf(L"=== 补测：实例数 / 通配符成本 / 适配器 ===\n");

    // ---------- GPU ----------
    wprintf(L"\n########## GPU ##########\n");
    PDH_HQUERY qGpu = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &qGpu) == ERROR_SUCCESS) {
        EnumWildcard(qGpu, L"\\GPU Engine(*)\\Utilization Percentage");
        EnumWildcard(qGpu, L"\\GPU Adapter Memory(*)\\Dedicated Usage");
        EnumWildcard(qGpu, L"\\GPU Adapter Memory(*)\\Shared Usage");
        EnumWildcard(qGpu, L"\\GPU Process Memory(*)\\Dedicated Usage");

        // 单独测总引擎计数器
        PDH_HQUERY q2 = nullptr;
        PdhOpenQueryW(nullptr, 0, &q2);
        PDH_HCOUNTER cTot = nullptr;
        PDH_STATUS st = PdhAddEnglishCounterW(q2,
            L"\\GPU Engine(_Total)\\Utilization Percentage", 0, &cTot);
        wprintf(L"\n  \\GPU Engine(_Total)\\Utilization Percentage -> 0x%08X\n", st);

        // 各种成本
        wprintf(L"\n  --- collect 成本对比 ---\n");
        MeasureCollect(qGpu, L"全部 4 个 GPU 通配符", 8);

        PdhCollectQueryData(qGpu);
        Sleep(300);
        MeasureCollect(qGpu, L"再次 collect (热)", 8);

        PdhCloseQuery(q2);
        PdhCloseQuery(qGpu);
    }

    // 只加一个 GPU 计数器时的成本
    {
        PDH_HQUERY q = nullptr;
        PdhOpenQueryW(nullptr, 0, &q);
        PDH_HCOUNTER c = nullptr;
        PdhAddEnglishCounterW(q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &c);
        PdhCollectQueryData(q);
        Sleep(300);
        wprintf(L"\n  --- 只订阅 1 个 GPU 通配符 ---\n");
        MeasureCollect(q, L"\\GPU Engine(*)\\Utilization Percentage", 10);
        PdhCloseQuery(q);
    }

    // ---------- 磁盘 ----------
    wprintf(L"\n########## 磁盘 ##########\n");
    PDH_HQUERY qDisk = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &qDisk) == ERROR_SUCCESS) {
        EnumWildcard(qDisk, L"\\PhysicalDisk(*)\\% Idle Time");
        EnumWildcard(qDisk, L"\\PhysicalDisk(*)\\Disk Read Bytes/sec");
        EnumWildcard(qDisk, L"\\PhysicalDisk(*)\\Disk Write Bytes/sec");
        EnumWildcard(qDisk, L"\\PhysicalDisk(*)\\Avg. Disk sec/Transfer");

        wprintf(L"\n  --- collect 成本 ---\n");
        MeasureCollect(qDisk, L"\\PhysicalDisk(*) 全部 4 个", 10);
        PdhCloseQuery(qDisk);
    }

    // LogicalDisk 对比
    {
        PDH_HQUERY q = nullptr;
        PdhOpenQueryW(nullptr, 0, &q);
        EnumWildcard(q, L"\\LogicalDisk(*)\\% Disk Time");
        PdhCloseQuery(q);
    }

    // ---------- 网络 ----------
    wprintf(L"\n########## 网络 ##########\n");
    PDH_HQUERY qNet = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &qNet) == ERROR_SUCCESS) {
        EnumWildcard(qNet, L"\\Network Interface(*)\\Bytes Received/sec");
        EnumWildcard(qNet, L"\\Network Interface(*)\\Bytes Sent/sec");
        EnumWildcard(qNet, L"\\Network Interface(*)\\Bytes Total/sec");
        wprintf(L"\n  --- collect 成本 ---\n");
        MeasureCollect(qNet, L"\\Network Interface(*) 3 个", 10);
        PdhCloseQuery(qNet);
    }

    // PDH 网络适配器 vs GetIfTable 对比
    {
        DWORD sz = 0;
        std::vector<double> v;
        for (int i = 0; i < 20; ++i) {
            DWORD probe = 0;
            double t0 = NowMs();
            GetIfTable(nullptr, &probe, FALSE);
            v.push_back(NowMs() - t0);
        }
        std::sort(v.begin(), v.end());
        double sum = 0; for (double d : v) sum += d;
        wprintf(L"\n  GetIfTable(NULL, &sz) 探查: avg %.3f ms  p95 %.3f\n",
                sum / v.size(), v[v.size() * 95 / 100]);
        (void)sz;
    }

    // ---------- CPU 曲线数据 ----------
    wprintf(L"\n########## CPU 曲线数据源 ##########\n");
    {
        PDH_HQUERY q = nullptr;
        PdhOpenQueryW(nullptr, 0, &q);
        PDH_HCOUNTER cAll = nullptr, cKernel = nullptr, cDpc = nullptr, cInt = nullptr;
        PdhAddEnglishCounterW(q, L"\\Processor(_Total)\\% Processor Time", 0, &cAll);
        PdhAddEnglishCounterW(q, L"\\Processor(_Total)\\% Privileged Time", 0, &cKernel);
        PdhAddEnglishCounterW(q, L"\\Processor(_Total)\\% DPC Time", 0, &cDpc);
        PdhAddEnglishCounterW(q, L"\\Processor(_Total)\\% Interrupt Time", 0, &cInt);
        PdhCollectQueryData(q);
        Sleep(500);
        PdhCollectQueryData(q);

        PDH_FMT_COUNTERVALUE v{};
        const wchar_t* names[] = { L"\\% Processor Time", L"\\% Privileged Time (kernel)", L"\\% DPC Time", L"\\% Interrupt Time" };
        PDH_HCOUNTER cs[] = { cAll, cKernel, cDpc, cInt };
        wprintf(L"\n  CPU 曲线需要的 4 条线:\n");
        for (int i = 0; i < 4; ++i) {
            if (PdhGetFormattedCounterValue(cs[i], PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS) {
                wprintf(L"    %-34ls = %.2f%%\n", names[i], v.doubleValue);
            }
        }
        MeasureCollect(q, L"4 个 CPU 计数器", 10);
        PdhCloseQuery(q);
    }

    // ---------- 结果汇总要点 ----------
    wprintf(L"\n########## 结论数据 ##########\n");
    wprintf(L"  见上方各节的实例数与 collect 成本\n");
    wprintf(L"\n=== 完成 ===\n");
    return 0;
}
