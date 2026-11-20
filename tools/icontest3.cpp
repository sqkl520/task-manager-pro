// icontest3.cpp - 最终验证：ExtractIconExW 对真实进程 exe 的成功率与耗时
#include <windows.h>
#include <shellapi.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <vector>
#include <set>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "psapi.lib")

static double NowMs() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

int wmain() {
    _setmode(_fileno(stdout), _O_U8TEXT);

    // 用 Toolhelp 枚举所有进程 exe（用 QueryFullProcessImageName 拿 DOS 路径）
    std::vector<std::wstring> paths;
    std::set<std::wstring> uniq;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (pe.th32ProcessID == 0) continue;
                HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                if (!h) continue;
                wchar_t buf[MAX_PATH * 2]; DWORD n = MAX_PATH * 2;
                if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
                    if (uniq.insert(buf).second) paths.emplace_back(buf, n);
                }
                CloseHandle(h);
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }

    wprintf(L"=== %zu unique exe paths from live processes ===\n\n", paths.size());

    // ExtractIconExW 计时 + 成功率
    int okSmall = 0, okLarge = 0, fail = 0;
    double t = NowMs();
    for (auto& p : paths) {
        HICON hL = nullptr, hS = nullptr;
        UINT n = ExtractIconExW(p.c_str(), 0, &hL, &hS, 1);
        if (n > 0) {
            if (hL) { ++okLarge; DestroyIcon(hL); }
            if (hS) { ++okSmall; DestroyIcon(hS); }
            if (!hL && !hS) ++fail;
        } else {
            ++fail;
        }
    }
    double total = NowMs() - t;
    wprintf(L"ExtractIconExW:\n");
    wprintf(L"  total      : %.2f ms for %zu exes\n", total, paths.size());
    wprintf(L"  per exe    : %.3f ms\n", total / paths.size());
    wprintf(L"  large icon : %d ok\n", okLarge);
    wprintf(L"  small icon : %d ok\n", okSmall);
    wprintf(L"  failed     : %d\n", fail);
    wprintf(L"  -> 450 processes estimated: %.0f ms\n\n", total / paths.size() * 450);

    // 冷/热对比：随机重复 3 轮
    wprintf(L"repeat x3 (warm cache):\n");
    for (int r = 0; r < 3; ++r) {
        double t2 = NowMs();
        int ok = 0;
        for (auto& p : paths) {
            HICON hS = nullptr;
            if (ExtractIconExW(p.c_str(), 0, nullptr, &hS, 1) > 0 && hS) { ++ok; DestroyIcon(hS); }
        }
        wprintf(L"  round %d: %.2f ms, %d ok\n", r + 1, NowMs() - t2, ok);
    }

    return 0;
}
