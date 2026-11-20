// classifier.h - 进程分组与状态判定
// 规则来自本机实测（见 docs/specs/proc_semantics.md）：
//   「应用」= EnumWindows 5 条排除规则命中的顶层窗口
//   「Windows 进程」= Session0 服务宿主 ∪ System32 下的非用户进程 ∪ 白名单
//   「后台进程」= 其余
//   「已挂起」= 该进程全部线程 WaitReason == Suspended
#pragma once

#include "snapshot.h"
#include <windows.h>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <string>

namespace tmp {

enum class ProcessGroup {
    Apps = 0,       // 应用
    Background,     // 后台进程
    WindowsProcs,   // Windows 进程
    Count
};

inline const wchar_t* GroupTitle(ProcessGroup g) {
    switch (g) {
    case ProcessGroup::Apps:         return L"应用";
    case ProcessGroup::Background:   return L"后台进程";
    case ProcessGroup::WindowsProcs: return L"Windows 进程";
    default:                         return L"";
    }
}

// 每个进程的分类结果
struct Classification {
    // pid -> 组
    std::unordered_map<uint32_t, ProcessGroup> groupOf;
    // pid -> 是否为"应用根"（有可见顶层窗口）
    std::unordered_set<uint32_t> appRoots;
    // pid -> 应用根 pid（用于把子进程归到应用名下）
    std::unordered_map<uint32_t, uint32_t> appRootOf;
    // pid -> 已挂起
    std::unordered_set<uint32_t> suspended;
    // pid -> 无响应（可见窗口 IsHungAppWindow 为真）
    // 与 suspended 同构。判定用纯查询 API，34 个可见窗口约 0.04ms。
    std::unordered_set<uint32_t> notResponding;
    // 应用根 pid -> 显示名（窗口标题）
    std::unordered_map<uint32_t, std::wstring> appTitle;

    void Clear() {
        groupOf.clear();
        appRoots.clear();
        appRootOf.clear();
        suspended.clear();
        notResponding.clear();
        appTitle.clear();
    }
};

class Classifier {
public:
    // 重新分类。对同一份快照可重复调用。
    // 注意：内部做一次 EnumWindows，实测开销 < 1ms，可放心每秒调用。
    //
    // pathMap（可选）：pid -> exe 完整路径。传入后「Windows 进程」判断
    // 会用真实路径（System32/SysWOW64 下的一律算），比纯名字启发式准确得多。
    void Classify(const Snapshot& snap, Classification& out,
                  const std::unordered_map<uint32_t, std::wstring>* pathMap = nullptr);

    // 单独暴露给 UI：判断某 pid 是否挂起（快照里已带线程状态）
    static bool IsSuspended(const Snapshot& snap, uint32_t pid);

private:
    void CollectAppRoots(Classification& out);
    void DetectSuspended(const Snapshot& snap, Classification& out);
    void AssignGroups(const Snapshot& snap, Classification& out,
                      const std::unordered_map<uint32_t, std::wstring>* pathMap);
    void BuildAppTree(const Snapshot& snap, Classification& out);
    static bool LooksLikeWindowsProcess(const ProcessEntry& e, bool isSession0,
                                        const std::wstring* fullPath);

    // 当前轮次的路径表（AssignGroups 期间有效）
    const std::unordered_map<uint32_t, std::wstring>* curPathMap_ = nullptr;
};

} // namespace tmp
