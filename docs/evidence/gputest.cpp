// _probe/gputest.cpp - 用真实项目编译选项验证交付模块
// 目的: 1) 用 build.bat 的完整 flags 编译 include/gpuproc.h + src/gpuproc.cpp
//       2) 跑若干轮, 打印真实输出、耗时分布
//       3) 与 PDH Local/Shared Usage 对照
//       4) 验证降级路径 (Init 前后都能安全调用)
#include "gpuproc.h"

#include <windows.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "pdh.lib")

static std::wstring ProcName(uint32_t pid) {
    wchar_t nm[MAX_PATH] = L"";
    DWORD n = MAX_PATH;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) { QueryFullProcessImageNameW(h, 0, nm, &n); CloseHandle(h); }
    std::wstring s(nm);
    size_t p = s.find_last_of(L'\\');
    return p == std::wstring::npos ? s : s.substr(p + 1);
}

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}

static std::vector<std::wstring> PdhExpandInst(const wchar_t* path) {
    std::vector<std::wstring> out;
    DWORD sz = 0;
    if (PdhExpandWildCardPathW(nullptr, path, nullptr, &sz, 0) != PDH_MORE_DATA) return out;
    if (!sz) return out;
    std::vector<wchar_t> buf((size_t)sz + 16);
    DWORD sz2 = (DWORD)buf.size();
    if (PdhExpandWildCardPathW(nullptr, path, buf.data(), &sz2, 0) != ERROR_SUCCESS) return out;
    for (size_t i = 0; i < buf.size() && buf[i];) {
        size_t l = wcslen(&buf[i]);
        std::wstring full(&buf[i], l);
        size_t lp = full.find(L'('), rp = full.rfind(L')');
        if (lp != std::wstring::npos && rp != std::wstring::npos && rp > lp)
            out.push_back(full.substr(lp + 1, rp - lp - 1));
        i += l + 1;
    }
    return out;
}

int main() {
    printf("=== gpuproc module verification ===\n\n");

    // ---- A) 未 Init 直接 Collect：必须安全返回空 ----
    {
        tmp::GpuProcCollector c;
        std::vector<tmp::GpuProcSample> v;
        c.Collect(v);
        printf("A) Collect() before Init: size=%zu available=%d (expect 0 / 0)\n",
               v.size(), (int)c.Available());
        v.push_back({123, 1.0, 2, 3, 4});   // 塞点脏数据，验证 Collect 会清空
        c.Collect(v);
        printf("   Collect() clears out: size=%zu (expect 0)\n", v.size());
    }

    // ---- B) 虚拟适配器名不能是空 ----
    tmp::GpuProcCollector gpu;
    bool ok = gpu.Init();
    printf("\nB) Init() = %d  available=%d  adapter=\"%ls\"\n",
           (int)ok, (int)gpu.Available(), gpu.AdapterName());
    if (!ok) { printf("   DEGRADED: nothing more to test\n"); return 0; }

    // ---- C) 多轮采样 ----
    printf("\nC) sampling rounds (interval ~1s)\n");
    std::vector<tmp::GpuProcSample> v;
    std::vector<double> costs;
    for (int r = 0; r < 5; ++r) {
        gpu.Collect(v);
        costs.push_back(gpu.LastCollectMs());
        printf("   round %d: %zu samples, %.3f ms\n", r, v.size(), gpu.LastCollectMs());
        if (r >= 1) {
            // 打印 top 6
            std::vector<tmp::GpuProcSample> s = v;
            std::sort(s.begin(), s.end(), [](const tmp::GpuProcSample& a, const tmp::GpuProcSample& b) {
                if (a.gpuPercent != b.gpuPercent) return a.gpuPercent > b.gpuPercent;
                return a.dedicatedBytes > b.dedicatedBytes;
            });
            printf("      %-8s %-24s %8s %12s %12s\n", "pid", "name", "GPU%", "dedicatedMB", "sharedMB");
            for (size_t i = 0; i < s.size() && i < 6; ++i) {
                std::wstring nm = ProcName(s[i].pid);
                printf("      %-8u %-24ls %7.3f%% %12.2f %12.2f\n", s[i].pid, nm.c_str(),
                       s[i].gpuPercent, s[i].dedicatedBytes / 1048576.0, s[i].sharedBytes / 1048576.0);
            }
        }
        if (r < 4) Sleep(1000);
    }

    // ---- D) 与 PDH Local/Shared 精确对照 ----
    printf("\nD) PDH cross-check (Local Usage / Shared Usage)\n");
    PDH_HQUERY hq = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &hq) == ERROR_SUCCESS) {
        // 找到 PDH 里 LUID 与适配器名对应 —— 直接用 4096 里的真实 LUID 过滤太麻烦，
        // 这里改为：只保留单实例内的 Dedicated（=Local）/Shared 对照，
        // 用模块输出的 dedicatedBytes 与 "Local Usage" 比。
        struct C { uint32_t pid; PDH_HCOUNTER loc, shr; };
        std::vector<C> cs;
        std::map<uint32_t, PDH_HCOUNTER> locMap, shrMap;
        for (const auto& s : PdhExpandInst(L"\\GPU Process Memory(*)\\Local Usage")) {
            uint32_t pid = 0;
            const wchar_t* pidp = wcsstr(s.c_str(), L"pid_");
            if (!pidp) continue;
            pid = (uint32_t)wcstoul(pidp + 4, nullptr, 10);
            std::wstring p = L"\\GPU Process Memory(" + s + L")\\Local Usage";
            PDH_HCOUNTER h = nullptr;
            if (PdhAddCounterW(hq, p.c_str(), 0, &h) == ERROR_SUCCESS) locMap[pid] = h;
        }
        for (const auto& s : PdhExpandInst(L"\\GPU Process Memory(*)\\Shared Usage")) {
            const wchar_t* pidp = wcsstr(s.c_str(), L"pid_");
            if (!pidp) continue;
            uint32_t pid = (uint32_t)wcstoul(pidp + 4, nullptr, 10);
            std::wstring p = L"\\GPU Process Memory(" + s + L")\\Shared Usage";
            PDH_HCOUNTER h = nullptr;
            if (PdhAddCounterW(hq, p.c_str(), 0, &h) == ERROR_SUCCESS) shrMap[pid] = h;
        }
        PdhCollectQueryData(hq);
        PdhCollectQueryData(hq);
        auto Get = [](const std::map<uint32_t, PDH_HCOUNTER>& m, uint32_t pid) -> uint64_t {
            auto it = m.find(pid);
            if (it == m.end()) return UINT64_MAX;
            PDH_FMT_COUNTERVALUE v{};
            if (PdhGetFormattedCounterValue(it->second, PDH_FMT_LARGE, nullptr, &v) != ERROR_SUCCESS) return UINT64_MAX;
            return (uint64_t)v.largeValue;
        };
        gpu.Collect(v);
        int eqD = 0, eqS = 0, cmp = 0;
        printf("   %-8s %-24s %14s %14s %6s %14s %14s %6s\n",
               "pid", "name", "modDedMB", "pdhLocMB", "eq", "modShMB", "pdhShrMB", "eq");
        for (auto& s : v) {
            uint64_t pl = Get(locMap, s.pid), ps = Get(shrMap, s.pid);
            if (pl == UINT64_MAX) continue;
            ++cmp;
            bool a = (s.dedicatedBytes == pl), b = (s.sharedBytes == ps);
            if (a) ++eqD;
            if (b) ++eqS;
            if (s.dedicatedBytes > 0)
                printf("   %-8u %-24ls %14.2f %14.2f %6s %14.2f %14.2f %6s\n",
                       s.pid, ProcName(s.pid).c_str(),
                       s.dedicatedBytes / 1048576.0, pl / 1048576.0, a ? "EXACT" : "no",
                       s.sharedBytes / 1048576.0, (ps == UINT64_MAX ? 0.0 : ps / 1048576.0),
                       b ? "EXACT" : "no");
        }
        printf("   dedicated exact %d/%d, shared exact %d/%d\n", eqD, cmp, eqS, cmp);
        PdhCloseQuery(hq);
    }

    // ---- E) 耗时统计 ----
    printf("\nE) timing: 30 rapid Collect() calls\n");
    double sum = 0, mn = 1e9, mx = 0;
    for (int i = 0; i < 30; ++i) {
        gpu.Collect(v);
        double m = gpu.LastCollectMs();
        sum += m;
        if (m < mn) mn = m;
        if (m > mx) mx = m;
    }
    printf("   avg=%.3f  min=%.3f  max=%.3f ms (n=30)\n", sum / 30.0, mn, mx);
    printf("\n   note: 1s-interval rounds above: ");
    for (double c : costs) printf("%.3f ", c);
    printf("ms\n");

    // ---- F) 析构安全 ----
    printf("\nF) destructor path: collector goes out of scope next line\n");
    return 0;
}
