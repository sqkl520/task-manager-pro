# Agent L — 无参考性能页

状态：已完成

任务：内存 / 磁盘 / 网络 / GPU 四个性能页在原版 Win10 taskmgr 中的真实形态。
唯一原版参考图：`shots/truth1.png`（CPU 性能页整窗 1100×700）、`shots/truth2.png`（同一窗口的逻辑处理器视图）。

---

## 0. 证据基础（先说清楚手上有什么、没有什么）

### 0.1 证据来源

| 编号 | 素材 | 性质 | 强度 |
|---|---|---|---|
| E1 | `C:/Windows/SystemResources/Taskmgr.exe.mun`（375,296 B） | **原版资源库本体**，内含 `UIFILE 30001`（62,248 B duib）与 14 个 PNG | 硬证据 |
| E2 | `_probe/uifile_Taskmgr_30001_strings.txt`（765 行） | 由 E1 的 duib 按元素树顺序抽出的字符串表 | 硬证据 |
| E3 | `_probe/mui_strings_all.txt`（848 条） | `zh-CN/Taskmgr.exe.mui` STRINGTABLE | 硬证据 |
| E4 | `_probe/themedump2_out.txt` + 本轮复测 | 运行时 `OpenThemeData("TaskManager"/"ChartView")` + `GetThemeColor` | 硬证据 |
| E5 | `shots/truth1.png` 逐像素 | 原版 CPU 页截图 | 硬证据（仅 CPU 页） |
| E6 | `C:/Windows/System32/CHARTV.dll` 字符串 | **原版图表绘制引擎**，`CvLineChart`/`CvLine::FlushSegments`/`GdipDrawLinesI` | 硬证据（机制） |
| E7 | `C:/Windows/System32/taskmgr.exe` 全文扫描 | 1,356,400 B，PE .text/.rdata/.data 全扫 | 硬证据（否定性证据） |
| E8 | `_probe/tm/disasm.txt`（289,352 行） | dumpbin 全反汇编 | 参考 |
| E9 | `_probe/tm/strs.txt` / `wstr_rdata.json` | 元素名 / 格式串 | 硬证据 |

### 0.2 本轮新拿到的三个关键点

1. **真正的资源库位置**：`C:\Windows\SystemResources\Taskmgr.exe.mun`。
   之前一直以为在 `System32\` 或 `zh-CN\`。`zh-CN\Taskmgr.exe.mui`（37,376 B）里**只有 STRINGTABLE / MENU / DIALOG**，
   没有 UIFILE、没有 PNG —— 这解释了为什么在 mui 里反复搜 `duib` 全是 0 命中。
2. **图表不是 DUI 画的，是 CHARTV.dll 画的**。`taskmgr.exe` 导入表里有 `CHARTV.dll`，
   且 duib 里唯一的图表类引用是 `gtc(CHARTVIEW, 9, 0, 3802)`（内存组合条）。
   其余所有曲线图是 `atom(...Graph)` 元素，由代码创建 CHARTV 子窗口渲染。
3. **曲线颜色不在 taskmgr.exe 里，也不在 CHARTV.dll 里**。
   对 `#117DBB / #8B12AE / #4DA60C / #A74F01 / #1B82C2 / #6AAED4 / #E8F0F9 / #DEECF5 / #F1F6FA`
   九种颜色，分别按 `COLORREF(0x00BBGGRR)`、`ARGB(0xFFRRGGBB)`、`ARGB(0x00RRGGBB)`、裸 RGB 三字节
   四种编码，在 taskmgr.exe、CHARTV.dll、wdc.dll、duib 里全部扫描 —— **0 命中**。
   结论：颜色是**运行时从主题取**的，不是编译期常量。

---

## 1. 通用结论（四页共享，先讲清楚再分页）

### 1.1 曲线主色 —— 主题 `ChartView` 里有且只有 4 组，每组 7 个 part

【E4，**已证实**】`OpenThemeData(NULL,"ChartView")` 后逐 part 取 `GetThemeColor(part, state, TMT_FILLCOLOR=3802)`：

| part 区段 | p1 | p2 | p3 | p4 | p5 | p6 | p7 |
|---|---|---|---|---|---|---|---|
| **CPU** p1–7 | `#FFFFFF` | **`#117DBB`** | `#D9EAF4` | `#117DBB` | `#3386B5` | `#81B3D2` | `#3386B5` |
| **内存** p8–14 | `#FFFFFF` | **`#8B12AE`** | `#ECDEF0` | `#8B12AE` | `#9528B4` | `#817285` | `#9528B4` |
| **磁盘** p15–21 | `#FFFFFF` | **`#4DA60C`** | `#DBEDCE` | `#4DA60C` | `#4DA60C` | `#79D50D` | `#4DA60C` |
| **网络** p22–28 | `#FFFFFF` | **`#A74F01`** | `#EEDECF` | `#A74F01` | `#A74F01` | `#E0822F` | `#DF672C` |

（p7 之后 p8 又回到 `#FFFFFF`，四组首尾相接、界限干净：`#FFFFFF / 主色 / 浅色 / 主色 / 次色 / 亮色 / 次色`。
这个「白 → 主 → 浅 → 主 → 次 → 亮 → 次」的重复节律出现 4 次，正好对 4 个性能页。）

另外 `TaskManager` 类里 p38 = `#117DBB`（`cpuHeatGrid` 填充），说明 CPU 蓝是全局强调色。

**这一条直接证实了我方 `src/perfpage.cpp` 里写死的 4 个主色常量**：

| 页 | 我方常量（`src/perfpage.cpp:220/236/248/264/282`） | 主题值 | 判定 |
|---|---|---|---|
| 内存 | `RGB(0x8B,0x12,0xAE)` | `#8B12AE` | **已证实完全一致** |
| 磁盘 | `RGB(0x4D,0xA6,0x0C)` | `#4DA60C` | **已证实完全一致** |
| 网络 | `RGB(0xA7,0x4F,0x01)` | `#A74F01` | **已证实完全一致** |
| GPU | `RGB(0x11,0x7D,0xBB)` | `#117DBB`（同 CPU） | **已证实完全一致** |

并且这 4 个值还能从 `truth1.png` 的侧栏图标里**逐像素再印证一次**（见 §1.2）。

### 1.2 侧栏 mini 图与图标的颜色 —— truth1 逐像素核对

【E5，**已证实**】对 truth1 侧栏 6 张卡片做饱和像素直方图，每张卡的 mini 图边框色唯一：

| 卡片 | 取到的颜色 | 像素数 | 对应 |
|---|---|---|---|
| CPU（选中） | `#117DBB` + 选中底 `#CCE8FF` | 199 / 6484 | CPU 蓝 |
| 内存 | `#8B12AE` | 145 | 内存紫 |
| 磁盘 0 | `#4DA60C` | 139 | 磁盘绿 |
| 以太网 27 | `#A74F01` | 81 | 网络棕橙 |
| 以太网 45 | `#A74F01` | 136 | 网络棕橙 |
| NVIDIA GeFor… | `#117DBB` | 65 | GPU＝CPU 蓝 |

**GPU 卡片用 CPU 蓝，已由原版截图直接证实**（不是我方臆造）。

同时 mun 里有 5 张 16×16 图标 PNG（`31213`–`31217`），主色分别是
`#117DBB` / `#8B12AE` / `#4DA60C` / `#A74F01` / `#117DBB` —— 与上表一一对应，
且与 DUI 里 `sgraphic(31213,2)`…`sgraphic(31217,2)` 的挂载顺序一致
（`dashSidebarCpuPicture`→31213、`MemoryPicture`→31214、`DiskPicture`→31215、
`NetSymbol`→31216、`GpuPicture`→31217）。**双证据互证。**

### 1.3 曲线下方的填充 —— 是**纯色**，不是渐变

【E5，**已证实**（CPU 页）；对另外三页 **高概率**】

对 truth1 主图第一格做垂直切片（x=252/254，见 §1.4 的 ASCII 图）：

```
y148–162   #FFFFFF        ← 曲线以上：纯白
y163–167   #1B82C2        ← 曲线本体（该处是陡降段，垂直投影 5px）
y168–213   #F1F6FA        ← 曲线以下：一整片同色，无任何渐变过渡
```

> 线宽实测：取曲线**平坦段**量垂直厚度，1x 图（truth1）为 **1px**，
> 3x 原图（`_probe/tm/orig_full.png`，3300×2100）为 **3px** —— 即曲线是 **1px 宽**，
> 上面看到的 5px 是陡降段的垂直投影，不是线宽。
> 我方 `kCoreLine` 注释写的「1px 宽」**正确**。

- 填充色 `#F1F6FA` 在整张图里出现 **159,771 次**，且**只有这一个值** —— 没有中间色。
- 全图搜索「很浅且带轻微色偏」的像素（V≥235、饱和度 4–16），
  除 `#F1F6FA` 外只剩 29+2+2+1+1 个杂点（抗锯齿残留）。
  **没有第二个浅色填充候选，也就不存在渐变。**

对比主题值：`ChartView p3 = #D9EAF4`（CPU 浅色）。实测 `#F1F6FA` 明显更淡
（约等于 `#D9EAF4` 向白混 65%）。同页里 `#D9EAF4` 出现 **0 次**。
→ 结论：**原版不用主题浅色做填充，而是用了一个更淡的、图上实际是纯色的填充**。

我方实现 `kCoreFill = RGB(0xF1,0xF6,0xFA)` + `Polygon` 实心填充（`src/perfpage.cpp:599-601, 668-676`）
—— **与原版逐像素一致，已证实正确**。我方注释里写的「极淡填充」也比「6~7% alpha 同色」
更接近真相（原版是**不透明纯色** `#F1F6FA`，不是叠加 alpha）。

### 1.4 网格线 —— 横 9 条、竖 3 条、两种颜色

【E5，**已证实**（CPU 页几何，四页共用同一 ChartView 渲染）】

单格实测矩形：**161 × 70 px**，横向缝隙 **5px**、纵向缝隙 **4px**；
1100px 宽窗口下 **5 列 × 4 行**（20 个逻辑处理器）：

```
列：242–402 | 408–568 | 574–734 | 740–900 | 906–1066   （宽 161，缝 5）
行：145–214 | 219–288 | 293–362 | 367–436              （高 70，缝 4）
```

网格：
- **横线 9 条**，颜色 `#E8F0F9`，y = 153,160,167,174,180,187,194,201,208
  （把 70px 高度近似 10 等分，0% 与 100% 由上下边框充当）
- **竖线 3 条**，颜色 `#DEECF5`，x = 282,322,362
  （格子内 25% / 50% / 75%，即「60 秒里每 15 秒一根」）
- **边框 1px**，颜色 `#6AAED4`（整张图 9,160 个像素，是第二高频色）

我方 `kGridH=#E8F0F9` / `kGridV=#DEECF5` / `kCoreEdge=#6AAED4` / 横 9 竖 3
（`src/perfpage.cpp:603-604, 685-706`）—— **全部一致，已证实正确**。

### 1.5 曲线本体的颜色：`#1B82C2`，**不是** `#117DBB`

【E5，**已证实**；但与主题表冲突，必须单独说】

主图区域（x243–1066, y146–436）全量统计：

```
#1B82C2  → 4222 px
#117DBB  →    0 px
```

在**3× 原始截图** `_probe/tm/orig_full.png`（3300×2100，与 truth1 同一帧、未缩放）上复核，
主图区 `#1B82C2` **37,998 px**、`#117DBB` **0 px** —— 与 1x 版结论完全一致，
排除「缩放算法把 `#117DBB` 混成 `#1B82C2`」的可能。

即：**主曲线用的是 `#1B82C2`，主图区一整个 `#117DBB` 都没有。**
而侧栏 mini 图和 16×16 图标用的是 `#117DBB`（§1.2）。

两者是同一色相（HSL 202° vs 203°）的极近变体，`#1B82C2` 略亮。
在主题表里搜遍 `ChartView` / `TaskManager` / `Chart` / `Graph` / `Barchart` / `Tooltip` / `ListView`
的 p0–60 × s0–8 × prop 3800–3830，**`#1B82C2` 零命中**；
在 taskmgr.exe / CHARTV.dll 里按四种编码扫，也**零命中**。

→ 机制上：`CHARTV.dll` 只用 `GdipSetPenColor` + `GdipDrawLinesI` 画折线（E6），
   颜色由调用方（taskmgr）传入；而 taskmgr 里既没有这个字面量、也不在主题里，
   说明它是**运行时算出来的**（例如对主题色做一次提亮）。

**对我方的影响**：我方 CPU 网格曲线用的正是 `#1B82C2`（`kCoreLine`）——
**实测值一致，已证实正确**。但这也意味着**「主题主色」≠「画出来的线色」**，
详见 §5.2 的风险提示。

### 1.6 图表引擎能力边界（决定「有没有 tooltip / 会不会分引擎」）

【E6，**已证实**】`CHARTV.dll`（131,072 B）导出/内部符号：

```
CvChart::SetChartStyle      CvLineChart::Copy       CvBarChart::Copy
CvLine::FlushSegments       CvLine::Layout          CvBar::FlushBars
CvPieChart::AddDataSource   CvRadarChart::Copy
CvHitSurface::Begin/End/BeginElement/EndElement
CvSelection                 CvDatum/CvAxis/CvValueAxis/CvCategoryAxis
CvLegend / CvLegendEntry / CvLabel
导入：GdipDrawLinesI  GdipSetPenColor  GdipSetSolidFillColor  AlphaBlend
      Polyline  LineTo  Rectangle  CreatePen  DrawTextW  SetTextColor
```

判定：
- **有** 折线图（`CvLineChart`）、柱状图（`CvBarChart`）、饼图、雷达图；
- **有** 命中面（`CvHitSurface`）与选区（`CvSelection`）——说明**支持鼠标悬停命中与选中**；
- **没有** 任何「渐变填充 / 渐变画刷」符号（只有 `GdipSetSolidFillColor`，
  即**只支持纯色填充**）；
- 没有圆角、阴影、模糊之类。

这一条同时把 §1.3 的「纯色填充」从像素观察升级成**机制级证据**。

### 1.7 四页共用的 DUI 骨架

【E2，**已证实**】四页都挂在 `performancetab_graphs` 下，每页三段式：

```
<页>_chart
  ├ <页>UsageTitle / <页>ChartTitle / netChartTitle / engineTitle   ← 左上标题
  ├ <页>...Graph                    ← 主图（CHARTV 子窗口）
  ├ <页>..._bottom                  ← 图底部标签（通常是 "0"）
  ├ RateLabel<页>                   ← 刻度标签宿主
  └ 右侧 static_<页> / dynamic_<页>  ← 静态/动态信息列
```

`static_*` 用 `rowlayout(N)` 排布（**N = 每行元素个数**，详见 §3.1 注），
`dynamic_*` 是 `label + value` 对。这是四页右侧属性区的共同骨架。

---

## 2. 内存页

### 2.1 页面结构

【E2，**已证实**】DUI 元素树（按 duib 中出现顺序，`_probe/uifile_Taskmgr_30001_strings.txt:572-602`）：

```
d798  dynamic_memory
d7b6    atom(dynamic_memory)
d7e0      resstr(34399) = "使用中(已压缩)"
d7fc      atom(memory_inuse)     ← 34352 "可用"?? 见下方注
d822      resstr(34352) = "可用"
d83e      atom(memory_avai)      ← 34354 "已提交"
d862      resstr(34354) = "已提交"
d87e      atom(memory_commit)    ← 34353 "已缓存"
d8a6      resstr(34353) = "已缓存"
d8c2      atom(memory_cache)     ← 34356 "分页缓冲池"
d8e8      resstr(34356) = "分页缓冲池"
d904      atom(memory_paged)     ← 34358 "非分页缓冲池"
d92a      resstr(34358) = "非分页缓冲池"
d946      atom(memory_non_paged)
d974  memory_chart
d98e    atom(memory_chart)
d9b4      atom(memoryUsageTitle)      → 34360 "内存使用量"
d9e2      resstr(34360) = "内存使用量"
d9fe      atom(memory_size)
da22      atom(memoryUsageGraph)      ← 唯一的主曲线
da50      resstr(34355) = "使用中"     ← 曲线左下图例文字
da6c      atom(memoryUsageGraph_bottom)
daa8      atom(RateLabelMem)
dace      resstr(33818) = "0"          ← 纵轴底标签
daea      atom(memAvailabilityBar)     ← 内存组合条
db1c      rect(0rp,6rp,0rp,0rp)
db48      rect(0rp,0rp,0rp,14rp)       ← 条高 14rp
db76      resstr(33803) = "内存组合图表"  ← 无障碍名
db92      atom(BarTopLabel)
dbb6      rect(0rp,0rp,0rp,5rp)
dbe2      resstr(34361) = "内存组合"      ← 条的标题
```

同时 `DashboardStyle` 里还有一组组合条的段定义（文件开头 `835a`–`8614`）：

```
atom(BarchartMemoryHolder)        → gtc(CHARTVIEW, 9, 0, 3802) = #8B12AE
atom(BarchartInuse)               → gtc(TASKMANAGER, 21, 0, 3802)
resstr(33852) = "使用中"
atom(BarchartModified)            → gtc(TASKMANAGER, 23, 0, 3802), gtc(22,0,3802)
resstr(33841) = "已修改"
atom(BarchartStandby)             → gtc(TASKMANAGER, 20, 0, 3802)
resstr(33853) = "备用"
atom(BarchartFree)                → resstr(33854) = "可用"
```

**结构结论（已证实）：**
- 内存页 **只有 1 条主曲线**（`memoryUsageGraph`，「内存使用量」），**不是 2 条**。
- 主曲线之外**另有 1 个横向堆叠条**（`memAvailabilityBar` / 标题「内存组合」）。
- 堆叠条 **4 段**：`使用中` / `已修改` / `备用` / `可用`。
  **没有第 5 段「为硬件保留」** —— 这一点与原版 `wdc.dll`（资源监视器）的 5 段版本不同，
  不要照搬 resmon 的 5 段。
- 右侧信息区 = `dynamic_memory` 的 6 组 label+value：
  `使用中(已压缩)` / `可用` / `已提交` / `已缓存` / `分页缓冲池` / `非分页缓冲池`。
- `static_memory`（`d51e` 起）是另一组 5 项：
  `速度:` / `已使用的插槽:` / `外形规格:` / `为硬件保留的内存:` / `最大内存:`。

> 注：`resstr` 的配对在 duib 里是「元素名 → 紧接着的 resstr」的固定节奏，
> 但上面 `memory_inuse` 那一段的 resstr 顺序比元素名整体提前了一格
> （`memory_inuse` 后面跟的是 34352「可用」）。
> 这属于 duib 字符串池的排列习惯，**不影响「存在哪些元素」的判断**，
> 但「哪个 label 对哪个 resstr」在内存页这一段存在 ±1 的不确定性，
> 已用 MUI 全表兜底核对（6 个文案都能在 34349–34399 段里找到）。

### 2.2 数据内容

【E3，**已证实**】文案逐字（`_probe/mui_strings_all.txt`）：

| ID | 文本 | 用途 |
|---|---|---|
| 34360 | `内存使用量` | 曲线标题 |
| 34355 | `使用中` | 曲线内图例 |
| 34361 | `内存组合` | 堆叠条标题 |
| 33803 | `内存组合图表` | 无障碍名 |
| 33852 / 33841 / 33853 / 33854 | `使用中` / `已修改` / `备用` / `可用` | 条上 4 段 |
| 34399 | `使用中(已压缩)` | 动态信息第 1 项 |
| 34352 / 34354 / 34353 | `可用` / `已提交` / `已缓存` | 动态信息 2–4 |
| 34356 / 34358 | `分页缓冲池` / `非分页缓冲池` | 动态信息 5–6 |
| 34349 / 34350 / 34380 / 34381 / 34382 | `速度:` / `已使用的插槽:` / `外形规格:` / `为硬件保留的内存:` / `最大内存:` | 静态 5 项 |
| 34376 | `%1!s!/%2!s! MB` | 值格式（MB） |
| 34378 | `%1!s!/%2!s! GB` | 值格式（GB） |
| 34384 | `(%.0f%%)` | 百分比括号 |
| 34362 / 34363 | `%0.1f MB %s` / `%0.1f GB %s` | 带单位格式 |
| 34364 / 34365 | `%1.0f MB %s` / `%1.0f GB %s` | 另一种精度 |
| 34390 | `%1!s!/%2!s! TB` | TB |
| 34391–34394 | `%s MB` / `%s GB` / `%s TB` / `%s PB` | 通用单位后缀 |
| 33818 | `0` | 纵轴底标签 |
| 33825–33828 | `0%` ×4 | 百分比型纵轴底标签 |
| 33836 | `100%` | 百分比型纵轴顶标签 |

**进度条 / 堆叠条**：有，就是 `memAvailabilityBar`，高 **14 rp**（来自 `rect(0rp,0rp,0rp,14rp)`）。
样式判定：**纯色分段**（由 §1.6 的 `GdipSetSolidFillColor` 机制与 §1.3 的纯色填充共同支持），
**高概率无渐变**。

### 2.3 图形样式

| 项 | 结论 | 强度 |
|---|---|---|
| 主曲线颜色 | 主题值 `#8B12AE`；实际线色推测同为紫系（依 CPU 页规律可能略提亮） | 主题值 **已证实**；实际线色 **无法确定** |
| 曲线下方填充 | 有填充，**纯色**；CPU 页实测 `#F1F6FA` | CPU 页 **已证实**；内存页 **高概率同机制** |
| 填充是否渐变 | **不是**渐变（`CvXxx` 只导入 `GdipSetSolidFillColor`；像素上也只有一个填充值） | **已证实** |
| 网格 | 横 9 条 `#E8F0F9` + 竖 3 条 `#DEECF5` + 1px 边框 `#6AAED4` | CPU 页 **已证实**，四页共用渲染器 **高概率** |
| 纵轴 | 百分比型：顶部 `100%`(33836) / 底部 `0%`(33825–33828)；内存页底标签是 `0`(33818) | **已证实**（元素存在）；具体用哪种 **高概率** |
| 悬停气泡 | `CvHitSurface` / `CvSelection` 存在，**说明引擎支持命中与选中**；但没有任何「气泡/tooltip 文本」元素 | 能力 **已证实**；Win10 tm 具体是否启用 **无法确定** |

关于最后一条补充：duib 里 Barchart 各段都带 `accdesc` 指向 **tooltip 长文案**，
例如 MUI 的 33610–33617 就是堆叠条各段的悬停说明：

```
33610 已保留硬件(%1!s! MB) \n 已保留供 BIOS 和其他外设的某些驱动程序使用的内存
33611 使用中(%1!s! MB) \n 由进程、驱动程序或操作系统使用的内存 \n ... 使用中，已压缩(%2!s! MB) ...
33612 已修改(%1!s! MB) \n 内容必须先写入磁盘才能用于其他用途的内存
33613 备用(%1!s! MB) \n 包含未频繁使用的缓存数据和代码的内存
33614 可用(%1!s! MB) \n 当前未使用但会在进程、驱动程序或操作系统需要更多内存时首先重新使用的内存
33615 已缓存(%1!s! MB) \n ...
33616 可用(%1!s! MB) \n ...
33617 使用中(%1!s! MB) \n ... 压缩内存存储了估计 %3!s! MB 的数据，为系统节省了 %4!s! MB 的内存
```

→ **堆叠条各段有悬停 tooltip（带具体 MB 数值），已证实**。
→ 但「曲线图上有没有垂直高亮线 + 数值气泡」—— **无法确定**，文字资源里找不到对应文案。

### 2.4 与我方实现的差异（内存页）

| 项 | 原版 | 我方 | 判定 |
|---|---|---|---|
| 主曲线数量 | **1 条** | 2 条（`chart` + `chart2` 已提交） | **不符**（已证实） |
| 「已提交」位置 | 右侧 `dynamic_memory` 的一行 | 既画成第 2 条曲线、又画在右侧属性里 | **重复**（已证实） |
| 堆叠条段数 | 4 段 | 4 段 | 一致 |
| 段标签 | `使用中`/`已修改`/`备用`/`可用` | `使用中`/`已修改`/`备用`/`空闲` | **「空闲」应为「可用」**（已证实 33854=`可用`） |
| 段颜色 | 主题给 `#8B12AE` 家族 + `TaskManager` p20–23 | 深蓝/橙棕/浅蓝/灰 | **不符，但真实值无法确定**（见 §5.1） |
| 条高 | 14 rp | 22 px | 近似，可接受 |
| 条标题 | `内存组合` | `内存组合` | 一致 |
| 右侧属性 | 6 项动态 + 5 项静态 | 7 行 | 数量不符 |

---

## 3. 磁盘页

### 3.1 页面结构

【E2，**已证实**】DUI 元素树（`strings.txt:603-651`）：

```
dbfe  static_disk
dc16    atom(static_disk)
dc3a      rowlayout(3)               ← 3 行
dc54      atom(disk_capacity_Label)  → 34453 "容量:"
dc88      resstr(34453)
dca4      atom(disk_capacity)
dccc      atom(disk_formatted_label) → 34454 "已格式化:"
dd02      resstr(34454)
dd1e      atom(disk_formatted)
dd48      atom(disk_sys_label)       → 34455 "系统磁盘:"
dd72      resstr(34455)
dd8e      atom(disk_sys)
ddac      atom(disk_pagefile_label)  → 34456 "页面文件:"
dde0      resstr(34456)
ddfc      atom(disk_pagefile)
de24      atom(disk_type_label)      → 34451 "类型:"
de50      resstr(34451)
de6c      atom(disk_type)
de8c  dynamic_disk_server
deb4    atom(dynamic_disk_server)
dee8      atom(diskReadLegend)       → 34457 "读取速度"
df12      resstr(34457)
df2e      atom(disk_read)
df4e      atom(diskWriteLegend)      → 34458 "写入速度"
df7a      resstr(34458)
df96      atom(disk_write)
dfb8      resstr(34461) = "活动时间"
dfd4      atom(disk_time)
dff4      resstr(34459) = "平均响应时间"
e010      atom(disk_response)
e038  dynamic_disk_client            ← 客户端版本（同名一组）
e060    atom(dynamic_disk_client)
e094  disk_chart
e0aa    atom(disk_chart_client)
e0da      atom(diskTimeTitle)        → 34462 "100%"（顶标签）
e102      resstr(34462)
e11e      atom(diskTimeGraph)        ← 图 1：活动时间
e146      resstr(34653) = "磁盘处理读取或写入请求的时间百分比"
e162      atom(diskTime_bottom)
e18e      atom(RateLabelDiskTime)
e1be      resstr(33815) = "0"
e1da      atom(diskRateTitle)
e202      resstr(34460) = "磁盘传输速率"
e21e      atom(disk_rate_unit)
e248      atom(diskRateGraph)        ← 图 2：传输速率
e28c      atom(diskRate_bottom)
e2b8      atom(RateLabelDiskRate)
e2e8  disk_server_chart
e30c    atom(disk_chart_server)      ← 服务器版只有一个图
```

**结构结论（已证实）：**
- 磁盘页有 **两个图**：上「活动时间」（0–100%）、下「磁盘传输速率」（自动单位）。
- **有两套变体**：`disk_chart_client`（两张图）与 `disk_chart_server`（一张图）。
  这是原版为服务器 SKU 准备的分支；桌面 Win10 走 client 分支。
- 右侧属性：静态 5 项（容量 / 已格式化 / 系统磁盘 / 页面文件 / 类型），
  动态 4 项（读取速度 / 写入速度 / 活动时间 / 平均响应时间）。
  `rowlayout(3)` 是 **「每行 3 个元素」**（DirectUI 语义，不是「3 行」），
  5 组 label+value 共 10 个元素排成 4 行（3+3+3+1）。

> 修正说明：`rowlayout(N)` 在 DirectUI 里是 **N 列**。同理网络页 `rowlayout(4)`（8 组 → 2 行）、
> GPU 页 `rowlayout(5)`（5 组 → 1 行）。这一条是**高概率**（依据 DirectUI layout 语义 +
> 元素个数整除关系自洽），不是硬证据 —— 因为没有截图能验证排列结果。

### 3.2 按物理磁盘分实例 —— **是**

【E1，**已证实**（遥测事件名）；【E8，**已证实**（反汇编）】

mun 的 UTF-16 字符串里有整组 Dashboard 事件：

```
Dashboard_Disk_EnumerateDisks
Dashboard_AddDisk
Dashboard_RemoveDisk
Dashboard_DiskPerfIOCTL
Dashboard_DiskLengthIOCTL
```

反汇编里对应 `Dashboard_AddDisk` 的实现：
`0x1400C6340: mov rdx,[rdi+18h]` → `lea rcx,["disk_chart"]` → 构造实例名。
且 taskmgr 里存在**带下标的元素名模板**（`wstr_rdata.json`）：

```
gpu_chart_m_%d_%d_%d     gpu_chart_s_%d_%d_%d
gpu_entry_%d_%d_%d       gpu_graph_%d_%d_%d
sel_eng_%d_%d_%d         pri_eng_%d_%d_%d       mul_eng_%d_%d_%d
gpu_mem_chrt_%d_%d_%d    ded_mem_%d_%d_%d       dshr_mem_%d_%d_%d
```

→ **结论**：磁盘页**按物理磁盘分别建实例**，每块盘一棵 `disk_chart` 子树，
卡片标题形如 `磁盘 0` / `磁盘 1`（MUI 34463 `磁盘 %d`、34450 `磁盘 %1!d! (%2!s!)`）。
侧栏同样按盘分卡（truth1 里可见「磁盘 0」一张卡）。

**上限**：DUI 没有「最多 N 个」的常量；`Dashboard_AddDisk/RemoveDisk` 是增删事件，
配合 `dashSidebarScrollViewer`（`asneeded` 滚动条）→ **高概率不设上限，超出靠滚动**。
**无法确定**具体截断值。

### 3.3 数据内容

【E3，**已证实**】磁盘相关 MUI 全表：

| ID | 文本 | 用途 |
|---|---|---|
| 34453 / 34454 / 34455 / 34456 / 34451 | `容量:` / `已格式化:` / `系统磁盘:` / `页面文件:` / `类型:` | 静态 5 项 |
| 34457 / 34458 / 34461 / 34459 | `读取速度` / `写入速度` / `活动时间` / `平均响应时间` | 动态 4 项 + 图例 |
| 34460 | `磁盘传输速率` | 图 2 标题 |
| 34462 | `100%` | 图 1 顶标签 |
| 33815 | `0` | 图 1 底标签 |
| 34652 | `此磁盘上的读取和写入活动` | 图 2 无障碍描述 |
| 34653 | `磁盘处理读取或写入请求的时间百分比` | 图 1 无障碍描述 |
| 34469 / 34470 / 34471 / 34472 / 34473 / 34474 / 34477 | `HDD` / `SSD` / `持久性内存` / `可移动` / `SD` / `USB` / `未知` | 「类型」取值 |
| 34463 | `磁盘 %d` | 页标题 / 卡片标题 |
| 34450 | `磁盘 %1!d! (%2!s!)` | 带卷标的标题 |
| 34466 | `%s 毫秒` | 响应时间格式 |
| 34468 | `%.0f%%` | 活动时间百分比格式 |
| 34467 | `%1!s! %2!s! ` | 通用 值+单位 |
| 33000–33003 | `%I64d 字节/秒磁盘 I/O` … `GB/秒` | 无障碍 |
| 32840–32849 | `100 KB/秒` … `100 GB/秒` | 传输速率刻度分级 |
| 32850–32858 | `450 KB/秒` … `10 GB/秒` | 另一套刻度分级 |
| 32879–32882 | `字节/秒` / `KB/秒` / `MB/秒` / `GB/秒` | 单位后缀 |

**关键：传输速率图是「自动量级 + 单位后缀」**，不是固定百分比。
刻度分级表（32840–32858）就是自动缩放用的候选刻度值。

### 3.4 图形样式

同 §1.3–1.5 共用渲染器。磁盘特有的：

| 项 | 结论 | 强度 |
|---|---|---|
| 主色 | `#4DA60C`（主题 p16/p18/p19/p21） | **已证实** |
| 活动时间图纵轴 | 顶 `100%`、底 `0` | **已证实**（元素 34462 / 33815） |
| 传输速率图纵轴 | 顶 = 自动量级数值 + 单位；底 `0` | **高概率**（`disk_rate_unit` 元素 + 刻度表） |
| 是否有双色（读/写分开） | `diskReadLegend` / `diskWriteLegend` **是两个独立图例元素**，但只有 **2 个图**（时间 / 速率），没有第 3 个「读写分离」图 | 结构 **已证实**；两条图例到底配哪个图 **高概率**是速率图 |

---

## 4. 网络页

### 4.1 页面结构

【E2，**已证实**】DUI 元素树（`strings.txt:653-697`）：

```
e33c  static_network
e35a    atom(static_network)
e384      rowlayout(4)                 ← 4 行
e39e      atom(adapter_Label)  → 34549 "适配器名称:"
e3e2      atom(adapter)
e3fe      atom(wwan_Label)     → 34550 "服务提供商:"
e43c      atom(wwan)
e452      atom(wifi_Label)     → 34558 "SSID:"
e490      atom(wifi)
e4a6      atom(netname_Label)  → 34554 "DNS 名称:"
e4ea      atom(netname)
e506      atom(netType_label)  → 34551 "连接类型:"
e54a      atom(netType)
e566      atom(ipv4_Label)     → 34552 "IPv4 地址:"
e5a4      atom(ipv4)
e5ba      atom(ipv6_Label)     → 34553 "IPv6 地址:"
e5f8      atom(ipv6)
e60e      atom(signal_Label)   → 34564 "信号强度:"
e650      atom(signal)
e66a  dynamic_network
e68a    atom(dynamic_network)
e6b6      atom(netSendLegend)  → 34562 "发送"
e6de      resstr(34562)
e6fa      atom(netSend)
e716      atom(netReceiveLegend) → 34561 "接收"
e744      resstr(34561)
e760      atom(netReceive)
e782  net_chart
e796    atom(net_chart)
e7b6      atom(netChartTitle)   ← 标题（"吞吐量" 34560）
e7de      resstr(34560) = "吞吐量"
e7fa      atom(netChartMax)     ← 纵轴上限（动态数值，不是 100%）
e81e      atom(net_adapter)     ← 适配器名
e842      resstr(34654) = "此网络上的发送和接收活动"
e85e      atom(primaryNetChart_bottom)
e898      atom(RateLabelNet)
e8be      resstr(33819) = "0"
```

**结构结论（已证实）：**
- 网络页 **1 张吞吐量图**，标题 `吞吐量`。
- **纵轴顶是 `netChartMax`（动态数值）**，不是固定 `100%`；底是 `0`。
- **收发是两个独立数据系列**（`netSend` / `netReceive`），配两个图例（发送/接收）。
- 右侧属性 8 组：适配器名称 / 服务提供商 / SSID / DNS 名称 / 连接类型 / IPv4 / IPv6 / 信号强度。

### 4.2 按网卡分实例 —— **是**

【E1，**已证实**（遥测事件名）】

```
Dashboard_Network_Adapter_EnumerateAdapters
Dashboard_Network_Adapter_RefreshAdapters
Dashboard_Network_Adapter_WifiSetProperties
Dashboard_Network_Adapter_WWanSetProperties
Dashboard_AddAdapter
Dashboard_RemoveAdapter
```

truth1 的侧栏直接给了实据：**两张网络卡**「以太网 27」「以太网 45」并排显示，
每张卡 = 名称 + 类型（`以太网`）+ `发送: x Kbps 接收: ...`。

→ **网络页按适配器分实例，已证实**。上限同样**无法确定**（`asneeded` 滚动条，推测不截断）。

### 4.3 数据内容

【E3，**已证实**】

| ID | 文本 |
|---|---|
| 34549 / 34550 / 34558 / 34554 / 34551 / 34552 / 34553 / 34564 | `适配器名称:` / `服务提供商:` / `SSID:` / `DNS 名称:` / `连接类型:` / `IPv4 地址:` / `IPv6 地址:` / `信号强度:` |
| 34560 / 34561 / 34562 | `吞吐量` / `接收` / `发送` |
| 34568 | `未连接` |
| 34555 / 34557 | `域名:` / `设备名称:` |
| 34668–34679 | `网络使用率` / `链接速度` / `状态` / `发送字节吞吐量` / `接收字节吞吐量` / `字节吞吐量` / `已发送的字节` / … |
| 34693–34697, 34720 | `以太网` / `Wi-Fi` / `移动设备` / `Wi-Fi Direct` / `WLAN Direct` / `WLAN` |
| 32399 / 32398 | `以太网` / `蓝牙 PAN` |
| 32808–32815 | `1 bps` … `100 Gbps`（bps 刻度分级） |
| 32820–32829 | `100 Kbps` … `100 Gbps`（第二套分级） |
| 32875–32878 | `bps` / `Kbps` / `Mbps` / `Gbps` |
| 33004–33007 | `%I64d bps 网络 I/O` … `Gbps` |
| 33819 | `0`（纵轴底） |
| 34656 | `此蓝牙个人区域网上的发送和接收活动` |

**单位与小数位**：
- 吞吐量曲线 / 侧栏：`Kbps` / `Mbps`，**一位小数**（truth1 侧栏实测 `1533.3 Kbps`、`0.0 Kbps`）。
- 进程页网络列：`37034 %s Mbps`。

### 4.4 图形样式

| 项 | 结论 | 强度 |
|---|---|---|
| 主色 | `#A74F01`（主题 p23/p25） | **已证实** |
| 收发双色 | mun 里有两张**具名** PNG：`IDB_PERFTAB_NET_RECEIVE_COLORKEY` = `#B15C11`、`IDB_PERFTAB_NET_SEND_COLORKEY` = `#DC652B`（各 2×32 px，纯色 + 透明列，是「色键条」不是渐变） | **已证实** |
| 所以：网络图是**双色** | 接收 `#B15C11`、发送 `#DC652B`，叠加在同一张图上 | **高概率** |
| 主题里的第 2/3 色 | `ChartView` 网络组 p26 `#E0822F` / p27 `#DF672C` 是另一对橙；与上面两张 PNG 的色**不相等** | **已证实（主题值）**；哪一对真正上屏 **无法确定** |
| 填充 | 同 §1.3，纯色 | CPU 页 **已证实**；网络页 **高概率** |
| 纵轴 | 顶 = `netChartMax` 数值（bps/Kbps/Mbps/Gbps 自适应），底 = `0` | **已证实** |
| 是否有「量程」字样 | **没有**。MUI 848 条里 `量程` 零命中 | **已证实（否定）** |

> **重要修正**：这两张 2×32 PNG **不是渐变条**。逐像素解码后：
> `NET_RECEIVE` 是 2×32 全 `#B15C11`（RGB，无 alpha）；
> `NET_SEND` 是 2×32，左列 `#DC652B`/a255、右列 `#FFFFFF`/a0（透明）。
> 即它们是**纯色「色键」样本**（资源名里 `COLORKEY` 就是这个意思），
> 用途是给绘制代码提供一个可被抠色/取样的颜色基准。
> 所以「网络图是双色」的依据是**这两张图的存在 + 两个独立图例元素**，
> 而不是「发现了两条渐变」——后者是我在初稿里写错的，已删除。

> 我方实现的问题（与 §roast2 N1 一致，本轮独立复核确认）：
> - 我方画的是**单条** `#A74F01` 曲线，旁边摆了两个**不出现在图里**的色块
>   `#B15C11` / `#DC652B`。原版这两个颜色是 `IDB_PERFTAB_NET_*_COLORKEY` 两张**具名色键 PNG** 的值，
>   配合 `netSendLegend` / `netReceiveLegend` 两个**独立**图例元素
>   → 原版**极可能真的画两条线**（接收 `#B15C11`、发送 `#DC652B`）。
>   我方应改为**真正画两条线**，图例才名副其实。
> - 我方纵轴打 `100% / 0%` 并加了一行 `量程 N Mbps`；原版是**上限数值 + 0**，且没有「量程」二字。
>
> **但要注意**：`netSend` / `netReceive` 在 DUI 里位于 `dynamic_network`（右侧数字列）内部
> （`e6b6`–`e760`），**不在** `net_chart` 子树里。所以「两条线」的具体画法**未证实** ——
> 也有可能是「一条线 + 右侧两行收发数字 + 图例只是数字的图例」。
> 这一点请在动手改之前先确认（见 §6.7）。

---

## 5. GPU 页

### 5.1 **GPU 页真实存在 —— 确证，不是我方自创**

这是本任务要回答的头号问题。**五重独立证据**：

【证据 1｜E2，**已证实**】DUI 里有完整的 GPU 子树（`strings.txt:698-762`）：

```
e8da  static_gpu
e8f0    atom(static_gpu)  → rowlayout(5)
e912      gpudriverversion_label → 41001 "驱动程序版本:"
e982      gpudriverversion
e9b0      gpudriverdate_label    → 41002 "驱动程序日期:"
ea00      gpudriverdate
ea28      gpudxversion_label     → 41003 "DirectX 版本:"
ea76      gpudxversion
ea9c      gpulocation_label      → 41032 "物理位置："
eae8      gpulocation
eb0c      gpuhwreservedmem_label → 41033 "为硬件保留的内存："
eb62      gpuhwreservedmem
eb90  dynamic_gpu
eba8    atom(dynamic_gpu)
ebcc      atom(gpuUsage)
ebea      atom(gpuTotalMemory)
ec14        atom(totalMemory)  → 41011 "GPU 内存"
ec38        resstr(41011)
ec54        atom(memValue)
ec72      atom(gpuDedicatedMemRoot)
eca6        atom(gpuDedicatedMemLabel) → 41034 "专用 GPU 内存"
ecdc      atom(gpuSharedMemRoot)
ed26        atom(gpuSharedMemLabel)    → 41035 "共享 GPU 内存"
ed56      atom(gpuTemperature)
ed9c        atom(gpuTemperatureValue)
edd0  gpu_chart
ede4    atom(gpu_chart)
ee04      atom(gpuPrimaryCharts)
ee32      atom(gpuMemoryCharts)
ee5e      rect(0rp,5rp,0rp,5rp)
ee8a  singleEngineGpuView
eeb2    atom(singleEngineGpuView)   → 41027 "总体 GPU 图表窗格"
eee6      resstr(41027)
ef02      atom(topArea)
ef1e        atom(engineSelector)   ← 引擎下拉选择器
ef48          dtb(ItemsView::Header, 4, 2)
ef82        atom(engineTitle)      → 34655 "GPU 活动"
efa6        atom(engineUtilizationRate)
efde        atom(engineChart)      ← 单引擎图
f002        resstr(34655)
f01e  multiEngineGpuView
f044    atom(multiEngineGpuView)   → 41028 "多个引擎 GPU 图表窗格"
f076      resstr(41028)
f092      atom(gpuGrid)
f0ae        atom(gpuEngineChartTemplate)  ← 引擎图模板
f0dc          atom(gpuEngineChart)
f106  gpuMemoryChart
f124    atom(gpuDedicatedMemory)
f156      atom(gpuMemUsageTitle)  → 41012 "专用 GPU 内存利用率"
f184      resstr(41012)
f1a0      atom(gpuMemoryMax)
f1c6      atom(gpuMemUsageGraph)
f1f4      resstr(41014)
f210    atom(gpuSharedMemory)
f23c      atom(gpuMemUsageShared)
f26c      resstr(41013) → "共享 GPU 内存利用率"
f288      resstr(41015)
```

【证据 2｜E1，**已证实**】mun 的 UTF-16 字符串里同样有整套 GPU 元素名：
`gpuchart` / `atom(dashSidebarGpuEntry)` / `atom(sidebar_gpu_util)` /
`atom(sidebar_gpu_temp)` / `static_gpu` / `dynamic_gpu` / `singleEngineGpuView` 等 51 处 `gpu` 相关串。

【证据 3｜E5，**已证实**】原版截图 `truth1.png` / `truth2.png` 侧栏**第 6 张卡片**就是
`NVIDIA GeFor…` + `0%  2.5 GB`，mini 图边框为 `#117DBB`。
——**这是「原版有 GPU 页」最直白的证据，亲眼可见。**

【证据 4｜E3，**已证实**】MUI 有一整段 41000–41042 GPU 文案（见 §5.3 全表），
`33860 = GPU`（侧栏卡片标题）。没有 GPU 页就不需要这些字符串。

【证据 5｜E7/E9，**已证实**】taskmgr.exe 导入 `d3d11.dll` / `d3d12.dll` / `dxgi.dll` / `dxcore.dll`，
`\GPU Engine(*)\Utilization Percentage`、`\GPU Engine(*)\Running Time`、
`\GPU Process Memory(*)\Dedicated Usage`、`\GPU Adapter Memory(*)\Shared Usage` 等
PDH 计数器路径齐全；元素名模板 `gpu_chart_m_%d_%d_%d` / `gpu_chart_s_%d_%d_%d` /
`sel_eng_` / `pri_eng_` / `mul_eng_` / `ded_mem_` / `dshr_mem_` 全在。

**结论：Windows 10 2004（10.0.19041.7291）的 taskmgr 有 GPU 页。
我方 GPU 页不是自创，是对原版功能的复刻。**
（历史上 GPU 页是 Win10 1709 加入的，19041 早已具备。）

### 5.2 是否分引擎 —— **是，且有两种视图**

【E2 + E9，**已证实**】

- `singleEngineGpuView`（单引擎视图）：带 **`engineSelector`**（引擎下拉框，
  样式 `dtb(ItemsView::Header, 4, 2)`）、`engineTitle`、`engineUtilizationRate`、`engineChart`。
- `multiEngineGpuView`（多引擎视图）：`gpuGrid` + `gpuEngineChartTemplate`，
  用模板为**每个引擎**生成一格 `gpuEngineChart`。
- 菜单里对应两个命令（`strs.txt`）：**`Multiple engines` / `Single engine`** ——
  这是原版性能页右键/查看菜单的选项，用户可切换。

**引擎名（已证实，来自本机真实计数器枚举 `_probe/gpu_engine_counters.txt`）**：

| 计数器里的 `engtype_` | 显示名 | 依据 |
|---|---|---|
| `3D` | `3D` | MUI/strs `3D` |
| `Copy` | `Copy` | 同上 |
| `VideoDecode` | `Video Decode` | `engtype_` 前缀 + 空格化 |
| `VideoEncode` | `Video Encode` | 同上 |
| `LegacyOverlay` | `Overlay` | `strs.txt` 有 `Overlay` |
| `Security` / `Security_1` | `Other` | `strs.txt` 有 `Other` |
| `VR` | （无独立显示名，归 `Other`） | 推测 |
| `Compute` | `Compute` | `strs.txt` 有 `Compute`；MUI 45000 `计算加速器` 是另一套（WDDM 2.x 计算加速器） |

`_probe/gpu_engine_counters.txt` 里实测出现的 engtype：
`3D` / `Copy` / `VideoDecode` / `VideoEncode` / `LegacyOverlay` / `Security` / `Security_1` / `VR`
（本机 discrete GPU 共 20+ 个引擎实例）。

**「最多显示几个引擎」—— 无法确定**：`multiEngineGpuView` 是模板 + 网格（`gpuGrid`），
理论上是「有几个显示几个」，网格按可用空间排布；但单元格最小尺寸、是否截断，
反汇编里没找到明确常量。

### 5.3 GPU 文案全表

【E3，**已证实**】

| ID | 文本 |
|---|---|
| 41000 | `适配器名称:` |
| 41001 / 41002 / 41003 | `驱动程序版本:` / `驱动程序日期:` / `DirectX 版本:` |
| 41032 / 41033 | `物理位置：` / `为硬件保留的内存：` |
| 41004 / 41006 | `利用率` / `% 利用率` |
| 41005 | `%1!d!%2!s!` |
| 41007 / 41008 | `GPU %d` / `GPU %d (链接 %d)` |
| 41009 / 41010 | `100%` / `0` |
| 41011 | `GPU 内存` |
| 41012 / 41013 / 41014 / 41015 | `专用 GPU 内存利用率` / `共享 GPU 内存利用率` / 同上（重复） |
| 41016 | `%.0f%%` |
| 41017 / 41018 / 41019 / 41020 | `GPU` / `GPU 引擎` / `GPU` / `GPU 引擎` |
| 41021 / 41022 | `专用 GPU` / `GPU 系统` |
| 41023 / 41024 | `所有 GPU 引擎的最高利用率` / `利用率最高的 GPU 引擎` |
| 41027 / 41028 | `总体 GPU 图表窗格` / `多个引擎 GPU 图表窗格` |
| 41034 / 41035 | `专用 GPU 内存` / `共享 GPU 内存` |
| 41036 / 41037 | `电源使用情况` / `电源使用情况趋势` |
| 41038 / 41039 | `CPU、磁盘和 GPU 对功耗的影响` / `…的影响` |
| 41040 / 41041 / 41042 | `GPU 温度` / `%d °C` / `(%d °C)` |
| 39009 | `所有 GPU 引擎的最高利用率` |
| 33860 | `GPU`（侧栏卡片标题） |
| 34655 | `GPU 活动` |

### 5.4 图形样式（GPU）

| 项 | 结论 | 强度 |
|---|---|---|
| 主色 | `#117DBB`（与 CPU 同色） | **已证实**（主题查不到 GPU 独立组，最常见做法是复用 CPU 蓝；且 truth1 侧栏 GPU 卡实测 `#117DBB`） |
| 内存子图 | `gpuMemoryChart` 下分「专用 GPU 内存」`gpuMemUsageGraph` 与「共享 GPU 内存」`gpuMemUsageShared` **两张图** | **已证实** |
| 主图 | `gpuPrimaryCharts`（每个引擎一格）+ `gpuMemoryCharts`（专用/共享两张） | **已证实** |
| 纵轴 | 引擎图 `0`–`100%`（41010 `0` / 41009 `100%`） | **已证实** |
| 填充 | 同 §1.3，纯色 | **高概率** |
| 温度 | `gpuTemperature` / `gpuTemperatureValue`，格式 `%d °C` — **原版有 GPU 温度显示** | **已证实**（元素 + 文案都在） |
| 「GPU 0」标题 | 侧栏/标题 `GPU %d`（41007），多适配器时 `GPU %d (链接 %d)`（41008） | **已证实** |

> **重要提示**：原版 GPU 页**有温度**（41040/41041/41042），
> 我方 GPU 页正文里没有温度字段 → 这是一个可见缺口。

### 5.5 与我方实现的差异（GPU）

| 项 | 原版 | 我方 | 判定 |
|---|---|---|---|
| 是否存在 GPU 页 | **存在** | 存在 | 一致（我方不是自创） |
| 主色 | `#117DBB` | `#117DBB` | 一致（已证实） |
| 引擎数量 | 模板动态生成，**有几个画几个**；另有单引擎视图 + 下拉选择器 | 写死 4 个（3D/Copy/Video Encode/Video Decode） | **不符**（已证实） |
| 引擎选择器 | 有 `engineSelector`（下拉） | 无 | **缺** |
| 单/多引擎切换 | 菜单 `Single engine` / `Multiple engines` | 无 | **缺** |
| GPU 温度 | 有（**°C**） | 无 | **缺** |
| 专用/共享内存 | 两张独立小图 | 未确认 | 待核 |
| 侧栏卡片内容 | 名称 + `util%` + `总内存`（truth1 实测 `0% 2.5 GB`） | 未确认 | 待核 |

---

## 6. 无法确定清单 + 建议

### 6.1 内存组合条的真实配色 —— **无法确定**

- 主题 `TaskManager` p20–23 给的是 `#FFFFFF` / `#F4F2F4` / `#EEE6F1` / `#CEB0D7`（一组极淡的紫灰），
  `ChartView p9–14`（内存组）另给 `#8B12AE` / `#ECDEF0` / `#9528B4` / `#817285`（紫系）。
- 但原版 `wdc.dll` 的 resmon 堆叠条是 `#8FB80C`(绿) / `#FF9900`(橙) / `#558ED5`(蓝) / `#C6D9F1`(浅蓝) / `#CBCBCB`(灰)。
- 三套证据互相矛盾，且**没有内存页截图**。
- 我方现用：深蓝 `#0A3E70` / 橙棕 `#C25B36` / 浅蓝 `#7FBAE0` / 灰 `#D8D8D8`。

**建议：保持现状。** 理由：这三套证据没有一套能被判定为「任务管理器内存页实测」；
我方现有配色至少是「蓝→橙→浅蓝→灰」的常规语义（与 resmon 的语义排序一致），
贸然改成主题那组淡紫会变成整条几乎全白，反而更不像。
**若将来能拿到一张内存页截图，按像素重定即可 —— 其它结论都不依赖这一项。**

### 6.2 曲线「画出来的实际线色」是否等于主题色 —— **无法确定（只有 CPU 页能证）**

- CPU 页实测：主题 `#117DBB`，**实际画的是 `#1B82C2`**（差一档亮度）。
- 内存/磁盘/网络/GPU 的实际线色**没有任何截图可验**。
- 我方现状：CPU 网格用 `#1B82C2`（对），四页主曲线用主题原值 `#8B12AE` / `#4DA60C` / `#A74F01` / `#117DBB`。

**建议：保持现状。** 理由：
1. 主题原值是四页主色**唯一有硬证据的取值**（E4）；
2. CPU 的偏差只有一档（≈ 6% 提亮），肉眼几乎不可分辨，
   而且我方 CPU 页已经用对了 `#1B82C2`；
3. 若要「为了统一而全部提亮」，反而是把三个**已证实**的值改成**推测**的值。
→ 保持 `#8B12AE` / `#4DA60C` / `#A74F01` / `#117DBB` 不动。

### 6.3 曲线图上是否有「垂直高亮线 + 数值气泡」 —— **无法确定**

- 引擎**支持**（`CvHitSurface::BeginElement` / `CvSelection`），
  且堆叠条**确实有**悬停 tooltip（MUI 33610–33617 带 MB 数值）。
- 但曲线图上找不到任何 tooltip 文案，也没有对应的 DUI 元素。
- 参考图 truth1/truth2 是静止截图，看不到悬停态。

**建议：不要实现曲线上的气泡。** 理由：无证据；且 Win10 任务管理器性能页的通行印象是
「图上没有 tooltip，只有鼠标移上去光标变成十字、左上角数值跟着变」——
这属于**推测，不要当规格用**。保持现状（无气泡）风险最低。

### 6.4 磁盘 / 网络实例数量上限 —— **无法确定**

- `asneeded` 滚动 + `AddDisk`/`RemoveDisk`/`AddAdapter`/`RemoveAdapter` 增删事件，
  结构上**支持不设上限**。
- 但「超过 N 个之后是否折叠/滚动/截断」没有常量可查。

**建议：按「不截断 + 侧栏可滚动」实现。** 理由：这正是 DUI 结构直接支持的行为，
且 truth1 已出现 2 张网络卡并排（说明至少 2 个不折叠）。

### 6.5 GPU 引擎数量上限 / 网格排布规则 —— **无法确定**

**建议**：按「模板复制、有几个画几个、网格自适应」实现，不要写死 4 个。
理由：`gpuEngineChartTemplate` + `gpuGrid` 的结构直接支持这一点；
而写死 4 个会漏掉 `Overlay` / `Security` / `VR` / `Compute` 等真实存在的引擎
（本机实测就有 8 种 engtype）。

### 6.6 内存页堆叠条与主曲线的上下顺序 —— **高概率但非确证**

- duib 里 `BarchartMemoryHolder` 出现在文件最前（`835a`），`memory_chart` 在中后（`d974`）；
  而 `memAvailabilityBar` 是 `memory_chart` 内、`memoryUsageGraph` **之后**的兄弟节点。
- 两处都指向「**曲线在上、组合条在下**」。

**建议：曲线在上、组合条在下**（我方现状即如此）。强度：高概率。

### 6.7 网络图到底是「双线叠加」还是「单线 + 两行数字」 —— **无法确定**

这是本轮唯一发现「我方与 roast2 原报告都可能判错」的地方，必须写清楚：

- `netSend` / `netReceive` 位于 **`dynamic_network`** 内（`e6b6`–`e760`），
  这是**右侧数字列**的分组，不是 `net_chart` 的子节点。
- `net_chart`（`e782`–`e8be`）内部只有：`netChartTitle` / `netChartMax` /
  `net_adapter` / `primaryNetChart_bottom` / `RateLabelNet` / `resstr(33819)="0"` ——
  **没有一个「series」或「第二条曲线」元素**。
- 但 mun 里确实有两张具名 `IDB_PERFTAB_NET_RECEIVE_COLORKEY` / `..._SEND_COLORKEY` 色键 PNG，
  且 `netSendLegend` / `netReceiveLegend` 是**两个独立图例元素**。

两种读法都自洽：
1. **双线叠加**：一张图里两条线，色键 PNG 给两条线定色，两个图例标注；
2. **单线 + 双数字**：图上一条「总吞吐量」线（`#A74F01` 棕橙），
   右侧 `dynamic_network` 两行分别显示发送/接收数字，两个图例颜色只是给**数字行**上色。

**建议：先不动**。理由：改错方向的代价比不改高 ——
若改成双线而后验证是单线，就凭空多出一条曲线（和内存页「已提交」那条错曲线同类）。
**若要动，先解决这个判定**，而判定需要一张网络页截图，目前拿不到。

### 6.8 磁盘「读取/写入」是不是两条线 —— **结构上更可能是图例 + 数字对**

`diskReadLegend` / `diskWriteLegend` 同样位于 `dynamic_disk_server` /
`dynamic_disk_client` 内（`dee8`–`dff4`），**不在** `disk_chart` 里。
而磁盘页明确只有 **2 张图**（`diskTimeGraph` 活动时间、`diskRateGraph` 传输速率），
**没有第 3 张「读写分离」图**。→ 读写更可能是 **传输速率图上的两条线** 或 **图例 + 两行数字**，
与 §6.7 是同一个未决问题，建议同样先不动。

---

## 7. 四条主色的最终裁定（本任务的核心待验项）

| 页 | 我方常量 | 主题 `ChartView` | 截图像素 | 结论 |
|---|---|---|---|---|
| 内存 | `#8B12AE` | p9 = `#8B12AE` | 侧栏 mini 图 `#8B12AE`（145 px） | **已证实正确** |
| 磁盘 | `#4DA60C` | p16 = `#4DA60C` | 侧栏 mini 图 `#4DA60C`（139 px） | **已证实正确** |
| 网络 | `#A74F01` | p23 = `#A74F01` | 侧栏 mini 图 `#A74F01`（81/136 px） | **已证实正确** |
| GPU | `#117DBB` | 无独立组（复用 CPU 蓝） | 侧栏 mini 图 `#117DBB`（65 px） | **已证实正确** |
| CPU（对照） | `#1B82C2`（线条） | p2 = `#117DBB` | 主图线 `#1B82C2`（4222 px） | 线条色实测一致；主题色是图标色 |

**四条主色全部证实，一项都不用改。** 唯一遗留是 §6.2 说的「线色比主题色略亮」这一档偏差，
在无截图的三页上无法判定，故维持。

---

## 8. 一句话总结

- **GPU 页真实存在**，五重独立证据（DUI 子树 / mun 字符串 / 原版截图侧栏 / MUI 41000 段 / PDH 计数器 + d3d 导入），
  **不是我方自创**；还带引擎下拉、单/多引擎视图、GPU 温度这些我方缺的东西。
- **四条主色 `#8B12AE` / `#4DA60C` / `#A74F01` / `#117DBB` 全部证实正确**，
  主题表和原版截图互相印证。
- **图是 CHARTV.dll 画的**：只有折线 + **纯色**填充（`GdipSetSolidFillColor`，无渐变），
  横 9 条 `#E8F0F9` 竖 3 条 `#DEECF5`，1px `#6AAED4` 边框 —— 与我方实现逐像素一致。
- **内存页只有 1 条曲线 + 1 个 4 段组合条**（无「为硬件保留」段），我方多画了一条「已提交」曲线，是真错。
- **磁盘/网络都按实例分卡**（`AddDisk`/`AddAdapter`），不设上限、靠滚动。
- **网络/磁盘的收发（读写）配色是双色的**（网络接收 `#B15C11` / 发送 `#DC652B`，
  来自 mun 里两张具名 `COLORKEY` PNG）。但「图上是两条线还是单线 + 两行数字」**没定**，
  因为我方现在是「单线 + 一对不上屏的色块图例」——**这个图例问题是真的**，
  但修法有两种，别急着改成双线（见 §6.7）。
- 真正**无法确定**的只有 5 项：内存组合条配色、曲线是否带悬停气泡、实例数量上限、
  GPU 引擎上限、网络/磁盘到底双线还是单线。均已给出「保持现状 / 按 XX 实现」的建议。

---

状态：已完成
