// pages_data_users_services.cpp - 用户页 + 服务页 + 应用历史页数据源
#include "pages_data.h"

#include <wtsapi32.h>
#include <winsvc.h>
#include <vector>
#include <algorithm>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "advapi32.lib")

namespace tmp {

// ============ 用户页 ============

namespace {

std::wstring WtsStateText(int state) {
    switch (state) {
    case WTSActive:           return L"活动";
    case WTSConnected:        return L"已连接";
    case WTSConnectQuery:     return L"连接查询";
    case WTSShadow:           return L"影子";
    case WTSDisconnected:     return L"已断开";
    case WTSIdle:             return L"空闲";
    case WTSListen:           return L"侦听";
    case WTSReset:            return L"重置";
    case WTSDown:             return L"已关闭";
    case WTSInit:             return L"初始化";
    default:                  return L"";
    }
}

std::wstring QuerySessionStr(DWORD sessionId, WTS_INFO_CLASS cls) {
    LPWSTR p = nullptr;
    DWORD cb = 0;
    std::wstring result;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId,
                                    cls, &p, &cb) && p) {
        result = p;
        WTSFreeMemory(p);
    }
    return result;
}

} // namespace

bool UserSource::Enumerate(std::vector<UserSession>& out) {
    out.clear();

    PWTS_SESSION_INFOW pSessions = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pSessions, &count)) {
        return false;
    }

    for (DWORD i = 0; i < count; ++i) {
        const WTS_SESSION_INFOW& s = pSessions[i];

        // 过滤掉 RDP 侦听器占位会话（sessionId >= 65536）
        if (s.SessionId >= 65536) continue;

        UserSession us;
        us.sessionId = s.SessionId;
        us.stateRaw = s.State;
        us.stateText = WtsStateText(static_cast<int>(s.State));
        us.winStation = s.pWinStationName ? s.pWinStationName : L"";
        us.userName = QuerySessionStr(s.SessionId, WTSUserName);
        us.domain = QuerySessionStr(s.SessionId, WTSDomainName);
        us.clientName = QuerySessionStr(s.SessionId, WTSClientName);

        out.push_back(std::move(us));
    }
    WTSFreeMemory(pSessions);

    std::sort(out.begin(), out.end(),
              [](const UserSession& a, const UserSession& b) {
                  return a.sessionId < b.sessionId;
              });
    return true;
}

// ============ 服务页 ============

namespace {

std::wstring ServiceStateText(DWORD state) {
    switch (state) {
    case SERVICE_RUNNING:          return L"正在运行";
    case SERVICE_STOPPED:          return L"已停止";
    case SERVICE_START_PENDING:    return L"正在启动";
    case SERVICE_STOP_PENDING:     return L"正在停止";
    case SERVICE_PAUSED:           return L"已暂停";
    case SERVICE_PAUSE_PENDING:    return L"正在暂停";
    case SERVICE_CONTINUE_PENDING: return L"正在继续";
    default:                       return L"";
    }
}

} // namespace

bool ServiceSource::Enumerate(std::vector<ServiceItem>& out) {
    out.clear();

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return false;

    // 第一次调用只为拿所需缓冲区大小（实测 306 个服务）
    DWORD bytesNeeded = 0, count = 0, resume = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
                          SERVICE_STATE_ALL, nullptr, 0,
                          &bytesNeeded, &count, &resume, nullptr);
    if (bytesNeeded == 0) {
        CloseServiceHandle(scm);
        return false;
    }

    std::vector<BYTE> buf(bytesNeeded + 8192);
    resume = 0;
    if (!EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
                               SERVICE_STATE_ALL, buf.data(),
                               static_cast<DWORD>(buf.size()),
                               &bytesNeeded, &count, &resume, nullptr)) {
        CloseServiceHandle(scm);
        return false;
    }

    auto* arr = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buf.data());
    for (DWORD i = 0; i < count; ++i) {
        ServiceItem it;
        it.name = arr[i].lpServiceName ? arr[i].lpServiceName : L"";
        it.displayName = arr[i].lpDisplayName ? arr[i].lpDisplayName : L"";
        it.pid = arr[i].ServiceStatusProcess.dwProcessId;
        it.stateRaw = arr[i].ServiceStatusProcess.dwCurrentState;
        it.stateText = ServiceStateText(it.stateRaw);
        it.canStop = (it.stateRaw == SERVICE_RUNNING);
        it.canStart = (it.stateRaw == SERVICE_STOPPED);
        out.push_back(std::move(it));
    }

    CloseServiceHandle(scm);

    std::sort(out.begin(), out.end(),
              [](const ServiceItem& a, const ServiceItem& b) {
                  return _wcsicmp(a.displayName.c_str(), b.displayName.c_str()) < 0;
              });
    return true;
}

bool ServiceSource::EnrichOne(ServiceItem& item) {
    if (!item.description.empty() && !item.group.empty()) return true;

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return false;

    SC_HANDLE h = OpenServiceW(scm, item.name.c_str(),
                               SERVICE_QUERY_CONFIG);
    if (h) {
        // 描述
        if (item.description.empty()) {
            DWORD need = 0;
            QueryServiceConfig2W(h, SERVICE_CONFIG_DESCRIPTION, nullptr, 0, &need);
            if (need > 0) {
                std::vector<BYTE> db(need);
                if (QueryServiceConfig2W(h, SERVICE_CONFIG_DESCRIPTION,
                                         db.data(), need, &need)) {
                    auto* d = reinterpret_cast<SERVICE_DESCRIPTIONW*>(db.data());
                    if (d->lpDescription) item.description = d->lpDescription;
                }
            }
        }
        // 组
        if (item.group.empty()) {
            DWORD need2 = 0;
            QueryServiceConfigW(h, nullptr, 0, &need2);
            if (need2 > 0) {
                std::vector<BYTE> cb(need2);
                if (QueryServiceConfigW(h, reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(cb.data()), need2, &need2)) {
                    auto* c = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(cb.data());
                    if (c->lpLoadOrderGroup) item.group = c->lpLoadOrderGroup;
                }
            }
        }
        CloseServiceHandle(h);
    }
    CloseServiceHandle(scm);
    return true;
}

bool ServiceSource::Start(const std::wstring& name) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE h = OpenServiceW(scm, name.c_str(), SERVICE_START | SERVICE_QUERY_STATUS);
    bool ok = false;
    if (h) {
        ok = StartServiceW(h, 0, nullptr) != FALSE;
        CloseServiceHandle(h);
    }
    CloseServiceHandle(scm);
    return ok;
}

bool ServiceSource::Stop(const std::wstring& name) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE h = OpenServiceW(scm, name.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS);
    bool ok = false;
    if (h) {
        SERVICE_STATUS st{};
        ok = ControlService(h, SERVICE_CONTROL_STOP, &st) != FALSE;
        CloseServiceHandle(h);
    }
    CloseServiceHandle(scm);
    return ok;
}

bool ServiceSource::Restart(const std::wstring& name) {
    if (!Stop(name)) return false;
    // 等状态变为 STOPPED，最多 5 秒
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    for (int i = 0; i < 50; ++i) {
        SC_HANDLE h = OpenServiceW(scm, name.c_str(), SERVICE_QUERY_STATUS);
        if (!h) break;
        SERVICE_STATUS_PROCESS stp{};
        DWORD need = 0;
        BOOL got = QueryServiceStatusEx(h, SC_STATUS_PROCESS_INFO,
                                        reinterpret_cast<LPBYTE>(&stp), sizeof(stp), &need);
        CloseServiceHandle(h);
        if (got && stp.dwCurrentState == SERVICE_STOPPED) break;
        Sleep(100);
    }
    CloseServiceHandle(scm);
    return Start(name);
}

// ============ 应用历史记录页 ============

bool AppHistorySource::Enumerate(std::vector<AppHistoryItem>& out) {
    // SRUM（System Resource Usage Monitor）的数据只能通过 srumapi.dll 拿，
    // 但：
    //   1. srumapi.dll 没有任何公开导出签名（Windows SDK 里搜不到 SruQueryStats）
    //   2. 实测直接调用 SruQueryStatsEx 会 segfault
    //   3. 需要 SYSTEM 权限（不是管理员）
    //   4. root\cimv2 里没有任何 SRUM 相关的 WMI 类（实测零结果）
    //   5. SRUDB.dat 是 ESE 新格式，需要 esent.dll 且被 DPS 服务独占
    //
    // 因此按规格建议：显示"无可用数据"，这正是原版在 SRUM 不可用时的行为
    // （MUI 里就有 "无可用数据" 这条字符串）。
    out.clear();
    return false;
}

std::wstring AppHistorySource::StatusText() const {
    return L"无可用数据";
}

} // namespace tmp
