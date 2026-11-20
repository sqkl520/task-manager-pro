// uistr.cpp - 从 taskmgr.exe 提取可读字符串（ASCII + UTF-16LE），聚焦 UI 文案
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>
#include <fcntl.h>
#include <io.h>

static bool IsGoodAscii(const char* s, int len) {
    if (len < 3) return false;
    int printable = 0;
    for (int i = 0; i < len; ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x20 && c < 0x7F) ++printable;
    }
    return printable >= len - 1;
}

static bool HasCJK(const wchar_t* s, int len) {
    for (int i = 0; i < len; ++i) {
        if (s[i] >= 0x4E00 && s[i] <= 0x9FFF) return true;
    }
    return false;
}

static bool IsGoodWide(const wchar_t* s, int len) {
    if (len < 2) return false;
    int good = 0;
    int cjk = 0;
    for (int i = 0; i < len; ++i) {
        wchar_t c = s[i];
        bool isCjk = (c >= 0x4E00 && c <= 0x9FFF);
        bool isPunct = (c >= 0x3001 && c <= 0x303F) || (c >= 0xFF01 && c <= 0xFF5E) ||
                       c == 0x00B7 || c == 0x2026;
        bool isAscii = (c >= 0x20 && c < 0x7F);
        if (isCjk) { ++good; ++cjk; }
        else if (isPunct || isAscii) { ++good; }
        else return false;   // 任何控制字符/其他范围 → 直接否决
    }
    // 必须几乎全部合法，且 CJK 占比要够（排除偶然含一两个中日韩码位的二进制垃圾）
    return good >= len && cjk * 100 >= len * 50;
}

int wmain(int argc, wchar_t** argv) {
    // 输出统一转成 UTF-8，避免控制台代码页（936）破坏中文
    _setmode(_fileno(stdout), _O_U8TEXT);

    const wchar_t* path = (argc >= 2) ? argv[1] : L"C:\\Windows\\System32\\Taskmgr.exe";

    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        wprintf(L"open failed %lu\n", GetLastError());
        return 1;
    }
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    std::vector<BYTE> buf(static_cast<size_t>(sz.QuadPart));
    DWORD read = 0;
    ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr);
    CloseHandle(f);
    wprintf(L"file %ls  %zu bytes\n\n", path, buf.size());

    // ---- UTF-16LE 字符串 ----
    // 严格模式：只接受「前后都是 NUL 边界」的完整 UTF-16 字符串，
    // 且长度 >= 2 个 CJK 字符、全部字符合法。滑动窗口方式误命中率太高。
    wprintf(L"===== WIDE STRINGS WITH CJK =====\n");
    {
        std::vector<wchar_t> w(reinterpret_cast<const wchar_t*>(buf.data()),
                               reinterpret_cast<const wchar_t*>(buf.data()) + buf.size() / 2);
        for (size_t k = 0; k < w.size(); ) {
            // 找一个起点：前一个 wchar 必须是 NUL 或这是文件开头
            if (k > 0 && w[k - 1] != 0) { ++k; continue; }
            size_t e = k;
            while (e < w.size() && w[e] != 0) ++e;
            size_t len = e - k;
            if (len >= 2 && len < 400 && IsGoodWide(w.data() + k, static_cast<int>(len))) {
                std::wstring s(w.data() + k, len);
                wprintf(L"%ls\n", s.c_str());
            }
            k = e + 1;
        }
    }

    wprintf(L"\n===== ASCII STRINGS (UI-ish) =====\n");
    size_t i = 0;
    while (i < buf.size()) {
        const char* p = reinterpret_cast<const char*>(buf.data() + i);
        int maxLen = static_cast<int>(buf.size() - i);
        int len = 0;
        while (len < maxLen && p[len] != 0) ++len;
        if (len >= 4 && len < 200 && IsGoodAscii(p, len)) {
            std::string s(p, len);
            // 只保留看起来像 UI 的
            if (s.find("Tm") != std::string::npos || s.find("tm") != std::string::npos ||
                s.find("Proc") != std::string::npos || s.find("cpu") != std::string::npos ||
                s.find("CPU") != std::string::npos || s.find("Mem") != std::string::npos ||
                s.find("Disk") != std::string::npos || s.find("Net") != std::string::npos ||
                s.find("GPU") != std::string::npos || s.find("Status") != std::string::npos ||
                s.find("Percent") != std::string::npos) {
                printf("%s\n", s.c_str());
            }
            i += static_cast<size_t>(len + 1);
        } else {
            ++i;
        }
    }
    return 0;
}
