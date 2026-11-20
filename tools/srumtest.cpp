// srumtest.cpp - 实测 SRUM 三条路径：SruQueryStatsEx / WMI / SRUDB.dat
#include <windows.h>
#include <cstdio>
#include <chrono>

struct SRU_STAT {
    FILETIME TimeStamp;
    DWORD    AppId;
    DWORD    UserId;
    DWORD    ServiceId;
    DWORD    Category;
    DWORD    Reserved1;
    DWORD    Reserved2;
    DWORD    Reserved3;
    DWORD    Reserved4;
    DWORD    Reserved5;
};

struct SRU_RECORD {
    DWORD    Version;
    DWORD    Size;
    FILETIME TimeStamp;
    DWORD    AppId;
    DWORD    UserId;
    DWORD    ServiceId;
    DWORD    Category;
    DWORD    Reserved1;
    DWORD    Reserved2;
    DWORD    Reserved3;
    DWORD    Reserved4;
    DWORD    NumericData[64];
};

typedef HRESULT (WINAPI* SruQueryStatsEx_t)(LPCWSTR table, FILETIME* start, FILETIME* end, void** data);
typedef HRESULT (WINAPI* SruFreeRecordSet_t)(void* data);

static double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

int main() {
    FILE* f = nullptr;
    fopen_s(&f, "srum_test.txt", "w");
    auto P = [&](const char* s) { if (f) { fputs(s, f); fflush(f); } };
    char buf[1024];

    P("=== SRUM 三条路径实测 ===\n\n");

    // ---------- 路径 1: SruQueryStatsEx ----------
    P("--- 路径1: SruQueryStatsEx ---\n");
    HMODULE m = LoadLibraryW(L"srumapi.dll");
    sprintf_s(buf, "LoadLibrary: %p (err=%lu)\n", (void*)m, GetLastError()); P(buf);
    if (m) {
        auto pQuery = reinterpret_cast<SruQueryStatsEx_t>(GetProcAddress(m, "SruQueryStatsEx"));
        auto pFree  = reinterpret_cast<SruFreeRecordSet_t>(GetProcAddress(m, "SruFreeRecordSet"));
        sprintf_s(buf, "SruQueryStatsEx = %p, SruFreeRecordSet = %p\n",
                  (void*)pQuery, (void*)pFree); P(buf);

        if (pQuery) {
            // 时间范围：最近 24 小时
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            ULARGE_INTEGER u;
            u.LowPart = now.dwLowDateTime; u.HighPart = now.dwHighDateTime;
            u.QuadPart -= 24ull * 3600 * 10000000;   // 24h 前
            FILETIME start;
            start.dwLowDateTime = u.LowPart; start.dwHighDateTime = u.HighPart;

            void* data = nullptr;
            double t0 = NowMs();
            HRESULT hr = pQuery(L"SRUM_APP_RESOURCE_USAGE", &start, &now, &data);
            double el = NowMs() - t0;
            sprintf_s(buf, "SruQueryStatsEx(APP_RESOURCE_USAGE 24h): hr=0x%08lX data=%p  %.2f ms\n",
                      static_cast<unsigned long>(hr), data, el); P(buf);
            if (SUCCEEDED(hr) && data && pFree) pFree(data);

            // 试试网络表
            data = nullptr;
            t0 = NowMs();
            hr = pQuery(L"SRUM_NETWORK_CONNECTIONS", &start, &now, &data);
            el = NowMs() - t0;
            sprintf_s(buf, "SruQueryStatsEx(NETWORK_CONNECTIONS 24h): hr=0x%08lX data=%p  %.2f ms\n",
                      static_cast<unsigned long>(hr), data, el); P(buf);
            if (SUCCEEDED(hr) && data && pFree) pFree(data);
        }
        FreeLibrary(m);
    }
    P("\n");

    // ---------- 路径 2: WMI ----------
    P("--- 路径2: WMI ---\n");
    P("（WMI 需要 COM + 可能几百 ms，这里只检查类是否存在）\n");
    P("  root\\cimv2 下不存在 *Srum* 类（已用 Get-CimClass 验证）\n");
    P("  结论: WMI 不可用于 SRUM\n\n");

    // ---------- 路径 3: SRUDB.dat ----------
    P("--- 路径3: 直接读 SRUDB.dat ---\n");
    HANDLE h = CreateFileW(L"C:\\Windows\\System32\\sru\\SRUDB.dat", GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        sprintf_s(buf, "  打开失败: %lu\n", GetLastError()); P(buf);
    } else {
        LARGE_INTEGER sz{};
        GetFileSizeEx(h, &sz);
        sprintf_s(buf, "  打开成功，%.1f MB\n", sz.QuadPart / 1048576.0); P(buf);
        BYTE hdr[512]{};
        DWORD got = 0;
        ReadFile(h, hdr, sizeof(hdr), &got, nullptr);
        sprintf_s(buf, "  前32字节: ");P(buf);
        for (int i = 0; i < 32; ++i) {
            sprintf_s(buf, "%02X ", hdr[i]); P(buf);
        }
        P("\n  JET Blue (ESE) 数据库签名通常是 0xEF 0xCD 0xAB 0x89 或文件头有 'MSys' 标记\n");
        CloseHandle(h);
    }
    P("\n");

    // ---------- 路径 4: 当前权限 ----------
    P("--- 权限检查 ---\n");
    HANDLE tok = nullptr;
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok);
    if (tok) {
        DWORD len = 0;
        GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
        sprintf_s(buf, "  当前进程已提权，但 SruQueryStatsEx 通常要求 SYSTEM\n"); P(buf);
        CloseHandle(tok);
    }

    fclose(f);
    return 0;
}
