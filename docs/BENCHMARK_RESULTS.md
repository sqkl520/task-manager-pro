# 里程碑验证结果

测量环境：Windows 10 Pro 22H2 22H2 / Xeon 服务器级 CPU (10C/20T @3.0GHz) / 32GB DDR3-1866
测量时系统负载：约 490 进程 / 8,450 线程 / 188,000 句柄 / CPU 40–90%（另有 java Ghidra 任务在跑）

---

## 里程碑 1：采集层

```
build\bench_snapshot.exe 20
```

```
System: 490 processes, 8456 threads, 188348 handles
Buffer: 1235.0 KB   First collection: 24.81 ms

--- NtQuerySystemInformation(5) full snapshot, 20 samples ---
  avg   9.13 ms   p50   9.46   p95  11.94   max  11.94  (n=20)

--- Budget check (1 Hz refresh) ---
  this collector         :     9.13 ms  =   0.9% of 1000ms
  [BANNED] \Thread(*)*   :   947.00 ms  =  94.7%
  [BANNED] WMI Win32_Thread: 3432.00 ms = 343.2%
  speedup vs \Thread(*): 104x

=== VERDICT ===
  PASS
```

**结论**：全量快照 9.13 ms，仅占 1 Hz 刷新预算的 0.9%。相对被禁用的 `\Thread(*)` 链路快 **104 倍**。

CPU% 差分计算验证正确——`java.exe 44.29%`、`shapezio.exe 6.56%` 与外部观测一致。

---

## 里程碑 2：渲染层

```
build\bench_ui.exe 10000
```

用 10000 行假数据灌入，测量 300 帧连续滚动。

### 优化过程（每一轮都是实测驱动）

| 版本 | 做法 | avg | p95 | 判定 |
|---|---|---|---|---|
| v1 | `std::wstring` 返回 + `DrawTextW` | **45.70 ms** | 73.00 | ❌ FAIL |
| v2 | 栈缓冲区 + `DrawTextW` | 32.15 ms | 42.71 | ❌ FAIL |
| v3 | `ExtTextOutW` + `GetTextExtent` 手工截断 | 8.99 ms | 11.05 | ⚠️ OK |
| v4 | `SetTextAlign(TA_RIGHT)` 跳过测量 | 6.70 ms | 7.59 | ⚠️ OK |
| v5 | 列布局预计算 + `SetTextAlign` 只在变化时调 | 7.28 ms | 9.67 | ⚠️ OK |
| v6 | 脏区填充 + 脏区 BitBlt | ~7.2 ms | ~9.5 | ⚠️ OK |
| **v7** | **后备缓冲内 BitBlt 搬移 + 只画新暴露条带** | **0.57 ms** | **0.87** | ✅ **PASS** |

**累计加速 80 倍**（45.70 → 0.57 ms）。

### v7 的关键设计

滚动时不在后备缓冲里重画全部可见行，而是：

```cpp
// 在后备缓冲内部直接搬移已有像素
::BitBlt(memDC_, 0, headerHeight_, width, bodyH,
         memDC_, 0, headerHeight_ + delta, SRCCOPY);

// 只把新暴露的那一条带标脏
if (delta > 0)
    ::InvalidateRect(hwnd_, &RECT{0, rc.bottom - delta, rc.right, rc.bottom}, FALSE);
else
    ::InvalidateRect(hwnd_, &RECT{0, headerHeight_, rc.right, headerHeight_ - delta}, FALSE);
```

效果：**10000 行数据，滚动时只渲染 3 行**，p95 = 0.87 ms = 60fps 预算的 **5%**。

```
=== UI RENDER BENCHMARK ===
  Total rows in dataset : 10000
  Visible rows rendered : 3
  Frames measured       : 300
  Paint time  avg 0.567 ms   p50 0.515   p95 0.867   max 2.482
  Breakdown (us): fill=16  rows=475  header=0  blit=51

  VERDICT: PASS
           Only 3 of 10000 rows rendered -> cost is independent of row count.
```

### 分段耗时定位（v5 时，证明瓶颈在文字绘制）

```
fill=498 us (6%)  rows=4775 us (65%)  header=822 us (9%)  blit=671 us (9%)
```

这组数据直接否定了"GDI 全表重绘是瓶颈"的猜测——瓶颈是**每次 `DrawTextW` 的字形解析与省略号测量**，不是像素填充。

---

## 里程碑 3：真实原型对比

```
build\TaskManagerPro.exe
```

### 启动与稳态

| 指标 | 原版任务管理器（实测） | Task Manager Pro（实测） |
|---|---|---|
| 打开后最长连续无响应 | **23.95 秒** | **0 秒**（30 秒观测内始终 Responding） |
| 打开后 CPU 峰值 | **127.8%**（单核） | 启动总耗时 **0.25 秒 CPU** |
| 稳态 CPU（单核占比） | 峰值 127.8%，平均数十 % | **2.39%** |
| 稳态 CPU（全系统占比） | 约 6.4% | **0.12%** |
| 工作集 | 47 MB+ | **13.1 MB** |
| 线程数 | 19 | **4** |
| 句柄数 | 491+ | **165** |

30 秒稳态测量原始数据：
```
CPU 增量      : 0.719 秒 / 30 秒
占单核        : 2.39 %
占全系统(20核): 0.12 %
Responding    : True
工作集        : 13.1 MB
线程          : 4
句柄          : 165
```

### 状态栏实时自监控

原型底部状态栏持续显示：
```
进程 499  线程 8562  句柄 189991  │  CPU 90.0%  物理内存 24.3 GB/31.9 GB  │
采集 10.41ms (缓冲 1235KB)  │  绘制 6.06ms  可见行 33  │  刷新 #18
```

- `采集 10.41ms`：每轮快照采集耗时（后台线程，不阻塞 UI）
- `绘制 6.06ms`：每秒一次的数据刷新重绘（全量可见区）
- `可见行 33`：虚拟化生效的证据
- `刷新 #18`：快照代际号

---

## 已知限制与后续工作

| 项 | 状态 | 说明 |
|---|---|---|
| 每秒数据刷新重绘 | 6.06 ms | 数据变化时必须重画可见区。可优化为"只重画数值真正变化的单元格"，预计可降到 1-2ms |
| 懒加载列（路径/命令行/图标/签名） | 已实现 `LazyLoader`，未接入 UI | 有界队列 + 持久缓存已就绪 |
| 进程树视图 | 未实现 | 需要 parent-pid 建树 + 展开/折叠 |
| 曲线图 | 未实现 | 建议照 System Informer 的 `circbuf.c` 环形缓冲 + GDI `Polyline` |
| 结束进程 / 优先级 / 亲和性 | 未实现 | 需要 SeDebugPrivilege 提权 |
| 高 DPI | 已调用 `SetProcessDpiAwarenessContext(PerMonitorV2)` | 但行高/列宽未按 DPI 缩放 |
| 单元格省略号 | 已去掉（用 `ETO_CLIPPED` 裁剪） | 长名字会被硬切，后续可加测量+省略号 |

---

## 复现方式

```bash
# 1. 构建
build.bat

# 2. 验证采集层（这是最关键的一步）
build\bench_snapshot.exe 20

# 3. 验证渲染层
build\bench_ui.exe 10000

# 4. 跑原型
build\TaskManagerPro.exe
```
