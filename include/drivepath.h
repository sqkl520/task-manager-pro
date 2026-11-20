// traypaths.h - NT 设备路径 → DOS 路径转换
//
// 为什么需要：NtQuerySystemInformation(SystemProcessIdInformation=88) 返回的是
//   \Device\HarddiskVolume3\Windows\System32\notepad.exe
// 这种 NT 设备路径。SHGetFileInfoW / CreateFileW / ExtractIconExW 都不认它，
// 必须转成 C:\Windows\System32\notepad.exe。
//
// 实测（docs/evidence/icontest.txt）：
//   直接喂 NT 路径给 SHGetFileInfoW → 返回 FALSE，HICON 为 NULL
//   转成 DOS 路径后 → 成功拿到真实图标
#pragma once

#include <windows.h>
#include <string>
#include <unordered_map>
#include <mutex>

namespace tmp {

class DriveMap {
public:
    DriveMap() { Refresh(); }

    // 重新枚举盘符映射。卷挂载变化后需要调用。
    void Refresh();

    // NT 路径 → DOS 路径。无法转换时返回空串。
    std::wstring ToDosPath(const std::wstring& ntPath) const;

private:
    // \Device\HarddiskVolume3 -> C:
    std::unordered_map<std::wstring, std::wstring> deviceToDrive_;
    mutable std::mutex mutex_;
};

// 全局单例，避免每次转换都枚举盘符
DriveMap& GetDriveMap();

} // namespace tmp
