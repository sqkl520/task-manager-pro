// muidlg.cpp - 提取 taskmgr.exe.mui 的 DIALOG / MENU 资源
// 目的：判定 Win10 任务管理器顶层结构是「左侧导航栏」还是「顶部横向标签」。
#include <windows.h>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <string>

static std::wstring ResNameToStr(LPCWSTR name) {
    if (reinterpret_cast<uintptr_t>(name) >> 16) return name;
    wchar_t b[24];
    swprintf_s(b, L"#%u", static_cast<unsigned>(reinterpret_cast<uintptr_t>(name)));
    return b;
}

static WORD RdW(const BYTE*& p, const BYTE* end) {
    if (p + 2 > end) { p = end; return 0; }
    WORD v = *reinterpret_cast<const WORD*>(p); p += 2; return v;
}
static DWORD RdD(const BYTE*& p, const BYTE* end) {
    if (p + 4 > end) { p = end; return 0; }
    DWORD v = *reinterpret_cast<const DWORD*>(p); p += 4; return v;
}
static std::wstring RdSz(const BYTE*& p, const BYTE* end) {
    std::wstring s;
    while (p + 2 <= end) {
        wchar_t c = *reinterpret_cast<const wchar_t*>(p);
        p += 2;
        if (c == 0) break;
        s.push_back(c);
    }
    return s;
}

static void ParseDialog(const BYTE* data, DWORD size, int resId) {
    const BYTE* p = data;
    const BYTE* end = data + size;

    DWORD style = RdD(p, end);
    DWORD exStyle = RdD(p, end);
    WORD itemCount = RdW(p, end);
    short x = (short)RdW(p, end), y = (short)RdW(p, end);
    short cx = (short)RdW(p, end), cy = (short)RdW(p, end);
    std::wstring menu = RdSz(p, end);
    std::wstring cls = RdSz(p, end);
    std::wstring title = RdSz(p, end);

    wprintf(L"\n[DIALOG %d] items=%u rect=(%d,%d %dx%d) class='%ls' title='%ls'\n",
            resId, itemCount, x, y, cx, cy, cls.c_str(), title.c_str());

    bool hasTab = false;
    for (WORD i = 0; i < itemCount && p < end; ++i) {
        (void)RdD(p, end);              // helpId
        (void)RdD(p, end);              // style
        (void)RdD(p, end);              // exStyle
        short ix = (short)RdW(p, end);
        short iy = (short)RdW(p, end);
        short icx = (short)RdW(p, end);
        short icy = (short)RdW(p, end);
        DWORD id = RdD(p, end);
        std::wstring c2 = RdSz(p, end);
        std::wstring text = RdSz(p, end);
        WORD extra = RdW(p, end);
        if (extra) p += extra;

        if (c2 == L"SysTabControl32") hasTab = true;
        if (!c2.empty()) {
            wprintf(L"    ctrl id=%5u class='%ls' text='%ls' (%d,%d %dx%d)\n",
                    id, c2.c_str(), text.c_str(), ix, iy, icx, icy);
        }
    }
    wprintf(L"    >>> SysTabControl32: %ls\n", hasTab ? L"YES" : L"no");
}

static void ParseMenu(const BYTE* data, DWORD size, int resId) {
    wprintf(L"\n[MENU %d] %u bytes\n", resId, size);
    const BYTE* p = data;
    const BYTE* end = data + size;
    WORD version = RdW(p, end);
    WORD headerSize = RdW(p, end);
    wprintf(L"    version=%u headerSize=%u\n", version, headerSize);

    int cnt = 0;
    for (const BYTE* q = p; q + 2 <= end && cnt < 120; ) {
        wchar_t c = *reinterpret_cast<const wchar_t*>(q);
        if (c >= 0x20) {
            std::wstring s;
            const BYTE* r = q;
            while (r + 2 <= end) {
                wchar_t ch = *reinterpret_cast<const wchar_t*>(r);
                if (ch == 0) break;
                s.push_back(ch);
                r += 2;
            }
            if (s.size() >= 2 && s.size() < 80) {
                wprintf(L"    '%ls'\n", s.c_str());
                ++cnt;
            }
            q = r + 2;
        } else {
            q += 2;
        }
    }
}

static BOOL WINAPI EnumTypesCb(HMODULE h, LPCWSTR type, LPARAM lp) {
    std::wstring tn = ResNameToStr(type);
    int* pn = reinterpret_cast<int*>(lp);
    *pn = 0;

    struct Local {
        static BOOL WINAPI CountCb(HMODULE, LPCWSTR, LPCWSTR, LPARAM l) {
            ++(*reinterpret_cast<int*>(l));
            return TRUE;
        }
    };
    EnumResourceNamesW(h, type, (ENUMRESNAMEPROCW)Local::CountCb, reinterpret_cast<LPARAM>(pn));
    wprintf(L"  type %-14ls : %d\n", tn.c_str(), *pn);
    return TRUE;
}

static BOOL WINAPI DlgCb(HMODULE h, LPCWSTR type, LPCWSTR name, LPARAM) {
    HRSRC res = FindResourceW(h, name, type);
    if (!res) return TRUE;
    HGLOBAL hg = LoadResource(h, res);
    DWORD sz = SizeofResource(h, res);
    const BYTE* d = static_cast<const BYTE*>(LockResource(hg));
    if (d && sz) ParseDialog(d, sz, static_cast<int>(reinterpret_cast<uintptr_t>(name)));
    return TRUE;
}

static BOOL WINAPI MenuCb(HMODULE h, LPCWSTR type, LPCWSTR name, LPARAM) {
    HRSRC res = FindResourceW(h, name, type);
    if (!res) return TRUE;
    HGLOBAL hg = LoadResource(h, res);
    DWORD sz = SizeofResource(h, res);
    const BYTE* d = static_cast<const BYTE*>(LockResource(hg));
    if (d && sz) ParseMenu(d, sz, static_cast<int>(reinterpret_cast<uintptr_t>(name)));
    return TRUE;
}

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);

    const wchar_t* path = (argc >= 2) ? argv[1]
                                      : L"C:\\Windows\\System32\\zh-CN\\taskmgr.exe.mui";
    HMODULE mod = LoadLibraryExW(path, nullptr,
                                 LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!mod) {
        wprintf(L"LoadLibraryEx failed: %lu\n", GetLastError());
        return 1;
    }
    wprintf(L"=== %ls ===\n", path);

    wprintf(L"\n--- resource type summary ---\n");
    int dummy = 0;
    EnumResourceTypesW(mod, (ENUMRESTYPEPROCW)EnumTypesCb, reinterpret_cast<LPARAM>(&dummy));

    wprintf(L"\n--- DIALOG resources ---\n");
    EnumResourceNamesW(mod, MAKEINTRESOURCEW(5), (ENUMRESNAMEPROCW)DlgCb, 0);

    wprintf(L"\n--- MENU resources ---\n");
    EnumResourceNamesW(mod, MAKEINTRESOURCEW(4), (ENUMRESNAMEPROCW)MenuCb, 0);

    FreeLibrary(mod);
    return 0;
}
