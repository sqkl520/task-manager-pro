# II_colors.md —— 组头热力着色 + 三处配色对齐

状态：已完成（组头热力着色 **未做成**，是文件边界问题，见「交接单」）

> 执行 Agent II。文件边界：只改了 `include/theme.h` 与 `src/tmwindow_paint.cpp`。
> `src/tmwindow_list.cpp` **一行未改**（组头绘制就在那里，归 HH）。
> 未编译、未运行 exe、未运行 taskmgr、未截图、未切前台 —— 全部像素取样用
> `System.Drawing.Bitmap` 直接读 `docs/reference/original_win10.png`，不产生屏幕截图。

---

## 0. 参考图验真（先做，否则后面全是废数据）

`docs/reference/original_win10.png`（1251×842）按 `theme.h` 文件头那四条逐一复核，
**确认是真原版，不是我方截图**：

| 判据 | 我方截图（m3/truth1） | 本图 | 结论 |
|---|---|---|---|
| 标题栏文字 | 无 | `任务管理器`（y=10..30 有 5 个字形簇） | 真 |
| 窗口图标 | IDI_APPLICATION | 带白色曲线的显示器 | 真 |
| 页签个数 | 7 个 | **5 个**：进程/性能/用户/详细信息/服务 | 真 |
| 底部状态栏 | 有「进程数: %u …」 | 无，只有工具条「简略信息(D)」 | 真 |
| 组头行 | 无 | 有「应用 (11)」「后台进程 (105)」 | 真 |

脚本：`docs/evidence/exec/_ii_scan.ps1` / `_ii_profile.ps1` / `_ii_hdr.ps1` / `_ii_bands.ps1` /
`_ii_textstats.ps1` / `_ii_ident.ps1`。

---

## 1. 关键实测数据（后面所有结论都从这里来）

### 1.1 组头行「应用 (11)」y=136..173

| 采样点 | 值 | 含义 |
|---|---|---|
| x=3..355（名称列整段） | `#FFFFFF` | 名称列**纯白**，没有灰底 |
| x=356 | `#EAD5A0` | 1px 竖线 |
| x=357..406 | `#FDF9E5` | 极浅暖色（R 比右侧少 2） |
| x=407..428 | `#FFF9E4` | 热力档 0 |
| x=429 | `#EAD5A0` | 1px 竖线 |
| x=430..501 | `#FFF9E4` | 热力档 0 |
| x=502 | `#EAD5A0` | 1px 竖线 |
| x=503..1246 | `#FFFFFF` | 右侧空白纯白 |

组头「后台进程 (105)」y=482..519 同样：CPU `#FFF9E4`、内存 `#FFF9E4`。

### 1.2 ⚠ 任务书「组头取聚合值」这个前提，被实测否掉了

`#FFF9E4` 是 `HeatColor(255.0)`，即**「无数据」哨兵**（`kFill[0]`，`thr[0]=255.0`）。
`0%` 是 `#FFF4C4`（`kFill[1]`，`thr[1]=0`）。**两者不是同一档。**

同列纵向对照（x=431，一个不落字形的位置）：

| 行 | 内存列 |
|---|---|
| 组头「应用 (11)」 y=136..173 | `#FFF9E4`（哨兵） |
| 数据行 y=174..481 | `#F9ECA8` / `#FFE486` / `#FBEAA9`（档 2/3） |
| 组头「后台进程 (105)」 y=482..519 | `#FFF9E4`（哨兵） |
| 数据行 y=520..792 | `#F9ECA8` / `#FFF4C4` 等（档 1/2） |

**两条反证：**

1. CPU 聚合即使恰好 0%，`HeatColor(0)` 命中 `thr[1]=0` → `#FFF4C4`（档 1），
   不会是 `#FFF9E4`（档 0）。**差一档，不是抗锯齿。**
2. 内存列的聚合值绝不可能是 0% —— 同组 11 个进程的内存之和只可能比单个进程更大。
   若按聚合着色，组头内存色必须**深于**同组任何数据行；实测却**最浅**。

**结论**：原版组头行的值列填的是**固定的「无数据」哨兵色 `#FFF9E4`**，
语义是「这一行不是进程，没有占比」。与「合计」「最大值」都无关。
任务书里「颜色取该组的聚合值」是推测，**实测不支持**，故不实现聚合版。

### 1.3 数据区的列间竖线 = `#EAD5A0`（我方完全没有这个元素）

y=136..792 全高，x=356 / 429 / 502 三处恒为 `#EAD5A0`；
而 **x=236（名称↔状态）没有**。

- 有线的三处 = CPU 左界、CPU/内存分界、内存右界 → 正好是**热力单元格的左/右框**
- 无线的 236 = 状态列不染色，没有热力框

即：热力块不是「留白隔开」，而是被 1px `#EAD5A0` 描边包住。
我方 `src/tmwindow_list.cpp:688` 注释「列间竖线：只在列头带有，数据区没有」——
**这句是错的**。数据区有，颜色 `#EAD5A0`。

UIFILE 双源：`gtc(TASKMANAGER, 2, 0, 3802)` = `#EAD5A0`，`S_duispec.md`
§「TASKMANAGER p2 s0」写明它是 `TmGroupHeaderName` / `TmFirstColumn` 的 bordercolor。

#### 1.3.1 精确几何（与列宽 73 定案后逐像素吻合）

| 格 | 列界 | 左描边 | 填色区 | 右描边 |
|---|---|---|---|---|
| CPU | `[356, 429)` 宽 73 | x=356 | x=357..428（**72px**） | x=429 |
| 内存 | `[429, 502)` 宽 73 | x=429 | x=430..501（**72px**） | x=502 |

→ 填色区 = `[cx+1, cx+cw-1)`，描边在 `cx` 与 `cx+cw-1`。
**关键**：热力块**铺满整格**，只是被左右各 1px 描边吃掉。
我方 `chRight = cx + cw - kCellPadRight(4)` 让右界内缩 4px，与原版差 3px。
（这解释了为什么我方热力块看起来「比列窄一截」。）

### 1.4 文字色实测（每格取最暗像素 + 前 5 高频色）

| 对象 | 取样框 | 实测 | 我方现值 |
|---|---|---|---|
| 数据行名称 × 3 行 | y=174/202/230 起 | **`#000000`**（最高频 27/54/27 次） | `#141414` |
| 选中行文字 | y=398..432（底 `#CDE8FF`） | **`#000000`**（26 次），最暗像素 (0,0,0) | `#141414` |
| 菜单/页签文字 | y=52..78 | **`#000000`**（282 次，最高频） | `#141414` |
| **组头文字** | y=150..165 | **`#1F59C3`（蓝）**，最暗像素就是它自己 | `kTextPrimary` 黑 |
| 列头文字 | y=96..130 | `#4C607A`（非本次任务，归 HH） | `#5A5A5A` |

**「最暗像素 = (0,0,0)」这条是关键**：抗锯齿只会让文字变浅，绝不会比字色更深。
若是 `#141414`，任何像素的通道都不可能低于 20 —— 实测有大量 (0,0,0)。

组头蓝字 `#1F59C3` 与 `U_colors_final.tsv:76` 的
`TASKMANAGER 16 0 TEXT = #1F59C3`（`atom(TmGroupHeaderName)`）逐字节一致。

### 1.5 组头行高 = **38px**（不是 28）

| | 起 | 止 | 高 |
|---|---|---|---|
| 组头「应用 (11)」 | y=136 | y=173 | **38** |
| 组头「后台进程 (105)」 | y=482 | y=519 | **38** |
| 数据行 | y=174 起，连续 | | 28 |

算术自洽：`482 − 173 − 1 = 308 = 28 × 11`（中间正好 11 个数据行）。
两个组头都是 38，不是偶发。

组头文字中心 y=157.5，行中心 y=154.5 → 文字偏下 3px，与 UIFILE
`TmGroupHeaderName padding rect(8,10,0,7)`（上 10 下 7 不对称）方向一致。

### 1.6 组头行没有展开三角

x=3..12 在 y=136..173 与 y=482..519 **全白（0 个非白像素）**。
组头文字从 x=13 开始。
我方 `PaintRows` 的组头分支里 `int triX = x0 - 12;` 处画了一个实心三角（快照 :938-953） —— 原版没有。

---

## 2. 已改（只改 theme.h + tmwindow_paint.cpp）

| 项 | 文件:行 | 旧值 | 新值 | 依据 |
|---|---|---|---|---|
| 组头行底色 | `include/theme.h:192` | `#F2F2F2` `RGB(242,242,242)` | `#FFFFFF` `RGB(255,255,255)` | 组头「应用 (11)」y=136..173 的 x=3..355 全白，非白像素 0 个 |
| 正文色 | `include/theme.h:239` | `#141414` `RGB(20,20,20)` | `#000000` `RGB(0,0,0)` | 3 行数据行名称最暗像素均 (0,0,0)，`#000000` 最高频（27/54/27 次） |
| 选中行文字色 | `include/theme.h:280` | `#141414` `RGB(20,20,20)` | `#000000` `RGB(0,0,0)` | 选中行 y=398..432（底 #CDE8FF）内最暗像素 (0,0,0)，`#000000` 26 次最高频 |
| 新增：组头值列热力色 | `include/theme.h:620` `GroupHeaderHeatColor()` | —（无此物） | `HeatColor(255.0)` = `#FFF9E4` | 两个组头的 CPU/内存列都恰为 `#FFF9E4`（哨兵档，不是 0% 的 `#FFF4C4`） |
| 新增：热力单元格描边 | `include/theme.h:648` `kLineHeatCell` | —（无此物） | `#EAD5A0` `RGB(0xEA,0xD5,0xA0)` | x=356/429/502 三处 y=136..792 全高恒为该色 |
| 新增：组头文字色 | `include/theme.h:665` `kGroupHeaderText` | —（无此物） | `#1F59C3` `RGB(0x1F,0x59,0xC3)` | 组头文字框 y=150..165 最高频 `#1F59C3`（61 次），最暗像素即该色 |
| 死代码说明 | `src/tmwindow_paint.cpp:100-131` | （无注释） | 加了 40 行说明（**函数未删**） | 见 §4 |

**未改（明确不做）：**
- `kLineHeader` `#828790`→`#A0A0A0` —— 归 HH，且已被 HH 改好（现 `theme.h:295` 已是 `RGB(0xA0,0xA0,0xA0)`）
- `kTextHeader` 列头文字 `#5A5A5A` vs 实测 `#4C607A` —— **不在任务书三项内**，只报告不动（见 §5）
- 组头行高 28→38 —— **不在任务书三项内**，且消费点在 `tmwindow_events.cpp`/`tmwindow.cpp`（不归我），只报告（见 §5）

---

## 3. 组头热力着色：**没做成，是文件边界问题**

组头行的绘制在 **`src/tmwindow_list.cpp:895-935`**（`TmWindow::PaintRows` 的
`if (row.kind == ViewRow::GroupHeaderRow)` 分支）。那个文件归 HH，我**一行没碰**。

我的准备：
- `include/theme.h` 已提供三个可直接调用的成品：
  `GroupHeaderHeatColor()`、`kLineHeatCell`、`kGroupHeaderText`
  —— 签名、语义、证据全写进注释，HH 直接 `using namespace theme` 就能用（该文件 :16 已有）。
- 精确交接单见下一节。

`GroupHeaderHeatColor()` 之所以包成**函数**而不是常量：将来若拿到「原版分组行
也走分桶函数」的硬证据，只改 theme.h 一处即可，不用再动绘制代码。

---

## 4. 交接单（精确到函数名 + 行号 + 要插入的代码）

> 目标文件：`src/tmwindow_list.cpp`（HH）
>
> **行号只是辅助，请按「锚点内容」定位** —— 这个文件在我工作期间被 HH 改了两次
> （23:44 画刷缓存重构、23:47 列宽改实测值），行号一直在漂（组头分支从
> :884→:895→:922）。每次我核对时都把它纠正到当时的真实位置，但你读到时可能又变了。
>
> 每个 H 项都给了**唯一可 grep 的锚点行**，按锚点找比按行号找可靠。
>
> 我最后一次核对（23:47 版本）确认：组头分支里只有一条统一的
> `FillSolidFast(hdc, clip, kBgGroupHeader);`（现 :929），
> 值列**没有**任何着色，文字传的是 `kTextPrimary`，实心三角仍在 ——
> **组头热力着色没被实现**，交接单仍然完整有效。

### H1 — 组头行值列热力着色 + 名称列白底（**核心**）

**位置**：`TmWindow::PaintRows` 的组头分支，当前 :922-962。

**H1-a · :929** 锚点 `FillSolidFast(hdc, clip, kBgGroupHeader);` —— 现有这一行**不用改**（`kBgGroupHeader` 已从 `#F2F2F2` 变 `#FFFFFF`）：

```cpp
FillSolidFast(hdc, clip, kBgGroupHeader);      // 现在是白底了
```

但 **:924-928 的注释已过期**，必须改掉（它还在说「#F2F2F2 是原版上屏值」）：

```cpp
            // ---- 分组标题行 ----
            // 【底】名称列与右侧空白区 = 纯白 #FFFFFF。
            // 依据：真原版 `docs/reference/original_win10.png` 的组头行
            // y=136..173，x=3..355 非白像素 0 个。theme.h 的 kBgGroupHeader
            // 已由 Agent II 从 #F2F2F2 改为 #FFFFFF，这里只引用、不重复定义。
            FillSolidFast(hdc, clip, kBgGroupHeader);
```

**H1-b（核心）· 锚点 `// 展开/折叠三角`（插在它之前）· 当前 :931 附近** 值列着色：

```cpp
            // ---- 值列热力色 ----
            // 【实测：不是聚合值，是固定哨兵色】
            // 两个组头（应用(11) y=136..173、后台进程(105) y=482..519）的
            // CPU 列与内存列都恰为 #FFF9E4 —— 那是 HeatColor() 的「无数据」哨兵档
            // （kFill[0]，thr[0]=255.0），**不是** 0% 对应的 #FFF4C4（kFill[1]）。
            // 两条反证：
            //   1) 聚合 CPU 即使为 0%，HeatColor(0) 也命中 #FFF4C4，差一档；
            //   2) 11 个进程的内存之和绝不可能是 0%，组头却是最浅档。
            // → 原版把组头行当「没有占比的非进程行」，填固定色。
            // 详见 docs/specs/II_colors.md §1.2。
            {
                const COLORREF ghHeat = GroupHeaderHeatColor();   // 已含证据注释
                const COLORREF ghLine = kLineHeatCell;            // #EAD5A0 1px 框
                // 只给可见的数值列（2=Cpu 3=Mem 4=Disk 5=Net 6=Gpu）上色。
                // 状态列(1)不染色 —— 原版 x=236 处没有 #EAD5A0 竖线，证明它不在框内。
                //
                // 【几何：铺满整格 + 左右各 1px 描边，不扣 kCellPadRight】
                // 实测（列宽已按原版 73 定案后逐像素对上）：
                //   CPU  格 [356,429)：356=#EAD5A0，357..428=底色(72px)，429=#EAD5A0
                //   内存 格 [429,502)：429=#EAD5A0，430..501=底色(72px)，502=#EAD5A0
                // 即 填色区 = [cx+1, cx+cw-1)，描边在 cx 与 cx+cw-1。
                // **不要**像进程行那样扣 kCellPadRight(4) —— 那会让描边比列界内缩 4px。
                for (int c = 2; c < kColCount; ++c) {
                    if (!LiveVisible(c)) continue;
                    const int cx  = x0 + colX[c];
                    const int cw  = LiveW(c);
                    if (cw < 3) continue;
                    const int cRight = cx + cw - 1;          // 右侧描边那一列
                    RECT cr{ cx + 1, clip.top, cRight, clip.bottom };
                    FillSolidFast(hdc, cr, ghHeat);
                    DrawVLineFast(hdc, cx,     clip.top, clip.bottom - 1, ghLine);
                    DrawVLineFast(hdc, cRight, clip.top, clip.bottom - 1, ghLine);
                }
            }
```

> `colX[]` 已在 :852-854 预计算好（每帧一次），`x0`(:843)、`clip`(:918-920)、
> `LiveVisible/LiveW`（匿名 namespace，:97/:103）、`FillSolidFast`(:582)、
> `DrawVLineFast`(:635) 全都在同一作用域内可直接用，**不需要新增任何 include 或成员**。
> `kColCount`（=7，定义在 :77）也在同一匿名 namespace 里。

**H1-c · :960-961** 锚点 `DrawTextClipped(hdc, gtext,` —— 组头文字色改成蓝色（现在传的是 `kTextPrimary`，会被我改成纯黑，
但原版这里是**蓝**）：

```cpp
            // 旧：DrawTextClipped(hdc, gtext, &tr, ..., kTextPrimary);
            // 新：组头文字是蓝色 #1F59C3（实测最高频 61 次，且最暗像素即该色），
            //     U_colors_final.tsv:76 `TASKMANAGER 16 0 TEXT` 双源吻合。
            DrawTextClipped(hdc, gtext, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT,
                            kGroupHeaderText);
```

**H1-d（可选，建议一并做）· 锚点 `int triX = x0 - 12;`（当前 :938-953）** 删掉组头行的实心三角。
实测：组头行 x=3..12 在 y=136..173 与 y=482..519 **全部为白（0 个非白像素）**，
文字从 x=13 才开始 —— 原版组头**没有**折叠三角。
（这是本次实测的**新发现**，不在任务书里；删了才更接近原版，但不删也不算错配色，
所以标「可选」。）

### H2 — 数据行热力块补 `#EAD5A0` 描边（**新元素，我方完全没有**）

**位置**：`TmWindow::PaintRows` 的进程行热力循环，**:1093-1103**（锚点 `int chRight = x0 + colX[hc.col] + cw`）。

现有代码（:1097-1101）：

```cpp
                    if (chRight > cx) {
                        RECT cr{ cx, clip.top, chRight, clip.bottom };
                        FillSolidFast(hdc, cr, HeatColor(hc.pct));
                    }
```

改成（**同时修掉右界少 4px 的偏差**）：

```cpp
                    // 【热力块右界：铺满整格，不扣 kCellPadRight】
                    // 实测列宽 73 定案后逐像素对上：
                    //   CPU  格 [356,429)：356=#EAD5A0，357..428=#FFF4C4(72px)，429=#EAD5A0
                    //   内存 格 [429,502)：429=#EAD5A0，430..501=#F9ECA8(72px)，502=#EAD5A0
                    // 原版热力块**铺满列宽**，只是被左右各 1px 的 #EAD5A0 描边吃掉。
                    // 旧代码 `- kCellPadRight`(4) 让右界内缩 4px，与原版差 3px，
                    // 且相邻列的描边会错开 —— 一并去掉。
                    int chRight = x0 + colX[hc.col] + cw;   // 全宽，不扣 pad
                    if (chRight - cx >= 3) {
                        RECT cr{ cx + 1, clip.top, chRight - 1, clip.bottom };
                        FillSolidFast(hdc, cr, HeatColor(hc.pct));
                        // 【1px 描边 #EAD5A0】实测 y=136..792 全高、x=356/429/502
                        // 三处恒为该色，而 x=236（名称↔状态）没有 —— 说明它是
                        // **热力单元格**的左右框，不是「列间竖线」（状态列不染色
                        // 所以没有框）。UIFILE 双源：gtc(TASKMANAGER,2,0,3802)。
                        // 旧注释「数据区没有竖线」是错的。
                        DrawVLineFast(hdc, cx,          clip.top, clip.bottom - 1, kLineHeatCell);
                        DrawVLineFast(hdc, chRight - 1, clip.top, clip.bottom - 1, kLineHeatCell);
                    }
```

> **注意 `cx` 不要加 1**：`cx = x0 + colX[hc.col]` 就是列左界本身，
> 描边画在 `cx`，填色从 `cx+1` 起 —— 这三行数字都必须与列界 `[cx, cx+cw)` 对齐，
> 否则会与 `PaintColumnHeader` 的列分隔线错开 1px。
>
> **改了这一处的实际影响**：热力块会向右长 3~4px，观感更接近原版
> 「整格被染色」；但也意味着数值文字（右对齐，右界是 `colRight - kCellPadRight`）
> 与色块的右边距会从 4px 变成... 仍然是 4px（文字右界不变），只是色块铺满了。
> 这正是原版的样子（实测 x=428 仍是 `#FFF4C4`，而文字右界在 425）。

### H3 — 不必改的

- `:16` 已有 `using namespace theme;` → 新常量/函数直接可见
- **不要**改 `kBgGroupHeader` 的字面值（它已在 theme.h 改好，重复定义会两套真值）
- **不要**自己算聚合值 —— 见 §1.2，实测证明原版不是聚合着色

---

## 5. 卡片区删除的影响分析（只报告，不改）

### 5.1 `PaintCards` 现在保证不会被调用（两道防护，都已就位）

| 位置 | 代码 | 效果 |
|---|---|---|
| `src/tmwindow.cpp:220-221` | `rcCards_ = {..., cardsTop, ..., cardsTop}` | `bottom == top`，零高 |
| `src/tmwindow_events.cpp:40` | `if (rcCards_.bottom > rcCards_.top) PaintCards(memDC_);` | 条件恒假，**进不去** |
| `src/tmwindow_paint.cpp:135` | `if (rcCards_.bottom <= rcCards_.top) return;` | 第二道保险 |

### 5.2 `PaintCards` 内部**没有**任何状态初始化 —— 删掉不会连锁破坏

逐行核过（`src/tmwindow_paint.cpp:132-240`）：

- 只**读**：`snap_`、`rcCards_`、`hist_`、`perfSnap_`（经 `SampleCardMetrics()`）
- 唯一写操作全是 GDI（`FillSolid`/`DrawFrame`/`DrawTextClipped`/`DrawMiniChart`）
- **不修改任何成员**、不清缓存、不推进索引、不 set 任何 flag

→ **删函数本身是安全的**，只是本任务书要求保留，所以留着。

### 5.3 ⚠ 但它留下了两条「算了没人读」的链，**建议主线程处理**

| # | 死链 | 证据 | 代价 |
|---|---|---|---|
| 1 | `TmWindow::hist_`（`ChartHistory`）**变成只写不读** | 唯一读者是 `PaintCards:197` 的 `DrawMiniChart(..., hist_.count, hist_.head)`。`PaintCards` 死后 `hist_` 再无消费者（grep `hist_` 在 `tmwindow*.cpp` 里只剩 `:392` 的 `hist_.Push`） | `src/tmwindow.cpp:381-392` 每秒算一次全进程 CPU 合计 + `memUsed` + 调一次 `SampleCardMetrics()`，**全部白算**（不是每帧，是每秒，可接受但不干净） |
| 2 | `SampleCardMetrics()` 在进程页变成只服务死链 | 两个调用点：`tmwindow_paint.cpp:136`（死）与 `tmwindow.cpp:391`（只为喂 `hist_`） | `SampleCardMetrics()` 成了间接死代码；`CardMetrics` 结构（`tmwindow.h:80-84`）唯一消费者也是它 |

**建议（不归我，写在这里给主线程）**：
- 若确定不要卡片：连 `hist_.Push` + `SampleCardMetrics()` + `DrawMiniChart()` + `PaintCards()` +
  `CardMetrics` + 5 个 `theme::kCard*` 常量 + 3 个 `fontCard*` 一起删。
  **这是一整条闭环死链，删一半会留下更难看的中途状态。**
- 若想保留「将来在性能页用迷你曲线」：至少把 `tmwindow.cpp:392` 的
  `hist_.Push` 挪到 `TabId::Performance` 分支里，避免进程页白算。

### 5.4 一个真实的副作用：`simpleView_` 变成 no-op

`src/tmwindow_menus.cpp:1480` 仍会翻转 `simpleView_`，但 `BuildLayout` 现在
（HH 改过之后）**完全不读它**了 → 点「简略信息(D)」只触发一次无效重排。
菜单项还在、勾选状态还会变、画面纹丝不动。这是**已知且已记录**的行为
（`tmwindow.cpp:213-217` 有注释说明），不算新问题，但用户会认为「这个菜单坏了」。
归按 `tmwindow_menus.cpp` 的人决定：隐藏菜单项 / 或让它折叠别的区域。

---

## 6. 本次实测发现、但**不在任务书范围**的差异（只报告）

| # | 项 | 我方 | 原版实测 | 归谁 |
|---|---|---|---|---|
| D1 | **组头行高** | 28（`kGroupRowHeight = kRowHeight`） | **38** | `theme.h` 是我的，但改动波及 `tmwindow_events.cpp`/`tmwindow.cpp`/`tmwindow_list.cpp` 的命中测试与滚动，**跨文件，未改** |
| D2 | 组头行有折叠三角 | x0-12 处画 8×5 实心三角 | x=3..12 **全白**，没有三角 | `tmwindow_list.cpp`（HH）→ 交接单 H1-d |
| D3 | 热力块右界少 4px + 缺 `#EAD5A0` 描边 | `chRight - kCellPadRight`，无描边 | 铺满整格 + 左右各 1px `#EAD5A0`（§1.3.1） | `tmwindow_list.cpp`（HH）→ 交接单 H2 |
| D4 | 列头文字色 | `#5A5A5A` | `#4C607A`（蓝灰，最暗像素即该色；与 `ItemsView::Header p1 s0 TEXTCOLOR` 双源一致） | `theme.h:267` 的 `kTextHeader`，**属「配色对齐」但任务书三项里没有它**，未改 |
| D5 | 组头行文字偏下 3px | `DT_VCENTER` 居中 | 文字中心比行中心低 3px（= UIFILE padding 上10/下7） | 与 D1 一起做才有意义 |

**D1 的补充说明**：如果只把 `kGroupRowHeight` 改成 38，`PaintRows`/`ContentHeight`
/`HitTestRow` 都走 `RowHeightFor()`，**会自动跟着对**（这正是 `RowHeightFor` 存在的理由）。
真正要改的是 `theme.h` 一行 + 确认没有别处写死 28。风险比想象小，但仍跨文件，
按任务书「不许越界」的要求我只报告。

---

## 7. 待编译

**未编译**（按禁令）。改动本身的编译安全性自查：

| 改动 | 风险 | 说明 |
|---|---|---|
| `kBgGroupHeader` 值变 | 无 | 同类型 `COLORREF`，只换字面值 |
| `kTextPrimary` / `kTextSelected` 值变 | 无 | 同上 |
| 新增 `GroupHeaderHeatColor()` | 无 | `inline` 函数，`HeatColor()` 在同文件前文已定义 |
| 新增 `kLineHeatCell` / `kGroupHeaderText` | 无 | `constexpr COLORREF`，全项目 grep 无同名（已核对 `.cpp/.h/.rc`） |
| `tmwindow_paint.cpp` 加注释 | 无 | 只加 `//` 注释块，未改任何语句 |

唯一需要编译器确认的是：`GroupHeaderHeatColor()` 里调 `HeatColor(255.0)`，
而 `HeatColor` 定义在第 541 行、本函数在第 617 行 → **同文件内前向已定义，无问题**。

建议编译后用 `--shot=` 离屏渲染一帧，检查：
1. 组头行名称列是否变白（不再是灰条）
2. 三处文字是否变纯黑
3. 若 HH 已落地 H1/H2：组头值列是否 `#FFF9E4` + `#EAD5A0` 框

---

## 8. 本轮删除的记录（诚实清单）

无。本轮**只增不减**：
- `theme.h`：3 个值改字面量 + 3 个新符号（2 常量 + 1 函数）+ 注释
- `tmwindow_paint.cpp`：40 行注释（`PaintCards` 保留未删）

没有任何函数/常量/成员被删除，也不存在「改了但没写在报告里」的项。
