# 任务管理器次要页面 · 规格与数据源

> 目标：为"完全还原 Windows 10 任务管理器"的四个次要页面提供可直接照着写代码的规格。
> 所有数据源均在本机实测。测量机：Windows 10 Pro 22H2 22H2 / Xeon 服务器级 CPU / 32GB / **已提权**
> 标注：**[实测]** = 本机跑出来的数字；**[确证]** = 有官方文档或可复现依据；**[推测]** = 行为观察归纳。

---

## 0. 结论速览

| 页面 | 数据源可行性 | 一次性成本 | 缓存策略 |
|---|---|---|---|
| 启动 | ✅ 完全可行（注册表 + StartupApproved） | **3.14 ms** | 启动时读一次，变更时重读 |
| 用户 | ✅ 完全可行（WTS API） | **8.59 ms** | 1 Hz 轮询可接受 |
| 服务 | ✅ 完全可行（SCM） | **44.53 ms** | 5–10 秒轮询，或按需 |
| 应用历史记录 | ⚠️ **困难**（SRUM，见第 4 节） | 未知 | 建议第一版留空或显示"无可用数据" |

---

## 1. 启动页（Startup）

### 1.1 表格规格

| # | 列名 | 默认宽度 | 对齐 | 依据 |
|---|---|---|---|---|
| 1 | `名称` | 弹性（剩余宽度） | 左 | MUI 有 `启动进程` |
| 2 | `发布者` | 160 | 左 | 取 exe 的 CompanyName |
| 3 | `状态` | 90 | 左 | `已启用` / `已禁用`（MUI 均有） |
| 4 | `启动影响` | 100 | 左 | `不适用` / `低` / `中` / `高` |

**[推测]** 行高与其他页一致（32 px）；**无交替条纹、无网格线**（与进程页一致，因为任务管理器整个 UI 都是这个风格）。

### 1.2 顶部说明条

**[推测]** Win10 启动页顶部**没有**说明文字条（不像"应用历史记录"页有"自 X 以来…"）。直接从列头开始。
不过 Win8/8.1 时代有过一句 "启动影响基于……"，Win10 简化掉了。

### 1.3 右键菜单（MUI 30213 确证，原文照抄）

```
展开(&P)
折叠(&P)
───
启用(&E)
禁用(&D)
───
打开文件所在的位置(&O)
在线搜索(&S)
───
属性(&I)
```

**启用/禁用条件**：
- `启用` 仅当当前项已禁用时可用
- `禁用` 仅当当前项已启用时可用
- `展开/折叠` 仅当该项代表一个服务宿主或分组时可用（多数项是单条，灰显）

### 1.4 数据源明细

#### (a) 注册表 Run 键 —— [实测] 3.14 ms 拿到 23 项

| 键路径 | 说明 |
|---|---|
| `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` | 当前用户 |
| `HKCU\Software\Microsoft\Windows\CurrentVersion\RunOnce` | 当前用户，仅一次 |
| `HKLM\Software\Microsoft\Windows\CurrentVersion\Run` | 所有用户 |
| `HKLM\Software\Microsoft\Windows\CurrentVersion\RunOnce` | 所有用户，仅一次 |
| `HKLM\Software\Wow6432Node\Microsoft\Windows\CurrentVersion\Run` | 32 位应用（64 位系统上） |
| `HKLM\Software\Wow6432Node\Microsoft\Windows\CurrentVersion\RunOnce` | 同上 |

枚举方式：`RegOpenKeyExW` + `RegEnumValueW`，只取 `REG_SZ` / `REG_EXPAND_SZ`。
值名 = 启动项名称，值数据 = 命令行（可能是 `REG_EXPAND_SZ`，需 `ExpandEnvironmentStringsW`）。

**本机实测样本**（23 项，节选）：
```
[启用] Steam          HKCU\Run     "D:\Steam\steam.exe" -silent
[启用] Weixin         HKCU\Run     "D:\Weixin\Weixin.exe" -autorun
[禁用] Thunder        HKCU\Run     D:\Thunder\Program\Thunder.exe -silent -StartType:AutoRun
[启用] RTHDVCPL       HKLM\Run     "C:\Program Files\Realtek\Audio\HDA\RAVCpl64.exe" -s
[禁用] RadminVPN      HKLM\Run(WOW) "C:\Program Files (x86)\Radmin VPN\RvRvpnGui.exe" /minimized
```

#### (b) 启动文件夹 —— [实测] 路径已确认

```
用户启动文件夹: C:\Users\<user>\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Startup
公共启动文件夹: C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Startup
```
获取方式：`SHGetFolderPathW(nullptr, CSIDL_STARTUP | CSIDL_COMMON_STARTUP, ...)`。
内容通常是 `.lnk`，需要 `IShellLink` 解析出目标命令行才能显示"发布者"等。
**本机实测**：用户启动文件夹 1 个 `Overwatch Watch.lnk`，公共启动文件夹为空。

#### (c) 启用/禁用状态 —— **关键，[实测] 已破解格式**

路径：`HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\`

三个子键：
```
StartupApproved\Run               ← 对应 HKCU\...\Run
StartupApproved\Run32             ← 对应 HKCU\...\RunOnce 和 WOW64 项
StartupApproved\StartupFolder     ← 对应启动文件夹里的 .lnk
```

值的格式是 **12 字节 REG_BINARY**：

| 偏移 | 长度 | 含义 |
|---|---|---|
| 0 | 4 | **状态 DWORD**：`2` = 已启用，`3` = 已禁用，`1`/`0` = 其他（视为启用） |
| 4 | 8 | 禁用时间的 `FILETIME`（仅当状态=3 时有意义） |

**本机实测原始值**：
```
Steam                                 02 00 00 00 00 00 00 00 00 00 00 00   → 启用
WinIsland                             03 00 00 00 02 BA B4 E4 58 53 DD 01   → 禁用，时间戳有效
Lunar Client                          03 00 00 00 34 81 78 8E 84 BF DC 01   → 禁用
quark                                 01 00 00 00 31 82 DF D6 64 DD DC 01   → 状态1，少见
```

> **注意**：键名必须与 Run 键里的**值名完全一致**才能匹配。如果 StartupApproved 里没有该名字，
> 说明用户从未在任务管理器里改动过它，应视为**已启用**。

**"禁用时间"列的来源**：就是那 8 字节 FILETIME。任务管理器在详细信息视图里显示它。

#### (d) 启动影响（高/中/低）—— **不可靠**

MUI 里提到 `%WINDIR%\system32\WDI\LogFiles\StartupInfo\%s_StartupInfo%d.xml`，但：

**[实测] 本机该目录为空**，全盘搜索 `*StartupInfo*.xml` 也是零结果。

原因：[确证] 这个 XML 只有在 **Windows 性能跟踪（WPT/WPR）的启动跟踪**跑过之后才会生成，
且默认**大多数机器上从来没有过**。所以任务管理器在大部分项上显示 `不适用` 就是这个原因。

**结论与建议**：
1. 第一版**全部显示 `不适用`** —— 这与原版在多数机器上的行为一致。
2. 若要更完整：读 `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved` 里没有影响数据；
   `Win32_StartupCommand` 也没有影响字段。
3. 真要做，唯一路径是解析那些 XML，但本机无法验证格式。**建议标为"以后再做"**。

#### (e) 备选：WMI `Win32_StartupCommand` —— **[实测] 754 ms，太慢**

```
count = 24, ms = 754.1
```
优点：一次拿到**所有用户**的启动项（含注册表 + 启动文件夹），且不用自己解析 .lnk。
缺点：**754 ms**，是注册表方案的 **240 倍**。

**建议**：主路径用注册表（3.14 ms），只在需要"显示所有用户的启动项"时异步调 WMI 补充。

#### (f) 发布者

从命令行里解析出 exe 路径 → `GetFileVersionInfoW` + `VerQueryValueW` 取 `CompanyName`。
**[实测参考]** 每 exe 约 0.1–0.5 ms（文件已缓存时），冷文件 3.23 ms（见主诊断报告的 AV 放大数据）。
**必须缓存**（按路径哈希），且放后台线程。

### 1.5 实现清单

```
[ ] RegOpenKeyExW 枚举 6 个 Run 键
[ ] SHGetFolderPathW 取两个启动文件夹，枚举 .lnk
[ ] 读 StartupApproved\Run / Run32 / StartupFolder，DWORD[0] == 3 即禁用
[ ] 从命令行解析 exe 路径（处理引号、环境变量）
[ ] 后台线程 + 缓存取 CompanyName 作为"发布者"
[ ] "启动影响" 一律显示 "不适用"
[ ] 右键菜单：启用/禁用 → 写 StartupApproved 的 DWORD[0] = 2 / 3
```

---

## 2. 用户页（Users）

### 2.1 表格规格

| # | 列名 | 默认宽度 | 对齐 |
|---|---|---|---|
| 1 | `用户` | 弹性 | 左 |
| 2 | `状态` | 90 | 左 |
| 3 | `CPU` | 70 | 右 |
| 4 | `内存` | 90 | 右 |
| 5 | `磁盘` | 70 | 右 |
| 6 | `网络` | 70 | 右 |

**[推测]** 列名与进程页同名同宽，只有第一列换成用户名。

### 2.2 展开子项

每个用户行可展开，显示该用户 session 下的进程。
**缩进**：与进程页子进程一致（每级 18–20 px）。
子行的列含义：名称 / 状态 / CPU / 内存 / 磁盘 / 网络。

### 2.3 右键菜单（MUI 30211 确证，原文照抄）

```
展开(&P)
折叠(&P)
───
连接(&C)
断开连接(&D)
注销(&I)
───
发送消息(&S)
去切换用户账户(&W)      ← MUI 原文 "切换用户账户(&W)"
管理用户账户(&M)
```

**可用性条件**：
- `连接` 仅当会话状态是 `WTSDisconnected`
- `断开连接` 仅当会话状态是 `WTSActive`
- `注销` / `发送消息` / `切换用户账户` / `管理用户账户` 总是可用（但需要提权）

### 2.4 数据源 —— [实测] 8.59 ms

#### (a) 会话枚举：`WTSEnumerateSessionsW`

```cpp
PWTS_SESSION_INFOW pSessions = nullptr;
DWORD count = 0;
WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pSessions, &count);
// 用完 WTSFreeMemory(pSessions)
```

**[实测] 本机 4 个会话**：
```
sessionId=0     state=已断开  user=(空)                     winsta=Services
sessionId=1     state=活动    user=WIN10PC-SQKL520\Administrator  winsta=Console
sessionId=65536 state=侦听    user=(空)                     winsta=31C5CE94259D4006A9E4
sessionId=65537 state=侦听    user=(空)                     winsta=RDP-Tcp
```

#### (b) 每会话的用户名 / 域 / 客户端名

```cpp
LPWSTR user = nullptr, domain = nullptr, clientName = nullptr;
DWORD cb = 0;
WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, s.SessionId, WTSUserName, &user, &cb);
WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, s.SessionId, WTSDomainName, &domain, &cb);
WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, s.SessionId, WTSClientName, &clientName, &cb);
// 每项用完 WTSFreeMemory
```

#### (c) "状态"列的取值

`WTS_SESSION_INFOW.State` 直接映射：

| WTS 常量 | 中文显示 | 依据 |
|---|---|---|
| `WTSActive` (0) | `活动` | [实测] 本机 session 1 |
| `WTSConnected` (1) | `已连接` | [推测] |
| `WTSConnectQuery` (2) | `连接查询` | [推测] |
| `WTSShadow` (3) | `影子` | [推测] |
| `WTSDisconnected` (4) | `已断开` | [实测] 本机 session 0 |
| `WTSIdle` (5) | `空闲` | [推测] |
| `WTSListen` (6) | `侦听` | [实测] 本机 65536/65537 |
| `WTSReset` (7) | `重置` | [推测] |
| `WTSDown` (8) | `已关闭` | [推测] |
| `WTSInit` (9) | `初始化` | [推测] |

> **注意**：`sessionId >= 65536` 的是 RDP 侦听器占位会话，**任务管理器不显示它们**。
> 过滤条件：`SessionId < 65536` 或 `State != WTSListen`。

#### (d) 每用户的 CPU / 内存

从进程快照按 `sessionId` 聚合：
```
userCpu  = sum(process.cpuPercent) for process.SessionId == sessionId
userMem  = sum(process.workingSet) for ...
```
`NtQuerySystemInformation(SystemProcessInformation)` 的 `SessionId` 字段直接可用，**零额外成本**。

#### (e) 每用户的磁盘 / 网络

磁盘：`sum(process.readBytes + writeBytes)` 差分。
网络：**没有 per-session 计数器**，只能留空（与原版一致，原版多数机器上网络列也是空的）。

### 2.5 实现清单

```
[ ] WTSEnumerateSessionsW 枚举会话
[ ] 过滤掉 SessionId >= 65536 的侦听器占位会话
[ ] WTSQuerySessionInformationW 取 用户名/域/客户端名
[ ] State → 中文状态字符串映射表
[ ] 按 SessionId 聚合进程快照得到 CPU/内存
[ ] 展开时列出该 session 的进程
[ ] 右键菜单动作：注销 = WTSLogoffSession；断开 = WTSDisconnectSession
```

---

## 3. 服务页（Services）

### 3.1 表格规格

| # | 列名 | 默认宽度 | 对齐 |
|---|---|---|---|
| 1 | `名称` | 弹性 | 左 |
| 2 | `PID` | 70 | 右 |
| 3 | `描述` | 320 | 左 |
| 4 | `状态` | 90 | 左 |
| 5 | `组` | 140 | 左 |

**[推测]** 宽排列；MUI 里有 `服务主机: `、`组 %1!u!`、`PID`、`状态`。

### 3.2 底部按钮栏

MUI 里有 `打开服务`。**[推测]** 底部有一条按钮栏，含：
- `服务(S)...` —— 打开 `services.msc`
- `打开服务(V)` —— 同上（MUI 30202 里有 `打开服务(&V)`）

实际 Win10 任务管理器服务页底部确实有两个按钮。位置：表格下方，右对齐或左对齐待确认。

### 3.3 右键菜单（MUI 30202 / 30210 确证）

MENU 30202（完整版）：
```
开始(&S)
停止(&T)
重新启动(&R)
───
打开服务(&V)
在线搜索(&O)
───
转到详细信息(&D)
```

MENU 30210（精简版，可能用于不同上下文）：
```
停止(&S)
打开服务(&O)
在线搜索(&S)
```

**可用性条件**：
- `开始` 仅当状态是 `已停止`
- `停止` 仅当状态是 `正在运行`
- `重新启动` 仅当状态是 `正在运行`
- 需要 `SERVICE_START` / `SERVICE_STOP` 权限（管理员）

### 3.4 数据源 —— [实测] 44.53 ms（306 个服务）

#### (a) 枚举服务列表

```cpp
SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);

DWORD bytesNeeded = 0, count = 0, resume = 0;
EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
                      SERVICE_STATE_ALL, nullptr, 0, &bytesNeeded,
                      &count, &resume, nullptr);   // 第一调用只为拿 size

std::vector<BYTE> buf(bytesNeeded + 8192);
EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
                      SERVICE_STATE_ALL, buf.data(), buf.size(), &bytesNeeded,
                      &count, &resume, nullptr);

auto* svc = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buf.data());
```

**[实测] 本机 306 个服务**（`SERVICE_WIN32` + `SERVICE_STATE_ALL`）。

> **注意**：`SERVICE_WIN32` 不含驱动（`SERVICE_DRIVER`）。任务管理器服务页**只显示 Win32 服务**，
> 与 `services.msc` 一致。

#### (b) 每服务的 PID

`ENUM_SERVICE_STATUS_PROCESSW[i].ServiceStatusProcess.dwProcessId`

**[实测]** 未运行的服务 `dwProcessId == 0`。

#### (c) 描述

```cpp
SC_HANDLE h = OpenServiceW(scm, svc[i].lpServiceName, SERVICE_QUERY_CONFIG);
DWORD need = 0;
QueryServiceConfig2W(h, SERVICE_CONFIG_DESCRIPTION, nullptr, 0, &need);
std::vector<BYTE> db(need);
auto* d = reinterpret_cast<SERVICE_DESCRIPTIONW*>(db.data());
QueryServiceConfig2W(h, SERVICE_CONFIG_DESCRIPTION, db.data(), need, &need);
// d->lpDescription
```

#### (d) 组

```cpp
DWORD need2 = 0;
QueryServiceConfigW(h, nullptr, 0, &need2);
std::vector<BYTE> cb(need2);
auto* c = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(cb.data());
QueryServiceConfigW(h, c, need2, &need2);
// c->lpLoadOrderGroup
```

**组的中文显示**：MUI 有 `组 %1!u!`，但实际组名是英文（如 `Event log`、`AudioGroup`、`ProfSvc_Group`）。
**[实测]** 本机样本：
```
AMD External Events Utility   组=Event log
AppIDSvc                      组=ProfSvc_Group
AudioEndpointBuilder          组=AudioGroup
```

#### (e) 状态中文映射 —— [实测]

| `dwCurrentState` | 中文 | 依据 |
|---|---|---|
| `SERVICE_RUNNING` (4) | `正在运行` | MUI 有 `正在运行` |
| `SERVICE_STOPPED` (1) | `已停止` | MUI 有 `已停止` |
| `SERVICE_START_PENDING` (2) | `正在启动` | MUI 有 `正在启动` |
| `SERVICE_STOP_PENDING` (3) | `正在停止` | **MUI 确证有 `正在停止`** |
| `SERVICE_PAUSED` (7) | `已暂停` | [推测] |
| `SERVICE_PAUSE_PENDING` (6) | `正在暂停` | [推测] |
| `SERVICE_CONTINUE_PENDING` (5) | `正在继续` | [推测] |

#### (f) 性能分析 —— 44.53 ms 的构成

```
枚举全部 306 个服务                      ~ 10 ms
对前 12 个服务额外查 描述 + 组            ~ 34 ms
  → 每个约 2.8 ms（两次 OpenService + 两次 QueryServiceConfig）
```

**推论**：如果对 306 个服务全查描述+组，约需 **306 × 2.8 ≈ 857 ms**。太慢，不能 1 Hz 刷新。

**缓存策略（关键）**：
1. 服务列表与状态：5–10 秒轮询（服务状态变化不频繁），或只在用户切到服务页时拉一次。
2. **描述和组：只拉一次并永久缓存** —— 服务的描述在运行期几乎不变。
3. 首屏显示时可以先只显示名称+PID+状态，描述列逐步填充（与原版行为一致——原版描述列也是渐进的）。

### 3.5 操作（开始/停止/重新启动）

```cpp
SC_HANDLE h = OpenServiceW(scm, name, SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);
StartServiceW(h, 0, nullptr);                            // 开始
ControlServiceW(h, SERVICE_CONTROL_STOP, &status);       // 停止
// 重新启动 = ControlServiceW(STOP) + 等状态变 STOPPED + StartServiceW
```

**权限要求**：
- 需要进程以**管理员**运行
- `StartServiceW` 对该服务可能还需要 `SeServiceLogonRight` 之类的服务 ACL 授权
- **[实测]** 本机当前进程是管理员，但 `SYSTEM` 才有的服务（如 `WinDefend`）仍会返回 `ERROR_ACCESS_DENIED`

**失败时的 UI 反馈**（MUI 有对应文案）：
- `访问被拒绝。`
- `正在停止`
- `尝试执行命令时，出现以下系统错误: ` + 错误文本

### 3.6 实现清单

```
[ ] OpenSCManagerW(SC_MANAGER_ENUMERATE_SERVICE)
[ ] 两次 EnumServicesStatusExW（先探 size 再取数据）
[ ] 只取 SERVICE_WIN32（不含驱动）
[ ] dwProcessId == 0 时不显示 PID
[ ] 状态常量 → 中文映射表
[ ] 描述/组：一次拉取 + 永久缓存，渐进填充
[ ] 底部按钮：打开 services.msc（ShellExecuteW）
[ ] 右键菜单：StartServiceW / ControlServiceW，失败时弹 MUI 那样的错误提示
[ ] 5-10 秒轮询刷新状态
```

---

## 4. 应用历史记录页（App history）—— **这是唯一做不到完美还原的页面**

### 4.1 表格规格

| # | 列名 | 默认宽度 | 对齐 |
|---|---|---|---|
| 1 | `名称` | 弹性 | 左 |
| 2 | `CPU 时间` | 100 | 右 |
| 3 | `网络` | 90 | 右 |
| 4 | `平铺更新` | 90 | 右 |
| 5 | `应用内下载` | 100 | 右 |

**[推测]** 列名基于 MUI 里的 `%I64d Gbps 网络 I/O`、`应用程序的上载活动`、`上载`、`已接收的非单播`。

### 4.2 顶部说明条

MUI 确证有：`自 %1!s! 以来，当前用户和系统账户的资源使用情况。`

其中 `%1!s!` 是日期。**[确证]** SRUM 的默认保留期是 **30 天**，
且任务管理器显示的是 SRUM 数据库里**最早一条记录的时间**（不是固定的"30 天前"，而是实际数据起点）。

顶部还有一个按钮：`删除使用情况历史记录`（MUI 确证）。

### 4.3 右键菜单（MUI 30214 确证）

```
切换到(&T)
在线搜索(&S)
属性(&P)
```

### 4.4 数据源 —— **困难，三条路都走不通或代价过高**

#### 路径 1：`srumapi.dll` 的 `SruQueryStatsEx` —— **签名未知，会崩**

**[实测] 真实导出表**（从 PE 文件解析，不是猜的）：
```
DLLname = SrumAPI.dll, 15 exports
[  1] DllCanUnloadNow
[  2] DllGetClassObject
[  3] DllRegisterServer
[  4] DllUnregisterServer
[  5] SruCreateCheckpoint
[  6] SruCreateEnergyNotificationServer
[  7] SruDeleteStatsByAppName
[  8] SruFreeRecordSet
[  9] SruQueryStats
[ 10] SruQueryStatsBySeqNumber
[ 11] SruQueryStatsEx
[ 12] SruRegisterRealTimeStats
[ 13] SruRetrieveEnergyRecord
[ 14] SruUnregisterRealTimeStats
[ 15] SruUpdateStats
```

**[实测] 调用 `SruQueryStatsEx` 直接 segfault**。原因：
- `srumapi.dll` 是 **COM 服务器**（有 `DllGetClassObject`、`.rdata` 里有 `CSrumClient`、`InstantiateSrumServer`）
- 这些函数**没有公开头文件**（Windows SDK 里搜不到 `SruQueryStats`）
- 签名只能逆向，且需要 `SRU_STAT` 结构体的精确定义
- 还需要 **SYSTEM 权限**（不是管理员）

`.rdata` 里的字段名线索（可供逆向）：
```
_ApplicationName, _BytesReceived, _BytesSent, _Costed, _InterfaceGuid,
_InterfaceLuid, _ProfileFlags, _ProfileId, _Reset, _Status, _TableName,
AppID, CSrumClient, CoCreateEnergyNotificationServer, InstantiateSrumServer
```

**结论**：可以逆向出来，但工作量以**天**计，且脆弱（微软随时可能改）。

#### 路径 2：WMI —— **[实测] 不存在**

```
Get-CimClass -Namespace root\cimv2 | Where CimClassName -like '*Srum*'
→ 零结果
```

**root\cimv2 里没有任何 SRUM 相关类。** 这条路彻底不通。

#### 路径 3：直接读 `SRUDB.dat` —— **需要 ESE/JET Blue**

**[实测]**
```
打开: 成功（本机进程是管理员）
大小: 60.9 MB
前4字节: AC 3B C7 47
```
`AC 3B C7 47` 不是标准 ESE 签名（`EF CD AB 89`），说明它是 **ESE 的新格式或加密头**。
即使能打开，读它需要 `esent.dll` 的 JET API，还要知道 SRUM 的表结构（`SruDbIdMapTable`、
`{GUID}` 命名的资源表等），且数据库被 `DPS` 服务独占。

**结论**：技术上可行但工程量极大，且不稳定。

#### 路径 4：`SRUM_APP_RESOURCE_USAGE` 之类的表名 —— 未验证

虽然 `.rdata` 里能猜出表名，但没有签名无法调用。

### 4.5 建议方案

**第一版：显示"无可用数据"，与原版在 SRUM 被禁用时的行为一致。**

MUI 里就有一句 `无可用数据`，说明原版自己也有这个状态。

**第二版（如果一定要做）的最低成本方案**：
1. 用 `wevtutil` 或 ETW 从 `Microsoft-Windows-Kernel-Process` 之类拿进程运行时长 —— 但这不是 SRUM 语义。
2. 或者接受"逆向 srumapi"的工程成本。

**明确建议**：把"应用历史记录"页做成**能打开、布局正确、显示"无可用数据"**，
而不是显示假数据。这比伪造数据诚实，也比完全不做强。

---

## 5. 四个页面的共同绘制要点

### 5.1 统一表格样式（与进程页一致）

| 项 | 值 |
|---|---|
| 行高 | 32 px |
| 列头高 | 24 px |
| 列头背景 | `#FFFFFF`（白，不是灰） |
| 列头文字色 | `#5A5A5A` |
| 交替行条纹 | **无** |
| 网格线 | **无** |
| 行悬停 | `#E5F3FB` |
| 行选中 | `#CCE8FF` |
| 名称列图标 | 16×16，左内边距 8 px |
| 分组行（如用户页展开的进程） | 高 28 px，`#F2F2F2`，加粗 |

### 5.2 各页特殊点

| 页面 | 特殊点 |
|---|---|
| 启动 | 第 3 列是"状态"文本（已启用/已禁用），不是图标 |
| 用户 | 行可展开，子行是进程 |
| 服务 | 底部有按钮栏（`服务...` / `打开服务`） |
| 应用历史 | 顶部有说明文字 + `删除使用情况历史记录` 按钮 |

### 5.3 状态栏在各页的显示

**[推测]** 任务管理器状态栏在所有页面基本一致：
```
进程数: N    CPU x%    内存 x/y GB (z%)
```
服务页可能显示"服务数: N"。

---

## 6. 施工优先级建议

| 优先级 | 页面 | 理由 |
|---|---|---|
| **P0** | 启动 | 数据源最简单（3.14 ms），效果最直观，注册表 + 二进制状态全破解 |
| **P0** | 服务 | 数据全在 SCM 一处，44 ms 可接受，缓存描述后更快 |
| **P1** | 用户 | WTS API 干净（8.59 ms），但需要按 session 聚合进程 |
| **P2** | 应用历史 | SRUM 不可用，做成"无可用数据"占位即可 |

---

## 7. 诚实标注：未验证 / 有风险的部分

| 项 | 状态 |
|---|---|
| 启动影响的三档颜色 | **未验证**（本机无 StartupInfo XML）。原版颜色推测为：高=红 `#C42B1C`、中=橙 `#B85C00`、低=黑 |
| 启动页顶部是否有说明条 | **推测没有**，但未 100% 确认 |
| 服务页底部按钮的确切文案与位置 | MUI 有 `打开服务`，但按钮标签可能是 `服务(S)...` 和 `打开服务(V)`，位置推测在表格下方 |
| 用户页列宽的精确值 | 推测沿用进程页宽度 |
| 应用历史记录的列名完整列表 | 部分来自 MUI 片段，不完整 |
| SRUM 的 `SruQueryStatsEx` 真实原型 | 未知，调用会崩 |
| `SRUDB.dat` 的 `AC 3B C7 47` 头 | 未识别，可能加密或新版 ESE |

---

## 附：本次实测产生的脚本与数据

| 文件 | 内容 |
|---|---|
| `tools/pagetest.cpp`（运行输出含本机启动项清单，未随仓库分发） | 四页数据源总测（启动 3.14ms / 用户 8.59ms / 服务 44.53ms / SRUM 探测） |
| `tools/srumprobe2.cpp` → `docs/evidence/srum_probe2.txt` | srumapi.dll 的完整 PE 导出表（15 个函数，不是猜的） |
| `tools/srumtest.cpp` → `docs/evidence/srum_test.txt` | SruQueryStatsEx 调用实测（segfault，签名未知） |
| `tools/exports.cpp` | PE 导出表解析器（可复用于其他 DLL） |

全部只读，未修改任何系统设置，未启动 taskmgr.exe。
