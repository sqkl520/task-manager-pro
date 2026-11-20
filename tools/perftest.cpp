// perftest.cpp - 图标提取的性能对比：找出 450 进程场景下最快的方案
#include "ntpath.h"

#include <windows.h>
#include <objbase.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <ntstatus.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_map>
#include <fcntl.h>
#include <io.h>

#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

typedef NTSTATUS (NTAPI* NtQSI_t)(ULONG, PVOID, ULONG, PULONG);
typedef struct _SPIDI { HANDLE ProcessId; UNICODE_STRING ImageName; } SPIDI;
static NtQSI_t g_NtQSI = nullptr;

static std::wstring Q88(DWORD pid) {
    wchar_t buf[2048];
    SPIDI info{};
    info.ProcessId = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(pid));
    info.ImageName.Length = 0;
    info.ImageName.MaximumLength = sizeof(buf);
    info.ImageName.Buffer = buf;
    ULONG n = 0;
    if (!NT_SUCCESS(g_NtQSI(88, &info, sizeof(info), &n)) || info.ImageName.Length == 0) return L"";
    return std::wstring(info.ImageName.Buffer, info.ImageName.Length / sizeof(wchar_t));
}

static double Ms(LARGE_INTEGER& a, LARGE_INTEGER& b, LARGE_INTEGER& f) {
    return (double)(b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
}

int wmain() {
    _setmode(_fileno(stdout), _O_U8TEXT);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    g_NtQSI = reinterpret_cast<NtQSI_t>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
    if (!g_NtQSI) return 1;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // 收集唯一 exe 路径
    std::vector<std::wstring> uniq;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (pe.th32ProcessID == 0) continue;
                std::wstring nt = Q88(pe.th32ProcessID);
                if (nt.empty()) continue;
                std::wstring dos = tmp::NtPathToDos(nt);
                if (dos.empty()) continue;
                bool dup = false;
                for (const auto& u : uniq) if (_wcsicmp(u.c_str(), dos.c_str()) == 0) { dup = true; break; }
                if (!dup) uniq.push_back(dos);
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    wprintf(L"唯一 exe 路径数：%zu\n\n", uniq.size());

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);

    // ---- 方案 A: SHGetFileInfoW 不带 USEFILEATTRIBUTES（真实读取 PE 资源）----
    {
        QueryPerformanceCounter(&t0);
        int ok = 0;
        for (const auto& p : uniq) {
            SHFILEINFOW s{};
            if (SHGetFileInfoW(p.c_str(), 0, &s, sizeof(s), SHGFI_ICON | SHGFI_SMALLICON) && s.hIcon) {
                DestroyIcon(s.hIcon); ++ok;
            }
        }
        QueryPerformanceCounter(&t1);
        double ms = Ms(t0, t1, f);
        wprintf(L"A. SHGetFileInfo (real)      : %d/%zu ok, %8.1f ms total, %6.2f ms/each\n",
                ok, uniq.size(), ms, ms / uniq.size());
    }

    // ---- 方案 B: SHGetFileInfoW + USEFILEATTRIBUTES（只按扩展名取通用图标）----
    {
        QueryPerformanceCounter(&t0);
        int ok = 0;
        for (const auto& p : uniq) {
            SHFILEINFOW s{};
            if (SHGetFileInfoW(p.c_str(), FILE_ATTRIBUTE_NORMAL, &s, sizeof(s),
                               SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES) && s.hIcon) {
                DestroyIcon(s.hIcon); ++ok;
            }
        }
        QueryPerformanceCounter(&t1);
        double ms = Ms(t0, t1, f);
        wprintf(L"B. SHGetFileInfo (+USEFILEATTR): %d/%zu ok, %8.1f ms total, %6.2f ms/each\n",
                ok, uniq.size(), ms, ms / uniq.size());
    }

    // ---- 方案 C: ExtractIconExW ----
    {
        QueryPerformanceCounter(&t0);
        int ok = 0;
        for (const auto& p : uniq) {
            HICON lg = nullptr, sm = nullptr;
            if (ExtractIconExW(p.c_str(), 0, &lg, &sm, 1) > 0) {
                if (sm) { DestroyIcon(sm); ++ok; }
                if (lg) DestroyIcon(lg);
            }
        }
        QueryPerformanceCounter(&t1);
        double ms = Ms(t0, t1, f);
        wprintf(L"C. ExtractIconEx             : %d/%zu ok, %8.1f ms total, %6.2f ms/each\n",
                ok, uniq.size(), ms, ms / uniq.size());
    }

    // ---- 方案 D: 第二次跑（模拟缓存命中，OS 文件缓存已热）----
    {
        QueryPerformanceCounter(&t0);
        int ok = 0;
        for (const auto& p : uniq) {
            SHFILEINFOW s{};
            if (SHGetFileInfoW(p.c_str(), 0, &s, sizeof(s), SHGFI_ICON | SHGFI_SMALLICON) && s.hIcon) {
                DestroyIcon(s.hIcon); ++ok;
            }
        }
        QueryPerformanceCounter(&t1);
        double ms = Ms(t0, t1, f);
        wprintf(L"D. SHGetFileInfo 第二轮(热OS缓存): %d/%zu ok, %8.1f ms total, %6.2f ms/each\n",
                ok, uniq.size(), ms, ms / uniq.size());
    }

    // ---- 方案 E: IShellItemImageFactory ----
    {
        QueryPerformanceCounter(&t0);
        int ok = 0;
        for (const auto& p : uniq) {
            IShellItem* item = nullptr;
            HRESULT hr = SHCreateItemFromParsingName(p.c_str(), nullptr, IID_PPV_ARGS(&item));
            if (SUCCEEDED(hr) && item) {
                IShellItemImageFactory* fac = nullptr;
                hr = item->QueryInterface(IID_PPV_ARGS(&fac));
                if (SUCCEEDED(hr) && fac) {
                    HBITMAP hbmp = nullptr;
                    SIZE sz{ 16, 16 };
                    hr = fac->GetImage(sz, SIIGBF_ICONONLY, &hbmp);
                    if (SUCCEEDED(hr) && hbmp) { DeleteObject(hbmp); ++ok; }
                    fac->Release();
                }
                item->Release();
            }
        }
        QueryPerformanceCounter(&t1);
        double ms = Ms(t0, t1, f);
        wprintf(L"E. IShellItemImageFactory   : %d/%zu ok, %8.1f ms total, %6.2f ms/each\n",
                ok, uniq.size(), ms, ms / uniq.size());
    }

    // ---- 方案 F: LoadImage 直接读 PE（不经过 shell）----
    {
        QueryPerformanceCounter(&t0);
        int ok = 0;
        for (const auto& p : uniq) {
            HICON ico = (HICON)LoadImageW(nullptr, p.c_str(), IMAGE_ICON, 16, 16,
                                          LR_LOADFROMFILE | LR_DEFAULTSIZE);
            if (ico) { DestroyIcon(ico); ++ok; }
        }
        QueryPerformanceCounter(&t1);
        double ms = Ms(t0, t1, f);
        wprintf(L"F. LoadImage (direct PE)    : %d/%zu ok, %8.1f ms total, %6.2f ms/each\n",
                ok, uniq.size(), ms, ms / uniq.size());
    }

    wprintf(L"\n=== 450 个进程（唯一 exe 约 140）的估算 ===\n");
    wprintf(L"  若每进程都提（含重复）：A 方案约 %.0f ms\n", 450 * 15.0);
    wprintf(L"  去重后 140 个          ：A 方案约 %.0f ms\n", 140 * 15.0);
    wprintf(L"  结论：必须放后台线程，且必须按 exe 路径去重缓存\n");

    CoUninitialize();
    return 0;
}
