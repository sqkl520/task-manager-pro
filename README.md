[English](README-EN.md) | 简体中文

# Task Manager Pro

Windows 10 任务管理器的重写实现。在原版会卡死的机器上——核多、线程多、进程多——
它保持响应。

![原版与 Task Manager Pro 对比](docs/images/compare.png)

---

## 它解决什么

Windows 10 自带的任务管理器在进程数、线程数一多就会长时间无响应。不是慢，
是**连续二十几秒点不动**，鼠标拖窗口掉帧。

根因是它自己选的采集链路：`\Thread(*)` PDH 通配符查询在 8000 线程的机器上
单次耗时近 1 秒，再加 WMI 线程查询（秒级）和逐进程签名验证（秒级），而这条
链路又和 UI 线程绑在一起、没有背压。采集一次的成本超过刷新周期，界面就死了。

这个实现换掉了整条数据链路：一次 `NtQuerySystemInformation` 拿全量快照，
所有昂贵字段（图标、发布者、命令行、签名）全部懒加载并持久缓存，
UI 线程永不调用任何阻塞 API。

### 实测（同一台机器，同等负载）

测试机：Xeon 服务器级 CPU（10C/20T）/ 32 GB / Windows 10 22H2 22H2，
测量时约 490 进程 / 8,450 线程。

| 指标 | 原版 | Task Manager Pro |
|---|---|---|
| 打开后最长连续无响应 | **23.95 秒** | **0 秒** |
| 稳态 CPU（全系统占比） | 约 6.4% | **0.12%** |
| 工作集 | 47 MB+ | **13.1 MB** |
| 线程数 | 19 | **4** |
| 全量快照耗时 | 卡死无数据 | **9.1 ms**（1 Hz 预算的 9%） |
| 滚动绘制 p95 | — | **0.87 ms**（60 fps 预算的 5%） |

完整的测量方法与各条链路的单独耗时见 [BENCHMARK_RESULTS.md](docs/BENCHMARK_RESULTS.md)
和 [DIAGNOSIS.md](docs/DIAGNOSIS.md)。

---

## 快速开始

### 依赖

Visual Studio 2022 或 Build Tools，需要 C++ 桌面开发工作负载和 Windows SDK。
没有其他依赖，不使用包管理器。

```bash
winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

### 构建

```bash
build.bat
```

脚本用 `vswhere` 自动定位 MSVC，不需要预先打开 "x64 Native Tools" 提示符。

### 先验证采集层

```bash
build\bench_snapshot.exe
```

这一步必须**先跑通**。它测量单次 `NtQuerySystemInformation` 的耗时与进程数的
关系。几十毫秒内通过，后面的架构才成立；超过 200 ms 说明采集层有问题，
不要急着看界面。

### 运行

```bash
build\TaskManagerPro.exe
```

底部状态栏实时显示自监控指标：采集耗时、绘制耗时、实际渲染行数、快照代际号。
滚动时「绘制」应稳定低于 4 ms，且「可见行」只等于窗口能显示的行数
（等于进程总数说明虚拟化没生效）。

---

## 架构

```
┌──────────────────────────────────────────────────────────────┐
│ UI 线程                                                       │
│   WM_TIMER(250ms) → 只比对代际号，无变化则什么都不做           │
│   WM_PAINT        → 只遍历可见行 [firstRow, lastRow]           │
│                     双缓冲 BitBlt，GDI DrawTextW（CJK 原生）   │
├──────────── 双缓冲 + 原子代际号发布 ─────────────────────────┤
│ Collector 线程（THREAD_PRIORITY_BELOW_NORMAL）                │
│   NtQuerySystemInformation(SystemProcessInformation) 单次全量  │
│   缓冲区跨次复用，仅 STATUS_INFO_LENGTH_MISMATCH 时扩容       │
│   CPU% = 两次 (KernelTime+UserTime) 差分，无额外系统调用       │
├──────────────────────────────────────────────────────────────┤
│ LazyLoader（独立组件，主界面未接入）                          │
│   路径 / 命令行 / 图标 / 签名 / 账户名，按需加载 + 持久缓存    │
└──────────────────────────────────────────────────────────────┘
```

### 四条铁律

1. **UI 线程永不调用** `NtQuerySystemInformation` / `OpenProcess` /
   `LookupAccountSid` / `WinVerifyTrust` / 任何 PDH `Collect`
2. **一次系统调用拿全量快照**，不做「枚举 N 个 PID 再对每个调 5 个 API」
3. **绝不轮询** `\Thread(*)` 或 `\Process(*)` PDH 通配符
4. **昂贵列全部懒加载 + 持久缓存**

---

## 目录

```
src/            实现（29 个 .cpp，按职责分文件，见下）
include/        对应头文件
tools/          一次性探针与验证工具（不进构建产物）
docs/           设计文档、实现规格、取证数据
build.bat       唯一构建入口
```

主界面相关文件按职责拆分：

| 文件 | 职责 |
|---|---|
| `tmwindow_paint.cpp` | 绘制 |
| `tmwindow_list.cpp` | 进程列表：分类、排序、行构建 |
| `tmwindow_events.cpp` | 消息处理 |
| `tmwindow_details.cpp` | 详细信息页 |
| `tmwindow_toolbar.cpp` | 工具栏 |
| `tmwindow_menus.cpp` | 上下文菜单 |
| `tmwindow_icons.cpp` | 图标异步解析 |
| `collector.cpp` | 快照采集线程 |
| `classifier.cpp` | 进程分组与命名规则 |

---

## 文档

- [为什么原版会卡](docs/DIAGNOSIS.md) — 各条数据链路的实测成本
- [实测对比](docs/BENCHMARK_RESULTS.md) — 完整测量数据
- [技术选型](docs/TECH_STACK.md) — 为什么是 C++ / Win32
- [UI 复刻说明](docs/UI_SPEC.md) — 界面规格从哪来
- [文档总览](docs/README.md) — 全部文档的索引

---

## 关于「复刻原版」

界面依据 Windows 10 任务管理器的**可观测规格**从零实现：资源里的声明式布局
（`UIFILE`）、MUI 字符串表、以及截图上的像素测量。

**没有复制、反编译或链接任何原版源代码。** 取证的对象是发布出来的二进制里
可以合法观测的部分——布局资源、字符串、以及运行时的绘制结果。

---

## 已知限制

- 只在 100% DPI 下做过像素级核对。高 DPI 的逻辑是对的（按 rp 换算），但缺逐像素验证
- 网络页需要在管理员权限下才能工作（ETW 会话要求）
- 部分原版菜单项的最终视觉形态未取到截图，实现按文案与常规行为推定，标注为 `[推测]`

---

## 许可

[MIT](LICENSE)
