// _probe/gputest_load.cpp - 在 GPU 负载下对照 gpuinfo / PDH / nvidia-smi
#include "gpuinfo.h"
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "psapi.lib")

static std::vector<std::wstring> Expand(const wchar_t* pat) {
    std::vector<std::wstring> r;
    DWORD sz = 0;
    if (PdhExpandWildCardPathW(nullptr, pat, nullptr, &sz, 0) != PDH_MORE_DATA) return r;
    std::vector<wchar_t> b(sz + 16);
    DWORD sz2 = (DWORD)b.size();
    if (PdhExpandWildCardPathW(nullptr, pat, b.data(), &sz2, 0) != ERROR_SUCCESS) return r;
    for (size_t i = 0; i < b.size() && b[i];) {
        size_t l = wcslen(&b[i]);
        std::wstring f(&b[i], l);
        size_t lp = f.find(L'('), rp = f.rfind(L')');
        if (lp != std::wstring::npos && rp > lp) r.push_back(f.substr(lp + 1, rp - lp - 1));
        i += l + 1;
    }
    return r;
}

int main() {
    tmp::GpuInfoCollector gpu;
    if (!gpu.Init()) { printf("Init failed\n"); return 1; }
    printf("adapter=%ls  dedicated=%.0f MiB  shared=%.0f MiB\n",
           gpu.Adapter().name.c_str(), gpu.Adapter().dedicatedTotal / 1048576.0,
           gpu.Adapter().sharedTotal / 1048576.0);

    // 只订阅本机 NVIDIA LUID 的实例（避免另一个 LUID 干扰）
    auto insts = Expand(L"\\GPU Engine(pid_*_luid_*_phys_0_*)\\Utilization Percentage");
    PDH_HQUERY hq = nullptr;
    PdhOpenQueryW(nullptr, 0, &hq);
    struct C { std::wstring type; PDH_HCOUNTER h; };
    std::vector<C> cs;
    for (auto& in : insts) {
        std::wstring ty;
        size_t t = in.find(L"engtype_");
        if (t != std::wstring::npos) ty = in.substr(t + 8);
        std::wstring path = L"\\GPU Engine(" + in + L")\\Utilization Percentage";
        PDH_HCOUNTER hc = nullptr;
        if (PdhAddEnglishCounterW(hq, path.c_str(), 0, &hc) == ERROR_SUCCESS) cs.push_back({ ty, hc });
    }
    PdhCollectQueryData(hq);

    // 先跑两轮取基线
    gpu.Collect();
    Sleep(1000);
    gpu.Collect();
    PdhCollectQueryData(hq);

    printf("\n%-8s %-10s %-10s %-10s\n", "engine", "gpuinfo", "PDH", "diff");
    std::map<std::wstring, double> pdh;
    for (auto& c : cs) {
        PDH_FMT_COUNTERVALUE v{};
        if (PdhGetFormattedCounterValue(c.h, PDH_FMT_DOUBLE, nullptr, &v) != ERROR_SUCCESS) continue;
        std::wstring k = c.type;
        if (k == L"VideoDecode") k = L"Video Decode";
        else if (k == L"VideoEncode") k = L"Video Encode";
        else if (k == L"LegacyOverlay") k = L"Overlay";
        else if (k == L"VideoProcessing") k = L"Video Processing";
        else if (k != L"3D" && k != L"Copy") k = L"Other";
        pdh[k] += v.doubleValue;
    }
    for (auto& e : gpu.Engines()) {
        double p = pdh.count(e.name) ? pdh[e.name] : 0.0;
        printf("%-8ls %8.2f%% %8.2f%% %8.2f\n", e.name.c_str(), e.utilPercent, p, e.utilPercent - p);
    }
    printf("total(gpuinfo) = %.2f%%\n", gpu.TotalUtilPercent());

    // 连续采样 + 与 nvidia-smi 对照
    printf("\n--- 连续采样（每轮打印，供与 nvidia-smi 对照）---\n");
    for (int r = 0; r < 8; ++r) {
        Sleep(1000);
        gpu.Collect();
        PdhCollectQueryData(hq);
        std::map<std::wstring, double> p2;
        for (auto& c : cs) {
            PDH_FMT_COUNTERVALUE v{};
            if (PdhGetFormattedCounterValue(c.h, PDH_FMT_DOUBLE, nullptr, &v) != ERROR_SUCCESS) continue;
            std::wstring k = c.type;
            if (k == L"VideoDecode") k = L"Video Decode";
            else if (k == L"VideoEncode") k = L"Video Encode";
            else if (k == L"LegacyOverlay") k = L"Overlay";
            else if (k != L"3D" && k != L"Copy") k = L"Other";
            p2[k] += v.doubleValue;
        }
        printf("t=%d  %5.2f ms | ", r, gpu.LastCollectMs());
        for (auto& e : gpu.Engines())
            if (e.utilPercent > 0.01 || (p2.count(e.name) && p2[e.name] > 0.01))
                printf("%ls=%.2f%%(pdh %.2f%%) ", e.name.c_str(), e.utilPercent,
                       p2.count(e.name) ? p2[e.name] : 0.0);
        printf("| total=%.2f%% ded=%.0fM shr=%.0fM\n", gpu.TotalUtilPercent(),
               gpu.Adapter().dedicatedInUse / 1048576.0, gpu.Adapter().sharedInUse / 1048576.0);
    }

    // 同时打 nvidia-smi
    printf("\n--- nvidia-smi 对照 ---\n");
    fflush(stdout);
    system("nvidia-smi --query-gpu=utilization.gpu,memory.used --format=csv,noheader");
    PdhCloseQuery(hq);
    return 0;
}
