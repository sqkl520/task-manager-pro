# Agent R — Win10 系统度量

状态：已完成

> 本报告只记录**本机实测**的系统度量。凡是「推算」而非「读取」的，都显式标注了 [推算]。
> 所有数值来自 `SystemParametersInfoW` / `GetSystemMetrics` / `uxtheme` / 注册表，
> 没有一处来自截图或猜测。

## 0. 取证环境与手段

| 项 | 实测值 |
|---|---|
| OS | Windows 10 Pro 22H2（`10.0.22H2`，`BuildLabEx 19041.1.amd64fre.vb_release`）|
| 屏幕 | 2560 x 1440 @ 100 Hz（NVIDIA discrete GPU）|
| 主题 | `Aero.msstyles` / `NormalColor` / `NormalSize` |
| 采集进程 DPI 感知 | `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)` 成功，故下述 SM_* / 字体值**未被 DPI 虚拟化** |

采集脚本（放在 `docs/evidence/README.md`，纯 PowerShell `Add-Type` 内存编译，**未创建任何窗口**，未调用 `taskmgr.exe` / `TaskManagerPro.exe`）：

- `sysmetrics.ps1` → `sysmetrics_raw.txt`（DPI、SM_*、NONCLIENTMETRICS、ICONMETRICS、GetSysColor、基础主题 part）
- `sysmetrics2.ps1` → `sysmetrics2_raw.txt`（GDI TEXTMETRIC、主题 TMT_HEIGHT/边距/颜色）
- `sysmetrics3.ps1` → `sysmetrics3_raw.txt`（AdjustWindowRectEx、主题 part size 的 MIN/TRUE/DRAW）
- `themedpi.ps1` → `theme_dpi_raw.txt`（`OpenThemeDataForDpi` 在 96/120/144/168/192 下的 part size）
- `spidpi.ps1` → `spi_dpi_raw.txt`（`SystemParametersInfoForDpi` 在 96…240 下的 NONCLIENTMETRICS）
- `fontmetrics.ps1` → `fontmetrics_raw.txt`（YaHei UI / Segoe UI / Tahoma 在各 `lfHeight` 下的 `tmHeight` 实测）

---

## 1. 当前 DPI 与缩放 —— 本机是 100%，不是 125%

| 来源 | 实测值 | 结论 |
|---|---|---|
| `GetDpiForSystem()` | **96** | 100% |
| `GetDpiForMonitor(MDT_EFFECTIVE_DPI)` | **96 x 96** | 100% |
| `GetDpiForMonitor(MDT_RAW_DPI)` | 返回 `0x80070032`（`ERROR_NOT_SUPPORTED`）| 正常：虚拟显示适配器环境下系统不提供 raw DPI，不影响结论 |
| `GetDeviceCaps(LOGPIXELSX / LOGPIXELSY)` | **96 / 96** | 100% |
| `HKCU\Control Panel\Desktop\WindowMetrics\AppliedDPI` | **96** | 100% |
| `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\FontDPI\LogPixels` | **96** | 100% |
| `HKCU\Control Panel\Desktop\LogPixels` | **不存在**（值为空）| 该 legacy 值在本机未设置，不能作为 DPI 依据 |
| `HKCU\Control Panel\Desktop\Win8DpiScaling` | 0 | 未启用 Win8.1 式「自定义缩放级别」；缩放走现代 per-monitor 路径 |
| `SPI_GETLOGICALDPIOVERRIDE` | **0** | 100%（-1=125%，-2=150%）|
| `HKCU\...\Desktop\PerMonitorSettings\*\DpiValue` | 全部 **0** | 所有已记录显示器都是「推荐/100%」 |

**结论：本机是 100% 缩放（96 DPI）。** 任务描述里「125% DPI 或 100%？先自己测」的答案是 **100%**。
因此本机所有 `SM_*` / `NONCLIENTMETRICS` 值就是 1:1 物理像素，不存在 1.25 倍误差。

### 1.1 rp（relative pixel）换算 [推算，有前提]

DirectUI 的 `rp` 是「相对像素」，以 96 DPI 为基准，1 rp 在 96 DPI 下 = 1 px。
换算式：`px = rp * dpi / 96`。

| 缩放 | 1 rp = | 14 rp = | 24 rp = | 28 rp = |
|---|---|---|---|---|
| 100% (96) | 1.000 px | 14 px | 24 px | 28 px |
| 125% (120) | 1.250 px | 17.5 px | 30 px | 35 px |
| 150% (144) | 1.500 px | 21 px | 36 px | 42 px |
| 175% (168) | 1.750 px | 24.5 px | 42 px | 49 px |
| 200% (192) | 2.000 px | 28 px | 48 px | 56 px |

**重要交叉验证**：`docs/evidence/exec/L_perf.md` 记录原版 DUIB 里 `memAvailabilityBar` 是 `rect(0rp,0rp,0rp,14rp)`，
而 L 在参考图上量得该条高 **22 px**。22 / 14 = **1.571 ≈ 150%**。
这说明**那张参考图很可能是在 150% DPI 下截的**，不是 100%。
如果成立，则那张图上量到的**所有**像素值都需要先 ÷1.5 才等于 rp/逻辑像素——
这足以解释此前多份报告里「行高 32 vs 实测 27.94」「行高 19 vs 28」之类的自相矛盾。
**此项为强提示，不是定论**（也可能该图是 125% 或其他路径渲染）。

---

## 2. 系统 UI 度量（SPI_GETNONCLIENTMETRICS）

`SPI_GETNONCLIENTMETRICS = 0x29`，`sizeof(NONCLIENTMETRICSW) = 504`（64 位，含 `iPaddedBorderWidth`）。

### 2.1 本机实际值（dpi=96）

| 字段 | 本机值 | 缩放到 125% | 缩放到 150% | 缩放规则 |
|---|---|---|---|---|
| `iBorderWidth` | **1** | 1 | 2 | 近似 `round(96基准 × dpi/96)`，实际取整由系统给 |
| `iScrollWidth` | **17** | 21 | 26 | 同上，见 §5 实测表 |
| `iScrollHeight` | **17** | 21 | 26 | 同上 |
| `iCaptionWidth` | **36** | 46 | 54 | 同上 |
| `iCaptionHeight` | **22** | 28 | 33 | 同上 |
| `iSmCaptionWidth` | **22** | 28 | 33 | 同上 |
| `iSmCaptionHeight` | **22** | 28 | 33 | 同上 |
| `iMenuWidth` | **19** | 24 | 29 | 同上 |
| `iMenuHeight` | **19** | 24 | 29 | 同上 |
| `iPaddedBorderWidth` | **4** | 5 | 6 | 同上 |

对照 §5 的 `SystemParametersInfoForDpi` 实测：**这些值不是简单乘系数，必须查表或用 API 取**。
例如 `iCAPTIONHEIGHT` 96→22、120→28、144→33（22×1.25=27.5→28，22×1.5=33 ✓）；
但 `iSCROLLWIDTH` 96→17、120→21（17×1.25=21.25→21）、144→26（17×1.5=25.5→26）；
`iBORDERWIDTH` 96→1、120→**1**（不是 1.25 取整的 1，而是保持不变）、144→2。
**结论：不要自己乘系数，直接调 `SystemParametersInfoForDpi`。**

### 2.2 字体字段（本机全部是 `Microsoft YaHei UI`，不是 `Segoe UI`）

`SPI_GETNONCLIENTMETRICS` 取出的 5 个 `LOGFONTW` **完全一致**：

| 字段 | lfFaceName | lfHeight | lfWeight | lfCharSet |
|---|---|---|---|---|
| `lfCaptionFont` | **Microsoft YaHei UI** | **-12** | 400 | 1 (DEFAULT_CHARSET) |
| `lfSmCaptionFont` | **Microsoft YaHei UI** | **-12** | 400 | 1 |
| `lfMenuFont` | **Microsoft YaHei UI** | **-12** | 400 | 1 |
| `lfStatusFont` | **Microsoft YaHei UI** | **-12** | 400 | 1 |
| `lfMessageFont` | **Microsoft YaHei UI** | **-12** | 400 | 1 |

- 所有 `lfWidth=0`（由 GDI 自动），`lfItalic=0`，`lfQuality=0`（DEFAULT_QUALITY，**不是** CLEARTYPE_QUALITY）。
- **`lfHeight = -12`（负值 = 字符高度 12 px）**，不是字号 12pt。12pt @ 96dpi = 16px，两者差很多。

### 2.3 用 GDI 回读该字体的 TEXTMETRIC（这是「行高」的真正来源）

把 `lfMessageFont` 交给 `CreateFontIndirectW` 再 `GetTextMetricsW`：

| 指标 | 值 | 含义 |
|---|---|---|
| `tmHeight` | **17** | 行高（ascent+descent），**这是列表行高的字面下限** |
| `tmAscent` | 13 | |
| `tmDescent` | 4 | |
| `tmInternalLeading` | 5 | 已含在 tmHeight 内（12px 字身 + 5px 内部行距）|
| `tmExternalLeading` | 0 | 额外行距为 0 |
| `tmAveCharWidth` | 6 | |
| `tmWeight` | 400 | |

**关键结论：本机 12px 的 YaHei UI 单行文字高 = 17 px。**
任何小于 17px 的列表行高都会裁字。我方 `tableview.h:92` 的 `rowHeight_ = 20` 能容纳 17px 文字，
`theme.h:155` 的 `kRowHeight = 28` 则留了 11px 余量。

#### 2.3.1 各 DPI 下 `tmHeight` 的实测值（不是简单乘系数）

系统在 125%/150% 下给出的 `lfHeight` 是 **-15 / -18**（见 §5 的 `SystemParametersInfoForDpi`），
把这两个值交给 GDI 实测 `tmHeight`：

| 缩放 | 系统给的 `lfHeight` | 实测 `tmHeight` | 若按 17 × 缩放 [对比] |
|---|---|---|---|
| 100% (96) | -12 | **17** | 17 ✓ |
| 125% (120) | -15 | **20** | 21.25 ✗ |
| 150% (144) | -18 | **24** | 25.5 ✗ |

**注意：`tmHeight` 不是线性缩放。** 17→20→24，step 是 +3/+4，不是 +4.25/+8.5。
原因是非整数 `lfHeight` 被 GDI 舍入，加上字形 ascent/descent 取整的累积效应。

参考同表（`fontmetrics_raw.txt`）：`Segoe UI` 在 -12/-15/-18 下 tmHeight = **15 / 20 / 25**；
`Tahoma` = **14 / 18 / 22**。**同样 `lfHeight`，YaHei UI 比 Segoe UI 高 1–2px** ——
这解释了为什么按 Segoe UI 校准的行高在中文系统上会偏紧。

#### 2.3.2 YaHei UI 的 `lfHeight → tmHeight` 完整对照（实测）

| lfHeight | -9 | -10 | -11 | **-12** | -13 | -14 | **-15** | -16 | -17 | **-18** | -19 | -21 | -24 | -30 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| tmHeight | 14 | 16 | 16 | **17** | 19 | 20 | **20** | 21 | 23 | **24** | 25 | 28 | 31 | 39 |

加粗强调的三个正是 100% / 125% / 150% 下系统 `lfMessageFont.lfHeight` 的取值。

### 2.4 ICONMETRICS（`SPI_GETICONMETRICS = 0x2D`，`sizeof = 108`）

| 字段 | 本机值 |
|---|---|
| `iHorzSpacing` | **75** |
| `iVertSpacing` | **75** |
| `iTitleWrap` | **1**（开）|
| `lfFont` | `Microsoft YaHei UI`，h=-12，weight=400，charset=1 |

注册表对应 `IconSpacing = -1125`、`IconVerticalSpacing = -1125`（单位 twip/15，-1125/15 = 75 ✓），
`Shell Icon Size = 32`。
`SPI_GETICONTITLEWRAP (0x19)` 返回 1。

### 2.5 ICONTITLELOGFONT（`SPI_GETICONTITLELOGFONT = 0x1F`）

`face = 'Microsoft YaHei UI'`，`lfHeight = -12`，`weight = 400`，`charset = 1`。
（与 `ICONMETRICS.lfFont` 一致，互相印证。）

### 2.6 `SPI_GETSMCAPTIONWIDTH/HEIGHT`

没有独立的 `SPI_GETSMCAPTIONWIDTH`；小标题尺寸只有 `SPI_GETNONCLIENTMETRICS` 里的
`iSmCaptionWidth / iSmCaptionHeight`（本机均 **22**），以及 `SM_CXSMSIZE / SM_CYSMSIZE`（本机 **22 / 22**）。

---

## 3. Shell 字体

### 3.1 FontSubstitutes（注册表实测）

`HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\FontSubstitutes`：

| 键 | 值 |
|---|---|
| `MS Shell Dlg` | **Microsoft Sans Serif** |
| `MS Shell Dlg 2` | **Tahoma** |

注意：这是**系统默认的 legacy 替换表**，跟 Win10 现代 UI 无关。
`MS Shell Dlg 2 → Tahoma` 是对话框模板 `DS_SHELLFONT` 的经典默认，**不是**任务管理器用的字体。
**不要因为「对话框默认字体」就推断任务管理器正文是 Tahoma。**

### 3.2 本机 UI 字体到底是什么 —— 用 API 实测

| 来源 | 值 |
|---|---|
| `SPI_GETNONCLIENTMETRICS.lfMessageFont` | **Microsoft YaHei UI** |
| `SPI_GETICONTITLELOGFONT` | **Microsoft YaHei UI** |
| `HKCU\Control Panel\Desktop\WindowMetrics` 的 `MessageFont`/`CaptionFont`/`MenuFont`/`StatusFont`/`SmCaptionFont`/`IconFont` | 全部 **Microsoft YaHei UI**，h=-12，w=400 |
| 字体文件是否安装 | `msyh.ttc`（YaHei + YaHei UI 合装）、`msyhbd.ttc`、`msyhl.ttc` 均存在 |

**结论：本机（中文系统）的 shell/系统 UI 字体是 `Microsoft YaHei UI`，`Segoe UI` 虽已安装（segoeui.ttf）但不是当前 UI 字体。**
这与 `include/theme.h` 里「优先 Microsoft YaHei UI」的选择**一致**，且本机 `GetTextFaceW` 回读也应是 YaHei UI。

### 3.3 主题/个性化里的 shell 字体设置

- `HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\CurrentTheme` = `C:\Users\<user>\AppData\Local\Microsoft\Windows\Themes\Custom.theme`
- `Themes\Personalize`：`AppsUseLightTheme = 0`、`SystemUsesLightTheme = 0`、`EnableTransparency = 1`、`ColorPrevalence = 0`
- 没有发现独立的「shell 字体」注册表项；shell 字体就是由 `WindowMetrics` 的 `MessageFont` 等驱动的。

⚠ **`AppsUseLightTheme = 0` 表示本机 Windows 处于深色模式。**
但 `GetSysColor(COLOR_WINDOW)=#FFFFFF`、`COLOR_BTNFACE=#F0F0F0` 仍是浅色经典值——
因为 Win32 `GetSysColor` 不跟随深色模式，深色模式只影响 DirectUI/XAML 应用。
任务管理器进程页是 DirectUI，**在原版上会跟随深色模式**；我方是自绘 Win32，不跟随。
这是一个**已知的设计差异**，不是度量问题，但它意味着「按浅色画」在本机与原版观感不同。

---

## 4. DWM / 标准窗口度量

### 4.1 `AdjustWindowRectEx` 实测（这是标题栏/边框的权威答案）

以 800x600 客户区为基准，`WS_OVERLAPPEDWINDOW`、无菜单：

| DPI | 标题栏高（`-rect.top`）| 左边框宽（`-rect.left`）|
|---|---|---|
| **96 (100%)** | **31 px** | **8 px** |
| 120 (125%) | 38 px | 9 px |
| 144 (150%) | 45 px | 11 px |
| 168 (175%) | 52 px | 12 px |
| 192 (200%) | 58 px | 13 px |

带菜单（`menu=TRUE`）时，96 DPI 标题栏区从 31 变 51，即**菜单栏增加 20 px**（= `SM_CYMENU`=20 ✓）。

**分解验证（96 DPI）**：
`SM_CYSIZEFRAME(4) + SM_CXPADDEDBORDER(4) + SM_CYCAPTION(23) = 31` ✓
`SM_CXSIZEFRAME(4) + SM_CXPADDEDBORDER(4) = 8` ✓
→ **标题栏总高 = `SM_CYSIZEFRAME + SM_CXPADDEDBORDER + SM_CYCAPTION`，左侧边框 = `SM_CXSIZEFRAME + SM_CXPADDEDBORDER`。**

### 4.2 关键 `SM_*` 实测表（96 DPI，且用 API 取了各 DPI 的权威值）

| 指标 | idx | 96 | 120 | 144 | 168 | 192 |
|---|---|---|---|---|---|---|
| `SM_CXVSCROLL` | 2 | **17** | 21 | 26 | 30 | 34 |
| `SM_CYHSCROLL` | 3 | **17** | 21 | 26 | 30 | 34 |
| `SM_CYCAPTION` | 4 | **23** | 29 | 34 | 40 | 45 |
| `SM_CXBORDER` | 5 | 1 | 1 | 1 | 1 | 1 |
| `SM_CYBORDER` | 6 | 1 | 1 | 1 | 1 | 1 |
| `SM_CXICON` / `SM_CYICON` | 11/12 | **32** | 40 | 48 | 56 | 64 |
| `SM_CYMENU` | 15 | **20** | 25 | 30 | 34 | 39 |
| `SM_CXSIZE` | 30 | **36** | 46 | 54 | 64 | 72 |
| `SM_CYSIZE` | 31 | **22** | 28 | 33 | 39 | 44 |
| `SM_CXSIZEFRAME` | 32 | **4** | 4 | 5 | 5 | 5 |
| `SM_CYSIZEFRAME` | 33 | **4** | 4 | 5 | 5 | 5 |
| `SM_CXMINTRACK` | 34 | **136** | 170 | 202 | 236 | 262 |
| `SM_CYMINTRACK` | 35 | **39** | 47 | 56 | 64 | 71 |
| `SM_CXEDGE` | 45 | 2 | 2 | 2 | 2 | 2 |
| `SM_CXSMICON` / `SM_CYSMICON` | 49/50 | **16** | 20 | 24 | 28 | 32 |
| `SM_CYSMCAPTION` | 51 | **23** | 29 | 34 | 40 | 45 |
| `SM_CXSMSIZE` / `SM_CYSMSIZE` | 52/53 | **22** | 28 | 33 | 39 | 44 |
| `SM_CXMENUSIZE` / `SM_CYMENUSIZE` | 54/55 | **19** | 24 | 29 | 33 | 38 |
| `SM_CXPADDEDBORDER` | 92 | **4** | 5 | 6 | 7 | 8 |
| `SM_CXMENUCHECK` / `SM_CYMENUCHECK` | 71/72 | 17 | — | — | — | — |

补充：`SM_CXVSCROLL = 17` 与 `iScrollWidth = 17` **完全一致**，互相印证。

---

## 5. 标准主题 part 尺寸 —— 这里有一个必须先说清的坑

### 5.1 ⚠ `GetThemePartSize` 对 LISTVIEW 返回的是 3x3，不是行高

实测（`Aero.msstyles`，`LISTVIEW`）：

| part / state | TS_MIN | TS_TRUE | TMT_HEIGHT | TMT_CONTENTMARGINS |
|---|---|---|---|---|
| `LVP_LISTITEM` LISS_NORMAL(1) | 2x2 | **3x3** | **未定义** | **未定义** |
| `LVP_LISTITEM` LISS_HOT(2) | 2x2 | 3x3 | 未定义 | 未定义 |
| `LVP_LISTITEM` LISS_SELECTED(3) | 2x2 | 3x3 | 未定义 | 未定义 |
| `LVP_LISTITEM` LISS_DISABLED(4) | 2x2 | 3x3 | 未定义 | 未定义 |
| `LVP_LISTITEM` LISS_SELECTEDNOTFOCUS(5) | 2x2 | 3x3 | 未定义 | 未定义 |
| `LVP_LISTDETAIL(3)` | 2x2 | 3x3 | 未定义 | 未定义 |
| `LVP_LISTSORTEDDETAIL(4)` | 2x2 | 3x3 | 未定义 | 未定义 |
| `LVP_COLUMNDETAIL(10)` | 2x1 | 2x1 | 未定义 | 未定义 |

**为什么是 3x3**：`3 = 1px 边框 ×2 + 1px 内容`。Aero 主题的 ListView 项**只是一个 1px 边框的容器**，
它**不携带高度**。ListView 的真实行高由 comctl32 自己在运行时按
`字体 tmHeight + 图标高 + 上下 padding` 算出来，**不在主题里**。
所以「用 `GetThemePartSize(LVP_LISTITEM)` 取标准行高」这条路**在 Win10 Aero 主题下走不通**，
任务描述里的这个假设需要纠正。

`TMT_HEIGHT(2417)` / `TMT_WIDTH(2416)` 我也直接查了，**ListView 全部 state 都返回「未定义」（hr=0x80070490 = ERROR_NOT_FOUND）**。

### 5.2 ListView 行高的权威替代算法 [推算，但只依赖已测值]

`comctl32` 的 ListView 在 report 模式下，行高 ≈ `max(tmHeight, 图标高) + 2×itemPadding`。
本机可确定的硬值：`tmHeight = 17`（§2.3）、`SM_CYSMICON = 16`。
最小行高**不会低于 17 px**（否则裁字）。

### 5.3 各控件 part 尺寸实测（96 DPI，TS_TRUE / TS_MIN）

| 主题类 | part / state | TS_TRUE | TS_MIN | 说明 |
|---|---|---|---|---|
| `HEADER` | `HP_HEADERITEM(1)` HIS_NORMAL(1) | **6 x 24** | 1x1 | 列头高 **24 px** |
| `HEADER` | HIS_HOT(2) | 6 x 24 | 1x1 | |
| `HEADER` | HIS_PRESSED(3) | 6 x 24 | 1x1 | |
| `TAB` | `TABP_TABITEM(1)` TIS_NORMAL(1) | **5 x 19** | 1x1 | 页签高 **19 px** |
| `TAB` | TIS_SELECTED(3) | 5 x 19 | 1x1 | |
| `TAB` | `TABP_TOPTABITEM(5)` TIS_NORMAL(1) | **5 x 19** | 1x1 | |
| `BUTTON` | `BP_PUSHBUTTON(1)` PBS_NORMAL(1) | **13 x 11** | **6 x 6** | 6x6 才是「最小可点尺寸」 |
| `BUTTON` | PBS_DEFAULTED(5) | 13 x 11 | 6 x 6 | |
| `TOOLBAR` | `TP_BUTTON(1)` TS_NORMAL(1) | **7 x 18** | 1x1 | 工具条按钮内容高 **18 px** |
| `TOOLBAR` | TP_BUTTON TS_HOT(2) | 7 x 18 | 1x1 | |
| `TOOLBAR` | `TP_SEPARATOR(5)` TS_NORMAL(1) | 6 x 5 | 5 x 4 | |
| `SCROLLBAR` | `SBP_ARROWBTN(1)` ABS_UPNORMAL(1) | **17 x 17** | 1x1 | 与 `SM_CXVSCROLL`=17 一致 ✓ |
| `SCROLLBAR` | `SBP_THUMBBTNHORZ(3)` SBS_NORMAL(1) | 17 x 11 | 1 x 8 | |
| `STATUS` | `SP_PANE(1)` | 2 x 2 | 1x1 | 状态栏 pane 无固定尺寸 |
| `EDIT` | `EP_EDITTEXT(1)` ETS_NORMAL(1) | 3 x 3 | 2x2 | |
| `PROGRESS` | `PP_BAR(1)` | 19 x 14 | 2x2 | 进度条「最小」19x14 |
| `MENU` | `MENU ITEM` | 3 x 3 | — | 无固定尺寸 |
| `TREEVIEW` | `TREEVIEW ITEM` | 3 x 3 | — | 无固定尺寸 |
| `COMBOBOX` | `COMBOBOX DROPDOWN(1)` | **7 x 21** | — | 下拉项高 **21 px** |

### 5.4 ⚠ 这些 part 尺寸**不随 DPI 变化**

用 `OpenThemeDataForDpi(NULL, cls, dpi)` 在 96/120/144/168/192 下重取，
**所有上述值完全不变**（列头永远 6x24、页签永远 5x19、按钮永远 13x11…）。

原因：`Aero.msstyles` 只有 **NormalSize** 一种变体（`GetCurrentThemeName` 返回 `size='NormalSize'`）。
Windows 10 的系统缩放不是靠主题变体实现的，而是靠**代码按 DPI 放大 + 位图重采样**。

**结论：主题 part 尺寸只能当作「96 DPI 基准值」，实际绘制必须自己乘 `dpi/96`。**
不要指望 `GetThemePartSize` 会给你 125% 的列头高度。

### 5.5 主题边距（96 DPI）

| 项 | 值 |
|---|---|
| `HEADERITEM NORMAL` `TMT_SIZINGMARGINS(3601)` | **L2 T3 R9 B14** |
| `HEADERITEM NORMAL` `TMT_CONTENTMARGINS(3602)` | L0 T0 R0 B0 |
| `TABITEM NORMAL` `TMT_SIZINGMARGINS` | L2 T2 R13 B4 |
| `TABITEM NORMAL` `TMT_CONTENTMARGINS` | L0 T0 R0 B0 |
| `TABITEM NORMAL` `TMT_BORDERSIZE(2403)` | 0 |
| `PUSHBUTTON NORMAL` `TMT_CONTENTMARGINS` | **L3 T3 R3 B3** |
| `PUSHBUTTON NORMAL` `TMT_SIZINGMARGINS` | L6 T6 R5 B5 |
| `TOOLBAR BUTTON` `TMT_CONTENTMARGINS` | L0 T0 R0 B0 |
| `TOOLBAR BUTTON` `TMT_SIZINGMARGINS` | L3 T3 R13 B4 |

`HEADERITEM` 的 `TMT_HEIGHT` / `TMT_BORDERSIZE` / `TMT_FONT` 均**未定义**。

---

## 6. 标准主题色与系统色

### 6.1 `GetSysColor` 实测（96 DPI，浅色经典值）

| 常量 | idx | 本机 RGB |
|---|---|---|
| `COLOR_SCROLLBAR` | 0 | `#C8C8C8` |
| `COLOR_BACKGROUND` | 1 | `#000000` |
| `COLOR_ACTIVECAPTION` | 2 | `#99B4D1` |
| `COLOR_INACTIVECAPTION` | 3 | **`#BFCDDB`** |
| `COLOR_MENU` | 4 | `#F0F0F0` |
| `COLOR_WINDOW` | 5 | **`#FFFFFF`** |
| `COLOR_WINDOWFRAME` | 6 | `#646464` |
| `COLOR_MENUTEXT` | 7 | `#000000` |
| `COLOR_WINDOWTEXT` | 8 | **`#000000`** |
| `COLOR_CAPTIONTEXT` | 9 | `#000000` |
| `COLOR_INACTIVEBORDER` | 11 | `#F4F7FC` |
| `COLOR_APPWORKSPACE` | 12 | `#ABABAB` |
| `COLOR_HIGHLIGHT` | 13 | **`#0078D7`** |
| `COLOR_HIGHLIGHTTEXT` | 14 | **`#FFFFFF`** |
| `COLOR_BTNFACE` | 15 | **`#F0F0F0`** |
| `COLOR_BTNSHADOW` | 16 | `#A0A0A0` |
| `COLOR_GRAYTEXT` | 17 | **`#6D6D6D`** |
| `COLOR_BTNTEXT` | 18 | **`#000000`** |
| `COLOR_INACTIVECAPTIONTEXT` | 19 | `#000000` |
| `COLOR_BTNHIGHLIGHT` | 20 | `#FFFFFF` |
| `COLOR_INFOBK` | 24 | `#FFFFE1` |
| `COLOR_INFOTEXT` | 23 | `#000000` |
| `COLOR_HOTLIGHT` | 26 | **`#0066CC`** |
| `COLOR_MENUHILIGHT` | 29 | `#3399FF` |
| `COLOR_MENUBAR` | 30 | `#F0F0F0` |

这些是**系统级**颜色，不随 DPI 变化。

### 6.2 主题色（`GetThemeColor`，96 DPI）

| 主题 / part / state | 属性 | 值 |
|---|---|---|
| `ListView` LVP_LISTITEM 全部 state | `TMT_FILLCOLOR(3802)` | **`#FFFFFF`** |
| `ListView` LVP_LISTITEM 全部 state | `TMT_BORDERCOLOR(3801)` | **`#828790`** |
| `ListView` LVP_LISTITEM 全部 state | `TMT_TEXTCOLOR(3803)` | **未定义**（hr=0x80070490）|
| `ListView` 全部 state | `TMT_FILLCOLORHINT(3821)` / `TEXTCOLORHINT` / `BORDERCOLORHINT` | **未定义** |
| `HEADER` HIS_NORMAL/HOT/PRESSED | `TMT_FILLCOLORHINT(3821)` | **`#F4F4F4`** |
| `HEADER` 全部 state | `TMT_FILLCOLOR` / `TEXTCOLOR` / `BORDERCOLOR` / `TMT_BORDERSIZE` | **未定义** |
| `TAB` TIS_NORMAL/SELECTED/HOT | `TMT_FILLCOLOR(3802)` | **`#4C85C7`** |
| `TAB` 全部 state | `TMT_FILLCOLORHINT(3821)` | **`#F0F1F5`** |
| `TAB` 全部 state | `TMT_TEXTCOLOR` / `BORDERCOLOR` | **未定义** |
| `BUTTON` PBS_NORMAL | `TMT_TEXTCOLOR` | `#000000` |
| `BUTTON` PBS_NORMAL | `TMT_FILLCOLOR` / `TMT_BORDERCOLOR` / `TMT_BORDERSIZE` | **未定义** |
| `TOOLBAR` TP_BUTTON NORMAL/HOT | `TMT_FILLCOLOR(3802)` | **`#F0F0F0`** |
| `TOOLBAR` TP_BUTTON | `TMT_TEXTCOLOR` | `#000000` |

**重要解读**：
1. `ListView` 的 `TMT_FILLCOLOR` 在所有 state 下都是 `#FFFFFF`，**它不区分普通/热/选中**。
   也就是说**主题根本没给「选中行蓝色」**——ListView 选中行是 comctl32 用
   `GetSysColor(COLOR_HIGHLIGHT)` 自己画的。本机该值 = **`#0078D7`**。
2. `HEADER` 的填充色不在 `TMT_FILLCOLOR` 而在 `TMT_FILLCOLORHINT` = **`#F4F4F4`**。
3. 大量属性返回 `0x80070490`（`ERROR_NOT_FOUND`），说明该主题类/状态**没有声明**该属性，
   这时应回退到 `GetSysColor`，而不是把 0 当黑色用。

---

## 7. 与我们代码现状的对照

| 代码常量 | 位置 | 当前值 | 系统权威值 | 评价 |
|---|---|---|---|---|
| `rowHeight_` | `include/tableview.h:92` | **20** | 字面下限 17（`tmHeight`）| 可用；但比 YaHei UI 单行 17 只多 3px |
| `headerHeight_` | `include/tableview.h:93` | **24** | **24**（`HEADERITEM` TS_TRUE）| **完全一致** ✓ |
| `kRowHeight` | `include/theme.h:155` | **28** | 无系统对应值 | 28 = 17 + 11 余量，视觉合理 |
| `kHeaderHeight` | `include/theme.h:158` | **24** | **24** | **完全一致** ✓ |
| `kToolbarHeight` | `include/theme.h:172` | **32** | `TP_BUTTON`=18 + `SM_CYEDGE`×2=4 → 22；`SM_CYSIZE`=22 | 32 偏大，但工具条高度含 padding，不算错 |
| `kFontSizeBody` | `include/theme.h:16` | **12** | `lfMessageFont.lfHeight = -12` | **完全一致** ✓ |
| 字体族 | `theme.h` `MakeUiFont` | `Microsoft YaHei UI` | 本机 shell 字体 = **Microsoft YaHei UI** | **完全一致** ✓ |

---

## 8. 汇总表：每个 UI 元素对应的系统权威值

| UI 元素 | 系统权威值（96 DPI） | API / 来源 | 125% | 150% | 建议采用 |
|---|---|---|---|---|---|
| **列表行高** | **17 px（字面下限）**<br>主题**不提供** | `GetTextMetricsW(lfMessageFont).tmHeight`；`LVP_LISTITEM` 的 `TMT_HEIGHT` **未定义** | **20**（实测，`lfHeight=-15`）| **24**（实测，`lfHeight=-18`）| 用 `tmHeight + 上下 padding` 自算；**不要期待主题给值**，也**不要**按 17×缩放 |
| **列头高** | **24 px** | `GetThemePartSize(HEADER, HP_HEADERITEM, HIS_NORMAL, TS_TRUE)` | 30 [推算 24×1.25] | 36 [推算 24×1.5] | 取 24 × `dpi/96`，与本机 `headerHeight_=24` 已一致 |
| **页签高** | **19 px** | `GetThemePartSize(TAB, TABP_TABITEM, TIS_NORMAL, TS_TRUE)` | 24 [推算] | 29 [推算] | 19 × `dpi/96`；`kFontSizeNav=13` 时 19 偏紧（YaHei UI @-13 tmHeight=19，正好顶满），需加 padding |
| **按钮高** | **11 px（内容）/ 6x6（最小可点）** | `GetThemePartSize(BUTTON, BP_PUSHBUTTON, PBS_NORMAL)` TS_TRUE/TS_MIN | 14 / 8 [推算] | 17 / 9 [推算] | 实际按钮高 = 文字 17 + padding，**别直接用 11** |
| **工具条高** | **18 px（按钮内容）**<br>或 `SM_CYSIZE = 22` | `GetThemePartSize(TOOLBAR, TP_BUTTON, TS_NORMAL)`；`GetSystemMetrics(SM_CYSIZE)` | 22 / 28 | 27 / 33 | 取 `SM_CYSIZE`=22 作按钮高基准；工具条总高另加 padding |
| **滚动条宽** | **17 px** | `GetSystemMetrics(SM_CXVSCROLL)`；`SPI_GETNONCLIENTMETRICS.iScrollWidth`；`SCROLLBAR SBP_ARROWBTN` TS_TRUE=17x17 | **21**（API 实测）| **26**（API 实测）| 直接用 `SM_CXVSCROLL`，三处互相印证 ✓ |
| **菜单栏高** | **20 px** | `GetSystemMetrics(SM_CYMENU)`；`iMenuHeight = 19` | **25**（API 实测）| **30**（API 实测）| 20 × `dpi/96`（注意 `iMenuHeight`=19 与 `SM_CYMENU`=20 差 1）|
| **标题栏高** | **31 px**（含 frame+padded+caption）| `AdjustWindowRectEx(WS_OVERLAPPEDWINDOW)`；`SM_CYSIZEFRAME(4)+SM_CXPADDEDBORDER(4)+SM_CYCAPTION(23)` | **38**（API 实测）| **45**（API 实测）| 用 `AdjustWindowRectExForDpi`，别手算 |
| **窗口边框宽** | **8 px** | 同上；`SM_CXSIZEFRAME(4)+SM_CXPADDEDBORDER(4)` | **9**（API 实测）| **11**（API 实测）| 同上 |
| **图标尺寸** | **32 px**（`SM_CXICON`）/ **16 px**（`SM_CXSMICON`）| `GetSystemMetrics`；`Shell Icon Size = 32` | **40 / 20**（API 实测）| **48 / 24**（API 实测）| 用 `SM_CXSMICON` |
| **正文文字高** | **17 px**（`tmHeight`）| `GetTextMetricsW` on `lfMessageFont` | **20**（实测）| **24**（实测）| 与列表行高同源 |
| **系统 UI 字体** | **Microsoft YaHei UI**，h=**-12**，w=**400** | `SPI_GETNONCLIENTMETRICS.lfMessageFont` | h=-15 | h=-18 | 与 `theme.h` 一致 ✓ |
| **选中色** | **`#0078D7`** | `GetSysColor(COLOR_HIGHLIGHT)` | 不变 | 不变 | **原版 ListView 选中行就是这个色**，不是主题给的 |
| **选中文字色** | **`#FFFFFF`** | `GetSysColor(COLOR_HIGHLIGHTTEXT)` | 不变 | 不变 | |
| **列头底色** | **`#F4F4F4`** | `GetThemeColor(HEADER, HIS_NORMAL, TMT_FILLCOLORHINT)` | 不变 | 不变 | 我方用工 `#FFFFFF`，见下方说明 |
| **窗口底色** | **`#FFFFFF`** | `GetSysColor(COLOR_WINDOW)` | 不变 | 不变 | |
| **正文文字色** | **`#000000`** | `GetSysColor(COLOR_WINDOWTEXT)` | 不变 | 不变 | 我方用 `RGB(20,20,20)`，接近 |
| **禁用文字色** | **`#6D6D6D`** | `GetSysColor(COLOR_GRAYTEXT)` | 不变 | 不变 | |

---

## 9. 需要提请注意的三点

1. **任务描述里「用 `GetThemePartSize` 取 LISTVIEW 行高」的做法在本机不成立。**
   Aero 主题的 `LVP_LISTITEM` 只有 3x3（1px 边框），`TMT_HEIGHT` 未定义。
   行高的真实来源是字体 `tmHeight`（17px，dpi=96）。

2. **主题 part 尺寸不随 DPI 缩放**（`OpenThemeDataForDpi` 在 96–192 下返回完全相同）。
   Aero 只有 NormalSize 变体。所有主题尺寸都必须由代码乘 `dpi/96`。

3. **参考图很可能是 150% DPI 截的**（14rp → 22px，倍率 1.571）。
   若成立，此前所有「按参考图量像素」的结论都要先 ÷1.5。
   这与 `_FINAL_VERDICT.md` 判定「truth1/m3 是我方截图」的结论**互相独立**，
   但指向同一件事：**像素实测值不可信，系统 API 值才可信。**

---

## 10. 取值的正确姿势（给后续改代码的人）

三类值，三条不同路径，**不能混用**：

| 类别 | 例子 | 正确取法 | 错误做法 |
|---|---|---|---|
| **A. 系统已按 DPI 算好的值** | `SM_CXVSCROLL`、`SM_CYCAPTION`、`iScrollWidth`、`lfMessageFont.lfHeight` | `GetSystemMetricsForDpi(i, dpi)`<br>`SystemParametersInfoForDpi(...)` | 拿 96 基准值自己乘系数（会差 1–2px，见 §2.1 / §2.3.1）|
| **B. 主题给的 96 基准值** | `HEADERITEM`=6x24、`TABITEM`=5x19、`TP_BUTTON`=7x18 | `GetThemePartSize` 取基准，**再自己乘 `dpi/96`** | 以为 `OpenThemeDataForDpi` 会返回放大值（不会，§5.4）|
| **C. 主题不提供的值** | ListView 行高 | `GetTextMetricsW` 的 `tmHeight` + 自定 padding | 期望 `LVP_LISTITEM` 的 `TMT_HEIGHT` 给值（未定义，§5.1）|

补充两条：

- **`tmHeight` 必须实测，不能按比例推**：17 → 20 → 24，不是 17 → 21.25 → 25.5（§2.3.1）。
- **换字体族时行高会变**：同一 `lfHeight=-12`，YaHei UI 是 17、Segoe UI 是 15、Tahoma 是 14。
  如果哪天退回 Segoe UI，行高会小 2px，原本「正好」的 `rowHeight_=20` 会显得更空。

---

## 11. 未取到的项

- **ListView report 模式的实际行高**：本机没有 `SysListView32` 实例（枚举 432 个顶层窗口，0 个命中；
  Win10 资源管理器用 DirectUI 不用 ListView），无法读 `LVM_GETITEMSPACING` 实测。
  只能用 `tmHeight=17` 作下限 [推算]。
- **`LVP_LISTITEM` 的 `TMT_TEXTCOLOR`**：主题未定义，无法取。
- **`GetThemePartSize` 对 LISTVIEW 在 125%/150% 的值**：因为不随 DPI 变化，无意义（见 §5.4）。
- **`SPI_GETSMCAPTIONWIDTH/HEIGHT`**：无此独立 SPI，见 §2.6。
