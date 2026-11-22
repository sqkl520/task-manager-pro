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
#include <winioctl.h>   // DISK_PERFORMANCE / STORAGE_DEVICE_DESCRIPTOR / BusType*
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
    uint64_t totalBytes = 0;        // 总物理内存        ← GlobalMemoryStatusEx ullTotalPhys
    uint64_t availableBytes = 0;    // 可用              ← GlobalMemoryStatusEx ullAvailPhys
    // 「已缓存」= 备用 + 已修改。
    //
    // 【两项依据，分开写清，别把弱证据说成强证据】
    // 1. **本项目自己的设计意图**：本字段从引入起注释就是「备用 + 已修改」，
    //    而实现曾额外加了一项 −− 注释与实现不符，这里改的是实现，不是注释。
    // 2. **重叠是实测证实的**（scratch/c01/c01_mem.exe，判据不依赖 available 的
    //    内部拆分）：`available < ResidentSystemCachePage + standby + modified + free`
    //    实测成立（−0.73 GB），而 available 不可能同时容下四个互不重叠的子集 ⇒
    //    ResidentSystemCachePage 必然与其中至少一项重叠。所以三项相加是重复计数。
    //    （旧实现实测比两项之和多约 0.5~1.0 GB。）
    // 3. **「原版口径就是 standby+modified」这一条我拿不到直接证据**：
    //    docs/specs/O_strings.md 只给出了图例字符串（34354「已缓存」/33854「备用」），
    //    并没有给出原版的计算公式；A11 §2.5 的那句「原版口径=standby+modified」
    //    是推断，不是测量。所以这里**不引用它当依据**，只当"方向上不冲突"。
    uint64_t cachedBytes = 0;       // 备用 + 已修改（"已缓存"）
    uint64_t standbyBytes = 0;      // 备用
    uint64_t modifiedBytes = 0;     // 已修改
    uint64_t freeBytes = 0;         // 空闲
    uint64_t committedBytes = 0;    // 已提交
    uint64_t commitLimitBytes = 0;  // 提交限制
    uint64_t pagedPoolBytes = 0;    // 分页缓冲池
    uint64_t nonPagedPoolBytes = 0; // 非分页缓冲池
    // 使用中 = 总量 − 可用。**不减 cachedBytes**：available 里已经含了可回收的
    // standby 页，再减一次就是重复扣除。原版也是这个口径（A11 §2.3 实测原版
    // 29.2 + 2.0 = 31.2 GB）。所以恒等式 `inUse + available == total` 成立。
    uint64_t inUseBytes = 0;        // 使用中 = totalBytes - availableBytes
};

struct DiskSample {
    std::wstring name;              // "磁盘 0"
    std::wstring driveLetters;      // "(C: D:)" —— 含括号，无盘符时为空
    std::wstring mediaType;         // "SSD" / "HDD" / "USB" / "" 未知
    std::wstring busType;           // "NVMe" / "SATA" / "USB" / "" —— 仅磁盘页「类型:」用

    // 静态属性（枚举时采一次，不参与差分）
    uint64_t capacityBytes = 0;     // 容量
    uint64_t freeBytes = 0;         // 已格式化（可用）
    bool isSystemDisk = false;
    bool hasPageFile = false;

    double activePercent = 0.0;     // 活动时间 %
    double readMBps = 0.0;
    double writeMBps = 0.0;
    double avgResponseMs = 0.0;
    uint32_t queueDepth = 0;

    // 首轮没有基线时，上面四个差分字段不可定义。
    // 界面必须显示「—」而不是 0 —— 显示 0 等于谎报「磁盘刚才完全空闲」。
    bool hasBaseline = false;
    // 分母为 0（这一拍没发生任何 IO）时响应时间同样不可定义。
    bool responseDefined = false;
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

    // 进程/线程/句柄（从主采集器复用，见 PerfCollector::SetProcessCounts）
    uint32_t totalProcesses = 0;
    uint32_t totalThreads = 0;
    uint64_t totalHandles = 0;
    // 上面三个数**来自哪一代**主采集器快照（取值空间 = tmp::Snapshot::sequence）。
    // 0 = 从来没收到过计数（还没采到主快照，或调用方没传）。
    //
    // 【为什么需要它】主采集器与性能采集器是两个线程、两个独立代际号，性能线程
    // 读计数的那一刻，手里的计数可能是上一代甚至更早的。实测同周期 1Hz +
    // 250 ms 轮询下约 53% 的帧是上一代（docs/audit/A11 §3.3）。这里**不"等配对"**
    // ——等待会让性能页卡住；只把来源代际**如实记下来**，让 UI 自己判断。
    //
    // 【UI 侧怎么用】countsSequence 与 PerfSnapshot::sequence **不是同一个计数器**，
    // 不能互相比。正确比较对象是 UI 手里那份进程快照的代际号：
    //     lag = <当前显示的 Snapshot::sequence> - countsSequence;
    //     lag == 0  → 同一代，三个数可以直接显示；
    //     lag >= 1  → 落后 lag 代，必须显示「—」或加「上一代」标注；
    //     countsSequence == 0 或 totalProcesses == 0 → 显示「—」。
    // 绝对不要因为 lag != 0 就把这三个数当成本代数字显示。
    // 它会随计数一起被记进 PerfSnapshot::countsSequence，供 UI 判断是否为本代。
    uint64_t countsSequence = 0;

    // 【补充·本轮】上面那条接口约定的问题是：UI 手里只有 PerfSnapshot，
    // 没有主采集器的 Snapshot::sequence，两个独立计数器没法直接比。
    // 所以这里**同时给出 age** —— 这份计数从被写入到被读取，跨了性能线程几拍：
    //   countsAge == 0 → 本次采集刚拿到
    //   countsAge >= 1 → 已经落后，UI 应显示「—」而不是旧数字
    // age 由 PerfCollector 自己维护，UI 不需要知道主线程的序号。
    uint32_t countsAge = 0;

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

    // 主采集器每轮把自己的进程/线程/句柄数塞进来（避免重复采集）。
    // snapshotSeq = 这份计数所属的那份 tmp::Snapshot 的代际号（Snapshot::sequence）。
    // 它会随计数一起被记进 PerfSnapshot::countsSequence，供 UI 判断是否为本代。
    // 【必须传】传 0 表示"来源未知"，UI 侧会当作不可信 → 显示「—」。
    void SetProcessCounts(uint32_t procs, uint32_t threads, uint64_t handles,
                          uint64_t snapshotSeq);

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
    // 上一拍发布的总量。采样窗短于计数器节拍（15.625 ms）时没有核产生增量，
    // 此时沿用上一拍的值而不是发布 0 —— 发布 0 等于谎报「CPU 空闲」。
    double prevTotalPercent_ = 0.0;
    double prevTotalKernelPercent_ = 0.0;
    bool prevTotalValid_ = false;

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

    // ---- 磁盘：逐盘 IOCTL_DISK_PERFORMANCE ----
    struct DiskInfo {
        std::wstring devPath;        // \\.\PhysicalDriveN
        std::wstring name;           // 磁盘 N
        std::wstring driveLetters;   // (C: D:) 或空
        std::wstring mediaType;      // SSD / HDD / USB / 空
        std::wstring busType;        // NVMe / SATA / USB / 空
        uint64_t capacityBytes = 0;
        uint64_t freeBytes = 0;
        bool isSystemDisk = false;
        bool hasPageFile = false;
    };
    std::vector<DiskInfo> diskInfos_;
    bool diskEnumDone_ = false;
    std::unordered_map<std::wstring, DISK_PERFORMANCE> prevDiskPerf_;
    void EnumeratePhysicalDisks();
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

    // 进程/线程/句柄计数 + 所属快照代际号。
    //
    // 【为什么用互斥量而不是几个 atomic】这里要发布的是**四个值的整体**
    // （procs / threads / handles / seq）。用 4 个独立 atomic 时，写侧即使
    // 用 release 写 seq，读侧也无法排除"读到 seq=5、但 threads 已经是第 6 代
    // 写进去的值"的撕裂 —— acquire 只保证"看到 seq=5 就能看到 5 之前的写"，
    // 不阻止读者看到 5 之后的写。撕裂出来的组合恰好是本次要消灭的假配对，
    // 用 atomic 反而会把它藏起来。调用频率只有 4 Hz（UI 定时器）对 1 Hz 采集，
    // 一个短临界区的代价完全可以忽略。
    struct ProcCounts {
        uint32_t procs = 0;
        uint32_t threads = 0;
        uint64_t handles = 0;
        uint64_t seq = 0;        // 这份计数所属的 tmp::Snapshot::sequence；0 = 未喂过
        // 写入时刻性能线程已经跑过的拍数。CollectOnce 每轮 ++，读取时相减得到 age。
        uint64_t writtenAtTick = 0;
    };
    mutable std::mutex procCountsMutex_;
    ProcCounts procCounts_;
    // 性能线程的拍数。采集线程自增、UI 线程在 SetProcessCounts 里读，所以用 atomic。
    std::atomic<uint64_t> collectTick_{0};
    // 上一拍读取到的计数所属拍数（只在采集线程内读写，无需 atomic）
    uint64_t countsSeenAtTick_ = 0;

    void ReadStaticCpuInfo();
};

} // namespace tmp
