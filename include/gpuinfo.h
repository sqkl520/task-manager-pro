// gpuinfo.h - 系统级 GPU 信息采集（每引擎使用率 / 适配器静态信息）
//
// 设计依据：本项目 8 轮 D3DKMT 探测（探测源码未随仓库分发）的实测结论
// （Win10 Pro 22H2 22H2 + NVIDIA discrete GPU + Xeon 服务器级 CPU）。
//
// 与 gpuproc.h 的分工：
//   gpuproc.h  -> 每进程 GPU 使用率 / 每进程显存（进程列表页用）
//   gpuinfo.h  -> 系统级 GPU 页：每引擎使用率、总使用率、适配器属性、引擎曲线
// 两者互不依赖，可同时存在；本模块不引用 gpuproc 的任何符号。
//
// 数据源（全部实测可用，见 gpuinfo.cpp 文件头的实测数字）：
//   - gdi32.dll 未文档化 D3DKMT 导出（动态解析，不引入 import lib）
//   - 注册表 HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-...}
//   - 不使用 PDH（\GPU Engine 通配符实测 1378~2455 ms，见文件头）
//
// 用法：
//   tmp::GpuInfoCollector gpu;
//   if (gpu.Init()) {                 // Init ~5ms（含一次 pid 枚举）
//       gpu.Collect();                // 每秒一次，实测 ~1.0 ms
//       double total = gpu.TotalUtilPercent();
//       for (auto& e : gpu.Engines()) ...;
//   }
//   失败时 Init() 返回 false，之后所有 getter 返回空/0，绝不崩溃。
//
// 线程模型：单线程调用（与项目现有采集线程一致）。内部不加锁。
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace tmp {

// 单个 GPU 引擎（按 D3DKMT 引擎类型聚合后的结果，不是单个 node）。
// 本机实测 14 个 node 聚合为 6 类（3D / Copy / Video Decode / Video Encode /
// Overlay / Other）。
struct GpuEngineSample {
    // 显示名，如 L"3D" / L"Copy" / L"Video Decode" / L"Video Encode"。
    // 优先用驱动的 FriendlyName（本机只有 Security / VR 这类有值），
    // 为空时回退为标准引擎类型名。
    std::wstring name;

    // D3DKMT 的引擎类型数值（DXGK_ENGINE_TYPE）：
    //   0=Other 1=3D 2=VideoDecode 3=VideoEncode 4=VideoProcessing
    //   5=SceneAssembly 6=Copy 7=Overlay 8=Crypto
    int engineType = 0;

    // 使用率百分比（0..100）。
    // 口径：本轮所有进程在该引擎上的 RunningTime 增量之和 / 墙钟间隔。
    // 实测与 PDH \GPU Engine(*)\Utilization Percentage 按引擎类型聚合后的
    // 误差在 0.1~0.7 个百分点以内（见 gpuinfo.cpp 文件头对照表）。
    double utilPercent = 0.0;

    // 本轮该引擎累计运行时间增量（100ns 单位，跨该类型所有 node 求和）。
    // 供调用方自行换算其它口径；不需要可忽略。
    uint64_t runningDelta = 0;

    // 参与聚合的 node 个数（本机 Copy 有 8 个 node，3D 只有 1 个）。
    int nodeCount = 0;
};

// GPU 适配器信息。静态字段在 Init() 后有效，inUse 字段每轮 Collect() 更新。
struct GpuAdapterInfo {
    std::wstring name;               // "NVIDIA GeForce discrete GPU"

    std::wstring driverVersion;      // 注册表 DriverVersion，如 "32.0.16.1714"
    std::wstring driverDate;         // 归一化为 "YYYY-MM-DD"（注册表原值是 "M-D-YYYY"）

    uint64_t dedicatedTotal = 0;     // 专用显存总量（D3DKMT GETSEGMENTSIZE）
    uint64_t sharedTotal = 0;        // 共享内存总量（同上）
    uint64_t dedicatedInUse = 0;     // 当前专用显存占用（segment resident）
    uint64_t sharedInUse = 0;        // 当前共享显存占用（同上）

    // "为硬件预留内存"。
    // 实测推导：注册表 HardwareInformation.qwMemorySize - GETSEGMENTSIZE.Dedicated
    //   = 8589934592 - 8377073664 = 212860928 (203 MiB)。
    // 注意：D3DKMT 没有任何字段直接给出这个值，它是两个来源相减的推导量，
    //       若注册表键缺失或小于 Dedicated 则保持 0（宁可为 0 也不填假数）。
    uint64_t hwReserved = 0;

    // DirectX 版本，如 L"12.1"。**Init() 不会自动填充**（探测需 480~1000 ms，
    // 见 ProbeDirectXVersion）。未调用探测时为空字符串，UI 应显示 "—"。
    std::wstring directXVersion;

    // 物理位置，如 L"PCI 总线 4，设备 0，功能 0"（D3DKMT ADAPTERADDRESS，0.004 ms）。
    std::wstring physicalLocation;

    // 本轮有 GPU 记录的进程数（实测本机 39 个）。
    int processCount = 0;

    // ---- 扩展字段（原始数值，供 UI 自行格式化）----
    uint32_t pciBus = 0;
    uint32_t pciDevice = 0;
    uint32_t pciFunction = 0;
    uint32_t nodeCount = 0;          // 本机 14
    uint32_t segmentCount = 0;       // 本机 3

    // 本适配器的总使用率（= 它所有引擎里最忙的那个），0..100。
    // 与 GpuInfoCollector::TotalUtilPercent() 的多卡语义不同：后者是
    // **所有适配器**里的最大值，这里是单卡的值。
    double totalUtilPercent = 0.0;

    // 本适配器各引擎当前使用率（与本适配器自己的 Engines() 同序）。
    std::vector<GpuEngineSample> engines;
};

// 单个引擎的历史曲线。定长数组，避免每帧堆分配。
struct GpuEngineHistory {
    int engineType = 0;
    std::wstring name;
    // 按时间从旧到新排列的使用率百分比。长度 <= GpuInfoCollector::kHistoryLength。
    std::vector<double> util;
};

class GpuInfoCollector {
public:
    // 历史曲线保留的采样点数。1 Hz 采集约等于 2 分钟。
    static constexpr size_t kHistoryLength = 120;

    GpuInfoCollector();
    ~GpuInfoCollector();

    GpuInfoCollector(const GpuInfoCollector&) = delete;
    GpuInfoCollector& operator=(const GpuInfoCollector&) = delete;

    // 初始化：解析 D3DKMT 导出、选真实 GPU、读引擎元数据、读适配器静态信息、
    // 枚举一次进程并建立句柄缓存。
    // 实测开销 ~5 ms（EnumProcesses 占 37 ms 里的绝大部分，仅这一次）。
    // 任何一步失败都返回 false 并进入永久降级状态。
    bool Init();

    // 采集一轮。内部自行维护 pid 缓存（每 60 轮刷新一次，刷新轮只重建缓存
    // 与基线、不产出使用率，避免单次调用同时承担枚举 + 查询的开销）。
    // 实测稳态 ~1.0 ms。
    void Collect();

    // 可选：由调用方提供本轮 pid 列表（例如复用主快照里的 pid），
    // 从而完全省掉 EnumProcesses。这是唯一能保证每轮都稳定 < 2 ms 的路径。
    // 不调用则 Collect() 内部自行枚举。
    void Collect(const uint32_t* pids, size_t count);

    // 让下一次 Collect() 重新枚举 pid（新进程可见性）。
    void RefreshPids();

    // 探测 DirectX 版本（D3D11 feature level）。
    // **警告：实测 480~1000 ms**（瓶颈是加载 NVIDIA 用户态驱动）。
    // Init() 不会调用它；请在初始化线程或后台线程调用一次，结果写回
    // Adapter().directXVersion。失败返回空字符串。
    // 只用 LoadLibrary("d3d11.dll") + GetProcAddress，不引入链接依赖。
    std::wstring ProbeDirectXVersion();

    // 适配器信息（静态字段 Init 后有效；inUse/processCount 每轮更新）。
    // **返回第一个真实 GPU**，保持与单卡时代调用方的兼容。
    // 多卡机器请改用 Adapters()。
    const GpuAdapterInfo& Adapter() const { return adapters_.empty() ? emptyAdapter_ : adapters_[0]; }

    // 全部真实 GPU（按 D3DKMT 枚举顺序；Init 时过滤掉软件渲染器与纯显示适配器）。
    // 多卡机器（核显 + 独显）在这里会有 2 项。**不包含** ToDesk / GameViewer
    // 这类纯显示适配器 —— 判定依据见 gpuinfo.cpp 文件头第 11 条。
    const std::vector<GpuAdapterInfo>& Adapters() const { return adapters_; }

    // 总 GPU 使用率：**所有适配器**里最忙的引擎（不是相加 —— 利用率是时间片
    // 占比，多卡相加会算出 >100%）。
    double TotalUtilPercent() const { return totalUtil_; }

    // 各引擎当前使用率，按显示顺序（3D / Copy / Video Encode / Video Decode / ...）。
    // 多卡时是**所有适配器同名引擎的合并视图**（每个引擎类型取最忙的那张卡），
    // 供只认识单一引擎列表的旧调用方使用；逐卡明细在 Adapters()[i].engines。
    const std::vector<GpuEngineSample>& Engines() const { return engines_; }

    // 各引擎历史曲线（与本轮 Engines() 同序）。
    const std::vector<GpuEngineHistory>& History() const { return history_; }

    // 最近一次 Collect() 的墙钟耗时（毫秒），用于项目自监控。
    double LastCollectMs() const { return lastCollectMs_; }

    // Init() 是否成功。
    bool Available() const { return available_; }

    // 探到的适配器名（Init 成功后有效，日志用）。多卡时是第一张卡的名字。
    const wchar_t* AdapterName() const { return Adapter().name.c_str(); }

private:
    struct Impl;
    Impl* impl_ = nullptr;

    void CollectInternal(const uint32_t* pids, size_t count, double t0);
    void ResetHistory();
    // 把 adapters_ 里所有卡的同名引擎合并成 engines_（每种取最忙的那张卡）
    void BuildMergedEngines();

    // 多卡：每张真实 GPU 一项。单卡时代这里是单个 adapter_。
    std::vector<GpuAdapterInfo> adapters_;
    // Adapter() 在 adapters_ 为空时的返回对象（绝不返回野引用）
    GpuAdapterInfo emptyAdapter_;
    std::vector<GpuEngineSample> engines_;
    std::vector<GpuEngineHistory> history_;
    double totalUtil_ = 0.0;
    double lastCollectMs_ = 0.0;
    bool available_ = false;
};

} // namespace tmp
