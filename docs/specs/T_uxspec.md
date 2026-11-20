# Agent T — Win10 UX 规范

状态：进行中

> 本文件由 Agent T 生成。所有数值均标注来源。
> 测量环境：Windows 10 Pro 22H2 build **19045**（`[Environment]::OSVersion` = 10.0.19045.0），zh-CN，
> 系统 DPI = **96**（`GetDpiForSystem()`），当前主题 =
> `C:\Windows\resources\themes\Aero\Aero.msstyles`，color scheme = `NormalColor`，size = `NormalSize`
> （`GetCurrentThemeName`）。
> **注意：本机 `AppsUseLightTheme = 0`（深色模式开启）**，但 Win10 的 `aero.msstyles` 只有一套浅色度量，
> 深色模式只影响 Explorer/系统 UI，不影响下表的主题度量。

## 测量方法（可复现）

全部通过 PowerShell `Add-Type` 在内存中 P/Invoke，**未落盘 DLL、未创建任何窗口、未运行任何 exe、未编译**：

- `OpenThemeData(IntPtr.Zero, class)` — 传 `NULL` hwnd 即可打开主题，无需窗口
- `GetThemePartSize(h, hdc, part, state, NULL, TS_MIN|TS_TRUE|TS_DRAW, &sz)` — eSize: 0=TS_MIN, 1=TS_TRUE, 2=TS_DRAW
- `GetThemeColor` / `GetThemeInt` / `GetThemeMargins` — 属性 ID 取自 `vssym32.h`
  （`TMT_FILLCOLOR`=3802, `TMT_TEXTCOLOR`=3803, `TMT_BORDERCOLOR`=3801, `TMT_BORDERSIZE`=2403,
  `TMT_SIZINGMARGINS`=3601, `TMT_CONTENTMARGINS`=3602）
- **关键技巧**：`aero.msstyles` 的大多数控件状态是**位图图像**（BGTYPE=5/6/8/12），
  `GetThemeColor` 对这些 part 返回的是类级回退常量（对 ListView 全部状态都返回 `#FFFFFF`），
  **完全不可用于取色**。真实颜色必须用 `DrawThemeBackground` 渲染到 32bpp DIB 上再逐像素采样。
  本报告的所有"渲染"值都由该方法得到。

---

## 0. 全局字体（一切高度的基准）

来源：**注册表实测** `HKCU:\Control Panel\Desktop\WindowMetrics`

| 项 | 值 |
|---|---|
| CaptionFont | `lfHeight=-12, weight=400, charSet=1, "Microsoft YaHei UI"` |
| SmCaptionFont | 同上 |
| MenuFont | 同上 |
| StatusFont | 同上 |
| MessageFont | 同上 |

`lfHeight = -12` → **9 pt**（负值 = 字符高度，`-12 * 72/96 = 9pt`）。
本机 UI 字体是 **Microsoft YaHei UI**（中文系统），英文系统是 **Segoe UI**，两者同为 9pt。

GDI 实测（`GetTextMetrics` + `CreateFontIndirect`，96 DPI）：

| 字体 | tmHeight | tmAscent | tmDescent | tmInternalLeading | 'Ag' 宽 |
|---|---|---|---|---|---|
| Segoe UI 9pt (`lfHeight=-12`) | **15** | 12 | 3 | 3 | 15 |
| Microsoft YaHei UI 9pt (`lfHeight=-12`) | **17** | 13 | 4 | 5 | — |
| Segoe UI 8pt (`-11`) | 13 | 11 | 2 | 2 | 14 |
| Segoe UI 10pt (`-13`) | 17 | 14 | 3 | 4 | 16 |

> 微软文档明确 9pt 是标准正文字号：
> "**The standard font size has been increased to 9 point.**" /
> "**For Segoe UI, use a 9 point font size or larger.**"
> — https://learn.microsoft.com/en-us/windows/win32/uxguide/vis-fonts

---

## 1. 列表（`SysListView32` / `LVS_REPORT`）

### 1.1 行高 —— **主题不定义，API 不返回行高**

**这是本题最重要的纠正**：`GetThemePartSize(LVP_LISTITEM, LISS_NORMAL)` **并不返回行高**。

实测（来源：API 实测，`TS_TRUE`）：

| class | part/state | TS_MIN | TS_TRUE |
|---|---|---|---|
| `ListView` | LVP_LISTITEM(1) / s1..s6 全部 | 2x2 | **3x3** |
| `Explorer::ListView` | LVP_LISTITEM/s1 NORMAL | 2x2 | **3x3** |
| `Explorer::ListView` | LVP_LISTITEM/s2 HOT | 2x2 | **7x7** |
| `Explorer::ListView` | LVP_LISTITEM/s3 SELECTED | 2x2 | **7x7** |
| `Explorer::ListView` | LVP_LISTITEM/s5 SELNOTFOCUS | 2x2 | **7x7** |
| `Explorer::ListView` | LVP_LISTITEM/s6 HOTSELECTED | 2x2 | **7x7** |

`LVP_LISTITEM` 的 `TS_TRUE` 只有 **3x3**（通用类）/ **7x7**（Explorer 选中态），
这是**选中背景九宫格的圆角扩展量**，不是行高。`LVP_LISTITEM` 也**没有定义任何 `TMT_SIZINGMARGINS`**
（实测返回 `na`），所以主题层根本不含行高信息。

行高由 comctl32 在运行时按字体计算。可用的推导式：

```
行高 = tmHeight(字体) + 2 × 垂直内边距
```

- 微软 UX 指南对列表行高 **官方未规定**；
  "Recommended sizing and spacing" 一节只有一句定性描述
  "Choose a list view height that displays an integral number of items. Avoid truncating items vertically."
  — https://learn.microsoft.com/en-us/windows/win32/uxguide/ctrl-list-views
- **结论：规范行高 = 无法确定（微软未规定，主题未定义）。**
  这不是"没查到"，而是**规范层面刻意不存在这个数**。
- 工程上唯一自洽的做法：行高 = `tmHeight + 2×pad`，`pad` 取 2（紧凑）或 3（标准）。
  本机 Segoe UI 9pt（tmHeight 15）→ 19~21px；YaHei UI 9pt（tmHeight 17）→ 21~23px。
  **此式为推导，非官方值**，落地时应作为可调常量。

### 1.2 列头高度 —— **24px**（高置信度）

来源：API 实测 `GetThemePartSize(HEADER, HEADERITEM, HIS_NORMAL, TS_TRUE)`

| part | TS_MIN | TS_TRUE |
|---|---|---|
| **HEADERITEM(1)** | 1x1 | **6x24** |
| HEADERITEMLEFT(2) | 2x23 | 3x24 |
| HEADERITEMRIGHT(3) | 2x23 | 3x24 |
| HEADERSORTARROW(4) | 1x1 | 13x5 |
| HEADERDROPDOWN(5) | 1x1 | 3x24 |
| HEADEROVERFLOW(7) | 1x1 | 8x21 |

- **列头标准高度 = 24 px @96DPI**。`HEADERITEMLEFT/RIGHT` 的 `TS_MIN.cy = 23`、
  `TS_TRUE.cy = 24` 互相印证；`HEADERDROPDOWN` 的 `TS_TRUE.cy = 24` 再次印证。
- 该值**与字体无关**：分别用 `lfHeight=-12/-15/-18`（96/120/144 DPI 等效字体）和 YaHei 测，
  全部返回 `6x24` —— 因为 aero 的 header 高度写死为 24。
- `HEADER` 与 `Explorer::Header` 返回**完全相同**的尺寸。

### 1.3 列头内边距 / 文字对齐

来源：API 实测 `GetThemeMargins(HEADERITEM, HIS_NORMAL, TMT_SIZINGMARGINS)`

| 项 | 值 |
|---|---|
| SIZINGMARGINS | **L2 R3 T9 B14** |
| CONTENTMARGINS | **L0 R0 T0 B0** |
| 主题字体 | 未定义（`TMT_FONT` 返回 na）→ 取系统 9pt |
| 主题文字色 | 未定义（`TMT_TEXTCOLOR` 返回 na）→ 取 `COLOR_BTNTEXT` = `#000000` |
| BGTYPE | 12（= 位图图像，非纯色） |

文字对齐：**列头文字左对齐**是 Win32 `HDITEM.fmt` 的默认（`HDF_LEFT`），
非主题控制；主题只提供背景与内边距。微软 UX 指南对列头对齐 **官方未规定**。

题外：`TMT_ADDTEXTFLAGS` 实测 = `16053492` = `0x00F50074`，含 `DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS` 类标志。

### 1.4 行间分隔线 —— **默认没有；`LVS_EX_GRIDLINES` 控制**

- `LVS_EX_GRIDLINES` 官方定义：
  > "**LVS_EX_GRIDLINES** — Displays gridlines around items and subitems.
  > This style is available only in conjunction with the **LVS_REPORT** style."
  > — https://learn.microsoft.com/en-us/windows/win32/controls/extended-list-view-styles
- 主题层面**没有**"行分隔线"part：`LVP_LISTITEM` 无 border 图元、无底边线 part。
  实测 `ListView` 类无任何 row-separator 视觉。
- **Win10 Explorer 默认不使用 `LVS_EX_GRIDLINES`**（Explorer 的列表是无线条的）——
  但 Explorer 究竟设了哪些 `LVS_EX_*`，**微软官方未公开，无法从文档确定**。
- 结论：**规范做法 = 不画行间分隔线**。需要时用 `LVS_EX_GRIDLINES`，其线色不由主题单独定义
  （走 `TMT_BORDERCOLOR` 回退）。
- 全行选中用 `LVS_EX_FULLROWSELECT`（UX 指南要求 "Use the Full Row Select attribute"）。

### 1.5 选中 / 热行配色 —— **必须用 `Explorer::ListView` 类**

**关键发现**：通用 `ListView` 类在 Win10 aero 中**所有状态都渲染成纯白**（实测 s1..s6 全部 `#FFFFFF`），
因为它只是"控件壳"；真正的选中/悬停视觉在 **`Explorer::ListView`** 子类里。

来源：**API 实测（`DrawThemeBackground` 渲染到 DIB 后像素采样）**

| state | 语义 | 渲染色（白底） | 说明 |
|---|---|---|---|
| s1 | LISS_NORMAL | `#FFFFFF` | 普通行（不绘制） |
| s2 | LISS_HOT | **`#E5F3FF`** | 悬停行（浅蓝） |
| s3 | LISS_SELECTED | **`#CDE8FF`** | 选中行（焦点态） |
| s4 | LISS_DISABLED | `#FFFFFF` | 禁用 |
| s5 | LISS_SELECTEDNOTFOCUS | **`#D9D9D9`** | 选中但失焦（灰） |
| s6 | LISS_HOTSELECTED | **`#CCE8FF`** | 悬停+选中 |

#### 关键：这些是**半透明叠加**，不是不透明填充

在**品红底**和**白底**上各渲染一次，解方程反解出源色 `C` 与 alpha：

| state | 白底结果 | 反解 `C` | **alpha** |
|---|---|---|---|
| LISS_HOT | `#E5F3FF` | **`#0089FF`** | **10.2%** |
| LISS_SELECTED | `#CDE8FF` | **`#008AFF`** | **19.6%** |
| LISS_SELECTEDNOTFOCUS | `#D9D9D9` | **`#000000`** | **14.9%** |
| LISS_HOTSELECTED | `#CCE8FF` | **`#008CFF`** | **20.0%** |

验证（正向合成，全部吻合）：
- `#0089FF` @10% over `#FFFFFF` → `(229,243,255)` = `#E5F3FF` ✓
- `#0089FF` @20% over `#FFFFFF` → `(205,232,255)` = `#CDE8FF` ✓
- `#000000` @15% over `#FFFFFF` → `217` = `#D9D9D9` ✓

**规范结论**：
```
选中/悬停 = 在行背景上叠加 #0089FF，alpha 20%（选中）/ 10%（悬停）
选中失焦   = 在行背景上叠加 #000000，alpha 15%
```
**落地建议：实现成 alpha 混合，不要把 `#CDE8FF` 写死成纯色。**
理由：Win10 故意用半透明，这样叠在任何行底色上都成立（斑马纹、图表单元格等）。
写死纯色在非白底上会出错。这是本报告对实现方式最重要的一条。

- `TMT_FILLCOLOR` 实测 = `#FFFFFF`（**对所有状态**），`TMT_BORDERCOLOR` = `#828790`，
  `TMT_BORDERSIZE` = 1 —— **但这是类级回退常量，不是实际渲染色，不要直接抄**。
  渲染值以上表为准。
- **选中行无边框**：渲染结果是纯平填充（`#CDE8FF`），`LVP_LISTITEM` 在 Explorer 类下
  `TS_TRUE = 7x7` 仅为圆角/内缩区，无描边。`TMT_BORDERSIZE=1` 的 1 不对应可见描边。
- **选中文字色**：主题未定义（`TMT_TEXTCOLOR` 对所有状态 `na`）。
  规范上走系统 `COLOR_HIGHLIGHTTEXT` = `#FFFFFF`（本机实测）。
  注意 Win10 Explorer 的浅蓝选中其实是配**黑字**的（因为底色是浅蓝而非深蓝），
  这是 Explorer 的绘制逻辑，**主题层不提供**，属于应用自己决定 → **无法从主题确定**。

### 1.6 其它 ListView part 尺寸（API 实测）

| part | TS_MIN | TS_TRUE |
|---|---|---|
| LVP_LISTGROUP(2) | 2x2 | 3x3 |
| LVP_LISTDETAIL(3) | 2x2 | 3x3 |
| LVP_LISTSORTEDDETAIL(4) | 2x2 | 3x3 |
| LVP_EMPTYTEXT(5) | 2x2 | 3x3 |
| LVP_GROUPHEADER(6) | 1x1 | 1x1 |
| LVP_EXPANDBUTTON(8) | 1x1 | **15x15** |
| LVP_COLLAPSEBUTTON(9) | 1x1 | **15x15** |
| LVP_COLUMNDETAIL(10) | 1x1 | 2x1 |

---

## 2. 页签（`SysTabControl32`）

### 2.1 页签尺寸

来源：**API 实测**

| part | TS_MIN | TS_TRUE |
|---|---|---|
| TABITEM(1) / s1,s3 | 1x1 | **5x19** |
| TOPTABITEM(5) / s1,s3 | 1x1 | **5x19** |
| TABITEMBOTHEDGE(4) | 1x1 | **5x19** |
| TOPTABITEMBOTHEDGE(8) | 1x1 | **5x19** |
| TABBODY(9) | 8x8 | 5x4 |

- **页签边框图元的最小尺寸 = 5 x 19 px @96DPI**（这是 `GetThemePartSize` 直接返回的值）。
- 与字体无关（`-12/-15/-18` + YaHei 全部返回 `5x19`）。

### 2.2 页签内边距

来源：**API 实测 `TMT_SIZINGMARGINS`，已验证与矩形无关**
（在 100x24 / 100x40 / 300x100 三种 rect 下返回值完全相同）

| part | SIZINGMARGINS | CONTENTMARGINS |
|---|---|---|
| **TABITEM(1)** | **L2 R2 T13 B4** | L0 R0 T0 B0 |
| TOPTABITEM(5) | L2 R2 T13 B4 | L0 R0 T0 B0 |
| **TABBODY(9)** | **L1 R3 T1 B2** | L4（四面 4） |

T=13 / B=4 是页签背景的上下"可拉伸边框"区（顶部 13px 是圆角+高光带）。

**推导页签总高**（非 API 直接返回，标注为推导）：
```
页签高 = tmHeight + T + B = tmHeight + 13 + 4
       = Segoe UI 9pt:  15 + 17 = 32 px
       = YaHei UI 9pt:  17 + 17 = 34 px
```
**此值为推导，置信度中等**。`:5x19` 是边框图元尺寸、`32` 是合成后总高，两者不是同一个量。

### 2.3 页签配色

来源：**API 实测（渲染 + 像素采样）**

| state | 语义 | 底色 | 边框色 |
|---|---|---|---|
| s1 | TIS_NORMAL | **`#F0F0F0`** | `#D9D9D9` |
| s2 | TIS_HOT | **`#D8EAF9`** | `#D9D9D9` |
| s3 | TIS_SELECTED | **`#FFFFFF`** | `#D9D9D9` |
| s4 | TIS_DISABLED | `#F0F0F0` | `#D9D9D9` |
| — | TABBODY（页体） | **`#FFFFFF`** | `#D9D9D9`（另含 `#F0F0F0` 边带） |

- `TMT_BORDERSIZE` = **0**（所有 TABITEM state）—— 边框是位图画出来的，不是 1px 描边。
- **文字色主题未定义**：`TMT_TEXTCOLOR` 对 `TAB` / `TaskManager` / `Explorer::TabView`
  的 TABITEM 全部返回 `na`。→ 取系统 `COLOR_BTNTEXT` = `#000000`。
- `TMT_FILLCOLOR` 实测 = `#4C85C7`（所有 state 相同）—— **这是类级回退，不是实际渲染色，
  不要用**。实际渲染值是上表。
- `TMT_ACCENTCOLORHINT` = `#929292`（TABITEM）/ `#9C9C9C` 量级（TABBODY）。

### 2.4 Win10 页签是"带边框方块"还是"下划线"？

**规范层面答案：Win10 经典 Win32 `SysTabControl32` 是"带边框方块"（bordered box），不是下划线。**

依据（均为实测，非印象）：

1. 主题为每个 TABITEM 提供**四向可拉伸边框**（`TABITEMBOTHEDGE` / `TABITEMLEFTEDGE` /
   `TABITEMRIGHTEDGE` / `TOPTABITEM*` 共 8 个变体），这是"方块"结构的直接证据——
   下划线方案只需要一条横线图元，不需要左右边缘 part。
2. `SIZINGMARGINS R2`（右边框 2px）+ `BORDERSIZE` 的存在说明有独立边框带。
3. 渲染实测：未选中 `#F0F0F0` 填充 + `#D9D9D9` 边框，选中 `#FFFFFF` 填充 + `#D9D9D9` 边框，
   **是一块有边界的面**，不是一条线。
4. `TOPTABITEM` 与 `TABITEM` 度量完全相同（`5x19`, `L2 R2 T13 B4`）—— 只是朝向不同。

**下划线（underline indicator）是 WinUI / UWP `Pivot` 的范式，不是 Win32 TabControl 的范式。**
微软官方**从未**文档化"Win10 页签改成了下划线"这一说法（官方未规定）。
WinUI 的 `TabView` 是另一套（`TabViewItemMinHeight = 32`，
`TabViewItemHeaderPadding = 8,3,4,3`，`TabViewItemHeaderFontSize = 12`）——
**不要把它当成 Win32 TabControl 的规范**。

> 结论：**复刻 Win10 任务管理器就用带边框方块页签**，底色 `#F0F0F0`/选中 `#FFFFFF`，
> 边框 `#D9D9D9`。若改用下划线，那是 Fluent/WinUI 风格，不是 Win10 桌面规范。

---

## 3. 按钮与工具条

来源：**API 实测**

| 控件 | part | TS_MIN | TS_TRUE | SIZINGMARGINS | CONTENTMARGINS |
|---|---|---|---|---|---|
| 标准按钮 | BP_PUSHBUTTON(1) | 6x6 | **13x11** | **L6 R6 T5 B5** | **L3 R3 T3 B3** |
| 复选框 | BP_CHECKBOX(3) | — | **13x13** | — | — |
| 工具条按钮 | TP_BUTTON(1) | 1x1 | **7x18** | **L3 R3 T13 B4** | L0 |
| 工具条分隔符 | TP_SEPARATOR(5) | 5x4 | — | L4 | — |

`BP_PUSHBUTTON` 另有 `TMT_MINIMUMW = 10`。

**按钮高度规范值**：微软 UX 指南给出 **75 x 23 relative pixels**
（"Command buttons | 50 x 14 | 75 x 23"，9pt Segoe UI @96dpi），
并且注明可见尺寸是 73x21（外围 1px 透明边框）：
> "Actual control size: 50 DLUs (75 pixels) × 14 DLUs (23 pixels)" /
> "Visible size: 49 DLUs (73 pixels) × 13 DLUs (21 pixels)"
> — https://learn.microsoft.com/en-us/windows/win32/uxguide/vis-layout
> — https://learn.microsoft.com/en-us/windows/win32/uxguide/ctrl-command-buttons

即 **按钮规范高 = 23 px @96DPI**（主题的 `13x11` 是边框图元最小尺寸，不是按钮高）。
这两个数是不同层面的量，不要混用：**布局用 23，主题绘制用 13x11**。

**工具条规范**：微软工具条图给出 **条高 28 px，图标 16x16 px**，边距 1/2/3px。
— https://learn.microsoft.com/en-us/windows/win32/uxguide/cmd-toolbars
（主题 `TP_BUTTON` 的 `7x18` 是按钮边框图元，`28` 是整条高度。）

### 3.1 按钮配色（渲染实测）

| state | 语义 | 填充 | 边框 | 文字 |
|---|---|---|---|---|
| s1 | NORMAL | **`#E1E1E1`** | **`#ADADAD`** | `#000000` |
| s2 | HOT | **`#E5F1FB`** | **`#0078D7`** | `#000000` |
| s3 | PRESSED | **`#CCE4F7`** | **`#005499`** | `#000000` |
| s4 | DISABLED | **`#CCCCCC`** | `#BFBFBF` | **`#838383`** |
| s5 | DEFAULTED | `#E1E1E1` | **`#0078D7`**（加粗） | `#000000` |

`TMT_TEXTCOLOR` 实测：s1/s2/s3/s5 = `#000000`，s4 = `#838383`（= `0x00838883` 的 BGR 解）。
`TMT_ACCENTCOLORHINT` = `#8E8F8E`。

按钮这些值是**不透明**的（品红/白底双测反解 alpha = 100%），可直接写死为纯色。

### 3.2 工具条按钮配色（渲染实测）

| state | 语义 | 白底渲染色 | 反解叠加 |
|---|---|---|---|
| s1 | NORMAL | **完全不绘制**（`inkBox=none`） | — |
| s2 | HOT | **`#E5F3FF`** | `#0089FF` @ **10%** |
| s3 | PRESSED | **`#CCE8FF`** | `#008CFF` @ **20%** |
| s4 | DISABLED | 不绘制 | — |
| s5 | CHECKED | **`#CCE8FF`** | 同 PRESSED，@ **20%** |

**工具条按钮与列表选中用同一套叠加模型**（`#0089FF` + 10%/20%），这不是巧合，
是 Win10 的统一交互色语言。→ **建议全项目共用一组 hover/pressed 常量。**

`TMT_TEXTCOLOR` = `#000000`（s4 = `#6D6D6D`），`TMT_FILLCOLOR` = `#F0F0F0`（类级回退）。
**工具条普通态不画背景**，这是 Win10 的标准做法（平面化），只有热/按下/选中才画。

---

## 4. 滚动条

### 4.1 宽度

| 来源 | 值 |
|---|---|
| `GetSystemMetrics(SM_CXVSCROLL)` @96DPI | **17** |
| `GetSystemMetricsForDpi(2, 120/144/168/192)` | 21 / 26 / 30 / **34** |
| `SM_CYHSCROLL` @96DPI | **17** |
| `GetThemeSysSize(2)`（主题系统尺寸） | **17** |
| `SCROLLBAR / SBP_ARROWBTN / TS_TRUE`（主题） | **17x17** |

**三者一致：17 px @96DPI。** 主题 part 尺寸与 `SM_CXVSCROLL` 完全吻合。

> 微软文档**只定义索引不定义值**：
> "**SM_CXVSCROLL** — 2 — The width of a vertical scroll bar, in pixels."
> — https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getsystemmetrics
> 所以"17"是**本机实测值**，不是文档常量。正确做法是运行时读 `GetSystemMetricsForDpi`，
> 按 `MulDiv(17, dpi, 96)` 亦可（实测 120→21 = 17*1.25 取整，144→26 = 17*1.5 取整，完全吻合）。

### 4.2 滑块最小尺寸

| part | TS_TRUE |
|---|---|
| **SBP_THUMBBTNVERT(3)** | **17 x 11** |
| SBP_THUMBBTNHORZ(2) | 11 x 17 |
| SBP_ARROWBTN(1) | **17 x 17** |
| SBP_SIZEBOX(8) | 10x9（s1）/ 10x9（s2） |

**滑块最小长度 = 11 px**（垂直滑块的高 / 水平滑块的宽）。
`SCROLLBAR` 没有定义 `TMT_MINIMUMSIZE`（实测 `na`），11 是九宫格的最小可绘制长度，
即工程上滑块不得小于 11px。箭头按钮 17x17 与轨道同宽。

### 4.3 滚动条配色（渲染实测）

| 元素 | state | 颜色 |
|---|---|---|
| 滑块 | s1 NORMAL | **`#CDCDCD`**（另含 `#F0F0F0` 边、`#FFFFFF` 高光） |
| 滑块 | s2 HOT | **`#A6A6A6`** |
| 滑块 | s3 PRESSED | **`#606060`** |
| 轨道 | — | **`#F0F0F0`**（= `COLOR_BTNFACE`，`SP_PANE` 实测同值） |
| 箭头按钮 | s1 | 底 `#F0F0F0`，箭头 `#606060` |

`COLOR_SCROLLBAR`（`GetSysColor(0)`）实测 = `#C8C8C8`（与滑块 `#CDCDCD` 接近但不等，
前者是纯色回退，后者是渲染结果）。

---

## 5. 颜色体系

### 5.1 `GetSysColor` 全套（本机实测，@96DPI，浅色 aero）

| 索引 | 名称 | 值 |
|---|---|---|
| 0 | COLOR_SCROLLBAR | `#C8C8C8` |
| 1 | COLOR_BACKGROUND | `#000000` |
| 2 | COLOR_ACTIVECAPTION | `#99B4D1` |
| 3 | COLOR_INACTIVECAPTION | `#BFCDDB` |
| 4 | COLOR_MENU | `#F0F0F0` |
| 5 | **COLOR_WINDOW** | **`#FFFFFF`** |
| 6 | COLOR_WINDOWFRAME | `#646464` |
| 7 | COLOR_MENUTEXT | `#000000` |
| 8 | **COLOR_WINDOWTEXT** | **`#000000`** |
| 9 | COLOR_CAPTIONTEXT | `#000000` |
| 10 | COLOR_ACTIVEBORDER | `#B4B4B4` |
| 11 | COLOR_INACTIVEBORDER | `#F4F7FC` |
| 12 | COLOR_APPWORKSPACE | `#ABABAB` |
| 13 | **COLOR_HIGHLIGHT** | **`#0078D7`** |
| 14 | **COLOR_HIGHLIGHTTEXT** | **`#FFFFFF`** |
| 15 | **COLOR_BTNFACE** | **`#F0F0F0`** |
| 16 | COLOR_BTNSHADOW | `#A0A0A0` |
| 17 | **COLOR_GRAYTEXT** | **`#6D6D6D`** |
| 18 | COLOR_BTNTEXT | `#000000` |
| 19 | COLOR_INACTIVECAPTIONTEXT | `#000000` |
| 20 | COLOR_BTNHIGHLIGHT | `#FFFFFF` |
| 21 | COLOR_3DDKSHADOW | `#696969` |
| 22 | COLOR_3DLIGHT | `#E3E3E3` |
| 23 | COLOR_INFOTEXT | `#000000` |
| 24 | COLOR_INFOBK | `#FFFFE1` |
| 26 | COLOR_HOTLIGHT | `#0066CC` |
| 27 | COLOR_GRADIENTACTIVECAPTION | `#B9D1EA` |
| 28 | COLOR_GRADIENTINACTIVECAPTION | `#D7E4F2` |
| 29 | COLOR_MENUHILIGHT | `#3399FF` |
| 30 | COLOR_MENUBAR | `#F0F0F0` |

> 微软对高亮色的语义定义（不是具体值）：
> "**COLOR_HIGHLIGHT** — Background or accent color for UI that is in selected, interacted with
> (hover, pressed), or in progress. Pair with COLOR_HIGHLIGHTTEXT."
> — https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getsyscolor

### 5.2 强调色（Accent）

本机实测（`HKCU\Software\Microsoft\Windows\DWM`）：

| 项 | 原始值 | 解码 | 说明 |
|---|---|---|---|
| `AccentColor` | `0xFFBC9900` | `#0099BC` | **ABGR** 格式（低字节 = R） |
| `ColorizationColor` | `0xC40099BC` | `#0099BC` | **ARGB** 格式（低字节 = B） |
| `ColorizationAfterglow` | `0xC40099BC` | `#0099BC` | 同上 |
| `DwmGetColorizationColor()` | `0xE3008FAF` | `#008FAF` | 带 alpha，与注册表值不同（做了混合） |
| `Explorer\Accent\AccentColorMenu` | `0xFFBC9900` | `#0099BC` | ABGR |
| `AppsUseLightTheme` | 0 | — | **深色模式开启** |
| `SystemUsesLightTheme` | 0 | — | **深色模式开启** |

`AccentPalette` (32 bytes) =
`A9EFFF00 79E6FF00 48D2F200 0099BC00 006B8300 004B5C00 00323D00 E3008C00`
（每 4 字节为 `BBGGRR00`，第 4 组 `0099BC` 即主强调色，与上面一致）

**强调色的应用方式**：
- 微软**不给固定 hex**，只给策略：
  > "Accent color is used to emphasize important elements in the user interface and to indicate
  > the state of an interactive object or control. Accent color values are generated automatically
  > and optimized for contrast in both light and dark modes."
  > — https://learn.microsoft.com/en-us/windows/apps/design/style/color
- 正规 API（**不要直接读注册表**，那两个键微软从未文档化）：
  - `UISettings.GetColorValue(UIColorType.Accent)`（WinRT，有 `AccentDark1..3 / AccentLight1..3`）
  - `DwmGetColorizationColor()`（见上）
  > "Retrieves the current color used for Desktop Window Manager (DWM) glass composition.
  > This value is based on the current color scheme and can be modified by the user."
  > — https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmgetcolorizationcolor

**本项目最重要的一条结论**：

> **本机用户强调色是 `#0099BC`（青），但 aero.msstyles 里烘死的选中/悬停蓝是 `#0078D7` 家族，
> 两者没有任何关系。** `COLOR_HIGHLIGHT` = `#0078D7` 是存储值，不是活强调色。
> 实测控件颜色（`#CDE8FF`、`#CCE8FF`、`#E5F3FF`、`#D9EBF9`）全部来自 aero 位图，
> **不随用户强调色变化**。
>
> 所以：**要复刻"标准 Win10 任务管理器"，用 `#0078D7` 家族常量；
> 要跟随系统强调色，就得改成运行时取色**——这是两条不同的路，必须二选一。
> 微软官方对桌面应用的建议是按主题走（`GetSysColor` / 主题绘制），而不是自己算。

### 5.3 灰阶分工（全部为本机实测，落在具体部件上）

| 灰阶 | 实测出现位置 | 用途 |
|---|---|---|
| **`#CCCCCC`** | `BP_PUSHBUTTON` s4 DISABLED 填充 | **禁用按钮底** |
| **`#D9D9D9`** | `TABITEM` 边框（全状态）、`ListView` s5 选中失焦填充 | **页签/控件边框**、选中失焦 |
| **`#E5E5E5`** | `HEADERITEM` s1 右边线（1px，24/2880 像素） | **列头分隔线** |
| `#E1E1E1` | `BP_PUSHBUTTON` s1 NORMAL 填充 | 按钮底 |
| `#F0F0F0` | `TABITEM` 未选中、`TP_BUTTON`、`SP_PANE`、滚动条轨道 = `COLOR_BTNFACE` | **通用控件面** |
| `#DDDDDD` | **本机 aero 部件中未出现** | **无法确定**——`#DDDDDD` 不是 Win10 aero 的标准灰，常见说法属第三方 |
| `#ADADAD` | 按钮边框 NORMAL | 按钮描边 |
| `#A0A0A0` | `COLOR_BTNSHADOW` | 阴影/凹陷 |
| `#828790` | `LVP_LISTITEM` `TMT_BORDERCOLOR` | 列表外框（类级回退） |
| `#BFBFBF` | 按钮禁用边框、`SP_PANE` 边框、`TaskManager` p40 | 弱化边框 |
| `#707070` | `TaskManager` p17/p44 文字、`CHARTVIEW` 轴文字 | **次级/图表文字灰** |
| `#6D6D6D` | `COLOR_GRAYTEXT`、`TP_BUTTON` s4 文字 | 禁用文字 |

> 微软明确禁止硬编码颜色：
> "Always base your font colors and backgrounds off system theme colors.
> Never make your own colors based on fixed RGB values."
> — https://learn.microsoft.com/en-us/windows/win32/uxguide/vis-fonts

### 5.4 文字色规范

微软唯一给出 hex 的是 Vista/Win7 默认（标题 "defaults for Windows Vista and Windows 7"）：

| 用途 | 主题符号 | 规格 |
|---|---|---|
| 标题栏 | `CaptionFont` | 9pt `#000000` Segoe UI |
| 主说明 | `MainInstruction` | 12pt `#003399` |
| 正文 | `BodyText` | 9pt `#000000` |
| **禁用** | `Disabled` | 9pt `#323232` |
| 链接 | `HyperLinkText` | 9pt `#0066CC` |
| 链接悬停 | `Hot` | 9pt `#3399FF` |
| 分组头 | — | 11pt `#003399` |

— https://learn.microsoft.com/en-us/windows/win32/uxguide/vis-fonts

本机实测的系统级值：`COLOR_WINDOWTEXT`/`COLOR_BTNTEXT` = `#000000`，
`COLOR_GRAYTEXT` = `#6D6D6D`，`COLOR_HOTLIGHT` = `#0066CC`（与上表链接色一致）。

WinUI 令牌（官方源码，非文档，半权威）：`TextFillColorPrimary #E4000000`、
`TextFillColorSecondary #9E000000`、`TextFillColorTertiary #72000000`、
`TextFillColorDisabled #5C000000`（浅色主题）。

---

## 6. 图表配色规范

### 6.1 `TaskManager` 主题类 —— **本项目应该用的调色板**

**重大发现**：`aero.msstyles` 里存在一个名为 **`TaskManager`** 的主题类
（`OpenThemeData(NULL,"TaskManager")` 成功；同样的字符串也以 UTF-16 出现在
`taskmgr.exe` 偏移 `0xEF6F8` 的字符串块里，与 `TASKMANAGER`/`taskman`/`TabView`/`TaskManagerMain`/`TmViewData` 相邻）。
这就是**微软官方给任务管理器预置的配色**。

来源：**API 实测（`GetThemeColor`，该类的状态索引对绝大多数 part 被忽略）**

| part | 颜色 | 推断用途（**推断，非文档**） |
|---|---|---|
| 1 | `#E2E7FE` | 浅靛（图例/系列） |
| 2 | `#EAD5A0` | 浅琥珀 |
| 3 | `#FFF9E4` | 琥珀梯度 1（最浅） |
| 4 | `#FFF4C4` | 琥珀梯度 2 |
| 5 | `#F9ECA8` | 琥珀梯度 3 |
| 6 | `#FFE487` | 琥珀梯度 4 |
| 7 | `#FFD264` | 琥珀梯度 5 |
| 8 | `#FFC63D` | 琥珀梯度 6 |
| 9 | `#FCB816` | 琥珀梯度 7 |
| 10 | `#FCA71D` | 琥珀梯度 8 |
| 11 | `#FC682A` | 琥珀梯度 9（橙红） |
| 12 | `#FC682A` | 琥珀梯度 10（同 11） |
| 13 | `#DB6B3A`（text） | 橙红文字 |
| 14 | `#000000`（text） | 正文 |
| 15 | `#000000`（text） | 正文 |
| 16 | `#1F59C3`（text） | **链接蓝文字** |
| 17 | `#707070`（text） | **次级/轴文字灰** |
| 18 | `#000000`（text） | 正文 |
| 19 | `#000000`（text） | 正文 |
| 20 | `#FFFFFF` | 白底 |
| 21 | `#F4F2F4` | 极浅灰面 |
| 22 | `#EEE6F1` | 浅紫 1 |
| 23 | `#CEB0D7` | 浅紫 2 |
| 24 | `#4C607A`（text） | 蓝灰文字 |
| 25 | `#000000`（text） | 正文 |
| 26 | `#FFFFFF` | 白底 |
| 27 | `#E0F0FF` | **蓝梯度 1（最浅）** |
| 28 | `#CDE9FF` | 蓝梯度 2 |
| 29 | `#B4DEFF` | 蓝梯度 3 |
| 30 | `#7EC6FF` | 蓝梯度 4 |
| 31 | `#43A9E4` | 蓝梯度 5 |
| 32 | `#3782AD` | 蓝梯度 6 |
| 33 | `#2477A7` | 蓝梯度 7 |
| 34 | `#1F71A1` | 蓝梯度 8 |
| 35 | `#186A99` | 蓝梯度 9 |
| 36 | `#14587F` | 蓝梯度 10 |
| 37 | `#0E405D` | 蓝梯度 11（最深） |
| 38 | **`#117DBB`** | **图表主系列蓝** — 与 `CHARTVIEW` 系列蓝**完全相同** |
| 39 | `#DFE7EB` | 浅蓝灰面 |
| 40 | `#BFCFDF` | 中蓝灰面 |
| 41 | `#BABABA` | 网格线灰 |
| 42 | `#E8E8E8` | 浅网格线 |
| 44 | `#707070`（text） | 次级文字 |
| 45 | `#DAE2E3` | 浅面 |
| 46 | `#EDE4CC` | 浅米面 |

**交叉验证**：`TaskManager` p38 = `#117DBB` 与 `CHARTVIEW` 系列蓝 `#117DBB` 一字不差；
p17/p44 = `#707070` 与 `CHARTVIEW` 轴文字灰一字不差。→ **两个类同源，调色板一致。**

**蓝梯度（27→37，11 级）与琥珀梯度（3→12，10 级）是热力/渐变用的成熟色阶**，
可直接用于 CPU 核心热力图或图表填充。

> 注意：part → 语义的映射是**根据数值形态推断的**，微软**没有任何文档**说明 `TaskManager`
> 类的 part 含义。落地前需按视觉效果核对。

### 6.2 `CHARTVIEW` 主题类

来源：**子 Agent 实测（`GetThemeColor` + 二进制 PVL 记录解析，两者互相印证）**

- 类名在 `aero.msstyles` 的 CMAP 中以混合大小写 **`ChartView`** 存储（索引 **323**），
  `OpenThemeData` **大小写不敏感**，`"CHARTVIEW"` 可正常打开。
- 共 **42 个 part**（43+ 返回 `E_PROP_ID_UNSUPPORTED`）；只定义了 3 个属性：
  `TMT_ALPHALEVEL(2402)`、`TMT_FILLCOLOR(3802)`、`TMT_TEXTCOLOR(3803)`。
- **part 1–28 = 数据系列调色板**，以 **7 为周期重复 4 次**（4 组基础色相 × 7 槽位）：

| 槽位 | 组1(base1) | 组2(base8) | 组3(base15) | 组4(base22) | alpha |
|---|---|---|---|---|---|
| +0 | `#FFFFFF` | `#FFFFFF` | `#FFFFFF` | `#FFFFFF` | 225 |
| +1 | **`#117DBB`** 蓝 | **`#8B12AE`** 紫 | **`#4DA60C`** 绿 | **`#A74F01`** 橙 | 225 |
| +2 | `#D9EAF4` | `#ECDEF0` | `#DBEDCE` | `#EEDECF` | 255 |
| +3 | 同 +1 | 同 +1 | 同 +1 | 同 +1 | 225 |
| +4 | `#3386B5` | `#817285` | `#4DA60C` | `#E0822F` | 18~25 |
| +5 | `#81B3D2` | `#9528B4` | `#79D50D` | `#DF672C` | 255 |
| +6 | `#3386B5` | `#817285` | `#4DA60C` | `#E0822F` | 23~33 |

→ **4 个基础色相**（蓝/紫/绿/橙），经明暗变化展开成 **16 个不同 RGB**。
槽位 +0 恒为白色 = 绘图区背景，不是系列色。

- **part 29–34 = 阈值/告警色**，**只有这部分 state 有意义**（state 1/2/3，0 与 4+ 失败）：

| part | s1 绿 | s2 黄 | s3 红 | alpha |
|---|---|---|---|---|
| 29, 31, 32 | **`#06B025`** | **`#B09800`** | **`#C73F36`** | 255 |
| 30 | `#06B025` | `#B09800` | `#C73F36` | 51（变暗） |
| 33, 34 | `#4ECB22` | `#E8DB51` | `#C73F36` | 127/153（半透明） |

- **part 35–42 = 非图形强调色**：35/37/39 = `#A4DE9A`；**36/38/40/42 = `#707070`（TEXTCOLOR，轴/标签文字）**；41 = `#D2A883`。
- **重要**：`TMT_BORDERCOLOR`/`TMT_BORDERSIZE` 对 CHARTVIEW **全部失败** ——
  **该类没有任何边框，`DrawThemeBackground` 对所有 part 都画纯黑**。
  它是一个**纯色板**，几何必须自己画。
- 继承的全局回退：`EDGELIGHTCOLOR #B4B4B4`、`EDGEHIGHLIGHTCOLOR #F4F7FC`、
  `EDGESHADOWCOLOR #B4B4B4`、`EDGEDARKCOLOR #646464`、`EDGEFILLCOLOR #DCDCDC`、`GLOWCOLOR #FFFFFF`。

### 6.3 `CHARTV.dll`

- `C:\Windows\System32\CHARTV.dll`，131,072 字节，FileVersion **10.0.19041.1**，描述 "Chart View"
- 导出 8 个函数（全 `Cv*`，数据源抽象，**不是渲染器**）：
  `CvCloseDataSource` `CvCreateDataSource` `CvGetData` `CvGetDataSourceName`
  `CvInitialize` `CvSetData` `CvSetDataSourceName` `CvUninitialize`
- DLL 内**不含** "CHARTVIEW" 字符串（两种编码均 0 命中）→ 它自己不调 `OpenThemeData`。
- `taskmgr.exe` 里 `"CHARTV.dll"` 是 ASCII 出现在一组**延迟加载 DLL 名**中
  （与 `dbghelp.dll`/`PROPSYS.dll`/`WINSTA.dll` 相邻）→ **`LoadLibrary` 动态加载**，
  所以静态导入表里没有它。`taskmgr` 静态导入含 `UxTheme.dll` 和 `pdh.dll`。

### 6.4 标准图表可用的颜色来源（结论）

1. **`CHARTVIEW` / `ChartView` 主题类** —— 4 色系列调色板 + 3 色阈值（绿/黄/红），
   纯色、无几何、无边框。
2. **`TaskManager` 主题类** —— 官方任务管理器专属，含 11 级蓝梯度、10 级琥珀梯度、
   浅紫/蓝灰面、网格灰 `#BABABA`/`#E8E8E8`、轴文字 `#707070`。
3. **`GetSysColor(COLOR_HIGHLIGHT)`** = `#0078D7` 作单系列兜底。

**推荐**：主系列蓝用 `#117DBB`（TM + CHARTVIEW 双重确认），
阈值绿/黄/红用 `#06B025`/`#B09800`/`#C73F36`，网格 `#E8E8E8`，轴文字 `#707070`。

---

## 7. DPI 与 rp 单位

### 7.1 `rp` 的官方定义

**"rp" 作为缩写不存在于任何微软文档中。** 全 MicrosoftDocs 检索 `"relative pixel"` 仅 7 个文件，
`"rp (relative pixels)"` / `"relative pixels (rp)"` **0 结果**。

官方术语是拼写全称 **relative pixels**：

> "**relative pixels** — A device-independent metric that is the same as a physical pixel at 96 dpi
> (dots per inch), but proportionately scaled in other dpis. See also: effective resolution."
> — https://learn.microsoft.com/en-us/windows/win32/uxguide/glossary

| DPI | 缩放 | 1 rp = 多少设备像素 | 依据 |
|---|---|---|---|
| **96** | 100% | **1** | 文档原文 |
| **120** | 125% | **1.25** | 由定义推导 |
| **144** | 150% | **1.5** | 由定义推导 |

（120/144 是 "proportionately scaled" 的算术结果；DIP 的等价表在
https://learn.microsoft.com/en-us/windows/win32/learnwin32/dpi-and-device-independent-pixels
中明确列出 "96 → 1 pixel; 120 → 1.25 pixels; 144 → 1.5 pixels"）

**rp 是文档/设计层单位，没有任何 Win32 API 接受它。** 三种单位必须分清：

| 单位 | 归属 | 官方定义 |
|---|---|---|
| **relative pixel (rp)** | Win32 UX 指南（**仅设计文档**） | 96dpi 下等于物理像素，其它 dpi 等比缩放 |
| **device-independent pixel (DIP)** | Win32 / Direct2D | "A DIP is defined as 1/96th of a *logical* inch." |
| **effective pixel (epx/ep/px)** | UWP / XAML | "a virtual unit of measurement … independent of screen density" |

### 7.2 关于 4px 网格 —— 重要纠正

**4px 网格是 UWP/XAML（epx）的规定，Win32 桌面指南里没有这条。**

> "The sizes, margins, and positions of UI elements should always be in multiples of 4 epx in your XAML apps."
> "The base unit is 4 because it scales to these plateaus as a whole number (for example, 4 × 125% = 5, 4 × 150% = 6)."
> — https://learn.microsoft.com/en-us/windows/apps/design/layout/screen-sizes-and-breakpoints-for-responsive-design

Win32 UX 指南的间距集**恰好不是 4 的倍数**（9pt Segoe UI @96dpi，官方表格）：

| 元素 | DLU | relative px |
|---|---|---|
| 命令按钮 | 50 x 14 | **75 x 23** |
| 复选框 / 单选 | 10 | **17** |
| 静态文字 | 8 | **13** |
| 滑块 | 15 | **24** |
| 对话框边距 | 7 (四边) | **11** |
| 标签↔控件 | 3 | **5** |
| 相关控件间 | 4 | **7** |
| 不相关控件间 | 7 | **11** |
| 分组框首个控件下移 | 11 | **16** |
| 从属控件缩进 | 12 | **18** |

— https://learn.microsoft.com/en-us/windows/win32/uxguide/vis-layout

触摸目标：常用控件最小 **23x23 px (13x13 DLU)**，最常用控件至少 **40x40 px**，
间距至少 **5 px (3 DLU)** — https://learn.microsoft.com/en-us/windows/win32/uxguide/inter-touch

> **结论**：Win32 若要用 4px 网格，那是**设计选择，不是微软规范**。
> 官方 Win32 间距集是 5/7/11/13/16/17/18/23 rp。

### 7.3 桌面应用 DPI 推荐做法 —— Per-Monitor V2

> "It is recommended that desktop applications use Per-Monitor v2 awareness mode,
> supported on Windows 10 1703 or above."
> "Per-Monitor V1 (PMv1) awareness is very limited. It is recommended that applications use PMv2."
> — https://learn.microsoft.com/en-us/windows/win32/hidpi/high-dpi-desktop-application-development-on-windows

最低 OS：**Windows 10 1703 (Creators Update)**
— https://learn.microsoft.com/en-us/windows/win32/hidpi/dpi-awareness-context

官方 manifest：

```xml
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0"
          xmlns:asmv3="urn:schemas-microsoft-com:asm.v3">
  <asmv3:application>
    <asmv3:windowsSettings>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true</dpiAware>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2</dpiAwareness>
    </asmv3:windowsSettings>
  </asmv3:application>
</assembly>
```
— https://learn.microsoft.com/en-us/windows/win32/hidpi/setting-the-default-dpi-awareness-for-a-process

（`<dpiAwareness>` 需 Win10 1607+；1607 起若两者同时存在，`<dpiAware>` 被忽略。）

缩放写法（官方示例）：
```c
#define INITIALX_96DPI 50
#define USER_DEFAULT_SCREEN_DPI 96
int iDpi = GetDpiForWindow(hWnd);
int dpiScaledX = MulDiv(INITIALX_96DPI, iDpi, USER_DEFAULT_SCREEN_DPI);
```
- `GetDpiForWindow`：Win10 1607+，返回窗口 DPI
  — https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getdpiforwindow
- `MulDiv`：乘后除再四舍五入 — https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-muldiv
- 必须处理 `WM_DPICHANGED` 重新布局（wParam 低字 X DPI、高字 Y DPI，如 96/120/144/192）
  — https://learn.microsoft.com/en-us/windows/win32/hidpi/wm-dpichanged
- **不要**用 `GetDpiForMonitor`（PMv2 下不适用）

### 7.4 本机 DPI 缩放实测（用于验证 `MulDiv` 假设）

| 对象 | 96 | 120 | 144 | 168 | 192 |
|---|---|---|---|---|---|
| `SM_CXVSCROLL` / `SM_CYHSCROLL` | 17 | 21 | 26 | 30 | 34 |
| `SM_CYCAPTION` | 23 | 29 | 34 | 40 | 45 |
| `SM_CXSMICON` / `SM_CYSMICON` | 16 | 20 | 24 | 28 | 32 |
| `SM_CXSMSIZE` / `SM_CYSMSIZE` | 22 | 28 | 33 | 39 | 44 |
| `SM_CXEDGE` / `SM_CYEDGE` | 2 | 2 | 2 | 2 | 2 |
| `SM_CYBORDER` | 1 | 1 | 1 | 1 | 1 |

- `SM_CXVSCROLL` 完全符合 `MulDiv(17, dpi, 96)`：120→21.25→21，144→25.5→**26**，
  168→29.75→**30**，192→34。**验证通过。**
- **`SM_CXEDGE` / `SM_CYBORDER` 不缩放**（恒为 2 / 1）—— 细边框不要乘 DPI。

**主题 part 尺寸不随 DPI 变化**：用 `OpenThemeDataForDpi(cls, 96/120/144/168/192)` 测
`LVP_LISTITEM` / `HEADERITEM` / `TABITEM` / `TP_BUTTON` / `BP_PUSHBUTTON` / `SBP_ARROWBTN`，
**五档返回值完全相同**（`3x3` / `6x24` / `5x19` / `7x18` / `13x11` / `17x17`）。
原因：`aero.msstyles` 只有一套 NormalSize 资源。
→ **结论：不能指望主题自动 DPI 缩放，必须自己 `MulDiv`。**

---

## 8. 可直接抄进代码的常量表

下列为 **Win10 22H2 / aero.msstyles / 96 DPI** 基准值。
C++ 命名，`kXxx` 前缀。所有像素值在运行时须 `MulDiv(v, dpi, 96)`。

### 8.1 几何

| 常量名 | 建议值 | 来源 | 置信 |
|---|---|---|---|
| `kUiFontHeightPt` | `9` | 注册表 `WindowMetrics` + 微软文档 | 高 |
| `kUiFontFace` | `"Segoe UI"`（中文系统 `"Microsoft YaHei UI"`） | 注册表实测 | 高 |
| `kUiTextEmHeight` | `15`（Segoe UI 9pt tmHeight）/ `17`（YaHei） | GDI 实测 | 高 |
| `kHeaderHeight` | **`24`** | 主题 `HEADERITEM/TS_TRUE` | **高** |
| `kHeaderPadLeft` | `2` | 主题 `SIZINGMARGINS.L` | 高 |
| `kHeaderPadRight` | `3` | 主题 `SIZINGMARGINS.R` | 高 |
| `kHeaderDividerColor` | `#E5E5E5`（**最右 1px**，实测 x=w-1 单列） | 渲染实测 | 高 |
| `kHeaderSortArrowW/H` | `13 / 5` | 主题 `HEADERSORTARROW` | 高 |
| `kTabBorderW/H` | `5 / 19` | 主题 `TABITEM/TS_TRUE` | 高 |
| `kTabPadX` | `2` | 主题 `SIZINGMARGINS.L/R` | 高 |
| `kTabPadTop` | `13` | 主题 `SIZINGMARGINS.T` | 高 |
| `kTabPadBottom` | `4` | 主题 `SIZINGMARGINS.B` | 高 |
| `kTabHeight` | `tmHeight + 17`（9pt→`32`） | 推导 | **中** |
| `kButtonHeight` | **`23`** | 微软 UX 指南 75x23 | **高** |
| `kButtonMinWidth` | `75` | 微软 UX 指南 | 高 |
| `kButtonBorderW/H` | `13 / 11` | 主题 `BP_PUSHBUTTON/TS_TRUE` | 高 |
| `kButtonContentPad` | `3`（四边） | 主题 `CONTENTMARGINS` | 高 |
| `kButtonSizingPad` | `6`（左右）/ `5`（上下） | 主题 `SIZINGMARGINS` | 高 |
| `kCheckBoxSize` | `13` | 主题 `BP_CHECKBOX/TS_TRUE` | 高 |
| `kToolbarBarHeight` | **`28`** | 微软 UX 指南 | 高 |
| `kToolbarIconSize` | **`16`** | 微软 UX 指南 | 高 |
| `kToolbarButtonBorderW/H` | `7 / 18` | 主题 `TP_BUTTON/TS_TRUE` | 高 |
| `kToolbarButtonPad` | `3`（左右）/ `13`（上）/ `4`（下） | 主题 `SIZINGMARGINS` | 高 |
| `kScrollbarWidth` | **`17`** | `SM_CXVSCROLL` + 主题，三源一致 | **高** |
| `kScrollbarArrowSize` | `17` | 主题 `SBP_ARROWBTN` | 高 |
| `kScrollbarThumbMinLen` | **`11`** | 主题 `SBP_THUMBBTNVERT/TS_TRUE` | 高 |
| `kScrollbarSizeBox` | `10x9` | 主题 `SBP_SIZEBOX` | 高 |
| `kListRowHeight` | `tmHeight + 2*2`（9pt→`19`） | **推导，官方未规定** | **低** |
| `kListExpandGlyphSize` | `15` | 主题 `LVP_EXPANDBUTTON` | 高 |
| `kEdgeWidth` | `2`（**不缩放**） | `SM_CXEDGE` 实测 | 高 |
| `kBorderWidth` | `1`（**不缩放**） | `SM_CYBORDER` 实测 | 高 |
| `kDialogMargin` | `11` | 微软 UX 指南 | 高 |
| `kSpacingRelated` | `7` | 微软 UX 指南 | 高 |
| `kSpacingUnrelated` | `11` | 微软 UX 指南 | 高 |
| `kSpacingLabelToControl` | `5` | 微软 UX 指南 | 高 |

### 8.2 列表颜色

| 常量名 | 值 | 来源 |
|---|---|---|
| `kListBg` | `#FFFFFF` | `Explorer::ListView` s1 渲染 |
| **`kOverlayTint`** | **`#0089FF`** | 反解源色（列表 + 工具条共用） |
| **`kOverlayHoverAlpha`** | **`0.10`**（10%） | 反解 |
| **`kOverlaySelectedAlpha`** | **`0.20`**（20%） | 反解 |
| **`kOverlayNotFocusAlpha`** | **`0.15`** @ `#000000` | 反解 |
| `kListRowHot` | `#E5F3FF` | `Explorer::ListView` s2 渲染（= 上述叠加在白底的结果） |
| `kListRowSelected` | `#CDE8FF` | `Explorer::ListView` s3 渲染（= 上述叠加在白底的结果） |
| `kListRowSelectedNotFocus` | `#D9D9D9` | `Explorer::ListView` s5 渲染 |
| `kListRowHotSelected` | `#CCE8FF` | `Explorer::ListView` s6 渲染 |
| `kListRowText` | `#000000` | `COLOR_WINDOWTEXT` |
| `kListBorder` | `#828790` | 主题 `TMT_BORDERCOLOR`（回退值） |

> 实施建议：优先实现**叠加**（`kOverlayTint` + alpha），把 `kListRowHot` 等纯色仅作白底快捷值。

### 8.3 页签 / 按钮 / 工具条 / 滚动条颜色

| 常量名 | 值 | 来源 |
|---|---|---|
| `kTabBgNormal` | **`#F0F0F0`** | `TABITEM` s1 渲染 |
| `kTabBgHot` | `#D8EAF9` | `TABITEM` s2 渲染 |
| `kTabBgSelected` | **`#FFFFFF`** | `TABITEM` s3 渲染 |
| `kTabBorder` | **`#D9D9D9`** | `TABITEM` 全状态渲染 |
| `kTabBodyBg` | `#FFFFFF` | `TABBODY` 渲染 |
| `kTabText` | `#000000` | 主题未定义 → `COLOR_BTNTEXT` |
| `kButtonBg` | `#E1E1E1` | `BP_PUSHBUTTON` s1 渲染 |
| `kButtonBorder` | `#ADADAD` | `BP_PUSHBUTTON` s1 渲染 |
| `kButtonBgHot` | `#E5F1FB` | s2 渲染 |
| `kButtonBorderHot` | `#0078D7` | s2 渲染 |
| `kButtonBgPressed` | `#CCE4F7` | s3 渲染 |
| `kButtonBorderPressed` | `#005499` | s3 渲染 |
| `kButtonBgDisabled` | `#CCCCCC` | s4 渲染 |
| `kButtonBorderDisabled` | `#BFBFBF` | s4 渲染 |
| `kButtonText` | `#000000` | `TMT_TEXTCOLOR` |
| `kButtonTextDisabled` | `#838383` | `TMT_TEXTCOLOR` s4 |
| `kToolbarBg` | `#F0F0F0` | `TMT_FILLCOLOR` |
| `kToolbarBtnHot` | `#E5F3FF` | s2 渲染 |
| `kToolbarBtnHotBorder` | `#CCE8FF` | s2 渲染 |
| `kToolbarBtnPressed` | `#CCE8FF` | s3 渲染 |
| `kToolbarBtnPressedBorder` | `#99D1FF` | s3 渲染 |
| `kScrollbarTrack` | `#F0F0F0` | 渲染 |
| `kScrollbarThumb` | `#CDCDCD` | s1 渲染 |
| `kScrollbarThumbHot` | `#A6A6A6` | s2 渲染 |
| `kScrollbarThumbPressed` | `#606060` | s3 渲染 |
| `kScrollbarArrowColor` | `#606060` | s1 渲染 |

### 8.4 全局颜色

| 常量名 | 值 | 来源 |
|---|---|---|
| `kWindowBg` | `#FFFFFF` | `COLOR_WINDOW` |
| `kWindowText` | `#000000` | `COLOR_WINDOWTEXT` |
| `kControlFace` | `#F0F0F0` | `COLOR_BTNFACE` |
| `kControlText` | `#000000` | `COLOR_BTNTEXT` |
| `kHighlight` | **`#0078D7`** | `COLOR_HIGHLIGHT` |
| `kHighlightText` | `#FFFFFF` | `COLOR_HIGHLIGHTTEXT` |
| `kGrayText` | `#6D6D6D` | `COLOR_GRAYTEXT` |
| `kDisabledText` | `#323232` | 微软 UX 指南 Disabled |
| `kHotlight` | `#0066CC` | `COLOR_HOTLIGHT` / UX 指南链接色 |
| `kDividerHeader` | `#E5E5E5` | 渲染实测 |
| `kDividerControl` | `#D9D9D9` | 渲染实测 |
| `kDisabledFill` | `#CCCCCC` | 渲染实测 |
| `kSecondaryText` | `#707070` | `TaskManager` p17 / `CHARTVIEW` |
| `kBorderStrong` | `#ADADAD` | 按钮边框 |
| `kBorderWeak` | `#BFBFBF` | 禁用边框 |
| `kMenuItemHot` | `#3399FF` | `COLOR_MENUHILIGHT` |

### 8.5 图表颜色

| 常量名 | 值 | 来源 |
|---|---|---|
| `kChartSeries1` | **`#117DBB`** | `CHARTVIEW` + `TaskManager` p38 双源一致 |
| `kChartSeries2` | `#8B12AE` | `CHARTVIEW` 紫 |
| `kChartSeries3` | `#4DA60C` | `CHARTVIEW` 绿 |
| `kChartSeries4` | `#A74F01` | `CHARTVIEW` 橙 |
| `kChartGrid` | `#E8E8E8` | `TaskManager` p42 |
| `kChartGridStrong` | `#BABABA` | `TaskManager` p41 |
| `kChartAxisText` | `#707070` | `CHARTVIEW` / `TaskManager` p17 |
| `kChartThresholdOk` | **`#06B025`** | `CHARTVIEW` p29-32 s1 |
| `kChartThresholdWarn` | **`#B09800`** | `CHARTVIEW` p29-32 s2 |
| `kChartThresholdCrit` | **`#C73F36`** | `CHARTVIEW` p29-32 s3 |
| `kChartBlockBg` | `#FFFFFF` | `TaskManager` p20/p26 |
| `kChartLinkText` | `#1F59C3` | `TaskManager` p16 |

---

## 9. 无法确定项汇总（明确列出，不要用"一般认为"糊过去）

| 项 | 状态 | 原因 |
|---|---|---|
| 列表**行高** | **无法确定** | 微软未规定；主题 `LVP_LISTITEM` 无 sizing margins，`TS_TRUE` 仅 3x3；由 comctl32 按字体运行时计算 |
| 页签**总高** | 推导值 `32` | `GetThemePartSize` 只给边框图元 `5x19`；总高须由 `tmHeight+13+4` 合成 |
| `#DDDDDD` 的用途 | **无法确定** | 本机 aero 全部部件中未出现该色 |
| 列表选中**文字色** | **无法确定** | 主题 `TMT_TEXTCOLOR` 对所有 state 返回 `na`；由应用自绘决定 |
| Explorer 用的 `LVS_EX_*` 组合 | **无法确定** | 微软未文档化 Explorer 的扩展样式 |
| `TaskManager` 类 part→语义映射 | **推断** | 无任何文档；按数值形态推断，需视觉核对 |
| `CHARTVIEW` 类 part→语义映射 | **推断** | 同上；子 Agent 已明确标注为推断 |
| `HKCU\...\DWM\AccentColor` 键 | **未文档化** | 微软从未文档化；应改用 `UISettings.GetColorValue` / `DwmGetColorizationColor` |
| `rp` 作为缩写 | **不存在** | 全 MicrosoftDocs 0 命中；只有全称 "relative pixels" |
| Win10 页签"改成下划线" | **官方未规定** | 无任何微软页面陈述此变更；实测仍为带边框方块 |
| 列头文字对齐 | **官方未规定** | 由 `HDITEM.fmt`/`HDF_LEFT` 决定，非主题 |
| 标准缩放档位全集 | **文档互相矛盾** | 硬件指南列 100/125/150/200/250/300/400/500（**无 175%**）；XAML 指南列 100/125/150/**175**/200/225/250/300/350/400 |

---

## 10. 给本项目的最小结论

1. **列头高 24px、滚动条宽 17px、按钮高 23px、工具条条高 28px** —— 有官方/实测硬依据，可直接用。
2. **列表行高没有规范值**，必须自己定（`tmHeight + 2×pad`），并做成可调常量。
3. **页签是带边框方块，不是下划线**。`#F0F0F0` / 选中 `#FFFFFF` / 边框 `#D9D9D9`。
4. **取色必须用 `Explorer::ListView` 子类，不是 `ListView`**；通用类渲染全是白色。
5. **`GetThemeColor` 对位图状态返回的是回退常量，不可信**；真实色只能渲染采样。
6. **选中/悬停是半透明叠加，不是纯色**：`#0089FF` @ 20%（选中）/ 10%（悬停），
   失焦 `#000000` @ 15%。列表和工具条**共用同一套**。应实现为 alpha 混合。
7. **选中/悬停蓝是 aero 烘死的 `#0089FF` 家族，不跟随用户强调色**（本机强调色是青色 `#0099BC`，
   但控件叠加色仍是 `#0089FF`）。要复刻"标准 Win10"就用常量，不要读强调色。
8. **图表直接用 `#117DBB`**（`CHARTVIEW` 与 `TaskManager` 双源一致），
   阈值 `#06B025`/`#B09800`/`#C73F36`。
9. **主题尺寸不随 DPI 缩放**，必须 `MulDiv(v, dpi, 96)`；`SM_CXEDGE`/`SM_CYBORDER` 除外（不缩放）。

---

状态：已完成
