// srumprobe2.cpp - 直接从文件读 PE 导出表（不加载 DLL，避免布局问题）
#include <windows.h>
#include <cstdio>
#include <vector>

int main() {
    FILE* f = nullptr;
    fopen_s(&f, "srum_probe2.txt", "w");
    auto P = [&](const char* s) { if (f) { fputs(s, f); fflush(f); } };
    char buf[512];

    P("=== srumapi.dll PE export table (from file) ===\n");

    HANDLE h = CreateFileW(L"C:\\Windows\\System32\\srumapi.dll", GENERIC_READ,
                           FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        sprintf_s(buf, "open failed %lu\n", GetLastError()); P(buf); fclose(f); return 1;
    }
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::vector<BYTE> file(static_cast<size_t>(sz.QuadPart));
    DWORD got = 0;
    ReadFile(h, file.data(), static_cast<DWORD>(file.size()), &got, nullptr);
    CloseHandle(h);

    sprintf_s(buf, "file size = %zu bytes\n", file.size()); P(buf);

    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { P("bad MZ\n"); fclose(f); return 1; }

    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(file.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { P("bad PE\n"); fclose(f); return 1; }

    sprintf_s(buf, "machine=0x%04X sections=%u\n",
              nt->FileHeader.Machine, nt->FileHeader.NumberOfSections);
    P(buf);

    // 节表
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char name[9] = {};
        memcpy(name, sec[i].Name, 8);
        sprintf_s(buf, "  section %-8s VA=0x%08lX VSize=0x%08lX Raw=0x%08lX RawSize=0x%08lX\n",
                  name, sec[i].VirtualAddress, sec[i].Misc.VirtualSize,
                  sec[i].PointerToRawData, sec[i].SizeOfRawData);
        P(buf);
    }

    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    sprintf_s(buf, "export RVA=0x%08lX size=%lu\n", dir.VirtualAddress, dir.Size);
    P(buf);

    if (!dir.VirtualAddress) { P("NO EXPORTS\n"); fclose(f); return 0; }

    // RVA -> 文件偏移
    auto rva2off = [&](DWORD rva) -> DWORD {
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            DWORD va = sec[i].VirtualAddress;
            DWORD vs = sec[i].Misc.VirtualSize;
            if (rva >= va && rva < va + vs) {
                return sec[i].PointerToRawData + (rva - va);
            }
        }
        return 0;
    };

    DWORD expOff = rva2off(dir.VirtualAddress);
    if (!expOff || expOff >= file.size()) { P("bad export offset\n"); fclose(f); return 1; }

    auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(file.data() + expOff);
    DWORD nameOff = rva2off(exp->Name);
    sprintf_s(buf, "DLLname=%.60s names=%lu funcs=%lu base=%lu\n",
              nameOff ? reinterpret_cast<const char*>(file.data() + nameOff) : "?",
              exp->NumberOfNames, exp->NumberOfFunctions, exp->Base);
    P(buf);

    DWORD namesOff = rva2off(exp->AddressOfNames);
    DWORD ordsOff  = rva2off(exp->AddressOfNameOrdinals);
    if (!namesOff || !ordsOff) { P("bad name/ord table\n"); fclose(f); return 1; }

    auto* names = reinterpret_cast<const DWORD*>(file.data() + namesOff);
    auto* ords  = reinterpret_cast<const WORD*>(file.data() + ordsOff);
    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        DWORD no = rva2off(names[i]);
        if (!no || no >= file.size()) continue;
        sprintf_s(buf, "  [%4lu] %s\n", exp->Base + ords[i],
                  reinterpret_cast<const char*>(file.data() + no));
        P(buf);
    }

    fclose(f);
    return 0;
}
