// perftest2.cpp - 补充：为什么 ExtractIconEx 失败率高 + 组合策略验证
#include "ntpath.h"

#include <windows.h>
#include <objbase.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <ntstatus.h>
#include <shellapi.h>
#include <cstdio>
#include <string>
#include <vector>
#include <fcntl.h>
#include <io.h>

#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

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

int wmain() {
    _setmode(_fileno(stdout), _O_U8TEXT);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    g_NtQSI = reinterpret_cast<NtQSI_t>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
    if (!g_NtQSI) return 1;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

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

    wprintf(L"=== ExtractIconEx 失败样本分析 ===\n");
    int fail = 0;
    for (const auto& p : uniq) {
        HICON lg = nullptr, sm = nullptr;
        UINT n = ExtractIconExW(p.c_str(), 0, &lg, &sm, 1);
        if (lg) DestroyIcon(lg);
        if (sm) DestroyIcon(sm);
        if (n == 0 && fail < 12) {
            // 查看文件是否存在、大小
            HANDLE h = CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
            LARGE_INTEGER sz{};
            bool exists = (h != INVALID_HANDLE_VALUE);
            if (exists) { GetFileSizeEx(h, &sz); CloseHandle(h); }
            wprintf(L"  FAIL n=0  exists=%d  size=%lld  %ls\n",
                    exists, sz.QuadPart, p.c_str());
            ++fail;
        }
    }

    wprintf(L"\n=== 组合策略：ExtractIconEx 失败时回退 SHGetFileInfo ===\n");
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    int viaExtract = 0, viaShell = 0, total_ok = 0;
    for (const auto& p : uniq) {
        HICON lg = nullptr, sm = nullptr;
        UINT n = ExtractIconExW(p.c_str(), 0, &lg, &sm, 1);
        bool got = false;
        if (n > 0 && sm) { DestroyIcon(sm); got = true; ++viaExtract; }
        if (lg) DestroyIcon(lg);
        if (!got) {
            SHFILEINFOW s{};
            if (SHGetFileInfoW(p.c_str(), 0, &s, sizeof(s), SHGFI_ICON | SHGFI_SMALLICON) && s.hIcon) {
                DestroyIcon(s.hIcon);
                got = true;
                ++viaShell;
            }
        }
        if (got) ++total_ok;
    }
    QueryPerformanceCounter(&t1);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    wprintf(L"  成功 %d/%zu  (ExtractIconEx %d + SHGetFileInfo 回退 %d)\n",
            total_ok, uniq.size(), viaExtract, viaShell);
    wprintf(L"  总耗时 %.1f ms, 平均 %.2f ms/each\n", ms, ms / uniq.size());

    CoUninitialize();
    return 0;
}
