// mui_dump.cpp - 提取 taskmgr.exe.mui 里的所有字符串资源
// MUI 是标准 PE 文件，字符串在 .rsrc 的 RT_STRING 资源里
#include <windows.h>
#include <cstdio>
#include <vector>
#include <string>

static void DumpStringTable(HMODULE mod, const wchar_t* label) {
    // RT_STRING 资源 ID 每 16 个字符串一组，组 ID 从 1 开始
    for (int block = 1; block < 200; ++block) {
        HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(block), MAKEINTRESOURCEW(6)); // 6 = RT_STRING
        if (!res) continue;
        HGLOBAL hg = LoadResource(mod, res);
        if (!hg) continue;
        DWORD size = SizeofResource(mod, res);
        const BYTE* p = static_cast<const BYTE*>(LockResource(hg));
        if (!p) continue;

        const BYTE* end = p + size;
        for (int i = 0; i < 16 && p + 2 <= end; ++i) {
            WORD len = *reinterpret_cast<const WORD*>(p);
            p += 2;
            if (len > 0 && p + len * 2 <= end) {
                std::wstring s(reinterpret_cast<const wchar_t*>(p), len);
                if (!s.empty()) {
                    // 只输出含中文或看起来像 UI 文案的
                    bool hasCJK = false;
                    for (wchar_t c : s) {
                        if (c >= 0x4E00 && c <= 0x9FFF) { hasCJK = true; break; }
                    }
                    int strId = (block - 1) * 16 + i;
                    if (hasCJK) {
                        wprintf(L"[%s][%3d] %ls\n", label, strId, s.c_str());
                    }
                }
                p += len * 2;
            } else {
                if (len > 0) p += len * 2;
            }
        }
    }
}

int wmain(int argc, wchar_t** argv) {
    const wchar_t* path = (argc >= 2) ? argv[1] : L"C:\\Windows\\System32\\zh-CN\\taskmgr.exe.mui";

    // MUI 需要 LOAD_LIBRARY_AS_DATAFILE 才能当资源容器加载
    HMODULE mod = LoadLibraryExW(path, nullptr,
                                 LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!mod) {
        wprintf(L"LoadLibraryEx failed: %lu\n", GetLastError());
        return 1;
    }
    wprintf(L"=== %ls ===\n\n", path);
    DumpStringTable(mod, L"STR");
    FreeLibrary(mod);
    return 0;
}
