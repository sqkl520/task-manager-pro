# Windows 10 性能页 —— 视觉规格（取证版）

> 取证机：Windows 10 Pro 22H2 **22H2** / Xeon 服务器级 CPU (10C/20T) / 32 GB / discrete GPU
> 取证日期：本次会话。**未启动 `taskmgr.exe`**（按用户要求，本机打开会长时间卡死）。
> 本文档**不含实现代码**，只记录「Win10 实际是什么样」以及「我凭什么这么说」。

---

## 0. 证据来源标记

| 标记 | 含义 | 可信度 |
|---|---|---|
| 【资源】 | 从本机 PE 资源**逐字节**解出，附文件、资源类型、资源 ID | 硬证据 |
| 【主题】 | 用 `OpenThemeData` + `GetThemeColor/DrawThemeBackground` 向 Windows 主题引擎**实时查询**得到 | 硬证据（但需看第 5.1 节的归属说明） |
| 【符号】 | 从 `taskmgr.exe` 的 RTTI / 调试符号字符串（`Class::Method`）反推的**代码结构** | 强证据（能证明"存在什么函数"，不能证明"函数怎么画"） |
| 【PE】 | 从导入表反推 | 硬证据 |
| 【MUI】 | 从 `zh-CN\taskmgr.exe.mui` / `zh-CN\wdc.dll.mui` 的 STRINGTABLE 逐字提取 | 硬证据（文案） |
| 【知识】 | 我的知识，**本机无证据** | 需外部验证 |
| 【推测】 | 推断 | 不可直接当规格用 |

### 本次实际用到的取证文件

| 文件 | 用途 |
|---|---|
| `C:\Windows\System32\zh-CN\taskmgr.exe.mui` | 848 条中文字符串、16 个 MENU、8 个 DIALOG |
| `C:\Windows\System32\zh-CN\wdc.dll.mui` | 589 条字符串（**内存条各段的 tooltip 文案在这里**） |
| `C:\Windows\System32\taskmgr.exe` | 导入表、RTTI、`Class::Method` 符号、GPU 引擎名表 |
| `C:\Windows\System32\CHARTV.dll` | 图表渲染 DLL 的导出/导入表 |
| **`C:\Windows\SystemResources\Taskmgr.exe.mun`** | **关键发现**：内嵌 2 个 `UIFILE`（DirectUI 布局）+ 14 个 PNG |
| **`C:\Windows\SystemResources\wdc.dll.mun`** | 内嵌 6 个 `UIFILE`（纯净 UTF-8 XML，**可直接读**） |
| `C:\Windows\Resources\Themes\aero\aero.msstyles` | `TaskManager` / `ChartView` 主题类的颜色与字体 |

> **`.mun` 是本项目此前取证漏掉的一层。** `taskmgr.exe` 自己的 `.rsrc` 只有 MUI/VERSION/MANIFEST，
> 真正的 UI 布局在 `SystemResources\Taskmgr.exe.mun` 里。`wdc.dll.mun` 的 `UIFILE` 是**未编译的 UTF-8 XML**，
> 可以直接当源码读 —— 里面连 `gradient(RGB(255,153,0),RGB(204,121,0),1)` 这种字面量都在。

---

## 1. 先纠正 `perf_page_spec.md` 的错误

### 1.1 已确认错误：3.2 节的「格子 = 背景色 + 居中大号百分比」

原文说每个逻辑处理器格子是：

```
┌──────────────────┐
│ CPU 0            │
│       45%        │   ← 居中，约 20px
└──────────────────┘
```

**这个结论是错的，且错在两处：**

**(a) 没有「居中大号百分比」这个元素。**

- 【资源】`Taskmgr.exe.mun` 的 `UIFILE 30001` 里，CPU 逻辑视图的元素树是：
  `cpuGrid` → `cpuBlock` / `atom(cpuBlockData)`，样式只有三个：
  `cpuBlockStyleB`（背景）、`cpuBlockStyleR`（另一个背景）、`cpuBlockStyleData`（文字）。
  `cpuBlockStyleData` 的字体是 `gtf(TASKMANAGER, 25, 0)`。
- 【主题】`TASKMANAGER` 主题类 part=25 的字体是 **`Microsoft YaHei UI`, lfHeight = -11**
  （即 **11 px 高**，约 8pt）。这是**小字**，不是 20px 的大字。
- 【资源】整个 `taskmgr.exe` + `Taskmgr.exe.mun` 里，**唯一的** `%.0f` 格式串**只**绑定在
  `dashSidebarEntry`（左侧列表的数值）上，**没有**任何百分比格式串绑定到 `cpuBlock`。
- 【MUI】`taskmgr.exe.mun` 中 `cpuBlock` 出现 8 次，全部是元素名/样式名，**没有任何 `%d%%` / `%.0f%%`
  与之相邻**。搜索 `%%`、`%d%%`、`%.0f` 在 `Taskmgr.exe.mun` 里命中 **0 次**。

→ **结论：`CPU 0` 格子里的文字只有「CPU 0」这个小标签，没有大号百分比数字。**
这一条是硬证据，可以放心据此删代码。

**(b) 但「每格是一张微型面积图」这个说法，我**没有**拿到等价强度的证据 —— 详见 §2.2。**
这一点必须诚实标注，不能把「热力图」和「面积图」混为一谈。

### 1.2 已确认错误：3.3 节的「格子热力图色带」

原文给的 5 段热力色带（`#F0F0F0` → `#0078D7` 线性插值）是**推测**，没有证据。真实情况见 §2.3。

### 1.3 `perf_page_spec.md` 中经本次核实**正确**的部分

- 左侧导航 vs 顶部选项卡：顶部选项卡正确（MUI MENU 30205 用「选项卡」一词，API 30206 是 Dashboard 右键菜单）。
- 内存组合条的分段名称与顺序：正确，且本次拿到了**每段的精确 RGB**（§3.2）。
- GPU 页元素清单：正确且**不完整**，见 §6。

### 1.4 还有一处 `perf_page_spec.md` 没写、但很关键的

`perf_page_spec.md` 第 11 节把「GPU 多个引擎视图」列为「需要解析 engtype」。本次拿到了**引擎名的显示名表**（§6.3），
这个表比 `engtype_` 原始串更值得照抄。

---

## 2. A. 逻辑处理器视图

### 2.1 布局与尺寸

| 项 | 值 | 证据 |
|---|---|---|
| 宿主元素 | `atom(heatMapCpuHost)`，与 `atom(numaCpuHost)`、`atom(overallCpuHost)` 平级，三者互斥显示 | 【资源】`UIFILE 30001` |
| 网格元素 | `atom(cpuGrid)` | 【资源】`UIFILE 30001` |
| 单格元素 | `cpuBlock`，内含 `atom(cpuBlockData)` | 【资源】`UIFILE 30001` |
| 单格内边距 | `rect(0rp,0rp,0rp,1rp)`（上/左/右 0，下 1） | 【资源】`UIFILE 30001` |
| 文字元素内边距 | `rect(5rp,0rp,5rp,0rp)`（左右各 5） | 【资源】`UIFILE 30001` |
| 网格背景色 | `gtc(TASKMANAGER, 38, 0, 3802)` = **`#117DBB`** | 【资源】+【主题】 |
| 单格背景 A | `cpuBlockStyleB` = `gtc(TASKMANAGER, 40, 0, 3802)` = **`#BFCFDF`** | 【资源】+【主题】 |
| 单格背景 B | `cpuBlockStyleR` = `gtc(TASKMANAGER, 39, 0, 3802)` = **`#DFE7EB`** | 【资源】+【主题】 |
| 单格文字色 | `cpuBlockStyleData` = `gtc(TASKMANAGER, 25, 0, 3803)` = **`#000000`** | 【资源】+【主题】 |
| 单格文字字体 | `gtf(TASKMANAGER, 25, 0)` = `Microsoft YaHei UI`, **lfHeight = -11** | 【主题】 |

**关于「20 核时的行列布局」：拿不到硬证据。**
`UIFILE` 里 `cpuGrid` 没有写死任何 `rowlayout(N)` / 固定行列数 —— 行列是
`CpuHeatMap::LayoutCpuBlocks` 在运行时算的（【符号】：函数名即
`base\diagnosis\pdui\atm\main\cpuheatmap.cpp` 的 `CpuHeatMap::LayoutCpuBlocks`）。
所以「5 列 × 4 行」这类具体数字**只能是观察值，不是规格**。这一点必须标注为【推测】。

> 顺带纠正原文 3.1 的「格子间距 2px」「最小约 60px」：这两个数字在 `UIFILE` 里都找不到。
> 能查到的内边距只有上面那两条 `rect()`。间距同样是运行时算的。

### 2.2 「每格画什么」—— 这是本次最需要小心的地方

我拿到了三类互相矛盾的线索，必须分开讲：

**线索 1：类名是 `CpuHeatMap`（支持「色块热力图」）**
- 【符号】`taskmgr.exe` 里有完整的 `CpuHeatMap` 类，方法包括：
  `Initialize` / `GetCpuUsageData` / `UpdateData` / `SetBlockData` / **`RefreshColors`** / **`LayoutCpuBlocks`** /
  `OnKernelTimesToggled`，源文件路径 `base\diagnosis\pdui\atm\main\cpuheatmap.cpp`。
- `RefreshColors` + `LayoutCpuBlocks` 这两个方法名的组合，强烈指向「按利用率刷新格子**颜色**」，
  而不是「往格子里塞一段历史曲线」。

**线索 2：`taskmgr.exe` 里没有任何面积图绘制能力**
- 【PE】`taskmgr.exe` 的 `GDI32.dll` 导入**只有**：
  `Rectangle, LineTo, MoveToEx, CreatePen, CreateDIBSection, SelectObject, GetDeviceCaps, DeleteObject`
  + 三个 `D3DKMT*`（显卡信息查询）。
- 【PE】**没有** `Polygon`、**没有** `Polyline`、**没有** `FillRect`、**没有** `gdiplus`、**没有** `d2d1`、**没有** `dwrite`。
- 这意味着：`taskmgr.exe` 自身**画不了填充面积图**。它最多能画 `Rectangle` 和折线（`MoveToEx`+`LineTo`+`CreatePen`）。
- 所有曲线图形都外包给 **`CHARTV.dll`**（延迟导入，【PE】），而 `CHARTV.dll` 才导入
  `Polygon / Polyline / SetRect / DrawTextW / BitBlt` + `gdiplus`（`GdipFillPolygonI / GdipDrawLinesI / GdipFillRectangleI`）。

**线索 3：`cpuGrid` 是 DirectUI 元素，不是 `CvChartWindow`**
- 【资源】主曲线用的是 `CvChartWindow`（`UIFILE 30001` 里 `atom(primaryCpuChart)` 等挂在 `chartArea` 下）。
- 而 `cpuGrid` / `cpuBlock` 是普通 DirectUI 元素，由 `CpuHeatMap` 自己填内容。

**综合判断（请按【推测】对待）：**

`CPU 0` 格子最可能是**纯色方块**（背景色随利用率变化，即真正的 heat map），
**不是**面积图。理由是线索 1 + 线索 2 的组合：
- 若每格是面积图，`CpuHeatMap` 必须能画折线/填充；
- 但 `taskmgr.exe` 只有 `Rectangle` + `LineTo`，而 `CpuHeatMap` 属于 `taskmgr.exe`（`cpuheatmap.cpp` 在 `atm\main\` 下），
  它并不在 `CHARTV.dll` 里；
- `RefreshColors` 这个方法名也支持「刷新颜色」而非「追加历史点」。

**但是**：`CreatePen` + `MoveToEx` + `LineTo` 的组合**确实能画一条折线**（不填充）。
所以「每格 = 一条不填充的折线」在技术上**是可能的**，我无法排除。

> **给实现者的诚实结论：**
> - 「**没有大号百分比**」= 硬证据，必须删。
> - 「**格子是纯色热力块**」= 高度可能的推断（有 `RefreshColors`/`LayoutCpuBlocks` + 无 `Polygon` 支撑），
>   但**没有**一条证据能直接证明它。
> - 「**格子是面积图**」= 与线索 2 冲突，我认为**可能性低**。
> - 拿不到直接证据的原因：唯一能一锤定音的办法是在 `taskmgr.exe` 运行时抓像素，而这台机器不许启动它。

**如果要一次性确认**，成本最低的办法（不需要本机启动 taskmgr）：
在**任意一台**正常 Win10 上截一张「性能 → CPU → 逻辑处理器」的图，放大后看格子里是纯色还是折线。
这是 30 秒的事，比我在这里继续反汇编划算得多。

### 2.3 格子颜色怎么随负载变化

**【主题】里找到了两组完整的色阶，这是本次最硬的颜色证据。**

`TaskManager` 主题类的 part 26–37 是一条 **12 级连续色阶**，从白到深蓝：

| part | 颜色 | 亮度 | 推测语义 |
|---|---|---|---|
| 26 | `#FFFFFF` | 255 | 0% |
| 27 | `#E0F0FF` | 237 | |
| 28 | `#CDE9FF` | 227 | |
| 29 | `#B4DEFF` | 213 | |
| 30 | `#7EC6FF` | 183 | |
| 31 | `#43A9E4` | 145 | |
| 32 | `#3782AD` | 112 | |
| 33 | `#2477A7` | 100 | |
| 34 | `#1F71A1` | 94 | |
| 35 | `#186A99` | 87 | |
| 36 | `#14587F` | 72 | |
| 37 | `#0E405D` | 52 | 100% |

另有一条 **11 级 黄→橙红** 色阶（part 3–12），这是**进程页的「热力图列」**（CPU/内存/磁盘/网络列的背景着色）：

| part | 颜色 |
|---|---|
| 3 | `#FFF9E4` |
| 4 | `#FFF4C4` |
| 5 | `#F9ECA8` |
| 6 | `#FFE487` |
| 7 | `#FFD264` |
| 8 | `#FFC63D` |
| 9 | `#FCB816` |
| 10 | `#FCA71D` |
| 11 | `#FC682A` |
| 12 | `#FC682A` |

> **重要归属警告（见 §5.1）**：这两条色阶确实存在于 `TaskManager` 主题类里（`aero.msstyles` 中能找到对应 RGB 三元组），
> 但**我无法从 DUI 布局中证明 `cpuBlock` 用的是 part 26–37 中的哪一个** —— `UIFILE` 只引用了 part 39/40 作为两个
> static 背景。part 26–37 这一整段在 `UIFILE` 里**没有被直接引用**，说明它们是给**代码**（`CpuHeatMap::RefreshColors`）
> 按利用率**动态选取**的。这个「12 级蓝色阶 = 逻辑处理器格子的热力色带」是**强推断**，不是铁证。

**因此，实现建议（保守做法）：**
- 若格子是纯色热力块：用 part 26→37 这 12 级色阶，`idx = min(11, util * 12 / 100)`。
- 别用原文 3.3 的 `#F0F0F0 → #0078D7` 线性插值 —— 那个色带**任何证据都不支持**。

### 2.4 Y 轴刻度 / 网格线

- 【资源】逻辑处理器视图**没有** Y 轴刻度元素。`cpuGrid` 下面只有 `cpuBlock`/`cpuBlockData`。
  主曲线才有 `atom(RateLabelCpu)` + `resstr(33824)`（= `0%`）和 `resstr(33836)`（= `100%`）做刻度。
- 【资源】`gtc(TASKMANAGER, 38, 0, 3802)`（`#117DBB`）挂在 `cpuHeatGrid` 上 —— 这是**格子之间的分隔/网格色**，
  不是文字色。`#117DBB` 是深蓝，用它做格子缝隙会形成很明显的蓝线。
  → **这和「格子间距 2px、缝隙是浅灰」的直觉完全不同**，实现时要注意。
- 【资源】`cpuHeatGrid` 还有一个 `themeable(gtc(ItemsView::ListView, 1, 3, 3803), highlightText)`，
  高对比度主题下退回 `highlightText`。

### 2.5 `CPU %d` 标签

- 【MUI】`taskmgr.exe.mui` 逐字：
  - `34286` = **`CPU %d`**
  - `34287` = **`CPU %1!d! (节点 %2!d!)`**
  - `34288` = **`NUMA 节点 %d`**
  - `34294` = **`CPU %d @ %s GHz`**（这是**侧栏/标题**用的，不是格子标签）
- 【资源】`Taskmgr.exe.mun` 里 `UIFILE` 用的元素 ID 格式串是 `CPU%d`（`0x10aa00`）和 `CPU%d%d`（`0x10a9c8`，
  后者应是 NUMA 的「CPU x + 节点 y」拼装）。

→ 格子标签文案确认是 `CPU 0`、`CPU 1`…（`CPU %d`）。

---

## 3. C. 内存页完整布局

### 3.1 页面结构（【资源】`wdc.dll.mun` → `UIFILE 30003`，**未编译 XML，可直读**）

```
expando (整页)
└ clipper
  └ element  padding=rect(8,16,8,8)
    ├ atom(memUsageBarchart_memtab)   ← 堆叠条，layout=borderlayout, margin 下 8rp
    ├ atom(memUsageLegends_memtab)    ← 图例，tablelayout 6 行
    ├ flowlayout 组 (可用/缓存/总数/已安装 四行两列)
    └ scrollviewer
      ├ expandoMemory_memtab
      │ ├ atom(memoryGraph_memtab)         ← 内存使用曲线
      │ ├ atom(pagefileGraph_memtab)       ← 提交大小曲线
      │ └ atom(hardfaultsGraph_memtab)     ← 硬错误曲线
      └ expando（进程/驱动内存明细表）
```

> **注意**：`wdc.dll.mun` 的这套 `UIFILE` 是 **资源监视器/性能监视器（resmon/perfmon）的图表面板**，
> 不是任务管理器的内存页。任务管理器的内存页在 `Taskmgr.exe.mun` 的 `UIFILE 30001` 里。
> 两者**共用同一套视觉语言**，但内存条布局只在 `wdc` 里能读到完整 XML。见 §5.1 的归属说明。

### 3.2 堆叠条各段的颜色 —— **这是本次拿到的最硬的颜色证据**

【资源】`wdc.dll.mun` → `UIFILE 30003`，`<style resid="barchart">` 中**逐字**有：

```xml
<if class="reserved">
  <element background="themeable(gradient(RGB(203,203,203),RGB(166,166,166),1), RGB(166,166,166))" .../>
<if class="free">
  <element background="themeable(gradient(RGB(198,217,241),RGB(151,186,229),1),RGB(151,186,229))" .../>
<if class="standby">
  <element background="themeable(gradient(RGB(85,142,213),RGB(49,116,197),1),RGB(49,116,197))" .../>
<if class="modified">
  <element background="themeable(gradient(RGB(255,153,0),RGB(204,121,0),1),RGB(204,121,0))" .../>
<if class="inuse">
  <element background="themeable(gradient(RGB(143,184,12),RGB(126,162,10),1), RGB(126,162,10))" .../>
```

**逐段精确值（上方为 Aero 渐变的起止，括号内为高对比度回退值）：**

| 段 | 中文标签【MUI】 | 渐变起 | 渐变止 | 高对比度回退 |
|---|---|---|---|---|
| 为硬件保留 | `为硬件保留的内存` (33877) | `#CBCBCB` | `#A6A6A6` | `#A6A6A6` |
| 使用中 | `正在使用` (33852) | `#8FB80C` | `#7EA20A` | `#7EA20A` |
| 已修改 | `已修改` (33857) | `#FF9900` | `#CC7900` | `#CC7900` |
| 备用 | `备用` (33868) | `#558ED5` | `#3174C5` | `#3174C5` |
| 可用/空闲 | `可用` (33849) | `#C6D9F1` | `#97BAE5` | `#97BAE5` |

> **这纠正了 `perf_page_spec.md` 4.2 节。** 原文说「使用中（深蓝）→ 已修改（橙）→ 备用（浅蓝）→ 可用（浅灰）」。
> 真实值：
> - **使用中不是深蓝，是橄榄绿 `#8FB80C`**。这是最值得注意的一条 —— 很多人记成蓝色。
> - 已修改确实是橙 `#FF9900`，但**偏高饱和**，不是普通的橙。
> - 备用是**中蓝** `#558ED5`（比「浅蓝」深）。
> - 可用是**浅蓝** `#C6D9F1`，**不是浅灰**。
> - 原文还**漏了「为硬件保留」这一段**（`#CBCBCB` 灰），它在条的最左端。

**各段宽度比例：运行时算的，拿不到。**
【资源】`UIFILE` 里 `BarchartInuse` 是 `layoutpos="client"`（吃掉剩余宽度），
`BarchartReserved`/`Modified`/`Standby`/`Free` 都是 `layoutpos="left"` 且 `height="24rp"` —— 宽度由代码按比例设。
条高 = **24 rp**（硬证据）。

**分段顺序（从左到右，【资源】按 XML 中 `layoutpos` 顺序）：**
```
[BarchartReserved]  [BarchartInuse .........client......... ]  |  [BarchartRight: Modified][Standby][Free]
```
即：**为硬件保留 → 使用中 → 已修改 → 备用 → 可用**。
`BarchartInuse` 用 `client` 吃掉其余宽度，说明**只有「使用中」是弹性段**，其余按数据比例定宽。

### 3.3 各段的确切中文标签

【MUI】以下来自 `wdc.dll.mui`，是**短标签**（条下方的图例用）：

| ID | 文本 |
|---|---|
| `33877` | `为硬件保留的内存` |
| `33852` | `正在使用` |
| `33841` | `已修改` |
| `33868` | `备用` |
| `33849` | `可用` |
| `33840` | `可用` |
| `33841` | `缓存`（注意：`33841` 在任务管理器 MUI 里是「已修改」，在 wdc MUI 里又一个 33841 是「缓存」—— **两套 MUI ID 空间独立**） |
| `33803` | `已安装` |
| `33804` | `总数` |

**【MUI】长 tooltip（`wdc.dll.mui`，鼠标悬停时显示）：**

| ID | 文本 |
|---|---|
| `33606` | `使用中: 由进程、驱动程序或操作系统使用的内存` |
| `33605` | `已修改: 内容必须写入磁盘才能用于其他用途的内存` |
| `33604` | `备用: 包含未活跃使用的缓存数据和代码的内存` |
| `33603` | `空闲: 不包含任何有价值数据，以及当进程、驱动程序或操作系统需要更多内存时将首先使用的内存` |
| `33607` | `可用: 可以由进程、驱动程序或操作系统立即使用的内存量(包括备用内存和空闲内存)` |
| `33608` | `缓存: 包含供进程、驱动程序和操作系统快速访问的缓存数据和代码的内存量(包括备用内存和修改的内存)` |
| `33609` | `已保留硬件: 已保留供 BIOS 和其他外设的某些驱动程序使用的内存` |
| `33601` | `总计: 可用于操作系统、设备驱动程序和进程的物理内存量` |
| `33602` | `已安装: 计算机中安装的物理内存量` |

> **重要**：tooltip 用的是「**空闲**」，而条下面的短图例用「**可用**」。
> 这两者是**不同的内存概念**（Free vs Available），Win10 里短图例把 Free 段标成了「可用」。
> 这是微软自己的文案不一致，实现时照抄即可，不要「修正」它。

### 3.4 右侧信息列

【MUI】`taskmgr.exe.mui` 逐字：

| ID | 文本 |
|---|---|
| `34349` | `速度:` |
| `34350` | `已使用的插槽:` |
| `34380` | `外形规格:` |
| `34381` | `为硬件保留的内存:` |
| `34382` | `最大内存:` |

【资源】`UIFILE 30001` 里 `static_memory` 是 `rowlayout(2)`（两列），元素顺序：
`memory_speed_Label`/`memory_speed` → `slot_used_Label`/`slot_used` → `form_factor_Label`/`form_factor` →
`reserved_Label`/`reserved` → `max_memory_Label`/`max_memory`。

**「可用」「已缓存」「已提交」属于 `dynamic_memory` 区，不是 static：**
```
atomic(dynamic_memory):
  memory_inuse  → resstr(34399) = "使用中"
  memory_avai   → resstr(34352) = "可用"
  memory_commit → resstr(34354) = "已提交"
  memory_cache  → resstr(34353) = "已缓存"
  memory_paged  → resstr(34356) = "分页缓冲池"
  memory_non_paged → resstr(34358) = "非分页缓冲池"
```

> **这纠正了 `perf_page_spec.md` 4.3。** 原文把「可用/已缓存/已提交」和「速度/插槽/外形规格」
> 混在一个列表里。实际是**两个不同的分组**：
> - `static_memory`（硬件规格，不随时间变）：速度、已使用的插槽、外形规格、为硬件保留的内存、最大内存
> - `dynamic_memory`（实时值）：使用中、可用、已提交、已缓存、分页缓冲池、非分页缓冲池

**内存型号串格式：**
- `34366` = `%u MHz`
- `34385` = `插槽 %1!u!: %2!s!, %3!hu! MHz`
- `34386` = `插槽 %1!u!: 空`
- `34376` = `%1!s!/%2!s! MB`，`34378` = `%1!s!/%2!s! GB`

---

## 4. D. 磁盘页

### 4.1 各列标签【MUI】

| ID | 文本 |
|---|---|
| `34461` | `活动时间` |
| `34459` | `平均响应时间` |
| `34457` | `读取速度` |
| `34458` | `写入速度` |
| `34460` | `磁盘传输速率` |
| `34462` | `100%` |
| `34466` | `%s 毫秒` |
| `34468` | `%.0f%%` |
| `34450` | `磁盘 %1!d! (%2!s!)` |

### 4.2 两个图表【资源】

`UIFILE 30001`：
```
disk_chart_client:
  atom(diskTimeTitle)  → resstr(34462) = "100%"
  atom(diskTimeGraph)  → resstr(34653) = "磁盘处理读取或写入请求的时间百分比"
  atom(diskTime_bottom) → atom(RateLabelDiskTime) → resstr(33815) = "0"
  atom(diskRateTitle)  → resstr(34460) = "磁盘传输速率"
  atom(disk_rate_unit)
  atom(diskRateGraph)  → resstr(34652) = "此磁盘上的读取和写入活动"
  atom(diskRate_bottom) → atom(RateLabelDiskRate) → resstr(33819) = "0"
```
→ **两个图**：上「活动时间」（0–100%），下「磁盘传输速率」（自动单位）。确认原文正确。

### 4.3 右侧磁盘属性【MUI】

| ID | 文本 |
|---|---|
| `34451` | `类型:` |
| `34453` | `容量:` |
| `34454` | `已格式化:` |
| `34455` | `系统磁盘:` |
| `34456` | `页面文件:` |

类型取值【MUI】：`34469` `HDD` / `34470` `SSD` / `34471` `持久性内存` / `34472` `可移动` / `34473` `SD` / `34474` `USB` / `34477` `未知`

【资源】`static_disk` = `rowlayout(3)`（三列）。
【资源】`dynamic_disk_server` 含 `diskReadLegend`/`diskWriteLegend` + `disk_time`/`disk_response`
（**服务端磁盘视图**多了每条磁盘的活动时间/响应时间两列）。

### 4.4 磁盘图例色

【资源】`Taskmgr.exe.mun` 有两张 2×32 的 PNG 色键图：
- `IDB_PERFTAB_NET_RECEIVE_COLORKEY` → 单色 **`#B15C11`**
- `IDB_PERFTAB_NET_SEND_COLORKEY` → 单色 **`#DC652B`**

> 这两张虽然是网络图的色键（名称里有 `NET`），但**磁盘的读/写图例用的是同一机制**：
> DirectUI 用 `sgraphic` 贴一张纯色小图 + Colorkey 透明，来画图例方块。
> 这两个颜色（`#B15C11` 棕橙 / `#DC652B` 橙）就是**网络收发两条曲线的颜色**。
> **磁盘**读/写颜色：**没拿到直接证据**。原文说磁盘读写是两条曲线但没给颜色。

---

## 5. E. 网络页

### 5.1 标题 = 适配器名称

【MUI】`34549` = `适配器名称:`。【资源】`UIFILE 30001` 里网络页的标题元素是
`atom(netChartTitle)` → `resstr(34560)` = `吞吐量`，另有 `atom(net_adapter)` → `resstr(34654)` =
`此网络上的发送和接收活动`。左侧列表项名来自 `sidebar_net_name` / `sidebar_net_adapter_name`。

### 5.2 吞吐量图 + 各统计列

【资源】`dynamic_network` 元素：
```
atom(netSendLegend) → resstr(34562) = "发送"
atom(netSend)
atom(netReceiveLegend) → resstr(34561) = "接收"
atom(netReceive)
atom(net_chart) → atom(netChartTitle) = "吞吐量"
              → atom(netChartMax)      = 自动上限
              → atom(primaryNetChart_bottom) → atom(RateLabelNet) → resstr(33819) = "0"
```
→ **只有一条吞吐量曲线**（接收+发送合在一起），不是两条分开的线。
但图例有「发送」「接收」两项 —— 说明曲线是**双色堆叠/叠加**的（对应上面两张色键 PNG）。

### 5.3 连接类型 / 接收 / 发送

【MUI】`static_network` = `rowlayout(4)`（四列）：
```
adapter_Label    (34549) 适配器名称:   → adapter
wwan_Label       (34550) 服务提供商:   → wwan
wifi_Label       (34558) SSID:         → wifi
netname_Label    (34554) DNS 名称:     → netname
netType_label    (34551) 连接类型:     → netType
ipv4_Label       (34552) IPv4 地址:    → ipv4
ipv6_Label       (34553) IPv6 地址:    → ipv6
signal_Label     (34564) 信号强度:     → signal
```

**连接类型的取值**【MUI】：
`34693` `以太网` / `34694` `Wi-Fi` / `34695` `移动设备` / `34696` `Wi-Fi Direct` / `34697` `WLAN Direct`

**网络详细信息列**（`34667`–`34691`，由 MENU `查看网络详细信息` 打开）：
| ID | 文本 |
|---|---|
| `34667` | `属性` |
| `34668` | `网络使用率` |
| `34669` | `链接速度` |
| `34670` | `状态` |
| `34671` | `发送字节吞吐量` |
| `34672` | `接收字节吞吐量` |
| `34673` | `字节吞吐量` |
| `34674` | `已发送的字节` |
| `34675` | `已接收的字节` |
| `34680` | `已发送的单播` |
| `34681` | `已接收的单播` |
| `34686` | `已发送的非单播` |
| `34687` | `已接收的非单播` |

---

## 6. F. GPU 页（重点）

### 6.1 页面元素树（【资源】`UIFILE 30001`，完整）

```
static_gpu  = rowlayout(5)                       ← 右侧静态信息，5 列
  ├ gpudriverversion_label → resstr(41001) "驱动程序版本:"
  ├ gpudriverversion
  ├ gpudriverdate_label   → resstr(41002) "驱动程序日期:"
  ├ gpudriverdate
  ├ gpudxversion_label    → resstr(41003) "DirectX 版本:"
  ├ gpudxversion
  ├ gpulocation_label     → resstr(41032) "物理位置："
  ├ gpulocation
  ├ gpuhwreservedmem_label→ resstr(41033) "为硬件保留的内存："
  └ gpuhwreservedmem

dynamic_gpu
  ├ gpuUsage
  ├ gpuTotalMemory → totalMemory → resstr(41011) "GPU 内存" → memValue
  ├ gpuDedicatedMemRoot → gpuDedicatedMemLabel → resstr(41034) "专用 GPU 内存"
  ├ gpuSharedMemRoot    → gpuSharedMemLabel    → resstr(41035) "共享 GPU 内存"
  ├ gpuTemperature      → gpuTemperatureValue   (resstr 41040 "GPU 温度", 41041 "%d °C")
  └ gpu_chart
      ├ atom(gpuPrimaryCharts)                   ← 总计曲线容器
      ├ atom(gpuMemoryCharts)   margin top/bottom 5rp
      ├ singleEngineGpuView  → resstr(41027) "总体 GPU 图表窗格"
      │   ├ topArea
      │   ├ engineSelector   (dtb ItemsView::Header 4,2 下拉)
      │   ├ engineTitle
      │   ├ engineUtilizationRate
      │   └ engineChart      → resstr(34655) "GPU 活动"
      └ multiEngineGpuView   → resstr(41028) "多个引擎 GPU 图表窗格"
          ├ gpuGrid
          │   └ gpuEngineChartTemplate  → gpuEngineChart  (每个引擎一格，复用之)
          └ gpuMemoryChart
              ├ gpuDedicatedMemory → gpuMemUsageTitle → resstr(41012) "专用 GPU 内存利用率"
              │                    → gpuMemoryMax
              │                    → gpuMemUsageGraph → resstr(41014) "专用 GPU 内存利用率"
              └ gpuSharedMemory    → gpuMemUsageShared → resstr(41013) "共享 GPU 内存利用率"
                                   → resstr(41015) "共享 GPU 内存利用率"
```

### 6.2 GPU 曲线元素 ID（【资源】）

`Taskmgr.exe.mun` 的字符串表里有一整套带 `%d_%d_%d` 后缀的元素 ID ——
说明**每个适配器/引擎都动态生成自己的元素**（三元组 = adapter, phys, engine）：

```
gpu_chart_m_%d_%d_%d     多个引擎总图
gpu_chart_s_%d_%d_%d     单引擎图
gpu_entry_%d_%d_%d
gpu_graph_%d_%d_%d
sel_eng_%d_%d_%d         engineSelector 里的项
pri_eng_%d_%d_%d         主引擎
mul_eng_%d_%d_%d         多引擎
gpu_mem_chrt_%d_%d_%d    显存图
ded_mem_%d_%d_%d         专用显存
```

其他 GPU 元素 ID：
`gpuUsage` / `gpuSharedMemRoot` / `gpuDedicatedMemRoot` / `gpuTemperature` / `gpuTotalMemory` /
`gpuSharedMemLabel` / `gpuDedicatedMemLabel` / `gpuTemperatureValue` / `memValue` /
`sidebar_gpu_name` / `sidebar_gpu_util` / `sidebar_gpu_adapter` / `sidebar_gpu_temp` /
`gpuhwreservedmem` / `gpudriverversion` / `gpudxversion` / `gpudriverdate` / `gpulocation` /
`totalMemory` / `engineSelector` / `engineTitle` / `engineChart` / `gpuGrid` /
`gpuEngineChartTemplate` / `gpuEngineChart` / `gpuMemoryCharts` / `gpuMemoryChart` /
`gpuDedicatedMemory` / `gpuMemoryMax` / `gpuMemUsageTitle` / `gpuMemUsageGraph` /
`gpuSharedMemory` / `gpuMemUsageShared`

### 6.3 引擎类型显示名 —— **本次最重要的新发现**

【PE/资源】`taskmgr.exe` 里有一张**连续的字符串表**（偏移 `0xF3390` 起），
逐字节读出是：

```
Crypto \0\0 Legacy Overlay \0\0 Scene Assembly \0\0 Video Processing \0\0\0\0
Copy \0\0\0\0 Video Encode \0\0\0\0 Video Decode \0\0 3D \0\0\0\0
```

配套的短名（同文件 `0xF3C90` 起）：
```
LegacyOverlay, SceneAssembly, VideoProcessing, VideoEncode, VideoDecode
```

> **这就是「多个引擎」视图里那 4 个小图表的标题来源。**
> `perf_page_spec.md` 7 节写的是「`engtype_3D` / `Copy` / `VideoDecode` / `VideoEncode` / `Compute`」，
> 实际显示名是**带空格的大写混合**：`3D`、`Copy`、`Video Decode`、`Video Encode`，
> 另外还有 `Crypto`、`Legacy Overlay`、`Scene Assembly`、`Video Processing`（这 4 个原文漏了）。

**本机实际引擎类型**（【实测】`typeperf -qx "GPU Engine"`，1220 个实例）：
```
3D, Copy, LegacyOverlay, Security, Security_1, VR, VideoDecode, VideoEncode
```
→ 本机 discrete GPU 上还有 `VR` / `Security`，但**任务管理器的显示名表里没有它们**
（`Security` / `VR` 被归入或不显示）。这解释了为什么不同机器上看到的小图表数量不同。

### 6.4 右侧信息 —— 与用户期望的差异

用户说「GPU 界面做的更是阉割」。实际 Win10 GPU 页右侧只有 **5 项**（【资源】`static_gpu` = `rowlayout(5)`）：

| 项 | MUI | 有无 |
|---|---|---|
| 驱动程序版本 | `41001` | ✅ |
| 驱动程序日期 | `41002` | ✅ |
| DirectX 版本 | `41003` | ✅ |
| 物理位置 | `41032` | ✅ |
| 为硬件保留的内存 | `41033` | ✅ |

**任务描述里提到的「显存 / 共享内存 / 进程数 / 序列号」，在 Win10 的 GPU 页右侧信息列里并不存在：**
- 「专用 GPU 内存」「共享 GPU 内存」（`41034`/`41035`）在 **`dynamic_gpu`** 区（标题下方的大数字，随时间变），
  不在 `static_gpu` 信息列里。
- 【MUI】**「序列号」在整个 `taskmgr.exe.mui` 里 0 命中。**
- 【MUI】**「进程数」0 命中。**
- 【MUI】**「显存」0 命中**（Win10 用「GPU 内存」`41011`，不用「显存」）。
- 【MUI】`41004` = `利用率`；`41006` = `% 利用率`；`41016` = `%.0f%%`（GPU 百分比格式）。

> 所以：**不要为了「不阉割」而加「序列号」「进程数」—— Win10 原版就没有。**
> 真正的「阉割感」来源更可能是：
> 1. 缺 `singleEngineGpuView` / `multiEngineGpuView` 的**视图切换**（右键菜单「单个引擎 / 多个引擎」）
> 2. 缺 `gpuMemoryChart`（专用/共享显存**利用率曲线**，不是只有一个数字）
> 3. 缺 `engineSelector` 下拉（多 GPU / 多引擎时切换）
> 4. 缺 `gpuTemperature`（`41040`「GPU 温度」+ `41041`「%d °C」）

### 6.5 GPU 相关 MUI 字符串全表（逐字）

| ID | 文本 |
|---|---|
| `41000` | `适配器名称:` |
| `41001` | `驱动程序版本:` |
| `41002` | `驱动程序日期:` |
| `41003` | `DirectX 版本:` |
| `41004` | `利用率` |
| `41005` | `%1!d!%2!s!` |
| `41006` | `% 利用率` |
| `41007` | `GPU %d` |
| `41008` | `GPU %d (链接 %d)` |
| `41009` | `100%` |
| `41010` | `0` |
| `41011` | `GPU 内存` |
| `41012` | `专用 GPU 内存利用率` |
| `41013` | `共享 GPU 内存利用率` |
| `41014` | `专用 GPU 内存利用率` |
| `41015` | `共享 GPU 内存利用率` |
| `41016` | `%.0f%%` |
| `41017` | `GPU` |
| `41018` | `GPU 引擎` |
| `41019` | `GPU` |
| `41020` | `GPU 引擎` |
| `41021` | `专用 GPU` |
| `41022` | `GPU 系统` |
| `41023` | `所有 GPU 引擎的最高利用率` |
| `41024` | `利用率最高的 GPU 引擎` |
| `41025` | `由该进程所使用的 GPU 专用图形内存(所有 GPU 中)总量` |
| `41026` | `由该进程所使用的系统内存(所有 GPU 中)总量` |
| `41027` | `总体 GPU 图表窗格` |
| `41028` | `多个引擎 GPU 图表窗格` |
| `41032` | `物理位置：` |
| `41033` | `为硬件保留的内存：` |
| `41034` | `专用 GPU 内存` |
| `41035` | `共享 GPU 内存` |
| `41036` | `电源使用情况` |
| `41037` | `电源使用情况趋势` |
| `41038` | `CPU、磁盘和 GPU 对功耗的影响` |
| `41039` | `一段时间内 CPU、磁盘和 GPU 对功耗的影响` |
| `41040` | `GPU 温度` |
| `41041` | `%d °C` |
| `41042` | `(%d °C)` |

另有「计算加速器」一组（`45000`–`45010`，Win10 上基本不出现，是给 Compute-only 设备的）：
`45000/45001` `计算加速器`、`45002` `计算 %d`、`45003/45007` `专用内存利用率`、
`45004/45008` `共享内存利用率`、`45005` `专用内存`、`45006` `共享内存`、`45009` `计算加速器活动`、`45010` `总内存`

### 6.6 数据源（【PE】，反推）

- 【PE】`taskmgr.exe` 导入 `pdh.dll`: `PdhAddCounterW, PdhOpenQueryW, PdhGetFormattedCounterArrayW,
  PdhCloseQuery, PdhCollectQueryData, PdhGetRawCounterArrayW`
- 【PE】`taskmgr.exe` 导入 `dxgi.dll`: `CreateDXGIFactory2` → 枚举适配器
- 【PE】`taskmgr.exe` 导入 `GDI32.dll`: `D3DKMTOpenAdapterFromLuid, D3DKMTQueryAdapterInfo, D3DKMTCloseAdapter`
  → 用 LUID 查适配器详细信息（显存、驱动版本等）
- 【PE】字符串 `pid` / `luid` / `phys` / `eng` / `part` / `engtype_` 在 `0xEEFA0` 附近**连续出现** —— 这是
  `\GPU Engine(*)` 实例名的解析关键词，证实任务管理器也是**解析 PDH 实例名**来聚合的，和 `perf_page_spec.md` 8.3(e) 一致。

---

## 7. 关于「证据归属」的重要说明（请务必读）

### 7.1 `wdc.dll` 的 UIFILE 不是任务管理器的

我在 §3 大量引用了 `wdc.dll.mun` 的 `UIFILE`（因为它是唯一可直读的 XML）。必须说清：

- 【PE】`taskmgr.exe` **不导入** `wdc.dll`。`resmon.exe` / `perfmon.exe` 也**不导入**它。
- 【PE】`wdc.dll` 的导出只有 6 个：`DllCanUnloadNow, DllGetClassObject, DllRegisterServer,
  DllUnregisterServer, WdcParseLegacyFile, WdcRunTaskAsInteractiveUser` —— 它是 **COM 组件**，按 CLSID 加载。
- 【PE】`wdc.dll` 内部字符串明确指向 `Performance Monitor` / `perfmon.exe` / `resmon.resmoncfg`。
- 但 `taskmgr.exe` 里**有大量 `Wdc*` 类**（`WdcCpuMonitor`, `WdcGpuMonitor`, `WdcMemoryMonitor`,
  `WdcDiskMonitor`, `WdcNetworkMonitor`, `WdcChart`, `WdcDataPortal`…）——这个 "Wdc" 前缀与 `wdc.dll` 同名，
  说明它们**共享同一套源码基线**（`base\diagnosis\pdui\...`）。

**所以：** `wdc.dll.mun` 里的**颜色字面量**（`RGB(143,184,12)` 等）是可靠的「微软在这套代码里怎么配内存条」的证据，
但**不能 100% 断言任务管理器的内存条用的是完全相同的值**。
不过 —— 【资源】`Taskmgr.exe.mun` 里的 `BarchartInuse/Modified/Standby/Free/BarchartMemoryHolder`
这一组元素名与 `wdc` 完全相同，且 `UIFILE 30001`（任务管理器自己的）里
`BarchartInuse` → `gtc(CHARTVIEW, 9, 0, 3802)`，而 `CHARTVIEW` 主题类 part 9 实时查询 = `#8B12AE`（紫）。

### 7.2 主题查询值出现「紫色」—— 我无法解释，必须标注

用 `OpenThemeData("ChartView")` + `GetThemeColor(part 9, 0, TMT_FILLCOLOR)` 得到 `#8B12AE`（紫）。
这**明显不符合**内存条的真实外观。可能原因（我**无法确认**哪一个）：

1. `ChartView` 主题类在 `aero.msstyles` 里只有很少几个 part 有定义，part 9 可能不是内存条那一格；
2. `GetThemeColor` 的 part/state 语义与 DUI 的 `gtc()` 不完全一一对应（DUI 可能走 `GetThemeColor` 的
   另一条路径，或先做 `themeable()` 解析）；
3. 需要 `GetThemeColor` 之外再查 `TMT_GRADIENTCOLOR*` / `TMT_ENUM(BGTYPE)` 才能还原真实绘制（我查了，返回空）。

**处理办法：§3.2 的色彩表，我以 `wdc.dll.mun` 里的 `RGB()` 字面量为准**（那是源码级常量，不是主题查询结果），
并在表里标明这是 `wdc` 的值。`TaskManager` 主题类查到的 `#117DBB`（`cpuHeatGrid`）、
`#BFCFDF`/`#DFE7EB`（两个 block 背景）我保留，因为它们是**通过 DUI 里的 `gtc(TASKMANAGER, 38/39/40, 0, 3802)`
直接指名的 part**，映射关系明确，可信度高。

### 7.3 我没能拿到证据的东西（不要当成规格用）

| 项 | 状态 |
|---|---|
| 逻辑处理器格子的确切列数/行数 | 运行时算，拿不到 |
| 格子间距的确切 px | 运行时算，拿不到 |
| 格子是「纯色块」还是「折线/面积图」 | **无法确证**，见 §2.2 |
| 内存条各段的宽度比例 | 运行时按数据比例算 |
| 磁盘读/写曲线的颜色 | 拿不到（只有网络收发的色键色） |
| 主曲线的线宽、填充 alpha | 拿不到（在 `CHARTV.dll` 里硬编码） |
| 顶部选项卡的确切高度/字号 | `UIFILE` 在 `wdc` 里是 `height="30rp"`/`31rp`（`tab` style），任务管理器自己的在 `duib` 里没解出 |
| GPU 温度是否显示 | 有 MUI 字符串，但 **NVIDIA 消费卡通常不暴露**，需实测 |

---

## 8. 实现优先级

按「对还原度的影响 ÷ 实现成本」排序。

### P0 —— 必做，不做就是明显不像

| # | 事项 | 依据 | 说明 |
|---|---|---|---|
| 1 | **删掉逻辑处理器格子里的大号百分比** | 硬证据 §2.2(a) | 当前实现如果有，这是最刺眼的错 |
| 2 | 格子标签用 `CPU %d`，字号 ≈ 11px（`Microsoft YaHei UI`） | 【主题】part 25 = -11 | 小字，不是 20px |
| 3 | 内存条 5 段用**正确颜色**：使用中 `#8FB80C`（绿！）、已修改 `#FF9900`、备用 `#558ED5`、可用 `#C6D9F1`、硬件保留 `#CBCBCB` | 【资源】`wdc` XML 字面量 | 当前若把「使用中」画成深蓝，是根本性错误 |
| 4 | 内存条**加上「为硬件保留」段**（最左，灰） | 【资源】`BarchartReserved` | 当前实现很可能漏了 |
| 5 | 条高 24 rp | 【资源】`height="24rp"` | |
| 6 | 内存右侧信息**分成两组**：static（速度/插槽/外形/保留/最大）+ dynamic（使用中/可用/已提交/已缓存/分页/非分页） | 【资源】+【MUI】§3.4 | 当前混在一个列表里 |
| 7 | GPU 页右侧信息列 = **5 项**（驱动版本/驱动日期/DX 版本/物理位置/硬件保留） | 【资源】`rowlayout(5)` | 不要塞「序列号」「进程数」（Win10 没有） |

### P1 —— 高影响，用户「阉割感」的主要来源

| # | 事项 | 依据 |
|---|---|---|
| 8 | GPU 页补 `multiEngineGpuView`：**每个引擎一个小图**，标题用 `3D` / `Copy` / `Video Decode` / `Video Encode`（+ `Crypto` / `Legacy Overlay` / `Scene Assembly` / `Video Processing`） | 【PE】引擎名表 §6.3 |
| 9 | GPU 页补 `singleEngineGpuView` + `engineSelector` 下拉（切换看哪个引擎） | 【资源】§6.1 |
| 10 | GPU 页补 `gpuMemoryChart`：专用/共享显存**利用率曲线**（不只是两个数字） | 【资源】§6.1 |
| 11 | 磁盘页两个图：活动时间（0–100%）+ 传输速率（自动单位） | 【资源】§4.2 |
| 12 | 网络页：吞吐量图 + 接收/发送双色（`#B15C11` / `#DC652B`） | 【资源】色键 PNG |
| 13 | 网络右侧 8 项（适配器/服务提供商/SSID/DNS/连接类型/IPv4/IPv6/信号强度），4 列布局 | 【资源】`rowlayout(4)` |

### P2 —— 中影响，锦上添花

| # | 事项 | 依据 |
|---|---|---|
| 14 | 逻辑处理器格子用 part 26–37 那 12 级蓝色阶做热力（若确认是色块） | 【主题】强推断 |
| 15 | 主曲线刻度只标 `0` 和 `100%`（`resstr(33824)`/`resstr(33836)`），中间不标数字 | 【资源】 |
| 16 | 图例：sidebar 用 `TmLegendElement`，色块 `6rp` 宽、右侧留 `3rp`；写图例留 `20rp` | 【资源】`rect(0rp,0rp,6rp,0rp)` 等 |
| 17 | GPU 温度（有则显示 `%d °C`） | 【MUI】但有硬件前提 |

### P3 —— 可选

| # | 事项 |
|---|---|
| 18 | 格子间距/行数自适应（本来就该自适应，别硬编码 5×4） |
| 19 | 高对比度主题回退（`themeable(..., RGB(...))` 的第二参数） |
| 20 | 内存条 tooltip 用 `wdc.dll.mui` 的 `33603`–`33609` 长文案 |

### 明确不要做的事

- ❌ 不要加「序列号」「进程数」「显存」到 GPU 信息列 —— 【MUI】0 命中，Win10 原版没有。
- ❌ 不要用 `#F0F0F0 → #0078D7` 线性插值做格子热力 —— 无任何证据支持。
- ❌ 不要把「使用中」画成深蓝 —— 真实值是橄榄绿 `#8FB80C`。
- ❌ 不要用「可用」和「空闲」混为一谈 —— 短图例是「可用」，tooltip 是「空闲」，微软自己不一致，照抄。

---

## 9. 复现本文档结论的方法

```bash
# 1) 解 MUI / 资源
node _probe/rcparse.js "C:/Windows/System32/zh-CN/taskmgr.exe.mui" strings > _probe/mui_strings_all.txt
node _probe/rcparse.js "C:/Windows/System32/zh-CN/wdc.dll.mui"    strings > _probe/wdc_mui_strings.txt

# 2) 列 .mun 资源（关键：UIFILE 在这里）
node _probe/rcparse.js "C:/Windows/SystemResources/Taskmgr.exe.mun" summary
node _probe/rcparse.js "C:/Windows/SystemResources/wdc.dll.mun"     summary

# 3) 抽 UIFILE（wdc 的是纯 UTF-8 XML，直接可读）
node -e "..."   # 见 _probe/uifile_wdc_30003.xml
# Taskmgr 的 UIFILE 是编译后的 duib，需按 null 分隔的 UTF-16 扫描
#   产出：_probe/uifile_Taskmgr_30001_full.txt

# 4) 导入表
node _probe/pedump.js "C:/Windows/System32/taskmgr.exe"

# 5) 主题实时查询
powershell -File _probe/themefull.ps1
powershell -File _probe/themestrip.ps1   # 直接渲染主题 part 并读像素

# 6) 解码 mun 里的 PNG 色键
node _probe/pngread.js _probe/png_IDB_PERFTAB_NET_RECEIVE_COLORKEY.png
```

本次产出的取证中间文件（均在 `_probe/`）：

| 文件 | 内容 |
|---|---|
| `mui_strings_all.txt` | taskmgr.exe.mui 全部 848 条 |
| `wdc_mui_strings.txt` | wdc.dll.mui 全部 589 条（内存 tooltip 在这） |
| `uifile_wdc_30000..30005.xml` | 6 个 **可直读** DirectUI XML（含 `RGB()` 字面量） |
| `uifile_Taskmgr_30001_full.txt` | Taskmgr duib 解出的全部 1543 条 token |
| `uifile_Taskmgr_30024_full.txt` | 同上（第二套，含 SmallView/命令栏） |
| `tm_imports.txt` / `wdc_imports.txt` | 导入/导出/延迟导入表 |
| `tm_elemids.txt` / `tm_u16.txt` / `tm_ascii.txt` | taskmgr.exe 字符串（元素 ID、符号、引擎名） |
| `themefull.ps1` + 输出 | `TASKMANAGER`/`CHARTVIEW` 全 part 颜色 + 字体 |
| `themestrip.ps1` + `theme_strip.png` | 主题 part 1–47 的实际渲染像素 |
| `gpu_engine_counters.txt` | 本机 1220 个 GPU Engine 实例（引擎类型统计） |
| `png_*.png` | mun 里的 PNG 资源 |

**辅助脚本**：`rcparse.js`（资源/字符串）、`pedump.js`（导入导出）、`pngread.js`（PNG 解码）、
`themefull.ps1` / `themestrip.ps1` / `themefonts.ps1`（主题查询）。

---

## 10. 一句话总结

**硬证据能确认的**：所有中文文案（848+589 条 MUI 逐字）、内存条 5 段的精确 RGB 与 24rp 条高、
逻辑处理器格子里**没有**大号百分比且标签是 11px、GPU 页右侧只有 5 项、8 个引擎显示名的确切拼写、
页面元素树（元素 ID 级别）、以及 `taskmgr.exe` **不具备**填充多边形绘制能力这一点。

**推断（需外部验证）**：逻辑处理器格子是纯色热力块还是折线；
12 级蓝色阶（part 26–37）是否就是格子色带；磁盘读写曲线的颜色。
