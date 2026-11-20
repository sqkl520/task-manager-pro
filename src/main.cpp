// main.cpp - Task Manager Pro 入口
//
// 架构（对应 DIAGNOSIS.md 四条铁律）：
//   1. UI 线程永不调用 NtQuerySystemInformation / OpenProcess / PDH Collect
//   2. 单次系统调用拿全量快照
//   3. 绝不轮询 \Thread(*) / \Process(*) PDH 通配符
//   4. 昂贵列懒加载（本版未接入）
#include "collector.h"
#include "perfdata.h"
#include "tmwindow.h"
#include "menus.h"

#include <windows.h>
#include <commctrl.h>
#include <memory>
#include <string>
#include <cstdarg>

#pragma comment(lib, "comctl32.lib")

using namespace tmp;

namespace {
HWND g_main = nullptr;
TmWindow* g_win = nullptr;
Collector* g_collector = nullptr;
PerfCollector* g_perf = nullptr;
uint64_t g_uiSeq = 0;
uint64_t g_perfSeq = 0;

// 诊断输出的落盘位置：exe 同目录。不要写死绝对路径 —— 换台机器就失效，
// 而那些 --gdileak / --paintstats 之类的开关恰恰是在排查时才用的。
std::wstring ExeDirFile(const wchar_t* name) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring out(path, n ? n : 0);
    size_t sl = out.find_last_of(L'\\');
    if (sl != std::wstring::npos) out.resize(sl + 1);
    else out.clear();
    out += name;
    return out;
}

// ============ 后台自测模式 ============
//
// --selftest 时不显示窗口、不抢焦点、不动鼠标指针。
// 所有交互都是用 SendMessage 直接投递给自己的窗口过程，等价于点击，
// 但完全不经过输入队列，不会打扰用户正在做别的事。
//
// 输出写到 exe 同目录 selftest.txt。
void RunSelfTest(HWND hwnd) {
    // 等采集器攒够两轮快照（首轮没有 CPU 差分基准）
    for (int i = 0; i < 60; ++i) {
        if (g_collector->Sequence() >= 2) break;
        Sleep(100);
    }
    auto snap = g_collector->GetSnapshot();
    if (!snap) {
        MessageBoxW(nullptr, L"采集器没有产出快照", L"自测失败", MB_OK);
        return;
    }

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring out = path;
    size_t sl = out.find_last_of(L'\\');
    if (sl != std::wstring::npos) out = out.substr(0, sl);
    out += L"\\selftest.txt";

    // 用 UTF-8 写文件。宽字符 fwprintf 在当前 ANSI 代码页（936）下会把
    // 中文变成 "??"，且 %s 的宽度/字节解释不一致，所以统一转成 UTF-8 窄字符。
    std::string outA;
    {
        int need = WideCharToMultiByte(CP_UTF8, 0, out.c_str(), -1, nullptr, 0, nullptr, nullptr);
        outA.resize(need > 0 ? need : 0);
        if (need > 0) {
            WideCharToMultiByte(CP_UTF8, 0, out.c_str(), -1, &outA[0], need, nullptr, nullptr);
            outA.pop_back();   // 去掉结尾 \0
        }
    }

    FILE* f = nullptr;
    fopen_s(&f, outA.c_str(), "wb");
    if (!f) return;
    // UTF-8 BOM，方便记事本正确识别
    fwrite("\xEF\xBB\xBF", 1, 3, f);

    // 把宽字符串转成 UTF-8 再写。用一个 lambda 省掉到处贴转换代码。
    auto W = [](const wchar_t* s) -> std::string {
        if (!s) return std::string();
        int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
        if (n <= 0) return std::string();
        std::string r(n - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, s, -1, &r[0], n, nullptr, nullptr);
        return r;
    };
    auto LOG = [&](const std::string& s) { fwrite(s.data(), 1, s.size(), f); };
    auto LOGF = [&](const char* fmt, ...) {
        char buf[2048];
        va_list ap;
        va_start(ap, fmt);
        _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
        va_end(ap);
        fwrite(buf, 1, strlen(buf), f);
    };
    // 宽 → UTF-8 后拼接
    auto LOGW = [&](const std::wstring& w) { LOG(W(w.c_str())); };

    LOGW(L"=== Task Manager Pro 后台自测 ===\n");
    LOGF("快照序号=%llu  进程数=%zu  采集耗时=%.2f ms\n\n",
         (unsigned long long)snap->sequence, snap->processes.size(),
         snap->collectionMs);

    LOGW(L"网络采集: ");
    LOGW(g_collector->NetworkAvailable() ? L"可用" : L"不可用");
    LOGW(L"  ");
    LOGW(g_collector->NetworkStatus());
    LOGW(L"\n\n");

    // 统计有数据的列，确认数据链路通了
    size_t withDisk = 0, withNet = 0, withGpu = 0;
    double maxDisk = 0, maxNet = 0, maxGpu = 0, maxCpu = 0, maxMem = 0;
    uint32_t maxDiskPid = 0, maxNetPid = 0;
    const ProcessEntry* topCpu = nullptr;
    const ProcessEntry* topMem = nullptr;
    for (const auto& e : snap->processes) {
        if (e.diskBytesPerSec > 0.05 * 1048576.0) ++withDisk;
        if (e.netBitsPerSec > 0.05 * 1.0e6) ++withNet;
        if (e.gpuPercent > 0.05) ++withGpu;
        if (e.diskBytesPerSec > maxDisk) { maxDisk = e.diskBytesPerSec; maxDiskPid = e.pid; }
        if (e.netBitsPerSec > maxNet) { maxNet = e.netBitsPerSec; maxNetPid = e.pid; }
        if (e.gpuPercent > maxGpu) maxGpu = e.gpuPercent;
        if (!topCpu || e.cpuPercent > topCpu->cpuPercent) { if (e.pid) topCpu = &e; }
        if (!topMem || e.workingSet > topMem->workingSet) { if (e.pid) topMem = &e; }
        if (e.cpuPercent > maxCpu) maxCpu = e.cpuPercent;
        if (static_cast<double>(e.workingSet) > maxMem) maxMem = static_cast<double>(e.workingSet);
    }
    LOGF("有磁盘数据的进程: %zu  最大 %.2f MB/s (pid %u)\n",
         withDisk, maxDisk / 1048576.0, maxDiskPid);
    LOGF("有网络数据的进程: %zu  最大 %.2f Mbps (pid %u)\n",
         withNet, maxNet / 1.0e6, maxNetPid);
    LOGF("有GPU数据的进程:  %zu  最大 %.2f %%\n\n", withGpu, maxGpu);

    if (topCpu) {
        LOGW(L"CPU 最高: ");
        LOGW(topCpu->imageName);
        LOGF("  %.2f%%\n", topCpu->cpuPercent);
    }
    if (topMem) {
        LOGW(L"内存最高: ");
        LOGW(topMem->imageName);
        LOGF("  %.1f MB\n\n", topMem->workingSet / 1048576.0);
    }

    // ---- 排序自测：直接发 WM_LBUTTONDOWN 到列头 ----
    LOGW(L"=== 列排序自测 ===\n");
    const wchar_t* colNames[] = { L"名称", L"状态", L"CPU", L"内存", L"磁盘", L"网络", L"GPU" };
    // 列头中心 y=144（客户区），x 从 12 起，名称列弹性宽
    RECT rc;
    GetClientRect(hwnd, &rc);
    int clientW = rc.right;
    int nameW = (clientW - 12) - 476;
    if (nameW < 180) nameW = 180;
    int widths[] = { nameW, 70, 66, 90, 86, 86, 72 };
    int acc = 12;
    for (int col = 0; col < 7; ++col) {
        int cx = acc + widths[col] / 2;
        acc += widths[col];
        LPARAM lp = MAKELPARAM(cx, 144);
        SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp);
        SendMessageW(hwnd, WM_LBUTTONUP, 0, lp);
        LOGW(L"  点击「");
        LOGW(colNames[col]);
        LOGF("」列 (x=%d) → 已发送\n", cx);
    }
    LOGW(L"\n");

    // ---- 右键菜单自测：逐个页面构建菜单并统计项数 ----
    LOGW(L"=== 右键菜单自测 ===\n");
    struct TabCase { int idx; const wchar_t* name; };
    TabCase cases[] = {
        { 0, L"进程" }, { 1, L"性能" }, { 2, L"应用历史" }, { 3, L"启动" },
        { 4, L"用户" }, { 5, L"详细信息" }, { 6, L"服务" },
    };
    for (auto& c : cases) {
        g_win->ShowPage(c.idx);
        Sleep(60);
        HMENU m = nullptr;
        switch (c.idx) {
        case 0: m = MenuBuilder::BuildProcessContext(true, false, true, false); break;
        case 1: m = MenuBuilder::BuildPerfContext(0, false, false); break;
        case 2: m = MenuBuilder::BuildAppHistoryContext(true); break;
        case 3: m = MenuBuilder::BuildStartupContext(true, true); break;
        case 4: m = MenuBuilder::BuildUserContext(true, false); break;
        case 5: m = MenuBuilder::BuildDetailsContext(true); break;
        case 6: m = MenuBuilder::BuildServiceContext(true); break;
        }
        if (m) {
            int n = GetMenuItemCount(m);
            int enabled = 0;
            for (int i = 0; i < n; ++i) {
                UINT st = GetMenuState(m, i, MF_BYPOSITION);
                if (st != 0xFFFFFFFF && !(st & (MF_GRAYED | MF_DISABLED))) ++enabled;
            }
            LOGW(L"  ");
            LOGW(c.name);
            LOGF("页: %d 项，其中可用 %d 项\n", n, enabled);
            DestroyMenu(m);
        }
        Sleep(40);
    }

    // ---- 进程树自测：展开第一个有子进程的应用行 ----
    LOGW(L"\n=== 进程树自测 ===\n");
    g_win->ShowPage(0);
    Sleep(120);
    LPARAM lp = MAKELPARAM(18, 246);
    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp);
    SendMessageW(hwnd, WM_LBUTTONUP, 0, lp);
    LOGW(L"  已向第一行应用点击展开三角\n");

    LOGW(L"\n=== 自测结束 ===\n");
    fclose(f);
    PostQuitMessage(0);
}
} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR lpCmdLine, int) {
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 支持 --page=N 直接打开指定页（0=进程 5=详细信息），便于验证与脚本化
    // --chart=logical 可让性能页直接以「逻辑处理器」子视图打开
    // --view=gpu 直接切到 GPU 页（cpu/memory/disk/network/gpu）
    int startPage = 0;
    bool selfTest = false;
    bool startLogical = false;
    bool startOverall = false;
    std::wstring startView;
    std::wstring shotPath;      // --shot=xxx.bmp：离屏渲染后直接退出，不显示窗口
    if (lpCmdLine && *lpCmdLine) {
        const wchar_t* p = wcsstr(lpCmdLine, L"--page=");
        if (p) startPage = _wtoi(p + 7);
        if (wcsstr(lpCmdLine, L"--selftest")) selfTest = true;
        if (wcsstr(lpCmdLine, L"--chart=logical")) startLogical = true;
        // --chart=overall：强制「总体利用率」单曲线视图。默认是逻辑处理器网格，
        // 内核时间叠加曲线只在单曲线视图里画，脚本化验证需要这个入口。
        if (wcsstr(lpCmdLine, L"--chart=overall")) startOverall = true;
        const wchar_t* v = wcsstr(lpCmdLine, L"--view=");
        if (v) {
            v += 7;
            const wchar_t* end = v;
            while (*end && *end != L' ' && *end != L'\t') ++end;
            startView.assign(v, end);
        }
        const wchar_t* s = wcsstr(lpCmdLine, L"--shot=");
        if (s) {
            s += 7;
            // 路径可能含空格（如 D:\Projects\Task Manager Pro\...）。
            // 命令行里用引号包住就能保留空格，但 wWinMain 收到的 lpCmdLine
            // 已经把引号去掉了。所以这里改成：如果整串以 .bmp 结尾，
            // 就整串当作路径（这是 --shot 的最后一个参数）。
            const wchar_t* end = s;
            while (*end) ++end;
            std::wstring raw(s, end);
            // 去掉尾部空格和引号
            while (!raw.empty() && (raw.back() == L' ' || raw.back() == L'"' ||
                                    raw.back() == L'\t')) {
                raw.pop_back();
            }
            while (!raw.empty() && (raw.front() == L'"' || raw.front() == L' ')) {
                raw.erase(raw.begin());
            }
            shotPath = raw;
        }
    }
    // 有 --shot 就强制隐藏窗口
    const bool offscreen = !shotPath.empty();

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"TaskManagerPro.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!selfTest) {
            MessageBoxW(nullptr, L"Task Manager Pro 已在运行。", L"提示", MB_OK | MB_ICONINFORMATION);
        }
        return 0;
    }

    // 采集器先起，UI 不等它
    g_collector = new Collector();
    g_collector->Start(1000);

    // 性能页采集器：独立线程，1 Hz
    g_perf = new PerfCollector();
    g_perf->Start(1000);

    g_win = new TmWindow();
    // 把采集器挂到窗口上，这样「查看 → 更新速度」「立即刷新」才能真正生效
    g_win->AttachCollector(g_collector);
    // 自测 / 离屏截图模式下窗口不显示：SW_HIDE。
    // 窗口仍然存在、仍然能收消息，但不会出现在屏幕上，不抢焦点，不动用户鼠标。
    if (!g_win->Create(inst, (selfTest || offscreen) ? SW_HIDE : SW_SHOW)) {
        MessageBoxW(nullptr, L"窗口创建失败。", L"错误", MB_OK | MB_ICONERROR);
        return 1;
    }
    g_main = g_win->Handle();
    if (startPage > 0) {
        g_win->ShowPage(startPage);
    } else if (!selfTest && !offscreen) {
        // 【「选项 → 设置默认选项卡」真正生效的地方】
        // 那个菜单项此前是死开关：只把 StartUpTab 写进注册表 + 挪一下圆点，
        // 而 ReadDefaultTab() 的唯一读点是「画圆点」，启动落哪一页只由 --page=
        // 决定。于是用户设了默认页，重启后仍然回到「进程」。
        // 这里在**没有显式 --page= 的情况下**才读注册表，保证脚本化验证不受影响。
        const int def = MenuBuilder::ReadDefaultTab(0);
        if (def > 0) g_win->ShowPage(def);
    }
    if (startLogical) {
        g_win->ShowPage(1);
        g_win->SetPerfChart(1);
    }
    if (startOverall) {
        g_win->ShowPage(1);
        g_win->SetPerfChart(0);
    }
    if (!startView.empty()) {
        g_win->ShowPage(1);
        g_win->SetPerfViewByName(startView.c_str());
    }
    // --sortcol=N 指定启动时的排序列（0=名称 … 便于脚本化验证）
    if (lpCmdLine) {
        const wchar_t* sc = wcsstr(lpCmdLine, L"--sortcol=");
        if (sc) g_win->SetSortColumn(_wtoi(sc + 10));
        // --nogroup：关掉「按类型分组」，用于验证平铺视图下的全局排序
        if (wcsstr(lpCmdLine, L"--nogroup")) g_win->SetGroupByType(false);
        // --colw=N:W 把第 N 列宽度设为 W（用于验证列宽拖拽的渲染与持久化路径）
        {
            const wchar_t* cw = wcsstr(lpCmdLine, L"--colw=");
            if (cw) {
                int col = 0, w = 0;
                if (swscanf_s(cw + 7, L"%d:%d", &col, &w) == 2) {
                    g_win->SetColumnWidth(col, w, false);   // false = 不写注册表
                }
            }
        }
        // --clickcol=N：模拟真实点击第 N 个列头（走完整的命中测试 + 排序路径），
        // 用于验证「点名称列到底有没有按名称重排」。与 --sortcol 不同，
        // 它不绕过 HitTestColumn，能抓出列索引错位这类 bug。
        const wchar_t* cc = wcsstr(lpCmdLine, L"--clickcol=");
        if (cc) g_win->SimulateHeaderClick(_wtoi(cc + 11));
        // --kernel：打开「查看 → 显示内核时间」，用于脚本化验证内核曲线确实画了出来
        if (wcsstr(lpCmdLine, L"--kernel")) {
            MenuBuilder::PerfToggles().kernelTime = true;
        }
        // --simple：打开底部「简略信息(D)」视图，用于脚本化验证它真的有可见效果
        if (wcsstr(lpCmdLine, L"--simple")) {
            g_win->ToggleSimpleView();
        }
    }

    if (selfTest) {
        // 同步跑完自测再进消息循环：自测内部用 SendMessage，
        // 所有点击都是直接投递给自己的窗口过程，不经过输入队列。
        RunSelfTest(g_main);
    }

    if (offscreen) {
        // 离屏截图：等采集器攒够几轮（首轮没有 CPU 差分基准），
        // 把快照推给窗口，然后画到内存位图存盘，直接退出。
        // 全程窗口是 SW_HIDE，不出现在屏幕上。

        // --gdileak=N：后备缓冲泄漏回归测试。
        // 反复以不同尺寸重建后备缓冲（模拟用户拖动/最大化窗口触发 WM_SIZE），
        // 前后各读一次 GDI 对象数。**修复前**每次重建都会泄漏一个 HBITMAP，
        // 所以 delta ≈ N；修复后 delta 应恒为 0。
        // 用 GetGuiResources 在进程内直接读，不依赖外部工具。
        if (lpCmdLine && wcsstr(lpCmdLine, L"--gdileak=")) {
            const int n = _wtoi(wcsstr(lpCmdLine, L"--gdileak=") + 10);
            DWORD before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            for (int i = 0; i < n; ++i) {
                // 尺寸每次不同，强制走 EnsureBackBuffer 的重建分支
                g_win->ForceBackBufferSize(600 + (i % 37) * 13, 400 + (i % 29) * 11);
            }
            DWORD after = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            wchar_t msg[256];
            swprintf_s(msg, L"GDI objects: before=%lu after=%lu delta=%ld (iterations=%d)\n",
                       before, after, static_cast<long>(after) - static_cast<long>(before), n);
            OutputDebugStringW(msg);
            FILE* gf = nullptr;
            const std::wstring gdiPath = ExeDirFile(L"gdi_leak.txt");
            if (_wfopen_s(&gf, gdiPath.c_str(), L"w") == 0) {
                fwprintf(gf, L"%s", msg);
                fclose(gf);
            }
            g_collector->Stop();
            return 0;
        }

        std::shared_ptr<const Snapshot> snap;
        std::shared_ptr<const PerfSnapshot> psnap;
        for (int i = 0; i < 80; ++i) {
            Sleep(100);
            if (g_collector->Sequence() >= 3) {
                snap = g_collector->GetSnapshot();
                if (snap) {
                    uint32_t procs = 0, threads = 0;
                    uint64_t handles = 0;
                    for (const auto& e : snap->processes) {
                        if (e.pid != 0) ++procs;
                        threads += e.threadCount;
                        handles += e.handleCount;
                    }
                    g_perf->SetProcessCounts(procs, threads, handles);
                    g_win->SetSnapshot(snap);
                }
                if (g_perf->Sequence() >= 2) {
                    psnap = g_perf->GetSnapshot();
                    if (psnap) g_win->SetPerfSnapshot(psnap);
                }
                if (snap && psnap) break;
            }
        }
        // 再等一拍，让页面的历史曲线攒出形状
        for (int i = 0; i < 60; ++i) {
            Sleep(100);
            auto s2 = g_collector->GetSnapshot();
            if (s2) g_win->SetSnapshot(s2);
            auto p2 = g_perf->GetSnapshot();
            if (p2) g_win->SetPerfSnapshot(p2);
        }

        // --expandall：展开所有应用行的子进程。
        // 【必须放在这里】cls_.appRootOf 由 SetSnapshot 填充，而命令行解析
        // 早于首次快照 —— 在那边调用时 appRootOf 还是空的，等于什么都没做。
        // 用于验证「子行高 22 vs 父行 28」的绘制接线。
        if (lpCmdLine && wcsstr(lpCmdLine, L"--expandall")) {
            g_win->ExpandAllApps();
        }

        {
            bool ok = g_win->SaveOffscreen(shotPath.c_str());
            std::wstring dbgPath = ExeDirFile(L"shot_debug.txt");
            FILE* df = nullptr;
            _wfopen_s(&df, dbgPath.c_str(), L"w");
            if (df) {
                fwprintf(df, L"ok=%d\npath=%s\n", ok ? 1 : 0, shotPath.c_str());
                fclose(df);
            }
        }

        KillTimer(g_main, 1);
        delete g_win;
        delete g_collector;
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    // UI 定时器只做「代际号比对」
    SetTimer(g_main, 1, 250, nullptr);

    // --paintstats=N：跑 N 秒后把绘制耗时统计写盘并退出。
    // 用于量化「每帧绘制到底多慢」，不引入外部工具、不改绘制路径。
    int paintStatsSecs = 0;
    if (lpCmdLine) {
        const wchar_t* ps = wcsstr(lpCmdLine, L"--paintstats=");
        if (ps) paintStatsSecs = _wtoi(ps + 13);
    }
    if (paintStatsSecs > 0) {
        // 用一次性定时器在 N 秒后落盘并退出。
        SetTimer(g_main, 99, static_cast<UINT>(paintStatsSecs) * 1000, nullptr);
    }

    MSG msg;
    // 【补漏】加速键表此前虽然构建了，但**消息循环里没有 TranslateAccelerator**，
    // 所以一条都不生效。原版的 ACCEL #26100 里 Delete=结束任务、Esc=退出、
    // F5=刷新，这些现在才真正可用。
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_win && TranslateAcceleratorW(g_main, g_win->Accel(), &msg)) {
            continue;
        }
        if (msg.message == WM_TIMER && msg.wParam == 99) {
            // --paintstats 的收尾：写盘 + 退出。
            KillTimer(g_main, 99);
            if (g_win) {
                g_win->DumpPaintStats(ExeDirFile(L"paintstats.txt").c_str());
            }
            DestroyWindow(g_main);
            break;
        }
        if (msg.message == WM_TIMER && msg.wParam == 1) {
            const uint64_t seq = g_collector->Sequence();
            if (seq != g_uiSeq) {
                auto snap = g_collector->GetSnapshot();
                if (snap) {
                    g_uiSeq = seq;
                    // 把进程/线程/句柄数转给性能采集器，避免重复统计
                    uint32_t procs = 0, threads = 0;
                    uint64_t handles = 0;
                    for (const auto& e : snap->processes) {
                        if (e.pid != 0) ++procs;
                        threads += e.threadCount;
                        handles += e.handleCount;
                    }
                    g_perf->SetProcessCounts(procs, threads, handles);
                    g_win->SetSnapshot(std::move(snap));
                }
            }
            // 性能页数据
            const uint64_t pseq = g_perf->Sequence();
            if (pseq != g_perfSeq) {
                auto ps = g_perf->GetSnapshot();
                if (ps) {
                    g_perfSeq = pseq;
                    g_win->SetPerfSnapshot(std::move(ps));
                }
            }
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    KillTimer(g_main, 1);
    delete g_win;
    delete g_collector;
    if (mutex) CloseHandle(mutex);
    return 0;
}
