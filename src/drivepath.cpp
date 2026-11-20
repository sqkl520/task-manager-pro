// drivepath.cpp - NT 设备路径 → DOS 路径
#include "drivepath.h"

#include <vector>

namespace tmp {

void DriveMap::Refresh() {
    std::lock_guard<std::mutex> lk(mutex_);
    deviceToDrive_.clear();

    // 枚举 A:-Z:
    wchar_t target[1024];
    for (wchar_t c = L'A'; c <= L'Z'; ++c) {
        wchar_t driveName[3] = { c, L':', 0 };
        DWORD n = QueryDosDeviceW(driveName, target, 1024);
        if (n == 0 || n >= 1024) continue;
        // QueryDosDevice 返回的可能是多个目标，以 NUL 分隔；只取第一个
        std::wstring dev(target);
        if (dev.empty()) continue;
        deviceToDrive_[dev] = std::wstring(1, c) + L":";
    }
}

std::wstring DriveMap::ToDosPath(const std::wstring& ntPath) const {
    if (ntPath.empty()) return L"";

    // 已经是 DOS 路径（含 ":\"）就直接返回
    if (ntPath.size() >= 3 && ntPath[1] == L':' && (ntPath[2] == L'\\' || ntPath[2] == L'/')) {
        return ntPath;
    }

    // 内核伪路径（Registry / System / Idle 等），没有对应的可执行文件
    if (ntPath[0] != L'\\') return L"";

    std::lock_guard<std::mutex> lk(mutex_);

    // 最长前缀匹配（有些卷路径是 \Device\HarddiskVolume3 的子路径）
    const std::wstring* bestDev = nullptr;
    const std::wstring* bestDrive = nullptr;
    size_t bestLen = 0;
    for (const auto& kv : deviceToDrive_) {
        const std::wstring& dev = kv.first;
        if (ntPath.size() > dev.size() &&
            ntPath.compare(0, dev.size(), dev) == 0 &&
            ntPath[dev.size()] == L'\\') {
            if (dev.size() > bestLen) {
                bestLen = dev.size();
                bestDev = &kv.first;
                bestDrive = &kv.second;
            }
        }
    }
    if (bestDev) {
        return *bestDrive + ntPath.substr(bestLen);
    }

    // 处理 \??\C:\... 形式
    if (ntPath.rfind(L"\\??\\", 0) == 0) {
        std::wstring rest = ntPath.substr(4);
        if (rest.size() >= 3 && rest[1] == L':') return rest;
    }

    return L"";
}

DriveMap& GetDriveMap() {
    static DriveMap m;
    return m;
}

} // namespace tmp
