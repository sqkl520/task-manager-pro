// iconcache.h - 进程图标缓存
//
// 为什么需要缓存：SHGetFileInfo 实测 6.1ms/次、最大 144ms。
// 450 个进程 = 2.7 秒。绝不能在绘制路径或采集路径调用。
// 策略：后台低优先级线程，按 exe 路径哈希缓存，绘制时只查表。
#pragma once

#include <windows.h>
#include <string>
#include <unordered_map>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>

namespace tmp {

class IconCache {
public:
    IconCache() = default;
    ~IconCache() { Stop(); }

    void Start();
    void Stop();

    // 请求某路径的图标（异步）。已缓存则立即可用。
    void RequestIcon(const std::wstring& exePath);

    // 取图标（没有则返回 nullptr）。绘制线程调用，无阻塞。
    HICON GetIcon(const std::wstring& exePath) const;

    size_t Pending() const;

private:
    void ThreadProc();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::wstring> queue_;
    std::unordered_map<std::wstring, HICON> cache_;
    std::unordered_map<std::wstring, bool> requested_;

    std::thread thread_;
    std::atomic<bool> running_{false};

    // 上限设大：图标是"一次性拉起"的工作（每个 exe 只提取一次），
    // 不是持续刷新的流。700 足够覆盖一台机器的全部进程。
    // 真正的背压由后台线程的低优先级保证，而不是靠丢弃请求。
    static constexpr size_t kMaxQueue = 700;

    // 图标缓存**哈希表**的条目上限（注意：不是 HICON 的回收上限）。
    //
    // 【必须说清它治什么、不治什么】
    //   治：cache_ 这张 unordered_map 不无限膨胀（每条是 wstring + HICON 指针）。
    //   不治：HICON 背后的 USER 对象。清空 map 只丢键值对，
    //         系统对象仍活着，只有 DestroyIcon 能释放 —— 而唯一的销毁点是
    //         Stop()（进程退出，先 join 线程再销毁，无竞态）。
    //
    // 为什么不在运行期 DestroyIcon：GetIcon() 锁内复制裸 HICON 就出锁，
    // UI 线程锁外 DrawIconEx，后台销毁全表 = use-after-free。
    //
    // 取 4096：本机同时可见的不同 exe 约一百多个，4096 给足余量，
    // 小时级运行到不了。这段实际几乎不触发，只防"跑几天"的极端情况。
    static constexpr size_t kMaxCache = 4096;
};

} // namespace tmp
