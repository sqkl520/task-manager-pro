# 最终裁决 —— 主线程独立复核后的结论

> 本文件由主线程（调度方）在收到全部 14 份 agent 报告后，对**存在冲突的关键结论**做独立复核。
> 复核手段：只用 `docs/evidence/tm/png.js`（自写 PNG 解码器）逐像素读图 + 读源码 + 读二进制。未运行任何 exe，未编译。

---

## 一、最重要的发现：`truth1.png` 不是原版截图

这件事决定了本轮一半改动的合法性，必须放在最前面。

### 1.1 我之前错在哪

我一直把 `shots/truth1.png`（以及 `truth2.png`）当成"用户最早提供的真机原版截图"，是**唯一的 A 级像素证据源**。E / F / L 三份执行报告的绝大部分改动都是"按 truth1 逐像素实测"做的。如果 truth1 不是原版，这些改动全部失去依据。

### 1.2 我独立复核到的硬证据

**证据 1：标题栏与 `p0_processes.png` 逐字节完全相同**

```
titlebar y=0..36 : 118800 字节，differing=0，maxDelta=0
menubar  y=36..48:  39600 字节，differing=0，maxDelta=0
```

而 `shots/p0_processes.png` 是我方 `--shot` 离屏产物（`main.cpp` 的 `--shot=` 路径）。

**证据 2：标题栏里没有任何标题文字**

```
truth1 标题栏 x=10..w-6, y=2..35，亮度<160 的像素：仅存在于
  x=1093（右边框）、x=1018/973（右侧三个caption按钮）、x=84 一带
  x=30..420 区间：最暗亮度 = 169.26 —— 有文字的话最暗会到 20~40 级
```

原版任务管理器标题栏必然有 `任务管理器` 五个字。我方 `--shot` 不画非客户区，DWM 画出的标题栏在我们 `SW_HIDE` 的窗口上也不会有标题文字。**没有标题文字 = 不是真窗口的屏幕截图。**

**证据 3：窗口图标是 `IDI_APPLICATION`**

`truth1` 标题栏图标（x=17..33, y=10..26）逐行形状与 `p0_processes.png` 同一区域**完全相同**（我逐行对比过 17 行，包括第 5-6 行 `BBBBB` 的渐变过渡位置）。我方 `src/tmwindow.cpp:116-117`：

```cpp
wc.hIcon   = LoadIconW(nullptr, IDI_APPLICATION);
wc.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
```

原版 `taskmgr.exe` 自带图标是"带白色曲线图的显示器"，与 `IDI_APPLICATION`（带蓝绿渐变的窗口图形）完全不同。

**证据 4：标签栏只有 5 个页签**

```
truth1 标签栏竖分隔线（#C8C8C8 连续列）：x = 8, 57/58, 107/108, 157/158, 233/234, 283
⇒ 5 个页签，宽度 49 / 49 / 49 / 75 / 49
```

原版 Windows 10 任务管理器是 **7 个页签**（进程 / 性能 / 应用历史记录 / 启动 / 用户 / 详细信息 / 服务）。`m3.png` 上的 7 个页签被 I agent 和我都判定为"我方旧版"，但"7 个"这个事实本身是对的 —— **错的是 m3 那张图的来源，不是页签数量**。

我去渲染了 truth1 的页签文字，字形确实是 `进程 / 性能 / 用户 / 详细信息 / 服务` —— **只有 5 个，缺了"应用历史记录"和"启动"**。

**证据 5：底部有我方独有的状态栏格式串**

`truth1` 底部 y=670..681 有文字，渲染出来是：

```
进程数: 562      CPU 95%      内存 22.5/31.9 GB (70%)
```

而 `src/tmwindow_list.cpp:912` 的格式串是：

```cpp
swprintf_s(text, L"进程数: %u      CPU %.0f%%      内存 %.1f/%.1f GB (%.0f%%)", ...);
```

**逐字符吻合**。MUI 资源里根本没有"进程数"这个词（I 和之前的 09 号 agent 都查过）。

### 1.3 结论

`truth1.png` / `truth2.png` 是**我方自己某一版程序的全窗口截图**（带 DWM 标题栏/边框），不是原版任务管理器。

它们是 11-14 13:45 / 14:02 生成的，而我方 `p0_*.png` 是 11-14 03:50，`m3.png` 是 11-14 04:25。时间顺序上，truth1 在 p0 之后 10 小时 —— 完全符合"我方程序迭代后重新截图"的模式。

### 1.4 这与 I agent 的结论的差异

I agent 把 truth1 定为"B 级：性能页客户区仍可信，标题栏/边框不可信"。**我的复核比它更严：既然标题栏、图标、页签数、底部状态栏全是我方的，那客户区也没有理由相信是原版。** 它是同一张我方截图的其余部分。

I agent 有一点是对的且很重要：truth1 里的数据（Xeon 服务器级 CPU、31.9 GB、562 进程）确实是本机真实数据 —— 但这只证明它是在这台机器上跑的，**不证明它是原版程序**。我方程序在这台机器上跑，数据当然也是本机的。

---

## 二、这条结论的后果：哪些改动失去依据

### 2.1 必须回滚的（依据只有 truth1）

| Agent | 改动 | 为什么失去依据 |
|---|---|---|
| **E** | 全部按"truth1 几何标定"改的坐标：`rc.left≙x234`、`rc.top≙y89`、网格 `rc.top+56`、网格左沿 `rc.left+8`、右沿 `rc.right-1`、10 条网格线、侧栏迷你图 61px 宽、卡片步进 61/68 | truth1 是我方截图，这些"实测"测的是我方旧版布局 |
| **F** | `tmwindow_toolbar.cpp` 全部几何（1px #DDDDDD 顶线、按钮高 23、文字内缩 23/30/11、图标中心 15）、`tabs.cpp` 的 truth1 逐像素数据 | 同上 |
| **L** | 四条主色"`#8B12AE`/`#4DA60C`/`#A74F01`/`#117DBB` 全部证实正确"（依据是 truth1 侧栏）| 侧栏是我方画的 |
| **A** | 选中行 `#CDE8FF` 的回滚建议（依据 `crops/svc_selrow_4x.png`，源图不可考） | 该裁切图属 I agent 定的 E 级 |

### 2.2 依然成立的（依据是 taskmgr.exe 二进制，不是图）

这部分**不受影响**，是真正的硬证据：

| Agent | 结论 | 依据 |
|---|---|---|
| **G** | 热力图 9 档色表 + 阈值 9/19/39/59/79/99 + 着色函数 RVA 0x74B04 + 255.0 哨兵 | `.data` RVA `0x1370F8` 逐字节 dump，且与 `_CONTEXT.md` 独立实测的 `#FFF4C4`/`#F9ECA8` 吻合 |
| **G** | 进程页列池 56 字节记录、`strId = colId + 1` | `.rdata` 静态表 |
| **K** | 加速键是 MUI 资源 `RT_ACCELERATOR id=26100` 的 4 条，不是 `CreateAcceleratorTableW` | MUI 文件偏移 36124，32 字节逐字节 dump；IAT 表证实无 `CreateAcceleratorTableW` |
| **K** | Esc→`PostMessage(WM_CLOSE)`、Shift+Esc→`ShowWindow(SW_MINIMIZE)`、F5→39100 带 100ms 去抖 | 反汇编 + 命令分发 switch |
| **D** | 16 个 RT_MENU 资源的项序/分隔线/置灰 | `docs/evidence/tm/menus_dump.txt` 权威原文 |
| **O** | MUI 全量字符串与格式串 | MUI 二进制 |
| **P** | `perfdata.cpp` 的 `SYS_MEM_LIST` 用 ULONG 读实际是 SIZE_T（取到 0 / 错位 346 倍） | 本机实测 class 80 返回 176 字节 = 22×8，并用 PDH 交叉验证了每个字段 |
| **N** | `CMD_COL_TOGGLE_BASE` +0..47 与 6 组命令 ID 撞车 | 纯算术，不需要外部证据 |
| **M** | P0 八条假功能 | 源码自证 |
| **B / C** | 所有标 C 级（代码自证）的条目 | 源码自证 |

---

## 三、必须立刻处理的三条（不分证据等级，因为都是会出事）

### 3.1 `CMD_COL_TOGGLE_BASE` ID 撞车（N 发现）

`include/menus.h:72` = `40132`，`src/menus.cpp:570` 用 `BASE + i`（i 到 47）。撞上：

| 列索引 | 算出 ID | 撞上 | 后果 |
|---|---|---|---|
| 8..11 | 40140..40143 | `CMD_SVC_START/STOP/RESTART/OPEN_SCM` | 点列头 → 启动服务 |
| 18..23 | 40150..40155 | `CMD_USR_*` 6 个 | 点列头 → 用户操作 |
| 38..46 | 40170..40178 | `CMD_CHART_*` 9 个含 `CMD_COPY` | 点列头 → 切性能页 |

现在因为 `BuildColumnHeaderContext` 零调用点**还没爆**，F 一接线就炸。**必须先修。**

### 3.2 `perfdata.cpp` 的 `SYS_MEM_LIST` 布局错误（P 发现）

```cpp
// 现状（错）：全部 ULONG
struct SYS_MEM_LIST { ULONG ZeroPageCount; ULONG FreePageCount; ... };
```

实际 class 80 返回 22 个 `SIZE_T`（8 字节）。后果：
- `freeBytes` 读到 `ZeroPageCount` 高 4 字节 = **永远 0**
- `modifiedBytes` 读到 `FreePageCount` = 3,239 MB（把空闲当已修改）
- `standbyBytes` 错位一半 = 28.4 MB（真值 9,834 MB，**差 346 倍**）

内存组合条的"备用"段几乎不可见 —— 这可能是内存页看起来发白的隐藏原因。**P 用 PDH 交叉验证过每个字段，证据很硬。**

### 3.3 三个契约未交付（N 发现，会让 F 一接线就编译失败）

1. `PagesView::OnHover` / `ScrollBy` 未声明未定义 → F 调用即 C2039
2. `BuildPerfSidebarContext` 是 2 参无默认值（契约要求 0 参）→ C2660
3. `kDetailHeaderTop` / `DetailContentHeight` 在 `include/tmwindow.h` 里不存在

---

## 四、我给用户的建议

**本轮 14 个 agent 的产出分两类，价值差别很大：**

**A 类 —— 直接可用，立即执行（约 60%）**
所有来自 `taskmgr.exe` 二进制、`docs/evidence/tm/menus_dump.txt`、MUI、以及代码自证（C 级）的条目。包括 G 的热力图、K 的加速键、D 的菜单结构、O 的文案、M 的 P0 假功能、N 的 ID 撞车、P 的 `SYS_MEM_LIST`、B/C 的所有 C 级条目。这些不依赖任何截图。

**B 类 —— 依据已失效，需要回滚或搁置（约 40%）**
所有"按 truth1/truth2/裁切图逐像素实测"的几何与配色改动。E 的整个几何标定、F 的 `tmwindow_toolbar.cpp` 全部几何、A 的选中行色、L 的四主色验证。

**唯一能让这些问题定案的办法：一张真正来自原版 Windows 10 任务管理器的截图。**

但这有个矛盾：用户明确禁止在原机上启动 `taskmgr.exe`（会假死）。所以：

1. 可以从**另一台机器**（虚拟机、别人的电脑、网上找的 Win10 19041 截图）拿一张原版截图，放进 `shots/` 并改名标明来源，作为新的 A 级基准；
2. 或者接受"几何部分按 Win10 通用设计规范实现"，并在代码注释里如实写明"无原版像素证据，按规范推定"。

**在拿到真原版图之前，B 类改动应该全部回滚到我方当前已验收的状态，不要用一个假的基准去"修正"代码。**
