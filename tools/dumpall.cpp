// dumpall.cpp - 提取 PE 内所有 UTF-16LE 字符串，直接写 UTF-8 文件
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

static void WriteUtf8(FILE* f, const std::wstring& s) {
    if (s.empty()) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return;
    std::string u(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &u[0], n, nullptr, nullptr);
    fwrite(u.data(), 1, u.size(), f);
}

static bool IsPrintableW(wchar_t c) {
    if (c >= 0x20 && c < 0x7F) return true;
    if (c >= 0x4E00 && c <= 0x9FFF) return true;   // CJK
    if (c >= 0x3000 && c <= 0x303F) return true;   // CJK punct
    if (c >= 0xFF00 && c <= 0xFFEF) return true;   // fullwidth
    if (c == 0x00B7 || c == 0x2026 || c == 0x2018 || c == 0x2019 ||
        c == 0x201C || c == 0x201D || c == 0x2013 || c == 0x2014) return true;
    return false;
}

int wmain(int argc, wchar_t** argv) {
    const wchar_t* in = (argc >= 2) ? argv[1] : L"C:\\Windows\\System32\\zh-CN\\taskmgr.exe.mui";
    const wchar_t* out = (argc >= 3) ? argv[2] : L"dump.txt";

    HANDLE h = CreateFileW(in, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 1;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::vector<BYTE> buf((size_t)sz.QuadPart);
    DWORD rd = 0;
    ReadFile(h, buf.data(), (DWORD)buf.size(), &rd, nullptr);
    CloseHandle(h);

    FILE* f = _wfopen(out, L"wb");
    if (!f) return 1;
    // 写 BOM
    fwrite("\xEF\xBB\xBF", 1, 3, f);

    const wchar_t* w = reinterpret_cast<const wchar_t*>(buf.data());
    size_t n = buf.size() / 2;
    std::wstring cur;
    for (size_t i = 0; i < n; ++i) {
        if (w[i] != 0 && IsPrintableW(w[i])) {
            cur.push_back(w[i]);
        } else {
            if (cur.size() >= 2) {
                WriteUtf8(f, cur);
                fputc('\n', f);
            }
            cur.clear();
        }
    }
    if (cur.size() >= 2) { WriteUtf8(f, cur); fputc('\n', f); }
    fclose(f);
    return 0;
}
