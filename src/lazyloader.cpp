// lazyloader.cpp - 懒加载实现
#include "lazyloader.h"

#include <winternl.h>
#include <ntstatus.h>

#pragma comment(lib, "ntdll.lib")

namespace tmp {

namespace {

typedef NTSTATUS (NTAPI* NtQuerySystemInformation_t)(ULONG, PVOID, ULONG, PULONG);

NtQuerySystemInformation_t GetNtQSI() {
    static auto fn = []() -> NtQuerySystemInformation_t {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) ntdll = LoadLibraryW(L"ntdll.dll");
        return reinterpret_cast<NtQuerySystemInformation_t>(
            GetProcAddress(ntdll, "NtQuerySystemInformation"));
    }();
    return fn;
}

constexpr ULONG SystemProcessIdInformationClass = 88;

// class 88 的输入/输出结构
typedef struct _TMP_SYSTEM_PROCESS_ID_INFORMATION {
    HANDLE         ProcessId;
    UNICODE_STRING ImageName;
} TMP_SYSTEM_PROCESS_ID_INFORMATION;

} // namespace

void LazyLoader::Start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&LazyLoader::ThreadProc, this);
}

void LazyLoader::Stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

size_t LazyLoader::QueueDepth() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return queue_.size();
}

bool LazyLoader::TryGetCachedPath(uint32_t pid, std::wstring& out) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = cache_.find(pid);
    if (it == cache_.end()) return false;
    out = it->second;
    return true;
}

void LazyLoader::RequestPath(uint32_t pid, const std::wstring& imageName,
                             std::function<void(uint32_t, const std::wstring&)> cb) {
    std::lock_guard<std::mutex> lk(mutex_);

    // 已缓存 → 立刻回调，不进队列
    auto it = cache_.find(pid);
    if (it != cache_.end()) {
        if (cb) cb(pid, it->second);
        return;
    }

    // 已在队列中 → 不重复入队（避免每轮刷新都堆一遍）
    if (requested_.find(pid) != requested_.end()) return;

    // 有界队列：满了就丢最老的（背压，不让请求堆叠）
    if (queue_.size() >= kMaxQueue) {
        requested_.erase(queue_.front().pid);
        queue_.pop_front();
    }

    requested_[pid] = true;
    queue_.push_back({ pid, imageName, std::move(cb) });
    cv_.notify_one();
}

void LazyLoader::ThreadProc() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);

    for (;;) {
        Request req;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [this] { return !running_.load() || !queue_.empty(); });
            if (!running_.load()) break;
            req = std::move(queue_.front());
            queue_.pop_front();
        }

        std::wstring path = ResolvePath(req.pid);

        {
            std::lock_guard<std::mutex> lk(mutex_);
            requested_.erase(req.pid);
            if (!path.empty()) cache_[req.pid] = path;
        }

        if (req.callback && !path.empty()) {
            req.callback(req.pid, path);
        }
    }
}

std::wstring LazyLoader::ResolvePath(uint32_t pid) {
    auto fn = GetNtQSI();
    if (!fn) return L"";

    // 方法 1：NtQuerySystemInformation(class 88) —— 实测 0.001ms/次，最便宜
    wchar_t buf[1024];
    TMP_SYSTEM_PROCESS_ID_INFORMATION info{};
    info.ProcessId = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(pid));
    info.ImageName.Length = 0;
    info.ImageName.MaximumLength = sizeof(buf);
    info.ImageName.Buffer = buf;

    ULONG needed = 0;
    NTSTATUS st = fn(SystemProcessIdInformationClass, &info, sizeof(info), &needed);
    if (NT_SUCCESS(st) && info.ImageName.Length > 0) {
        return std::wstring(info.ImageName.Buffer, info.ImageName.Length / sizeof(wchar_t));
    }

    // 方法 2：OpenProcess + QueryFullProcessImageNameW（回退）
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    DWORD size = 1024;
    wchar_t buf2[1024];
    std::wstring result;
    if (QueryFullProcessImageNameW(h, 0, buf2, &size)) {
        result.assign(buf2, size);
    }
    CloseHandle(h);
    return result;
}

} // namespace tmp
