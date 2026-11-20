// srumprobe.cpp - 用最直接的方式探测 srumapi.dll
#include <windows.h>
#include <cstdio>
#include <string>

int main() {
    FILE* f = nullptr;
    fopen_s(&f, "srum_probe.txt", "w");

    auto P = [&](const char* s) { if (f) { fputs(s, f); fflush(f); } };
    auto PW = [&](const wchar_t* s) { if (f) { fwprintf(f, L"%ls", s); fflush(f); } };

    P("=== srumapi.dll probe ===\n");

    HMODULE m = LoadLibraryExW(L"C:\\Windows\\System32\\srumapi.dll", nullptr,
                               LOAD_LIBRARY_AS_DATAFILE);
    if (!m) {
        char buf[128];
        sprintf_s(buf, "LoadLibraryEx(DATAFILE) failed: %lu\n", GetLastError());
        P(buf);
        m = LoadLibraryW(L"C:\\Windows\\System32\\srumapi.dll");
        sprintf_s(buf, "LoadLibrary failed: %lu (handle=%p)\n", GetLastError(), (void*)m);
        P(buf);
        if (!m) { fclose(f); return 1; }
        P("Loaded via LoadLibrary (executable mode)\n");
    } else {
        P("Loaded via LoadLibraryEx(DATAFILE)\n");
    }

    // 手工解析 PE 导出表
    auto* base = reinterpret_cast<const BYTE*>(m);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];

    char buf[512];
    sprintf_s(buf, "e_lfanew=%ld exportRVA=0x%lX exportSize=%lu\n",
              dos->e_lfanew, dir.VirtualAddress, dir.Size);
    P(buf);

    if (dir.VirtualAddress == 0) {
        P("NO EXPORTS\n");
    } else {
        auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
        sprintf_s(buf, "DLLname=%.60s names=%lu funcs=%lu base=%lu\n",
                  reinterpret_cast<const char*>(base + exp->Name),
                  exp->NumberOfNames, exp->NumberOfFunctions, exp->Base);
        P(buf);

        auto* names = reinterpret_cast<const DWORD*>(base + exp->AddressOfNames);
        auto* ords  = reinterpret_cast<const WORD*>(base + exp->AddressOfNameOrdinals);
        for (DWORD i = 0; i < exp->NumberOfNames && i < 200; ++i) {
            const char* nm = reinterpret_cast<const char*>(base + names[i]);
            sprintf_s(buf, "  [%4lu] %s\n", exp->Base + ords[i], nm);
            P(buf);
        }
    }

    FreeLibrary(m);
    fclose(f);
    return 0;
}
