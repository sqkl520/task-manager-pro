// svchost_indirect_probe.cpp - 验证 @dll,-id 间接字符串的解析结果与语言
#include <windows.h>
#include <clocale>
#include <winsvc.h>
#include <shlwapi.h>
#include <stdio.h>
#include <vector>
#include <string>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shlwapi.lib")

static void Show(const wchar_t* label, const wchar_t* s) {
    wprintf(L"    %-30ls = [%ls]\n", label, s ? s : L"(null)");
}

int main() {
    SetConsoleOutputCP(65001);
    _wsetlocale(LC_ALL, L".UTF-8");

    const wchar_t* cases[][2] = {
        { L"TermService",  L"@%SystemRoot%\\System32\\termsrv.dll,-268" },
        { L"RpcSs",        L"@combase.dll,-5010" },
        { L"DcomLaunch",   L"@combase.dll,-5012" },
        { L"cbdhsvc",      L"@%SystemRoot%\\system32\\cbdhsvc.dll,-100" },
        { L"WpnUserService", L"@%SystemRoot%\\system32\\WpnUserService.dll,-1" },
    };

    printf("=== SHLoadIndirectString（taskmgr/WMI 用的正是这条）===\n");
    for (auto& c : cases) {
        wchar_t out[512] = {};
        HRESULT hr = SHLoadIndirectString(c[1], out, 512, nullptr);
        wprintf(L"    %-16ls hr=0x%08lX  \"%ls\"\n", c[0], (unsigned long)hr, out);
    }

    printf("\n=== 强制 zh-CN 线程语言后再试 ===\n");
    SetThreadUILanguage(0x0804);
    SetThreadPreferredUILanguages(MUI_LANGUAGE_NAME, L"zh-CN", nullptr);
    for (auto& c : cases) {
        wchar_t out[512] = {};
        HRESULT hr = SHLoadIndirectString(c[1], out, 512, nullptr);
        wprintf(L"    %-16ls hr=0x%08lX  \"%ls\"\n", c[0], (unsigned long)hr, out);
    }

    printf("\n=== EnumServicesStatusExW 给的 lpDisplayName ===\n");
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    DWORD need = 0, count = 0, resume = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
        nullptr, 0, &need, &count, &resume, nullptr);
    std::vector<BYTE> buf(need + 8192);
    resume = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
        buf.data(), (DWORD)buf.size(), &need, &count, &resume, nullptr);
    auto* arr = reinterpret_cast<LPENUM_SERVICE_STATUS_PROCESSW>(buf.data());
    int zh = 0, en = 0, other = 0;
    for (DWORD i = 0; i < count; ++i) {
        const wchar_t* d = arr[i].lpDisplayName;
        if (!d) continue;
        bool nonAscii = false;
        for (const wchar_t* p = d; *p; ++p) if (*p > 0x7F) { nonAscii = true; break; }
        if (nonAscii) zh++; else en++;
    }
    wprintf(L"    共 %lu 个服务：含非 ASCII（中文）%d 个 / 纯 ASCII %d 个\n", count, zh, en);
    for (DWORD i = 0; i < count; ++i) {
        if (_wcsicmp(arr[i].lpServiceName, L"TermService") == 0 ||
            _wcsicmp(arr[i].lpServiceName, L"RpcSs") == 0 ||
            _wcsicmp(arr[i].lpServiceName, L"cbdhsvc") == 0)
            wprintf(L"    %-16ls -> \"%ls\"\n", arr[i].lpServiceName, arr[i].lpDisplayName);
    }
    CloseServiceHandle(scm);

    printf("\n完成。\n");
    return 0;
}
