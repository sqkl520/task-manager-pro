// pagetest.cpp - 验证四个次要页面的数据源与成本
// 1) 启动页   : 注册表 Run/RunOnce + 启动文件夹 + StartupApproved 状态
// 2) 用户页   : WTSEnumerateSessionsW + 按 session 聚合
// 3) 服务页   : EnumServicesStatusExW + 描述 + 组
// 4) 应用历史 : SRUM（srumapi.dll 能否直接用 / WMI 是否可用）
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <wtsapi32.h>
#include <shlobj.h>
#include <winsvc.h>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <vector>
#include <chrono>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "advapi32.lib")

static double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// ---------------- 启动页 ----------------
static void TestStartup() {
    wprintf(L"\n========== 启动页数据源 ==========\n");
    double t0 = NowMs();

    struct Item { std::wstring name, cmd, source; bool enabled; };
    std::vector<Item> items;

    struct KeyDef { HKEY root; const wchar_t* path; const wchar_t* src; const wchar_t* approvedSub; };
    KeyDef keys[] = {
        { HKEY_CURRENT_USER,  L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
          L"HKCU\\Run",        L"Run" },
        { HKEY_CURRENT_USER,  L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
          L"HKCU\\RunOnce",    L"Run32" },
        { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
          L"HKLM\\Run",        L"Run" },
        { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
          L"HKLM\\RunOnce",    L"Run32" },
        { HKEY_LOCAL_MACHINE, L"Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Run",
          L"HKLM\\Run(WOW)",   L"Run32" },
    };

    for (auto& k : keys) {
        HKEY hk = nullptr;
        if (RegOpenKeyExW(k.root, k.path, 0, KEY_READ, &hk) != ERROR_SUCCESS) continue;
        // 该键对应的 StartupApproved 子键
        HKEY hAppr = nullptr;
        std::wstring apprPath =
            std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\")
            + k.approvedSub;
        RegOpenKeyExW(k.root, apprPath.c_str(), 0, KEY_READ, &hAppr);

        for (DWORD i = 0; ; ++i) {
            wchar_t name[512]; DWORD nameLen = 512;
            BYTE data[4096]; DWORD dataLen = sizeof(data); DWORD type = 0;
            LONG r = RegEnumValueW(hk, i, name, &nameLen, nullptr, &type, data, &dataLen);
            if (r != ERROR_SUCCESS) break;
            if (type != REG_SZ && type != REG_EXPAND_SZ) continue;

            Item it;
            it.name = name;
            it.cmd = reinterpret_cast<wchar_t*>(data);
            it.source = k.src;
            it.enabled = true;

            // 查启用状态
            if (hAppr) {
                BYTE st[64]; DWORD stLen = sizeof(st); DWORD stType = 0;
                if (RegQueryValueExW(hAppr, name, nullptr, &stType, st, &stLen) == ERROR_SUCCESS
                    && stLen >= 4) {
                    DWORD v = *reinterpret_cast<DWORD*>(st);
                    // 实测：2 = 启用，3 = 禁用，1/0 = 其他（视为启用）
                    it.enabled = (v == 2 || v == 0 || v == 1);
                }
            }
            items.push_back(it);
        }
        if (hAppr) RegCloseKey(hAppr);
        RegCloseKey(hk);
    }

    wprintf(L"注册表启动项: %zu 项\n", items.size());
    for (auto& it : items) {
        wprintf(L"  [%ls] %-40ls  %-8ls  %ls\n",
                it.enabled ? L"启用" : L"禁用",
                it.name.c_str(), it.source.c_str(),
                it.cmd.substr(0, 70).c_str());
    }

    double elapsed = NowMs() - t0;
    wprintf(L"耗时: %.2f ms\n", elapsed);

    // 启动文件夹
    wchar_t startup[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_STARTUP, nullptr, 0, startup) == S_OK) {
        wprintf(L"用户启动文件夹: %ls\n", startup);
    }
    if (SHGetFolderPathW(nullptr, CSIDL_COMMON_STARTUP, nullptr, 0, startup) == S_OK) {
        wprintf(L"公共启动文件夹: %ls\n", startup);
    }
}

// ---------------- 用户页 ----------------
static void TestUsers() {
    wprintf(L"\n========== 用户页数据源 ==========\n");
    double t0 = NowMs();

    PWTS_SESSION_INFOW pSessions = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pSessions, &count)) {
        wprintf(L"WTSEnumerateSessionsW 失败: %lu\n", GetLastError());
        return;
    }
    wprintf(L"WTSEnumerateSessionsW: %lu 个会话\n", count);

    for (DWORD i = 0; i < count; ++i) {
        auto& s = pSessions[i];
        LPWSTR user = nullptr, domain = nullptr, clientName = nullptr;
        DWORD cb = 0;
        WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, s.SessionId,
                                    WTSUserName, &user, &cb);
        WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, s.SessionId,
                                    WTSDomainName, &domain, &cb);
        WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, s.SessionId,
                                    WTSClientName, &clientName, &cb);

        // 会话 ID 与状态
        const wchar_t* stateStr = L"?";
        switch (s.State) {
        case WTSActive:       stateStr = L"活动"; break;
        case WTSConnected:    stateStr = L"已连接"; break;
        case WTSConnectQuery: stateStr = L"连接查询"; break;
        case WTSShadow:       stateStr = L"影子"; break;
        case WTSDisconnected: stateStr = L"已断开"; break;
        case WTSIdle:         stateStr = L"空闲"; break;
        case WTSListen:       stateStr = L"侦听"; break;
        case WTSReset:        stateStr = L"重置"; break;
        case WTSDown:         stateStr = L"已关闭"; break;
        case WTSInit:         stateStr = L"初始化"; break;
        }

        wprintf(L"  sessionId=%-3lu state=%-8ls user=%ls\\%ls client=%ls winsta=%ls\n",
                s.SessionId, stateStr,
                domain ? domain : L"", user ? user : L"",
                clientName ? clientName : L"", s.pWinStationName ? s.pWinStationName : L"");

        if (user) WTSFreeMemory(user);
        if (domain) WTSFreeMemory(domain);
        if (clientName) WTSFreeMemory(clientName);
    }
    WTSFreeMemory(pSessions);

    wprintf(L"耗时: %.2f ms\n", NowMs() - t0);
}

// ---------------- 服务页 ----------------
static void TestServices() {
    wprintf(L"\n========== 服务页数据源 ==========\n");
    double t0 = NowMs();

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) { wprintf(L"OpenSCManager 失败: %lu\n", GetLastError()); return; }

    DWORD bytesNeeded = 0, count = 0, resume = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
                          SERVICE_STATE_ALL, nullptr, 0, &bytesNeeded,
                          &count, &resume, nullptr);
    DWORD err = GetLastError();
    if (err != ERROR_MORE_DATA && bytesNeeded == 0) {
        wprintf(L"EnumServicesStatusEx 探测失败: %lu\n", err);
        CloseServiceHandle(scm);
        return;
    }

    std::vector<BYTE> buf(bytesNeeded + 8192);
    resume = 0;
    if (!EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
                               SERVICE_STATE_ALL, buf.data(),
                               static_cast<DWORD>(buf.size()), &bytesNeeded,
                               &count, &resume, nullptr)) {
        wprintf(L"EnumServicesStatusEx 失败: %lu\n", GetLastError());
        CloseServiceHandle(scm);
        return;
    }

    wprintf(L"服务总数: %lu\n", count);

    auto* svc = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buf.data());
    int shown = 0;
    for (DWORD i = 0; i < count && shown < 12; ++i) {
        auto& s = svc[i];
        const wchar_t* stStr = L"?";
        DWORD st = s.ServiceStatusProcess.dwCurrentState;
        if (st == SERVICE_RUNNING)          stStr = L"正在运行";
        else if (st == SERVICE_STOPPED)     stStr = L"已停止";
        else if (st == SERVICE_START_PENDING) stStr = L"正在启动";
        else if (st == SERVICE_STOP_PENDING)  stStr = L"正在停止";
        else if (st == SERVICE_PAUSED)      stStr = L"已暂停";

        // 拿描述与组
        std::wstring desc, group;
        SC_HANDLE h = OpenServiceW(scm, s.lpServiceName, SERVICE_QUERY_CONFIG);
        if (h) {
            DWORD need = 0;
            QueryServiceConfig2W(h, SERVICE_CONFIG_DESCRIPTION, nullptr, 0, &need);
            if (need) {
                std::vector<BYTE> db(need);
                auto* d = reinterpret_cast<SERVICE_DESCRIPTIONW*>(db.data());
                if (QueryServiceConfig2W(h, SERVICE_CONFIG_DESCRIPTION, db.data(), need, &need)
                    && d->lpDescription) {
                    desc = d->lpDescription;
                }
            }
            DWORD need2 = 0;
            QueryServiceConfigW(h, nullptr, 0, &need2);
            if (need2) {
                std::vector<BYTE> cb(need2);
                auto* c = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(cb.data());
                if (QueryServiceConfigW(h, c, need2, &need2) && c->lpLoadOrderGroup) {
                    group = c->lpLoadOrderGroup;
                }
            }
            CloseServiceHandle(h);
        }

        wprintf(L"  %-38ls pid=%-6lu %-8ls 组=%-20ls %ls\n",
                s.lpServiceName, s.ServiceStatusProcess.dwProcessId, stStr,
                group.empty() ? L"-" : group.c_str(),
                desc.substr(0, 50).c_str());
        ++shown;
    }

    CloseServiceHandle(scm);
    wprintf(L"耗时: %.2f ms（含前 %d 个服务的描述与组查询）\n", NowMs() - t0, shown);
}

// ---------------- 应用历史记录 ----------------
static void TestAppHistory() {
    wprintf(L"\n========== 应用历史记录数据源 (SRUM) ==========\n");

    double t0 = NowMs();
    HMODULE srum = LoadLibraryW(L"srumapi.dll");
    wprintf(L"srumapi.dll 加载: %ls\n", srum ? L"成功" : L"失败");
    if (srum) {
        const char* names[] = {
            "SruCreate", "SruOpenTable", "SruClose",
            "SruRetrieveRecords", "SruQueryRecordTable", "SruDeleteRecords"
        };
        for (auto* n : names) {
            FARPROC p = GetProcAddress(srum, n);
            wprintf(L"  %-22hs -> %ls\n", n, p ? L"有导出" : L"无导出");
        }
        FreeLibrary(srum);
    }
    wprintf(L"（LoadLibrary + GetProcAddress 全部检查耗时: %.2f ms）\n", NowMs() - t0);

    // SRUDB.dat 是否可读
    HANDLE f = CreateFileW(L"C:\\Windows\\System32\\sru\\SRUDB.dat",
                           GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz{};
        GetFileSizeEx(f, &sz);
        wprintf(L"SRUDB.dat 直接打开: 成功，%.1f MB\n", sz.QuadPart / 1048576.0);
        // 读前 4KB 看是不是 ESE/JET Blue 格式
        BYTE hdr[4096]{};
        DWORD got = 0;
        ReadFile(f, hdr, sizeof(hdr), &got, nullptr);
        wprintf(L"  前 4 字节: %02X %02X %02X %02X  (ESE 签名应为 EF CD AB 89)\n",
                hdr[0], hdr[1], hdr[2], hdr[3]);
        CloseHandle(f);
    } else {
        wprintf(L"SRUDB.dat 直接打开: 失败 (错误 %lu) —— 需 SYSTEM 权限或 ESE 独占\n",
                GetLastError());
    }
}

int wmain() {
    _setmode(_fileno(stdout), _O_U8TEXT);
    wprintf(L"=== 任务管理器次要页面 · 数据源本机验证 ===\n");
    wprintf(L"进程是否提权: %ls\n", IsUserAnAdmin() ? L"是(管理员)" : L"否");

    TestStartup();
    TestUsers();
    TestServices();
    TestAppHistory();
    return 0;
}
