// icontest.cpp - 诊断 class 88 路径格式 + SHGetFileInfoW + NT->DOS 转换
#include <windows.h>
#include <objbase.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <ntstatus.h>
#include <shellapi.h>
#include <cstdio>
#include <string>
#include <vector>
#include <utility>
#include <fcntl.h>
#include <io.h>

#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")

typedef NTSTATUS (NTAPI* NtQSI_t)(ULONG, PVOID, ULONG, PULONG);

typedef struct _SPIDI {
    HANDLE ProcessId;
    UNICODE_STRING ImageName;
} SPIDI;

static NtQSI_t g_NtQSI = nullptr;

static std::wstring QueryClass88(DWORD pid) {
    wchar_t buf[2048];
    SPIDI info{};
    info.ProcessId = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(pid));
    info.ImageName.Length = 0;
    info.ImageName.MaximumLength = sizeof(buf);
    info.ImageName.Buffer = buf;
    ULONG needed = 0;
    NTSTATUS st = g_NtQSI(88, &info, sizeof(info), &needed);
    if (!NT_SUCCESS(st) || info.ImageName.Length == 0) return L"";
    return std::wstring(info.ImageName.Buffer, info.ImageName.Length / sizeof(wchar_t));
}

static std::vector<std::pair<std::wstring, std::wstring>> BuildDeviceMap() {
    std::vector<std::pair<std::wstring, std::wstring>> map;
    wchar_t target[2048];
    for (wchar_t c = L'A'; c <= L'Z'; ++c) {
        wchar_t drive[3] = { c, L':', 0 };
        DWORD len = QueryDosDeviceW(drive, target, 2048);
        if (len > 0) map.push_back({ std::wstring(target), std::wstring(drive) });
    }
    return map;
}

static const std::vector<std::pair<std::wstring, std::wstring>>& DevMap() {
    static std::vector<std::pair<std::wstring, std::wstring>> m = BuildDeviceMap();
    return m;
}

static std::wstring NtToDos_QueryDosDevice(const std::wstring& nt) {
    for (const auto& kv : DevMap()) {
        if (nt.size() >= kv.first.size() &&
            _wcsnicmp(nt.c_str(), kv.first.c_str(), kv.first.size()) == 0) {
            return kv.second + nt.substr(kv.first.size());
        }
    }
    return L"";
}

static std::wstring NtToDos_ByHandle(const std::wstring& nt) {
    HANDLE h = CreateFileW(nt.c_str(), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::wstring q = L"\\??\\" + nt;
        h = CreateFileW(q.c_str(), 0,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) return L"";
    }
    wchar_t out[MAX_PATH * 2] = {};
    DWORD n = GetFinalPathNameByHandleW(h, out, MAX_PATH * 2,
                                        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (n == 0) return L"";
    std::wstring s(out, n);
    if (s.rfind(L"\\\\?\\", 0) == 0) s = s.substr(4);
    return s;
}

static void TestIcon(const wchar_t* label, const std::wstring& path) {
    if (path.empty()) { wprintf(L"  %-22ls : <empty>\n", label); return; }
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);

    SHFILEINFOW sfi{};
    QueryPerformanceCounter(&t0);
    BOOL ok = SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi),
                             SHGFI_ICON | SHGFI_SMALLICON);
    QueryPerformanceCounter(&t1);
    double msA = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    wprintf(L"  %-22ls : plain      ok=%d icon=%p  %.3f ms\n",
            label, ok, (void*)sfi.hIcon, msA);
    if (sfi.hIcon) DestroyIcon(sfi.hIcon);

    SHFILEINFOW sfi2{};
    QueryPerformanceCounter(&t0);
    BOOL ok2 = SHGetFileInfoW(path.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi2, sizeof(sfi2),
                              SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    QueryPerformanceCounter(&t1);
    double msB = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    wprintf(L"  %-22ls : USEFILEATTR ok=%d icon=%p  %.3f ms\n",
            label, ok2, (void*)sfi2.hIcon, msB);
    if (sfi2.hIcon) DestroyIcon(sfi2.hIcon);
}

int wmain() {
    _setmode(_fileno(stdout), _O_U8TEXT);

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    g_NtQSI = reinterpret_cast<NtQSI_t>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
    if (!g_NtQSI) { wprintf(L"no NtQuerySystemInformation\n"); return 1; }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    wprintf(L"CoInitializeEx -> 0x%08X\n\n", hr);

    wprintf(L"=== 1. 设备映射表 ===\n");
    for (const auto& kv : DevMap())
        wprintf(L"  %-4ls -> %ls\n", kv.second.c_str(), kv.first.c_str());
    wprintf(L"\n");

    wprintf(L"=== 2. class 88 返回的路径格式 ===\n");
    std::vector<DWORD> pids;
    pids.push_back(GetCurrentProcessId());
    pids.push_back(4);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (pids.size() >= 14) break;
                pids.push_back(pe.th32ProcessID);
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }

    for (DWORD pid : pids) {
        std::wstring nt = QueryClass88(pid);
        if (nt.empty()) { wprintf(L"  pid=%5lu : <fail/empty>\n", pid); continue; }
        const wchar_t* kind = L"UNKNOWN";
        if (nt.rfind(L"\\Device\\", 0) == 0)       kind = L"\\Device\\";
        else if (nt.rfind(L"\\\\?\\", 0) == 0)     kind = L"\\\\?\\";
        else if (nt.rfind(L"\\??\\", 0) == 0)      kind = L"\\??\\";
        else if (nt.size() >= 2 && nt[1] == L':')  kind = L"DOS";
        wprintf(L"  pid=%5lu : [%-9ls] %ls\n", pid, kind, nt.c_str());
    }
    wprintf(L"\n");

    wprintf(L"=== 3. 同一 exe 不同路径形式 → SHGetFileInfoW ===\n");
    const wchar_t* exe = L"C:\\Windows\\System32\\notepad.exe";

    std::wstring ntConstructed;
    for (const auto& kv : DevMap()) {
        if (kv.second == L"C:") {
            ntConstructed = kv.first + L"\\Windows\\System32\\notepad.exe";
            break;
        }
    }

    wprintf(L"\n  --- (a) DOS 路径 ---\n  路径: %ls\n", exe);
    TestIcon(L"(a) DOS", exe);

    if (!ntConstructed.empty()) {
        wprintf(L"\n  --- (b) NT 设备路径 ---\n  路径: %ls\n", ntConstructed.c_str());
        TestIcon(L"(b) NT device", ntConstructed);
    }

    std::wstring q1 = L"\\??\\C:\\Windows\\System32\\notepad.exe";
    wprintf(L"\n  --- (c) \\??\\ 前缀 ---\n  路径: %ls\n", q1.c_str());
    TestIcon(L"(c) ??\\DOS", q1);

    std::wstring q2 = L"\\\\?\\C:\\Windows\\System32\\notepad.exe";
    wprintf(L"\n  --- (d) \\\\?\\ 前缀 ---\n  路径: %ls\n", q2.c_str());
    TestIcon(L"(d) \\\\?\\DOS", q2);

    wprintf(L"\n=== 4. NT -> DOS 转换对比 ===\n");
    if (!ntConstructed.empty()) {
        wprintf(L"  输入: %ls\n", ntConstructed.c_str());
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);

        QueryPerformanceCounter(&t0);
        std::wstring a = NtToDos_QueryDosDevice(ntConstructed);
        QueryPerformanceCounter(&t1);
        wprintf(L"  A QueryDosDevice -> \"%ls\"  (%.4f ms)\n", a.c_str(),
                (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);

        QueryPerformanceCounter(&t0);
        std::wstring b = NtToDos_ByHandle(ntConstructed);
        QueryPerformanceCounter(&t1);
        wprintf(L"  B GetFinalPath   -> \"%ls\"  (%.4f ms)\n", b.c_str(),
                (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);

        wprintf(L"\n  A 的输出测图标:\n"); TestIcon(L"A result", a);
        wprintf(L"\n  B 的输出测图标:\n"); TestIcon(L"B result", b);
    }

    wprintf(L"\n=== 5. ExtractIconExW ===\n");
    {
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        HICON lg = nullptr, sm = nullptr;
        QueryPerformanceCounter(&t0);
        UINT n = ExtractIconExW(exe, 0, &lg, &sm, 1);
        QueryPerformanceCounter(&t1);
        wprintf(L"  ExtractIconEx n=%u large=%p small=%p  %.3f ms\n",
                n, (void*)lg, (void*)sm,
                (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
        if (lg) DestroyIcon(lg);
        if (sm) DestroyIcon(sm);
    }

    wprintf(L"\n=== 6. 性能：热 / 冷 ===\n");
    {
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        const wchar_t* files[] = {
            L"C:\\Windows\\System32\\notepad.exe",
            L"C:\\Windows\\System32\\mspaint.exe",
            L"C:\\Windows\\System32\\cmd.exe",
            L"C:\\Windows\\System32\\regedit.exe",
            L"C:\\Windows\\System32\\charmap.exe",
            L"C:\\Windows\\System32\\write.exe",
            L"C:\\Windows\\System32\\magnify.exe",
            L"C:\\Windows\\System32\\osk.exe",
        };
        const int nf = 8;

        QueryPerformanceCounter(&t0);
        for (int i = 0; i < 20; ++i) {
            SHFILEINFOW s{};
            if (SHGetFileInfoW(files[0], 0, &s, sizeof(s), SHGFI_ICON | SHGFI_SMALLICON) && s.hIcon)
                DestroyIcon(s.hIcon);
        }
        QueryPerformanceCounter(&t1);
        double hot = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
        wprintf(L"  热同文件 x20 : %.3f ms total, %.3f ms/each\n", hot, hot / 20);

        QueryPerformanceCounter(&t0);
        int ok = 0;
        for (int r = 0; r < 3; ++r)
            for (int i = 0; i < nf; ++i) {
                SHFILEINFOW s{};
                if (SHGetFileInfoW(files[i], 0, &s, sizeof(s), SHGFI_ICON | SHGFI_SMALLICON) && s.hIcon) {
                    DestroyIcon(s.hIcon); ++ok;
                }
            }
        QueryPerformanceCounter(&t1);
        double cold = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
        wprintf(L"  不同文件 x%d : %.3f ms total, %.3f ms/each (ok=%d)\n",
                nf * 3, cold, cold / (nf * 3), ok);
    }

    CoUninitialize();
    return 0;
}
