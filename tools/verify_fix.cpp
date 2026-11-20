// verify_fix.cpp - 验证修正后的完整链路：class88 → NT路径 → DOS路径 → 图标
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

static std::wstring QueryClass88(DWORD pid) {
    wchar_t buf[2048];
    SPIDI info{};
    info.ProcessId = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(pid));
    info.ImageName.Length = 0;
    info.ImageName.MaximumLength = sizeof(buf);
    info.ImageName.Buffer = buf;
    ULONG needed = 0;
    if (!NT_SUCCESS(g_NtQSI(88, &info, sizeof(info), &needed))) return L"";
    if (info.ImageName.Length == 0) return L"";
    return std::wstring(info.ImageName.Buffer, info.ImageName.Length / sizeof(wchar_t));
}

int wmain() {
    _setmode(_fileno(stdout), _O_U8TEXT);

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    g_NtQSI = reinterpret_cast<NtQSI_t>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
    if (!g_NtQSI) return 1;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // 收集所有进程
    struct Item { DWORD pid; std::wstring nt; std::wstring dos; bool iconOk; };
    std::vector<Item> items;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (pe.th32ProcessID == 0) continue;
                Item it;
                it.pid = pe.th32ProcessID;
                it.nt = QueryClass88(it.pid);
                it.iconOk = false;
                if (!it.nt.empty()) items.push_back(it);
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    wprintf(L"拿到了 %zu 个进程的 NT 路径\n\n", items.size());

    // 全量转换 + 提图标，计时
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);

    QueryPerformanceCounter(&t0);
    int converted = 0;
    for (auto& it : items) {
        it.dos = tmp::NtPathToDos(it.nt);
        if (!it.dos.empty()) ++converted;
    }
    QueryPerformanceCounter(&t1);

    // 去重后提图标（同一个 exe 只提一次，模拟真实缓存策略）
    std::vector<std::wstring> uniq;
    for (const auto& it : items) {
        if (it.dos.empty()) continue;
        bool dup = false;
        for (const auto& u : uniq) if (_wcsicmp(u.c_str(), it.dos.c_str()) == 0) { dup = true; break; }
        if (!dup) uniq.push_back(it.dos);
    }

    LARGE_INTEGER t2, t3;
    QueryPerformanceCounter(&t2);
    int iconOk = 0;
    for (const auto& p : uniq) {
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW(p.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON) && sfi.hIcon) {
            ++iconOk;
            DestroyIcon(sfi.hIcon);
        }
    }
    QueryPerformanceCounter(&t3);

    wprintf(L"=== 结果 ===\n");
    wprintf(L"  NT 路径获取     : %zu / %zu\n", items.size(), items.size());
    wprintf(L"  成功转 DOS 路径 : %d  (%.1f%%)  %.3f ms\n",
            converted, items.empty() ? 0.0 : converted * 100.0 / items.size(),
            (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    wprintf(L"  唯一 exe 数     : %zu\n", uniq.size());
    wprintf(L"  提图标成功      : %d / %zu (%.1f%%)  %.3f ms total, %.3f ms/each\n",
            iconOk, uniq.size(),
            uniq.empty() ? 0.0 : iconOk * 100.0 / uniq.size(),
            (double)(t3.QuadPart - t2.QuadPart) * 1000.0 / f.QuadPart,
            uniq.empty() ? 0.0 : (double)(t3.QuadPart - t2.QuadPart) * 1000.0 / f.QuadPart / uniq.size());

    // 列出失败的
    wprintf(L"\n=== 转换失败的样本（前 10 个）===\n");
    int shown = 0;
    for (const auto& it : items) {
        if (it.dos.empty() && shown < 10) {
            wprintf(L"  pid=%5lu  %ls\n", it.pid, it.nt.c_str());
            ++shown;
        }
    }

    wprintf(L"\n=== 成功样本（前 8 个）===\n");
    shown = 0;
    for (const auto& it : items) {
        if (!it.dos.empty() && shown < 8) {
            wprintf(L"  pid=%5lu\n    NT : %ls\n    DOS: %ls\n", it.pid, it.nt.c_str(), it.dos.c_str());
            ++shown;
        }
    }

    CoUninitialize();
    return 0;
}
