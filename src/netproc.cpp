// netproc.cpp - 每进程网络使用率采集实现（ETW realtime）
//
// 全部结论来自 docs/evidence/netprobe.cpp 的实机运行（Win10 19045 / 服务器级 CPU / 20 逻辑核）：
//
//   管理员启动：StartTraceW ret=0，会话建立成功
//   受限令牌  ：StartTraceW ret=5 (ERROR_ACCESS_DENIED)
//   事件 ID   ：10=接收，11=发送（另有 12/13/15/16/18/26/27/28/29/32/34/42/43/58/59 无关事件）
//   payload   ：+0 PID(ULONG)，+4 字节数(ULONG)；id=11 总长 28，id=10 总长 36
//   数值校验  ：子进程实发 67108864 字节 → ETW 汇总 67108864，误差 0.000000%
//   CPU       ：空载 0.02% 单核；64MB 双向流量期 1.70% 单核（17.0 ms/s）
//
// 本文件不引用任何现有头文件，也不修改任何现有文件。
#include "netproc.h"

#include <evntrace.h>
#include <evntcons.h>
#include <evntprov.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#pragma comment(lib, "advapi32.lib")   // StartTraceW / OpenTraceW / ProcessTrace / EnableTraceEx2

namespace tmp {

namespace {

// Microsoft-Windows-Kernel-Network 固定 GUID
const GUID kKernelNetworkProvider =
    {0x7dd42a49, 0x5329, 0x4832, {0x8d, 0xfd, 0x43, 0xd9, 0x79, 0x15, 0x3a, 0x88}};

// 会话名。**固定名**，不带 PID。
//
// 为什么不用 PID 命名：实测（探测源码未随仓库分发）进程被强杀后，以 PID 命名的
// realtime 会话会永久残留在内核里（logman 可见），而且没有任何进程会去回收它
// —— 因为下次运行的 PID 不是那个号，永远撞不上。反复崩溃就会攒一堆死会话，
// 每个都还占着缓冲区。
// 用固定名则下次启动必然撞上 ERROR_ALREADY_EXISTS，正好可以显式回收。
// 代价：同一用户同时跑两个实例时第二个会接管会话（对任务管理器这种单实例应用可接受）。
const wchar_t* const kSessionName = L"TaskManagerPro-NetProc";

constexpr uint16_t kEventIdRecv = 10;
constexpr uint16_t kEventIdSend = 11;

// 只读前 8 字节（PID + 字节数），所以最小长度按 8 校验即可。
// 实测完整长度：id=11 → 28 字节，id=10 → 36 字节，两者都满足。
constexpr ULONG kMinPayload = 8;

constexpr uint32_t kBufferSizeKb    = 64;
constexpr uint32_t kMinBuffers      = 8;
constexpr uint32_t kMaxBuffers      = 32;
// 1 秒刷一次缓冲区：本模块就是 1Hz 取样。给 0 会等到缓冲区写满才投递，
// 低流量时会长时间收不到事件，差分窗口就对不上。
constexpr uint32_t kFlushTimerSec   = 1;

LARGE_INTEGER QpcNow() {
    LARGE_INTEGER v{};
    QueryPerformanceCounter(&v);
    return v;
}

double QpcFreq() {
    static const double f = [] {
        LARGE_INTEGER v{};
        QueryPerformanceFrequency(&v);
        return double(v.QuadPart ? v.QuadPart : 1);
    }();
    return f;
}

} // namespace

// ------------------------------------------------------------------ 构造/析构

NetProcCollector::NetProcCollector() = default;

NetProcCollector::~NetProcCollector() {
    Shutdown();
}

// ------------------------------------------------------------------ ETW 回调
//
// 在 consumer 线程里被 ProcessTrace 同步调用。必须极便宜：
// 实测这条路径跑 1.7% 单核 @ 流量高峰，其中绝大部分就是这个回调 + 内核投递。
// 所以这里只做：读两个 DWORD、加锁、加两次。
VOID WINAPI NetProcCollector::EventCallback(void* eventRecord) {
    // 注意：签名里用的是 void*，但 ETW 实际传进来的就是 PEVENT_RECORD。
    // 这样做是为了让 netproc.h 不必包含 <evntcons.h>。
    auto* rec = static_cast<PEVENT_RECORD>(eventRecord);
    if (!rec || !rec->UserContext) return;
    auto* self = static_cast<NetProcCollector*>(rec->UserContext);

    const uint16_t id = rec->EventHeader.EventDescriptor.Id;
    const bool isRecv = (id == kEventIdRecv);
    if (!isRecv && id != kEventIdSend) return;      // 其它事件直接丢

    if (!rec->UserData || rec->UserDataLength < kMinPayload) return;

    const uint8_t* p = static_cast<const uint8_t*>(rec->UserData);
    uint32_t pid = 0, bytes = 0;
    // 用 memcpy 而不是强转，避免 UB / 非对齐访问（x64 上这会被编译成单条 mov）
    std::memcpy(&pid, p + 0, sizeof(pid));
    std::memcpy(&bytes, p + 4, sizeof(bytes));

    {
        std::lock_guard<std::mutex> lk(self->aggMutex_);
        Agg& a = self->agg_[pid];
        if (isRecv) a.recvBytes += bytes;
        else        a.sendBytes += bytes;
    }
    self->totalEvents_.fetch_add(1, std::memory_order_relaxed);
}

// ------------------------------------------------------------------ Init

bool NetProcCollector::Init() {
    if (running_.load(std::memory_order_acquire)) return true;

    lastError_.store(0, std::memory_order_relaxed);

    // 会话名固定。存在成员里而不是栈上 —— OpenTraceW 的 LoggerName 会持有这个指针，
    // 必须活到 Shutdown()。
    wcscpy_s(sessionName_, kSessionName);
    const wchar_t* sessionName = sessionName_;

    const ULONG nameBytes = static_cast<ULONG>((wcslen(sessionName) + 1) * sizeof(wchar_t));
    const ULONG propsSize = sizeof(EVENT_TRACE_PROPERTIES) + nameBytes;

    // 每次 StartTrace / ControlTrace 都要一份独立的 EVENT_TRACE_PROPERTIES，
    // 上一个调用会把这块内存改写。
    auto makeProps = [&](std::vector<uint8_t>& buf) -> EVENT_TRACE_PROPERTIES* {
        buf.assign(propsSize, 0);
        auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
        p->Wnode.BufferSize    = propsSize;
        p->Wnode.Flags         = WNODE_FLAG_TRACED_GUID;
        p->Wnode.ClientContext = 1;                    // 1 = QPC 时间戳
        p->LogFileMode         = EVENT_TRACE_REAL_TIME_MODE;
        p->BufferSize          = kBufferSizeKb;
        p->MinimumBuffers      = kMinBuffers;
        p->MaximumBuffers      = kMaxBuffers;
        p->FlushTimer          = kFlushTimerSec;
        p->LoggerNameOffset    = sizeof(EVENT_TRACE_PROPERTIES);
        return p;
    };

    auto stopByName = [&](const wchar_t* name) -> ULONG {
        std::vector<uint8_t> b;
        auto* p = makeProps(b);
        // BufferSize / Buffers 这些在 STOP 时会被忽略，但结构体必须合法
        return ControlTraceW(0, name, p, EVENT_TRACE_CONTROL_STOP);
    };

    std::vector<uint8_t> pbuf;
    auto* props = makeProps(pbuf);

    TRACEHANDLE session = 0;
    ULONG rc = StartTraceW(&session, sessionName, props);

    // 固定会话名 → 上次进程被强杀留下的残留会话必然在此撞上。
    // 实测（探测源码未随仓库分发）：realtime 会话在拥有者进程被 TerminateProcess
    // 之后会一直留着，必须由下一个进程显式 STOP 才能回收。这正是用固定名的理由。
    if (rc == ERROR_ALREADY_EXISTS) {
        stopByName(sessionName);
        // STOP 是异步的，内核需要一点时间真正销毁会话；不退让就重试会继续拿 183。
        // 每次重试都重新 makeProps()，因为 StartTraceW 会把结果写回这块内存，
        // 复用上一轮被写脏的结构体会行为未定义。
        for (int i = 0; i < 50; ++i) {
            Sleep(20);
            props = makeProps(pbuf);
            rc = StartTraceW(&session, sessionName, props);
            if (rc != ERROR_ALREADY_EXISTS) break;
        }
    }

    if (rc != ERROR_SUCCESS) {
        lastError_.store(rc, std::memory_order_relaxed);
        return false;
    }

    // 打开 realtime consumer。必须在 EnableTraceEx2 之前打开，
    // 这样会话缓冲区里已有的数据也会被消费到。
    EVENT_TRACE_LOGFILEW lf{};
    lf.LoggerName          = sessionName_;   // 成员，生命周期覆盖整个会话
    lf.ProcessTraceMode    = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    lf.EventRecordCallback = reinterpret_cast<PEVENT_RECORD_CALLBACK>(
        &NetProcCollector::EventCallback);
    lf.Context             = this;

    TRACEHANDLE consumer = OpenTraceW(&lf);
    if (consumer == INVALID_PROCESSTRACE_HANDLE) {
        DWORD gle = GetLastError();
        stopByName(sessionName);
        lastError_.store(gle, std::memory_order_relaxed);
        return false;
    }

    // 订阅 Kernel-Network，level = Information(4)，关键字 0 = 全收
    ULONG erc = EnableTraceEx2(session, &kKernelNetworkProvider,
                               EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                               TRACE_LEVEL_INFORMATION, 0, 0, 0, nullptr);
    if (erc != ERROR_SUCCESS) {
        // 用 Verbose 再试一次（个别系统上 Information 级别过滤掉了 10/11）
        erc = EnableTraceEx2(session, &kKernelNetworkProvider,
                             EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                             TRACE_LEVEL_VERBOSE, 0, 0, 0, nullptr);
    }
    if (erc != ERROR_SUCCESS) {
        ControlTraceW(session, sessionName, nullptr, EVENT_TRACE_CONTROL_STOP);
        CloseTrace(consumer);
        lastError_.store(erc, std::memory_order_relaxed);
        return false;
    }

    session_        = session;
    consumerHandle_ = consumer;

    {
        std::lock_guard<std::mutex> lk(aggMutex_);
        agg_.clear();
        deferred_.clear();
    }
    hasLastCollect_   = false;
    prevTotalEvents_  = totalEvents_.load(std::memory_order_relaxed);
    lastEventCount_.store(0, std::memory_order_relaxed);
    lastCollectQpc_   = QpcNow();

    running_.store(true, std::memory_order_release);
    consumer_ = std::thread(&NetProcCollector::ConsumerThreadProc, this);

    return true;
}

// ------------------------------------------------------------------ consumer 线程
//
// ProcessTrace() 阻塞到会话被 Stop 或 CloseTrace 为止。
// 这是全模块唯一一个"常驻"线程，也是 1.7% 单核开销的来源。
void NetProcCollector::ConsumerThreadProc() {
    consumerThreadId_.store(GetCurrentThreadId(), std::memory_order_release);
    TRACEHANDLE h = consumerHandle_;
    ProcessTrace(&h, 1, nullptr, nullptr);   // 阻塞
    consumerThreadId_.store(0, std::memory_order_release);
}

// ------------------------------------------------------------------ 停会话

ULONG NetProcCollector::StopSession() {
    // 会话名是成员 sessionName_，Init() 之后一直有效
    if (!sessionName_[0]) return ERROR_INVALID_PARAMETER;

    const ULONG nameBytes = static_cast<ULONG>((wcslen(sessionName_) + 1) * sizeof(wchar_t));
    const ULONG propsSize = sizeof(EVENT_TRACE_PROPERTIES) + nameBytes;

    std::vector<uint8_t> b(propsSize, 0);
    auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(b.data());
    p->Wnode.BufferSize = propsSize;
    p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

    return ControlTraceW(0, sessionName_, p, EVENT_TRACE_CONTROL_STOP);
}

void NetProcCollector::Shutdown() {
    if (!running_.load(std::memory_order_acquire)) {
        // 走到这里可能是 Init() 半途失败，仍然尝试清理残留 handle
        if (consumerHandle_ && consumerHandle_ != INVALID_PROCESSTRACE_HANDLE) {
            CloseTrace(consumerHandle_);
            consumerHandle_ = 0;
        }
        session_ = 0;
        return;
    }

    running_.store(false, std::memory_order_release);

    // STOP 会让阻塞中的 ProcessTrace 返回。实测返回 0，通常 < 1ms。
    StopSession();

    if (consumer_.joinable()) {
        // 给 consumer 线程最多 2 秒退出，正常远小于此
        for (int i = 0; i < 200; ++i) {
            if (consumerThreadId_.load(std::memory_order_acquire) == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        consumer_.join();
    }

    if (consumerHandle_ && consumerHandle_ != INVALID_PROCESSTRACE_HANDLE) {
        CloseTrace(consumerHandle_);
    }
    consumerHandle_ = 0;
    session_ = 0;

    std::lock_guard<std::mutex> lk(aggMutex_);
    agg_.clear();
    deferred_.clear();
    hasLastCollect_ = false;
}

// ------------------------------------------------------------------ Collect

void NetProcCollector::Collect(std::unordered_map<uint32_t, double>& outMbps) {
    const auto t0 = std::chrono::steady_clock::now();

    outMbps.clear();

    LARGE_INTEGER now = QpcNow();
    // 第一轮没有基准，只能建基准、返回空表。
    // （否则第一个窗口会把自 Init 以来的全部字节算成"1 秒内"，速率虚高几十倍。）
    const bool first = !hasLastCollect_;
    double elapsedSec = 0.0;
    if (!first) {
        elapsedSec = double(now.QuadPart - lastCollectQpc_.QuadPart) / QpcFreq();
        if (elapsedSec < 0.05) {
            // 间隔太短，差分没有意义；沿用上一轮结果不动
            lastCollectMs_.store(
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count(),
                std::memory_order_relaxed);
            return;
        }
    }
    lastCollectQpc_  = now;
    hasLastCollect_  = true;

    // totalEvents_ 是单调递增的，这里取"本轮增量"给 LastEventCount() 查询。
    // （不要用清零再累加，那样和回调线程有竞态，会丢事件。）
    const uint64_t evBefore = prevTotalEvents_;
    prevTotalEvents_ = totalEvents_.load(std::memory_order_relaxed);

    if (first) {
        // 清掉 Init 之后到第一次 Collect 之间攒下的字节，避免算进第一个窗口
        std::lock_guard<std::mutex> lk(aggMutex_);
        agg_.clear();
        deferred_.clear();
        lastEventCount_.store(0, std::memory_order_relaxed);
        lastCollectMs_.store(
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count(),
            std::memory_order_relaxed);
        return;
    }

    lastEventCount_.store(prevTotalEvents_ - evBefore, std::memory_order_relaxed);

    // 在锁内只做"整体换出"，把耗时工作（换算）留到锁外做。
    {
        std::lock_guard<std::mutex> lk(aggMutex_);
        agg_.swap(deferred_);
        agg_.clear();          // agg_ 现在是 deferred_ 的旧内容，清空复用
        if (agg_.bucket_count() == 0) agg_.reserve(deferred_.size() * 2 + 16);
    }

    const double kBytesToMbps = 8.0 / 1e6 / elapsedSec;   // 字节 → 兆比特/秒

    outMbps.reserve(deferred_.size());
    for (const auto& kv : deferred_) {
        const uint64_t total = kv.second.recvBytes + kv.second.sendBytes;
        if (total == 0) continue;                         // 只有 0 字节事件的进程不报
        outMbps.emplace(kv.first, double(total) * kBytesToMbps);
    }

    deferred_.clear();

    lastCollectMs_.store(
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count(),
        std::memory_order_relaxed);
}

} // namespace tmp

// ==================================================================
// 选型结论（实测支撑，供决策用）
// ==================================================================
//
// 路径 1 ETW Kernel-Network —— 【唯一可用】数值准确到 0 误差。
//   代价：需管理员；流量高峰 1.70% 单核（17 ms/s CPU）；独占一个阻塞线程。
//   空载 0.02% 单核（0.2 ms/s），对 1Hz 采集基本免费。
//
// 路径 2 GetPerTcpConnectionEStats —— 【不可用】
//   实测 SetPerTcpConnectionEStats 大多数运行返回 1168 (ERROR_NOT_FOUND)；
//   少数运行返回 0，但随后 DataBytesOut=0 而连接上实际发了 500 字节
//   —— 统计打开后不回溯已发生的流量，对已建立的连接不可靠。
//   更致命的是它按"连接"而非"进程"给数据：TCP 长连接 + UDP 都要自己聚合，
//   每条连接都要各调一次 Set/Get，连接数上百时成本远超 ETW。
//   另外非管理员下 SetPerTcpConnectionEStats 直接返回 5 (ERROR_ACCESS_DENIED)。
//
// 路径 3 NtDeviceIoControlFile / NSI —— 【不可行】
//   winnsi.dll 只导出 15 个 NsiRpc* 函数，全部走 RPC 到内核 NSI 服务，
//   没有公开的每进程字节计数器；taskmgr 导入了 winnsi.dll 但未导入任何
//   StartTrace/Tdh* 符号，说明它拿网络列走的是别的私有通道（很可能就是 SRUM
//   或内部 provider）。没有文档、随版本变，不碰。
//
// 路径 4 GetIpStatisticsEx / GetTcpStatisticsEx —— 【只有系统级】
//   本模块的 probe 用它做交叉校验：整机收包数增量与 ETW 汇总同向增加，
//   但拿不到 PID 维度。不能用于每进程列。
//
// 因此：本模块是"能用的前提下唯一可行的实现"，但它把程序钉死在
// "必须以管理员运行"这个前提上。是否接入由上层决定：
//   - 若接受管理员运行（与 Windows 自带 taskmgr 的 highestAvailable+autoElevate 一致），
//     直接 Init() 即可，1Hz 空载成本可忽略。
//   - 若坚持普通用户运行，网络列只能留空 —— 这与 Windows 自身在非提权下的行为一致。
// ==================================================================
