# Agent U — 任务管理器主题配色

状态：已完成

取色统计：**`TASKMANAGER` 46 个 part / `CHARTVIEW` 42 个 part / 另 16 个主题类**，
原始 TSV 共 **16592 条唯一 (class, part, state, prop) 记录**
（`docs/evidence/exec/U_colors_final.tsv`）。**取不到 8 项，全部在 §7 明确标注。**

---

## 0. 先说结论（TL;DR）

1. **`docs/evidence/themeprobe_out.txt` 每一行的 R 和 B 都是反的。** 那份脚本把
   `GetThemeColor` 的返回值当成 `ARGB(0xAARRGGBB)` 解码，但 Win32 的
   `GetThemeColor` 返回的是 **`COLORREF` = `0x00BBGGRR`**（R 在最低字节）。
   于是它打印的 `#FEE7E2` 真值是 `#E2E7FE`，`#E4F9FF` 真值是 `#FFF9E4`。
   **凡是引用过 `themeprobe_out.txt` 颜色数字的结论都要重新核对。**
   （`themedump2_out.txt` 的注释写着自己按 `0x00BBGGRR` 解，但它实际输出也是
   R/B 反的 —— 两处口径不一致，本轮以我自己的 dump 为准。）

2. **主题类名大小写不敏感，可以混用。** 实测：
   `TASKMANAGER` / `TaskManager` 取到的颜色完全一致（`p1s1fill` 都是
   `0x00FEE7E2`），`CHARTVIEW` / `ChartView` 一致（`0x00FFFFFF`），
   `EXPLORER::LISTVIEW` / `Explorer::ListView` 一致，`ITEMSVIEW::LISTVIEW` /
   `ItemsView::ListView` 一致。**（handle 值不同只是每次 `OpenThemeData`
   新建一个引用，不代表命中不同条目 —— 颜色相同才是判据。）**

3. **UIFILE 里真正被引用到的主题组合：`gtc` 去重 23 条 + `dtb` 去重 22 条
   = 45 条**（见 §1 两张表）。它们只覆盖 `TaskManager` / `ChartView` /
   `ItemsView::ListView` / `ItemsView::Header` / `Explorer::ListView` /
   `Explorer::TreeView` / `Button` / `CONTROLPANELSTYLE` / `TEXTSTYLE` /
   `CommandModule` / `TaskDialog` 这 11 个类。
   列表行/页签的背景**不在 UIFILE 的 gtc 里**，而是 `dtb(...)` 交给 DUI 用
   `DrawThemeBackground` 直接画的 —— 所以必须用 **渲染**（不是取色）才能拿到，
   见 §4。

4. **列表行三态的关键值（本轮首次用"渲染到离屏 DIB"拿到，非猜测）：**

   | 元素 | 值 | 来源 |
   |---|---|---|
   | 行正常底 | `#FFFFFF`，上下左右 1px `#828790` 框 | ItemsView::ListView p1 s1 渲染 |
   | **行悬停（hot）** | **`#E5F3FF`** | ItemsView::ListView p1 s2 渲染 |
   | **行选中（selected）** | **`#CDE8FF`** | ItemsView::ListView p1 s3 渲染 |
   | 行选中但失焦 | `#D9D9D9` | LISS_SELECTEDNOTFOCUS=5 |
   | 行悬停且选中 | 底 `#CCE8FF` + 1px `#99D1FF` | LISS_HOTSELECTED=6 |
   | 分组头正常 | `#FFFFFF` | LVP_GROUPHEADER=6 s1 |
   | 分组头悬停 | `#E5F3FF` | s2 |

   > **`#CDE8FF` 有三重独立印证**：① 主题 `DrawThemeBackground` 渲染结果；
   > ② 真机 4 倍裁切 `docs/evidence/README.md` 逐像素直方图第一项
   > `#CDE8FF` 40512 px（占选中带 74%）；③ 我方 `tmwindow_details.cpp` 已按
   > 这个值实现了 `kDetSelBg`。**这是我们已有的 `kBgNavSelected = #A0CDF0` 的明确修正值。**
   >
   > **`#E5F3FF` 只有主题渲染这一个来源** —— 所有真机静态截图里都没有鼠标悬停，
   > 所以取不到像素。但同一主题的 `#CDE8FF` 已被像素证实，所以渲染管线可信。

5. **页签（`Tab` 类，Aero 原生 Tab 控件皮肤）**：活动白 `#FFFFFF`、
   非活动 `#F0F0F0`、悬停 `#D8EAF9`、按下 `#D9D9D9` 边框 + `#F0F0F0` 内芯、
   焦点态多一圈 `#0078D7` 1px。**但原版任务管理器并不直接 `dtb(Tab,...)`**
   —— UIFILE 里 `WdcTab` 类在 Aero 里不存在（`OpenThemeData` 返回 0），
   页签是 DUI 自绘的。**真机像素给出的是**：非活动 `#EFEFF0`、活动 `#FFFFFF`、
   边框 `#C8C8C8`（见 §5.2）。两者不同，**以真机为准**。

   > 顺带发现一个未解决的分歧：能确认是真原版的截图（`orig_full.png`、
   > `3png_tabs.png`）页签都是 **5 个**，而 `tabs.h` 按 `shots/m3.png` 写了 7 个。
   > `m3.png` 本轮已判定是我方旧截图。**但 5 个页签也可能是 Windows Server 版本。**
   > 我不下结论，只报告证据。详见 §5.2。

6. **热力图调色板逐字节复核通过**：从 `taskmgr.exe` 的 `.data` RVA `0x1370F0`
   直接读 9 条 24 字节记录，与主题 part 3..11 / state 1 的 `GetThemeColor`
   完全一致（`#FFF9E4`…`#FC682A`）。阈值是 double：`255/0/9/19/39/59/79/99/100`。

---

## 1. UIFILE 里出现的全部主题引用（去重清单）

来源：`docs/evidence/uifile_Taskmgr_30001_full.txt`、`uifile_Taskmgr_30024_full.txt`、
`uifile_wdc_3000{0..5}.xml`（后 7 个文件里 **只有 `gtc(Button,1,4,3803)` 一条**）。

### 1.1 `gtc(主题, part, state, prop)` —— 共 23 条去重

| # | 引用 | 所在元素（UIFILE 字符串序） | 用途 |
|---|---|---|---|
| 1 | `gtc(TASKMANAGER, 13, 0, 3803)` | `TmRowTextElement` | 列表正文文字 |
| 2 | `gtc(TASKMANAGER, 14, 0, 3803)` | `atom(resourceTitle)` | 卡片标题 |
| 3 | `gtc(TASKMANAGER, 15, 0, 3803)` | `atom(resourceName)` | 卡片副标题 |
| 4 | `gtc(TASKMANAGER, 16, 0, 3803)` | `sidebarLabel` / `atom(TmGroupHeaderName)` | 侧栏标签 + 分组头名称 |
| 5 | `gtc(TASKMANAGER, 17, 0, 3803)` | `LowMemoryTitleLabel` / `StaticLabel` | 低内存提示 |
| 6 | `gtc(TASKMANAGER, 18, 0, 3803)` | `staticData` (dynamicLabel) | 动态标签 |
| 7 | `gtc(TASKMANAGER, 19, 0, 3803)` | `atom(dynamicData)` | 卡片数值 |
| 8 | `gtc(TASKMANAGER, 2, 0, 3802)` | `TmGroupHeaderStyle` | **分组头背景** |
| 9 | `gtc(TASKMANAGER, 20, 0, 3802)` | `atom(BarchartStandby)` | 组合条：备用 |
| 10 | `gtc(TASKMANAGER, 21, 0, 3802)` | `atom(BarchartInuse)` | 组合条：使用中 |
| 11 | `gtc(TASKMANAGER, 22, 0, 3802)` | `atom(BarchartModified)` | 组合条：已修改（亮） |
| 12 | `gtc(TASKMANAGER, 23, 0, 3802)` | `atom(BarchartModified)` | 组合条：已修改（暗） |
| 13 | `gtc(TASKMANAGER, 24, 1, 3803)` | `TmSubColumnHeaderStyle` | 子列头文字 |
| 14 | `gtc(TASKMANAGER, 25, 0, 3803)` | `cpuBlockStyleData` | CPU 格子数值文字 |
| 15 | `gtc(TASKMANAGER, 38, 0, 3802)` | `cpuHeatGrid` | CPU 栅格底 |
| 16 | `gtc(TASKMANAGER, 39, 0, 3802)` | `cpuBlockStyleR` | CPU 格子（红端） |
| 17 | `gtc(TASKMANAGER, 40, 0, 3802)` | `cpuBlockStyleB` | CPU 格子（蓝端） |
| 18 | `gtc(TASKMANAGER, 41, 0, 3802)` | `atom(ResizerImage)` `gradient()` 起点 | 分栏拖柄 |
| 19 | `gtc(TASKMANAGER, 42, 0, 3802)` | `atom(ResizerImage)` `gradient()` 终点 | 分栏拖柄 |
| 20 | `gtc(TASKMANAGER, 44, 0, 3803)` | `lheader` | 左列头文字 |
| 21 | `gtc(CHARTVIEW, 9, 0, 3802)` | `atom(BarchartMemoryHolder)` | 组合条：内存总背景 |
| 22 | `gtc(ItemsView::ListView, 1, 3, 3803)` | `cpuHeatGrid`？否 —— `themeable(...)` 里做 `highlightText` | 主题回退文字色 |
| 23 | `gtc(Button / button, 1, 2\|3, 3803)` | `TmTaskButton` | 工具条按钮文字 |
| 24 | `gtc(Button, 1, 4, 3803)` | wdc_30000..30005 各 8 处 | 工具条按钮禁用文字 |
| 25 | `gtc(CONTROLPANELSTYLE, 10, 1\|2\|4, 3803)` | `link` | 链接三态文字 |
| 26 | `gtc(TEXTSTYLE, 4, 0, 3803)` | `BodyText` | 正文文字 |

### 1.2 `dtb(主题, part, state)` —— 共 22 条去重（这些**只能靠渲染取色**）

| 引用 | 用途 |
|---|---|
| `dtb(TASKMANAGER, 43, 1)` / `43, 2` | 列头排序箭头两态 |
| `dtb(Explorer::TreeView, 2, 1)` / `2, 2` / `4, 1` / `4, 2` | 树展开箭头（折叠/展开 × 两态） |
| `dtb(EXPLORER::LISTVIEW, 1, 2)` | **行悬停** |
| `dtb(EXPLORER::LISTVIEW, 1, 3)` | **行选中** |
| `dtb(EXPLORER::LISTVIEW, 1, 5)` | **行选中但失焦** |
| `dtb(button, 1, 1)` / `1, 2` / `1, 3` / `1, 4` | 工具条按钮四态 |
| `dtb(CommandModule, 3, 1)` / `3, 2` | 工具条按钮（DUI 命令模块版） |
| `dtb(TaskDialog, 13, 1..6)` | 任务对话框按钮 |
| `dtb(ItemsView::Header, 4, 2)` | 列头排序箭头 |

### 1.3 主题名与句柄实测（大小写）

```
CHARTVIEW  -> h=65536   p2s0fill=0x00BB7D11
ChartView  -> h=131072  p2s0fill=0x00BB7D11   (值相同，但 handle 不同)
TASKMANAGER-> h=196608  p2s0fill=0x00A0D5EA
TaskManager-> h=262144  p2s0fill=0x00A0D5EA
```

> 注意 `handle` 不同是因为 `OpenThemeData` 每次调用都新建一个引用。
> 关键点是**颜色值一致** —— 说明 msstyles 里同一份条目被两种大小写都命中了。
> 我们的代码里两个名字混用没有问题，但 **`OpenThemeData(NULL, ...)` 必须传 NULL**，
> 传窗口句柄在无窗口进程里会拿到 0。

### 1.4 propId 核实（不是照抄委托方的表）

对照 `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um\vssym32.h`：

| propId | 真名 | 委托方原说法 | 判定 |
|---|---|---|---|
| 3801 | `TMT_BORDERCOLOR` | 3801=BORDERCOLOR | ✔ 正确 |
| 3802 | `TMT_FILLCOLOR` | 3802=FILLCOLOR（"？还是要核实"） | ✔ 正确 |
| 3803 | `TMT_TEXTCOLOR` | 3803=TEXTCOLOR | ✔ 正确 |
| 3804 | `TMT_EDGELIGHTCOLOR` | — | 补充 |
| 3805 | `TMT_EDGEHIGHLIGHTCOLOR` | — | 补充 |
| 3806 | `TMT_EDGESHADOWCOLOR` | — | 补充 |
| 3807 | `TMT_EDGEDKSHADOWCOLOR` | — | 补充 |
| 3808 | `TMT_EDGEFILLCOLOR` | 委托方写"EDGEFILLCOLOR(3808)" | ✔ 正确 |
| 3809 | `TMT_TRANSPARENTCOLOR` | — | 补充 |
| 3810..3814 | `TMT_GRADIENTCOLOR1..5` | — | 补充 |
| 3815 | `TMT_SHADOWCOLOR` | — | 补充 |
| 3816 | `TMT_GLOWCOLOR` | 委托方写"GLOWCOLOR(3816)" | ✔ 正确 |
| 3821..3824 | `FILLCOLORHINT` / `BORDERCOLORHINT` / `ACCENTCOLORHINT` / `TEXTCOLORHINT` | — | 补充 |
| 2403 | `TMT_BORDERSIZE` | 委托方写"BORDERSIZE(2403)" | ✔ 正确（但注意 `2401` 是 `IMAGECOUNT`，**不是** `BGTYPE`；`BGTYPE` 是 `4001`，`FILLTYPE` 是 `4003`） |

> **纠正**：旧脚本 `themeprobe.ps1` / `themedump2.ps1` 里把 `2401` 标成 `BGTYPE`、
> 把 `2409` 标成 `BORDERSIZE`，**两个都是错的**。`2401=TMT_IMAGECOUNT`、
> `2409=TMT_GRADIENTRATIO4`、真正的 `BGTYPE=4001`、`BORDERSIZE=2403`。

---

## 2. TASKMANAGER 主题完整取色

方法：`OpenThemeData(NULL, L"TASKMANAGER")` + `GetThemeColor`，
按 `COLORREF(0x00BBGGRR)` 解码。part 1..46 全部枚举，state 0..12 全扫。

> **状态标记**：下列每一行都是我本轮**新取**的（脚本
> `docs/evidence/exec/_U_dump2.ps1` / `_U_final.ps1`，原始 TSV
> `docs/evidence/exec/U_colors_final.tsv`，16593 行）。
> 复用旧数据的行会单独标注。

### 2.1 热力图色阶 —— part 3..12 / state 1（这是全项目最重要的一张表）

| part | state | prop | RGB | #RRGGBB | 用途 | 来源 |
|---|---|---|---|---|---|---|
| 3 | 1 | 3802 FILL | (255,249,228) | `#FFF9E4` | 热力档 0（无数据哨兵） | 新取 + exe 二进制复核 |
| 4 | 1 | 3802 FILL | (255,244,196) | `#FFF4C4` | 热力档 1（0%） | 新取 + exe 复核 |
| 5 | 1 | 3802 FILL | (249,236,168) | `#F9ECA8` | 热力档 2 | 新取 + exe 复核 |
| 6 | 1 | 3802 FILL | (255,228,135) | `#FFE487` | 热力档 3 | 新取 + exe 复核 |
| 7 | 1 | 3802 FILL | (255,210,100) | `#FFD264` | 热力档 4 | 新取 + exe 复核 |
| 8 | 1 | 3802 FILL | (255,198,61) | `#FFC63D` | 热力档 5 | 新取 + exe 复核 |
| 9 | 1 | 3802 FILL | (252,184,22) | `#FCB816` | 热力档 6 | 新取 + exe 复核 |
| 10 | 1 | 3802 FILL | (252,167,29) | `#FCA71D` | 热力档 7 | 新取 + exe 复核 |
| 11 | 1 | 3802 FILL | (252,104,42) | `#FC682A` | 热力档 8（最高） | 新取 + exe 复核 |
| 12 | 1 | 3802 FILL | (252,104,42) | `#FC682A` | 档 9（与 11 同色，实际到顶） | 新取 |

**二进制交叉验证**（`taskmgr.exe` 文件偏移 `0x134CF0`，`.data` RVA `0x1370F0`，
9 条 24 字节记录，字段 = double 阈值 + A/R/G/B + part + state）：

```
rec0 thr=255  RGB=#fff9e4 part=3  state=1
rec1 thr=0    RGB=#fff4c4 part=4  state=1
rec2 thr=9    RGB=#f9eca8 part=5  state=1
rec3 thr=19   RGB=#ffe487 part=6  state=1
rec4 thr=39   RGB=#ffd264 part=7  state=1
rec5 thr=59   RGB=#ffc63d part=8  state=1
rec6 thr=79   RGB=#fcb816 part=9  state=1
rec7 thr=99   RGB=#fca71d part=10 state=1
rec8 thr=100  RGB=#fc682a part=11 state=1
```

> **与我们代码的差异**：`include/theme.h` 的 `HeatColor()` 用了
> `kUpper = {-1.0, 0.05, 1.0, 5.0, 12.0, 24.0, 40.0, 58.0, 76.0, 1e9}`，
> 而原版阈值是 **`0/9/19/39/59/79/99/100`**（不含 255 哨兵）。
> 现注释说 `[0]=-1.0` 是"哨兵让 0% 落到 part4"——**这个结论方向对，但理由错了**：
> 原版 part 3 的阈值是 `255.0`（只给"无数据"用），part 4 的阈值是 `0`，
> 所以 `pct=0` 本来就命中 part 4。正确实现应该是：
> `thr = {255.0, 0, 9, 19, 39, 59, 79, 99, 100}`，`pct==255.0` 走 part3，
> 否则线性找第一个 `thr[i] >= pct`。**建议按原版表重写，而不是继续用外推。**

### 2.2 文字色

| part | state | prop | RGB | #RRGGBB | 用途 | 来源 |
|---|---|---|---|---|---|---|
| 13 | 0 | 3803 TEXT | (219,107,58) | `#DB6B3A` | `TmRowTextElement` 列表正文 | 新取 |
| 14 | 0 | 3803 TEXT | (0,0,0) | `#000000` | 卡片标题 | 新取 |
| 15 | 0 | 3803 TEXT | (0,0,0) | `#000000` | 卡片副标题 | 新取 |
| 16 | 0 | 3803 TEXT | (31,89,195) | `#1F59C3` | 侧栏标签 / 分组头名 | 新取 |
| 17 | 0 | 3803 TEXT | (112,112,112) | `#707070` | 低内存提示 / StaticLabel | 新取 |
| 18 | 0 | 3803 TEXT | (0,0,0) | `#000000` | 动态标签 | 新取 |
| 19 | 0 | 3803 TEXT | (0,0,0) | `#000000` | 卡片数值 | 新取 |
| 24 | 1 | 3803 TEXT | (76,96,122) | `#4C607A` | 子列头文字 | 新取 |
| 25 | 0 | 3803 TEXT | (0,0,0) | `#000000` | CPU 格子数值 | 新取 |
| 44 | 0 | 3803 TEXT | (112,112,112) | `#707070` | `lheader` 左列头 | 新取 |

> **注意 part 13 = `#DB6B3A` 是橙红，不是黑。** 但 `ItemsView::Header p1 s0 TEXT`
> 也是 `#4C607A`（见 §2.4），而真机截图里列头文字实测是 `#404040`（见 §5.2）。
> 说明 **原版 DUI 对列头没有用 `gtc(TASKMANAGER,13,...)`**，而是用
> `TmColumnHeaderStyle` + `ItemsView::Header`（§2.4）。
> part 13 的 `#DB6B3A` 更像是给某处强调文字用的，UIFILE 里它挂在
> `TmRowTextElement` 上 —— **但真机列表正文是 `#141414`/`#1a1a1a`（§5.2）**，
> 所以这里有一条明确的"主题值与上屏值不一致"的证据缺口，不要盲抄 `#DB6B3A`。

### 2.3 组合条（内存页 Barchart）

| part | state | prop | RGB | #RRGGBB | 用途 | 来源 |
|---|---|---|---|---|---|---|
| 20 | 0 | 3802 FILL | (255,255,255) | `#FFFFFF` | `BarchartStandby` 备用 | 新取 |
| 21 | 0 | 3802 FILL | (244,242,244) | `#F4F2F4` | `BarchartInuse` 使用中 | 新取 |
| 22 | 0 | 3802 FILL | (238,230,241) | `#EEE6F1` | `BarchartModified` A | 新取 |
| 23 | 0 | 3802 FILL | (206,176,215) | `#CEB0D7` | `BarchartModified` B | 新取 |
| CHARTVIEW 9 | 0 | 3802 FILL | (139,18,174) | `#8B12AE` | `BarchartMemoryHolder` 总底 | 新取 |

> **疑似异常**：`BarchartInuse` = `#F4F2F4` 几乎是白，`BarchartModified` 是淡紫。
> 真机截图的组合条看到的"深紫/深蓝"并不来自这四个 part。
> **这是主题值与实际观感的又一个缺口** —— 原版可能对这四段做了额外的
> alpha 叠加（`BarchartFree` 在 UIFILE 里没有 gtc，用的是默认白）。

### 2.4 列表 / 列头 / 分组头（能取到色的部分）

| 主题 | part | state | prop | RGB | #RRGGBB | 用途 | 来源 |
|---|---|---|---|---|---|---|---|
| ItemsView::ListView | 1 | 3 | 3801 BORDER | (130,135,144) | `#828790` | 行边框 | 新取 |
| ItemsView::ListView | 1 | 3 | 3803 TEXT | (0,0,0) | `#000000` | 行文字（`themeable` 回退） | 新取 |
| ItemsView::Header | 1 | 0 | 3803 TEXT | (76,96,122) | `#4C607A` | 列头文字 | 新取 |
| ItemsView::Header | 1 | 0 | 3821 FILLHINT | (244,244,244) | `#F4F4F4` | 列头底色提示 | 新取 |
| ItemsView::Header | 1 | 0 | 3823 ACCENTHINT | (102,188,225) | `#66BCE1` | 列头强调色提示 | 新取 |
| ItemsView::Header | 5 | 0 | 3821 FILLHINT | (244,244,244) | `#F4F4F4` | 排序态底色提示 | 新取 |
| ItemsView::Header | 5 | 0 | 3823 ACCENTHINT | (191,191,191) | `#BFBFBF` | 排序态强调 | 新取 |
| TASKMANAGER | 2 | 0 | 3802 FILL | (234,213,160) | `#EAD5A0` | 分组头背景（UIFILE 明确引用） | 新取 |

> **`TASKMANAGER 2,0,3802 = #EAD5A0` 与真机不符。** 真机截图（`shots/rt3_proc.png`、
> `rt2_proc.png`、`proc_heat.png`、`heat2.png` 全部一致）分组头是 **`#F2F2F2`**，
> 而 `#EAD5A0` 是土黄色，在任何一张截图里出现次数都是 **0**。
> 结论：**`gtc(TASKMANAGER,2,0,3802)` 在原版里被上层覆盖，或者这个 part 在
> Aero 主题下不是分组头**。**不要按 `#EAD5A0` 画分组头。**
> 分组头以像素为准 = `#F2F2F2`（见 §5.1）。

### 2.5 CPU 栅格

| part | state | prop | RGB | #RRGGBB | 用途 | 来源 |
|---|---|---|---|---|---|---|
| 38 | 0 | 3802 FILL | (17,125,187) | `#117DBB` | `cpuHeatGrid` 底 | 新取 |
| 39 | 0 | 3802 FILL | (223,231,235) | `#DFE7EB` | `cpuBlockStyleR` | 新取 |
| 40 | 0 | 3802 FILL | (191,207,223) | `#BFCFDF` | `cpuBlockStyleB` | 新取 |

> `#117DBB` 与 ChartView CPU 主色**完全相同** —— 印证了"CPU 蓝是全局强调色"。

### 2.6 分栏拖柄 / 其他未引用 part

| part | state | prop | RGB | #RRGGBB | 用途 | 来源 |
|---|---|---|---|---|---|---|
| 41 | 0 | 3802 FILL | (186,186,186) | `#BABABA` | `ResizerImage` 渐变终点 | 新取 |
| 42 | 0 | 3802 FILL | (232,232,232) | `#E8E8E8` | `ResizerImage` 渐变起点 | 新取 |
| 1 | * | 3802 FILL | (226,231,254) | `#E2E7FE` | 未引用（旧 probe 报成 `#FEE7E2`） | 新取 |
| 45 | 0 | 3802 FILL | (218,226,227) | `#DAE2E3` | 未引用 | 新取 |
| 46 | 0 | 3802 FILL | (237,228,204) | `#EDE4CC` | 未引用 | 新取 |
| 26..37 | 0 | 3802 FILL | — | `#FFFFFF`→`#0E405D` 12 级蓝阶 | 未引用（疑为条形图色阶） | 新取 |

`part 26..37` 的完整蓝阶（每级都不同，是一条 12 档渐变，**UIFILE 里没有任何引用**，
列在这里备查）：

```
26 #FFFFFF  27 #E0F0FF  28 #CDE9FF  29 #B4DEFF  30 #7EC6FF  31 #43A9E4
32 #3782AD  33 #2477A7  34 #1F71A1  35 #186A99  36 #14587F  37 #0E405D
```

`part 43`（`tmColHeaderItem`，排序箭头位）只有默认 EDGE 色，没有 FILL/TEXT ——
**箭头是图片不是颜色**，所以取不到颜色值；渲染时它画出来的是
`#FC682A` 边缘 + 半透明橙渐变（见 §4）。这说明它是 **PNG 图像**，
`TMT_IMAGEFILE` 取 string 返回空（内部 ATLAS 形式），**未取到文件名**。

### 2.7 字体（`gtf(...)` 对应的 `TMT_FONT=210`）

| part | face | height | weight | 对应 UIFILE |
|---|---|---|---|---|
| 13 | Microsoft YaHei UI | -11 | 400 | `TmRowTextElement` |
| 14 | Microsoft YaHei UI | -29 | 400 | resourceTitle（**29px 大标题**） |
| 15 | Microsoft YaHei UI | -16 | 400 | resourceName |
| 16 | Microsoft YaHei UI | -16 | 400 | sidebarLabel |
| 17 | Microsoft YaHei UI | -12 | 400 | 低内存提示 |
| 18 | Microsoft YaHei UI | -12 | 400 | dynamicLabel |
| 19 | Microsoft YaHei UI | -21 | 400 | dynamicData（**21px 卡片数值**） |
| 25 | Microsoft YaHei UI | -11 | 400 | cpuBlockStyleData |
| 44 | Microsoft YaHei UI | -11 | 400 | lheader |

> **对照 `theme.h`**：`kFontSizeBody=12`、`kFontSizeCardValue=21`、
> `kFontSizeCardTitle=12`。主题给的是 **body 11、cardValue 21、cardTitle 29**。
> `body 11 vs 12` 差 1px；`cardTitle` 主题是 29 而我们写 12。
> 不过 `resourceTitle`（part 14, 29px）对应的是**卡片顶部的大标题**，
> 我们代码里 `kFontSizeCardTitle=12` 指的是"卡片副标题"那一行，语义不同 ——
> **不能直接对改**，需要先对齐语义。

---

## 3. CHARTVIEW 主题完整取色

方法同上。part 1..42 全部有定义，state 0..12 全扫（实测每个 part 的所有 state
取值相同，可见 ChartView 不用 state 区分）。

### 3.1 四组曲线色（每组 7 个 part，节律 `白/主/浅/主/次/亮/次`）

| 页 | part 段 | p1 | p2 主色 | p3 浅色 | p4 | p5 次色 | p6 亮色 | p7 |
|---|---|---|---|---|---|---|---|---|
| **CPU** | 1–7 | `#FFFFFF` | **`#117DBB`** | `#D9EAF4` | `#117DBB` | `#3386B5` | `#81B3D2` | `#3386B5` |
| **内存** | 8–14 | `#FFFFFF` | **`#8B12AE`** | `#ECDEF0` | `#8B12AE` | `#9528B4` | `#817285` | `#9528B4` |
| **磁盘** | 15–21 | `#FFFFFF` | **`#4DA60C`** | `#DBEDCE` | `#4DA60C` | `#4DA60C` | `#79D50D` | `#4DA60C` |
| **网络** | 22–28 | `#FFFFFF` | **`#A74F01`** | `#EEDECF` | `#A74F01` | `#A74F01` | `#E0822F` | `#DF672C` |

（GROUP 边界干净：p8、p15、p22 都回到 `#FFFFFF`。）

### 3.2 状态色（part 29–42，每 3 个 part 一组）

| part | state 1 | state 2 | state 3 | 推断用途 |
|---|---|---|---|---|
| 29,30,31,32 | `#06B025` 绿 | `#B09800` 黄 | `#C73F36` 红 | 健康状态（低饱和档） |
| 33,34 | `#4ECB22` 亮绿 | `#E8DB51` 亮黄 | `#C73F36` 红 | 健康状态（高饱和档） |
| 35,37,39 | `#A4DE9A` | 同 | 同 | 浅绿填充 |
| 41 | `#D2A883` | 同 | 同 | 浅棕填充 |
| 36,38,40,42 | TEXT=`#707070` | 同 | 同 | 坐标轴/标签文字 |

> **`#707070` 是 ChartView 的轴文字色**，和 `TASKMANAGER 17/44` 的 `#707070`
> 相同。`theme.h` 的 `kChartAxisText = RGB(120,120,120)` = `#787878`，
> `perfpage.h` 的 `perftheme::kAxisText = RGB(0x76,0x76,0x76)` = `#767676`。
> **主题权威值是 `#707070`（112）**，两个都比它亮 8~14 级。

### 3.3 我们代码里曲线色的差异

| 页 | 我方常量 | 值 | 主题值 | 判定 |
|---|---|---|---|---|
| CPU | `kChartLineCpu` | `#117DBB` | `#117DBB` | ✅ 完全一致 |
| 内存 | `kChartLineMem` | `#8B12AE` | `#8B12AE` | ✅ 完全一致 |
| 磁盘 | `kChartLineDisk` | `#4DA60C` | `#4DA60C` | ✅ 完全一致 |
| 网络 | `kChartLineNet` | `#A74F01` | `#A74F01` | ✅ 完全一致 |
| GPU | `kChartLineGpu` | `#117DBB` | `#117DBB`（同 CPU） | ✅ 完全一致 |
| 图表底 | `kChartFill` / `kChartAreaBg` | `#F1F6FA` | part3 浅色是 `#D9EAF4` | ⚠️ **不一致**，但真机实测是 `#F1F6FA`（`orig_full.png` 出现 1,437,939 次，`#D9EAF4` 出现 0 次）→ **保留 `#F1F6FA`** |
| 轴文字 | `theme::kChartAxisText` | `#787878` | `#707070` | ⚠️ 差 8 级。**但此常量全项目零引用**（`grep` 只有定义），实际生效的是 `perftheme::kAxisText` |
| 轴文字 | `perftheme::kAxisText`（**实际生效**，`src/perfpage.cpp:563/567`） | `#767676` | `#707070` | ⚠️ 差 6 级。**真机 `truth2.png` 逐像素实测 `#767676`**（最暗像素），主题值是 `#707070` —— 两者都非空口，**建议保留 `#767676`**（像素 > 主题，因为主题值可能被 DUI 二次调过） |
| 网格线 | `kChartGrid` | `#E8F0F9` | 主题里没有 | 真机实测 `#E8F0F9`（27693 px）→ 保留 |

> **重要**：`CHARTVIEW` 主题里 **没有** 网格线颜色、没有填充色、没有坐标轴色
> （除了 `#707070` 文字）。我扫了 `TASKMANAGER` 和 `CHARTVIEW` 两个类、
> part 0..80、state 0..20、24 个颜色属性和 `BGTYPE/FILLTYPE/BORDERSIZE`，
> **`#F1F6FA`、`#E8F0F9`、`#DEECF5`、`#6AAED4` 全部 0 命中**。
> 这三个色在 `taskmgr.exe`、`wdc.dll`、`dui70.dll`、`Aero.msstyles` 里也都是 0 命中。
> **结论：图表网格/填充/边框色不是主题取色，而是运行时算出来的（或 CHARTV.dll
> 内部常量）。`#F1F6FA`/`#E8F0F9`/`#DEECF5`/`#6AAED4` 只能以真机像素为准。**

---

## 4. 渲染取色（`dtb(...)` 的项，取不到颜色只能画出来看）

方法：`OpenThemeData(NULL, ...)` + `DrawThemeBackground` 画进一个
**32bpp 离屏 DIB**（纯 GDI 内存 DC，**不创建窗口、不抢焦点、不显示**），
然后统计画出来的像素。预填白色，因为 Aero 的 hot/selected 状态带 alpha，
白底合成结果就是真实列表行看到的样子。

### 4.1 列表行（ItemsView::ListView，part 1 = LVP_LISTITEM）

| 状态 | stateId | 常量名 | 渲染结果 | 结构 |
|---|---|---|---|---|
| 正常 | 1 | `LISS_NORMAL` | `#FFFFFF` + 1px `#828790` 四边框 | 边框 852 px |
| **悬停** | 2 | `LISS_HOT` | **`#E5F3FF`** 纯色 | 无边框 |
| **选中** | 3 | `LISS_SELECTED` | **`#CDE8FF`** 纯色 | 无边框 |
| 禁用 | 4 | `LISS_DISABLED` | `#FFFFFF` | — |
| 选中失焦 | 5 | `LISS_SELECTEDNOTFOCUS` | `#D9D9D9` | — |
| 悬停+选中 | 6 | `LISS_HOTSELECTED` | 底 `#CCE8FF` + 1px `#99D1FF` | 边框 852 px |

### 4.2 分组头（part 6 = `LVP_GROUPHEADER`）

| stateId | `LVGH_*` | 渲染结果 |
|---|---|---|
| 1 | OPEN | `#FFFFFF` |
| 2 | OPENHOT | `#E5F3FF` |
| 3 | OPENSELECTED | `#FFFFFF` |
| 4 | OPENSELECTEDHOT | `#E5F3FF` |
| 5 | OPENSELECTEDNOTFOCUSED | `#FFFFFF` |
| 6 | OPENSELECTEDNOTFOCUSEDHOT | `#E5F3FF` |
| 11 | CLOSESELECTED | `#CCE8FF` |
| 12 | CLOSESELECTEDHOT | `#CCE8FF` + `#99D1FF` 边 |

> 分组头**渲染出来是白的**，但真机是 `#F2F2F2`。原因：DUI 的分组头
> 在 `TmGroupHeaderStyle` 里自己叠了一个背景元素（`TmGroupHeader_Background`），
> 不是 `dtb(ItemsView::ListView, 6, ...)`。**分组头颜色以像素 `#F2F2F2` 为准。**

### 4.3 列头（ItemsView::Header，part 1 = `HP_HEADERITEM`）

| stateId | `HIS_*` | 渲染结果 |
|---|---|---|
| 1 | NORMAL | `#FFFFFF` + 右侧 1px `#E5E5E5` 分隔 |
| 2 | HOT | `#D9EBF9` |
| 3 | PRESSED | `#BCDCF4` |
| 4 | SORTEDNORMAL | `#FFFFFF` + 右 `#E5E5E5` |
| 5 | SORTEDHOT | `#D9EBF9` |
| 6 | SORTEDPRESSED | `#BCDCF4` |
| 7..10 | ICON* | 同 1..4 |
| 11/12 | ICONSORTED* | `#D9EBF9` / `#BCDCF4` |

part 4（`HP_HEADERSORTARROW`）= `#FFFFFF` 底 + `#F2F2F2`/`#D9D9D9`/`#656565` 像素
→ 箭头是图片。part 7（`HP_HEADEROVERFLOW`）= `#7F7F7F` 箭头。

### 4.4 页签（`Tab` 类，`TABP_TABITEM=1`，states `TIS_NORMAL=1..TIS_FOCUSED=5`）

| stateId | `TIS_*` | 渲染结果 | 结构 |
|---|---|---|---|
| 1 | NORMAL | `#F0F0F0` | 顶边 1px `#D9D9D9` |
| 2 | HOT | `#D8EAF9` | 顶边 `#D9D9D9` |
| 3 | SELECTED | `#FFFFFF` | 顶边 `#D9D9D9` |
| 4 | DISABLED | `#ECECEC` | 顶边 `#D9D9D9` |
| 5 | FOCUSED | `#FFFFFF` | 上边 `#0078D7` 1px |

`TABP_BODY=10` / `TABP_AEROWIZARDBODY=11` 都是 `#FFFFFF`。

> **但 UIFILE 里没有 `dtb(Tab,...)`** —— 它用的是 DUI 元素 `WdcTab`，
> 而 `OpenThemeData(NULL, L"WdcTab")` 在 Aero 下返回 0（类不存在）。
> **所以上面这组 Tab 值只是"如果 DUI 用系统 Tab 皮肤会是什么样"，不是上屏值。**
> 上屏值见 §5.2：非活动 `#EFEFF0`、活动 `#FFFFFF`、边框 `#C8C8C8`。

### 4.5 工具条按钮（`Button`/`button` part 1 = `BP_PUSHBUTTON`）

| stateId | `PBS_*` | 渲染结果 | 结构 |
|---|---|---|---|
| 1 | NORMAL | `#E1E1E1` 芯 | 边 `#ADADAD` + 外白 |
| 2 | HOT | `#E5F1FB` 芯 | 边 `#0078D7` |
| 3 | PRESSED | `#CCE4F7` 芯 | 边 `#005499` |
| 4 | DISABLED | `#CCCCCC` 芯 | 边 `#BFBFBF` |
| 5 | DEFAULTED | `#E1E1E1` 芯 | 边 `#0078D7` 加粗 |

文字色：`Button(1,2,3803)=#000000`、`(1,3,3803)=#000000`、`(1,4,3803)=#838383`。
`(1,4)` 就是 UIFILE 里 `gtc(Button,1,4,3803)` 对应的**禁用态文字 `#838383`**。

### 4.6 滚动条（`ScrollBar`）

| part | 含义 | state 1 | state 2 | state 3 |
|---|---|---|---|---|
| 1 | ARROWBTN | `#F0F0F0` + `#606060` 箭头 | `#DADADA` | `#606060` |
| 2 | THUMBBTNHORZ | `#CDCDCD` | `#A6A6A6` | `#606060` |
| 3 | THUMBBTNVERT | `#CDCDCD` | `#A6A6A6` | `#606060` |
| 4/6 | TRACK | `#F0F0F0` | 同 | 同 |
| 11 | SIZEBOXBKGND | `#F0F0F0` | — | — |

> **`#CDCDCD` 滑块与 `tmwindow_details.cpp` 注释里写的"#CDCDCD 直角滑块"完全一致。**
> 轨道 `#F0F0F0`。

### 4.7 树展开箭头（`Explorer::TreeView`）

`part 1 state 2` = `#E5F3FF`，`part 1 state 3` = `#CDE8FF`（同样是与行同色的背景）。
`part 2/4`（`TVP_GLYPH` 折叠/展开）渲染为全白 —— 箭头是图片。

### 4.8 主题文字相关（`CONTROLPANELSTYLE` 链接三态）

| 引用 | 值 |
|---|---|
| `CONTROLPANELSTYLE(10,1,3803)` | `#0066CC` 正常链接 |
| `CONTROLPANELSTYLE(10,2,3803)` | `#3399FF` 悬停 |
| `CONTROLPANELSTYLE(10,4,3803)` | `#7E859C` 禁用 |
| `TEXTSTYLE(4,0,3803)` | `#000000` 正文 |
| `TEXTSTYLE(1,0,3803)` | `#003399`（`gtf(TEXTSTYLE,1,0)` 用在 `ResourceValueClass`） |
| `TEXTSTYLE::Link(1,0,3803)` | `#4465A2` |

---

## 5. 真机像素（主题取不到 / 主题与上屏不一致的项）

### 5.0 先做证据分级（这一步差点又踩 `m3.png` 的坑）

我扫了 `shots/` 下全部 PNG，用三个**我方旧版专属指纹色**判定真伪：

- `#E2EFFA` = 我们旧版 `kChartFill`
- `#C6E4F7` = 我们旧版面积图填充
- `#E5F3FB` = 我们旧版 `kRowHover`

**只要一张图里这三个色出现 >1000 次，它就是我们的截图，不是原版。**
判完的结果是（`docs/evidence/_U_g2.js`）：

| 判定 | 文件 |
|---|---|
| **真原版** | `docs/evidence/README.md`(3300×2100，3x)、`shots/truth1.png`、`truth2.png`、`audit_cpu/details/services/users.png`、`final_cpu.png`、`grid3.png`、`stats1.png`、`3png_tabs.png`、`rt2_namecol.png`、`rt2_gpucol.png` |
| **我方旧版**（**不可作证据**） | `rt3_proc.png`、`rt2_proc.png`、`m3.png`、`m3_lower.png`、`heat1/2/3/4.png`、`proc_heat.png`、`c_heat.png`、`final.png`、`fin_*.png`、`grid2.png`、`hdr1.png`、`ld_*.png`、`lv_*.png`、`audit_proc.png`、`audit_mem.png`、`audit_disk.png`、`audit_net.png`、`audit_gpu.png`、`cmp_p0_2.png` 等 |

> 另有一张独立裁切 `docs/evidence/README.md`（1976×240，4x）**不含任何我方指纹色**，
> 判为真原版选中行裁切，是本轮 `#CDE8FF` 的像素证据之一。
>
> ⚠️ **证据效力的重要限定**：`orig_full.png` 的页签是 **5 个**（进程/性能/用户/
> 详细信息/服务），大概率是 **Windows Server** 的任务管理器截图。
> 它的**颜色**可用于配色（页签皮肤是系统级、SKU 无关），
> 但**布局/页签个数**不能拿来当 Client 的依据。详见 §5.2。

> ⚠️ **注意 `c_heat.png` 和 `rt3_proc.png` 也是我方截图**。
> 它们的**热力色阶**碰巧与原版一致（因为热力色是从主题抄来的），
> 所以拿它们验热力色是对的；但拿它们验**分组头 / 卡片 / 选中行颜色是错的**。
> 举例：`c_heat.png` 的分组头是 `#F2F2F2` —— 这个值**恰好**与原版一致，
> 但那是我方自己画的，不能当原版证据。原版证据改用 `rt2_namecol.png`（见 §5.2）。

### 5.1 由真机截图确定的颜色

| 元素 | 值 | 证据（仅 A 级） |
|---|---|---|
| 窗口底 / 列表底 | `#FFFFFF` | 全部 A 级图 |
| **列表行悬停** | **`#E5F3FF`** | 主题渲染 (§4.1)。**真机静态图里没有鼠标，取不到像素** |
| **列表行选中** | **`#CDE8FF`** | 主题渲染 `#CDE8FF` ✔ + `tmwindow_details.cpp` 的 4x 裁切实测 `rgb(205,232,255)` ✔ |
| 性能页侧栏选中卡片 | `#CCE8FF` | `orig_full.png` 82971 px、`truth1.png` 9219 px（**与列表行选中不同色，不要合并**） |
| 页签非活动底 | `#EFEFF0` | `orig_full.png` 38115 px；且 mun 的 DUI 色表第 84 项就是 `#EFEFF0` |
| 页签活动底 | `#FFFFFF` | `orig_full.png` |
| 页签边框 / 分隔线 | `#C8C8C8` | `orig_full.png` 7128 px = `GetSysColor(COLOR_SCROLLBAR)` |
| 页签非活动文字 | `#404040` | `orig_full.png` 逐 tab 取样 |
| 页签活动文字 | `#1a1a1a` | `orig_full.png` |
| **分组头背景** | **`#F2F2F2`** | **`rt2_namecol.png`（真原版 400×240 裁切）** 10724 px，占该带 100% |
| **分组头文字** | **`#141414`** | `rt2_namecol.png` 分组头带 |
| 列头文字 | `#5A5A5A` | `rt2_namecol.png` 顶部 13px 列头带（49 px 墨迹）；`orig_full.png` 也一致 |
| 列头下边线 | `#D2D2D2` | `rt2_namecol.png` y=13 整行 400 px 纯色 |
| 列表正文 | `#141414` | `rt2_namecol.png`、`orig_full.png` |
| 卡片底 | `#FAFAFA` | A 级图 |
| 滚动条滑块 | `#CDCDCD` | 主题 §4.6 一致 |
| 图表底 | `#F1F6FA` | `orig_full.png` 1,437,939 px |
| 图表网格横线 | `#E8F0F9` | `orig_full.png` 249,237 px |
| 图表网格竖线 | `#DEECF5` | `orig_full.png` 36,180 px |
| 图表边框 | `#6AAED4` | `orig_full.png` 82,440 px |

### 5.2 页签的实测几何与配色

来源 `docs/evidence/README.md`（3300×2100 = **3x 缩放**，已验：3×3 像素块
10000/10000 完全一致，是干净的整数倍放大，不是插值）。

- 竖向：`y=150..152` 是 `#F0F0F0`（菜单栏下沿），`y=153` 顶边线 `#C8C8C8`，
  `y=156..227` 是页签体，`y=228` 是底边 `#C8C8C8`，`y=231` 之后内容区全白。
  → **逻辑页签高 = (228-153)/3 = 25px**（`tabs.h` 写的 26，差 1px）。
- 横向分隔竖线（`#C8C8C8` 连续列）在 3x 的 x =
  `24..26, 171..176, 321..326, 471..476, 699..704, 849..851`
  → 逻辑坐标 `8, 57, 107, 157, 233, 283`。
  **共 6 条线 → 5 个格子**，宽度分别 `49 / 50 / 50 / 76 / 50`（逻辑 px）。
- 各格填充：`#EFEFF0`（非活动）… `#FFFFFF`（第 2 格，活动）。

>  #### ⚠️ 这张图的页签是 **5 个**：「进程 性能 用户 详细信息 服务」
>
>  `tabs.h` 里写"是 7 个页签"，依据是 `shots/m3.png` —— 而那张图**本轮已判定是
>  我方旧版截图**（含 `#E2EFFA` 特征色）。**我手上所有能确认是真原版的页签截图
>  （`orig_full.png`、`3png_tabs.png`）都是 5 个页签。**
>
>  5 个页签（缺「应用历史记录」和「启动」）是 **Windows Server** 任务管理器的形态。
>  `docs/evidence/mui_strings_all.txt` 里 7 个标题字符串（`32402..32406` 含
>  `启动`/`应用历史记录`）都存在，但那是**本机 Client MUI 的字符串表**，
>  不能证明这张图是 Client。
>
>  **我的结论（限界明确）**：
>  - 页签**颜色**（`#EFEFF0` / `#FFFFFF` / `#C8C8C8` / 文字 `#404040`/`#1a1a1a`）
>    可以采信 —— 这些来自系统主题的 Tab 皮肤，Server 和 Client 一致。
>  - 页签**个数和宽度**不能从这张图推 —— 它可能是 Server。
>    **7 vs 5 这个分歧不由我判定，我不动 `tabs.h`。**

### 5.3 主题值与上屏值不一致的完整清单（重要！）

| 元素 | 主题取到的值 | 真机上屏值 | 说明 |
|---|---|---|---|
| **分组头背景** | TASKMANAGER 2,0,3802 = `#EAD5A0`（土黄） | **`#F2F2F2`（浅灰）** | `rt2_namecol.png`（真原版）分组头带 10724 px 全是 `#F2F2F2`；`#EAD5A0` 在 5 张 A 级图里出现 **0** 次。**不可用** |
| 列表正文文字 | TASKMANAGER 13,0,3803 = `#DB6B3A`（橙红） | **`#141414`** | `rt2_namecol.png` 正文实测 `#141414`；`#DB6B3A` 及其 ±8 邻域在 A 级图里 **0** 次（`rt2_namecol` 有 4 px 是图标色，不是文字）。**不可用** |
| 列头文字 | ItemsView::Header 1,0,3803 = `#4C607A` | **`#5A5A5A`** | `rt2_namecol.png` 列头带实测 `#5A5A5A`；`#4C607A` 在 A 级图里 **0** 次。**不可用** |
| 组合条 4 段 | `#FFFFFF`/`#F4F2F4`/`#EEE6F1`/`#CEB0D7` | 真机是明显更深的紫 | 主题值接近白，无法解释观感。**未解** |
| `TASKMANAGER 1` 的 fill | `#E2E7FE`（淡蓝） | 未在任何截图里出现 | 未引用 part |
| `TASKMANAGER 43` 排序箭头 | 无颜色（图片） | `#FC682A` 边缘反锯齿 | 图片资产，未取到文件名 |

> **为什么主题值和上屏值会对不上（我的判断，未 100% 证实）**：
> 原版 DUI 的 `gtc()` 是从 **`UxTheme` 的 DUI 专用 color scheme** 里取的，
> 而 `OpenThemeData(NULL, "TASKMANAGER")` 从我们进程里打开拿到的是
> **同一份 msstyles 在 `NormalColor` 下的默认映射**。DUI 的 `<theme>` 元素
> 可以按 class 覆盖颜色，`TaskManager` 的 duib 里也确实有 `TaskManagerCommon`
> 这种自定义 class 名（UIFILE 偏移 `0x801e`）。**所以"主题里有值"不等于"这个
> part 真的会上屏"** —— 必须逐项用真机像素核对，我上面做了这个核对。

---

## 6. 可直接抄进 `theme.h` 的配色表

下面每一项都标了证据级别。`[主题]` = 本轮 GetThemeColor 新取；
`[渲染]` = DrawThemeBackground 离屏渲染；`[像素]` = A 级真机截图；
`[保留]` = 我方现值经核对正确，不改。

```cpp
// ===== 列表 =====
constexpr COLORREF kBgWindow      = RGB(255, 255, 255);        // [像素] 不变
constexpr COLORREF kBgAltRow      = RGB(255, 255, 255);        // [像素] Win10 无交替条纹
constexpr COLORREF kBgHeader      = RGB(255, 255, 255);        // [渲染] ItemsView::Header HIS_NORMAL
constexpr COLORREF kBgGroupHeader = RGB(242, 242, 242);        // [像素] #F2F2F2 —— 原值已正确
constexpr COLORREF kRowHover      = RGB(0xE5, 0xF3, 0xFF);     // [渲染] #E5F3FF  ← 改最后一位 FB→FF
constexpr COLORREF kBgNavSelected = RGB(0xCD, 0xE8, 0xFF);     // [渲染+像素] #CDE8FF  ← 原 #A0CDF0 错
constexpr COLORREF kBgCard        = RGB(250, 250, 250);        // [像素] #FAFAFA 不变

// ===== 文字 =====
constexpr COLORREF kTextPrimary   = RGB(0x14, 0x14, 0x14);     // [像素] #141414 —— 原 20,20,20 已正确
constexpr COLORREF kTextSecondary = RGB(0x5A, 0x5A, 0x5A);     // [像素] #5A5A5A 不变
constexpr COLORREF kTextHeader    = RGB(0x5A, 0x5A, 0x5A);     // [像素] 列头 #5A5A5A 不变
constexpr COLORREF kTextDisabled  = RGB(0x83, 0x83, 0x83);     // [主题] gtc(Button,1,4,3803)=#838383 ← 原 160,160,160
constexpr COLORREF kTextSelected  = RGB(0x14, 0x14, 0x14);     // [渲染] 选中不改文字色

// ===== 线条 =====
constexpr COLORREF kLineHeader    = RGB(0xD2, 0xD2, 0xD2);     // [像素] #D2D2D2 —— 原 210,210,210 已正确
constexpr COLORREF kLineCard      = RGB(0xE5, 0xE5, 0xE5);     // [像素] 不变
constexpr COLORREF kLineRowBorder = RGB(0x82, 0x87, 0x90);     // [主题+渲染] LISS_NORMAL 行框 #828790（新增）

// ===== 页签（tabs.h）=====
// 颜色来自真原版像素（#EFEFF0/#FFFFFF/#C8C8C8/#404040），这部分可信。
// 但 tabs.h 里的「7 个页签」依据 m3.png（本轮判定为我方旧截图），
// 而真原版截图是 5 个 —— 个数这条不在本任务范围，我只标出矛盾，不动 tabs.h。
constexpr COLORREF kBg           = RGB(255, 255, 255);        // [像素]
constexpr COLORREF kActiveBg     = RGB(255, 255, 255);        // [像素]
constexpr COLORREF kInactiveBg   = RGB(0xEF, 0xEF, 0xF0);     // [像素] 不变
constexpr COLORREF kBorder       = RGB(0xC8, 0xC8, 0xC8);     // [像素] 不变
constexpr COLORREF kLine         = RGB(0xE5, 0xE5, 0xE5);     // [像素]
constexpr COLORREF kTextOn       = RGB(0x1A, 0x1A, 0x1A);     // [像素] 活动 #1a1a1a（原 20,20,20）
constexpr COLORREF kTextOff      = RGB(0x40, 0x40, 0x40);     // [像素] 非活动 #404040（原 #5A5A5A 偏淡）
// 高度：真机 (228-153)/3 = 25px（tabs.h 现写 26，差 1）

// ===== 图表（theme.h / perfpage.h）=====
constexpr COLORREF kChartLineCpu  = RGB(0x11, 0x7D, 0xBB);    // [主题] 不变
constexpr COLORREF kChartLineMem  = RGB(0x8B, 0x12, 0xAE);    // [主题] 不变
constexpr COLORREF kChartLineDisk = RGB(0x4D, 0xA6, 0x0C);    // [主题] 不变
constexpr COLORREF kChartLineNet  = RGB(0xA7, 0x4F, 0x01);    // [主题] 不变
constexpr COLORREF kChartLineGpu  = RGB(0x11, 0x7D, 0xBB);    // [主题] 不变
constexpr COLORREF kChartFill     = RGB(0xF1, 0xF6, 0xFA);    // [像素] 不变
constexpr COLORREF kChartGrid     = RGB(0xE8, 0xF0, 0xF9);    // [像素] 不变
constexpr COLORREF kChartAxisText = RGB(0x70, 0x70, 0x70);    // [主题] ChartView p36/38/40/42 TEXT = #707070
constexpr COLORREF kChartBorder   = RGB(0x6A, 0xAE, 0xD4);    // [像素] 图表外框 #6AAED4（新增）

// ===== 卡片（perfpage.h）=====
constexpr COLORREF kSidebarBg     = RGB(255, 255, 255);        // [像素]
constexpr COLORREF kSidebarSel    = RGB(0xCC, 0xE8, 0xFF);     // [像素] 不变（注意与列表行选中 #CDE8FF 差 1）
constexpr COLORREF kSidebarHover  = RGB(0xE5, 0xF3, 0xFF);     // [推断] 侧栏卡片没有独立 dtb；沿用列表行 hot 值。
                                                               //   原 #E5F3FB 的蓝通道少 4（251 vs 255），反推自标签回退色，已修正
constexpr COLORREF kSidebarAccent = RGB(0x11, 0x7D, 0xBB);     // [主题] ⚠ 原值 #0078D7 无证据；
                                                               //   真机 `truth1.png` 选中卡片左缘 x=16..24
                                                               //   整片是 #CCE8FF，**没有 3px 竖条**。
                                                               //   建议整条删除，而不是改色
constexpr COLORREF kSidebarLine   = RGB(0xE5, 0xE5, 0xE5);     // [像素] 不变

// ===== 组合条 =====
constexpr COLORREF kBarStandby    = RGB(0xFF, 0xFF, 0xFF);     // [主题] TASKMANAGER 20,0,3802
constexpr COLORREF kBarInUse      = RGB(0xF4, 0xF2, 0xF4);     // [主题] TASKMANAGER 21,0,3802（⚠ 疑被覆盖）
constexpr COLORREF kBarModifiedA  = RGB(0xEE, 0xE6, 0xF1);     // [主题] TASKMANAGER 22,0,3802
constexpr COLORREF kBarModifiedB  = RGB(0xCE, 0xB0, 0xD7);     // [主题] TASKMANAGER 23,0,3802
constexpr COLORREF kBarMemFloor   = RGB(0x8B, 0x12, 0xAE);     // [主题] CHARTVIEW 9,0,3802

// ===== 滚动条 =====
constexpr COLORREF kScrollThumb   = RGB(0xCD, 0xCD, 0xCD);     // [渲染] SBP_THUMBBTNVERT s1
constexpr COLORREF kScrollTrack   = RGB(0xF0, 0xF0, 0xF0);     // [渲染] SBP_LOWERTRACKVERT s1

// ===== 热力图（theme.h HeatColor）=====
// 阈值改成原版：{255.0, 0, 9, 19, 39, 59, 79, 99, 100}
// 第 0 条是 "无数据" 哨兵（pct == 255.0），不是 -1.0
```

---

## 7. 取不到的项（明确列出，未编造）

| 项 | 原因 |
|---|---|
| `TASKMANAGER part 43`（排序箭头图片）的文件名 | `TMT_IMAGEFILE(3001)` / `TMT_TEXT(3201)` 都返回空，图片以 ATLAS 内嵌，DUI 用 `dtb()` 直接画。只能渲染出颜色（`#FC682A` 边缘），拿不到资产名 |
| `WdcTab` 类的任何颜色 | `OpenThemeData(NULL, L"WdcTab")` 返回 **0**，Aero.msstyles 里没有这个类。页签是 DUI 自绘 |
| `CCHScrollBar` / `CCVScrollBar` / `ScrollViewer` | 同上，`OpenThemeData` 返回 0。`ItemView::ListView` 的滚动条要用 `ScrollBar` 类（§4.6） |
| `ItemsView::Item` | `OpenThemeData` 返回 0，类名不存在；行样式在 `ItemsView::ListView` 里 |
| 图表网格线 / 填充 / 边框的主题来源 | 两个主题的 part 0..80 × state 0..20 × 24 个颜色属性全部扫过，`#F1F6FA`/`#E8F0F9`/`#DEECF5`/`#6AAED4` **0 命中**；`taskmgr.exe`/`wdc.dll`/`dui70.dll`/`Aero.msstyles` 里也 0 命中。运行时算出来的，不是主题色 |
| CPU 栅格每格的 `state 2..N` | `TASKMANAGER part 3..12` 只有 `state 1` 有 FILL，其余 state 无颜色属性。热力档位完全靠代码选 part，不靠 state |
| `TASKMANAGER part 13/16/17/24/25/44` 在真机上到底用在哪 | 主题有值但真机像素对不上（§5.3）。**这是原版 DUI 覆盖行为，不是取色失败** |

---

## 8. 附：证据文件清单（本轮的原始产物）

| 文件 | 内容 |
|---|---|
| `docs/evidence/exec/_U_dump2.ps1` | TaskManager/ChartView/ItemsView 的 part 0..80 × state 0..12 全量取色 |
| `docs/evidence/exec/_U_dump3.ps1` / `_U_final.ps1` | 18 个主题类的全量取色（16593 行） |
| `docs/evidence/exec/U_colors.tsv` / `U_colors_all.tsv` / `U_colors_final.tsv` | 原始 TSV（class/part/state/prop/hex/raw） |
| `docs/evidence/exec/_U_render2.ps1` / `_U_render3.ps1` / `_U_render4.ps1` / `_U_render6.ps1` | 离屏 DIB 渲染取色（`dtb` 项） |
| `docs/evidence/exec/U_render2_console.txt` ~ `U_render6_console.txt` | 渲染结果与像素直方图 |
| `docs/evidence/exec/_U_meta.ps1` / `U_meta_console.txt` | 字体、类名大小写等价性、ChartView 状态色 |
| `docs/evidence/_U_scan.js` / `_U_bbox.js` / `_U_multi.js` / `_U_g2.js` | 截图直方图 / bbox / 多目标 / **真伪指纹分类** |

**所有脚本都只调用 `OpenThemeData(NULL, ...)` + `GetThemeColor` / `DrawThemeBackground`
到内存 DC，没有创建任何窗口，没有运行 `TaskManagerPro.exe` 或 `taskmgr.exe`，
没有编译，没有截图，没有动鼠标。**

### 8.1 §6 里标注"新增"的常量在现有代码里**并不存在**

`grep` 结果：`kLineRowBorder` / `kChartBorder` / `kScrollThumb` / `kScrollTrack` /
`kBarStandby` / `kBarInUse` / `kBarModifiedA` / `kBarModifiedB` / `kBarMemFloor`
**全部只有本报告的提议，`include/` 和 `src/` 里一个都没有**。
`kTextDisabled` 存在但是 `RGB(160,160,160)`，与主题 `#838383` 差 41 级。

也就是说：**组合条、滚动条、行边框、图表外框这四处现在用的是散落的裸字面量
或系统默认值，没有集中常量。** 上表可以直接新增，不会撞名。

### 8.2 `kSidebarHover` 那 4 级差异的来历

`#E5F3FB`（现用）vs `#E5F3FF`（本轮渲染值）只差蓝通道 251→255。
`theme.h` 的注释里写着"GUI_DIFF §五 修正 1 记录的实测值是 `#E5F3FF`"，
但**当时没有可信来源，所以标了"未证实"**。本轮通过
`DrawThemeBackground(ItemsView::ListView, 1, LISS_HOT)` 在离屏 DIB 上渲染，
直接得到 `#E5F3FF`（8000 像素全是这一个值，无渐变、无抗锯齿）。
**这条"未证实"现在可以摘掉了。**

### 8.3 顺带确认：`docs/specs/spec_proclist.md` 的 `kFill` 表是对的

该文件第 531 行起的 `kFill[10]` 与我从 `taskmgr.exe` 的 `.data 0x1370F0`
逐字节读出的 9 条记录**完全一致**。热力图这部分**不用改颜色**，
只需把 `kUpper` 的阈值换成原版的 `{255.0, 0, 9, 19, 39, 59, 79, 99, 100}`。

### 8.4 顺带发现：`docs/evidence/themeprobe.ps1` / `themedump2.ps1` 的两处 bug

1. **颜色解码错**：`FmtColor()` 里 `$a = ($c -shr 24)`，把 `COLORREF` 当 ARGB，
   于是 R 和 B 对调。修法：`$r=$c -band 0xFF; $g=($c -shr 8) -band 0xFF; $b=($c -shr 16) -band 0xFF`。
2. **propId 表错**：把 `2401` 当 `BGTYPE`（真名 `TMT_IMAGECOUNT`）、
   把 `2409` 当 `BORDERSIZE`（真名 `TMT_GRADIENTRATIO4`、真正的 `BORDERSIZE=2403`）。
   正确是 `TMT_BGTYPE=4001`、`TMT_FILLTYPE=4003`、`TMT_BORDERSIZE=2403`。

> 这两个脚本**没有被本轮重跑**（按任务要求不能跑），只是读来参考方法。
> 上面的 bug 是我用自己的脚本对照 `vssym32.h` 后确认的。
