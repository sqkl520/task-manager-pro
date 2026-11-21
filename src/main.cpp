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
// 交互一律走 TmWindow 的 *ForTest() 入口（它内部用真实几何做命中 + 真状态转移），
// 并且**每一项都回读结果判定 PASS/FAIL** —— 只打印「已发送」是假自测：
// 消息投出去之后有没有命中任何目标根本没人看。
//
// 输出写到 exe 同目录 selftest.txt，末尾一行机器可判：
//   === 结果: PASS n / FAIL n / SKIP n ===
// 返回值：0 = 全过，1 = 有 FAIL，2 = 单实例冲突（未执行）。

namespace {

// 自测日志：UTF-8 + BOM 落盘。宽字符 fwprintf 在 936 代码页下会把中文变成
// "??"，所以统一转 UTF-8 窄字符写。
class SelfTestLog {
public:
    ~SelfTestLog() { Close(); }

    bool Open(const std::wstring& path) {
        std::string a;
        int need = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (need <= 0) return false;
        a.resize(need);
        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, &a[0], need, nullptr, nullptr);
        a.pop_back();   // 去掉结尾 \0
        if (fopen_s(&f_, a.c_str(), "wb") != 0) { f_ = nullptr; return false; }
        fwrite("\xEF\xBB\xBF", 1, 3, f_);   // BOM，方便记事本正确识别
        return true;
    }
    bool ok() const { return f_ != nullptr; }
    void Close() {
        if (f_) { fclose(f_); f_ = nullptr; }
    }

    void Raw(const char* s) { if (f_) fwrite(s, 1, strlen(s), f_); }
    void Raw(const std::string& s) { if (f_) fwrite(s.data(), 1, s.size(), f_); }
    // 宽 → UTF-8
    void W(const wchar_t* s) {
        if (!s || !f_) return;
        int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
        if (n <= 1) return;
        std::string r(n - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, s, -1, &r[0], n, nullptr, nullptr);
        fwrite(r.data(), 1, r.size(), f_);
    }
    void W(const std::wstring& s) { W(s.c_str()); }
    void F(const char* fmt, ...) {
        if (!f_) return;
        char buf[2048];
        va_list ap;
        va_start(ap, fmt);
        _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
        va_end(ap);
        fwrite(buf, 1, strlen(buf), f_);
    }

private:
    FILE* f_ = nullptr;
};

std::wstring SelfTestPath() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring out(path, n ? n : 0);
    size_t sl = out.find_last_of(L'\\');
    out.resize(sl == std::wstring::npos ? 0 : sl + 1);
    out += L"selftest.txt";
    return out;
}

// 单实例冲突时**必须**覆写这份文件。
// 否则脚本读到的是上一次运行留下的 PASS，会把「压根没跑」当成「跑过了」。
void WriteSelfTestNotRun(const wchar_t* why) {
    SelfTestLog log;
    if (!log.Open(SelfTestPath())) return;
    log.W(L"=== Task Manager Pro 后台自测 ===\n");
    log.W(L"未执行：");
    log.W(why);
    log.W(L"\n");
    log.W(L"=== 结果: PASS 0 / FAIL 0 / SKIP 0 （未执行） ===\n");
}

// 逐项结论统计。名字 + [PASS]/[FAIL]/[SKIP] + 说明，一行一项。
struct Tally {
    int pass = 0, fail = 0, skip = 0;
};

const char* VerdictTag(int v) {   // 1=PASS 0=FAIL -1=SKIP
    return v == 1 ? "[PASS] " : (v == 0 ? "[FAIL] " : "[SKIP] ");
}

} // namespace

int RunSelfTest() {
    SelfTestLog log;
    if (!log.Open(SelfTestPath())) return 1;

    Tally t;
    // 记一项结论。detail 可为空。
    auto Mark = [&](int verdict, const wchar_t* name, const std::string& detail) {
        if (verdict == 1) ++t.pass;
        else if (verdict == 0) ++t.fail;
        else ++t.skip;
        log.F("%s", VerdictTag(verdict));
        log.W(name);
        if (!detail.empty()) {
            log.Raw(" — ");
            log.Raw(detail);
        }
        log.Raw("\n");
    };
    auto MarkW = [&](int verdict, const wchar_t* name, const std::wstring& detail) {
        std::string d;
        int n = WideCharToMultiByte(CP_UTF8, 0, detail.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) {
            d.resize(n - 1);
            WideCharToMultiByte(CP_UTF8, 0, detail.c_str(), -1, &d[0], n, nullptr, nullptr);
        }
        Mark(verdict, name, d);
    };

    log.W(L"=== Task Manager Pro 后台自测 ===\n");

    // 等采集器攒够两轮快照（首轮没有 CPU 差分基准）
    for (int i = 0; i < 60; ++i) {
        if (g_collector->Sequence() >= 2) break;
        Sleep(100);
    }
    auto snap = g_collector->GetSnapshot();
    if (!snap) {
        // 【不弹模态框】无属主、可见、抢焦点、阻塞 —— 用户正在用这台机器。
        // 写日志 + FAIL 即可，调用方按退出码判定。
        Mark(0, L"采集器产出快照", "GetSnapshot() 返回空");
        log.W(L"\n=== 结果: PASS 0 / FAIL 1 / SKIP 0 ===\n");
        return 1;
    }

    // 【关键修复】必须把快照交给窗口，否则 snap_ 为 null、rows_ 为空，
    // 下面所有交互都点在一个空列表上。之前一直缺这一行，
    // 导致自测的交互部分全部空转（数字统计部分是真的）。
    // g_win->SetSnapshot(snap);   // ← 负向对照：临时注释，验证自测能抓出这个缺陷

    log.F("快照序号=%llu  进程数=%zu  采集耗时=%.2f ms\n",
         (unsigned long long)snap->sequence, snap->processes.size(),
         snap->collectionMs);
    log.W(L"网络采集: ");
    log.W(g_collector->NetworkAvailable() ? L"可用" : L"不可用");
    log.W(L"  ");
    log.W(g_collector->NetworkStatus());
    log.W(L"\n\n");

    // 自测一律在进程页做（列模型/行模型只有这一页有）
    g_win->ShowPage(0);

    Mark(g_win->RowCount() > 0 ? 1 : 0, L"快照已交给窗口，列表非空",
         "RowCount=" + std::to_string(g_win->RowCount()));

    // ---- 统计有数据的列，确认数据链路通了 ----
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
    log.F("有磁盘数据的进程: %zu  最大 %.2f MB/s (pid %u)\n",
         withDisk, maxDisk / 1048576.0, maxDiskPid);
    log.F("有网络数据的进程: %zu  最大 %.2f Mbps (pid %u)\n",
         withNet, maxNet / 1.0e6, maxNetPid);
    log.F("有GPU数据的进程:  %zu  最大 %.2f %%\n\n", withGpu, maxGpu);

    if (topCpu) {
        log.W(L"CPU 最高: ");
        log.W(topCpu->imageName);
        log.F("  %.2f%%\n", topCpu->cpuPercent);
    }
    if (topMem) {
        log.W(L"内存最高: ");
        log.W(topMem->imageName);
        log.F("  %.1f MB\n\n", topMem->workingSet / 1048576.0);
    }
    // 数据链路：CPU 汇总必须是一个**合理**的百分比。
    // 【PID 0 陷阱】把空闲进程算进去会让汇总恒在 90~100%（pid 0 的 CPU 时间
    // 就是系统空闲时间）。所以这里只校验「不是恒满」这一个可判定的性质。
    Mark(maxCpu > 0.0 && maxCpu < 100.0 ? 1 : 0, L"单进程 CPU 峰值在 (0,100) 区间",
         [&] {
             char b[64];
             sprintf_s(b, "maxCpu=%.2f%%", maxCpu);
             return std::string(b);
         }());

    // ---- 列排序自测：点列头 → 回读 sortCol_ ----
    //
    // 坐标不再硬编码。此前写死「列头 y=144」，而 BuildLayout 里
    // rcColHeader_ = {0, 45, W, 89}，y=144 落在 rcList_ 的第 2 行上 ——
    // 消息被「选中行」分支吃掉，排序一次都没变过，日志却全绿。
    // 现在由 TmWindow::ClickColumnHeaderForTest 用真实几何逐像素反查目标列。
    log.W(L"\n=== 列排序自测 ===\n");
    const int colCount = g_win->ColumnCountForTest();
    // 先按名称列排一次，拿到一份基准 pid 顺序，后面用来验证「真的重排了」。
    std::wstring baseOrder;
    bool haveBase = false;
    int reordered = -1;   // 1=确实变了 0=没变 -1=未测

    for (int col = 0; col < colCount; ++col) {
        const wchar_t* title = g_win->ColumnTitleForTest(col);
        std::wstring name = L"点击列头「";
        name += (title ? title : L"?");
        name += L"」改变排序列";

        if (!g_win->ColumnVisibleForTest(col)) {
            MarkW(-1, name.c_str(), L"该列被隐藏（不占位、命中不到），无法点击");
            continue;
        }
        const int beforeCol = g_win->SortColumnForTest();
        const bool beforeDesc = g_win->SortDescendingForTest();
        const bool clicked = g_win->ClickColumnHeaderForTest(col);
        const int afterCol = g_win->SortColumnForTest();
        const bool afterDesc = g_win->SortDescendingForTest();

        char detail[160];
        sprintf_s(detail, "sortCol %d→%d, desc %d→%d", beforeCol, afterCol,
                  beforeDesc ? 1 : 0, afterDesc ? 1 : 0);
        const bool ok = clicked && afterCol == col &&
                        (afterCol != beforeCol || afterDesc != beforeDesc);
        Mark(ok ? 1 : 0, name.c_str(), detail);

        if (col == 0) {
            baseOrder.clear();
            for (int r = 0; r < g_win->RowCount(); ++r) {
                baseOrder += std::to_wstring(g_win->RowPidForTest(r));
                baseOrder += L',';
            }
            haveBase = true;
        } else if (col == 2 && haveBase) {
            // CPU 列（数值列）排完，行序必须与名称序不同 —— 否则 rows_ 根本没重排。
            std::wstring now;
            for (int r = 0; r < g_win->RowCount(); ++r) {
                now += std::to_wstring(g_win->RowPidForTest(r));
                now += L',';
            }
            reordered = (now != baseOrder) ? 1 : 0;
        }
    }
    if (reordered >= 0) {
        Mark(reordered, L"按 CPU 列排序后行表确实重排", reordered
                 ? "行序与名称序不同"
                 : "行序与名称序完全相同（rows_ 可能没重排）");
    } else {
        Mark(-1, L"按 CPU 列排序后行表确实重排", "CPU 列被隐藏，未测");
    }

    // ---- 选中行自测 ----
    // 缺陷 1 的直接体现：SetSnapshot 缺失时 rows_ 为空、SelectedProcess() 恒 nullptr，
    // 「结束任务」「转到详细信息」这类菜单命令全部静默返回。
    log.W(L"\n=== 选中行自测 ===\n");
    {
        int pick = -1;
        for (int r = 0; r < g_win->RowCount(); ++r) {
            if (g_win->RowPidForTest(r) != 0) { pick = r; break; }
        }
        if (pick < 0) {
            Mark(-1, L"点击第 1 个进程行后 SelectedProcess() 非空", "列表里没有进程行");
        } else {
            const uint32_t wantPid = g_win->RowPidForTest(pick);
            const bool clicked = g_win->ClickRowForTest(pick);
            const ProcessEntry* sel = g_win->SelectedProcessForTest();
            char detail[128];
            sprintf_s(detail, "点击 pid=%u, SelectedProcess=%s",
                      wantPid, sel ? "非空" : "nullptr");
            Mark(clicked && sel && sel->pid == wantPid ? 1 : 0,
                 L"点击第 1 个进程行后 SelectedProcess() 非空", detail);
        }
    }

    // ---- 进程树自测：找到第一个有子进程的应用行，点它的展开三角 ----
    // 行高不相等（组头 38 / 父行 28 / 子行 22），y 由 TmWindow 现场算，
    // 不再硬编码 y=246（那个值在默认 4 列布局下早已对不上）。
    log.W(L"\n=== 进程树自测 ===\n");
    {
        g_win->ShowPage(0);
        int appRow = -1;
        for (int r = 0; r < g_win->RowCount(); ++r) {
            if (g_win->RowHasChildrenForTest(r)) { appRow = r; break; }
        }
        if (appRow < 0) {
            Mark(-1, L"展开应用行后子进程真的插入行表", "没有带子进程的应用行");
        } else {
            const uint32_t pid = g_win->RowPidForTest(appRow);
            const int rowsBefore = g_win->RowCount();
            const bool okClick = g_win->ClickRowExpandoForTest(appRow);
            // 展开状态翻转后行表会重建，不能再用旧下标 —— 按 pid 重新定位
            int after = -1;
            for (int r = 0; r < g_win->RowCount(); ++r) {
                if (g_win->RowPidForTest(r) == pid) { after = r; break; }
            }
            const bool expanded = after >= 0 && g_win->RowExpandedForTest(after);
            char detail[160];
            sprintf_s(detail, "pid=%u, 行数 %d→%d, expanded=%d",
                      pid, rowsBefore, g_win->RowCount(), expanded ? 1 : 0);
            Mark(okClick && expanded && g_win->RowCount() > rowsBefore ? 1 : 0,
                 L"展开应用行后子进程真的插入行表", detail);
            // 还原，避免影响后面的菜单自测
            g_win->ClickRowExpandoForTest(after);
        }
    }

    // ---- 右键菜单自测：逐个页面构建菜单并统计项数 ----
    log.W(L"\n=== 右键菜单自测 ===\n");
    struct TabCase { int idx; const wchar_t* name; };
    TabCase cases[] = {
        { 0, L"进程" }, { 1, L"性能" }, { 2, L"应用历史" }, { 3, L"启动" },
        { 4, L"用户" }, { 5, L"详细信息" }, { 6, L"服务" },
    };
    for (auto& c : cases) {
        g_win->ShowPage(c.idx);
        Sleep(40);
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
        std::wstring name = std::wstring(L"右键菜单「") + c.name + L"」有可用项";
        if (!m) {
            MarkW(0, name.c_str(), L"BuildXxxContext 返回 NULL");
            continue;
        }
        const int n = GetMenuItemCount(m);
        int enabled = 0;
        for (int i = 0; i < n; ++i) {
            UINT st = GetMenuState(m, i, MF_BYPOSITION);
            if (st != 0xFFFFFFFF && !(st & (MF_GRAYED | MF_DISABLED))) ++enabled;
        }
        char detail[96];
        sprintf_s(detail, "%d 项，其中可用 %d 项", n, enabled);
        Mark(n > 0 && enabled > 0 ? 1 : 0, name.c_str(), detail);
        DestroyMenu(m);
        Sleep(20);
    }

    g_win->ShowPage(0);

    log.F("\n=== 结果: PASS %d / FAIL %d / SKIP %d ===\n", t.pass, t.fail, t.skip);
    log.Close();
    return t.fail > 0 ? 1 : 0;
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
        if (selfTest) {
            // 自测模式：不能弹模态框（用户正在用这台机器，无属主的框会抢焦点、
            // 阻塞脚本）。但也**必须覆写 selftest.txt** —— 否则脚本读到的是上一次
            // 运行留下的 PASS，把「压根没跑」当成「跑过了」。
            WriteSelfTestNotRun(L"另一实例正在运行（单实例互斥），本次自测未执行");
            return 2;   // 约定：2 = 单实例冲突
        }
        MessageBoxW(nullptr, L"Task Manager Pro 已在运行。", L"提示", MB_OK | MB_ICONINFORMATION);
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
        // 同步跑完自测再退出：交互全部走 TmWindow 的 *ForTest() 入口，
        // 不经过输入队列，也不产生任何可见窗口。
        // 【返回值即退出码】0=全过 1=有 FAIL 2=单实例冲突（在更早处返回）。
        const int rc = RunSelfTest();
        if (mutex) CloseHandle(mutex);
        delete g_win;
        delete g_collector;
        return rc;
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
