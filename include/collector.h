// collector.h - 快照采集器
// 核心设计（对应 DIAGNOSIS.md 铁律 1/2/7）：
//   1. 全程在独立线程运行，UI 线程永不调用这些 API
//   2. 单次 NtQuerySystemInformation(class 5) 拿全量，不做每进程 OpenProcess
//   3. 缓冲区大小跨次缓存，只在 STATUS_INFO_LENGTH_MISMATCH 时扩容
//   4. 不可变快照 + shared_ptr 原子发布，UI 侧零拷贝
#pragma once

#include "snapshot.h"
#include "netproc.h"
#include "gpuproc.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <string>

namespace tmp {

class Collector {
public:
    Collector();
    ~Collector();

    void Start(uint32_t intervalMs = 1000);
    void Stop();

    // 请求一次立即采集（插队，不等下一个 tick）
    void RequestRefresh();

    // 运行时改变采集间隔（对应「查看 → 更新速度」的 高/正常/低/已暂停）。
    // 【补漏】此前菜单里的 updateIntervalMs_ 只写进 TmWindow 自己的成员、
    // 从没传给采集器 —— 四个档位点了完全没效果。这里补上真正的通道：
    // 存原子值并唤醒采集线程的事件，让它立刻按新间隔重启等待。
    // ms == 0 表示「已暂停」：采集线程不再自动采集，只有 RequestRefresh 才采一次。
    void SetInterval(uint32_t ms);
    uint32_t Interval() const { return intervalMs_.load(std::memory_order_relaxed); }

    // 取当前已发布快照。返回 shared_ptr，UI 侧持有期间快照保证有效，
    // 且不产生任何深拷贝（这是相对拷贝版的关键改进）。
    std::shared_ptr<const Snapshot> GetSnapshot() const;

    double LastCollectionMs() const { return lastCollectionMs_.load(std::memory_order_relaxed); }
    uint32_t LastBufferBytes() const { return lastBufferBytes_.load(std::memory_order_relaxed); }
    uint64_t Sequence() const { return publishedSequence_.load(std::memory_order_acquire); }

    // 网络列是否可用。ETW Kernel-Network 需要管理员权限，
    // 普通用户下 Init 会失败，网络列保持空白（与系统任务管理器行为一致）。
    bool NetworkAvailable() const { return netAvailable_; }
    const std::wstring& NetworkStatus() const { return netStatus_; }

private:
    void ThreadProc();
    bool CollectOnce(Snapshot& out);

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> refreshRequested_{false};
    std::atomic<uint32_t> intervalMs_{1000};

    mutable std::mutex publishMutex_;
    std::shared_ptr<const Snapshot> published_;
    std::atomic<uint64_t> publishedSequence_{0};

    std::atomic<double> lastCollectionMs_{0.0};
    std::atomic<uint32_t> lastBufferBytes_{0};

    // 跨次复用的查询缓冲区（铁律 7）
    std::vector<uint8_t> buffer_;
    uint32_t bufferSize_ = 0;

    // CPU% 差分状态
    LARGE_INTEGER prevWallClock_{};
    bool hasPrev_ = false;
    std::unordered_map<uint32_t, uint64_t> prevCpu_;
    // 磁盘吞吐差分状态：pid -> 累计 (read+write) 字节
    std::unordered_map<uint32_t, uint64_t> prevIo_;

    // 每进程网络（ETW）。Init 失败时 netAvailable_ 为 false，Collect 变成空操作。
    NetProcCollector netProc_;
    bool netAvailable_ = false;
    std::wstring netStatus_;
    // 复用容器，避免每轮重新分配
    std::unordered_map<uint32_t, double> netRates_;

    // 每进程 GPU（D3DKMT 未文档化导出）。失败时静默降级。
    GpuProcCollector gpuProc_;
    bool gpuAvailable_ = false;
    std::vector<GpuProcSample> gpuSamples_;
    std::vector<uint32_t> gpuPids_;
    HANDLE wakeEvent_ = nullptr;
};

} // namespace tmp
