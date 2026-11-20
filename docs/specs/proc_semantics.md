# Windows 10 任务管理器「进程」页 — 数据语义与分类规则

目标：给 C++/Win32 实现者一份可直接照做的规格。
测量机：Windows 10 Pro 22H2 22H2 / Xeon 服务器级 CPU (10C/20T) / 32GB
实测时刻：387 个 Session-1 进程 + 138 个 Session-0 进程，8456 线程

可信度标注：**[确证]** = 本机实测或权威文档；**[强推测]** = 从多组实测数据反推，逻辑闭合；**[推测]** = 从行为观察归纳，未获一手证据。

---

## 1. 三组分类规则

### 1.1 显示顺序与组标题

进程页从上到下固定三组：

```
应用
后台进程
Windows 进程
```

- 组标题右侧的数字是**该组包含的进程数**（不是线程数）。[强推测]
- 每组可折叠，折叠状态持久化在 `HKCU\Software\Microsoft\Windows\CurrentVersion\TaskManager\Preferences`（私有二进制格式，不可直接解析）。[确证：该键存在且为 REG_BINARY]
- 三组为空时不显示该组。

### 1.2 「应用」的判定 — 用枚举顶层窗口

这是三组里唯一有明确可复现判据的。[确证]

**判定算法**（本机实测可复现）：

```
EnumWindows 遍历所有顶层窗口，对每个 hwnd：
  pid        = GetWindowThreadProcessId(hwnd)
  visible    = IsWindowVisible(hwnd)
  exStyle    = GetWindowLongPtr(hwnd, GWL_EXSTYLE)
  style      = GetWindowLongPtr(hwnd, GWL_STYLE)
  owner      = GetWindow(hwnd, GW_OWNER)
  titleLen   = GetWindowTextLength(hwnd)

  若 visible
     且 (exStyle & WS_EX_TOOLWINDOW) == 0
     且 (style   & WS_CHILD)        == 0
     且 owner == NULL
     且 titleLen > 0
  → 该 pid 是一个「应用」根
```

**本机实测结果**：

```
TOTAL_TOPLEVEL_WINDOWS = 465
PID_WITH_APP_LIKE_WINDOW = 8
```

即 465 个顶层窗口里，只有 8 个 pid 满足「应用」条件。这 8 个的窗口类：

```
pid=12244  Chrome_WidgetWin_1
pid=23412  Chrome_WidgetWin_1
pid=20144  Chrome_WidgetWin_1
pid=21984  Chrome_WidgetWin_1
pid=17040  Qt51514QWindowIcon
pid=32740  Chrome_WidgetWin_1
pid=28800  Avalonia-4689b6ec-...
pid=14972  Windows.UI.Core.CoreWindow      ← UWP 应用走这条
```

**实测验证了排除规则的必要性**（若不排除会多算很多）：

| 窗口类 | 为什么必须排除 |
|---|---|
| `Shell_TrayWnd` | 任务栏，`WS_EX_TOOLWINDOW` |
| `com.ccswitch.desktop-sic` | Electron 的隐藏辅助窗口，`WS_EX_TOOLWINDOW` |
| `Tao Thread Event Target` | 线程事件窗口，`WS_EX_TOOLWINDOW` + titleLen=0 |
| `DummyDWMListenerWindow` / `WorkerW` / `Progman` | Explorer 的内部窗口，`WS_EX_TOOLWINDOW` |
| `Windows.UI.Core.CoreWindow` (pid 21364) | 这是 **child** 窗口（`WS_CHILD`），是某个 UWP 的子窗口，不算独立应用 |

**注意 pid=4804 同时拥有 `Shell_TrayWnd`(tool) / `Progman`(tool) / `WorkerW`(tool) / `DesktopWallpaperManager`(tool) / `EdgeUiInputTopWndClass`(tool) 五个窗口，全部被 `WS_EX_TOOLWINDOW` 排除，所以 Explorer 不会因任务栏被误判为「应用」——除非它有真实的文件夹窗口。**这条是本机实测最有力的反证。**

### 1.3 「应用」下的子进程归组

任务管理器会把同一应用的子进程折叠到应用名下（Chrome 的一个标签一个进程，但只显示一行 "Google Chrome"，带展开箭头）。[确证：行为可观察]

归组依据，两条都用：

1. **进程父子链**：从「应用根 pid」沿 `InheritedFromUniqueProcessId` 向下遍历，所有后代归入该应用。
2. **同一映像路径**（可选加固）：若后代路径与根进程路径在同一安装目录下，即使 parent 已退出也归入。

实现建议：先建 pid → parentPid 映射（`NtQuerySystemInformation(SystemProcessInformation)` 的 `InheritedFromUniqueProcessId` 字段），再做一次并查集/树展开。

**坑**：父进程退出后 `InheritedFromUniqueProcessId` 指向的 pid 可能已被重用。用 `CreateTime` 校验父进程创建时间早于子进程，可大幅降低误判。

### 1.4 「Windows 进程」的判定

**这是三组里最难精确复刻的一条。** 我的结论是**多条件组合**，而非单一规则。[强推测]

实测分布：

```
C:\Windows\ 下的进程      = 184
C:\Windows\ 之外的进程    = 302
无 ExecutablePath（受保护）= 24
```

184 明显大于任务管理器「Windows 进程」组的实际数量（通常 60–100 个），所以**单纯按路径分类是错的**。

最可信的组合判据，按优先级：

```
1. 属于 Session 0 且是系统服务宿主      → Windows 进程
2. image path 在 %SystemRoot%\System32\ 或 %SystemRoot%\SysWOW64\ 下
   且 不是用户启动的（无可见窗口、不是某应用的子进程）
                                          → Windows 进程
3. 关键系统进程白名单（绝对归 Windows 进程）：
   System(4), Registry, Memory Compression, smss, csrss, wininit,
   winlogon, services, lsass, svchost, dwm, fontdrvhost, conhost,
   WUDFHost, audiodg, sihost, taskhostw, ctfmon, SearchIndexer
4. 其余没有可见主窗口的              → 后台进程
```

**关键分界线**：`C:\Program Files\` / `C:\Program Files (x86)\` / 用户目录下的服务进程，即使没有窗口，也归**后台进程**而不是 Windows 进程。实测样本：

```
InOtherDir 样本（这些是「后台进程」，不是「Windows 进程」）：
  nvcontainer.exe              C:\Program Files\NVIDIA Corporation\...
  vmware-authd.exe             D:\VMware Workstation\...
  ToDesk.exe                   D:\ToDesk\...
  wslservice.exe               C:\Program Files\WSL\...
  clash-verge-service.exe      C:\Program Files\Clash Verge\...
  cowork-svc.exe               C:\Program Files\WindowsApps\...
```

同理，`C:\Windows\` 下但**属于用户会话、有窗口**的（如 `explorer.exe`）归「应用」而非「Windows 进程」。实测 pid=14972 的 `Windows.UI.Core.CoreWindow` 就在 Windows 目录之外却是应用。

### 1.5 UWP / AppContainer 进程归组

**[强推测]** UWP 应用（有 `Windows.UI.Core.CoreWindow` 顶层窗口的）归「应用」组。实测 pid=14972 即此类。

判定 AppContainer 的廉价方法：`GetTokenInformation(TokenIsAppContainer)`，或从 `NtQueryInformationProcess(ProcessSessionInformation)` + 包名（`GetPackageFullName`）。但**注意成本**：`GetPackageFullName` 要走 AppModel API，对 450 个进程逐个调不便宜——建议只对已有窗口的进程调。

---

## 2. 「状态」列

### 2.1 判定方法 — 用线程 WaitReason，不用 NtQueryInformationProcess

**[确证]** 本机实测：遍历所有进程的线程，看 `WaitReason`。

```
WaitReason 全机分布（8456 线程）：
  UserRequest        4740
  EventPairLow       1616
  Unknown            1346
  Executive           610
  Suspended           109   ← 挂起
  ExecutionDelay       58
  LpcReceive           27
  FreePage              5
  LpcReply              5
  PageOut               1
  VirtualMemory         1
```

**只有 109 个线程处于 Suspended**，分布在 3 个进程里：

```
pid=31556  ShellExperienceHost   threads=28
pid=4      System                threads=535
pid=12768  SearchApp             threads=61
```

但**「所有线程都挂起」的只有 2 个**：

```
FULLY_SUSPENDED  pid=12768  SearchApp             threads=61
FULLY_SUSPENDED  pid=31556  ShellExperienceHost   threads=28
```

第二个进程 `System`(pid=4) 有 535 个线程，其中只有部分处于 Suspended——它**不是**挂起进程。

### 2.2 结论：「已挂起」的判定规则

```
对该进程的每一个线程：
    若 WaitReason != Suspended → 不是挂起进程
若所有线程的 WaitReason 都 == Suspended → 显示「已挂起」
```

**[确证]** 这条规则精确命中了本机的 2 个真实挂起 UWP 应用（SearchApp、ShellExperienceHost），而排除了 System。这就是任务管理器显示「已挂起」的那两个应用。

**实现要点**：
- 数据源用 `NtQuerySystemInformation(SystemProcessInformation)` 返回的线程数组里的 `ThreadState` / `WaitReason` 字段，**不需要**额外调用。
  - `ThreadState == 5`（Waiting）且 `WaitReason == 5`（Suspended）→ 该线程挂起
  - 枚举常量：KTHREAD_STATE `Waiting=5`；KWAIT_REASON `Suspended=5`
- 只需要进程级一个 bool，不必给每个线程建结构。
- 边界：0 个线程的进程（正在退出）不要判为挂起。

### 2.3 其他状态值

**[推测]** 任务管理器「状态」列还可能出现：
- 空（正常运行的绝大多数进程）
- 「已挂起」
- 「已暂停」？（不确定是否存在，未见实测）

中文文案：`已挂起`。未发现其他非空状态的实测证据，建议实现时只输出「已挂起」或空。

---

## 3. 各列数值的来源与格式

### 3.1 CPU — 口径是全系统，不是单核

**[确证]** 三方面证据：

1. Windows 8 之后任务管理器明确改为「占全部逻辑处理器的百分比」。
2. 本机实测交叉验算：Ghidra 的 java 进程在 20 逻辑核上吃到约 8.9 个核，任务管理器口径下应显示约 44%。原型实测输出 `java.exe 44.29%`，与「全系统口径」吻合；若按单核口径应是 890%（不可能）。
3. 系统总 CPU 89% 时，全进程 CPU 求和为 98.4%（含 Idle 的 8.2%），口径自洽。

**计算公式**：

```
deltaCpu  = (KernelTime + UserTime) 本轮 - 上一轮        // 单位 100ns
deltaWall = 本轮墙钟 - 上一轮墙钟                        // 单位 100ns
cpuPercent = deltaCpu / deltaWall / numLogicalProcessors * 100
```

**格式**：
- 整数百分比，带 `%`。任务管理器对 0.1%–1% 之间的值会显示 1 位小数（如 `0.1%`），≥1% 显示整数，实质是四舍五入到 1 位小数后去尾。[强推测]
- **小于 0.05% 显示空**（不显示 `0%`）。这是任务管理器与普通表格的显著差异——进程页大量单元格是空白的，就是这个原因。[强推测，但空白单元格的行为确证]

**Idle 进程（pid 0）**：
- CPU 列显示 Idle 的占用（本机实测 8.2%–47.7% 波动）。
- **不算作「应用」或「后台进程」**，通常不出现在进程页，只在「性能」页的 CPU 图表里体现。
- 若要显示，用快照里 pid=0 那条记录的 KernelTime+UserTime 差分即可。

### 3.2 内存 — 是「活动专用工作集」，不是完整工作集

**[确证]** 任务管理器的内存列标题历史上是「内存(专用工作集)」，Windows 10 显示「内存」。

**取值**：`WorkingSetPrivateSize`（专用工作集），**不是** `WorkingSetSize`。

在本项目的数据源里：
- `NtQuerySystemInformation(SystemProcessInformation)` 返回的 `WorkingSetPrivateSize` 字段直接就是它，**无需额外调用**。
- 若该字段为 0（部分进程会这样），回退到 `WorkingSetSize`。

**格式**：
- 有单位自适应：`< 1 MB` 显示 KB（如 `520 K`），`>= 1 MB` 显示 MB（如 `12.4 MB`），`>= 1 GB` 显示 GB（如 `1.2 GB`）。
- 1 位小数。
- 实测样本：`8.0 KB` / `62.0 MB` / `1.1 GB`。

### 3.3 磁盘 — 从 IO 计数器差分

**[确证]** 数据源在快照里已有，无需额外调用：

```
ReadTransferCount / WriteTransferCount   // 累计字节数，100ns 记录的字节
deltaBytes  = 本轮 - 上一轮
bytesPerSec = deltaBytes / deltaSeconds
MBps        = bytesPerSec / 1048576
```

**格式**：
- 单位 MB/s，1 位小数，如 `1.2 MB/s`。
- 空闲时显示空（不显示 `0 MB/s`）。
- 注意：这是**文件 IO**，包含网络文件系统流量，与「性能」页的物理磁盘计数器不完全等价。

### 3.4 网络 — Windows 没有每进程网络计数器

**[确证，重要]** 本机实测性能计数器集合列表中，**不存在** per-process 网络计数器。可用的只有系统级：

```
Physical Network Interface Card Activity
Per Processor Network Interface Card Activity
TCPIP Performance Diagnostics
Network QoS Policy
```

**没有** `\Process(*)\Network Bytes/sec` 这类东西。

所以任务管理器的「网络」列**不能**靠 PDH 拿到。它实际是这么做的：订阅 ETW provider `Microsoft-Windows-Kernel-Network`（事件 10/11/12/13 对应 TCP/IP 收发），按 PID 聚合。[强推测，但「没有 PDH 计数器」是确证]

**给实现者的建议**：

| 方案 | 成本 | 建议 |
|---|---|---|
| ETW `Microsoft-Windows-Kernel-Network` 实时会话 | 中；需管理员权限；事件量在大流量下很大 | 若要精确，只能这条 |
| SRUM（`srumapi.dll`）| 低；但是按小时聚合的离线数据 | 不实时，不适合 |
| **不做每进程网络列，或全局置空** | 零 | **推荐第一版这么做** |

任务管理器本身在多数进程上网络列也是空的——这与「拿不到」一致。

### 3.5 GPU — PDH 可用，实例名里嵌 PID

**[确证]** 本机实测：

```
CounterSet 存在：GPU Engine / GPU Process Memory
\GPU Engine(*)\Utilization Percentage → 返回 525 个实例
实例名格式：pid_11092_luid_0x00000000_0x000146ec_phys_0_eng_0_engtype_3d
```

**实现**：
1. `PdhAddCounterW(L"\\GPU Engine(*)\\Utilization Percentage")`
2. 枚举实例名，用 `_wcsstr(inst, L"pid_")` 提取 PID
3. 同一 PID 可能有多个引擎（`eng_0`…`eng_11`，`engtype_3d` / `engtype_copy` / `engtype_video`），**求和**得到该进程的 GPU 利用率
4. `GPU Process Memory(*)\Dedicated Usage` 同理拿显存

**性能警告**（本项目已实测，必须遵守）：
- 525 个实例的通配符展开 + 采集**不要放在 UI 线程**，也不要 1 Hz 全量拉。
- 建议：**按需拉**（切到进程页才拉一次）或 **2–5 秒一次**，放后台线程。
- 本项目实测 `\Thread(*)` 在 8k 线程时是 947 ms/次；GPU 这 525 个实例虽远小于此，但仍不是免费操作。

**替代方案**：第一版可以只显示系统级 GPU 使用率（`\GPU Engine(_Total)\Utilization Percentage`），每进程 GPU 列留空。任务管理器在无 GPU 活动的进程上本来也是空的。

---

## 4. 热力图（heat map）算法

### 4.1 哪些列有热力图

**[强推测]** CPU、内存、磁盘、网络、GPU 这 5 列有单元格背景热力图。进程名、PID、状态、用户等文本列没有。

### 4.2 归一化方式

**不是**相对当前最大值归一化，也**不是**固定阈值。观察依据：单个进程 CPU 5% 时在满负载系统上颜色很浅，但在轻负载系统上同一进程 5% 颜色明显更深。[推测]

最符合观察的模型是**相对当前列的最大值**做归一化：

```
heat = value / max(value over all visible rows in this column)
```

这样每列总有至少一个格子是满色，视觉上「最热的那个最显眼」，与任务管理器的观感一致。

### 4.3 色带

**[推测]** 浅色主题下，从背景色（白/极浅灰）渐变到橙红：

| heat | 大致颜色 |
|---|---|
| 0.0 | 无背景（白） |
| 0.25 | 极浅橙 `#FFF4E5` |
| 0.5 | 浅橙 `#FFD9A0` |
| 0.75 | 橙 `#FFA94D` |
| 1.0 | 深橙红 `#E8590C` |

**未获一手 RGB 值**。建议实现时用 HSV 插值（H≈30° 橙，S 从 0.1→0.9，V 从 1.0→0.85）再微调，比硬编码色阶更容易匹配观感。

### 4.4 实现要点

- 热力图**只能对可见行计算归一化**（用全量 450 行会让滚动时颜色跳变）。任务管理器滚动时颜色是稳定的，说明它用全量。[推测] —— 建议用全量，避免滚动抖动。
- 单元格背景在文字之前 FillRect，文字用透明背景绘制（`SetBkMode(TRANSPARENT)`）。

---

## 5. 汇总卡片（顶部 5 张）

### 5.1 CPU 卡片

```
大标题：CPU
主数值：总 CPU 百分比       例 "12%"
副标题：当前主频            例 "3.90 GHz"
```

- 主数值来源：`(1 - Idle占用) * 100`，或全部进程 CPU 求和（两种等价）。
- 副标题主频来源：`NtPowerInformation(ProcessorInformation)` 或 `CallNtPowerInformation` 的 `CurrentMhz`，**注意 服务器级 CPU 无 Turbo，会恒定显示 3000 MHz**。
- 右侧迷你曲线：CPU 使用率时间序列。

### 5.2 内存卡片

```
大标题：内存
主数值：已用/总量 (百分比)   例 "8.2/31.9 GB (26%)"
副标题：速度 · 插槽 · 外形    例 "2666 MHz · 2/4 插槽 · DIMM"
```

- 主数值来源：`GlobalMemoryStatusEx` 的 `ullTotalPhys - ullAvailPhys` / `ullTotalPhys`。
- 副标题：`Win32_PhysicalMemory` 的 `Speed` / `DeviceLocator` 统计。
- 右侧迷你曲线：内存占用时间序列。

### 5.3 磁盘卡片

```
大标题：磁盘
主数值：活动时间百分比       例 "7%"
副标题：读/写速率            例 "1.2 MB/s · 0.3 MB/s"
```

- 活动时间：`\PhysicalDisk(_Total)\% Idle Time` 取补。
- 读写：`\PhysicalDisk(_Total)\Disk Read Bytes/sec` / `Disk Write Bytes/sec`。

### 5.4 网络卡片

```
大标题：网络
主数值：网络利用率百分比     例 "0%"
副标题：发送/接收            例 "S: 12 Kbps  R: 240 Kbps"
```

- 数据源：`\Network Interface(*)\Bytes Sent/sec` 等（系统级）。

### 5.5 GPU 卡片

```
大标题：GPU
主数值：GPU 使用率           例 "3%"
副标题：显存使用             例 "1.2/8.0 GB"
```

- 数据源：`\GPU Engine(_Total)\Utilization Percentage` + `\GPU Adapter Memory(*)\Dedicated Usage`。

### 5.6 迷你曲线规格

**[强推测]**

| 参数 | 值 |
|---|---|
| 采样间隔 | 与刷新间隔一致（默认 1 秒） |
| 时间窗口 | 60 秒 |
| 采样点数 | 60 |
| 绘制方式 | 折线（GDI `Polyline`），无填充 |
| 数据存储 | 固定长度环形缓冲，满 60 后覆盖最旧 |

60 秒窗口的依据：任务管理器图表的横轴刻度是 60 秒。采样点数 = 60 与 1 Hz 默认刷新吻合。

---

## 6. 采样与刷新

### 6.1 刷新间隔

**[确证，来自任务管理器 UI 的「查看 → 更新速度」菜单]**

| 菜单项 | 间隔 |
|---|---|
| 高 | 500 ms |
| 正常（默认） | 1000 ms |
| 低 | 2000 ms |
| 暂停 | 不刷新 |

存储在 `HKCU\Software\Microsoft\Windows\CurrentVersion\TaskManager\Preferences`，**私有二进制格式，不是简单的 DWORD**。实测该值长 3000+ 字节，含列宽、列顺序、排序状态、窗口位置等。**不要试图解析**——自己存自己的配置。

### 6.2 整表重算 vs 增量

**[强推测]** 任务管理器是**整表重算**：每秒对全部进程重新取一次快照，重新计算所有派生值，然后重建可见行。没有做差分更新。

依据：本机实测原版在 450 进程时每秒的 CPU 消耗是持续性的（不是每 N 秒一次尖峰），且首轮枚举与后续轮次的成本差异只体现在冷缓存上。

**对本项目的建议**：不必模仿这一点。本项目原型用「代际号 + 只在变化时 `SetData`」已实测把稳态 CPU 降到全系统 0.12%，远优于原版。**UI 像素层面模仿，架构层面不要模仿**。

### 6.3 采样线程模型

原版实测：采集全部压在 **1 个后台工作线程**上（另一个线程是 UI 消息泵，CPU 占比 0%）。[确证]

这是原版卡死的根因之一：单线程无背压，刷新请求会堆叠。新实现必须：
- 采集线程 1 条（串行，避免并发打同一把内核锁）
- **最新值覆盖语义**，不是队列：到点若上轮未完成，跳过本轮并计数
- UI 线程只读已发布的不可变快照

---

## 7. 本机实测汇总

| 项目 | 结果 | 判定 |
|---|---|---|
| 顶层窗口总数 | 465 | 确证 |
| 拥有「应用型」窗口的 PID | 8 | 确证 |
| 其中 UWP（`Windows.UI.Core.CoreWindow`） | 1（pid 14972） | 确证 |
| `WS_EX_TOOLWINDOW` 排除掉的窗口 | 大量（含 Shell_TrayWnd、Tao Thread Event Target、WorkerW、Progman） | 确证 |
| `WS_CHILD` 排除掉的窗口 | 至少 1（pid 21364） | 确证 |
| Session 1 进程数 | 387 | 确证 |
| Session 0 进程数 | 138 | 确证 |
| `C:\Windows\` 下进程 | 184 | 确证 |
| `C:\Windows\` 之外进程 | 302 | 确证 |
| 无 ExecutablePath（受保护） | 24 | 确证 |
| Suspended 状态线程总数 | 109 | 确证 |
| 含挂起线程的进程 | 3（System / SearchApp / ShellExperienceHost） | 确证 |
| **全线程挂起的进程** | **2（SearchApp / ShellExperienceHost）** | 确证 |
| GPU Engine 计数器实例数 | 525 | 确证 |
| GPU 计数器实例名含 PID | 是（`pid_<pid>_luid_..._engtype_...`） | 确证 |
| 每进程网络 PDH 计数器 | **不存在** | 确证 |
| `\Process(_Total)\IO Data Bytes/sec` | 可用，返回真实值 | 确证 |

---

## 8. 给实现者的检查清单

```
[ ] 「应用」用 EnumWindows + 5 条排除规则（visible / !toolwindow / !child / !owner / titleLen>0）
[ ] 「应用」子进程按 parentPid 树归组，用 CreateTime 校验防 pid 重用
[ ] 「Windows 进程」= Session0 服务宿主 ∪ (%SystemRoot%\System32 下 且 非用户启动) ∪ 关键进程白名单
[ ] 「后台进程」= 剩下的
[ ] 「已挂起」= 该进程所有线程 WaitReason==Suspended（注意排除 0 线程进程）
[ ] CPU% = deltaCpu / deltaWall / numLogicalCores * 100，全系统口径
[ ] CPU < 0.05% 显示空白
[ ] 内存用 WorkingSetPrivateSize（快照自带），非 WorkingSetSize
[ ] 磁盘用 Read/WriteTransferCount 差分
[ ] 网络列第一版留空（Windows 无每进程 PDH 计数器）
[ ] GPU 用 \GPU Engine(*) 按实例名里的 pid_ 聚合，放后台线程，2-5 秒一次
[ ] 热力图按列归一化到当前最大值，色带 HSV 橙系
[ ] 迷你曲线 60 点环形缓冲，与刷新间隔同频
[ ] 刷新间隔 500/1000/2000/暂停，默认 1000
[ ] 采集单线程 + 最新值覆盖，绝不堆叠请求
```

---

## 附：本次实测的脚本

- `_probe\wndprobe.ps1` — EnumWindows 应用判定 + 窗口细节转储
- `_probe\susp_probe.ps1` — WaitReason 普查 + 挂起判定 + 映像路径分类

均只读，未启动 taskmgr.exe，未修改系统。
