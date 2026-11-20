// bench_final.cpp - 交付模块最终基准（贴近真实 1Hz 采集节奏）
// 只测"调用方提供 pid"这条推荐路径，以及默认路径的分布。
#include "gpuproc.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "psapi.lib")

static double Ms() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static double Pct(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t i = (size_t)(p * (double)(v.size() - 1) + 0.5);
    return v[i];
}

static std::wstring Name(uint32_t pid) {
    wchar_t nm[MAX_PATH] = L"";
    DWORD n = MAX_PATH;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) { QueryFullProcessImageNameW(h, 0, nm, &n); CloseHandle(h); }
    std::wstring s(nm);
    size_t p = s.find_last_of(L'\\');
    return p == std::wstring::npos ? s : s.substr(p + 1);
}

int main() {
    printf("=== gpuproc final benchmark ===\n");
    tmp::GpuProcCollector g;
    if (!g.Init()) { printf("Init FAILED -> degraded (no crash)\n"); return 0; }
    printf("adapter = %ls\n", g.AdapterName());

    std::vector<tmp::GpuProcSample> v;

    // ---------- 1) 推荐路径：调用方给 pid，1Hz 节奏 20 轮 ----------
    {
        std::vector<DWORD> dp(16384); DWORD got = 0;
        EnumProcesses(dp.data(), (DWORD)(dp.size() * sizeof(DWORD)), &got);
        dp.resize(got / 4);
        std::vector<uint32_t> p32(dp.begin(), dp.end());
        printf("\n[1] Collect(pids,count,out)  @1Hz x20  (%zu pids)\n", p32.size());

        g.Collect(p32.data(), p32.size(), v);   // 第一轮只建立差分基线
        std::vector<double> t;
        for (int i = 0; i < 20; ++i) {
            Sleep(1000);
            double a = Ms();
            g.Collect(p32.data(), p32.size(), v);
            t.push_back(Ms() - a);
        }
        printf("    median=%.3f ms  p90=%.3f  min=%.3f  max=%.3f  (samples=%zu)\n",
               Pct(t, 0.5), Pct(t, 0.9), *std::min_element(t.begin(), t.end()),
               *std::max_element(t.begin(), t.end()), v.size());
    }

    // ---------- 2) 默认路径：内部缓存 pid，1Hz 节奏 30 轮 ----------
    {
        printf("\n[2] Collect(out) (pid cache, refresh every 10 rounds) @1Hz x30\n");
        std::vector<double> t;
        g.RefreshPids();
        for (int i = 0; i < 30; ++i) {
            Sleep(1000);
            double a = Ms();
            g.Collect(v);
            t.push_back(Ms() - a);
        }
        printf("    median=%.3f ms  p90=%.3f  max=%.3f\n",
               Pct(t, 0.5), Pct(t, 0.9), *std::max_element(t.begin(), t.end()));
        int slow = 0;
        for (double x : t) if (x > 15.0) ++slow;
        printf("    rounds >15ms: %d / %d\n", slow, (int)t.size());
    }

    // ---------- 3) 真实数据快照 ----------
    printf("\n[3] live snapshot\n");
    std::vector<tmp::GpuProcSample> s = v;
    std::sort(s.begin(), s.end(), [](const tmp::GpuProcSample& a, const tmp::GpuProcSample& b) {
        if (a.gpuPercent != b.gpuPercent) return a.gpuPercent > b.gpuPercent;
        return a.dedicatedBytes > b.dedicatedBytes;
    });
    printf("    %-8s %-26s %8s %12s %12s\n", "pid", "name", "GPU%", "dedicatedMB", "sharedMB");
    for (size_t i = 0; i < s.size() && i < 10; ++i)
        printf("    %-8u %-26ls %7.2f%% %12.2f %12.2f\n", s[i].pid, Name(s[i].pid).c_str(),
               s[i].gpuPercent, s[i].dedicatedBytes / 1048576.0, s[i].sharedBytes / 1048576.0);

    // ---------- 4) 降级路径 ----------
    printf("\n[4] degradation checks\n");
    {
        tmp::GpuProcCollector bad;
        std::vector<tmp::GpuProcSample> bv;
        bv.push_back(tmp::GpuProcSample{});
        bad.Collect(bv);
        printf("    Collect before Init  -> out.size=%zu, available=%d\n",
               bv.size(), (int)bad.Available());
        bad.Collect(nullptr, 0, bv);
        printf("    Collect(nullptr,0)   -> out.size=%zu (no crash)\n", bv.size());
    }
    printf("    ok\n");
    return 0;
}
