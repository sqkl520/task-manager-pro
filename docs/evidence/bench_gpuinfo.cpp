// bench_gpuinfo.cpp - 最终耗时基准（分 Stage 1/2/3 与 Collect(pids) 两条路径）
#include "gpuinfo.h"
#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <algorithm>
#include <vector>
#pragma comment(lib, "psapi.lib")

static double NowMs() {
    static LARGE_INTEGER f{}; static bool i = false;
    if (!i) { QueryPerformanceFrequency(&f); i = true; }
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
static void Rep(const char* n, std::vector<double> v) {
    if (v.empty()) return;
    std::sort(v.begin(), v.end());
    double s = 0; for (double x : v) s += x;
    printf("  %-40s n=%3zu avg=%7.3f p50=%7.3f p90=%7.3f p95=%7.3f p99=%7.3f max=%7.3f ms\n",
           n, v.size(), s / v.size(), v[v.size()/2], v[v.size()*90/100],
           v[v.size()*95/100], v[v.size()*99/100], v.back());
}
int main() {
    // Init 耗时（5 次）
    std::vector<double> initT;
    for (int i = 0; i < 5; ++i) {
        tmp::GpuInfoCollector c;
        double t0 = NowMs();
        c.Init();
        initT.push_back(NowMs() - t0);
    }
    Rep("Init()", initT);

    tmp::GpuInfoCollector gpu;
    gpu.Init();
    gpu.Collect(); Sleep(1000); gpu.Collect();

    // Stage 3 稳态（紧密循环）
    std::vector<double> s3;
    for (int i = 0; i < 200; ++i) { double t0 = NowMs(); gpu.Collect(); s3.push_back(NowMs() - t0); Sleep(20); }
    Rep("Collect() 稳态 (Stage3)", s3);

    // Collect(pids)
    std::vector<DWORD> pids(16384); DWORD got = 0;
    EnumProcesses(pids.data(), (DWORD)(pids.size()*sizeof(DWORD)), &got);
    size_t n = got / sizeof(DWORD);
    std::vector<double> sp;
    for (int i = 0; i < 200; ++i) {
        double t0 = NowMs();
        gpu.Collect(reinterpret_cast<const uint32_t*>(pids.data()), n);
        sp.push_back(NowMs() - t0);
        Sleep(20);
    }
    Rep("Collect(pids) 稳态", sp);

    // Stage 1（每次 RefreshPids 后调一次 Collect）
    std::vector<double> s1;
    for (int i = 0; i < 15; ++i) {
        gpu.RefreshPids();
        double t0 = NowMs();
        gpu.Collect();
        s1.push_back(NowMs() - t0);
        Sleep(100);
    }
    Rep("Stage1 pid 枚举（每 60 轮一次）", s1);

    // Stage 2（Stage1 之后再调一次 Collect）
    std::vector<double> s2;
    for (int i = 0; i < 15; ++i) {
        gpu.RefreshPids();
        gpu.Collect();      // Stage1
        double t0 = NowMs();
        gpu.Collect();      // Stage2
        s2.push_back(NowMs() - t0);
        Sleep(100);
    }
    Rep("Stage2 建缓存+基线（每 60 轮一次）", s2);

    printf("\n  进程数：%zu   GPU 进程：%d   引擎数：%zu\n", n, gpu.Adapter().processCount,
           gpu.Engines().size());
    return 0;
}
