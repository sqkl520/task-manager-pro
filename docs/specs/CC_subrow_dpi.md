# CC —— 子行行高（22rp）+ DPI 适配机制

**状态：已完成（未编译，按指令）**

> 代码快照：2026-11-17 19:5x。
> ⚠ `src/tmwindow_events.cpp` 在本次作业期间被另一个 agent（P0 假功能）改动过，
> 行号已漂移（例如 HitTestGroupHeader 的 `const int rh = ...` 从我开工时的 :97
> 漂到 :101，滚轮分支从 :384 漂到 :427）。**交接单里的行号仅供定位，
> 请以函数名 + 引用的原始代码片段为准**。

---

## 任务一：子行行高

### 结论先行：怎么判"子行"

`include/tmwindow.h` 的 `ViewRow`（:299-308）**不需要加字段**。现有字段已经足够：

| 判定 | 含义 |
|---|---|
| `kind == GroupHeaderRow` | 组头 |
| `kind == ProcessRow && indentLevel == 0` | 父行（应用行、后台进程行） |
| `kind == ProcessRow && indentLevel > 0` | **子行**（应用展开后的子进程行） |

`src/tmwindow.cpp` 的 `RebuildRows` 里只有两处写 `indentLevel = 1`，都是子进程行：
- `:604`（平铺视图分支的 `appendKids` lambda 内，`kr.indentLevel = 1;`）
- `:719`（分组视图分支，`kr.indentLevel = 1;`）

其余 `:624 / :663 / :676` 全是 0。

原版结构对照（`docs/specs/S_duispec.md` §3.1 元素树）：

```
TmScrollerItemList
├ TmAppViewItem  (sheet=TmViewItemStyle)      → TmViewRow height = 28rp
│   └ TmFirstColumn padding = rect(6rp,0,0,0)
├ TmAppChildViewItem                          → TmViewRow height = 22rp
│   └ TmFirstColumn padding = rect(14rp,0,0,0)
└ TmGroupHeader_Apps / _Windows               → TmGroupRow（UIFILE 无 height）
```

即：父/子行不是"缩进多一级的同一行"，而是**两个不同的 item 类型、两个不同的行高**。

### 已做（theme.h 机制）

`include/theme.h`：

1. 保留 `kRowHeight = 28` / `kSubRowHeight = 22` / `kGroupRowHeight = kRowHeight` 三个
   `constexpr` 不变（值没动，仍是 96 DPI 基准）。
2. 新增**唯一入口函数**（:361 / :368）：

```cpp
inline int RowHeightFor(bool isGroupHeader, int indentLevel) {
    if (isGroupHeader) return kGroupRowHeight;
    return indentLevel > 0 ? kSubRowHeight : kRowHeight;
}
inline int RowHeightForPx(bool isGroupHeader, int indentLevel) {
    return Scaled(RowHeightFor(isGroupHeader, indentLevel));
}
```

签名设计说明：`theme.h` 看不到 `ViewRow`（`tmwindow.h` include 它，反向不行），
所以只收两个值，不收 `ViewRow&`。`RowHeightForPx` 直接给绘制/命中测试用
（本机 96 DPI 下与 `RowHeightFor` 逐值相同）。
3. 注释里写明了判定式、`RowHeightFor` 的典型改法、以及"组头优先于子行"的理由。

**没有改 `src/tmwindow.cpp` 的行高使用点** —— 原因见下面"为什么不在这里顺手改完"。

### 为什么不在这里顺手改完（重要）

`kRowHeight` 的行高使用点一共 **15 处**，分布：

| 文件 | 处数 | 归谁 |
|---|---|---|
| `src/tmwindow_list.cpp` | 3（PaintRows / ContentHeight / 一行注释） | 另一个 agent，**不归我** |
| `src/tmwindow_events.cpp` | 9 | 另一个 agent（P0 假功能），**不归我** |
| `src/tmwindow.cpp` | 1（`RebuildRows` 末尾的 scroll 钳制） | 归我，但**不在 BuildLayout 里** |

任务书对我说"只改 `src/tmwindow.cpp` 的 BuildLayout"，而 tmwindow.cpp 里唯一那处
行高使用点在 `RebuildRows`（:813，`finish_rows:` 标号下面的滚动范围钳制），
**不在 BuildLayout**。所以按指令我没有动它 —— 否则我改 1 处、list/events 那边 12 处
不跟，会出现「列表按 28 画、命中测试按 22 算」的错位，比现在更糟。

**这是有意的半成品：函数就位、迁移未完。** 全部 15 处的迁移必须**同一个提交**做掉，
否则点行会选错行（PaintRows 用 rh 累加 y、HitTestRow 用 rh 反查 i，两边必须同一套值）。

### 交接单（精确到行号 + 原片段）

改法统一：把
`const int rh = (X.kind == ViewRow::GroupHeaderRow) ? kGroupRowHeight : kRowHeight;`
换成
`const int rh = RowHeightForPx(X.kind == ViewRow::GroupHeaderRow, X.indentLevel);`

（`RowHeightForPx` 而不是 `RowHeightFor`：一次改到位，DPI 适配这一维也就不用再碰第二遍。
本机 96 DPI 下两者输出完全相同。）

#### A. `src/tmwindow_list.cpp`（3 处，最关键）

| # | 行号 | 函数 | 原代码 | 改成 |
|---|---|---|---|---|
| A1 | **:583** | `PaintRows` | `const int rh = (row.kind == ViewRow::GroupHeaderRow) ? kGroupRowHeight : kRowHeight;` | `const int rh = RowHeightForPx(row.kind == ViewRow::GroupHeaderRow, row.indentLevel);` |
| A2 | **:1091** | `ContentHeight` | `h += (r.kind == ViewRow::GroupHeaderRow) ? kGroupRowHeight : kRowHeight;` | `h += RowHeightForPx(r.kind == ViewRow::GroupHeaderRow, r.indentLevel);` |
| A3 | **:506-518** 的注释块 | `PaintRows` 门口 | 注释写「行高 = tmHeight + 2×pad，pad 取 5.5 得 28」 | **这段依据已经作废**：行高是 UIFILE 声明的 28rp/22rp，不是 tmHeight 推导。删掉推导句，改成引用 `theme::RowHeightFor` |

A1 是**绘制侧**的 row 循环（`y += rh`，:944 附近收尾）；A2 是滚动条用的总高。
两者必须同时改，否则滚动条滑块长度和实际内容高对不上。

⚠ A1 改完后 `PaintRows` 内部所有 `y + rh`、`(rh - kIconSize) / 2`、`(rh - boxH) / 2`
（:717 / :750 等）会自动跟着子行变矮，**不需要另外改**，那些用的都是 `rh`。

#### B. `src/tmwindow_events.cpp`（9 处，行号已漂移，按函数定位）

| # | 位置（函数 + 原片段） | 原代码 | 改成 |
|---|---|---|---|
| B1 | `HitTestGroupHeader` 内（快照 :101） | `const int rh = (rows_[i].kind == ViewRow::GroupHeaderRow) ? kGroupRowHeight : kRowHeight;` | `const int rh = RowHeightForPx(rows_[i].kind == ViewRow::GroupHeaderRow, rows_[i].indentLevel);` |
| B2 | `HitTestRow` 内（快照 :114） | 同上（`rows_[i]`） | 同上 |
| B3 | `WM_KEYDOWN` 方向键的"视图跟随"累加（快照 :744-745） | `y += (rows_[i].kind == ViewRow::GroupHeaderRow) ? kGroupRowHeight : kRowHeight;` | `y += RowHeightForPx(rows_[i].kind == ViewRow::GroupHeaderRow, rows_[i].indentLevel);` |
| B4 | 同 B3 块内，紧随其后（快照 :749-750） | `if (y + kRowHeight > scrollY_ + bodyH) { scrollY_ = y + kRowHeight - bodyH; }` | `const int rhSel = RowHeightForPx(rows_[next].kind == ViewRow::GroupHeaderRow, rows_[next].indentLevel);` 然后用 `rhSel` 替换这两处 `kRowHeight` |

**B1/B2 是硬性要求，漏了就是功能性 bug**：PaintRows 按 22 画子行、HitTestRow 按 28 反查，
每展开一个应用，它下面所有行的选中都会往上偏。

B5-B9 是 **明细页 / 滚轮 / 滚动条箭头**，与父子行无关，属于"顺手 DPI 化"，见任务二交接单。

#### C. `src/tmwindow.cpp`（1 处，不归我的 BuildLayout，但归"我"这个文件 —— 由主线程裁决做不做）

| # | 行号 | 函数 | 原代码 |
|---|---|---|---|
| C1 | **:813** | `RebuildRows`（`finish_rows:` 之后） | `for (const auto& r : rows_) totalH += (r.kind == ViewRow::GroupHeaderRow) ? kGroupRowHeight : kRowHeight;` |

改成 `totalH += RowHeightForPx(r.kind == ViewRow::GroupHeaderRow, r.indentLevel);`。

⚠ 我**没有**改：它在 `RebuildRows`，不在 BuildLayout 范围内，且必须与 A1/A2 同批落。
如果只改 C1 不改 A1/A2，`RebuildRows` 的滚动上限按新高度算、`PaintRows` 仍按旧高度画，
列表底部会凭空多出一段滚不动的空白（子行越多越明显）。

#### D. `include/tmwindow.h`（不在我的文件列表，只能写交接）

- `:284` 有 `int ContentHeight() const;` 声明，实现是 A2。**声明不用改**（返回类型不变）。
- `:299-308` 的 `ViewRow` **不需要加 `isChild` 字段**。若坚持要加，唯一正确的位置是
  `RebuildRows` 里 `kr.indentLevel = 1` 的两处（tmwindow.cpp:604 / :719）同步置 true，
  但那只是给 `indentLevel == 1` 起个别名，徒增"两处要同步"的风险。**建议不加。**

#### E. `src/pages_view.cpp` / `src/tmwindow_details.cpp`（**不是子行问题**，别误改）

这两个文件里的 `kRowHeight`（pages_view 40+ 处、details 20+ 处）是**启动/用户/服务/详细信息**页
的平铺表格行高 —— 那些页没有父子行，全部保持单一高度。它们不需要 `RowHeightFor`，
只需要在将来 DPI 化时统一包 `Scaled()`。**本轮不动。**

---

## 任务二：DPI

### 现状（两条硬事实）

1. 本机 **96 DPI / 100%**：`GetDpiForSystem()`、`GetDeviceCaps(LOGPIXELSX)`、
   `SPI_GETLOGICALDPIOVERRIDE` 全部指向 96（`R_sysmetrics.md` §1）。
2. 主题 part 尺寸**不随 DPI 变化**（`R_sysmetrics.md` §5.4）：`Aero.msstyles` 只有
   NormalSize 一种变体，`GetThemePartSize` 在 96/120/144/168/192 下返回完全相同
   （列头永远 6x24、页签图元永远 5x19）。**系统不会帮你乘，必须自己乘。**

所以：所有 `constexpr` 几何常量 = 96 DPI 基准值，125%/150% 下必须在使用处乘 `dpi/96`。

### 已做（theme.h 机制）

`include/theme.h` 顶部新增（:70-95）：

```cpp
inline int Dpi();                        // 进程级，GetDeviceCaps(LOGPIXELSX)，取一次，回退 96
inline int Scaled(int px96) { return MulDiv(px96, Dpi(), 96); }   // 96基准px → 当前px
inline int Rpx(int rp)      { return Scaled(rp); }                // 同义，名字对应 UIFILE 的 rp
```

并在 `// ---------- 度量 ----------` 上方（:303-308）加了一段"所有常量都是 96 DPI 基准 px"的总说明。
**没有把任何 `constexpr` 改成函数**（按指令：本机 100%，改名会波及 100+ 调用点，风险 > 收益）。

### 已做（tabs.cpp，本次唯一做了实质迁移的地方）

`src/tabs.cpp` 是"页签几何的两个出口"，只有两处，收口代价低，所以顺手做了真迁移：

| 位置 | 改法 | 96 DPI 等价性 |
|---|---|---|
| `tabtheme::MeasureHeight`（:21） | `const int baseH = theme::Scaled(tabtheme::kHeight);` 后面全用 `baseH`（含防裁字比较 `if (tmHeight > baseH)`） | `Scaled(30) == 30`，逐点等价 |
| `TabBar::RecomputeWidths`（:68） | `widths_[i] = sz.cx + theme::Scaled(tabtheme::kPadX) * 2;` | `Scaled(11) == 11`，逐点等价 |

同时 `src/tabs.cpp` 现在 `#include "theme.h"`（原来只 include `tabs.h`）。
`include/tabs.h` 也补了"本 namespace 的 int 常量是 96 DPI 基准值"的说明，
并注明**缩放已收敛在 tabs.cpp 的两个出口，调用方（BuildLayout）拿到的就是当前 DPI 下的 px，不要重复乘**。

### 已做（tmwindow.cpp BuildLayout，本次唯一动过的文件范围）

`BuildLayout`（:154-247）就是"整窗几何的唯一出口"，全部几何量已包 `Scaled()`：

| 行 | 原 | 现 |
|---|---|---|
| :181-184 | `H - kToolbarHeight` / `H - kToolbarHeight` | `const int toolbarH = theme::Scaled(kToolbarHeight);` 然后用 `toolbarH` |
| :196 | `const int pad = 12;` | `const int pad = theme::Scaled(12);` |
| :219-221 | `rcContent_.top + 10` / `kCardHeight` | `const int gap = theme::Scaled(10);` / `Scaled(kCardHeight)` |
| :236-238 | `+ 10` 两处 / `colTop + kProcHeaderHeight` | `+ gap` 两处 / `+ theme::Scaled(kProcHeaderHeight)` |
| :180 | `tabtheme::MeasureHeight(fontNav_)` | **未包**（已由 tabs.cpp 内部缩放过，重复包会双倍放大）—— 代码里写明了这条 |

并把原来两句 `+ 10` 提成 `const int gap`，避免漏改一处导致卡片区和列头间距不一致。

**96 DPI 等价性**：以上每一处 `Scaled(x) == x`，`rcTabs_ / rcContent_ / rcToolbar_ / rcCards_ /
rcColHeader_ / rcList_` 六个矩形与改动前**逐像素相同**。所以这一改动在本机是零视觉变化。

### 交接单：`perfpage.h` 的 `Rpx` 冲突怎么解决

**冲突本体**：

- `include/perfpage.h:63`（`namespace tmp` 内，**不在** `perftheme` 里）：
  `inline int Rpx(int rp)` —— dpi 用的同样是 `GetDeviceCaps(LOGPIXELSX)`，逻辑与 `theme::Rpx` 等价。
- `include/theme.h:95`：`namespace tmp::theme` 内 `inline int Rpx(int rp)`。

**为什么现在不炸**：`perfpage.h` 在 `namespace tmp` 里，`theme::Rpx` 在 `tmp::theme` 里，
两者不同名空间，`perfpage.cpp` 只 `using namespace perftheme;`，不 `using namespace theme;`，
所以现有的 39 处 `Rpx(...)` 调用（全在 `src/perfpage.cpp`）解析到的是 `tmp::Rpx`，无二义性。

**什么时候会炸**：任何 TU 同时 `using namespace theme;`（或 `using theme::Rpx;`）
又**不加限定**地调 `Rpx(...)`。`tmwindow.h` 同时 include `theme.h` 和 `perfpage.h`，
`tmwindow_*.cpp` 全都 `using namespace theme;` —— **只要有人在这些 .cpp 的无名 namespace 之外
写一句 `Rpx(20)`，立刻二义性编译失败**。这是埋在现有的代码里的地雷。

**解法（`perfpage.h` 的 owner 执行，二选一）**：

- **方案 A（推荐，改动最小）**：删掉 `include/perfpage.h:63-71` 那份 `Rpx` 定义，原地换成
  ```cpp
  // DPI 换算已上提到 include/theme.h，全项目只有一份实现。
  using theme::Rpx;
  ```
  `perfpage.cpp` 那 39 处 `Rpx(...)` **一行都不用改**（`tmp::Rpx` 变成别名，解析不变）。
  前提：`perfpage.h` 已经 `#include "theme.h"`（:27，已满足）。
- **方案 B**：装进 `namespace perftheme`，变成 `perftheme::Rpx`，`perfpage.cpp` 靠现有
  `using namespace perftheme;` 继续工作。但这会让"DPI 换算"看起来仍是性能页私有概念，
  与"提到公共头"的目标相反，不推荐。

**别做的事**：不要在 `theme.h` 里把 `theme::Rpx` 删掉只留 `Scaled` —— 名字 `Rpx` 直接对应
UIFILE 的 `28rp` 写法，将来迁移 `perfpage.cpp` 以外的地方时最不容易抄错单位。

### 将来适配 125% 的完整清单

按"改一个文件、验证一个"的顺序，从收口最少的往最多的排：

**第 0 步 · 前置（必做）**

1. `include/theme.h` 的 `Dpi()` 是**进程级**缓存（`GetDeviceCaps(LOGPIXELSX)` 取一次）。
   `main.cpp:217` 用的是 `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`，窗口拖到另一块
   不同缩放的显示器上时 DPI 会变、这里的缓存不会变。
   真做 per-monitor 时：`TmWindow` 加一个 `int dpi_ = 96;` 成员，在 `WM_DPICHANGED` 里
   `dpi_ = HIWORD(wparam)`（或 `GetDpiForWindow(hwnd_)`），`BuildLayout` 用成员而不是 `theme::Dpi()`。
   同时要做字体重建（`BuildFonts()` 里的 `kFontSize*` 也要乘）。
   **注意 `tm_mHeight` 不是线性缩放**：96→17、120→20、144→24（`R_sysmetrics.md` §2.3.1），
   不能按 17×1.25=21.25 推。行高目前是 UIFILE 硬值不受影响，但任何"按 tmHeight 撑高"的
   保护逻辑（如 `MeasureHeight` 的防裁字比较）要重新核对。

**第 1 步 · 已经做完的（本轮）**

2. `src/tabs.cpp`：`MeasureHeight` / `RecomputeWidths` 已 `Scaled()`。✅
3. `src/tmwindow.cpp` 的 `BuildLayout`：整窗骨架已 `Scaled()`。✅

**第 2 步 · 列表主体（子行迁移同批做，见任务一交接单）**

4. `src/tmwindow_list.cpp`：
   - `PaintRows`（:583 的 `rh`）：改用 `RowHeightForPx` —— 一次把行高和 DPI 两件事都解决。
   - `ContentHeight`（:1091）：同上。
   - `PaintColumnHeader`（:344 起）：`kIndentPerLevel` / `kCellPadLeft` / `kCellPadRight` /
     `kIconSize` / `NameColWidth(...)` 的入参都要 `Scaled()`。
   - `PaintScrollBar` 的 `kScrollBarW(17)` / `kScrollBtnH(17)` / `kThumbMinLen(11)`（:1075-1077 一带）：
     这三个都来自 `SM_CXVSCROLL` / `SBP_ARROWBTN` 的 96 DPI 值 → `Scaled()`。
     **更好的做法**：直接调 `GetSystemMetrics(SM_CXVSCROLL)`，它**会**随 DPI 变，
     比 `Scaled(17)` 更准（自定义缩放级别下两者可能不等）。
   - `TreeTriangleX`（:299）：`kTreeIndent` → `Scaled()`。
   - `PaintRows` 里的图标槽 `kIconLeft / kIconSize / kExpandoWidth`：`Scaled()`，
     或改调 `GetSystemMetrics(SM_CXSMICON)`（同样随 DPI 变）。

5. `src/tmwindow_events.cpp`：
   - `HitTestGroupHeader` / `HitTestRow`：跟任务一 B1/B2 一起换 `RowHeightForPx`。
   - 滚轮 `kRowHeight * 3`（快照 :681 / :696）：滚 3 行，行高已随 `RowHeightForPx` 变，此处仍算对；
     但**别**把它换成 `Scaled(kRowHeight)` —— 那会在大字号下滚得太远。
   - 滚动条箭头 `btnH = 16`（快照 :424 附近）与 `tmwindow_list.cpp` 的 `kScrollBtnH = 17` 差 1px，
     **这是已知遗留**（`tmwindow_list.cpp:1043` 的【交接】注释写了），本轮不修，DPI 化时一并统一。
   - 明细页 `int row = (y - hdrBottom + detailScrollY_) / kRowHeight;`（快照 :557 / :604）：
     `kRowHeight` → `Scaled(kRowHeight)`，且必须与 `tmwindow_details.cpp` 的行高**同一表达式**，
     否则又会出现"点第 N 行选中第 N+1 行"（那个 bug 之前就发生过，见该处注释）。

**第 3 步 · 其余页面（工作量最大，优先级最低）**

6. `src/pages_view.cpp`（44 处几何常量引用）：启动/用户/服务/应用历史四个页的表格。
   `kHeaderHeight` / `kRowHeight` / `kIconSize` / 各 `*Cols[].width` 全部要 `Scaled()`。
   **注意**：`*Cols[].width` 里有些是"按比例分配后剩下的 flexW"（如 `flexW - kIconSize - 12`），
   那些派生值不要重复缩放。
7. `src/tmwindow_details.cpp`（27 处）：同 6，且与 events 的明细页命中测试联动（见第 5 条最后一款）。
8. `src/tmwindow_paint.cpp`（12 处）：`kCardGap` / `kCardMinWidth` / `kCardPadding` 与
   `card.top + 5 / + 21 / + 49 / + 10` 这类**裸字面量**。后者是 `kCardHeight` 的内部切分，
   DPI 化时必须和 `Scaled(kCardHeight)` 一起按比例改（或者把卡片内部布局改成相对比例）。
9. `src/tmwindow_toolbar.cpp`（1 处 + 内部字面量）：`kToolbarBtnHeight` / `kToolbarPadX` /
   `kIconBoxSize` 等都在该文件的匿名 namespace 里，按同样规则 `Scaled()`。
10. `src/perfpage.cpp`：已经全程用 `Rpx()`，**只需要**把 `perfpage.h` 的 `Rpx` 按上面方案 A 换成
    `using theme::Rpx;`，其余零改动。
11. `src/tmwindow_menus.cpp` / `menus.cpp`：菜单是系统原生菜单（`HMENU`），
    尺寸由系统按 DPI 处理，**不需要**改。只有自绘的菜单度量（若有）需要。
12. `build.bat` / 清单文件：目前没有 `dpiAware` 清单，靠 `main.cpp:217` 的
    `SetProcessDpiAwarenessContext` 在运行时声明。这已经够了；**不要**再加
    `SetProcessDPIAware()`（那会覆盖成 system-aware，把 per-monitor 降级）。

**风险提示（必须写进任何 DPI 任务书）**

- 每一处 `Scaled()` 都要能回答"这个值是不是已经是派生值"。给派生值再包一层是 DPI 化的
  头号错误，症状是 125% 下越往嵌套深处越肿。判断方法：看它是不是刚从另一个已缩放值算出来的。
- 行高和命中测试必须**成对**改（PaintRows ↔ HitTestRow），否则点错行。
- 改完必须真机在 125%/150% 下目视核一遍。本轮**全程未启动程序**（用户明令禁止），
  所以以上所有"逐像素等价"的结论都是**逻辑推导**（`Scaled(x) == x` at 96 DPI），
  不是实测。

---

## 待编译

**按指令未编译。** 需要在下次编译时重点验证的三点：

1. `include/theme.h` 新增的 `Dpi()` / `Scaled()` / `Rpx()` / `RowHeightFor()` /
   `RowHeightForPx()` 都是 `inline`，且定义在 `namespace tmp::theme` 内 —— 头文件多 TU 包含不会重复定义。
2. **潜在二义性**：`include/perfpage.h` 的 `tmp::Rpx` 与新的 `theme::Rpx` 目前不冲突
   （没有任何 TU 同时 `using namespace theme;` 且不加限定调 `Rpx`）。
   `perfpage.cpp` 只 `using namespace perftheme;`，39 处 `Rpx(...)` 仍解析到 `tmp::Rpx`。
   **但这是一颗地雷**，见上面任务二交接单的方案 A。
3. `src/tabs.cpp` 新增 `#include "theme.h"`；`tabs.h` 与 `theme.h` 之间没有循环包含
   （`theme.h` 只 include `<windows.h>`）。

另外记录一个**不属于我范围、但编译时应该会看到**的情况：`src/tmwindow.cpp` 与
`src/tmwindow_events.cpp` 在本次作业期间被其他 agent 改动过（`tmwindow.cpp` 里
`simpleView_` 已经真正接进 `BuildLayout` 了，与我改前的快照不同）。我改的 BuildLayout
与那个改动**不冲突**（`simpleView_` 的三元分支我保留了，只把里面的字面量换成 `gap` / `Scaled(...)`），
合并后语义一致。
