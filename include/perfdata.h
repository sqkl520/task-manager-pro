// perfdata.h - 性能页数据采集
//
// 全部数据源都是本机实测选出来的（见 docs/specs/perf_page_spec.md 第 8 节）：
//   NtQSI(8)  每核 CPU       0.10 ms   ← 一次拿 20 核，比 PDH 便宜 5 倍
//   NtQSI(2)  内存组成       0.003 ms
//   NtQSI(80) 备用列表       0.000 ms
//   NtQSI(3)  启动时间       0.000 ms
//   CallNtPowerInformation(11) CPU 频率  0.001 ms
//   PDH PhysicalDisk(*)      0.31 ms
//   PDH Network Interface(*) 2.17 ms   ← 最贵，但仍可接受
//   PDH GPU Engine(*)        1.41 ms   ← 623 实例也只要 1.4ms
//   合计 ≈ 4.6 ms，占 1Hz 预算 0.46%
//
// 禁止：PDH \Process(*)\IO Read Bytes/sec（18.75ms / 533 实例）
#pragma once

#include <windows.h>
#include <vector>
#include <string>
#include <cstdint>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "gpuinfo.h"

namespace tmp {

struct CpuCoreSample {
    double utilPercent = 0.0;
    uint64_t kernelTime = 0;
    uint64_t userTime = 0;
    uint64_t idleTime = 0;
};

struct MemBreakdown {
    uint64_t totalBytes = 0;        // 总物理内存
    uint64_t availableBytes = 0;    // 可用
    uint64_t cachedBytes = 0;       // 备用 + 已修改（"已缓存"）
    uint64_t standbyBytes = 0;      // 备用
    uint64_t modifiedBytes = 0;     // 已修改
    uint64_t freeBytes = 0;         // 空闲
    uint64_t committedBytes = 0;    // 已提交
    uint64_t commitLimitBytes = 0;  // 提交限制
    uint64_t pagedPoolBytes = 0;    // 分页缓冲池
    uint64_t nonPagedPoolBytes = 0; // 非分页缓冲池
    uint64_t inUseBytes = 0;        // 使用中 = total - available - cached
};

struct DiskSample {
    std::wstring name;
    double activePercent = 0.0;     // 活动时间 %
    double readMBps = 0.0;
    double writeMBps = 0.0;
    double avgResponseMs = 0.0;
};

struct NetSample {
    bool hasName = false;   // name 是否来自系统的友好名称（而非"网卡 N"占位）
    std::wstring name;
    double sendMbps = 0.0;
    double recvMbps = 0.0;
    uint64_t linkSpeedBps = 0;
};

struct GpuSample {
    std::wstring name;
    // "GPU 0"
    double utilPercent = 0.0;
    uint64_t dedicatedBytes = 0;
    uint64_t sharedBytes = 0;
};

struct PerfSnapshot {
    uint64_t sequence = 0;

    // CPU
    std::vector<CpuCoreSample> cores;
    double cpuTotalPercent = 0.0;
    double cpuKernelPercent = 0.0;   // 内核时间占比
    std::wstring cpuName;            // "Intel(R) Xeon(R) CPU @ 3.00GHz"
    uint32_t cpuBaseMhz = 0;
    uint32_t cpuCurrentMhz = 0;
    uint32_t physicalCores = 0;
    uint32_t logicalCores = 0;
    uint32_t sockets = 1;
    uint64_t l1CacheBytes = 0, l2CacheBytes = 0, l3CacheBytes = 0;
    bool virtualizationEnabled = false;
    uint64_t uptimeSeconds = 0;      // 系统运行时间

    // 内存
    MemBreakdown mem;

    // 磁盘 / 网络 / GPU
    std::vector<DiskSample> disks;
    std::vector<NetSample> nets;
    std::vector<GpuSample> gpus;

    // GPU 页专用（gpuinfo 走 D3DKMT，不是 PDH）
    double gpuTotalPercent = 0.0;
    std::vector<GpuEngineSample> gpuEngines;   // 3D / Copy / Video Encode / Video Decode ...
    GpuAdapterInfo gpuAdapter;                 // 驱动版本 / 显存 / 物理位置 ...（第一张卡）

    // 多卡：全部真实 GPU 的适配器信息（含各自的引擎明细与显存）。
    // gpuAdapter 保留为 gpus[0] 的兼容别名，旧调用点不用改。
    // 顺序与 gpus 一致（同一张卡在两边是同一下标）。
    std::vector<GpuAdapterInfo> gpuAdapters;

    // 磁盘/网络的聚合值（不走 PDH 通配符）
    double diskTotalMBps = 0.0;
    double netMaxMbps = 0.0;

    // 进程/线程/句柄（从主采集器复用）
    uint32_t totalProcesses = 0;
    uint32_t totalThreads = 0;
    uint64_t totalHandles = 0;

    double collectionMs = 0.0;
};

class PerfCollector {
public:
    PerfCollector();
    ~PerfCollector();

    void Start(uint32_t intervalMs = 1000);
    void Stop();
    void RequestRefresh();

    std::shared_ptr<const PerfSnapshot> GetSnapshot() const;
    uint64_t Sequence() const { return publishedSeq_.load(std::memory_order_acquire); }

    // 主采集器每轮把自己的进程/线程/句柄数塞进来（避免重复采集）
    void SetProcessCounts(uint32_t procs, uint32_t threads, uint64_t handles);

private:
    void ThreadProc();
    bool CollectOnce(PerfSnapshot& out);
    bool InitPdH();
    void ClosePdH();

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> refresh_{false};
    std::atomic<uint64_t> publishedSeq_{0};
    uint32_t intervalMs_ = 1000;
    HANDLE wakeEvent_ = nullptr;

    mutable std::mutex publishMutex_;
    std::shared_ptr<const PerfSnapshot> published_;

    // CPU 差分状态
    struct CoreTimes { uint64_t idle, kernel, user; };
    std::vector<CoreTimes> prevCores_;
    bool hasPrev_ = false;

    // 【PDH 句柄已全部移除 · 2026-11-21】
    // 原来这里有 diskQuery_ / netQuery_ / gpuQuery_ / gpuMemQuery_ 四个 void*：
    //   - diskQuery_ / netQuery_ 在 InitPdH 里建了 6 个 `(*)` 通配符计数器，
    //     但全项目零处 PdhCollectQueryData，从未读过一次数据
    //   - gpuQuery_ / gpuMemQuery_ 连赋值都没有过
    // 保留它们只会让人以为这里有一条 PDH 数据通路。真实路径：
    //   磁盘 NtQuerySystemInformation / 网络 ETW / GPU D3DKMT。

    // GPU 页专用：每引擎使用率 + 适配器静态信息。
    //
    // 为什么不用 PDH：`\GPU Engine(*)\Utilization Percentage` 在本机实测
    // **1378-2455 ms**（通配符要枚举 1220 个实例），完全不可接受。
    // gpuinfo 走 D3DKMT 的未文档化导出，实测稳态 ~1.0 ms，相差三个数量级。
    GpuInfoCollector gpuInfo_;
    bool gpuInfoAvailable_ = false;
    std::vector<uint32_t> gpuInfoPids_;   // 复用主快照的 pid，省掉 EnumProcesses

    // 磁盘/网络差分状态（NtQSI(2) + GetIfTable2，都避开 PDH 通配符）
    LARGE_INTEGER qpcFreq_{};
    LARGE_INTEGER prevDiskQpc_{};
    uint64_t prevDiskRead_ = 0;
    uint64_t prevDiskWrite_ = 0;
    bool diskSeeded_ = false;
    LARGE_INTEGER netPrevQpc_{};
    bool netIfTableSeeded_ = false;
    std::unordered_map<uint32_t, uint64_t> netPrevIn_;
    std::unordered_map<uint32_t, uint64_t> netPrevOut_;
    // 接口去重：累计收发签名 -> 选中的 dwIndex（用于同一网卡多 index 时择优）
    std::unordered_map<uint64_t, uint32_t> netSigToIndex_;
    // 接口索引 -> 友好名称（"WLAN 2" / "VMware Network Adapter VMnet1"…）。
    // 适配器名称不会变，所以只建一次。
    std::unordered_map<uint32_t, std::wstring> ifNameMap_;
    bool ifNameMapBuilt_ = false;
    void BuildIfNameMap();

    // 静态信息（只读一次）
    std::wstring cpuName_;
    uint32_t baseMhz_ = 0;
    uint32_t physCores_ = 0;
    uint32_t logCores_ = 0;
    uint32_t sockets_ = 1;
    uint64_t l1_ = 0, l2_ = 0, l3_ = 0;
    bool virtEnabled_ = false;

    std::atomic<uint32_t> procCounts_[3] = {};   // procs, threads, (handles 用下面)
    std::atomic<uint64_t> handleCount_{0};

    void ReadStaticCpuInfo();
};

} // namespace tmp
