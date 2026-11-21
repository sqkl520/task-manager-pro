// gpuinfo_probe16.cpp - 原版任务管理器会列出哪些 GPU？
//
// 判据：Win10 任务管理器 GPU 页/侧栏的数据源是 PDH 的
//   \GPU Adapter Memory(luid_0x..._phys_0)\*  和  \GPU Engine(pid_..._luid_0x..._phys_0_eng_...)\*
// 这两组计数器的**实例集合**就是任务管理器能看到的适配器集合。
// 本探针只枚举实例名，不运行 taskmgr.exe。
//
// 编译见 _build_probe16.bat
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "pdh.lib")

static void DumpInstances(const wchar_t* path, const char* label) {
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) { printf("[%s] open query failed\n", label); return; }
    PDH_HCOUNTER c = nullptr;
    PDH_STATUS st = PdhAddEnglishCounterW(q, path, 0, &c);
    if (st != ERROR_SUCCESS) {
        printf("[%s] AddEnglishCounter failed 0x%08X\n", label, (unsigned)st);
        PdhCloseQuery(q);
        return;
    }
    PdhCollectQueryData(q);
    DWORD size = 0, count = 0;
    st = PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &size, &count, nullptr);
    if (st != PDH_MORE_DATA) {
        printf("[%s] GetFormattedCounterArray(size) st=0x%08X\n", label, (unsigned)st);
        PdhCloseQuery(q);
        return;
    }
    std::vector<BYTE> buf(size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    st = PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &size, &count, items);
    printf("[%s] instances=%lu st=0x%08X\n", label, count, (unsigned)st);
    for (DWORD i = 0; i < count; ++i) {
        wprintf(L"    %s = %.0f\n", items[i].szName, items[i].FmtValue.doubleValue);
    }
    PdhCloseQuery(q);
}

int wmain() {
    // 这两组计数器的实例名里带 LUID，正好是任务管理器用的那张表
    DumpInstances(L"\\GPU Adapter Memory(*)\\Dedicated Usage", "GPU Adapter Memory");
    printf("\n");
    DumpInstances(L"\\GPU Engine(*)\\Utilization Percentage", "GPU Engine");
    printf("\n");
    // 适配器内存的另两组（共享/总量）用来确认实例集合一致
    DumpInstances(L"\\GPU Adapter Memory(*)\\Shared Usage", "GPU Adapter Memory Shared");
    return 0;
}
