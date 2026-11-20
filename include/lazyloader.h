// lazyloader.h - 昂贵列的按需加载器
//
// 对应 DIAGNOSIS.md 铁律 4：签名/图标/命令行/路径全部懒加载 + 持久缓存。
//
// 实测成本（本机 450 进程）：
//   逐进程 WinVerifyTrust   11.0 ms × 450 = 3.6-5.7 s   ← 绝不在刷新路径调用
//   逐进程 SHGetFileInfo     6.1 ms × 450 = 2.7 s        ← 同上
//   NtQSI(88) PID→路径       0.001 ms × 435 = 0.53 ms    ← 便宜，但仍放后台
//   NtQIP(60) 命令行         4.77 ms / 431 进程          ← 便宜，仍放后台
#pragma once

#include <windows.h>
#include <string>
#include <unordered_map>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>

namespace tmp {

class LazyLoader {
public:
    struct Request {
        uint32_t pid;
        std::wstring imageName;
        std::function<void(uint32_t pid, const std::wstring& path)> callback;
    };

    LazyLoader() = default;
    ~LazyLoader() { Stop(); }

    void Start();
    void Stop();

    // 请求某个 PID 的映像路径。若已缓存则立刻回调，否则入队。
    // 队列有界，超出时丢弃最老的请求（防堆积 —— 这是原版任务管理器的死因之一）。
    void RequestPath(uint32_t pid, const std::wstring& imageName,
                     std::function<void(uint32_t, const std::wstring&)> cb);

    bool TryGetCachedPath(uint32_t pid, std::wstring& out);

    size_t QueueDepth() const;

private:
    void ThreadProc();
    std::wstring ResolvePath(uint32_t pid);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Request> queue_;
    std::unordered_map<uint32_t, std::wstring> cache_;
    std::unordered_map<uint32_t, bool> requested_;

    std::thread thread_;
    std::atomic<bool> running_{false};

    static constexpr size_t kMaxQueue = 64;     // 有界队列，防堆积
};

} // namespace tmp
