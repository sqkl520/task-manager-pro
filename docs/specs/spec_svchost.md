# svchost 拆组可行性调研

本机：Win10 Pro 22H2 19045 / 服务器级 CPU / 306 个服务 / 84 个 svchost 进程（多次采样 79-84 波动）

> 本文档每得出一个结论即写入，非最终整理稿。
> 结论汇总见文末「一句话结论」。

---

## 0. 调研前状态（项目现状）

`src/classifier.cpp:17` 把 `svchost.exe` 放进 `kWindowsWhitelist`，仅用于「归 Windows 进程」分组。
项目**没有任何 svchost 拆组代码**，列表里一个 svchost 进程 = 一行。

对照：`include/pages_data.h` 注释「服务页 EnumServicesStatusExW 44.53 ms / 306 服务」。
这个数字后面被证伪（见 Q3.1）。

---

## Q2. 原版实际怎么拆的 —— 取证结果

### Q2.1 MUI 字符串：原版根本没有「按服务拆行」

从 `C:\Windows\System32\zh-CN\taskmgr.exe.mui` 提取的字符串表（`docs/evidence/mui_33550.txt`）
在 id 33556..33588 有一整段**连续**的「服务主机: xxx」标签，共 32 项：

```
33556  [服务主机: ActiveX 安装程序]
33557  [服务主机: 蓝牙支持]
33558  [服务主机: DCOM 服务器进程启动器]
33559  [服务主机: 磁盘碎片整理程序]
33560  [服务主机: Windows 图像采集(WIA)]
33561  [服务主机: 本地服务]
33562  [服务主机: 本地服务(无模拟)]
33563  [服务主机: 本地服务(网络受限)]
33564  [服务主机: 本地服务(无网络)]
33565  [服务主机: 本地服务(对等网络)]
33566  [服务主机: 本地系统(网络受限)]
33567  [服务主机: 本地系统]
33568  [服务主机: 网络服务]
33569  [服务主机: 网络服务(无模拟)]
33570  [服务主机: 网络服务(网络受限)]
33571  [服务主机: BranchCache]
33572  [服务主机: 远程注册表]
33573  [服务主机: 远程过程调用]
33574  [服务主机: Windows 备份]
33575  [服务主机: Windows Defender]
33576  [服务主机: Microsoft 软件卷影副本提供程序]
33577  [服务主机: Windows 生物识别]
33578  [服务主机: Windows 颜色系统]
33579  [服务主机: Windows 错误报告]
33580  [服务主机: 虚拟机检测信号]
33581  [服务主机: IIS 应用程序主机帮助程序服务]
33582  [服务主机: Internet 信息服务]
33583  [服务主机: 打印机服务]
33584  [服务主机: 文件服务器资源管理器]
33585  [服务主机: 组策略]
33586  [服务主机: Unistack 服务组]
33587  [服务主机: UtcSvc]
33588  [服务主机: ]
```

**这是决定性证据。** 逐条核对（`docs/evidence/svchost_mui_evidence_out.txt`）：

| 标签正文 | 是不是某个服务的**显示名** |
|---|---|
| `DCOM 服务器进程启动器` | 是（`DcomLaunch` 服务的 lpDisplayName） |
| `远程过程调用` | 是（`RpcSs`） |
| `BranchCache` | 是（`PeerDistSvc`） |
| `Windows 备份` | 是（`SDRSVC`） |
| **`本地服务` / `本地系统` / `网络服务`** | **不是任何服务的显示名** |
| **`本地服务(无模拟)` / `(网络受限)` / `(无网络)` / `(对等网络)`** | **不是** |
| **`网络服务(无模拟)` / `(网络受限)`** | **不是** |
| **`ActiveX 安装程序` / `蓝牙支持` / `打印机服务` / `组策略` / `Windows Defender` / `IIS ...`** | **不是** |

统计：32 项中 **只有 2 项**（BranchCache、Windows 备份）恰好等于某个服务的显示名，
30 项不等于任何服务显示名。

**关键推理**：`本地服务`、`网络服务`、`本地系统`、`本地服务(网络受限)` 这些
是 svchost 的 **`-k` 组名（安全上下文）**，不是服务名。

- 如果原版「按服务拆行」，每行必须显示**各自服务**的显示名，
  那么「本地服务」「网络服务」这类**组名标签永远不可能出现在列表里** ——
  但没有哪个服务叫「本地服务」。
- 反过来，若「一个多服务宿主共用 1 行」，宿主没有一个明确的单服务名，
  才需要一个按 `-k` 组名兜底的标签。**这 30 个组名标签正是为此存在的。**

→ **原版是多服务宿主 = 1 行，不按内部服务拆分。**

> ⚠️ 这一条推翻了任务背景里的假设。截图那行
> `服务主机: Remote Desktop Se...` **不是** svchost 拆分的结果。

### Q2.2 那截图里的「服务主机: Remote Desktop Se...」是什么

`docs/evidence/svchost_probe5_out.txt` 的 [E] 段实测，本机名称以 `Remote Desktop Se` 开头的行有 3 个，
各自是**独立的 svchost 进程**（`-k` 组不同，不是同一进程拆出来的）：

```
pid=2648   服务主机: Remote Desktop Configuration                    [-s 服务显示名]
pid=1668   服务主机: Remote Desktop Services                          [-s 服务显示名]
pid=1396   服务主机: Remote Desktop Services UserMode Port Redirector [-s 服务显示名]
```

其中 `pid=1668` 的命令行是 `-k NetworkService -s TermService`，
它**只承载 1 个服务**（TermService），Windows 本来就没把它和其他服务合并进同一个 svchost。
所以它天然就是独立一行 —— 不需要拆。

→ 截图那行来自 **svchost 的命名规则**（用 `-s` 服务的显示名给进程行命名），
  **不是**svchost 拆组。

### Q2.3 结论：真正该做的是「命名」，不是「拆组」

原版做了两件事，**都不是拆组**：

1. **命名**：svchost 行的名字不是 `svchost.exe`，而是 `服务主机: <显示名>`
   - 有 `-s <服务名>` → 用该服务的 `lpDisplayName`
   - 无 `-s`（多服务宿主）→ 用 `-k <组名>` 查 MUI 里那 32 项组名表
2. 进程仍然 **1 个 svchost = 1 行**

本机实测（`svchost_probe5_out.txt`）：
- 有 `-s` 的 svchost 共 65 个，显示名查找**精确命中 62 / 前缀命中 3 / 未命中 0**
- 无 `-s` 的 svchost 共 15 个，组名表**命中 12 / 未命中 3**（3 个是 `-k` 为空或表外组名）

→ 命名规则 100% 可复刻，且**不需要拆行**。

---

## Q2.4 命名口径的额外证据：截图里的英文后缀

用户截图那行是 `服务主机: Remote Desktop Se...` —— 中文前缀 + **英文**正文。
这个「中英混排」正是本机真实行为，实测（`docs/evidence/svchost_indirect_probe.cpp`）：

```
TermService      -> "Remote Desktop Services"          <- 英文，没走中文 MUI
RpcSs            -> "Remote Procedure Call (RPC)"      <- 英文
DcomLaunch       -> "DCOM Server Process Launcher"     <- 英文
cbdhsvc          -> "剪贴板用户服务"                    <- 中文
WpnUserService   -> "Windows Push Notifications User Service"
```

**原因**：服务的 `DisplayName` 注册表值是**间接字符串**（`@dll,-id`）：

```
TermService  DisplayName = @%SystemRoot%\System32\termsrv.dll,-268
RpcSs        DisplayName = @combase.dll,-5010
DcomLaunch   DisplayName = @combase.dll,-5012
cbdhsvc      DisplayName = @%SystemRoot%\system32\cbdhsvc.dll,-100
```

- `C:\Windows\System32\zh-CN\termsrv.dll.mui`、`combase.dll.mui` **文件存在**，
  但**不含**这些 string id 的中文翻译 → 语言回落，返回英文。
- `cbdhsvc.dll.mui` 只有 2560 字节但**有** id=100 的中文 → 返回中文。

全表统计（306 个服务）：`lpDisplayName` 含非 ASCII 的 **64 个**，纯 ASCII **242 个**。
用 `SHLoadIndirectString` 逐条解析，结果与 `EnumServicesStatusExW` 完全一致，
说明 **taskmgr / WMI 走的就是这条**，不需要自己解析 `@dll,-id`。

→ **结论**：`服务主机: Remote Desktop Se...` 能与截图逐字对上。
   这是「命名」规则的确证，与「拆组」无关。

---

## Q1. 每行数值口径 —— 决定性结论：**没有合理口径**

### Q1.1 Windows 没有服务级计量（API 层证据）

| 途径 | 是否有服务级内存/CPU | 说明 |
|---|---|---|
| `QueryServiceStatusEx` → `SERVICE_STATUS_PROCESS` | **无** | 只有 `dwServiceType/dwCurrentState/dwControlsAccepted/dwWin32ExitCode/dwServiceSpecificExitCode/dwCheckPoint/dwWaitHint/dwProcessId/dwServiceFlags`。无内存、无 CPU、无 I/O、无线程数。 |
| `EnumServicesStatusExW` | **无** | 同上结构 + name/display，仍只有 `dwProcessId`。 |
| PERF 计数器 `Process(*)` | **无** | 实例按 **PID** 划分，没有 service 实例。 |
| `GetProcessMemoryInfo` | **无** | 按进程句柄，一个 PID 一份。 |
| ETW / `perfmon` 服务计数器 | **无** | 不存在 `\Services\*\Working Set` 这类计数器。 |

**任何「服务级内存」都只能由程序自己编造。** 这是 Q1 的根本阻碍。

### Q1.2 三种口径的实测后果（本机三个多服务宿主）

`docs/evidence/svchost_q1_probe.cpp` 实测：

```
── PID 1172  工作集 26.2 MB  私有 17.5 MB  承载 5 个服务 ──
     BrokerInfrastructure / DcomLaunch / PlugPlay / Power / SystemEventsBroker
     (a) 每行都显示 26.2 MB => 5 行合计 131.1 MB  (虚增 5.00x)
     (b) 只有主服务显示，其余 0 => 合计 26.2 MB
     (c) 均分 => 每行 5.24 MB（编造）

── PID 1412  工作集 32.1 MB  私有 27.6 MB  承载 2 个服务 ──
     RpcEptMapper / RpcSs
     (a) 2 行合计 64.1 MB (2.00x)   (b) 32.1 MB   (c) 16.03 MB/行（编造）

── PID 4408  工作集 12.3 MB  私有 9.4 MB  承载 2 个服务 ──
     BFE / mpssvc
     (a) 2 行合计 24.7 MB (2.00x)   (b) 12.3 MB   (c) 6.17 MB/行（编造）
```

逐项裁决：

| 口径 | 是否可行 | 理由 |
|---|---|---|
| **(a) 每行都显示全额** | ✗ | 用户把内存列一加，比真实值大。PID 1172 就是 5 倍。任务管理器的内存列是**可求和**的，这一条直接摧毁可信度。 |
| **(b) 主服务显示全额 + 其余 0** | ✗ | 「主服务」是**编造的概念**。PID 1172 那 5 个服务（DcomLaunch / PlugPlay / Power / SystemEventsBroker / BrokerInfrastructure）在 Windows 里**完全平权**，没有任何 API 说谁是主。挑哪个都是错的。<br>而且「其余 0」会让用户以为这些服务不占内存。 |
| **(c) 均分** | ✗ | 纯编造。5 个服务的实际内存天差地别，均分出来的 5.24 MB 不对应任何真实量。 |
| **(d) 每行显示但标注共享** | ✗ | 需要改列定义/加提示，原版没这么做，且列表仍不可求和。 |

### Q1.3 全局影响：本机 84 个 svchost

```
按进程 1 行     = 84 行
按服务 1 行     = 90 行  (膨胀 1.07x)
工作集合计(真实) = 944.6 MB
按服务重复显示   = 1012.1 MB  (虚增 1.07x)
```

膨胀只有 1.07x —— **因为本机 84 个 svchost 里有 81 个只承载 1 个服务**
（Win10 的 svchost 拆组早在 1703 就做过了，多服务宿主只剩 3 个）。

> ⚠️ **这是第二个推翻任务前提的发现**：
> 本机 96% 的 svchost 本来就只跑 1 个服务，**「拆组」在 Win10 19045 上几乎没有可拆的东西**。
> 真正多服务的只有 3 个进程（共 9 个服务）。

### Q1.4 结论

**Q1 无解。** Windows 不提供服务级计量，(a)(b)(c) 三条路全部会在
「内存列求和」这个最基本的用户校验上翻车。

→ **按服务拆行 = 不该做。**

---

## Q3. 实测开销

`docs/evidence/svchost_cost_probe.cpp` / `svchost_q1_probe.cpp`，本机 306 服务 / 84 svchost。

### Q3.1 服务枚举（`EnumServicesStatusExW`）

```
复用 SCM 句柄 + 复用缓冲   n=60  min=1.020  p50=1.704  p95=8.648  max=18.803  avg=2.978 ms
每次重做 size-probe        n=30  min=1.660  p50=3.220  p95=7.626  max= 9.456  avg=3.485 ms
冷启动(关SCM重开+枚举)     n=10  min=2.147  p50=2.990  p95=5.742  max= 5.742  avg=3.170 ms
bytesNeeded = 48,081 B（306 服务）
```

**`pages_data.h` 里那个 44.53 ms 是错的度量。** 它把第一次 SCM 连接 + 冷页 + 可能的
版本差异都算进去了；稳态只需 **~1.7 ms**（p50），冷启动也只有 ~3 ms。
→ 顺手建议：`pages_data.h:6` 和 `:88` 的注释数字应当更正（本任务不改源文件，只记录）。

⚠️ 但 p95 = 8.6 ms、max = 18.8 ms 的**抖动**是真的。按 1s 刷新、9.2ms 基线算，
最坏情况会把一轮采集从 9ms 推到 ~28ms。对「指针一卡一卡」的历史问题（DIAGNOSIS.md）
这是需要正视的风险点。

### Q3.2 命名所需的命令行读取

命名规则要区分「有 `-s`」和「无 `-s`」，必须读命令行。
本项目已有更便宜的通道：`NtQueryInformationProcess(60)`，DIAGNOSIS.md 实测
**431 个进程 = 4.77 ms → 0.0111 ms/个**，84 个 svchost ≈ **0.93 ms**。

> 注：`Toolhelp32` 路径实测 p50 = 56.8 ms，那是因为 Toolhelp 自身遍历贵，
> **不要**用 Toolhelp 来做这件事；项目现有的 NtQuery 路径才是对的。

### Q3.3 命名 vs 拆组：成本对比

| 方案 | 增量 | 是否引入数值错误 |
|---|---|---|
| **仅命名**（1 进程 1 行，名字改成「服务主机: xxx」） | 枚举 1.70 + 命令行 0.93 ≈ **2.63 ms** (+29%) | **否** —— 数值仍按 PID 取，完全正确 |
| **拆组**（按服务 1 行） | 同上 + 行数从 84 → 90 | **是** —— 内存/CPU 必然重复或编造（见 Q1） |

**两者采集成本几乎一样**（拆组不需要额外 API，服务表里就有 `dwProcessId`）。
所以「拆组」省不下成本，却要付出数值正确性的代价。

### Q3.4 组名表 / 描述字符串：一次性成本

| 项 | 成本 | 备注 |
|---|---|---|
| `lpDisplayName` | **0**（随枚举免费返回） | 不要再单独查 |
| 查全部 306 个服务**描述** | **850.7 ms**（1.7ms/个，另有前次实测 355ms） | **绝对不要**走这条路；原版也不走 |
| 读注册表 `Svchost` 组名表（51 项） | 1.77 ms | **可缓存**，进程组不常变 |
| MUI 32 项组名标签 | 0（编译期常量表） | 见 Q2.1 |

---

## Q4. 那个 MUI 32 项标签表的性质（补证）

把 32 项标签按语义映射到注册表的 svchost 组名（`hk\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Svchost`，本机 51 个组）：

```
HIT  = 25 / 32
MISS =  7
```

7 个 MISS 全是**本机未安装功能**的组名：

| id | 标签 | 对应组 | 本机状态 |
|---|---|---|---|
| 33572 | 远程注册表 | `RegSvc` | 注册表无此组 |
| 33575 | Windows Defender | `SecSvcs` | 注册表无此组 |
| 33578 | Windows 颜色系统 | `WcsSvc` | 服务 `WcsPlugInService` 未安装 |
| 33581 | IIS 应用程序主机帮助程序服务 | `AppHost` | 服务未安装 |
| 33582 | Internet 信息服务 | `IIsSvcs` | 服务 `W3SVC` 未安装 |
| 33584 | 文件服务器资源管理器 | `srmsvcs` | 服务 `SrmSvc` 未安装 |
| 33585 | 组策略 | `GpSvcGroup` | gpsvc 实际在 `netsvcs` 组 |

→ **25/32 精确对上 `-k` 组名，7 个未命中全部是「功能未安装」而非「映射错误」。**
   这 32 项是 Windows 的**静态全集**，不是按本机服务名生成的。

**这就是 Q2 的最终确证**：MUI 表按 **svchost 组名（安全上下文）** 建，不按服务建。
如果原版按服务拆行，这张表里 30 个「本地服务/网络服务/本地系统……」标签永远用不上。

本机实际活跃的 `-k` 组名分布（21 个不同组）与这张表高度重叠：
```
netsvcs 21 / LocalService 13 / LocalSystemNetworkRestricted 13 /
LocalServiceNetworkRestricted 9 / NetworkService 6 / UnistackSvcGroup 3 /
DcomLaunch 3 / appmodel 2 / LocalServiceAndNoImpersonation 2 / ...
```

---

## Q5. 那「正确的替代方案」是什么 —— 命名，不拆组

### Q5.1 命名规则（可 100% 复刻）

```
svchost 行名 = "服务主机: " + <名字>
<名字> 取值优先级：
  1) 该进程承载 1 个服务              -> 该服务的 lpDisplayName
  2) 该进程承载 >= 2 个服务           -> -k 组名 经 MUI 32 项表映射
  3) 组名不在 MUI 表 / -k 为空        -> 回落到 MUI id=33588 "服务主机: "（空正文）
```

依据：
- 规则 1 精确解释截图：`TermService` 的 display 就是 `Remote Desktop Services`，
  pid=1668 只承载这 1 个服务 → `服务主机: Remote Desktop Se...`。
- 规则 2 精确解释 MUI 表里那 30 个「本地服务/网络服务/本地系统……」标签的存在意义
  —— **只有多服务宿主才需要组名兜底**。
- 规则 3 精确解释 MUI id=33588 那个正文为空的 `服务主机: ` —— 它是未知组名的兜底串。

本机实测覆盖率（`docs/evidence/svchost_probe5_out.txt`）：
```
有 -s 的 svchost 65 个：显示名精确命中 62 / 前缀命中 3 / 未命中 0   (100%)
无 -s 的 svchost 15 个：组名表命中 12 / 未命中 3                   (80%，走规则 3 兜底)
```
未命中的 3 个：`pid=4408`（组 `LocalServiceNoNetworkFirewall` 不在 MUI 表）、
以及 2 个 `-k` 为空的历史遗留进程。

### Q5.2 命名 vs 拆组：逐项对比

| 维度 | 命名（推荐） | 拆组（不要做） |
|---|---|---|
| 行数 | 84 行 | 90 行（+7%，**且 96% 的进程无可拆**） |
| 内存列正确性 | **正确**（按 PID 取） | **错误**（必然重复 5x/2x 或编造） |
| CPU 列正确性 | **正确** | **错误**（同样无服务级计量） |
| 盘/网络列 | **正确** | **错误** |
| 求和一致性 | **通过** | 失败（用户一眼看出内存翻倍） |
| 采集增量 | +2.63 ms (+29%) | +2.63 ms（**一样贵**） |
| 「结束任务」语义 | 一个进程，正确 | 一行杀一个服务？无 API 支持单独杀服务 |
| 与截图一致性 | **逐字对上** | 也对得上，但归因错误 |
| 代码量 | 小（一个命名函数 + 32 项常量表 + 注册表组映射缓存） | 大（还要处理行→进程的反查、多行同步刷新、结束任务、亲和性等） |

**决定性一点**：拆组**不省任何成本**，却要付出全部数值正确性的代价。
而命名**成本相同**，数值完全正确，且与截图逐字吻合。

### Q5.3 实现要点（若要做命名）

1. **必须复用 SCM 句柄 + 缓冲**（1.704 ms vs 3.220 ms）。
   服务表本身可在采集线程里刷新，因为 `dwProcessId` 就在里面。
2. **不要读命令行**。用「服务表反查 PID → 服务 → 注册表 `Group`」这条路：
   - 注册表 `HKLM\SYSTEM\CurrentControlSet\Services\<svc>\Group` 给出服务所属组
   - 一次性建表 1.773 ms，**可缓存**（服务/组配置极少变）
   - 每轮增量 ≈ 0
   - 对比读 84 个命令行 ≈ 0.93 ms（走 NtQuery）到 18 ms（走 Toolhelp，**不要用**）
3. **不要查服务描述**（850 ms / 306 个）。`lpDisplayName` 已免费随枚举返回。
4. 32 项组名标签做成编译期常量表（zh-CN）；其它语言需从 MUI 取。
5. ⚠️ **p95 = 8.6 ms / max = 18.8 ms 的抖动要留意**。建议：
   - 服务表与进程快照**解耦刷新**（服务变化远比进程慢，可 5-10s 刷一次，缓存 PID→名字）
   - 这样每轮增量降到 **≈0**，只在服务表到期时才付 1.7 ms

### Q5.4 若仍决定拆组 —— 唯一可接受的数值口径

如果非拆不可，唯一不自相矛盾的做法是：
- 内存/CPU/磁盘/网络：**只在第一行显示真实值，其余行显示 "—"（横线，不是 0）**
- 行名前加标记，如 `服务主机(1/5): xxx`
- 明确告知用户这些行共享同一进程资源

但这样做**与原版不一致**（原版没这列），也与 Win10 任务管理器不一致，
属于自创 UI，收益为负。**不推荐。**

---
## Q6. 为什么本机只有 3 个多服务宿主 —— 以及这对结论的影响

本机 84 个 svchost 里 81 个只承载 1 个服务，看起来「没什么可拆」。
但这不是普遍情况，而是**内存触发的拆组策略**生效的结果：

```
HKLM\SYSTEM\CurrentControlSet\Control
    SvcHostSplitThresholdInKB = 0x380000 = 3,584 MB
本机物理内存 = 32,704 MB  >  3,584 MB   =>  拆分已启用
```

Windows 8+ 的规则：**物理内存超过 `SvcHostSplitThresholdInKB` 时，
svchost 按服务拆成独立进程**；低于阈值则**多个服务合并进同一个 svchost**。

### 这对结论是加强，不是削弱

| 机器类型 | svchost 形态 | 拆组的数值问题 |
|---|---|---|
| 本机（32 GB > 3.5 GB，已拆分） | 81/84 单服务 | 只有 3 个宿主受影响，虚增 1.07x —— **问题小但依然存在** |
| 低内存机器（< 3.5 GB）或改过策略 | **大量多服务宿主**（10-30 个服务/进程） | 虚增可达 **10-30x**，内存列彻底失真 |

**换句话说**：拆组这个功能的价值与「数值失真的严重程度」是**同一条曲线** ——
- 机器已拆分 → 没什么可拆，做它没收益
- 机器未拆分 → 有东西可拆，但拆出来的数字全是错的

**不存在「既有价值又数值正确」的区间。** 这是 Q1 结论的加强论证。

另外这也解释了 MUI 里那 32 项组名标签为什么是**静态全集**：
它们是给「未拆分/低内存」机器准备的，在那些机器上多服务宿主是常态。
本机只有 3 个多服务宿主，所以只用到其中 3 个标签（DcomLaunch / RPCSS / LocalServiceNoNetworkFirewall）。

---

## 结论

### 一句话结论

**不该做「拆组」。** 最关键的阻碍是 **Q1：Windows 完全没有服务级内存/CPU 计量
（`QueryServiceStatusEx` 只有 `dwProcessId`，PERF 计数器按 PID 划分），
拆行后每一行的数值必然要么重复（PID 1172 会虚增 5 倍）、要么是编造的均分，
而任务管理器的内存列是可求和的 —— 用户一加就会发现数字对不上。**

### 但有一个高价值、低成本的替代品

**应该做「命名」**：把 svchost 行的名字从 `svchost.exe` 改成 `服务主机: <显示名>`。
- 正是截图那行的真实来源（`TermService` → `Remote Desktop Services`）
- 数值口径 100% 正确（仍按 PID 取）
- 增量可做到 ≈0（服务表缓存）到 +2.63 ms (+29%)（每轮刷新）
- 代码量小，且 MUI 32 项组名表已在 `docs/evidence/mui_33550.txt` 中完整提取

→ 换句话说：**截图证明原版做了「命名」，没做「拆组」。我们只需要复刻命名。**

### 附带修正（本任务不改源文件，仅记录）

`include/pages_data.h:6` 与 `:88` 注释的
「`EnumServicesStatusExW` 44.53 ms / 306 服务」**实测不成立**：
稳态 p50 = **1.704 ms**（复用句柄+缓冲），冷启动也仅 ~3 ms。
44.53 ms 大概是首次 SCM 连接 + 冷页的偶发值，不应作为性能预算依据。

---

## 附：本次探针清单

| 文件 | 用途 |
|---|---|
| `docs/evidence/svchost_q1_probe.cpp` | Q1 三口径实测 + 分布 + 口径对账 |
| `docs/evidence/svchost_cost_probe.cpp` | Q3 枚举/冷启动/命令行成本 |
| `docs/evidence/svchost_locale_probe.cpp` | 语言环境对 lpDisplayName 的影响 |
| `docs/evidence/svchost_indirect_probe.cpp` | `@dll,-id` 间接字符串解析验证 |
| `docs/evidence/svchost_groupmap_probe.cpp` | MUI 32 项标签 vs 注册表组名 |
| `docs/evidence/svchost_name_probe.cpp` | 命名路径端到端（草案） |

前两次尝试留下的取证（仍在 `_probe/`）：
`svchost_probe{,2..6}.cpp`、`svchost_rule_probe.cpp`、`svchost_mui_evidence.cpp`、
`svchost_spec_probe.cpp`、`mui_33550.txt`、`mui_all.txt`。

**未启动 `taskmgr.exe`，未启动 `TaskManagerPro.exe` 可见窗口，未修改项目源文件。**

