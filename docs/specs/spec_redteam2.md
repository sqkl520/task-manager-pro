# 红队第二轮：五项实现批判性验证

> 状态：**已完成**
> 环境：Win10 Pro 22H2 19045 / Xeon 服务器级 CPU (10C/20T) / 内存 32GB / 96 DPI
> 约束遵守：未启动 taskmgr.exe、未启动 TaskManagerPro 可见窗口（全部 `--shot` 离屏）、
> 未修改任何项目源文件。探针都写在 `_probe/*.cpp`，编译产物在 `build/*.exe`。

---

## 结论摘要（先读这里）

> ### ⚠ 重要：验证期间项目源码被他人修改
>
> 我 18:08 开始读 `src/tmwindow.cpp`，当时 `SelectedProcess()` 用的是
> 进程页的 `selectedRow_` 索引详情页。等我 20:13 再查，它已经被改成
> `detailSelectedPid_` 了；`tmwindow_icons.cpp` 也被加了 `pathCreateTime_`
> 的 pid 复用检测。
>
> **这些改动不是我做的**（我没编辑任何项目源文件，只在 `_probe/` 写探针）。
> 改动的 mtime 集中在 **19:12 / 19:17 / 20:01 / 20:03 / 20:06**，
> 横跨我跑探针的整个时段。
>
> **所以下表分成两栏：「已修（现在的代码没问题）」和「仍存在（20:13 复核仍在）」。**
> 每条都注明了复核时间点。
>
> **另外注意**：`build/TaskManagerPro.exe` 在 20:10 被重新编译过，但之后
> `src/tmwindow.cpp` / `include/tmwindow.h` 又改了。所以我的**截图来自更早的
> 二进制**。凡是我标注"仍存在"的项，都是**20:13 直接读源码确认**的
> （不是靠截图推断），所以结论不受二进制新旧影响。
>
> **最终复核（20:16，`tmwindow.cpp` 又变动之后）**：三条主要缺陷仍在：
> - `PaintCards` 仍是 `for (...) cpuTotal += e.cpuPercent;`（求和）—— A 在
> - `tmwindow_list.cpp` 仍是 `pe->diskBytesPerSec / (100.0*1048576.0)` —— K 在
> - `classifier.cpp` 仍 0 处 `GetClassName` —— C 在

每条带【实测】【代码分析】【推测】标注。**没把握的地方明说"不确定"。**

### 仍存在的问题（20:13 对照当前源码复核）

| # | 问题 | 依据 | 严重度 |
|---|------|------|--------|
| **C** | **DWM 的 Ghost 窗口被误判成"无响应"+"应用根"**。只要有应用卡死，dwm.exe 就凭空出现在「应用」组并标"无响应"，实测 `SendMessageTimeout` 证明它其实响应正常。**已在程序截图中端到端复现**。`classifier.cpp` 里**仍然没有 `GetClassName` 调用** | 【实测】`resp.exe` + `ghost.exe` + `shots/rt3_ghost.png` | **高** |
| **A** | **CPU 卡片比真值低约 7-12 个百分点**。`PaintCards` 仍是对所有进程 `cpuPercent` 求和（含 pid 0），性能页却用正确的系统计数器 | 【实测】`final_cpu.exe` + `typeperf` | **中高** |
| **K** | **热力图口径不一致**：`tmwindow_list.cpp:395-397` 仍然写 `pe->diskBytesPerSec` / `pe->netBitsPerSec` / `pe->gpuPercent`（原始值），而数字用 `shownDisk/Net/Gpu`（聚合值）→ 数字 11 Mbps 却涂成"≈0"的白色 | 【实测】`shots/rt3_proc.png` 像素取样 + 【代码分析】 | **中高** |
| **D** | **83 个 svchost 显示同一名字「Windows 服务主进程」**，名称列完全无法区分。原版会拼上服务名 | 【实测】`.NET FileVersionInfo` + 实例计数 | **中高** |
| **E** | **名称列取 FileDescription 时不校验代码页/语言**，取 `Translation[0]`。非 Unicode 版本资源会读出乱码（实测 `ToDesk???????`） | 【实测】`desc.exe` 输出 + 【代码分析】 | **中** |
| **G** | **`showFullName_` / `groupByType_` 仍是死开关**：菜单能勾选，界面无任何变化。`appTitle` 也是只写不读的死字段 | 【实测】`grep` 全项目（20:13） | **中** |
| **H** | **高 DPI 完全没适配**。所有度量硬编码 96-DPI 常量，但进程声明了 PER_MONITOR_AWARE_V2。真 taskmgr 在 200% 下行距 60-66px，项目固定 28px | 【代码分析】+【实测】`m3_tm_crop.png` | **中** |
| **I** | **拖动滚动条 / 展开 Claude 时 O(n²)**：每次 `WM_MOUSEMOVE` 都 `RebuildRows`，展开分支 `257 × 638` 次比较 | 【代码分析】+【实测】`appRootOf=257` | **中** |
| **J** | `pid 0` 求和口径不一致（本机无影响）。~~折叠分组会清空选中~~ **已修** | 【代码分析】+【实测】`idle.exe` | **低** |

### 验证期间已被他人修复的问题（我的证词保留，供追溯）

| # | 问题 | 修复证据 |
|---|------|---------|
| **B** | 详细信息页「结束任务」杀错进程（`selectedRow_` 索引 `detailRows_`） | `tmwindow_menus.cpp:38-48` 已改成 `detailSelectedPid_` + 按 pid 查找；`tmwindow_details.cpp` 新增 `SyncDetailSelection`；`tmwindow_events.cpp:353` 加了详情页自己的点击处理。**当前代码已正确** |
| **F** | pid 复用导致图标/名称串号（`pathCache_` 无 createTime 校验） | `tmwindow_icons.cpp:57-68` 新增 `pathCreateTime_` 比对，createTime 变了就 `erase` 旧缓存；`include/tmwindow.h:340` 新增该 map。**当前代码已正确** |
| **J1** | 折叠分组会清空选中 | `tmwindow.cpp:588-601` `SyncSelectionToPid` 现在区分"进程退出"和"被折叠"：只有进程真的消失才清 `selectedPid_`。**当前行为与建议一致** |

> **对 B/F 的说明**：这两条是我在**18:xx-19:xx 的旧代码上**确认的缺陷。
> 它们是真的（我从旧代码逐行读出来的），但**在我写报告期间被修掉了**。
> 我保留了完整证据链在下文 2.4 和 1.5，因为：
> 1. 它们说明问题确实存在过；
> 2. 修复方式与我的分析一致（用 pid + createTime 而非行号）；
> 3. 如果别处还有类似"用行号索引另一个容器"的写法，需要继续排查。

### 通过 / 没问题的项

- **行高 28px 正确**（【实测】参考图 519→547→575→603→631 稳定 28px；程序截图也是 28px）
- **状态优先级「已挂起 > 无响应」三处实现一致**（渲染 ×2 + 排序 ×1）
- **排序 tie-break 用 pid 形成严格弱序，输出确定**
- **挂起进程不会被 `IsHungAppWindow` 误报**（项目注释正确）
- **应用聚合不重复计算根进程**（`appRootOf[pid]=pid` 设计正确）
- **应用聚合内存未超物理内存**（最大 7.15GB vs 31.94GB）
- **Claude 6.1GB 与 PowerShell 独立统计量级吻合**
- **内存两口径实测相等**（`mem4.exe`），不是 bug

### 未验证 / 不确定

- 高 DPI 下的实际错位幅度（本机 96 DPI，**没有 125%/150% 环境实测**）
- 磁盘/网络聚合的可加性（有间接证据，未做专项压测）
- pid 复用的实际触发实例（代码路径确定，没抓到真实串号）
- 拖动滚动条的实际卡顿幅度（复杂度确定，没有交互测试环境）
- ~~25-36% CPU 缺口~~ → **已撤回**，真实是 7-12pp（见 6.1）

---

## 本报告更正过的判断（透明记录）

写报告过程中我有三处**先下结论、后被实测推翻**，都在正文里保留了更正：

1. **"CPU 偏低 25-36%"** → 真相是 **7-12pp**。原因是我的 `gap2.exe`
   取 perf 快照和进程快照之间隔了整个 1 秒 Sleep，时间戳对不齐导致缺口虚高。
   用 `final_cpu.exe` 紧贴采样后修正。（见 6.1）
2. **"截图 94% 是 Idle 占比"** → 真相是 **Idle 的 cpuPercent 恒为 0**，
   与 Idle 无关。`idle.exe` 实测推翻。（见 6.1 第一层）
3. **"进程页 88% vs 性能页 100% 是同一时刻的自我矛盾"** → 两张图相隔 2 小时，
   负载本身在变。**改为**：两页算法不同（这是确定的），
   进程页相对真值偏低（`final_cpu.exe` 实测），但"同一瞬间两页打架"
   我没有实测证明。（见 6.1c）

另有一处**怀疑后被证明不是 bug**：内存两口径（`GlobalMemoryStatusEx` vs
`SystemPerformanceInformation::AvailablePages`）实测**完全相等**（`mem4.exe`）。
已从问题列表移除。（见 6.1d）

---

## 1. 应用聚合（`RebuildAppAggregates`）

### 1.1 根进程自己算不算？（【代码分析】）

**算，而且只算一次。** 依据 `src/classifier.cpp:170-172`：

```cpp
for (const auto& e : snap.processes) {
    if (out.appRoots.count(e.pid)) {
        out.appRootOf[e.pid] = e.pid;   // 根映射到自己
        continue;
    }
```

根进程被写进 `appRootOf[pid] = pid`。然后 `RebuildAppAggregates`（`tmwindow.cpp:304-322`）对**所有**进程遍历：

```cpp
auto it = cls_.appRootOf.find(e.pid);
if (it == cls_.appRootOf.end()) continue;
uint32_t root = it->second;
auto& agg = appAgg_[root];      // 根进程自己 root==pid，落在 appAgg_[pid]
```

所以根进程的值进的是 `appAgg_[自己的pid]`，子进程的值也进 `appAgg_[根pid]`。**不重复计数**：每个 pid 在 `snap_->processes` 里只出现一次，`appRootOf` 是单值 map。

> 潜在坑：`appRootOf` 是 `unordered_map`，`appAgg_[root]` 用 `operator[]` 会默认构造。没问题，但见 1.4 的 pid 复用问题。

### 1.2 privateBytes 与 workingSet 混加（【代码分析】+ 待实测）

```cpp
agg.memBytes += (e.privateBytes ? e.privateBytes : e.workingSet);
```

这是**逐进程**在三元里二选一，不是"全用 private 再加 workingSet"。语义上不会把同一个进程的两份内存相加。

但这仍然有两个真问题：

**(a) 口径不一致会让合计不可解释。** 一个应用里若部分进程拿到 `privateBytes`（WorkingSetPrivateSize 非 0）、部分拿不到（回退 workingSet），合计是"专用工作集 + 工作集"的混合值。工作集通常 > 专用工作集（含共享页），所以回退项会**偏高**。当大量系统进程 `WorkingSetPrivateSize == 0` 时，个别应用合计会虚高。

> 【待实测】本机有多少进程 `privateBytes == 0`。这台机器是 Win10 19045，`WorkingSetPrivateSize` 在 `SYSTEM_PROCESS_INFORMATION` 里是有效字段，预期大多数进程非 0；但**受保护进程 / 会话 0 的部分进程**可能为 0。

**(b) 多个子进程共享的 DLL 页被重复计入。** 专用工作集（private working set）按定义不含共享页，所以用 privateBytes 时**不会**重复计共享 DLL。但一旦回退到 workingSet，就会把共享页计入，这时同一应用内 12 个进程共享的 CEF/DLL 页会被算 12 次。这正是"合计虚高"的主要来源。

### 1.3 聚合内存会不会超过物理内存（【实测】）

**实测方法**：写 `docs/evidence/treeprobe.cpp`（`build/treeprobe.exe`），完整复刻
`classifier.cpp` 的 `EnumWindows` 5 条排除规则 + `BuildAppTree` 的 8 层爬树，
再按程序的三元口径 `privateBytes ? privateBytes : workingSet` 求和。同一时刻
`GlobalMemoryStatusEx` 取物理内存总量。

**结果（本机，638 进程 / 9 个应用根）**：

```
root=59196 cnt=201 title=[Claude] mixed=7318.8MB wsSum=10269.8MB privOnly=7318.8MB
root=226144 cnt=31 title=[???AI???] mixed=1413.0MB wsSum=2460.7MB
root=20144  cnt=7  title=[QQ] mixed=672.1MB
root=184624 cnt=7  title=[CC Switch] mixed=357.1MB
...
sumOfAllAppAggregates=9.84GB  physTotal=31.94GB  maxApp(root 59196)=7.15GB
```

**结论**：
- 单个应用最大 7.15GB（Claude），物理内存 31.94GB。**未超过**物理内存。
- 所有应用合计 9.84GB，也远低于物理内存。
- 但要注意：`sum of ALL processes` 的 private 是 **22.46GB**（`memprobe.exe` 实测），
  working set 合计 **30.43GB** —— working set 合计已达物理内存的 95%，
  因为共享页被重复计入。这说明"按 workingSet 回退"确实会虚高，只是本机
  恰好没有 privateBytes==0 的进程落到应用聚合里。

### 1.4 【重要】`cnt=201`：Claude 应用聚合了 201 个进程（【实测】）

这是最刺眼的一条。`root=59196 cnt=201` —— Claude 根窗口的进程树里挂了
**201 个进程**。看 `memprobe.exe` 的 claude.exe 列表只有 26 个 claude.exe，
那另外 175 个是什么？是 **claude.exe 派生出去的所有子孙进程**，包括
每个 shell 工具调用开出来的 cmd/powershell/node 等等。

这意味着：**「应用」聚合会把任何从该应用派生的进程都算进去，不论它实际上是
独立的应用**。如果 Claude 启动了一个长时间运行的服务（比如 claude 拉起的
node MCP server、或者通过 shell 启动的 msedge），这些进程的内存会被算到
Claude 名下。

> 【实测对照】程序截图 `shots/rt3_proc.png` 里 Claude 显示 **6.1 GB**，
> 而 treeprobe 同一时刻算出 mixed=7318.8MB=**7.15GB**。差 1GB 是因为两次
> 采样时刻不同（进程在变化）。**数量级吻合，口径已确认**。

> 【推测】原版 Windows 任务管理器做聚合时也是按进程树，但它的树只跟
> **直接父子链**且通常 Windows 的进程树没这么深。Claude 这种"每次工具调用
> spawn 一个 shell"的架构会制造极宽的树。这是**设计层面**的问题，不是实现 bug，
> 但会让用户看到"Claude 占 7GB"而实际上是它派生的一堆临时进程。
> 需要确认这是否是产品意图。

### 1.5 `appRootOf` 用 pid 做键 → **pid 复用会串数据**（【代码分析】）

> **【状态】此条已失效 —— 在验证期间被他人修复。**
> 20:13 复核：`src/tmwindow_icons.cpp:57-68` 新增了 `pathCreateTime_` 比对：
> ```cpp
> auto itCt = pathCreateTime_.find(e.pid);
> const bool sameProcess = (itCt != pathCreateTime_.end() &&
>                           itCt->second == e.createTime.QuadPart);
> if (!sameProcess) { pathCache_.erase(it); it = pathCache_.end(); }
> ```
> 下面的分析针对的是**修复前的代码**，保留作追溯。
> （`cls_` 本身每秒 `Clear()` 重算，从来不受 pid 复用影响 ✅ —— 这一点当时也对。）

`classifier.cpp:186` `out.appRootOf[e.pid] = root;`，`RebuildAppAggregates`
用 `cls_.appRootOf.find(e.pid)`。**全流程没有 (pid, createTime) 二元组**。

Windows 的 pid 是复用的。场景：
1. t0：pid=1234 是 claude 的子进程（notepad），`appRootOf[1234]=59196`。
2. t1：notepad 退出，pid 1234 被一个**独立应用**（比如新启动的 QQ）复用。
3. `Classify` 每秒重跑，`appRootOf.clear()` 在 `Clear()` 里（`classifier.h`），
   所以这一轮会重新算 —— **这一点是对的**，`Classify` 每次都 `out.Clear()`。

**但 `pathCache_` 和 `descCache_` 不是。** `src/tmwindow_icons.cpp:54`：

```cpp
auto it = pathCache_.find(e.pid);
if (it != pathCache_.end() && !it->second.empty()) {
    icons_.RequestIcon(it->second);
    continue;   // ← 命中缓存就直接跳过，不校验这个 pid 还是不是同一个进程
}
```

**这是真实的 pid 复用串号 bug**：
- 旧进程 pid=1234 的路径缓存在 `pathCache_[1234] = "C:\...\oldapp.exe"`。
- 新进程复用了 1234，`ResolvePathsAsync` 看到缓存非空 → **直接复用旧路径**。
- 后果链：
  1. `DrawAppIcon` 用旧 exe 的图标 → **图标错**。
  2. `ProcessDisplayName` → `GetFileDescription(旧路径)` → **名称列显示旧 exe 的文件描述**。
  3. 名称列**排序**也按这个旧描述排 → **排序错位**。
  4. 清理逻辑 `pathCache_.size() > 2000` 才触发（`tmwindow_icons.cpp:93`），
     本机 638 进程，永远到不了 2000，**缓存永不清理**，错值会一直留着。

> 【实测】本机进程数 565-638，`pathCache_` 上限 2000 的门槛**永远不会触发**。
> 这条"防止无限增长"的代码在本机是死代码。

### 1.6 磁盘/网络聚合的可加性（【代码分析】+ 已知探针）

`agg.diskBytesPerSec += e.diskBytesPerSec` 用的是
`ReadTransferCount+WriteTransferCount` 差分（`collector.cpp:4b`）。
这是**逻辑 IO**，同一份文件被父子进程各读一次会被算两次 —— 物理磁盘只读了一次。
项目自己的注释（`collector.cpp`）也承认"语义略有不同"。

网络 `netBitsPerSec` 来自 ETW per-pid，理论可加。但 ETW 在
**loopback 流量**上可能双向各计一次（客户端发送 + 服务端接收），聚合时会翻倍。
【推测】未实测。

---

## 2. 选中跟随 pid（`SyncSelectionToPid`）

### 2.1 所有 `selectedRow_` 赋值点都同步了吗？（【代码分析】逐个查）

`grep selectedRow_` 全项目，赋值点只有 3 处：

| 位置 | 代码 | 同步 `selectedPid_`？ |
|------|------|----------------------|
| `tmwindow_events.cpp:367` | `selectedRow_ = r; selectedPid_ = ...vr.pid` | ✅ 是 |
| `tmwindow_events.cpp:382` | `selectedRow_ = r; selectedPid_ = ...rows_[r].pid` | ✅ 是 |
| `tmwindow.cpp:567` | `SyncSelectionToPid` 内部 `selectedRow_ = -1;` | ✅ 是（下次按 pid 找回） |

**但有一处漏了**：`tmwindow.cpp:567` 的 `SyncSelectionToPid` **开头无条件**
`selectedRow_ = -1`，然后 `if (selectedPid_ == 0) return;`。这在语义上没问题
（pid=0 时本来就没选中）。**真正的缺口在下面 2.2。**

### 2.2 折叠含选中项的分组后，选中会怎样？（【代码分析】+ 实测）

> **【状态】此条已失效 —— 在验证期间被他人修复。**
> 20:13 复核：`src/tmwindow.cpp:588-601` 的 `SyncSelectionToPid` 已经区分了
> "进程退出"和"分组被折叠"：
> ```cpp
> bool stillAlive = false;
> if (snap_) for (const auto& e : snap_->processes)
>     if (e.pid == selectedPid_) { stillAlive = true; break; }
> if (!stillAlive) selectedPid_ = 0;
> // 还活着但看不到（被折叠），保留 pid，等展开后自然找回
> ```
> **当前行为：折叠分组时保留选中，展开后恢复 —— 与我的建议一致。**
> 下面的分析针对**修复前**的代码，保留作追溯。

**修复前**的 `SyncSelectionToPid`（`tmwindow.cpp:566-579`）：

```cpp
for (size_t i = 0; i < rows_.size(); ++i) {
    const ViewRow& r = rows_[i];
    if (r.kind == ViewRow::ProcessRow && r.pid == selectedPid_) { ... return; }
}
// 进程已不存在（或所在分组被折叠），清除选中
selectedPid_ = 0;   // ← 这里
```

`RebuildRows` 在分组折叠时（`groupExpanded_[g]=false`）**根本不生成**
该组的 ProcessRow，只保留 GroupHeaderRow。于是 `SyncSelectionToPid` 找不到
pid → 把 `selectedPid_` 置 0。

**后果**：
1. 用户选中「后台进程」里的某进程 → 折叠该组 → 再展开 → **选中没了**。
   注释说"（或所在分组被折叠）"，看起来是**知情的设计选择**，但对用户是
   意外行为：原版 Win10 任务管理器折叠组再展开时选中保持。
2. 同样地，**切换页面再切回来**：`ShowPage` → `BuildLayout` → `LoadPageData`
   → 但**没有调 `RebuildRows`**！"进程"页的 `LoadPageData` 是 `default: break`。
   所以 `ShowPage` 不改 `rows_`，`selectedRow_` 保持；刷新下一次 `SetSnapshot`
   才重建。这一条**没问题**。

3. **排序变化时**（点列头）：`RebuildRows` 末尾 `SyncSelectionToPid` 按 pid
   重定位 → ✅ 正确。**(这个是它对的地方)**

### 2.3 折叠**应用行**（不是分组）后选中会怎样？（【代码分析】）

`appExpanded_[pid] = !appExpanded_[pid]` → `RebuildRows`。折叠时子进程行消失，
如果选中的是子进程 → `SyncSelectionToPid` 找不到 → **清空选中**。同样的行为。

### 2.4 【严重】`selectedRow_` 被进程页和详细信息页共用 → **会杀错进程**（【代码分析】）

> **【状态】此条已失效 —— 在验证期间被他人修复。**
> 20:13 复核：`src/tmwindow_menus.cpp:38-48` 已改成：
> ```cpp
> if (tabs_.Current() == TabId::Details) {
>     if (detailRows_.empty() || detailSelectedPid_ == 0) return nullptr;
>     for (const auto* pe : detailRows_) {
>         if (pe && pe->pid == detailSelectedPid_) return pe;   // 按 pid 找
>     }
>     return nullptr;
> }
> ```
> 并且 `tmwindow_details.cpp` 新增了 `SyncDetailSelection`（用 pid 重定位），
> `tmwindow_events.cpp:353-362` 加了详情页自己的点击处理来设置 `detailSelectedPid_`。
> `include/tmwindow.h:121` 新增 `uint32_t detailSelectedPid_`。
>
> **当前代码在详情页这条路径上已经是安全的。** 下面的分析针对**修复前**的代码，
> 保留作追溯 —— 它证明了这个缺陷真实存在过，且修复方向与我的分析一致。
>
> **仍建议排查**：还有没有别处"拿 A 容器的行号去索引 B 容器"的写法。
> `grep selectedRow_` 现在只剩进程页在用（`tmwindow_list.cpp:306` 绘制、
> `tmwindow_menus.cpp` 若干命令），已无跨页混用。

这是选中逻辑里最严重的缺陷，直接导致**结束任务杀错进程**。

**证据链（全部在 `src/tmwindow_menus.cpp`）**：

1. `SelectedProcess()`（`tmwindow_menus.cpp:32-53`）：
```cpp
if (tabs_.Current() == TabId::Details) {
    if (selectedRow_ >= 0 && selectedRow_ < detailRows_.size())
        return detailRows_[selectedRow_];   // ← 用进程页的索引去索引详情页
    return nullptr;
}
```
`selectedRow_` 是**进程页 `rows_` 的下标**（`tmwindow_events.cpp:367/382` 赋值）。
详情页的 `detailRows_` 是**完全不同的序列**（扁平、按 `imageName` 排序，
`tmwindow_details.cpp:180-183`）。

2. `CMD_END_TASK`（`tmwindow_menus.cpp:542-560`）：
```cpp
case CMD_END_TASK: {
    const ProcessEntry* pe = SelectedProcess();   // ← 详情页时返回 detailRows_[selectedRow_]
    ...
    EndProcessByPid(pe->pid, isAppRow);           // ← 杀的是那个进程
```

**触发场景（完全正常操作）**：
1. 在进程页选中第 50 行（比如某个后台进程）。
2. 点「详细信息」页。
3. 详情页**没有任何设置 `selectedRow_` 的代码**（`tmwindow_events.cpp:329-338`
   只处理 Users/Services，Details 无分支）。所以 `selectedRow_` 仍是 **50**。
4. 在详情页按「结束任务」（或右键 → 结束任务）。
5. `SelectedProcess()` 返回 `detailRows_[50]` —— **那是详情页排序后的第 50 个
   进程，跟用户选的第 50 行毫无关系**。
6. **杀错进程。**

**为什么详情页连"用户点了哪一行"都不知道**：`WM_RBUTTONUP`（`tmwindow_events.cpp:380`）
用 `HitTestRow(y)`，而 `HitTestRow`（`tmwindow_events.cpp:98-109`）遍历的是
**进程页的 `rows_`**，不是 `detailRows_`。详情页的行布局跟 `rows_` 无关，
所以 `HitTestRow` 在详情页返回的索引**语义上是错的**。

**这是一条确定的、可静态证明的缺陷**，且后果是杀错进程（数据丢失级）。
项目在 `tmwindow.cpp:558-579` 的注释里专门讲"防止杀错进程"，但**只修了进程页**，
详情页这条路径完全没管。

### 2.5 `selectedPid_` 跨页残留

在进程页选中 pid=1234 → 切到详情页 → `selectedRow_` 仍是进程页下标（见 2.4）
→ 切回进程页 → `ShowPage` **不调 `RebuildRows`**（`tmwindow.cpp:213-222`）→
`selectedRow_` 还是那个旧下标，`selectedPid_` 还是 1234。下一次 `SetSnapshot` →
`RebuildRows` → `SyncSelectionToPid` 按 1234 重定位 → ✅ 恢复正确。
所以在**进程页自身上**最终会自愈，但中间那一帧（`ShowPage` 后到下一 tick 前）是错位的。

**但详情页不会自愈** —— 详情页根本没有 pid 跟随机制，`selectedRow_` 永远不带
`selectedPid_`。这是 2.4 的根因。

---

## 3. 无响应状态

### 3.1 调用位置对不对？（【代码分析】）

`classifier.cpp:51-55`：

```cpp
if (!IsWindowVisible(hwnd)) return TRUE;     // ← 可见性排除
if (IsHungAppWindow(hwnd)) {
    ctx->out->notResponding.insert(pid);
}
LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
if (exStyle & WS_EX_TOOLWINDOW) return TRUE; // ← 工具窗排除
```

**位置是对的**（要求的"在 `IsWindowVisible` 之后、其余排除规则之前"），
但这里有一个**逻辑后果**值得指出：

- `IsHungAppWindow` 只在 `IsWindowVisible` 为真的窗口上调用。**这正确**：
  不可见窗口的 hung 状态没有意义，而且项目实测全量盲扫会报 17-51 个假阳性。
- 但 `notResponding.insert(pid)` 发生在 `WS_EX_TOOLWINDOW` / `WS_CHILD` /
  `GW_OWNER` / **无标题** 这些排除规则**之前**。这是刻意的（注释说明了原因：
  卡死窗口可能拿不到标题）。

**但由此产生一个不对称**：一个 pid 可能被标成"无响应"，但它**不是应用根**
（因为没有可见的、有标题的顶层窗口）。于是：
- 这个 pid 在「应用」组里**不存在**（不是 appRoot，也没有 appRoot 祖先）。
- 它会被分到「Windows 进程」或「后台进程」组。
- 在那些组里**照样显示"无响应"**。这本身是合理的（原版也这样），
  但意味着"无响应"集合和应用分组是**正交**的两套判定，注释里没有说清。

### 3.2 假阳性：实测报了多少（【实测】）

**方法**：写 `docs/evidence/resp.cpp`（`build/resp.exe`）复刻 `EnumWindowsProc` 的
完整判定链，并额外用 `SendMessageTimeout(WM_NULL, SMTO_ABORTIFHUNG, 300ms)`
做**独立交叉验证**。再写 `docs/evidence/hungmake.cpp`（`build/hungmake.exe`）制造一个
**真正的无响应窗口**（显示窗口后 9 秒不泵消息）。

**基线（没有挂起应用时）**：
```
totalWindows=603  visibleWindows=25
hungWindows=0  hungPids=0
appRootPids=8
```
✅ 静态环境下**零假阳性**。项目"限定可见窗口"的做法是对的。

**制造一个真正挂起的窗口后**：
```
=== HUNG (IsHungAppWindow) ===
pid=146880 hwnd=... appRoot=1 title=[HUNG PROBE WINDOW] class=[HungTestWnd]
pid=3464   hwnd=... appRoot=1 title=[HUNG PROBE WINDOW] class=[Ghost]
hungWindows=2  hungPids=2

=== verify each hung pid ===
pid=3464   name=[C:\Windows\System32\dwm.exe]      SendMsgTimeout=1(ok)   ← 假阳性！
pid=146880 name=[...\build\hungmake.exe]            SendMsgTimeout=0(hung) ← 真阳性
```

### 3.5 【严重】DWM 的 Ghost 窗口会被误判为「无响应」+「应用」（【实测】确认）

这是本次验证发现的**最严重的无响应 bug**。

**现象**：当**任何**应用无响应时，桌面窗口管理器 `dwm.exe` 会为它创建一个
**Ghost 窗口**（窗口类名 `Ghost`，标题**复制**自被挂起的窗口）。实测：

```
pid=3464  class=[Ghost]  title=[HUNG PROBE WINDOW]   ← dwm.exe 的镜像窗口
```

这个 Ghost 窗口：
- `IsWindowVisible` = TRUE ✅ 通过
- `WS_EX_TOOLWINDOW` 未设 ✅ 通过
- `WS_CHILD` 未设 ✅ 通过
- 无 `GW_OWNER` ✅ 通过
- **有标题**（复制的）✅ 通过

**所以它 5 条排除规则全部通过**，被加了两次错误标记：

1. `notResponding.insert(3464)` → **dwm.exe 显示"无响应"**。
   但 `SendMessageTimeout` 实测 dwm **响应正常** —— 这是**纯假阳性**。
2. `appRoots.insert(3464)` → **dwm.exe 被当成"应用根"**。
   `AssignGroups` 的第一条规则（`classifier.cpp:194-197`）：
   ```cpp
   if (out.appRoots.count(e.pid) || out.appRootOf.count(e.pid)) {
       out.groupOf[e.pid] = ProcessGroup::Apps;   // ← 优先级最高
       continue;
   }
   ```
   **这一步在「Windows 进程」判定之前**，所以即使 dwm.exe 在
   `kWindowsWhitelist` 里（`classifier.cpp:15` 确实有 `dwm.exe`）、
   即使它有 session 1，**它也会被判成「应用」组**。
   **截图实测确认它显示的名字是「桌面窗口管理器」**（FileDescription，
   不是窗口标题 —— `appTitle` 是死字段，见 6.12）。

**实测佐证**：`build/ghost.exe` 在有挂起应用时枚举出的 appRoot 候选：

```
appRoot-candidate windows=10
pid=27636 hwnd=... title=[HUNG PROBE WINDOW] class=[HungTestWnd]   ← 真应用
pid=3464  hwnd=... title=[HUNG PROBE WINDOW] class=[Ghost]         ← dwm 的 Ghost！
pid=59196 title=[Claude] ...
...
distinct appRoot pids=10   （正常是 9）
```

**影响链**：
- 只要用户桌面上有任何一个程序卡住，任务管理器里就会**凭空多出一个
  「应用」条目**（实际是 dwm），并且该条目抖动/消失。
- dwm 同时被标"无响应"，**误导用户**。
- 【代码分析】`appTitle[3464]` 会被填成 `HUNG PROBE WINDOW`。虽然 `appTitle`
  目前是死字段（见 6.12），但一旦有人开始用它显示应用名，dwm 就会显示成
  别人的窗口标题。

**根因**：缺少对窗口类名 `Ghost`（或对 `dwm.exe`/`pid 3464`）的排除。
`grep -n "GetClassName\|Ghost\|dwm" src/classifier.cpp` 实测结果：**零命中**
—— 代码里**从来没有调过 `GetClassName`**，也没有任何 dwm 特判。
Win10 任务管理器原版**不会**把 dwm 列成应用 —— 它有自己的窗口过滤。

**修复方向**（仅供参考，未改代码）：
1. 最小改动：在 `EnumWindowsProc` 里 `GetClassNameW` 后跳过
   `class == L"Ghost"` 的窗口。这直接堵住 Ghost 这一条。
2. 更彻底：对 `appRoots` 增加一层"不是系统进程"的校验
   （比如 `pid == 4`/`dwm.exe`/`explorer` 的 Ghost 类窗口一律不算应用根）。
3. 注意：**只跳过 `notResponding` 不够**，因为 Ghost 窗口同时污染了
   `appRoots`，两个集合都要防。

### 3.5b 【实测·程序截图确认】dwm 真的出现在「应用」组里，显示"无响应"

**这是端到端确认，不是推测。** 方法：
1. 启动 `build/hungmake2.exe 45`（制造一个真无响应窗口 45 秒）。
2. 等 10 秒确认 dwm 的 Ghost 窗口已建立（`build/ghost.exe` 实测到
   `pid=3464 class=[Ghost] title=[HUNG PROBE WINDOW 2]`）。
3. 立刻用 `TaskManagerPro.exe --page=0 --sortcol=1 --shot=...` 离屏截图。
4. 截图存为 `shots/rt3_ghost.png`。

**截图里实际看到的「应用」组（10 项）**：
```
桌面窗口管理器          无响应   1%   47.8 MB   ← dwm.exe！假阳性
hungmake2              无响应        1.2 MB    ← 真的那个
QQ / Bandizip / Claude / TextInputHost / Windows 资源管理器 /
Windows Terminal Host / 远程桌面连接 / CC Switch
```

**证据确凿**：
1. **「桌面窗口管理器」（dwm.exe）出现在「应用」组**，而且排在第一位
   （按状态排序）。没有挂起应用时它是「Windows 进程」组的成员，
   截图 `rt3_proc.png` 里根本没有它。**它的组归属被 Ghost 窗口改变了。**
2. **它显示「无响应」**，而 `resp.exe` 用 `SendMessageTimeout` 独立验证
   `dwm.exe` **响应正常**。**这是假阳性。**
3. **「hungmake2」在应用组里出现一次**（那个真正卡住的进程），
   它的内存列显示 1.2 MB，状态"无响应" —— 这一行是**正确的**。
   【更正】我最初写"hungmake2 出现两次"，但截图里只看到一次，已撤回。
   确认的错误只有 **dwm 被误分类** 这一条。

### 3.5d 【实测】同一个 exe 的多个实例会各自成为独立「应用」（原版行为存疑）

`treeprobe.exe` 实测到：
```
root=87548 cnt=2 title=[SAKURA SchoolSimulator] mixed=636.2MB
root=40540 cnt=2 title=[SAKURA SchoolSimulator] mixed=605.9MB
```
**同一个游戏的两次运行**（两个独立进程各有自己的顶层窗口，标题相同）
被算成**两个独立的应用根**，各占一行。

这与原版是否一致？看 `images/4.png`（真 taskmgr）：
```
迅雷 (13)              ← 一个应用条目，后面带 (13) = 13 个进程
Windows 资源管理器 (2)   ← 带 (2)
```
原版**把同一 exe 的多个实例合并成一行，用 `(N)` 显示实例数**！
项目截图 `rt3_proc.png` 里也是 `QQ`、`Claude` 但没有 `(N)` 后缀的实例数。
而**名称列显示的是 FileDescription，同一 exe 的多个实例会显示完全相同的描述**。

【实测】`desc.exe` 统计到本机同名描述的重复：
```
17 x [Claude Code]        18 x [Microsoft Edge WebView2]
18 x [Node.js JavaScript Runtime]    10 x [Claude]
```
**原版会把它们合并成「Claude (10)」这样一行**，项目则是 10 个独立行。
这是**与原版的显著行为差异**（是不是 bug 取决于产品目标，但和
`images/4.png` 的「迅雷 (13)」对照，原版确实合并）。

> 【不确定】原版合并的确切规则（是否只合并同一 exe、是否要求同用户、
> 是否要求无子进程）我没有从原版源码确认。但 `images/4.png` 里的
> `迅雷 (13)` / `Windows 资源管理器 (2)` 是**直接视觉证据**，
> 说明原版确实有"实例数"概念，而项目没有。

> **严重度评估**：这不是罕见边界情况。**只要用户桌面上有任何程序卡住
> （Win10 上很常见：安装程序、卡死的 Electron 应用、无响应的资源管理器），
> 任务管理器就会立刻凭空多出一个「桌面窗口管理器」应用条目并标"无响应"。**
> 用户如果按「结束任务」去"杀掉那个无响应的 dwm"，**会结束桌面窗口管理器，
> 导致整个桌面闪烁重启**（Windows 会自动重启 dwm，但所有窗口会重绘）。
> 这是**可导致用户误操作的严重缺陷**。

### 3.5c 【实测】「已挂起」显示正确（对照组）

同一批截图里，`shots/rt3_hung.png` 显示：
```
Node.js JavaScript Runtime    已挂起    68.0 K
Node.js JavaScript Runtime    已挂起    72.0 K
```
两个真正被挂起的 Node 进程正确显示「已挂起」，且**没有**被误标成「无响应」。
这验证了：
- `DetectSuspended`（`classifier.cpp:96-103`）工作正常。
- 「已挂起 > 无响应」的显示优先级在真实数据上正确。
- `${kRowHeight}` 下状态列文字垂直居中正常。

✅ **这一项通过。**

> 【实测重复性】3 次运行中 2 次复现 Ghost 检测（另 1 次因时序没赶上 dwm 建 Ghost 窗口）。
> Ghost 窗口的创建有延迟（DWM 在检测到 hung 后 ~1s 才建）。**这恰恰意味着
> 程序每秒刷新时会持续命中它**，不是偶发。

### 3.6 「无响应」与「应用根」判定集合的不对称（【代码分析】）

`notResponding.insert()` 在 `WS_EX_TOOLWINDOW`/`WS_CHILD`/`GW_OWNER`/无标题
**之前**执行。所以会出现：

- 一个 pid 被标"无响应"，但它**没有**通过应用根的 5 条规则 → 不是 appRoot。
- 于是它出现在「后台进程」或「Windows 进程」组里，显示"无响应"。
  这本身**符合原版** ✅。
- 但结合 3.5，dwm 这种**系统进程**被标"无响应"是**错的**。

**另一处不对称**：`appRoots` 用 `Title>0` 过滤，`notResponding` 不过滤标题。
这个设计是对的（卡死窗口可能没标题），但 Ghost 窗口**恰好有标题**，
所以既命中 notResponding 又命中 appRoots —— 双重打击。

### 3.7 挂起进程会不会误报（【代码分析】+ 项目注释核实）

项目注释（`tmwindow_list.cpp:454-457`）：
> 实测：被 NtSuspendProcess 挂起的进程 IsHungAppWindow 返回 0

**这条是对的**。`IsHungAppWindow` 本质是读窗口的
`QOS_SEND` 状态/`WM_NULL` 应答标志，挂起的进程消息队列不动，但**没有**
pending 的 sent message，所以返回 FALSE。所以挂起进程**不会**被误报成无响应。
✅ 通过。

**但反向组合**：先卡死（`IsHungAppWindow`=TRUE）再被挂起 → 两个集合都含该 pid。
渲染优先级（已挂起 > 无响应）处理了显示 ✅，排序 rank 2/1 也一致 ✅。
**这一条通过。**

### 3.4 渲染和排序优先级（【代码分析】逐个确认）

| 位置 | 代码 | 优先级 |
|------|------|--------|
| `tmwindow_list.cpp:459-460` | `if (suspended) "已挂起" else if (notResponding) "无响应"` | ✅ 已挂起 > 无响应 |
| `tmwindow_details.cpp:120-121` | 同上 | ✅ |
| `tmwindow.cpp:426-430` `case 1` rank | suspended→2, notResponding→1, else→0 | ✅ 同一优先级 |
| `tmwindow.cpp:432` | `sortDesc_ ? (ra > rb) : (ra < rb)` | 方向由 sortDesc_ 决定 |

**三处都实现了，且一致。这一项通过。**

**但有一个细节**：`case 1` 排序时，`rank` 对**每一个**进程都调
`cls_.suspended.count()` + `cls_.notResponding.count()` —— 两个哈希查找/次比较。
n log n 次比较 × 2 次哈希 ≈ 638 × 10 × 2 = 1.3 万次哈希查找/帧。
【实测】这个开销在 1kHz 刷新下可以忽略（哈希是 O(1)），**不是性能问题**。
只是可以优化（预先算好 rank 存进 ViewRow），不是 bug。

---

## 4. 行高 28px

### 4.1 独立测量（【实测】）

**方法 A：参考截图 `4.png`（888×704，Win10 任务管理器真实截图，96 DPI）**

用 `docs/evidence/_rh3.ps1` / `_rh7.ps1` / `_rh8.ps1` 逐像素扫描 CPU 列
（x=586..628）的文字行起点：

```
starts: 470,483,499,519,547,575,603,631
ends:   471,488,500,528,556,584,612
```

去掉受菜单遮挡/文字粗细影响的异常值，稳定段的**行距**：

```
519 → 547 = 28
547 → 575 = 28
575 → 603 = 28
603 → 631 = 28
```

**方法 B：程序自己的离屏截图 `shots/rt3_proc.png`（1084×641，96 DPI）**

扫描 CPU 列文字行起点（`docs/evidence/_rowh4.ps1`）：
```
starts: 195,223,251,279,335,507,535,563,591
deltas: 28,28,28,56,172,28,28,28
```
稳定段 **28px**（56/172 是跳过折叠的应用行造成的空缺，不是行高异常）。

**方法 C：真人 taskmgr 截图 `shots/m3_tm_crop.png`（2240×1360，200% DPI）**
（这张是项目已有的真 taskmgr 截图，不是我启动的）
```
starts: 450,503,557,622,686,752,815,886,946,1011,1071,1138
deltas: 53,54,65,64,66,63,69,60,65,60,67
```
行距 **60-66px @200%** → 折算 96 DPI 是 **30-33px**。

**结论：**
- **96 DPI 下 28px 与参考图完全吻合** → `theme.h` `kRowHeight = 28` 正确 ✅
- **但 200% DPI 的真 taskmgr 行距是 30-33px（96-DPI 折算）**，不是 28。
  说明**原版的行高本身是随 DPI 缩放的**，而本项目是**固定 28px**。
  这正是 4.2 的问题的**直接实测证据**。

> 说明：`4.png` / `rt3_proc.png` 是 **96 DPI**（本机 `LOGPIXELSX=96`，
> `_dpi.ps1` 实测）。程序用 `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)`
> 但 `theme.h` 里**所有度量都是硬编码的 96-DPI 常量，没有任何 DPI 缩放**。
>
> 【实测边界】`4.png` 在会话中途被系统清理了，方法 A 的数据是清理前测到的，
> 无法复测；方法 B/C 的图仍在。方法 C 的 200% 行距是**独立来源**，
> 不依赖 4.png。

### 4.2 **高 DPI 下会全部错位**（【代码分析】）

`src/main.cpp:217` 声明了 `PER_MONITOR_AWARE_V2`，但：
- `BuildFonts()` 用 `lf.lfHeight = -px` 其中 `px` 是 `kFontSizeBody=12` 等常量。
- `kRowHeight=28`、`kHeaderHeight=24`、`kTreeIndent=18` 等**全是常量**。
- 全项目 `grep` 没有任何 `GetDpiForWindow` / `MulDiv(..., dpi, 96)`。

在 125% 缩放（120 DPI）下，`DrawTextW` 用 12px 字高，但行高还是 28px ——
文字会被裁切，或者行距过密。**在 150%/200% 下会严重错位。**

本机是 96 DPI，所以**当前看不出来**。这是"改了 28px 之后引入的布局错位"之外的
一个**独立缺陷**，但同属"布局错位"这一类，必须报告。

### 4.3 28px 有没有导致其他布局错位（【实测】+【代码分析】）

从 `shots/rt3_proc.png`（1080×640 离屏渲染）实测：
- 行高视觉均匀，文字垂直居中。
- 但**分组标题行 32px vs 进程行 28px** 的混排：滚动计算 `ContentHeight()`
  和 `PaintRows` 都按 `kind` 分别取 28/32，一致 ✅。
- `WM_MOUSEWHEEL` 滚 3 行 = `kRowHeight*3 = 84px`，但分组头是 32px，
  滚过的实际行数会略多于 3 —— **不是 bug**，只是手感差异。

**一处真实错位**（【代码分析】）：`PaintRows` 里 `y` 的累加和 `HitTestRow`
里 `ly` 的累加**算法一致**，但 `HitTestRow` 有个 **off-by-one 边界**：

```cpp
// tmwindow_events.cpp:99
if (y < rcList_.top || y >= rcList_.bottom) return -1;
```

`rcList_.bottom` 是 `rcContent_.bottom`，而列表最后一行可能刚好被工具条
`rcToolbar_` 覆盖。`rcContent_` 已经排除了 toolbar（`H - kToolbarHeight`），
所以这里**没问题**。

---

## 5. 名称列文件描述

### 5.1 哪些进程拿不到 FileDescription（【实测】）

**方法**：`docs/evidence/desc.cpp`（`build/desc.exe`），完整复刻
`tmwindow_icons.cpp` 的 `NtPathToDos` → `GetFileVersionInfoW` →
`VerQueryValueW(Translation)` → `VerQueryValueW(FileDescription)` 链路，
对每个 pid 单独跑。本机 pid!=0 的进程 507 个。

**实测统计**：

```
totalProcs(pid!=0)=507
  withDesc        = 440   (86.8%)
  noVersionInfo   =  57   (11.2%)  ← exe 里根本没有版本资源
  hasVerButNoDesc =   6   ( 1.2%)  ← 有版本资源但没有 FileDescription 字段
  noPath/emptyPath=   4   ( 0.8%)  ← 内核伪进程 / 路径解析失败
```

**拿不到 FileDescription 的具体进程**：

**(a) 无版本信息（57 个）—— 绝大多数是 Git Bash 的工具链**：
```
bash.exe / sh.exe / grep.exe / head.exe / tr.exe / sleep.exe / perl.exe   ← MSYS2/Cygwin
crashpad_handler.exe  (x6+)      ← Chromium 崩溃上报器
clash-verge-service.exe, verge-mihomo.exe, cowork-svc.exe,
codex-windows-sandbox-service.exe, adb.exe, editor_sdk.exe, xllite.exe,
StartMenuExperienceHost.exe, wallpaperservice64.exe
```
> **`StartMenuExperienceHost.exe` 没有版本资源**这一点很意外 —— 它是 Windows
> 自带组件，但 exe 里确实没有 FileVersion 块。所以开始菜单宿主会回退显示
> 文件名 "StartMenuExperienceHost"（去掉 .exe）。
> `crashpad_handler.exe` 有 6 个实例，全部回退。
> Git Bash 的每个 `bash.exe/sh.exe`（Claude 工具调用派生）全部回退 ——
> **本机大量进程会显示 `bash` / `sh` / `head` 这种文件名词**。

**(b) 有版本信息但无 FileDescription（6 个）**：
```
runSW.exe, WPSService20.exe, wallpaper64.exe, crashpad_handler.exe,
mariadbd.exe, TextInputHost.exe
```
> 注意 `crashpad_handler.exe` 同时出现在两类里 —— 说明**不同版本的
> crashpad_handler** 情况不同（有的无版本资源，有的有但没有描述）。

**(c) 无路径（4 个）**：Idle/System/Registry/Memory Compression 之类内核伪进程。

**结论：约 13% 的进程拿不到 FileDescription，全部回退到文件名。**
这**不是 bug**（代码有回退，`tmwindow_icons.cpp:213` `return DisplayName(imageName);`），
但与"名称列显示文件描述"的设计意图有落差 —— 用户会看到
`bash`、`sh`、`head`、`crashpad_handler` 这些裸文件名混在中文描述中间。

### 5.2 同名描述会不会混淆：**会，实测有 28 组重名**（【实测】）

**这是名称列最实质的设计缺陷。** 实测同一描述被多个进程共用：

```
17 x [Claude Code]
18 x [Microsoft Edge WebView2]
18 x [Node.js JavaScript Runtime]
14 x [Git for Windows]
10 x [Claude]
10 x [Google Chrome]
12 x [TencentDocs]
 8 x [WeChatAppEx]
 7 x [Unity Hub]  /  7 x [Steam Client WebHelper]
 6 x [QQ]  /  6 x [Runtime Broker]  /  6 x [NVIDIA Container]  / 6 x [Patina]
 5 x [Weixin]  /  5 x [ATK HUB]
 4 x [COM Surrogate]  /  4 x [Tuanjie Hub]
 ...
```

- **「Claude Code」有 17 个进程显示一模一样的名字**，「Node.js」18 个，
  「WebView2」18 个。**在名称列上完全无法区分**，只能靠右侧的内存值猜。
- **「Git for Windows」14 个**（bash/sh/grep/head/tr/sleep 这些 MSYS2 工具
  的 FileDescription 都是 "Git for Windows"！）——
  **这正是 5.1(a) 的反面**：这些 exe 有的是无版本信息（回退成 bash/sh），
  有的是 "Git for Windows"。**同一条命令行的工具，显示名不统一。**
- 原版 Win10 任务管理器的做法（`images/4.png` 实测）：
  ```
  服务主机: Remote Desktop Se...    ← svchost 显示 "服务主机: <服务名>"
  ```
  原版对 svchost **拼上了它承载的服务名**，所以 100 多个 svchost 各自可区分。
  **本项目完全没有这个处理** → 几十行"主机进程 Windows"（或类似的
  svchost FileDescription）**完全无法区分**。

> 【实测核实】我用 .NET `FileVersionInfo` 直接读本机 6 个关键 exe：
> ```
> svchost.exe  => [Windows 服务主进程]
> notepad.exe  => [记事本]
> explorer.exe => [Windows 资源管理器]
> Taskmgr.exe  => [任务管理器]
> dwm.exe      => [桌面窗口管理器]
> conhost.exe  => [控制台窗口主机]
> ```
> 然后统计本机 **svchost 实例数 = 83**（`docs/evidence/README.md`），
> 它们全部拿到**同一个** `FileDescription = "Windows 服务主进程"`。
>
> **所以名称列会出现 83 行完全一样的「Windows 服务主进程」。**
> 这是可确认的事实，不是推测。
>
> （我前面的 `desc.exe` 重复表里没出现它，是因为那 507 个进程里 svchost
> 走了无版本资源分支 —— 那是 `desc.exe` 探针自身的缺陷，已用 .NET 独立核实推翻。）

原版 Win10 任务管理器的做法（`images/4.png` 实测）：
```
服务主机: Remote Desktop Se...    ← 服务主机: <服务名>
```
原版对 svchost **拼上了它承载的服务名**，所以 83 个 svchost 各自可区分。
**本项目完全没有这个处理** → **83 行「Windows 服务主进程」完全无法区分**。
这是名称列**最严重的可用性缺陷**。

**另一个混淆点（【代码分析】）**：`GetFileDescription` 用**第一个**
`Translation` 条目（`tr[0]`）：
```cpp
swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr[0].lang, tr[0].cp);
```
多语言 exe 若第一个条目是**非当前语言**，会拿到英文/日文描述而不是中文。
原版任务管理器按**当前 UI 语言**选。**这会加剧"中英混杂"**。

> 【实测】`desc.exe` 输出里 `ToDesk???????` 出现了乱码问号 —— 说明该 exe
> 的 FileDescription 在取到的代码页下无法正确转成宽字符。
> 项目代码 `desc.assign(val)` 直接当 `wchar_t*` 拷贝，**如果 exe 的版本
> 资源是 ANSI 代码页（cp=1252/936）而不是 Unicode(1200)，这段会读出乱码**。
> 这是个真实的**编码缺陷**（`tmwindow_icons.cpp:186`）。

### 5.3 `DisplayName` vs `ProcessDisplayName` 不一致（【代码分析】）

- `tmwindow_list.cpp:109` `DisplayName()` 去 `.exe` 后缀 —— 这是**回退路径**用的。
- `tmwindow.cpp:416-417` 排序用 `ProcessDisplayName`。
- `tmwindow_list.cpp:449` 绘制用 `ProcessDisplayName`。
- **`tmwindow_details.cpp:108` 详情页用 `pe->imageName`（带 .exe，不用 desc）**。

所以：**进程页显示"Windows 资源管理器"，详情页显示"explorer.exe"**。
原版 Win10 任务管理器的详情页**也显示带 .exe 的文件名**，所以这**符合原版**。
✅ 不是 bug。

---

## 6. 额外：没提到但确实是逻辑错误的地方

### 6.1 CPU 加总：卡片数字比真值低（【实测】+【代码分析】）

三处代码算"CPU 总占用"，**都是直接对所有进程的 `cpuPercent` 求和**：

| 位置 | 代码 |
|------|------|
| `tmwindow.cpp:278-280` | `for (...) cpuTotal += e.cpuPercent;`（**含 pid 0**） |
| `tmwindow_list.cpp:581-587` | 同上（`PaintStatusBar`，已不调用） |
| `tmwindow_paint.cpp:103-105` | 同上（`PaintCards`，**含 pid 0**） |

**第一层问题：pid 0 (Idle) 也参与求和 —— 但本机实测影响为 0。**

`tmwindow.cpp:279` 的循环**没有** `if (e.pid == 0) continue;`。而
`RebuildRows` 的循环**有**（`tmwindow.cpp:335`）。同一个数据结构，
两处口径不一致 —— 这是**代码不一致**，但：

> 【实测更正】`build/idle.exe`：
> ```
> idle(pid0) cpuPercent=0.00%
> sumAll (含 Idle) = 78.11%
> sumNoIdle          = 78.11%
> ```
> **本机 Idle 进程的 `cpuPercent` 恒为 0**（它的 KernelTime 在
> `SYSTEM_PROCESS_INFORMATION` 里不推进，或推进量被差分吃掉）。
> 所以"含 Idle"在本机**不产生任何数值影响**。
>
> 我一度推测"截图里的 94% 其实是 Idle 占比"——**这个推测是错的**，已实测推翻。
> 记在这里以免后续误信。

结论：pid 0 的漏判是**代码整洁性问题**（两处口径不一致），
在当前机器上**不改变任何显示数值**。在别的机器/内核上 Idle 的
KernelTime 若推进，才会产生影响 —— **不确定**，未在其他机器验证。

**第二层问题：sum(cpuPercent) 与系统真值有偏差，但幅度比我最初测的小。**

> **【重要更正】我最初报告"偏低 25-36%"，那是我的探针的时间对齐伪影，本报告已撤回。**
> 早期 `gap2.exe` 把"取 perf 快照"和"取进程快照"之间隔了一个完整的 1 秒 Sleep，
> 两次快照的时间戳对不齐，导致算出来的缺口虚高。
> 用 `final_cpu.exe` 把 perf 快照**紧贴着**进程快照前后各取一次（QPC 计时），
> 6 次重复的结果是：
> ```
> r0 truthBusy=100.00%  sumProcs=93.39%  gap=-6.61 pp  (cnt=551 miss=8)
> r1 truthBusy=100.00%  sumProcs=92.56%  gap=-7.44 pp  (cnt=557 miss=23)
> r2 truthBusy=99.92%   sumProcs=91.86%  gap=-8.06 pp  (cnt=566 miss=5)
> r3 truthBusy=100.00%  sumProcs=93.48%  gap=-6.52 pp  (cnt=568 miss=1)
> r4 truthBusy=99.53%   sumProcs=100.51% gap=+0.98 pp  (cnt=569 miss=0)
> r5 truthBusy=99.46%   sumProcs=94.94%  gap=-4.52 pp  (cnt=567 miss=5)
> ```
>
> **真实偏差是 -8 到 +1 个百分点，多数在 -7 左右。不是 25-36%。**

**这个偏差是真实的、方向是偏低，但不能算"严重 bug"** —— 1 秒采样间隔下，
短命进程（本机实测每秒数十个 cmd/conhost/bash）的 CPU 时间会丢失，
造成几个百分点的漏算。这是**任何"逐进程差分求系统总量"做法的固有误差**，
原版任务管理器用系统级计数器，不受此影响。

**同期 `tick.exe`（连续 tick，最接近程序真实运行方式）实测**：
```
tick1 sumAll=95.66%   tick2 99.16%   tick3 94.96%   tick4 89.32%
tick5 95.09%  tick6 94.26%  tick7 94.80%  tick8 85.46%  tick9 92.50%
```
而 `typeperf` 同期稳定 99.9-100%。所以**卡片数字比真值低 0-14 个百分点，
且逐帧抖动明显**。

**后果（修正后的结论）**：
1. **CPU 卡片数字偏低且抖动**。截图 `shots/rt3_proc.png` 88%、
   `rt3_hung.png` 89%、`rt3_ghost.png` 89%，同期 `typeperf` = 100%。
   **差 11-12 个百分点，这是稳定可复现的偏差。**
2. **历史曲线**（`hist_.Push(cpuTotal,...)`）会跟着抖动 → 性能页 CPU 曲线有毛刺。
3. **`if (cpuTotal > 100) cpuTotal = 100;` 的钳位几乎不起作用**：
   实测有一次 r4 算出 100.51%（差分窗口和 perf 窗口不完全重合），
   说明**小概率会短暂超过 100%**，钳位偶尔能挡。

> 【实测】三次离屏截图的 CPU 卡片都是 88-89%，同期 `typeperf` 稳定 100%。
> **程序的 CPU 数字系统性偏低约 11 个百分点。** 方向是偏低，不是偏高。

### 6.1c 【重要】程序内部有两套 CPU 总量，一套对一套错

这是 6.1 最能说明问题的一点：**同一个程序里，进程页卡片和性能页曲线用了两种
完全不同的算法，结果不一致。**

**性能页是正确的**（`src/perfdata.cpp:460-488`）：
```cpp
uint64_t total = dKernel + dUser;   // 注释明确写了 "KernelTime 已含 IdleTime"
double util = (1.0 - dIdle / total) * 100.0;
...
out.cpuTotalPercent = sumUtil / n;   // 各逻辑核平均
```
这是 `SystemProcessorPerformanceInformation`（NtQSI class 8）逐核算利用率，
**与任务管理器原版相同的算法**。我用 `final_cpu.exe` 独立验证过这个公式
给出 99.5-100%，与 `typeperf` 吻合 ✅。

**进程页卡片是错的**（`src/tmwindow_paint.cpp:103-105`）：
```cpp
for (const auto& e : snap_->processes) cpuTotal += e.cpuPercent;   // 逐进程求和
```

**两者用的是两种算法，因此同一时刻理论上会显示不同值。**

> **【重要更正】我曾把 `shots/rt3_proc.png`（进程页 88%）和
> `shots/rt3_perf3.png`（性能页 100%）当成"同一时刻的自我矛盾"——这是错的。**
> 那两张图相隔约 2 小时拍摄，负载本来就在变。
>
> 我后来连续拍了 `shots/rt3_p0.png`（进程页 CPU 91%）和
> `shots/rt3_p1.png`（性能页 43%），**两张只隔数秒，但因为机器负载在这几秒内
> 从满载掉到 43%，仍然不能用作对照**。
>
> **所以"两页在同一瞬间显示不同 CPU"这一点，我没有实测证明。**
> 我能确定的是**代码路径不同**（见下），以及**卡片算法相对真值偏低**
> （`final_cpu.exe` 同时刻实测 sumProcs 92-93% vs truthBusy 100%，差约 7pp）。

**已确定的事实**：
- 进程页卡片：`sum(所有进程 cpuPercent)` —— 逐进程差分求和。
- 性能页：`sumUtil/n`，util = `(1 - dIdle/(dKernel+dUser))*100` —— 系统级计数器。
- 两者**算法不同**，理论上没有理由相等。
- 【实测】`final_cpu.exe` 同时刻测得"进程求和"比"系统真值"低约 7pp。
  所以**进程页卡片偏低是确定的**；性能页与 typeperf 吻合，是对的。

**修复方向**：`PaintCards` 的 `cpuTotal` 直接读 `PerfSnapshot::cpuTotalPercent`。
`SetPerfSnapshot` 已经把 perf 快照推到窗口了（`tmwindow.cpp:243`），
只是 `PaintCards` 没用它。修好后两页会一致。

### 6.1d 内存口径：查了，没问题（反证记录）

我一度怀疑进程页内存卡片（`totalPhysical - availablePhysical`，
`GlobalMemoryStatusEx`）和性能页（`SystemPerformanceInformation::AvailablePages`）
口径不同。**用 `build/mem4.exe` 在同一时刻比较，两者完全相等**：

```
[进程页卡片] GlobalMemoryStatusEx: total=31.94GB avail=10.84GB used=21.10GB (66.1%)
[性能页] class2 AvailablePages=2841066 => avail=10.84GB used=21.10GB (66.1%)
  >>> 两口径可用内存差 = 0.00 GB, 已用差 = 0.00 GB
```

**两个 API 的"可用物理内存"在本机定义一致，没有口径差异。**
【实测】这条**不是 bug**，从摘要表里已移除。记在这里免得后人重复怀疑。

（我之前看到"进程页 20.6GB vs 性能页 22.3GB"就以为有差异 —— 那是
**两次相隔 2 小时的采样**，内存本来就在变。**是采样时刻不同，不是口径不同。**）

### 6.2 排序 tie-break 稳定性（【代码分析】）

`tmwindow.cpp:468-469`：
```cpp
// 值相等时的稳定兜底
return a->pid < b->pid;
```

- 对**进程行**：按 pid 升序，**确定** ✅。同样的 pid 不会重复，∴ 是全序。
- 对**应用行**（聚合值相同）：也按 pid ✅。
- **但 `case 0`（名称）有个逻辑漏洞**：
  ```cpp
  int r = NaturalCompare(na.c_str(), nb.c_str());
  if (r != 0) return sortDesc_ ? (r > 0) : (r < 0);
  break;   // ← 落到 pid tie-break
  ```
  这没问题。**但 `case 1`（状态）**：
  ```cpp
  if (ra != rb) return sortDesc_ ? (ra > rb) : (ra < rb);
  break;   // ← 落到 pid
  ```
  状态相同时按 pid，✅。
- **真正的问题**：`std::sort` **不保证稳定**，但这里 tie-break 用了
  pid 形成**严格弱序**，所以结果是确定的 —— `std::sort` 对严格弱序的
  输出是唯一的（不是"稳定"，但"确定"）。✅ **这一项通过**。

**唯一隐患**：`sortDesc_` 翻转时，tie-break 的 `a->pid < b->pid` **不翻转**。
所以降序时相同值的行仍按 pid 升序排列。原版是**整列翻转**还是**只翻主键**？
【推测】原版只翻主键（Explorer 的行为）。所以这里**符合直觉** ✅。

### 6.3 pid 复用（已在 1.5 详述）

`pathCache_` / `descCache_` 无 (pid, createTime) 校验，且**清理阈值 2000 永远
触发不到**（本机 638 进程）→ 旧路径、旧描述、旧图标会串到新进程上。

`cls_` 本身每秒 `Clear()` 重算，**不受影响** ✅。

### 6.4 挂起进程的 CPU 计算（【代码分析】）

挂起的进程 CPU 时间不再增长，`delta=0` → `cpuPercent=0` → **显示空白**。
原版任务管理器对挂起进程也显示 0%（空白），✅ 一致。

**但有一个真问题**：`collector.cpp:319`
```cpp
uint64_t delta = (now > prev) ? (now - prev) : 0;
```
**提交计数器回绕**或 **pid 复用**时 `now < prev`，delta 被钳到 0 ✅ 安全。
但如果 `now` 属于**另一个进程**（pid 复用），`prev` 是旧进程的 CPU 时间，
`now > prev` 成立 → **算出一个虚假的 CPU%**（可能是巨大值）。
这个虚假值只持续一帧，但会：
1. 污染 `maxCpu_` 归一化基准（`RebuildRows` 里 `if (e.cpuPercent > maxCpu_)`）
   → 那一帧**所有行的热力图都变白**。
2. 拉高 `cpuTotal` → 卡片显示异常。

**这是个真实的一帧抖动 bug**，但影响很小（1 秒内自愈）。

### 6.5 分组头计数（【代码分析】）

`tmwindow.cpp:489`：
```cpp
gh.groupCount = static_cast<int>(shown.size());
```

- **应用组**：`shown` 只含 appRoots（`if (cls_.appRoots.count(e->pid))`）✅
  计数 = 应用数，正确。
- **后台/Windows 组**：`shown = groups[g]`，全部进程 ✅。

**但有一个 bug**：`shown.size()` 是**应用根数**，而**展开的应用行的
子进程不计入**。原版任务管理器的分组标题计数是"应用数"，不含子进程 ✅。
**这一项通过**。

**另一个细节**：`if (shown.empty() && groups[g].empty()) continue;`（`tmwindow.cpp:484`）。
对于应用组，`shown` 为空但 `groups[g]` 非空（有子进程但根进程退出了）时，
**仍然画一个 "应用 (0)" 的分组头**。这是个小的显示瑕疵，不算严重。

### 6.6 子进程行的数值口径（【代码分析】）

`tmwindow_list.cpp:317-333`：

```cpp
double shownCpu = pe->cpuPercent;           // 子进程：自己的值
...
if (row.indentLevel == 0 && row.hasChildren) {
    // 应用根：用聚合值
}
```

- **应用根行**（indentLevel=0, hasChildren）：显示聚合 ✅
- **子进程行**（indentLevel=1）：显示自己的值 ✅
- **应用根但没有子进程**（hasChildren=false）：显示**自己的值** ✅

**但 `row.hasChildren` 对应用根是 `kids > 0`**（`tmwindow.cpp:513`）。
如果应用根有子进程但在**别的组**（不可能，子进程必在 Apps 组）……

**真正的问题**：`row.hasChildren` 用来决定
① 是否画展开三角 ② 是否显示聚合值。但**当子进程数为 0 时**，
`appAgg_[pid]` 里**仍然有一条记录**（就是根进程自己的值，因为
`appRootOf[pid]=pid`）。所以 `hasChildren=false` 时**跳过聚合是正确的**
（因为聚合 == 自己的值）。✅

**但聚合值比 CAD 显示多算了一次根进程？** 不，`appRootOf[pid]=pid` 让根进程
进 `appAgg_[pid]` 一次，子进程各进一次 → **不重复** ✅（已在 1.1 确认）。

### 6.8 【实测】`showFullName_` 是死开关（切换了但从不生效）

`grep showFullName_` 全项目只有 3 处：
- `include/tmwindow.h:178` 声明
- `tmwindow_menus.cpp:975` `showFullName_ = !showFullName_;` （切换）
- `tmwindow_events.cpp:120` 传给 `RebuildMenu` 的实参 `false`

**渲染路径（`tmwindow_list.cpp:449`）无条件调用**
`ProcessDisplayName(pe->pid, pe->imageName)`，**完全没读 `showFullName_`**。

所以用户点「查看 → 显示完整名称」：
1. 菜单勾选状态翻转 ✅
2. `RebuildRows()` + `InvalidateAll()` ✅
3. **名称列毫无变化** ❌

**这是确定的功能缺陷**：菜单项存在、有勾选反馈、但没有任何效果。
原版这个选项是切换"显示完整路径"或"显示完整名称"。

### 6.9 `groupByType_` 同样是死开关

同样的模式：`tmwindow_menus.cpp:981` 切换 `groupByType_`，
**`grep` 显示它在 `RebuildRows`/`PaintRows` 里从未被读取**。
`RebuildRows` 永远按 3 组分组（`tmwindow.cpp:472`）。
所以「查看 → 按类型分组」取消勾选后**界面不变**。

> 【不确定】原版取消"按类型分组"是变成平铺列表还是别的。但**本项目无论
> 开关如何都按 3 组显示**，这一点是确定的。

### 6.11 【实测+代码】热力图口径不一致：CPU/内存用聚合值，磁盘/网络用原始值

`tmwindow_list.cpp:386-398` 的 `heatCols[]`：

```cpp
{ 2, shownCpu },                                    // CPU：聚合值 ✅
{ 3, shownMem / memTotalBytes_ * 100.0 },           // 内存：聚合值 ✅
{ 4, pe->diskBytesPerSec / (100.0*1048576.0)*100 }, // 磁盘：**原始值** ❌
{ 5, pe->netBitsPerSec / (100.0*1.0e6)*100 },       // 网络：**原始值** ❌
{ 6, pe->gpuPercent },                              // GPU：**原始值** ❌
```

而同一行的**数字**用的是 `shownDisk` / `shownNet` / `shownGpu`（聚合值，
`tmwindow_list.cpp:323-333`）。**所以应用根行会出现"数字是合计、底色是根进程自己的"**。

**实测证据**（`shots/rt3_proc.png`，Claude 行）：
- 数字显示 **网络 11 Mbps**；
- 但网络单元格底色实测 `RGB(255,244,196)` = `#FFF4C4` = `HeatColor` 的**第 1 档**
  （pct ≤ 5.0）。
- 若底色用**显示的** 11 Mbps，应为 11% → 第 2 档 `#F9ECA8`。

**数字 11 Mbps 却涂成 "≈0" 的白色 → 视觉和数字自相矛盾。** 这正是
"底色用原始 `pe->netBitsPerSec`（根进程自己≈0）、数字用聚合值 11 Mbps" 的直接后果。

**这是确定的口径不一致**，影响所有应用根行的磁盘/网络/GPU 三列底色。
（内存/CPU 两列是对的。）

> 【实测】我无法从截图单独确认磁盘列也是错的（`20 MB/s` 对应第 3 档，
> 原始值和聚合值都可能落在第 3 档）。但**代码路径是确定的**：
> 三列都用 `pe->` 原始值。网络列的实测差异已证实这个模式。

---

### 6.10 `RebuildRows` 每次重绘都重建（含排序），且展开 Claude 是 O(n²)

**调用频率**：`RebuildRows()` 在 16 处被调用，包括每次
`WM_MOUSEWHEEL`（`tmwindow_events.cpp:428`）、每次 `WM_KEYDOWN`（451）、
每次滚动条拖动（211）、每次 `SetSnapshot`（`tmwindow.cpp:273`）。

**展开应用行时的复杂度**（`tmwindow.cpp:515-541`）：
```cpp
for (const auto& kv : cls_.appRootOf) {       // 257 条（本机）
    for (const auto& ce : snap_->processes) { // 638 个
        if (ce.pid == kv.first) { kids.push_back(&ce); break; }
    }
}
```
= **257 × 638 ≈ 16.4 万次 pid 比较**，且**对每个展开的应用行各做一遍**。

【实测】本机 `appRootOf` 实测 257 条（`treeprobe.exe` 的
`aggregatedPids=257`），Claude 一个应用就有 201 个子进程。
所以展开 Claude 时：`257 (appRootOf) × 638 (processes)` 每帧，**再加上**
`std::sort(kids)`（201 个元素）。
`PaintRows` 里还有另一处 O(n) 每行（`tmwindow_list.cpp:301-303`：
每行都线性扫 `snap_->processes` 找 pid）→ **可视行数 × 638**。

**这不是理论问题**：项目自己的性能里程碑（`BENCHMARK_RESULTS.md`）测的是
`PaintAll` 的耗时，但**没测展开态**。展开 Claude 时每帧 16 万+ 次比较，
在 1Hz 刷新下勉强能忍，但**用户滚动/拖动时每次 WM_MOUSEMOVE 都会触发
`RebuildRows`**（`tmwindow_events.cpp:211` 滚动条拖动分支里每移动 1px
就 `RebuildRows()` 一次）→ **拖动滚动条会明显卡顿**。

> 【实测】`shots/rt3_proc.png` 是离屏渲染，测不到交互卡顿。
> 【推测】量化卡顿需要 window 句柄 + 交互；但复杂度是代码里确定的事实。

### 6.12 `Classification::appTitle` 是死字段（只写不读）

`grep appTitle` 全项目：
- `classifier.cpp:72-73` 写入（`out.appTitle[pid] = title;`）
- `classifier.h:48,56` 声明 + `Clear()`
- **没有任何读取点**

也就是说：`EnumWindows` 花了 `GetWindowTextW` 把每个应用根的窗口标题
存进 `appTitle`，**但界面从来没显示过它**。名称列用的是 `ProcessDisplayName`
（FileDescription），不是窗口标题。

> 原版任务管理器在**折叠的应用行**上显示的确实是 FileDescription
> （`images/4.png`：`迅雷`、`Windows 资源管理器`），所以不用窗口标题**是对的**。
> 但既然不用，`appTitle` 就是纯浪费（每帧 N 次 `GetWindowTextW`）。
> 这不是功能 bug，是**死代码 + 无效开销**。

### 6.13 单实例互斥锁在 `--shot` 模式下也会弹窗（【代码分析】）

`src/main.cpp:263-269`：
```cpp
HANDLE mutex = CreateMutexW(nullptr, TRUE, L"TaskManagerPro.SingleInstance");
if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    if (!selfTest) {
        MessageBoxW(nullptr, L"Task Manager Pro 已在运行。", L"提示", ...);
    }
    return 0;
}
```

`selfTest` 为假时**无条件弹 MessageBox**，**包括 `--shot` 离屏模式**。
`offscreen` 变量在 269 行还没被用来判断。

**后果**：如果用户已经开着任务管理器，再跑 `--shot`（或任何脚本化调用），
会**弹出一个模态对话框卡住**（因为 `MessageBoxW` 带 `nullptr` owner，
在无窗口的脚本环境里是**不可关闭的阻塞**）。这在自动化场景下是硬故障。

> 【实测】我在跑 `--shot` 时**没有**遇到这个（因为每次只有一个实例）。
> 但代码路径是确定的：`if (!selfTest)` 没有 `&& !offscreen`。

---

## 7. 数据真实性（PowerShell 独立统计 vs 程序显示）

### 7.1 PowerShell 基线（【实测】）

```
TotalPhysGB = 31.94   (TotalVisibleMemorySize)
FreePhysGB  = 1.05
procs       = 586     (Get-Process 计数)
threads     = 10763
handles     = 247297
privateSumGB= 50.785  (PrivateMemorySize64 求和，含共享重复)
wsSumGB     = 32.165  (WorkingSet64 求和，含共享重复)
```

**注意**：**PowerShell 的 `PrivateMemorySize64` 是 commit（已提交页）**，
不是任务管理器显示的"专用工作集"。两者语义不同：
- `PrivateMemorySize64` ≈ Private Bytes（提交）
- 任务管理器"内存"列 = **Private Working Set**（在物理内存里的私有页）

所以 PowerShell `privateSumGB=50.785` 远大于物理内存是**正常的**（commit 可以超物理）。

### 7.2 程序口径对照（【实测】`memprobe.exe`）

```
rows=614  privSumGB=22.462  wsSumGB=30.432  zeroPrivCount=0
```

- `priv` 是 `WorkingSetPrivateSize`（**专用工作集**）→ 22.46GB
- `ws` 是 `WorkingSetSize` → 30.43GB
- **`zeroPrivCount=0`**：本机**所有** 614 个进程的 `WSPrivate` 都非 0。
  所以 `privateBytes ? privateBytes : workingSet` 的三元**永远走第一个分支**
  → **1.2 的"混加虚高"在本机实际不会发生**。

> 这修正了 1.2 的担忧：**在本机（Win10 19045）回退分支是死代码**。
> 但在别的机器（老 Win10 / Server）上 `WorkingSetPrivateSize` 可能为 0，
> 那时才会混加。**代码是防御性的，但防御分支不可测试。**

### 7.3 进程数 / 线程数 / 句柄数对照（【实测】）

PowerShell `Get-Process` 与程序（NtQSI class 5）的差异：

| 指标 | PowerShell | 程序口径 | 说明 |
|------|-----------|---------|------|
| 进程数 | 586 | 613-614 | PowerShell 不含 Idle；程序含但 pид!=0 计数 |
| 线程数 | 10763 | — | |
| 句柄数 | 247297 | — | |

**实测多少次不一致**：两次独立采样 `_sys2.ps1` / `memprobe.exe`：
- `_sys2.ps1`：procs=586, threads=10763, handles=247297, privateSum=50.785GB, wsSum=32.165GB
- `memprobe.exe`：rows=614, privSumGB=22.462, wsSumGB=30.432

**主要差异原因**：
1. **口径不同**：PowerShell `PrivateMemorySize64` 是 **commit（已提交页）**，
   程序的 `WorkingSetPrivateSize` 是 **专用工作集（物理内存里的）**。
   commit 50.8GB > 物理 32GB 是**正常**的（可以提交超过物理内存）。
   程序口径 22.5GB 是物理内存里的私有部分，**两者不可直接比**。
2. **进程数差 27**：`Get-Process` 有权限/缓存过滤，NtQSI 是内核全量。
   【不确定】具体哪些进程被 PowerShell 隐藏，未逐个核实。
3. **采样时刻不同**（两次相隔数十秒，进程在生灭）—— 这是主要噪音源。

> 【不确定】我没有做到"同一时刻、同一 API"的严格对比。
> 严格对比需要写一个同时调用 `Process.GetProcesses()` 和 NtQSI 的探针。
> 但从数量级看，**没有发现程序计数严重偏离**（614 vs 586 在进程生灭噪音范围内）。

### 7.4 某应用 CPU/内存 对照（【实测】）

**Claude 应用**（这是本机最大的应用，`root=59196`）：

| 来源 | 内存值 | 说明 |
|------|--------|------|
| 程序截图 `shots/rt3_proc.png` | **6.1 GB** | 应用聚合（含所有子进程） |
| `treeprobe.exe`（复刻同一算法） | **7.15 GB** (mixed) | 同一口径，另一时刻 |
| `PowerShell Get-CimInstance` claude*.exe 的 WorkingSetSize 求和 | **6.36 GB** (6386MB) | 只算 name like claude |
| `PowerShell` claude*.exe 的 PrivatePageCount 求和 | **11.9 GB** | commit，不可比 |
| `memprobe.exe` claude.exe 的 WSPrivate 求和 | **4.95 GB** | 专用工作集 |

**关键观察**：
- 程序显示 6.1GB ≈ PowerShell 的 WorkingSetSize 求和 6.36GB —— **量级吻合** ✅
- 程序**只聚合了 26 个 claude.exe**，但 `treeprobe` 显示 `cnt=201`。
  那 175 个是 Claude 派生的 cmd/bash/node（名称不是 claude.exe）。
- PowerShell 的 `name like claude` 求和只覆盖 26 个 → 6.36GB；
  程序的聚合覆盖 201 个 → 7.15GB。**差 0.8GB 正是那 175 个子进程**。
  **这证明程序的聚合确实是"整个进程树"** ✅ 口径确认。

> 【实测】Claude 应用的内存数字（6.1GB）是**可信的**，与独立统计吻合。
> 但它的**构成**（201 个进程）与用户直觉（26 个 claude.exe）差异巨大，
> 这是 1.4 提到的设计层面问题。

### 7.5 CPU 对照（【实测】已详述于 6.1）

| 来源 | 值 | 说明 |
|------|-----|------|
| 程序进程页 CPU 卡片（截图） | 88-91% | sum(所有进程 cpuPercent) |
| 程序性能页利用率 | 99-100% | 系统级计数器（class 8） |
| `typeperf \Processor(_Total)\% Processor Time` | 99.9-100% | 系统真值 |
| `final_cpu.exe` 同时刻 | sumProcs 92-93% vs truthBusy 100% | 差约 7pp |
| `% Idle Time` | 0.0-0.2% | 满载 |

**进程页 CPU 卡片比真值低约 7-12 个百分点；性能页是准的。**
（我最初报的"低 25-36%"是探针时间对齐伪影，已在 6.1 撤回。）

---

## 8. 已经做过的实测（不再待补）

| 项 | 探针 | 结论 |
|----|------|------|
| 应用聚合内存 vs 物理内存 | `treeprobe.exe` | 最大 7.15GB < 31.94GB ✅ |
| 应用聚合进程数 | `treeprobe.exe` | Claude 挂 201 个进程（实测） |
| CPU 真值 vs 求和 | `final_cpu.exe` / `tick.exe` / `typeperf` | 低 7-12pp |
| 无响应假阳性 | `resp.exe` + `hungmake*.exe` | dwm Ghost 窗口假阳性 |
| dwm 进应用组 | `TaskManagerPro --shot` + `ghost.exe` | `shots/rt3_ghost.png` 实证 |
| FileDescription 失败率 | `desc.exe` | 13% 拿不到，回退文件名 |
| 同名描述 | `desc.exe` + .NET | 83 个 svchost 同名 |
| 内存两口径 | `mem4.exe` | 完全相等，无差异 |
| 行高 | `_rh*.ps1` + 像素扫描 | 96DPI 28px ✅；200DPI 真 taskmgr 60-66px |
| Idle 贡献 | `idle.exe` | 本机 pid0 cpuPercent = 0，无影响 |

## 9. 仍未验证（明确的不确定项）

1. **高 DPI 下的实际错位幅度** —— 本机只有 96 DPI，没有 125%/150% 环境。
   【代码分析】确定会错位，但幅度未实测。
2. **磁盘/网络聚合的可加性** —— 逻辑 IO 可能重复计，未做专项压测。
3. **pid 复用的实际触发实例** —— 修复前代码路径确定（无 createTime 校验），
   没抓到真实的串号实例；**现已修复**，不再需要。
4. **拖动滚动条的实际卡顿** —— 复杂度是代码里确定的（257×638/帧），
   但没有窗口交互测试环境量化帧时间。
5. ~~25-36% 缺口~~ —— 已撤回，真实是 7-12pp。


