# EE — 列可见性（隐藏列 / 选择列 / 列名清单）接线

状态：已完成

## 完成度

| 步骤 | 状态 | 说明 |
|---|---|---|
| 1. 列可见性维度（`include/tmwindow.h`） | ✅ 完成 | 新增 `bool colVisible_[kSortColCount]`（默认全 true）、`IsColumnVisible` / `SetColumnVisible` / `ResetColumnVisibility` / `LoadColumnVisibility` / `SaveColumnVisibility`。名称列(0) 硬约束恒可见。独立注册表值名持久化。 |
| 2. 命令 case（`src/tmwindow_menus.cpp`） | ✅ 完成 | `CMD_COL_TOGGLE_BASE..CMD_COL_TOGGLE_END` 区间分支（**列名清单走这条，可达**）+ `CMD_HIDE_COLUMN` + `CMD_SELECT_COLUMNS` 两个分支（实现真实，但对应菜单 `BuildHeaderContext` 仍零调用点，所以当前不可达）。 |
| 3. 调用点（`src/tmwindow_events.cpp`） | ✅ 完成 | `WM_RBUTTONUP` 里先判「进程页 + 落在 `rcColHeader_` 内」→ 记 `headerContextCol_` → `BuildColumnHeaderContext` → `TrackPopupMenu` → `DestroyMenu` → `return 0`。 |
| 4. 绘制侧尊重可见性（`src/tmwindow_list.cpp`） | ✅ 完成 | 见下方「关键改动」。 |

## 已改文件与关键改动

### `include/tmwindow.h`
- `bool colVisible_[kSortColCount] = {true×7}` —— 与 `colWidths_` / `kCols[]` 索引同源
  （0=名称 1=状态 2=CPU 3=内存 4=磁盘 5=网络 6=GPU）。
- `int headerContextCol_ = -1` —— 弹菜单前记住「鼠标在哪一列」，供无参的菜单命令读取。
- `static int ColumnCount()` / `static const wchar_t* ColumnTitle(int)` —— 列名唯一真值源，
  避免调用点另抄一份列名字符串。
- 声明 `LoadColumnVisibility` / `SaveColumnVisibility` / `ResetColumnVisibility` /
  `IsColumnVisible` / `SetColumnVisible`。

### `src/tmwindow_list.cpp`（核心：几何与绘制）
- 新增 `const bool* g_liveVisible`，与既有 `g_liveWidths` 同一「文件级直读」约定，
  每个绘制/命中函数开头赋值。
- 新增 `LiveVisible(int)`（名称列恒 true）与 **`VisW(int)`**（隐藏列返回 0）。
- **`NameColWidth()` 累加 `VisW(i)` 而不是 `LiveW(i)`** —— 隐藏列的宽度不再从名称列的
  弹性空间里被扣掉，名称列会吃掉腾出来的空间。
- **`ColX()` 累加 `VisW(i)`** —— 隐藏列不占位，后面的列自动左移填位。
- `ColW()` 走 `VisW`（隐藏列为 0）。
- `PaintColumnHeader()`：`if (!LiveVisible(i)) continue;` —— 不画列头、不画列间竖线；
  排序箭头在 `sortCol_` 不可见时不画。
- `PaintRows()`：
  - 热力色块 `if (!LiveVisible(hc.col)) continue;`
  - 状态/CPU/内存/磁盘/网络/GPU 六个单元格各自 `if (LiveVisible(n))`。**这一条是必须的**：
    `colX[]` 里隐藏列宽为 0、与后一列同起点，但 `LiveW()` 仍是真实宽度 ——
    不跳过就会把文字画进后一列格子里（两列文字叠在一起）。
- `HitTestColumn()`：走 `ColW`；右侧留白回落到**最后一个可见列**（不再无脑落到第 6 列）。
- `HitTestColResize()`：隐藏列不占位、不可拖（不再在没有内容的位置出现双向箭头）。
- `LoadColumnWidths()` 开头调用 `LoadColumnVisibility()`（`Create()` 已调用
  `LoadColumnWidths`，所以启动即恢复；不需要改 `tmwindow.cpp`）。
  `LoadColumnVisibility()` 末尾校正两件事：`colVisible_[0] = true`；若 `sortCol_`
  指向隐藏列则回落到名称列。
- 持久化用**独立值名** `TaskManagerPro_ColumnVisible`（7 字节 REG_BINARY，
  同 `HKCU\...\CurrentVersion\TaskManager` 键），**没有改动** `TaskManagerPro_ColumnWidths`
  的格式与长度，老数据照常读取。

### `src/tmwindow_menus.cpp`
- `OnMenuCommand` 开头（`CMD_DEF_TAB_BASE` 分支之后、`switch` 之前）新增
  `CMD_COL_TOGGLE_BASE..CMD_COL_TOGGLE_END` 区间分支：`idx = id - BASE`，
  `idx <= 0 || idx >= kSortColCount` 直接返回（名称列/越界），否则
  `SetColumnVisible(idx, !IsColumnVisible(idx))`。
- `switch` 末尾新增：
  - `case CMD_HIDE_COLUMN:` 读 `headerContextCol_`，`col > 0` 时 `SetColumnVisible(col, false)`。
  - `case CMD_SELECT_COLUMNS:` 调 `ResetColumnVisibility()`（全部恢复可见）。

### `src/tmwindow_events.cpp`
- `WM_RBUTTONUP` 最前面新增列头分支（在详细信息页分支和行右键之前）：
  条件 `tabs_.Current() == TabId::Processes && y ∈ [rcColHeader_.top, bottom) &&
  x ∈ [rcColHeader_.left, right)`。
  - `headerContextCol_ = HitTestColumn(x, y);`
  - 组装 `titles[]`（**索引 0 传 nullptr** —— `BuildColumnHeaderContext` 会跳过 null 项，
    于是名称列不进菜单，不会出现「点了没反应」的死项；其余列 ID 映射不变）
    与 `visible[]`。
  - `BuildColumnHeaderContext(titles, visible, n, MenuBuilder::ResourceValueMode(), headerContextCol_)`
    → `TrackPopupMenu` → `DestroyMenu` → `return 0`。
- `WM_MOUSEMOVE` 列宽拖拽：原先 `colDragCol_ == 0` 时硬写 `colDragCol_ = 1`，
  改为取**第一个可见的固定列**；只剩名称列时放弃这次拖拽（防止拖一个看不见的列）。

## 用户操作路径（怎么验证）

1. 启动后切到「进程」页。
2. 在列头带（名称/状态/CPU/内存/磁盘/网络/GPU 那一行）上右键 → 弹出列名清单，
   **6 项**（状态/CPU/内存/磁盘/网络/GPU），当前可见的带 ✓，末尾分隔线后是「资源值 ▸」。
3. 点「CPU」→ **CPU 列从列头和数据区同时消失，内存/磁盘/网络/GPU 整列左移填补空位，
   名称列同时变宽**（因为它吃掉了腾出的 82px）。
4. 再次右键 → 「CPU」的 ✓ 消失；点它 → CPU 列回到原位（在「内存」左边）。
5. 把 6 个数值列全隐藏 → 只剩名称列铺满整个列表区，不崩、不错位。
6. 关闭程序再启动 → 隐藏状态保持（`HKCU\...\TaskManager\TaskManagerPro_ColumnVisible`）。
   若上次隐藏了排序列（默认是内存），排序回落到名称列升序，列头箭头画在名称列上。
7. 名称列：菜单里**根本没有**这一项，`SetColumnVisible(0, false)` 也返回 false
   （硬约束，两层都挡）。
8. 隐藏排序列后点列头排序：只会在可见列上生效，右侧留白也回落到最后一个可见列。

## 未完成 / 风险

- **`CMD_SELECT_COLUMNS` 不是原版的「选择列」对话框**。原版弹的是一个一列一个复选框的
  列表对话框（MUI STR 2027/2028）。本项目没有这个对话框，我把它实现成
  **「恢复所有列可见」**（语义一致、用户点完立刻看得到效果），而不是置灰
  或假装弹了对话框。这是一个明确的降级，不是完整实现。
- **`BuildHeaderContext()`（MENU 30201 两项简化版）仍然零调用点**。真实列头右键弹的是
  `BuildColumnHeaderContext()`（列名清单）。因此：
  - `CMD_COL_TOGGLE_BASE..END` 这一路是**真正可达**的（就是列名清单里的每一项）；
  - `CMD_SELECT_COLUMNS` 只能在 `BuildHeaderContext` 被接上后从菜单点到，**当前不可达**
    （但它和 `CMD_HIDE_COLUMN` 都有真实实现，不是死代码）；
  - `CMD_HIDE_COLUMN` 同理：**实现是真的、当前不可达**（该 ID 只出现在
    `BuildHeaderContext` 里，而那个函数还没有调用点）。`headerContextCol_` 在
    `WM_RBUTTONUP` 里被写入，唯一消费者就是这一分支 —— 所以严格说它现在是
    「写了一个将来才会被读的值」。功能上不重要：列名清单里取消勾选就是隐藏列，
    不需要额外的「隐藏列」项。
  按任务约束没有改 `src/menus.cpp`（菜单形状归那份文件）。
- **`hoveredColumn` 参数在 `src/menus.cpp` 里仍然 `(void)` 掉**。列名清单里每一项
  本身就能切换可见性，再叠一个「隐藏这一列」是冗余项；`headerContextCol_` 的消费者
  是 `CMD_HIDE_COLUMN`（见上条）。
- **列头右键菜单里的「资源值 ▸」子菜单是共享的 `MenuBuilder::ResourceValueMode()`**，
  与行右键那套同源。点它改的是 `resourceValueMode_`，但**进程页的数值列目前不读
  `resourceValueMode_`**（`FmtMemText` 等是固定格式）—— 这是既有问题，不在本任务范围，
  本次没有让它变好也没有让它变坏。原来这个子菜单只能从行右键点到，现在列头右键
  也能点到，暴露面变大了一点。
- **DPI**：所有几何都按现有像素值缩放（沿用既有做法），隐藏列没有引入新的像素常量，
  所以 DPI 行为与改动前一致。
- 没有运行、没有编译、没有截图（按任务禁令）。

## 待编译

四个文件均未编译过。改动涉及：
- `include/tmwindow.h`：新增成员 + 6 个方法声明（2 个 static）。
- `src/tmwindow_list.cpp`：`g_liveVisible` / `LiveVisible` / `VisW` 定义在匿名命名空间，
  `TmWindow::` 的新方法定义在匿名命名空间**之后**（`kRegKey` 已在此前定义，
  `kCols`/`kColCount` 在匿名命名空间内、同文件可见）。
- `src/tmwindow_menus.cpp`：只用了 `IsColumnVisible` / `SetColumnVisible` /
  `ResetColumnVisibility` / `headerContextCol_`，均在 `tmwindow.h` 中声明。
- `src/tmwindow_events.cpp`：用了 `TmWindow::ColumnCount()` / `TmWindow::ColumnTitle()`、
  `kSortColCount`、`MenuBuilder::BuildColumnHeaderContext`（声明在已包含的 `menus.h`）；
  `std::min` 来自已包含的 `<algorithm>`，且 `build.bat` 带 `/DNOMINMAX`，写法安全。

编译时重点看：
1. `ColumnCount()` / `ColumnTitle()` 声明在 `private` 段。它们只在
   `TmWindow::WndProc`（`WM_RBUTTONUP` 分支，是成员函数）里被调用，所以 `private` 合法；
   若主线程将来从别处（非成员函数）调用，需要把它们挪到 `public`。
2. `VisW` 在 `NameColWidth` / `ColX` / `ColW` 之前定义 —— 已确认顺序正确。
3. `TmWindow::ColumnTitle` 返回 `kCols[idx].title`（`const wchar_t*`），
   `kCols` 是 `const ColDef[]`，`title` 类型是 `const wchar_t*` —— 匹配。
4. `DRAG` 改动里 `ReleaseCapture()` 已在同文件多处使用，无新增依赖。

## 自检（任务要求的四条）

1. **`colVisible_` 被读了吗？在哪读？**
   被读了 **4 处**：
   - `tmwindow_list.cpp` 的 `LiveVisible()` / `IsColumnVisible()` / `LoadColumnVisibility()` /
     `SetColumnVisible()` / `ResetColumnVisibility()`；
   - 几何侧经 `VisW()` 间接读，而 `VisW` 被 `NameColWidth` / `ColX` / `ColW` 调用 ——
     也就是说**列宽、列位置、宽度、命中测试全部经过它**；
   - 绘制侧 `PaintColumnHeader` / `PaintRows` 的 `LiveVisible(i)` 判断（6 个单元格 + 热力色块）；
   - `tmwindow_events.cpp` 构造菜单的 `visible[]`，以及 `WM_MOUSEMOVE` 拖拽时选下一个可见列。
   没有任何一处是「只写不读」。
2. **用户操作后能看到可见变化吗？**
   能。隐藏列宽度按 0 计入 `NameColWidth`/`ColX`，所以后续列左移、名称列变宽；
   列头与数据区同时消失（两处都走同一套 `ColW`/`VisW`）。
3. **名称列能被隐藏吗？**
   不能，**两层拦截**：菜单里索引 0 传 `nullptr` 根本不生成该项（`BuildColumnHeaderContext`
   跳过 null）；即使命令 id 直接喂到 `OnMenuCommand`，`idx <= 0` 直接返回，
   `SetColumnVisible(0, ...)` 也返回 false；`LoadColumnVisibility` 还会把
   `colVisible_[0]` 强制改回 true。
4. **隐藏最后一列会怎样？**
   只能隐藏到「只剩名称列」：名称列硬约束不可隐藏，所以不会出现零列；
   此时 `NameColWidth` 的 `fixed` 为 0，名称列吃满整个列表区（仍保留下限 180px）。
   `HitTestColumn` 的右侧留白回落循环也仍能返回名称列；`HitTestColResize` 不会命中任何边界
   （所以拖不出双向箭头）；`WM_MOUSEMOVE` 里拖名称列右边界会因找不到下一个可见列
   而主动放弃这次拖拽。
