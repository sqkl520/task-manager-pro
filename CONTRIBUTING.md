[English](CONTRIBUTING-EN.md) | 简体中文

# 参与开发

这个项目没有包管理器、没有依赖安装、没有测试框架。构建就是一个 `build.bat`。

## 环境要求

- **Windows 10 或更高**（代码直接用 Win32 API 和 `ntdll.dll`，没有跨平台层）
- **Visual Studio 2022 或 Build Tools**，需要 C++ 桌面开发工作负载 + Windows SDK
- 没有别的

```bash
winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

## 构建

```bash
build.bat
```

脚本用 `vswhere` 找 MSVC，不需要先开 "x64 Native Tools" 提示符。产物在 `build\`。

构建会生成三个可执行文件：

| 产物 | 用途 |
|---|---|
| `build\TaskManagerPro.exe` | 主程序 |
| `build\bench_snapshot.exe` | 采集层基准，改采集代码后跑这个 |
| `build\bench_ui.exe` | 表格控件基准，改绘制代码后跑这个 |

## 提 PR 之前

### 1. 先跑基准

```bash
build\bench_snapshot.exe
build\bench_ui.exe
```

两个都必须通过，而且数字不能明显退化。

`bench_snapshot.exe` 验证单次 `NtQuerySystemInformation` 的耗时与进程数弱相关。
这是整个架构成立的前提——如果这一项退化，界面再好看也没意义。

`bench_ui.exe` 验证虚拟化表格的滚动绘制成本。

### 2. 改界面前先看规格

界面的每个尺寸、颜色、文案都有出处，在 `docs/specs/` 和 `docs/evidence/` 里。
改了值就说明旧值错了——**在代码注释里写清为什么**，引用你核对的那份证据。

这个项目被"凭感觉调数值"坑过很多次。凡是标注 `[推测]` 的结论，
如果你拿到了真凭实据，欢迎改；但不要把一个 `[推测]` 值换成另一个 `[推测]` 值。

### 3. 不要提交构建产物

`.gitignore` 已经排除了 `build/`、`*.obj`、`*.exe`、`*.pdb`。
如果你发现有别的东西混进了暂存区，改 `.gitignore`，不要 `git add -f`。

### 4. 提交信息

格式 `type(scope): 描述`，写**为什么**改而不只是改了什么。

```
fix(list): 按数值列排序时抑制分组头，与原版一致

原版在 SortColumn 非 0 时会清掉分组，所以「应用 (N)」这些表头消失。
之前一直显示分组头，导致排序后行序看起来和原版对不上。
```

`type` 用这七个：`feat` `fix` `docs` `refactor` `test` `chore` `perf`。

## 关于这个仓库的特殊约束

### 不能复制原版源代码

界面的依据是**可观测规格**：布局资源（`UIFILE`）、MUI 字符串表、
截图上的像素测量。这三类都可以合法观察。

提交里不要出现从网上找来的任务管理器源代码、反编译出来的 C 伪代码，
或者任何"照着改了改"的产物。反汇编用来**读数值**（某个常量是 28 还是 30）
是可以的，用来还原实现逻辑就不行。

### 不要用绝对路径

脚本、注释、文档里都不能出现 `D:\Projects\...` 这类机器相关路径。
`build.bat` 用 `%~dp0` 定位自己，`tools\*.bat` 用 `%~dp0..`。
如果你的代码需要知道项目位置，从可执行文件路径推，不要写死。

### 真实进程数据不进仓库

探针工具会输出本机进程名、PID、路径、命令行。这些属于真实数据，
输出文件在 `.gitignore` 里。要提交一份样例，自己造数据。

## 目录约定

```
src/          实现，按职责分文件
include/      对应头文件
tools/        一次性探针与验证工具
docs/         设计文档、实现规格、取证数据
docs/specs/   逐模块实现规格（代码注释直接引用）
docs/evidence/规格的一手数据来源
docs/reference/原版的参考截图
```

新增源文件时：

1. 放进 `src/`，头文件放 `include/`
2. 在 `build.bat` 的链接行里加上
3. 如果是新的一类功能，考虑是否真的需要新文件——现有文件已按职责拆得比较细

## 报告问题

用 Issue 模板。填环境时**务必写上 CPU 核数和进程/线程数**——这个项目存在的
理由就是原版在大机器上失效，规模信息比其他项目重要得多。

如果某个行为"和原版不一样"，附一张原版截图。这是本项目能拿到的最强证据。
