// pages_data_startup.cpp - 启动页数据源
#include "pages_data.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <vector>
#include <algorithm>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "version.lib")

namespace tmp {

namespace {

// 从命令行里解析出 exe 路径（处理引号与环境变量）
std::wstring ExtractExePath(const std::wstring& cmd) {
    if (cmd.empty()) return L"";

    std::wstring expanded;
    wchar_t buf[2048];
    DWORD n = ExpandEnvironmentStringsW(cmd.c_str(), buf, 2048);
    if (n > 0 && n < 2048) expanded = buf;
    else expanded = cmd;

    size_t start = expanded.find_first_not_of(L" \t");
    if (start == std::wstring::npos) return L"";
    if (expanded[start] == L'"') {
        size_t end = expanded.find(L'"', start + 1);
        if (end != std::wstring::npos)
            return expanded.substr(start + 1, end - start - 1);
    }
    // 无引号：取到第一个空格
    size_t end = expanded.find(L' ', start);
    if (end == std::wstring::npos) return expanded.substr(start);
    return expanded.substr(start, end - start);
}

} // namespace

void StartupSource::ReadRunKey(HKEY root, const wchar_t* subKey, const wchar_t* label,
                                std::vector<StartupItem>& out) {
    HKEY k;
    if (RegOpenKeyExW(root, subKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return;

    for (DWORD i = 0; ; ++i) {
        wchar_t name[512] = {};
        DWORD nameLen = 512;
        DWORD type = 0;
        BYTE data[4096] = {};
        DWORD dataLen = sizeof(data);

        LONG r = RegEnumValueW(k, i, name, &nameLen, nullptr, &type, data, &dataLen);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) continue;
        if (type != REG_SZ && type != REG_EXPAND_SZ) continue;

        StartupItem item;
        item.name = name;
        item.command = reinterpret_cast<const wchar_t*>(data);
        item.exePath = ExtractExePath(item.command);
        item.fromRegistry = true;
        item.sourceKey = label;
        out.push_back(std::move(item));
    }
    RegCloseKey(k);
}

void StartupSource::ReadStartupFolder(const wchar_t* folderPath, const wchar_t* label,
                                       std::vector<StartupItem>& out) {
    if (!folderPath || !*folderPath) return;

    std::wstring pattern = std::wstring(folderPath) + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (_wcsicmp(fd.cFileName, L"desktop.ini") == 0) continue;

        StartupItem item;
        item.name = fd.cFileName;
        // 去掉扩展名作为显示名
        size_t dot = item.name.find_last_of(L'.');
        if (dot != std::wstring::npos) item.name = item.name.substr(0, dot);
        item.command = std::wstring(folderPath) + L"\\" + fd.cFileName;
        item.fromRegistry = false;
        item.sourceKey = label;
        out.push_back(std::move(item));
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

void StartupSource::ApplyApprovedState(std::vector<StartupItem>& out) {
    // StartupApproved 的值格式：12 字节 REG_BINARY
    //   [0..3] 状态 DWORD：2=启用, 3=禁用
    //   [4..11] 禁用时间 FILETIME
    struct ApprovedMap {
        const wchar_t* subKey;
        const wchar_t* label;
    };
    const ApprovedMap maps[] = {
        { L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run",           L"HKCU\\Run" },
        { L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run32",         L"HKCU\\Run32" },
        { L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder", L"启动文件夹" },
    };

    for (const auto& m : maps) {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, m.subKey, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;

        for (DWORD i = 0; ; ++i) {
            wchar_t name[512] = {};
            DWORD nameLen = 512;
            DWORD type = 0;
            BYTE data[64] = {};
            DWORD dataLen = sizeof(data);

            LONG r = RegEnumValueW(k, i, name, &nameLen, nullptr, &type, data, &dataLen);
            if (r == ERROR_NO_MORE_ITEMS) break;
            if (r != ERROR_SUCCESS) continue;
            if (type != REG_BINARY || dataLen < 4) continue;

            DWORD state = *reinterpret_cast<DWORD*>(data);
            // 值是 12 字节，[0..3] 就是状态
            if (state != 2 && state != 3 && state != 1 && state != 0) continue;

            for (auto& item : out) {
                // 键名与启动项名称必须完全一致
                if (_wcsicmp(item.name.c_str(), name) != 0) continue;
                // 启动文件夹的项要同时匹配来源
                if (!item.fromRegistry && item.sourceKey != L"启动文件夹") continue;

                item.enabled = (state != 3);
                if (!item.enabled && dataLen >= 12) {
                    item.disabledTime.dwLowDateTime = *reinterpret_cast<DWORD*>(data + 4);
                    item.disabledTime.dwHighDateTime = *reinterpret_cast<DWORD*>(data + 8);
                }
                break;
            }
        }
        RegCloseKey(k);
    }
}

void StartupSource::ResolvePublisher(StartupItem& item) {
    if (item.exePath.empty()) return;

    DWORD dummy = 0;
    DWORD sz = GetFileVersionInfoSizeW(item.exePath.c_str(), &dummy);
    if (sz == 0) return;

    std::vector<BYTE> buf(sz);
    if (!GetFileVersionInfoW(item.exePath.c_str(), 0, sz, buf.data())) return;

    struct LANGANDCODEPAGE { WORD language; WORD codePage; } *trans = nullptr;
    UINT cbTrans = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<LPVOID*>(&trans), &cbTrans) ||
        cbTrans < sizeof(LANGANDCODEPAGE)) {
        return;
    }

    wchar_t query[256];
    swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\CompanyName",
               trans[0].language, trans[0].codePage);

    wchar_t* company = nullptr;
    UINT cbCompany = 0;
    if (VerQueryValueW(buf.data(), query,
                       reinterpret_cast<LPVOID*>(&company), &cbCompany) && cbCompany > 0) {
        item.publisher = company;
    }
}

bool StartupSource::Enumerate(std::vector<StartupItem>& out) {
    out.clear();

    // 6 个注册表 Run 键
    ReadRunKey(HKEY_CURRENT_USER,  L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
               L"HKCU\\Run", out);
    ReadRunKey(HKEY_CURRENT_USER,  L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
               L"HKCU\\RunOnce", out);
    ReadRunKey(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
               L"HKLM\\Run", out);
    ReadRunKey(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
               L"HKLM\\RunOnce", out);
    ReadRunKey(HKEY_LOCAL_MACHINE,
               L"Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Run",
               L"HKLM\\Run32", out);
    ReadRunKey(HKEY_LOCAL_MACHINE,
               L"Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
               L"HKLM\\Run32Once", out);

    // 两个启动文件夹
    wchar_t userStartup[MAX_PATH] = {};
    wchar_t commonStartup[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, CSIDL_STARTUP, nullptr, 0, userStartup) == S_OK) {
        ReadStartupFolder(userStartup, L"启动文件夹", out);
    }
    if (SHGetFolderPathW(nullptr, CSIDL_COMMON_STARTUP, nullptr, 0, commonStartup) == S_OK) {
        ReadStartupFolder(commonStartup, L"公共启动文件夹", out);
    }

    // 应用启用/禁用状态
    ApplyApprovedState(out);

    // 解析发布者（只对前 40 项做，其余留给后台渐进填充）
    int n = 0;
    for (auto& item : out) {
        if (n++ >= 40) break;
        ResolvePublisher(item);
    }

    std::sort(out.begin(), out.end(),
              [](const StartupItem& a, const StartupItem& b) { return a.name < b.name; });
    return true;
}

bool StartupSource::SetEnabled(const StartupItem& item, bool enabled) {
    // 决定写到哪个 StartupApproved 子键
    const wchar_t* subKey = nullptr;
    if (!item.fromRegistry) {
        subKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder";
    } else if (item.sourceKey.find(L"Run32") != std::wstring::npos) {
        subKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run32";
    } else if (item.sourceKey.find(L"HKLM") != std::wstring::npos) {
        // HKLM 项的状态也在 HKCU 的 StartupApproved 下（每用户视图）
        subKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
    } else {
        subKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
    }

    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    BYTE data[12] = {};
    DWORD state = enabled ? 2 : 3;
    *reinterpret_cast<DWORD*>(data) = state;

    if (!enabled) {
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        *reinterpret_cast<DWORD*>(data + 4) = now.dwLowDateTime;
        *reinterpret_cast<DWORD*>(data + 8) = now.dwHighDateTime;
    }

    LONG r = RegSetValueExW(k, item.name.c_str(), 0, REG_BINARY, data, 12);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

} // namespace tmp
