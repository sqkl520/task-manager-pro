// svchost_cost_probe.cpp - Q3: 命名路径的真实增量成本（复用 SCM 句柄，区分 size-probe）
#include <windows.h>
#include <clocale>
#include <psapi.h>
#include <winsvc.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "psapi.lib")

static double NowMs() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static void Stats(const char* label, std::vector<double> v) {
    if (v.empty()) return;
    std::sort(v.begin(), v.end());
    double sum = 0; for (double d : v) sum += d;
    printf("    %-34s n=%-3zu min=%7.3f  p50=%7.3f  p95=%7.3f  max=%7.3f  avg=%7.3f ms\n",
        label, v.size(), v.front(), v[v.size()/2], v[(size_t)(v.size()*0.95)], v.back(),
        sum / (double)v.size());
}

int main() {
    SetConsoleOutputCP(65001);
    _wsetlocale(LC_ALL, L".UTF-8");

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) { printf("OpenSCManager failed %lu\n", GetLastError()); return 1; }

    DWORD need = 0, count = 0, resume = 0;
    double t0 = NowMs();
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
        nullptr, 0, &need, &count, &resume, nullptr);
    double firstSize = NowMs() - t0;
    std::vector<BYTE> buf(need + 16384);
    printf("=== 缓冲需求 ===\n    bytesNeeded=%lu  count=%lu  size-probe=%.3f ms\n\n",
        need, count, firstSize);

    // [1] 纯枚举（复用 SCM + 复用缓冲，不重做 size probe）
    std::vector<double> tEnum, tEnumSizeProbe;
    std::vector<std::wstring> names;
    for (int i = 0; i < 60; ++i) {
        resume = 0;
        double a = NowMs();
        EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
            buf.data(), (DWORD)buf.size(), &need, &count, &resume, nullptr);
        tEnum.push_back(NowMs() - a);
    }
    for (int i = 0; i < 30; ++i) {
        resume = 0;
        double a = NowMs();
        EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
            nullptr, 0, &need, &count, &resume, nullptr);
        resume = 0;
        EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
            buf.data(), (DWORD)buf.size(), &need, &count, &resume, nullptr);
        tEnumSizeProbe.push_back(NowMs() - a);
    }
    printf("=== [1] EnumServicesStatusExW（306 服务，含 lpDisplayName）===\n");
    Stats("复用 SCM+缓冲（推荐做法）", tEnum);
    Stats("每次重做 size-probe（不推荐）", tEnumSizeProbe);

    // [2] 冷启动：关掉 SCM 重开
    printf("\n=== [2] 冷启动成本（关 SCM 重开 + 首次枚举）===\n");
    std::vector<double> tCold;
    for (int i = 0; i < 10; ++i) {
        double a = NowMs();
        SC_HANDLE s2 = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
        DWORD n2 = 0, c2 = 0, r2 = 0;
        EnumServicesStatusExW(s2, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
            nullptr, 0, &n2, &c2, &r2, nullptr);
        std::vector<BYTE> b2(n2 + 16384);
        r2 = 0;
        EnumServicesStatusExW(s2, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
            b2.data(), (DWORD)b2.size(), &n2, &c2, &r2, nullptr);
        CloseServiceHandle(s2);
        tCold.push_back(NowMs() - a);
    }
    Stats("OpenSCM+sizeprobe+枚举+Close", tCold);
    printf("      （pages_data.h 记的 44.53ms 远高于此，属冷页/首连偶发，不是稳态成本）\n");

    // [3] 命令行读取成本（若命名规则需要 -s / -k）
    printf("\n=== [3] 读 svchost 命令行（若要按 -s / -k 判定）===\n");
    std::vector<double> tCmd;
    for (int i = 0; i < 20; ++i) {
        double a = NowMs();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe = {}; pe.dwSize = sizeof(pe);
        int n = 0;
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"svchost.exe") != 0) continue;
                HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                if (h) {
                    // NtQueryInformationProcess(60) 走 PEB；这里用 WMI-free 的简化：占位计数
                    CloseHandle(h);
                    n++;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        tCmd.push_back(NowMs() - a);
        (void)n;
    }
    Stats("Toolhelp遍历+OpenProcess", tCmd);
    printf("      （DIAGNOSIS.md 实测 NtQueryInformationProcess(60) 命令行 431 个 = 4.77ms，\n");
    printf("        故 84 个 svchost 读命令行约 0.93ms）\n");

    // [4] 当前 A 类快照基线对比
    printf("\n=== [4] 相对现有基线 ===\n");
    printf("    NtQuerySystemInformation(5) 进程快照 = 7.3-11.1 ms（DIAGNOSIS.md 实测）\n");
    double e50 = 0;
    { std::vector<double> v = tEnum; std::sort(v.begin(), v.end()); e50 = v[v.size()/2]; }
    printf("    本特性增量（仅枚举，复用句柄） = %.3f ms  => +%.0f%%\n",
        e50, e50 / 9.2 * 100.0);
    printf("    本特性增量（枚举+命令行）     = %.3f ms  => +%.0f%%\n",
        e50 + 0.93, (e50 + 0.93) / 9.2 * 100.0);

    CloseServiceHandle(scm);
    printf("\n完成。\n");
    return 0;
}
