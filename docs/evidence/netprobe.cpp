// netprobe.cpp - 每进程网络用量采集可行性实测
//
// 目的：回答四个问题
//   1. 本机能否启动 ETW 实时会话订阅 Microsoft-Windows-Kernel-Network？(StartTraceW 错误码)
//   2. 管理员权限是否硬要求？
//   3. realtime consumer 每秒吃多少 CPU？
//   4. 事件 payload 里到底有没有 PID / 字节数，数值对不对？
//
// 用法：
//   netprobe.exe                     主探针（当前权限）
//   netprobe.exe --low              用受限令牌（去掉管理员组）重新拉起自己，验证非管理员
//   netprobe.exe --gen <port> <bytes>  子进程：双向收发，用于 PID 归属验证
//
// 编译（独立 console exe，不进主工程）：
//   cl /nologo /utf-8 /std:c++17 /EHsc /O2 /W3 /MD /DUNICODE /D_UNICODE /DNOMINMAX
//      /DWIN32_LEAN_AND_MEAN netprobe.cpp /Fe:netprobe.exe /link ws2_32.lib iphlpapi.lib tdh.lib

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x06010000
#endif

#include <winsock2.h>
#include <ws2tcpip.h>      // 必须先于 iphlpapi.h：它定义 _WS2IPDEF_，否则 EStats 声明不出现
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#include <iphlpapi.h>
#include <tcpmib.h>
#include <tcpestats.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "tdh.lib")
#pragma comment(lib, "advapi32.lib")

// Microsoft-Windows-Kernel-Network
static const GUID kKernelNetworkGuid =
    {0x7dd42a49, 0x5329, 0x4832, {0x8d, 0xfd, 0x43, 0xd9, 0x79, 0x15, 0x3a, 0x88}};

static const wchar_t* kSessionName = L"tmp-netprobe-session";

// ------------------------------------------------------------------ 日志
//
// 同时写 stdout 和文件：--low 模式是 de-elevated 拉起，
// 拿不到父控制台 stdout，只能靠文件。

static FILE* g_log = nullptr;

static void LogOpen(const wchar_t* path) {
    if (path && *path) _wfopen_s(&g_log, path, L"wb");   // 二进制写，避免 ccs 流模式
}

// 宽字符串先转 UTF-8 再交给 fprintf：二进制流里用 %ls 会崩
static const char* U8(const wchar_t* w) {
    static char bufs[8][1024];
    static int slot = 0;
    char* out = bufs[slot++ & 7];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, 1024, nullptr, nullptr);
    if (n <= 0) out[0] = 0;
    return out;
}

static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
}

// ------------------------------------------------------------------ 上下文

struct Agg {
    uint64_t recvBytes = 0;
    uint64_t sendBytes = 0;
    uint32_t recvEvents = 0;
    uint32_t sendEvents = 0;
};

struct ProbeCtx {
    std::atomic<uint64_t> totalEvents{0};
    std::atomic<uint64_t> netEvents{0};
    std::atomic<uint64_t> otherEvents{0};
    std::atomic<uint64_t> malformed{0};

    uint32_t idCount[256] = {};
    uint16_t versionSeen[256] = {};

    std::unordered_map<uint32_t, Agg> perPid;
    CRITICAL_SECTION lock;

    uint32_t dumpsDone = 0;
    uint32_t rawPid[8] = {};
    uint32_t rawSize[8] = {};
    uint16_t rawId[8] = {};
    uint16_t rawVer[8] = {};
    uint8_t  rawBuf[8][40] = {};
    uint32_t rawLen[8] = {};
};

// 事件 10 / 11 的 payload 布局（广泛记载，本探针用实测偏移核对）
//   +0  PID   (ULONG)
//   +4  size  (ULONG)
//   +8  daddr (ULONG)
//   +12 saddr (ULONG)
//   +16 dport (USHORT)  +18 sport (USHORT)
static const ULONG kNetPayloadSize = 20;

static VOID WINAPI OnEvent(PEVENT_RECORD rec) {
    auto* ctx = static_cast<ProbeCtx*>(rec->UserContext);
    if (!ctx) return;
    ctx->totalEvents.fetch_add(1, std::memory_order_relaxed);

    const uint16_t id = rec->EventHeader.EventDescriptor.Id;
    ctx->idCount[id & 0xFF]++;
    ctx->versionSeen[id & 0xFF] = rec->EventHeader.EventDescriptor.Version;

    if (id != 10 && id != 11) {
        ctx->otherEvents.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ctx->netEvents.fetch_add(1, std::memory_order_relaxed);

    if (rec->UserDataLength < kNetPayloadSize || rec->UserData == nullptr) {
        ctx->malformed.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const uint8_t* p = static_cast<const uint8_t*>(rec->UserData);
    uint32_t pid = 0, size = 0;
    memcpy(&pid, p + 0, 4);
    memcpy(&size, p + 4, 4);

    EnterCriticalSection(&ctx->lock);
    Agg& a = ctx->perPid[pid];
    if (id == 10) { a.recvBytes += size; a.recvEvents++; }
    else          { a.sendBytes += size; a.sendEvents++; }

    if (ctx->dumpsDone < 8) {
        uint32_t i = ctx->dumpsDone++;
        ctx->rawPid[i]  = pid;
        ctx->rawSize[i] = size;
        ctx->rawId[i]   = id;
        ctx->rawVer[i]  = rec->EventHeader.EventDescriptor.Version;
        uint32_t n = rec->UserDataLength < 40 ? rec->UserDataLength : 40;
        memcpy(ctx->rawBuf[i], p, n);
        ctx->rawLen[i] = n;
    }
    LeaveCriticalSection(&ctx->lock);
}

// ------------------------------------------------------------------ 辅助

static void PrintHex(const uint8_t* b, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) printf("%02X ", b[i]);
}

static bool IsElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION te{};
    DWORD ret = 0;
    BOOL ok = GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &ret);
    CloseHandle(tok);
    return ok && te.TokenIsElevated != 0;
}

static bool IsMemberOfAdmins() {
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID adminSid = nullptr;
    if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminSid))
        return false;
    BOOL isMember = FALSE;
    CheckTokenMembership(nullptr, adminSid, &isMember);
    FreeSid(adminSid);
    return isMember != 0;
}

// "Performance Log Users" 组：文档说它也能建 trace 会话
static bool IsMemberOfPerfLogUsers() {
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID sid = nullptr;
    if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_LOGGING_USERS, 0, 0, 0, 0, 0, 0, &sid))
        return false;
    BOOL isMember = FALSE;
    CheckTokenMembership(nullptr, sid, &isMember);
    FreeSid(sid);
    return isMember != 0;
}

static uint64_t FileTimeToU64(const FILETIME& ft) {
    return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

static uint64_t ProcCpu100ns() {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    return FileTimeToU64(k) + FileTimeToU64(u);
}

// ------------------------------------------------------------------ 流量发生器
//
// 双向并发收发，避免"发满了等读、读满了等发"的死锁。
// 角色：--gen <port> <bytes> 主动连接并发起；--sink <port> 监听并回声。

struct GenArgs { uint16_t port; uint64_t bytes; };

static int RunGen(GenArgs ga) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { printf("GEN: WSAStartup fail\n"); return 1; }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { printf("GEN: socket fail\n"); return 1; }

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(ga.port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == SOCKET_ERROR) {
        printf("GEN: connect fail err=%d\n", WSAGetLastError());
        return 1;
    }

    std::atomic<uint64_t> sent{0}, recvd{0};

    std::thread rx([&]() {
        std::vector<char> buf(64 * 1024);
        for (;;) {
            int r = recv(s, buf.data(), static_cast<int>(buf.size()), 0);
            if (r <= 0) break;
            recvd.fetch_add(static_cast<uint64_t>(r));
        }
    });

    std::vector<char> tx(64 * 1024, 'x');
    uint64_t total = 0;
    while (total < ga.bytes) {
        uint64_t remain = ga.bytes - total;
        int n = static_cast<int>(remain < tx.size() ? remain : tx.size());
        int w = send(s, tx.data(), n, 0);
        if (w <= 0) break;
        total += static_cast<uint64_t>(w);
        sent.fetch_add(static_cast<uint64_t>(w));
    }
    shutdown(s, SD_SEND);

    for (int i = 0; i < 500 && recvd.load() == 0; ++i) Sleep(10);
    Sleep(300);
    if (rx.joinable()) rx.join();

    printf("GEN: pid=%lu sent=%llu recvd=%llu\n",
           GetCurrentProcessId(),
           (unsigned long long)sent.load(),
           (unsigned long long)recvd.load());
    closesocket(s);
    WSACleanup();
    return 0;
}

// 服务端：监听 → accept → 逐字节回声 → 全部收完后 shutdown
static int RunSink(uint16_t port) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { printf("SINK: WSAStartup fail\n"); return 1; }

    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == SOCKET_ERROR) {
        printf("SINK: bind fail err=%d\n", WSAGetLastError());
        return 1;
    }
    listen(ls, 1);

    SOCKET sv = accept(ls, nullptr, nullptr);
    if (sv == INVALID_SOCKET) { printf("SINK: accept fail err=%d\n", WSAGetLastError()); return 1; }

    std::vector<char> buf(64 * 1024);
    uint64_t total = 0;
    for (;;) {
        int r = recv(sv, buf.data(), static_cast<int>(buf.size()), 0);
        if (r <= 0) break;
        total += static_cast<uint64_t>(r);
        int off = 0;
        while (off < r) {
            int w = send(sv, buf.data() + off, r - off, 0);
            if (w <= 0) break;
            off += w;
        }
    }
    shutdown(sv, SD_SEND);
    printf("SINK: pid=%lu echoed=%llu\n", GetCurrentProcessId(), (unsigned long long)total);
    Sleep(200);
    closesocket(sv);
    closesocket(ls);
    WSACleanup();
    return 0;
}

// ------------------------------------------------------------------ 路径 2：PerTcpConnectionEStats

static void ProbePerConnEStats() {
    Log("\n=== 4. 路径 2: GetPerTcpConnectionEStats ===\n");

    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    listen(ls, 1);
    int alen = sizeof(a);
    getsockname(ls, reinterpret_cast<sockaddr*>(&a), &alen);
    uint16_t port = ntohs(a.sin_port);

    SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in ra{};
    ra.sin_family = AF_INET;
    ra.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ra.sin_port = htons(port);
    connect(c, reinterpret_cast<sockaddr*>(&ra), sizeof(ra));
    SOCKET sv = accept(ls, nullptr, nullptr);

    char tmp[64] = "hello";
    send(c, tmp, 5, 0);
    int r = recv(sv, tmp, sizeof(tmp), 0);
    Log("  回环连接建立 ok (recv=%d)\n", r);

    DWORD sz = 0;
    GetExtendedTcpTable(nullptr, &sz, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    std::vector<uint8_t> tbl(sz);
    DWORD rc = GetExtendedTcpTable(tbl.data(), &sz, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    DWORD nrows = rc == 0 ? reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(tbl.data())->dwNumEntries : 0;
    Log("  GetExtendedTcpTable rc=%lu rows=%lu\n", rc, nrows);

    auto* row = reinterpret_cast<MIB_TCPROW_OWNER_PID*>(
        reinterpret_cast<uint8_t*>(tbl.data()) + sizeof(DWORD));

    MIB_TCPROW found{};
    bool got = false;
    DWORD myPid = GetCurrentProcessId();
    for (DWORD i = 0; i < nrows; ++i) {
        if (row[i].dwOwningPid == myPid && row[i].dwLocalPort == htons(port)) {
            found.dwState      = row[i].dwState;
            found.dwLocalAddr  = row[i].dwLocalAddr;
            found.dwLocalPort  = row[i].dwLocalPort;
            found.dwRemoteAddr = row[i].dwRemoteAddr;
            found.dwRemotePort = row[i].dwRemotePort;
            got = true;
            break;
        }
    }
    Log("  在 TCP 表里找到本进程连接: %s\n", got ? "是" : "否");
    if (got) {
        TCP_ESTATS_DATA_RW_v0 rw{};
        rw.EnableCollection = TRUE;
        ULONG r2 = SetPerTcpConnectionEStats(&found, TcpConnectionEstatsData,
                                             reinterpret_cast<PUCHAR>(&rw), 0,
                                             (ULONG)sizeof(rw), 0);
        Log("  SetPerTcpConnectionEStats(Data, enable) rc=%lu %s\n",
            r2, r2 == 0 ? "OK" : "FAIL");

        // 制造一点流量再读
        for (int i = 0; i < 100; ++i) { send(c, tmp, 5, 0); recv(sv, tmp, sizeof(tmp), 0); }
        Sleep(300);

        TCP_ESTATS_DATA_ROD_v0 rod{};
        ULONG r3 = GetPerTcpConnectionEStats(&found, TcpConnectionEstatsData,
                                             nullptr, 0, 0, nullptr, 0, 0,
                                             reinterpret_cast<PUCHAR>(&rod), 0, (ULONG)sizeof(rod));
        Log("  GetPerTcpConnectionEStats(Data, ROD) rc=%lu %s\n",
            r3, r3 == 0 ? "OK" : "FAIL");
        if (r3 == 0) {
            Log("    DataBytesOut=%llu DataBytesIn=%llu\n",
                (unsigned long long)rod.DataBytesOut,
                (unsigned long long)rod.DataBytesIn);
        }
    }

    closesocket(c); closesocket(sv); closesocket(ls);
    WSACleanup();
}

// ------------------------------------------------------------------ 主探针

// 在自身令牌上启用某特权（CreateProcessAsUser 需要 SeAssignPrimaryToken / SeIncreaseQuota）
static bool EnablePriv(LPCWSTR name) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return false;
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, name, &luid)) { CloseHandle(tok); return false; }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL ok = AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    DWORD gle = GetLastError();
    CloseHandle(tok);
    return ok && gle == ERROR_SUCCESS;
}

static int RunMain(bool lowMode);

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // 子进程模式
    if (argc >= 4 && strcmp(argv[1], "--gen") == 0) {
        GenArgs ga{};
        ga.port  = static_cast<uint16_t>(atoi(argv[2]));
        ga.bytes = _strtoui64(argv[3], nullptr, 10);
        return RunGen(ga);
    }
    if (argc >= 3 && strcmp(argv[1], "--sink") == 0) {
        return RunSink(static_cast<uint16_t>(atoi(argv[2])));
    }

    bool lowMode = false;
    const wchar_t* logPath = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--low") == 0) lowMode = true;
        else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            static wchar_t buf[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, argv[++i], -1, buf, MAX_PATH);
            logPath = buf;
        }
    }

    // 写死日志路径：de-elevated 拉起的进程拿不到控制台，只能写文件
    wchar_t defLog[MAX_PATH];
    if (!logPath) {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        wcscpy_s(defLog, self);
        wchar_t* dot = wcsrchr(defLog, L'.');
        if (dot) *dot = 0;
        wcscat_s(defLog, L"_run.txt");
        logPath = defLog;
    }
    LogOpen(logPath);

    return RunMain(lowMode);
}

static int RunMain(bool lowMode) {
    if (lowMode) {
        // ---- 用受限令牌重新拉起自己（去掉 Administrators / 高完整性），验证非管理员行为 ----
        Log("=== --low: 用受限令牌重新拉起自己 ===\n");

        HANDLE tok = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &tok)) {
            Log("  OpenProcessToken 失败 GLE=%lu\n", GetLastError());
            return 1;
        }
        HANDLE restricted = nullptr;
        SID_AND_ATTRIBUTES disable[1]{};
        SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
        PSID admins = nullptr;
        AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                 0, 0, 0, 0, 0, 0, &admins);
        disable[0].Sid = admins;
        disable[0].Attributes = 0;

        // 不要用 DISABLE_MAX_PRIVILEGE：它会把 SeChangeNotifyPrivilege 也删掉，
        // 导致 CreateProcessAsUser 直接 ERROR_ACCESS_DENIED(5)。
        // 只用 SidsToDisable 把 Administrators 变成 deny-only，效果等同"非管理员运行"。
        BOOL ok = CreateRestrictedToken(tok, 0,
                                        1, disable,      // SidsToDisable: Administrators → deny-only
                                        0, nullptr,      // PrivilegesToDelete
                                        0, nullptr, &restricted);
        if (!ok) {
            Log("  CreateRestrictedToken 失败 GLE=%lu\n", GetLastError());
            return 1;
        }

        // 把完整性级别降到 Medium（普通用户级别）
        {
            BYTE mediumSid[SECURITY_MAX_SID_SIZE]{};
            DWORD sidLen = SECURITY_MAX_SID_SIZE;
            CreateWellKnownSid(WinMediumLabelSid, nullptr, mediumSid, &sidLen);
            TOKEN_MANDATORY_LABEL tml{};
            tml.Label.Attributes = SE_GROUP_INTEGRITY;
            tml.Label.Sid = reinterpret_cast<PSID>(mediumSid);
            if (!SetTokenInformation(restricted, TokenIntegrityLevel, &tml,
                                     sizeof(tml) + sidLen)) {
                Log("  SetTokenInformation(IntegrityLevel) 失败 GLE=%lu\n", GetLastError());
            }
        }

        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);

        // 子进程日志另开一个文件
        wchar_t childLog[MAX_PATH];
        wcscpy_s(childLog, self);
        wchar_t* dot = wcsrchr(childLog, L'.');
        if (dot) *dot = 0;
        wcscat_s(childLog, L"_low.txt");
        DeleteFileW(childLog);

        // 先把两个特权打开，否则 CreateProcessAsUser 报 1314
        Log("  EnablePriv(SeAssignPrimaryToken)=%d EnablePriv(SeIncreaseQuota)=%d\n",
            EnablePriv(SE_ASSIGNPRIMARYTOKEN_NAME), EnablePriv(SE_INCREASE_QUOTA_NAME));

        wchar_t cmd[MAX_PATH * 2];
        swprintf_s(cmd, L"\"%ls\" --log \"%ls\"", self, childLog);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        BOOL ok2 = CreateProcessAsUserW(restricted, self, cmd, nullptr, nullptr, FALSE,
                                        CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi);
        Log("  CreateProcessAsUserW(受限令牌) ok=%d GLE=%lu\n", ok2, ok2 ? 0 : GetLastError());
        ok = ok2;
        CloseHandle(restricted);
        CloseHandle(tok);
        FreeSid(admins);
        if (!ok) return 1;

        WaitForSingleObject(pi.hProcess, 60000);
        DWORD ec = 0;
        GetExitCodeProcess(pi.hProcess, &ec);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        Log("  受限进程退出码=%lu，其日志 %s:\n\n", ec, U8(childLog));

        // 把子日志贴出来（二进制读，原样转发 UTF-8 字节）
        FILE* f = nullptr;
        _wfopen_s(&f, childLog, L"rb");
        if (f) {
            char line[1024];
            while (fgets(line, sizeof(line), f)) { printf("%s", line); if (g_log) fputs(line, g_log); }
            fclose(f);
        }
        if (g_log) fflush(g_log);
        return 0;
    }

    Log("========== 每进程网络采集可行性探针 ==========\n");

    // ---- 0 环境 ----
    Log("\n=== 0. 环境 ===\n");
    {
        typedef LONG (WINAPI* RtlGetVersion_t)(PRTL_OSVERSIONINFOW);
        auto rtl = reinterpret_cast<RtlGetVersion_t>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        RTL_OSVERSIONINFOW vi{};
        vi.dwOSVersionInfoSize = sizeof(vi);
        if (rtl) rtl(&vi);
        Log("  OS: %lu.%lu build %lu\n", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
    }
    Log("  elevated=%s  in-Admins=%s  in-PerfLogUsers=%s  pid=%lu\n",
        IsElevated() ? "YES" : "NO",
        IsMemberOfAdmins() ? "YES" : "NO",
        IsMemberOfPerfLogUsers() ? "YES" : "NO",
        GetCurrentProcessId());
    {
        wchar_t user[256]; DWORD n = 256;
        GetUserNameW(user, &n);
        Log("  用户: %s\n", U8(user));
    }

    MIB_IPSTATS ipBefore{};
    GetIpStatisticsEx(&ipBefore, AF_INET);

    // ---- 1 StartTraceW ----
    Log("\n=== 1. StartTraceW（realtime session \"%s\"）===\n", U8(kSessionName));

    const ULONG nameLen = static_cast<ULONG>(wcslen(kSessionName) + 1);
    const ULONG propsSize = sizeof(EVENT_TRACE_PROPERTIES) + nameLen * sizeof(wchar_t);

    auto initProps = [&](std::vector<uint8_t>& b) -> EVENT_TRACE_PROPERTIES* {
        b.assign(propsSize, 0);
        auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(b.data());
        p->Wnode.BufferSize = propsSize;
        p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        p->Wnode.ClientContext = 1;            // QPC
        p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        p->BufferSize = 64;                    // KB
        p->MinimumBuffers = 8;
        p->MaximumBuffers = 32;
        p->FlushTimer = 0;
        p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        return p;
    };

    auto stopSession = [&]() {
        std::vector<uint8_t> b;
        initProps(b);
        return ControlTraceW(0, kSessionName, reinterpret_cast<EVENT_TRACE_PROPERTIES*>(b.data()),
                             EVENT_TRACE_CONTROL_STOP);
    };

    ULONG pre = stopSession();
    Log("  (预清理残留会话 ret=%lu GLE=%lu — 4201=没有该服务，正常)\n", pre, GetLastError());

    std::vector<uint8_t> pbuf;
    auto* props = initProps(pbuf);
    TRACEHANDLE session = 0;
    SetLastError(0);
    ULONG rc = StartTraceW(&session, kSessionName, props);
    DWORD gle = GetLastError();
    Log("  StartTraceW ret=%lu  GetLastError=%lu  session=0x%llX\n",
        rc, gle, (unsigned long long)session);
    Log("  → %s\n",
        rc == ERROR_SUCCESS ? "SUCCESS" :
        rc == ERROR_ACCESS_DENIED ? "ERROR_ACCESS_DENIED (没权限)" :
        rc == ERROR_ALREADY_EXISTS ? "ERROR_ALREADY_EXISTS (会话已存在)" :
        rc == ERROR_NO_SYSTEM_RESOURCES ? "ERROR_NO_SYSTEM_RESOURCES" :
        rc == ERROR_NOT_SUPPORTED ? "ERROR_NOT_SUPPORTED" : "OTHER");

    if (rc != ERROR_SUCCESS) {
        Log("\n  [结论] 无法启动 ETW 会话，ETW 路径全部跳过。\n");
        ProbePerConnEStats();
        return 2;
    }

    // ---- 2 EnableTraceEx2 ----
    Log("\n=== 2. EnableTraceEx2(Microsoft-Windows-Kernel-Network) ===\n");
    SetLastError(0);
    ULONG e1 = EnableTraceEx2(session, &kKernelNetworkGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                              TRACE_LEVEL_INFORMATION, 0, 0, 0, nullptr);
    Log("  EnableTraceEx2(level=4, keyword=ANY) ret=%lu GLE=%lu %s\n",
        e1, GetLastError(), e1 == ERROR_SUCCESS ? "OK" : "FAIL");

    // ---- 3 实时消费 ----
    Log("\n=== 3. OpenTraceW + ProcessTrace (realtime consumer) ===\n");

    ProbeCtx ctx;
    InitializeCriticalSection(&ctx.lock);

    EVENT_TRACE_LOGFILEW lf{};
    lf.LoggerName = const_cast<LPWSTR>(kSessionName);
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    lf.EventRecordCallback = OnEvent;
    lf.Context = &ctx;

    SetLastError(0);
    TRACEHANDLE consumer = OpenTraceW(&lf);
    Log("  OpenTraceW → 0x%llX  GLE=%lu\n", (unsigned long long)consumer, GetLastError());
    if (consumer == INVALID_PROCESSTRACE_HANDLE) {
        Log("  失败，终止。\n");
        stopSession();
        return 3;
    }

    std::atomic<DWORD> consumerTid{0};
    std::atomic<bool> running{true};
    std::thread consumerThread([&]() {
        consumerTid = GetCurrentThreadId();
        ULONG pr = ProcessTrace(&consumer, 1, nullptr, nullptr);
        Log("  [consumer 线程] ProcessTrace 返回 %lu\n", pr);
        running = false;
    });
    for (int i = 0; i < 200 && consumerTid.load() == 0; ++i) Sleep(5);
    Sleep(100);

    HANDLE cThread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, consumerTid.load());
    Log("  consumer 线程 tid=%lu 句柄=%s\n",
        consumerTid.load(), cThread ? "OK" : "FAIL");

    // 线程级 CPU：GetThreadTimes 只有 15.6ms 精度（一个时钟 tick），
    // 所以另外用 QueryThreadCycleTime / QueryProcessCycleTime 拿 CPU 周期数，
    // 精度到单条指令级别（这两个 API 在 Win7+ 可用，内部用 rdtsc/线程周期计数）。
    typedef BOOL (WINAPI* QueryThreadCycleTime_t)(HANDLE, PULONG64);
    typedef BOOL (WINAPI* QueryProcessCycleTime_t)(HANDLE, PULONG64);
    auto fnQtc = reinterpret_cast<QueryThreadCycleTime_t>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "QueryThreadCycleTime"));
    auto fnQpc = reinterpret_cast<QueryProcessCycleTime_t>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "QueryProcessCycleTime"));

    auto threadCpu = [&]() -> uint64_t {
        if (!cThread) return 0;
        FILETIME c, e, k, u;
        if (!GetThreadTimes(cThread, &c, &e, &k, &u)) return 0;
        return FileTimeToU64(k) + FileTimeToU64(u);
    };
    auto procCycles = [&]() -> uint64_t {
        ULONG64 c = 0;
        if (fnQpc && fnQpc(GetCurrentProcess(), &c)) return c;
        return 0;
    };
    auto threadCycles = [&]() -> uint64_t {
        ULONG64 c = 0;
        if (cThread && fnQtc && fnQtc(cThread, &c)) return c;
        return 0;
    };
    Log("  高精度计数器: QueryProcessCycleTime=%s QueryThreadCycleTime=%s\n",
        fnQpc ? "OK" : "N/A", fnQtc ? "OK" : "N/A");

    // ---- 3a 空载 6 秒 ----
    Log("\n  [阶段 A] 空载 6 秒（只看本机后台网络，不主动造流量）...\n");
    uint64_t proc0 = ProcCpu100ns();
    uint64_t thr0  = threadCpu();
    uint64_t pcy0  = procCycles();
    uint64_t tcy0  = threadCycles();
    uint64_t ev0 = ctx.totalEvents.load();
    LARGE_INTEGER q0, qf;
    QueryPerformanceCounter(&q0);
    QueryPerformanceFrequency(&qf);

    for (int i = 0; i < 6; ++i) {
        Sleep(1000);
        Log("    t=%ds events=%llu net=%llu\n", i + 1,
            (unsigned long long)ctx.totalEvents.load(),
            (unsigned long long)ctx.netEvents.load());
    }

    uint64_t proc1 = ProcCpu100ns();
    uint64_t thr1  = threadCpu();
    uint64_t pcy1  = procCycles();
    uint64_t tcy1  = threadCycles();
    LARGE_INTEGER q1;
    QueryPerformanceCounter(&q1);
    uint64_t ev1 = ctx.totalEvents.load();

    double wallA = double(q1.QuadPart - q0.QuadPart) / double(qf.QuadPart);
    double procA = double(proc1 - proc0) / 1e7;
    double thrA  = double(thr1 - thr0) / 1e7;
    Log("    空载 %.2fs: 事件 %llu 条 | 进程CPU %.6fs | consumer线程CPU %.6fs\n",
        wallA, (unsigned long long)(ev1 - ev0), procA, thrA);
    Log("    → 进程占单核 = %.3f%%   consumer 线程占单核 = %.3f%%   (事件率 %.0f 条/秒)\n",
        procA / wallA * 100.0, thrA / wallA * 100.0, double(ev1 - ev0) / wallA);
    Log("    周期计数: 进程 Δ=%llu cycles, consumer 线程 Δ=%llu cycles（3GHz ≈ ÷3e9 得秒）\n",
        (unsigned long long)(pcy1 - pcy0), (unsigned long long)(tcy1 - tcy0));

    // ---- 3b 回环流量 + 子进程归属验证 ----
    //
    // 关键：流量两端都放到子进程里跑。探针进程这段时间不做任何 socket 收发，
    // 它消耗的 CPU 就只剩 ETW consumer 回调和解析，测出来才是"纯采集开销"。
    Log("\n  [阶段 B] 回环 TCP 双向流量（sink 子进程 + gen 子进程，各 64MB）...\n");

    uint64_t childSent = 0, childRecvd = 0;
    DWORD childPid = 0, sinkPid = 0;
    // 这两组计数只量"纯流量期间"的采集开销，把建立进程/等端口的时间排除掉
    uint64_t pcyB0 = 0, pcyB1 = 0;
    LARGE_INTEGER qB0{}, qB1{};
    {
        // 先起 sink（监听在固定端口），再起 gen 去连
        uint16_t port = 0;
        {
            // 用一个临时 socket 挑空闲端口，然后关掉让 sink 复用
            WSADATA wsa{};
            WSAStartup(MAKEWORD(2, 2), &wsa);
            SOCKET tmp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port = 0;
            bind(tmp, reinterpret_cast<sockaddr*>(&a), sizeof(a));
            int alen = sizeof(a);
            getsockname(tmp, reinterpret_cast<sockaddr*>(&a), &alen);
            port = ntohs(a.sin_port);
            closesocket(tmp);
            WSACleanup();
        }

        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION piSink{}, piGen{};

        wchar_t cmdSink[MAX_PATH + 64];
        swprintf_s(cmdSink, L"\"%ls\" --sink %u", self, port);
        BOOL okSink = CreateProcessW(nullptr, cmdSink, nullptr, nullptr, FALSE,
                                     CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &piSink);
        if (okSink) {
            sinkPid = piSink.dwProcessId;
            CloseHandle(piSink.hThread);
            Sleep(300);   // 等 sink 进 listen

            // 从 gen 启动前开始计，gen 退出后结束 —— 这段窗口里探针只跑 ETW consumer
            pcyB0 = procCycles();
            QueryPerformanceCounter(&qB0);

            wchar_t cmdGen[MAX_PATH + 64];
            swprintf_s(cmdGen, L"\"%ls\" --gen %u 67108864", self, port);
            BOOL okGen = CreateProcessW(nullptr, cmdGen, nullptr, nullptr, FALSE,
                                        CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &piGen);
            if (okGen) {
                childPid = piGen.dwProcessId;
                CloseHandle(piGen.hThread);
                WaitForSingleObject(piGen.hProcess, 60000);
                CloseHandle(piGen.hProcess);
            } else {
                Log("    gen 子进程创建失败 GLE=%lu\n", GetLastError());
            }
            pcyB1 = procCycles();
            QueryPerformanceCounter(&qB1);

            WaitForSingleObject(piSink.hProcess, 15000);
            CloseHandle(piSink.hProcess);
            // 双向各 64MB（gen 发 64MB，sink 回声 64MB）
            childSent = 67108864ull;
            childRecvd = 67108864ull;
            Log("    sink pid=%lu, gen pid=%lu，双向各 64MB\n", sinkPid, childPid);
        } else {
            Log("    sink 子进程创建失败 GLE=%lu\n", GetLastError());
        }
    }

    Sleep(3000);   // 等事件全部落定

    uint64_t proc2 = ProcCpu100ns();
    uint64_t thr2  = threadCpu();
    uint64_t pcy2  = procCycles();
    uint64_t tcy2  = threadCycles();
    LARGE_INTEGER q2;
    QueryPerformanceCounter(&q2);
    double wallB = double(q2.QuadPart - q1.QuadPart) / double(qf.QuadPart);
    double procB = double(proc2 - proc1) / 1e7;
    double thrB  = double(thr2 - thr1) / 1e7;
    Log("    阶段B 累计 %.2fs: 探针进程CPU %.6fs (%.3f%% 单核) | consumer线程 %.6fs (%.3f%% 单核)\n",
        wallB, procB, procB / wallB * 100.0, thrB, thrB / wallB * 100.0);
    Log("    周期计数: 进程 Δ=%llu cycles, consumer 线程 Δ=%llu cycles\n",
        (unsigned long long)(pcy2 - pcy1), (unsigned long long)(tcy2 - tcy1));
    Log("    → 探针进程在阶段B 只做 ETW 消费，没有任何 socket 收发\n");

    // ---- 4 收尾 ----
    Log("\n=== 4. 停止会话 ===\n");
    ULONG sr = stopSession();
    Log("  ControlTraceW(STOP) ret=%lu\n", sr);
    for (int i = 0; i < 100 && running.load(); ++i) Sleep(100);
    if (consumerThread.joinable()) consumerThread.join();
    CloseTrace(consumer);
    Log("  CloseTrace 完成，consumer 线程退出\n");

    // ---- 5 结果 ----
    Log("\n=== 5. 捕获结果 ===\n");
    Log("  总事件 = %llu\n", (unsigned long long)ctx.totalEvents.load());
    Log("  网络事件(10/11) = %llu\n", (unsigned long long)ctx.netEvents.load());
    Log("  其他事件 = %llu\n", (unsigned long long)ctx.otherEvents.load());
    Log("  畸形(长度不足) = %llu\n", (unsigned long long)ctx.malformed.load());

    Log("  按事件 ID 计数:\n");
    for (int i = 0; i < 256; ++i) {
        if (ctx.idCount[i])
            Log("    id=%-3d ver=%d : %u 条\n", i, ctx.versionSeen[i], ctx.idCount[i]);
    }

    Log("\n  前 %u 条网络事件原始 payload:\n", ctx.dumpsDone);
    for (uint32_t i = 0; i < ctx.dumpsDone; ++i) {
        printf("    [%u] id=%u ver=%u pid=%-6u size=%-8u len=%u raw=",
               i, ctx.rawId[i], ctx.rawVer[i], ctx.rawPid[i], ctx.rawSize[i], ctx.rawLen[i]);
        PrintHex(ctx.rawBuf[i], ctx.rawLen[i]);
        printf("\n");
        if (g_log) {
            fprintf(g_log, "    [%u] id=%u ver=%u pid=%-6u size=%-8u len=%u raw=",
                    i, ctx.rawId[i], ctx.rawVer[i], ctx.rawPid[i], ctx.rawSize[i], ctx.rawLen[i]);
            for (uint32_t k = 0; k < ctx.rawLen[i]; ++k) fprintf(g_log, "%02X ", ctx.rawBuf[i][k]);
            fprintf(g_log, "\n");
        }
    }

    Log("\n  按 PID 汇总（前 20，按总量降序）:\n");
    {
        std::vector<std::pair<uint32_t, Agg>> v(ctx.perPid.begin(), ctx.perPid.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
            return (a.second.recvBytes + a.second.sendBytes) >
                   (b.second.recvBytes + b.second.sendBytes);
        });
        Log("    %-8s %-14s %-14s %-8s %-8s\n", "pid", "recvBytes", "sendBytes", "recvEv", "sendEv");
        uint64_t sumR = 0, sumS = 0;
        for (size_t i = 0; i < v.size(); ++i) {
            sumR += v[i].second.recvBytes;
            sumS += v[i].second.sendBytes;
            if (i < 20)
                Log("    %-8u %-14llu %-14llu %-8u %-8u\n", v[i].first,
                    (unsigned long long)v[i].second.recvBytes,
                    (unsigned long long)v[i].second.sendBytes,
                    v[i].second.recvEvents, v[i].second.sendEvents);
        }
        Log("    ... 共 %zu 个 PID 有网络事件；合计 recv=%llu send=%llu\n",
            v.size(), (unsigned long long)sumR, (unsigned long long)sumS);

        auto it = ctx.perPid.find(childPid);
        auto itSink = ctx.perPid.find(sinkPid);
        if (childPid && it != ctx.perPid.end()) {
            double errS = childSent ? (double(it->second.sendBytes) - double(childSent)) / double(childSent) * 100.0 : 0;
            Log("\n    ★ gen 子进程 pid=%lu 归属验证（唯一变量：它只做了 socket 收发）:\n", childPid);
            Log("      ETW send=%llu  实际发出=%llu  误差=%.6f%%\n",
                (unsigned long long)it->second.sendBytes,
                (unsigned long long)childSent, errS);
            Log("      ETW recv=%llu  实际收到=%llu\n",
                (unsigned long long)it->second.recvBytes,
                (unsigned long long)childRecvd);
        } else {
            Log("\n    ★ gen 子进程 pid=%lu 未出现在 ETW 汇总里（PID 归属失败）\n", childPid);
        }
        if (sinkPid && itSink != ctx.perPid.end()) {
            Log("    ★ sink 子进程 pid=%lu: recv=%llu send=%llu（回声端）\n", sinkPid,
                (unsigned long long)itSink->second.recvBytes,
                (unsigned long long)itSink->second.sendBytes);
        }

        auto me = ctx.perPid.find(GetCurrentProcessId());
        if (me != ctx.perPid.end()) {
            Log("    ★ 本探针 pid=%lu: recv=%llu send=%llu\n", GetCurrentProcessId(),
                (unsigned long long)me->second.recvBytes,
                (unsigned long long)me->second.sendBytes);
        }

        MIB_IPSTATS ipAfter{};
        GetIpStatisticsEx(&ipAfter, AF_INET);
        Log("\n    系统级 GetIpStatisticsEx dwInReceives: %lu → %lu (Δ=%llu 包)\n",
            ipBefore.dwInReceives, ipAfter.dwInReceives,
            (unsigned long long)(ipAfter.dwInReceives - ipBefore.dwInReceives));
    }

    // 周期 → 秒：服务器级 CPU 恒频 3.00GHz（PROGRESS.md 已确认无 Turbo）
    const double kGhz = 3.0;
    double cycA = double(pcy1 - pcy0) / (kGhz * 1e9);
    double cycB = double(pcy2 - pcy1) / (kGhz * 1e9);

    // 纯流量窗口（gen 存活期间）的精确开销
    double wallBw = 0.0, cycBw = 0.0;
    if (qB1.QuadPart > qB0.QuadPart) {
        wallBw = double(qB1.QuadPart - qB0.QuadPart) / double(qf.QuadPart);
        cycBw  = double(pcyB1 - pcyB0) / (kGhz * 1e9);
    }
    uint64_t evB = ctx.totalEvents.load();   // 供参考

    Log("\n=== 6. CPU 开销汇总 ===\n");
    Log("  阶段A 空载 %.2fs: 进程 %.4f%% 单核 | consumer 线程 %.4f%% 单核 (事件 %.0f/s)\n",
        wallA, procA / wallA * 100.0, thrA / wallA * 100.0, double(ev1 - ev0) / wallA);
    Log("  阶段B 流量 %.2fs: 进程 %.4f%% 单核 | consumer 线程 %.4f%% 单核\n",
        wallB, procB / wallB * 100.0, thrB / wallB * 100.0);
    Log("  整机折算（20 逻辑核）: 空载 %.5f%%  含流量 %.5f%%\n",
        procA / wallA * 100.0 / 20.0, procB / wallB * 100.0 / 20.0);
    Log("  绝对 CPU 时间: 空载 %.4f ms/s, 流量期 %.4f ms/s\n",
        procA / wallA * 1000.0, procB / wallB * 1000.0);
    Log("\n  [周期计数器交叉验证, 按 %.1fGHz 折算 — 精度高于 15.6ms 的时钟 tick]\n", kGhz);
    Log("    阶段A 空载 %.2fs: 进程周期 %.4fs → %.4f%% 单核 (%.4f ms/s)\n",
        wallA, cycA, cycA / wallA * 100.0, cycA / wallA * 1000.0);
    Log("    阶段B 全段 %.2fs: 进程周期 %.4fs → %.4f%% 单核 (%.4f ms/s)\n",
        wallB, cycB, cycB / wallB * 100.0, cycB / wallB * 1000.0);
    if (wallBw > 0.1) {
        Log("    阶段B 纯流量窗口 %.2fs（gen 子进程存活期间，探针只消费 ETW）:\n", wallBw);
        Log("      进程周期 %.4fs → %.4f%% 单核 (%.4f ms/s)\n",
            cycBw, cycBw / wallBw * 100.0, cycBw / wallBw * 1000.0);
        Log("      期间累计事件 %llu 条\n", (unsigned long long)evB);
    }

    if (cThread) CloseHandle(cThread);

    ProbePerConnEStats();

    DeleteCriticalSection(&ctx.lock);
    Log("\n========== 探针结束 ==========\n");
    return 0;
}
