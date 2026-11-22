# Agent P — Phase 2 采集层预研

状态：已完成

**方法说明**：本轮**没有修改任何源码、没有编译、没有运行 `TaskManagerPro.exe`、没有运行 `taskmgr.exe`、没有截图**。
所有性能数字来自**本机 PowerShell + C# `Add-Type` P/Invoke 热循环实测**（`Stopwatch`，先热身再计数，取平均），
所有 struct/枚举/签名来自**本机 Windows SDK 10.0.26100.0 头文件逐行读取**，
所有硬件事实来自**本机真实硬件**（Xeon 服务器级 CPU / 32 GB / 2×NVMe / TP-LINK USB 无线网卡 / discrete GPU / Win10 22H2 19045）。

> **重要限定**：PowerShell/C# 实测的是**内核态接口本身的耗时**，不含目标工程里
> `perfdata.cpp` 的 `std::vector` 分配、字符串构造、`std::wstring` 拼接等托管/堆开销。
> 带「实测」字样的数字可直接用；凡我未测的写「未验证」。

> **⚠ 行号时效性警告**：本报告写作期间，`src/perfpage.cpp`、`src/tmwindow_details.cpp`、
> `include/perfpage.h` 正在被其他 agent（E / B）并发修改 —— 我读完首版之后这三份文件
> **又被改过**（例如 `PaintChart` 的签名已从 `(…, const wchar_t* unit, …)` 改成
> `(…, const wchar_t* maxLabel, const wchar_t* minLabel, …)`，E-06 已被修掉；
> `DetCol` 已加了 `DetField` 字段、`kDetCols` 从 54 行移到 106 行）。
> **报告里的行号已按我最后一次读取的时刻校正**，但下一轮动手前**必须重新 grep 定位**，
> 不要直接照抄行号。函数名和语义引用是稳定的。
> 同理 `E-16` 里提到的 `(void)unit;` —— 该行**已经不存在**了，E-06 已完成。

---

## 0. 本机的实测基准事实（后面每一项都要用到）

### 0.1 硬件

| 项 | 实测值 | 来源 |
|---|---|---|
| 内存条 | 2×16 GB DDR4-1866（`Node0_Dimm4` / `Node0_Dimm6`），8 插槽，最大 96 GB | SMBIOS Type16/17 解析 |
| 形态 | `formFactor=0x09`(DIMM) ×8 条全报 9，`memoryType=0x18`(DDR4) | SMBIOS Type17 |
| 物理内存总量 | `GlobalMemoryStatusEx` = 34,292,195,328 B（31.94 GB） | 实测 |
| 已安装内存 | `GetPhysicallyInstalledSystemMemory` = 34,359,738,368 B（32.00 GiB） | 实测 |
| 差值（=为硬件保留） | **67,543,040 B（64.4 MiB）** | 实测，两者相减 |
| 磁盘 | 2 块 NVMe（2 × NVMe SSD），**不是 1 块** | `IOCTL_STORAGE_QUERY_PROPERTY` BusType=17(NVMe) |
| 卷 | `C:` → 物理盘 0 分区3；`D:` → 物理盘 1 | `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS` |
| 磁盘分区表 | 两块都是 **MBR**（`PartitionStyle=1`） | `IOCTL_DISK_GET_DRIVE_LAYOUT_EX` |
| 网卡 | 7 条 `GetAdaptersAddresses` 返回项，其中真实物理 2 条（Realtek 有线 / TP-LINK 无线） | 实测 |
| 无线 | `WLAN 2` / SSID `HUAWEI-KY6PRW_5G` / 信号 60% / 325 Mbps | `wlanapi` 实测 |
| GPU 引擎 | 6 类：3D / Copy / Video Encode / Video Decode / Overlay / Other | `docs/evidence/FINAL.txt` + 本轮 PDH 复核 |
| PDH 引擎实例名 | 8 种：`3d` `copy` `videoencode` `videodecode` `legacyoverlay` `security` `security_1` `vr` | 本轮 `Get-Counter` 实测 483 实例 |
| 内存压缩 | **本机 `MemoryCompression=False`**（`Get-MMAgent`），无 `MemCompression` 进程 | 实测 —— 影响 (已压缩) 项 |
| `SystemDrive` | `C:` | 环境变量实测 |

### 0.2 关键 API 耗时（热态，C# 紧循环，n=50~500）

| API | 耗时 | 说明 |
|---|---|---|
| `DeviceIoControl(IOCTL_STORAGE_QUERY_PROPERTY/SeekPenalty)` | **0.0071 ms** | 句柄已缓存 |
| `DeviceIoControl(IOCTL_STORAGE_QUERY_PROPERTY/DeviceProperty, 1KB out)` | **0.0153 ms** | 同上 |
| `DeviceIoControl(IOCTL_DISK_GET_LENGTH_INFO)` | **0.0427 ms** | 同上 |
| `DeviceIoControl(IOCTL_DISK_GET_DRIVE_GEOMETRY_EX)` | **0.0433 ms** | 同上 |
| `CreateFileW("\\.\C:", GENERIC_READ)` 热态 | **0.083 ms** | 冷态 17–81 ms（首次） |
| 完整「2 卷 × (打开+3 个 IOCTL+关闭)」 | **0.266 ms** | 实测 |
| `GetVolumeInformationW` | **0.129 ms** | |
| `GetDiskFreeSpaceExW` | **0.112 ms** | |
| `GetLogicalDrives` | **0.013 ms** | |
| `GetSystemFirmwareTable('RSMB')` 全量 4363B | **0.091 ms** | 冷态 27 ms |
| `GetPhysicallyInstalledSystemMemory` | **0.0009 ms** | 冷态 23 ms，之后内核缓存 |
| `NtQuerySystemInformation(18)` 页文件信息 | **0.0003 ms** | ⭐ 比读注册表快 360× |
| 读注册表 `PagingFiles`/`ExistingPageFiles` | **0.109 ms** | |
| `GetAdaptersAddresses(AF_UNSPEC, SKIP_DNS)` | **11.8 ms** | ⚠ 昂贵 |
| `GetAdaptersAddresses(AF_UNSPEC, 含DNS)` | **16.9 ms** | ⚠ 更贵 |
| `WlanOpenHandle` 首次 | **4.17 ms** | |
| `WlanOpenHandle`+`Close` 热态 | **0.75 ms** | |
| `WlanEnumInterfaces` | **0.043 ms** | |
| `WlanGetAvailableNetworkList` | **0.151 ms** | |
| `WlanQueryInterface(current_connection)` | **0.164 ms** | |
| `NtQuerySystemInformation(80)` 内存列表 | **0.0003 ms** | |
| `NtQuerySystemInformation(5)` 全进程 | **16.4 ms** | 现有采集主体 |

### 0.3 ⚠ 现有 `perfdata.cpp` 的 `SYS_MEM_LIST` 布局是**错的**（新发现，附带证据）

`src/perfdata.cpp:100-108` 把 class 80 当成 **ULONG** 数组读：

```cpp
struct SYS_MEM_LIST {
    ULONG ZeroPageCount;            // +0
    ULONG FreePageCount;            // +4
    ULONG ModifiedPageCount;        // +8
    ULONG ModifiedNoWritePageCount; // +12
    ULONG BadPageCount;             // +16
    ULONG PageCountByPriority[8];   // +20
```

实测 class 80 返回 176 字节 = **22 个 `SIZE_T`（8 字节）**，不是 22 个 ULONG。
真实布局（phnt `SYSTEM_MEMORY_LIST_INFORMATION`，本人逐字段与 PDH 交叉验证）：

| 偏移 | 字段 | 本机实测值 | PDH 对照 |
|---|---|---|---|
| +0x00 | ZeroPageCount | 3,320 pages (13.0 MB) | — |
| +0x08 | FreePageCount | 829,209 pages (3,239.1 MB) | `Free & Zero Page List Bytes` = 3.056 GB ✅ |
| +0x10 | ModifiedPageCount | 11,632 pages (45.4 MB) | `Modified Page List Bytes` = 46.6 MB ✅ |
| +0x18 | ModifiedNoWritePageCount | 33 pages | — |
| +0x20 | BadPageCount | 0 | — |
| +0x28..+0x60 | PageCountByPriority[8] | 合计 2,517,662 pages (9,834.6 MB) | `Standby Cache Normal Priority` = 9,623 MB ✅ |
| +0x68..+0xA0 | RepurposedByPriority[8] | 合计 17,458,632 pages | — |
| +0xA8 | ModifiedPageCountPageFile | 11,313 pages | — |

**当前代码实际读出的值（本机同一时刻）**：

- `freeBytes` 读 `+4` → 拿到的是 `ZeroPageCount` 的高 4 字节 = **0**（永远 0）
- `modifiedBytes` 读 `+8` → 拿到的其实是 `FreePageCount` = 3,239.1 MB（**把空闲当已修改**）
- `standbyBytes` 读 `+20..+48` → 拿到的是 `BadPageCount` + `PageCountByPriority` 的**错位一半** = 28.4 MB（真值 9,834 MB，**差 346 倍**）

这不是 Phase 2 清单里的项目，但它直接决定了 E-16 里「已压缩 / 备用 / 已修改」能不能做对，
所以放在最前面：**任何 E-16 的改动都必须先修这个布局**，否则新加的数据也是错的。
（`standbyBytes` 偏小 346 倍会让内存组合条「备用」段几乎不可见 —— 这可能是内存页看起来"发白"的一个隐藏原因。）

**修法**（只改 `perfdata.cpp` 内 `SYS_MEM_LIST` 与读取处，不动语义）：

```cpp
struct SYS_MEM_LIST {
    SIZE_T ZeroPageCount;              // +0x00
    SIZE_T FreePageCount;              // +0x08
    SIZE_T ModifiedPageCount;          // +0x10
    SIZE_T ModifiedNoWritePageCount;   // +0x18
    SIZE_T BadPageCount;               // +0x20
    SIZE_T PageCountByPriority[8];     // +0x28
    SIZE_T RepurposedByPriority[8];    // +0x68
    SIZE_T ModifiedPageCountPageFile;  // +0xA8
};                                     // sizeof = 176 ✅
```

---

## 1. E-15 磁盘页属性（容量 / 已格式化 / 系统磁盘 / 页面文件 / 类型）

### 目标
磁盘页右侧属性区 5 行，MUI 原文（`docs/evidence/mui_strings_all.txt:493-497`）：
```
类型:      HDD / SSD / 持久性内存 / 可移动 / SD / USB / 未知     (34451 / 34469-34477)
容量:
已格式化:
系统磁盘:
页面文件:
```
同时 E-13 要求标题 `磁盘 0 (D: E: C:)` 的盘符列表也来自同一份数据。

### 数据源（全部实测可用，无需 WMI）

| 界面项 | 数据源 | 本机实测结果 |
|---|---|---|
| 容量（物理） | `IOCTL_DISK_GET_LENGTH_INFO` → `GET_LENGTH_INFORMATION.Length`（**字节**）；或 `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX` → `DISK_GEOMETRY_EX.DiskSize` | C: 255,608,225,792 B = 238.05 GiB；D: 512,108,789,760 B |
| 容量（物理盘级） | `DISK_GEOMETRY_EX.DiskSize` 于 `\\.\PhysicalDriveN` | 盘0 256,060,514,304；盘1 512,110,190,592 |
| 已格式化 | **= 卷的 `GetDiskFreeSpaceExW(lpTotalNumberOfBytes)`**（不是物理容量） | C: 255,608,221,696 B（比物理长度少 4096 B）；D: 512,108,785,664 B |
| 系统磁盘 | 比较 `%SystemDrive%`（本机 `C:`）与该卷盘符 → 是/否 | 需要 `是/否`（MUI 32280/32281） |
| 页面文件 | **`NtQuerySystemInformation(18)`** → 每个 `SYSTEM_PAGEFILE_INFORMATION` 的 `Name`（`\??\C:\pagefile.sys`）与盘符前缀匹配 | `\??\C:\pagefile.sys`，Total=7,174,867 pages (28,026.8 MB)，InUse=102,629 pages (400.9 MB) |
| 类型 | `IOCTL_STORAGE_QUERY_PROPERTY` + `StorageDeviceSeekPenaltyProperty` → `DEVICE_SEEK_PENALTY_DESCRIPTOR.IncursSeekPenalty` = **FALSE → SSD**；TRUE → HDD。辅以 `StorageDeviceProperty` → `STORAGE_DEVICE_DESCRIPTOR.BusType` | C:/D: `IncursSeekPenalty=0` → SSD；`BusType=17`(NVMe) |

**关于「已格式化」的判据**（回答任务书的问题）：**是，它就是分区/卷实际可用容量**，
与物理盘容量不同。本机证据：`C:` 物理长度 255,608,225,792 与 `GetDiskFreeSpaceExW` 总量
255,608,221,696 差 4096 字节（一个扇区，MBR 备份扇区）。这是同一台机器上能拿到的最直接对比。
注意 `GetDiskFreeSpaceExW` 的 `lpTotalNumberOfBytes` **受磁盘配额影响**，
真正不受配额影响的是 `lpTotalNumberOfFreeBytes`（总空闲）。总量若要求"精确无配额"，应当用
`IOCTL_DISK_GET_LENGTH_INFO`（卷长度）作为「容量」，`GetDiskFreeSpaceExW` 作为「已格式化」。
两者本机相差 1 个扇区，肉眼无差，但如果要严格区分语义，这样取最干净。

**关于「类型」的取值表**（MUI 34469–34477）只有 7 档：`HDD / SSD / 持久性内存 / 可移动 / SD / USB / 未知`。
本机能可靠分出的只有 4 档：
- `IncursSeekPenalty == FALSE` → `SSD`
- `IncursSeekPenalty == TRUE` → `HDD`
- `STORAGE_DEVICE_DESCRIPTOR.RemovableMedia != 0` → `可移动`
- `BusType == BusTypeSd(12)` → `SD`；`BusType == BusTypeUsb(7)` → `USB`
- 其余（含 IOCTL 失败、虚拟盘、`BusTypeVirtual/FileBackedVirtual`）→ `未知`
`持久性内存`（PMEM）需要 `BusTypeSCM(18)`，**未验证**（本机无此硬件）。

### 实现步骤

**A. `include/perfdata.h`** — 扩展 `DiskSample`（新增字段不影响任何现有读取点）：

```cpp
struct DiskSample {
    std::wstring name;              // 保留（现在是 "磁盘 0"，Phase 2 后应由 perfdata 拼盘符）
    double activePercent = 0.0;
    double readMBps = 0.0;
    double writeMBps = 0.0;
    double avgResponseMs = 0.0;

    // ---- E-15 新增（静态，采集一次即可）----
    int      diskNumber   = -1;     // 物理盘号（\\.\PhysicalDriveN 的 N），-1 = 未知
    std::vector<wchar_t> driveLetters;  // 该物理盘上的盘符，如 {L'D', L'E', L'C'}
    uint64_t capacityBytes  = 0;    // 容量（IOCTL_DISK_GET_LENGTH_INFO / GET_DRIVE_GEOMETRY_EX）
    uint64_t formattedBytes = 0;    // 已格式化（Σ 各卷 GetDiskFreeSpaceExW 的 total）
    std::wstring typeText;          // L"SSD" / L"HDD" / L"可移动" / L"SD" / L"USB" / L"未知"
    std::vector<std::pair<wchar_t,bool>> volumes; // (盘符, 是否为系统盘)
    bool pageFileOnDisk = false;    // 该盘上有 pagefile.sys
    // 可选：多卷时每卷的"系统磁盘/页面文件"，若 UI 只显示一行则取"任一为真"
};
```

**B. `src/perfdata.cpp`** — 新增静态采集函数 `CollectDiskStatic(std::vector<DiskSample>&)`，
在 `ThreadProc()` 里 `InitPdH()` 之后调用**一次**（不是每帧），结果存进 `PerfCollector` 成员，
`CollectOnce` 每轮只做 `out.disks = diskStatic_;` 再填动态速率。步骤：

1. `GetLogicalDrives()` → 位掩码 → 枚举出所有盘符（本机 0xC = C、D 两个）。
2. 对每个盘符 `X`：
   - `CreateFileW(L"\\\\.\\X:", GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE, ...)` （**必须 `GENERIC_READ`**，见 §1.权限）
   - `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS` → `Extents[0].DiskNumber` = 物理盘号
   - `IOCTL_DISK_GET_LENGTH_INFO` → 卷字节长度（= 容量）
   - `IOCTL_STORAGE_QUERY_PROPERTY` + `StorageDeviceSeekPenaltyProperty` → SSD/HDD
   - `IOCTL_STORAGE_QUERY_PROPERTY` + `StorageDeviceProperty`（两段式：先 `STORAGE_DESCRIPTOR_HEADER` 探长，再全量）→ `BusType` / `RemovableMedia`
   - `GetDiskFreeSpaceExW(L"X:\\", ..., &totalBytes, ...)` → 已格式化
   - `CloseHandle`
3. `NtQuerySystemInformation(18, ...)` 一次拿到所有页文件路径，转成盘符集合。
4. 按物理盘号聚合：同一 `DiskNumber` 的多个盘符合并，`capacity` 取该盘 `GET_DRIVE_GEOMETRY_EX.DiskSize`（物理），`formattedBytes` 累加各卷；`driveLetters` 按卷挂载顺序追加（原版 `磁盘 0 (D: E: C:)` 就是挂载顺序，**不是字母序** —— 本机 `D:` 是盘 1、`C:` 是盘 0，如果按字母序会拼成 `(C: D:)`，与原版 `(D: E: C:)` 的形态不同；顺序来源应为 `GetLogicalDrives` 的枚举顺序 + `MountMgr` 挂载序，**具体顺序规则未验证**，先按 `QueryDosDevice`/盘符枚举序即可，并写明"顺序未证实"）。
5. `pageFileOnDisk` = 该盘符集合 ∩ 页文件盘符集合非空。
6. `volumes[i].second` = (`X:` == `%SystemDrive%`)。

**C. 绘制侧（`src/perfpage.cpp`，属 E agent）** — 只需把 `PaintDiskPage` 的属性行从
`读取速度/写入速度/平均响应时间/传输速率` 换成 `类型/容量/已格式化/系统磁盘/页面文件`，
数据全部来自 `DiskSample` 新字段。**这一半不归 Phase 2 采集，属于 E-15 的 UI 侧**，
建议与采集侧同批做，否则采了不用。

### 成本
- 稳态：**0**（全部是静态信息，采集一次）
- 首次：实测「2 卷 × (打开 + 3 IOCTL + 关闭)」热态 **0.266 ms**；冷态首个 `CreateFile` **17–81 ms**
  （NVMe 首次访问要唤醒设备栈）。粗估首次总计 **5–20 ms**（含冷启动 2 个 `CreateFile` + 2 个 `GetVolumeInformationW`）。
  放在采集线程 `Init` 阶段做，UI 顶多多显示一次"正在采集…"。
- 每帧调用：**绝对不要**。`GetDiskFreeSpaceExW` 112 µs、`IOCTL` 也都要重新 `CreateFile`，
  按 1 Hz 算 1 ms/s 虽然能接受，但没有任何理由每帧做。
- 需要缓存：**是**，永久缓存（盘符/容量/类型在插拔盘之前不会变）。
  可选：注册 `WM_DEVICECHANGE` 或每 30 秒重扫一次。

### 权限
**必须要 `GENERIC_READ` 打开卷句柄，而这需要管理员。本机实测关键证据：**

| 打开方式 | `STORAGE_QUERY_PROPERTY` | `DISK_GET_LENGTH_INFO` | `GET_DRIVE_GEOMETRY_EX` | `GET_DRIVE_LAYOUT_EX` |
|---|---|---|---|---|
| `access = 0` | ✅ 成功 | ❌ **err=5 (拒绝访问)** | ⚠ 返回 `ok=True` 但 `err=5`、数据不可信 | ⚠ 同左 |
| `GENERIC_READ` | ✅ | ✅ | ✅ | ✅ |

（本机当前是管理员令牌，所以 `CreateFile` 本身能成功；`access=0` 时 `IOCTL_DISK_GET_LENGTH_INFO`
因为 `CTL_CODE` 里带 `FILE_READ_ACCESS` 而被句柄访问权拦下 → **err=5 是句柄权限问题，不是管理员问题**。）

结合 `CreateFileW` 文档的「Physical Disks and Volumes」段（MS Learn 原文：
"the caller must have administrative privileges … dwCreationDisposition must be OPEN_EXISTING …
when opening a volume … dwShareMode must have FILE_SHARE_WRITE"），结论：

- **普通用户（非管理员）：拿不到 `\\.\X:` 的 `GENERIC_READ` 句柄 → 这 5 项全部拿不到**。
- 本项目 `main.cpp` 已有 `highestAvailable` + `autoElevate` 的清单（与 `taskmgr.exe` 一致），
  所以**默认以管理员跑时全都能拿到**；被降权运行时必须**优雅降级为「—」**，不能崩。
- `GetVolumeInformationW` / `GetDiskFreeSpaceExW` / `GetLogicalDrives` / `NtQSI(18)` **不需要管理员**。
  也就是说：**非管理员下「已格式化」「系统磁盘」「页面文件」仍可拿到，「容量」「类型」拿不到。**
  这个不对称必须在实现里体现（逐项独立降级，不要整块失败）。

### 风险
1. **`CreateFile("\\.\X:")` 在非管理员下 err=5** —— 必须 `if (h == INVALID_HANDLE_VALUE) { 该盘 typeText = L"未知"; capacity = 0; }` 后继续，绝不 `return false` 整块放弃。
2. **`access=0` 时 `GET_DRIVE_GEOMETRY_EX` 返回 `ok=True` 但 `GetLastError()=5`** —— 实测这个组合返回的 `ret=112` 但缓冲区内容不可信（同一轮里 `bytesPerSector` 读出 -1637523456 这种垃圾）。**判断成功必须用 `DeviceIoControl` 的返回值 + `lpBytesReturned` 是否等于期望长度**，不能用 `GetLastError`。
3. **可移动盘/U盘在枚举后被拔出** → `CreateFile` 失败或 IOCTL 超时。U 盘拔出时 `IOCTL` 可能阻塞数秒。建议：只对 `GetDriveTypeW == DRIVE_FIXED(3)` 的盘做深度探测；`DRIVE_REMOVABLE` 只填 `类型=可移动`、其余留空。
4. **本机两块盘都是 MBR**。`DRIVE_LAYOUT_INFORMATION_EX` 的 `PartitionStyle` 是 `DWORD`，而
   `PARTITION_INFORMATION_EX.PartitionStyle` 是 `PARTITION_STYLE` 枚举 —— **两个类型不同，不要互相赋值**。
   且 `PARTITION_INFORMATION_EX` 在 `NTDDI_VERSION >= NTDDI_WIN10_RS3` 时**多出 `IsServicePartition` 字段**，
   `sizeof` 与老文档不一致。**绝对不要硬编码 `sizeof(PARTITION_INFORMATION_EX) == 144`**，要用 SDK 的 `sizeof`。
   （本次探针里我按 144 硬算偏移，盘 0 的 3 个分区结果是对的，但这是巧合，不能写进产品代码。）
5. **本机 `DiskNumber` 与盘符不是一一对应**（`C:`→盘0、`D:`→盘1），若按"盘符字母序"拼标题会得到完全错误的
   `磁盘 0 (C:)`、`磁盘 1 (D:)`。必须走 `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS`。
6. **`GetDiskFreeSpaceExW` 的 `lpTotalNumberOfBytes` 受配额影响** —— 若将来做"已格式化"，要注释写明这一点。

### E-15 可行性结论
**直接可做，但有一个硬条件：必须以管理员运行。**
5 项里「已格式化 / 系统磁盘 / 页面文件」在普通权限下也能做；
「容量 / 类型」需要管理员句柄，非管理员必须降级为「—」。
全部 API 已在本机逐项验证过返回值和真实数据，无猜测。

---

## 2. E-16 内存页 `static_memory` 组 + `(已压缩)`

### 目标
内存页右侧属性区从「7 行 dynamic」变成「5 行 static + 6 行 dynamic」：
```
static_memory (MUI 34349/34350/34380/34381/34382)
  速度:               %u MHz                 (34349 + 34366)
  已使用的插槽:        %u/%u                  (34350 + 34351 "%1!u!/%2!u!")
  外形规格:                                   (34380)
  为硬件保留的内存:                            (34381)
  最大内存:                                   (34382)
dynamic_memory
  使用中(已压缩)                               (34399)
  可用 / 已提交 / 已缓存 / 分页缓冲池 / 非分页缓冲池
```

### 数据源（逐个实测）

| 界面项 | 数据源 | 本机实测值 | 可行性 |
|---|---|---|---|
| **已使用的插槽** | SMBIOS **Type 16**（Physical Memory Array）`Number of Memory Devices`（+0x0D，WORD）作分母；SMBIOS **Type 17** 里 `Size != 0` 的条目数作分子 | Type16 报 **8**；Type17 报 8 条、其中 **2 条** `sizeCode=0x4000`=16384 MB → 已用 **2/8** | ✅ 直接可做 |
| **外形规格** | SMBIOS Type 17 `Form Factor`（+0x0E，BYTE） | 全部 `0x09` = **DIMM** | ✅ 直接可做 |
| **速度** | SMBIOS Type 17 `Speed`（+0x15，WORD，MT/s）；若为 0 用 `Configured Memory Clock Speed`（+0x20，WORD） | **1866**（两条都是 1866 / configured 1866） | ✅ 直接可做 |
| **最大内存** | SMBIOS **Type 16** `Maximum Capacity`（+0x07，DWORD，**KB**）；若 = `0x80000000` 则用 `Extended Maximum Capacity`（+0x0F，QWORD，**字节**） | **100,663,296 KB = 96 GB** | ✅ 直接可做 |
| **为硬件保留的内存** | `GetPhysicallyInstalledSystemMemory()`（安装量）− `GlobalMemoryStatusEx().ullTotalPhys`（可用物理量） | 34,359,738,368 − 34,292,195,328 = **67,543,040 B = 64.4 MiB** | ✅ 直接可做 |
| **使用中(已压缩)** | **本机不可用** —— 见下 | — | ⚠ 见下 |

### SMBIOS 读取的确切路径（本机已验证到字段级）

```cpp
// 1) 探长度
UINT need = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);   // 'RSMB' = 0x52534D42
// 2) 取全量
std::vector<BYTE> buf(need);
GetSystemFirmwareTable('RSMB', 0, buf.data(), need);
// 3) 头部：RawSMBIOSData { BYTE Used20CallingMethod; BYTE MajorVersion;
//                          BYTE MinorVersion; BYTE DmiRevision;
//                          DWORD Length; BYTE SMBIOSTableData[1]; }
//    结构体起点 = buf + 8
// 4) 遍历：每条 = header(类型@+0, 长度@+1) + 格式化区 + 字符串区（双 NUL 结尾）
```

**本机实测输出（真实数据，可作为单测基准）**：

```
RawSMBIOSData: Used20=0 SMBIOS 2.8 DMI=0 Length=4355 (buffer=4363)
Type16 len=23 handle=0x0028 location=3 use=3 ecc=6 MaxCapacity=100663296 KB (96.00 GB) devices=8 extMax=0
Type17 len=40 handle=0x002A sizeCode=0x0000 = 0 MB ff=0x09 type=0x02 speed=0 locator='Node0_Dimm0'
Type17 len=40 handle=0x002C sizeCode=0x0000 = 0 MB ff=0x09 type=0x02 speed=0 locator='Node0_Dimm1'
Type17 len=40 handle=0x002E sizeCode=0x0000 = 0 MB ff=0x09 type=0x02 speed=0 locator='Node0_Dimm2'
Type17 len=40 handle=0x0030 sizeCode=0x0000 = 0 MB ff=0x09 type=0x02 speed=0 locator='Node0_Dimm3'
Type17 len=40 handle=0x0032 sizeCode=0x4000 = 16384 MB ff=0x09 type=0x18 speed=1866 cfg=1866 locator='Node0_Dimm4'
Type17 len=40 handle=0x0034 sizeCode=0x0000 = 0 MB ff=0x09 type=0x02 speed=0 locator='Node0_Dimm5'
Type17 len=40 handle=0x0036 sizeCode=0x4000 = 16384 MB ff=0x09 type=0x18 speed=1866 cfg=1866 locator='Node0_Dimm6'
Type17 len=40 handle=0x0038 sizeCode=0x0000 = 0 MB ff=0x09 type=0x02 speed=0 locator='Node0_Dimm7'
End-of-table at structure offset 4357
SUMMARY: type16=1 type17=8 populated=2
```

**关键字段偏移（从本机真实字节流反推出的，可直接照抄）**：

- Type 16: `MaximumCapacity` @ +0x07 (DWORD, KB)；`NumberOfMemoryDevices` @ +0x0D (WORD)；
  `ExtendedMaximumCapacity` @ +0x0F (QWORD) —— **仅当 len >= 23 且 MaximumCapacity == 0x80000000 时有效**
- Type 17: `Size` @ +0x0C (WORD)；`FormFactor` @ +0x0E (BYTE)；`DeviceLocator` 字符串索引 @ +0x10 (BYTE)；
  `MemoryType` @ +0x12 (BYTE)；`Speed` @ +0x15 (WORD)；`ConfiguredMemoryClockSpeed` @ +0x20 (WORD，需 len >= 34)
- **`Size` 的三种编码**（本机全部命中第一种）：
  - `0x0000` = 该槽未安装
  - `0xFFFF` = 未知
  - `bit15` 置位 = 单位 KB（`value & 0x7FFF` 是 KB）
  - 否则 = MB

**「已压缩」的数据源 —— 本机不可用，且有硬障碍：**

- `Get-MMAgent` 实测：**`MemoryCompression : False`**（本机未启用内存压缩）
- 全进程扫描 596 个进程，**没有 `MemCompression` 进程**（`NtQuerySystemInformation(5)` 逐条比对 ImageName 含 `ompress` 的结果 = 0 条）
- `PDH` 全部 206 个计数器集里**没有任何 Memory Compression 相关计数器**（只有 SMB / RemoteFX / Hyper-V 迁移的同名不同义项）
- 社区熟知的 `SystemMemoryUsageInformation` 类号 **97 (0x61)** 在本机返回
  **`STATUS_INVALID_INFO_CLASS (0xC0000003)`** —— 该类在 Win10 19045 上不存在
- 我扫了 class 0..199 全部，**没有任何 class 直接暴露"压缩存储大小"**。
  唯一疑似的是 **class 100（888 字节）**，其 +0x50 起是 `{ 848, 8589934592, 15334220253, 0, 11541179288, 255 }` 的 6 元组循环 3 遍
  （8589934592 = 8 GiB 整，看着像 NUMA/分区粒度），**语义未验证，不能用来画「已压缩」**。

**结论**：「使用中(已压缩)」里的**压缩字节数在本机拿不到**。
但 MUI 的文案本身有两种形态（`docs/evidence/mui_strings_all.txt:375`）：

```
33611  使用中(%1!s! MB) \n ... \n 使用中，已压缩(%2!s! MB) \n 未启用内存压缩
```

即原版**明确有「未启用内存压缩」这一分支**。所以正确的降级路径是：
- 检测到内存压缩未启用（`Get-MMAgent` 的 `MemoryCompression == FALSE`，或 `MemCompression` 进程不存在）
  → 属性行只显示 `使用中`，tooltip 走「未启用内存压缩」
- 检测到已启用 → 显示 `使用中(已压缩)`，但**压缩字节数仍填不出来**

**唯一可行的压缩字节数来源**（本机无法验证）：`MemCompression` 进程（pid 4 之外的一个独立进程）的
`WorkingSetSize` / `PrivatePageCount`，即 `NtQuerySystemInformation(5)` 里 `ImageName == "MemCompression"` 的那条。
这条在本机拿不到（进程不存在）。**标注未验证**。

> `Get-MMAgent` 是 `mmagent.dll` 的 PowerShell cmdlet，C++ 侧对应的是
> `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management\EnableCompression`
> —— **本机这个值不存在**（`reg query` 报"系统找不到指定的注册表项"）。
> 所以"是否启用压缩"在 C++ 里**没有可靠的、已文档化的读法**。建议：只在 `MemCompression` 进程存在时才显示 `(已压缩)`。

### 实现步骤

**A. `include/perfdata.h`** — `PerfSnapshot` 加一组 static 字段（一次性填充）：

```cpp
struct MemHardware {
    uint32_t slotUsed  = 0;      // SMBIOS T17 中 Size!=0 的条数
    uint32_t slotTotal = 0;      // SMBIOS T16 NumberOfMemoryDevices
    uint32_t speedMhz  = 0;      // SMBIOS T17 Speed（非 0 那条），否则 ConfiguredMemoryClockSpeed
    int      formFactor = -1;    // SMBIOS T17 FormFactor 原始 BYTE，-1 = 未知
    uint64_t maxCapacityBytes = 0;   // SMBIOS T16 MaximumCapacity(KB)*1024 或 ExtendedMaximumCapacity
    uint64_t hwReservedBytes  = 0;   // GetPhysicallyInstalledSystemMemory - GlobalMemoryStatusEx.ullTotalPhys
    bool     compressionEnabled = false;  // MemCompression 进程存在
    uint64_t compressedBytes = 0;         // 未验证，本机恒 0
};
```

**B. `src/perfdata.cpp`** — 新增 `ReadMemoryHardware()`，在 `Start()` 里调一次（和 `ReadStaticCpuInfo()` 并列）。
SMBIOS 解析代码约 120 行，注意三件事：
1. **`sizeof` 用 SDK 定义还是手写？** `RawSMBIOSData` **不在 SDK 里**（我 grep 过 `10.0.26100.0` 全目录，只有 `winioctl.h:8390` 的注释命中 SMBIOS 字样）。
   `RSMB` 常量也不在。**必须自己定义**，并加注释说明"取自 MS Learn `GetSystemFirmwareTable` 示例，非 SDK 头文件"。
2. **字符串区遍历要防越界**：本机最后一条 Type17 的字符串区之后紧接 `00 00`，然后才是 End-of-table。
   必须 `while (off + 4 <= len)`，且 `if (len < 4) break;`。
3. **`MaximumCapacity == 0x80000000` 分支本机不触发**（本机是 96 GB < 4 TB），但**必须实现**，否则大内存机器会读出负数。

**C. `src/perfperf.cpp` / `perfpage.cpp`（E agent）** — `PaintMemPage` 的属性区改两段。

### 成本
- **首次**（冷态）：`GetSystemFirmwareTable` 探长 **27.9 ms** → 全量 + 解析 **25.6 ms**；
  `GetPhysicallyInstalledSystemMemory` **22.6 ms**（冷态）。合计约 **50–150 ms**（冷态首跑波动大，实测第一次 144 ms）
- **稳态**（热态）：`GetSystemFirmwareTable` **0.091 ms**；`GetPhysicallyInstalledSystemMemory` **0.0009 ms**
- **每帧调用：不要**。这些全是静态值。采集一次缓存即可。
- **缓存策略**：`Start()` 里跑一次，之后永不刷新。内存条不会热插拔。

### 权限
**全部不需要管理员。** 实测：`GetSystemFirmwareTable` / `GetPhysicallyInstalledSystemMemory` /
`GlobalMemoryStatusEx` 在当前（管理员）令牌下成功；这几个都是 `WINAPI_PARTITION_DESKTOP | SYSTEM`
的普通 Win32 API，无提权要求。SMBIOS 表是只读固件数据，本机 `Used20CallingMethod=0` 表示由 SMBIOS 本身提供。

### 风险
1. **`GetSystemFirmwareTable('RSMB')` 在部分机器/部分 BIOS 上返回 0 或权限失败** —— 必须判空后整体跳过 static 组，**不能崩溃**。（本机正常）
2. **`GetPhysicallyInstalledSystemMemory` 与 `GlobalMemoryStatusEx` 的语义变化**：MS Learn 明确说 2020 年后
   该函数改为返回 SMBIOS 报告的总量（含为硬件保留）。本机差值 64.4 MiB 与"显卡/固件保留"量级吻合。
   但若两者相等（某些 VM / 某些 BIOS），`hwReservedBytes` 会是 0 → UI 显示 `0 MB` 还是 `—`？
   **建议显示 `—`**（`hwReservedBytes==0` 时）。
3. **本机 8 条 Type17 里有 6 条是"空槽但仍有完整 40 字节结构"**，`Size=0`。若只按"Type17 条目数"算插槽，
   会得出 8/8 = 全满，与原版语义（已使用）相反。**分子必须过滤 `Size != 0`**。
4. **外型规格的枚举名**：MUI 里**没有** `DIMM` / `SODIMM` 等字符串（我 grep 过 `mui_strings_all.txt`，
   只有 `34477 未知`）。但 `taskmgr.exe` 的 `.rdata` 里有一张完整的 UTF-16 表
   （`docs/evidence/main/str_utf16.txt:2161-2178`）：`EDRAM / FLASH / FEPROM / EEPROM / CDRAM / EPROM / SDRAM / 3DRAM / RDRAM / SGRAM / Reserved / DDR2 FB-DIMM / Not Found / Proprietary Card / Row of chips / SODIMM / FB-DIMM / SRIMM`，
   再往前后是 `DIMM`（本机实测打印得出 `ff=0x09` 对应 DIMM）。
   **这张表是 SMBIOS Type 17 FormFactor 取值 0..0x0F 的显示名**，可以直接照抄；
   但 `0x09 = DIMM` 这个映射是我从"本机 ff=0x09 且机器是台式机 DDR4 DIMM"推的，**没有硬证据**，
   SMBIOS spec 里 FormFactor 0x09 确实是 DIMM。标"spec 依据，非本机反查"。
5. **「已压缩」的压缩字节数拿不到**（本机无压缩 + 无任何 class 暴露）。
   **不要为了这一项编造数据**。建议：压缩未启用 → 不显示后缀；启用但拿不到 → 显示 `使用中(已压缩)` 但括号内留空或显示 `—`。

### E-16 可行性结论
**static_memory 组 5 项：直接可做**（SMBIOS 路径本机已逐字段验证，全部无需管理员）。
**`(已压缩)` 后缀：可做（检测进程是否存在）；但括号里的压缩字节数：硬障碍 —— 本机无数据源，且 Win10 19045 上没有可用的 NtQSI class。**
建议做法：检测到未启用就按原版 `未启用内存压缩` 分支走，这是**原版真实存在的分支**（MUI 33611 明文），不算降级。

---

## 3. E-28 网络页属性（IPv4 / IPv6 / SSID / DNS / 服务提供商 / 信号强度）

### 目标
`static_network` 是 `rowlayout(4)` 的 8 组 label+value（`docs/evidence/uifile_Taskmgr_30001_strings.txt:653-679`）：

| 元素 | MUI | 文案 |
|---|---|---|
| `adapter_Label` / `adapter` | 34549 | 适配器名称: |
| `wwan_Label` / `wwan` | 34550 | 服务提供商: |
| `wifi_Label` / `wifi` | 34558 | SSID: |
| `netname_Label` / `netname` | 34554 | DNS 名称: |
| `netType_label` / `netType` | 34551 | 连接类型: |
| `ipv4_Label` / `ipv4` | 34552 | IPv4 地址: |
| `ipv6_Label` / `ipv6` | 34553 | IPv6 地址: |
| `signal_Label` / `signal` | 34564 | 信号强度: |

### 数据源：`GetAdaptersAddresses` 逐字段对应（**struct 已实测验证**）

`IP_ADAPTER_ADDRESSES_LH`（`IPTypes.h:286-350`），本机实测可用的字段：

| 界面项 | 字段 | 本机实测值 | 备注 |
|---|---|---|---|
| 适配器名称 | `FriendlyName` | `WLAN 2` / `以太网` / `VMware Network Adapter VMnet8` | ✅ |
| 连接类型 | `IfType` | 71=无线 / 6=以太网 / 24=loopback | 映射见下 |
| IPv4 地址 | `FirstUnicastAddress` 链中 `sockaddr_in`（`sa_family==AF_INET`） | `192.168.3.20`、`169.254.93.161` 等 | ✅ |
| IPv6 地址 | 同上链中 `sockaddr_in6`（`sa_family==AF_INET6`） | `2408:8214:c31:d4e0:...`（WLAN 有 **3 个全局 + 1 个链路本地**） | 多值 → 需选或拼接 |
| DNS 名称 | **注意语义**：`DnsSuffix` 是 DNS 后缀（如 `corp.local`）；`FirstDnsServerAddress` 是 DNS 服务器地址链 | 本机 `DnsSuffix` 为空，DNS 服务器链未打印（用 `0x0006` 才能拿到） | 原版「DNS 名称:」应取 `DnsSuffix`（域名后缀），不是服务器地址 |
| 信号强度 | **不在 `GetAdaptersAddresses` 里**，要 `wlanapi` | 见下 | — |
| SSID | **不在 `GetAdaptersAddresses` 里**，要 `wlanapi` | 见下 | — |
| 服务提供商 | **不在 `GetAdaptersAddresses` 里**；`wwan` 是移动宽带专用，要 `wcmapi` / `mbnapi` | 本机无 WWAN，**未验证** | — |

**`GetAdaptersAddresses` 本机实测返回（逐条，含 `IfType` / `OperStatus` / 速率）**：

```
if=13 type=6   oper=2 mtu=1500 tx=18446744073709551615(INT64_MAX,未知) rx=同上  paLen=6 friendly='以太网'      desc='Realtek PCIe GbE Family Controller'
if=4  type=71  oper=2 mtu=1500 tx=INT64_MAX rx=INT64_MAX paLen=6 friendly='本地连接* 9'    desc='Microsoft Wi-Fi Direct Virtual Adapter'
if=15 type=71  oper=2 mtu=1500 tx=INT64_MAX rx=INT64_MAX paLen=6 friendly='本地连接* 10'   desc='Microsoft Wi-Fi Direct Virtual Adapter #2'
if=20 type=6   oper=1 mtu=1500 tx=100000000   rx=100000000 paLen=6 friendly='以太网 3'     desc='Famatech Radmin VPN Ethernet Adapter'
if=25 type=6   oper=1 mtu=1500 tx=100Mbps rx=100Mbps paLen=6 friendly='VMware Network Adapter VMnet1'
if=8  type=6   oper=1 mtu=1500 tx=100Mbps rx=100Mbps paLen=6 friendly='VMware Network Adapter VMnet8'
if=26 type=71  oper=1 mtu=1492 tx=325000000 (325Mbps) rx=325Mbps paLen=6 friendly='WLAN 2' desc='TP-LINK Wireless USB Adapter'
if=1  type=24  oper=1 mtu=4294967295 tx=1Gbps rx=1Gbps paLen=0 friendly='Loopback Pseudo-Interface 1'
```

**关键实测发现（会直接影响实现）**：
1. `OperStatus` 语义是 `1=Up`、`2=Down`（**不是** `IF_OPER_STATUS` 里的 1=Connected 含义）。本机 WLAN（真连着网）报 `oper=1`，而"已断开的有线网卡"报 `oper=2`。**别把 2 当 Up**。
2. `TransmitLinkSpeed`/`ReceiveLinkSpeed` 在**未连接/虚拟适配器上返回 `INT64_MAX`（0xFFFFFFFFFFFFFFFF）**，直接显示会变成天文数字。必须判 `> 1e12` 就当作"未知"。
3. `IfType=71` 的适配器有 **3 条**（2 条 Wi-Fi Direct 虚拟 + 1 条真无线），仅靠 `IfType` 分不出真假。`Description` 里含 "Virtual" 的是虚拟的。
4. `WLAN 2` 的 `mtu=1492`（PPPoE），不是 1500 —— 不能拿 MTU 当判据。

**`IfType` → 连接类型映射（MUI 34693-34697）**：

| `IfType` | 值 | MUI 文案 |
|---|---|---|
| `IF_TYPE_ETHERNET_CSMACD` | 6 | `以太网` (34693) |
| `IF_TYPE_IEEE80211` | 71 | `Wi-Fi` (34694) |
| `IF_TYPE_WWANPP` | 243 | `移动设备` (34695) |
| `IF_TYPE_WWANPP2` | 244 | `移动设备` |
| — | — | `Wi-Fi Direct` (34696) / `WLAN Direct` (34697) —— **无对应 `IfType`**，需靠 `Description` 判断 |

（`ipifcons.h` 实测：`IF_TYPE_ETHERNET_CSMACD 6`@:49、`IF_TYPE_PPP 23`@:66、`IF_TYPE_SOFTWARE_LOOPBACK 24`@:67、
`IF_TYPE_TUNNEL 131`@:174、`IF_TYPE_IEEE80211 71`@:114、`IF_TYPE_IEEE80216_WMAN 237`@:240、
`IF_TYPE_WWANPP 243`@:241、`IF_TYPE_WWANPP2 244`@:242）

### `wlanapi` —— SSID / 信号强度（本机实测全部可用）

```cpp
DWORD neg; HANDLE h;
WlanOpenHandle(2, nullptr, &neg, &h);                 // 首次 4.17 ms，热态 0.75 ms
PWLAN_INTERFACE_INFO_LIST list;
WlanEnumInterfaces(h, nullptr, &list);                // 0.043 ms
// list->InterfaceInfo[i].InterfaceGuid / .strInterfaceDescription / .isState
WlanQueryInterface(h, &guid, wlan_intf_opcode_current_connection,
                   nullptr, &sz, (PVOID*)&data, &vt); // 0.164 ms, sz=604
auto* c = (WLAN_CONNECTION_ATTRIBUTES*)data;
//  SSID        = c->wlanAssociationAttributes.dot11Ssid.ucSSID（+ uSSIDLength）
//  信号强度     = c->wlanAssociationAttributes.wlanSignalQuality   （0..100 的百分比）
//  PHY         = c->wlanAssociationAttributes.dot11PhyType
//  rx/tx Kbps  = c->wlanAssociationAttributes.ulRxRate / ulTxRate
//  profile 名   = c->strProfileName
WlanFreeMemory(data);
WlanCloseHandle(h, nullptr);
```

**本机实测输出**：
```
iface 0 state=1 (connected) desc='TP-LINK Wireless USB Adapter'
WlanQueryInterface(current_connection) ret=0 size=604
ssid='HUAWEI-KY6PRW_5G' len=16  bssType=1  phyType=8  phyIdx=5
signalQuality=60%  rx=325000 Kbps  tx=292000 Kbps  securityEnabled=1
profile='HUAWEI-KY6PRW_5G'
```

`WlanGetAvailableNetworkList` 也能用（0.151 ms），本机返回 2 条。但**这个 API 返回的是"可用网络列表"，
不是"当前连的那个"** —— 要 SSID/信号强度应该用 `WlanQueryInterface(current_connection)`，
它的 `wlanAssociationAttributes` 直接就是当前关联信息。用 `WlanGetAvailableNetworkList` 再按 profile 名匹配
会拿到**错误的信号值**（本机实测：available list 里两条 `signalQuality=1%`，而 current_connection 报 `60%`）。

**`WLAN_AVAILABLE_NETWORK` 的 `sizeof` = 628 字节**（我按 SDK 字段顺序算过：512 + 36 + 4×12 + 32 + 4 + 4 + 4×4）。
本机按 628 步进读出的 profile 名与 SSID 正确；但**信号字段读出来是 1% 而真值 60%**，
说明**该结构在我手算的偏移上还有对齐差异**（`DOT11_SSID` 是 36 字节，后面 `WLAN_REASON_CODE` 是 ULONG 无对齐问题，
但 `DOT11_MAC_ADDRESS` 等可能引入 padding）。**结论：不要用 `WlanGetAvailableNetworkList` 取当前 SSID，
用 `WlanQueryInterface` —— 后者我用 `Marshal.PtrToStructure` 按 SDK 结构体布局验证过，值全部自洽。**

### 「服务提供商」（wwan）

`taskmgr.exe` **没有导入 `wlanapi.dll`**（我逐条列了 `docs/evidence/tm_imports.txt` 的全部 DLL：
`wcmapi.dll` 在列，`wlanapi.dll` **不在**）。它导入的网络相关 API 只有：
- `wcmapi.dll`（Windows Connection Manager：`WcmQueryProperty` / `WcmGetProfileList` / `WcmFreeMemory`）
- `NSI.dll: NsiGetParameter`
- `netutils.dll`

**这是一个重要线索**：原版取 SSID / 信号强度**很可能走的不是 `wlanapi`，而是 `wcmapi` 或
`NsiGetParameter` 的 `Nl80211` 接口**。`wcmapi.h`（SDK 里有，173 行）的 `WCM_PROPERTY` 枚举**只有**
`connection_cost` / `dataplan_status` / `hotspot_profile` 三类，**没有 SSID/信号**。
`WcmGetProfileList` 能给出 `WCM_PROFILE_INFO { strProfileName, AdapterGUID, Media }`，
其中 `Media == wcm_media_wlan` 的 profile 名**通常就等于 SSID**（本机 `profile == ssid == 'HUAWEI-KY6PRW_5G'`）。

所以「服务提供商」出现在原版网络页，说明原版确实支持 WWAN（`wcm_media_mbn`）。
本机无 WWAN 硬件，**这条完全未验证**。可行降级：无 WWAN 时显示 `—`。

### 实现步骤

**A. `include/perfdata.h`** — `NetSample` 加字段：

```cpp
struct NetSample {
    bool hasName = false;
    std::wstring name;               // FriendlyName
    double sendMbps = 0.0, recvMbps = 0.0;
    uint64_t linkSpeedBps = 0;

    // ---- E-28 新增 ----
    uint64_t luid = 0;               // IP_ADAPTER_ADDRESSES.Luid（与 gpuinfo 的 LUID 同域，可用于关联）
    uint32_t ifType = 0;             // IfType 原值
    uint32_t operStatus = 0;         // IF_OPER_STATUS 原值
    std::wstring description;        // Description（用于识别 "Virtual"）
    std::wstring dnsSuffix;          // DnsSuffix（「DNS 名称:」）
    std::wstring ipv4;               // 第一个非链路本地的 IPv4；多值用 L", " 拼接
    std::wstring ipv6;               // 第一个全局 IPv6；多值拼接
    bool     isWireless = false;     // IfType==71 且非 Wi-Fi Direct
    std::wstring ssid;               // wlanapi，非无线为空
    int      signalQuality = -1;     // 0..100，-1 = 不可用
    std::wstring provider;           // WWAN 服务提供商，本机恒空
};
```

**B. `src/perfdata.cpp`** — 在 `BuildIfNameMap()` 旁边新增 `FillAdapterDetails(NetSample&)`，
把现有的 `GetAdaptersAddresses` 调用**从 `GAA_FLAG_SKIP_DNS_SERVER` 改成不 skip**（要 DNS），
并改用一个 `std::unordered_map<uint32_t /*IfIndex*/, AdapterDetail>` 缓存，`GetIfTable` 主循环里按 index 查表填充。

**这一步会显著改变成本**：`GetAdaptersAddresses` 是 **11.8 ms（skip DNS）/ 16.9 ms（含 DNS）**，
而现有代码只调一次（`BuildIfNameMap` 里，且只在第一轮），主循环用的是 `GetIfTable`（<1 ms）。
若把 `GetAdaptersAddresses` 搬到主循环 = 每秒多 17 ms，**这是不可接受的**。
正确做法：**静态信息（IP/DNS/IfType/SSID）每 30 秒刷一次**，动态速率仍走 `GetIfTable` 差分。

**C. `wlanapi` 的调用位置**：独立函数 `RefreshWlanInfo()`，每 30 秒一次：
```
WlanOpenHandle → WlanEnumInterfaces → 对 isState==1 的接口 WlanQueryInterface(current_connection)
→ 匹配到 NetSample（用 strInterfaceDescription == Description，或 GUID ↔ 注册表 Connection\Name == FriendlyName）
→ WlanCloseHandle
```
接口匹配**必须靠 `Description`**（本机 `'TP-LINK Wireless USB Adapter'` 与 `GetAdaptersAddresses` 的 `Description` 逐字相同），
或者用 `GUID` 查注册表 `HKLM\SYSTEM\CurrentControlSet\Control\Network\{4d36e972-...}\<GUID>\Connection\Name` 得到 FriendlyName
（本机实测该键下 `Name = "WLAN 2"`，与 `FriendlyName` 一致）。
**`FriendlyName` 与 `strInterfaceDescription` 不同名**（一个是 `WLAN 2`，一个是 `TP-LINK Wireless USB Adapter`），
不能直接字符串相等匹配。

### 成本

| 项 | 耗时 | 频率建议 |
|---|---|---|
| `GetAdaptersAddresses`（含 DNS，全量 8 适配器） | **16.9 ms** | **每 30 秒一次**，绝不每帧 |
| `WlanOpenHandle` + `WlanCloseHandle` | 0.75 ms | 同上（或进程内长期持有 handle） |
| `WlanEnumInterfaces` | 0.043 ms | 同上 |
| `WlanQueryInterface(current_connection)` | 0.164 ms | 同上 |
| `WlanGetAvailableNetworkList` | 0.151 ms | **不需要**（见上） |
| 以上合计 | **≈18 ms / 30 s** | 折算 0.6 ms/s，可忽略 |

若要每帧刷新 IP，**必须**改用缓存 + 只刷 `WlanQueryInterface`（0.164 ms），
`GetAdaptersAddresses` 绝不能进 1 Hz 路径。

### 权限
- `GetAdaptersAddresses`：**不需要管理员**。
- `wlanapi`：**不需要管理员**，但**需要 WLAN AutoConfig 服务（`WlanSvc`）在运行**。
  本机实测 `WlanSvc = Running / Automatic`，`WlanOpenHandle` ret=0。
  若服务未启动，`WlanOpenHandle` 返回 `ERROR_SERVICE_DEPENDENCY_FAIL (1068)` —— **未验证**（本机服务在跑）。
  实现必须判返回值并降级为"无 SSID"。
- 「服务提供商」（WWAN）：未验证。

### 风险
1. **`GetAdaptersAddresses` 16.9 ms 放进 1 Hz 采集线程 → 每秒白烧 1.7% 单核**。必须每 30 秒。
2. **`IPv6` 有多个全局地址**（本机 WLAN 有 3 个全局 + 1 个链路本地）。原版显示几个？未证实。
   建议只取第一个全局（跳过 `fe80::` 开头的链路本地），并在注释里写明"多地址只显示第一个，原版行为未证实"。
3. **`TransmitLinkSpeed = INT64_MAX`** 在未连接/虚拟适配器上是常态（本机 8 条里 3 条如此）。不处理会显示天文数字。
4. **`OperStatus` 值含义**：本机 `1=Up`、`2=Down`。SDK 的 `IF_OPER_STATUS` 枚举是
   `IfOperStatusUp=1, IfOperStatusDown=2, IfOperStatusTesting=3, ...`，所以 1/2 是对的；
   但**现有代码用的是 `GetIfTable` 的 `dwOperStatus != 5`**（`MIB_IF_OPER_STATUS_OPERATIONAL=5`），
   两套编码不同，**不能混用**。新代码若改用 `GetAdaptersAddresses` 的 `OperStatus`，判据要改成 `== 1`。
5. **`Wi-Fi Direct` 虚拟适配器也报 `IfType=71`**（本机 2 条），会被误判成无线网卡并去查 `wlanapi`（查不到，返回错误）。
   必须用 `Description` 含 "Virtual"/"Wi-Fi Direct" 排除。
6. **`wlanapi` 的 `wlanAssociationAttributes` 结构偏移**：我实测踩过一次坑 ——
   手算 `WLAN_CONNECTION_ATTRIBUTES` 的偏移（`strProfileName[256]` 是 512 字节，assoc 从 +520 起）
   能读出正确的 SSID（16 字符）和正确的 signalQuality(60)，但用同样的手算偏移去读
   `WLAN_AVAILABLE_NETWORK` 就拿到错误的信号值。**结论：能用 SDK 结构体就不要手算偏移。**
   工程里 `gpuinfo.cpp` 已有「SDK 结构体 + `#pragma pack` + `static_assert`」的成熟做法，照抄即可。
7. **本机 7 条适配器里只有 2 条是真实网卡**（Realtek 有线 dis­connected + TP-LINK 无线 up），
   其余 5 条是 VMware×2、Radmin VPN、Wi-Fi Direct×2、Loopback。现有 `perfdata.cpp:720-742` 的
   过滤规则（`dwSpeed==0` + `operStatus!=5` + `dwType∈{6,71}` + 累计流量 >1 MB + 必须有名字）
   在 `GetIfTable` 视图上有效；**换到 `GetAdaptersAddresses` 视图后这套规则不成立**（它没有累计流量字段）。
   建议：**保留现有 `GetIfTable` 的过滤 + 速率计算不动**，`GetAdaptersAddresses` 只用来补充静态字段（按 IfIndex 关联）。

### E-28 可行性结论
**拆成三档：**
- **IPv4 / IPv6 / DNS 名称 / 连接类型 / 适配器名称：直接可做**，`GetAdaptersAddresses` 全部字段本机验证。
- **SSID / 信号强度：直接可做**，但要 `wlanapi`（`WlanQueryInterface` + `current_connection`），
  本机实测 0.164 ms，且需要 `WlanSvc` 在跑。**不要用 `WlanGetAvailableNetworkList`**。
- **服务提供商（WWAN）：本机无硬件，完全未验证。** 只能降级为 `—`。
  原版走的可能是 `wcmapi.dll`（`taskmgr.exe` 导入它、不导入 `wlanapi.dll`），但 `wcmapi.h` 的属性枚举里没有 SSID/信号。

---

## 4. E-19 / E-20 / E-21 GPU 引擎（6 型）

### 目标
- **E-19**：GPU 页「多个引擎」视图从 4 格扩到 N 格，本机 6 型全显示（当前丢 Overlay / Other）
- **E-20**：单引擎窗格 vs 多引擎窗格**二选一**（原版是两个平级互斥元素 `singleEngineGpuView` / `multiEngineGpuView`）
- **E-21**：引擎格要有刻度（当前各自自适应量程，3% 和 100% 长得一样）

### 引擎列表怎么枚举 —— 两条路径都实测了，结论是**继续用 D3DKMT**

| 路径 | 本机实测 | 结论 |
|---|---|---|
| **D3DKMT**（现有 `gpuinfo.cpp`） | Init 12.79 ms，稳态 Collect **0.905 ms**（p50 0.589），6 引擎 | ✅ 保留 |
| **PDH `\GPU Engine(*)\Utilization Percentage`** | 首次 expand **7,100 ms**（483 实例）；`_probe` 记录 1350 ms（525 实例） | ❌ 灾难级，绝不能用于 UI 路径 |

D3DKMT 的引擎枚举逻辑在 `src/gpuinfo.cpp:790-836`：
对每个 node 调 `D3DKMT_QUERYADAPTERINFO` + `KMTQAITYPE_NODEMETADATA(25)`，
拿 `DXGK_NODEMETADATA.EngineType`，按类型分组，再按 `EngineDisplayOrder()` 排序。

### PDH 实例名格式（本机实测，回答任务书的问题）

本机 483 个实例，实测样本：
```
pid_10020_luid_0x00000000_0x000136d4_phys_0_eng_0_engtype_3d
pid_10020_luid_0x00000000_0x000136d4_phys_0_eng_1_engtype_legacyoverlay
pid_10020_luid_0x00000000_0x000136d4_phys_0_eng_10_engtype_copy
pid_10020_luid_0x00000000_0x000136d4_phys_0_eng_13_engtype_security_1
```

**格式**：`pid_<十进制PID>_luid_0x<高32位 8 hex>_0x<低32位 8 hex>_phys_<物理适配器号>_eng_<node号>_engtype_<小写类型名>`

解析要点：
- PID 在 `pid_` 后到下一个 `_` 之间，十进制
- LUID 是**两段** `0x%08x_0x%08x`（高、低），合成 `LUID{LowPart, HighPart}` 时注意顺序（实例名里**先高后低**）
- `phys_` 是物理适配器号
- `eng_` 是 node 序号（**不是引擎类型**）
- `engtype_` 是类型名，**小写、无空格**，且**可能出现 `_1` 后缀**（`security` / `security_1`）

本机实测的 `engtype` 取值分布（483 实例）：
| engtype | 实例数 |
|---|---|
| `copy` | 189 |
| `3d` | 132 |
| `security` | 27 |
| `videoencode` | 27 |
| `vr` | 27 |
| `security_1` | 27 |
| `legacyoverlay` | 27 |
| `videodecode` | 27 |

**引擎类型名与显示名对照（`taskmgr.exe` `.rdata` 0x0F3390 起，逐字节读出，硬证据）**：

```
Crypto \0\0 Legacy Overlay \0\0 Scene Assembly \0\0 Video Processing \0\0\0\0
Copy \0\0\0\0 Video Encode \0\0\0\0 Video Decode \0\0 3D \0\0\0\0
```
（本轮我用 `[IO.File]::ReadAllBytes` + UTF-16 解码在 `0xF3390` 处**独立复核过**，与 `docs/evidence/win10_perf_visual_spec.md:571` 的记录逐字一致。）

**6 型（本机）/ 9 型（DXGK_ENGINE_TYPE 全集）的对照表**：

| `DXGK_ENGINE_TYPE` | PDH `engtype_` | 原版显示名 | 本机有此引擎 |
|---|---|---|---|
| 0 Other | （无独立实例名，归 `engtype_other`） | `Other` ⚠ | ✅（3 个 node） |
| 1 3D | `3d` | `3D` | ✅ |
| 2 VideoDecode | `videodecode` | `Video Decode` | ✅ |
| 3 VideoEncode | `videoencode` | `Video Encode` | ✅ |
| 4 VideoProcessing | （`engtype_videoprocessing`，本机无） | `Video Processing` | ❌ |
| 5 SceneAssembly | （`engtype_sceneassembly`，本机无） | `Scene Assembly` | ❌ |
| 6 Copy | `copy` | `Copy` | ✅（7 个 node） |
| 7 Overlay | `legacyoverlay` | `Legacy Overlay` ⚠ | ✅ |
| 8 Crypto | （`engtype_crypto`，本机无） | `Crypto` | ❌ |

⚠ **两个已知不确定点：**
1. **`Other`（type 0）没有显示名**。 `_rdata` 那张表里 `3D` 是最后一条，前面没有 `Other`。
   但 `gpuinfo.cpp:422` 现在返回 `L"Other"`。本机 `docs/evidence/FINAL.txt` 也报 `Other type=0 nodeCount=3`。
   **原版是否显示 `Other` 这一格，未证实。**
2. **`Overlay`（type 7）在 PDH 里叫 `legacyoverlay`，而 `_rdata` 表里写的是 `Legacy Overlay`。**
   两者一致。但 `gpuinfo.cpp:429` 返回的是 `L"Overlay"`（少了 `Legacy ` 前缀），**与 `_rdata` 不符**。
   这是 E-19 里一个可以直接修的字符串 bug：`case kEngineOverlay: return L"Overlay";` → `L"Legacy Overlay"`。
3. `security` / `security_1` / `vr` 三种 PDH 类型**在 `_rdata` 表里没有**，
   映射到 `DXGK_ENGINE_TYPE` 时会被归入某类（`security` 可能是 type 8 Crypto？本机 `Crypto` 有 1 个 node —— 
   `docs/evidence/FINAL.txt` 的 6 引擎列表里没有 Crypto，所以 `security` 大概被驱动报成了 Overlay 或 Other）。

### `kEngineSlots` 改成多少

**建议改成 8。** 理由：
- `DXGK_ENGINE_TYPE` 全集是 0..8 共 9 种，但 type 4/5 极罕见（Video Processing / Scene Assembly 在消费级卡上几乎不出现）
- 本机实际 6 种；`_rdata` 显示名表是 8 条（Crypto / Legacy Overlay / Scene Assembly / Video Processing / Copy / Video Encode / Video Decode / 3D）——
  **没有 `Other`**，所以"原版最多显示 8 格"
- 8 格正好 2 行 × 4 列（或 4 行 × 2 列），布局自然
- 内存代价：`double gpuEngine[8][60]` = 3840 B，可忽略（现在 4 格是 1920 B）

**超出怎么办**：`gpuinfo` 的 `Engines()` 是按实际 node 聚合的，理论上最多 9 条。
若驱动报了 9 种（type 0..8），第 9 种**丢弃**并 `#ifdef _DEBUG` 里 `OutputDebugStringW` 记一笔。
**不要让 `PerfHistory::gpuEngine` 越界** —— `set` 侧要有 `if (slot >= kEngineSlots) break;`。

### 实现步骤

**A. `include/perfpage.h`（E agent 的文件）**：
```cpp
static constexpr int kEngineSlots = 9;      // 当前是 4（include/perfpage.h:114）
// 槽位不再按"固定 4 型"映射，直接令 slot == engineType：
//   0=Other 1=3D 2=VideoDecode 3=VideoEncode 4=VideoProcessing
//   5=SceneAssembly 6=Copy 7=Overlay 8=Crypto
// 9 个槽 × 60 点 × 8 B = 4320 B（现在是 1920 B），可忽略。
```

**⚠ 这里有个设计抉择必须写清楚**：现在 `gpuEngine[e]` 的 `e` 是**显示序号**（0..3），
`SetData` 里用 `wantedTypes[]{1,6,3,2}`（`perfpage.cpp:184`）把 engineType 映射到序号。
扩到 6/8 型后，**建议直接令 `slot == engineType`，槽位 0..8 共 9 个**，
`SetData` 变成 `hist_.gpuEngine[eng.engineType][slot_] = eng.utilPercent;`，
`PaintEngineChart` 直接用 `engineType` 索引。这样：
- 不需要维护 `wanted[]` / `switch(engineType)` 两处映射（现有代码里 `SetData` 的
  `wantedTypes[]{1,6,3,2}`（`perfpage.cpp:184`）和 `PaintEngineChart` 的 `switch (engineType)`
  （`perfpage.cpp:1599-1605`）**各维护一份**，是典型的"改一处忘另一处"隐患）
- 9 个槽 × 60 点 × 8 B = 4320 B，仍然可忽略
- 代价：`kEngineSlots` 得是 9（type 值 0..8），不是 8

**B. `src/perfpage.cpp`（E agent）**：
- `SetData`（`perfpage.cpp:184`）：删掉 `wantedTypes[]`，直接 `hist_.gpuEngine[std::min(eng.engineType, kEngineSlots-1)][slot_] = ...`
- `PaintGpuPage:1497-1503`：删掉 `struct EngineSlot wanted[4]`，改成遍历 `snap_->gpuEngines`
  （它已经是排序好、只有真实存在的类型），格数 = `gpuEngines.size()`，格子按 2 列自适应
- `PaintEngineChart`（`perfpage.cpp:1599-1605`）：删掉 `switch (engineType)` 求 slot，直接用 `engineType`
- **E-21**：`PaintEngineChart` 里给每格加上 `100%` / `0%` 或 `上限值`，量程改用**固定档位**而不是"本格历史最大值"

**C. `gpuinfo.cpp`（采集层，Phase 2 范围内，但注意它现在无人拥有）**：
- `EngineTypeDisplayName(7)` 从 `L"Overlay"` 改成 `L"Legacy Overlay"`（依据 `.rdata` 0x0F33A0）
- 考虑给 `GpuEngineSample` 加 `bool displayable`（用于过滤 `Other`）

### 成本
- D3DKMT 路径：Init 12.79 ms（含一次 EnumProcesses），稳态 **0.905 ms**（`Collect(pids, count)` 重载，n=527）。
  `docs/evidence/FINAL.txt` 记录 avg=0.905 / p50=0.589 / p95=4.078 / max=4.078 ms。
- **扩到 6/8/9 槽后成本不变** —— 采集端本来就是枚举全部 node 再聚合，`perfdata` 只是多拷几条 `GpuEngineSample`。
  内存增长：`gpuEngine[9][60]` = 4320 B。
- **每帧调用**：不是每帧，是每采集周期（1 Hz）。**不要每帧**（`SetData` 每帧跑一次也算，但它只是拷 9 个 double）。

### 权限
**不需要管理员。** `D3DKMT_*` 是 `gdi32.dll` 的未文档化导出，普通用户可用
（`docs/evidence/FINAL.txt` 与 `gpuinfo.h:12` 都写明"不需要权限，普通用户也能用"）。
PDH 路径也不需要管理员，但太慢，不考虑。

### 风险
1. **`kEngineSlots=4 → 9` 会改变 `PerfHistory` 的 `sizeof`**。`PerfHistory` 是 `PerfPage` 的成员、
   不是 POD 传输结构，所以**不影响采集层接口**。但要确认没有人 `memcpy` 它。
2. **`Other`（type 0）该不该显示** —— 本机有 3 个 node 报 Other。若显示，多一格常数 `Other 0%` 永远不动。
   建议显示（原版 8 格表里没有 Other，但原版也可能不显示它）。**标未证实**。
3. **`security` / `vr` 两种 PDH 类型在 D3DKMT 里没有对应 `DXGK_ENGINE_TYPE`**，
   而 `gpuinfo` 走 D3DKMT 拿的是驱动报的 `EngineType` —— 本机报出 6 种（含 Overlay/Other），
   **PDH 报的是 8 种**。两者不一致说明**驱动对同一 node 的 PDH 名字与 DXGK 类型不是一一对应**。
   不要试图用 PDH 名字反推 DXGK 类型。
4. **E-20（双窗格互斥）的"哪个是默认"未证实**。UIFILE 里 `singleEngineGpuView` 在前、
   `multiEngineGpuView` 在后，但没有任何可见性标志。**建议默认多引擎**（6 格），
   并按 D-14 的菜单项（`多个引擎`）切换。但这属于 UI 侧，不是采集层。
5. **E-21（引擎刻度）需要"档位表"**，而引擎利用率是 0–100%，所以刻度就是 `100%`/`0%` 两个标签 +
   可选中间网格线。不需要走 E-12 的速率档位表。

### E-19/E-20/E-21 可行性结论
**E-19：直接可做。** 只需把 `kEngineSlots` 从 4 改到 9（或 8+特殊处理），
删掉两处硬编码的 `wanted[]` / `switch`，顺带修 `Overlay → Legacy Overlay` 的字符串 bug。
采集端已经给出 6 引擎，`docs/evidence/FINAL.txt` 是本机实测证据。
**E-20：可做，但"默认哪个窗格"未证实**，属于 UI 决策。
**E-21：可做**，只是给 `PaintEngineChart` 加刻度。

---

## 5. B-03 / B-04 详情页排序与拖宽

### B-03 目标
详细信息页列头可点击排序，6 列全可排，箭头 + 升降序，与原版 MUI 2322 的
`已排序(升序)` / `已排序(降序)` / `无` 一致。

### 排序键类型（48 列全集的分类，本机 6 列默认可见的在前）

依据 `docs/evidence/roast2/08_details.md` §1.2-B 的 48 列清单，按"排序键类型"归类：

| 池索引 | 列名 | 默认可见 | 排序键类型 | 取值字段（`ProcessEntry`） | 需要的额外 API |
|---|---|---|---|---|---|
| 0 | 名称 | ✅ | **字符串**（文件描述，自然序） | `imageName` → `ProcessDisplayName(pid, imageName)` | 无（已有） |
| 2 | PID | ✅ | **数值**（uint32） | `pid` | 无 |
| 3 | 状态 | ✅ | **枚举**（三态 rank） | `cls_.suspended` / `cls_.notResponding` | 无 |
| 4 | 用户名 | ✅ | **字符串**（`domain\user`） | `ResolveUser(pid)` | ⚠ 见下 |
| 7 | CPU | ✅ | **数值**（double %） | `cpuPercent` | 无 |
| 13 | 内存(活动的专用工作集) | ✅ | **数值**（uint64 字节） | `privateBytes ? privateBytes : workingSet` | 无 |
| 1 | 程序包名称 | ❌ | 字符串 | 无字段 | `NtQueryInformationProcess(ProcessAppPackageName=?)` 未验证 |
| 5 | 会话 ID | ❌ | 数值 | `sessionId` | 无 |
| 6 | 作业对象 ID | ❌ | 数值 | 无字段 | `IsProcessInJob` + `QueryInformationJobObject` |
| 8 | CPU 时间 | ❌ | 时间（100ns） | `kernelTime + userTime` | 无 |
| 9 | 周期 | ❌ | 数值（`CycleTime`） | 快照里有 `CycleTime` 但 `ProcessEntry` 没存 | 扩快照 |
| 10–12 | 工作集 / 峰值工作集 / 工作集增量 | ❌ | 数值 | `workingSet` / 快照有 `PeakWorkingSetSize` 未存 | 扩快照 |
| 14 | 内存(专用工作集) | ❌ | 数值 | `privateBytes` | 无 |
| 15 | 内存(共享工作集) | ❌ | 数值 | `workingSet - privateBytes`（推导） | 无 |
| 16 | 提交大小 | ❌ | 数值 | `pagefileUsage` | 无 |
| 17/18 | 分页/非分页缓冲池 | ❌ | 数值 | 快照有 `QuotaPagedPoolUsage` 未存 | 扩快照 |
| 19/20 | 页面错误 / 增量 | ❌ | 数值 | 快照有 `PageFaultCount` 未存 | 扩快照 |
| 21 | 基本优先级 | ❌ | 数值 | 快照有 `BasePriority` 未存 | 扩快照 |
| 22/23 | 句柄 / 线程 | ❌ | 数值 | `handleCount` / `threadCount` | 无 |
| 24/25 | 用户对象 / GDI 对象 | ❌ | 数值 | 无字段 | `GetGuiResources` |
| 26–31 | I/O 读取/写入/其他（及字节） | ❌ | 数值 | `readBytes` / `writeBytes` / 快照有 Other 未存 | 无 |
| 32 | 路径名称 | ❌ | 字符串 | `imagePath`（懒加载） | 已有 `LazyLoader` |
| 33 | 命令行 | ❌ | 字符串 | 无字段 | `NtQueryInformationProcess(60)` |
| 34 | 操作系统上下文 | ❌ | 字符串 | 无 | 未验证 |
| 35 | 平台 | ❌ | 枚举 | 无 | `IsWow64Process2` |
| 36 | 特权 | ❌ | 枚举 | 无 | `NtQueryInformationProcess` |
| 37 | UAC 虚拟化 | ❌ | 枚举 | 无 | `NtQueryInformationProcess(ProcessExecuteFlags=44)` |
| 38 | 描述 | ❌ | 字符串 | 同"名称"的文件描述 | 已有 |
| 39 | 数据执行保护 | ❌ | 枚举 | 无 | `GetProcessMitigationPolicy` |
| 40 | 企业上下文 | ❌ | 字符串 | 无 | 未验证 |
| 42 | 电源节流 | ❌ | 枚举 | 无 | 未验证 |
| 43–46 | 4 个 GPU 相关 | ❌ | 数值 | `gpuPercent` / `gpuDedicatedBytes`（已采） | 无 |
| 47 | 硬件强制堆栈实施的保护 | ❌ | 枚举 | 无 | `GetProcessMitigationPolicy` |

**用到的 Windows API 汇总**（按"默认 6 列"算）：**一个都不需要新增**。
六列全部有现成数据源：
- `imageName` / `pid` / `cpuPercent` / `privateBytes` / `workingSet` 都在 `ProcessEntry`
- 状态来自 `cls_.suspended` / `cls_.notResponding`
- 用户名来自 `ResolveUser(pid)`（`src/usercache.cpp`，**已有缓存**）

**关键实现点（不是"照抄进程页"就行）**：

1. **排序状态不能复用进程页的 `sortCol_` / `sortDesc_`**。
   `sortCol_` 的语义绑死在 `tmwindow_list.cpp` 的 `kCols[]`（7 列：名称/状态/CPU/内存/磁盘/网络/GPU），
   而详情页是 6 列且**列 1 是 PID、列 2 是状态**，索引完全对不上。
   必须新增独立成员：
   ```cpp
   int  detailSortCol_ = 0;        // 0..5，对应 kDetCols 索引
   bool detailSortDesc_ = false;   // 默认名称升序（现状）
   ```
   （`include/tmwindow.h` 是 F agent 的文件，需写在交接单里）

2. **`RebuildDetailRows()` 现在是硬编码 `imageName` 升序**（`tmwindow_details.cpp:427-430`）。
   拆成一个 6 分支的 comparator，**参照 `src/tmwindow.cpp:448-511` 的 `cmp` lambda 写法**——
   那段已经处理了"值相等时按 pid 兜底防抖动"，这是 600 进程每秒重排时**必须**的，
   否则行会每帧上下跳。直接照抄那个 `return a->pid < b->pid;` 兜底。

3. **`detailRows_` 存的是 `const ProcessEntry*`，指向 `snap_->processes` 的元素**。
   快照每秒换一份，指针会失效 —— 但现有代码已经在 `PaintDetailsPage` 开头
   `if (detailRows_.empty()) RebuildDetailRows();`，且 `SetSnapshot` 后会清空。
   **排序本身不引入新风险**，但要注意：**排序必须在 `RebuildDetailRows` 里做，不能在 Paint 里做**。

4. **`ResolveUser(pid)` 在排序里被调用 = 性能陷阱**。
   6 列里"用户名"列排序需要对 600 个进程各调一次 `ResolveUser`。
   `usercache.cpp` 的缓存命中时是 `unordered_map::find`（O(1)），**首轮**会对 600 进程做
   `OpenProcess`+`OpenProcessToken`+`LookupAccountSid` —— 头部注释实测 **61 ms / 425 进程**。
   排序里调用它 = 每帧第一次排序多 61 ms。
   **做法**：排序前先把用户名字符串全解析进一个临时 `std::vector<std::wstring>`（或用 `userCache_` 预热），
   或者干脆在 `RebuildDetailRows` 里先把 `ResolveUser` 结果写进一个 `detailUserCache_` 再排序。
   （注：B-06 本来就要给 `userCache_` 加存活裁剪，两件事可以一起做。）

### B-04 目标
详情页列边界可拖动改宽。

### 列宽存哪里 —— 与 `TaskManagerPro_ColumnWidths` 的共存方案

**现状**：
- 进程页：`include/tmwindow.h:373` `int colWidths_[kSortColCount]`（7 个 int），
  存在 `HKCU\Software\Microsoft\Windows\CurrentVersion\TaskManager` 的
  **`TaskManagerPro_ColumnWidths`**（`REG_BINARY`，28 字节）（`tmwindow_list.cpp:168`）
- 详情页：`const DetCol kDetCols[]`（**`const`，编译期常量，无运行时容器**）
  + `kDetCols[i].width` 在 `DetColX()` 里被加和

**共存方案：新增一个独立的注册表值，不要塞进 `colWidths_`**。

理由（三条硬理由）：
1. `colWidths_` 的长度是 `kSortColCount=7`，含义是"进程页 7 列"。
   详情页是 6 列、**语义完全不同**（列 1=PID vs 列 1=状态）。塞进去会让 `LoadColumnWidths` 的
   `for (int i = 1; i < kSortColCount; ++i)` 校正循环把详情页的宽度当进程页的宽度校验。
2. `SaveColumnWidths()` 是**一次性把整个 `colWidths_` 写进 28 字节**。混装必须改格式 ⇒
   老用户升级后读到的 28 字节会被当成新格式解析 ⇒ 列宽错乱。**（这是真实的兼容性事故）**
3. 详情页可能扩到 48 列（B-03 的 Phase 2 后半），7 个 int 装不下。

**建议**：
```cpp
// perfdata 不管，属 tmwindow.h（F agent）
static constexpr wchar_t kRegValueDetails[] = L"TaskManagerPro_DetailColumnWidths";
int detailColWidths_[kDetColCount] = { 145, 50, 100, 80, 40, 80 };  // 默认值 = .rdata 里的宽度
void LoadDetailColumnWidths();
void SaveDetailColumnWidths();
```
- 值类型 `REG_BINARY`，长度 `sizeof(detailColWidths_)` = 24 字节，与进程页的 28 字节**不同**
  ⇒ 即使有人误读也会因长度校验（`cb == sizeof(...)`）被拒，不会静默解析错。
- 校正规则照抄进程页（`<40 || >600` → 回落默认），但**名称列例外**：
  详情页名称列原版是 145 固定（不是弹性），所以 6 列全部可拖。

**`const DetCol kDetCols[]` 必须去 const**（`tmwindow_details.cpp:106`）。
这是 B-04 的核心改动：`const DetCol[]` 编译期常量数组，运行时无法改宽。
改成运行时数组后，`DetColX(idx)` 要改成读 `detailColWidths_[i]`。

**命中测试与事件分发**（F agent 的文件）：
- 现有 `HitTestColResize(x, y)`（`tmwindow_list.cpp:206-222`）用的是
  `rcColHeader_` / `kColCount` / `NameColWidth()` —— 全部绑进程页。
- 详情页列头在 `rcContent_.top + 40`，高度 `kHeaderHeight(24)`
  （`tmwindow_details.cpp:92-93`），**不是** `rcColHeader_`。
- 必须在 `tmwindow_events.cpp` 的 `WM_LBUTTONDOWN` / `WM_MOUSEMOVE` / `WM_SETCURSOR` 里
  为 `TabId::Details` 加分支（现在三处都是 `if (tabs_.Current() == TabId::Processes)`）。
  **这是 F 的活**，B 只提供 `HitTestDetailColResize(x, y)` 和 `SetDetailColumnWidth(int, int)`。

### 成本
- 排序：`std::sort` 600 元素，6 种 comparator 里最贵的"名称"列要调 `ProcessDisplayName`（可能触发
  `GetFileDescription` 缓存未命中）。已有 `descCache_`。
  600 元素排序 ≈ `600 × log2(600) ≈ 5500` 次比较；名称列比较要 `NaturalCompare`（忽略大小写 + 数字按数值 + 中文拼音序）——
  **这是最贵的一种**。进程页现在已经在做同样的事（`RebuildRows` 每帧一次），所以**没有新增成本**。
- 拖宽：无常驻成本，只在 `WM_MOUSEMOVE` 里改一个 int。
- 注册表：`LoadDetailColumnWidths` 一次 `RegQueryValueExW`（≈0.1 ms），`SaveDetailColumnWidths` 只在拖拽结束时调用。

### 权限
不需要。

### 风险
1. **`const DetCol[]` 去 const 是 B-04 的前置条件**。不去 const，任何"运行时改宽"都编译不过。
2. **`kDetColCount` 是 `sizeof` 算出来的**，现在是 6。改成运行时数组后 `sizeof` 仍是 6，
   但若 B-09 同时改成具名枚举 + `static_assert`，两处改动会打架。**建议 B-09 先做，B-04 再做**。
3. **排序 + 拖宽同时存在时，`DetColX()` 每次绘制被调用多次**（`PaintDetailsPage:103/146/157/178/188/194` 共 6 处，
   且 `DetColX(i)` 内部是 `O(i)` 循环）⇒ 每行 6 次 `O(6)` = 36 次加法 × 30 行可见 = 1080 次/帧。
   可忽略，但**如果加了缓存就别忘了拖宽后失效**。
4. **拖宽后 `SaveColumnWidths` 的调用时机**：现有进程页在 `WM_LBUTTONUP` 和 `WM_CAPTURECHANGED` 两处都保存。
   详情页照抄即可，**不要只在 `WM_LBUTTONUP` 存**（拖拽中 Alt+Tab 会丢）。

### B-03/B-04 可行性结论
**两项都直接可做，且不需要任何新采集。**
- 排序：6 个排序键全部有现成数据（`ProcessEntry` + `cls_` + `ResolveUser`），
  唯一的坑是"用户名列排序会触发 61 ms 的 SID 解析"，需要预热或缓存。
- 拖宽：需要先 `const DetCol[]` 去 const，并新增独立的注册表值
  `TaskManagerPro_DetailColumnWidths`，**不要复用 `TaskManagerPro_ColumnWidths`**。

---

## 6. C-09 用户页子进程展开（Phase 2 部分）

### 目标
用户页每行变成「chevron + 9×9 `#E6E6E6` 首字母方块 + 用户名」，
展开后显示该用户会话下的进程列表（缩进）。

### 如何按用户把进程归组 —— 现有数据够，不需要新采集

**现成的键**：`ProcessEntry::sessionId`（`include/snapshot.h:40`）+ `UserSession::sessionId`
（`include/pages_data.h:51`）。
`PagesView::RefreshUsers()`（`pages_view.cpp:438-462`）已经在做按 `sessionId` 聚合：
```cpp
for (const auto& p : snap_->processes)
    for (auto& u : users_)
        if (static_cast<uint32_t>(p.sessionId) == u.sessionId) { ...aggregate...; break; }
```

**⚠ 这个聚合是 O(进程数 × 会话数)**：本机 596 进程 × 4 会话 ≈ 2400 次比较，可忽略。
但**它没有保存进程列表** —— 只累加了 `cpuPercent` / `workingSet` / `processCount`。
C-09 需要的是**列表**，所以要给 `UserSession` 加：
```cpp
std::vector<uint32_t> pids;   // 该会话下的 pid（按原版顺序：未证实，建议按内存降序或按名称）
```

**归组键的正确性（本机实测）**：
- 本机 596 进程分布在 `sessionId` 0（系统会话）和 1（当前交互会话）。
- 用户页 `UserSource::Enumerate` 枚举的是**会话**（`WTSEnumerateSessions`），
  用户页每一行是**一个会话**（不是"一个用户名"）。
- **原版是按"用户"还是按"会话"分组？** 本机只有一个活跃用户，**分不出来**。
  MUI 里 `34350 已使用的插槽:` 之类无关；`userstab` 的列是 `用户 / 状态 / CPU / 内存 / 磁盘 / 网络 / GPU`，
  "状态"列取的是 `WTS_SESSION_INFO.State`（活动/已断开/侦听）—— 这是**会话级**属性，
  所以**原版大概率是按会话分行**。标"未证实"。

### `ResolveUser(pid)` 的性能够不够（600 进程每帧一次）

**不够，绝对不能每帧调。** 数据来源 `src/usercache.cpp:1-6` 的实测注释：
> 「成本实测：425 个进程全量 OpenProcessToken + LookupAccountSid = 61ms，其中 SID→名字占 55.5ms」

现有实现是**带双层缓存**的：
- `userCache_[pid]` → pid 级缓存（首轮 61 ms，之后 O(1)）
- `sidNameCache_[sidKey]` → SID 级缓存（同用户多进程只查一次 SAM）

**所以稳态是 O(1) / 进程**，600 进程 × 1 次 `unordered_map::find` ≈ **微秒级**，够用。
**但是有个陷阱**：C-09 展开后要显示子进程行，若每个子行都要显示用户名，
而 `ResolveUser` 是 `TmWindow` 的成员（`include/tmwindow.h:444`），
**`PagesView` 拿不到**（它是独立类）。这需要把用户名字符串**提前塞进 `ProcessEntry` 或一个 `pid→user` map**。

**建议**：`PagesView::RefreshUsers()` 里不用 `ResolveUser`，改用 `UserSession::userName`（已经是会话级，
所有子进程共享同一个用户名）—— **一个会话下所有进程的用户名必然相同**，
所以子行**根本不需要**每进程解析。这比调 600 次 `ResolveUser` 便宜 600 倍。

### 展开后的子行：缩进 / 图标 / 列对齐

**本机原版裁切图的实测锚点**（`docs/evidence/roast2/09_pages.md:474-500`，三张不同倍数交叉验证）：

```
用户行（父行）：chevron 墨迹 x = 10.00..14.94（宽 4.94）
                9×9 灰方块 x = 32..41，色 #E6E6E6
                字母 'A'  x = 51..59
                用户名     x = 51 起
```

**子行没有原版样本** —— `docs/evidence/roast2/09_pages.md` 只说"原版用户页展开后显示该会话下的进程列表（缩进）"，
**没有任何像素依据**。所以：
- **缩进量**：`theme::kTreeIndent = 18` 是进程页的值（`_PLAN.md` §5.1 明确列为"证据不足，暂不改"）。
  用户页子行缩进**无原版依据** → 只能自拟。建议用 `51 + 18 = 69`（用户名起点 + 一个缩进），
  并**在注释里写明"缩进量无原版依据，自拟"**（这个项目最大的教训就是"注释说谎"）。
- **图标**：进程页有 16×16 应用图标（`DrawAppIcon`）。用户页子行应该是**进程的应用图标**，
  可以复用 `DrawAppIcon(hdc, rc, imageName, pid)`（`tmwindow_icons.cpp`）。
  **但 `PagesView` 现在没有应用图标缓存** —— 它只有一个 `iconCache_`，键是**路径**（`pages_view.cpp:411-417`，供启动页用），
  不是 `(imageName, pid)`。展开子行需要按进程取 16×16 图标，得新增一份键为 `imageName` 的缓存
  （进程页的做法见 `include/iconcache.h` + `DrawAppIcon`）。
- **列对齐**：用户名占第 1 列（弹性），子行也要占同一列。
  父行的 chevron/方块/文字锚点（10/32/51）在**第 1 列内部**，子行文字要对齐到第 1 列的某处。
  其余 6 列（状态/CPU/内存/磁盘/网络/GPU）**子行是否显示**？
  **原版未证实**。倾向：显示（进程页的子进程行就是这样），但这需要 `ProcessEntry` 的数据。

### 实现步骤（全部在 C agent 的文件 + 一处 F 的接口）

**A. `include/pages_data.h`**：
```cpp
struct UserSession {
    // ...已有...
    std::vector<uint32_t> pids;         // 新增：该会话下的进程 pid（未证实原版是否也这样存）
    std::vector<double>   cpuOf;        // 与 pids 平行，避免展开时再查快照
    std::vector<uint64_t> wsOf;
    bool expanded = false;              // 新增：本行是否展开
};
```

**B. `src/pages_view.cpp` `RefreshUsers()`**：把内层 `break;` 后改为同时 `u.pids.push_back(p.pid);`
（现在 `break` 之后什么都不做）。注意：**`pids` 要每轮 `clear()`**，否则会无限增长。

**C. `src/pages_view.cpp` `PaintUsers()`**：改成"父行 + 若 `expanded` 则插 `pids.size()` 个子行"的虚拟列表。
`HitRow(y, rowCount)` 现在是 `(y - listTop + scroll_) / kRowHeight`（`pages_view.cpp:401-407`），
**展开后行数不再等于 `users_.size()`**，必须改成遍历累加的行号映射。

**D. 点击 chevron 切换 `expanded`**：`OnClickUsers(x,y)` 现在只看 y（`(void)x`），
要改成先判 x 是否落在 chevron 的 [10,15] 区间。

**E. `SetSnapshot` 后要能重建**：`PagesView` 每轮重建 `users_` 时，**展开状态会丢**。
必须像 `appExpanded_` 那样按 `sessionId` 保存（`std::unordered_set<uint32_t> expandedSessions_`）。

### 成本
- 归组：已有 O(进程数 × 会话数)，本机 596×4 = 2400 次，**≈0**。加 `pids.push_back` 后仍是 O(n)。
- 用户名：**不调 `ResolveUser`**（用 `UserSession::userName`），成本 0。
- 图标：展开后每个子行一次 `DrawAppIcon`。现有进程页有 `icons_` 缓存（`include/iconcache.h`），
  `PagesView` 需要自己的缓存。**首次展开时** 会为每个新 exe 做一次 `SHGetFileInfoW`（进程页实测过，
  单个约 0.2–2 ms）。600 进程全展开 ≈ 数十到数百 ms，**必须惰性 + 缓存**，且按可见行绘制（已有虚拟化）。

### 权限
不需要。

### 风险
1. **`pids` 不 clear → 内存无限增长**（每秒 push 600 个）。必须每轮重建。
2. **展开状态丢失**：`RefreshUsers` 每轮 `src.Enumerate(users_)` 会**整体重建** `users_`
   （`pages_data_users_services.cpp:48-50` 里 `out.clear()`），`expanded` 和 `pids` 都会没。
   必须在重建后按 `sessionId` 恢复展开状态。
3. **行号映射**：`HitRow` / `OnClickUsers` / 滚动范围 / 滚动条拇指，**四处**都假设"行数 = `users_.size()`"。
   展开后一处不改就会点错行（本项目已有 B-01「点第 N 行选中第 N+1 行」的同类事故）。
4. **子行缩进、是否显示其余列、展开图标用哪个** —— **全部无原版依据**。
   建议按 `_PLAN.md` §5.1 的纪律：只在注释里写明"自拟，无原版样本"，并在报告里列为待证。
5. **`WTSEnumerateSessions` 不含 session 0 的 System 进程**？本机 596 进程里 sessionId=0 的有一大批
   （system/svchost 等）。`UserSource::Enumerate` 过滤 `SessionId >= 65536`（侦听器），
   但 session 0 是保留的。所以展开 session 0 会显示上百个系统进程 —— **原版是否这么显示，未证实**。

### C-09 可行性结论
**可直接做，数据全部现成（`sessionId` 关联），不需要新采集。**
硬约束只有一条：**`ResolveUser` 不能进子行路径**（用会话级用户名即可，语义上也对）。
其余（缩进量、子行是否显示其余列、图标）**无原版依据**，属"自拟并标注"。

---

## 7. D-18 资源值三组（`resourceValueMode_` 真正影响绘制）

### 目标
`资源值(&V)` 三组（内存/磁盘/网络）× 2 项（百分比/值），单选圆点。
菜单侧**已经做完了**（`menus.cpp:94-127` 的 `BuildResourceValueSubmenu` + `CheckMenuRadioItem`），
缺的是**绘制层消费它**。

### 现状（已复核）

```cpp
// include/tmwindow.h:216
int  resourceValueMode_ = 0;        // 0=百分比 1=值
// src/tmwindow_menus.cpp:955-956
MenuBuilder::ResourceValueMode() = g * 2 + v;
resourceValueMode_ = MenuBuilder::ResourceValueMode();
// menus.cpp:27  static int mode = 1;   ← 默认 1（内存→值）
```

**三处不一致**（这是必须先修的）：
1. `MenuBuilder::ResourceValueMode()` 的静态初值是 **`1`**（内存→值）
2. `TmWindow::resourceValueMode_` 的初值是 **`0`**（百分比）
3. `grep resourceValueMode_` 的结果：**只有 `tmwindow_menus.cpp` 的两行写入 + `tmwindow.h` 的声明，
   零读取点**

⇒ 启动时菜单圆点在「值」上，而 `resourceValueMode_` 是 0。**两个状态从一开始就分叉。**

### 「模式」如何真正影响绘制层

**注意语义**：原版的「资源值」是**三组独立**的，不是全局一个开关。
`resourceValueMode_` 现在的编码是 `group*2 + (0=百分比 1=值)`，**只能表示"最后一次点的那个组"**，
这是一处设计缺陷：点过网络组之后，内存组和磁盘组的选择就丢了。

**建议改成三元素**（`include/tmwindow.h`，F agent 的文件，写进交接单）：
```cpp
// 按组存：0=内存 1=磁盘 2=网络；值 0=百分比 1=值
// 默认全部 1（值），依据 docs/evidence/README.md 实拍圆点在「值」上
int resourceValueMode_[3] = { 1, 1, 1 };
int ResourceModeFor(int group) const { return (group>=0 && group<3) ? resourceValueMode_[group] : 1; }
```
`menus.cpp` 那边 `ResourceValueMode()` 是**单个 int**，要同步改成 3 个（或改成 `int&` 返回数组元素）——
**这是 menus.h 的接口变更，属 D agent**。

**要改的绘制函数（按影响面从大到小）**：

| # | 函数 | 文件 | 影响 |
|---|---|---|---|
| 1 | `FmtMemText` | `tmwindow_list.cpp:113` | 进程页「内存」列：`%.1f MB/GB` ↔ `%`（相对 `memTotalBytes_`） |
| 2 | `FmtDiskText` | `tmwindow_list.cpp:139` | 进程页「磁盘」列：`%.1f MB/s` ↔ `%`（相对热力图量程 `maxDisk_`） |
| 3 | `FmtNetText` | `tmwindow_list.cpp:146` | 进程页「网络」列：`%.1f Mbps` ↔ `%`（相对 `maxNet_`） |
| 4 | 内存列热力图染色 | `tmwindow_list.cpp:598-600` | 已经是 `mem / memTotalBytes_ * 100`，即**百分比模式下的取值**；数值模式下不该染色 |
| 5 | `PaintCards`（5 张卡片） | `tmwindow_paint.cpp:100-152` | 内存卡片 `%.1f GB` + `%.0f%%`；磁盘/网络卡片恒 `0%`（F-10 待修） |
| 6 | `PaintMemPage` 属性区 | `perfpage.cpp:1178-1200` | 内存页右侧 7 行（`速度:` 在 `:1190`） |
| 7 | `PaintDiskPage` 属性区 | `perfpage.cpp:1305-1315` | 磁盘页右侧 4 行（E-15 同批改） |
| 8 | `PaintNetPage` 属性区 | `perfpage.cpp:1425-1440` | 网络页右侧 5 行（E-28 同批改，`连接类型:` 在 `:1432`） |

**注意 #1–#3 有个共同点：它们只是"格式化"，不改变排序**。
`RebuildRows` 的 comparator（`tmwindow.cpp:448-511`）永远按**原始数值**排序，
**不能因为切了百分比就按百分比排** —— 但那两件事在数学上是等价的（同一分母），
所以**不用改 comparator**（内存/磁盘/网络三列，数值排序 = 百分比排序，因为分母是常量）。

### 两种模式各自怎么格式化

**值模式（现有行为，保持不变）**：
```
内存  FmtMemText : <1024KB → "%.1f K"；<1MB → "%.1f MB"；否则 "%.1f GB"
磁盘  FmtDiskText: <10 MB/s → "%.1f MB/s"；否则 "%.0f MB/s"
网络  FmtNetText : "%.1f Mbps"（无条件一位小数，依据原版 162.8 MB/s 同源证据）
```
（`FmtMemText` 的阈值来自 `tmwindow_list.cpp:113-137`，`FmtDiskText:139-144`，`FmtNetText:146-152`）

**百分比模式（新增）**：
```
内存  = memBytes / memTotalBytes_ * 100        → "%.0f%%"   分母 = snap_->totalPhysical（已有 memTotalBytes_）
磁盘  = diskBytesPerSec / maxDisk_ * 100       → "%.0f%%"   分母 = 热力图量程（全量最大，非本屏）
网络  = netBitsPerSec / maxNet_ * 100          → "%.0f%%"   分母 = 同上
```

**⚠ 「磁盘/网络的百分比」的分母是什么，是个真问题**：
- 内存有天然分母（物理内存总量）—— 无争议
- 磁盘/网络的"百分比"在原版里**是"占该资源总量的百分比"**（即磁盘活动时间 %、网络链路利用率 %），
  不是"占当前最大进程的百分比"。
- 本工程**没有**这两个总量：`DiskSample.activePercent` 现在是"吞吐/500 MB/s"的伪值（D1 已判定为错），
  `NetSample.linkSpeedBps` 有链路速度但只对有链路的适配器有效。
- **所以"磁盘/网络按百分比显示"在本工程里没有正确分母**。
  - 磁盘：可用 `DiskSample.activePercent`（但它是伪值）或 PDH `% Idle Time` 的反面（真实磁盘活动时间）
  - 网络：可用 `linkSpeedBps`（本机 WLAN 325 Mbps，但有 3 条适配器 `INT64_MAX`）

**建议**：D-18 的"磁盘/网络百分比"**必须和 E-15/E-28 的采集一起做**，
否则只能显示一个错误的分母。这是 D-18 被推到 Phase 2 的**真正原因** ——
不是"没法改绘制"，而是"没有正确的分母"。
**若必须在采集前先上 UI，则磁盘/网络两组百分比应置灰**（与 D-14 的处理方式一致），
比显示一个假百分比好。

### 实现步骤
1. **`include/tmwindow.h`**（F）：`resourceValueMode_` → `int resourceValueMode_[3]`，初值 `{1,1,1}`；
   加 `int ResourceModeFor(int group) const;`
2. **`include/menus.h` + `src/menus.cpp`**（D）：`ResourceValueMode()` 返回 `int[3]`（或三个引用）；
   `BuildResourceValueSubmenu` 的 `mode` 参数由 `int` 改成 `const int[3]`，
   每组独立判 `sel = modes[g]`。
3. **`src/tmwindow_menus.cpp`**（D）：`CMD_VAL_*` 分支里只改 `resourceValueMode_[g] = v;`，
   不再覆盖其它组。
4. **`src/tmwindow_list.cpp`**（A）：`FmtMemText` / `FmtDiskText` / `FmtNetText` 各加一个
   `bool asPercent, double denom` 参数（或新增 `FmtMemTextPct` 等），调用点 `tmwindow_list.cpp:658/666/674/682` 改传参。
5. **`src/tmwindow_paint.cpp`**（A）：`PaintCards` 的内存卡片按模式切换主数值。
6. **`src/perfpage.cpp`**（E）：三页属性区按模式切换。

### 成本
**0** —— 纯格式化分支，无新采集。

### 权限
不需要。

### 风险
1. **`resourceValueMode_` 从 `int` 改成 `int[3]`，所有引用点会编译错**。
   现在只有 2 个写入点（`tmwindow_menus.cpp:955-956`），改动面很小，但要同步 `menus.cpp` 的接口。
2. **默认值三方不一致**（`menus.cpp:27` 静态初值 `1`、`tmwindow.h:216` 初值 `0`、圆点实测在「值」）。
   **改的时候要一次对齐到 `{1,1,1}`**，否则启动第一帧的圆点和实际显示不符。
3. **磁盘/网络百分比缺正确分母**（见上）。这是 D-18 的实质障碍，
   **不是"改绘制层"能解决的**。若无 E-15/E-28 的采集，磁盘/网络两组百分比是假数。
4. **进程页热力图染色逻辑（`tmwindow_list.cpp:598-600`）在百分比模式下会重复表达**
   —— 染色本身就是"百分比可视化"，数字再显示百分比，等于同一信息两次。
   原版是否在百分比模式下关掉染色，**未证实**。

### D-18 可行性结论
**菜单层：已完成（Phase 1 就做完了）。绘制层：可做，但有一个真实障碍 —— 磁盘/网络的百分比没有正确分母。**
- **内存组：直接可做**（分母 = 物理内存总量，现成）。
- **磁盘组 / 网络组：需要额外条件** —— 必须先有真实的「磁盘活动时间 %」和「链路利用率 %」，
  而这两个正是 E-15（`% Idle Time`）与 E-28（`linkSpeedBps` 可用性）要解决的。
  在此之前建议把这两组置灰，不要显示假百分比。

---

## 8. 汇总

### 可立即实施（无硬障碍，数据源本机已验证）

| 项 | 关键结论 | 前提 |
|---|---|---|
| **E-15** | `IOCTL_*` + `GetVolumeInformationW` + `NtQSI(18)` 全覆盖；单次 0.27 ms；缓存一次永不刷新 | **需管理员**（`\\.\X:` 的 `GENERIC_READ`）；非管理员时「容量/类型」降级为 `—` |
| **E-16 static 组** | SMBIOS T16/T17 本机逐字段验证：2/8 槽、DIMM、1866 MHz、96 GB、64.4 MiB 保留 | 无 |
| **E-19 引擎 6 槽** | `kEngineSlots: 4→9`，删两处硬编码映射，顺带修 `Overlay→Legacy Overlay` | 无（D3DKMT 已给出 6 引擎） |
| **E-21 引擎刻度** | 给 `PaintEngineChart` 加 `100%/0%` | 无 |
| **B-03 详情页排序** | 6 列排序键全有现成数据；照抄 `tmwindow.cpp:448-511` 的 comparator（含 pid 兜底） | 新增 `detailSortCol_` / `detailSortDesc_`（F 的文件） |
| **B-04 详情页拖宽** | 需 `const DetCol[]` 去 const + 新增独立注册表值 `TaskManagerPro_DetailColumnWidths` | 不能复用 `TaskManagerPro_ColumnWidths`（长度/语义都不同） |
| **C-09 子进程展开** | `sessionId` 关联够用；用户名用**会话级**，不调 `ResolveUser` | 无原版依据的部分（缩进量）须自拟并标注 |
| **D-18 内存组** | 分母 = 物理内存总量，现成 | 其余两组见下 |

### 需要额外条件

| 项 | 条件 |
|---|---|
| **E-28 IPv4/IPv6/DNS/连接类型** | 可做，但 `GetAdaptersAddresses` **16.9 ms**，必须**每 30 秒**刷，不能进 1 Hz 路径 |
| **E-28 SSID/信号强度** | 可做，需 `wlanapi` + `WlanSvc` 运行；**必须用 `WlanQueryInterface(current_connection)`，不能用 `WlanGetAvailableNetworkList`**（后者本机实测信号值错：1% vs 真值 60%） |
| **D-18 磁盘/网络百分比** | **缺正确分母**。磁盘要真实的「活动时间 %」（E-15 的 `% Idle Time`），网络要可用的 `linkSpeedBps`。在此之前应置灰 |
| **E-20 GPU 双窗格** | "默认哪个窗格"**未证实**，属 UI 决策而非采集问题 |

### 硬障碍 / 本机无法验证

| 项 | 障碍 |
|---|---|
| **E-16 `(已压缩)` 的压缩字节数** | 本机 `MemoryCompression=False`，无 `MemCompression` 进程；`NtQSI` class 97 返回 `STATUS_INVALID_INFO_CLASS`；PDH 206 个计数器集里无此指标；扫遍 class 0..199 无可直接读出压缩大小的类。**降级走原版真实存在的「未启用内存压缩」分支**（MUI 33611 明文），不算编造 |
| **E-28 服务提供商（WWAN）** | 本机无 WWAN 硬件。原版导入 `wcmapi.dll`（不导入 `wlanapi.dll`），但 `wcmapi.h` 的 `WCM_PROPERTY` 枚举里没有 SSID/信号/运营商。只能降级为 `—` |

### 附带发现（Phase 2 之外，但会污染 E-16 的正确性）

**`src/perfdata.cpp:100-108` 的 `SYS_MEM_LIST` 结构体布局错误**：把 class 80 当成 ULONG 数组，
实际是 22 个 `SIZE_T`。本机实测后果：
- `freeBytes` 恒读成 0（读到 `ZeroPageCount` 的高 4 字节）
- `modifiedBytes` 读到的是 `FreePageCount` = 3,239 MB（真值 45 MB）
- `standbyBytes` = 28.4 MB（真值 9,834 MB，**差 346 倍**）

**这会让内存页组合条的"备用"段几乎看不见**，且「已修改」显示的是空闲内存量。
修法见 §0.3。**建议与 E-16 同批修，否则新加的 static 组会和错的 dynamic 组并排显示。**

---

## 9. 给下一轮的交接要点（谁拥有哪个文件）

| 改动 | 文件 | 归属 |
|---|---|---|
| `SYS_MEM_LIST` 布局修正 | `src/perfdata.cpp` | **无 owner**（Phase 2） |
| `DiskSample` / `NetSample` / `MemHardware` 新字段 | `include/perfdata.h` | **无 owner** |
| `CollectDiskStatic()` / `ReadMemoryHardware()` / `RefreshWlanInfo()` | `src/perfdata.cpp` | **无 owner** |
| `EngineTypeDisplayName(7) → "Legacy Overlay"` | `src/gpuinfo.cpp` | **无 owner** |
| `kEngineSlots: 4→9`、`gpuEngine` 索引改 `engineType` | `include/perfpage.h` + `src/perfpage.cpp` | **E** |
| `PaintEngineChart` 加刻度 | `src/perfpage.cpp` | **E** |
| 三页属性区（磁盘 5 项 / 内存两组 / 网络 8 组） | `src/perfpage.cpp` | **E** |
| `detailSortCol_` / `detailSortDesc_` / `detailColWidths_` 成员 | `include/tmwindow.h` | **F** |
| 详情页列头命中 / 拖动 / 排序事件分发 | `src/tmwindow_events.cpp` | **F** |
| `const DetCol[]` 去 const + 排序 comparator + `DetColX` 读运行时宽度 | `src/tmwindow_details.cpp` | **B** |
| `resourceValueMode_` → `int[3]` | `include/tmwindow.h` | **F** |
| `ResourceValueMode()` 接口改三组 | `include/menus.h` + `src/menus.cpp` + `src/tmwindow_menus.cpp` | **D** |
| `FmtMemText` / `FmtDiskText` / `FmtNetText` 加百分比分支 | `src/tmwindow_list.cpp` | **A** |
| `PaintCards` 内存卡片按模式 | `src/tmwindow_paint.cpp` | **A** |
| `UserSession::pids` / `expanded` | `include/pages_data.h` | **C** |
| `RefreshUsers` 保存 pids + 恢复展开态 | `src/pages_view.cpp` | **C** |
| `PaintUsers` 父行+子行 + `HitRow` 行号映射 | `src/pages_view.cpp` | **C** |

**本轮未编译、未运行、未改任何源码。** 所有 API 行为均为本机 SDK 头文件 + 本机 P/Invoke 实测，
不确定项已逐条标注「未验证 / 未证实」。
