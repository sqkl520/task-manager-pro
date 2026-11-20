# Task Manager Pro — 技术栈选型（最终推荐）

> 详细决策矩阵与一手源码证据见 `docs/specs/main/STACK_comparison.md`
> 卡顿根因见 `DIAGNOSIS.md`

---

## 一、最终推荐

### Top 1：**C++ / Win32 + 自绘虚拟化表格（GDI 优先，Direct2D 按需）** — 对标 System Informer

**唯一的硬理由**：这个星球上唯一被验证过能在数千进程规模下保持流畅的开源 Windows 任务管理器（System Informer，原 Process Hacker，16,489★，**MIT 许可**），就是 C + Win32 GDI 自绘写成的。我读了它的源码，能明确说出它做对了什么。

可直接借鉴的三处（全部 MIT，可商用）：

| System Informer 的做法 | 源码位置 | 解决你的什么问题 |
|---|---|---|
| `PhEnumProcessesEx` 用 `static ULONG initialBufferSize[3]` 缓存缓冲区大小，命中 `STATUS_INFO_LENGTH_MISMATCH` 才扩容 | `phlib/native.c:1640-1690` | 避免每帧从 16KB 起反复试探（这是任务管理器常见的 O(n) 次系统调用浪费） |
| `PhpProviderThreadStart` 独立采集线程 + `NtWaitForSingleObject(Timer)` + `PhAcquireRundownProtection` | `phlib/provider.c:97-250` | **采集与 UI 彻底解耦**——UI 线程永不等系统调用。这是核心答案 |
| `PhBoostProvider` / `STATUS_ALERTED` 插队机制 | 同上 | 用户操作可插队触发采集，不必等下一个 tick |
| `PhTnpPaint` 算 `firstRow/lastRow` 只遍历可见行；`GetUpdateRgn` 脏区裁剪 + `BitBlt` 双缓冲 | `phlib/treenew.c:1189, 7613, 7681` | 只重绘可见区，与总行数无关 |
| **刻意注释掉了 DirectWrite/D2D 代码**（`directdraw.cpp:1775-1807`） | `phlib/directdraw.cpp` | 说明纯 GDI 足够，这是个有分量的信号 |

**为什么不是别的**：
- **WinUI 3（两种语言）** — GitHub issue `microsoft/microsoft-ui-xaml#9643`：`ScrollViewer + ItemsRepeater` 虚拟化 `StackLayout`，10000 项拖动滚动条出现**帧与空白帧交替**；`#6661`：`ItemsRepeater` 从底部回收实例失败。正好命中你的场景。XAML 栈启动也压不到 300ms。
- **Tauri / WebView2** — 光起 `msedgewebview2.exe` 进程链就吃掉全部启动预算。更致命：你机器上有**腾讯管家 HIPS + 火绒**双 AV，WebView2 的多进程模型每个进程都要过一遍 AV 挂钩扫描，冷启动会被进一步放大。用 WebView 做系统监控工具是选错了工具。
- **Qt 6** — 技术够用（`QTableView` 虚拟化已源码验证 `qabstractitemview.cpp:2120`），但 **LGPL 动态链接**分发麻烦，静态链接需商业授权。
- **Avalonia** — Skia 渲染与虚拟化都没问题，但 `DataGrid` 在商业包，自绘要自己搭；.NET 的 GC 问题与 WPF 相同，却没换来 WPF 的生态与中文渲染成熟度。
- **egui** — 架构很对（无 GC、`ScrollArea::show_rows` 内建行虚拟化），但**默认字体只含 Hack/Ubuntu-Light/NotoEmoji，无任何 CJK**（`epaint_default_fonts/src/lib.rs:20-67`），且 **`allow_ime` 默认 false**（`egui-winit/src/lib.rs:196`）。对中文 Windows 系统工具是硬伤。若你 Rust 强且愿意处理这两点，可升到并列 Top 2。
- **Dear ImGui** — 性能极好、`ImGuiListClipper` 现成，但 immediate-mode 在"每秒全表刷新 + 用户正在滚动/选中/编辑"下状态管理要小心，CJK/IME 同样要自己接。

### Top 2：**C# / .NET 8+ WPF（自绘 `DrawingVisual` 或严格配置的 `ListView`）** — 兼顾开发效率

如果更看重迭代速度，WPF 是唯一同时满足"原生中文渲染无误 + 可控虚拟化 + 无 WebView 起进程开销"的托管方案。本机只需装 .NET SDK（约 200MB）而非 MSVC + Windows SDK（2–4GB）。

**必须遵守（缺一即退化）**：
```xml
<ListView VirtualizingPanel.IsVirtualizing="True"
          VirtualizingPanel.VirtualizationMode="Recycling"
          VirtualizingPanel.ScrollUnit="Pixel"
          ScrollViewer.CanContentScroll="True">   <!-- 设 False 会彻底关闭虚拟化 -->
  <ListView.ItemsPanel>
    <ItemsPanelTemplate><VirtualizingStackPanel /></ItemsPanelTemplate>
  </ListView.ItemsPanel>
</ListView>
```
已从 `dotnet/wpf` 源码确认的坑：
- `ScrollViewer.CanContentScroll="False"`（很多人为"平滑滚动"这么设）会**彻底关闭虚拟化**
- 不设 `Recycling` → 每次滚动新建 visual，GC 压力剧增
- **分组会破坏虚拟化**（源码注释 `// Freeze occurs when using VirtualizingPanel.IsVirtualizingWhenGrouping`）——进程树分组会踩这个坑
- **不要用 `DataGrid`**：1000×20 = 20000 个 `DataGridCell` visual，开销远超 `ListView+GridView`
- **数据层用 `struct` 数组**，不要为每个进程建带 `INotifyPropertyChanged` 的类（332 × 20 属性 × 每秒刷新 = 通知风暴）
- WPF **不支持 NativeAOT**，用 ReadyToRun 压启动

---

## 二、架构设计（两个方案共用）

```
┌─────────────────────────────────────────────────────────────────┐
│  UI 线程（唯一创建窗口的线程，绝不调用任何可能阻塞的 API）        │
│   ├─ WM_PAINT  → 只遍历 [firstRow, lastRow]                      │
│   ├─ GetUpdateRgn 脏区裁剪 → SelectClipRgn → BitBlt 双缓冲        │
│   ├─ WM_DPICHANGED → 重算行高/字体/列宽                          │
│   ├─ WM_TIMER(16ms) → 只检查"新快照代际号"，有则 Invalidate       │
│   └─ 每帧耗时目标 < 4ms（60fps 预算 16.6ms 的 1/4）              │
├─────────────── 无锁双缓冲（seqlock 或 SPSC 环形队列）───────────┤
│  Collector 线程（1 条，可 boost）                                │
│   ├─ 每 1000ms（可配 500/1000/2000/暂停）                         │
│   ├─ NtQuerySystemInformation(SystemProcessInformation)          │
│   │   └─ 缓冲区大小跨次缓存，命中 INFO_LENGTH_MISMATCH 才扩容    │
│   ├─ 单次系统调用拿全 进程+线程+IO+内存  ← 不要每进程开句柄      │
│   ├─ 差分算 CPU%：两次 (KernelTime+UserTime) 之差 / 墙钟         │
│   ├─ 序列号自增 → 写 inactive buffer → 原子切换指针              │
│   └─ 禁止在 Collector 里做：签名验证、账户名解析、命令行获取      │
├──────────────────────────────────────────────────────────────────┤
│  懒加载 Worker 池（2–4 线程，按需、可取消、带超时）              │
│   ├─ 命令行：NtQueryInformationProcess(ProcessCommandLineInfo)   │
│   ├─ 账户名：OpenProcessToken + LookupAccountSid（**缓存 SID→名**）│
│   ├─ 图标：SHGetFileInfo + 位图缓存（按 exe 路径哈希）           │
│   └─ 签名：WinVerifyTrust —— 绝不在主刷新路径调用                │
└──────────────────────────────────────────────────────────────────┘
```

### 四条不可妥协的铁律（这四条正是原版任务管理器出问题的地方）

1. **UI 线程永不调用** `NtQuerySystemInformation`、`OpenProcess`、`LookupAccountSid`、`WinVerifyTrust`、PDH 任何 `Collect`。全部后台完成，UI 只读已发布快照。
2. **一次系统调用拿全量快照**，而不是"枚举 332 个 PID 再对每个调 5 个 API"——后者是 1660 次可能阻塞的调用，前者成本与进程数几乎无关。
3. **绝不轮询 `\Thread(*)` 或 `\Process(*)` PDH 通配符**。实测 `\Thread(*)` 在 7641 线程时 947ms/次，占 1Hz 预算 95%。
4. **账户名、命令行、签名、图标全部懒加载 + 持久缓存**。首屏只需"名字 + PID + CPU + 内存"——这些都在一次快照里。

### 曲线/图表
照抄 System Informer 的 `circbuf.c` — 固定长度环形缓冲只存最近 N 个采样点，绘制时按像素宽度降采样。**不需要 GPU**，GDI `Polyline` 就够。只在实测发现 GDI 曲线成为瓶颈时才升级 D2D。

---

## 三、实测数据链路选型表（直接照用）

### 应当用（全部实测便宜）

| 用途 | 方案 | 实测成本（450 进程 / 7700 线程） |
|---|---|---|
| 进程 + 线程快照 | `NtQuerySystemInformation(5)` 或 `(57)`，**一次拿全** | **7.3–11.1 ms** |
| 逐 CPU 占用 | `NtQuerySystemInformation(8)` ⚠️ 必须给精确 960 B | **0.10 ms** |
| 进程 CPU% | 自己对快照做 `KernelTime + UserTime` 差分 | **0**（快照里已有） |
| 工作集 / 私有内存 | 快照字段 或 `GetProcessMemoryInfo` | 2.9 ms / 425 进程 |
| PID → 映像路径 | `NtQuerySystemInformation(88)` 逐 PID | **0.001 ms/次**，435 个 0.53 ms |
| 命令行 | `NtQueryInformationProcess(60)` | **4.77 ms / 431 进程**（比 WMI 快 **84×**） |
| 句柄数 | 快照里的 `HandleCount` | 近乎免费 |
| 账户名 | 一次性建 SID→名缓存 | 76 ms 冷启动一次 |
| 线程级数据 | 从 `(5)`/`(57)` 快照自己算 | **0** |

### 绝对禁用

| 禁止 | 实测原因 |
|---|---|
| PDH `\Thread(*)\*` 任何形式 | expand **1,780–2,761 ms** + collect **672–1,145 ms**，随线程数线性增长 |
| WMI `Win32_PerfRawData_PerfProc_Thread` | **13,699 ms / 次**（P95 16,424） |
| WMI `Win32_Thread` | **3,432 ms / 次** |
| WMI `Win32_PerfFormattedData_PerfProc_Process` | **869 ms / 次**（P95 2,049） |
| WMI `Win32_Process` 拿全表 | 363–427 ms（比 `NtQSI(5)` 慢 **40×**） |
| 逐进程 `WinVerifyTrust`（主刷新路径） | 11.0 ms × 450 = **3.6–5.7 s**（P95 82.5 ms） |
| 逐进程 `SHGetFileInfo(ICON)`（主刷新路径） | 6.1 ms × 450 = **2.7 s**（max 144 ms） |

---

## 四、落地路径（本机工具链现状：全部未装）

```
.NET 运行时：8.0.24 / 9.0.8 / 10.0.8（WindowsDesktop 齐全）
.NET SDK   ：未安装   → 无法 dotnet build
MSVC       ：未安装   （VS 目录是空的，无 cl.exe）
Windows SDK：仅 UnionMetadata\Windows.WinMD，无 Include/Lib → 无法编译 Win32
Rust       ：未安装   （无 cargo / rustc / ~/.cargo）
Qt         ：未安装
Node       ：v26.12.1 ✅
WebView2   ：157.0.4341.46 已装
```

**选 C++** → 先装 VS 2022/2026 Build Tools（勾「使用 C++ 的桌面开发」+ Windows 11 SDK），约 2–4GB。这是唯一前置阻塞项。
**选 C#/WPF** → 只需装 .NET SDK，约 200MB。**这是 WPF 相对 C++ 的现实优势。**

### 建议的推进顺序

1. **先装工具链**（C++: 2–4GB / C#: 200MB）
2. **第一步只做"一次 `NtQuerySystemInformation` + 控制台打印 450 行 + 计时"**，验证采集耗时是否真的与进程数弱相关。**这一步的结果决定后面所有事，别急着写 UI。**
3. 第二步做最小自绘表格（可见区绘制 + 双缓冲 + 滚动），用**假数据灌 10000 行**压测，确认 60fps
4. 第三步接入真实数据、差分 CPU 计算、曲线
5. 最后才做懒加载项（命令行 / 账户名 / 签名 / 图标）

> **你的机器单核性能弱**（服务器级 CPU 基频 3.0GHz、无 Turbo 优势），所以"每帧 CPU 预算"这个指标在你这里比在普通桌面 CPU 上**更紧**。好消息是：如果原型在你这台机器上能稳 60fps，到别的机器只会更好——这台机器是天然的"最差工况"。

---

## 五、开源项目参考清单

| 项目 | 语言 | ★ | 许可 | 价值 |
|---|---|---|---|---|
| [winsiderss/systeminformer](https://github.com/winsiderss/systeminformer) | C 33.6MB + C++ 3.6MB | 16,489 | **MIT** | **首选参考。** `phnt` 头文件 + `treenew.c` 虚拟化表格 + `provider.c` 采集线程模型，MIT 可商用 |
| [DavidXanatos/TaskExplorer](https://github.com/DavidXanatos/TaskExplorer) | C/C++ + Qt | 3,845 | **GPL-3.0** | 架构可学，**但传染性许可，闭源商用不可用** |
| [ocornut/imgui](https://github.com/ocornut/imgui) | C++ | — | MIT | `ImGuiListClipper` 是裁剪式虚拟化的教科书实现 |
| [emilk/egui](https://github.com/emilk/egui) | Rust | 32,263 | MIT/Apache-2.0 | `ScrollArea::show_rows` 是 Rust 侧最佳虚拟化参考；CJK/IME 需自补 |
| [AvaloniaUI/Avalonia](https://github.com/AvaloniaUI/Avalonia) | C# | 31,808 | MIT | `DataGrid` 在商业包；核心有虚拟化 |

---

## 六、诚实标注

- **✅ 一手源码证据**：各栈的虚拟化机制是否存在、System Informer 的采集/绘制架构、WinUI 3 的已知缺陷、egui 的字体与 IME 现状、各项目语言构成/许可/活跃度。
- **⚠️ 纯架构推断（可信度中–高）**：冷启动时间区间、内存区间、二进制体积。基于"有无运行时""是否多进程""是否 XAML 栈"判断，**不是实测**。
- **❌ 未找到公开实测**：WPF / Avalonia / Qt 在"1000 行 × 20 列、每秒刷新"下的具体帧时间。网上数字多为单篇博客个例，不足以选型。

**强烈建议**：无论选 Top 1 还是 Top 2，落地第一步都应该是**用假数据灌 10000 行、自绘、测实际帧时间**。花半天，能省几周返工。
