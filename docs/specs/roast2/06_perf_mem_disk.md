# 锐评 06 — 性能页 内存页 / 磁盘页

已完成 · 28 条发现（10 严重 / 13 明显 / 2 像素级 / 3 证据不足未上报）

## 证据基础与参考图盘点（先说清楚我能拿到什么）

| 素材 | 内容 | 是否可用于内存/磁盘页逐像素比对 |
|---|---|---|
| `shots/truth1.png` (1100×700) | **原版 CPU 页整窗** | 只能比侧栏卡片和标题区 |
| `docs/evidence/README.md` (3300×2100) | **原版 CPU 页整窗 3×**（与 truth1 同一帧放大） | 侧栏几何/配色的最佳来源 |
| `docs/evidence/README.md` | 原版侧栏 4× | 侧栏细节 |
| `docs/evidence/README.md` | 原版内嵌 16×16 图标 | 只证明侧栏主色 |
| `docs/evidence/uifile_Taskmgr_30001_strings.txt` | **taskmgr.exe 真实 DUI 元素树（未编译）** | 元素 ID / 层级 / 文案归属的硬证据 |
| `docs/evidence/mui_strings_all.txt` | 848 条 MUI 字符串 | 文案硬证据 |
| `docs/evidence/themedump2_out.txt` | TaskManager / ChartView 主题 part 实时色 | 主色硬证据 |
| `shots/audit_mem.png` `shots/fin_memory.png` `shots/p_disk.png` `shots/rt2_perf.png` | **这些全部是我方重实现自己的离屏截图** | ❌ 不能当原版 |

**结论：我没有原版「内存页」和「磁盘页」的整页截图。** 唯一覆盖侧栏（内存/磁盘卡片）的真图是
truth1 / orig_full（CPU 页选中态，但侧栏五张卡片全在同一帧里）。
所以：

- **侧栏卡片**（内存卡、磁盘卡）—— 有逐像素证据，可以硬比。
- **内存页右侧主区、磁盘页右侧主区** —— **原版参考缺失，无法逐像素比对**。
  这一块我只做「源码 vs DUI 元素树 / MUI 文案」的结构性与文案级比对，
  凡是涉及像素位置/颜色的都标注 `未证实`，不编坐标。

---

## 一、结构级发现（DUI 元素树 vs 源码，证据充分）

### M1【严重】内存页图表数量：实现画了 2 条曲线，但原版曲线只有 1 条 + 1 条柱

**问题**：`PaintMemPage` 先画「内存使用率」曲线，再画「已提交」曲线，两条都是
`PaintChart`（折线 + 100%/0% 标注）。

**原版**（`docs/evidence/uifile_Taskmgr_30001_strings.txt` 行 586–602，taskmgr.exe 自带 DUI 树）：

```
d974  memory_chart
d98e    atom(memory_chart)
d9b4      atom(memoryUsageTitle)      → resstr(34360) = "内存使用量"
da22      atom(memoryUsageGraph)      → resstr(34355) = "使用中"
da6c      atom(memoryUsageGraph_bottom) → atom(RateLabelMem) → resstr(33818) = "0"
daea    atom(memAvailabilityBar)      → resstr(33803) = "内存组合图表"
db92      atom(BarTopLabel)           → resstr(34361) = "内存组合"
```

内存页**只有一个 `<graph>` 元素**（`memoryUsageGraph`），
下面紧跟的是 `memAvailabilityBar`（内存组合条），**没有第二条曲线**。
（对比：`wdc` 的 resmon 内存页有 memory/pagefile/hardfault **三条**
曲线，但那不是任务管理器，`win10_perf_visual_spec.md` §3.1 自己已经在
注释里承认这一点。）

**重实现**：`src/perfpage.cpp:1027` 起 `chart`（内存使用率），
`src/perfpage.cpp:1042` 起 `chart2`（已提交），两条都是 `PaintChart`。

**差多少**：多一条完整曲线图（含自己的 `100%`/`0%` 刻度、网格、独立底色）。
按 `src/perfpage.cpp` 的布局算式，chart 高 105 + 间距 24 + chart2 高 90 = **219px**
的多余图表带。

**严重度：严重**（结构不符，页面上凭空多出一个原版没有的图表）

---

### M2【明显】「已提交」不该是第二条曲线，它是右侧属性行

**问题**：实现把「已提交  X / Y」既画成第二条曲线的标题
（`src/perfpage.cpp:1048-1055`），又在右侧属性列表里画一遍
（`src/perfpage.cpp:1163`），造成**同一指标在页面上出现两次**。

**原版**：`memory_commit` 只出现在 `dynamic_memory` 分组里
（`docs/evidence/uifile_Taskmgr_30001_strings.txt:578-579`）：

```
d862  resstr(34354) = "已提交"
d87e  atom(memory_commit)
```

且 `dynamic_memory` 整组是**右侧属性区**，不隶属于 `memory_chart`。

**差多少**：重复展示一项指标。

**严重度：明显**

---

### M3【严重】内存组合条：颜色全错，且缺「为硬件保留」段

**问题**：`src/perfpage.cpp:1095-1100` 的四段配色：

```cpp
{ m.inUseBytes,     RGB(0x0A,0x3E,0x70), L"使用中" },   // 深蓝
{ m.modifiedBytes,  RGB(0xC2,0x5B,0x36), L"已修改" },   // 橙棕
{ m.standbyBytes,   RGB(0x7F,0xBA,0xE0), L"备用" },     // 浅蓝
{ idleBytes,        RGB(0xD8,0xD8,0xD8), L"空闲" },     // 灰
```

**原版**（两条独立硬证据，互相印证）：

证据 A —— `docs/evidence/uifile_wdc_30003.xml:145-163`
（`wdc.dll.mun` UIFILE 30003 `<style resid="barchart">`，逐字 RGB）：

| 段 | class | 实测色 |
|---|---|---|
| 为硬件保留 | `reserved` | `#CBCBCB` → `#A6A6A6` |
| 使用中 | `inuse` | **`#8FB80C`** → `#7EA20A`（橄榄绿） |
| 已修改 | `modified` | **`#FF9900`** → `#CC7900`（高饱和橙） |
| 备用 | `standby` | **`#558ED5`** → `#3174C5`（中蓝） |
| 可用/空闲 | `free` | **`#C6D9F1`** → `#97BAE5`（浅蓝，不是灰） |

证据 B —— taskmgr 自己的 DUI 树用的是**主题 part**，
`docs/evidence/uifile_Taskmgr_30001_strings.txt:1-17`：
`BarchartMemoryHolder → gtc(CHARTVIEW,9,0)`、`BarchartInuse → gtc(TASKMANAGER,21,0)`、
`BarchartModified → gtc(TASKMANAGER,23,0)` / `gtc(TASKMANAGER,22,0)`、
`BarchartStandby → gtc(TASKMANAGER,20,0)`、`BarchartFree → resstr(33854)`。
`docs/evidence/themedump2_out.txt` 实时查询结果：

```
TASKMANAGER(21,0,3802) = #F4F2F4   ← BarchartInuse
TASKMANAGER(23,0,3802) = #CEB0D7   ← BarchartModified 渐变A
TASKMANAGER(22,0,3802) = #EEE6F1   ← BarchartModified 渐变B
TASKMANAGER(20,0,3802) = #FFFFFF   ← BarchartStandby
CHARTVIEW (9,0,3802)   = #8B12AE   ← BarchartMemoryHolder（条的外框/底色）
```

⚠️ 这里有个**必须说明的矛盾**：taskmgr 的主题 part 值是
`Inuse=#F4F2F4`（近乎白）、`Modified=#EEE6F1/#CEB0D7`（淡紫）、`Standby=#FFFFFF` —
**全是紫白色系**，而 `wdc` 里是绿/橙/蓝色系。
两个来源不可能同时是「任务管理器内存页的实际像素」。
`shots/` 里所有「内存组合」截图（`audit_mem.png` 等）都是**我方自己的输出**，
而且我方输出的是深蓝/橙棕/浅蓝/灰 —— 与两边都不一致，**无法用真图裁决**。

因此我给出的是**有条件结论**：
- **可以确定的**：我方那四个色（`#0A3E70` 深蓝 / `#C25B36` 橙棕 / `#7FBAE0` 浅蓝 / `#D8D8D8` 灰）
  **既不等于 wdc 的资源字面值，也不等于 taskmgr 的主题 part 值**，三套色系互不相同。
  也就是说实现里的注释「各段颜色取自规格 §3.2 的实测值」，**这个说法不成立** ——
  它抄的是 `perf_page_spec.md` 4.2 的散文描述（那份文档已被
  `win10_perf_visual_spec.md` §3.2 明确纠正过：「使用中不是深蓝，是橄榄绿」）。
- **缺段**：`wdc` 的条有 **5 段**，最左是「为硬件保留」，实现只有 **4 段**，
  完全没有 reserved。taskmgr 自己的 DUI 树也**没有** `BarchartReserved`
  （grep 计数 = 0），所以任务管理器究竟画不画这一段，**未证实**；
  但实现的注释「Win10 的内存组合是五段」与它实际只画四段自相矛盾。

**差多少**：至少 4 个颜色值与资源字面值不符（红/绿/蓝通道全不同量级）；
段数 4 vs 5（是否该 5 段未证实）。

**严重度：严重（配色）/ 明显（段数）**

---

### M4【明显】「内存组合」条高度 22px，资源写死 24rp

**问题**：`src/perfpage.cpp:1084`
```cpp
RECT bar{ rc.left + 8, barTop, rc.right - 8, barTop + 22 };
```

**原版**：`docs/evidence/uifile_wdc_30003.xml:301-306`，`BarchartReserved` /
`BarchartInuse` / `BarchartModified` / `BarchartStandby` / `BarchartFree`
**五处全部写死 `height="24rp"`**。taskmgr DUI 树里
`atom(BarchartMemoryHolder)` 的高度是 `52rp`（`docs/evidence/uifile_Taskmgr_30001_strings.txt:4`），
即条本身 24 + 上下留白。

**差多少**：22 vs 24 → **少 2px**。

**严重度：像素级**（但 24rp 是资源里唯一的硬数字，没理由不照抄）

---

### M5【明显】图例色块 10×10，资源写死 14rp×14rp

**问题**：`src/perfpage.cpp:1118`
```cpp
RECT box{ lx, ly + 3, lx + 10, ly + 13 };   // 10×10
```

**原版**：`docs/evidence/uifile_wdc_30003.xml:324 / 334 / 344 / 354 / 364`，
五个图例色块全部 `width="14rp" height="14rp"`，且 `borderthickness="rect(1rp,1rp,1rp,1rp)"`
（**有 1px 边框**）。

**差多少**：宽度少 4px、高度少 4px（面积只有原版的 51%），并且**完全没有边框**。

**严重度：明显**

---

### M6【明显】磁盘页标题栏拼的是「SSD」+「活动时间 xx%」，都是错位/多余的内容

**问题**：`src/perfpage.cpp:1184-1189`
```cpp
wchar_t right[64] = L"";
if (d) swprintf_s(right, L"活动时间 %.0f%%", d->activePercent);
PaintTitleBlock(hdc, rc, L"磁盘 0", d ? L"SSD" : L"", right);
```

**原版**：
1. 「活动时间」不是标题栏右侧的内容 —— 它在下图第一个图的**左上角**
   （`docs/evidence/uifile_Taskmgr_30001_strings.txt:637-643`：
   `atom(diskTimeTitle) → resstr(34462) = "100%"`，
   而 `"活动时间"` = resstr(34461) 挂在 `disk_time`（`dynamic_disk_server` 的表格列）上，
   `atom(diskTimeGraph)` 的 accname 才是 resstr(34653)）。**标题栏右侧没有「活动时间 xx%」。**
   注意 34462 **就是字符串 "100%"** —— 说明原版标题栏右侧放的是量程上限，不是当前值。
2. 标题的格式串是 `resstr(34450) = "磁盘 %1!d! (%2!s!)"`，
   第二个槽填**盘符列表**（如 `C: D: E:`），不是介质类型。
   `docs/evidence/README.md` 里原版侧栏第一行逐字是 **`磁盘 0 (D: E: C:)`** ——
   盘符顺序按卷挂载顺序，格式是 `(D: E: C:)` 带空格分隔。
3. 介质类型 `SSD` 是**第二行**（`crop_sidebar_4x.png` 第二行就是孤零零一个 `SSD`）。

**重实现**：标题右侧 `活动时间 3%`（重复了下图标题），副标题 `SSD`（类型放在了副标题位），
**盘符完全缺失** —— 页面上永远显示「磁盘 0」，`src/perfdata.cpp:622` 里
`d.name = L"磁盘 0";` 是**写死的字面量**，`disks` 数组只有一个元素
（`src/perfdata.cpp:596-604` 注释自己写了「只能拿到全系统聚合值，不能分盘」）。

**差多少**：
- 盘符列表 `(D: E: C:)` 100% 缺失
- 「活动时间 xx%」出现在标题栏（原版此处是 `100%` 量程上限）
- 多磁盘场景直接失效（原版侧栏 truth1 里有「磁盘 0」卡片，多盘机器会有「磁盘 1」）

**严重度：严重**

---

### M7【明显】磁盘页右侧属性：4 行，原版是 5 行且内容不同

**问题**：`src/perfpage.cpp:1275-1279`
```
读取速度 / 写入速度 / 平均响应时间 / 传输速率
```

**原版**（`docs/evidence/uifile_Taskmgr_30001_strings.txt:603-620`，`static_disk = rowlayout(3)`）：

| 元素 | MUI ID | 文案 |
|---|---|---|
| `disk_capacity_Label` | 34453 | `容量:` |
| `disk_formatted_label` | 34454 | `已格式化:` |
| `disk_sys_label` | 34455 | `系统磁盘:` |
| `disk_pagefile_label` | 34456 | `页面文件:` |
| `disk_type_label` | 34451 | `类型:` |

而 `读取速度 / 写入速度 / 平均响应时间 / 活动时间 / 磁盘传输速率` **全部属于
`dynamic_disk_server`**（`docs/evidence/uifile_Taskmgr_30001_strings.txt:621-632`）——
即**「服务端磁盘视图」的表格列**，不是普通磁盘页的右侧属性区。

**差多少**：
- 缺 **4 项**：容量 / 已格式化 / 系统磁盘 / 页面文件、类型
- 多 **4 项**：读取速度 / 写入速度 / 平均响应时间 / 传输速率（且这四项在原版里是
  `dynamic_disk_server` 的表格列，**不是同一屏的右侧属性**）
- 命中率 0/5

**严重度：严重**

---

### M8【明显】内存页右侧属性：7 行内容对，但漏了 static_memory 整组

**问题**：`src/perfpage.cpp:1153-1167` 的七行是
`速度 / 使用中 / 可用 / 已缓存 / 已提交 / 分页缓冲池 / 非分页缓冲池`。

**原版**是**两个分组**（`docs/evidence/uifile_Taskmgr_30001_strings.txt:554-585`）：

```
static_memory (rowlayout(2))  ← 硬件规格，5 项
  memory_speed_Label  34349 "速度:"
  slot_used_Label     34350 "已使用的插槽:"
  form_factor_Label   34380 "外形规格:"
  reserved_Label      34381 "为硬件保留的内存:"
  max_memory_Label    34382 "最大内存:"

dynamic_memory                ← 实时值，6 项
  memory_inuse      34399 "使用中(已压缩)"
  memory_avai       34352 "可用"
  memory_commit     34354 "已提交"
  memory_cache      34353 "已缓存"
  memory_paged      34356 "分页缓冲池"
  memory_non_paged  34358 "非分页缓冲池"
```

**差多少**：
- dynamic 组：实现写了「使用中」，原版是 **「使用中(已压缩)」**（34412→实为 34399）
  —— 漏了 `(已压缩)` 后缀，也漏了压缩内存这个指标。
  其余 5 项（可用/已提交/已缓存/分页缓冲池/非分页缓冲池）**对**。
- static 组：**整组 5 项全部缺失**（已使用的插槽 / 外形规格 / 为硬件保留的内存 / 最大内存）。
  只有「速度」沾了点边，但实现写的是 `速度: %u MHz`（当前频率），
  原版 `memory_speed` 配 `34366 = "%u MHz"`，语义上更接近内存条的时钟频率。

**严重度：明显**（漏 5 项硬件规格，正是「用户想看自己的内存条」的部分）

---

### M9【明显】内存页「73% / 已使用 23.3 GB / 31.9 GB」大数字块是自创的

**问题**：`src/perfpage.cpp:1004-1022` 画了一个 28px 大号 `73%` + 一行
`已使用  23.3 GB / 31.9 GB`。

**原版**：`atom(memoryUsageTitle)` 的内容是 `resstr(34360) = "内存使用量"`
（`docs/evidence/uifile_Taskmgr_30001_strings.txt:588-590`），
`memory_size` 是它的配套值元素。MUI 里**没有**「已使用 X / Y GB」这个格式串
（grep `已使用` 只命中 `34350 "已使用的插槽:"`）。
原版使用的格式串是 `34376 = "%1!s!/%2!s! MB"` 与 `34378 = "%1!s!/%2!s! GB"`
—— 即 `23.3/31.9 GB`，**斜杠两边没有空格，前面没有「已使用」三字**。

对照原版侧栏（`docs/evidence/README.md`）：`6.5/31.2 GB (21%)` ——
确认原版的写法是 `X/Y GB (Z%)`，**紧凑格式，不是「已使用 X / Y」**。

**差多少**：
- 文案从 `内存使用量` 变成裸百分比大数字
- 数字格式从 `X/Y GB` 变成 `已使用  X / Y`（多了前缀 + 空格）

**严重度：明显**

---

### M10【明显】磁盘页「磁盘传输速率」图的 Y 轴没有单位

**问题**：`src/perfpage.cpp:1226` 把读写速率画成标题右侧的
`R %.1f MB/s   W %.1f MB/s`，而 `PaintChart` 的 `unit` 参数在
`src/perfpage.cpp:570` 直接 `(void)unit;` —— **单位被丢弃，一个字都不画**。
第二个图（`src/perfpage.cpp:1258`）传的 `L"MB/s"` 同样被丢掉。

**原版**：`docs/evidence/uifile_Taskmgr_30001_strings.txt:644-650`
```
e1da  atom(diskRateTitle)  → resstr(34460) = "磁盘传输速率"
e21e  atom(disk_rate_unit)              ← 独立的单位元素！
e248  atom(diskRateGraph)
e28c  atom(diskRate_bottom) → atom(RateLabelDiskRate) → resstr(33819) = "0"
```
原版有**专门一个 `disk_rate_unit` 元素**显示动态单位（MUI 里有整套单位串：
`32879 字节/秒`、`32880 KB/秒`、`32881 MB/秒`、`32882 GB/秒`）。

**差多少**：
- 「磁盘传输速率」图的量程单位 100% 缺失
- 量程上限（`RateLabelDiskRate`，原版是动态数值如 `2 MB/s`）也缺失 ——
  实现的上图有 `100%`/`0%`，下图什么都没有

**严重度：明显**

---

### M11【像素级】`速度` 一行在标题栏和属性区各画一次

`src/perfpage.cpp:995`（标题栏 `right`）与 `src/perfpage.cpp:1153`（属性区首行）
都画了 `速度:  %u MHz`，同一屏出现两次。

原版：`memory_speed` 只在 `static_memory` 里出现一次
（`docs/evidence/uifile_Taskmgr_30001_strings.txt:557-559`），标题栏
（`performancetab_graphs` 的 `atom(chartAndTitle)` 系）里没有这一项。

**严重度：明显**（重复信息）

---

### M12【明显】图表曲线颜色：内存第二条曲线用了蓝色 `#3A7AB0`，与内存主色不搭

`src/perfpage.cpp:1069`：
```cpp
PaintChart(hdc, chart2, ordered, hist_.count, hist_.count, L"GB", RGB(0x3A,0x7A,0xB0));
```
`#3A7AB0` 是**蓝色**，而内存页主色是紫 `#8B12AE`
（`docs/evidence/themedump2_out.txt` 的 `CHARTVIEW(9,0,3802) = #8B12AE`，
`shots/truth1.png` (25,165) 实测 `rgb(139,18,174)`）。

原版内存页只有一条曲线，所以没有「第二条曲线该用什么色」这个问题；
但既然实现多画了一条，用蓝色就同时犯了「多画」和「串色」两个错。

**严重度：明显**

---

## 二、侧栏卡片（有原版真图，可逐像素比）

### S1【证据冲突 · 不上报为缺陷】磁盘卡第二行：两套「原版」参考互相打架

**我拿到的两张原版侧栏图，对同一张磁盘卡的描述完全不同：**

`shots/truth1.png`（1100×700，CPU 页整窗）的磁盘卡，5× 放大实测：

```
磁盘 0                      ← L1
162.8 MB/s  写入: …         ← L2（只有两行文字）
```

`docs/evidence/README.md`（860×1520，4× 侧栏）的磁盘卡：

```
磁盘 0 (D: E: C:)           ← L1（带盘符列表）
SSD                         ← L2（介质类型）
读取: 0 KB/秒 写入: 2.0…    ← L3（灰色）
```

**三行 vs 两行、带盘符 vs 不带盘符、有 SSD 行 vs 无。**

**为什么这不能用「不同 DPI」解释**：两张图的机器是同一台 ——
truth1 侧栏显示 `23.5/31.9 GB (74%)`，crop_sidebar_4x 显示 `6.5/31.2 GB (21%)`，
物理内存总量都是 ~32GB。但两者的网卡名不同（truth1 是 `以太网 27` / `以太网 45`
两条同名适配器，crop 是 `以太网 / Tailscale` 和 `以太网 / 以太网`），
说明是**同一台机器的不同时刻**（Hyper-V 虚拟网卡增减）。
同一台机器同一版本的 UI 不会对磁盘卡换布局，所以**至少有一张不是普通磁盘页的侧栏**。

**MUI / DUI 资源站在三行版这一边**（这是最硬的证据）：

`docs/evidence/uifile_Taskmgr_30001_strings.txt:402-417` 里磁盘卡的原子是：
```
c032  resstr(33847) = "磁盘"          ← 卡标题
c0ac  resstr(34489) = "磁盘图标"
c0c8  atom(sidebar_disk_name)
c0f8  atom(sidebar_disk_type)         ← 介质类型（SSD/HDD）独立原子存在
c128  atom(sidebar_disk_util)
c158  atom(sidebar_disk_server)
c18c  atom(sidebar_disk_r) → resstr(34448) = "读取: "
c1d2  atom(sidebar_disk_read)
c202  atom(sidebar_disk_w) → resstr(34449) = "写入: "
c248  atom(sidebar_disk_write)
```
`atom(sidebar_disk_type)` **确实存在**，且 `读取:` / `写入:` 是**两个带中文标签的独立原子**
（不是 `162.8 MB/s 写入:` 这种拼装串）。

**实现**：`src/perfpage.cpp:243-249`
```cpp
items_.push_back({ d.name,
    W(L"%.1f MB/s  写入: %.1f MB/s", d.readMBps, d.writeMBps),
    L"",                                  // detail 恒空 → 卡片只有 2 行
    PerfView::Disk, ... });
```

**可以确定的三点（与冲突无关，两边都指向实现的错）**：
1. **盘符列表**：`sidebar_disk_name` 的格式来自 `resstr(34450) = "磁盘 %1!d! (%2!s!)"`，
   第二个槽是盘符。实现 `src/perfdata.cpp:622` 写死 `L"磁盘 0"`，**盘符 100% 缺失**。
   这一点 `crop_sidebar_4x.png` 是直接证据（`磁盘 0 (D: E: C:)`）。
2. **介质类型缺失**：`atom(sidebar_disk_type)` 在 DUI 树里真实存在，实现从不显示。
3. **单位未本土化**：MUI 里磁盘速率的单位串是 `32880 = "KB/秒"` / `32881 = "MB/秒"`，
   实现用 `MB/s`。侧栏 `crop_sidebar_4x.png` 显示 `读取: 0 KB/秒`，确认是中文单位。

**不上报的部分**：卡片是 2 行还是 3 行（61px vs 68px）——
两张原版图冲突，**无法裁决，标注未证实**。

**严重度：严重（盘符缺失）/ 明显（类型、单位）**

---

### S2【像素级】侧栏卡片行基线：L1 在 +6、L2 在 +25、L3 在 +41

**重实现**：`src/perfpage.cpp:450-466`
```cpp
RECT l1{ textX, item.top +  6, ... item.top + 26 };
RECT l2{ textX, item.top + 25, ... item.top + 41 };
RECT l3{ textX, item.top + 41, ... item.top + 57 };
```

**原版实测**（`docs/evidence/README.md` 3300×2100，3× DPI，换算回 1× 后
与 `shots/truth1.png` 一致）。truth1 里侧栏文字墨迹带（x=88..210 区域）：

| 卡片 | L1 墨迹 | L2 墨迹 |
|---|---|---|
| CPU（选中，卡片 y77 起） | y101–112 | y119–127 |
| 内存（卡片 y≈152 起） | y163–174 | y180–190 |
| 磁盘（卡片 y≈213 起） | y223–235 | y240–251 |

即 L1 相对卡片顶 **+24~25**、L2 相对卡片顶 **+67**（因为原版 L1 是 16px 大字，
行框顶部留白比实现大）。**这一条我标注为未证实** ——
我需要先确认卡片顶边的精确 y，而 truth1 的卡片之间没有可见分隔线，
只能靠下一条卡片的 L1 反推。文字基线位置属于「看起来差一点但说不准」的范畴，
**不作为缺陷上报**，仅记录测量值供后续核对。

**严重度：未证实，不上报**

---

### S3【像素级】侧栏迷你图表尺寸 —— 实现正确，非缺陷

`kMiniChartW = 61`，`chartH = cardH - 20`（2 行卡 → 41px）。

原版实测（`docs/evidence/README.md`，3×）：
- 内存迷你图紫色像素 bbox = x 75..257, y 483..605 → **183×123**，÷3 = **61×41**
- 磁盘迷你图绿色像素 bbox = x 75..257, y 666..788 → **183×123**，÷3 = **61×41**

**与实现完全一致（61×41）。非缺陷，记录以正视听。**

---

### S4【明显】侧栏「磁盘」永远只有一张卡（多盘机器上失效）

`src/perfpage.cpp:241`：
```cpp
for (size_t i = 0; i < snap_->disks.size() && i < 8; ++i)
```
看起来支持多盘，但 `src/perfdata.cpp:596-641` 只 `push_back` **一个**
`DiskSample`，`name` 写死 `L"磁盘 0"`（`src/perfdata.cpp:622`）。所以无论机器有几块盘，
侧栏永远只有「磁盘 0」一张卡，磁盘页也永远只显示「磁盘 0」。

原版 DUI 树里侧栏磁盘条目是**可重复的模板**：
`docs/evidence/uifile_Taskmgr_30001_strings.txt:402-417`
`atom(dashSidebarDiskButton)` 内含 `sidebar_disk_name`，
而 `disk.name` 的格式串 `resstr(34450) = "磁盘 %1!d! (%2!s!)"`
第一个槽是**磁盘序号**（`%1!d!`）—— 强烈暗示侧栏会有「磁盘 0」「磁盘 1」多张卡。

**实测限制**：`shots/truth1.png` 和 `docs/evidence/README.md` 都只有一张
「磁盘 0」卡（那台机器可能就是单盘 + 三个卷 `D: E: C:`，所以一张卡是对的）。
**多盘机器上的原版形态我没有参考图，未证实。**

**严重度：明显**（能力缺失；单盘环境下无观感差异）

---

## 三、侧栏主色复核（结论：正确，非缺陷）

| 页面 | 实现值 | 原版实测 | 判定 |
|---|---|---|---|
| CPU | `RGB(0x11,0x7D,0xBB)` `src/perfpage.cpp:220` | `#117DBB`（`orig_full.png` (100,300) 实测 `rgb(17,125,187)`） | ✅ |
| 内存 | `RGB(0x8B,0x12,0xAE)` `src/perfpage.cpp:236` | `#8B12AE`（`orig_full.png` (76,486) 实测 `rgb(139,18,174)`） | ✅ |
| 磁盘 | `RGB(0x4D,0xA6,0x0C)` `src/perfpage.cpp:248` | `#4DA60C`（主题 `ChartView(16,0,3802)`，truth1 绿色像素命中） | ✅ |
| 网络 | `RGB(0xA7,0x4F,0x01)` `src/perfpage.cpp:264` | `#A74F01`（主题 `ChartView(23,0,3802)`） | ✅ |
| GPU | `RGB(0x11,0x7D,0xBB)` `src/perfpage.cpp:282` | `#117DBB` | ✅ |

主色五项**全对**，与 `GUI_DIFF.md` 第 1 节已修结论一致。**不重复上报**。

补充一条**新证据**（此前没记录过）：`docs/evidence/themedump2_out.txt` 里
`ChartView` 类 part 9/16/23 分别是 `#8B12AE` / `#4DA60C` / `#A74F01`，
且每个 part 后面还跟着一个**同色系的淡色 part**
（`p=10 = #ECDEF0` 淡紫配 `p=9` 紫、`p=17 = #DBEDCE` 淡绿配 `p=16` 绿、
`p=24 = #EEDECF` 淡橙配 `p=23` 橙）。
这一对「主色 + 淡色」正是**曲线色 + 曲线下填充色**的配对。
`GUI_DIFF.md` 第 13 节判定「原版不做曲线下填充」——
按这个主题对照，`#ECDEF0` / `#DBEDCE` / `#EEDECF` 这三个淡色就是**填充色**。
但 truth1 逐像素取样显示曲线下方仍是 `#F1F6FA`（`GUI_DIFF.md:139-140`），
**两方证据冲突，我无法裁决**，标注为**未证实**，不改建议。
**这一条是「已知结论可能有问题」的提示，不是新缺陷。**

---

## 四、图表绘制内核（`PaintChart`）—— 这一层两页共用，问题会同时传导到内存页和磁盘页

### C1【明显】曲线线宽 2px，原版实测 1px（CPU 页可量，内存/磁盘页同内核）

**重实现**：`src/perfpage.cpp:564`
```cpp
HPEN pen = CreatePen(PS_SOLID, 2, color);
```

**原版实测**：`shots/truth2.png`（原版 CPU 页，逻辑处理器格子里的面积图）
逐列扫描蓝线（`#1B82C2` 级）的纵向厚度：

```
x=420 → 蓝色行 157,158,159,160,161   （这是峰值的竖直段，不可用作线宽）
x=340 → 148（1 行）
x=460 → 148（1 行）
```

横向穿越处只有 **1 个像素行**。这与 `docs/evidence/spec_cpugrid.md` 记录的
「曲线 RGB(27,130,194)，**1px 宽**」（`src/perfpage.cpp:592` 的注释也照抄了这个结论）
一致。

⚠️ 注意：`GUI_DIFF.md` 第 15 节 / `perf_page_spec.md` §2.2 说的是主图曲线
「1.5px」，而实测的最小可分辨值是 1px（GDI 只能画整数宽度，1.5px 会被
放大成 2px 或退化成 1px）。**原版主图（内存/磁盘）真实线宽未证实。**
但**逻辑处理器格子里的曲线是 1px**（有实测），而实现给
`PaintChart`（主图）用了 **2px**，给 `PaintLogicalCpuGrid` 用了 1px
（`src/perfpage.cpp:703`）—— 同一个应用里两处曲线粗细不一致，这本身就可疑。

**差多少**：主图曲线厚 1px（100% 偏差）。2px 会让 60 秒的抖动折线糊成一条粗带，
原版是纤细的折线。

**严重度：明显**（主图线宽未证实，但内部控制不一致是确定的）

---

### C2【明显】`0%` / `100%` 标注画在**图表内部左上/左下角**，原版在**图表外部的上下方**

**重实现**：`src/perfpage.cpp:524-527`
```cpp
RECT t100{ rc.left + 4, rc.top - 2,    rc.left + 60, rc.top + 14 };
DrawText2(hdc, L"100%", &t100, ...);
RECT t0{   rc.left + 4, rc.bottom - 14, rc.left + 60, rc.bottom + 2 };
DrawText2(hdc, L"0%",   &t0, ...);
```
`rc` 是**图表矩形本身**。`rc.top - 2` 意味着 `100%` 的文本框顶端比图表顶边高 2px，
于是文字**压在图表内部的左上角**；`0%` 同理压在内部左下角。

**原版**（`shots/truth2.png`，原版 CPU 页，有 `60 秒内的利用率 %` 那一版）实测：

- 网格顶边 = **y≈145**（`x=420` 处第一根 `#6AAED4` 边框线在 y=145）
- `100%` 文字墨迹在 **y=101..109**，右沿 x=1074（网格右沿 x≈1067）
  → 标注在**网格上方 36px 处**，且**右对齐到网格右缘**
- `60 秒内的利用率 %` 同一行（y=101..112），左对齐 x≈243（= 网格左沿 x=242）

也就是说原版的 `100%` 是**右上角、图表外、与左上角说明文字同一行**，
不是「图表内左上角」。**这一条 `GUI_DIFF.md` 第 15 节已经写对了**
（「原版网格左上角有 `60 秒内的利用率 %`，右上角有 `100%`」），
并且 `PaintCpuPage` 的 Logical 分支（`src/perfpage.cpp:951-957`）也是这么画的。
**但 `PaintChart`（主图，内存页和磁盘页都用它）没改** ——
仍是 `rc.left + 4, rc.top - 2` 的左上角内嵌写法。

**差多少**：
- 水平：`100%` 从**右对齐**变成**左对齐**（横跨整个图宽，差约 900px 的位置）
- 垂直：从图表**外上方**变成图表**内顶部**（约 36px）
- `0%`：原版在网格**下方**（`atom(..._bottom)` + `RateLabel` 元素），
  实现在图表**内底部**

**严重度：严重**（位置完全反了，且这是内存页/磁盘页每一张图都会犯的错）

---

### C3【明显】`PaintChart` 画的网格线颜色与原版不符

**重实现**：`include/perfpage.h:66`
```cpp
constexpr COLORREF kChartGrid = RGB(0xE8, 0xF0, 0xF9);
```
即 `#E8F0F9`。

**原版实测**（`shots/truth2.png`，`x=420` 纵向扫描，格子里）：
```
241,246,250 = #F1F6FA   ← 底色
232,240,249 = #E8F0F9   ← 网格线
```
**网格线 `#E8F0F9` 是对的**。但注意原版的网格线**只在逻辑处理器格子里出现**；
主图的网格线颜色没有实测依据。

另外 `src/perfpage.cpp:517` 的网格线循环：
```cpp
for (int i = 0; i <= 10; ++i) {
    int y = rc.top + (rc.bottom - rc.top) * i / 10;
```
`i=0` 和 `i=10` 恰好与图表外框重合（`Rectangle` 在 `src/perfpage.cpp:1619` 又画一次），
等于**首尾两条网格线被画了两遍**（一遍 `#E8F0F9`、一遍 `kChartGrid` 外框）。
原版 `perf_page_spec.md` §2.2 写的是「横向 10 条（含顶底）」，
实现画的是 **11 条**（`i<=10`）。**差 1 条**。

**严重度：像素级**

---

### C4【明显】图表没有任何外框/刻度尺（`rsquare` 元素缺失）

**原版**：`docs/evidence/uifile_Taskmgr_30001_strings.txt:184-189` 的 `graphbox` 样式里，
每个图表箱都有一个 `class="rmargin"` + `class="rsquare"` 的子元素：

```
c9be  rect(0rp,0rp,0rp,10rp)
...
d20a  atom(chartArea)
```
以及 `wdc` 版 `docs/evidence/uifile_wdc_30003.xml:167-174`：
```xml
<if class="rmargin">
  <element layoutpos="right" width="16rp" padding="rect(1rp,7rp,0rp,7rp)" .../>
<if class="rsquare">
  <element layoutpos="left" width="6rp" borderthickness="rect(0rp,1rp,1rp,1rp)"
           bordercolor="themeable(captiontext, buttontext)"/>
```
即：图表右侧有 **16rp 的留白**，留白里有一个 **6rp 宽的「小方块」**
（`rsquare`，带 1px 边框）—— 这是 Win10 任务管理器性能页图表的
**右下角量程指示小方块**（很多人以为没有，其实有）。
实现在 `PaintChart` 里完全没有画。

**差多少**：缺 16px 右留白 + 6px 量程方块；同时因为缺右留白，
实现的曲线被画到 `rc.right - 2`，比原版更贴近右边缘。

**严重度：明显**（元素缺失）
**未证实部分**：`rsquare` 在 taskmgr 的实际渲染效果（是否可见、什么颜色）我没有原版内存/磁盘页截图，**标注未证实**。

---

## 五、数据可信度（不是视觉差异，但会让页面「看起来是假的」）

### D1【严重】「活动时间」不是活动时间，是吞吐量除以 500MB/s

**重实现**：`src/perfdata.cpp:628-634`
```cpp
// 活动时间：用吞吐占「合理上限」的比例近似。
// 这不是真正的 % Disk Time（那要 PDH），但至少反映负载强度
double totalMBps = d.readMBps + d.writeMBps;
double pct = totalMBps / 500.0 * 100.0;   // 500 MB/s 视作饱和
d.activePercent = pct;
```

原版「活动时间」的语义是 `resstr(34653) = "磁盘处理读取或写入请求的时间百分比"`
—— **时间占比**（% Disk Time 计数器）。

**差多少**：两个完全不同的物理量。一块 SSD 用 4KB 随机读把队列打满时，
真实「活动时间」可以 100% 而吞吐只有几十 MB/s；反过来大文件顺序写
500MB/s 时真实活动时间接近 100%。实现里 `500 MB/s = 100%` 这个映射
在任何一块 NVMe 盘上都会**恒定显示接近 0%**，在任何一块老 HDD 上
（100MB/s 就算满载）会**恒定显示 5%**。

`src/perfpage.cpp:1208` 把 `d->activePercent` 直接当成 `%.0f%%` 显示在
「活动时间」标题右侧 —— 用户看到的是**一个和真实活动时间无关的数**。

**严重度：严重**（数值语义错误，且被明确标注成原版指标名）

---

### D2【严重】「平均响应时间」永远是 0

**重实现**：`src/perfpage.cpp:1277`
```cpp
swprintf_s(line, 128, L"平均响应时间:   %.1f ms", d->avgResponseMs); Put(line);
```
而 `avgResponseMs` 在 `include/perfdata.h:58` 定义为 `double avgResponseMs = 0.0;`，
**全工程没有任何一处给它赋值**（grep `avgResponseMs` 只有这两处：声明 + 这一行绘制）。

**原版**：`resstr(34459) = "平均响应时间"`，格式串 `34466 = "%s 毫秒"`
（注意原版单位是**中文「毫秒」**，不是 `ms`）。

**差多少**：永远显示 `0.0 ms`。且单位不是原版的 `毫秒`。

**严重度：严重**（死数据）

---

### D3【明显】磁盘页四个属性行全部右对齐

**修正**：本节先前写的「原版是 rowlayout(3) 三列」不准确 ——
`static_disk` 的 `rowlayout(3)` 指的是**标签/值对的三列排布**（容量/已格式化/系统磁盘
三个标签 + 三个值），而 `disk_pagefile` 和 `disk_type` 是后续行。
但**对齐方向**这一条仍然成立且证据清楚：

**重实现**：`src/perfpage.cpp:1272`
```cpp
DrawText2(hdc, s, &r, DT_SINGLELINE | DT_RIGHT | DT_END_ELLIPSIS, kSubText);
```
四行（读取速度/写入速度/平均响应时间/传输速率）都是 `DT_RIGHT`，
而且**标签和值拼成同一个字符串**（`L"读取速度:   %.1f MB/s"`），
整串一起右对齐。

**原版**：`docs/evidence/uifile_Taskmgr_30001_strings.txt:603-620` 是
`*_Label` + 值元素**成对**出现（`disk_capacity_Label` / `disk_capacity`、
`disk_formatted_label` / `disk_formatted` …），
且 `graphbox` 样式（`docs/evidence/uifile_wdc_30003.xml:178-183`）明确区分：
- `lheader` → `contentalign="topleft|endellipsis"`
- `rheader` → `contentalign="topright|endellipsis"` ← **值才是右对齐的**

即原版是**标签左对齐 + 值右对齐**（两端对齐的一行），
不是一个整串右对齐的窄条。

**另外的不一致**：内存页的对应实现（`src/perfpage.cpp:1149`）用的是 `DT_LEFT`，
磁盘页用 `DT_RIGHT` —— **同一种信息，两页对齐方式相反**。

**差多少**：原版「标签…值」两端对齐，实现「标签: 值」整体右对齐。

**严重度：明显**

---

### D4【明显】磁盘页两图之间的间距 26px，原版有独立的 `_bottom` 标签带

**重实现**：`src/perfpage.cpp:1218`
```cpp
int top2 = c1.bottom + 26;
```

**原版**：每个图下面都有独立的 `*_bottom` 头部元素
（`docs/evidence/uifile_Taskmgr_30001_strings.txt:641-642`：
`atom(diskTime_bottom)` → `atom(RateLabelDiskTime)`；
`:649-650`：`atom(diskRate_bottom)` → `atom(RateLabelDiskRate)`），
而 `graphbox` 的 `header` 样式是 `padding="rect(0rp,0rp,0rp,2rp)"`
（`docs/evidence/uifile_wdc_30003.xml:175-177`）。

原版下图的位置由**上图底 + bottom 标签带高度**决定，不是 26px 的魔数。
`shots/truth2.png` 里逻辑处理器格子的纵向间距是
`y=214→219`（格底 214、下格顶 219，**间隙 5px**）、`288→293`（5px）、`362→367`（5px），
和 `src/perfpage.cpp:616` 注释里的「横向缝隙 5px、纵向 4px」基本吻合。

**差多少**：26px 是自拟值，**未证实**。原版真实间距取决于
`RateLabel` 那一行文字的高度 + 2rp padding，估 16–20px 量级。

**严重度：像素级（未证实）**

---

## 六、文案核对表（MUI 逐字 vs 实现）

| # | 位置 | 原版（MUI 逐字） | 实现 | 判定 |
|---|---|---|---|---|
| 1 | 内存页曲线标题 | `内存使用量`（34360） | `内存使用率`（`perfpage.cpp:1034`） | ❌ **用词错**：原版「使用量」，实现「使用率」 |
| 2 | 内存页第二条曲线标题 | 不存在 | `已提交  X / Y`（`perfpage.cpp:1053`） | ❌ 多余 |
| 3 | 内存组合条标题 | `内存组合`（34361） | `内存组合`（`perfpage.cpp:1079`） | ✅ |
| 4 | 内存条图例 | `使用中`(33852) / `已修改`(33841/33857) / `备用`(33853) / `可用`(33854) | 使用中/已修改/备用/**空闲** | ❌ 末段原版是**「可用」**，实现写「空闲」 |
| 5 | 内存属性 | `使用中(已压缩)`(34399) | `使用中:`（`perfpage.cpp:1155`） | ❌ 漏 `(已压缩)` |
| 6 | 内存属性 | `已使用的插槽:`(34350) | 无 | ❌ 缺失 |
| 7 | 内存属性 | `外形规格:`(34380) | 无 | ❌ 缺失 |
| 8 | 内存属性 | `为硬件保留的内存:`(34381) | 无 | ❌ 缺失 |
| 9 | 内存属性 | `最大内存:`(34382) | 无 | ❌ 缺失 |
| 10 | 内存属性 | `速度:`(34349) + `%u MHz`(34366) | `速度:   %u MHz` | ✅ 措辞对，但**画了两遍** |
| 11 | 磁盘标题 | `磁盘 %1!d! (%2!s!)`(34450) | `磁盘 0`（硬编码） | ❌ 缺 `(盘符)` |
| 12 | 磁盘图1标题 | `活动时间`(34461) | `活动时间` | ✅ |
| 13 | 磁盘图1右上 | `100%`(34462，量程上限) | `活动时间 3%` | ❌ 位置对但内容错 |
| 14 | 磁盘图2标题 | `磁盘传输速率`(34460) | `磁盘传输速率` | ✅ |
| 15 | 磁盘图2单位 | 独立 `disk_rate_unit` 元素（32879–32882 一整套） | 无（`unit` 参数被 `(void)` 丢弃） | ❌ 缺失 |
| 16 | 磁盘属性 | `类型:`(34451) / `容量:`(34453) / `已格式化:`(34454) / `系统磁盘:`(34455) / `页面文件:`(34456) | 无 | ❌ 全缺 |
| 17 | 磁盘属性 | 读取速度/写入速度/平均响应时间是 `dynamic_disk_server` 表格列 | 当作右侧属性行 | ❌ 归属错 |
| 18 | 磁盘类型取值 | `HDD`(34469) / `SSD`(34470) / `持久性内存`(34471) / `可移动`(34472) / `SD`(34473) / `USB`(34474) / `未知`(34477) | 写死 `SSD`（`perfpage.cpp:1189`） | ❌ 永远 SSD |
| 19 | 侧栏磁盘卡 L2 | 介质类型（`SSD`） | 速率 | ❌ |
| 20 | 侧栏磁盘卡 L3 | `读取: … 写入: …`（`34448`/`34449`） | 空 | ❌ |
| 21 | 侧栏磁盘卡单位 | `KB/秒`(32880) / `MB/秒`(32881) | `MB/s` | ❌ 未本土化 |

统计：**21 项里 4 项正确（3/10/12/14），17 项有问题。**

---

## 七、`#F1F6FA` 图表区底色 —— 在内存/磁盘页上无法验证

`include/perfpage.h:65 / 81 / 82` 三处都是 `#F1F6FA`
（`kChartBg` / `kChartFill` / `kChartAreaBg`）。

这个值来自 `GUI_DIFF.md` 第 13 节，取样自 `shots/truth1.png` 的
**逻辑处理器格子内部**（(300,200)/(300,240)/(300,300)/(600,400)）。

我刚才对 `shots/truth2.png` 复核（`x=420` 纵向扫描，格内）：
底色 `241,246,250 = #F1F6FA` ✅ 格子底色确实是这个值。

但**内存页/磁盘页的主图底色**是另一个元素（`atom(memoryUsageGraph)` /
`atom(diskTimeGraph)`，样式 `sheet="graph"`，`background="rgb(0,0,0)"`
—— 见 `docs/evidence/uifile_wdc_30003.xml:190-192`，这个 `rgb(0,0,0)` 是
「交给代码画」的占位，不是真黑色）。

**结论：内存/磁盘页主图底色 `#F1F6FA` 未证实，我无法确认也无法否认。**
侧栏迷你图的底色（`docs/evidence/README.md`）看起来是**白色**，
而实现的 `PaintSidebarMiniChart` 用的什么底色见下一节。

---

## 八、未能在源码里找到的、需要回头核查的两点

### N1 侧栏迷你图的填充画法 —— 已核查，**无缺陷**

`src/perfpage.cpp` 的 `PaintSidebarMiniChart` 实现是：
白底 → 该卡片自身强调色的 1px 边框 → 6.7% alpha 同色淡填充 → 1px 曲线。

`docs/evidence/README.md` 的内存卡迷你图里，紫色曲线下方确实有一片极淡的
同色填充，且边框是**紫色**（不是灰色）。**实现与之一致，非缺陷。**
（注意：这里**没有**复用 `kChartFill`，用的是 `mix(v, 0.067)` 现算的，
所以 `kChartFill` 改成 `#F1F6FA` 不影响侧栏。已核实 `src/perfpage.cpp`
里 `PaintSidebarMiniChart` 的 `CreateSolidBrush(RGB(mix(r,0.067),...)` 一行。）

### N2 内存页「内存组合」条下方图例的排布

原版 `wdc` UIFILE 的 `memUsageLegends_memtab` 是
`tablelayout` **6 列**（`docs/evidence/uifile_wdc_30003.xml:309-318`），
即图例横向并排 5 段 + 两端留白。
实现的 `src/perfpage.cpp:1103-1137` 用游标 `lx` 累加绘制，
效果也是横排，**结构一致**。但原版每段的**文字是上下两行**
（`verticalflowlayout`：第一行短标签如「使用中」，第二行值 `resstr(33811)`），
实现是**一行** `%s  %s`（名字 + 容量）。

**差多少**：图例从 2 行变 1 行。原版 5 段 × 2 行 = 10 行文字的空间被压成 5 行。
`docs/evidence/uifile_wdc_30003.xml:325-328` 的
`<element layout="verticalflowlayout(0,0,0,0)">` 含**两个** `class="label"` 子元素，
是硬证据。

**严重度：明显**（未在原版任务管理器截图上验证过，但资源结构明确）

---

## 九、补充：磁盘传输速率图的量程是「自动取本列最大值」，原版是「漂亮档位序列」

### A1【严重】磁盘速率图用了「本窗口最大值」做满格，曲线永远顶格

**重实现**：`src/perfpage.cpp:1237-1249`
```cpp
double mx = 1.0;
for (int i = 0; i < hist_.count; ++i) {
    double v = hist_.At(hist_.diskRate, i);
    if (v > mx) mx = v;                 // 当前 60 点里的最大值
}
...
double v = hist_.diskRate[i] / mx * 100.0;   // 归一化到 0-100
```

**问题**：`mx` 恰是**本窗口内的峰值**，所以峰值点必然落在 **100%**，
曲线**每一帧都贴着顶边**。数据平稳时（比如恒定 2 MB/s）也会被放大成
一条顶格的满幅曲线，用户完全看不出「磁盘很闲」。

**原版**：MUI 里有**一整套离散档位**（`docs/evidence/mui_strings_all.txt` 32840–32858），
这是磁盘速率轴专用的一组：

```
32840 100 KB/秒   32845 250 MB/秒
32841 500 KB/秒   32846 500 MB/秒
32842 1 MB/秒     32847 1 GB/秒
32843 10 MB/秒    32848 10 GB/秒
32844 100 MB/秒   32849 100 GB/秒
32850 450 KB/秒   32853 60 MB/秒    32856 300 MB/秒
32851 800 KB/秒   32854 125 MB/秒   32857 2 GB/秒
32852 7 MB/秒     32855 200 MB/秒   32858 10 GB/秒
```

`1/10/100/1000` 的十进制阶梯（32840–32849）是最标准的一组 ——
**原版是从这个固定档位表里挑一个「刚好包住当前数据」的档位**，
而不是取窗口最大值。这样曲线高度是**跨时间可比**的，量程标签也才有意义
（`atom(disk_rate_unit)` / `RateLabelDiskRate` 显示的就是选中的档位）。

**差多少**：曲线高度的语义完全不同（绝对值档位 vs 相对峰值）。
且因为量程不显示（见 M10），用户看到一条永远顶格的曲线，
会以为磁盘 100% 忙。

**严重度：严重**（数据表达能力失效）

---

### A2【明显】两条曲线的 `100%` 标签都是硬编码字面量，与实际量程无关

`src/perfpage.cpp:525 / 527` 直接 `DrawText2(hdc, L"100%", ...)` / `L"0%"`。
对内存使用率图（0–100%）是对的，但对**磁盘传输速率图**（量程是 MB/s）就纯属乱写 ——
一张量程从 1 MB/s 到 100 GB/s 的图，纵轴写「100% / 0%」是错的。
`PaintChart` 的 `unit` 参数本来就是为了解决这个，但被 `(void)unit;` 丢掉了
（`src/perfpage.cpp:570`）。

原版：`diskRateGraph` 的上下标签来自 `atom(disk_rate_unit)` 与
`atom(RateLabelDiskRate)`，**显示的是一对速率值**（如 `100 MB/秒` / `0`）。

**严重度：严重**

---

### A3【明显】`PaintChart` 的 `unit` 参数是死参数（三页共用）

`src/perfpage.cpp:511-512` 的签名收 `const wchar_t* unit`，
`571` 行 `(void)unit;` 明示不用。

调用点：
- `:1038` 内存使用率传 `L"%"`
- `:1068` 已提交传 `L"GB"`
- `:1215` 活动时间传 `L"%"`
- `:1258` 磁盘传输速率传 `L"MB/s"`
- `:1346` 网络吞吐量传 `L"Mbps"`
- `:1465` GPU 传 `L"%"`

**六个调用点里有两个（磁盘速率 `MB/s`、网络 `Mbps`）传的单位是必需的**，
全部被丢弃。这不是「未实现」，是**接口设计出来又没用**。

**严重度：明显**

---

## 十、汇总：按严重度排序

### 严重（结构/语义级，用户一眼能看出不对）

| 编号 | 问题 | 位置 |
|---|---|---|
| M1 | 内存页多画一条原版没有的「已提交」曲线 | `perfpage.cpp:1042` |
| M3 | 内存组合条四段配色与资源字面值全部不符；缺「为硬件保留」段 | `perfpage.cpp:1095-1100` |
| M6 | 磁盘标题缺盘符列表 `(D: E: C:)`；「活动时间 %」错位到标题栏 | `perfpage.cpp:1185-1189`、`perfdata.cpp:622` |
| M7 | 磁盘右侧属性：原版 5 项（容量/已格式化/系统磁盘/页面文件/类型）全缺 | `perfpage.cpp:1275-1279` |
| C2 | `100%`/`0%` 画在图表内左上角，原版在图表外上下、右对齐 | `perfpage.cpp:524-527` |
| D1 | 「活动时间」用吞吐/500MBps 冒充，与真实 %DiskTime 无关 | `perfdata.cpp:628-634` |
| D2 | 「平均响应时间」字段从未被赋值，恒为 0 | `perfpage.cpp:1277` + `perfdata.h:58` |
| A1 | 磁盘速率图按窗口峰值归一化，曲线永远顶格 | `perfpage.cpp:1237-1249` |
| A2 | 磁盘速率图纵轴硬写 `100%`/`0%`，与 MB/s 量程无关 | `perfpage.cpp:525` |
| S1 | 侧栏磁盘卡缺盘符列表、缺介质类型（两套原版图冲突，只报这两点） | `perfpage.cpp:243-249` |

### 明显

| 编号 | 问题 | 位置 |
|---|---|---|
| M2 | 「已提交」同时画成曲线标题和属性行，重复 | `perfpage.cpp:1053` / `:1163` |
| M4 | 内存组合条高 22px，资源 24rp | `perfpage.cpp:1084` |
| M5 | 图例色块 10×10，资源 14rp×14rp 且带 1px 边框 | `perfpage.cpp:1118` |
| M8 | 内存属性缺 static_memory 整组 5 项；「使用中」漏 `(已压缩)` | `perfpage.cpp:1153-1167` |
| M9 | 「73% + 已使用 X / Y GB」大数字块是自创，原版是 `内存使用量` + `X/Y GB` | `perfpage.cpp:1004-1022` |
| M10 | 「磁盘传输速率」图缺单位元素 `disk_rate_unit` | `perfpage.cpp:1226` |
| M11 | 「速度」在标题栏和属性区各画一次 | `perfpage.cpp:995` / `:1153` |
| M12 | 内存第二条曲线蓝色 `#3A7AB0`，与内存主色紫 `#8B12AE` 串色 | `perfpage.cpp:1069` |
| C1 | 主图曲线 2px，格子内曲线 1px（内部不一致） | `perfpage.cpp:564` vs `:703` |
| C4 | 缺图表右侧 16rp 留白 + 6rp 量程方块（`rsquare`） | `PaintChart` 全局 |
| D3 | 磁盘属性行整串 `DT_RIGHT`，内存页 `DT_LEFT`；原版是标签左+值右 | `perfpage.cpp:1272` vs `:1149` |
| A3 | `PaintChart` 的 `unit` 参数被 `(void)` 丢弃，6 个调用点全失效 | `perfpage.cpp:570` |
| N2 | 内存条图例原版是 2 行（标签 + 值），实现 1 行 | `perfpage.cpp:1125-1127` |
| S4 | 侧栏磁盘永远只有一张卡（`disks` 恒 1 元素），多盘机器失效 | `perfdata.cpp:596-641` |

### 像素级

| 编号 | 问题 | 位置 |
|---|---|---|
| C3 | 网格线画 11 条（`i<=10`），应为 10 条 | `perfpage.cpp:516` |
| D4 | 两图间距 26px 为自拟值（未证实） | `perfpage.cpp:1218` |

### 已验证正确（不上报，记录以正视听）

| 项 | 实测 | 实现 |
|---|---|---|
| 侧栏迷你图尺寸 | `orig_full.png` 内存/磁盘均 **61×41** | `kMiniChartW=61` + `cardH-20=41` ✅ |
| 五页主色 | `#117DBB` / `#8B12AE` / `#4DA60C` / `#A74F01` / `#117DBB` | 全部一致 ✅ |
| 侧栏选中底色 | `#CCE8FF`，高度 **61px** | `RGB(0xCC,0xE8,0xFF)` ✅ |
| 网格线色 | `#E8F0F9`（truth2 格内实测 232,240,249） | `kChartGrid` ✅ |
| 图表底 `#F1F6FA` | 逻辑处理器格内实测一致（主图未证实） | ✅（格内） |
| 迷你图填充画法 | 原版有淡填充 | `PaintSidebarMiniChart` 用 6.7% alpha 混合 ✅ |
| 迷你图边框 | 1px、用卡片自身强调色 | 实现一致 ✅ |

### 明确标注「原版参考缺失，无法逐像素比对」的部分

- **内存页右侧主区**（图表位置/高度/间距、组合条位置、属性列 x/y）——
  没有原版内存页整页截图，全部未证实。
- **磁盘页右侧主区**（两图高度、间距、属性列位置）—— 同上。
- 本文所有涉及这两块的「位置」判断都只到**结构级**（元素该不该存在、
  文案是什么），**没有任何一处是我硬编的像素坐标**。

### 已知差异清单（`GUI_DIFF.md`）里需要复核的两条

1. **第 10 节**「原版内存页是 2 条曲线 + 底部内存组合条，我方结构一致」
   —— **这条结论是错的**。`uifile_Taskmgr_30001_strings.txt` 的 `memory_chart`
   下只有一个 `<graph>`（`memoryUsageGraph`）。见 M1。
2. **第 13 节**「原版不做曲线下填充」—— `themedump2_out.txt` 里
   `ChartView` 每个主色 part 后面都紧跟一个同色系淡色 part
   （`p=9 #8B12AE` → `p=10 #ECDEF0`；`p=16 #4DA60C` → `p=17 #DBEDCE`；
   `p=23 #A74F01` → `p=24 #EEDECF`），这正是「曲线色 + 填充色」的配对。
   与 truth1 取样（曲线下仍是 `#F1F6FA`）冲突。**未证实，仅提示复核。**



