// gpuproc.h - 每进程 GPU 使用率采集（D3DKMT 内核统计）
//
// 设计依据：本项目 D3DKMT 探测（探测源码未随仓库分发）的实测结论
// （Win10 Pro 22H2 22H2 + discrete GPU + Xeon 服务器级 CPU）。
// 只使用 gdi32.dll 里未文档化的 D3DKMT 导出，不链接任何额外库、不依赖 PDH。
//
// 用法（推荐：直接复用主快照的 pid 列表）：
//   tmp::GpuProcCollector gpu;
//   if (gpu.Init()) {                     // 失败也只是降级，不影响调用方
//       std::vector<tmp::GpuProcSample> v;
//       gpu.Collect(pids, n, v);          // 每秒调用一次，稳定 < 2ms
//   }
//
// 线程模型：Collect() 只应由单一线程调用（与现有 Collector 采集线程一致）。
//           不使用锁；若要多线程访问请由调用方串行化。
#pragma once

#include <windows.h>
#include <cstdint>
#include <vector>

namespace tmp {

// 单进程的一次 GPU 采样。POD，无堆分配。
struct GpuProcSample {
    uint32_t pid = 0;

    // GPU 使用率百分比（0..100）。
    // 口径与 Windows 任务管理器一致：取该进程所有 GPU 引擎里最忙的那一个引擎。
    // 由两次采样差分得到，所以对一个进程的第一次采样是 0。
    double gpuPercent = 0.0;

    // 专用 GPU 内存（显存）。等价于 PDH \GPU Process Memory(*)\Local Usage。
    // 实测与 PDH 逐进程字节级精确相等（38/39 个进程；唯一不等的是正在剧烈
    // 变化的 dwm.exe，属于两次读取之间的瞬时差异）。
    uint64_t dedicatedBytes = 0;

    // 共享 GPU 内存（系统内存里的 GPU 资源）。
    // 等价于 PDH \GPU Process Memory(*)\Shared Usage，同样实测逐进程精确相等。
    uint64_t sharedBytes = 0;

    // 本轮时钟周期内累计的 GPU 运行时间（100ns 单位，跨所有引擎求和）。
    // 供调用方自行做其它口径的百分比或历史曲线，不需要可忽略。
    uint64_t gpuRunningDelta = 0;
};

class GpuProcCollector {
public:
    GpuProcCollector();
    ~GpuProcCollector();

    GpuProcCollector(const GpuProcCollector&) = delete;
    GpuProcCollector& operator=(const GpuProcCollector&) = delete;

    // 首次初始化：加载 gdi32 的 D3DKMT 导出、枚举适配器、挑出真实 GPU。
    // 任何一步失败都返回 false 并进入永久降级状态（后续 Collect 填空，绝不崩溃）。
    bool Init();

    // 采集一轮。out 会被清空后填充，只包含本轮确实能拿到 GPU 数据的进程。
    // 未 Init 或 Init 失败时 out 为空。可安全重复调用。
    //
    // 注意：这条路径内部要自己枚举 pid，而 EnumProcesses 在本机 600+ 进程时
    // 需要 11~57 ms（内核重建进程快照，约 1.2 MB）。因此 pid 列表被缓存在
    // 采集器内部，每 10 次调用才刷新一次；即便如此，刷新那一轮仍可能很慢。
    // 若调用方已有 pid 列表（主快照里就有），请用下面的 Collect(pids,...) 重载。
    void Collect(std::vector<GpuProcSample>& out);

    // 可选：由调用方提供本轮 pid 列表（例如复用已有进程快照里的 pid），
    // 从而省掉一次 EnumProcesses。传入的容器在调用后不变。
    // 不调用则 Collect() 内部自行枚举。
    //
    // 实测：本机 600+ 进程时 EnumProcesses 本身要 11~57 ms，而 GPU 查询只要
    // 0.9~1.7 ms。所以要让整体满足 15 ms 预算，强烈建议走这个重载，
    // 把采集器已经在用的 pid 列表直接传进来。
    void Collect(const uint32_t* pids, size_t count, std::vector<GpuProcSample>& out);

    // 让下一次 Collect(out) 重新枚举 pid。默认每 10 次 Collect(out) 自动做一次。
    void RefreshPids();

    // 最近一次 Collect() 的耗时（毫秒）。用于项目自监控。
    double LastCollectMs() const { return lastCollectMs_; }

    // Init() 是否成功（是否可用）。
    bool Available() const { return available_; }

    // 采到的 GPU 适配器名（Init 成功后有效，仅用于日志/UI 展示）。
    const wchar_t* AdapterName() const { return adapterName_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;

    // 两轮采集共用的实现：pids 已经由调用方或 EnumProcesses 准备好。
    void CollectFromPids(const uint32_t* pids, size_t count,
                         std::vector<GpuProcSample>& out, double t0);

    bool available_ = false;
    double lastCollectMs_ = 0.0;
    wchar_t adapterName_[128] = L"";
};

} // namespace tmp
