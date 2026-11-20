// iconcache.cpp
#include "iconcache.h"

#include <shlobj.h>
#include <shellapi.h>
#include <algorithm>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace tmp {

void IconCache::Start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&IconCache::ThreadProc, this);
}

void IconCache::Stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();

    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& kv : cache_) {
        if (kv.second) DestroyIcon(kv.second);
    }
    cache_.clear();
}

size_t IconCache::Pending() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return queue_.size();
}

HICON IconCache::GetIcon(const std::wstring& exePath) const {
    if (exePath.empty()) return nullptr;
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = cache_.find(exePath);
    return (it != cache_.end()) ? it->second : nullptr;
}

void IconCache::RequestIcon(const std::wstring& exePath) {
    if (exePath.empty()) return;
    std::lock_guard<std::mutex> lk(mutex_);
    if (cache_.find(exePath) != cache_.end()) return;       // 已有
    if (requested_.find(exePath) != requested_.end()) return; // 已排队
    if (queue_.size() >= kMaxQueue) {
        requested_.erase(queue_.front());
        queue_.pop_front();
    }
    requested_[exePath] = true;
    queue_.push_back(exePath);
    cv_.notify_one();
}

void IconCache::ThreadProc() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    SHFILEINFOW sfi{};

    for (;;) {
        std::wstring path;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [this] { return !running_.load() || !queue_.empty(); });
            if (!running_.load()) break;
            path = std::move(queue_.front());
            queue_.pop_front();
        }

        HICON icon = nullptr;
        // 注意：不能用 SHGFI_USEFILEATTRIBUTES —— 那个标志会让系统按扩展名
        // 返回通用图标，而不是读取 exe 内嵌的真实图标。
        if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi),
                           SHGFI_ICON | SHGFI_SMALLICON)) {
            icon = sfi.hIcon;
        }

        {
            std::lock_guard<std::mutex> lk(mutex_);
            requested_.erase(path);
            if (icon) {
                // 【这不是"回收"，是"上限保护" —— 措辞必须诚实】
                //
                // cache_ 是 unordered_map<wstring, HICON>。`clear()` 只丢掉
                // 键值对，**不会释放 HICON 背后的 USER 对象** —— 那是系统对象，
                // 只有 DestroyIcon 能释放。所以这段代码治的是"哈希表不涨"，
                // 不是"HICON 不涨"。
                //
                // 为什么不能在这里 DestroyIcon：GetIcon() 在锁内复制裸 HICON
                // 就解锁，UI 线程拿着它在锁外 DrawIconEx（tmwindow_list.cpp:1616）。
                // 后台线程这时销毁全表 = use-after-free。要安全销毁必须改成
                // CopyIcon 给调用方一份副本（每次绘制两次系统调用，太重），
                // 或者把绘制也挪进锁内（绘制最长几十毫秒，会阻塞后台线程）。
                // 两个代价都比"多留几百 KB 图标"高。
                //
                // 真正的回收点是 Stop()：那里先 join 线程、再 DestroyIcon，
                // 无竞态。进程退出时全部释放。
                //
                // 上限取 4096：本机同时可见的不同 exe 约一百多个，
                // 4096 给足余量；小时级运行也到不了。所以这段实际几乎不会触发，
                // 它只是防止"跑几天"这种极端情况把哈希表撑爆。
                if (cache_.size() >= kMaxCache && cache_.find(path) == cache_.end()) {
                    cache_.clear();
                }
                cache_[path] = icon;
            }
        }
    }

    CoUninitialize();
}

} // namespace tmp
