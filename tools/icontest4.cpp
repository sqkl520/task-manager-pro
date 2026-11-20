#include <windows.h>
#include <shellapi.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <set>
#include <vector>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "psapi.lib")
int wmain() {
    _setmode(_fileno(stdout), _O_U8TEXT);
    std::set<std::wstring> uniq;
    std::vector<std::wstring> paths;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) do {
        if (!pe.th32ProcessID) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t buf[MAX_PATH*2]; DWORD n = MAX_PATH*2;
        if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
            if (uniq.insert(std::wstring(buf,n)).second) paths.emplace_back(buf,n);
        }
        CloseHandle(h);
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap);

    int shown = 0;
    for (auto& p : paths) {
        HICON hS = nullptr;
        UINT n = ExtractIconExW(p.c_str(), 0, nullptr, &hS, 1);
        bool ok = (n > 0 && hS);
        if (hS) DestroyIcon(hS);
        if (!ok && shown < 25) {
            DWORD attr = GetFileAttributesW(p.c_str());
            wprintf(L"FAIL  attr=0x%08X  %ls\n", attr, p.c_str());
            ++shown;
        }
    }
    return 0;
}
