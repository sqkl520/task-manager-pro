// gpuinfo_probe7.cpp - 第七轮：_Total 单实例计数器 + 最终采集路径耗时复测
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>

#pragma comment(lib, "pdh.lib")

static double NowMs() {
    static LARGE_INTEGER f{}; static bool i = false;
    if (!i) { QueryPerformanceFrequency(&f); i = true; }
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

int main() {
    printf("=== probe7: 非通配符单实例计数器 ===\n\n");

    struct Case { const wchar_t* path; };
    const wchar_t* cases[] = {
        L"\\GPU Engine(_Total)\\Utilization Percentage",
        L"\\GPU Engine(_Total)\\Running Time",
        L"\\GPU Adapter Memory(luid_0x00000000_0x00013812_phys_0)\\Dedicated Usage",
        L"\\GPU Adapter Memory(luid_0x00000000_0x00013812_phys_0)\\Shared Usage",
        L"\\GPU Adapter Memory(_Total)\\Dedicated Usage",
        L"\\GPU Local Adapter Memory(luid_0x00000000_0x00013812_phys_0)\\Local Usage",
        L"\\GPU Non Local Adapter Memory(luid_0x00000000_0x00013812_phys_0)\\Non Local Usage",
    };
    for (auto path : cases) {
        PDH_HQUERY q = nullptr;
        PDH_HCOUNTER hc = nullptr;
        double t0 = NowMs();
        PDH_STATUS os = PdhOpenQueryW(nullptr, 0, &q);
        PDH_STATUS as = os == ERROR_SUCCESS ? PdhAddEnglishCounterW(q, path, 0, &hc) : os;
        if (as != ERROR_SUCCESS) {
            printf("  %-72ls AddCounter=0x%08lX\n", path, (unsigned long)as);
            if (q) PdhCloseQuery(q);
            continue;
        }
        PdhCollectQueryData(q);
        Sleep(1000);
        std::vector<double> costs;
        PDH_FMT_COUNTERVALUE v{};
        PDH_STATUS gs = 0;
        for (int i = 0; i < 20; ++i) {
            double c0 = NowMs();
            PdhCollectQueryData(q);
            gs = PdhGetFormattedCounterValue(hc, PDH_FMT_DOUBLE, nullptr, &v);
            costs.push_back(NowMs() - c0);
            Sleep(50);
        }
        std::sort(costs.begin(), costs.end());
        double s = 0;
        for (double x : costs) s += x;
        printf("  %-72ls value=%.3f getStatus=0x%08lX collect avg=%.3f p95=%.3f max=%.3f ms\n",
               path, gs == ERROR_SUCCESS ? v.doubleValue : -1.0, (unsigned long)gs,
               s / costs.size(), costs[costs.size() * 95 / 100], costs.back());
        PdhCloseQuery(q);
    }

    printf("\n=== done ===\n");
    return 0;
}
