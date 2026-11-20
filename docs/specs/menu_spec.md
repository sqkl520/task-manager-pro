# 任务管理器菜单命令 · 实现规格（65 条）

> 目标：把 `include/menus.h` 里 40001–40181 的全部 65 个菜单命令，逐条写成"照着就能写代码"的规格。
> 本文档**不改任何源文件**，只做调研与规格。
>
> **取证方式**（本机 Windows 10 Pro 22H2 22H2 / Xeon 服务器级 CPU 10C20T / 单处理器组 / 已提权）：
> - 【确证-PE】解析 `C:\Windows\System32\taskmgr.exe` 与 `wdc.dll` 的导入表 / 延迟导入表（自写 PE 解析器，未用 dumpbin）
> - 【确证-STR】扫描 `taskmgr.exe` 的 ASCII + UTF-16 字符串字面量
> - 【确证-MUI】`docs/evidence/muiall.txt`、`docs/evidence/muidlg.txt`（从 `zh-CN\taskmgr.exe.mui` 提取的 MENU / DIALOG / STRINGTABLE）
> - 【确证-SDB】`C:\Windows\AppPatch\sysmain.sdb` 与 `AcLayers.dll` 里的 shim 层名
> - 【推测】= 由上面三类证据 + 行为归纳得出，无直接证据
> - 【需验证】= 不确定，实现前必须实测
>
> 标注 `原版:` 的行为均指 Windows 10 19045 的 `taskmgr.exe`。

---

## 0. 结论速览

### 0.1 分类统计（详见第 8 节）

| 类 | 定义 | 数量 |
|---|---|---|
| **A** | 纯本地 API，无提权要求，几十行内可完成 | **39** |
| **B** | 需要管理员权限，或需要自绘/复杂对话框，或未公开 API | **19** |
| **C** | 只是 `ShellExecute` 打开外部程序 / URL / 系统页 | **7** |
| 合计 | | **65** |

> 计数口径：`menus.h` 里共 **65 个具名枚举项**；其中 `CMD_DEF_TAB_BASE` 一个名字代表
> `+0..6` 共 7 个实际命令（7 个选项卡），按 1 个计入上表（跟随"65"这个既有口径）。
> 若按可路由的 ID 数算，总数是 71。

### 0.2 原版行为里最容易被做错的 5 件事

1. **`CMD_DEBUG` 不是硬编码 `vsjitdebugger.exe`**。原版读
   `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\AeDebug` 的 `Debugger` 值，
   把 `"%s -p %ld"` 套进去启动；取不到就显示 `No default debugger present`。
   **本机该键没有 `Debugger` 值**（只有 `UserDebuggerHotKey`、`Auto`），所以在这台机器上
   "调试"的正确表现就是报"没有默认调试器"，而不是去 System32 找 `vsjitdebugger.exe`
   （本机该文件不存在）。【确证-PE/STR + 注册表】
2. **`CMD_PROVIDE_FEEDBACK` 用的是自定义协议 `feedback-hub:?tabid=2&origin=taskmanager`**，
   不是 support.microsoft.com 的网页。【确证-STR：该字符串在 taskmgr.exe 中逐字存在】
3. **`CMD_USR_SWITCH`（切换用户账户）用的是 `WinStationConnectAndLockDesktop`**，
   不是 `tsdiscon.exe`。后者只是断开当前会话，不会切到另一个用户。【确证-PE：WINSTA.dll 延迟导入】
4. **`CMD_ANALYZE_CHAIN` 有公开 API**：`OpenThreadWaitChainSession` / `GetThreadWaitChain`
   （`wct.h`，SDK 里有头文件；`advapi32.dll` 延迟导入）。**不是** `NtQueryInformationThread`。
   【确证-PE + 读 `10.0.26100.0\um\wct.h`】
5. **`CMD_END_TREE` 不用 Job Object、也不用 Toolhelp**：taskmgr 与 wdc.dll 的导入表里
   **没有** `CreateToolhelp32Snapshot`、也没有任何 Job API，它是拿内部进程列表的父子关系建树
   （`WdcListView::KillProcessTree`）。所以本项目用快照 `parentPid` 建树的思路是对的。【确证-PE】

---

## 1. 进程右键（MENU 30208）

### CMD_EXPAND / CMD_COLLAPSE (40100 / 40101)

- **规格**：纯本地 UI 状态。`CMD_EXPAND` → `appExpanded_[pid] = true`；`CMD_COLLAPSE` → `false`。
  组头行则改 `groupExpanded_[group]`。
- **原版**：MENU 30208 里"展开/折叠"是**同一个位置**的两个互斥项（只显示其中一个），
  代码里也是同一个命令位的两态。菜单构建时按 `expanded` 二选一（`menus.cpp` 已这样写）。
- **API**：无。改完 `RebuildRows()` + `InvalidateAll()`。
- **失败处理**：无。
- **提权**：不需要。
- **注意**：进程级展开的子进程集合必须来自快照的 `parentPid` 关系；PID 复用会导致错挂，
  建议按 `(pid, createTime)` 双键匹配。
- **标注**：【确证-MUI】菜单文案；【推测】交互细节。

### CMD_SWITCH_TO (40102)

- **规格**：把目标进程的窗口切到前台。
- **API 序列**：
  1. `EnumWindows` 找目标 pid 的窗口，筛选条件：`IsWindowVisible` **且** `GetWindow(h, GW_OWNER)==nullptr`
     **且** `GetWindowTextLengthW(h) > 0`（任务管理器自己也是这个三条件）。
  2. 前台锁解除：
     - 首选 `SwitchToThisWindow(hwnd, TRUE)` —— 【确证-PE】taskmgr.exe 导入表里有它，
       这也是原版"切换到"最可能用的 API。它**不受** `SetForegroundWindow` 的前台锁限制。
     - 备选（更通用）：`AttachThreadInput(GetCurrentThreadId(), GetWindowThreadProcessId(GetForegroundWindow()), TRUE)`
       → `ShowWindow(h, IsIconic(h) ? SW_RESTORE : SW_SHOW)` → `BringWindowToTop` →
       `SetForegroundWindow` → `SetActiveWindow` → `AttachThreadInput(..., FALSE)`。
     - 第三备选：`AllowSetForegroundWindow(ASFW_ANY)`（自身已是前台时才有效）。
  3. UWP/挂起进程的窗口可能属于 `ApplicationFrameHost.exe`，需要额外按
     `Windows.UI.Core.CoreWindow` 类名找子窗口；原版有 `TmCheckImmersiveProcess` 专门处理。
- **失败处理**：找不到窗口 → 提示"该进程没有可见的顶层窗口"（不要弹系统错误）。
  前台锁定失败时**不报错**，只让任务栏闪烁（`FlashWindowEx`）提示用户。
- **提权**：不需要。但对更高完整性级别的进程（如以管理员运行的程序），
  UIPI 会阻止 `SetForegroundWindow` 跨完整性生效，需要 `ChangeWindowMessageFilterEx` 或本程序自身提权。
- **原版**：`WdcListView` 的 "Switch to"；挂起进程会先 `NtResumeProcess`（wdc.dll 导入
  `NtSuspendProcess`/`NtResumeProcess`）再切窗口。
- **标注**：【确证-PE】`SwitchToThisWindow`、`AllowSetForegroundWindow`、`IsHungAppWindow`、
  `GhostWindowFromHungWindow`、`CoAllowSetForegroundWindow`(ole32) 全部在导入表；
  【推测】具体的窗口筛选条件。

### CMD_RESTART (40103)

- **规格**：杀掉目标进程再按原命令行重新拉起。**只对"应用"根行显示**
  （`menus.cpp` 里 `hasChildren` 才加这一项，与原版一致）。
- **拿命令行**（两条路，按可靠性排序）：
  1. **PEB 读法**（原版走这条，【确证-PE】taskmgr 导入了 `NtQueryInformationProcess` +
     `ReadProcessMemory`，并有 `TmGetProcessCommandLine` 符号）：
     `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_VM_READ)` →
     `NtQueryInformationProcess(ProcessBasicInformation=0)` 得 `PebBaseAddress` →
     `ReadProcessMemory(peb+0x20)` 得 `ProcessParameters` →
     `ReadProcessMemory(params+0x70)` 得 `UNICODE_STRING CommandLine` → 再读 `Buffer`。
     **必须按目标进程位数分支**：64 位进程 PEB 偏移 `0x20` / CommandLine `0x70`；
     32 位 WOW64 进程是 `0x10` / `0x40`，且要先 `IsWow64Process` 判断。
     **不要用 `PROCESS_QUERY_INFORMATION`** —— 对受保护进程会直接失败，
     `PROCESS_QUERY_LIMITED_INFORMATION` 的成功率高得多。
  2. **WMI 兜底**：`SELECT CommandLine FROM Win32_Process WHERE ProcessId=<pid>`。
     慢（首次 COM+WMI 初始化 100–700 ms），只在 PEB 失败且用户明确点了"重新启动"时才用，
     **绝不能放进 1 Hz 刷新路径**。
  3. **最后兜底**：`QueryFullProcessImageNameW` 拿 exe 路径，按 `"path"` 拉起（丢参数）。
- **拉起**：`CreateProcessW(nullptr, cmdline, ..., CREATE_UNICODE_ENVIRONMENT, ...)`。
  注意命令行是**可写缓冲区**（`CreateProcessW` 会就地改它）。
  UWP 应用不能这样拉，要 `IApplicationActivationManager::ActivateApplication`，
  用 `GetPackageFullName` / `GetApplicationUserModelId` 拿 AUMID
  （【确证-PE】taskmgr 导入了 `GetPackageFamilyName`、`ParseApplicationUserModelId`，
  并有 `LaunchImmersiveApplication`）。
- **顺序**：先读命令行，**再**杀进程，**最后**拉起 —— 反过来就拿不到 PEB 了。
- **失败处理**：命令行读不到 → 提示后降级为按 exe 路径启动；`CreateProcessW` 失败 →
  `FormatMessageW` 弹窗（对应 MUI `STR 2034` "尝试执行命令时，出现以下系统错误: "）。
- **提权**：读其他用户/高完整性进程的 PEB 需要管理员。**杀自己的进程再拉起自己**会在
  退出瞬间被重启，属于正常行为。
- **标注**：【确证-PE】API 与符号名；【推测】PEB 偏移（这是社区共识的固定布局，
  但**必须在 32/64 位两条路径上分别实测**，标【需验证】）。

### CMD_END_TASK (40104) — 参考，已实现

- `OpenProcess(PROCESS_TERMINATE|SYNCHRONIZE, FALSE, pid)` → `TerminateProcess(h, 1)` → `CloseHandle`。
- 失败：`ERROR_ACCESS_DENIED`(5) → 提示需要管理员；`ERROR_INVALID_PARAMETER`(87) → 进程已退出。
- 原版会先查 `IsProcessCritical` / `WdcGetProcessCritical`（[确证-PE] 有
  `WdcProcessMonitor::GetCritical`），关键进程弹红色警告，确认后仍会杀（杀完系统蓝屏）。
  建议：`IsProcessCritical(h,&b)`（`processthreadsapi.h`，Win8.1+，SDK 已确认存在）为 TRUE 时弹警告。

### CMD_END_TREE (40105)

- **规格**：递归终止子进程树。
- **建树方式**：**用快照的 `ProcessEntry.parentPid` 建树**，不要用 Job Object。
  理由：【确证-PE】taskmgr.exe 与 wdc.dll 的导入表里**没有** `CreateToolhelp32Snapshot`，
  也**没有任何 Job API**（`CreateJobObject`/`AssignProcessToJobObject`/`TerminateJobObject` 全无），
  原版就是靠内部进程列表的父子关系（`WdcListView::KillProcessTree`）。Job Object 方案还有个
  硬伤：只能杀"你创建并已加入 Job"的进程，对任意已有进程无效。
- **API 序列**：
  1. 从快照 BFS 出 `{pid | parentPid 链}` 闭包。
  2. **按深度倒序**（先叶子后根）逐个 `OpenProcess(PROCESS_TERMINATE|SYNCHRONIZE)` +
     `TerminateProcess(h,1)`，避免父进程在子进程死之前把它重新拉起来。
  3. 建议每杀一个 `WaitForSingleObject(h, 500ms)` 再看是否还有存活，最多两轮。
- **失败处理**：`CollectProcessTree` 时若遇到 PID 复用（同一 pid 但 `createTime` 不同），
  不要并入树。单个进程杀失败（`ERROR_ACCESS_DENIED`）**不要整棵树放弃**，收集起来最后汇总
  弹一次"其中 N 个进程无法结束（拒绝访问）"。
- **提权**：杀 System/SYSTEM 会话的服务进程需要管理员；杀自己同级的普通进程不需要。
- **实现注意（现有代码的 bug）**：`CollectProcessTree` 里
  `for (uint32_t p : out) { if (...) { out.push_back(...) } }` 在 range-for 内 `push_back`，
  一旦 `vector` 扩容就是迭代器失效（UB）。应先建 `parentPid -> children` 索引再 BFS。
- **标注**：【确证-PE】无 Toolhelp / 无 Job；【推测】倒序 + 两轮等待。

### CMD_PROVIDE_FEEDBACK (40106)

- **规格**：打开反馈中心（Feedback Hub），并尽量把目标进程作为上下文带过去。
- **确切 URL**：【确证-STR】`taskmgr.exe` 里逐字存在：
  ```
  feedback-hub:?tabid=2&origin=taskmanager
  ```
  另有独立字符串 `&contextid=`、`&appid=`、`&binaryid=`（推测是拼在后面的可选上下文参数，
  【需验证】具体取值格式）。
- **实现**：`ShellExecuteW(hwnd, L"open", L"feedback-hub:?tabid=2&origin=taskmanager", nullptr, nullptr, SW_SHOWNORMAL)`。
- **降级**：协议未注册（企业版精简镜像常见）→ 打开 `https://support.microsoft.com/` 的反馈中心页；
  仍失败 → 弹 MUI `STR 2751` 的原文"无法打开所选进程的反馈中心"。
- **提权**：不需要。
- **分类**：C（一行 `ShellExecute`）。

### CMD_DEBUG (40110)

- **规格**：用系统注册的默认调试器附加到目标 pid。
- **确切行为**（【确证-STR】，这是本次调研纠正的最大错误）：
  1. 读 `HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion\AeDebug`
     的 `Debugger`（REG_SZ）。
  2. 有值 → 拼成 `"<Debugger>" -p <pid>`（字符串模板**逐字**是 `"%s -p %ld"`）→ `CreateProcessW`。
  3. 无值 → 显示 `No default debugger present`（【确证-STR】该英文串在 taskmgr.exe 里逐字存在，
     说明这条路径上原版**没走 MUI 本地化**）。
- **本机实测**：`AeDebug` 只有 `UserDebuggerHotKey=0`、`Auto=0`，**没有 `Debugger` 值**；
  `C:\Windows\System32\vsjitdebugger.exe` **不存在**。所以在这台机器上正确的表现是弹
  "No default debugger present"，**不是**去找 vsjitdebugger.exe。
- **提权**：调试更高完整性进程需要管理员 + `SeDebugPrivilege`。不需要主动启用特权，
  失败由调试器自己报。
- **标注**：【确证-PE/STR】+ 注册表实测；`-p %ld` 用的是 `%ld`（十进制有符号），不是 `%u`。
- **分类**：C（读注册表 + 一行 ShellExecute/CreateProcess）。

### CMD_CREATE_DUMP (40112)

- **规格**：给目标进程写一个 minidump。
- **API 序列**：
  1. 目标路径：`GetTempPathW` + `_GenerateCrashDumpFilePath` 风格的
     `<映像名>_<pid>_<时间戳>.DMP`（【确证-STR】`GetTempPathW` 与 `.DMP` 都在导入/字符串里；
     原版两个对话框（MUI `DLG 34801` 进度 / `DLG 34802` "文件位于:"）说明它先 dump 后告诉路径）。
  2. `OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ, FALSE, pid)`。
  3. `CreateFileW(path, GENERIC_WRITE, 0, ...)`。
  4. **动态加载** `dbghelp.dll`（【确证-PE】是延迟导入，静态链接会把 dbghelp 塞进导入表）
     → `GetProcAddress("MiniDumpWriteDump")`。
  5. `MiniDumpWriteDump(hProc, pid, hFile, DumpType, nullptr, nullptr, nullptr)`。
- **`DumpType` 取值**（`minidumpapiset.h` 已确认常量）：
  原版最可能用 `MiniDumpWithDataSegs(0x1) | MiniDumpWithFullMemory(0x2)`（"转储文件"能直接用
  WinDbg 打开且有全部内存）；只给 `MiniDumpNormal(0)` 会小很多但信息不足。
  【需验证】原版实际取值——建议先按 `MiniDumpWithDataSegs|MiniDumpWithFullMemory` 实现，
  并**提示文件可能很大**（几 GB 级）。
- **失败处理**：`ERROR_ACCESS_DENIED` → 需要管理员 + `SeDebugPrivilege`；
  `ERROR_PARTIAL_COPY`(299) → 目标进程在 dump 期间退出；
  `ERROR_NOT_ENOUGH_MEMORY` → 换 `MiniDumpNormal`。
  长耗时（几十秒）必须**放后台线程**，否则 UI 卡死（原版用 `MiniDumpThreadProc` 线程，
  【确证-STR】该符号存在）。
- **提权**：dump 其他用户 / SYSTEM / 受保护进程**必须管理员**；dump 自己同级进程不需要。
- **分类**：B（需 dbghelp + 后台线程 + 权限）。

### CMD_OPEN_LOCATION (40113)

- **规格**：在资源管理器里选中该进程的 exe。
- **API**：
  - 首选（原版做法，【确证-PE】导入了 `SHOpenFolderAndSelectItems` + `SHParseDisplayName`）：
    `SHParseDisplayName(path)` → `SHOpenFolderAndSelectItems(pidlFolder, 1, &pidlChild, 0)`。
  - 简化：`ShellExecuteW(hwnd, L"open", L"explorer.exe", L"/select,\"<path>\"", ...)`。
- **路径来源**：`QueryFullProcessImageNameW`（`PROCESS_QUERY_LIMITED_INFORMATION`），
  失败再退 `NtQuerySystemInformation(SystemProcessIdInformation=88)`。
- **失败**：路径为空（受保护进程）→ **把这一项灰显**，而不是点了没反应。
- **提权**：不需要。
- **分类**：C（打开外部程序）。

### CMD_SEARCH_ONLINE (40114)

- **规格**：用默认浏览器必应搜索该进程的映像名。
- **确切 URL**：【确证-STR】`https://www.bing.com/search?q=` 逐字存在（`http` 单独一条，
  说明原版按协议切换）。查询词是**映像名**（`chrome.exe`），不是完整路径，也不是 PID。
- **实现**：`ShellExecuteW(hwnd, L"open", L"https://www.bing.com/search?q=chrome.exe", ...)`
  —— URL 里的查询词要做百分号编码（`_wcsupr` 不必，但空格、`+`、`&`、`#` 必须转义）。
- **提权**：不需要。**分类**：C。

### CMD_PROPERTIES (40115)

- **规格**：弹出该 exe 的**标准 Shell 属性表**（不是自绘对话框）。
- **API（一行）**：
  `SHObjectProperties(hwnd, SHOP_FILEPATH, exePath, nullptr)` ——
  `ShlObj_core.h` 已确认签名与 `SHOP_FILEPATH = 0x00000002`。
- **回退**：`ShellExecuteExW` + `verb=L"properties"` + `SEE_MASK_INVOKEIDLIST`。
- **失败**：路径读不到 → 灰显。属性表里的"兼容性"页正好覆盖 `CMD_UAC_VIRT` 的手工路径。
- **提权**：不需要（读文件属性）。**分类**：A。
- **注**：现有实现弹的是一个自绘文本 MessageBox，与原版差距明显，建议替换。

### CMD_GOTO_SERVICES (40116)

- **规格**：从详细信息页的某个进程跳到"服务"页，并选中以该 pid 托管的服务
  （`svchost.exe` 一个 pid 可能对应几十个服务，原版会**多选**全部匹配项）。
- **API**：无。`tabs_.SetCurrent(TabId::Services)` + 刷新服务表 + 按
  `ServiceItem.pid == selectedPid` 批量设选中。
- **边界**：目标进程不托管任何服务 → 只切页、不选中，不要弹窗。
- **提权**：不需要（`OpenSCManager(SC_MANAGER_ENUMERATE_SERVICE)` 普通用户可用）。
- **分类**：A。

### CMD_GOTO_DETAILS (40117)

- **规格**：切到"详细信息"页并选中同一 pid。
- **API**：无，纯 UI 状态。注意现有 `PaintDetailsPage` 用 `detailRows_` 的**索引**，
  切页后要按 pid 反查索引再设选中，否则刷一轮就丢。
- **分类**：A。

### CMD_VAL_PERCENT / CMD_VAL_VALUE (40180 / 40181)

- **规格**：资源值显示方式三态开关：内存 / 磁盘 / 网络 各可独立选"百分比 / 值"。
  MUI `MENU 30208` 的结构是：
  ```
  资源值(&V) ▸ 内存(&M) ▸ 百分比 / 值
                磁盘(&K) ▸ 百分比 / 值
                网络(&N) ▸ 百分比 / 值
  ```
  【确证-MUI：MENU 30208 与 MENU 30216 都有这个三组结构】
- **现状差异**：`menus.h` 只给了 1 个 `CMD_VAL_PERCENT` + 1 个 `CMD_VAL_VALUE`，
  **不足以表达 3×2 组合**。建议扩成 `CMD_VAL_BASE (40180) + 0..5`
  （内存%/内存值/磁盘%/磁盘值/网络%/网络值），或保留现值但让"百分比/值"作用于全部三项。
- **API**：无。改 `struct { int mem; int disk; int net; }` 三项 + `CheckMenuRadioItem` +
  `RebuildRows()`。
- **分类**：A。

---

## 2. 详细信息右键（MENU 30200）

### CMD_SET_PRIORITY (40107) + 6 个子项

- **规格**：6 个优先级，映射到 `SetPriorityClass` 常量：

| 菜单 ID | 文案（MUI 30200） | 常量 | 值 |
|---|---|---|---|
| `CMD_PRIO_REALTIME` 40120 | 实时(&R) | `REALTIME_PRIORITY_CLASS` | 0x00000100 |
| `CMD_PRIO_HIGH` 40121 | 高(&H) | `HIGH_PRIORITY_CLASS` | 0x00000080 |
| `CMD_PRIO_ABOVE` 40122 | 高于正常(&A) | `ABOVE_NORMAL_PRIORITY_CLASS` | 0x00008000 |
| `CMD_PRIO_NORMAL` 40123 | 正常(&N) | `NORMAL_PRIORITY_CLASS` | 0x00000020 |
| `CMD_PRIO_BELOW` 40124 | 低于正常(&B) | `BELOW_NORMAL_PRIORITY_CLASS` | 0x00004000 |
| `CMD_PRIO_LOW` 40125 | 低(&L) | `IDLE_PRIORITY_CLASS` | 0x00000040 |

- **API 序列**：
  `OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid)` → `SetPriorityClass(h, cls)` →
  `CloseHandle(h)`。
  读当前值的另一条路：`OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` → `GetPriorityClass(h)`
  用于在菜单上打勾。
- **失败处理**：
  - `ERROR_ACCESS_DENIED`(5)：提升到更高的优先级而自身权限不足。原版不是直接报错，
    而是**降级重试**：`REALTIME` 失败 → 自动改成 `HIGH` 并提示
    MUI `STR 2029`"无法将优先级设置为"实时"。已将优先级设置为"高"。"。
  - `ERROR_INVALID_PARAMETER`(87)：进程已退出。
  - 通用错误弹 MUI `STR 2034` 的模板"尝试执行命令时，出现以下系统错误: " + `FormatMessageW`。
- **确认框**：原版在改优先级前会问 MUI `STR 2028`"是否要更改"%s"的优先级?"。
- **提权**：**`REALTIME` 必须 `SeIncreaseBasePriorityPrivilege`（管理员）**，
  且对非自己拥有的进程设 `HIGH`/`ABOVE_NORMAL` 也需要管理员。
  其余（`NORMAL`/`BELOW`/`IDLE`）对自己的同级进程不需要。
- **分类**：`CMD_PRIO_REALTIME` = B；其余 5 项 + `CMD_SET_PRIORITY` = A。

### CMD_SET_AFFINITY (40108)

- **规格**：弹"处理器相关性"对话框（原版 MUI `DLG 34700` / `34701`，
  控件类型【确证-MUI】是 **`SysListView32`**，不是复选框矩阵）。但**视觉上**它呈现为
  2 列多行的 `CPU 0 / CPU 1 / ...` 勾选列表，所以用复选框矩阵还原外观是合理的等价实现。
- **API 序列**：
  1. `OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_SET_INFORMATION, FALSE, pid)`。
  2. `GetSystemInfo(&si)` → `si.dwNumberOfProcessors`（本机 20）作为上限。
     **更稳**：`GetActiveProcessorGroupCount()` → 若 >1，则要按
     `GetLogicalProcessorInformationEx(RelationGroup)` 分处理器组处理，
     一个 `DWORD_PTR` 掩码只能覆盖一个组（>64 逻辑核时会被截断）。
  3. `GetProcessAffinityMask(h, &procMask, &sysMask)` 取当前状态；
     用户可勾选的项 = `sysMask` 的位。
  4. 对话框确定后：`SetProcessAffinityMask(h, newMask)`。
- **失败处理**：
  - `ERROR_ACCESS_DENIED` → 原版弹 MUI `STR 2327`"无法访问或设置进程相关性" +
    `STR 2328`"访问被拒绝。"。
  - 修正掩码后仍无法设置（进程被 Job 限制）→ 同上。
  - 一个都不勾 → 本地拦掉，`SetProcessAffinityMask(h,0)` 会失败。
- **提权**：对**更高完整性/其他用户**的进程需要管理员；对自己同级不需。
  （注意：`PROCESS_SET_INFORMATION` 不是特权，真正的门槛是对象 DACL。）
- **不需要标准控件**：可以用 `CreateWindowExW(L"BUTTON", ..., BS_AUTOCHECKBOX)` 动态建，
  也可以用 `DialogBoxIndirectParamW` + 内存模板。**SDK 里没有现成的亲和性对话框**。
- **分类**：B（复杂对话框 + 可能的权限）。

### CMD_ANALYZE_CHAIN (40109)

- **规格**：弹"分析等待链"对话框（MUI `DLG 30120`）。控件【确证-MUI】：一棵
  **`SysTreeView32`** 显示等待链 + 一个 **`SysLink`** 指向
  `https://go.microsoft.com/fwlink/?LinkID=136333`（"有关等待链的详细信息"）。
- **这是公开 API**（本次调研纠正的第二个错误）：`wct.h`（SDK 存在，已读取）
  - `OpenThreadWaitChainSession(flags, callback)` → `HWCT`
  - `GetThreadWaitChain(hWct, ctx, flags, ThreadId, &nodeCount, nodes, &isCycle)`
  - `CloseThreadWaitChainSession(hWct)`
  - 【确证-PE】taskmgr.exe 的 **ADVAPI32 延迟导入**里同时有
    `OpenThreadWaitChainSession` / `GetThreadWaitChain` / `CloseThreadWaitChainSession` /
    `RegisterWaitChainCOMCallback`；wdc.dll 里还有 `GetProcessWaitChainAsync`。
    **不是** `NtQueryInformationThread`。
- **API 序列**：
  1. `EnumProcesses` / `CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD)` 或
     `NtQuerySystemInformation(SystemProcessInformation)` 的 `Threads[]` 拿该进程所有 TID。
  2. `HWCT h = OpenThreadWaitChainSession(0, nullptr)`（同步模式）。
  3. 对每个 TID：`nodeCount = WCT_MAX_NODE_COUNT(16)`；
     `GetThreadWaitChain(h, 0, WCT_OUT_OF_PROC_FLAG|WCT_OUT_OF_PROC_COM_FLAG|WCT_OUT_OF_PROC_CS_FLAG,
     tid, &nodeCount, nodes, &isCycle)`。
  4. 返回的 `WAITCHAIN_NODE_INFO[]` 里 `ObjectType`(WctThread/WctMutex/WctCriticalSection/...)
     + `ObjectStatus`(WctStatusBlocked/WctStatusRunning/WctStatusNoAccess/...)
     → 填充 `SysTreeView32`。
  5. `CloseThreadWaitChainSession(h)`。
- **文案**：MUI `STR 2068` 是一组以 `;` 分隔的 5 条状态文本
  （"%s 正在等待另一个进程(%s)。" / "…处于死锁状态…" 等），按 `isCycle` 与节点状态选一条。
- **失败处理**：
  - `WctStatusNoAccess` 节点 → 显示"拒绝访问"而不是漏掉（原版会画出来）。
  - `GetThreadWaitChain` 返回 FALSE 且 `GetLastError()==ERROR_ACCESS_DENIED` →
    整进程都无法分析，弹"访问被拒绝"。
  - 进程已退出 → 提示后关闭对话框。
- **提权**：跨用户/更高完整性进程的等待链要管理员；自己同级进程不需要。
- **标注**：【确证-PE + SDK 头文件】API 与对话框控件；【推测】节点→文案的映射细节。
- **分类**：B（树控件对话框 + 潜在权限）。

### CMD_UAC_VIRT (40111)

- **规格**：切换目标 exe 的 **UAC 文件/注册表虚拟化** shim 层。
- **三态**（对应 MUI `STR 2323` 的"不允许"与 `STR 2097` 的描述"指定是在进程中启用、禁用还是禁止…"）：
  `启用 / 禁用 / 不允许`。
- **注册表位置**【确证-注册表实测】：本机
  `HKCU\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers` 存在，
  实际条目形如：
  ```
  D:\game\Grand Theft Auto Vice City\gta-vc.exe  REG_SZ  ~ WINXPSP3 RUNASADMIN HIGHDPIAWARE
  ```
  即 **值名 = exe 完整路径，值 = `~ ` 开头的空格分隔 token 列表**。
- **层名**【确证-SDB】：`VIRTUALIZE` 这个层名在 `AcLayers.dll`（UTF-16）与
  `C:\Windows\AppPatch\sysmain.sdb`（UTF-16，6 处命中）里都存在。所以
  **启用 = 在 Layers 值里加 `VIRTUALIZE` token**；禁用 = 从 token 列表里删掉它。
- **实现**：
  1. `RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers", 0, nullptr, 0, KEY_READ|KEY_WRITE, ...)`。
  2. `RegQueryValueExW(key, exePath, ...)` 读现有 token 串（可能不存在）。
  3. 拆 `~` 前缀 + token 列表 → 增删 `VIRTUALIZE` → 重组 →
     `RegSetValueExW(key, exePath, 0, REG_SZ, ...)`。
  4. **立刻生效需要重新启动目标进程**（shim 只在进程创建时装载）。
- **"不允许"态**：UAC 虚拟化只对**32 位、且清单里没有显式 `requestedExecutionLevel`** 的进程生效。
  64 位进程、或有清单的进程，原版这一项灰显为"不允许"。
  判断 32 位：`IsWow64Process2`（跨位读目标进程用 `IsWow64Process` + `PROCESS_QUERY_LIMITED_INFORMATION`）。
- **失败处理**：`ERROR_ACCESS_DENIED` → 值可能写在 `HKLM` 的 Layers（需管理员）；
  `ERROR_FILE_NOT_FOUND` 读值时属正常（首次写入）。
  SDK 还有一组半公开的 `apphelp.dll` 导出（`SetPermLayerState` / `SetPermLayers` /
  `SdbSetPermLayerKeys`，已通过导出表确认存在，但**无公开文档**）——
  **建议不要用**，直接写 Layers 键更可控。
- **提权**：写 `HKCU` 不需要；写 `HKLM` 的 Layers 需要管理员。
- **标注**：【确证】Layers 键结构与 `VIRTUALIZE` 层名；【推测/需验证】taskmgr 走的是
  直接写注册表还是 `apphelp` 的 Sdb API —— 因为 **`AppCompatFlags` 这个路径字符串
  在 taskmgr.exe 与 wdc.dll 里都搜不到**（ASCII 与 UTF-16 均为 0 命中），
  且两者都没有静态/延迟导入 `apphelp.dll`。所以它极可能是运行时 `LoadLibrary`+`GetProcAddress`，
  或者根本由别的组件（`wdc.dll` 之外）完成。**实现前建议用 Process Monitor 观察一次原版的
  注册表写入**，再决定路径。
- **分类**：B。

### CMD_DEBUG / CMD_CREATE_DUMP / CMD_OPEN_LOCATION / CMD_SEARCH_ONLINE / CMD_PROPERTIES

详细右键与进程右键共用同一实现（见 1.x）。差异：
- 详细页的"在线搜索"助记符是 `(&N)`，进程页是 `(&S)`（【确证-MUI】30200 vs 30208）。
- 详细页有 `CMD_GOTO_SERVICES` 而没有 `CMD_GOTO_DETAILS`（本来就这页）。

---

## 3. 服务右键（MENU 30202）

### CMD_SVC_START (40140)

- **API 序列**：
  ```
  OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)
  OpenServiceW(scm, name, SERVICE_START | SERVICE_QUERY_STATUS)
  StartServiceW(hSvc, 0, nullptr)
  CloseServiceHandle / CloseServiceHandle
  ```
- **失败处理**：
  - `ERROR_ACCESS_DENIED`(5) → 需管理员（几乎总是这个）。
  - `ERROR_SERVICE_DISABLED`(1058) → "该服务已被禁用"（要用 `ChangeServiceConfig` 改启动类型才能启）。
  - `ERROR_SERVICE_ALREADY_RUNNING`(1056) → 视为成功，静默。
  - `ERROR_SERVICE_DEPENDENCY_FAIL`(1068) / `ERROR_SERVICE_DEPENDENCY_DELETED` → 提示哪个依赖失败
    （`EnumDependentServicesW` 可枚举）。
  - 原版用 `TmShowErrorMessage` → `TaskDialog`。
- **提权**：**必然需要管理员**（除非该服务的 DACL 显式放权）。本机 `net session` 成功，
  说明当前环境已提权，非管理员下这一项会 100% 失败。
- **注意**：`SC_MANAGER_CONNECT` 就够了；不要用 `SC_MANAGER_ALL_ACCESS`（会无谓地要求管理员）。
- **原版**：`WdcListView::StartServiceW` / `WdcStartService`（【确证-STR】符号名）。
- **分类**：B。

### CMD_SVC_STOP (40141)

- **API 序列**：
  ```
  OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT)
  OpenServiceW(scm, name, SERVICE_STOP | SERVICE_QUERY_STATUS)
  ControlService(hSvc, SERVICE_CONTROL_STOP, &status)
  ```
- **失败**：`ERROR_SERVICE_NOT_ACTIVE`(1062) → 视为成功；
  `ERROR_SERVICE_CANNOT_ACCEPT_CTRL`(1061) → 服务不可停止（要说明原因）；
  `ERROR_ACCESS_DENIED` → 需管理员。
- **提权**：需要管理员。**分类**：B。

### CMD_SVC_RESTART (40142)

- **顺序**：`ControlService(STOP)` → `QueryServiceStatusEx(SC_STATUS_PROCESS_INFO)` 轮询到
  `SERVICE_STOPPED`（超时 5–10 s）→ `StartServiceW`。
- **重要边界**：服务若有依赖/被依赖，直接 Stop 会失败。原版对"依赖其他服务的服务"会先
  提示（`EnumDependentServicesW` + `SERVICE_DEPENDENT_SERVICES`）。
- **替代（更稳，需管理员）**：`net stop X && net start X` 同理；
  服务若配了 `SERVICE_FAILURE_ACTIONS` 会被 SCM 自动重启，再手动 Start 会得到
  `ERROR_SERVICE_ALREADY_RUNNING`，按成功处理。
- **提权**：需要管理员。**分类**：B。
- **现有实现**：`ServiceSource::Restart` 已按此顺序实现，但 `Stop` 失败就整体返回 false，
  没区分"本来就没运行"。

### CMD_SVC_OPEN_SCM (40143)

- **确切行为**：【确证-STR】taskmgr 里有 `%SystemRoot%\System32\services.msc` 与
  `Open Services` 字样。
- **实现**：`ShellExecuteW(hwnd, L"open", L"services.msc", nullptr, nullptr, SW_SHOWNORMAL)`。
  想直接定位到该服务可用 `/s` 参数，但 **`services.msc` 不支持按服务名跳转**（【需验证】），
  所以原版只是打开服务控制台。
- **提权**：不需要（普通用户也能打开 services.msc，只是不能操作）。
- **分类**：C。

---

## 4. 用户右键（MENU 30211）

> 会话 ID 来自 `WTSEnumerateSessionsW`（本项目已实现，8.59 ms / 4 会话）。
> 所有 WTS 函数第一个参数都用 `WTS_CURRENT_SERVER_HANDLE`。

### CMD_USR_CONNECT (40150)

- **API**：`WTSConnectSessionW(ULONG LogonId, ULONG TargetLogonId, PWSTR pPassword, BOOL bWait)`
  —— 签名已在 `um\wtsapi32.h` 第 841 行确认，`wtsapi32.dll` 导出也已确认。
- **取值**：`WTSQuerySessionInformationW(..., WTSLogonId, ...)` 拿两个会话的 logon id
  （`LogonId` = 当前会话的，`TargetLogonId` = 目标会话的）。
- **需要密码**：`pPassword` 为 `NULL` 时**只能重连"已断开但已登录"的会话**；
  连到别人的活动会话需要该用户的密码。原版会弹一个凭据输入框（【推测】用 `credui.dll`，
  【确证-PE】taskmgr 确实导入了 `credui.dll`）。
- **失败**：`ERROR_ACCESS_DENIED` → 需管理员 / 密码错；
  `ERROR_CTX_WINSTATION_BUSY` → 目标会话已在连接。
- **提权**：把一个会话连到控制台需要管理员。**分类**：B。

### CMD_USR_DISCONNECT (40151)

- **API**：`WTSDisconnectSession(WTS_CURRENT_SERVER_HANDLE, sessionId, FALSE)`。
  【确证-PE】taskmgr 延迟导入里有它。
- **失败**：`ERROR_ACCESS_DENIED` → 需管理员（断开其他用户）；
  session 0（服务会话）不可断开 → `ERROR_CTX_WINSTATION_NOT_FOUND`，应**灰显**该项。
- **提权**：断开自己不需要，断开别人需要管理员。**分类**：B。

### CMD_USR_LOGoff (40152)

- **API**：`WTSLogoffSession(WTS_CURRENT_SERVER_HANDLE, sessionId, FALSE)`。【确证-PE】
- **确认框**：原版必弹，文案是 MUI `STR 2326`
  "如果注销用户，则该用户未保存的数据可能会丢失。"（**这句是本次调研直接读到的，
  请逐字使用**）。
- **失败**：`ERROR_ACCESS_DENIED` → 需管理员；session 0 → 灰显。
- **提权**：注销其他用户需要管理员。**分类**：B。

### CMD_USR_SEND_MSG (40153)

- **API**【确证-PE：`WTSSendMessageW` 在延迟导入里】：
  ```
  DWORD resp = 0;
  WTSSendMessageW(WTS_CURRENT_SERVER_HANDLE, sessionId,
                  pTitle, wcslen(pTitle)*sizeof(WCHAR),
                  pText,  wcslen(pText)*sizeof(WCHAR),
                  MB_OK | MB_ICONINFORMATION,
                  timeoutSeconds, &resp, FALSE);
  ```
  **注意 `TitleLength`/`MessageLength` 是字节数，不是字符数**（最常见的实现错误）。
- **对话框**：MUI `DLG 34850` "发送消息"，三个输入：标题 / 消息 / 超时秒数
  （超时单位见 `STR 2351` "%1 秒"）。
- **失败**：`ERROR_ACCESS_DENIED` → 需管理员；目标会话已断开 → `ERROR_CTX_*`，
  应询问是否改为发送到下一次登录。
- **提权**：发给自己会话不需要；发给别人需要管理员。**分类**：B。

### CMD_USR_SWITCH (40154)

- **正确做法**：`WinStationConnectAndLockDesktop(...)`（`winsta.dll`）。
  【确证-PE】taskmgr 的 WINSTA.dll 延迟导入里正好是
  **`WinStationConnectAndLockDesktop`** + `WinStationConnectW` + `WinStationGetProcessSid`。
  这解释了"切换用户账户"的原版语义：**把控制台连到目标会话，同时锁住原桌面**。
- **巨大坑**：**SDK 里没有 `winsta.h`**。本次调研确认
  `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um\` 下**只有**
  `winsta.lib`（在 Lib 目录），头文件不存在。所以调用它必须：
  1. 自己按 `WinStationConnectW(HANDLE hServer, ULONG SessionId, ULONG TargetSessionId,
     PWCHAR pPassword, BOOL bWait)` 声明原型（【需验证】`WinStationConnectAndLockDesktop`
     的确切参数表——**没有头文件就无法从 SDK 确认**，必须靠实测或逆向）。
  2. 链接 `winsta.lib`（已确认存在）。
  - 或者改用公开的 `WTSConnectSessionW`（见 `CMD_USR_CONNECT`），但它不带"锁桌面"语义。
- **降级路径**：`ShellExecuteW(hwnd, L"open", L"LogonUI.exe", ...)` 不可行（没有这种用法）。
  可行的近似：`rundll32.exe user32.dll,LockWorkStation`（锁当前桌面，不切换），
  或直接打开 `tsdiscon.exe`（**错误**，只是断开）。
- **提权**：需要管理员。**分类**：B（无头文件 + 需管理员）。
- **现有实现**：`ShellExecuteW(L"tsdiscon.exe")` —— **语义错误**，建议改成上面对话框方案。

### CMD_USR_MANAGE (40155)

- **原版线索**：【确证-STR】taskmgr 里有 `Microsoft.UserAccounts`、
  `%SystemRoot%\System32\ShellStyle.dll`、`properties` 三串紧邻出现 ——
  说明它是打开"控制面板 → 用户帐户"的**属性页**。
- **实现**：
  - 首选：`ShellExecuteW(hwnd, L"open", L"control.exe", L"/name Microsoft.UserAccounts", ...)`
  - 备选（Win10 设置）：`ShellExecuteW(hwnd, L"open", L"ms-settings:otherusers", ...)`
  - 不要用 `lusrmgr.msc` —— 那是"本地用户和组"（计算机管理），语义不同。
- **提权**：`control.exe /name Microsoft.UserAccounts` 普通用户可用（只能改自己）；
  改别人要 UAC 提升。**分类**：C。
- **现有实现**：`ShellExecuteW(L"lusrmgr.msc")` —— 语义偏差，建议改。

---

## 5. 启动项右键（MENU 30213）

### CMD_STARTUP_ENABLE (40160) / CMD_STARTUP_DISABLE (40161)

- **规格**：改 `StartupApproved` 的状态 DWORD。
- **注册表**（本项目 `pages_data_startup.cpp` 已实测并实现）：
  ```
  HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run       (HKCU\Run 及 HKLM\Run 的每用户视图)
  HKCU\...\StartupApproved\Run32     (Wow6432Node 的 Run)
  HKCU\...\StartupApproved\StartupFolder
  ```
  值格式：**12 字节 `REG_BINARY`**，`[0..3]` = 状态 DWORD（**2 = 启用，3 = 禁用**），
  `[4..11]` = 禁用时刻的 `FILETIME`。
- **API**：`RegCreateKeyExW(KEY_SET_VALUE)` → `RegSetValueExW(name, REG_BINARY, 12 字节)`。
- **失败**：`ERROR_ACCESS_DENIED` → 极少数项的状态在 HKLM 的策略键下，需管理员。
- **提权**：写 `HKCU` **不需要**。
- **生效时机**：下次登录才生效（原版会在"启动影响"列后加一句提示，**没有**）。
- **分类**：A。
- **补充**：UWP 启动项走的是
  `Windows.ApplicationModel.Internal.StartupTaskInternal` WinRT API
  （【确证-STR】该字符串与一整套 `IAsyncOperation<IVectorView<StartupTaskInternal>>`
  WRL 模板符号都在 taskmgr 里），本项目当前只枚举注册表 Run 键，暂不涉及。

---

## 6. 性能页右键（MENU 30206 / 30207）

### CMD_CHART_OVERALL (40170) / CMD_CHART_LOGICAL (40171)

- **规格**：切 CPU 页的图表模式。纯本地状态。
- **API**：无。改 `ChartMode` + `CheckMenuRadioItem`（互斥单选）+ `InvalidateAll()`。
- **注意**：原版是**单选**（同一时刻只有一个勾），不是复选。
  本项目用 `MF_CHECKED`，多选会出现两个勾，应换成 `CheckMenuRadioItem`。
- **数据**：`PerfSnapshot::cores[]` 已有每核利用率与 kernel/user 拆分，两种模式都够用。
- **分类**：A。

### CMD_CHART_NUMA (40172)

- **规格**：按 NUMA 节点折叠的逻辑处理器矩阵。
- **API**：
  `GetNumaHighestNodeNumber(&maxNode)` +
  `GetLogicalProcessorInformationEx(RelationNumaNode, buf, &len)`
  （`SYSTEM_NUMA_NODE` 含 `NodeNumber`；配 `RelationGroup` 拿组内掩码）。
- **失败**：单节点机器（本机就是 1 个节点）→ 原版把这一项**灰显**。
- **提权**：不需要。但需要新增数据结构（节点→CPU 列表）。
- **分类**：B（需新采集，无权限要求）。

### CMD_CHART_SINGLE_ENG (40173) / CMD_CHART_MULTI_ENG (40174)

- **规格**：GPU 页按"单个引擎 / 多个引擎"显示。
  MUI 里对应 `Multiple engines` / `Single engine`（【确证-STR】）。
- **数据缺口**：本项目 `PerfSnapshot::gpus[]` 只有**每适配器**的总利用率，
  没有**每引擎**（3D / Copy / VideoDecode / VideoEncode / Compute）细分。
  要做需要在 `perfdata.cpp` 里按实例名解析
  `\GPU Engine(*)\Utilization Percentage` 的 `pid_<n>_luid_<x>_<y>_phys_<n>_eng_<n>_engtype_<TYPE>` 串。
- **失败**：非 GPU 页 → 灰显；无 GPU → 两项都灰显。
- **提权**：不需要。**分类**：B（需扩充采集）。

### CMD_SHOW_KERNEL_TIME (40175)

- **规格**：CPU 图叠加内核时间曲线（原版是图里加一条深蓝的"内核时间"线）。
- **API**：无。**数据已就绪** —— `PerfSnapshot::cpuKernelPercent` 已算好
  （`perfdata.cpp` 用 `KernelTime - IdleTime` 差分）。
- **实现**：`PaintChart` 里画第二条线（或填充）+ `MF_CHECKED` 切换。
- **分类**：A（现成数据，只差绘制）。

### CMD_GRAPH_SUMMARY (40176)

- **规格**：性能页右侧是否显示"摘要视图"（图形摘要）。
  【确证-STR】`Graph summary view on/off`、`Summary view on/off`。
- **API**：无，纯本地布尔。注意原版在 `MENU 30207`（侧栏右键）里也有同一项，两处共享状态。
- **分类**：A。

### CMD_COPY (40177)

- **规格**：把当前选中行以 **UTF-16 文本**放进剪贴板（制表符分列）。
  【确证-PE】taskmgr 导入 `OpenClipboard`/`EmptyClipboard`/`SetClipboardData`/`CloseClipboard`。
- **API 序列**：
  ```
  OpenClipboard(hwnd) → EmptyClipboard()
  → HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (len+1)*sizeof(WCHAR))
  → memcpy → GlobalUnlock → SetClipboardData(CF_UNICODETEXT, h) → CloseClipboard()
  ```
  **`SetClipboardData` 成功后不要 `GlobalFree`**（所有权已转移）。
- **失败**：`OpenClipboard` 失败（别的进程占着）→ 重试 3 次 × 50 ms，再失败静默放弃。
- **边界**：组头行 / 无选中 → 不做事，**不要**复制空串。
- **提权**：不需要。**分类**：A。

### CMD_OPEN_RESMON (40178)

- **确切行为**：【确证-STR】`%systemroot%\system32\resmon.exe` +
  `Open Resource Monitor` + `resmonLaunch`。
- **实现**：`ShellExecuteW(hwnd, L"open", L"resmon.exe", nullptr, nullptr, SW_SHOWNORMAL)`。
  resmon 的清单里有 `autoElevate`/`highestAvailable`，系统会自动弹 UAC，
  **不需要**在调用端用 `runas` 动词。
- **失败**：被组策略禁用 → `ShellExecuteW` 返回值 ≤ 32 → 弹提示。
- **提权**：resmon 自身要管理员，UAC 由系统接管。**分类**：C。

---

## 7. 主菜单（MENU 30205 / 30208）

### CMD_NEW_TASK (40001)

- **规格**："运行新任务"对话框。MUI 里没有对应 DIALOG 资源（【确证-MUI】DIALOG 只有
  30120/30130/34665/34700/34701/34801/34802/34850），说明它是自绘或运行时构造的。
- **API**：对话框确定后
  `CreateProcessW(nullptr, cmdline, ..., CREATE_UNICODE_ENVIRONMENT, ...)`。
- **"以管理员权限创建此任务"复选框**（原版有）→ 用 `ShellExecuteW(hwnd, L"runas", exeOrCmd, ...)`
  或 `CreateProcessWithTokenW`（需 `SeImpersonatePrivilege`）。
  【确证-STR】taskmgr 里有一处孤立的 `runas` 字符串，支持这个判断。
- **失败**：`ERROR_FILE_NOT_FOUND`(2) / `ERROR_PATH_NOT_FOUND`(3) → 原版会**再试一次**
  用 `%COMSPEC%` 包一层（`%systemroot%\system32\cmd.exe` 这个串在 taskmgr 里逐字存在，
  【确证-STR】），这样 `dir` 这种内建命令也能跑。
- **提权**：本身不需要；勾了复选框才需要 UAC 同意。
- **分类**：B（对话框）。

### CMD_ALWAYS_ON_TOP (40010)

- **API**：`SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE)`。
- **持久化**：原版把首选项存在
  `HKCU\Software\Microsoft\Windows\CurrentVersion\TaskManager` 的 `Preferences`（REG_BINARY）
  （【确证-STR】这两个串在 taskmgr 里紧邻出现）。本项目目前不持久化，可接受。
- **分类**：A。

### CMD_MINIMIZE_ON_USE (40011)

- **规格**："使用时最小化" —— **让出前台时自动最小化**。
  实现：处理 `WM_ACTIVATE`/`WM_KILLFOCUS`，当 `WA_INACTIVE` 且该选项开启 →
  `ShowWindow(hwnd, SW_MINIMIZE)`。
- **坑**：不要用 `WM_KILLFOCUS`（点自己菜单也会触发）。用 `WM_ACTIVATE` 的
  `LOWORD(wp)==WA_INACTIVE`，并排除"新前台是本进程的窗口"的情况。
- **分类**：A。

### CMD_HIDE_WHEN_MIN (40012)

- **规格**："最小化时隐藏" —— 最小化时把窗口从任务栏移除。
- **API**：`ShowWindow(hwnd, SW_HIDE)` 会连任务栏一起隐藏；或者
  `ITaskbarList::DeleteTab(hwnd)`（COM，`CLSID_TaskbarList`）。
  两者都要提供"怎么再显示出来"的路径（托盘图标 → 原版 `MENU 30215` 托盘菜单有"还原"）。
- **分类**：A（但要配托盘图标，工作量翻倍）。

### CMD_SHOW_FULL_NAME (40013)

- **规格**："显示完整账户名" —— 用户名列从 `DOMAIN\user` 变 `user`（或反之）。
- **实现**：纯数据层开关，影响 `ResolveUser()` 的输出格式 + `RebuildRows()`。
- **坑**：`sidNameCache_` / `userCache_` 里的缓存值不应随开关变化，
  应在**渲染时**按开关拼接，而不是缓存两种格式。
- **分类**：A。

### CMD_SHOW_ALL_HISTORY (40014)

- **规格**："显示所有进程的历史记录" —— 应用历史记录页是否包含已退出应用。
- **现状**：本项目 `AppHistorySource::Enumerate` 恒返回 false（SRUM 不可达，
  见 `docs/specs/minor_pages_spec.md` 第 4 节），所以这一项**只改一个布尔、页面仍显示"无可用数据"**。
- **实现**：菜单打勾 + 传参给 `RefreshAppHistory`。**分类**：A（占位实现即可）。

### CMD_GROUP_BY_TYPE (40035)

- **规格**："按类型分组" —— 关闭后进程页变成扁平列表（不显示"应用/后台进程/Windows 进程"组头）。
- **实现**：`groupByType_` 布尔 + `RebuildRows()` 里跳过 `GroupHeaderRow`。
- **坑**：关闭分组时，原来缩进的应用子进程要**保持缩进或拉平**，二者选一并保持一致；
  另外 `selectedRow_` 是扁平索引，切换后必须按 pid 重新定位。
- **分类**：A。

### 其它已属于 A 类、无需展开的

`CMD_EXIT`（`PostMessage(hwnd, WM_CLOSE)`）、`CMD_REFRESH_NOW`（触发一次采集）、
`CMD_UPDATE_HIGH/NORMAL/LOW/PAUSED`（改定时器间隔）、`CMD_EXPAND_ALL`/`CMD_COLLAPSE_ALL`、
`CMD_HIDE_COLUMN`（隐藏当前列 → 需要列模型支持按列 id 隐藏）、
`CMD_DEF_TAB_BASE+0..6`。

`CMD_SELECT_COLUMNS` (40131) 例外：原版是"选择列"对话框（MUI `STR 2027`/`2028`
"选择将显示在表中的列。"），带复选框列表 + 上移/下移按钮 → **B 类**。

---

## 8. 实现优先级（A / B / C）

### A 类 —— 39 个：纯本地 API，无提权，应优先做

| 分组 | 命令 |
|---|---|
| 主菜单·文件/选项 | `CMD_EXIT`、`CMD_ALWAYS_ON_TOP`、`CMD_MINIMIZE_ON_USE`、`CMD_HIDE_WHEN_MIN`、`CMD_SHOW_FULL_NAME`、`CMD_SHOW_ALL_HISTORY` |
| 主菜单·查看 | `CMD_REFRESH_NOW`、`CMD_UPDATE_HIGH`、`CMD_UPDATE_NORMAL`、`CMD_UPDATE_LOW`、`CMD_UPDATE_PAUSED`、`CMD_GROUP_BY_TYPE`、`CMD_EXPAND_ALL`、`CMD_COLLAPSE_ALL`、`CMD_DEF_TAB_BASE`(+6 子项)、`CMD_HIDE_COLUMN` |
| 进程右键 | `CMD_EXPAND`、`CMD_COLLAPSE`、`CMD_SWITCH_TO`、`CMD_END_TASK`、`CMD_END_TREE`、`CMD_GOTO_DETAILS`、`CMD_PROPERTIES` |
| 详细右键 | `CMD_SET_PRIORITY`（容器）、`CMD_PRIO_HIGH`、`CMD_PRIO_ABOVE`、`CMD_PRIO_NORMAL`、`CMD_PRIO_BELOW`、`CMD_PRIO_LOW`、`CMD_GOTO_SERVICES` |
| 进程/详细·资源值 | `CMD_VAL_PERCENT`、`CMD_VAL_VALUE` |
| 启动项 | `CMD_STARTUP_ENABLE`、`CMD_STARTUP_DISABLE` |
| 性能页 | `CMD_CHART_OVERALL`、`CMD_CHART_LOGICAL`、`CMD_SHOW_KERNEL_TIME`、`CMD_GRAPH_SUMMARY`、`CMD_COPY` |

> 注意：`CMD_END_TASK` / `CMD_END_TREE` / `CMD_PROPERTIES` 对**更高完整性级别**的目标进程
> 仍会失败，但不需要预先提权，失败时按 `ERROR_ACCESS_DENIED` 提示即可，所以归 A。

### B 类 —— 19 个：需管理员，或复杂/自绘对话框，或未公开 API

| 命令 | 门槛 |
|---|---|
| `CMD_NEW_TASK` | 对话框 + 可选 `runas` |
| `CMD_RESTART` | 跨位 PEB 读命令行 / AUMID |
| `CMD_SET_AFFINITY` | 自绘对话框 + 处理器组 |
| `CMD_ANALYZE_CHAIN` | `wct.h` API + 树控件对话框 + 权限 |
| `CMD_UAC_VIRT` | 注册表 Layers + 三态判定 |
| `CMD_CREATE_DUMP` | dbghelp + 后台线程 + `SeDebugPrivilege` |
| `CMD_PRIO_REALTIME` | `SeIncreaseBasePriorityPrivilege` |
| `CMD_SELECT_COLUMNS` | 选择列对话框 |
| `CMD_SVC_START` | 管理员 |
| `CMD_SVC_STOP` | 管理员 |
| `CMD_SVC_RESTART` | 管理员 |
| `CMD_USR_CONNECT` | 密码输入 + 管理员 |
| `CMD_USR_DISCONNECT` | 管理员 |
| `CMD_USR_LOGoff` | 管理员 + 确认框 |
| `CMD_USR_SEND_MSG` | 管理员 + 三个输入的对话框 |
| `CMD_USR_SWITCH` | **SDK 无 `winsta.h`**，需自声明 + 管理员 |
| `CMD_CHART_NUMA` | 需新增 NUMA 采集 |
| `CMD_CHART_SINGLE_ENG` | 需新增每引擎 GPU 采集 |
| `CMD_CHART_MULTI_ENG` | 需新增每引擎 GPU 采集 |

### C 类 —— 7 个：一行 `ShellExecute`

| 命令 | 目标 |
|---|---|
| `CMD_PROVIDE_FEEDBACK` | `feedback-hub:?tabid=2&origin=taskmanager` |
| `CMD_DEBUG` | 读 `AeDebug\Debugger` → `"<dbg>" -p <pid>`；无值则提示 |
| `CMD_OPEN_LOCATION` | `explorer.exe /select,"<path>"` 或 `SHOpenFolderAndSelectItems` |
| `CMD_SEARCH_ONLINE` | `https://www.bing.com/search?q=<exe名>` |
| `CMD_SVC_OPEN_SCM` | `services.msc` |
| `CMD_USR_MANAGE` | `control.exe /name Microsoft.UserAccounts` |
| `CMD_OPEN_RESMON` | `%systemroot%\system32\resmon.exe` |

### 8.1 最值得先做的 10 个

按"用户可见度 ÷ 实现成本"排序：

| # | 命令 | 理由 | 成本 |
|---|---|---|---|
| 1 | **`CMD_SWITCH_TO`** | 每天用最多、非管理员可用、有 `SwitchToThisWindow` 现成解 | ~40 行 |
| 2 | **`CMD_END_TREE`** | 可见度极高；建树用现成 `parentPid`，无新 API | ~50 行（含修 `CollectProcessTree` 的迭代器失效） |
| 3 | **`CMD_SET_PRIORITY` + 5 个非实时子项** | 6 个菜单项一次点亮，一个 `SetPriorityClass` | ~60 行 |
| 4 | **`CMD_COPY`** | `Ctrl+C` 已在加速键表里绑了但没实现，用户一按就没反应 | ~40 行 |
| 5 | **`CMD_SEARCH_ONLINE`** | 1 行 `ShellExecute`，URL 已确证 | ~10 行 |
| 6 | **`CMD_PROPERTIES`** | 1 行 `SHObjectProperties` 就能拿到原版效果（替换现在的自绘 MessageBox） | ~5 行 |
| 7 | **`CMD_CREATE_DUMP`** | 原版标志性功能，`MiniDumpWriteDump` 一个调用（要注意后台线程） | ~120 行 |
| 8 | **`CMD_VAL_PERCENT`/`CMD_VAL_VALUE`** | 菜单里已有但点了没反应，改动极小（但要先把 ID 扩成 3×2） | ~30 行 |
| 9 | **`CMD_SHOW_KERNEL_TIME`** | `cpuKernelPercent` 已采好，只差画第二条线 | ~30 行 |
| 10 | **`CMD_SVC_START`/`STOP`/`RESTART`** | `ServiceSource` 里三个函数已实现，只差菜单分发 + 错误文案 | ~40 行 |

> 次优（第 11–14）：`CMD_UAC_VIRT`、`CMD_ANALYZE_CHAIN`、`CMD_SET_AFFINITY`、
> `CMD_USR_SEND_MSG` —— 都要对话框或注册表，建议排第二轮。

---

## 9. 附录

### 9.1 taskmgr.exe 导入表证据（本次实测，非引用文档）

| DLL | 与菜单相关的导入 | 对应命令 |
|---|---|---|
| KERNEL32 | `SetPriorityClass`、`GetPriorityClass`、`OpenProcess`、`TerminateProcess`、`CreateProcessW`、`QueryFullProcessImageNameW`、`ReadProcessMemory`、`GetLogicalProcessorInformationEx`、`GetSystemInfo`、`SetProcessWorkingSetSize`、`OpenProcessToken` | PRIO_*、END_TASK、NEW_TASK、OPEN_LOCATION、RESTART、CHART_NUMA |
| ntdll | `NtQueryInformationProcess`、`NtQueryInformationThread`、`NtSetInformationProcess`、`NtQuerySystemInformation`、`LdrQueryProcessModuleInformation` | RESTART(命令行)、END_TASK(关键进程) |
| USER32 | `SwitchToThisWindow`、`AllowSetForegroundWindow`、`SetForegroundWindow`、`ChangeWindowMessageFilterEx`、`IsHungAppWindow`、`GhostWindowFromHungWindow`、`HungWindowFromGhostWindow`、`OpenClipboard`/`SetClipboardData`、`TaskDialogIndirect`、`TaskDialog` | SWITCH_TO、COPY、所有错误提示 |
| SHELL32 | `ShellExecuteW`、`ShellExecuteExW`、`SHOpenFolderAndSelectItems`、`SHParseDisplayName`、`SHGetKnownFolderItem`、`CommandLineToArgvW` | OPEN_LOCATION、SEARCH_ONLINE、所有 C 类 |
| ole32 | `CoAllowSetForegroundWindow`、`CreateBindCtx` | SWITCH_TO |
| **延迟加载** ADVAPI32 | `OpenThreadWaitChainSession`、`GetThreadWaitChain`、`CloseThreadWaitChainSession`、`RegisterWaitChainCOMCallback` | **ANALYZE_CHAIN** |
| **延迟加载** WTSAPI32 | `WTSEnumerateSessionsW`、`WTSQuerySessionInformationW`、`WTSLogoffSession`、`WTSDisconnectSession`、**`WTSSendMessageW`**、`WTSRegisterSessionNotification` | USR_* |
| **延迟加载** WINSTA | **`WinStationConnectAndLockDesktop`**、**`WinStationConnectW`**、`WinStationGetProcessSid` | **USR_SWITCH / USR_CONNECT** |
| **延迟加载** dbghelp | **`MiniDumpWriteDump`** | **CREATE_DUMP** |
| **延迟加载** faultrep | `ReportHang` | END_TASK 的"未响应"上报 |
| **延迟加载** api-ms-win-service-management | `OpenSCManagerW`、`OpenServiceW`、`StartServiceW`、`ControlService` | SVC_* |
| credui | （整套凭据 UI） | USR_CONNECT 的密码框 |
| api-ms-win-core-appcompat | `BaseReadAppCompatDataForProcess`、`BaseFreeAppCompatDataForProcess` | UAC_VIRT 的**读**（只读，无写） |
| 注册表 API 集 | `RegCreateKeyExW`、`RegSetValueExW`、`RegDeleteValueW`、`RegQueryValueExW`、`RegEnumValueW` | STARTUP_ENABLE/DISABLE、UAC_VIRT |

**没有出现的**（同样重要）：`CreateToolhelp32Snapshot`（0 次）、任何 Job Object API（0 次）、
`SetProcessAffinityMask`（0 次，**在 wdc.dll 里也没有** —— 亲和性对话框极可能经由
`NtSetInformationProcess(ProcessAffinityMask)`，或者由别处实现，见 9.3）、
`vsjitdebugger`（0 次）。

### 9.2 taskmgr.exe 字符串证据（UTF-16，逐字）

```
feedback-hub:?tabid=2&origin=taskmanager
https://www.bing.com/search?q=
%SystemRoot%\System32\services.msc
%systemroot%\system32\resmon.exe
SOFTWARE\Microsoft\Windows NT\CurrentVersion\AeDebug
No default debugger present
%s -p %ld
.DMP
properties / Microsoft.UserAccounts / %SystemRoot%\System32\ShellStyle.dll
%windir%\Explorer.exe
%systemroot%\system32\cmd.exe
Open Resource Monitor / resmonLaunch / servicesLaunch
Graph summary view on|off, Summary view on|off, Show kernel times
Set affinity / Analyze wait chain / End process tree / Create dump file /
UAC virtualization / Go to service(s) / Open file location / Search online /
Priority Realtime|High|Above normal|Normal|Below normal|Low / Properties / Restart
```

### 9.3 MUI 资源证据（对话框与错误文案）

```
DIALOG 30120  "分析等待链"      SysTreeView32 + SysLink(fwlink 136333)
DIALOG 30130  "选择处理器"      SysListView32  ("处理器")
DIALOG 34700/34701 "处理器相关性" SysListView32  ("处理器")
DIALOG 34801/34802 "转储进程"   34802 含 "文件位于:"
DIALOG 34850  "发送消息"

STR 2028 "是否要更改"%s"的优先级?"
STR 2029 "无法将优先级设置为"实时"。已将优先级设置为"高"。"
STR 2034 "尝试执行命令时，出现以下系统错误: "
STR 2068 等待链 5 条状态文案（"…正在等待另一个进程(%s)。" 等，';' 分隔）
STR 2097 "指定是在进程中启用、禁用还是禁止用户账户控制(UAC)虚拟化"
STR 2323 "不允许"                       ← UAC 虚拟化的第三态
STR 2326 "如果注销用户，则该用户未保存的数据可能会丢失。"
STR 2327 "无法访问或设置进程相关性"
STR 2328 "访问被拒绝。"
STR 2751 "无法打开所选进程的反馈中心"
STR 2351 "%1 秒"                        ← 发送消息的超时单位
```

### 9.4 shim 层名证据

- `VIRTUALIZE` 在 `C:\Windows\System32\AcLayers.dll`（UTF-16，1 处）与
  `C:\Windows\AppPatch\sysmain.sdb`（UTF-16，6 处）中存在。
- `RUNASADMIN` 只出现在**注册表实际数据**里（本机 HKCU Layers 有 3 条真实条目），
  不在 SDB 明文中 —— 说明 token 由 `apphelp` 运行时按层名匹配，不需要 SDB 明文。
- `C:\Windows\AppPatch\sysmain.sdb` 是 "Windows application compatibility Shim DataBase"
  （`sdbf` 魔数），非明文文本格式，不能用 `strings` 直接枚举全部层名。

### 9.5 与当前代码的已知偏差（写作时的快照，供对照）

> 调研期间 `src/tmwindow_menus.cpp` 已经出现了一份 54 个 case 的实现（另一路并行产出）。
> 下面是"实际代码 vs 本规格"的差异点，**只做记录，未修改**：

| 命令 | 当前代码 | 规格要求 |
|---|---|---|
| `CMD_DEBUG` | 硬编码 `GetSystemDirectory()+vsjitdebugger.exe`，本机不存在 → 恒弹"未找到调试器" | 读 `HKLM\...\AeDebug\Debugger`，套 `"%s -p %ld"` |
| `CMD_PROVIDE_FEEDBACK` | `https://support.microsoft.com/windows/send-feedback-to-...` | `feedback-hub:?tabid=2&origin=taskmanager` |
| `CMD_USR_SWITCH` | `ShellExecute("tsdiscon.exe")`（只是断开，不切换） | `WinStationConnectAndLockDesktop` |
| `CMD_USR_MANAGE` | `ShellExecute("lusrmgr.msc")`（本地用户和组） | `control.exe /name Microsoft.UserAccounts` |
| `CMD_PROPERTIES` | 自绘文本 MessageBox | `SHObjectProperties(SHOP_FILEPATH, path, NULL)` |
| `CMD_UAC_VIRT`、`CMD_ANALYZE_CHAIN` | 合并成一条"尚未实现"的 MessageBox | 两条独立实现（见 2.x） |
| `CMD_CREATE_DUMP` | 存到桌面，`MiniDumpNormal\|WithDataSegs` | 存到 `%TEMP%`，建议加 `WithFullMemory` |
| `CMD_RESTART` | PEB 偏移写死 64 位（`0x20`/`0x70`），无 WOW64 分支 | 需 `IsWow64Process` 分支到 `0x10`/`0x40` |
| `CMD_END_TREE` | `CollectProcessTree` 在 range-for 内 `push_back`（迭代器失效 UB） | 先建 `parent→children` 索引再 BFS |
| `CMD_VAL_PERCENT/VALUE` | 单一全局开关 | 需 内存/磁盘/网络 三组独立（ID 需扩展） |
| `CMD_CHART_*` | 用 `MF_CHECKED`（可多选） | 应 `CheckMenuRadioItem` 单选 |
| 未实现 | `CMD_CHART_NUMA/SINGLE_ENG/MULTI_ENG`、`CMD_GRAPH_SUMMARY`、`CMD_SHOW_KERNEL_TIME`、`CMD_HIDE_COLUMN`、`CMD_SELECT_COLUMNS`、`CMD_SHOW_ALL_HISTORY`、`CMD_USR_CONNECT`、`CMD_SET_PRIORITY`（容器本身） | 见第 6/7 节 |
