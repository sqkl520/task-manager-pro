// icontest2.cpp - 验证原版方案：LoadLibraryEx + LoadImage 直接取 exe 主图标
// 对比三种方案的速度与成功率：
//   A) SHGetFileInfoW(DOS path)
//   B) LoadLibraryExW(LOAD_LIBRARY_AS_DATAFILE) + LoadImageW
//   C) ExtractIconExW
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <psapi.h>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "psapi.lib")

static std::wstring NtToDos(const std::wstring& nt) {
    if (nt.size() > 1 && nt[1] == L':') return nt;
    wchar_t target[2048];
    for (wchar_t c = L'A'; c <= L'Z'; ++c) {
        wchar_t drive[3] = { c, L':', 0 };
        DWORD len = QueryDosDeviceW(drive, target, 2048);
        if (len == 0) continue;
        std::wstring dev(target);
        if (!dev.empty() && nt.compare(0, dev.size(), dev) == 0) {
            return std::wstring(1, c) + L":" + nt.substr(dev.size());
        }
    }
    return L"";
}

static double NowMs() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

int wmain() {
    _setmode(_fileno(stdout), _O_U8TEXT);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // 收集一批真实 exe 路径
    std::vector<std::wstring> exes;
    HMODULE hMods[1024]; DWORD cbNeeded;
    if (EnumProcessModules(GetCurrentProcess(), hMods, sizeof(hMods), &cbNeeded)) {
        for (unsigned i = 0; i < cbNeeded / sizeof(HMODULE); ++i) {
            wchar_t p[MAX_PATH];
            if (GetModuleFileNameExW(GetCurrentProcess(), hMods[i], p, MAX_PATH)) {
                exes.emplace_back(p);
            }
        }
    }
    // 补充一些系统 exe
    const wchar_t* extra[] = {
        L"C:\\Windows\\System32\\notepad.exe",
        L"C:\\Windows\\System32\\calc.exe",
        L"C:\\Windows\\explorer.exe",
        L"C:\\Windows\\System32\\mspaint.exe",
        L"C:\\Windows\\System32\\taskmgr.exe",
        L"C:\\Windows\\System32\\cmd.exe",
    };
    for (auto* e : extra) exes.emplace_back(e);

    wprintf(L"=== %zu exes ===\n\n", exes.size());

    // A) SHGetFileInfoW
    {
        int ok = 0; double t = NowMs();
        for (auto& p : exes) {
            SHFILEINFOW sfi{};
            if (SHGetFileInfoW(p.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON) && sfi.hIcon) {
                ++ok; DestroyIcon(sfi.hIcon);
            }
        }
        wprintf(L"A) SHGetFileInfoW      : %d/%zu ok, %.2f ms total, %.3f ms/each\n",
                ok, exes.size(), NowMs() - t, (NowMs() - t) / exes.size());
    }

    // B) LoadLibraryEx + LoadImage  （原版方案）
    {
        int ok = 0; double t = NowMs();
        for (auto& p : exes) {
            HMODULE h = LoadLibraryExW(p.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
            if (!h) continue;
            HICON hIco = (HICON)LoadImageW(h, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
            if (hIco) { ++ok; DestroyIcon(hIco); }
            FreeLibrary(h);
        }
        wprintf(L"B) LoadLibraryEx+Load: %d/%zu ok, %.2f ms total, %.3f ms/each\n",
                ok, exes.size(), NowMs() - t, (NowMs() - t) / exes.size());
    }
    // B2) 不销毁，测纯加载速度
    {
        int ok = 0; double t = NowMs();
        for (auto& p : exes) {
            HMODULE h = LoadLibraryExW(p.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
            if (!h) continue;
            HICON hIco = (HICON)LoadImageW(h, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
            if (hIco) ++ok;
            FreeLibrary(h);
        }
        wprintf(L"B2) same but no Destroy: %d/%zu ok, %.2f ms total\n",
                ok, exes.size(), NowMs() - t);
    }

    // C) ExtractIconExW
    {
        int ok = 0; double t = NowMs();
        for (auto& p : exes) {
            HICON hSmall = nullptr;
            UINT n = ExtractIconExW(p.c_str(), 0, nullptr, &hSmall, 1);
            if (n > 0 && hSmall) { ++ok; DestroyIcon(hSmall); }
        }
        wprintf(L"C) ExtractIconExW     : %d/%zu ok, %.2f ms total, %.3f ms/each\n",
                ok, exes.size(), NowMs() - t, (NowMs() - t) / exes.size());
    }

    // 验证 NT 路径直喂 LoadLibraryEx
    wprintf(L"\n=== NT path -> LoadLibraryEx direct? ===\n");
    {
        const wchar_t* nt = L"\\Device\\HarddiskVolume3\\Windows\\System32\\notepad.exe";
        HMODULE h = LoadLibraryExW(nt, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        wprintf(L"  NT path LoadLibraryEx: %ls (err=%lu)\n", h ? L"OK" : L"FAIL", GetLastError());
        if (h) {
            HICON hIco = (HICON)LoadImageW(h, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
            wprintf(L"  LoadImage: %ls\n", hIco ? L"OK" : L"FAIL");
            if (hIco) DestroyIcon(hIco);
            FreeLibrary(h);
        }
        std::wstring dos = NtToDos(nt);
        wprintf(L"  converted: %ls\n", dos.c_str());
        HMODULE h2 = LoadLibraryExW(dos.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        wprintf(L"  DOS path LoadLibraryEx: %ls\n", h2 ? L"OK" : L"FAIL");
        if (h2) {
            HICON hIco = (HICON)LoadImageW(h2, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
            wprintf(L"  LoadImage: %ls\n", hIco ? L"OK" : L"FAIL");
            if (hIco) DestroyIcon(hIco);
            FreeLibrary(h2);
        }
    }

    CoUninitialize();
    return 0;
}
