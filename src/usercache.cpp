// usercache.cpp - 进程所属用户名解析（带缓存）
//
// 成本实测：425 个进程全量 OpenProcessToken + LookupAccountSid = 61ms，
// 其中 SID→名字占 55.5ms。虽然不贵，但每秒一次就是 61ms/s，没必要。
// 策略：SID → 名字 建全局映射表缓存；每个进程只解析一次。
//
// 【本文件的缓存有两条必须同时满足的要求】见 include/tmwindow.h 里
// userCache_ 的成员注释：
//   1. 不能无界增长 —— 每条带 pid，进程反复启停会越攒越多；
//   2. pid 复用必须失效 —— 否则进程 A 退出、pid 被 B 拿到后，
//      详细信息页的「用户名」列会显示 A 的用户名，是完全错误的信息。
// 所以缓存值是 (用户名, 进程创建时间)，命中时比对创建时间。
#include "tmwindow.h"

#include <sddl.h>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace tmp {

namespace {

// 从当前快照里取某 pid 的创建时间。取不到返回 0。
// 0 表示"不知道"——此时不做 pid 复用判断（宁可缓存命中，也不要每次重查）。
int64_t CreateTimeOf(const Snapshot* snap, uint32_t pid) {
    if (!snap) return 0;
    for (const auto& e : snap->processes) {
        if (e.pid == pid) return e.createTime.QuadPart;
    }
    return 0;
}

} // namespace

std::wstring TmWindow::ResolveUser(uint32_t pid) {
    // 已缓存：还要确认缓存没有因为 pid 复用而失效。
    // 创建时间对不上 → 这是新进程，旧条目作废。
    const int64_t ct = CreateTimeOf(snap_.get(), pid);
    auto it = userCache_.find(pid);
    if (it != userCache_.end()) {
        // 【不再用 `createTime == 0` 当作"匹配"】
        // 原写法 `sameProc = (ct == 0 || it->second.createTime == 0 || ...)`
        // 有个致命后果：缓存里存的就是 0（因为写入时 CreateTimeOf 返回 0），
        // 于是此后**每次命中都走这条短路**，pid 复用永远检测不到 ——
        // 一旦某次取不到创建时间，那条错误用户名就被永久钉死。
        //
        // 正确语义：只有两边都有确切值时才做比对；任一边未知就保守地
        // 认为"可能已复用"，重新查一次（查一次的成本是 OpenProcess +
        // LookupAccountSid，约 0.15 ms，换来正确性）。
        const bool bothKnown = (ct != 0 && it->second.createTime != 0);
        const bool sameProc = bothKnown && (it->second.createTime == ct);
        if (sameProc) return it->second.name;
        userCache_.erase(it);   // 已知不同，或至少一边未知 → 丢弃重查
    }

    std::wstring result;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
        HANDLE hTok = nullptr;
        if (OpenProcessToken(hProc, TOKEN_QUERY, &hTok)) {
            DWORD len = 0;
            GetTokenInformation(hTok, TokenUser, nullptr, 0, &len);
            if (len > 0) {
                std::vector<BYTE> buf(len);
                if (GetTokenInformation(hTok, TokenUser, buf.data(), len, &len)) {
                    auto* tu = reinterpret_cast<TOKEN_USER*>(buf.data());
                    // SID → 字符串作为缓存键（同一用户只查一次 SAM）
                    LPWSTR sidStr = nullptr;
                    if (ConvertSidToStringSidW(tu->User.Sid, &sidStr)) {
                        std::wstring sidKey(sidStr);
                        LocalFree(sidStr);

                        auto sit = sidNameCache_.find(sidKey);
                        if (sit != sidNameCache_.end()) {
                            result = sit->second;
                        } else {
                            wchar_t name[256] = {};
                            wchar_t domain[256] = {};
                            DWORD nameLen = 256, domLen = 256;
                            SID_NAME_USE use;
                            if (LookupAccountSidW(nullptr, tu->User.Sid,
                                                  name, &nameLen,
                                                  domain, &domLen, &use)) {
                                // 任务管理器显示 "域\用户名"；本机账户只显示用户名
                                if (domLen > 0 && _wcsicmp(domain, L"") != 0) {
                                    result = std::wstring(domain) + L"\\" + name;
                                } else {
                                    result = name;
                                }
                            }
                            sidNameCache_[sidKey] = result;
                        }
                    }
                }
            }
            CloseHandle(hTok);
        }
        CloseHandle(hProc);
    }

    // 缓存（空结果也缓存，避免对受保护进程反复 OpenProcess）。
    // 带上创建时间，供下次命中时判断 pid 有没有被复用。
    userCache_[pid] = UserCacheEntry{ result, ct };
    return result;
}

} // namespace tmp
