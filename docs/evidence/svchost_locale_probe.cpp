// svchost_locale_probe.cpp - 为何 lpDisplayName 有时中文有时英文 + 热路径计时
#include <windows.h>
#include <clocale>
#include <winsvc.h>
#include <stdio.h>
#include <vector>
#include <string>
#include <algorithm>
#pragma comment(lib, "advapi32.lib")

static double NowMs() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

int main() {
    SetConsoleOutputCP(65001);

    printf("=== 语言环境 ===\n");
    printf("    GetUserDefaultUILanguage      = 0x%04X\n", GetUserDefaultUILanguage());
    printf("    GetSystemDefaultUILanguage    = 0x%04X\n", GetSystemDefaultUILanguage());
    printf("    GetUserDefaultLocaleName      = ");
    wchar_t ln[LOCALE_NAME_MAX_LENGTH] = {};
    if (GetUserDefaultLocaleName(ln, LOCALE_NAME_MAX_LENGTH)) wprintf(L"%ls\n", ln); else printf("?\n");
    printf("    GetSystemDefaultLocaleName    = ");
    if (GetSystemDefaultLocaleName(ln, LOCALE_NAME_MAX_LENGTH)) wprintf(L"%ls\n", ln); else printf("?\n");
    wchar_t ui[LOCALE_NAME_MAX_LENGTH] = {};
    printf("    GetUserDefaultUILanguage name = ");
    if (GetUserDefaultLocaleName(ui, LOCALE_NAME_MAX_LENGTH)) wprintf(L"%ls\n", ui); else printf("?\n");
    printf("    GetThreadUILanguage           = 0x%04X\n", GetThreadUILanguage());

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    DWORD need = 0, count = 0, resume = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
        nullptr, 0, &need, &count, &resume, nullptr);
    std::vector<BYTE> buf(need + 8192);

    auto dumpOne = [&](const wchar_t* svc) {
        resume = 0;
        if (!EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
            buf.data(), (DWORD)buf.size(), &need, &count, &resume, nullptr)) return;
        auto* arr = reinterpret_cast<LPENUM_SERVICE_STATUS_PROCESSW>(buf.data());
        for (DWORD i = 0; i < count; ++i) {
            if (_wcsicmp(arr[i].lpServiceName, svc) == 0) {
                wprintf(L"    [%ls] display = %ls\n", svc, arr[i].lpDisplayName);
                return;
            }
        }
    };

    printf("\n=== lpDisplayName 当前线程默认 ===\n");
    dumpOne(L"TermService");
    dumpOne(L"RpcSs");
    dumpOne(L"DcomLaunch");

    // 强制中文（2052 = zh-CN）
    printf("\n=== SetThreadUILanguage(2052) 后 ===\n");
    SetThreadUILanguage(0x0804);
    dumpOne(L"TermService");
    dumpOne(L"RpcSs");
    dumpOne(L"DcomLaunch");

    printf("\n=== SetThreadPreferredUILanguages(zh-CN) 后 ===\n");
    SetThreadPreferredUILanguages(MUI_LANGUAGE_NAME, L"zh-CN", nullptr);
    dumpOne(L"TermService");
    dumpOne(L"RpcSs");
    dumpOne(L"DcomLaunch");

    // 热路径计时（只做完整枚举，不含 size probe）
    printf("\n=== 热路径 EnumServicesStatusExW 计时（不含 size probe）===\n");
    std::vector<double> t;
    for (int i = 0; i < 20; ++i) {
        double t0 = NowMs();
        resume = 0;
        EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
            buf.data(), (DWORD)buf.size(), &need, &count, &resume, nullptr);
        t.push_back(NowMs() - t0);
    }
    std::sort(t.begin(), t.end());
    printf("    n=20 min=%.3f p50=%.3f p95=%.3f max=%.3f ms\n",
        t.front(), t[t.size()/2], t[(size_t)(t.size()*0.95)], t.back());

    CloseServiceHandle(scm);
    printf("\n完成。\n");
    return 0;
}
