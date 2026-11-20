// snapshot.h - 进程快照数据结构
// 设计依据: DIAGNOSIS.md 六、实测选型表
//   - 单次 NtQuerySystemInformation(SystemProcessInformation) 拿全量 (实测 7.3-11.1ms)
//   - CPU% 用两次 KernelTime+UserTime 差分, 成本 0
//   - 绝不调用 PDH \Thread(*) / WMI
#pragma once

#include <windows.h>
#include <cstdint>
#include <vector>
#include <string>

namespace tmp {

// 单进程的采集数据。POD，无堆分配（字符串除外），便于整体 swap 发布。
struct ProcessEntry {
    uint32_t pid = 0;
    uint32_t parentPid = 0;
    uint32_t threadCount = 0;
    uint32_t handleCount = 0;
    // 处于 Suspended 等待原因的线程数。全部线程都挂起 → 进程显示"已挂起"。
    uint32_t suspendedThreadCount = 0;

    // 时间（累计 100ns 单位）
    uint64_t kernelTime = 0;
    uint64_t userTime = 0;
    LARGE_INTEGER createTime{};

    // 内存
    uint64_t workingSet = 0;          // WorkingSetSize
    uint64_t privateBytes = 0;        // WorkingSetPrivateSize (可能为 0，需回退)
    uint64_t virtualSize = 0;         // VirtualSize
    uint64_t pagefileUsage = 0;

    // IO
    uint64_t readBytes = 0;
    uint64_t writeBytes = 0;

    // 身份
    uint32_t sessionId = 0;
    std::wstring imageName;           // 快照里直接带（ImageName.Buffer），无 IO

    // ---- 以下为差分/派生字段，每轮刷新时重算 ----
    double cpuPercent = 0.0;          // 相对全系统 20 个逻辑核
    uint64_t cpuCycleDelta = 0;       // 本轮消耗的 100ns

    // 磁盘吞吐（字节/秒），由 readBytes/writeBytes 差分而来。
    // 任务管理器的"磁盘"列显示的就是这个值的 MB/s。
    double diskBytesPerSec = 0.0;
    // 网络吞吐（比特/秒）。数据源是 ETW Kernel-Network，未接入时为 0。
    double netBitsPerSec = 0.0;
    // GPU 使用率百分比。数据源是 D3DKMT，未接入时为 0。
    double gpuPercent = 0.0;
    // GPU 专用内存字节数（"GPU 内存"列，本程序暂不显示但保留）
    uint64_t gpuDedicatedBytes = 0;

    // ---- 以下为懒加载字段（不在采集线程里取）----
    // 路径/命令行/图标/签名/账户名 全部由 LazyLoader 按需填充
    std::wstring imagePath;           // 懒加载: NtQuerySystemInformation(88) 或 QueryFullProcessImageName
    bool imagePathResolved = false;
};

// 一次完整快照。生成后即为不可变，UI 只读。
struct Snapshot {
    uint64_t sequence = 0;            // 单调递增代际号，UI 靠它判断要不要重绘
    LARGE_INTEGER timestamp{};        // QPC
    LARGE_INTEGER wallClock{};        // FILETIME 风格的墙钟，用于差分

    std::vector<ProcessEntry> processes;

    // 系统级
    uint32_t totalProcesses = 0;
    uint32_t totalThreads = 0;
    uint64_t totalHandles = 0;
    uint64_t totalPhysical = 0;
    uint64_t availablePhysical = 0;
    uint64_t commitTotal = 0;
    uint64_t commitLimit = 0;

    // 采集耗时（自监控，用于验证验收指标）
    double collectionMs = 0.0;
    uint32_t bufferBytes = 0;         // 本轮用的缓冲区大小，用于跨次复用
};

} // namespace tmp
