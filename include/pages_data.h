// pages_data.h - 四个次要页面的数据源
//
// 全部方案来自本机实测（docs/specs/minor_pages_spec.md）：
//   启动页：注册表 Run 键 3.14 ms / 23 项；WMI 备选 754 ms（慢 240 倍，不用）
//   用户页：WTSEnumerateSessionsW 8.59 ms / 4 会话
//   服务页：EnumServicesStatusExW 44.53 ms / 306 服务
//   应用历史：SRUM 需要 SYSTEM 权限 + 逆向签名，第一版显示"无可用数据"
#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

namespace tmp {

// ============ 启动页 ============

struct StartupItem {
    std::wstring name;          // 启动项名称（注册表值名或 .lnk 文件名）
    std::wstring command;       // 命令行
    std::wstring exePath;       // 解析出的 exe 路径
    std::wstring publisher;     // 从 exe 版本信息取 CompanyName
    bool enabled = true;
    bool fromRegistry = true;
    std::wstring sourceKey;     // 来源（"HKCU\Run" / "启动文件夹"）
    FILETIME disabledTime{};    // 禁用时间（仅 enabled=false 时有效）
};

class StartupSource {
public:
    // 枚举所有启动项。实测 3.14 ms / 23 项。
    bool Enumerate(std::vector<StartupItem>& out);

    // 启用/禁用。写 StartupApproved 的 DWORD[0] = 2 / 3。
    // 实测格式：12 字节 REG_BINARY，头 4 字节是状态。
    bool SetEnabled(const StartupItem& item, bool enabled);

private:
    void ReadRunKey(HKEY root, const wchar_t* subKey, const wchar_t* label,
                    std::vector<StartupItem>& out);
    void ReadStartupFolder(const wchar_t* folderPath, const wchar_t* label,
                           std::vector<StartupItem>& out);
    void ApplyApprovedState(std::vector<StartupItem>& out);
    void ResolvePublisher(StartupItem& item);
};

// ============ 用户页 ============

struct UserSession {
    uint32_t sessionId = 0;
    std::wstring userName;      // "Administrator"
    std::wstring domain;        // "WIN10PC-SQKL520"
    std::wstring clientName;
    std::wstring winStation;
    std::wstring stateText;     // "活动" / "已断开" / "侦听"
    int stateRaw = 0;
    // 从进程快照聚合
    double cpuPercent = 0.0;
    uint64_t workingSet = 0;
    uint32_t processCount = 0;
    bool connected() const { return stateRaw == 0; }   // WTSActive
    bool disconnected() const { return stateRaw == 4; } // WTSDisconnected
};

class UserSource {
public:
    // 枚举会话。实测 8.59 ms / 4 会话（过滤掉 >=65536 的侦听器）。
    bool Enumerate(std::vector<UserSession>& out);
};

// ============ 服务页 ============

struct ServiceItem {
    std::wstring name;
    std::wstring displayName;
    std::wstring description;   // 渐进填充
    std::wstring group;         // 渐进填充
    uint32_t pid = 0;
    uint32_t stateRaw = 0;
    std::wstring stateText;     // "正在运行" / "已停止" ...
    bool canStop = false;
    bool canStart = false;
};

class ServiceSource {
public:
    // 枚举服务（不含驱动）。实测 44.53 ms / 306 服务。
    bool Enumerate(std::vector<ServiceItem>& out);

    // 补充描述与组。每个约 2.8ms，必须缓存 + 渐进填充。
    bool EnrichOne(ServiceItem& item);

    bool Start(const std::wstring& name);
    bool Stop(const std::wstring& name);
    bool Restart(const std::wstring& name);
};

// ============ 应用历史页 ============

struct AppHistoryItem {
    std::wstring name;
    uint64_t cpuTimeMs = 0;
    uint64_t networkBytes = 0;
    uint64_t tileUpdates = 0;
    uint64_t downloadsBytes = 0;
};

// SRUM 需要 SYSTEM 权限 + srumapi.dll 未公开签名（实测会 segfault）。
// 第一版按规格建议：显示"无可用数据"，与原版在 SRUM 禁用时一致。
class AppHistorySource {
public:
    // 返回 false 表示无数据（本实现恒为 false，但接口保留以便将来接入）
    bool Enumerate(std::vector<AppHistoryItem>& out);
    std::wstring StatusText() const;
};

} // namespace tmp
