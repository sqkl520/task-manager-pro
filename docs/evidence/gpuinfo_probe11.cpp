// gpuinfo_probe11.cpp - NtQSI pid 枚举实现验证 + GPU 负载下的使用率
#include <windows.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <winternl.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <iterator>
#include <algorithm>
#include <d3dkmthk.h>
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "pdh.lib")

// ---- NtQSI 解析 ----
namespace nq {
struct SPInfo {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
};
using PFN = NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG);

static size_t EnumPids(std::vector<uint32_t>& out, std::vector<BYTE>& scratch) {
    static PFN pNtQSI = nullptr;
    if (!pNtQSI) pNtQSI = (PFN)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    if (!pNtQSI) return 0;
    if (scratch.size() < (1u << 20)) scratch.resize(1u << 20);
    ULONG need = 0;
    NTSTATUS st = 0;
    // 5 = SystemProcessInformation；缓冲不足时按 need 扩容重试
    for (int attempt = 0; attempt < 4; ++attempt) {
        st = pNtQSI(5, scratch.data(), (ULONG)scratch.size(), &need);
        if (st != 0xC0000004L) break;   // STATUS_INFO_LENGTH_MISMATCH
        scratch.resize(need + 65536);
    }
    if (st < 0) return 0;
    out.clear();
    const BYTE* p = scratch.data();
    for (size_t guard = 0; guard < 4096; ++guard) {
        const auto* e = (const SPInfo*)p;
        out.push_back((uint32_t)(uintptr_t)e->UniqueProcessId);
        if (!e->NextEntryOffset) break;
        p += e->NextEntryOffset;
    }
    return out.size();
}
}

static double NowMs() {
    static LARGE_INTEGER f{}; static bool i = false;
    if (!i) { QueryPerformanceFrequency(&f); i = true; }
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
static double Pct(std::vector<double> v, double q) {
    std::sort(v.begin(), v.end());
    return v[std::min((size_t)(v.size() * q), v.size() - 1)];
}
static double Avg(const std::vector<double>& v) {
    double s = 0; for (double x : v) s += x; return v.empty() ? 0 : s / v.size();
}

int main() {
    printf("=== probe11: NtQSI pid 枚举 + GPU 负载验证 ===\n\n");
    std::vector<uint32_t> p;
    std::vector<BYTE> scratch;
    size_t n = nq::EnumPids(p, scratch);
    printf("NtQSI pid 枚举: %zu 个\n", n);
    printf("前 10 个 pid: ");
    for (size_t i = 0; i < 10 && i < p.size(); ++i) printf("%u ", p[i]);
    printf("\n\n");

    // 与 EnumProcesses 对比正确性
    {
        std::vector<DWORD> b(16384);
        DWORD got = 0;
        EnumProcesses(b.data(), (DWORD)(b.size() * sizeof(DWORD)), &got);
        std::vector<uint32_t> ep(b.begin(), b.begin() + got / sizeof(DWORD));
        std::sort(p.begin(), p.end());
        std::sort(ep.begin(), ep.end());
        std::vector<uint32_t> onlyNq, onlyEp;
        std::set_difference(p.begin(), p.end(), ep.begin(), ep.end(), std::back_inserter(onlyNq));
        std::set_difference(ep.begin(), ep.end(), p.begin(), p.end(), std::back_inserter(onlyEp));
        printf("差异: 只有NtQSI=%zu 只有EnumProcesses=%zu (两者本应基本一致)\n", onlyNq.size(), onlyEp.size());
        if (!onlyNq.empty() || !onlyEp.empty()) {
            printf("  只有NtQSI: "); for (size_t i = 0; i < onlyNq.size() && i < 10; ++i) printf("%u ", onlyNq[i]);
            printf("\n  只有EP  : "); for (size_t i = 0; i < onlyEp.size() && i < 10; ++i) printf("%u ", onlyEp[i]);
            printf("\n");
        }
    }

    // 耗时对比
    {
        std::vector<double> a, b;
        for (int i = 0; i < 40; ++i) {
            double t0 = NowMs();
            nq::EnumPids(p, scratch);
            a.push_back(NowMs() - t0);
        }
        std::vector<DWORD> tmp(16384);
        for (int i = 0; i < 40; ++i) {
            double t0 = NowMs();
            DWORD got = 0;
            EnumProcesses(tmp.data(), (DWORD)(tmp.size() * sizeof(DWORD)), &got);
            b.push_back(NowMs() - t0);
        }
        printf("\nNtQSI         : avg=%.3f p50=%.3f p95=%.3f max=%.3f ms\n", Avg(a), Pct(a, .5), Pct(a, .95),
               *std::max_element(a.begin(), a.end()));
        printf("EnumProcesses : avg=%.3f p50=%.3f p95=%.3f max=%.3f ms\n", Avg(b), Pct(b, .5), Pct(b, .95),
               *std::max_element(b.begin(), b.end()));
    }

    // 复用 scratch 的第二/三次调用成本（缓冲已足够大，不扩容）
    {
        std::vector<double> a;
        for (int i = 0; i < 40; ++i) {
            double t0 = NowMs();
            nq::EnumPids(p, scratch);
            a.push_back(NowMs() - t0);
        }
        printf("NtQSI (scratch 已热): avg=%.3f p50=%.3f p95=%.3f ms\n", Avg(a), Pct(a, .5), Pct(a, .95));
    }

    printf("\n=== done ===\n");
    return 0;
}
