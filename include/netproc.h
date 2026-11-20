// netproc.h - 每进程网络使用率采集（ETW Microsoft-Windows-Kernel-Network）
//
// ============================ 先读这段 ============================
// 本模块能工作，但有三个硬约束，全部在 docs/evidence/netprobe.cpp
// 实测得出（原始输出见 docs/evidence/netprobe_run.txt）：
//
// 1) 【需要管理员权限】普通用户下 StartTraceW 直接返回 ERROR_ACCESS_DENIED(5)，
//    没有任何回退办法。实测：受限令牌（Administrators 为 deny-only）跑同一份 probe，
//    StartTraceW ret=5。而 Windows 自带 taskmgr.exe 的清单里写的是
//        <requestedExecutionLevel level="highestAvailable"/>
//        <autoElevate>true</autoElevate>
//    —— 它就是靠"默认以管理员启动"换来这个数据源的。
//
// 2) 【单条流控路径的 CPU 开销不可忽略】纯流量窗口实测 1.70% 单核（17.0 ms/s CPU）。
//    空载（本机后台网络 ~120-2000 事件/秒）0.02% 单核（0.20 ms/s）。
//    也就是说空闲时几乎免费，但一旦有人跑满带宽就会明显起跳。
//    这是 20 逻辑核 Xeon 服务器级 CPU @3.0GHz 上按周期计数器（3GHz）折算的结果。
//
// 3) 【realtime consumer 必须独占一个线程】ProcessTrace() 是阻塞调用，
//    退出靠 ControlTraceW(STOP) 让它返回。所以本类自带一个阻塞线程。
//    会话名固定为 "TaskManagerPro-NetProc"：实测进程被强杀后 realtime 会话会
//    永久残留（以 PID 命名的话永远没人回收），固定名使得下次启动必定撞上
//    ERROR_ALREADY_EXISTS，Init() 正好借此显式回收。
//
// 结论性建议见 netproc.cpp 末尾的"选型结论"。
// =================================================================
//
// 数据语义（实测核对，误差 0.000000%）：
//   事件 10 = 接收，事件 11 = 发送；payload +0 是 PID，+4 是本条事件的字节数。
//   同一 PID 的多条事件累加即该进程在该时间窗内的字节数。
//   注意 payload 长度按事件 ID 变化：id=11 是 28 字节，id=10 是 36 字节，
//   两个都 >= 20，所以只读前 8 字节是安全的；但**不要**假设结构体总长固定。
#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace tmp {

class NetProcCollector {
public:
    NetProcCollector();
    ~NetProcCollector();

    // 启动 ETW 会话 + realtime consumer 线程。
    // 失败返回 false，lastError() 给具体 Win32 错误码（5 = 没管理员权限）。
    // 重复 Init() 是幂等的。
    bool Init();

    // 取"自上次 Collect 以来"的每进程速率（Mbps），pid -> Mbps。
    // 内部做差分和清零，所以两次调用之间不能隔太久（建议 1 秒一次）。
    // 非线程安全（调用方单线程持锁），内部只对 ETW 回调那份表加锁。
    void Collect(std::unordered_map<uint32_t, double>& outMbps);

    // 停会话 + 收线程。可安全重复调用。
    void Shutdown();

    bool IsRunning() const { return running_.load(std::memory_order_acquire); }

    // Init() 失败时的 Win32 错误码（0 表示成功）
    DWORD LastError() const { return lastError_.load(std::memory_order_relaxed); }

    // 上一次 Collect() 的耗时（毫秒），用于自监控
    double LastCollectMs() const { return lastCollectMs_.load(std::memory_order_relaxed); }

    // 自上次 Collect 以来收到的事件总数（诊断用）
    uint64_t LastEventCount() const { return lastEventCount_.load(std::memory_order_relaxed); }

private:
    struct Agg {
        uint64_t recvBytes = 0;
        uint64_t sendBytes = 0;
    };

    // ETW 回调。声明成 void* 而不是 PEVENT_RECORD，是为了让本头文件
    // 不依赖 <evntcons.h>（也就不会被 include 顺序坑到）。实参就是 PEVENT_RECORD。
    static void WINAPI EventCallback(void* eventRecord);
    void ConsumerThreadProc();
    ULONG StopSession();

    std::thread consumer_;
    std::atomic<bool> running_{false};
    std::atomic<DWORD> lastError_{0};
    std::atomic<double> lastCollectMs_{0.0};
    std::atomic<uint64_t> lastEventCount_{0};   // 上一轮 Collect 统计到的事件数
    std::atomic<uint64_t> totalEvents_{0};      // 单调递增总数，回调线程累加

    // ETW 句柄。用 uint64_t 而不是 TRACEHANDLE，避免在头文件里拖进 <evntrace.h>，
    // 也避免和现有 include 顺序打架。语义上就是 TRACEHANDLE。
    uint64_t session_ = 0;
    uint64_t consumerHandle_ = 0;
    std::atomic<DWORD> consumerThreadId_{0};

    // 会话名必须活到会话结束：OpenTraceW 的 LoggerName 指向它。
    wchar_t sessionName_[128] = {};

    // ETW 回调线程写、Collect 线程读，必须加锁
    mutable std::mutex aggMutex_;
    std::unordered_map<uint32_t, Agg> agg_;

    // Collect 侧复用，避免每轮重新分配
    std::unordered_map<uint32_t, Agg> deferred_;

    LARGE_INTEGER lastCollectQpc_{};
    bool hasLastCollect_ = false;
    uint64_t prevTotalEvents_ = 0;      // 上次 Collect 时的 totalEvents_ 快照
};

} // namespace tmp
