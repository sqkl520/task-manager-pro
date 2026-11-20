// exports.cpp - 精确读取 DLL 导出表（含序号）
#include <windows.h>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <vector>

static void DumpExports(const wchar_t* path) {
    HMODULE mod = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_AS_DATAFILE);
    if (!mod) { wprintf(L"load fail %lu\n", GetLastError()); return; }

    auto* base = reinterpret_cast<const BYTE*>(mod);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { FreeLibrary(mod); return; }
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { FreeLibrary(mod); return; }

    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    wprintf(L"=== %ls ===\n", path);
    wprintf(L"  export dir RVA=0x%X size=%u\n", dir.VirtualAddress, dir.Size);
    if (!dir.VirtualAddress) { wprintf(L"  (no exports)\n"); FreeLibrary(mod); return; }

    auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    wprintf(L"  DLL name: %hs\n", base + exp->Name);
    wprintf(L"  NumberOfNames=%lu NumberOfFunctions=%lu Base=%lu\n",
            exp->NumberOfNames, exp->NumberOfFunctions, exp->Base);

    auto* names = reinterpret_cast<const DWORD*>(base + exp->AddressOfNames);
    auto* ords  = reinterpret_cast<const WORD*>(base + exp->AddressOfNameOrdinals);
    auto* funcs = reinterpret_cast<const DWORD*>(base + exp->AddressOfFunctions);

    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        const char* nm = reinterpret_cast<const char*>(base + names[i]);
        wprintf(L"  [%3lu] %hs\n", exp->Base + ords[i], nm);
    }
    FreeLibrary(mod);
}

int wmain(int argc, wchar_t** argv) {
    
    if (argc >= 2) { DumpExports(argv[1]); return 0; }
    DumpExports(L"C:\\Windows\\System32\\srumapi.dll");
    return 0;
}
