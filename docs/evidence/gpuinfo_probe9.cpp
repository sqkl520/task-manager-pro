// gpuinfo_probe9.cpp - pid 枚举成本对比（决定刷新策略）
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <cstdio>
#include <vector>
#include <algorithm>
#pragma comment(lib, "psapi.lib")

static double NowMs() {
    static LARGE_INTEGER f{}; static bool i = false;
    if (!i) { QueryPerformanceFrequency(&f); i = true; }
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
static void Report(const char* name, std::vector<double> v, size_t n) {
    std::sort(v.begin(), v.end());
    double s = 0; for (double x : v) s += x;
    printf("  %-34s avg=%7.3f p50=%7.3f p95=%7.3f max=%7.3f ms  (n=%zu)\n",
           name, s / v.size(), v[v.size() / 2], v[v.size() * 95 / 100], v.back(), n);
}

int main() {
    printf("=== probe9: pid 枚举成本 ===\n\n");
    const int N = 40;
    size_t cnt = 0;

    {
        std::vector<double> c; std::vector<DWORD> b(16384);
        for (int i = 0; i < N; ++i) {
            double t0 = NowMs();
            DWORD got = 0;
            EnumProcesses(b.data(), (DWORD)(b.size() * sizeof(DWORD)), &got);
            c.push_back(NowMs() - t0);
            cnt = got / sizeof(DWORD);
        }
        Report("EnumProcesses", c, cnt);
    }
    {
        std::vector<double> c;
        for (int i = 0; i < N; ++i) {
            double t0 = NowMs();
            HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            DWORD n = 0;
            if (h != INVALID_HANDLE_VALUE) {
                PROCESSENTRY32W e{}; e.dwSize = sizeof(e);
                if (Process32FirstW(h, &e)) { do { ++n; } while (Process32NextW(h, &e)); }
                CloseHandle(h);
            }
            c.push_back(NowMs() - t0);
            cnt = n;
        }
        Report("CreateToolhelp32Snapshot+SNAP", c, cnt);
    }
    {
        auto pNtQSI = (NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG))
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
        std::vector<BYTE> buf(1 << 21);
        std::vector<double> c;
        for (int i = 0; i < N; ++i) {
            double t0 = NowMs();
            ULONG need = 0;
            pNtQSI(5, buf.data(), (ULONG)buf.size(), &need);
            c.push_back(NowMs() - t0);
        }
        Report("NtQSI(SystemProcessInformation)", c, cnt);
    }
    // 合并：Toolhelp 一次 + 全量 OpenProcess + PROCESS_ADAPTER 门
    printf("\n=== done ===\n");
    return 0;
}
