// muiall.cpp - 完整提取 taskmgr.exe.mui 的所有可读字符串资源
// 包括 RT_STRING(6)、RT_MENU(4)、RT_DIALOG(5)、RT_ACCELERATOR(9)
#include <windows.h>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <vector>

static std::wstring ResNameToStr(LPCWSTR name) {
    if (reinterpret_cast<uintptr_t>(name) >> 16) return name;
    wchar_t b[24];
    swprintf_s(b, L"#%u", static_cast<unsigned>(reinterpret_cast<uintptr_t>(name)));
    return b;
}

// 从一块裸数据里扫出所有 UTF-16 可读字符串
static void ScanWide(const BYTE* data, DWORD size, const wchar_t* tag) {
    const BYTE* p = data;
    const BYTE* end = data + size;
    while (p + 2 <= end) {
        wchar_t c = *reinterpret_cast<const wchar_t*>(p);
        if (c >= 0x20) {
            std::wstring s;
            const BYTE* r = p;
            bool ok = true;
            while (r + 2 <= end) {
                wchar_t ch = *reinterpret_cast<const wchar_t*>(r);
                if (ch == 0) break;
                if (ch < 0x20) { ok = false; break; }
                s.push_back(ch);
                r += 2;
                if (s.size() > 400) { ok = false; break; }
            }
            if (ok && s.size() >= 2) {
                wprintf(L"%ls %ls\n", tag, s.c_str());
            }
            p = r + 2;
        } else {
            p += 2;
        }
    }
}

static BOOL WINAPI EnumNameCb(HMODULE h, LPCWSTR type, LPCWSTR name, LPARAM lp) {
    const wchar_t* tag = reinterpret_cast<const wchar_t*>(lp);
    HRSRC res = FindResourceW(h, name, type);
    if (!res) return TRUE;
    HGLOBAL hg = LoadResource(h, res);
    DWORD sz = SizeofResource(h, res);
    const BYTE* d = static_cast<const BYTE*>(LockResource(hg));
    if (d && sz) {
        std::wstring nm = ResNameToStr(name);
        wchar_t tagBuf[128];
        swprintf_s(tagBuf, L"[%ls %ls]", tag, nm.c_str());
        ScanWide(d, sz, tagBuf);
    }
    return TRUE;
}

static BOOL WINAPI EnumTypeCb(HMODULE h, LPCWSTR type, LPARAM lp) {
    unsigned t = static_cast<unsigned>(reinterpret_cast<uintptr_t>(type));
    if (reinterpret_cast<uintptr_t>(type) >> 16) return TRUE;   // 具名类型跳过
    const wchar_t* tag = nullptr;
    switch (t) {
    case 4:  tag = L"MENU"; break;
    case 5:  tag = L"DLG";  break;
    case 6:  tag = L"STR";  break;
    case 9:  tag = L"ACCEL";break;
    case 240:tag = L"DLGINIT"; break;
    default: return TRUE;
    }
    EnumResourceNamesW(h, type, (ENUMRESNAMEPROCW)EnumNameCb,
                       reinterpret_cast<LPARAM>(const_cast<wchar_t*>(tag)));
    return TRUE;
}

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    const wchar_t* path = (argc >= 2) ? argv[1]
                                      : L"C:\\Windows\\System32\\zh-CN\\taskmgr.exe.mui";
    HMODULE mod = LoadLibraryExW(path, nullptr,
                                 LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!mod) { wprintf(L"load failed %lu\n", GetLastError()); return 1; }

    wprintf(L"=== %ls ===\n", path);
    EnumResourceTypesW(mod, (ENUMRESTYPEPROCW)EnumTypeCb, 0);
    FreeLibrary(mod);
    return 0;
}
