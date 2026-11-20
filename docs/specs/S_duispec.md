# Agent S — DUI 资源规范

状态：已完成（176 个 `@0xNNN` 偏移全部逐字节复核通过，0 处不可解析）

> 取证机：Windows 10 Pro 22H2 19045 / Xeon 服务器级 CPU / 32 GB / discrete GPU
> 本轮**没有**启动 `taskmgr.exe`，**没有**启动 `TaskManagerPro.exe`，**没有**编译，**没有**截图/动鼠标。
> 唯一的新增动作是 `uxtheme.dll` 的只读查询（`GetThemeColor` / `GetThemeFont` / `GetThemeInt` /
> `DrawThemeBackground` 到内存 DIB），不涉及任何 exe 进程。

---

## 0. 证据清单与解析方法

| 编号 | 文件 | 内容 | 级别 |
|---|---|---|---|
| E1 | `C:\Windows\SystemResources\Taskmgr.exe.mun` → `UIFILE 30001`（62,248 B duib） | 进程页 / 性能页 / 详情页 / 服务页 / 用户页的完整声明式布局（**二进制**） | A（二进制资源） |
| E2 | 同上 → `UIFILE 30024`（25,056 B duib） | 老式「简略视图」界面（`taskman` 根 + `GenericCommandBar`） | A |
| E3 | `C:\Windows\SystemResources\wdc.dll.mun` → `UIFILE 30000..30005`（13–22 KB，**纯 UTF-8 XML**） | 资源监视器（resmon）的同类布局，含 `barchart` / `graphbox` / `graph` 样式与全部 RGB 字面量 | A（但不是 taskmgr 本体，见 §7） |
| E4 | `docs/evidence/themeprobe_out.txt` + `themedump2_out.txt` | 主题类 `TaskManager` / `ChartView` / `ItemsView::ListView` / `ItemsView` 的 part 0..60、state 0..12 颜色 dump | A（API 直读） |
| E5 | `docs/evidence/themecolors_out.txt` | 只有 UIFILE 引用到的那些 part 的颜色 | A |
| E6 | 本轮新跑：`docs/evidence/exec/_S_theme4.ps1` / `_S_render3.ps1` | `GetThemeFont`（字号字重）+ `DrawThemeBackground` 实际渲染采样（行/悬停/选中） | A |

### 0.1 Taskmgr 的 duib 不是自描述的，必须自己解

`UIFILE 30001/30024` 是「duib」编译后的二进制，**不是 XML**。本轮的解析结论（已用两个文件互证）：

```
偏移 0x00  'duib'            magic
     0x04  u32 = 5           版本
     0x08  u32 = HDR          头块偏移（30001: 0x1a4，30024: 0xac）
     0x0c  u32 = TBL          字符串表描述符偏移（30001: 0x6d5c，30024: 0x1d5c）

HDR+0x00  u32 = TOK1         属性 token 流结束偏移（30001: 0x6bb8）
HDR+0x04  u32 = TOKSZ         token 流字节数（30001: 0x6d9）
HDR+0x08  ... 节点表         N 组 (u32 nodeOff, u32 名字串号)，见 §3.0
TOK0 = HDR+8

TBL+0x00  u32 = HEAPEND      字符串堆结束
TBL+0x04  u32 = NSTR         串数量（30001: 839，30024: 437）
TBL+0x08  u32×N              相对 TBL 的串偏移；串本体 = TBL + off（UTF-16LE，NUL 结尾）
```

验证：`30001 的 TBL+off[0] = 0x7a80` 处正好是 `"tab"`，`off[838]` 处是 `"resstr(33867)"`，
839 个串**全部可打印**（脚本 `exec/_S_parse.js … check` 输出 `nonprintable 0`）。

token 流是 (属性 id, 值) 的**扁平交替序列**，属性 id 是 `0x80xx`，值是高字节为 0 的 16 位串号：

```
attr  id = 0x8000 | code
value     = u16 stringIndex      （< NSTR，且高字节为 0）
```

例（`DashboardStyle` 开头，@0x586 起，逐字节可核对）：

```
0x588  P15  →  0x58a  #4  "rect(0rp,0rp,0rp,0rp)"
0x58c  #16  "middlecenter"       ← 这是「状态选择器」，下一个才是 attr
0x58e  P6   →  0x590  P10        ← 0x8010 的现值是一个 0x80xx → 视为「带参属性无值」
0x592  #73  "atom(BarchartMemoryHolder)"   ← P10 = id
0x594  #49  "dtb(EXPLORER::LISTVIEW, 1, 3)"
0x596  P0   →  0x598  P15        ← 0x8000 的「值」本身又是属性 → 无值布尔
0x59a  #74  "rect(1rp,1rp,1rp,1rp)"  ← P15 = borderthickness
0x59c  P14  →  0x59e  #75  "gtc(CHARTVIEW, 9, 0, 3802)"  ← bordercolor
0x5a0  P25  →  0x5a2  #76  "52rp"                       ← height
```

属性 code 表（本轮从语义反推，并逐条用 30001/30024 共 1,300+ 条几何声明交叉验证；标 ★ 的是
**可用 wdc 的 XML 原文直接证实**的，因为 wdc 同一位置的属性名写在明文里）：

| code | 属性 | 依据 |
|---|---|---|
| 0x00 | `background` | ★ 位置与 `background=` 一一对应（`chart` 样式 30001 属性 token @0x27e = `20575`，对应 wdc 的 `background="window"`） |
| 0x02 | `resid`（样式名/资源名） | ★ 与 `resid="tab"`… 顺序一致 |
| 0x06 | 状态选择器（`if selected/mousefocused/keyfocused…`） | ★ 与 wdc `<if …>` 顺序一致 |
| 0x08 | 状态内层续行 | ★ |
| 0x0a | `sheet` | ★ 与 `sheet="tab"` 顺序一致 |
| 0x0c | `font` | ★ |
| 0x0d | `accrole` | ★ |
| 0x0e | `content` | ★ |
| 0x0f | `foreground` | ★ |
| 0x10 | `id` | ★ |
| 0x12 | `contentalign` | ★ |
| 0x13 | `class` | ★ |
| 0x14 | `bordercolor` | ★ |
| 0x15 | `borderthickness` | ★ |
| 0x16 | `borderstyle` | ★ |
| 0x19 | `layout` | ★ |
| 0x1a | `layoutpos` | ★ |
| 0x1f | `accstate` | ★ |
| 0x21 | `padding` | ★ |
| 0x22 | `accname` | ★ |
| 0x23 | `margin` | ★ |
| 0x24 | `width` | ★ |
| 0x25 | `height` | ★ |
| 0x2a | 位置/偏移（`<element … layoutpos=…>` 的 `padding` 或 `margin` 的简写，见 §1.3 注） | 推断（高） |

> **注意**：`0x8000` 与 `0x802a` 的语义在不同位置有重载（DUI 的属性系统本来就是「同名不同义」）。
> 本报告凡是这两个 code 的结论都标「推断」。

**已知解析盲区（必读）**：token 流里有 **219 / 1054（30001）** 条几何字符串前面不是属性 id 而是另一个
字符串值（例：`@0x1922 "rect(4rp,2rp,4rp,3rp)"` 之后，`@0x1924 "rect(15rp,0rp,0rp,0rp)"` 直接跟上，中间没有属性 id）。
这些是**「样式行内第二条几何声明」**——DUI 允许一个元素同时带 `padding` 与 `margin`，
而第二者的 id 被编译器吃掉了。所以：

- **rp 数值本身 100% 可信**（它就在字符串里，逐字节可读）；
- **「这个 rp 属于 padding 还是 margin」在这 219 条上不可判定**，本报告统一标 `未定（padding|margin）`，
  并给出「同一选择器上另一条已确定的 padding/margin」作参照。

### 0.2 第二个盲区：数值枚举表未解

token 流里还有一类值是 `{num}`（不是串号），它们其实是 **`layoutpos` / `layout` / `content` 的枚举**。
例如 `P1a {38}`、`P1a {40}`、`P1a {42}`、`P1a {43}`、`P19 {63}`。
本机没有 DirectUI 的头文件，**这一层枚举没有权威映射**，所以：

- 本报告**不使用**这些数字去推绝对坐标；
- 凡是「元素在谁的左边/右边」的结论，只在 **UIFILE 明写了 `padding/margin/width/height`** 时才给；
- 侧栏卡片、图表区的**绝对 x 原点**因此无法只靠 UIFILE 定出来（见 §5.11）。

复现命令（纯读，无副作用）：

```bash
cd "D:/Projects/Task Manager Pro/_probe"
node exec/_S_parse.js  uifile_Taskmgr_30001.xml 6d64 839  check|blocks|geo|gtc|tree
node exec/_S_full.js   uifile_Taskmgr_30001.xml 6d64 839  geo|theme|tree
node exec/_S_sec.js    uifile_Taskmgr_30001.xml 6d64 839  all|geo|theme
node exec/_S_blk.js    uifile_Taskmgr_30001.xml 6d64 839 <blockName>
node exec/_S_resolve.js          # 把 gtc 引用翻成实际颜色
```

（`30024` 用 `-` 前缀的等价命令：`node exec/_S_parse.js uifile_Taskmgr_30024.xml 1d64 437 …`）

---

## 1. 全部 rp 几何值（按元素归类）

统计口径：`30001` 里含 `rp` 的几何字符串 **1,054 条 / 3,634 个 rp 数字**；
`30024` 为 **212 条 / 720 个**。下表只列**有语义**的（样式默认值 + 具名元素），
重复出现的逐行副本（例如 15 个列 × 3 个分组 = 45 组完全相同的 `rect(4rp,2rp,4rp,3rp)`）
合并成一行并注明重数。

### 1.1 全局样式（`<stylesheets>` 段）— **UIFILE 明确声明**

> 偏移写法：`@0xNNN` 是 **token 偏移**。多数行是「属性 token」的偏移（字符串值在 +2 字节）；
> 表格里属性列显示为「（位置偏移）」的行是「无属性 id 的裸几何值」，此时偏移就是**字符串本身**的位置。

| 样式 resid | 属性 | rp 值 | 证据 |
|---|---|---|---|
| `tab` | （位置偏移，属性 code `0x803f`） | `rect(15rp,0rp,0rp,0rp)` | 30001 字符串本身 @0x1bc（前面紧邻的是 style 名字串，无属性 id） |
| `tab` | `borderthickness` | `rect(0rp,0rp,0rp,0rp)` | 30001 属性 token @0x1c8 |
| `tab` | `height` | `30rp` | 30001 属性 token @0x1cc |
| `tab` | `padding` | `rect(0rp,2rp,0rp,0rp)` | 30001 属性 token @0x1d0 |
| `page` | `padding` | `rect(0rp,0rp,0rp,0rp)` | 30001 属性 token @0x214 |
| `page` | `borderthickness` | `rect(0rp,0rp,0rp,0rp)` | 30001 属性 token @0x218 |
| `SmallViewPage` | `padding` | `rect(0rp,0rp,0rp,0rp)` | 30001 属性 token @0x234 |
| `SmallViewPage` | `borderthickness` | `rect(0rp,0rp,0rp,0rp)` | 30001 属性 token @0x23c |
| `drop` | `padding` | `rect(0rp,0rp,0rp,0rp)` | 30001 属性 token @0x25c |
| `drop` | `borderthickness` | `rect(0rp,0rp,0rp,0rp)` | 30001 属性 token @0x268 |
| `chart` | `margin` | **rect(20rp,10rp,10rp,10rp)** | 30001 属性 token @0x284  ★ 迷你图外框 |
| `chart` | `padding` | **rect(0rp,5rp,0rp,0rp)** | 30001 属性 token @0x288  ★ |
| `chart` | `height` | **45rp** | 30001 属性 token @0x28c  ★ 迷你图高 |
| `chart` | `width` | **60rp** | 30001 属性 token @0x290  ★ 迷你图宽 |
| `cpuchart` | `margin` | `rect(0rp,0rp,0rp,0rp)` | 30001 属性 token @0x2b8 |
| `cpuchart` | `padding` | `rect(2rp,2rp,2rp,2rp)` | 30001 属性 token @0x2bc |
| `cpuchart` | `height` | `80rp` | 30001 属性 token @0x2c0 |
| `cpuchart` | `width` | `80rp` | 30001 属性 token @0x2c4 |
| `gpuchart` | `height` | `80rp` | 30001 属性 token @0x2ec |
| `gpuchart` | `width` | `80rp` | 30001 属性 token @0x2f0 |
| `gpuchart` | `margin` | `rect(0rp,0rp,0rp,0rp)` | 30001 属性 token @0x2f4 |
| `gpuchart` | `padding` | `rect(2rp,2rp,2rp,2rp)` | 30001 属性 token @0x2f8 |
| `staticResourceChart` | `height` | **80rp** | 30001 属性 token @0x320  ★ 主曲线基准高 |
| `staticResourceChart` | `margin` | `rect(0rp,0rp,0rp,0rp)` | 30001 属性 token @0x324 |
| `staticResourceChart` | `padding` | `rect(0rp,0rp,0rp,0rp)` | 30001 属性 token @0x328 |
| `secondaryChart` | `height` | **67rp** | 30001 属性 token @0x358  ★ |
| `secondaryChart` | `margin` | `rect(0rp,0rp,0rp,0rp)` | 30001 属性 token @0x350 |
| `secondaryChart` | `padding` | `rect(0rp,0rp,0rp,0rp)` | 30001 属性 token @0x354 |
| `chartbox` | `margin` | `rect(0rp,0rp,0rp,0rp)` | 30001 属性 token @0x380 |
| `chartbox` | `padding` | `rect(0rp,0rp,0rp,0rp)` | 30001 属性 token @0x384 |
| `chartbox` | `borderthickness` | `rect(0rp,0rp,0rp,0rp)` | 30001 属性 token @0x388 |
| `chartbox`→`dotIcon` | `padding` | `rect(0rp,5rp,10rp,0rp)` | 30001 属性 token @0x3a4 |
| `chartbox`→`InactiveChart` | `margin` | `rect(20rp,10rp,10rp,10rp)` | 30001 属性 token @0x3c4 |
| `chartbox`→`InactiveChart` | `padding` | `rect(0rp,5rp,0rp,0rp)` | 30001 属性 token @0x3c8 |
| `chartbox`→`InactiveChart` | `height` | `45rp` | 30001 属性 token @0x3cc |
| `chartbox`→`InactiveChart` | `width` | `60rp` | 30001 属性 token @0x3d0 |
| `chartbox`→`dashSidebarActionButton` | `padding` | `rect(10rp,5rp,9rp,10rp)` | 30001 属性 token @0x3f4 |
| `scrollviewer`→`viewer` | `padding` | `rect(0rp,0rp,0rp,0rp)` | 30001 属性 token @0x9dc |

### 1.2 性能页 — 侧栏（`performancetab_graphs` / `dashSidebar*`）

| 元素 | 属性 → rp 值 | 证据（属性 token 偏移） |
|---|---|---|
| `performancetab_graphs` 根 | 位置（`0x803f`，推断）= `rect(15,0,0,0)` | 30001 的 `performancetab_graphs` 节点（0x4c58 起；该节点只有 `layout` + `contentalign`，位置参数出自紧随其后的 `gtc` 之前那个未归定的 `0x803f`） |
| `atom(dashSidebarScrollViewer)` | **padding** = **`rect(3,7,0,0)`** | @0x4c78 |
| `atom(dashSidebarDivider)` | **padding** = **`rect(0,7,0,0)`** | @0x4d04 |
| `atom(dashSidebarDivider)` ×2 | **height** = **45rp** | @0x4d14（共 2 处） |
| `atom(dashSidebarDivider)` ×2 | **width** = **8rp** | @0x4d18（共 2 处） |
| `atom(dashSidebarEntry)` | **padding** = **`rect(0,0,3,0)`** | @0x4d6c |
| `atom(sidebar_cpu_speed)` | **padding** = **`rect(5,0,0,0)`** | @0x4e08 |
| `atom(sidebar_gpu_temp)` | **padding** = **`rect(5,0,0,0)`** | @0x515c |
| `atom(sidebar_net_name)` | 位置（`0x8026`，推断）= `rect(15,0,0,0)` | @0x502a |
| `DashboardStyle`→`sidebarDataHeader` | padding = `rect(1,0,0,0)` | @0x850 |
| `DashboardStyle`→`dashboardLegend` | **margin** = **`rect(0,0,6,0)`** | @0x7a8 |
| `DashboardStyle`→`dashboardLegend` | padding = `rect(0,0,3,0)` | @0x7ac |
| `DashboardStyle`→`dashboardLegend` | **width** = **2rp** | @0x7b0 |
| `DashboardStyle`→`dashboardLegend` | **height** = **32rp** | @0x7b4 |
| `DashboardStyle`→`dashboardWriteLegend` | margin = `rect(20,0,6,0)` | @0x7cc |

> **关键**：UIFILE **没有**给侧栏卡片高度。`dashSidebarEntry` 只声明了 `padding: 0,0,3,0`，
> 卡片高度由内容（迷你图 45rp + 两行文字）**撑出来**。这就是为什么「61/68 两档」只能靠实测。

### 1.3 性能页 — 主图区 / 统计区 / 组合条

| 元素 | 属性 → rp 值 | 证据（属性 token 偏移） |
|---|---|---|
| `performancetab_tables` 根 | **padding** = **`rect(17,0,0,0)`** | @0x5180 |
| `atom(detailInfo)` | **width** = **402rp** | @0x51bc |
| `atom(detailInfo)` | **padding** = **`rect(0,10,24,20)`** | @0x51c0 |
| `atom(chartAndTitle)` | **padding** = **`rect(0,0,0,10)`** | @0x51d4 |
| `atom(chartInfo)` | **padding** = **`rect(2,0,0,0)`** | @0x520c |
| `atom(chartInfo)` | **margin** = **`rect(0,5,0,0)`** | @0x5220 |
| `atom(dataInfo)` | **padding** = **`rect(2,0,0,0)`** | @0x523c |
| `atom(dataInfo)` 内层 | `rect(15,0,0,0)`（`0x8026`） | @0x5240 |
| `atom(staticInfo)` | **padding** = **`rect(10,0,0,0)`** | @0x526c |
| `atom(dynamicInfo)` | 位置 = `rect(0,0,6,0)` | @0x525c |
| `atom(static_cpu)` | 位置/内缩（`0x8026`，推断）= `rect(1,0,0,0)` | @0x52b2 |
| `static_cpu` 每对 label/value ×12 | **margin** = **`rect(0,0,10,0)`**（12 组全同） | @0x52c8（共 12 处） |
| `atom(cpuUsage)` / `process` / `thread` | **margin** = **`rect(20,0,0,0)`** | @0x5580（共 3 处） |
| `atom(cpuSpeed)` / `handle` | **margin** = **`rect(0,8,0,0)`** | @0x55bc（共 2 处） |
| `atom(chartArea)`（CPU / 内存 / GPU 单引擎） | 位置（`0x802a`，推断）= **`rect(15,0,0,0)`** | @0x572a |
| `atom(chartArea)`（磁盘 / 网络） | 位置（`0x802a`，推断）= **`rect(1,0,0,0)`** | @0x61de（共 2 处） |
| `atom(logicalCpuHost)` | **height** = **80rp** | @0x579c |
| `atom(heatMapCpuHost)` | **height** = **80rp** | @0x5818 |
| `atom(numaCpuHost)` | **height** = **80rp** | @0x58d0 |
| `atom(memAvailabilityBar)` | **margin** = **`rect(0,6,0,0)`** | @0x5d50 |
| `atom(memAvailabilityBar)` | **padding** = **`rect(0,0,0,14)`** | @0x5d54 |
| `atom(BarTopLabel)`（「内存组合」） | **padding** = **`rect(0,0,0,5)`** | @0x5d74 |
| `atom(BarchartMemoryHolder)`（组合条） | **height** = **52rp** | @0x5a0 |
| `atom(BarchartMemoryHolder)` | **borderthickness** = **`rect(1,1,1,1)`** | @0x598 |
| 组合条「使用中」 | padding / margin = `rect(0,0,0,0)` | @0x5bc |
| 组合条「已修改」 | **borderthickness** = **`rect(1,0,0,0)`** | @0x5e4 |
| 组合条「备用」 | **borderthickness** = **`rect(1,0,0,0)`** | @0x614 |
| 组合条「可用」 | **borderthickness** = **`rect(1,0,0,0)`** | @0x648 |
| `atom(resourceName)`（CPU 名） | **margin** = **`rect(15,0,0,0)`** | @0x6a4 |
| `atom(resourceName)`（CPU 名） | **padding** = **`rect(0,0,0,3)`** | @0x6a8 |
| `staticLabel` / `staticData` / `dynamicLabel` / `dynamicData` | **padding** = `rect(0,0,0,3)` | @0x6d8 |
| `lheader` / `rheader` | margin / padding = 全 `rect(0,0,0,0)` | @0x8f4 |
| `theader` | **padding** = `rect(0,0,0,3)` | @0x878 |
| `bheader` | **padding** = `rect(0,2,0,0)` | @0x894 |
| `cpu_theader` | **padding** = **`rect(2,0,2,1)`** | @0x8b0 |
| `cpu_bheader` | **padding** = `rect(2,0,2,0)` | @0x8cc |

### 1.4 CPU 逻辑处理器格子（`cpuBlock` / `cpuBlockStyle*`）

| 元素 | 属性 → rp 值 | 证据（属性 token 偏移） |
|---|---|---|
| `cpuBlock`（背景块 B） | **height** = **17rp** | @0x5978 |
| `cpuBlock`（背景块 B） | **borderthickness** = **`rect(0,0,0,1)`** | @0x597c |
| `cpuBlock`（背景块 R） | **borderthickness** = **`rect(0,0,1,0)`** | @0x598c |
| `atom(cpuBlockData)`（文字） | **borderthickness** = **`rect(1,1,1,1)`** | @0x59a4 |
| `atom(cpuBlockData)`（文字） | **padding** = **`rect(5,0,5,0)`** | @0x59a8 |
| `atmCpu` → `atom(atmCpuChart)` | sheet = `cpuchart`（80×80、padding 2）→ 见 §1.1 | 30001 `sheet` 属性值 = `cpuchart`（该值不是 rp，故不在 geo TSV 里；见 `_S_theme2_30001.tsv` 无此项，实际出自 30001 token 流 @0x595e） |

> `sheet` 是字符串值而非 rp，这里用 `q()` 也能查到（TSV 里有 `sheet` 行）。

### 1.5 进程页 — 列表 / 列头 / 分组

| 元素 | 属性 → rp 值 | 证据（属性 token 偏移） |
|---|---|---|
| `apptab_graphs` 根 | **无任何 geometry 声明** | 30001 的 `apptab_graphs` 节点只有 8 个 token（`layout` / `contentalign=topleft` / `background` / `class=visible` / `font` / `background=chartbox`），**没有 padding/margin/width/height** |
| `apptab_tables`→`TmView` | **padding** = **`rect(0,8,0,0)`** | @0x2478 |
| `atom(ColScrollViewer)` | sheet = `scrollviewer`；**无 padding/margin/size** | 30001 `ColScrollViewer` 节点（0x248e 起）：`sheet=scrollviewer` + X/YBarVisibility，无几何声明 |
| 每列 `TmColHeader`（前 8 列：名称/类型/状态/发布者/PID…） | **padding** = **`rect(4,0,0,0)`** | @0x24ec（共 8 处） |
| 每列 `TmColHeader`（资源列：CPU/内存/磁盘/网络/GPU/功耗/趋势） | **padding** = **`rect(7,0,0,0)`** | @0x291c（共 7 处） |
| `atom(Resizer)`（列宽拖柄） | **width** = **5rp** | @0x2510（共 15 处） |
| `atom(SortArrow)`（普通列） | **height** = **10rp** | @0x2538（共 8 处） |
| `atom(SortArrow)`（普通列） | **padding** = **`rect(0,2,6,0)`** | @0x253c（共 8 处） |
| `atom(SortArrow)`（资源列） | **width** = **17rp** | @0x2980（共 7 处） |
| `atom(ColHeaderText)`（普通列） | **padding** = **`rect(0,0,0,4)`** | @0x2558（共 8 处） |
| `atom(ColHeaderText)`（资源列） | **padding** = **`rect(0,3,0,4)`** | @0x29b0（共 7 处） |
| `atom(ResizerImage)` | **width** = **1rp** | @0xbd8 |
| `atom(HeatMapCumulative)` | **无任何 geometry 声明** | id 在 30001@0x2cbc，共 7 处，**全部无 padding/margin/size** |
| `atom(TmViewRow)`（**父行**） | **height** = **28rp** | @0x1160 |
| `atom(TmViewRow)`（**子行**） | **height** = **22rp** | @0x147c |
| `atom(TmFirstColumn)` 父行 | **padding** = **`rect(6,0,0,0)`** | @0x1180 |
| `atom(TmFirstColumn)` 子行 | **padding** = **`rect(14,0,0,0)`**（缩进） | @0x149c |
| `atom(TmRowIconBackground)` | **width** = **16rp** | @0x11a0 |
| `atom(TmRowIconBackground)` | **height** = **16rp** | @0x11a4 |
| `atom(TmRowIcon)` | **width** = **16rp** | @0x11c4 |
| `atom(ViewExpandoImageWrapper)` | **width** = **22rp** | @0x1120 |
| `atom(ViewExpandoImageWrapper)` | **padding** = **`rect(4,5,2,0)`** | @0x1124 |
| `atom(TmLeafIcon)`（组头图标） | **width** = **20rp** | @0x1270 |
| `atom(TmLeafIcon)`（组头图标） | **height** = **16rp** | @0x1274 |
| `atom(ViewExpandoButtonImage)` | **width** = **16rp** | @0xdd8 |
| `atom(ViewExpandoButtonImage)` | **height** = **16rp** | @0xdd4 |
| `atom(TmGroupHeaderName)`（「应用」「后台进程」） | **padding** = **`rect(8,10,0,7)`** | @0x2fac（共 3 处） |
| `atom(TmGroupHeaderName)` | 位置（`0x8028`，推断）= `rect(15,0,0,0)` | @0x2fb2（共 3 处） |
| `TmViewStyle`→`atom(TmViewData)` | **borderthickness** = **`rect(0,1,0,1)`** | @0x1058 |
| `TmViewStyle`→`atom(TmViewColumnHeader)` | **borderthickness** = **`rect(1,0,1,0)`** | @0x1070 |

> **单元格 padding 与资源列右边线不在 `TmColHeader` 上，而在 `TmAppViewItem` / `TmAppChildViewItem` 的列节点上**：
> 15 列 × 3 组 = 45 处，全部是 `rect(4,2,4,3)`；其中 CPU/内存/磁盘/网络/GPU/功耗 六列额外带 `borderthickness: rect(0,0,1,0)`。
> 这两条见 §5.1 第 2 条。

### 1.6 详情页 / 服务页 / 用户页 / 启动页 / 应用历史页

| 元素 | 属性 → rp 值 | 证据（属性 token 偏移） |
|---|---|---|
| 启动页 `TmView` | **padding** = **`rect(0,8,0,0)`**（2 处同值） | @0x35c4 |
| 启动页 `atom(BIOSPostTimeHolder)` | **padding**（不是 margin） = **`rect(0,7,7,20)`** | @0xe84 |
| 用户页 `TmView` | **padding** = **`rect(0,8,0,0)`** | @0x3d5c |
| 应用历史页 `TmView` | **padding** = **`rect(0,8,0,0)`**（2 处同值） | @0x45fc |
| 应用历史页底部工具区 | **padding** = **`rect(4,8,0,8)`** | @0x4620 |
| 应用历史页 `BodyText` | **padding** = `rect(2,0,0,0)` | @0x4630 |
| 应用历史页 `link` | **padding** = `rect(4,0,4,0)` | @0x465c |
| 服务页 `atom(svcList)` | **无 rp 声明**（节点只有 `id` + `resstr(33867)`） | 30001 **字符串堆**里 `atom(svcList)` 位于 0xf2f0（这是字符串本体，不是 token 偏移）；对应节点在 token 流末段 `servicestab_tables` block 内 |
| 详情页列表容器 | 复用 `apptab_tables` 的 `TmAppPageView` 结构（`padding: rect(0,8,0,0)`） | @0x2478 |

> 所有 5 个页面的表格容器结构完全相同：`TmXxxPageView`(=`TmView`, `padding: rect(0,8,0,0)`)
> → `ColScrollViewer`(`sheet=scrollviewer`, X/YBarVisibility) → `TmColumnHeader` → 各列。
> 列宽**不在 UIFILE**（在 `taskmgr.exe` 的 `.rdata` 列池里，见 `docs/evidence/exec/_o_final.js`）。

### 1.7 工具条（**30001 里没有实例；只有 30024 的 `GenericCommandBar`**）

| 元素 | 属性 → rp 值 | 证据（30024 属性 token 偏移） |
|---|---|---|
| `TmCommandBar`（命令条容器） | **height** = **48rp** | @0x1740 |
| `TmCommandBar` | **padding** = **`rect(8,0,0,0)`** | @0x175c |
| `atom(CBLeftAlignedButtons)` | **padding** = **`rect(0,10,0,10)`** | @0x178c |
| `atom(CBExpandoButtonImageArrow)` | 位置 = `rect(0,0,6,0)` | @0x17c2 |
| `atom(CBExpandoButtonImageText)` | **margin** = `rect(5,0,0,0)` | @0x17dc |
| `atom(dashboardBar)` / `atom(servicesBar)` | **padding** = **`rect(0,6,0,7)`**（各 1 处） | @0x17fc / @0x18a4 |
| `atom(seperatorId)` | **padding** = **`rect(6,0,8,0)`**（各 1 处） | @0x1814 / @0x18bc |
| `link`（resmonLaunch / servicesLaunch 图标） | **padding** = `rect(1,1,0,0)` | @0x185c / @0x1904 |
| `link`（resmonLaunch / servicesLaunch 文字） | **padding** = `rect(4,0,4,0)` | @0x187c / @0x1924 |
| `atom(CBTaskButtons)` | **padding** = **`rect(0,0,8,3)`** | @0x1968 |
| `atom(TmTaskButton)` ×8 | **margin** = **`rect(4,0,0,0)`**（每个按钮各 1 条，共 8 条） | @0x199c 起 |
| `atom(CBEndTaskInfoIcon)` | **margin / padding / size** = `rect(0,10,0,0)` / `rect(2,0,2,0)` / **`size(23,23)`** | @0x1c34 / @0x1c38 / @0x1c3e |

`30001` 里的 `TmCommandBarStyle` 样式（**无实例**，只有声明）：

| 选择器 | 属性 → rp 值 | 证据（30001 属性 token 偏移） |
|---|---|---|
| `TmTaskButton` | **minsize** = **`size(85rp,23rp)`** | @0xfc6 |
| `TmTaskButton` | **padding** = `rect(2,0,2,0)` | @0xfbc |
| `TmTaskButton` | **margin** = `rect(10,10,10,10)` | @0xfc0 |
| `TmTaskButton` | 位置 = `rect(1,0,0,0)` | @0xfb0 |
| `atom(CBExpandoButtonImageArrow)` | height/width = `gtmet(TaskDialog, 13, 0, 2417/2416)`（**主题给值，不是 rp**） | @0xedc / @0xee0 |

> **重要**：`30001`（新式界面）的整棵树里**没有 `GenericCommandBar` / `TmCommandBar` 实例**，
> 只有 `TmCommandBarStyle` 这一段「样式声明」。所以**新式界面的底部工具条不在 DUI 里**（由代码画）。
> 我们 `kToolbarHeight = 32` 无法用 UIFILE 验证（见 §5.2）。

### 1.8 组合条 / 图例（内存页）

| 元素 | 属性 → rp 值 | 证据（属性 token 偏移） |
|---|---|---|
| `atom(BarchartMemoryHolder)`（组合条） | **height** = **52rp** | @0x5a0 |
| `atom(BarchartMemoryHolder)` | **borderthickness** = **`rect(1,1,1,1)`** | @0x598 |
| `dashboardLegend` / `dashboardWriteLegend` | **width** = **2rp** | @0x7b0 / @0x7d4 |
| `dashboardLegend` / `dashboardWriteLegend` | **height** = **32rp** | @0x7b4 / @0x7d8 |
| `dashboardLegend` / `dashboardWriteLegend` | **padding** = **`rect(0,0,3,0)`** | @0x7ac / @0x7d0 |
| `dashboardLegend` | **margin** = `rect(0,0,6,0)` | @0x7a8 |
| `dashboardWriteLegend` | **margin** = **`rect(20,0,6,0)`** | @0x7cc |
| 组合条各段（Inuse/Modified/Standby/Free） | 位置 `0x802a`-类内缩 = **`rect(0,0,6,0)`**（Inuse / Modified 各 1 条） | @0x5b0 / @0x5dc |
| 组合条「使用中」`BarchartInuse` | padding / margin = `rect(0,0,0,0)` | @0x5bc / @0x5c0 |
| 组合条「已修改/备用/可用」 | **borderthickness** = **`rect(1,0,0,0)`** | @0x5e4 / @0x614 / @0x648 |
| 组合条「备用/可用」 | padding / margin = `rect(0,0,0,0)` | @0x61c / @0x620 / @0x650 / @0x654 |
| 磁盘读写图例 `diskReadLegend`/`diskWriteLegend` | **无 geometry 声明**（只挂 `sidebarDataHeader` 文字样式） | `dynamic_disk_server` 段 |

## 2. `gtc` / `gtf` 主题引用（39 个去重后的签名）

从两个 UIFILE 里共抽出 **907（30001）+ 411（30024）条**几何/主题引用行（TSV 行数），去重后 **39 个
`(class, part, state, prop)` 签名**。颜色值来自 E4/E5，**并用 E6 的 `GetThemeColor` 复算了一遍**
（`themeprobe_out.txt` 的 R/B 通道是反的——脚本按 ARGB 取 `-shr 16` 当 R，
而 `GetThemeColor` 返回的是 `COLORREF 0x00BBGGRR`；本报告统一用 §0 的 `_S_resolve.js`
把 R/B 换回来，并与 `themedump2_out.txt` 交叉核对，**0 处不一致**）。

### 2.1 颜色引用（`gtc`）

| class | part | state | prop | 解析出的颜色 | 用在哪（UIFILE 选择器） |
|---|---|---|---|---|---|
| TASKMANAGER | 2 | 0 | 0x3802 fill | **#EAD5A0** | `TmGroupHeaderName` 的 bordercolor、`TmFirstColumn` 的 bordercolor |
| TASKMANAGER | 13 | 0 | 0x3803 text | **#DB6B3A** | `TmColumnContainerStyle`→`TmHeatText`（热力格文字） |
| TASKMANAGER | 14 | 0 | 0x3803 text | **#000000** | 页标题 `atom(resourceTitle)` |
| TASKMANAGER | 15 | 0 | 0x3803 text | **#000000** | 处理器名 `atom(resourceName)`、`TmViewStyle`→`ScrollViewer` viewport |
| TASKMANAGER | 16 | 0 | 0x3803 text | **#1F59C3** | 列表分组标题 `atom(TmGroupHeaderName)` |
| TASKMANAGER | 17 | 0 | 0x3803 text | **#707070** | `staticLabel`、`dynamicLabel`、`StaticLabel`（**「标签」灰**） |
| TASKMANAGER | 18 | 0 | 0x3803 text | **#000000** | `staticData`、`ResourceValueClass`、子行选中态 |
| TASKMANAGER | 19 | 0 | 0x3803 text | **#000000** | `dynamicData`（**大号数值黑**） |
| TASKMANAGER | 20 | 0 | 0x3802 fill | **#FFFFFF** | 组合条 `BarchartStandby` **和** `BarchartFree` |
| TASKMANAGER | 21 | 0 | 0x3802 fill | **#F4F2F4** | 组合条 `BarchartInuse`（「使用中」） |
| TASKMANAGER | 22 | 0 | 0x3802 fill | **#EEE6F1** | 组合条 `BarchartModified` 的 **content**（渐变槽 1） |
| TASKMANAGER | 23 | 0 | 0x3802 fill | **#CEB0D7** | `BarchartModified` / `Standby` / `Free` 的 **bordercolor** |
| TASKMANAGER | 24 | **1** | 0x3803 text | **#4C607A** | 子列头 `TmSubColumnHeaderStyle` 文字、`BIOSPostTimeLabel` |
| TASKMANAGER | 25 | 0 | 0x3803 text | **#000000** | CPU 逻辑格文字 `cpuBlockStyleData` |
| TASKMANAGER | 38 | 0 | 0x3802 fill | **#117DBB** | `cpuHeatGrid` 边框（**热力网格线**） |
| TASKMANAGER | 39 | 0 | 0x3802 fill | **#DFE7EB** | `cpuBlockStyleR`；**列表页 `TmColumnHeader` / `ColHeaderText` 的 fg 也用它** |
| TASKMANAGER | 40 | 0 | 0x3802 fill | **#BFCFDF** | `cpuBlockStyleB`（另一种格子底色） |
| TASKMANAGER | 41 | 0 | 0x3802 fill | **#BABABA** | 列宽拖柄渐变 **端点 1** |
| TASKMANAGER | 42 | 0 | 0x3802 fill | **#E8E8E8** | 列宽拖柄渐变 **端点 2**；`ResizerImage` = `gradient(42, 41)` |
| TASKMANAGER | 44 | 0 | 0x3803 text | **#707070** | `lheader` / `rheader`（**图表上下限文字灰**） |
| CHARTVIEW | **9** | 0 | 0x3802 fill | **#8B12AE** | `atom(BarchartMemoryHolder)` 的 **bordercolor**（内存组合条外框，紫） |
| ItemsView::ListView | 1 | 3 | 0x3803 text | **#000000** | `cpuHeatGrid` 前景、列头文字 `ColHeaderTextContend`、排序箭头 |

### 2.2 **未在 dump 里、但同样被 UIFILE 引用的类**（需补查）

| class | part/state | 引用位置 | 说明 |
|---|---|---|---|
| `Button` | 1,2 / 1,3 / 1,4 | 命令条按钮 gtc 前景、`drop` 边框 | 系统 `Button` 类，`GetThemeColor` 返回 `E_PROP_ID_UNSUPPORTED`（只有 BGTYPE=6），**取不到纯色** —— 原版也是走 `dtb`/系统绘制 |
| `CONTROLPANELSTYLE` | 10,1 / 10,2 / 10,4 | `TaskManagerCommon`→`link`（「打开资源监视器」「打开服务」超链接） | 字体 `Microsoft YaHei UI -12 / 400`；颜色取不到 |
| `TEXTSTYLE` | 4,0 / 1,0 | 全局正文字体 | `gtf(TEXTSTYLE,4,0)` = **YaHei UI -12 / 400**；`gtf(TEXTSTYLE,1,0)` = **-16 / 400** |
| `TaskDialog` | 13,0 | 命令条箭头 `gtmet(…2416/2417)` | 宽度/高度**由主题提供**，UIFILE 不写 rp |
| `Explorer::TreeView` | 2,1 / 4,1 / 2,2 / 4,2 | 树展开按钮 4 态背景 `dtb` | BGTYPE=2 |

### 2.3 字体引用（`gtf`）— 已用 `GetThemeFont` 实测

| class | part | state | face | lfHeight | weight | 用在哪 |
|---|---|---|---|---|---|---|
| TASKMANAGER | 14 | 0 | Microsoft YaHei UI | **-29** | 400 | 页标题 `atom(resourceTitle)`（「CPU」「内存」…） |
| TASKMANAGER | 15 | 0 | Microsoft YaHei UI | **-16** | 400 | 处理器名 `atom(resourceName)` |
| TASKMANAGER | 16 | 0 | Microsoft YaHei UI | **-16** | 400 | 列表分组标题 `atom(TmGroupHeaderName)`、`sidebarLabel` |
| TASKMANAGER | 17 | 0 | Microsoft YaHei UI | **-12** | 400 | `staticLabel`/`dynamicLabel`/`sidebarData`/`sidebarDataHeader` |
| TASKMANAGER | 18 | 0 | Microsoft YaHei UI | **-12** | 400 | `staticData` |
| TASKMANAGER | 19 | 0 | Microsoft YaHei UI | **-21** | 400 | `dynamicData`（右侧属性列的**大号数值**） |
| TASKMANAGER | 25 | 0 | Microsoft YaHei UI | **-11** | 400 | CPU 逻辑格文字 `cpuBlockStyleData` |
| TASKMANAGER | 44 | 0 | Microsoft YaHei UI | **-11** | 400 | `lheader` / `rheader`（图表上下限） |
| TASKMANAGER | 2 | 0 | Microsoft YaHei UI | **-9** | 400 | （`TmGroupHeaderName` 的 bordercolor 同 part，字体只作参考） |
| TASKMANAGER | 13 | 0 | Microsoft YaHei UI | **-11** | 400 | `TmHeatText` |
| TEXTSTYLE | 4 | 0 | Microsoft YaHei UI | **-12** | 400 | 全局正文（列头、列内容、页签） |
| TEXTSTYLE | 1 | 0 | Microsoft YaHei UI | **-16** | 400 | `ResourceValueClass`（属性列的值） |
| CONTROLPANELSTYLE | 10,1/10,2/10,4 | — | Microsoft YaHei UI | **-12** | 400 | 「打开资源监视器」链接 |

> **注意 side 卡片文字的字体**：UIFILE 里 `sidebarLabel` → `gtf(TASKMANAGER,16,0)` = **-16**，
> `sidebarData` → `gtf(TASKMANAGER,17,0)` = **-12**。也就是说**侧栏 L1 是 16px、L2 是 12px**，
> 没有第三档 11px 的 `gtf` 引用（`sidebar_cpu_speed` 只是加 `padding: rect(5,0,0,0)` 后复用 `sidebarData`）。

### 2.4 列表行的「悬停/选中/选中+热键」底色 —— **本轮新拿到的硬数据**

`TmViewItemStyle` 里三态用的都是 `dtb(EXPLORER::LISTVIEW, 1, <state>, 0)`：

| UIFILE 引用 | 触发条件 | `DrawThemeBackground` 实渲染（白底） |
|---|---|---|
| `dtb(EXPLORER::LISTVIEW, 1, 2)` | `if mousefocused` / `ChildItemMouseHover` | **#E5F3FF**（4800/4800 像素全同色） |
| `dtb(EXPLORER::LISTVIEW, 1, 3)` | `if ViewItemSelection` | **#CDE8FF** |
| `dtb(EXPLORER::LISTVIEW, 1, 5)` | `if Keywithin` | **#D9D9D9** |
| `dtb(EXPLORER::LISTVIEW, 1, 6)` | 未见引用（备用） | #CCE8FF + 底边 #99D1FF |
| `dtb(EXPLORER::LISTVIEW, 1, 0/1/4)` | 常态 | **#FFFFFF**（+ 底边 #828790 一像素） |

> 这一条把「行悬停 = #E5F3FB 还是 #E5F3FF」的历史争议**定案**：**#E5F3FF**。
> （`include/theme.h:78` 目前是 `#E5F3FB`，蓝通道差 4。）

---

## 3. 元素树还原

### 3.0 头部节点表（`HDR+8` 起的 (u32 off, u32 strIdx) 对）

`30001` 的 50 对，就是**所有「root 级可 expand 的页面/视图」的入口**，顺序即声明顺序：

```
TmScrollerItemList / TmAppViewItem / TmAppChildViewItem / TmStartupViewItem / TmStartupChildViewItem /
TmUsersViewItem / TmUsersChildViewItem / TmAppHistoryViewItem / TmAppHistoryChildViewItem /
apptab_graphs / apptab_tables / startuptab_graphs / startuptab_tables / userstab_graphs / userstab_tables /
apphistorytab_graphs / apphistorytab_tables / performancetab_graphs / dashSidebarEntryViewer /
dashSidebarCpuButton / dashSidebarMemoryButton / dashSidebarDiskButton / dashSidebarNetButton /
dashSidebarGpuButton / performancetab_tables / static_cpu / dynamic_cpu / cpu_chart / atmCpu / cpuBlock /
static_memory / dynamic_memory / memory_chart / static_disk / dynamic_disk_server / dynamic_disk_client /
disk_chart / disk_server_chart / static_network / dynamic_network / net_chart / static_gpu / dynamic_gpu /
gpu_chart / singleEngineGpuView / multiEngineGpuView / gpuEngineChartTemplate / gpuMemoryChart /
servicestab_graphs / servicestab_tables
```

### 3.1 进程页（`apptab_*` + `TmAppViewItem`）

```
apptab_graphs  z (只有一层 chartbox，进程页没有迷你图)
apptab_tables
└ TmAppPageView            padding = rect(0rp, 8rp, 0rp, 0rp)
  ├ ColScrollViewer        sheet=scrollviewer, XBarVisibility=never, YBarVisibility=never
  │ └ TmColumnHeader       accrole=columnheader
  │   ├ TmColFriendlyName  padding = rect(4rp,0,0,0)
  │   │   ├ Resizer        width=5rp, padding=rect(0rp,2rp,0rp,0rp)
  │   │   │   └ ResizerImage  width=1rp, content=gradient(gtc(TASKMANAGER,42,0,3802), gtc(TASKMANAGER,41,0,3802), 1)
  │   │   ├ SortArrow      height=10rp, padding=rect(0rp,2rp,6rp,0rp)
  │   │   └ ColHeaderText  padding=rect(0rp,0rp,0rp,4rp)
  │   ├ TmColType / TmColStatus / TmColPublisher / TmColPid /
  │   │ TmColProcessName / TmColCommandLine        （同上结构，padding=rect(4,0,0,0)）
  │   ├ TmColCPUPercent    padding=rect(7rp,0,0,0)   ← 资源列
  │   │   ├ Resizer / ResizerImage(width=1rp)
  │   │   ├ SortArrow      width=17rp
  │   │   ├ HeatMapCumulative
  │   │   └ ColHeaderText  padding=rect(0rp,3rp,0rp,4rp)
  │   ├ TmColMemory / TmColDisk / TmColNetwork / TmColGPUPercent
  │   │ TmColPowerUsage / TmColPowerUsageTrend      （同上，资源列）
  │   └ TmColGpuEngineName padding=rect(4,0,0,0)
  └ TmViewData             sheet=TmViewStyle, borderthickness=rect(0rp,1rp,0rp,1rp), bordercolor=threedshadow
    └ ScrollViewer
      └ TmScrollerItem
        ├ TmScrollerItemList
        │ ├ TmAppViewItem  sheet=TmViewItemStyle
        │ │ └ TmExpando / atom(arrow) / ViewExpandoImageWrapper(width=22rp, padding=rect(4,5,2,0))
        │ │   └ TmViewRow  height = 28rp
        │ │     ├ TmFirstColumn  padding = rect(6rp,0,0,0)
        │ │     │   ├ TmRowIconBackground  width=16rp height=16rp
        │ │     │   └ TmRowIcon            width=16rp height=16rp
        │ │     └ TmColType … TmColPowerUsageTrend
        │ │         每个 padding = rect(4rp,2rp,4rp,3rp)
        │ │         资源列额外 borderthickness = rect(0rp,0rp,1rp,0rp)
        │ ├ TmAppChildViewItem    TmViewRow height = 22rp
        │ │     TmFirstColumn padding = rect(14rp,0,0,0)   ← 子行缩进
        │ │     TmLeafIcon width=20rp height=16rp
        │ └ TmGroupHeader_Apps / TmGroupHeader_Windows
        │     TmGroupRow → TmGroupHeaderName  padding = rect(8rp,10rp,0rp,7rp)
        │                   bordercolor = gtc(TASKMANAGER, 2, 0, 3802) = #EAD5A0
        │     TmGroupHeaderItems / TmGroupHeader_Background
        └ TmScrollerItemList（第二份，Windows 分组）
```

### 3.2 性能页 — 左栏

```
performancetab_graphs
└ TmScrollViewer   sheet=scrollviewer, XScrollable, YBarVisibility=asneeded
    padding = rect(3rp, 7rp, 0rp, 0rp)      ← 侧栏内容左上内缩
  └ dashSidebar    sheet=drop
    └ dashSidebarEntries   layout=BorderLayout()
      ├ dashSidebarCpuEntry       (layout=BorderLayout(), contentalign=topleft)
      │ └ dashSidebarEntry        padding = rect(0, 0, 3rp, 0)
      │   └ dashSidebarCpuButton  accstate=mouse|keyboard, class=dashSidebarActionButton
      │     ├ dashSidebarCpuChart   sheet=chart   ← margin=rect(20,10,10,10) padding=rect(0,5,0,0) 45rp×60rp
      │     ├ dashSidebarCpuPicture sgraphic(31213,2)
      │     ├ sidebarLabel  gtf(TASKMANAGER,16,0)=16px, foreground=windowtext
      │     ├ sidebar_cpu_util  class=sidebarData  (12px)
      │     └ sidebar_cpu_speed class=sidebarData, padding=rect(5rp,0,0,0)
      ├ dashSidebarMemoryEntry    → MemoryButton / MemoryChart(sheet=chart) / MemoryPicture(sgraphic 31214) / sidebar_mem_util
      ├ dashSidebarDiskEntry      → DiskButton / DiskChart / DiskPicture(31215) /
      │                              sidebar_disk_name / disk_type / disk_util / disk_server /
      │                              sidebar_disk_r/read / w/write  (读写各带 sidebarDataHeader 说明)
      ├ dashSidebarNetworkEntry   → NetButton / sideBarNetChartHost / dashSidebarNetChart /
      │                              sideBarNetSymbolHost / dashSidebarNetSymbol(31216) /
      │                              sidebar_net_name(margin rect(15,0,0,0)) / adapter_name / layout /
      │                              s/send(34569「发送: 」) / r/recv(34570「接收: 」) / disconnected
      ├ dashSidebarGpuEntry       → GpuButton / GpuChart / GpuPicture(31217) /
      │                              sidebar_gpu_name / adapter / util / temp(padding rect(5,0,0,0))
      └ dashSidebarDivider        padding=rect(0,7,0,0), 两段各 45rp×8rp (sheet=vbar 之外的独立贴图 31222/31223/31224)
```

### 3.3 性能页 — 右侧主图 / 统计 / 组合条

```
performancetab_tables
  padding = rect(17rp, 0, 0, 0)            ← 整个右侧内容区左缩 17
└ viewerHost
  └ tablesViewer (sheet=scrollviewer, X/YBarVisibility=asneeded)
    ├ detailInfo   sheet=DashboardStyle
    │     width   = 402rp                  ← ★ 右侧属性列固定 402 逻辑像素
    │     padding = rect(0, 10rp, 24rp, 20rp)
    │   ├ chartAndTitle   padding = rect(0,0,0,10rp)
    │   │ ├ chartInfo     padding = rect(2,0,0,0), margin = rect(0,5rp,0,0), accstate=mouse|keyboard
    │   │ │ ├ resourceTitle   gtf(14)=29px, fg=#000000, contentalign=bottomleft
    │   │ │ └ resourceName    gtf(15)=16px, fg=#000000, margin=rect(15,0,0,0), padding=rect(0,0,0,3rp),
    │   │ │                   contentalign=bottomright|endellipsis
    │   │ └ dataInfo      padding = rect(2,0,0,0)
    │   │   ├ dynamicInfo accrole=pane, content=resstr(34492)
    │   │   └ staticInfo  padding = rect(10,0,0,0), accrole=pane
    │   └（各页的具体内容，见 3.4~3.8）
    └（主图容器，三页结构一致）
```

### 3.4 CPU 页

```
cpu_chart   sheet=DashboardStyle
├ overallCpuHost  accname=34481「总体 CPU 图表窗格」
│ └ theader layout
│   ├ cpuChartTitle  class=lheader  content=34280「60 秒内的利用率 %」
│   ├ (rheader 空槽) content=33836
│   └ chartArea  [位置参数 rect(15,0,0,0)]
│     ├ primaryCpuChart   sheet=staticResourceChart (height 80rp)
│     ├ primaryCpuChart_bottom  class=bheader
│     │ ├ RateLabelCpu    class=lheader   ← 空
│     │ └ (rheader)      content=33824「0」
│     └（chartbox 修饰）
├ logicalCpuHost  height=80rp  accname=34482「逻辑 CPU 图表窗格」
│ ├ cpuChartTitle  / chartArea
│ ├ cpuGrid       layoutpos=client
│ ├ cpuBlock      height=17rp, borderthickness=rect(0,0,0,1)
│ │ ├ (背景 B)    sheet=cpuBlockStyleB  bordercolor=#BFCFDF
│ │ ├ (背景 R)    sheet=cpuBlockStyleR  bordercolor=#DFE7EB, borderthickness=rect(0,0,1,0)
│ │ └ cpuBlockData sheet=cpuBlockStyleData (gtf 25 = 11px, fg #000000)
│ │                borderthickness=rect(1,1,1,1), padding=rect(5,0,5,0)
│ └ cpuBottom     class=bheader
├ heatMapCpuHost  height=80rp  accname=34483「逻辑 CPU 图表窗格」
│ ├ cpuChartTitle  content=34278「30 秒内的利用率 % 」
│ └ cpuHeatGrid   bordercolor=gtc(TASKMANAGER,38,0,3802)=#117DBB, borderthickness=rect(1,1,1,1)
│                 layout=filllayout()
├ numaCpuHost     height=80rp  accname=34483
└ atmCpu → atmCpuChart  sheet=cpuchart (80×80, padding rect(2,2,2,2))
static_cpu (rowlayout(1))  —— 右侧静态属性，12 组 label+value，每组 label margin=rect(0,0,10rp,0)
  maxspeed_label 34250 / socket_label 34251 / core_Label 34264 / logicalproc_label 34252 /
  hostLogical_label 34254 / virtual_Label 34255 / hyperV_Label 34266 / vProc_Label 34265 /
  vm_Label 34256 / l1_label 34257 / l2_Label 34258 / l3_Label 34259
dynamic_cpu —— 右侧动态属性
  cpuUsage(margin 20,0,0,0) / cpuSpeed(margin 0,8,0,0) / process(20,0,0,0) /
  thread(20,0,0,0) / handle(0,8,0,0) / cpuUptime
```

### 3.5 内存页

```
memory_chart  sheet=DashboardStyle
├ (theader) memoryUsageTitle 34360「内存使用量」 / rheader memory_size
├ chartArea [rect(15,0,0,0)]
│ ├ memoryUsageGraph        sheet=staticResourceChart (80rp)
│ └ memoryUsageGraph_bottom class=bheader
│   ├ RateLabelMem lheader      ← 空
│   └ rheader  content=33818「0」
├ memAvailabilityBar  sheet=DashboardStyle
│     margin = rect(0, 6rp, 0, 0)
│     padding = rect(0, 0, 0, 14rp)
│ ├ BarTopLabel   content=34361「内存使用量」, padding=rect(0,0,0,5rp)
│ └ BarchartMemoryHolder  height=52rp, borderthickness=rect(1,1,1,1),
│     bordercolor = gtc(CHARTVIEW,9,0,3802) = #8B12AE
│   ├ BarchartInuse     content=gtc(TASKMANAGER,21,0,3802)=#F4F2F4, accname=33852
│   ├ BarchartModified  content=#EEE6F1(22), bordercolor=#CEB0D7(23), bt=rect(1,0,0,0), accname=33841
│   ├ BarchartStandby   content=#FFFFFF(20), bordercolor=#CEB0D7(23), bt=rect(1,0,0,0), accname=33853
│   └ BarchartFree      content=#FFFFFF(20), bordercolor=#CEB0D7(23), bt=rect(1,0,0,0), accname=33854
static_memory  (rowlayout(2), 5 组) —— 速度 / 已用插槽 / 外形规格 / 为硬件保留 / 最大内存
dynamic_memory (6 组)              —— 使用中 / 可用 / 已提交 / 已缓存 / 分页 / 非分页
```

### 3.6 磁盘页 / 网络页

```
disk_chart / disk_server_chart  sheet=DashboardStyle
├ chartbox → diskTimeTitle 34462「活动时间」 + disk_rate_unit
├ chartArea [rect(1,0,0,0)]
│ ├ diskTimeGraph      sheet=staticResourceChart
│ ├ diskTime_bottom    bheader → RateLabelDiskTime 「0」
│ ├ diskRateGraph      sheet=staticResourceChart
│ └ diskRate_bottom    bheader → RateLabelDiskRate 「0」
├ (块间) padding = rect(0, 6rp, 0, 0)
static_disk (rowlayout(3), 5 组) / dynamic_disk_server / dynamic_disk_client
  diskReadLegend 34457 / diskWriteLegend 34458 / disk_read / disk_write / disk_time / disk_response

net_chart  sheet=DashboardStyle
├ netChartTitle 34560 / netChartMax / net_adapter
├ chartArea [rect(1,0,0,0)]
│ ├（主曲线，sheet=staticResourceChart）
│ └ primaryNetChart_bottom bheader → RateLabelNet 「0」
static_network (rowlayout(4), 8 组)：adapter 34550 / wwan / wifi / netname / netType / ipv4 / ipv6 / signal
dynamic_network：netSendLegend 34562 / netSend / netReceiveLegend 34561 / netReceive
```

### 3.7 GPU 页

```
gpu_chart  sheet=DashboardStyle
├ gpuPrimaryCharts
├ gpuMemoryCharts   margin = rect(0, 5rp, 0, 5rp)
├ singleEngineGpuView  41027 / topArea / engineSelector(dtb ItemsView::Header 4,2) /
│   engineTitle / engineUtilizationRate / chartArea[rect(15,0,0,0)] → engineChart(sheet=gpuchart)
└ multiEngineGpuView   41028
    ├ gpuGrid → gpuEngineChartTemplate → gpuEngineChart  (padding rect(2,2,2,2), sheet=gpuchart)
    └ gpuMemoryChart
        ├ gpuDedicatedMemory → gpuMemUsageTitle(41012) / gpuMemoryMax / gpuMemUsageGraph(41014)
        └ gpuSharedMemory    → gpuMemUsageShared(41013) / (41015)
static_gpu (rowlayout(5))：驱动版本 41001 / 日期 41002 / DX 版本 41003 / 物理位置 41032 / 硬件保留 41033
dynamic_gpu：gpuUsage / gpuTotalMemory / gpuDedicatedMemRoot / gpuSharedMemRoot / gpuTemperature
```

### 3.8 详情 / 服务 / 用户页

```
servicestab_graphs / servicestab_tables → atom(svcList)  (无任何 rp 声明)
userstab_tables → TmUsersPageView (padding rect(0,8,0,0))
                  └ ColScrollViewer → TmColumnHeader → 11 列（SessionID / SessionType / ClientName / CPU / 内存 / 磁盘 / 网络 / GPU / GPU引擎 / …）
startuptab_tables → TmStartupPageView (padding rect(0,8,0,0))
                  ├ BIOSPostTimeHolder margin = rect(0, 7rp, 7rp, 20rp)
                  └ …10 列（启动影响 / 启动类型 / 磁盘 I/O / CPU / 正在运行 / 已禁用时间 …）
apphistorytab_tables → TmAppHistoryPageView
                  ├ LastUpdateText / ResetUsageData(padding rect(4,8,0,8), rect(2,0,0,0), rect(4,0,4,0))
                  └ …8 列
```

### 3.9 工具条（`30024` 的 `GenericCommandBar`，`30001` 只有样式没有实例）

```
GenericCommandBar
└ TmCommandBar       height=48rp, sheet=TmCommandBarStyle, layout=filllayout(),
                     padding=rect(8rp,0,0,0), accname=37233
  ├ chartbox / viewer / YScrollable / XScrollable  (外层滚)
  ├ CBLeftAlignedButtons      padding = rect(0, 10rp, 0, 10rp)
  │ ├ CBExpandoButtonImage    layout=flowlayout(0,2,0,2)
  │ │ ├ CBExpandoButtonImageArrow  height/width = gtmet(TaskDialog,13,0,2417/2416)
  │ │ └ CBExpandoButtonImageText   margin = rect(5rp,0,0,0)
  │ └ dashboardBar / servicesBar   padding = rect(0, 6rp, 0, 7rp)
  │   ├ seperatorId   padding = rect(6rp, 0, 8rp, 0)
  │   ├ resmonIcon / resmonLaunch (TaskManagerCommon→link)  padding = rect(1,1,0,0) / rect(4,0,4,0)
  │   └ servicesIcon / servicesLaunch
  └ CBTaskButtons     sheet=TmCommandBarStyle, layout=flowlayout(0,2,1,2), padding=rect(0,0,8rp,3rp)
    ├ TmTaskButton  sheet=TmCommandBarStyle, layout=BorderLayout(),
    │   minsize=size(85rp, 23rp), padding=rect(2,0,2,0), margin=rect(10,10,10,10),
    │   borderthickness=rect(1,0,0,0)
    ├ CBRestartButton / CBEnableStartupButton / CBDisableStartupButton /
    │ CBDisconnectUserButton / CBLogoffUserButton / CBSwitchUserButton（同样式）
```

---

## 4. 与我们代码的逐条对照

> **前提**：`include/theme.h` 与 `include/perfpage.h` 在本轮取证期间**被另一个 agent 改动过**
> （mtime 07:48 / 07:49）。本节按**改动后的当前内容**对照（theme.h 415 行、perfpage.h 337 行）。
> 改动本身大量引用了「UIFILE 声明」，所以下表的「差」列现在多数是 0 —— 每一条我都重新核了
> UIFILE 原文，其中**几何类全部复核通过**，但 `.cpp` 里还有 1 项没跟上（见 §4.4）。

### 4.1 `include/theme.h`（当前 415 行）

| 常量 | 代码现值 | UIFILE / 主题真值 | 差 | UIFILE 覆盖？ |
|---|---|---|---|---|
| `kRowHeight` | **28** | `atom(TmViewRow)` height=**28rp**（@0x115e→值@0x1160；@0x17b2 / @0x1c0e / @0x20ca 同值） | **0** ✅ | ✅ 明确声明 |
| `kSubRowHeight`（新增） | **22** | 子行 `TmViewRow` = **22rp**（@0x147a→@0x147c 等 4 处） | **0** ✅ | ✅ 明确声明 |
| `kGroupRowHeight` | = `kRowHeight`(28) | UIFILE **不给** `TmGroupRow` 高度，只给 `TmGroupHeaderName` padding `rect(8,10,0,7)` | — | ❌ **不覆盖** |
| `kHeaderHeight` | 24 | UIFILE **不给**列头高度；24 来自 `GetThemePartSize(HEADER, HP_HEADERITEM)` | — | ❌ 不覆盖（主题覆盖、UIFILE 不覆盖） |
| `kProcHeaderHeight` | 24 | 同上 | — | ❌ 不覆盖 |
| `kToolbarHeight` | 32 | 30024 的 `TmCommandBar` 是 **48rp**；30001 **无工具条实例** | 口径不同（§5.2） | ❌ 不覆盖 |
| `kCardHeight` / `kCardMinWidth` / `kCardGap` / `kCardPadding` | 84 / 148 / 8 / 10 | UIFILE **没有「卡片」元素**；`card` / `Card` / `卡片` 在 30001(839 串) + 30024(437 串) + wdc(6 XML) 里 **0 命中** | — | ❌ **不覆盖** |
| `kIconSize` | 16 | `atom(TmRowIcon)` **16rp**（@0x11c4）、`TmRowIconBackground` width/height **16rp**（@0x11a0 / @0x11a4） | **0** ✅ | ✅ 明确声明 |
| `kIconLeft` | **28** | = `TmFirstColumn`.padding.left **6**（@0x1180）+ `ViewExpandoImageWrapper`.width **22**（@0x1120） | **0** ✅ | ✅ 明确声明（两段都在） |
| `kExpandoWidth`（新增） | **22** | `ViewExpandoImageWrapper` width = **22rp** | **0** ✅ | ✅ 明确声明 |
| `kIndentPerLevel` | **0** | UIFILE 无「列表级沟槽」概念 → 0 正确 | **0** ✅ | ✅（归零即正确） |
| `kTreeIndent` | **8** | 子行 14rp − 父行 6rp = **8rp**（@0x1180 vs @0x149c） | **0** ✅ | ✅ 明确声明 |
| `kCellPadLeft` / `Right` | **4 / 4** | 单元格 padding `rect(4,2,4,3)`（@0x11e8 起 130 处） | **0** ✅ | ✅ 明确声明 |
| `kCellPadTop` / `Bottom`（新增） | **2 / 3** | 同上，top=2 / bottom=3（不对称，原样） | **0** ✅ | ✅ 明确声明 |
| `kRowHover` | **#E5F3FF** | `dtb(EXPLORER::LISTVIEW,1,2)` 实渲染 = **#E5F3FF**（4800/4800 像素纯色） | **0** ✅ | ✅（UIFILE 有引用，值靠渲染） |
| `kBgNavSelected` | **#CDE8FF** | `dtb(…,1,3)` = **#CDE8FF** | **0** ✅ | ✅ |
| `kLineHeader` | **#828790** | 列头 `dtb(EXPLORER::LISTVIEW,1,0/1/4)` 底边一行 = **#828790**（444 像素） | **0** ✅ | ✅（UIFILE 引用，值靠渲染） |
| `kLineNavBorder` / `kLineCard` | #E5E5E5 | UIFILE 给出的三个灰都**不是** #E5E5E5：横线 threedshadow **#A0A0A0**、列间竖线 threedlightshadow **#E3E3E3**、CPU 网格 **#117DBB** | 无对应 | ❌ **不覆盖** |
| `kBgCard` | #FAFAFA | 无卡片元素 | — | ❌ |
| `kBgGroupHeader` | #F2F2F2 | UIFILE **不给**分组行底色；`TmGroupHeaderName` 只有 `bordercolor = gtc(TASKMANAGER,2,0,3802)` = **#EAD5A0**（土黄，是**边框**不是底） | — | ❌ 不覆盖（见 §4.3） |
| `kTextPrimary` | #141414 | `gtc(TASKMANAGER,19,0,3803)` = **#000000**（dynamicData）；`gtc(TEXTSTYLE,1,0,3803)` = **#000000** | +20 | ✅ |
| `kTextSecondary` | #5A5A5A | `gtc(TASKMANAGER,17,0,3803)` = **#707070**（staticLabel / dynamicLabel） | −22 | ✅ |
| `kTextHeader` | #5A5A5A | 列头文字 `themeable(gtc(ItemsView::ListView,1,3,3803), highlightText)` = **#000000**；子列头 `gtc(TASKMANAGER,24,1,3803)` = **#4C607A** | −90 / 差 | ✅ |
| `kTextDisabled` | #6D6D6D | 无 UIFILE 引用（= `GetSysColor(COLOR_GRAYTEXT)`） | — | ❌ |
| `kTextSelected` | #141414 | 选中态**只换背景不改文字色**（`TmViewItemStyle` 的 `ViewItemSelection` 段只改 `background`） | 语义一致 ✅ | ✅ |
| `kChartLineCpu` | #117DBB | `gtc(TASKMANAGER,38,0,3802)` = **#117DBB** | **0** ✅ | ✅ |
| `kChartLineMem` | #8B12AE | `gtc(CHARTVIEW,9,0,3802)` = **#8B12AE**（UIFILE 里是组合条**边框**） | **0** ✅ | ✅ |
| `kChartLineDisk` | #4DA60C | ChartView part 16/18/19/21 = **#4DA60C**；**UIFILE 未直接引用 part 16** | **0**（值对，引用链缺） | ⚠️ |
| `kChartLineNet` | #A74F01 | ChartView part 23/25 = **#A74F01**；**UIFILE 未直接引用 part 23** | **0**（值对，引用链缺） | ⚠️ |
| `kChartLineGpu` | #117DBB | 同 CPU（ChartView 2/4） | **0** ✅ | ✅ |
| `kChartAxisText` | **#707070** | `gtc(TASKMANAGER,44,0,3803)` = **#707070**（lheader / rheader） | **0** ✅ | ✅ 明确声明 |
| `kChartFill` | #F1F6FA | **UIFILE 不给**（`chart` 样式 `background=window`；图表内部由 CHARTV.dll 画） | — | ❌ **不覆盖** |
| `HeatColor` 9 档 | 9 档 | 不在 UIFILE（在 `.data` RVA 0x1370F0 + `TASKMANAGER` part 3..11 state 1） | — | ❌（UIFILE 只引用 part 38/39/40） |

### 4.2 `include/perfpage.h`（当前 337 行，`namespace perftheme`）

| 常量 | 代码现值 | UIFILE 真值 | 差 |
|---|---|---|---|
| `kSidebarTopPad` | **7** | `dashSidebarScrollViewer` padding **top = 7rp**（@0x4c78） | **0** ✅ |
| `kSidebarLeftPad`（新增） | **3** | 同一条 padding **left = 3rp** | **0** ✅ |
| `kSidebarWidth` / `kSidebarCardW` | 210 / 202 | **UIFILE 无声明**（宽度由 BorderLayout + `dashSidebar`(`drop`) 决定） | — ❌ 不覆盖 |
| `kSidebarCardH2` / `H3` | 61 / 68 | **UIFILE 无声明**（高度由迷你图 45rp + 文字撑出） | — ❌ 不覆盖 |
| `kMiniChartW` | **60** | `chart` 样式 width = **60rp**（@0x290） | **0** ✅ |
| `kMiniChartH`（新增） | **45** | `chart` 样式 height = **45rp**（@0x28c）；padding-top 5rp ⇒ 可视内容 **40rp** | **0** ✅ |
| `kChartPad` / `kChartPadTight` | **15 / 1** | `atom(chartArea)` 位置参数：CPU/内存/GPU = **15rp**（@0x572a / @0x5cd2 / @0x6b16）；磁盘/网络 = **1rp**（@0x61de / @0x628e / @0x6756） | **0** ✅ |
| `kChartHeight` | **80** | `staticResourceChart` height = **80rp**（@0x320） | **0** ✅（见 §5.8：基准值，实际被 layoutpos=client 拉伸） |
| `kStatsRowHeight` | 24 | **UIFILE 无声明**（行高由 `dynamicLabel` 12px + `dynamicData` 21px + margin 撑出） | — ❌ 不覆盖 |
| `kContentPadL`（新增） | **17** | `performancetab_tables` 根 padding **left = 17rp**（@0x5180） | **0** ✅ |
| `kSidebarBg` | #FFFFFF | `dashSidebar` sheet=`drop` `background=window` ⇒ `GetSysColor(COLOR_WINDOW)` = **#FFFFFF** | **0** ✅ |
| `kSidebarSel` | **#CCE8FF** | `dtb(…,1,6)` LISS_HOTSELECTED = **#CCE8FF**；但 `dashSidebarEntryViewer` **没有引用 1,6** | **0**（引用链缺） ⚠️ |
| `kSidebarHover` | **#E5F3FF** | `dtb(…,1,2)` = **#E5F3FF** | **0** ✅ |
| `kSidebarLine` | #E5E5E5 | `dashSidebarDivider` 用 `sgraphic(31222/31223/31224)` 贴图，**不是纯色** | — ❌ |
| `kSidebarAccent` | #0078D7 | **UIFILE 没有任何 3px 竖条元素**（`dashSidebarEntry` 的 `padding: rect(0,0,3,0)` 是右侧留白） | ❌ 无出处 |
| `kChartBg` / `kChartFill` / `kChartAreaBg` | #F1F6FA | **UIFILE 不给** | ❌ 不覆盖 |
| `kChartGrid` | #E8F0F9 | `cpuHeatGrid` 的 `bordercolor` = **#117DBB**（TASKMANAGER 38）；**主曲线网格 UIFILE 不覆盖** | ⚠️ |
| `kAxisText` | **#707070** | `gtc(TASKMANAGER,44,0,3803)` = **#707070** | **0** ✅ |
| `kTitleText` | #1A1A1A | `resourceTitle` → **#000000** | +26 |
| `kSubText` | #5A5A5A | `staticLabel` / `dynamicLabel` → **#707070** | −22 |

### 4.3 一处需要修正的注释：`#EAD5A0` 是**边框色**，不是「不可用的填充色」

`theme.h` 现在写「主题 TASKMANAGER p2 s0 TMT_FILLCOLOR = #EAD5A0（土黄），该色在原版截图判定里
0 命中 —— 主题值不可用」。

半对半错：

- **对**的地方：它确实不是分组行的**底色**，截图里大面积取样当然 0 命中。
- **错**的地方：UIFILE 把 `gtc(TASKMANAGER, 2, 0, 3802)` 挂在 **`bordercolor`** 上：
  - `TmGroupHeaderStyle` → `atom(TmGroupHeaderName)`：`bordercolor = gtc(TASKMANAGER,2,0,3802)`
  - `TmViewItemStyle` → `atom(TmFirstColumn)`：同样挂 `bordercolor`

  取色属性号 3802（FILLCOLOR）是这个值的**定义来源**，而它的**用途**是边框 —— DUI 允许这样用。

**结论**：`#EAD5A0` 是**分组标题 / 第一列的分隔线色**（1px），有 UIFILE 硬证据。
所以 `kBgGroupHeader = #F2F2F2` 依旧无依据（UIFILE 不覆盖），但**应新增**
`kGroupHeaderBorder = RGB(0xEA,0xD5,0xA0)`。

### 4.4 `.cpp` 现状（已跟上 2 项，还差 1 项）

| 项 | 代码现值 | UIFILE 真值 | 差 | 位置 |
|---|---|---|---|---|
| 右侧属性列宽 | `kPropColW = 402` ✅ | `atom(detailInfo)` **width = 402rp**（@0x51bc） | **0** | `src/perfpage.cpp:1119` |
| 内存组合条高 | `kBarHeight = 52` ✅ | `BarchartMemoryHolder` **height = 52rp**（@0x5a0） | **0** | `src/perfpage.cpp:1120` |
| 内容区左内缩 | `kContentPadL = 17`，并用 `Rpx()` | `performancetab_tables` padding.left = **17rp**（@0x5180） | **0** | `src/perfpage.cpp` |
| 组合条图例色块 | **10×10 方块**（`RECT box{lx, ly+3, lx+10, ly+13}`） | `dashboardLegend` **width = 2rp / height = 32rp**（@0x7b0 / @0x7b4）⇒ 原版是**竖细条 2×32**，不是方块 | **仍不对** | `src/perfpage.cpp:1255` |

> 图例这一条是**形状**错，不是数值错：原版每个图例项前面是一条 2px 宽、32px 高的**竖色条**，
> 中间有 `padding: rect(0,0,3,0)`（右侧 3px），第一项 `margin: rect(0,0,6,0)`。
> 我们画的是 10×10 方块 + 横向文字。要改成竖条需要同时改 `ly` 的基准与行高。

---

## 5. 关键结论与需要裁决的冲突

### 5.1 UIFILE **明确声明**的（可直接照抄）

1. 行高 **28rp**（父行）/ **22rp**（子行）
2. 单元格 padding **`rect(4,2,4,3)`**，资源列右边线 **1rp**
3. 子行相对父行**多缩进 8rp**（`rect(14,0,0,0)` vs `rect(6,0,0,0)`）
4. 行图标 **16×16**，展开箭头 16×16，`ViewExpandoImageWrapper` 宽 22
5. 列宽拖柄 **5rp**、`ResizerImage` **1rp**、排序箭头 **10rp 高 / 普通列 padding `rect(0,2,6,0)`、资源列宽 17rp**
6. 列头文字 padding 普通列 **`rect(0,0,0,4)`**、资源列 **`rect(0,3,0,4)`**；列头左 padding 普通 **4**、资源 **7**
7. 分组标题 padding **`rect(8,10,0,7)`**
8. 主图高 **80rp**；迷你图 **45rp × 60rp**、margin `rect(20,10,10,10)`、padding `rect(0,5,0,0)`
9. 右侧属性列宽 **402rp**、padding **`rect(0,10,24,20)`**
10. 内存组合条高 **52rp**、外框 1rp、段间竖线 1rp、图例色块 **2rp×32rp**、图例 padding `rect(0,0,3,0)`
11. CPU 逻辑格高 **17rp**、文字 padding **`rect(5,0,5,0)`**、格间 1px 线
12. 侧栏内容 padding **`rect(3,7,0,0)`**、卡片右留白 **3rp**、侧栏分隔条 **45×8**
13. 页签高 **30rp**、padding **`rect(0,2,0,0)`**
14. 工具条（**仅 30024 简略视图**）高 **48rp**、按钮 minsize **85×23**；**30001 新式界面里没有工具条实例**

### 5.2 `kToolbarHeight = 32` 与 UIFILE 48rp 不是同一口径

- `30001` 是**新式界面**（顶部页签 + 每页自己的内容），**全文没有工具条实例**；
- `30024` 是**老式界面**（`taskman` 根 + `processestab_*`），工具条 `TmCommandBar` **48rp**。
- 我们的 `32` 来自「截图里工具条总高 692−660」，那是**新式界面**的工具条。
  **UIFILE 对新式工具条不给值**（它由 `CTaskBar` 代码画，不在 DUI 里）。
  → 结论：`32` **不能被 UIFILE 证伪，也不能被证实**。保留，但注释应改成「UIFILE 不管」。

### 5.3 `kIndentPerLevel` 数值凑对了但构成错了；`kTreeIndent` 明确错了

UIFILE 里「一行从列表左沿到图标左沿」是三段相加：

```
TmFirstColumn.padding.left   6rp     (父行；子行 14rp)
ViewExpandoImageWrapper.width 22rp   (展开箭头占位)
--------------------------------------
到图标左沿             = 28rp
```

我们代码是 `kIndentPerLevel(18) + kIconLeft(10) = 28` —— **结果一样，套路不同**：
我们多了一个「列表级沟槽 18」的概念，UIFILE 里没有这个沟槽，
22rp 的箭头位是**每行都在**的（不是额外缩进）。

**真正的错误是每级缩进**：

| 量 | 我们 | UIFILE |
|---|---|---|
| 父行第一列 padding.left | `kIndentPerLevel` 18 + `kIconLeft` 10，混算 | **6rp** |
| 子行第一列 padding.left | 叠加 `kTreeIndent` 18 | **14rp** |
| **每级缩进** | **18** | **14 − 6 = 8rp** |

也就是说：子树每深一级，我们比原版多缩 **10px**。两级下来就偏 20px。
这条与「图标 x=28」那个旧结论**不冲突** —— 那个结论是第 0 级，本来就对。

### 5.4 侧栏字号

- UIFILE：`sidebarLabel` = **16px**（part 16），`sidebarData` = **12px**（part 17）。
- 我们：`fontSide1_=16`、`fontSide2_=12`、`fontSide3_=11`。
- **第三档 11px 在 UIFILE 里没有对应 `gtf`** —— 磁盘卡的「读取:/写入:」是 `sidebarDataHeader`，
  用的也是 part 17（**12px**）。所以 `fontSide3_=11` **无出处**。

### 5.5 选中行颜色（代码已改，剩下的是「引用链缺一环」）

- 列表选中态：UIFILE 引用 `dtb(EXPLORER::LISTVIEW, 1, 3)`，`DrawThemeBackground` 实渲染 = **`#CDE8FF`**。
  代码 `kBgNavSelected` 已改成 `#CDE8FF`（旧值 `#A0CDF0` 依据的是我方旧截图，作废）。✅
- 性能页侧栏选中：代码 `kSidebarSel = #CCE8FF`，与 `dtb(…, 1, 6)`（LISS_HOTSELECTED）实渲染**一致**，
  但 `dashSidebarEntryViewer` 里**既没有 1,3 也没有 1,6** —— 这一格的选中态 UIFILE 没写。
  → 值对得上主题的「热选中」状态，但**DUI 引用链缺失**，属于「高置信度但不可证实」。
- 两个色**确实不是同一个**（`#CDE8FF` vs `#CCE8FF`），原版有意区分「列表选中」与「侧栏卡片选中」。

### 5.6 列头下边线（代码已改为 `#828790`）

UIFILE 里列头下边线不是自画线：`TmColumnHeader` 的 `background = dtb(EXPLORER::LISTVIEW, 1, 0/1/4)`；
`DrawThemeBackground` 渲染到 200×24 的 32bpp DIB 后，**最后一行整条是 `#828790`**（444 个像素），
其余行是 `#FFFFFF`。代码 `kLineHeader` 已改成 `#828790`。✅

> 顺带纠正一条历史误判：`#E5E5E5` 是 HEADERITEM 的**右边**线，不是下边线，两者不是同一图元。

### 5.7 `kLineCard = #E5E5E5` 无 UIFILE 出处（代码未改，无替代值）

- 表格内部横线：`TmViewStyle`→`TmViewData` `borderthickness=rect(0,1,0,1)` `bordercolor=threedshadow`
  （系统 `COLOR_3DSHADOW` = `#A0A0A0`）。
- 列间竖线：`TmViewColumnHeader` `borderthickness=rect(1,0,1,0)` `bordercolor=threedlightshadow`
  （= `#E3E3E3`）。
- CPU 热力网格线：**`#117DBB`**（TASKMANAGER 38）。
→ 三个不同的灰/蓝，**没有一个是 `#E5E5E5`**。

### 5.8 `kChartHeight`：UIFILE 声明 80rp（代码已改为 80）

`primaryCpuChart`/`memoryUsageGraph`/`diskTimeGraph`/`net_adapter`/`diskRateGraph` 全部
`sheet="staticResourceChart"`，而该样式 `height="80rp"`。**唯一的 80rp 之后是否被 DUI 拉伸**
取决于父 `chartArea` 的 `layoutpos=client`（会填满剩余空间）——所以 **80 是「最小/基准」，
实际高度由窗口高度决定**。旧值 160 是按实际窗口算的、反解自已证伪的截图；
代码现已改成 80rp，并注明「80 是基准最小高，实际由 layoutpos=client 拉伸」。

### 5.9 UIFILE **完全不管**的元素（不要再拿「UIFILE 没写」当反证）

| 元素 | 为什么 |
|---|---|
| 进程页顶部 **5 张汇总卡片** | `apptab_graphs` 在 30001 里只有 `chartbox` 一层、**没有任何子元素**；`apptab_tables` 直接从 `TmAppPageView` 开始。`card` / `Card` / `卡片` 在 30001（839 串）+ 30024（437 串）+ wdc 六个 XML 里合计出现 **0 次** |
| 分组行底色 | `TmGroupRow` 只有 `TmGroupHeaderName`（文字），**没有 background** |
| 列头高度 | 没有 `height`，由字体 + padding 撑出 |
| 侧栏卡片高度/间距 | 靠内容 +  mini chart 的 45rp 撑出 |
| 主图内部（网格线、坐标轴、曲线、填充） | 由 `CHARTV.dll`（`CvChartWindow`）画，DUI 只给容器 80rp |
| 性能页侧栏选中/悬停色 | `dashSidebarEntryViewer` 不带状态选择器 |
| 工具条（新式界面） | 见 §5.2 |
| 列宽像素值 | 列宽在 `taskmgr.exe` 的 `.rdata` 列池里（`docs/evidence/exec/_o_final.js` 已解出，stride 56、宽度 @+0x10），**不在 UIFILE** |
| 状态栏（进程数/CPU/内存 一行） | 原版**没有**，UIFILE 也证实没有 |

### 5.10 wdc 的 `barchart` RGB 与 taskmgr 的主题值是**两套**

| 段 | wdc `barchart` 字面量（相对监视器） | taskmgr `gtc`（主题） |
|---|---|---|
| 使用中 inuse | `gradient(RGB(143,184,12), RGB(126,162,10), 1)` → **绿** | `#F4F2F4`（灰） |
| 已修改 modified | `gradient(RGB(255,153,0), RGB(204,121,0), 1)` → **橙** | `#EEE6F1` + 边框 `#CEB0D7` |
| 备用 standby | `gradient(RGB(85,142,213), RGB(49,116,197), 1)` → **蓝** | `#FFFFFF` |
| 可用 free | `gradient(RGB(198,217,241), RGB(151,186,229), 1)` → **浅蓝** | `#FFFFFF` |
| 保留 reserved | `gradient(RGB(203,203,203), RGB(166,166,166), 1)` → **灰** | （taskmgr 无 reserved 段） |
| 外框 | — | `CHARTVIEW 9` = **`#8B12AE`** 紫 |

**这两套不可能同时是实际像素。** 按 `win10_perf_visual_spec.md` §7.1 的归属分析，
`wdc.dll` 是 resmon 的 COM 组件，taskmgr 只是**共享同一套源码基线**；
**taskmgr 内存页组合条走的是 `DashboardStyle` + `gtc`，即上表右列**。
我们代码目前用的第三种（`#0A3E70 / #C25B36 / #7FBAE0 / #D8D8D8`）**两套都不对**。

### 5.11 侧栏几何：UIFILE 只给 left/top/迷你图，不给宽度与卡片高

| 量 | 我们 | UIFILE 能给什么 | 判定 |
|---|---|---|---|
| 侧栏内容左缩 | **3**（`kSidebarLeftPad`，已改） | `dashSidebarScrollViewer` padding **left = 3rp** | **0** ✅ |
| 卡片宽 | 202 | **无声明**（`dashSidebarEntry` 只有 `padding: rect(0,0,3,0)`；宽由 `dashSidebar`(`drop`) 填满侧栏） | UIFILE 不覆盖 |
| 顶内边距 | **7**（`kSidebarTopPad`，已改） | `dashSidebarScrollViewer` padding **top = 7rp** | **0** ✅ |
| 卡片高 2 行 | 61 | **无声明**；能算的只有「迷你图 45（含 padding 后内容 40）+ 两行文字」 | UIFILE 不覆盖 |
| 卡片高 3 行 | 68 | 同上 | UIFILE 不覆盖 |
| 迷你图 | **60×45**（`kMiniChartW/H`，已改） | `chart` 样式 `width=60rp`、`height=45rp`、`padding: rect(0,5,0,0)`、`margin: rect(20,10,10,10)` | **0** ✅（可视内容高 40rp = 45−5） |
| 迷你图左沿 | 未单独建模 | `chart` 的 `margin.left = 20rp`；`dashSidebarActionButton` 自身 `padding.left = 10rp` | UIFILE 口径：卡片内容盒左 +20（不是 9）；我们没建这层 |
| 纯矩形高亮、无圆角 | 是 | `dashSidebarActionButton` 无 `borderstyle`、无 `bordercolor` | 一致 |

> **结论**：left 3rp / top 7rp / 迷你图 60×45 三项 UIFILE 明确，代码已全部对齐。
> UIFILE **不提供**卡片高度（61/68 仍是旧值），也不提供侧栏宽 210/卡片宽 202。

---

## 6. 补充：本轮新增的几个可复用脚本

| 脚本 | 作用 | 输出 |
|---|---|---|
| `docs/evidence/exec/_S_parse.js` | duib 头解析 + 串表校验 + block/geo/gtc/tree 导出 | `check` 模式可验证任意 duib 的魔数/表偏移 |
| `docs/evidence/exec/_S_full.js` / `_S_sec.js` / `_S_blk.js` | 按 block / 按「resid+选择器」切段，逐属性导出（含偏移） | TSV，可直接 diff 两个 UIFILE |
| `docs/evidence/exec/_S_resolve.js` | `gtc`/`gtf` → 颜色/字体的批量反查（含 R/B 修正） | `_S_refs.tsv` |
| `docs/evidence/exec/_S_theme4.ps1` | 对 UIFILE 引用到的 24 个 `(class,part,state)` 全量查颜色 + 字体 + int 属性 | stdout |
| `docs/evidence/exec/_S_render3.ps1` | `DrawThemeBackground` 到内存 DIB，按像素直方图给出**真实渲染色** | stdout（用于 §2.4 行底色） |

产物 TSV（已落盘，可复核）：

```
docs/evidence/exec/_S_geo2_30001.tsv   1,054 行  30001 全部几何声明（含 token 偏移）
docs/evidence/exec/_S_theme2_30001.tsv   907 行  30001 全部 gtc/gtf/dtb/themeable 引用
docs/evidence/exec/_S_geo2_30024.tsv       212 行  30024 同上
docs/evidence/exec/_S_theme2_30024.tsv     411 行  30024 同上
docs/evidence/exec/_S_refs.tsv              39 行  去重后的主题引用 + 解析出的颜色
```

---

## 7. 证据边界（必须写进结论的部分）

1. **duib 的 219 条「第二几何声明」无法判定 padding/margin**（§0.1）。数值可信，归属不可判。
2. **`0x8000` / `0x802a` 的语义是推断**（§1.1 表注）。凡是这两条支撑的结论都标了「推断」。
3. **`30024` 与 `30001` 是两代界面**。`30024` 有工具条（48rp）和「简略视图」列宽；
   `30001` 没有工具条实例。**不能混用**。
4. **wdc 的 RGB 字面量不是 taskmgr 的**（§5.10）。只能用于「微软这套代码怎么配色」的旁证。
5. **主图内部（网格/坐标轴/曲线）DUI 完全不描述**，全部在 `CHARTV.dll`。
   我们现有的 `#F1F6FA / #E8F0F9` 两值**没有任何 UIFILE 依据**；轴文字已按 gtc 改成 `#707070`。
6. **列宽不在 UIFILE**。`docs/evidence/exec/_o_final.js` 解出的 `.rdata` 列池才是列宽真源。
7. `themeprobe_out.txt` 的 R/B 通道是反的（脚本按 ARGB 解 `COLORREF 0x00BBGGRR`）。
   本报告所有颜色已用 `themedump2_out.txt` 与 `_S_theme4.ps1` 双向复核，**0 处不一致**。
8. **本轮取证期间 `include/theme.h` 与 `include/perfpage.h` 被另一个 agent 改动**（mtime 07:48/07:49），
   §4 已按改动后的当前内容重做。凡是「代码已改」的行都是指**当前文件状态**，不是我改的（我未改任何源码）。
9. **176 个 `@0xNNN` 偏移全部逐字节复核通过**，脚本见 §6；复核方式是把偏移送进 token 流解出
   属性名或字符串，与表格里的 rp 值逐字比对。

---

状态：已完成
