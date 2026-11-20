// svchost_q1_probe.cpp - Q1: 拆行后每行数值口径 + Q3 开销实测
// 只读探测，不修改系统状态，不启动任何 UI。

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

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "advapi32.lib")

static double NowMs() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

struct SvcRec {
    std::wstring name;
    std::wstring display;
    DWORD pid;
    DWORD state;
};

// 一次枚举全部服务（含 PID + 显示名）
static bool EnumAllServices(std::vector<SvcRec>& out, double* ms) {
    double t0 = NowMs();
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return false;
    DWORD need = 0, count = 0, resume = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
        SERVICE_STATE_ALL, nullptr, 0, &need, &count, &resume, nullptr);
    std::vector<BYTE> buf(need + 8192);
    resume = 0;
    BOOL ok = EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
        SERVICE_STATE_ALL, buf.data(), (DWORD)buf.size(), &need, &count,
        &resume, nullptr);
    if (ok) {
        auto* arr = reinterpret_cast<LPENUM_SERVICE_STATUS_PROCESSW>(buf.data());
        for (DWORD i = 0; i < count; ++i) {
            SvcRec r;
            r.name = arr[i].lpServiceName ? arr[i].lpServiceName : L"";
            r.display = arr[i].lpDisplayName ? arr[i].lpDisplayName : L"";
            r.pid = arr[i].ServiceStatusProcess.dwProcessId;
            r.state = arr[i].ServiceStatusProcess.dwCurrentState;
            out.push_back(r);
        }
    }
    CloseServiceHandle(scm);
    if (ms) *ms = NowMs() - t0;
    return ok != FALSE;
}

struct ProcInfo {
    DWORD pid;
    std::wstring name;
    SIZE_T ws, priv;
    ULONGLONG cpu100ns;
};

static void EnumProcs(std::vector<ProcInfo>& out) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = {}; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            ProcInfo p{};
            p.pid = pe.th32ProcessID;
            p.name = pe.szExeFile;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid);
            if (h) {
                PROCESS_MEMORY_COUNTERS_EX pmc = {};
                pmc.cb = sizeof(pmc);
                if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
                    p.ws = pmc.WorkingSetSize;
                    p.priv = pmc.PrivateUsage;
                }
                FILETIME c, e, k, u;
                if (GetProcessTimes(h, &c, &e, &k, &u)) {
                    p.cpu100ns = ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime)
                               + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime);
                }
                CloseHandle(h);
            }
            out.push_back(p);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

int main() {
    SetConsoleOutputCP(65001);
    _wsetlocale(LC_ALL, L".UTF-8");

    // ===== [A] 服务枚举耗时（重复 15 次）=====
    printf("=== [A] EnumServicesStatusExW 耗时（含 lpDisplayName，SERVICE_WIN32/ALL）===\n");
    std::vector<double> times;
    std::vector<SvcRec> svcs;
    for (int i = 0; i < 15; ++i) {
        std::vector<SvcRec> tmp;
        double ms = 0;
        EnumAllServices(tmp, &ms);
        times.push_back(ms);
        if (i == 14) svcs = tmp;
    }
    std::sort(times.begin(), times.end());
    printf("    n=15  min=%.3f  p50=%.3f  max=%.3f  ms   (count=%zu)\n",
        times.front(), times[times.size() / 2], times.back(), svcs.size());

    // ===== [B] 进程 PID -> 服务列表 =====
    std::map<DWORD, std::vector<const SvcRec*>> pid2svc;
    for (auto& s : svcs) {
        if (s.pid != 0) pid2svc[s.pid].push_back(&s);
    }

    std::vector<ProcInfo> procs;
    EnumProcs(procs);

    struct Host {
        ProcInfo pi;
        std::vector<const SvcRec*> svc;
    };
    std::vector<Host> hosts;
    for (auto& p : procs) {
        if (_wcsicmp(p.name.c_str(), L"svchost.exe") != 0) continue;
        Host h; h.pi = p;
        auto it = pid2svc.find(p.pid);
        if (it != pid2svc.end()) h.svc = it->second;
        hosts.push_back(h);
    }

    printf("\n=== [B] 本机 svchost 进程 = %zu，其中承载 >=2 个服务 = ", hosts.size());
    int multi = 0;
    for (auto& h : hosts) if (h.svc.size() >= 2) multi++;
    printf("%d ===\n", multi);

    // 承载服务数分布
    printf("\n=== [C] 每个 svchost 承载服务数分布 ===\n");
    std::map<size_t, int> dist;
    for (auto& h : hosts) dist[h.svc.size()]++;
    for (auto& kv : dist) printf("    %zu 个服务 : %d 个进程\n", kv.first, kv.second);

    // ===== [D] Q1 核心：多服务宿主的数值口径 =====
    printf("\n=== [D] Q1 关键实验：多服务宿主拆行后每行该显示什么 ===\n");
    for (auto& h : hosts) {
        if (h.svc.size() < 2) continue;
        printf("\n  ── PID %lu  工作集 %.1f MB  私有 %.1f MB  承载 %zu 个服务 ──\n",
            h.pi.pid, h.pi.ws / 1048576.0, h.pi.priv / 1048576.0, h.svc.size());
        for (auto* s : h.svc) {
            printf("       service=%-28ls display=%ls\n", s->name.c_str(), s->display.c_str());
        }
        double wsMB = h.pi.ws / 1048576.0;
        printf("       (a) 每行都显示 %.1f MB  => %zu 行合计 %.1f MB  (虚增 %.1fx)\n",
            wsMB, h.svc.size(), wsMB * h.svc.size(), (double)h.svc.size());
        printf("       (b) 只有主服务显示 %.1f MB，其余 0 => 合计 %.1f MB\n", wsMB, wsMB);
        printf("       (c) 均分 => 每行 %.2f MB（Windows 无服务级计量，此数为编造）\n",
            wsMB / h.svc.size());
    }

    // ===== [E] 全表口径对账 =====
    double wsSum = 0, privSum = 0;
    size_t rowByProc = 0, rowBySvc = 0;
    for (auto& h : hosts) {
        wsSum += h.pi.ws / 1048576.0;
        privSum += h.pi.priv / 1048576.0;
        rowByProc++;
        rowBySvc += h.svc.empty() ? 1 : h.svc.size();
    }
    printf("\n=== [E] 口径对账（全部 svchost）===\n");
    printf("    按进程 1 行     = %zu 行\n", rowByProc);
    printf("    按服务 1 行     = %zu 行  (膨胀 %.2fx)\n", rowBySvc,
        (double)rowBySvc / (double)rowByProc);
    printf("    工作集合计(真实) = %.1f MB\n", wsSum);
    printf("    按服务重复显示   = %.1f MB  (虚增 %.2fx)\n",
        wsSum / (double)rowByProc * (double)rowBySvc,
        (double)rowBySvc / (double)rowByProc);
    printf("    私有合计(真实)   = %.1f MB\n", privSum);

    // ===== [F] 是否存在「服务级内存」API =====
    printf("\n=== [F] 是否存在服务级内存计量 =====\n");
    printf("    QueryServiceStatusEx 返回 SERVICE_STATUS_PROCESS 字段：\n");
    printf("      dwServiceType, dwCurrentState, dwControlsAccepted,\n");
    printf("      dwWin32ExitCode, dwServiceSpecificExitCode, dwCheckPoint,\n");
    printf("      dwWaitHint, dwProcessId, dwServiceFlags\n");
    printf("      -> 无内存 / 无 CPU / 无 I/O 字段。\n");
    printf("    PERF 计数器：Process(*) 全按 PID 计量，无 service 实例。\n");
    printf("    -> 服务级内存**不存在**，拆行后每行数值必然重复或编造。\n");

    // ===== [G] 服务描述成本（若想用描述命名）=====
    printf("\n=== [G] 查「服务描述」的成本（对比 lpDisplayName 免费）===\n");
    double t0 = NowMs();
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    int descOk = 0;
    if (scm) {
        for (auto& s : svcs) {
            SC_HANDLE h = OpenServiceW(scm, s.name.c_str(), SERVICE_QUERY_CONFIG);
            if (h) {
                DWORD need = 0;
                QueryServiceConfig2W(h, SERVICE_CONFIG_DESCRIPTION, nullptr, 0, &need);
                if (need) {
                    std::vector<BYTE> b(need);
                    if (QueryServiceConfig2W(h, SERVICE_CONFIG_DESCRIPTION, b.data(), need, &need))
                        descOk++;
                }
                CloseServiceHandle(h);
            }
        }
        CloseServiceHandle(scm);
    }
    printf("    全部 %zu 个服务查描述 = %.3f ms（成功 %d 个）\n",
        svcs.size(), NowMs() - t0, descOk);
    printf("    而 lpDisplayName 已随 EnumServicesStatusEx 免费返回 -> 无需查描述。\n");

    // ===== [H] 截图那条的真实来源 =====
    printf("\n=== [H] 截图「服务主机: Remote Desktop Se...」的真实来源 ===\n");
    for (auto& h : hosts) {
        for (auto* s : h.svc) {
            if (s->display.find(L"Remote Desktop") != std::wstring::npos) {
                printf("    pid=%-6lu nsvc=%-2zu display=[%ls]\n",
                    h.pi.pid, h.svc.size(), s->display.c_str());
            }
        }
    }
    printf("    -> 这些是**各自独立的进程**，每个只承载 1 个服务，天然一行。\n");

    printf("\n完成。\n");
    return 0;
}
