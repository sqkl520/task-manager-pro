// collector.cpp - 采集器实现
// 关键点：
//   - 直接使用 NtQuerySystemInformation(SystemProcessInformation=5)
//   - 绝不使用 PDH \Thread(*) / \Process(*) / WMI（实测灾难级）
//   - 缓冲区跨次复用，STATUS_INFO_LENGTH_MISMATCH 才扩容
#include "collector.h"

#include <windows.h>
#include <winternl.h>
#include <ntstatus.h>

#include <algorithm>
#include <cstdio>

#pragma comment(lib, "ntdll.lib")

namespace tmp {
namespace {

// 我们自己定义，避免依赖 phnt。字段顺序严格按 NT 内部结构。
typedef struct _TMP_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} TMP_UNICODE_STRING;

typedef struct _TMP_SYSTEM_THREAD_INFORMATION {
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER CreateTime;
    ULONG         WaitTime;
    PVOID         StartAddress;
    CLIENT_ID     ClientId;
    LONG          Priority;
    LONG          BasePriority;
    ULONG         ContextSwitches;
    ULONG         ThreadState;
    ULONG         WaitReason;
} TMP_SYSTEM_THREAD_INFORMATION;

// SystemProcessInformation 返回的每进程条目（按内存顺序）
typedef struct _TMP_SYSTEM_PROCESS_INFORMATION {
    ULONG          NextEntryOffset;
    ULONG          NumberOfThreads;
    LARGE_INTEGER  WorkingSetPrivateSize;
    ULONG          HardFaultCount;
    ULONG          NumberOfThreadsHighWatermark;
    ULONGLONG      CycleTime;
    LARGE_INTEGER  CreateTime;
    LARGE_INTEGER  UserTime;
    LARGE_INTEGER  KernelTime;
    TMP_UNICODE_STRING ImageName;
    LONG           BasePriority;
    HANDLE         UniqueProcessId;
    HANDLE         InheritedFromUniqueProcessId;
    ULONG          HandleCount;
    ULONG          SessionId;
    ULONG_PTR      UniqueProcessKey;
    SIZE_T         PeakVirtualSize;
    SIZE_T         VirtualSize;
    ULONG          PageFaultCount;
    SIZE_T         PeakWorkingSetSize;
    SIZE_T         WorkingSetSize;
    SIZE_T         QuotaPeakPagedPoolUsage;
    SIZE_T         QuotaPagedPoolUsage;
    SIZE_T         QuotaPeakNonPagedPoolUsage;
    SIZE_T         QuotaNonPagedPoolUsage;
    SIZE_T         PagefileUsage;
    SIZE_T         PeakPagefileUsage;
    SIZE_T         PrivatePageCount;
    LARGE_INTEGER  ReadOperationCount;
    LARGE_INTEGER  WriteOperationCount;
    LARGE_INTEGER  OtherOperationCount;
    LARGE_INTEGER  ReadTransferCount;
    LARGE_INTEGER  WriteTransferCount;
    LARGE_INTEGER  OtherTransferCount;
    TMP_SYSTEM_THREAD_INFORMATION Threads[1];
} TMP_SYSTEM_PROCESS_INFORMATION;

typedef NTSTATUS (NTAPI* NtQuerySystemInformation_t)(ULONG, PVOID, ULONG, PULONG);

NtQuerySystemInformation_t ResolveNtQSI() {
    static NtQuerySystemInformation_t fn = []() -> NtQuerySystemInformation_t {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) ntdll = LoadLibraryW(L"ntdll.dll");
        return reinterpret_cast<NtQuerySystemInformation_t>(
            GetProcAddress(ntdll, "NtQuerySystemInformation"));
    }();
    return fn;
}

constexpr ULONG SystemProcessInformationClass = 5;

// 系统内存信息（用 GlobalMemoryStatusEx + SystemPerformanceInformation 更省）
typedef struct _TMP_SYSTEM_PERFORMANCE_INFORMATION {
    LARGE_INTEGER IdleProcessTime;
    LARGE_INTEGER IoReadTransferCount;
    LARGE_INTEGER IoWriteTransferCount;
    LARGE_INTEGER IoOtherTransferCount;
    ULONG         IoReadOperationCount;
    ULONG         IoWriteOperationCount;
    ULONG         IoOtherOperationCount;
    ULONG         AvailablePages;
    ULONG         CommittedPages;
    ULONG         CommitLimit;
    ULONG         PeakCommitment;
    // ... 后续字段本次不需要
} TMP_SYSTEM_PERFORMANCE_INFORMATION;

} // namespace

Collector::Collector() = default;

Collector::~Collector() { Stop(); }

void Collector::SetInterval(uint32_t ms) {
    // 0 表示「已暂停」：存 0，采集线程的 waitMs 会是 0，
    // 于是不再自动采集，只有 RequestRefresh 才会采一次。
    intervalMs_.store(ms, std::memory_order_relaxed);
    // 立刻唤醒采集线程，让它按新间隔重新等待 ——
    // 否则从「低(4s)」切到「高(0.5s)」要等满 4 秒才生效。
    if (wakeEvent_) SetEvent(wakeEvent_);
}

void Collector::Start(uint32_t intervalMs) {
    if (running_.exchange(true)) return;
    intervalMs_.store(intervalMs ? intervalMs : 1000, std::memory_order_relaxed);
    if (!wakeEvent_) wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // 每进程网络需要 ETW Kernel-Network，这只在管理员下可用。
    // 系统自带任务管理器的清单写的是 highestAvailable + autoElevate，
    // 它也是靠"以管理员启动"换来这个数据源的。我们做同样的尝试：
    // 失败就静默降级，网络列留空，绝不因此拖慢或阻塞任何东西。
    if (netProc_.Init()) {
        netAvailable_ = true;
        netStatus_.clear();
    } else {
        netAvailable_ = false;
        DWORD e = netProc_.LastError();
        netStatus_ = (e == ERROR_ACCESS_DENIED)
            ? L"每进程网络需要管理员权限"
            : L"ETW 网络会话启动失败";
    }
    // 诊断：设置 TMP_NETDIAG=1 时把网络初始化结果写到 exe 同目录
    {
        wchar_t envbuf[8] = {};
        if (GetEnvironmentVariableW(L"TMP_NETDIAG", envbuf, 8) > 0 && envbuf[0] == L'1') {
            wchar_t p[MAX_PATH];
            GetModuleFileNameW(nullptr, p, MAX_PATH);
            std::wstring logp = p;
            size_t sl = logp.find_last_of(L"\\/");
            if (sl != std::wstring::npos) logp = logp.substr(0, sl);
            logp += L"\\netdiag.txt";
            FILE* f = nullptr;
            _wfopen_s(&f, logp.c_str(), L"w");
            if (f) {
                fwprintf(f, L"net init=%d err=%lu status=%s\n",
                         netAvailable_ ? 1 : 0,
                         (unsigned long)netProc_.LastError(),
                         netStatus_.c_str());
                fclose(f);
            }
        }
    }


    // 每进程 GPU：D3DKMT 未文档化导出，不需要权限，普通用户也能用。
    // 实测单轮 0.8-1.7 ms（复用 pid 列表时），代价可接受。
    gpuAvailable_ = gpuProc_.Init();

    thread_ = std::thread(&Collector::ThreadProc, this);
}
void Collector::Stop() {
    if (!running_.exchange(false)) return;
    if (wakeEvent_) SetEvent(wakeEvent_);
    if (thread_.joinable()) thread_.join();
    if (wakeEvent_) { CloseHandle(wakeEvent_); wakeEvent_ = nullptr; }
    // ETW 会话必须在采集线程停下之后再关，否则 ProcessTrace 可能还在收
    netProc_.Shutdown();
    netAvailable_ = false;
}

void Collector::RequestRefresh() {
    refreshRequested_.store(true, std::memory_order_release);
    if (wakeEvent_) SetEvent(wakeEvent_);
}

std::shared_ptr<const Snapshot> Collector::GetSnapshot() const {
    std::lock_guard<std::mutex> lk(publishMutex_);
    return published_;
}

void Collector::ThreadProc() {
    // 采集线程优先级略低于 normal，避免和前台抢 CPU
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    while (running_.load(std::memory_order_acquire)) {
        auto snap = std::make_shared<Snapshot>();
        if (CollectOnce(*snap)) {
            uint64_t seq;
            {
                std::lock_guard<std::mutex> lk(publishMutex_);
                // 上一份快照的序号 +1
                seq = (published_ ? published_->sequence : 0) + 1;
                snap->sequence = seq;
                published_ = std::move(snap);
            }
            publishedSequence_.store(seq, std::memory_order_release);
        }

        // 等待：要么到间隔，要么被 RequestRefresh/SetInterval/Stop 唤醒
        const bool forced = refreshRequested_.exchange(false);
        const uint32_t iv = intervalMs_.load(std::memory_order_relaxed);
        DWORD waitMs = forced ? 0 : iv;
        if (!running_.load(std::memory_order_acquire)) break;
        if (waitMs > 0 && wakeEvent_) {
            WaitForSingleObject(wakeEvent_, waitMs);
            refreshRequested_.store(false);
        } else if (waitMs == 0 && wakeEvent_) {
            // 已暂停：阻塞等待，直到有人 RequestRefresh / SetInterval / Stop。
            //
            // 【必须在这里再查一次 running_】Stop() 会 SetEvent 把我们从
            // INFINITE 里叫醒；但醒来后 intervalMs_ 仍然是 0，如果不检查
            // running_ 就又回到 INFINITE —— Stop() 里的 thread_.join()
            // 会永久卡死，程序退不掉。这是本轮改动引入的死锁。
            WaitForSingleObject(wakeEvent_, INFINITE);
            refreshRequested_.store(false);
            if (!running_.load(std::memory_order_acquire)) break;
        }
    }
}

bool Collector::CollectOnce(Snapshot& out) {
    auto fn = ResolveNtQSI();
    if (!fn) return false;

    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    QueryPerformanceFrequency(&freq);

    // ---- 1. 进程快照：单次调用拿全量 ----
    // 缓冲区跨次复用：初始 256KB，命中 LENGTH_MISMATCH 就按返回的 needed 扩容。
    if (buffer_.empty()) {
        bufferSize_ = 512 * 1024;
        buffer_.resize(bufferSize_);
    }

    NTSTATUS status = STATUS_INFO_LENGTH_MISMATCH;
    ULONG needed = 0;
    int attempts = 0;
    while (status == STATUS_INFO_LENGTH_MISMATCH && attempts < 8) {
        ++attempts;
        status = fn(SystemProcessInformationClass, buffer_.data(), bufferSize_, &needed);
        if (status == STATUS_INFO_LENGTH_MISMATCH) {
            // 留 25% 余量，减少后续扩容次数
            uint32_t newSize = needed ? needed + needed / 4 : bufferSize_ * 2;
            if (newSize < bufferSize_ * 2) newSize = bufferSize_ * 2;
            bufferSize_ = newSize;
            buffer_.resize(bufferSize_);
        }
    }
    if (!NT_SUCCESS(status)) return false;

    lastBufferBytes_.store(bufferSize_, std::memory_order_relaxed);

    // ---- 2. 墙钟（用于 CPU% 差分）----
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    LARGE_INTEGER nowWall;
    nowWall.LowPart = ft.dwLowDateTime;
    nowWall.HighPart = static_cast<LONG>(ft.dwHighDateTime);

    // ---- 3. 解析 ----
    out.processes.clear();
    out.processes.reserve(512);

    auto* base = reinterpret_cast<uint8_t*>(buffer_.data());
    auto* cur = reinterpret_cast<TMP_SYSTEM_PROCESS_INFORMATION*>(base);

    double deltaSeconds = 0.0;
    if (hasPrev_) {
        deltaSeconds = static_cast<double>(nowWall.QuadPart - prevWallClock_.QuadPart) / 1.0e7;
    }

    uint32_t totalThreads = 0;
    uint64_t totalHandles = 0;

    for (;;) {
        ProcessEntry e;
        e.pid = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(cur->UniqueProcessId));
        e.parentPid = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(cur->InheritedFromUniqueProcessId));
        e.threadCount = cur->NumberOfThreads;
        e.handleCount = cur->HandleCount;
        e.kernelTime = static_cast<uint64_t>(cur->KernelTime.QuadPart);
        e.userTime = static_cast<uint64_t>(cur->UserTime.QuadPart);
        e.createTime = cur->CreateTime;
        e.workingSet = cur->WorkingSetSize;
        e.privateBytes = static_cast<uint64_t>(cur->WorkingSetPrivateSize.QuadPart);
        e.virtualSize = cur->VirtualSize;
        e.pagefileUsage = cur->PagefileUsage;
        e.readBytes = static_cast<uint64_t>(cur->ReadTransferCount.QuadPart);
        e.writeBytes = static_cast<uint64_t>(cur->WriteTransferCount.QuadPart);
        e.sessionId = cur->SessionId;

        if (cur->ImageName.Buffer && cur->ImageName.Length > 0) {
            e.imageName.assign(cur->ImageName.Buffer,
                               cur->ImageName.Length / sizeof(wchar_t));
        } else {
            // Idle / System 等内核进程没有 ImageName
            e.imageName = (e.pid == 0) ? L"Idle" : L"System";
        }

        // 统计挂起线程数：ThreadState==Waiting(5) 且 WaitReason==Suspended(5)
        // 常量来自 KTHREAD_STATE / KWAIT_REASON。这不需要额外系统调用，
        // 线程数组本来就在快照里。
        {
            uint32_t susp = 0;
            const auto* th = cur->Threads;
            for (ULONG t = 0; t < cur->NumberOfThreads; ++t) {
                if (th[t].ThreadState == 5 && th[t].WaitReason == 5) ++susp;
            }
            e.suspendedThreadCount = susp;
        }

        totalThreads += e.threadCount;
        totalHandles += e.handleCount;

        out.processes.push_back(std::move(e));

        if (cur->NextEntryOffset == 0) break;
        cur = reinterpret_cast<TMP_SYSTEM_PROCESS_INFORMATION*>(
            reinterpret_cast<uint8_t*>(cur) + cur->NextEntryOffset);
    }

    // ---- 4. CPU% 差分 ----
    // 用 Windows 自身 Taskmgr 的算法：delta(processCpu) / delta(wall) / numCpus
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const double numCpus = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;

    if (hasPrev_ && deltaSeconds > 0.0 && !prevCpu_.empty()) {
        // 建 pid -> (kernel+user) 的上一轮表（用 unordered_map 会更慢，直接顺序匹配）
        for (auto& e : out.processes) {
            auto it = prevCpu_.find(e.pid);
            if (it != prevCpu_.end()) {
                uint64_t prev = it->second;
                uint64_t now = e.kernelTime + e.userTime;
                uint64_t delta = (now > prev) ? (now - prev) : 0;
                e.cpuCycleDelta = delta;
                // delta 是 100ns；百分比 = delta / (deltaSeconds * 1e7) / numCpus * 100
                e.cpuPercent = (static_cast<double>(delta) / (deltaSeconds * 1.0e7)) / numCpus * 100.0;
            }
        }
    }

    prevCpu_.clear();
    prevCpu_.reserve(out.processes.size() * 2);
    for (auto& e : out.processes) {
        prevCpu_[e.pid] = e.kernelTime + e.userTime;
    }
    prevWallClock_ = nowWall;
    hasPrev_ = true;

    // ---- 4b. 磁盘吞吐差分 ----
    // ReadTransferCount/WriteTransferCount 是进程生命周期内的累计字节数，
    // 差分一次就得到本轮的真实吞吐。零额外系统调用。
    //
    // 这里报告的是「逻辑 IO 吞吐」，不是「磁盘活动时间百分比」。
    // 原版任务管理器的磁盘列是后者（由 PDH PhysicalDisk 提供），
    // 但那需要每进程的 IO 栈深度采样，NtQuerySystemInformation 拿不到。
    // 本程序显示真实吞吐，单位 MB/s —— 数值本身是真的，语义略有不同。
    if (hasPrev_ && deltaSeconds > 0.0 && !prevIo_.empty()) {
        for (auto& e : out.processes) {
            auto it = prevIo_.find(e.pid);
            if (it == prevIo_.end()) continue;
            uint64_t now = e.readBytes + e.writeBytes;
            uint64_t prev = it->second;
            if (now > prev) {
                e.diskBytesPerSec = static_cast<double>(now - prev) / deltaSeconds;
            }
        }
    }
    prevIo_.clear();
    prevIo_.reserve(out.processes.size() * 2);
    for (const auto& e : out.processes) {
        prevIo_[e.pid] = e.readBytes + e.writeBytes;
    }

    // ---- 4c. 每进程网络 ----
    // netproc 内部自己做差分（Collect 返回"自上次调用以来"的 Mbps），
    // 所以这里只负责把结果按 pid 填进快照。
    //
    // 注意：这个 map 是 per-pid 的，进程退出后条目会自然消失；
    // 不需要额外清理。
    if (netAvailable_) {
        netRates_.clear();
        netProc_.Collect(netRates_);
        if (!netRates_.empty()) {
            for (auto& e : out.processes) {
                auto it = netRates_.find(e.pid);
                if (it != netRates_.end()) {
                    // Mbps → bps
                    e.netBitsPerSec = it->second * 1.0e6;
                }
            }
        }
    }

    // ---- 4d. 每进程 GPU ----
    // 复用本快照已枚举好的 pid 列表，避免 GpuProcCollector 内部再调一次
    // EnumProcesses（实测本机 600+ 进程要 11-57ms，会直接吃掉整个时间预算）。
    if (gpuAvailable_) {
        gpuPids_.clear();
        gpuPids_.reserve(out.processes.size());
        for (const auto& e : out.processes) {
            if (e.pid != 0) gpuPids_.push_back(e.pid);
        }
        gpuSamples_.clear();
        gpuProc_.Collect(gpuPids_.data(), gpuPids_.size(), gpuSamples_);
        if (!gpuSamples_.empty()) {
            // 建 pid -> 样本 的索引。样本数通常 < 100，直接用线性查找
            // 反而比哈希快（避免每轮重新分配哈希桶）。
            for (auto& e : out.processes) {
                for (const auto& g : gpuSamples_) {
                    if (g.pid == e.pid) {
                        e.gpuPercent = g.gpuPercent;
                        e.gpuDedicatedBytes = g.dedicatedBytes;
                        break;
                    }
                }
            }
        }
    }

    // ---- 5. 系统内存 ----
    MEMORYSTATUSEX memSt{};
    memSt.dwLength = sizeof(memSt);
    if (GlobalMemoryStatusEx(&memSt)) {
        out.totalPhysical = memSt.ullTotalPhys;
        out.availablePhysical = memSt.ullAvailPhys;
        out.commitTotal = memSt.ullTotalPageFile - memSt.ullAvailPageFile;
        out.commitLimit = memSt.ullTotalPageFile;
    }

    out.totalProcesses = static_cast<uint32_t>(out.processes.size());
    out.totalThreads = totalThreads;
    out.totalHandles = totalHandles;
    out.wallClock = nowWall;

    QueryPerformanceCounter(&t1);
    double elapsedMs = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 /
                       static_cast<double>(freq.QuadPart);
    out.collectionMs = elapsedMs;
    out.bufferBytes = bufferSize_;
    lastCollectionMs_.store(elapsedMs, std::memory_order_relaxed);

    return true;
}

} // namespace tmp
